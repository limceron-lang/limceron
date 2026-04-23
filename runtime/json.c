/*
 * Limceron JSON Parser/Serializer
 * Recursive descent parser, compact serializer.
 * Pure C99, no external dependencies.
 */
#include "json.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <errno.h>

/* -------------------------------------------------------------------------- */
/*  Internal parser state                                                     */
/* -------------------------------------------------------------------------- */

typedef struct {
    const char *src;
    size_t      len;
    size_t      pos;
} JsonParser;

#define INIT_CAP 8

/* Forward declarations */
static LcnJsonValue *parse_value(JsonParser *p);

/* -------------------------------------------------------------------------- */
/*  Helpers                                                                   */
/* -------------------------------------------------------------------------- */

static void skip_whitespace(JsonParser *p)
{
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            p->pos++;
        } else {
            break;
        }
    }
}

static char peek(JsonParser *p)
{
    skip_whitespace(p);
    if (p->pos >= p->len) return '\0';
    return p->src[p->pos];
}

static bool consume(JsonParser *p, char c)
{
    skip_whitespace(p);
    if (p->pos < p->len && p->src[p->pos] == c) {
        p->pos++;
        return true;
    }
    return false;
}

static bool match_literal(JsonParser *p, const char *lit, size_t n)
{
    if (p->pos + n > p->len) return false;
    if (memcmp(p->src + p->pos, lit, n) != 0) return false;
    p->pos += n;
    return true;
}

static LcnJsonValue *alloc_value(LcnJsonType type)
{
    LcnJsonValue *v = (LcnJsonValue *)calloc(1, sizeof(LcnJsonValue));
    if (v) v->type = type;
    return v;
}

/* -------------------------------------------------------------------------- */
/*  String parsing                                                            */
/* -------------------------------------------------------------------------- */

/* Decode a 4-digit hex value for \uXXXX escapes. Returns -1 on failure. */
static int hex4(const char *s)
{
    int val = 0;
    int i;
    for (i = 0; i < 4; i++) {
        char c = s[i];
        int d;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        val = (val << 4) | d;
    }
    return val;
}

/* Encode a Unicode code point as UTF-8 into buf. Returns bytes written (1-4), or 0 on error. */
static size_t encode_utf8(unsigned int cp, char *buf)
{
    if (cp <= 0x7F) {
        buf[0] = (char)cp;
        return 1;
    } else if (cp <= 0x7FF) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp <= 0xFFFF) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp <= 0x10FFFF) {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

/*
 * Parse a JSON string (opening '"' already consumed by caller? No -- we consume it here).
 * Returns a malloc'd NUL-terminated string and sets *out_len. Returns NULL on error.
 */
static char *parse_string_raw(JsonParser *p, size_t *out_len)
{
    if (p->pos >= p->len || p->src[p->pos] != '"') return NULL;
    p->pos++; /* skip opening quote */

    size_t cap = 64;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;

#define SBUF_PUSH(ch) do {             \
    if (len + 1 >= cap) {              \
        cap *= 2;                      \
        char *nb = (char *)realloc(buf, cap); \
        if (!nb) { free(buf); return NULL; }  \
        buf = nb;                      \
    }                                  \
    buf[len++] = (ch);                 \
} while(0)

    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == '"') {
            p->pos++;
            buf[len] = '\0';
            if (out_len) *out_len = len;
            return buf;
        }
        if (c == '\\') {
            p->pos++;
            if (p->pos >= p->len) { free(buf); return NULL; }
            char esc = p->src[p->pos++];
            switch (esc) {
                case '"':  SBUF_PUSH('"');  break;
                case '\\': SBUF_PUSH('\\'); break;
                case '/':  SBUF_PUSH('/');  break;
                case 'b':  SBUF_PUSH('\b'); break;
                case 'f':  SBUF_PUSH('\f'); break;
                case 'n':  SBUF_PUSH('\n'); break;
                case 'r':  SBUF_PUSH('\r'); break;
                case 't':  SBUF_PUSH('\t'); break;
                case 'u': {
                    if (p->pos + 4 > p->len) { free(buf); return NULL; }
                    int hi = hex4(p->src + p->pos);
                    if (hi < 0) { free(buf); return NULL; }
                    p->pos += 4;

                    unsigned int cp = (unsigned int)hi;

                    /* Handle surrogate pairs */
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (p->pos + 6 > p->len ||
                            p->src[p->pos] != '\\' ||
                            p->src[p->pos + 1] != 'u') {
                            free(buf);
                            return NULL;
                        }
                        p->pos += 2;
                        int lo = hex4(p->src + p->pos);
                        if (lo < 0 || lo < 0xDC00 || lo > 0xDFFF) {
                            free(buf);
                            return NULL;
                        }
                        p->pos += 4;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + ((unsigned int)lo - 0xDC00);
                    }

                    char u8[4];
                    size_t u8len = encode_utf8(cp, u8);
                    if (u8len == 0) { free(buf); return NULL; }
                    {
                        size_t k;
                        for (k = 0; k < u8len; k++) {
                            SBUF_PUSH(u8[k]);
                        }
                    }
                    break;
                }
                default:
                    free(buf);
                    return NULL;
            }
        } else if ((unsigned char)c < 0x20) {
            /* Control characters are invalid in JSON strings */
            free(buf);
            return NULL;
        } else {
            SBUF_PUSH(c);
            p->pos++;
        }
    }
