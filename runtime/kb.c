/*
 * Limceron Knowledge Base — Implementation
 * Document ingestion, FTS5 indexing, full-text search,
 * and context formatting for RAG.
 * C99, single-threaded (Stage 0).
 */

#include "kb.h"
#include "sqlite3.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

/* ── Constants ──────────────────────────────────────────────────── */

#define KB_DEFAULT_CHUNK_SIZE    500
#define KB_DEFAULT_CHUNK_OVERLAP 50
#define KB_MAX_FILE_SIZE         (10 * 1024 * 1024)  /* 10 MB */

/* ── Schema ─────────────────────────────────────────────────────── */

static const char *KB_SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS kb_meta ("
    "    key TEXT PRIMARY KEY,"
    "    value TEXT"
    ");"
    "CREATE TABLE IF NOT EXISTS kb_chunks ("
    "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "    source_file TEXT NOT NULL,"
    "    chunk_index INTEGER NOT NULL,"
    "    content TEXT NOT NULL,"
    "    content_len INTEGER NOT NULL"
    ");"
    "CREATE VIRTUAL TABLE IF NOT EXISTS kb_fts USING fts5("
    "    content, "
    "    content='kb_chunks', "
    "    content_rowid='id'"
    ");"
    /* Triggers to keep FTS in sync with kb_chunks */
    "CREATE TRIGGER IF NOT EXISTS kb_ai AFTER INSERT ON kb_chunks BEGIN "
    "    INSERT INTO kb_fts(rowid, content) VALUES (new.id, new.content); "
    "END;"
    "CREATE TRIGGER IF NOT EXISTS kb_ad AFTER DELETE ON kb_chunks BEGIN "
    "    INSERT INTO kb_fts(kb_fts, rowid, content) VALUES ('delete', old.id, old.content); "
    "END;"
    "CREATE TRIGGER IF NOT EXISTS kb_au AFTER UPDATE ON kb_chunks BEGIN "
    "    INSERT INTO kb_fts(kb_fts, rowid, content) VALUES ('delete', old.id, old.content); "
    "    INSERT INTO kb_fts(kb_fts, rowid, content) VALUES (new.id, new.content); "
    "END;";

/* ── Helpers ────────────────────────────────────────────────────── */

static void kb_safe_copy(char *dst, const char *src, size_t cap)
{
    if (src) {
        snprintf(dst, cap, "%s", src);
    } else {
        dst[0] = '\0';
    }
}

/* Check if a filename has a supported extension */
static bool kb_is_supported_file(const char *name)
{
    static const char *exts[] = {
        ".txt", ".md", ".json", ".csv", ".log",
        ".yaml", ".yml", ".xml", ".html", ".htm",
        ".rst", ".ini", ".cfg", ".conf", ".toml",
        NULL
    };
    size_t nlen = strlen(name);
    int i;

    for (i = 0; exts[i]; i++) {
        size_t elen = strlen(exts[i]);
        if (nlen > elen && strcmp(name + nlen - elen, exts[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Read entire file into malloc'd buffer. Returns NULL on error. */
static char *kb_read_file(const char *path, size_t *out_len)
{
    FILE *f;
    long size;
    char *buf;
    size_t read_len;

    f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0 || (size_t)size > KB_MAX_FILE_SIZE) {
        fclose(f);
        return NULL;
    }

    buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    read_len = fread(buf, 1, (size_t)size, f);
    fclose(f);

    buf[read_len] = '\0';
    if (out_len) *out_len = read_len;
    return buf;
}

/* ── Static global ──────────────────────────────────────────────── */

static LcnKnowledgeBase g_kb_store;

LcnKnowledgeBase *lcn_kb_store(void)
{
    if (!g_kb_store.initialized) {
        const char *db_path = getenv("LCN_KB_DB");
        if (!db_path) db_path = "lcn_kb.db";
        lcn_kb_init(&g_kb_store, db_path,
                     KB_DEFAULT_CHUNK_SIZE, KB_DEFAULT_CHUNK_OVERLAP);
    }
    return &g_kb_store;
}

/* ── Lifecycle ──────────────────────────────────────────────────── */

bool lcn_kb_init(LcnKnowledgeBase *kb, const char *db_path,
                  int chunk_size, int chunk_overlap)
{
    int rc;
    char *err_msg = NULL;

    if (!kb || !db_path) return false;

    memset(kb, 0, sizeof(*kb));
    kb_safe_copy(kb->db_path, db_path, sizeof(kb->db_path));
    kb->chunk_size = (chunk_size > 0) ? chunk_size : KB_DEFAULT_CHUNK_SIZE;
    kb->chunk_overlap = (chunk_overlap >= 0) ? chunk_overlap : KB_DEFAULT_CHUNK_OVERLAP;

    rc = sqlite3_open(db_path, &kb->db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[lcn_kb] Cannot open database: %s\n",
                sqlite3_errmsg(kb->db));
        return false;
    }

    sqlite3_exec(kb->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);

    rc = sqlite3_exec(kb->db, KB_SCHEMA_SQL, NULL, NULL, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[lcn_kb] Schema error: %s\n", err_msg);
        sqlite3_free(err_msg);
        sqlite3_close(kb->db);
        kb->db = NULL;
        return false;
    }

    kb->initialized = true;

    /* Check if we already have data */
    kb->ingested = lcn_kb_has_data(kb);

    return true;
}

void lcn_kb_close(LcnKnowledgeBase *kb)
{
    if (!kb) return;
    if (kb->db) {
        sqlite3_close(kb->db);
        kb->db = NULL;
    }
    kb->initialized = false;
}

/* ── Ingestion ──────────────────────────────────────────────────── */

int lcn_kb_ingest_file(LcnKnowledgeBase *kb, const char *file_path)
{
    char *content;
    size_t content_len;
    sqlite3_stmt *stmt;
    int rc;
    int chunk_idx = 0;
    size_t pos = 0;
    int chunk_size;
    int overlap;

    if (!kb || !kb->db || !file_path) return 0;

    content = kb_read_file(file_path, &content_len);
    if (!content) return 0;

    chunk_size = kb->chunk_size;
    overlap = kb->chunk_overlap;

    /* Check if this file was already ingested */
    {
        sqlite3_stmt *check;
        rc = sqlite3_prepare_v2(kb->db,
            "SELECT COUNT(*) FROM kb_chunks WHERE source_file = ?",
            -1, &check, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(check, 1, file_path, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(check) == SQLITE_ROW) {
                int existing = sqlite3_column_int(check, 0);
                if (existing > 0) {
                    sqlite3_finalize(check);
                    free(content);
                    return 0;  /* Already ingested */
                }
            }
            sqlite3_finalize(check);
        }
    }

    rc = sqlite3_prepare_v2(kb->db,
        "INSERT INTO kb_chunks (source_file, chunk_index, content, content_len) "
        "VALUES (?, ?, ?, ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        free(content);
        return 0;
    }

    /* Chunk the content */
    while (pos < content_len) {
        size_t end = pos + (size_t)chunk_size;
        size_t actual_end;
        int clen;

        if (end > content_len) end = content_len;

        /* Try to break at a sentence or line boundary */
        actual_end = end;
        if (actual_end < content_len) {
            /* Look back for a good break point */
            size_t look = actual_end;
            while (look > pos + (size_t)(chunk_size / 2)) {
                char c = content[look];
                if (c == '\n' || c == '.' || c == '!' || c == '?') {
                    actual_end = look + 1;
                    break;
                }
                look--;
            }
        }

        clen = (int)(actual_end - pos);

        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, file_path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, chunk_idx);
        sqlite3_bind_text(stmt, 3, content + pos, clen, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, clen);

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) break;

        chunk_idx++;

        /* Advance with overlap */
        if (actual_end >= content_len) break;
        if (overlap > 0 && (int)(actual_end - pos) > overlap) {
            pos = actual_end - (size_t)overlap;
        } else {
            pos = actual_end;
        }
    }

    sqlite3_finalize(stmt);
    free(content);

    kb->ingested = true;
    return chunk_idx;
}

int lcn_kb_ingest_dir(LcnKnowledgeBase *kb, const char *dir_path)
{
    DIR *d;
    struct dirent *ent;
    int total_chunks = 0;

    if (!kb || !kb->db || !dir_path) return 0;

    d = opendir(dir_path);
    if (!d) {
        fprintf(stderr, "[lcn_kb] Cannot open directory: %s\n", dir_path);
        return 0;
    }

    /* Begin transaction for bulk insert */
    sqlite3_exec(kb->db, "BEGIN TRANSACTION;", NULL, NULL, NULL);

    while ((ent = readdir(d)) != NULL) {
        char full_path[1024];
        struct stat st;

        /* Skip hidden files and directories */
        if (ent->d_name[0] == '.') continue;

        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ent->d_name);

        if (stat(full_path, &st) != 0) continue;

        if (S_ISREG(st.st_mode) && kb_is_supported_file(ent->d_name)) {
            int n = lcn_kb_ingest_file(kb, full_path);
            if (n > 0) {
                fprintf(stderr, "[lcn_kb] Ingested: %s (%d chunks)\n",
                        ent->d_name, n);
            }
            total_chunks += n;
        }
    }

    sqlite3_exec(kb->db, "COMMIT;", NULL, NULL, NULL);

    closedir(d);

    if (total_chunks > 0) {
        fprintf(stderr, "[lcn_kb] Total: %d chunks from %s\n",
                total_chunks, dir_path);
    }

    return total_chunks;
}

bool lcn_kb_has_data(LcnKnowledgeBase *kb)
{
    sqlite3_stmt *stmt;
    int rc;
    bool has = false;

    if (!kb || !kb->db) return false;

    rc = sqlite3_prepare_v2(kb->db,
        "SELECT COUNT(*) FROM kb_chunks", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return false;

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        has = sqlite3_column_int(stmt, 0) > 0;
    }
    sqlite3_finalize(stmt);
    return has;
}

int lcn_kb_chunk_count(LcnKnowledgeBase *kb)
{
    sqlite3_stmt *stmt;
    int rc;
    int count = 0;

    if (!kb || !kb->db) return 0;

    rc = sqlite3_prepare_v2(kb->db,
        "SELECT COUNT(*) FROM kb_chunks", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return 0;

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count;
}

/* ── Search ─────────────────────────────────────────────────────── */

int lcn_kb_search(LcnKnowledgeBase *kb, const char *query,
                   int limit, LcnKbChunk *out, int cap)
{
    sqlite3_stmt *stmt;
    int rc;
    int count = 0;
    int max_results;

    if (!kb || !kb->db || !query || !out || cap <= 0) return 0;

    max_results = (limit > 0 && limit < cap) ? limit : cap;

    /*
     * FTS5 search with bm25() ranking.
     * Join back to kb_chunks for source_file and chunk_index.
     */
    rc = sqlite3_prepare_v2(kb->db,
        "SELECT c.id, c.source_file, c.chunk_index, c.content, c.content_len, "
        "       bm25(kb_fts) as rank "
        "FROM kb_fts f "
        "JOIN kb_chunks c ON c.id = f.rowid "
        "WHERE kb_fts MATCH ? "
        "ORDER BY rank "
        "LIMIT ?",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        /* FTS match syntax error — try prefix search as fallback */
        char prefix_query[512];
        snprintf(prefix_query, sizeof(prefix_query), "\"%s\"", query);
        rc = sqlite3_prepare_v2(kb->db,
            "SELECT c.id, c.source_file, c.chunk_index, c.content, c.content_len, "
            "       bm25(kb_fts) as rank "
            "FROM kb_fts f "
            "JOIN kb_chunks c ON c.id = f.rowid "
            "WHERE kb_fts MATCH ? "
            "ORDER BY rank "
            "LIMIT ?",
            -1, &stmt, NULL);
        if (rc != SQLITE_OK) return 0;
        sqlite3_bind_text(stmt, 1, prefix_query, -1, SQLITE_TRANSIENT);
    } else {
        /* Build FTS5 query: quote each word for safe matching */
        char fts_query[1024];
        const char *p = query;
        char *q = fts_query;
        char *q_end = fts_query + sizeof(fts_query) - 2;
        bool in_word = false;

        while (*p && q < q_end) {
            if (*p == ' ' || *p == '\t' || *p == '\n') {
                if (in_word) {
                    *q++ = '"';
                    if (q < q_end) *q++ = ' ';
                    in_word = false;
                }
            } else {
                if (!in_word) {
                    *q++ = '"';
                    in_word = true;
                }
                *q++ = *p;
            }
            p++;
        }
        if (in_word && q < q_end) *q++ = '"';
        *q = '\0';

        if (fts_query[0] == '\0') {
            sqlite3_finalize(stmt);
            return 0;
        }

        sqlite3_bind_text(stmt, 1, fts_query, -1, SQLITE_TRANSIENT);
    }

    sqlite3_bind_int(stmt, 2, max_results);

    while (sqlite3_step(stmt) == SQLITE_ROW && count < max_results) {
        LcnKbChunk *chunk = &out[count];
        const char *text;

        memset(chunk, 0, sizeof(*chunk));
        chunk->id = sqlite3_column_int64(stmt, 0);

        text = (const char *)sqlite3_column_text(stmt, 1);
        kb_safe_copy(chunk->source_file, text, sizeof(chunk->source_file));

        chunk->chunk_index = sqlite3_column_int(stmt, 2);

        text = (const char *)sqlite3_column_text(stmt, 3);
        chunk->content = text ? strdup(text) : strdup("");

        chunk->content_len = sqlite3_column_int(stmt, 4);
        chunk->rank = sqlite3_column_double(stmt, 5);

        count++;
    }

    sqlite3_finalize(stmt);
    return count;
}

/* ── Context Formatting ─────────────────────────────────────────── */

char *lcn_kb_format_context(const LcnKbChunk *chunks, int count)
{
    size_t total_len = 0;
    char *result;
    char *pos;
    int i;
    const char *header = "--- Relevant context ---\n";
    const char *footer = "--- End context ---\n";

    if (!chunks || count <= 0) return NULL;

    /* Estimate total size */
    total_len += strlen(header) + strlen(footer);
    for (i = 0; i < count; i++) {
        /* [source:N] content\n\n */
        total_len += strlen(chunks[i].source_file) + 20;
        total_len += (chunks[i].content ? strlen(chunks[i].content) : 0);
        total_len += 4;  /* newlines + brackets */
    }

    result = (char *)malloc(total_len + 1);
    if (!result) return NULL;

    pos = result;
    pos += sprintf(pos, "%s", header);

    for (i = 0; i < count; i++) {
        /* Extract just the filename from path */
        const char *fname = chunks[i].source_file;
        const char *slash = strrchr(fname, '/');
        if (slash) fname = slash + 1;

        pos += sprintf(pos, "[%s:%d] %s\n\n",
                       fname, chunks[i].chunk_index,
                       chunks[i].content ? chunks[i].content : "");
    }

    pos += sprintf(pos, "%s", footer);

    return result;
}

/* ── JSON Serialization ─────────────────────────────────────────── */

char *lcn_kb_chunk_to_json(const LcnKbChunk *chunk)
{
    LcnJsonValue *obj;
    char *result;

    if (!chunk) return NULL;

    obj = lcn_json_object_new();
    lcn_json_set_number(obj, "id", (double)chunk->id);
    lcn_json_set_string(obj, "source_file", chunk->source_file);
    lcn_json_set_number(obj, "chunk_index", (double)chunk->chunk_index);
    lcn_json_set_string(obj, "content", chunk->content ? chunk->content : "");
    lcn_json_set_number(obj, "content_len", (double)chunk->content_len);
    lcn_json_set_number(obj, "rank", chunk->rank);

    result = lcn_json_stringify(obj);
    lcn_json_free(obj);
    return result;
}

char *lcn_kb_chunks_to_json(const LcnKbChunk *chunks, int count)
{
    LcnJsonValue *arr;
    int i;
    char *result;

    arr = lcn_json_array_new();

    for (i = 0; i < count; i++) {
        LcnJsonValue *obj = lcn_json_object_new();

        lcn_json_set_number(obj, "id", (double)chunks[i].id);
        lcn_json_set_string(obj, "source_file", chunks[i].source_file);
        lcn_json_set_number(obj, "chunk_index", (double)chunks[i].chunk_index);
        lcn_json_set_string(obj, "content",
            chunks[i].content ? chunks[i].content : "");
        lcn_json_set_number(obj, "content_len", (double)chunks[i].content_len);
        lcn_json_set_number(obj, "rank", chunks[i].rank);

        lcn_json_array_push(arr, obj);
    }

    result = lcn_json_stringify(arr);
    lcn_json_free(arr);
    return result;
}

/* ── Cleanup ────────────────────────────────────────────────────── */

void lcn_kb_chunk_free(LcnKbChunk *chunk)
{
    if (!chunk) return;
    free(chunk->content);
    chunk->content = NULL;
}
