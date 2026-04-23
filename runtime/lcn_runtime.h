/*
 * Limceron Runtime — Consolidated Header
 *
 * This is the single header that generated C code includes.
 * It provides all types, macros, and forward declarations needed
 * by Limceron-generated C output.
 *
 * Generated code emits:  #include "lcn_runtime.h"
 * Compile with:          cc -I<runtime-dir> generated.c runtime objects
 */

#ifndef LCN_RUNTIME_H
#define LCN_RUNTIME_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ════════════════════════════════════════════════
 * Limceron Runtime Types
 * ════════════════════════════════════════════════ */

#ifndef LCN_STRING_TYPEDEF
#define LCN_STRING_TYPEDEF
typedef const char *LcnString;
#endif

#ifndef LCN_RESULT_TYPEDEF
#define LCN_RESULT_TYPEDEF
typedef struct {
    bool ok;
    void *value;
    LcnString error;
} LcnResult;
#endif

#define LCN_OK   ((LcnResult){ .ok = true,  .value = NULL, .error = NULL })
#define LCN_ERR(msg) ((LcnResult){ .ok = false, .value = NULL, .error = (msg) })

typedef struct {
    void **items;
    int32_t len;
    int32_t cap;
} LcnVec;

static inline LcnVec lcn_vec_new(void) {
    LcnVec v = { NULL, 0, 0 };
    return v;
}

static inline void lcn_vec_push(LcnVec *v, void *item) {
    if (v->len >= v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = (void **)realloc(v->items, sizeof(void *) * (size_t)v->cap);
    }
    v->items[v->len++] = item;
}

static inline void *lcn_vec_get(LcnVec *v, int32_t index) {
    if (!v || index < 0 || index >= v->len) return NULL;
    return v->items[index];
}

static inline int32_t lcn_vec_len(LcnVec *v) {
    return v ? v->len : 0;
}

typedef struct {
    bool has_value;
    int64_t value;
} LcnOption;

#ifndef LCN_CAPABILITY_DEFINED
#define LCN_CAPABILITY_DEFINED
typedef uint64_t LcnCapability;
#endif

/* Capability delegation (Hurd-inspired) */
#include "delegation.h"

/* Runtime capability fence — tool dispatch interception */
#include "capability_fence.h"

/* LcnBudget — must match budget.h layout for linking */
#include <time.h>
typedef struct {
    int64_t max_tokens;
    double  max_cost;
    int64_t max_duration_secs;
    int64_t used_tokens;
    double  used_cost;
    time_t  start_time;
    bool    exhausted;
    const char *exhausted_reason;
} LcnBudget;

typedef enum {
    LCN_STRATEGY_ONE_FOR_ONE,
    LCN_STRATEGY_REST_FOR_ALL,
    LCN_STRATEGY_ONE_FOR_ALL,
} LcnSupervisorStrategy;

/* ════════════════════════════════════════════════
 * Supervisor Runtime
 * ════════════════════════════════════════════════ */

#include "supervisor.h"

/* ════════════════════════════════════════════════
 * JSON Parser
 * ════════════════════════════════════════════════ */

#include "json.h"

/* ════════════════════════════════════════════════
 * Runtime Function Declarations
 * ════════════════════════════════════════════════ */

/* Budget */
LcnBudget lcn_budget_new(int64_t max_tokens, double max_cost, int64_t max_duration_secs);
bool lcn_budget_check_runtime(LcnBudget *b);
bool lcn_budget_deduct_tokens(LcnBudget *b, int64_t tokens);
bool lcn_budget_deduct_cost(LcnBudget *b, double cost);

/* LLM Inference */
typedef struct {
    char    *content;
    int64_t  prompt_tokens;
    int64_t  completion_tokens;
    int64_t  total_tokens;
    double   entropy;       /* Shannon entropy of first token logprobs */
    double   confidence;    /* 1.0 - normalized_entropy [0.0, 1.0] */
    bool     ok;
    char    *error;
} LcnLlmResult;

LcnLlmResult lcn_llm_call(const char *endpoint, const char *model,
                             const char *system_prompt, const char *user_message,
                             LcnBudget *budget, const char *api_key);

/* Local ONNX model registration for agents with endpoint:"local" */
struct LcnModel;
void lcn_set_local_model(struct LcnModel *m);
struct LcnModel *lcn_get_local_model(void);

/* ════════════════════════════════════════════════
 * LLM Output — Typed ADT for ask() results
 * Forces exhaustive handling of LLM response variants.
 * ════════════════════════════════════════════════ */