#undef SBUF_PUSH

    free(buf);
    return NULL; /* unterminated string */
}

static LcnJsonValue *parse_string(JsonParser *p)
{
    size_t slen = 0;
    char *s = parse_string_raw(p, &slen);
    if (!s) return NULL;

    LcnJsonValue *v = alloc_value(LCN_JSON_STRING);
    if (!v) { free(s); return NULL; }
    v->data.string.str = s;
    v->data.string.len = slen;
    return v;
}

/* -------------------------------------------------------------------------- */
/*  Number parsing                                                            */
/* -------------------------------------------------------------------------- */

static LcnJsonValue *parse_number(JsonParser *p)
{
    const char *start = p->src + p->pos;
    size_t remaining = p->len - p->pos;
    size_t i = 0;

    /* Optional minus */
    if (i < remaining && start[i] == '-') i++;

    /* Integer part */
    if (i >= remaining) return NULL;
    if (start[i] == '0') {
        i++;
    } else if (start[i] >= '1' && start[i] <= '9') {
        i++;
        while (i < remaining && start[i] >= '0' && start[i] <= '9') i++;
    } else {
        return NULL;
    }

    /* Fractional part */
    if (i < remaining && start[i] == '.') {
        i++;
        if (i >= remaining || start[i] < '0' || start[i] > '9') return NULL;
        while (i < remaining && start[i] >= '0' && start[i] <= '9') i++;
    }

    /* Exponent part */
    if (i < remaining && (start[i] == 'e' || start[i] == 'E')) {
        i++;
        if (i < remaining && (start[i] == '+' || start[i] == '-')) i++;
        if (i >= remaining || start[i] < '0' || start[i] > '9') return NULL;
        while (i < remaining && start[i] >= '0' && start[i] <= '9') i++;
    }

    /* Parse the number with strtod */
    char *tmp = (char *)malloc(i + 1);
    if (!tmp) return NULL;
    memcpy(tmp, start, i);
    tmp[i] = '\0';

    char *end = NULL;
    errno = 0;
    double num = strtod(tmp, &end);
    if (end != tmp + i || errno == ERANGE) {
        /* Allow infinity from overflow -- some APIs return huge numbers */
        if (errno == ERANGE && (num == HUGE_VAL || num == -HUGE_VAL)) {
            /* accept it */
        } else {
            free(tmp);
            return NULL;
        }
    }
    free(tmp);

    LcnJsonValue *v = alloc_value(LCN_JSON_NUMBER);
    if (!v) return NULL;
    v->data.number = num;
    p->pos += i;
    return v;
}

/* -------------------------------------------------------------------------- */
/*  Array parsing                                                             */
/* -------------------------------------------------------------------------- */

static LcnJsonValue *parse_array(JsonParser *p)
{
    if (!consume(p, '[')) return NULL;

    LcnJsonValue *arr = alloc_value(LCN_JSON_ARRAY);
    if (!arr) return NULL;
    arr->data.array.cap = INIT_CAP;
    arr->data.array.count = 0;
    arr->data.array.items = (LcnJsonValue **)malloc(INIT_CAP * sizeof(LcnJsonValue *));
    if (!arr->data.array.items) { free(arr); return NULL; }

    if (peek(p) == ']') {
        p->pos++;
        return arr;
    }

    for (;;) {
        LcnJsonValue *item = parse_value(p);
        if (!item) { lcn_json_free(arr); return NULL; }

        /* Grow array if needed */
        if (arr->data.array.count >= arr->data.array.cap) {
            size_t newcap = arr->data.array.cap * 2;
            LcnJsonValue **newitems = (LcnJsonValue **)realloc(
                arr->data.array.items, newcap * sizeof(LcnJsonValue *));
            if (!newitems) { lcn_json_free(item); lcn_json_free(arr); return NULL; }
            arr->data.array.items = newitems;
            arr->data.array.cap = newcap;
        }
        arr->data.array.items[arr->data.array.count++] = item;

        if (consume(p, ',')) continue;
        if (consume(p, ']')) break;
        lcn_json_free(arr);
        return NULL;
    }
    return arr;
}

