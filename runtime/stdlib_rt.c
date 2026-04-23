/*
 * Limceron Runtime — Standard Library Implementations
 * C99, no external dependencies beyond libc + POSIX time.
 */

#include "stdlib_rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ════════════════════════════════════════════════
 * Time
 * ════════════════════════════════════════════════ */

int64_t lcn_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

void lcn_sleep_ms(int64_t ms) {
    if (ms <= 0) return;
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

const char *lcn_format_timestamp(int64_t epoch_ms) {
    static char buf[64];
    time_t secs = (time_t)(epoch_ms / 1000);
    struct tm *tm = gmtime(&secs);
    if (!tm) {
        snprintf(buf, sizeof(buf), "%lld", (long long)epoch_ms);
        return buf;
    }
    int ms = (int)(epoch_ms % 1000);
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec, ms);
    return buf;
}

/* ════════════════════════════════════════════════
 * Logging — JSON structured to stderr
 * ════════════════════════════════════════════════ */

void lcn_log(const char *level, const char *message) {
    int64_t ts = lcn_now_ms();
    const char *formatted = lcn_format_timestamp(ts);
    fprintf(stderr, "{\"ts\":\"%s\",\"level\":\"%s\",\"msg\":\"%s\"}\n",
            formatted, level, message);
}

/* ════════════════════════════════════════════════
 * Batch — env-driven batch processing helpers
 * ════════════════════════════════════════════════ */

int64_t lcn_batch_size(void) {
    const char *v = getenv("BATCH_SIZE");
    return v ? atoll(v) : 50;
}

int64_t lcn_batch_offset(void) {
    const char *v = getenv("BATCH_OFFSET");
    return v ? atoll(v) : 0;
}

bool lcn_batch_dry_run(void) {
    const char *v = getenv("DRY_RUN");
    if (!v) return false;
    return (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 || strcmp(v, "yes") == 0);
}

void lcn_batch_progress(int64_t current, int64_t total) {
    if (total <= 0) return;
    double pct = (double)current / (double)total * 100.0;
    fprintf(stderr, "  [batch] %lld/%lld (%.1f%%)\n",
            (long long)current, (long long)total, pct);
}

/* ════════════════════════════════════════════════
 * Budget introspection
 *
 * These use a global budget pointer set by generated code.
 * When no budget is active, they return safe defaults.
 * ════════════════════════════════════════════════ */

/* Global budget pointer — set by agent init code */
static void *_lcn_active_budget = NULL;

void lcn_set_active_budget(void *budget) {
    _lcn_active_budget = budget;
}

/* Internal: cast to LcnBudget layout (matching lcn_runtime.h) */
typedef struct {
    int64_t max_tokens;
    double  max_cost;
    int64_t max_duration_secs;
    int64_t used_tokens;
    double  used_cost;
    time_t  start_time;
    bool    exhausted;
    const char *exhausted_reason;
} LcnBudgetLayout;

int64_t lcn_budget_tokens_used(void) {
    if (!_lcn_active_budget) return 0;
    return ((LcnBudgetLayout *)_lcn_active_budget)->used_tokens;
}

int64_t lcn_budget_tokens_left(void) {
    if (!_lcn_active_budget) return 0;
    LcnBudgetLayout *b = (LcnBudgetLayout *)_lcn_active_budget;
    return b->max_tokens - b->used_tokens;
}

double lcn_budget_cost_used(void) {
    if (!_lcn_active_budget) return 0.0;
    return ((LcnBudgetLayout *)_lcn_active_budget)->used_cost;
}

double lcn_budget_cost_left(void) {
    if (!_lcn_active_budget) return 0.0;
    LcnBudgetLayout *b = (LcnBudgetLayout *)_lcn_active_budget;
    return b->max_cost - b->used_cost;
}