typedef enum {
    LCN_LLM_OUTPUT_OK,         /* Structured/expected text response */
    LCN_LLM_OUTPUT_TEXT,       /* Free text (no schema match) */
    LCN_LLM_OUTPUT_TOOL_CALL,  /* Model requested tool execution */
    LCN_LLM_OUTPUT_ERROR       /* Timeout, refusal, rate limit, etc. */
} LcnLlmOutputKind;

typedef struct {
    LcnLlmOutputKind kind;
    LcnString content;     /* For OK and TEXT: the response text */
    LcnString tool_name;   /* For TOOL_CALL: which tool */
    LcnString tool_args;   /* For TOOL_CALL: arguments JSON */
    LcnString error;       /* For ERROR: error description */

    /* Entropy-aware fields (Shannon entropy of LLM response) */
    double    entropy;     /* H = -sum(p * log2(p)), raw Shannon entropy */
    double    confidence;  /* 1.0 - normalized_entropy, range [0.0, 1.0] */
} LcnLlmOutput;

/* Constructors */
static inline LcnLlmOutput lcn_llm_output_ok(LcnString content) {
    LcnLlmOutput o = {0};
    o.kind = LCN_LLM_OUTPUT_OK;
    o.content = content;
    return o;
}

static inline LcnLlmOutput lcn_llm_output_text(LcnString content) {
    LcnLlmOutput o = {0};
    o.kind = LCN_LLM_OUTPUT_TEXT;
    o.content = content;
    return o;
}

static inline LcnLlmOutput lcn_llm_output_tool_call(LcnString name, LcnString args) {
    LcnLlmOutput o = {0};
    o.kind = LCN_LLM_OUTPUT_TOOL_CALL;
    o.tool_name = name;
    o.tool_args = args;
    return o;
}

static inline LcnLlmOutput lcn_llm_output_error(LcnString error) {
    LcnLlmOutput o = {0};
    o.kind = LCN_LLM_OUTPUT_ERROR;
    o.error = error;
    return o;
}

/* Convenience: extract text content (returns "" on non-text variants) */
static inline LcnString lcn_llm_output_unwrap(LcnLlmOutput o) {
    if (o.kind == LCN_LLM_OUTPUT_OK || o.kind == LCN_LLM_OUTPUT_TEXT)
        return o.content ? o.content : "";
    return "";
}

/* MCP Tool Dispatch */
typedef struct {
    char *result_json;
    bool  ok;
    char *error;
} LcnMcpResult;

LcnMcpResult lcn_mcp_dispatch(const char *server_command, const char *tool_name,
                                 const char *args_json);

/* Channel — typed, thread-safe ring buffer (see channel.h) */
#ifndef LCN_CHANNEL_TYPEDEF
#define LCN_CHANNEL_TYPEDEF
typedef struct LcnChannel LcnChannel;
#endif
LcnChannel *lcn_channel_new(int capacity, size_t elem_size);
bool lcn_channel_send(LcnChannel *ch, const void *data);
bool lcn_channel_recv(LcnChannel *ch, void *data);
bool lcn_channel_try_recv(LcnChannel *ch, void *data);
void lcn_channel_close(LcnChannel *ch);
bool lcn_channel_is_closed(LcnChannel *ch);
int  lcn_channel_len(LcnChannel *ch);
void lcn_channel_free(LcnChannel *ch);

/* ════════════════════════════════════════════════
 * Agent Memory (SQLite-backed)
 * ════════════════════════════════════════════════ */

#include "memory.h"

/* ════════════════════════════════════════════════
 * Knowledge Base (FTS5 RAG)
 * ════════════════════════════════════════════════ */

#include "kb.h"

/* ════════════════════════════════════════════════
 * Event Instrumentation
 * ════════════════════════════════════════════════ */

#include "event.h"

/* ════════════════════════════════════════════════
 * Dashboard Server
 * ════════════════════════════════════════════════ */

#include "httpd.h"
#include "dashboard_api.h"

/* ════════════════════════════════════════════════
 * Thread Pool (spawn/await)
 * ════════════════════════════════════════════════ */

#include "threads.h"

/* ════════════════════════════════════════════════
 * Green Threads (M:N Scheduler)
 * ════════════════════════════════════════════════ */

#include "green_threads.h"

/* ════════════════════════════════════════════════
 * Mesh Pipeline (parallel fan-out / fan-in)
 * ════════════════════════════════════════════════ */

#include "mesh.h"

/* ════════════════════════════════════════════════
 * Select Multiplexing
 * ════════════════════════════════════════════════ */

#include "select.h"

/* ════════════════════════════════════════════════
 * String Utilities
 * ════════════════════════════════════════════════ */

#include "string_utils.h"

/* ════════════════════════════════════════════════
 * MCP Server (agent-as-service)
 * ════════════════════════════════════════════════ */

#include "mcp_server.h"