/* -------------------------------------------------------------------------- */
/*  Object parsing                                                            */
/* -------------------------------------------------------------------------- */

static LcnJsonValue *parse_object(JsonParser *p)
{
    if (!consume(p, '{')) return NULL;

    LcnJsonValue *obj = alloc_value(LCN_JSON_OBJECT);
    if (!obj) return NULL;
    obj->data.object.cap = INIT_CAP;
    obj->data.object.count = 0;
    obj->data.object.keys = (char **)malloc(INIT_CAP * sizeof(char *));
    obj->data.object.values = (LcnJsonValue **)malloc(INIT_CAP * sizeof(LcnJsonValue *));
    if (!obj->data.object.keys || !obj->data.object.values) {
        free(obj->data.object.keys);
        free(obj->data.object.values);
        free(obj);
        return NULL;
    }

    if (peek(p) == '}') {
        p->pos++;
        return obj;
    }

    for (;;) {
        skip_whitespace(p);

        /* Parse key */
        size_t klen = 0;
        char *key = parse_string_raw(p, &klen);
        if (!key) { lcn_json_free(obj); return NULL; }

        if (!consume(p, ':')) { free(key); lcn_json_free(obj); return NULL; }

        /* Parse value */
        LcnJsonValue *val = parse_value(p);
        if (!val) { free(key); lcn_json_free(obj); return NULL; }

        /* Grow if needed */
        if (obj->data.object.count >= obj->data.object.cap) {
            size_t newcap = obj->data.object.cap * 2;
            char **newkeys = (char **)realloc(
                obj->data.object.keys, newcap * sizeof(char *));
            LcnJsonValue **newvals = (LcnJsonValue **)realloc(
                obj->data.object.values, newcap * sizeof(LcnJsonValue *));
            if (!newkeys || !newvals) {
                /* If one succeeded and the other didn't, we need to handle it.
                 * Since realloc returns the old pointer on failure, just bail. */
                if (newkeys) obj->data.object.keys = newkeys;
                if (newvals) obj->data.object.values = newvals;
                free(key);
                lcn_json_free(val);
                lcn_json_free(obj);
                return NULL;
            }
            obj->data.object.keys = newkeys;
            obj->data.object.values = newvals;
            obj->data.object.cap = newcap;
        }

        /* Check for duplicate key -- overwrite if found */
        {
            size_t idx;
            bool found = false;
            for (idx = 0; idx < obj->data.object.count; idx++) {
                if (strcmp(obj->data.object.keys[idx], key) == 0) {
                    free(key);
                    lcn_json_free(obj->data.object.values[idx]);
                    obj->data.object.values[idx] = val;
                    found = true;
                    break;
                }
            }
            if (!found) {
                obj->data.object.keys[obj->data.object.count] = key;
                obj->data.object.values[obj->data.object.count] = val;
                obj->data.object.count++;
            }
        }

        if (consume(p, ',')) continue;
        if (consume(p, '}')) break;
        lcn_json_free(obj);
        return NULL;
    }
    return obj;
}

/* -------------------------------------------------------------------------- */
/*  Value parsing (dispatch)                                                  */
/* -------------------------------------------------------------------------- */

static LcnJsonValue *parse_value(JsonParser *p)
{
    skip_whitespace(p);
    if (p->pos >= p->len) return NULL;

    char c = p->src[p->pos];

    switch (c) {
    case '"':
        return parse_string(p);
    case '{':
        return parse_object(p);
    case '[':
        return parse_array(p);
    case 't':
        if (match_literal(p, "true", 4)) {
            LcnJsonValue *v = alloc_value(LCN_JSON_BOOL);
            if (v) v->data.boolean = true;
            return v;
        }
        return NULL;
    case 'f':
        if (match_literal(p, "false", 5)) {
            LcnJsonValue *v = alloc_value(LCN_JSON_BOOL);
            if (v) v->data.boolean = false;
            return v;
        }
        return NULL;
    case 'n':
        if (match_literal(p, "null", 4)) {
            return alloc_value(LCN_JSON_NULL);
        }
        return NULL;
    default:
        if (c == '-' || (c >= '0' && c <= '9')) {
            return parse_number(p);
        }
        return NULL;
    }
}

