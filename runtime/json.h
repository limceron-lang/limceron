/*
 * Limceron JSON Parser/Serializer
 * Pure C99, no external dependencies.
 * Handles OpenAI-compatible API response parsing.
 */
#ifndef LCN_JSON_H
#define LCN_JSON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    LCN_JSON_NULL,
    LCN_JSON_BOOL,
    LCN_JSON_NUMBER,
    LCN_JSON_STRING,
    LCN_JSON_ARRAY,
    LCN_JSON_OBJECT
} LcnJsonType;

typedef struct LcnJsonValue LcnJsonValue;

struct LcnJsonValue {
    LcnJsonType type;
    union {
        bool boolean;
        double number;
        struct { char *str; size_t len; } string;
        struct { LcnJsonValue **items; size_t count; size_t cap; } array;
        struct {
            char **keys;
            LcnJsonValue **values;
            size_t count;
            size_t cap;
        } object;
    } data;
};

/* Parse JSON string. Returns NULL on error. Caller must free with lcn_json_free(). */
LcnJsonValue *lcn_json_parse(const char *json, size_t len);

/* Accessors */
const char    *lcn_json_get_string(const LcnJsonValue *v, const char *key);
double         lcn_json_get_number(const LcnJsonValue *v, const char *key);
bool           lcn_json_get_bool(const LcnJsonValue *v, const char *key);
LcnJsonValue *lcn_json_get(const LcnJsonValue *v, const char *key);
LcnJsonValue *lcn_json_array_get(const LcnJsonValue *v, size_t index);
size_t         lcn_json_array_len(const LcnJsonValue *v);

/* Builders */
LcnJsonValue *lcn_json_object_new(void);
LcnJsonValue *lcn_json_array_new(void);
LcnJsonValue *lcn_json_string_new(const char *s);
LcnJsonValue *lcn_json_number_new(double n);
LcnJsonValue *lcn_json_bool_new(bool b);
LcnJsonValue *lcn_json_null_new(void);
void           lcn_json_set(LcnJsonValue *obj, const char *key, LcnJsonValue *val);
void           lcn_json_set_string(LcnJsonValue *obj, const char *key, const char *val);
void           lcn_json_set_number(LcnJsonValue *obj, const char *key, double val);
void           lcn_json_set_bool(LcnJsonValue *obj, const char *key, bool val);
void           lcn_json_array_push(LcnJsonValue *arr, LcnJsonValue *val);

/* Serialize to string. Caller must free() result. */
char *lcn_json_stringify(const LcnJsonValue *v);

/* Free a JSON value tree */
void lcn_json_free(LcnJsonValue *v);

#endif /* LCN_JSON_H */
