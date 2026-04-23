/*
 * Limceron Agent Memory — Implementation
 * SQLite-backed persistent memory with vector search, sessions,
 * JSONL mirroring, TTL expiry, and compaction.
 * C99, single-threaded (Stage 0).
 */

#include "memory.h"
#include "sqlite3.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ── Helpers ────────────────────────────────────────────────────── */

static uint64_t mem_now_ms(void)
{
    return (uint64_t)time(NULL) * 1000;
}

static void mem_safe_copy(char *dst, const char *src, size_t cap)
{
    if (src) {
        snprintf(dst, cap, "%s", src);
    } else {
        dst[0] = '\0';
    }
}

/* ── Memory type names ──────────────────────────────────────────── */

static const char *g_memory_type_names[LCN_MEM_TYPE_COUNT] = {
    "message", "fact", "summary", "state", "tool_result"
};

const char *lcn_memory_type_name(LcnMemoryType type)
{
    if ((int)type >= 0 && type < LCN_MEM_TYPE_COUNT) {
        return g_memory_type_names[type];
    }
    return "unknown";
}

/* memory_type_from_name reserved for future use in REST API parsing */

/* ── SQL Schema ─────────────────────────────────────────────────── */

static const char *SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS entries ("
    "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    agent TEXT NOT NULL,"
    "    session_id TEXT,"
    "    type INTEGER NOT NULL,"
    "    key TEXT,"
    "    value_json TEXT NOT NULL,"
    "    embedding BLOB,"
    "    embedding_dim INTEGER DEFAULT 0,"
    "    timestamp_ms INTEGER NOT NULL,"
    "    ttl_ms INTEGER DEFAULT 0,"
    "    access_count INTEGER DEFAULT 0"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_entries_agent ON entries(agent);"
    "CREATE INDEX IF NOT EXISTS idx_entries_session ON entries(agent, session_id);"
    "CREATE INDEX IF NOT EXISTS idx_entries_type ON entries(agent, type);"
    "CREATE TABLE IF NOT EXISTS sessions ("
    "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    agent TEXT NOT NULL,"
    "    session_id TEXT NOT NULL UNIQUE,"
    "    channel TEXT DEFAULT '',"
    "    started_at_ms INTEGER NOT NULL,"
    "    last_active_ms INTEGER NOT NULL,"
    "    summary TEXT,"
    "    active INTEGER DEFAULT 1"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_sessions_agent ON sessions(agent);";

/* ── JSONL Mirroring ────────────────────────────────────────────── */

static void jsonl_append(LcnMemoryStore *store, const char *agent,
                          const char *session_id, LcnMemoryType type,
                          const char *key, const char *value_json,
                          int64_t timestamp_ms)
{
    FILE *f;

    if (store->jsonl_path[0] == '\0') return;

    f = fopen(store->jsonl_path, "a");
    if (!f) return;

    fprintf(f, "{\"agent\":\"%s\",\"session_id\":\"%s\","
               "\"type\":\"%s\",\"key\":\"%s\","
               "\"value\":%s,\"timestamp_ms\":%lld}\n",
            agent ? agent : "",
            session_id ? session_id : "",
            lcn_memory_type_name(type),
            key ? key : "",
            value_json ? value_json : "null",
            (long long)timestamp_ms);
    fclose(f);
}

/* ── Static global ──────────────────────────────────────────────── */

static LcnMemoryStore g_memory_store;

LcnMemoryStore *lcn_memory_store(void)
{
    if (!g_memory_store.initialized) {
        const char *db_path = getenv("LCN_MEMORY_DB");
        if (!db_path) db_path = "lcn_memory.db";
        lcn_memory_init(&g_memory_store, db_path);
    }
    return &g_memory_store;
}

/* ── Lifecycle ──────────────────────────────────────────────────── */

bool lcn_memory_init(LcnMemoryStore *store, const char *db_path)
{
    int rc;
    char *err_msg = NULL;

    if (!store || !db_path) return false;

    memset(store, 0, sizeof(*store));
    mem_safe_copy(store->db_path, db_path, sizeof(store->db_path));

    /* Derive JSONL path: replace .db with .jsonl */
    {
        size_t len = strlen(db_path);
        if (len > 3 && strcmp(db_path + len - 3, ".db") == 0) {
            snprintf(store->jsonl_path, sizeof(store->jsonl_path),
                     "%.*s.jsonl", (int)(len - 3), db_path);
        } else {
            snprintf(store->jsonl_path, sizeof(store->jsonl_path),
                     "%s.jsonl", db_path);
        }
    }

    rc = sqlite3_open(db_path, &store->db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[lcn_memory] Cannot open database: %s\n",
                sqlite3_errmsg(store->db));
        return false;
    }

    /* Enable WAL mode for better concurrent read performance */
    sqlite3_exec(store->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);

    /* Create schema */
    rc = sqlite3_exec(store->db, SCHEMA_SQL, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[lcn_memory] Schema error: %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(store->db);
        store->db = NULL;
        return false;
    }

    store->initialized = true;
    return true;
}

