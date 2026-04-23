/*
 * Limceron Agent Memory — SQLite-backed persistent memory
 * Supports: CRUD, sessions, vector search (cosine similarity),
 *           JSONL mirroring, TTL expiry, compaction.
 * C99, single-threaded (Stage 0).
 */
#ifndef LCN_MEMORY_H
#define LCN_MEMORY_H

#include <stdint.h>
#include <stdbool.h>

/* Forward-declare sqlite3 to avoid pulling sqlite3.h into every TU.
 * When sqlite3.h is included first, its own typedef takes precedence. */
struct sqlite3;

/* ── Memory entry types ─────────────────────────────────────────── */

typedef enum {
    LCN_MEM_MESSAGE,
    LCN_MEM_FACT,
    LCN_MEM_SUMMARY,
    LCN_MEM_STATE,
    LCN_MEM_TOOL_RESULT,
    LCN_MEM_TYPE_COUNT
} LcnMemoryType;

/* ── Memory entry ───────────────────────────────────────────────── */

typedef struct {
    int64_t         id;
    char            agent[64];
    char            session_id[64];
    LcnMemoryType   type;
    char            key[256];
    char           *value_json;     /* heap-allocated */
    float          *embedding;      /* heap-allocated, may be NULL */
    int             embedding_dim;
    int64_t         timestamp_ms;
    int64_t         ttl_ms;
    int64_t         access_count;
} LcnMemoryEntry;

/* ── Session ────────────────────────────────────────────────────── */

typedef struct {
    int64_t  id;
    char     agent[64];
    char     session_id[64];
    char     channel[256];
    int64_t  started_at_ms;
    int64_t  last_active_ms;
    char    *summary;               /* heap-allocated, may be NULL */
    bool     active;
} LcnMemorySession;

/* ── Memory store ───────────────────────────────────────────────── */

typedef struct {
    struct sqlite3 *db;
    char     db_path[512];
    char     jsonl_path[512];
    bool     initialized;
} LcnMemoryStore;

/* ── Global singleton (auto-init) ───────────────────────────────── */

LcnMemoryStore *lcn_memory_store(void);

/* ── Lifecycle ──────────────────────────────────────────────────── */

bool lcn_memory_init(LcnMemoryStore *store, const char *db_path);
void lcn_memory_close(LcnMemoryStore *store);

/* ── CRUD ───────────────────────────────────────────────────────── */

int64_t lcn_memory_insert(LcnMemoryStore *store,
                           const char *agent, const char *session_id,
                           LcnMemoryType type, const char *key,
                           const char *value_json,
                           const float *embedding, int embedding_dim,
                           int64_t ttl_ms);

bool lcn_memory_delete(LcnMemoryStore *store, int64_t id);

/* ── Queries ────────────────────────────────────────────────────── */

int lcn_memory_query_session(LcnMemoryStore *store,
                              const char *agent, const char *session_id,
                              int limit,
                              LcnMemoryEntry *out, int cap);

int lcn_memory_query_type(LcnMemoryStore *store,
                           const char *agent, LcnMemoryType type,
                           int limit,
                           LcnMemoryEntry *out, int cap);

int lcn_memory_search(LcnMemoryStore *store,
                       const char *agent,
                       const float *query_embedding, int dim,
                       int limit,
                       LcnMemoryEntry *out, int cap);

int lcn_memory_query_key(LcnMemoryStore *store,
                          const char *agent,
                          const char *key_substring,
                          int limit,
                          LcnMemoryEntry *out, int cap);

/* ── Maintenance ────────────────────────────────────────────────── */

int  lcn_memory_expire(LcnMemoryStore *store);

int  lcn_memory_compact(LcnMemoryStore *store,
                         const char *agent, const char *session_id,
                         const char *summary_json, int n);

/* ── Session management ─────────────────────────────────────────── */

int64_t lcn_session_create(LcnMemoryStore *store,
                            const char *agent, const char *session_id,
                            const char *channel);

int64_t lcn_session_get_or_create(LcnMemoryStore *store,
                                   const char *agent,
                                   const char *session_id,
                                   const char *channel);

int  lcn_session_list(LcnMemoryStore *store,
                      const char *agent,
                      LcnMemorySession *out, int cap);

bool lcn_session_close(LcnMemoryStore *store, const char *session_id);

bool lcn_session_set_summary(LcnMemoryStore *store,
                              const char *session_id,
                              const char *summary);

/* ── Vector math ────────────────────────────────────────────────── */

float lcn_vec_cosine_similarity(const float *a, const float *b, int dim);

/* ── JSON serialization ─────────────────────────────────────────── */

char *lcn_memory_entry_to_json(const LcnMemoryEntry *entry);
char *lcn_memory_entries_to_json(const LcnMemoryEntry *entries, int count);
char *lcn_memory_session_to_json(const LcnMemorySession *session);
char *lcn_memory_sessions_to_json(const LcnMemorySession *sessions, int count);

/* ── Cleanup helpers ────────────────────────────────────────────── */

void lcn_memory_entry_free(LcnMemoryEntry *entry);
void lcn_memory_session_free(LcnMemorySession *session);

/* ── Memory type name ───────────────────────────────────────────── */

const char *lcn_memory_type_name(LcnMemoryType type);

#endif /* LCN_MEMORY_H */