/* -------------------------------------------------------------------------- */
/*  Public: Parse                                                             */
/* -------------------------------------------------------------------------- */

LcnJsonValue *lcn_json_parse(const char *json, size_t len)
{
    if (!json) return NULL;

    JsonParser p;
    p.src = json;
    p.len = len;
    p.pos = 0;

    LcnJsonValue *v = parse_value(&p);
    if (!v) return NULL;

    /* Verify no trailing non-whitespace */
    skip_whitespace(&p);
    if (p.pos != p.len) {
        lcn_json_free(v);
        return NULL;
    }
    return v;
}

/* -------------------------------------------------------------------------- */
/*  Public: Accessors                                                         */
/* -------------------------------------------------------------------------- */

LcnJsonValue *lcn_json_get(const LcnJsonValue *v, const char *key)
{
    size_t i;
    if (!v || v->type != LCN_JSON_OBJECT || !key) return NULL;
    for (i = 0; i < v->data.object.count; i++) {
        if (strcmp(v->data.object.keys[i], key) == 0) {
            return v->data.object.values[i];
        }
    }
    return NULL;
}

const char *lcn_json_get_string(const LcnJsonValue *v, const char *key)
{
    LcnJsonValue *child = lcn_json_get(v, key);
    if (!child || child->type != LCN_JSON_STRING) return NULL;
    return child->data.string.str;
}

double lcn_json_get_number(const LcnJsonValue *v, const char *key)
{
    LcnJsonValue *child = lcn_json_get(v, key);
    if (!child || child->type != LCN_JSON_NUMBER) return 0.0;
    return child->data.number;
}

bool lcn_json_get_bool(const LcnJsonValue *v, const char *key)
{
    LcnJsonValue *child = lcn_json_get(v, key);
    if (!child || child->type != LCN_JSON_BOOL) return false;
    return child->data.boolean;
}

LcnJsonValue *lcn_json_array_get(const LcnJsonValue *v, size_t index)
{
    if (!v || v->type != LCN_JSON_ARRAY) return NULL;
    if (index >= v->data.array.count) return NULL;
    return v->data.array.items[index];
}

size_t lcn_json_array_len(const LcnJsonValue *v)
{
    if (!v || v->type != LCN_JSON_ARRAY) return 0;
    return v->data.array.count;
}

/* -------------------------------------------------------------------------- */
/*  Public: Builders                                                          */
/* -------------------------------------------------------------------------- */

LcnJsonValue *lcn_json_object_new(void)
{
    LcnJsonValue *v = alloc_value(LCN_JSON_OBJECT);
    if (!v) return NULL;
    v->data.object.cap = INIT_CAP;
    v->data.object.count = 0;
    v->data.object.keys = (char **)malloc(INIT_CAP * sizeof(char *));
    v->data.object.values = (LcnJsonValue **)malloc(INIT_CAP * sizeof(LcnJsonValue *));
    if (!v->data.object.keys || !v->data.object.values) {
        free(v->data.object.keys);
        free(v->data.object.values);
        free(v);
        return NULL;
    }
    return v;
}

LcnJsonValue *lcn_json_array_new(void)
{
    LcnJsonValue *v = alloc_value(LCN_JSON_ARRAY);
    if (!v) return NULL;
    v->data.array.cap = INIT_CAP;
    v->data.array.count = 0;
    v->data.array.items = (LcnJsonValue **)malloc(INIT_CAP * sizeof(LcnJsonValue *));
    if (!v->data.array.items) { free(v); return NULL; }
    return v;
}

LcnJsonValue *lcn_json_string_new(const char *s)
{
    if (!s) return NULL;
    LcnJsonValue *v = alloc_value(LCN_JSON_STRING);
    if (!v) return NULL;
    v->data.string.len = strlen(s);
    v->data.string.str = (char *)malloc(v->data.string.len + 1);
    if (!v->data.string.str) { free(v); return NULL; }
    memcpy(v->data.string.str, s, v->data.string.len + 1);
    return v;
}

