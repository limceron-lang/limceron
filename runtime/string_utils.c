/*
 * Limceron Runtime — String Utilities
 */
#include "string_utils.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <stdint.h>

char *lcn_str_concat(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    size_t la = strlen(a);
    size_t lb = strlen(b);
    char *result = (char *)malloc(la + lb + 1);
    if (!result) return strdup("");
    memcpy(result, a, la);
    memcpy(result + la, b, lb);
    result[la + lb] = '\0';
    return result;
}

char *lcn_str_replace(const char *src, const char *old, const char *new_str) {
    if (!src || !old || !new_str) return src ? strdup(src) : strdup("");
    size_t old_len = strlen(old);
    size_t new_len = strlen(new_str);
    if (old_len == 0) return strdup(src);

    /* Count occurrences */
    int count = 0;
    const char *p = src;
    while ((p = strstr(p, old)) != NULL) { count++; p += old_len; }

    size_t result_len = strlen(src) + count * (new_len - old_len);
    char *result = (char *)malloc(result_len + 1);
    if (!result) return strdup(src);

    char *dst = result;
    p = src;
    while (*p) {
        if (strncmp(p, old, old_len) == 0) {
            memcpy(dst, new_str, new_len);
            dst += new_len;
            p += old_len;
        } else {
            *dst++ = *p++;
        }
    }
    *dst = '\0';
    return result;
}

char *lcn_str_trim(const char *src) {
    if (!src) return strdup("");
    const char *start = src;
    while (*start && isspace((unsigned char)*start)) start++;
    const char *end = src + strlen(src) - 1;
    while (end > start && isspace((unsigned char)*end)) end--;
    size_t len = (size_t)(end - start + 1);
    char *result = (char *)malloc(len + 1);
    if (!result) return strdup("");
    memcpy(result, start, len);
    result[len] = '\0';
    return result;
}

char *lcn_str_substring(const char *src, int start, int length) {
    if (!src) return strdup("");
    int src_len = (int)strlen(src);
    if (start < 0) start = 0;
    if (start >= src_len) return strdup("");
    if (length < 0 || start + length > src_len) length = src_len - start;
    char *result = (char *)malloc((size_t)length + 1);
    if (!result) return strdup("");
    memcpy(result, src + start, (size_t)length);
    result[length] = '\0';
    return result;
}

char **lcn_str_split(const char *src, const char *delim, int *out_count) {
    *out_count = 0;
    if (!src || !delim) {
        char **r = (char **)malloc(sizeof(char *));
        r[0] = strdup(src ? src : "");
        *out_count = 1;
        return r;
    }

    size_t delim_len = strlen(delim);
    if (delim_len == 0) {
        char **r = (char **)malloc(sizeof(char *));
        r[0] = strdup(src);
        *out_count = 1;
        return r;
    }

    /* Count splits */
    int cap = 8;
    char **parts = (char **)malloc(sizeof(char *) * (size_t)cap);
    int count = 0;
    const char *p = src;
    const char *found;

    while ((found = strstr(p, delim)) != NULL) {
        if (count >= cap - 1) {
            cap *= 2;
            parts = (char **)realloc(parts, sizeof(char *) * (size_t)cap);
        }
        size_t seg_len = (size_t)(found - p);
        parts[count] = (char *)malloc(seg_len + 1);
        memcpy(parts[count], p, seg_len);
        parts[count][seg_len] = '\0';
        count++;
        p = found + delim_len;
    }

    /* Last segment */
    if (count >= cap) {
        cap++;
        parts = (char **)realloc(parts, sizeof(char *) * (size_t)cap);
    }
    parts[count] = strdup(p);
    count++;

    *out_count = count;
    return parts;
}

bool lcn_str_ends_with(const char *src, const char *suffix) {
    if (!src || !suffix) return false;
    size_t src_len = strlen(src);
    size_t suf_len = strlen(suffix);
    if (suf_len > src_len) return false;
    return strcmp(src + src_len - suf_len, suffix) == 0;
}

char *lcn_str_from_int(int64_t val) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)val);
    return strdup(buf);
}

char *lcn_sql_escape(const char *s) {
    if (!s) return strdup("");
    size_t len = strlen(s);
    char *out = (char *)malloc(len * 2 + 1);
    if (!out) return strdup(s);
    size_t j = 0;
    size_t i;
    for (i = 0; i < len; i++) {
        switch (s[i]) {
        case '\'': out[j++] = '\''; out[j++] = '\''; break;
        case '\\': out[j++] = '\\'; out[j++] = '\\'; break;
        case '\0': break;
        default:   out[j++] = s[i]; break;
        }
    }
    out[j] = '\0';
    return out;
}

/* ════════════════════════════════════════════════
 * Character-level string manipulation
 * ════════════════════════════════════════════════ */

char *lcn_char_at(const char *s, int64_t i) {
    if (!s || i < 0 || i >= (int64_t)strlen(s)) return strdup("");
    char *buf = (char *)malloc(2);
    if (!buf) return strdup("");
    buf[0] = s[i];
    buf[1] = '\0';
    return buf;
}

int64_t lcn_char_code(const char *s) {
    return (s && s[0]) ? (int64_t)(unsigned char)s[0] : 0;
}

char *lcn_str_from_code(int64_t code) {
    char *buf = (char *)malloc(2);
    if (!buf) return strdup("");
    buf[0] = (char)code;
    buf[1] = '\0';
    return buf;
}

char *lcn_str_slice(const char *s, int64_t start, int64_t end) {
    if (!s) return strdup("");
    int64_t len = (int64_t)strlen(s);
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start >= end) return strdup("");
    size_t slice_len = (size_t)(end - start);
    char *out = (char *)malloc(slice_len + 1);
    if (!out) return strdup("");
    memcpy(out, s + start, slice_len);
    out[slice_len] = '\0';
    return out;
}

int64_t lcn_str_find(const char *s, const char *needle) {
    if (!s || !needle) return -1;
    const char *p = strstr(s, needle);
    return p ? (int64_t)(p - s) : -1;
}

bool lcn_char_is_alpha(const char *s) {
    if (!s || !s[0]) return false;
    char c = s[0];
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool lcn_char_is_digit(const char *s) {
    if (!s || !s[0]) return false;
    return s[0] >= '0' && s[0] <= '9';
}

bool lcn_char_is_alnum(const char *s) {
    return lcn_char_is_alpha(s) || lcn_char_is_digit(s);
}