/* ════════════════════════════════════════════════
 * Standard Library Runtime
 * ════════════════════════════════════════════════ */

#include "stdlib_rt.h"
#include "mysql_driver.h"
#include "postgres_driver.h"

/* ════════════════════════════════════════════════
 * ONNX Model Binding (local inference)
 * ════════════════════════════════════════════════ */

#include "onnx_model.h"

/* ════════════════════════════════════════════════
 * Entropy Tracking & Drift Detection
 * ════════════════════════════════════════════════ */

#include "entropy.h"
#include "drift.h"

/* ════════════════════════════════════════════════
 * Inference Router (health checking + selection)
 * ════════════════════════════════════════════════ */

#include "router.h"

/* ════════════════════════════════════════════════
 * A2A (Agent-to-Agent) Protocol Client
 * ════════════════════════════════════════════════ */

#include "a2a.h"

/* ════════════════════════════════════════════════
 * Ask Helper — convenience wrapper for LLM calls
 * ════════════════════════════════════════════════ */

/* Legacy ask — returns plain string (backward compatible) */
static inline LcnString lcn_ask(const char *model, const char *prompt,
                                 const char *question, LcnBudget *budget) {
    LcnLlmResult r = lcn_llm_call(NULL, model, prompt, question, budget, NULL);
    if (r.ok && r.content) return r.content;
    free(r.error);
    return "";
}

/* unwrap(Result) → extract string value, or "" on error */
static inline LcnString lcn_unwrap(LcnResult r) {
    if (r.ok && r.value) return (LcnString)r.value;
    if (r.error) fprintf(stderr, "unwrap error: %s\n", r.error);
    return "";
}

/* Typed ask — returns LcnLlmOutput ADT (new: forces exhaustive handling)
 * policy: if non-NULL, the resolved endpoint URL is checked against
 * the LcnAccessPolicy before sending the HTTP request. */
static inline LcnLlmOutput lcn_ask_typed(const char *endpoint, const char *model,
                                          const char *prompt, const char *question,
                                          LcnBudget *budget,
                                          const void *policy,
                                          const char *api_key) {
    /* Resolve endpoint the same way lcn_llm_call does */
    const char *ep = endpoint;
    if (!ep) {
        ep = getenv("LCN_LLM_ENDPOINT");
        if (!ep) ep = getenv("OLLAMA_HOST");
        if (!ep) ep = "http://localhost:11434";
    }

    /* Enforce network policy on the LLM endpoint URL */
    if (policy) {
        /* Build the full URL that lcn_llm_call will POST to */
        char _url[1024];
        size_t _elen = strlen(ep);
        int _strip = (_elen > 0 && ep[_elen - 1] == '/') ? 1 : 0;
        snprintf(_url, sizeof(_url), "%.*s/v1/chat/completions",
                 (int)(_elen - (size_t)_strip), ep);
        /* Cast to LcnAccessPolicy* — defined later in this header */
        typedef struct { const void *ep; const void *bi; const void *pa;
                         bool dp; bool dd; } LcnAP_;
        const LcnAP_ *p = (const LcnAP_ *)policy;
        /* Check endpoint against policy rules */
        bool _denied = false;
        /* Parse host:port from URL */
        const char *_hp = _url;
        if (strncmp(_hp, "https://", 8) == 0) _hp += 8;
        else if (strncmp(_hp, "http://", 7) == 0) _hp += 7;
        char _host[256]; int _port = 80;
        {
            const char *_sl = strchr(_hp, '/');
            const char *_co = strchr(_hp, ':');
            if (_co && (!_sl || _co < _sl)) {
                size_t _hl = (size_t)(_co - _hp);
                if (_hl >= sizeof(_host)) _hl = sizeof(_host) - 1;
                memcpy(_host, _hp, _hl); _host[_hl] = 0;
                _port = atoi(_co + 1);
            } else if (_sl) {
                size_t _hl = (size_t)(_sl - _hp);
                if (_hl >= sizeof(_host)) _hl = sizeof(_host) - 1;
                memcpy(_host, _hp, _hl); _host[_hl] = 0;
            } else {
                size_t _hl = strlen(_hp);
                if (_hl >= sizeof(_host)) _hl = sizeof(_host) - 1;
                memcpy(_host, _hp, _hl); _host[_hl] = 0;
            }
        }
        if (strncmp(_url, "https://", 8) == 0 && _port == 80) _port = 443;
        /* Check deny_private */
        if (p->dp) {
            unsigned _a = 0, _b = 0, _c = 0, _d = 0;
            if (sscanf(_host, "%u.%u.%u.%u", &_a, &_b, &_c, &_d) == 4) {
                if (_a == 10 || (_a == 172 && _b >= 16 && _b <= 31) ||
                    (_a == 192 && _b == 168) || _a == 127 ||
                    (_a == 169 && _b == 254)) _denied = true;
            }
        }
        /* Check endpoint rules */
        if (!_denied && p->ep && p->dd) {
            typedef struct { const char *h; int po; bool al; const char *pg; } EPR_;
            const EPR_ *_r = (const EPR_ *)p->ep;
            bool _found = false;
            while (_r->h) {
                if (strcmp(_r->h, _host) == 0 && (_r->po == 0 || _r->po == _port)) {
                    _found = _r->al;
                    break;
                }
                _r++;
            }
            if (!_found) _denied = true;
        }
        if (_denied) {
            char _ebuf[512];
            snprintf(_ebuf, sizeof(_ebuf),
                     "access denied: LLM endpoint '%s:%d' blocked by network policy",
                     _host, _port);
            fprintf(stderr, "SECURITY: %s\n", _ebuf);
            return lcn_llm_output_error(_ebuf);
        }
    }

    LcnLlmResult r = lcn_llm_call(endpoint, model, prompt, question, budget, api_key);
    if (!r.ok) {
        LcnString err = r.error ? r.error : "unknown error";
        return lcn_llm_output_error(err);
    }
    if (r.content) {
        LcnLlmOutput out = lcn_llm_output_ok(r.content);
        out.entropy = r.entropy;
        out.confidence = r.confidence;
        return out;
    }
    return lcn_llm_output_error("empty response");
}