LcnJsonValue *lcn_json_number_new(double n)
{
    LcnJsonValue *v = alloc_value(LCN_JSON_NUMBER);
    if (v) v->data.number = n;
    return v;
}

LcnJsonValue *lcn_json_bool_new(bool b)
{
    LcnJsonValue *v = alloc_value(LCN_JSON_BOOL);
    if (v) v->data.boolean = b;
    return v;
}

LcnJsonValue *lcn_json_null_new(void)
{
    return alloc_value(LCN_JSON_NULL);
}

void lcn_json_set(LcnJsonValue *obj, const char *key, LcnJsonValue *val)
{
    size_t i;
    if (!obj || obj->type != LCN_JSON_OBJECT || !key || !val) return;

    /* Check if key already exists */
    for (i = 0; i < obj->data.object.count; i++) {
        if (strcmp(obj->data.object.keys[i], key) == 0) {
            lcn_json_free(obj->data.object.values[i]);
            obj->data.object.values[i] = val;
            return;
        }
    }

    /* Grow if needed */
    if (obj->data.object.count >= obj->data.object.cap) {
        size_t newcap = obj->data.object.cap * 2;
        char **newkeys = (char **)realloc(
            obj->data.object.keys, newcap * sizeof(char *));
        LcnJsonValue **newvals = (LcnJsonValue **)realloc(
            obj->data.object.values, newcap * sizeof(LcnJsonValue *));
        if (!newkeys || !newvals) {
            if (newkeys) obj->data.object.keys = newkeys;
            if (newvals) obj->data.object.values = newvals;
            return; /* silently fail on OOM */
        }
        obj->data.object.keys = newkeys;
        obj->data.object.values = newvals;
        obj->data.object.cap = newcap;
    }

    obj->data.object.keys[obj->data.object.count] = (char *)malloc(strlen(key) + 1);
    if (!obj->data.object.keys[obj->data.object.count]) return;
    strcpy(obj->data.object.keys[obj->data.object.count], key);
    obj->data.object.values[obj->data.object.count] = val;
    obj->data.object.count++;
}

void lcn_json_set_string(LcnJsonValue *obj, const char *key, const char *val)
{
    LcnJsonValue *sv = lcn_json_string_new(val);
    if (sv) lcn_json_set(obj, key, sv);
}

void lcn_json_set_number(LcnJsonValue *obj, const char *key, double val)
{
    LcnJsonValue *nv = lcn_json_number_new(val);
    if (nv) lcn_json_set(obj, key, nv);
}

void lcn_json_set_bool(LcnJsonValue *obj, const char *key, bool val)
{
    LcnJsonValue *bv = lcn_json_bool_new(val);
    if (bv) lcn_json_set(obj, key, bv);
}

void lcn_json_array_push(LcnJsonValue *arr, LcnJsonValue *val)
{
    if (!arr || arr->type != LCN_JSON_ARRAY || !val) return;

    if (arr->data.array.count >= arr->data.array.cap) {
        size_t newcap = arr->data.array.cap * 2;
        LcnJsonValue **newitems = (LcnJsonValue **)realloc(
            arr->data.array.items, newcap * sizeof(LcnJsonValue *));
        if (!newitems) return;
        arr->data.array.items = newitems;
        arr->data.array.cap = newcap;
    }
    arr->data.array.items[arr->data.array.count++] = val;
}

/* -------------------------------------------------------------------------- */
/*  Public: Stringify                                                         */
/* -------------------------------------------------------------------------- */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} StrBuf;

static StrBuf strbuf_new(void)
{
    StrBuf sb;
    sb.cap = 256;
    sb.len = 0;
    sb.buf = (char *)malloc(sb.cap);
    if (sb.buf) sb.buf[0] = '\0';
    return sb;
}

static bool strbuf_append(StrBuf *sb, const char *data, size_t dlen)
{
    if (!sb->buf) return false;
    while (sb->len + dlen + 1 > sb->cap) {
        size_t newcap = sb->cap * 2;
        if (newcap < sb->len + dlen + 1) newcap = sb->len + dlen + 1;
        char *nb = (char *)realloc(sb->buf, newcap);
        if (!nb) { free(sb->buf); sb->buf = NULL; return false; }
        sb->buf = nb;
        sb->cap = newcap;
    }
    memcpy(sb->buf + sb->len, data, dlen);
    sb->len += dlen;
    sb->buf[sb->len] = '\0';
    return true;
}

