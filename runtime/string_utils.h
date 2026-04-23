/*
 * Limceron Runtime — String Utilities
 * Helper functions for string manipulation in generated code.
 * Pure C99, no external dependencies.
 */
#ifndef LCN_STRING_UTILS_H
#define LCN_STRING_UTILS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Replace all occurrences of `old` with `new_str` in `src`. Caller must free(). */
char *lcn_str_replace(const char *src, const char *old, const char *new_str);

/* Trim leading and trailing whitespace. Caller must free(). */
char *lcn_str_trim(const char *src);

/* Extract substring from `start` with `length` chars. Caller must free(). */
char *lcn_str_substring(const char *src, int start, int length);

/* Concatenate two strings. Caller must free(). */
char *lcn_str_concat(const char *a, const char *b);

/* Split string by delimiter. Returns array of strings, sets *out_count.
 * Caller must free each string and the array itself. */
char **lcn_str_split(const char *src, const char *delim, int *out_count);

/* Check if string ends with suffix. */
bool lcn_str_ends_with(const char *src, const char *suffix);

/* Convert integer to string. Caller must free(). */
char *lcn_str_from_int(int64_t val);

/* Escape dangerous characters for safe SQL interpolation. Caller must free(). */
char *lcn_sql_escape(const char *s);

/* ── Character-level string manipulation ── */

/* Return character at index i as a 1-char string. Caller must free(). */
char *lcn_char_at(const char *s, int64_t i);

/* Return ASCII code of first character (0 if empty). */
int64_t lcn_char_code(const char *s);

/* Return 1-char string from ASCII code. Caller must free(). */
char *lcn_str_from_code(int64_t code);

/* Return substring s[start..end). Caller must free(). */
char *lcn_str_slice(const char *s, int64_t start, int64_t end);

/* Return index of needle in s, or -1. */
int64_t lcn_str_find(const char *s, const char *needle);

/* True if first char is a-zA-Z_ */
bool lcn_char_is_alpha(const char *s);

/* True if first char is 0-9 */
bool lcn_char_is_digit(const char *s);

/* True if first char is a-zA-Z0-9_ */
bool lcn_char_is_alnum(const char *s);

#endif /* LCN_STRING_UTILS_H */