double lcn_budget_percentage(void) {
    if (!_lcn_active_budget) return 0.0;
    LcnBudgetLayout *b = (LcnBudgetLayout *)_lcn_active_budget;
    if (b->max_tokens <= 0) return 0.0;
    return (double)b->used_tokens / (double)b->max_tokens * 100.0;
}

int64_t lcn_budget_elapsed_ms(void) {
    if (!_lcn_active_budget) return 0;
    LcnBudgetLayout *b = (LcnBudgetLayout *)_lcn_active_budget;
    if (b->start_time == 0) return 0;
    return (int64_t)(time(NULL) - b->start_time) * 1000;
}

/* ════════════════════════════════════════════════
 * Token estimation
 *
 * Approximation: ~4 chars per token for English text.
 * This is a reasonable cl100k_base approximation for
 * budget pre-checking without a full BPE implementation.
 * ════════════════════════════════════════════════ */

int64_t lcn_estimate_tokens(const char *text) {
    if (!text) return 0;
    size_t len = strlen(text);
    /* Heuristic: ~4 characters per token for English/code.
     * Add 10% buffer for safety. */
    return (int64_t)((len + 3) / 4 * 1.1);
}

bool lcn_fits_in_budget(const char *text, int64_t max_tokens) {
    return lcn_estimate_tokens(text) <= max_tokens;
}

/* ════════════════════════════════════════════════
 * Tracing — spans with timing
 * ════════════════════════════════════════════════ */

#define MAX_SPANS 256

typedef struct {
    int64_t     id;
    const char *name;
    int64_t     start_ms;
} LcnSpan;

static LcnSpan _spans[MAX_SPANS];
static int _span_count = 0;
static int64_t _next_span_id = 1;

int64_t lcn_trace_begin(const char *name) {
    int64_t id = _next_span_id++;
    if (_span_count < MAX_SPANS) {
        _spans[_span_count].id = id;
        _spans[_span_count].name = name;
        _spans[_span_count].start_ms = lcn_now_ms();
        _span_count++;
    }
    fprintf(stderr, "{\"trace\":\"begin\",\"span\":%lld,\"name\":\"%s\"}\n",
            (long long)id, name);
    return id;
}

void lcn_trace_end(int64_t span_id) {
    for (int i = _span_count - 1; i >= 0; i--) {
        if (_spans[i].id == span_id) {
            int64_t elapsed = lcn_now_ms() - _spans[i].start_ms;
            fprintf(stderr, "{\"trace\":\"end\",\"span\":%lld,\"name\":\"%s\",\"elapsed_ms\":%lld}\n",
                    (long long)span_id, _spans[i].name, (long long)elapsed);
            /* Remove by shifting */
            for (int j = i; j < _span_count - 1; j++)
                _spans[j] = _spans[j + 1];
            _span_count--;
            return;
        }
    }
}

void lcn_trace_tag(int64_t span_id, const char *key, const char *value) {
    fprintf(stderr, "{\"trace\":\"tag\",\"span\":%lld,\"key\":\"%s\",\"value\":\"%s\"}\n",
            (long long)span_id, key, value);
}

void lcn_trace_event(const char *name, const char *data) {
    fprintf(stderr, "{\"trace\":\"event\",\"name\":\"%s\",\"data\":\"%s\",\"ts\":\"%s\"}\n",
            name, data, lcn_format_timestamp(lcn_now_ms()));
}

/* ════════════════════════════════════════════════
 * File I/O
 * ════════════════════════════════════════════════ */

const char *lcn_read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return "";

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0 || size > 64 * 1024 * 1024) {
        fclose(f);
        return "";
    }

    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return ""; }

    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[read] = '\0';
    return buf;
}

bool lcn_write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t len = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    fclose(f);
    return written == len;
}

const char *lcn_read_line(void) {
    static char buf[4096];
    if (fgets(buf, sizeof(buf), stdin)) {
        /* Strip trailing newline */
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
        return buf;
    }
    return "";
}