void lcn_memory_close(LcnMemoryStore *store)
{
    if (!store) return;
    if (store->db) {
        sqlite3_close(store->db);
        store->db = NULL;
    }
    store->initialized = false;
}

/* ── Insert ─────────────────────────────────────────────────────── */

int64_t lcn_memory_insert(LcnMemoryStore *store,
                           const char *agent, const char *session_id,
                           LcnMemoryType type, const char *key,
                           const char *value_json,
                           const float *embedding, int embedding_dim,
                           int64_t ttl_ms)
{
    sqlite3_stmt *stmt;
    int rc;
    int64_t ts;
    int64_t row_id;

    if (!store || !store->db || !value_json) return -1;

    ts = (int64_t)mem_now_ms();

    rc = sqlite3_prepare_v2(store->db,
        "INSERT INTO entries (agent, session_id, type, key, value_json, "
        "embedding, embedding_dim, timestamp_ms, ttl_ms) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, agent ? agent : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, session_id ? session_id : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, (int)type);
    sqlite3_bind_text(stmt, 4, key ? key : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, value_json, -1, SQLITE_TRANSIENT);

    if (embedding && embedding_dim > 0) {
        sqlite3_bind_blob(stmt, 6, embedding,
                          embedding_dim * (int)sizeof(float), SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 7, embedding_dim);
    } else {
        sqlite3_bind_null(stmt, 6);
        sqlite3_bind_int(stmt, 7, 0);
    }

    sqlite3_bind_int64(stmt, 8, ts);
    sqlite3_bind_int64(stmt, 9, ttl_ms);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return -1;
    }

    row_id = sqlite3_last_insert_rowid(store->db);
    sqlite3_finalize(stmt);

    /* JSONL mirror */
    jsonl_append(store, agent, session_id, type, key, value_json, ts);

    /* Update session last_active_ms if session exists */
    if (session_id && session_id[0] != '\0') {
        sqlite3_stmt *upd;
        rc = sqlite3_prepare_v2(store->db,
            "UPDATE sessions SET last_active_ms = ? WHERE session_id = ?",
            -1, &upd, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_int64(upd, 1, ts);
            sqlite3_bind_text(upd, 2, session_id, -1, SQLITE_TRANSIENT);
            sqlite3_step(upd);
            sqlite3_finalize(upd);
        }
    }

    return row_id;
}

/* ── Delete ─────────────────────────────────────────────────────── */

