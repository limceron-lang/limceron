/*
 * Limceron Knowledge Base — Document RAG via SQLite FTS5
 * Ingests files from a directory, chunks them, indexes with FTS5,
 * and provides full-text search for retrieval-augmented generation.
 * C99, single-threaded (Stage 0).
 */
#ifndef LCN_KB_H
#define LCN_KB_H

#include <stdint.h>
#include <stdbool.h>

/* Forward-declare sqlite3 */
struct sqlite3;

/* ── Knowledge chunk ────────────────────────────────────────────── */

typedef struct {
    int64_t  id;
    char     source_file[512];   /* original file path */
    int      chunk_index;        /* 0-based chunk number within file */
    char    *content;            /* heap-allocated chunk text */
    int      content_len;
    double   rank;               /* FTS5 relevance score (lower = better) */
} LcnKbChunk;

/* ── Knowledge base store ───────────────────────────────────────── */

typedef struct {
    struct sqlite3 *db;
    char     db_path[512];
    int      chunk_size;         /* chars per chunk (default 500) */
    int      chunk_overlap;      /* overlap chars (default 50) */
    bool     initialized;
    bool     ingested;           /* true after first ingest */
} LcnKnowledgeBase;

/* ── Global singleton (auto-init) ───────────────────────────────── */

LcnKnowledgeBase *lcn_kb_store(void);

/* ── Lifecycle ──────────────────────────────────────────────────── */

bool lcn_kb_init(LcnKnowledgeBase *kb, const char *db_path,
                  int chunk_size, int chunk_overlap);
void lcn_kb_close(LcnKnowledgeBase *kb);

/* ── Ingestion ──────────────────────────────────────────────────── */

/* Ingest a single text file. Returns number of chunks created. */
int lcn_kb_ingest_file(LcnKnowledgeBase *kb, const char *file_path);

/* Ingest all supported files in a directory (non-recursive).
 * Supported: .txt, .md, .json, .csv, .log, .yaml, .yml, .xml, .html
 * Returns total chunks created. */
int lcn_kb_ingest_dir(LcnKnowledgeBase *kb, const char *dir_path);

/* Check if KB has been ingested (has chunks) */
bool lcn_kb_has_data(LcnKnowledgeBase *kb);

/* Get total chunk count */
int lcn_kb_chunk_count(LcnKnowledgeBase *kb);

/* ── Search ─────────────────────────────────────────────────────── */

/* Full-text search using FTS5. Returns number of results.
 * Results sorted by relevance (best first). */
int lcn_kb_search(LcnKnowledgeBase *kb, const char *query,
                   int limit, LcnKbChunk *out, int cap);

/* ── Context formatting ─────────────────────────────────────────── */

/* Format chunks as a context string for LLM prompt injection.
 * Returns heap-allocated string. Caller must free(). */
char *lcn_kb_format_context(const LcnKbChunk *chunks, int count);

/* ── JSON serialization ─────────────────────────────────────────── */

char *lcn_kb_chunk_to_json(const LcnKbChunk *chunk);
char *lcn_kb_chunks_to_json(const LcnKbChunk *chunks, int count);

/* ── Cleanup ────────────────────────────────────────────────────── */

void lcn_kb_chunk_free(LcnKbChunk *chunk);

#endif /* LCN_KB_H */