static bool strbuf_append_str(StrBuf *sb, const char *s)
{
    return strbuf_append(sb, s, strlen(s));
}

static bool strbuf_append_char(StrBuf *sb, char c)
{
    return strbuf_append(sb, &c, 1);
}

static bool stringify_string(StrBuf *sb, const char *s, size_t len)
{
    size_t i;
    if (!strbuf_append_char(sb, '"')) return false;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  if (!strbuf_append_str(sb, "\\\"")) return false; break;
        case '\\': if (!strbuf_append_str(sb, "\\\\")) return false; break;
        case '\b': if (!strbuf_append_str(sb, "\\b"))  return false; break;
        case '\f': if (!strbuf_append_str(sb, "\\f"))  return false; break;
        case '\n': if (!strbuf_append_str(sb, "\\n"))  return false; break;
        case '\r': if (!strbuf_append_str(sb, "\\r"))  return false; break;
        case '\t': if (!strbuf_append_str(sb, "\\t"))  return false; break;
        default:
            if (c < 0x20) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", c);
                if (!strbuf_append_str(sb, esc)) return false;
            } else {
                if (!strbuf_append_char(sb, (char)c)) return false;
            }
            break;
        }
    }
    if (!strbuf_append_char(sb, '"')) return false;
    return true;
}

static bool stringify_value(StrBuf *sb, const LcnJsonValue *v)
{
    size_t i;
    if (!v) return strbuf_append_str(sb, "null");

    switch (v->type) {
    case LCN_JSON_NULL:
        return strbuf_append_str(sb, "null");

    case LCN_JSON_BOOL:
        return strbuf_append_str(sb, v->data.boolean ? "true" : "false");

    case LCN_JSON_NUMBER: {
        char num[64];
        double d = v->data.number;
        /* Print integer values without decimal point */
        if (d == (double)(long long)d && d >= -1e15 && d <= 1e15) {
            snprintf(num, sizeof(num), "%lld", (long long)d);
        } else {
            snprintf(num, sizeof(num), "%.17g", d);
        }
        return strbuf_append_str(sb, num);
    }

    case LCN_JSON_STRING:
        return stringify_string(sb, v->data.string.str, v->data.string.len);

    case LCN_JSON_ARRAY:
        if (!strbuf_append_char(sb, '[')) return false;
        for (i = 0; i < v->data.array.count; i++) {
            if (i > 0 && !strbuf_append_char(sb, ',')) return false;
            if (!stringify_value(sb, v->data.array.items[i])) return false;
        }
        return strbuf_append_char(sb, ']');

    case LCN_JSON_OBJECT:
        if (!strbuf_append_char(sb, '{')) return false;
        for (i = 0; i < v->data.object.count; i++) {
            if (i > 0 && !strbuf_append_char(sb, ',')) return false;
            if (!stringify_string(sb, v->data.object.keys[i],
                                  strlen(v->data.object.keys[i])))
                return false;
            if (!strbuf_append_char(sb, ':')) return false;
            if (!stringify_value(sb, v->data.object.values[i])) return false;
        }
        return strbuf_append_char(sb, '}');
    }

    return false;
}

char *lcn_json_stringify(const LcnJsonValue *v)
{
    StrBuf sb = strbuf_new();
    if (!sb.buf) return NULL;
    if (!stringify_value(&sb, v)) {
        free(sb.buf);
        return NULL;
    }
    return sb.buf;
}

/* -------------------------------------------------------------------------- */
/*  Public: Free                                                              */
/* -------------------------------------------------------------------------- */

void lcn_json_free(LcnJsonValue *v)
{
    size_t i;
    if (!v) return;

    switch (v->type) {
    case LCN_JSON_STRING:
        free(v->data.string.str);
        break;
    case LCN_JSON_ARRAY:
        for (i = 0; i < v->data.array.count; i++) {
            lcn_json_free(v->data.array.items[i]);
        }
        free(v->data.array.items);
        break;
    case LCN_JSON_OBJECT:
        for (i = 0; i < v->data.object.count; i++) {
            free(v->data.object.keys[i]);
            lcn_json_free(v->data.object.values[i]);
        }
        free(v->data.object.keys);
        free(v->data.object.values);
        break;
    case LCN_JSON_NULL:
    case LCN_JSON_BOOL:
    case LCN_JSON_NUMBER:
        break;
    }
    free(v);
}