bool lcn_memory_delete(LcnMemoryStore *store, int64_t id)
{
    sqlite3_stmt *stmt;
    int rc;

    if (!store || !store->db) return false;

    rc = sqlite3_prepare_v2(store->db,
        "DELETE FROM entries WHERE id = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int64(stmt, 1, id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

/* ── Query helpers ──────────────────────────────────────────────── */

static void row_to_entry(sqlite3_stmt *stmt, LcnMemoryEntry *entry)
{
    const char *text;
    const void *blob;
    int blob_size;

    memset(entry, 0, sizeof(*entry));

    entry->id = sqlite3_column_int64(stmt, 0);

    text = (const char *)sqlite3_column_text(stmt, 1);
    mem_safe_copy(entry->agent, text, sizeof(entry->agent));

    text = (const char *)sqlite3_column_text(stmt, 2);
    mem_safe_copy(entry->session_id, text, sizeof(entry->session_id));

    entry->type = (LcnMemoryType)sqlite3_column_int(stmt, 3);

    text = (const char *)sqlite3_column_text(stmt, 4);
    mem_safe_copy(entry->key, text, sizeof(entry->key));

    text = (const char *)sqlite3_column_text(stmt, 5);
    entry->value_json = text ? strdup(text) : strdup("");

    /* Embedding */
    blob = sqlite3_column_blob(stmt, 6);
    entry->embedding_dim = sqlite3_column_int(stmt, 7);
    if (blob && entry->embedding_dim > 0) {
        blob_size = entry->embedding_dim * (int)sizeof(float);
        entry->embedding = (float *)malloc((size_t)blob_size);
        if (entry->embedding) {
            memcpy(entry->embedding, blob, (size_t)blob_size);
        }
    } else {
        entry->embedding = NULL;
        entry->embedding_dim = 0;
    }

    entry->timestamp_ms = sqlite3_column_int64(stmt, 8);
    entry->ttl_ms = sqlite3_column_int64(stmt, 9);
    entry->access_count = sqlite3_column_int64(stmt, 10);
}

int lcn_memory_query_session(LcnMemoryStore *store,
                              const char *agent, const char *session_id,
                              int limit,
                              LcnMemoryEntry *out, int cap)
{
    sqlite3_stmt *stmt;
    int rc;
    int count = 0;
    int max_results;

    if (!store || !store->db || !out || cap <= 0) return 0;

    max_results = (limit > 0 && limit < cap) ? limit : cap;

    rc = sqlite3_prepare_v2(store->db,
        "SELECT id, agent, session_id, type, key, value_json, "
        "embedding, embedding_dim, timestamp_ms, ttl_ms, access_count "
        "FROM entries WHERE agent = ? AND session_id = ? "
        "ORDER BY timestamp_ms DESC LIMIT ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return 0;

    sqlite3_bind_text(stmt, 1, agent ? agent : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, session_id ? session_id : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, max_results);

    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_results) {
        row_to_entry(stmt, &out[count]);
        count++;
    }

    sqlite3_finalize(stmt);
    return count;
}

int lcn_memory_query_type(LcnMemoryStore *store,
                           const char *agent, LcnMemoryType type,
                           int limit,
                           LcnMemoryEntry *out, int cap)
{
    sqlite3_stmt *stmt;
    int rc;
    int count = 0;
    int max_results;

    if (!store || !store->db || !out || cap <= 0) return 0;

    max_results = (limit > 0 && limit < cap) ? limit : cap;

    rc = sqlite3_prepare_v2(store->db,
        "SELECT id, agent, session_id, type, key, value_json, "
        "embedding, embedding_dim, timestamp_ms, ttl_ms, access_count "
        "FROM entries WHERE agent = ? AND type = ? "
        "ORDER BY timestamp_ms DESC LIMIT ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return 0;

    sqlite3_bind_text(stmt, 1, agent ? agent : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, (int)type);
    sqlite3_bind_int(stmt, 3, max_results);

    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_results) {
        row_to_entry(stmt, &out[count]);
        count++;
    }

    sqlite3_finalize(stmt);
    return count;
}

/* ── Key Search (LIKE substring) ─────────────────────────────────── */

int lcn_memory_query_key(LcnMemoryStore *store,
                          const char *agent,
                          const char *key_substring,
                          int limit,
                          LcnMemoryEntry *out, int cap)
{
    sqlite3_stmt *stmt;
    int rc;
    int count = 0;
    int max_results;
    char like_pattern[300];

    if (!store || !store->db || !key_substring || !out || cap <= 0) return 0;

    max_results = (limit > 0 && limit < cap) ? limit : cap;
    snprintf(like_pattern, sizeof(like_pattern), "%%%s%%", key_substring);

    if (agent && agent[0] != '\0') {
        rc = sqlite3_prepare_v2(store->db,
            "SELECT id, agent, session_id, type, key, value_json, "
            "embedding, embedding_dim, timestamp_ms, ttl_ms, access_count "
            "FROM entries WHERE agent = ? AND key LIKE ? "
            "ORDER BY timestamp_ms DESC LIMIT ?",
            -1, &stmt, NULL);
        if (rc != SQLITE_OK) return 0;
        sqlite3_bind_text(stmt, 1, agent, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, like_pattern, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, max_results);
    } else {
        rc = sqlite3_prepare_v2(store->db,
            "SELECT id, agent, session_id, type, key, value_json, "
            "embedding, embedding_dim, timestamp_ms, ttl_ms, access_count "
            "FROM entries WHERE key LIKE ? "
            "ORDER BY timestamp_ms DESC LIMIT ?",
            -1, &stmt, NULL);
        if (rc != SQLITE_OK) return 0;
        sqlite3_bind_text(stmt, 1, like_pattern, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, max_results);
    }

    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_results) {
        row_to_entry(stmt, &out[count]);
        count++;
    }

    sqlite3_finalize(stmt);
    return count;
}

/* ── Vector Search ──────────────────────────────────────────────── */

float lcn_vec_cosine_similarity(const float *a, const float *b, int dim)
{
    float dot = 0.0f, norm_a = 0.0f, norm_b = 0.0f;
    float denom;
    int i;

    for (i = 0; i < dim; i++) {
        dot    += a[i] * b[i];
        norm_a += a[i] * a[i];
        norm_b += b[i] * b[i];
    }

    denom = sqrtf(norm_a) * sqrtf(norm_b);
    if (denom < 1e-8f) return 0.0f;
    return dot / denom;
}

typedef struct {
    int    index;
    float  score;
} VecMatch;

static int vec_match_cmp(const void *a, const void *b)
{
    float sa = ((const VecMatch *)a)->score;
    float sb = ((const VecMatch *)b)->score;
    if (sb > sa) return 1;
    if (sb < sa) return -1;
    return 0;
}

int lcn_memory_search(LcnMemoryStore *store,
                       const char *agent,
                       const float *query_embedding, int dim,
                       int limit,
                       LcnMemoryEntry *out, int cap)
{
    sqlite3_stmt *stmt;
    int rc;
    int max_results;

    /* Temp arrays for O(n) scan */
    LcnMemoryEntry *all_entries = NULL;
    VecMatch *matches = NULL;
    int total = 0;
    int alloc = 256;
    int match_count = 0;
    int i;
    int result_count;

    if (!store || !store->db || !query_embedding || dim <= 0 ||
        !out || cap <= 0) {
        return 0;
    }

    max_results = (limit > 0 && limit < cap) ? limit : cap;

    /* Load all entries with embeddings for this agent */
    rc = sqlite3_prepare_v2(store->db,
        "SELECT id, agent, session_id, type, key, value_json, "
        "embedding, embedding_dim, timestamp_ms, ttl_ms, access_count "
        "FROM entries WHERE agent = ? AND embedding IS NOT NULL "
        "AND embedding_dim = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return 0;

    sqlite3_bind_text(stmt, 1, agent ? agent : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, dim);

    all_entries = (LcnMemoryEntry *)malloc((size_t)alloc * sizeof(LcnMemoryEntry));
    if (!all_entries) {
        sqlite3_finalize(stmt);
        return 0;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (total >= alloc) {
            alloc *= 2;
            all_entries = (LcnMemoryEntry *)realloc(all_entries,
                (size_t)alloc * sizeof(LcnMemoryEntry));
            if (!all_entries) {
                sqlite3_finalize(stmt);
                return 0;
            }
        }
        row_to_entry(stmt, &all_entries[total]);
        total++;
    }
    sqlite3_finalize(stmt);

    if (total == 0) {
        free(all_entries);
        return 0;
    }

    /* Compute cosine similarity for each entry */
    matches = (VecMatch *)malloc((size_t)total * sizeof(VecMatch));
    if (!matches) {
        for (i = 0; i < total; i++) lcn_memory_entry_free(&all_entries[i]);
        free(all_entries);
        return 0;
    }

    for (i = 0; i < total; i++) {
        if (all_entries[i].embedding && all_entries[i].embedding_dim == dim) {
            matches[match_count].index = i;
            matches[match_count].score = lcn_vec_cosine_similarity(
                query_embedding, all_entries[i].embedding, dim);
            match_count++;
        }
    }

    /* Sort by score descending */
    qsort(matches, (size_t)match_count, sizeof(VecMatch), vec_match_cmp);

    /* Copy top results */
    result_count = match_count < max_results ? match_count : max_results;
    for (i = 0; i < result_count; i++) {
        out[i] = all_entries[matches[i].index];
        /* Null out so we don't free these */
        all_entries[matches[i].index].value_json = NULL;
        all_entries[matches[i].index].embedding = NULL;
    }

    /* Free remaining entries */
    for (i = 0; i < total; i++) {
        lcn_memory_entry_free(&all_entries[i]);
    }
    free(all_entries);
    free(matches);

    return result_count;
}

/* ── Expire ─────────────────────────────────────────────────────── */

int lcn_memory_expire(LcnMemoryStore *store)
{
    sqlite3_stmt *stmt;
    int rc;
    int64_t now;
    int changes;

    if (!store || !store->db) return 0;

    now = (int64_t)mem_now_ms();

    rc = sqlite3_prepare_v2(store->db,
        "DELETE FROM entries WHERE ttl_ms > 0 AND "
        "(timestamp_ms + ttl_ms) < ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return 0;

    sqlite3_bind_int64(stmt, 1, now);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    changes = sqlite3_changes(store->db);
    return changes;
}

/* ── Compact ────────────────────────────────────────────────────── */

int lcn_memory_compact(LcnMemoryStore *store,
                        const char *agent, const char *session_id,
                        const char *summary_json, int n)
{
    sqlite3_stmt *stmt;
    int rc;
    int64_t *ids = NULL;
    int count = 0;
    int i;

    if (!store || !store->db || !agent || !summary_json || n <= 0) return 0;

    /* Find oldest N entries for this agent+session */
    ids = (int64_t *)malloc((size_t)n * sizeof(int64_t));
    if (!ids) return 0;

    rc = sqlite3_prepare_v2(store->db,
        "SELECT id FROM entries WHERE agent = ? AND session_id = ? "
        "ORDER BY timestamp_ms ASC LIMIT ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        free(ids);
        return 0;
    }

    sqlite3_bind_text(stmt, 1, agent, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, session_id ? session_id : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, n);

    while (sqlite3_step(stmt) == SQLITE_ROW && count < n) {
        ids[count++] = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);

    if (count == 0) {
        free(ids);
        return 0;
    }

    /* Delete old entries */
    for (i = 0; i < count; i++) {
        lcn_memory_delete(store, ids[i]);
    }
    free(ids);

    /* Insert summary entry */
    lcn_memory_insert(store, agent, session_id,
                       LCN_MEM_SUMMARY, "compaction_summary",
                       summary_json, NULL, 0, 0);

    return count;
}

/* ── Session Management ─────────────────────────────────────────── */

int64_t lcn_session_create(LcnMemoryStore *store,
                            const char *agent, const char *session_id,
                            const char *channel)
{
    sqlite3_stmt *stmt;
    int rc;
    int64_t now;

    if (!store || !store->db || !agent || !session_id) return -1;

    now = (int64_t)mem_now_ms();

    rc = sqlite3_prepare_v2(store->db,
        "INSERT INTO sessions (agent, session_id, channel, "
        "started_at_ms, last_active_ms) VALUES (?, ?, ?, ?, ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, agent, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, session_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, channel ? channel : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, now);
    sqlite3_bind_int64(stmt, 5, now);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return -1;
    }

    {
        int64_t row_id = sqlite3_last_insert_rowid(store->db);
        sqlite3_finalize(stmt);
        return row_id;
    }
}

int64_t lcn_session_get_or_create(LcnMemoryStore *store,
                                   const char *agent,
                                   const char *session_id,
                                   const char *channel)
{
    sqlite3_stmt *stmt;
    int rc;

    if (!store || !store->db || !agent || !session_id) return -1;

    /* Check if session exists */
    rc = sqlite3_prepare_v2(store->db,
        "SELECT id FROM sessions WHERE session_id = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;

    sqlite3_bind_text(stmt, 1, session_id, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return id;
    }
    sqlite3_finalize(stmt);

    /* Create new session */
    return lcn_session_create(store, agent, session_id, channel);
}

int lcn_session_list(LcnMemoryStore *store,
                     const char *agent,
                     LcnMemorySession *out, int cap)
{
    sqlite3_stmt *stmt;
    int rc;
    int count = 0;
    const char *text;

    if (!store || !store->db || !out || cap <= 0) return 0;

    rc = sqlite3_prepare_v2(store->db,
        "SELECT id, agent, session_id, channel, started_at_ms, "
        "last_active_ms, summary, active FROM sessions "
        "WHERE agent = ? ORDER BY last_active_ms DESC",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return 0;

    sqlite3_bind_text(stmt, 1, agent ? agent : "", -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW && count < cap) {
        LcnMemorySession *s = &out[count];
        memset(s, 0, sizeof(*s));

        s->id = sqlite3_column_int64(stmt, 0);

        text = (const char *)sqlite3_column_text(stmt, 1);
        mem_safe_copy(s->agent, text, sizeof(s->agent));

        text = (const char *)sqlite3_column_text(stmt, 2);
        mem_safe_copy(s->session_id, text, sizeof(s->session_id));

        text = (const char *)sqlite3_column_text(stmt, 3);
        mem_safe_copy(s->channel, text, sizeof(s->channel));

        s->started_at_ms = sqlite3_column_int64(stmt, 4);
        s->last_active_ms = sqlite3_column_int64(stmt, 5);

        text = (const char *)sqlite3_column_text(stmt, 6);
        s->summary = text ? strdup(text) : NULL;

        s->active = sqlite3_column_int(stmt, 7) != 0;

        count++;
    }

    sqlite3_finalize(stmt);
    return count;
}

bool lcn_session_close(LcnMemoryStore *store, const char *session_id)
{
    sqlite3_stmt *stmt;
    int rc;

    if (!store || !store->db || !session_id) return false;

    rc = sqlite3_prepare_v2(store->db,
        "UPDATE sessions SET active = 0, last_active_ms = ? "
        "WHERE session_id = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_int64(stmt, 1, (int64_t)mem_now_ms());
    sqlite3_bind_text(stmt, 2, session_id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

bool lcn_session_set_summary(LcnMemoryStore *store,
                              const char *session_id,
                              const char *summary)
{
    sqlite3_stmt *stmt;
    int rc;

    if (!store || !store->db || !session_id) return false;

    rc = sqlite3_prepare_v2(store->db,
        "UPDATE sessions SET summary = ?, last_active_ms = ? "
        "WHERE session_id = ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return false;

    sqlite3_bind_text(stmt, 1, summary ? summary : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (int64_t)mem_now_ms());
    sqlite3_bind_text(stmt, 3, session_id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return rc == SQLITE_DONE;
}

/* ── JSON Serialization ─────────────────────────────────────────── */

char *lcn_memory_entry_to_json(const LcnMemoryEntry *entry)
{
    LcnJsonValue *obj;
    LcnJsonValue *val_parsed;
    char *result;

    if (!entry) return NULL;

    obj = lcn_json_object_new();

    lcn_json_set_number(obj, "id", (double)entry->id);
    lcn_json_set_string(obj, "agent", entry->agent);
    lcn_json_set_string(obj, "session_id", entry->session_id);
    lcn_json_set_string(obj, "type", lcn_memory_type_name(entry->type));
    lcn_json_set_string(obj, "key", entry->key);

    /* Try to parse value_json as JSON; fallback to string */
    if (entry->value_json &&
        (entry->value_json[0] == '{' || entry->value_json[0] == '[' ||
         entry->value_json[0] == '"')) {
        val_parsed = lcn_json_parse(entry->value_json,
                                     strlen(entry->value_json));
        if (val_parsed) {
            lcn_json_set(obj, "value", val_parsed);
        } else {
            lcn_json_set_string(obj, "value", entry->value_json);
        }
    } else {
        lcn_json_set_string(obj, "value", entry->value_json ? entry->value_json : "");
    }

    lcn_json_set_number(obj, "embedding_dim", (double)entry->embedding_dim);
    lcn_json_set_number(obj, "timestamp_ms", (double)entry->timestamp_ms);
    lcn_json_set_number(obj, "ttl_ms", (double)entry->ttl_ms);
    lcn_json_set_number(obj, "access_count", (double)entry->access_count);

    result = lcn_json_stringify(obj);
    lcn_json_free(obj);
    return result;
}

char *lcn_memory_entries_to_json(const LcnMemoryEntry *entries, int count)
{
    LcnJsonValue *arr;
    int i;
    char *result;

    arr = lcn_json_array_new();
    for (i = 0; i < count; i++) {
        LcnJsonValue *obj = lcn_json_object_new();
        LcnJsonValue *val_parsed;

        lcn_json_set_number(obj, "id", (double)entries[i].id);
        lcn_json_set_string(obj, "agent", entries[i].agent);
        lcn_json_set_string(obj, "session_id", entries[i].session_id);
        lcn_json_set_string(obj, "type", lcn_memory_type_name(entries[i].type));
        lcn_json_set_string(obj, "key", entries[i].key);

        if (entries[i].value_json &&
            (entries[i].value_json[0] == '{' || entries[i].value_json[0] == '[' ||
             entries[i].value_json[0] == '"')) {
            val_parsed = lcn_json_parse(entries[i].value_json,
                                         strlen(entries[i].value_json));
            if (val_parsed) {
                lcn_json_set(obj, "value", val_parsed);
            } else {
                lcn_json_set_string(obj, "value", entries[i].value_json);
            }
        } else {
            lcn_json_set_string(obj, "value",
                entries[i].value_json ? entries[i].value_json : "");
        }

        lcn_json_set_number(obj, "embedding_dim", (double)entries[i].embedding_dim);
        lcn_json_set_number(obj, "timestamp_ms", (double)entries[i].timestamp_ms);
        lcn_json_set_number(obj, "ttl_ms", (double)entries[i].ttl_ms);
        lcn_json_set_number(obj, "access_count", (double)entries[i].access_count);

        lcn_json_array_push(arr, obj);
    }

    result = lcn_json_stringify(arr);
    lcn_json_free(arr);
    return result;
}

char *lcn_memory_session_to_json(const LcnMemorySession *session)
{
    LcnJsonValue *obj;
    char *result;

    if (!session) return NULL;

    obj = lcn_json_object_new();

    lcn_json_set_number(obj, "id", (double)session->id);
    lcn_json_set_string(obj, "agent", session->agent);
    lcn_json_set_string(obj, "session_id", session->session_id);
    lcn_json_set_string(obj, "channel", session->channel);
    lcn_json_set_number(obj, "started_at_ms", (double)session->started_at_ms);
    lcn_json_set_number(obj, "last_active_ms", (double)session->last_active_ms);
    lcn_json_set_string(obj, "summary", session->summary ? session->summary : "");
    lcn_json_set(obj, "active", lcn_json_bool_new(session->active));

    result = lcn_json_stringify(obj);
    lcn_json_free(obj);
    return result;
}

char *lcn_memory_sessions_to_json(const LcnMemorySession *sessions, int count)
{
    LcnJsonValue *arr;
    int i;
    char *result;

    arr = lcn_json_array_new();
    for (i = 0; i < count; i++) {
        LcnJsonValue *obj = lcn_json_object_new();

        lcn_json_set_number(obj, "id", (double)sessions[i].id);
        lcn_json_set_string(obj, "agent", sessions[i].agent);
        lcn_json_set_string(obj, "session_id", sessions[i].session_id);
        lcn_json_set_string(obj, "channel", sessions[i].channel);
        lcn_json_set_number(obj, "started_at_ms", (double)sessions[i].started_at_ms);
        lcn_json_set_number(obj, "last_active_ms", (double)sessions[i].last_active_ms);
        lcn_json_set_string(obj, "summary",
            sessions[i].summary ? sessions[i].summary : "");
        lcn_json_set(obj, "active", lcn_json_bool_new(sessions[i].active));

        lcn_json_array_push(arr, obj);
    }

    result = lcn_json_stringify(arr);
    lcn_json_free(arr);
    return result;
}

/* ── Cleanup helpers ────────────────────────────────────────────── */

void lcn_memory_entry_free(LcnMemoryEntry *entry)
{
    if (!entry) return;
    free(entry->value_json);
    entry->value_json = NULL;
    free(entry->embedding);
    entry->embedding = NULL;
}

void lcn_memory_session_free(LcnMemorySession *session)
{
    if (!session) return;
    free(session->summary);
    session->summary = NULL;
}
