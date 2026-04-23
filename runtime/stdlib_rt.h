/*
 * Limceron Runtime — Standard Library Functions
 * Runtime implementations for std.* modules.
 * C99, no external dependencies beyond libc.
 */
#ifndef LCN_STDLIB_RT_H
#define LCN_STDLIB_RT_H

#include <stdint.h>
#include <stdbool.h>

/* ── Math ── */
static inline int64_t lcn_min(int64_t a, int64_t b) { return a < b ? a : b; }
static inline int64_t lcn_max(int64_t a, int64_t b) { return a > b ? a : b; }
static inline int64_t lcn_clamp(int64_t x, int64_t lo, int64_t hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

/* ── Time ── */
int64_t     lcn_now_ms(void);
void        lcn_sleep_ms(int64_t ms);
const char *lcn_format_timestamp(int64_t epoch_ms);

/* ── Logging ── */
void lcn_log(const char *level, const char *message);

/* ── Batch ── */
int64_t lcn_batch_size(void);
int64_t lcn_batch_offset(void);
bool    lcn_batch_dry_run(void);
void    lcn_batch_progress(int64_t current, int64_t total);

/* ── Budget introspection ── */
int64_t lcn_budget_tokens_used(void);
int64_t lcn_budget_tokens_left(void);
double  lcn_budget_cost_used(void);
double  lcn_budget_cost_left(void);
double  lcn_budget_percentage(void);
int64_t lcn_budget_elapsed_ms(void);

/* ── Token estimation ── */
int64_t lcn_estimate_tokens(const char *text);
bool    lcn_fits_in_budget(const char *text, int64_t max_tokens);

/* ── Tracing ── */
int64_t lcn_trace_begin(const char *name);
void    lcn_trace_end(int64_t span_id);
void    lcn_trace_tag(int64_t span_id, const char *key, const char *value);
void    lcn_trace_event(const char *name, const char *data);

/* ── File I/O ── */
const char *lcn_read_file(const char *path);
bool        lcn_write_file(const char *path, const char *content);
const char *lcn_read_line(void);

/* ── Character-level string manipulation ── */
/* (Declarations only — implementations in string_utils.c) */
/* These are also declared in string_utils.h; repeated here for */
/* consumers that include only stdlib_rt.h. */
char       *lcn_char_at(const char *s, int64_t i);
int64_t     lcn_char_code(const char *s);
char       *lcn_str_from_code(int64_t code);
char       *lcn_str_slice(const char *s, int64_t start, int64_t end);
int64_t     lcn_str_find(const char *s, const char *needle);
bool        lcn_char_is_alpha(const char *s);
bool        lcn_char_is_digit(const char *s);
bool        lcn_char_is_alnum(const char *s);

#endif /* LCN_STDLIB_RT_H */