/* ════════════════════════════════════════════════
 * Access Control Policy (network + binary)
 * ════════════════════════════════════════════════ */

typedef struct {
    const char *host;
    int         port;
    bool        allow;
    const char *path_glob;
} LcnEndpointRule;

typedef struct {
    const char *path;
    bool        allow;
} LcnBinaryRule;

typedef struct {
    const char *pattern;
    bool        allow;
    bool        can_read;
    bool        can_write;
} LcnPathRule;

typedef struct {
    const LcnEndpointRule *endpoints;   /* NULL-terminated array */
    const LcnBinaryRule   *binaries;    /* NULL-terminated array */
    const LcnPathRule     *paths;       /* NULL-terminated array */
    bool                   deny_private;
    bool                   default_deny;
} LcnAccessPolicy;

LcnResult    lcn_fetch_checked(const char *url, const LcnAccessPolicy *policy);
LcnResult    lcn_exec_checked(const char *command, const LcnAccessPolicy *policy);
LcnString    lcn_read_file_checked(const char *path, const LcnAccessPolicy *policy);
bool         lcn_write_file_checked(const char *path, const char *content,
                                    const LcnAccessPolicy *policy);

/* ════════════════════════════════════════════════
 * CLI Args Globals
 * ════════════════════════════════════════════════ */

static int _lcn_argc = 0;
static char **_lcn_argv = NULL;

/* ════════════════════════════════════════════════
 * String Builder
 * ════════════════════════════════════════════════ */

/* ════════════════════════════════════════════════
 * Closure (function pointer + captured environment)
 * ════════════════════════════════════════════════ */

typedef struct {
    void *fn;      /* function pointer */
    void *env;     /* captured environment (heap-allocated struct) */
} LcnClosure;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} LcnStringBuilder;

static inline void *lcn_sb_new(void) {
    LcnStringBuilder *sb = (LcnStringBuilder *)calloc(1, sizeof(LcnStringBuilder));
    sb->cap = 256;
    sb->data = (char *)malloc(sb->cap);
    sb->data[0] = '\0';
    return sb;
}

static inline void lcn_sb_append(void *handle, LcnString s) {
    LcnStringBuilder *sb = (LcnStringBuilder *)handle;
    if (!sb || !s) return;
    size_t slen = strlen(s);
    while (sb->len + slen + 1 > sb->cap) {
        sb->cap *= 2;
        sb->data = (char *)realloc(sb->data, sb->cap);
    }
    memcpy(sb->data + sb->len, s, slen);
    sb->len += slen;
    sb->data[sb->len] = '\0';
}

static inline LcnString lcn_sb_to_string(void *handle) {
    LcnStringBuilder *sb = (LcnStringBuilder *)handle;
    if (!sb) return "";
    char *result = sb->data;
    free(sb);
    return result;
}

/* Non-destructive peek — returns a copy of the current string without freeing the builder */
static inline LcnString lcn_sb_peek(void *handle) {
    LcnStringBuilder *sb = (LcnStringBuilder *)handle;
    if (!sb) return "";
    char *copy = (char *)malloc(sb->len + 1);
    memcpy(copy, sb->data, sb->len);
    copy[sb->len] = '\0';
    return copy;
}

#endif /* LCN_RUNTIME_H */
