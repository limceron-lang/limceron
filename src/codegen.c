/*
 * Limceron Compiler — Code Generator (Limceron→C Transpiler)
 *
 * Walks the AST and emits C99 code. The output is a single .c file
 * that can be compiled with any C99 compiler (gcc, clang).
 *
 * Strategy:
 *   1. Emit runtime preamble (types, helpers)
 *   2. Generate code for each declaration in order
 *   3. Generate main() entry point
 */

#include "lcn.h"

/* ============================================================
 * Builtin Function Names (skip codegen for these — runtime provides them)
 * ============================================================ */

static bool is_codegen_builtin(const char *name) {
    static const char *builtins[] = {
        "print", "println", "len", "contains", "starts_with", "ends_with",
        "env", "env_or", "str_eq", "str_replace", "str_trim", "str_substring",
        "to_string", "to_int", "split", "format",
        "json_parse", "json_get", "json_get_number", "json_array_len",
        "json_array_get", "json_stringify",
        "abs", "min", "max", "clamp", "floor", "ceil", "round",
        "abs_f", "min_f", "max_f",
        "now_ms", "sleep_ms", "elapsed_ms", "format_timestamp",
        "log_info", "log_warn", "log_error", "log_debug",
        "batch_size", "batch_offset", "batch_dry_run", "batch_progress",
        "budget_tokens_used", "budget_tokens_left", "budget_cost_used",
        "budget_cost_left", "budget_percentage", "budget_elapsed_ms",
        "estimate_tokens", "fits_in_budget",
        "trace_begin", "trace_end", "trace_tag", "trace_event",
        "read_file", "write_file", "read_line",
        "retry_ask", "retry_ask_backoff",
        "args", "sql_escape", "unwrap",
        "char_at", "char_code", "str_from_code", "str_slice",
        "str_len", "str_find", "str_char_is_alpha", "str_char_is_digit",
        "str_char_is_alnum", "to_char_code",
        "arg", "arg_count",
        "sb_new", "sb_append", "sb_to_string", "sb_peek",
        "delegate", "revoke", "revoke_all", "has_capability",
        "secret_redact", "secret_unwrap",
        "vec_pop",
        NULL
    };
    if (!name) return false;
    for (int i = 0; builtins[i]; i++) {
        if (strcmp(name, builtins[i]) == 0) return true;
    }
    return false;
}

/* ============================================================
 * Static Call Defense-in-Depth
 *
 * Any call matching a known sensitive pattern must be routed
 * through the capability fence when inside an agent method.
 * ============================================================ */

static bool is_sensitive_call(const char *name) {
    static const char *sensitive[] = {
        "http_get", "http_post", "http_put", "http_delete",
        "read_file", "write_file",
        "execute", "shell", "shell_exec",
        "db_query", "db_execute",
        "send", "send_message",
        "fetch", "exec",
        NULL
    };
    if (!name) return false;
    for (int i = 0; sensitive[i]; i++) {
        if (strcmp(name, sensitive[i]) == 0) return true;
    }
    return false;
}

/* ============================================================
 * String Builder
 * ============================================================ */

#define CG_INITIAL_CAP (64 * 1024)
#define CG_MAX_CAPS    256

typedef struct {
    const char *group;
    const char *name;
    int         bit;
} CapEntry;

typedef struct {
    char   *buf;
    size_t  len;
    size_t  cap;
    int     indent;
    Arena  *arena;

    /* Capability registry */
    CapEntry caps[CG_MAX_CAPS];
    int      cap_count;
    int      next_cap_bit;

    /* Tool registry (for call rewriting, type inference, and capability fence) */
    struct {
        const char *name;
        AstNode    *return_type;  /* AST_TYPE_* node */
        const char *requires[16]; /* required capability names (qualified) */
        int         requires_count;
    } tools[256];
    int         tool_count;

    /* Agent name registry */
    const char *agent_names[64];
    int         agent_count;

    /* Agent method registry (for self-call rewriting) */
    struct {
        const char *agent_name;
        const char *method_name;
    } agent_methods[256];
    int         agent_method_count;

    /* Budget name registry */
    const char *budget_names[64];
    int         budget_count;

    /* Impl method registry (for struct method dispatch) */
    struct {
        const char *type_name;
        const char *method_name;
    } impl_methods[256];
    int         impl_method_count;

    /* Variable-to-agent-type tracking (for method call rewriting) */
    struct {
        const char *var_name;
        const char *agent_name;
    } var_types[256];
    int         var_type_count;

    /* Agent method context */
    bool     in_agent_method;
    const char *current_agent_name;

    /* Impl method context */
    bool     in_impl_method;
    const char *current_impl_type;

    /* Track if source has fn main */
    bool     has_main;
    bool     main_returns_void; /* true if fn main() has no return type */

    /* Build mode: use #include instead of inline preamble */
    bool     use_runtime_header;
    bool     serve_mode; /* Generate MCP server main instead of normal main */
    bool     use_green_threads; /* Use green threads instead of pthreads for spawn */

    /* Default agent for main generation (first agent with prompt + llm) */
    const char *default_agent_name;

    /* Agents with memory: true */
    const char *memory_agents[64];
    int         memory_agent_count;

    /* Knowledge base config (from first agent with knowledge:) */
    bool        has_kb;
    const char *kb_path;
    int         kb_chunk_size;
    int         kb_chunk_overlap;

    /* MCP alias registry (for method call rewriting) */
    struct {
        const char *alias;   /* e.g. "db" */
    } mcp_aliases[32];
    int         mcp_alias_count;

    /* Model alias registry (for method call rewriting: clf.predict → lcn_model_clf_predict) */
    struct {
        const char *alias;   /* e.g. "clf" */
    } model_aliases[32];
    int         model_alias_count;

    /* Access control: name of first "use" policy group for current agent */
    const char *current_access_policy;

    /* String variable tracking for concat detection (BUG-2 fix) */
    const char *string_vars[256];
    int         string_var_count;

    /* User-defined function return type registry (for type inference at call sites) */
    struct {
        const char *fn_name;
        const char *ret_type;     /* C type name, e.g. "Point", "int64_t" */
    } fn_ret_types[256];
    int         fn_ret_type_count;

    /* Enum registry: tracks which enums exist and whether they have data (ADTs) */
    struct {
        const char *name;         /* enum name, e.g. "Token" */
        bool        has_data;     /* true if any variant has fields */
        AstNode    *node;         /* pointer to AST_ENUM node for field lookup */
    } enums[128];
    int         enum_count;

    /* Struct name registry (for recognizing struct types) */
    const char *struct_names[128];
    int         struct_name_count;

    /* Defer stack: LIFO deferred expressions per scope */
    AstNode    *defer_stack[256];
    int         defer_depth;

    /* Invariant registry: names of emitted invariant functions */
    const char *invariant_names[64];
    int         invariant_count;

    /* Closure codegen state */
    int         closure_counter;

    /* Spawn counter for unique spawn function names */
    int         spawn_counter;

    /* Closure variable tracking (for detecting closure calls) */
    const char *closure_vars[128];
    int         closure_var_count;

    /* Deferred closure definitions (emitted before the function that uses them) */
    char       *closure_defs_buf;
    size_t      closure_defs_len;
    size_t      closure_defs_cap;

    /* Monomorphization registry: tracks emitted generic type specializations */
    #define MONO_MAX 128
    struct {
        char name[128];     /* e.g. "Result_LcnString_LcnString" */
        char base[16];      /* "Result" or "Option" */
        AstNode *generics;  /* pointer to the generics linked list from the AST */
        bool emitted;       /* whether typedef has been emitted */
    } mono[MONO_MAX];
    int         mono_count;

    /* Current function's return type AST node (for Ok/Err constructor rewriting) */
    AstNode *current_fn_ret_type;

    /* Capability fence: pending agent name for fence injection at block start */
    const char *pending_fence_agent;

    /* Capability fence: per-agent allowed tool lists (populated during agent emit) */
    struct {
        const char *agent_name;
        const char *tool_names[256];
        int         tool_name_count;
    } agent_tool_lists[64];
    int         agent_tool_list_count;

    /* Cross-compilation target (NULL/zero = native) */
    const LcnTarget *target;

    /* Supervisor registry: tracks supervisor names and their children for startup wiring */
    struct {
        const char *name;
        int strategy;       /* 0=one_for_one, 1=rest_for_all, 2=one_for_all */
        int max_restarts;
        int window_seconds;
        const char *child_names[64];
        int child_count;
    } supervisors[32];
    int supervisor_count;

    /* FFI link directive: collected linker flags from AST_LINK nodes */
    const char *link_flags[64];
    int         link_flag_count;

    /* Interface/trait registry (for vtable codegen and impl validation) */
    struct {
        const char *name;           /* interface/trait name */
        AstNode    *node;           /* AST_INTERFACE or AST_TRAIT node */
    } interfaces[64];
    int         interface_count;

    /* Union type registry (for tagged union codegen) */
    struct {
        const char *name;           /* type alias name */
        AstNode    *type_node;      /* AST_TYPE_UNION node */
    } union_types[64];
    int         union_type_count;

    /* Prometheus metrics: collected from AST_METRICS node */
    bool        has_metrics;
    int         metrics_port;       /* port for /metrics endpoint (default 9091) */
    struct {
        const char *name;           /* metric name (e.g. "processed_total") */
        const char *description;    /* help text */
        int         kind;           /* 0=counter, 1=gauge, 2=histogram */
    } metrics[64];
    int         metrics_count;

    /* Progress reporting: collected from AST_PROGRESS node */
    bool        has_progress;
    const char *progress_total_var;    /* identifier name for total (e.g. "count") */
    const char *progress_current_var;  /* identifier name for current (e.g. "processed") */
    /* Health probe: collected from AST_HEALTH node */
    bool        has_health;
    int         health_port;        /* port for /healthz and /readyz endpoints */
} CodeGen;

static void cg_grow(CodeGen *g, size_t need) {
    while (g->len + need + 1 > g->cap) {
        g->cap *= 2;
        g->buf = (char *)realloc(g->buf, g->cap);
    }
}

static void cg_raw(CodeGen *g, const char *s, size_t n) {
    cg_grow(g, n);
    memcpy(g->buf + g->len, s, n);
    g->len += n;
    g->buf[g->len] = '\0';
}

static void cg_str(CodeGen *g, const char *s) {
    if (s) cg_raw(g, s, strlen(s));
}

static void cg_fmt(CodeGen *g, const char *fmt, ...) {
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) cg_raw(g, tmp, (size_t)n);
}

static void cg_indent(CodeGen *g) {
    int i;
    for (i = 0; i < g->indent; i++) cg_raw(g, "    ", 4);
}

static void cg_line(CodeGen *g, const char *fmt, ...) {
    cg_indent(g);
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) cg_raw(g, tmp, (size_t)n);
    cg_raw(g, "\n", 1);
}

static void cg_nl(CodeGen *g) { cg_raw(g, "\n", 1); }

/* ============================================================
 * Defer Stack (LIFO scope-exit execution)
 * ============================================================ */

/* Push a deferred expression onto the stack */
static void cg_defer_push(CodeGen *g, AstNode *expr) {
    if (g->defer_depth < 256)
        g->defer_stack[g->defer_depth++] = expr;
}

/* Forward-declare cg_expr for use in defer emit */
static void cg_expr(CodeGen *g, AstNode *expr);

/* Emit all deferred expressions from 'saved_depth' to current depth in LIFO order, then restore */
static void cg_defer_emit_from(CodeGen *g, int saved_depth) {
    int i;
    for (i = g->defer_depth - 1; i >= saved_depth; i--) {
        cg_indent(g);
        cg_str(g, "/* defer */ ");
        cg_expr(g, g->defer_stack[i]);
        cg_str(g, ";\n");
    }
    g->defer_depth = saved_depth;
}

/* ============================================================
 * Capability Registry
 * ============================================================ */

static void cg_register_cap(CodeGen *g, const char *group, const char *name) {
    if (g->cap_count >= CG_MAX_CAPS) return;
    CapEntry *e = &g->caps[g->cap_count++];
    e->group = group;
    e->name = name;
    e->bit = g->next_cap_bit++;
}

/* Build CAP_GROUP_NAME string (uppercase). Returns arena-allocated string. */
static const char *cg_cap_define(CodeGen *g, const char *group, const char *name) {
    char tmp[256];
    int i, o = 0;
    tmp[o++] = 'C'; tmp[o++] = 'A'; tmp[o++] = 'P'; tmp[o++] = '_';
    for (i = 0; group[i] && o < 240; i++)
        tmp[o++] = (group[i] >= 'a' && group[i] <= 'z') ? group[i] - 32 : group[i];
    tmp[o++] = '_';
    for (i = 0; name[i] && o < 254; i++)
        tmp[o++] = (name[i] >= 'a' && name[i] <= 'z') ? name[i] - 32 : name[i];
    tmp[o] = '\0';
    return arena_strdup(g->arena, tmp);
}

/* Lookup a capability reference like "web.search" and return its define name */
static const char *cg_lookup_cap(CodeGen *g, const char *qualified) {
    /* Split on '.' */
    const char *dot = strchr(qualified, '.');
    if (!dot) return NULL;
    size_t glen = (size_t)(dot - qualified);
    const char *name = dot + 1;
    int i;
    for (i = 0; i < g->cap_count; i++) {
        if (strlen(g->caps[i].group) == glen &&
            strncmp(g->caps[i].group, qualified, glen) == 0 &&
            strcmp(g->caps[i].name, name) == 0) {
            return cg_cap_define(g, g->caps[i].group, g->caps[i].name);
        }
    }
    /* Not found — generate uppercase anyway */
    char group_buf[128];
    if (glen >= sizeof(group_buf)) glen = sizeof(group_buf) - 1;
    memcpy(group_buf, qualified, glen);
    group_buf[glen] = '\0';
    return cg_cap_define(g, group_buf, name);
}

/* ============================================================
 * Agent / Budget Registries
 * ============================================================ */

static void cg_register_agent(CodeGen *g, const char *name) {
    if (g->agent_count < 64) g->agent_names[g->agent_count++] = name;
}

static const char *cg_lookup_agent(CodeGen *g, const char *name) {
    int i;
    for (i = 0; i < g->agent_count; i++)
        if (strcmp(g->agent_names[i], name) == 0) return name;
    return NULL;
}

static void cg_register_budget(CodeGen *g, const char *name) {
    if (g->budget_count < 64) g->budget_names[g->budget_count++] = name;
}

static const char *cg_lookup_budget(CodeGen *g, const char *name) {
    int i;
    for (i = 0; i < g->budget_count; i++)
        if (strcmp(g->budget_names[i], name) == 0) return name;
    return NULL;
}

static void cg_register_agent_method(CodeGen *g, const char *agent_name, const char *method_name) {
    if (g->agent_method_count < 256) {
        g->agent_methods[g->agent_method_count].agent_name = agent_name;
        g->agent_methods[g->agent_method_count].method_name = method_name;
        g->agent_method_count++;
    }
}

static bool cg_is_self_method(CodeGen *g, const char *agent_name, const char *fn_name) {
    int i;
    for (i = 0; i < g->agent_method_count; i++)
        if (strcmp(g->agent_methods[i].agent_name, agent_name) == 0 &&
            strcmp(g->agent_methods[i].method_name, fn_name) == 0)
            return true;
    return false;
}

static void cg_track_var(CodeGen *g, const char *var_name, const char *agent_name) {
    if (g->var_type_count < 256) {
        g->var_types[g->var_type_count].var_name = var_name;
        g->var_types[g->var_type_count].agent_name = agent_name;
        g->var_type_count++;
    }
}

static const char *cg_var_agent_type(CodeGen *g, const char *var_name) {
    int i;
    for (i = 0; i < g->var_type_count; i++)
        if (strcmp(g->var_types[i].var_name, var_name) == 0)
            return g->var_types[i].agent_name;
    return NULL;
}

/* ============================================================
 * Function Return Type Registry
 * ============================================================ */

static void cg_register_fn_ret(CodeGen *g, const char *fn_name, const char *ret_type) {
    if (!fn_name || !ret_type) return;
    if (g->fn_ret_type_count < 256) {
        g->fn_ret_types[g->fn_ret_type_count].fn_name = fn_name;
        g->fn_ret_types[g->fn_ret_type_count].ret_type = ret_type;
        g->fn_ret_type_count++;
    }
}

static const char *cg_lookup_fn_ret(CodeGen *g, const char *fn_name) {
    int i;
    for (i = 0; i < g->fn_ret_type_count; i++)
        if (strcmp(g->fn_ret_types[i].fn_name, fn_name) == 0)
            return g->fn_ret_types[i].ret_type;
    return NULL;
}

/* ============================================================
 * Enum / Struct Registries
 * ============================================================ */

static void cg_register_enum(CodeGen *g, const char *name, bool has_data, AstNode *node) {
    if (!name || g->enum_count >= 128) return;
    g->enums[g->enum_count].name = name;
    g->enums[g->enum_count].has_data = has_data;
    g->enums[g->enum_count].node = node;
    g->enum_count++;
}

static int cg_lookup_enum(CodeGen *g, const char *name) {
    int i;
    for (i = 0; i < g->enum_count; i++)
        if (strcmp(g->enums[i].name, name) == 0) return i;
    return -1;
}

static void cg_register_struct(CodeGen *g, const char *name) {
    if (!name || g->struct_name_count >= 128) return;
    g->struct_names[g->struct_name_count++] = name;
}

static bool cg_is_struct(CodeGen *g, const char *name) {
    int i;
    for (i = 0; i < g->struct_name_count; i++)
        if (strcmp(g->struct_names[i], name) == 0) return true;
    return false;
}

/* ============================================================
 * Closure Variable Tracking
 * ============================================================ */

static void cg_register_closure_var(CodeGen *g, const char *name) {
    if (!name || g->closure_var_count >= 128) return;
    g->closure_vars[g->closure_var_count++] = name;
}

static bool cg_is_closure_var(CodeGen *g, const char *name) {
    int i;
    if (!name) return false;
    for (i = 0; i < g->closure_var_count; i++)
        if (strcmp(g->closure_vars[i], name) == 0) return true;
    return false;
}

/* Append to the deferred closure definitions buffer */
static void cg_closure_def_append(CodeGen *g, const char *s) {
    size_t slen = strlen(s);
    if (!g->closure_defs_buf) {
        g->closure_defs_cap = 4096;
        g->closure_defs_buf = (char *)malloc(g->closure_defs_cap);
        g->closure_defs_buf[0] = '\0';
        g->closure_defs_len = 0;
    }
    while (g->closure_defs_len + slen + 1 > g->closure_defs_cap) {
        g->closure_defs_cap *= 2;
        g->closure_defs_buf = (char *)realloc(g->closure_defs_buf, g->closure_defs_cap);
    }
    memcpy(g->closure_defs_buf + g->closure_defs_len, s, slen);
    g->closure_defs_len += slen;
    g->closure_defs_buf[g->closure_defs_len] = '\0';
}

static void cg_closure_def_fmt(CodeGen *g, const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    cg_closure_def_append(g, tmp);
}

/* ============================================================
 * Monomorphization Registry
 *
 * Maps Limceron generic type arguments to C type names and tracks
 * which specializations have been emitted as typedefs.
 * ============================================================ */

/* Map a Limceron type name to a C type name component (for building mono names) */
static const char *mono_type_component(const char *name) {
    if (!name) return "void";
    if (strcmp(name, "string") == 0 || strcmp(name, "String") == 0) return "LcnString";
    if (strcmp(name, "int") == 0)    return "int64_t";
    if (strcmp(name, "i8") == 0)     return "int8_t";
    if (strcmp(name, "i16") == 0)    return "int16_t";
    if (strcmp(name, "i32") == 0)    return "int32_t";
    if (strcmp(name, "i64") == 0)    return "int64_t";
    if (strcmp(name, "u8") == 0)     return "uint8_t";
    if (strcmp(name, "u16") == 0)    return "uint16_t";
    if (strcmp(name, "u32") == 0)    return "uint32_t";
    if (strcmp(name, "u64") == 0)    return "uint64_t";
    if (strcmp(name, "f32") == 0)    return "float";
    if (strcmp(name, "f64") == 0)    return "double";
    if (strcmp(name, "float") == 0)  return "double";
    if (strcmp(name, "bool") == 0)   return "bool";
    if (strcmp(name, "void") == 0)   return "void";
    return name;  /* user-defined type: use as-is */
}

/* Map a Limceron type name to a safe C identifier component (no dots, stars, spaces) */
static const char *mono_type_ident(const char *name) {
    if (!name) return "void";
    if (strcmp(name, "string") == 0 || strcmp(name, "String") == 0) return "LcnString";
    if (strcmp(name, "int") == 0)    return "int64_t";
    if (strcmp(name, "i8") == 0)     return "int8_t";
    if (strcmp(name, "i16") == 0)    return "int16_t";
    if (strcmp(name, "i32") == 0)    return "int32_t";
    if (strcmp(name, "i64") == 0)    return "int64_t";
    if (strcmp(name, "u8") == 0)     return "uint8_t";
    if (strcmp(name, "u16") == 0)    return "uint16_t";
    if (strcmp(name, "u32") == 0)    return "uint32_t";
    if (strcmp(name, "u64") == 0)    return "uint64_t";
    if (strcmp(name, "f32") == 0)    return "float";
    if (strcmp(name, "f64") == 0)    return "double";
    if (strcmp(name, "float") == 0)  return "double";
    if (strcmp(name, "bool") == 0)   return "bool";
    if (strcmp(name, "void") == 0)   return "void";
    return name;
}

/* Get C type name from a type_expr AST node (for mono name building) */
static const char *mono_type_from_expr(AstNode *type_expr) {
    if (!type_expr) return "LcnString";
    if (type_expr->kind == AST_TYPE_NAMED && type_expr->name)
        return mono_type_component(type_expr->name);
    return "void";
}

/* Get C identifier component from a type_expr AST node */
static const char *mono_ident_from_expr(AstNode *type_expr) {
    if (!type_expr) return "LcnString";
    if (type_expr->kind == AST_TYPE_NAMED && type_expr->name)
        return mono_type_ident(type_expr->name);
    return "void";
}

/* Check if a monomorphized type has been registered */
static bool mono_is_registered(CodeGen *g, const char *name) {
    int i;
    for (i = 0; i < g->mono_count; i++) {
        if (strcmp(g->mono[i].name, name) == 0)
            return true;
    }
    return false;
}

/* Register a monomorphized type name (mark as not yet emitted) */
static void mono_register_full(CodeGen *g, const char *name, const char *base, AstNode *generics) {
    if (g->mono_count >= MONO_MAX) return;
    if (mono_is_registered(g, name)) return;
    strncpy(g->mono[g->mono_count].name, name, 127);
    g->mono[g->mono_count].name[127] = '\0';
    strncpy(g->mono[g->mono_count].base, base, 15);
    g->mono[g->mono_count].base[15] = '\0';
    g->mono[g->mono_count].generics = generics;
    g->mono[g->mono_count].emitted = false;
    g->mono_count++;
}

/* Build the monomorphized name for Result<T, E> */
static void mono_result_name(char *buf, size_t bufsz, AstNode *generics) {
    const char *t_ident = "LcnString";
    const char *e_ident = "LcnString";
    if (generics) {
        t_ident = mono_ident_from_expr(generics);
        if (generics->next)
            e_ident = mono_ident_from_expr(generics->next);
    }
    snprintf(buf, bufsz, "Result_%s_%s", t_ident, e_ident);
}

/* Build the monomorphized name for Option<T> */
static void mono_option_name(char *buf, size_t bufsz, AstNode *generics) {
    const char *t_ident = "int64_t";  /* default for bare Option */
    if (generics) {
        t_ident = mono_ident_from_expr(generics);
    }
    snprintf(buf, bufsz, "Option_%s", t_ident);
}

/* Emit the typedef and helpers for a Result<T, E> specialization */
static void mono_emit_result(CodeGen *g, const char *mono_name, AstNode *generics) {
    const char *t_c = "LcnString";
    const char *e_c = "LcnString";
    const char *t_id = "LcnString";
    const char *e_id = "LcnString";
    if (generics) {
        t_c = mono_type_from_expr(generics);
        t_id = mono_ident_from_expr(generics);
        if (generics->next) {
            e_c = mono_type_from_expr(generics->next);
            e_id = mono_ident_from_expr(generics->next);
        }
    }

    /* Check if it's a pointer-sized type (string = const char*) */
    bool t_is_ptr = (strcmp(t_c, "LcnString") == 0 || strcmp(t_c, "void") == 0);
    bool e_is_ptr = (strcmp(e_c, "LcnString") == 0 || strcmp(e_c, "void") == 0);

    cg_fmt(g, "/* Monomorphized: Result<%s, %s> */\n", t_id, e_id);
    cg_fmt(g, "typedef struct { bool ok; %s value; %s error; } %s;\n\n",
           t_c, e_c, mono_name);

    /* ok() constructor */
    cg_fmt(g, "static %s %s_ok(%s val) {\n", mono_name, mono_name, t_c);
    if (e_is_ptr)
        cg_fmt(g, "    %s _r; _r.ok = true; _r.value = val; _r.error = NULL; return _r;\n", mono_name);
    else
        cg_fmt(g, "    %s _r; memset(&_r, 0, sizeof(_r)); _r.ok = true; _r.value = val; return _r;\n", mono_name);
    cg_str(g, "}\n\n");

    /* err() constructor */
    cg_fmt(g, "static %s %s_err(%s err) {\n", mono_name, mono_name, e_c);
    if (t_is_ptr)
        cg_fmt(g, "    %s _r; _r.ok = false; _r.value = NULL; _r.error = err; return _r;\n", mono_name);
    else
        cg_fmt(g, "    %s _r; memset(&_r, 0, sizeof(_r)); _r.ok = false; _r.error = err; return _r;\n", mono_name);
    cg_str(g, "}\n\n");

    (void)t_is_ptr;
}

/* Emit the typedef and helpers for an Option<T> specialization */
static void mono_emit_option(CodeGen *g, const char *mono_name, AstNode *generics) {
    const char *t_c = "int64_t";
    if (generics) {
        t_c = mono_type_from_expr(generics);
    }

    bool t_is_ptr = (strcmp(t_c, "LcnString") == 0 || strcmp(t_c, "void") == 0);

    cg_fmt(g, "/* Monomorphized: Option<%s> */\n", t_c);
    cg_fmt(g, "typedef struct { bool has_value; %s value; } %s;\n\n", t_c, mono_name);

    /* some() constructor */
    cg_fmt(g, "static %s %s_some(%s val) {\n", mono_name, mono_name, t_c);
    cg_fmt(g, "    %s _o; _o.has_value = true; _o.value = val; return _o;\n", mono_name);
    cg_str(g, "}\n\n");

    /* none() constructor */
    cg_fmt(g, "static %s %s_none(void) {\n", mono_name, mono_name);
    if (t_is_ptr)
        cg_fmt(g, "    %s _o; _o.has_value = false; _o.value = NULL; return _o;\n", mono_name);
    else
        cg_fmt(g, "    %s _o; memset(&_o, 0, sizeof(_o)); _o.has_value = false; return _o;\n", mono_name);
    cg_str(g, "}\n\n");
}

/* Walk the entire AST and collect all generic type specializations used.
 * Populates the mono registry with names that need typedef emission. */
static void mono_collect_from_node(CodeGen *g, AstNode *node);

static void mono_collect_from_node(CodeGen *g, AstNode *node) {
    if (!node) return;

    /* Check if this is a generic type reference */
    if (node->kind == AST_TYPE_NAMED && node->name && node->generics) {
        char buf[128];
        if (strcmp(node->name, "Result") == 0) {
            mono_result_name(buf, sizeof(buf), node->generics);
            mono_register_full(g, buf, "Result", node->generics);
        } else if (strcmp(node->name, "Option") == 0) {
            mono_option_name(buf, sizeof(buf), node->generics);
            mono_register_full(g, buf, "Option", node->generics);
        }
    }

    /* Also handle AST_TYPE_OPTIONAL (T?) as Option<T> */
    if (node->kind == AST_TYPE_OPTIONAL && node->left &&
        node->left->kind == AST_TYPE_NAMED && node->left->name) {
        char buf[128];
        mono_option_name(buf, sizeof(buf), node->left);
        mono_register_full(g, buf, "Option", node->left);
    }

    /* Recurse into all children */
    mono_collect_from_node(g, node->left);
    mono_collect_from_node(g, node->right);
    mono_collect_from_node(g, node->params);
    mono_collect_from_node(g, node->type_expr);
    mono_collect_from_node(g, node->generics);
    mono_collect_from_node(g, node->attributes);
    mono_collect_from_node(g, node->next);
}

/* ============================================================
 * Closure: Captured Variable Detection
 *
 * Walk the closure body AST, collect every AST_IDENT reference
 * that is NOT in the parameter list. Those are the captured vars.
 * ============================================================ */

#define MAX_CAPTURES 64

typedef struct {
    const char *names[MAX_CAPTURES];
    int count;
} CaptureList;

static bool capture_is_param(const char *name, AstNode *params) {
    AstNode *p;
    for (p = params; p; p = p->next)
        if (p->name && strcmp(p->name, name) == 0) return true;
    return false;
}

static bool capture_already_listed(CaptureList *cl, const char *name) {
    int i;
    for (i = 0; i < cl->count; i++)
        if (strcmp(cl->names[i], name) == 0) return true;
    return false;
}

/* Known builtin names that should NOT be treated as captures */
static bool is_builtin_name(const char *name) {
    static const char *builtins[] = {
        "print", "println", "len", "contains", "starts_with", "ends_with",
        "env", "env_or", "str_eq", "str_replace", "str_trim", "str_substring",
        "to_string", "to_int", "split", "format", "true", "false",
        "json_parse", "json_get", "json_get_number", "json_array_len",
        "json_array_get", "json_stringify",
        "abs", "min", "max", "clamp", "floor", "ceil", "round",
        "now_ms", "sleep_ms", "elapsed_ms", "format_timestamp",
        "log_info", "log_warn", "log_error", "log_debug",
        "read_file", "write_file", "read_line",
        "arg", "arg_count",
        "sb_new", "sb_append", "sb_to_string", "sb_peek",
        "secret_redact", "secret_unwrap",
        NULL
    };
    int i;
    if (!name) return false;
    for (i = 0; builtins[i]; i++)
        if (strcmp(name, builtins[i]) == 0) return true;
    return false;
}

/* Track locally-declared variable names inside the closure body */
typedef struct {
    const char *names[MAX_CAPTURES];
    int count;
} LocalsList;

static bool is_local_var(LocalsList *locals, const char *name) {
    int i;
    for (i = 0; i < locals->count; i++)
        if (strcmp(locals->names[i], name) == 0) return true;
    return false;
}

static void add_local_var(LocalsList *locals, const char *name) {
    if (name && locals->count < MAX_CAPTURES)
        locals->names[locals->count++] = name;
}

static void collect_captures_inner(AstNode *node, AstNode *params,
                                   CaptureList *cl, LocalsList *locals) {
    if (!node) return;

    if (node->kind == AST_IDENT && node->name) {
        if (!capture_is_param(node->name, params) &&
            !capture_already_listed(cl, node->name) &&
            !is_builtin_name(node->name) &&
            !is_local_var(locals, node->name) &&
            cl->count < MAX_CAPTURES) {
            cl->names[cl->count++] = node->name;
        }
    }

    /* Skip callee identifiers in function calls (they are function names, not captures).
     * But still walk the call arguments. */
    if (node->kind == AST_CALL) {
        if (node->left && node->left->kind != AST_IDENT)
            collect_captures_inner(node->left, params, cl, locals);
        AstNode *arg;
        for (arg = node->params; arg; arg = arg->next)
            collect_captures_inner(arg, params, cl, locals);
        return;
    }

    /* For let declarations: walk the initializer FIRST (before adding to locals),
     * then register the name as local for subsequent statements. */
    if (node->kind == AST_LET && node->name) {
        collect_captures_inner(node->right, params, cl, locals);
        add_local_var(locals, node->name);
        return;
    }

    /* For blocks: walk statements sequentially so locals accumulate */
    if (node->kind == AST_BLOCK) {
        AstNode *s;
        for (s = node->params; s; s = s->next)
            collect_captures_inner(s, params, cl, locals);
        return;
    }

    /* For expression statements, recurse into the expression */
    if (node->kind == AST_EXPR_STMT) {
        collect_captures_inner(node->left, params, cl, locals);
        return;
    }

    /* For for loops: the iterator variable is local */
    if (node->kind == AST_FOR) {
        collect_captures_inner(node->params, params, cl, locals); /* iterator expr */
        if (node->left && node->left->name)
            add_local_var(locals, node->left->name);
        collect_captures_inner(node->right, params, cl, locals); /* body */
        return;
    }

    /* Recurse into children */
    collect_captures_inner(node->left, params, cl, locals);
    collect_captures_inner(node->right, params, cl, locals);
    {
        AstNode *p;
        for (p = node->params; p; p = p->next)
            collect_captures_inner(p, params, cl, locals);
    }
    collect_captures_inner(node->generics, params, cl, locals);
}

static void collect_captures(AstNode *node, AstNode *params, CaptureList *cl) {
    LocalsList locals;
    locals.count = 0;
    collect_captures_inner(node, params, cl, &locals);
}

/* ============================================================
 * Type Emission
 * ============================================================ */

static void cg_type(CodeGen *g, AstNode *type_expr);
static void cg_expr(CodeGen *g, AstNode *expr);
static void cg_block(CodeGen *g, AstNode *block);
static void cg_stmt(CodeGen *g, AstNode *stmt);
static void cg_block_with_implicit_return(CodeGen *g, AstNode *block, AstNode *ret_type);
static const char *cg_type_to_c(AstNode *type_expr);

static void cg_type(CodeGen *g, AstNode *type_expr) {
    if (!type_expr) { cg_str(g, "void"); return; }

    if (type_expr->kind == AST_TYPE_NAMED) {
        const char *n = type_expr->name;
        if (!n) { cg_str(g, "void"); return; }

        /* Primitive type mapping */
        if (strcmp(n, "i8") == 0)       { cg_str(g, "int8_t"); return; }
        if (strcmp(n, "i16") == 0)      { cg_str(g, "int16_t"); return; }
        if (strcmp(n, "i32") == 0)      { cg_str(g, "int32_t"); return; }
        if (strcmp(n, "i64") == 0)      { cg_str(g, "int64_t"); return; }
        if (strcmp(n, "i128") == 0)     { cg_str(g, "int64_t"); return; } /* approx */
        if (strcmp(n, "u8") == 0)       { cg_str(g, "uint8_t"); return; }
        if (strcmp(n, "u16") == 0)      { cg_str(g, "uint16_t"); return; }
        if (strcmp(n, "u32") == 0)      { cg_str(g, "uint32_t"); return; }
        if (strcmp(n, "u64") == 0)      { cg_str(g, "uint64_t"); return; }
        if (strcmp(n, "u128") == 0)     { cg_str(g, "uint64_t"); return; } /* approx */
        if (strcmp(n, "f32") == 0)      { cg_str(g, "float"); return; }
        if (strcmp(n, "f64") == 0)      { cg_str(g, "double"); return; }
        if (strcmp(n, "int") == 0)      { cg_str(g, "int64_t"); return; }
        if (strcmp(n, "float") == 0)    { cg_str(g, "double"); return; }
        if (strcmp(n, "bool") == 0)     { cg_str(g, "bool"); return; }
        if (strcmp(n, "void") == 0)     { cg_str(g, "void"); return; }
        if (strcmp(n, "string") == 0)   { cg_str(g, "LcnString"); return; }
        if (strcmp(n, "String") == 0)   { cg_str(g, "LcnString"); return; }
        if (strcmp(n, "Result") == 0) {
            if (type_expr->generics) {
                /* Monomorphized Result<T, E> */
                char mono_name[128];
                mono_result_name(mono_name, sizeof(mono_name), type_expr->generics);
                cg_str(g, mono_name);
            } else {
                /* Bare Result → backward-compatible LcnResult */
                cg_str(g, "LcnResult");
            }
            return;
        }
        if (strcmp(n, "Vec") == 0)      { cg_str(g, "LcnVec"); return; }
        if (strcmp(n, "List") == 0)     { cg_str(g, "LcnVec"); return; }
        if (strcmp(n, "Option") == 0) {
            if (type_expr->generics) {
                /* Monomorphized Option<T> */
                char mono_name[128];
                mono_option_name(mono_name, sizeof(mono_name), type_expr->generics);
                cg_str(g, mono_name);
            } else {
                /* Bare Option → backward-compatible LcnOption */
                cg_str(g, "LcnOption");
            }
            return;
        }
        if (strcmp(n, "Map") == 0)      { cg_str(g, "void *"); return; }
        if (strcmp(n, "Set") == 0)      { cg_str(g, "void *"); return; }
        if (strcmp(n, "Channel") == 0)  { cg_str(g, "LcnChannel *"); return; }

        /* User-defined type — emit as-is */
        cg_str(g, n);
        return;
    }

    if (type_expr->kind == AST_TYPE_REF) {
        cg_type(g, type_expr->left);
        cg_str(g, " *");
        return;
    }

    if (type_expr->kind == AST_TYPE_PTR) {
        cg_type(g, type_expr->left);
        cg_str(g, " *");
        return;
    }

    if (type_expr->kind == AST_TYPE_OPTIONAL) {
        if (type_expr->left && type_expr->left->kind == AST_TYPE_NAMED &&
            type_expr->left->name) {
            /* T? → Option_T (monomorphized) */
            char mono_name[128];
            mono_option_name(mono_name, sizeof(mono_name), type_expr->left);
            cg_str(g, mono_name);
        } else {
            /* Bare ? → LcnOption (fallback) */
            cg_str(g, "LcnOption");
        }
        return;
    }

    if (type_expr->kind == AST_TYPE_TAINTED) {
        cg_str(g, "Tainted_");
        if (type_expr->left) cg_type(g, type_expr->left);
        return;
    }

    /* secret T → same C type as T (protection is compile-time only) */
    if (type_expr->kind == AST_TYPE_SECRET) {
        if (type_expr->left) cg_type(g, type_expr->left);
        else cg_str(g, "LcnString");
        return;
    }

    /* Fallback */
    cg_str(g, "void /* unknown type */");
}

/* Forward declaration for string heuristic (defined below cg_expr) */
static bool might_be_string_expr(CodeGen *g, AstNode *expr);

/* Infer C type from expression (for untyped let bindings) */
static void cg_infer_type_emit(CodeGen *g, AstNode *expr) {
    if (!expr) { cg_str(g, "void"); return; }
    switch (expr->kind) {
    case AST_INT_LIT:    cg_str(g, "int64_t"); return;
    case AST_FLOAT_LIT:  cg_str(g, "double"); return;
    case AST_STRING_LIT: cg_str(g, "LcnString"); return;
    case AST_BOOL_LIT:   cg_str(g, "bool"); return;
    case AST_ARRAY:      cg_str(g, "LcnVec"); return;
    case AST_CLOSURE:    cg_str(g, "LcnClosure"); return;
    case AST_MAP:
        /* Struct literal: Name { ... } → type is Name */
        if (expr->name) { cg_str(g, expr->name); return; }
        cg_str(g, "LcnString"); return;
    case AST_CALL:
        /* Check if calling a known tool — use its return type */
        if (expr->left && expr->left->kind == AST_IDENT && expr->left->name) {
            const char *cn = expr->left->name;
            int ti;
            for (ti = 0; ti < g->tool_count; ti++) {
                if (strcmp(g->tools[ti].name, cn) == 0 &&
                    g->tools[ti].return_type) {
                    cg_type(g, g->tools[ti].return_type);
                    return;
                }
            }
            /* Builtin return types */
            if (strcmp(cn, "len") == 0 || strcmp(cn, "to_int") == 0 ||
                strcmp(cn, "json_array_len") == 0 ||
                strcmp(cn, "abs") == 0 || strcmp(cn, "min") == 0 ||
                strcmp(cn, "max") == 0 || strcmp(cn, "clamp") == 0 ||
                strcmp(cn, "floor") == 0 || strcmp(cn, "ceil") == 0 ||
                strcmp(cn, "round") == 0 || strcmp(cn, "now_ms") == 0 ||
                strcmp(cn, "elapsed_ms") == 0 ||
                strcmp(cn, "budget_tokens_used") == 0 || strcmp(cn, "budget_tokens_left") == 0 ||
                strcmp(cn, "budget_elapsed_ms") == 0 ||
                strcmp(cn, "estimate_tokens") == 0 ||
                strcmp(cn, "batch_size") == 0 || strcmp(cn, "batch_offset") == 0 ||
                strcmp(cn, "trace_begin") == 0 ||
                strcmp(cn, "char_code") == 0 || strcmp(cn, "to_char_code") == 0 ||
                strcmp(cn, "str_find") == 0 || strcmp(cn, "str_len") == 0) {
                cg_str(g, "int64_t"); return;
            }
            if (strcmp(cn, "json_get_number") == 0 ||
                strcmp(cn, "budget_cost_used") == 0 || strcmp(cn, "budget_cost_left") == 0 ||
                strcmp(cn, "budget_percentage") == 0) {
                cg_str(g, "double"); return;
            }
            if (strcmp(cn, "contains") == 0 || strcmp(cn, "starts_with") == 0 ||
                strcmp(cn, "ends_with") == 0 || strcmp(cn, "str_eq") == 0 ||
                strcmp(cn, "fits_in_budget") == 0 || strcmp(cn, "batch_dry_run") == 0 ||
                strcmp(cn, "write_file") == 0 ||
                strcmp(cn, "str_char_is_alpha") == 0 || strcmp(cn, "str_char_is_digit") == 0 ||
                strcmp(cn, "str_char_is_alnum") == 0) {
                cg_str(g, "bool"); return;
            }
            if (strcmp(cn, "env") == 0 || strcmp(cn, "str_replace") == 0 ||
                strcmp(cn, "str_trim") == 0 || strcmp(cn, "str_substring") == 0 ||
                strcmp(cn, "to_string") == 0 || strcmp(cn, "json_get") == 0 ||
                strcmp(cn, "json_stringify") == 0 ||
                strcmp(cn, "format_timestamp") == 0 || strcmp(cn, "read_file") == 0 ||
                strcmp(cn, "read_line") == 0 || strcmp(cn, "format") == 0 ||
                strcmp(cn, "env_or") == 0 || strcmp(cn, "sql_escape") == 0 ||
                strcmp(cn, "unwrap") == 0 ||
                strcmp(cn, "char_at") == 0 || strcmp(cn, "str_from_code") == 0 ||
                strcmp(cn, "str_slice") == 0) {
                cg_str(g, "LcnString"); return;
            }
            if (strcmp(cn, "json_parse") == 0 || strcmp(cn, "json_array_get") == 0) {
                cg_str(g, "LcnJsonValue *"); return;
            }
            if (strcmp(cn, "vec_new") == 0) {
                cg_str(g, "LcnVec"); return;
            }
            if (strcmp(cn, "vec_get") == 0) {
                cg_str(g, "LcnString"); return;
            }
            if (strcmp(cn, "vec_len") == 0) {
                cg_str(g, "int64_t"); return;
            }
            if (strcmp(cn, "vec_pop") == 0) {
                cg_str(g, "void *"); return;
            }
            if (strcmp(cn, "arg") == 0 || strcmp(cn, "sb_to_string") == 0 || strcmp(cn, "sb_peek") == 0) {
                cg_str(g, "LcnString"); return;
            }
            if (strcmp(cn, "arg_count") == 0) {
                cg_str(g, "int64_t"); return;
            }
            if (strcmp(cn, "sb_new") == 0) {
                cg_str(g, "void *"); return;
            }
            /* Capability delegation builtins */
            if (strcmp(cn, "delegate") == 0) {
                cg_str(g, "LcnDelegation *"); return;
            }
            if (strcmp(cn, "has_capability") == 0) {
                cg_str(g, "bool"); return;
            }
            /* Check user-defined function return type registry */
            {
                const char *fn_ret = cg_lookup_fn_ret(g, cn);
                if (fn_ret) { cg_str(g, fn_ret); return; }
            }
            /* Closure call: return int64_t by default (Stage 1) */
            if (cg_is_closure_var(g, cn)) {
                cg_str(g, "int64_t"); return;
            }
        }
        cg_str(g, "LcnResult");
        return;
    case AST_METHOD_CALL:
        /* ADT enum constructor: Token.Ident("hello") → type is Token */
        if (expr->left && expr->left->kind == AST_IDENT && expr->left->name && expr->name) {
            int ei = cg_lookup_enum(g, expr->left->name);
            if (ei >= 0 && g->enums[ei].has_data) {
                cg_str(g, g->enums[ei].name); return;
            }
        }
        /* MCP alias.call() returns LcnString */
        if (expr->left && expr->left->kind == AST_IDENT && expr->left->name &&
            expr->name && strcmp(expr->name, "call") == 0) {
            int mi;
            for (mi = 0; mi < g->mcp_alias_count; mi++) {
                if (strcmp(g->mcp_aliases[mi].alias, expr->left->name) == 0) {
                    cg_str(g, "LcnString"); return;
                }
            }
        }
        /* Driver methods — type depends on method name */
        if (expr->left && expr->left->kind == AST_IDENT && expr->name) {
            int mi;
            for (mi = 0; mi < g->mcp_alias_count; mi++) {
                if (strcmp(g->mcp_aliases[mi].alias, expr->left->name) == 0) {
                    const char *m = expr->name;
                    if (strcmp(m, "connect") == 0 || strcmp(m, "query") == 0) {
                        cg_str(g, "void *"); return;
                    }
                    if (strcmp(m, "row_count") == 0 || strcmp(m, "execute") == 0 ||
                        strcmp(m, "get_number") == 0) {
                        cg_str(g, "int64_t"); return;
                    }
                    if (strcmp(m, "get") == 0) {
                        cg_str(g, "LcnString"); return;
                    }
                    if (strcmp(m, "close") == 0 || strcmp(m, "free") == 0) {
                        cg_str(g, "void"); return;
                    }
                }
            }
        }
        /* Model alias methods: clf.predict() returns LcnModelResult */
        if (expr->left && expr->left->kind == AST_IDENT && expr->name) {
            int mi;
            for (mi = 0; mi < g->model_alias_count; mi++) {
                if (strcmp(g->model_aliases[mi].alias, expr->left->name) == 0) {
                    if (strcmp(expr->name, "predict") == 0) {
                        cg_str(g, "LcnModelResult"); return;
                    }
                    if (strcmp(expr->name, "info") == 0) {
                        cg_str(g, "LcnString"); return;
                    }
                }
            }
        }
        /* Agent methods typically return Result */
        cg_str(g, "LcnResult");
        return;
    case AST_ASK:           cg_str(g, "LcnLlmOutput"); return;
    case AST_TRY_OTHERWISE: cg_str(g, "LcnLlmOutput"); return;
    case AST_KEEP_WHERE:    cg_str(g, "int"); return;
    case AST_EACH:          cg_str(g, "int"); return;
    case AST_TELL:    cg_str(g, "void"); return;
    case AST_CHANNEL: cg_str(g, "LcnChannel *"); return;
    case AST_SPAWN:      cg_str(g, "void"); return;
    case AST_TASK_GROUP: cg_str(g, "void **"); return;
    case AST_AWAIT:   cg_str(g, "void *"); return;
    case AST_SELECT:  cg_str(g, "int"); return;
    case AST_BINARY:
        if (expr->val.op == TOK_PLUS &&
            (might_be_string_expr(g, expr->left) || might_be_string_expr(g, expr->right))) {
            cg_str(g, "LcnString"); return;
        }
        cg_str(g, "int64_t"); return;
    case AST_IDENT:
        /* Check if identifier is a tracked string variable */
        if (expr->name) {
            int si;
            for (si = 0; si < g->string_var_count; si++) {
                if (strcmp(g->string_vars[si], expr->name) == 0) {
                    cg_str(g, "LcnString"); return;
                }
            }
            /* Check if identifier is a tracked typed variable (struct/enum) */
            {
                const char *vtype = cg_var_agent_type(g, expr->name);
                if (vtype && (cg_is_struct(g, vtype) || cg_lookup_enum(g, vtype) >= 0)) {
                    cg_str(g, vtype); return;
                }
            }
        }
        /* Non-string identifier: default to int64_t (most common in loops/counters) */
        cg_str(g, "int64_t");
        return;
    case AST_FIELD_ACCESS:
        /* Metrics field access: metrics.field_name -> _lcn_metrics.field_name */
        if (expr->left && expr->left->kind == AST_IDENT && expr->left->name &&
            strcmp(expr->left->name, "metrics") == 0 && expr->name && g->has_metrics) {
            cg_fmt(g, "_lcn_metrics.%s", expr->name);
            break;
        }
        /* Enum variant access: Color.Red → type is the enum */
        if (expr->left && expr->left->kind == AST_IDENT && expr->left->name) {
            int ei = cg_lookup_enum(g, expr->left->name);
            if (ei >= 0) {
                /* For ADT enums, type is the enum name; for simple, it's LcnString */
                if (g->enums[ei].has_data)
                    cg_str(g, g->enums[ei].name);
                else
                    cg_str(g, "LcnString");
                return;
            }
        }
        /* Default for field access — could be int, string, etc. */
        cg_str(g, "int64_t");
        return;
    case AST_COMPTIME: {
        /* Infer type from the block's contents heuristically.
         * Check the last expression in the block for its kind. */
        if (expr->left && expr->left->kind == AST_BLOCK) {
            AstNode *s = expr->left->params;
            AstNode *last = NULL;
            while (s) { last = s; s = s->next; }
            if (last) {
                AstNode *last_expr = last;
                if (last->kind == AST_EXPR_STMT && last->left)
                    last_expr = last->left;
                if (last_expr->kind == AST_STRING_LIT ||
                    might_be_string_expr(g, last_expr)) {
                    cg_str(g, "LcnString"); return;
                }
                if (last_expr->kind == AST_FLOAT_LIT) {
                    cg_str(g, "double"); return;
                }
                if (last_expr->kind == AST_BOOL_LIT) {
                    cg_str(g, "bool"); return;
                }
            }
        }
        cg_str(g, "int64_t"); return;
    }
    default:
        cg_str(g, "int64_t");
        return;
    }
}

/* ============================================================
 * String Escaping
 * ============================================================ */

/* Emit a C string literal with proper escaping of special chars */
static void cg_string_literal(CodeGen *g, const char *s)
{
    cg_str(g, "\"");
    if (s) {
        const char *p = s;
        while (*p) {
            switch (*p) {
            case '\n': cg_str(g, "\\n"); break;
            case '\r': cg_str(g, "\\r"); break;
            case '\t': cg_str(g, "\\t"); break;
            case '\\': cg_str(g, "\\\\"); break;
            case '"':  cg_str(g, "\\\""); break;
            default:   cg_raw(g, p, 1); break;
            }
            p++;
        }
    }
    cg_str(g, "\"");
}

/* ============================================================
 * Binary/Unary Operator Mapping
 * ============================================================ */

static const char *cg_binop(TokenKind op) {
    switch (op) {
    case TOK_PLUS:      return "+";
    case TOK_MINUS:     return "-";
    case TOK_STAR:      return "*";
    case TOK_SLASH:     return "/";
    case TOK_PERCENT:   return "%";
    case TOK_EQ_EQ:     return "==";
    case TOK_NOT_EQ:    return "!=";
    case TOK_LT:        return "<";
    case TOK_GT:        return ">";
    case TOK_LT_EQ:     return "<=";
    case TOK_GT_EQ:     return ">=";
    case TOK_AND_AND:   return "&&";
    case TOK_PIPE_PIPE: return "||";
    case TOK_AMP:       return "&";
    case TOK_PIPE:      return "|";
    case TOK_CARET:     return "^";
    case TOK_SHL:       return "<<";
    case TOK_SHR:       return ">>";
    default:            return "/* ? */";
    }
}

static const char *cg_unop(TokenKind op) {
    switch (op) {
    case TOK_MINUS: return "-";
    case TOK_BANG:  return "!";
    case TOK_TILDE: return "~";
    default:        return "/* ? */";
    }
}

static const char *cg_assign_op(TokenKind op) {
    switch (op) {
    case TOK_EQ:        return "=";
    case TOK_PLUS_EQ:   return "+=";
    case TOK_MINUS_EQ:  return "-=";
    case TOK_STAR_EQ:   return "*=";
    case TOK_SLASH_EQ:  return "/=";
    case TOK_PERCENT_EQ:return "%=";
    case TOK_AMP_EQ:    return "&=";
    case TOK_PIPE_EQ:   return "|=";
    case TOK_CARET_EQ:  return "^=";
    case TOK_SHL_EQ:    return "<<=";
    case TOK_SHR_EQ:    return ">>=";
    default:            return "=";
    }
}

/* ============================================================
 * String Concatenation Heuristic
 * ============================================================ */

__attribute__((unused))
static bool might_be_string_expr(CodeGen *g, AstNode *expr) {
    if (!expr) return false;
    if (expr->kind == AST_STRING_LIT) return true;
    if (expr->kind == AST_BINARY && expr->val.op == TOK_PLUS) {
        /* Nested concatenation — if any part is string, whole thing is */
        return might_be_string_expr(g, expr->left) || might_be_string_expr(g, expr->right);
    }
    if (expr->kind == AST_CALL && expr->left && expr->left->kind == AST_IDENT) {
        const char *fn = expr->left->name;
        if (fn && (strcmp(fn, "ask") == 0 || strcmp(fn, "env") == 0 ||
                   strcmp(fn, "env_or") == 0 || strcmp(fn, "str_trim") == 0 ||
                   strcmp(fn, "str_replace") == 0 || strcmp(fn, "str_substring") == 0 ||
                   strcmp(fn, "to_string") == 0 || strcmp(fn, "json_get") == 0 ||
                   strcmp(fn, "json_stringify") == 0 || strcmp(fn, "read_file") == 0 ||
                   strcmp(fn, "read_line") == 0 || strcmp(fn, "format_timestamp") == 0 ||
                   strcmp(fn, "format") == 0 || strcmp(fn, "lcn_str_concat") == 0 ||
                   strcmp(fn, "split") == 0 || strcmp(fn, "json_array_get") == 0 ||
                   strcmp(fn, "sql_escape") == 0 ||
                   strcmp(fn, "unwrap") == 0 ||
                   strcmp(fn, "char_at") == 0 || strcmp(fn, "str_from_code") == 0 ||
                   strcmp(fn, "str_slice") == 0))
            return true;
    }
    /* Method calls that return strings */
    if (expr->kind == AST_METHOD_CALL && expr->name) {
        if (strcmp(expr->name, "call") == 0 ||
            strcmp(expr->name, "to_string") == 0 ||
            strcmp(expr->name, "trim") == 0 ||
            strcmp(expr->name, "replace") == 0)
            return true;
    }
    /* Field access on LLM output content */
    if (expr->kind == AST_FIELD_ACCESS && expr->name &&
        strcmp(expr->name, "content") == 0)
        return true;
    /* BUG-2 fix: check if identifier is a known string variable */
    if (expr->kind == AST_IDENT && expr->name && g) {
        int i;
        for (i = 0; i < g->string_var_count; i++) {
            if (strcmp(g->string_vars[i], expr->name) == 0) return true;
        }
        return false;
    }
    /* Comptime blocks that evaluate to a string */
    if (expr->kind == AST_COMPTIME) {
        /* Peek into the block: if last expression is a string literal or string concat, it's string */
        if (expr->left && expr->left->kind == AST_BLOCK) {
            AstNode *s = expr->left->params;
            AstNode *last = NULL;
            while (s) { last = s; s = s->next; }
            if (last && last->kind == AST_EXPR_STMT && last->left)
                return might_be_string_expr(g, last->left);
            if (last && last->kind == AST_STRING_LIT) return true;
        }
        return false;
    }
    return false;
}

/* ============================================================
 * Compile-Time Evaluator (comptime)
 *
 * A mini-interpreter that evaluates a subset of Limceron at
 * compile time. Supports integers, floats, strings, booleans,
 * arithmetic, comparisons, logic, let bindings, if/else, and
 * compile-time assertions.
 * ============================================================ */

typedef enum {
    COMPTIME_INT,
    COMPTIME_FLOAT,
    COMPTIME_STRING,
    COMPTIME_BOOL,
    COMPTIME_NONE,
    COMPTIME_ERROR   /* evaluation failed */
} ComptimeKind;

typedef struct {
    ComptimeKind kind;
    union {
        int64_t  int_val;
        double   float_val;
        const char *str_val;
        bool     bool_val;
    } v;
} ComptimeValue;

/* Comptime local variable environment */
#define COMPTIME_MAX_LOCALS 64

typedef struct {
    const char   *names[COMPTIME_MAX_LOCALS];
    ComptimeValue values[COMPTIME_MAX_LOCALS];
    int           count;
    bool          had_error;
    char          error_msg[512];
    Arena        *arena;
} ComptimeEnv;

static ComptimeValue comptime_eval_expr(ComptimeEnv *env, AstNode *expr);
static void comptime_eval_stmt(ComptimeEnv *env, AstNode *stmt, ComptimeValue *result);

static ComptimeValue comptime_error(ComptimeEnv *env, const char *fmt, ...) {
    ComptimeValue cv;
    cv.kind = COMPTIME_ERROR;
    cv.v.int_val = 0;
    env->had_error = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(env->error_msg, sizeof(env->error_msg), fmt, ap);
    va_end(ap);
    return cv;
}

static ComptimeValue comptime_int(int64_t val) {
    ComptimeValue cv;
    cv.kind = COMPTIME_INT;
    cv.v.int_val = val;
    return cv;
}

static ComptimeValue comptime_float(double val) {
    ComptimeValue cv;
    cv.kind = COMPTIME_FLOAT;
    cv.v.float_val = val;
    return cv;
}

static ComptimeValue comptime_string(const char *val) {
    ComptimeValue cv;
    cv.kind = COMPTIME_STRING;
    cv.v.str_val = val;
    return cv;
}

static ComptimeValue comptime_bool(bool val) {
    ComptimeValue cv;
    cv.kind = COMPTIME_BOOL;
    cv.v.bool_val = val;
    return cv;
}

static ComptimeValue comptime_none_val(void) {
    ComptimeValue cv;
    cv.kind = COMPTIME_NONE;
    cv.v.int_val = 0;
    return cv;
}

static ComptimeValue *comptime_lookup(ComptimeEnv *env, const char *name) {
    int i;
    for (i = env->count - 1; i >= 0; i--) {
        if (env->names[i] && strcmp(env->names[i], name) == 0) {
            return &env->values[i];
        }
    }
    return NULL;
}

static void comptime_set(ComptimeEnv *env, const char *name, ComptimeValue val) {
    /* Update existing binding first */
    int i;
    for (i = env->count - 1; i >= 0; i--) {
        if (env->names[i] && strcmp(env->names[i], name) == 0) {
            env->values[i] = val;
            return;
        }
    }
    /* New binding */
    if (env->count < COMPTIME_MAX_LOCALS) {
        env->names[env->count] = name;
        env->values[env->count] = val;
        env->count++;
    } else {
        comptime_error(env, "too many comptime local variables (max %d)", COMPTIME_MAX_LOCALS);
    }
}

/* Evaluate a comptime binary operation */
static ComptimeValue comptime_eval_binary(ComptimeEnv *env, AstNode *expr) {
    ComptimeValue lhs = comptime_eval_expr(env, expr->left);
    if (lhs.kind == COMPTIME_ERROR) return lhs;
    ComptimeValue rhs = comptime_eval_expr(env, expr->right);
    if (rhs.kind == COMPTIME_ERROR) return rhs;

    TokenKind op = expr->val.op;

    /* String concatenation: string + string */
    if (op == TOK_PLUS && lhs.kind == COMPTIME_STRING && rhs.kind == COMPTIME_STRING) {
        const char *a = lhs.v.str_val ? lhs.v.str_val : "";
        const char *b = rhs.v.str_val ? rhs.v.str_val : "";
        size_t la = strlen(a);
        size_t lb = strlen(b);
        char *buf = (char *)arena_alloc(env->arena, la + lb + 1);
        memcpy(buf, a, la);
        memcpy(buf + la, b, lb);
        buf[la + lb] = '\0';
        return comptime_string(buf);
    }

    /* Promote int to float if mixed */
    if (lhs.kind == COMPTIME_INT && rhs.kind == COMPTIME_FLOAT) {
        lhs.kind = COMPTIME_FLOAT;
        lhs.v.float_val = (double)lhs.v.int_val;
    }
    if (lhs.kind == COMPTIME_FLOAT && rhs.kind == COMPTIME_INT) {
        rhs.kind = COMPTIME_FLOAT;
        rhs.v.float_val = (double)rhs.v.int_val;
    }

    /* Integer arithmetic */
    if (lhs.kind == COMPTIME_INT && rhs.kind == COMPTIME_INT) {
        int64_t a = lhs.v.int_val, b = rhs.v.int_val;
        switch (op) {
        case TOK_PLUS:      return comptime_int(a + b);
        case TOK_MINUS:     return comptime_int(a - b);
        case TOK_STAR:      return comptime_int(a * b);
        case TOK_SLASH:
            if (b == 0) return comptime_error(env, "comptime: division by zero");
            return comptime_int(a / b);
        case TOK_PERCENT:
            if (b == 0) return comptime_error(env, "comptime: modulo by zero");
            return comptime_int(a % b);
        case TOK_EQ_EQ:    return comptime_bool(a == b);
        case TOK_NOT_EQ:   return comptime_bool(a != b);
        case TOK_LT:       return comptime_bool(a < b);
        case TOK_GT:       return comptime_bool(a > b);
        case TOK_LT_EQ:    return comptime_bool(a <= b);
        case TOK_GT_EQ:    return comptime_bool(a >= b);
        case TOK_SHL:      return comptime_int(a << b);
        case TOK_SHR:      return comptime_int(a >> b);
        case TOK_AMP:      return comptime_int(a & b);
        case TOK_PIPE:     return comptime_int(a | b);
        case TOK_CARET:    return comptime_int(a ^ b);
        default:
            return comptime_error(env, "comptime: unsupported integer operator");
        }
    }

    /* Float arithmetic */
    if (lhs.kind == COMPTIME_FLOAT && rhs.kind == COMPTIME_FLOAT) {
        double a = lhs.v.float_val, b = rhs.v.float_val;
        switch (op) {
        case TOK_PLUS:      return comptime_float(a + b);
        case TOK_MINUS:     return comptime_float(a - b);
        case TOK_STAR:      return comptime_float(a * b);
        case TOK_SLASH:
            if (b == 0.0) return comptime_error(env, "comptime: division by zero");
            return comptime_float(a / b);
        case TOK_EQ_EQ:    return comptime_bool(a == b);
        case TOK_NOT_EQ:   return comptime_bool(a != b);
        case TOK_LT:       return comptime_bool(a < b);
        case TOK_GT:       return comptime_bool(a > b);
        case TOK_LT_EQ:    return comptime_bool(a <= b);
        case TOK_GT_EQ:    return comptime_bool(a >= b);
        default:
            return comptime_error(env, "comptime: unsupported float operator");
        }
    }

    /* Boolean logic */
    if (lhs.kind == COMPTIME_BOOL && rhs.kind == COMPTIME_BOOL) {
        bool a = lhs.v.bool_val, b = rhs.v.bool_val;
        switch (op) {
        case TOK_AND_AND:   return comptime_bool(a && b);
        case TOK_PIPE_PIPE: return comptime_bool(a || b);
        case TOK_EQ_EQ:    return comptime_bool(a == b);
        case TOK_NOT_EQ:   return comptime_bool(a != b);
        default:
            return comptime_error(env, "comptime: unsupported boolean operator");
        }
    }

    /* String comparison */
    if (lhs.kind == COMPTIME_STRING && rhs.kind == COMPTIME_STRING) {
        const char *a = lhs.v.str_val ? lhs.v.str_val : "";
        const char *b = rhs.v.str_val ? rhs.v.str_val : "";
        int cmp = strcmp(a, b);
        switch (op) {
        case TOK_EQ_EQ:    return comptime_bool(cmp == 0);
        case TOK_NOT_EQ:   return comptime_bool(cmp != 0);
        case TOK_LT:       return comptime_bool(cmp < 0);
        case TOK_GT:       return comptime_bool(cmp > 0);
        case TOK_LT_EQ:    return comptime_bool(cmp <= 0);
        case TOK_GT_EQ:    return comptime_bool(cmp >= 0);
        default:
            return comptime_error(env, "comptime: unsupported string operator");
        }
    }

    return comptime_error(env, "comptime: incompatible types in binary operation");
}

/* Evaluate a comptime unary operation */
static ComptimeValue comptime_eval_unary(ComptimeEnv *env, AstNode *expr) {
    ComptimeValue operand = comptime_eval_expr(env, expr->left);
    if (operand.kind == COMPTIME_ERROR) return operand;

    TokenKind op = expr->val.op;

    switch (op) {
    case TOK_MINUS:
        if (operand.kind == COMPTIME_INT) return comptime_int(-operand.v.int_val);
        if (operand.kind == COMPTIME_FLOAT) return comptime_float(-operand.v.float_val);
        break;
    case TOK_BANG:
        if (operand.kind == COMPTIME_BOOL) return comptime_bool(!operand.v.bool_val);
        break;
    case TOK_TILDE:
        if (operand.kind == COMPTIME_INT) return comptime_int(~operand.v.int_val);
        break;
    default:
        break;
    }
    return comptime_error(env, "comptime: unsupported unary operator");
}

/* Evaluate comptime function calls to known pure builtins */
static ComptimeValue comptime_eval_call(ComptimeEnv *env, AstNode *expr) {
    if (!expr->left || expr->left->kind != AST_IDENT || !expr->left->name) {
        return comptime_error(env, "comptime: only named function calls supported");
    }
    const char *fn = expr->left->name;

    /* Count arguments */
    int argc = 0;
    ComptimeValue args[16];
    AstNode *arg = expr->params;
    while (arg && argc < 16) {
        args[argc] = comptime_eval_expr(env, arg);
        if (args[argc].kind == COMPTIME_ERROR) return args[argc];
        argc++;
        arg = arg->next;
    }

    /* assert(condition, message) — compile-time assertion */
    if (strcmp(fn, "assert") == 0) {
        if (argc < 1) return comptime_error(env, "comptime: assert requires at least 1 argument");
        if (args[0].kind != COMPTIME_BOOL) {
            return comptime_error(env, "comptime: assert condition must be boolean");
        }
        if (!args[0].v.bool_val) {
            if (argc >= 2 && args[1].kind == COMPTIME_STRING && args[1].v.str_val) {
                return comptime_error(env, "comptime assertion failed: %s", args[1].v.str_val);
            }
            return comptime_error(env, "comptime assertion failed");
        }
        return comptime_none_val();
    }

    /* len(string) — string length */
    if (strcmp(fn, "len") == 0 || strcmp(fn, "str_len") == 0) {
        if (argc != 1) return comptime_error(env, "comptime: %s takes 1 argument", fn);
        if (args[0].kind != COMPTIME_STRING) {
            return comptime_error(env, "comptime: %s requires a string argument", fn);
        }
        return comptime_int((int64_t)strlen(args[0].v.str_val ? args[0].v.str_val : ""));
    }

    /* min(a, b), max(a, b) */
    if (strcmp(fn, "min") == 0 || strcmp(fn, "max") == 0) {
        if (argc != 2) return comptime_error(env, "comptime: %s takes 2 arguments", fn);
        bool is_max = (fn[1] == 'a'); /* max */
        if (args[0].kind == COMPTIME_INT && args[1].kind == COMPTIME_INT) {
            int64_t a = args[0].v.int_val, b = args[1].v.int_val;
            return comptime_int(is_max ? (a > b ? a : b) : (a < b ? a : b));
        }
        if (args[0].kind == COMPTIME_FLOAT && args[1].kind == COMPTIME_FLOAT) {
            double a = args[0].v.float_val, b = args[1].v.float_val;
            return comptime_float(is_max ? (a > b ? a : b) : (a < b ? a : b));
        }
        return comptime_error(env, "comptime: %s requires numeric arguments of the same type", fn);
    }

    /* abs(x) */
    if (strcmp(fn, "abs") == 0) {
        if (argc != 1) return comptime_error(env, "comptime: abs takes 1 argument");
        if (args[0].kind == COMPTIME_INT) {
            int64_t cv = args[0].v.int_val;
            return comptime_int(cv < 0 ? -cv : cv);
        }
        if (args[0].kind == COMPTIME_FLOAT) {
            double cv = args[0].v.float_val;
            return comptime_float(cv < 0 ? -cv : cv);
        }
        return comptime_error(env, "comptime: abs requires a numeric argument");
    }

    /* to_string(x) — convert to string representation */
    if (strcmp(fn, "to_string") == 0) {
        if (argc != 1) return comptime_error(env, "comptime: to_string takes 1 argument");
        char buf[128];
        switch (args[0].kind) {
        case COMPTIME_INT:
            snprintf(buf, sizeof(buf), "%lld", (long long)args[0].v.int_val);
            return comptime_string(arena_strdup(env->arena, buf));
        case COMPTIME_FLOAT:
            snprintf(buf, sizeof(buf), "%g", args[0].v.float_val);
            return comptime_string(arena_strdup(env->arena, buf));
        case COMPTIME_BOOL:
            return comptime_string(args[0].v.bool_val ? "true" : "false");
        case COMPTIME_STRING:
            return args[0];
        default:
            return comptime_string("none");
        }
    }

    return comptime_error(env, "comptime: unknown function '%s'", fn);
}

/* Evaluate an if expression at comptime */
static ComptimeValue comptime_eval_if(ComptimeEnv *env, AstNode *expr) {
    ComptimeValue cond = comptime_eval_expr(env, expr->left);
    if (cond.kind == COMPTIME_ERROR) return cond;

    if (cond.kind != COMPTIME_BOOL) {
        return comptime_error(env, "comptime: if condition must be boolean");
    }

    if (cond.v.bool_val) {
        /* then branch */
        if (expr->right) {
            ComptimeValue result = comptime_none_val();
            if (expr->right->kind == AST_BLOCK) {
                AstNode *s = expr->right->params;
                while (s) {
                    comptime_eval_stmt(env, s, &result);
                    if (env->had_error) return result;
                    s = s->next;
                }
            } else {
                result = comptime_eval_expr(env, expr->right);
            }
            return result;
        }
    } else {
        /* else branch */
        if (expr->params) {
            ComptimeValue result = comptime_none_val();
            if (expr->params->kind == AST_BLOCK) {
                AstNode *s = expr->params->params;
                while (s) {
                    comptime_eval_stmt(env, s, &result);
                    if (env->had_error) return result;
                    s = s->next;
                }
            } else {
                result = comptime_eval_expr(env, expr->params);
            }
            return result;
        }
    }
    return comptime_none_val();
}

/* Evaluate a comptime expression */
static ComptimeValue comptime_eval_expr(ComptimeEnv *env, AstNode *expr) {
    if (!expr) return comptime_none_val();
    if (env->had_error) {
        ComptimeValue ev;
        ev.kind = COMPTIME_ERROR;
        ev.v.int_val = 0;
        return ev;
    }

    switch (expr->kind) {
    case AST_INT_LIT:
        return comptime_int(expr->val.int_val);

    case AST_FLOAT_LIT:
        return comptime_float(expr->val.float_val);

    case AST_STRING_LIT:
        return comptime_string(expr->val.str_val);

    case AST_BOOL_LIT:
        return comptime_bool(expr->val.bool_val);

    case AST_NONE_LIT:
        return comptime_none_val();

    case AST_IDENT: {
        if (!expr->name) return comptime_error(env, "comptime: unnamed identifier");
        ComptimeValue *found = comptime_lookup(env, expr->name);
        if (!found) return comptime_error(env, "comptime: undefined variable '%s'", expr->name);
        return *found;
    }

    case AST_BINARY:
        return comptime_eval_binary(env, expr);

    case AST_UNARY:
        return comptime_eval_unary(env, expr);

    case AST_CALL:
        return comptime_eval_call(env, expr);

    case AST_IF:
        return comptime_eval_if(env, expr);

    case AST_BLOCK: {
        /* Block expression: evaluate all statements, return last expression value */
        ComptimeValue result = comptime_none_val();
        AstNode *s = expr->params;
        while (s) {
            comptime_eval_stmt(env, s, &result);
            if (env->had_error) return result;
            s = s->next;
        }
        return result;
    }

    case AST_COMPTIME:
        /* Nested comptime — just evaluate the inner block */
        if (expr->left) return comptime_eval_expr(env, expr->left);
        return comptime_none_val();

    default:
        return comptime_error(env, "comptime: unsupported expression kind '%s'",
                              ast_kind_name(expr->kind));
    }
}

/* Evaluate a comptime statement.
 * Updates *result with the value of the last expression in the block. */
static void comptime_eval_stmt(ComptimeEnv *env, AstNode *stmt, ComptimeValue *result) {
    if (!stmt || env->had_error) return;

    switch (stmt->kind) {
    case AST_LET: {
        /* let x = expr  or  let x: Type = expr */
        ComptimeValue val = comptime_none_val();
        if (stmt->right) {
            val = comptime_eval_expr(env, stmt->right);
            if (val.kind == COMPTIME_ERROR) return;
        }
        if (stmt->name) {
            comptime_set(env, stmt->name, val);
        }
        *result = val;
        break;
    }

    case AST_ASSIGN: {
        /* x = expr */
        if (stmt->left && stmt->left->kind == AST_IDENT && stmt->left->name) {
            ComptimeValue val = comptime_eval_expr(env, stmt->right);
            if (val.kind == COMPTIME_ERROR) return;
            comptime_set(env, stmt->left->name, val);
            *result = val;
        } else {
            comptime_error(env, "comptime: assignment target must be an identifier");
        }
        break;
    }

    case AST_RETURN: {
        if (stmt->left) {
            *result = comptime_eval_expr(env, stmt->left);
        }
        break;
    }

    case AST_IF: {
        *result = comptime_eval_if(env, stmt);
        break;
    }

    case AST_EXPR_STMT: {
        if (stmt->left) {
            *result = comptime_eval_expr(env, stmt->left);
        }
        break;
    }

    case AST_BLOCK: {
        AstNode *s = stmt->params;
        while (s) {
            comptime_eval_stmt(env, s, result);
            if (env->had_error) return;
            s = s->next;
        }
        break;
    }

    default:
        comptime_error(env, "comptime: unsupported statement kind '%s'", ast_kind_name(stmt->kind));
        break;
    }
}

/* Public entry point: evaluate a comptime block and return the result.
 * The block node should be an AST_COMPTIME with left pointing to AST_BLOCK. */
static ComptimeValue comptime_evaluate(AstNode *comptime_node, Arena *arena) {
    ComptimeEnv env;
    memset(&env, 0, sizeof(env));
    env.arena = arena;

    if (!comptime_node) return comptime_none_val();

    AstNode *body = comptime_node->left;
    if (!body) return comptime_none_val();

    ComptimeValue result = comptime_none_val();

    if (body->kind == AST_BLOCK) {
        AstNode *s = body->params;
        while (s) {
            comptime_eval_stmt(&env, s, &result);
            if (env.had_error) {
                result.kind = COMPTIME_ERROR;
                return result;
            }
            s = s->next;
        }
    } else {
        result = comptime_eval_expr(&env, body);
    }

    return result;
}

/* Emit a ComptimeValue as a C literal into the codegen buffer */
static void cg_comptime_value(CodeGen *g, ComptimeValue val) {
    switch (val.kind) {
    case COMPTIME_INT:
        cg_fmt(g, "%lldLL", (long long)val.v.int_val);
        break;
    case COMPTIME_FLOAT:
        cg_fmt(g, "%g", val.v.float_val);
        break;
    case COMPTIME_STRING: {
        cg_str(g, "\"");
        if (val.v.str_val) {
            const char *s = val.v.str_val;
            while (*s) {
                switch (*s) {
                case '"':  cg_str(g, "\\\""); break;
                case '\\': cg_str(g, "\\\\"); break;
                case '\n': cg_str(g, "\\n"); break;
                case '\t': cg_str(g, "\\t"); break;
                case '\r': cg_str(g, "\\r"); break;
                default:   cg_raw(g, s, 1); break;
                }
                s++;
            }
        }
        cg_str(g, "\"");
        break;
    }
    case COMPTIME_BOOL:
        cg_str(g, val.v.bool_val ? "true" : "false");
        break;
    case COMPTIME_NONE:
        cg_str(g, "0 /* comptime none */");
        break;
    case COMPTIME_ERROR:
        cg_str(g, "0 /* comptime error */");
        break;
    }
}

/* Check if a comptime evaluation had an error and report it.
 * Returns true if there was an error. */
static bool comptime_check_error(ComptimeValue val, AstNode *node) {
    (void)node;
    return val.kind == COMPTIME_ERROR;
}

/* ============================================================
 * Expression Generation
 * ============================================================ */

static void cg_expr(CodeGen *g, AstNode *expr) {
    if (!expr) return;

    switch (expr->kind) {
    case AST_INT_LIT:
        cg_fmt(g, "%lld", (long long)expr->val.int_val);
        break;

    case AST_FLOAT_LIT:
        cg_fmt(g, "%g", expr->val.float_val);
        break;

    case AST_STRING_LIT: {
        /* String interpolation: "Hello {name}!" → lcn_str_concat(lcn_str_concat("Hello ", name), "!")
         * Only interpolate {ident} where ident is a valid identifier (no spaces, starts with letter/_) */
        const char *s = expr->val.str_val;
        /* Check if string contains any valid interpolation targets */
        bool has_interp = false;
        if (s) {
            const char *p = s;
            while (*p) {
                if (*p == '{') {
                    const char *start = p + 1;
                    /* Valid interpolation: {identifier} where identifier is [a-zA-Z_][a-zA-Z0-9_.] */
                    if ((*start >= 'a' && *start <= 'z') || (*start >= 'A' && *start <= 'Z') || *start == '_') {
                        const char *end = start;
                        while (*end && *end != '}' && ((*end >= 'a' && *end <= 'z') || (*end >= 'A' && *end <= 'Z') ||
                               (*end >= '0' && *end <= '9') || *end == '_' || *end == '.'))
                            end++;
                        if (*end == '}' && end > start) { has_interp = true; break; }
                    }
                }
                p++;
            }
        }
        if (!s || !has_interp) {
            /* No interpolation — plain string literal */
            cg_string_literal(g, s);
        } else {
            /* Collect segments: text/expr pairs */
            typedef struct { const char *start; size_t len; bool is_expr; } InterpSeg;
            InterpSeg segs[64];
            int nseg = 0;
            const char *p = s;
            while (*p && nseg < 64) {
                if (*p == '{') {
                    /* Check if this is a valid interpolation {ident} */
                    const char *test = p + 1;
                    bool valid_interp = false;
                    if ((*test >= 'a' && *test <= 'z') || (*test >= 'A' && *test <= 'Z') || *test == '_') {
                        const char *te = test;
                        while (*te && *te != '}' && ((*te >= 'a' && *te <= 'z') || (*te >= 'A' && *te <= 'Z') ||
                               (*te >= '0' && *te <= '9') || *te == '_' || *te == '.'))
                            te++;
                        if (*te == '}' && te > test) valid_interp = true;
                    }
                    if (valid_interp) {
                        p++;
                        const char *es = p;
                        while (*p && *p != '}') p++;
                        if (p > es) {
                            segs[nseg].start = es;
                            segs[nseg].len = (size_t)(p - es);
                            segs[nseg].is_expr = true;
                            nseg++;
                        }
                        if (*p == '}') p++;
                    } else {
                        /* Not valid interpolation — treat as text including the { */
                        const char *ts = p;
                        p++;
                        while (*p && *p != '{') p++;
                        segs[nseg].start = ts;
                        segs[nseg].len = (size_t)(p - ts);
                        segs[nseg].is_expr = false;
                        nseg++;
                    }
                } else {
                    const char *ts = p;
                    while (*p && *p != '{') p++;
                    segs[nseg].start = ts;
                    segs[nseg].len = (size_t)(p - ts);
                    segs[nseg].is_expr = false;
                    nseg++;
                }
            }
            if (nseg == 0) { cg_str(g, "\"\""); }
            else if (nseg == 1 && !segs[0].is_expr) {
                /* Single text segment — just emit as literal */
                cg_str(g, "\"");
                for (size_t si = 0; si < segs[0].len; si++) {
                    char c = segs[0].start[si];
                    switch (c) {
                    case '\n': cg_str(g, "\\n"); break;
                    case '"':  cg_str(g, "\\\""); break;
                    case '\\': cg_str(g, "\\\\"); break;
                    default:   cg_raw(g, &c, 1); break;
                    }
                }
                cg_str(g, "\"");
            } else {
                /* Emit nested lcn_str_concat: concat(concat(seg0, seg1), seg2) ... */
                int i;
                for (i = 0; i < nseg - 1; i++) cg_str(g, "lcn_str_concat(");
                /* Emit first segment */
                if (segs[0].is_expr) {
                    char tmp[256];
                    size_t tl = segs[0].len < 255 ? segs[0].len : 255;
                    memcpy(tmp, segs[0].start, tl); tmp[tl] = '\0';
                    /* Check if this identifier is a known string variable */
                    bool is_str_var0 = false;
                    { int si; for (si = 0; si < g->string_var_count; si++) {
                        if (strcmp(g->string_vars[si], tmp) == 0) { is_str_var0 = true; break; }
                    }}
                    if (is_str_var0) {
                        cg_str(g, tmp);
                    } else {
                        cg_str(g, "lcn_str_from_int((int64_t)(");
                        cg_str(g, tmp);
                        cg_str(g, "))");
                    }
                } else {
                    cg_str(g, "\"");
                    for (size_t si = 0; si < segs[0].len; si++) {
                        char c = segs[0].start[si];
                        switch (c) {
                        case '\n': cg_str(g, "\\n"); break;
                        case '"':  cg_str(g, "\\\""); break;
                        case '\\': cg_str(g, "\\\\"); break;
                        default:   cg_raw(g, &c, 1); break;
                        }
                    }
                    cg_str(g, "\"");
                }
                /* Emit remaining segments with closing parens */
                for (i = 1; i < nseg; i++) {
                    cg_str(g, ", ");
                    if (segs[i].is_expr) {
                        char tmp[256];
                        size_t tl = segs[i].len < 255 ? segs[i].len : 255;
                        memcpy(tmp, segs[i].start, tl); tmp[tl] = '\0';
                        /* Check if this identifier is a known string variable */
                        bool is_str_varN = false;
                        { int si; for (si = 0; si < g->string_var_count; si++) {
                            if (strcmp(g->string_vars[si], tmp) == 0) { is_str_varN = true; break; }
                        }}
                        if (is_str_varN) {
                            cg_str(g, tmp);
                        } else {
                            cg_str(g, "lcn_str_from_int((int64_t)(");
                            cg_str(g, tmp);
                            cg_str(g, "))");
                        }
                    } else {
                        cg_str(g, "\"");
                        for (size_t si = 0; si < segs[i].len; si++) {
                            char c = segs[i].start[si];
                            switch (c) {
                            case '\n': cg_str(g, "\\n"); break;
                            case '"':  cg_str(g, "\\\""); break;
                            case '\\': cg_str(g, "\\\\"); break;
                            default:   cg_raw(g, &c, 1); break;
                            }
                        }
                        cg_str(g, "\"");
                    }
                    cg_str(g, ")");
                }
            }
        }
        break;
    }

    case AST_BOOL_LIT:
        cg_str(g, expr->val.bool_val ? "true" : "false");
        break;

    case AST_NONE_LIT:
        cg_str(g, "NULL");
        break;

    case AST_IDENT:
        if (expr->name) {
            /* Check if it's an agent constructor reference */
            if (cg_lookup_agent(g, expr->name)) {
                cg_fmt(g, "lcn_agent_%s_new()", expr->name);
            }
            /* Check if it's a budget factory reference */
            else if (cg_lookup_budget(g, expr->name)) {
                cg_fmt(g, "lcn_budget_%s()", expr->name);
            } else {
                cg_str(g, expr->name);
            }
        }
        break;

    case AST_BINARY:
        /* String equality: detect when either side is a string literal or string expression
         * and emit strcmp instead of pointer comparison */
        if ((expr->val.op == TOK_EQ_EQ || expr->val.op == TOK_NOT_EQ) &&
            (might_be_string_expr(g, expr->left) || might_be_string_expr(g, expr->right))) {
            if (expr->val.op == TOK_EQ_EQ) {
                cg_str(g, "(strcmp(");
            } else {
                cg_str(g, "(strcmp(");
            }
            cg_expr(g, expr->left);
            cg_str(g, ", ");
            cg_expr(g, expr->right);
            if (expr->val.op == TOK_EQ_EQ)
                cg_str(g, ") == 0)");
            else
                cg_str(g, ") != 0)");
        }
        /* String concatenation: + with string operands -> lcn_str_concat() */
        else if (expr->val.op == TOK_PLUS &&
                 (might_be_string_expr(g, expr->left) || might_be_string_expr(g, expr->right))) {
            cg_str(g, "lcn_str_concat(");
            cg_expr(g, expr->left);
            cg_str(g, ", ");
            cg_expr(g, expr->right);
            cg_str(g, ")");
        } else {
            cg_str(g, "(");
            cg_expr(g, expr->left);
            cg_fmt(g, " %s ", cg_binop(expr->val.op));
            cg_expr(g, expr->right);
            cg_str(g, ")");
        }
        break;

    case AST_UNARY:
        cg_str(g, cg_unop(expr->val.op));
        cg_str(g, "(");
        cg_expr(g, expr->left);
        cg_str(g, ")");
        break;

    case AST_CALL: {
        const char *fn_name = (expr->left && expr->left->kind == AST_IDENT)
                               ? expr->left->name : NULL;

        /* Check if calling a builtin function */
        bool is_builtin = false;
        bool is_string_builtin = false;
        bool is_json_builtin = false;
        bool is_stdlib_builtin = false;
        if (fn_name) {
            if (strcmp(fn_name, "print") == 0 || strcmp(fn_name, "println") == 0) {
                is_builtin = true;
            }
            /* String builtins */
            else if (strcmp(fn_name, "len") == 0 || strcmp(fn_name, "contains") == 0 ||
                     strcmp(fn_name, "starts_with") == 0 || strcmp(fn_name, "ends_with") == 0 ||
                     strcmp(fn_name, "env") == 0 || strcmp(fn_name, "str_eq") == 0 ||
                     strcmp(fn_name, "str_replace") == 0 || strcmp(fn_name, "str_trim") == 0 ||
                     strcmp(fn_name, "sql_escape") == 0 || strcmp(fn_name, "unwrap") == 0 ||
                     strcmp(fn_name, "str_substring") == 0 || strcmp(fn_name, "str_split") == 0 ||
                     strcmp(fn_name, "to_string") == 0 || strcmp(fn_name, "to_int") == 0 ||
                     strcmp(fn_name, "char_at") == 0 || strcmp(fn_name, "char_code") == 0 ||
                     strcmp(fn_name, "str_from_code") == 0 || strcmp(fn_name, "str_slice") == 0 ||
                     strcmp(fn_name, "str_len") == 0 || strcmp(fn_name, "str_find") == 0 ||
                     strcmp(fn_name, "str_char_is_alpha") == 0 || strcmp(fn_name, "str_char_is_digit") == 0 ||
                     strcmp(fn_name, "str_char_is_alnum") == 0 || strcmp(fn_name, "to_char_code") == 0) {
                is_string_builtin = true;
            }
            /* JSON builtins */
            else if (strcmp(fn_name, "json_parse") == 0 || strcmp(fn_name, "json_get") == 0 ||
                     strcmp(fn_name, "json_get_number") == 0 ||
                     strcmp(fn_name, "json_array_len") == 0 ||
                     strcmp(fn_name, "json_array_get") == 0 ||
                     strcmp(fn_name, "json_stringify") == 0) {
                is_json_builtin = true;
            }
            /* Stdlib builtins: math, time, log, batch, budget, tokens, trace, file I/O */
            else if (strcmp(fn_name, "abs") == 0 || strcmp(fn_name, "min") == 0 ||
                     strcmp(fn_name, "max") == 0 || strcmp(fn_name, "clamp") == 0 ||
                     strcmp(fn_name, "floor") == 0 || strcmp(fn_name, "ceil") == 0 ||
                     strcmp(fn_name, "round") == 0 ||
                     strcmp(fn_name, "now_ms") == 0 || strcmp(fn_name, "sleep_ms") == 0 ||
                     strcmp(fn_name, "elapsed_ms") == 0 || strcmp(fn_name, "format_timestamp") == 0 ||
                     strcmp(fn_name, "log_info") == 0 || strcmp(fn_name, "log_warn") == 0 ||
                     strcmp(fn_name, "log_error") == 0 || strcmp(fn_name, "log_debug") == 0 ||
                     strcmp(fn_name, "batch_size") == 0 || strcmp(fn_name, "batch_offset") == 0 ||
                     strcmp(fn_name, "batch_dry_run") == 0 || strcmp(fn_name, "batch_progress") == 0 ||
                     strcmp(fn_name, "budget_tokens_used") == 0 || strcmp(fn_name, "budget_tokens_left") == 0 ||
                     strcmp(fn_name, "budget_cost_used") == 0 || strcmp(fn_name, "budget_cost_left") == 0 ||
                     strcmp(fn_name, "budget_percentage") == 0 || strcmp(fn_name, "budget_elapsed_ms") == 0 ||
                     strcmp(fn_name, "estimate_tokens") == 0 || strcmp(fn_name, "fits_in_budget") == 0 ||
                     strcmp(fn_name, "trace_begin") == 0 || strcmp(fn_name, "trace_end") == 0 ||
                     strcmp(fn_name, "trace_tag") == 0 || strcmp(fn_name, "trace_event") == 0 ||
                     strcmp(fn_name, "read_file") == 0 || strcmp(fn_name, "write_file") == 0 ||
                     strcmp(fn_name, "read_line") == 0 ||
                     strcmp(fn_name, "env_or") == 0 || strcmp(fn_name, "format") == 0 ||
                     strcmp(fn_name, "spawn_parallel") == 0 ||
                     strcmp(fn_name, "threadpool_init") == 0 ||
                     strcmp(fn_name, "threadpool_shutdown") == 0 ||
                     strcmp(fn_name, "vec_new") == 0 ||
                     strcmp(fn_name, "vec_push") == 0 ||
                     strcmp(fn_name, "vec_get") == 0 ||
                     strcmp(fn_name, "vec_len") == 0 ||
                     strcmp(fn_name, "vec_pop") == 0) {
                is_stdlib_builtin = true;
            }
        }

        /* CLI args builtins */
        bool is_cli_builtin = false;
        if (fn_name && (strcmp(fn_name, "arg") == 0 || strcmp(fn_name, "arg_count") == 0)) {
            is_cli_builtin = true;
        }

        /* String builder builtins */
        bool is_sb_builtin = false;
        if (fn_name && (strcmp(fn_name, "sb_new") == 0 || strcmp(fn_name, "sb_append") == 0 ||
                        strcmp(fn_name, "sb_to_string") == 0 || strcmp(fn_name, "sb_peek") == 0)) {
            is_sb_builtin = true;
        }

        /* Capability delegation builtins */
        bool is_delegation_builtin = false;
        if (fn_name && (strcmp(fn_name, "delegate") == 0 ||
                        strcmp(fn_name, "revoke") == 0 ||
                        strcmp(fn_name, "revoke_all") == 0 ||
                        strcmp(fn_name, "has_capability") == 0)) {
            is_delegation_builtin = true;
        }
        (void)is_delegation_builtin; /* codegen emitted below in delegation section */

        /* Secret builtins: secret_redact, secret_unwrap */
        bool is_secret_builtin = false;
        if (fn_name && (strcmp(fn_name, "secret_redact") == 0 ||
                        strcmp(fn_name, "secret_unwrap") == 0)) {
            is_secret_builtin = true;
        }

        /* Check if calling a known tool — rewrite to lcn_tool_X(caps, ...) */
        bool is_tool = false;
        if (fn_name) {
            int ti;
            for (ti = 0; ti < g->tool_count; ti++) {
                if (strcmp(g->tools[ti].name, fn_name) == 0) {
                    is_tool = true;
                    break;
                }
            }
        }

        /* Check if calling a sibling agent method (self-call) */
        bool is_self_method = false;
        if (!is_tool && fn_name && g->in_agent_method && g->current_agent_name) {
            is_self_method = cg_is_self_method(g, g->current_agent_name, fn_name);
        }

        if (is_builtin) {
            if (strcmp(fn_name, "println") == 0) {
                cg_str(g, "printf(\"%s\\n\", ");
            } else {
                cg_str(g, "printf(\"%s\", ");
            }
            AstNode *arg = expr->params;
            if (arg) cg_expr(g, arg);
            else cg_str(g, "\"\"");
            cg_str(g, ")");
        } else if (is_cli_builtin) {
            AstNode *arg1 = expr->params;
            if (strcmp(fn_name, "arg") == 0) {
                cg_str(g, "(_lcn_argc > ");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "0");
                cg_str(g, " ? _lcn_argv[");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "0");
                cg_str(g, "] : \"\")");
            } else if (strcmp(fn_name, "arg_count") == 0) {
                cg_str(g, "(int64_t)_lcn_argc");
            }
        } else if (is_sb_builtin) {
            AstNode *arg1 = expr->params;
            AstNode *arg2 = arg1 ? arg1->next : NULL;
            if (strcmp(fn_name, "sb_new") == 0) {
                cg_str(g, "lcn_sb_new()");
            } else if (strcmp(fn_name, "sb_append") == 0) {
                cg_str(g, "lcn_sb_append(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "NULL");
                cg_str(g, ", ");
                if (arg2) cg_expr(g, arg2);
                else cg_str(g, "\"\"");
                cg_str(g, ")");
            } else if (strcmp(fn_name, "sb_to_string") == 0) {
                cg_str(g, "lcn_sb_to_string(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "NULL");
                cg_str(g, ")");
            } else if (strcmp(fn_name, "sb_peek") == 0) {
                cg_str(g, "lcn_sb_peek(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "NULL");
                cg_str(g, ")");
            }
        } else if (is_delegation_builtin) {
            AstNode *arg1 = expr->params;
            AstNode *arg2 = arg1 ? arg1->next : NULL;
            AstNode *arg3 = arg2 ? arg2->next : NULL;

            if (strcmp(fn_name, "delegate") == 0) {
                /* delegate(parent_caps, [cap1, cap2], timeout)
                 * → lcn_delegate_new(parent_caps, (cap1 | cap2), timeout)
                 * If only 1 arg (array literal), use self->capabilities as parent.
                 * If 2 args: parent_caps, [cap_list]
                 * If 3 args: parent_caps, [cap_list], revoke_after_ms
                 */
                int parent_is_self = 0;
                AstNode *cap_list_arg = arg1;
                AstNode *timeout_arg = arg2;
                /* Detect forms: if first arg is an array, assume self caps */
                if (arg1 && arg1->kind == AST_ARRAY) {
                    parent_is_self = 1;
                    cap_list_arg = arg1;
                    timeout_arg = arg2;
                } else if (arg2 && arg2->kind == AST_ARRAY) {
                    parent_is_self = 0;
                    cap_list_arg = arg2;
                    timeout_arg = arg3;
                }
                cg_str(g, "lcn_delegate_new(");
                if (parent_is_self) {
                    if (g->in_agent_method)
                        cg_str(g, "self->capabilities");
                    else
                        cg_str(g, "(LcnCapability)0xFFFFFFFFFFFFFFFFULL");
                } else {
                    if (arg1) cg_expr(g, arg1);
                    else cg_str(g, "(LcnCapability)0");
                }
                cg_str(g, ", ");
                /* Emit the capability list as bitwise OR of defines */
                if (cap_list_arg && cap_list_arg->kind == AST_ARRAY) {
                    AstNode *elem = cap_list_arg->params;
                    cg_str(g, "(");
                    bool first = true;
                    while (elem) {
                        if (!first) cg_str(g, " | ");
                        first = false;
                        /* Element could be an identifier like "llm.classify" stored as field access,
                         * or a dotted string. Try to look up as capability. */
                        if (elem->kind == AST_FIELD_ACCESS && elem->left &&
                            elem->left->kind == AST_IDENT && elem->name) {
                            char qbuf[256];
                            snprintf(qbuf, sizeof(qbuf), "%s.%s",
                                     elem->left->name, elem->name);
                            const char *cap_def = cg_lookup_cap(g, qbuf);
                            if (cap_def) cg_str(g, cap_def);
                            else { cg_str(g, "(LcnCapability)0 /* unknown: "); cg_str(g, qbuf); cg_str(g, " */"); }
                        } else if (elem->kind == AST_IDENT && elem->name) {
                            /* Try bare name lookup with a wildcard group */
                            const char *cap_def = cg_lookup_cap(g, elem->name);
                            if (cap_def) cg_str(g, cap_def);
                            else { cg_str(g, "(LcnCapability)0 /* unknown: "); cg_str(g, elem->name); cg_str(g, " */"); }
                        } else {
                            cg_expr(g, elem);
                        }
                        elem = elem->next;
                    }
                    if (first) cg_str(g, "(LcnCapability)0"); /* empty list */
                    cg_str(g, ")");
                } else if (cap_list_arg) {
                    cg_expr(g, cap_list_arg);
                } else {
                    cg_str(g, "(LcnCapability)0");
                }
                cg_str(g, ", ");
                if (timeout_arg) {
                    cg_expr(g, timeout_arg);
                } else {
                    cg_str(g, "0");
                }
                cg_str(g, ")");
            } else if (strcmp(fn_name, "revoke") == 0) {
                /* revoke(handle, cap_name) → lcn_delegate_revoke(handle, CAP_X) */
                cg_str(g, "lcn_delegate_revoke(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "NULL");
                cg_str(g, ", ");
                if (arg2 && arg2->kind == AST_FIELD_ACCESS && arg2->left &&
                    arg2->left->kind == AST_IDENT && arg2->name) {
                    char qbuf[256];
                    snprintf(qbuf, sizeof(qbuf), "%s.%s",
                             arg2->left->name, arg2->name);
                    const char *cap_def = cg_lookup_cap(g, qbuf);
                    if (cap_def) cg_str(g, cap_def);
                    else cg_str(g, "(LcnCapability)0");
                } else if (arg2) {
                    cg_expr(g, arg2);
                } else {
                    cg_str(g, "(LcnCapability)0");
                }
                cg_str(g, ")");
            } else if (strcmp(fn_name, "revoke_all") == 0) {
                /* revoke_all(handle) → lcn_delegate_revoke_all(handle) */
                cg_str(g, "lcn_delegate_revoke_all(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "NULL");
                cg_str(g, ")");
            } else if (strcmp(fn_name, "has_capability") == 0) {
                /* has_capability(handle, cap_name) → lcn_delegate_has(handle, CAP_X) */
                cg_str(g, "lcn_delegate_has(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "NULL");
                cg_str(g, ", ");
                if (arg2 && arg2->kind == AST_FIELD_ACCESS && arg2->left &&
                    arg2->left->kind == AST_IDENT && arg2->name) {
                    char qbuf[256];
                    snprintf(qbuf, sizeof(qbuf), "%s.%s",
                             arg2->left->name, arg2->name);
                    const char *cap_def = cg_lookup_cap(g, qbuf);
                    if (cap_def) cg_str(g, cap_def);
                    else cg_str(g, "(LcnCapability)0");
                } else if (arg2) {
                    cg_expr(g, arg2);
                } else {
                    cg_str(g, "(LcnCapability)0");
                }
                cg_str(g, ")");
            }
        } else if (is_secret_builtin) {
            AstNode *arg1 = expr->params;
            if (strcmp(fn_name, "secret_redact") == 0) {
                /* secret_redact(val) → "[REDACTED]" (ignores the argument) */
                (void)arg1;
                cg_str(g, "(LcnString)\"[REDACTED]\"");
            } else if (strcmp(fn_name, "secret_unwrap") == 0) {
                /* secret_unwrap(val) → val (pass-through, marks intent) */
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "(LcnString)\"\"");
            }
        } else if (is_string_builtin) {
            AstNode *arg1 = expr->params;
            AstNode *arg2 = arg1 ? arg1->next : NULL;
            AstNode *arg3 = arg2 ? arg2->next : NULL;

            if (strcmp(fn_name, "len") == 0) {
                cg_str(g, "(int64_t)strlen(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "\"\"");
                cg_str(g, ")");
            } else if (strcmp(fn_name, "contains") == 0) {
                cg_str(g, "(strstr(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "\"\"");
                cg_str(g, ", ");
                if (arg2) cg_expr(g, arg2);
                else cg_str(g, "\"\"");
                cg_str(g, ") != NULL)");
            } else if (strcmp(fn_name, "starts_with") == 0) {
                cg_str(g, "(strncmp(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "\"\"");
                cg_str(g, ", ");
                if (arg2) cg_expr(g, arg2);
                else cg_str(g, "\"\"");
                cg_str(g, ", strlen(");
                if (arg2) cg_expr(g, arg2);
                else cg_str(g, "\"\"");
                cg_str(g, ")) == 0)");
            } else if (strcmp(fn_name, "ends_with") == 0) {
                cg_str(g, "lcn_str_ends_with(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "\"\"");
                cg_str(g, ", ");
                if (arg2) cg_expr(g, arg2);
                else cg_str(g, "\"\"");
                cg_str(g, ")");
            } else if (strcmp(fn_name, "env") == 0) {
                cg_str(g, "(getenv(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "\"\"");
                cg_str(g, ") ? getenv(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "\"\"");
                cg_str(g, ") : \"\")");
            } else if (strcmp(fn_name, "str_eq") == 0) {
                cg_str(g, "(strcmp(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "\"\"");
                cg_str(g, ", ");
                if (arg2) cg_expr(g, arg2);
                else cg_str(g, "\"\"");
                cg_str(g, ") == 0)");
            } else if (strcmp(fn_name, "str_replace") == 0) {
                cg_str(g, "lcn_str_replace(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "\"\"");
                cg_str(g, ", ");
                if (arg2) cg_expr(g, arg2);
                else cg_str(g, "\"\"");
                cg_str(g, ", ");
                if (arg3) cg_expr(g, arg3);
                else cg_str(g, "\"\"");
                cg_str(g, ")");
            } else if (strcmp(fn_name, "str_trim") == 0) {
                cg_str(g, "lcn_str_trim(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "\"\"");
                cg_str(g, ")");
            } else if (strcmp(fn_name, "sql_escape") == 0) {
                cg_str(g, "lcn_sql_escape(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "\"\"");
                cg_str(g, ")");
            } else if (strcmp(fn_name, "unwrap") == 0) {
                /* unwrap(Result) → extract string value or "" on error */
                cg_str(g, "lcn_unwrap(");
                if (arg1) cg_expr(g, arg1);
                cg_str(g, ")");
            } else if (strcmp(fn_name, "str_substring") == 0) {
                cg_str(g, "lcn_str_substring(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "\"\"");
                cg_str(g, ", ");
                if (arg2) cg_expr(g, arg2);
                else cg_str(g, "0");
                cg_str(g, ", ");
                if (arg3) cg_expr(g, arg3);
                else cg_str(g, "-1");
                cg_str(g, ")");
            } else if (strcmp(fn_name, "str_split") == 0) {
                /* str_split returns char**, not directly usable — emit helper variable */
                cg_str(g, "/* str_split: use lcn_str_split() */ (LcnString)\"\"");
            } else if (strcmp(fn_name, "to_string") == 0) {
                cg_str(g, "lcn_str_from_int(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "0");
                cg_str(g, ")");
            } else if (strcmp(fn_name, "to_int") == 0) {
                cg_str(g, "(int64_t)atoll(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "\"0\"");
                cg_str(g, ")");
            }
            /* Character-level string builtins */
            else if (strcmp(fn_name, "char_at") == 0) {
                cg_str(g, "lcn_char_at(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "\"\"");
                cg_str(g, ", ");
                if (arg2) cg_expr(g, arg2);
                else cg_str(g, "0");
                cg_str(g, ")");
            } else if (strcmp(fn_name, "char_code") == 0 || strcmp(fn_name, "to_char_code") == 0) {
                cg_str(g, "lcn_char_code(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "\"\"");
                cg_str(g, ")");
            } else if (strcmp(fn_name, "str_from_code") == 0) {
                cg_str(g, "lcn_str_from_code(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "0");
                cg_str(g, ")");
            } else if (strcmp(fn_name, "str_slice") == 0) {
                cg_str(g, "lcn_str_slice(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "\"\"");
                cg_str(g, ", ");
                if (arg2) cg_expr(g, arg2);
                else cg_str(g, "0");
                cg_str(g, ", ");
                if (arg3) cg_expr(g, arg3);
                else cg_str(g, "0");
                cg_str(g, ")");
            } else if (strcmp(fn_name, "str_len") == 0) {
                cg_str(g, "(int64_t)strlen(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "\"\"");
                cg_str(g, ")");
            } else if (strcmp(fn_name, "str_find") == 0) {
                cg_str(g, "lcn_str_find(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "\"\"");
                cg_str(g, ", ");
                if (arg2) cg_expr(g, arg2);
                else cg_str(g, "\"\"");
                cg_str(g, ")");
            } else if (strcmp(fn_name, "str_char_is_alpha") == 0) {
                cg_str(g, "lcn_char_is_alpha(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "\"\"");
                cg_str(g, ")");
            } else if (strcmp(fn_name, "str_char_is_digit") == 0) {
                cg_str(g, "lcn_char_is_digit(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "\"\"");
                cg_str(g, ")");
            } else if (strcmp(fn_name, "str_char_is_alnum") == 0) {
                cg_str(g, "lcn_char_is_alnum(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "\"\"");
                cg_str(g, ")");
            }
        } else if (is_json_builtin) {
            AstNode *arg1 = expr->params;
            AstNode *arg2 = arg1 ? arg1->next : NULL;

            if (strcmp(fn_name, "json_parse") == 0) {
                cg_str(g, "lcn_json_parse(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "\"{}\"");
                cg_str(g, ", strlen(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "\"{}\"");
                cg_str(g, "))");
            } else if (strcmp(fn_name, "json_get") == 0) {
                cg_str(g, "lcn_json_get_string(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "NULL");
                cg_str(g, ", ");
                if (arg2) cg_expr(g, arg2);
                else cg_str(g, "\"\"");
                cg_str(g, ")");
            } else if (strcmp(fn_name, "json_get_number") == 0) {
                cg_str(g, "lcn_json_get_number(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "NULL");
                cg_str(g, ", ");
                if (arg2) cg_expr(g, arg2);
                else cg_str(g, "\"\"");
                cg_str(g, ")");
            } else if (strcmp(fn_name, "json_array_len") == 0) {
                cg_str(g, "(int64_t)lcn_json_array_len(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "NULL");
                cg_str(g, ")");
            } else if (strcmp(fn_name, "json_array_get") == 0) {
                cg_str(g, "lcn_json_array_get(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "NULL");
                cg_str(g, ", (size_t)(");
                if (arg2) cg_expr(g, arg2);
                else cg_str(g, "0");
                cg_str(g, "))");
            } else if (strcmp(fn_name, "json_stringify") == 0) {
                cg_str(g, "lcn_json_stringify(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "NULL");
                cg_str(g, ")");
            }
        } else if (is_stdlib_builtin) {
            AstNode *arg1 = expr->params;
            AstNode *arg2 = arg1 ? arg1->next : NULL;
            AstNode *arg3 = arg2 ? arg2->next : NULL;

            /* Math builtins */
            if (strcmp(fn_name, "abs") == 0) {
                cg_str(g, "llabs("); cg_expr(g, arg1); cg_str(g, ")");
            } else if (strcmp(fn_name, "min") == 0) {
                cg_str(g, "lcn_min("); cg_expr(g, arg1); cg_str(g, ", "); cg_expr(g, arg2); cg_str(g, ")");
            } else if (strcmp(fn_name, "max") == 0) {
                cg_str(g, "lcn_max("); cg_expr(g, arg1); cg_str(g, ", "); cg_expr(g, arg2); cg_str(g, ")");
            } else if (strcmp(fn_name, "clamp") == 0) {
                cg_str(g, "lcn_clamp("); cg_expr(g, arg1); cg_str(g, ", "); cg_expr(g, arg2); cg_str(g, ", "); cg_expr(g, arg3); cg_str(g, ")");
            } else if (strcmp(fn_name, "floor") == 0) {
                cg_str(g, "(int64_t)floor((double)"); cg_expr(g, arg1); cg_str(g, ")");
            } else if (strcmp(fn_name, "ceil") == 0) {
                cg_str(g, "(int64_t)ceil((double)"); cg_expr(g, arg1); cg_str(g, ")");
            } else if (strcmp(fn_name, "round") == 0) {
                cg_str(g, "(int64_t)round((double)"); cg_expr(g, arg1); cg_str(g, ")");
            }
            /* Time builtins */
            else if (strcmp(fn_name, "now_ms") == 0) {
                cg_str(g, "lcn_now_ms()");
            } else if (strcmp(fn_name, "sleep_ms") == 0) {
                if (g->use_green_threads) cg_str(g, "(lcn_green_yield(), ");
                cg_str(g, "lcn_sleep_ms("); cg_expr(g, arg1); cg_str(g, ")");
                if (g->use_green_threads) cg_str(g, ")");
            } else if (strcmp(fn_name, "elapsed_ms") == 0) {
                cg_str(g, "(lcn_now_ms() - ("); cg_expr(g, arg1); cg_str(g, "))");
            } else if (strcmp(fn_name, "format_timestamp") == 0) {
                cg_str(g, "lcn_format_timestamp("); cg_expr(g, arg1); cg_str(g, ")");
            }
            /* Log builtins */
            else if (strcmp(fn_name, "log_info") == 0) {
                cg_str(g, "lcn_log(\"INFO\", "); cg_expr(g, arg1); cg_str(g, ")");
            } else if (strcmp(fn_name, "log_warn") == 0) {
                cg_str(g, "lcn_log(\"WARN\", "); cg_expr(g, arg1); cg_str(g, ")");
            } else if (strcmp(fn_name, "log_error") == 0) {
                cg_str(g, "lcn_log(\"ERROR\", "); cg_expr(g, arg1); cg_str(g, ")");
            } else if (strcmp(fn_name, "log_debug") == 0) {
                cg_str(g, "lcn_log(\"DEBUG\", "); cg_expr(g, arg1); cg_str(g, ")");
            }
            /* Batch builtins */
            else if (strcmp(fn_name, "batch_size") == 0) {
                cg_str(g, "lcn_batch_size()");
            } else if (strcmp(fn_name, "batch_offset") == 0) {
                cg_str(g, "lcn_batch_offset()");
            } else if (strcmp(fn_name, "batch_dry_run") == 0) {
                cg_str(g, "lcn_batch_dry_run()");
            } else if (strcmp(fn_name, "batch_progress") == 0) {
                cg_str(g, "lcn_batch_progress("); cg_expr(g, arg1); cg_str(g, ", "); cg_expr(g, arg2); cg_str(g, ")");
            }
            /* Budget introspection builtins */
            else if (strcmp(fn_name, "budget_tokens_used") == 0) {
                cg_str(g, "lcn_budget_tokens_used()");
            } else if (strcmp(fn_name, "budget_tokens_left") == 0) {
                cg_str(g, "lcn_budget_tokens_left()");
            } else if (strcmp(fn_name, "budget_cost_used") == 0) {
                cg_str(g, "lcn_budget_cost_used()");
            } else if (strcmp(fn_name, "budget_cost_left") == 0) {
                cg_str(g, "lcn_budget_cost_left()");
            } else if (strcmp(fn_name, "budget_percentage") == 0) {
                cg_str(g, "lcn_budget_percentage()");
            } else if (strcmp(fn_name, "budget_elapsed_ms") == 0) {
                cg_str(g, "lcn_budget_elapsed_ms()");
            }
            /* Token estimation builtins */
            else if (strcmp(fn_name, "estimate_tokens") == 0) {
                cg_str(g, "lcn_estimate_tokens("); cg_expr(g, arg1); cg_str(g, ")");
            } else if (strcmp(fn_name, "fits_in_budget") == 0) {
                cg_str(g, "lcn_fits_in_budget("); cg_expr(g, arg1); cg_str(g, ", "); cg_expr(g, arg2); cg_str(g, ")");
            }
            /* Trace builtins */
            else if (strcmp(fn_name, "trace_begin") == 0) {
                cg_str(g, "lcn_trace_begin("); cg_expr(g, arg1); cg_str(g, ")");
            } else if (strcmp(fn_name, "trace_end") == 0) {
                cg_str(g, "lcn_trace_end("); cg_expr(g, arg1); cg_str(g, ")");
            } else if (strcmp(fn_name, "trace_tag") == 0) {
                cg_str(g, "lcn_trace_tag("); cg_expr(g, arg1); cg_str(g, ", "); cg_expr(g, arg2); cg_str(g, ", "); cg_expr(g, arg3); cg_str(g, ")");
            } else if (strcmp(fn_name, "trace_event") == 0) {
                cg_str(g, "lcn_trace_event("); cg_expr(g, arg1); cg_str(g, ", "); cg_expr(g, arg2); cg_str(g, ")");
            }
            /* File I/O builtins */
            else if (strcmp(fn_name, "read_file") == 0 &&
                     g->current_access_policy) {
                cg_str(g, "lcn_read_file_checked(");
                cg_expr(g, arg1);
                cg_fmt(g, ", &lcn_policy_%s)", g->current_access_policy);
            } else if (strcmp(fn_name, "read_file") == 0) {
                cg_str(g, "lcn_read_file("); cg_expr(g, arg1); cg_str(g, ")");
            } else if (strcmp(fn_name, "write_file") == 0 &&
                       g->current_access_policy) {
                cg_str(g, "lcn_write_file_checked(");
                cg_expr(g, arg1); cg_str(g, ", "); cg_expr(g, arg2);
                cg_fmt(g, ", &lcn_policy_%s)", g->current_access_policy);
            } else if (strcmp(fn_name, "write_file") == 0) {
                cg_str(g, "lcn_write_file("); cg_expr(g, arg1); cg_str(g, ", "); cg_expr(g, arg2); cg_str(g, ")");
            } else if (strcmp(fn_name, "read_line") == 0) {
                cg_str(g, "lcn_read_line()");
            }
            /* Env builtins */
            else if (strcmp(fn_name, "env_or") == 0) {
                cg_str(g, "(getenv("); cg_expr(g, arg1); cg_str(g, ") ? getenv("); cg_expr(g, arg1); cg_str(g, ") : "); cg_expr(g, arg2); cg_str(g, ")");
            }
            /* Format builtin */
            else if (strcmp(fn_name, "format") == 0) {
                cg_str(g, "lcn_format("); cg_expr(g, arg1); cg_str(g, ")");
            }
            /* Concurrency builtins */
            else if (strcmp(fn_name, "spawn_parallel") == 0) {
                /* spawn_parallel(fn_ptr, arg) → lcn_spawn_task((LcnTaskFn)fn, arg) */
                cg_str(g, "lcn_spawn_task((LcnTaskFn)");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "NULL");
                cg_str(g, ", (void *)");
                if (arg2) cg_expr(g, arg2);
                else cg_str(g, "NULL");
                cg_str(g, ")");
            } else if (strcmp(fn_name, "threadpool_init") == 0) {
                cg_str(g, "lcn_threadpool_init(");
                if (arg1) cg_expr(g, arg1);
                else cg_str(g, "0");
                cg_str(g, ")");
            } else if (strcmp(fn_name, "threadpool_shutdown") == 0) {
                cg_str(g, "lcn_threadpool_shutdown()");
            }
            /* Vec builtins */
            else if (strcmp(fn_name, "vec_new") == 0) {
                cg_str(g, "lcn_vec_new()");
            } else if (strcmp(fn_name, "vec_push") == 0) {
                cg_str(g, "lcn_vec_push(&");
                if (arg1) cg_expr(g, arg1);
                cg_str(g, ", (void *)(intptr_t)");
                if (arg2) cg_expr(g, arg2);
                cg_str(g, ")");
            } else if (strcmp(fn_name, "vec_get") == 0) {
                cg_str(g, "(LcnString)lcn_vec_get(&");
                if (arg1) cg_expr(g, arg1);
                cg_str(g, ", ");
                if (arg2) cg_expr(g, arg2);
                cg_str(g, ")");
            } else if (strcmp(fn_name, "vec_len") == 0) {
                cg_str(g, "(int64_t)lcn_vec_len(&");
                if (arg1) cg_expr(g, arg1);
                cg_str(g, ")");
            } else if (strcmp(fn_name, "vec_pop") == 0) {
                cg_str(g, "lcn_vec_pop(&");
                if (arg1) cg_expr(g, arg1);
                cg_str(g, ")");
            }
        } else if (is_tool) {
            cg_fmt(g, "lcn_tool_%s(", fn_name);
            if (g->in_agent_method)
                cg_str(g, "self->capabilities");
            else
                cg_str(g, "(LcnCapability)0");
            AstNode *arg = expr->params;
            while (arg) {
                cg_str(g, ", ");
                cg_expr(g, arg);
                arg = arg->next;
            }
            cg_str(g, ")");
        } else if (is_self_method) {
            cg_fmt(g, "lcn_agent_%s_%s(self", g->current_agent_name, fn_name);
            AstNode *arg = expr->params;
            while (arg) {
                cg_str(g, ", ");
                cg_expr(g, arg);
                arg = arg->next;
            }
            cg_str(g, ")");
        } else if (fn_name && strcmp(fn_name, "fetch") == 0 &&
                   g->current_access_policy) {
            /* Runtime-checked fetch: policy enforcement */
            cg_fmt(g, "lcn_fetch_checked(");
            if (expr->params) cg_expr(g, expr->params);
            else cg_str(g, "\"\"");
            cg_fmt(g, ", &lcn_policy_%s)", g->current_access_policy);
        } else if (fn_name && strcmp(fn_name, "fetch") == 0) {
            /* Statically verified or no policy: direct HTTP call */
            cg_str(g, "lcn_http_get(");
            if (expr->params) cg_expr(g, expr->params);
            else cg_str(g, "\"\"");
            cg_str(g, ")");
        } else if (fn_name && strcmp(fn_name, "exec") == 0 &&
                   g->current_access_policy) {
            /* Runtime-checked exec: policy enforcement */
            cg_fmt(g, "lcn_exec_checked(");
            if (expr->params) cg_expr(g, expr->params);
            else cg_str(g, "\"\"");
            cg_fmt(g, ", &lcn_policy_%s)", g->current_access_policy);
        } else if (fn_name && strcmp(fn_name, "exec") == 0) {
            /* Statically verified or no policy: direct exec */
            cg_str(g, "(system(");
            if (expr->params) cg_expr(g, expr->params);
            else cg_str(g, "\"\"");
            cg_str(g, ") == 0)");
        } else if (fn_name && cg_is_closure_var(g, fn_name)) {
            /* Closure call: fn_name is a LcnClosure variable.
             * Emit: ((_lcn_closureN_fn_type)var.fn)((_lcn_closureN_env*)var.env, args...)
             * Since we don't know the exact closure ID at call site, use a
             * generic cast pattern:
             *   ((ret_type (*)(void*, arg_types...))var.fn)(var.env, args...)
             * For Stage 1, default to int64_t return and int64_t args. */
            int argc = ast_list_len(expr->params);
            /* Emit return type and function pointer cast */
            cg_str(g, "((int64_t (*)(void *");
            {
                int ai;
                for (ai = 0; ai < argc; ai++)
                    cg_str(g, ", int64_t");
            }
            cg_fmt(g, "))%s.fn)(%s.env", fn_name, fn_name);
            {
                AstNode *arg = expr->params;
                while (arg) {
                    cg_str(g, ", ");
                    cg_expr(g, arg);
                    arg = arg->next;
                }
            }
            cg_str(g, ")");
        } else if (fn_name &&
                   (strcmp(fn_name, "Ok") == 0 ||
                    strcmp(fn_name, "Err") == 0 ||
                    strcmp(fn_name, "Error") == 0) &&
                   g->current_fn_ret_type &&
                   g->current_fn_ret_type->kind == AST_TYPE_NAMED &&
                   g->current_fn_ret_type->name &&
                   strcmp(g->current_fn_ret_type->name, "Result") == 0 &&
                   g->current_fn_ret_type->generics) {
            /* Ok/Err/Error constructor in a Result-returning function:
             * emit monomorphized constructor, e.g. Result_int64_t_LcnString_ok(val) */
            char mono_name[128];
            mono_result_name(mono_name, sizeof(mono_name),
                             g->current_fn_ret_type->generics);
            if (strcmp(fn_name, "Ok") == 0) {
                cg_fmt(g, "%s_ok(", mono_name);
            } else {
                cg_fmt(g, "%s_err(", mono_name);
            }
            AstNode *arg = expr->params;
            while (arg) {
                cg_expr(g, arg);
                if (arg->next) cg_str(g, ", ");
                arg = arg->next;
            }
            cg_str(g, ")");
        } else if (fn_name && g->in_agent_method && is_sensitive_call(fn_name)) {
            /* Defense-in-depth: wrap sensitive calls with capability check
             * inside agent methods, even if they weren't declared as tools */
            cg_fmt(g, "(lcn_capability_check_tool(\"%s\", "
                   "(const char **)_agent_%s_tools, _agent_%s_tool_count) ? (",
                   fn_name, g->current_agent_name, g->current_agent_name);
            if (!is_codegen_builtin(fn_name)) {
                cg_fmt(g, "lcn_%s", fn_name);
            } else {
                cg_expr(g, expr->left);
            }
            cg_str(g, "(");
            AstNode *arg = expr->params;
            while (arg) {
                cg_expr(g, arg);
                if (arg->next) cg_str(g, ", ");
                arg = arg->next;
            }
            cg_fmt(g, ")) : (fprintf(stderr, \"SECURITY: capability denied: %s in agent %s\\n\"), "
                   "(%s)0))",
                   fn_name, g->current_agent_name,
                   /* Default zero-value type for denied calls */
                   "int64_t");
        } else {
            /* User-defined function: prefix with lcn_ to match definition */
            if (fn_name && !is_codegen_builtin(fn_name)) {
                cg_fmt(g, "lcn_%s", fn_name);
            } else {
                cg_expr(g, expr->left);
            }
            cg_str(g, "(");
            AstNode *arg = expr->params;
            while (arg) {
                cg_expr(g, arg);
                if (arg->next) cg_str(g, ", ");
                arg = arg->next;
            }
            cg_str(g, ")");
        }
        break;
    }

    case AST_FIELD_ACCESS:
        /* Enum variant access: Color.Red → "Red" (simple) or enum constant (ADT) */
        if (expr->left && expr->left->kind == AST_IDENT && expr->left->name && expr->name) {
            int ei = cg_lookup_enum(g, expr->left->name);
            if (ei >= 0) {
                if (g->enums[ei].has_data) {
                    /* ADT enum: check if this variant has no fields → call zero-arg constructor */
                    AstNode *v = g->enums[ei].node->params;
                    bool variant_has_data = false;
                    while (v) {
                        if (v->kind == AST_VARIANT && v->name &&
                            strcmp(v->name, expr->name) == 0) {
                            if (v->params) variant_has_data = true;
                            break;
                        }
                        v = v->next;
                    }
                    if (variant_has_data) {
                        /* Has fields — this shouldn't appear as bare field access,
                         * it should be a call. Emit the constructor name. */
                        cg_fmt(g, "%s_%s_new", expr->left->name, expr->name);
                    } else {
                        /* No-data variant: call zero-arg constructor */
                        cg_fmt(g, "%s_%s_new()", expr->left->name, expr->name);
                    }
                } else {
                    /* Simple enum: emit string define */
                    cg_fmt(g, "%s_%s", expr->left->name, expr->name);
                }
                break;
            }
        }
        /* Field access on ADT enum variable: t.name → t._Ident_name
         * Search all variants for a field matching the accessed name. */
        if (expr->left && expr->left->kind == AST_IDENT && expr->left->name && expr->name) {
            const char *var_type = cg_var_agent_type(g, expr->left->name);
            if (var_type) {
                int ei = cg_lookup_enum(g, var_type);
                if (ei >= 0 && g->enums[ei].has_data) {
                    /* Find which variant has this field */
                    AstNode *v = g->enums[ei].node->params;
                    while (v) {
                        if (v->kind == AST_VARIANT && v->params) {
                            AstNode *f = v->params;
                            while (f) {
                                if (f->kind == AST_FIELD && f->name &&
                                    strcmp(f->name, expr->name) == 0) {
                                    cg_expr(g, expr->left);
                                    cg_fmt(g, "._%s_%s", v->name, f->name);
                                    goto field_done;
                                }
                                f = f->next;
                            }
                        }
                        v = v->next;
                    }
                }
            }
        }
        cg_expr(g, expr->left);
        /* Use -> for pointer member access on self inside impl methods */
        if (g->in_impl_method && expr->left && expr->left->kind == AST_IDENT &&
            expr->left->name && strcmp(expr->left->name, "self") == 0)
            cg_fmt(g, "->%s", expr->name ? expr->name : "");
        else
            cg_fmt(g, ".%s", expr->name ? expr->name : "");
    field_done:
        break;

    case AST_METHOD_CALL: {
        /* Metrics histogram observe: metrics.field.observe(val) */
        if (g->has_metrics && expr->name && strcmp(expr->name, "observe") == 0 &&
            expr->left && expr->left->kind == AST_FIELD_ACCESS &&
            expr->left->left && expr->left->left->kind == AST_IDENT &&
            expr->left->left->name && strcmp(expr->left->left->name, "metrics") == 0 &&
            expr->left->name) {
            cg_fmt(g, "_lcn_metrics_observe_%s(", expr->left->name);
            if (expr->params) cg_expr(g, expr->params);
            cg_str(g, ")");
            goto method_done;
        }
        /* ADT enum variant constructor: Token.Ident("hello") → Token_Ident_new("hello") */
        if (expr->left && expr->left->kind == AST_IDENT && expr->left->name && expr->name) {
            int ei = cg_lookup_enum(g, expr->left->name);
            if (ei >= 0 && g->enums[ei].has_data) {
                cg_fmt(g, "%s_%s_new(", expr->left->name, expr->name);
                AstNode *arg = expr->params;
                while (arg) {
                    cg_expr(g, arg);
                    if (arg->next) cg_str(g, ", ");
                    arg = arg->next;
                }
                cg_str(g, ")");
                goto method_done;
            }
        }

        /* MCP alias method: db.call("tool", args) → lcn_mcp_db_call("tool", args) */
        if (expr->left && expr->left->kind == AST_IDENT && expr->left->name &&
            expr->name && strcmp(expr->name, "call") == 0) {
            int mi;
            for (mi = 0; mi < g->mcp_alias_count; mi++) {
                if (strcmp(g->mcp_aliases[mi].alias, expr->left->name) == 0) {
                    cg_fmt(g, "lcn_mcp_%s_call(", expr->left->name);
                    AstNode *arg = expr->params;
                    while (arg) {
                        cg_expr(g, arg);
                        if (arg->next) cg_str(g, ", ");
                        arg = arg->next;
                    }
                    cg_str(g, ")");
                    goto method_done;
                }
            }
        }

        /* Driver alias methods: db.connect(), db.query(), db.execute(), etc.
         * Rewrite to lcn_driver_<alias>_<method>(args) */
        if (expr->left && expr->left->kind == AST_IDENT && expr->left->name && expr->name) {
            int mi;
            for (mi = 0; mi < g->mcp_alias_count; mi++) {
                if (strcmp(g->mcp_aliases[mi].alias, expr->left->name) == 0) {
                    const char *method = expr->name;
                    /* Check if this is a known driver method */
                    if (strcmp(method, "connect") == 0 || strcmp(method, "query") == 0 ||
                        strcmp(method, "execute") == 0 || strcmp(method, "close") == 0 ||
                        strcmp(method, "row_count") == 0 || strcmp(method, "get") == 0 ||
                        strcmp(method, "get_number") == 0 || strcmp(method, "free") == 0 ||
                        strcmp(method, "escape") == 0) {
                        cg_fmt(g, "lcn_driver_%s_%s(", expr->left->name, method);
                        AstNode *arg = expr->params;
                        while (arg) {
                            cg_expr(g, arg);
                            if (arg->next) cg_str(g, ", ");
                            arg = arg->next;
                        }
                        cg_str(g, ")");
                        goto method_done;
                    }
                }
            }
        }

        /* Model alias methods: clf.predict(text) → lcn_model_clf_predict(text)
         *                      clf.info() → lcn_model_clf_info() */
        if (expr->left && expr->left->kind == AST_IDENT && expr->left->name && expr->name) {
            int mi;
            for (mi = 0; mi < g->model_alias_count; mi++) {
                if (strcmp(g->model_aliases[mi].alias, expr->left->name) == 0) {
                    const char *method = expr->name;
                    if (strcmp(method, "predict") == 0 || strcmp(method, "info") == 0) {
                        cg_fmt(g, "lcn_model_%s_%s(", expr->left->name, method);
                        AstNode *arg = expr->params;
                        while (arg) {
                            cg_expr(g, arg);
                            if (arg->next) cg_str(g, ", ");
                            arg = arg->next;
                        }
                        cg_str(g, ")");
                        goto method_done;
                    }
                }
            }
        }

        /* Channel methods: .send() .recv() .close() .len() .is_closed() .try_recv() */
        if (expr->name && (strcmp(expr->name, "send") == 0 ||
                           strcmp(expr->name, "recv") == 0 ||
                           strcmp(expr->name, "try_recv") == 0 ||
                           strcmp(expr->name, "close") == 0 ||
                           strcmp(expr->name, "is_closed") == 0)) {
            if (strcmp(expr->name, "send") == 0) {
                /* ch.send(val) → { LcnString _tmp = val; lcn_channel_send(ch, &_tmp); }
                 * Uses statement expression for inline usage. */
                static int send_counter = 0;
                int _sid = send_counter++;
                cg_fmt(g, "({ LcnString _ch_send_%d = ", _sid);
                if (expr->params) cg_expr(g, expr->params);
                else cg_str(g, "\"\"");
                cg_fmt(g, "; lcn_channel_send(");
                cg_expr(g, expr->left);
                cg_fmt(g, ", &_ch_send_%d); })", _sid);
                break;
            }
            if (strcmp(expr->name, "recv") == 0) {
                /* ch.recv() → ({ LcnString _tmp; lcn_channel_recv(ch, &_tmp); _tmp; })
                 * With green threads: insert yield point before blocking recv */
                static int recv_counter = 0;
                int _rid = recv_counter++;
                cg_str(g, "({ ");
                if (g->use_green_threads) cg_str(g, "lcn_green_yield(); ");
                cg_fmt(g, "LcnString _ch_recv_%d; lcn_channel_recv(", _rid);
                cg_expr(g, expr->left);
                cg_fmt(g, ", &_ch_recv_%d); _ch_recv_%d; })", _rid, _rid);
                break;
            }
            if (strcmp(expr->name, "try_recv") == 0) {
                /* ch.try_recv() → ({ LcnString _tmp = NULL; lcn_channel_try_recv(ch, &_tmp); _tmp; }) */
                static int tryrecv_counter = 0;
                int _trid = tryrecv_counter++;
                cg_fmt(g, "({ LcnString _ch_tryrecv_%d = NULL; lcn_channel_try_recv(", _trid);
                cg_expr(g, expr->left);
                cg_fmt(g, ", &_ch_tryrecv_%d); _ch_tryrecv_%d; })", _trid, _trid);
                break;
            }
            if (strcmp(expr->name, "close") == 0) {
                cg_str(g, "lcn_channel_close(");
                cg_expr(g, expr->left);
                cg_str(g, ")");
                break;
            }
            if (strcmp(expr->name, "is_closed") == 0) {
                cg_str(g, "lcn_channel_is_closed(");
                cg_expr(g, expr->left);
                cg_str(g, ")");
                break;
            }
        }

        /* Check if obj has a known type (agent or struct) */
        const char *var_type = NULL;
        if (expr->left && expr->left->kind == AST_IDENT && expr->left->name)
            var_type = cg_var_agent_type(g, expr->left->name);

        /* Check impl methods first (struct methods take priority) */
        const char *struct_type = NULL;
        if (var_type && expr->name) {
            int mi;
            for (mi = 0; mi < g->impl_method_count; mi++) {
                if (strcmp(g->impl_methods[mi].type_name, var_type) == 0 &&
                    strcmp(g->impl_methods[mi].method_name, expr->name) == 0) {
                    struct_type = var_type;
                    break;
                }
            }
        }

        if (struct_type) {
            cg_fmt(g, "%s_%s(&", struct_type, expr->name);
            cg_expr(g, expr->left);
        } else if (var_type && cg_lookup_agent(g, var_type)) {
            /* Agent variable → rewrite to lcn_agent_X_method */
            cg_fmt(g, "lcn_agent_%s_%s(&", var_type, expr->name ? expr->name : "method");
            cg_expr(g, expr->left);
        } else {
            /* Generic: obj.method(args) → method(&obj, args) */
            cg_fmt(g, "%s(&", expr->name ? expr->name : "method");
            cg_expr(g, expr->left);
        }
        if (expr->params) cg_str(g, ", ");
        {
            AstNode *arg = expr->params;
            while (arg) {
                cg_expr(g, arg);
                if (arg->next) cg_str(g, ", ");
                arg = arg->next;
            }
        }
        cg_str(g, ")");
        method_done:
        break;
    }

    case AST_INDEX:
        cg_expr(g, expr->left);
        cg_str(g, "[");
        cg_expr(g, expr->right);
        cg_str(g, "]");
        break;

    case AST_ARRAY: {
        /* For now, emit as compound literal or list */
        cg_str(g, "/* array */ (void *)0");
        break;
    }

    case AST_MAP: {
        /* Struct literal: Name { field: value, ... } → (Name){.field = value, ...} */
        if (expr->name) {
            cg_fmt(g, "(%s){", expr->name);
            AstNode *entry = expr->params;
            while (entry) {
                if (entry->kind == AST_MAP_ENTRY && entry->left &&
                    entry->left->kind == AST_IDENT && entry->left->name) {
                    cg_fmt(g, ".%s = ", entry->left->name);
                    cg_expr(g, entry->right);
                    if (entry->next) cg_str(g, ", ");
                }
                entry = entry->next;
            }
            cg_str(g, "}");
        } else {
            cg_str(g, "/* map literal */ (void *)0");
        }
        break;
    }

    case AST_CAST:
        cg_str(g, "(");
        cg_type(g, expr->type_expr);
        cg_str(g, ")(");
        cg_expr(g, expr->left);
        cg_str(g, ")");
        break;

    case AST_REF:
        /* Ownership: &x = immutable borrow (const ptr),
         *            &mut x = mutable borrow (non-const ptr) */
        if (expr->is_mut) {
            cg_str(g, "/* &mut */ &(");
        } else {
            cg_str(g, "/* & */ &(");
        }
        cg_expr(g, expr->left);
        cg_str(g, ")");
        break;

    case AST_DEREF:
        cg_str(g, "*(");
        cg_expr(g, expr->left);
        cg_str(g, ")");
        break;

    case AST_PIPE:
        /* a |> b → b(a)
         * Special cases:
         *   a |> keep where <cond>   → filter: iterate a, test cond, collect matches
         *   a |> each <field>        → map: iterate a, extract field, collect results
         */
        if (expr->right && expr->right->kind == AST_KEEP_WHERE) {
            /* items |> keep where <condition>
             * Desugar to GCC statement-expression:
             *   ({ int _kw_N = 0;
             *      for (int _kw_i = 0; _kw_i < items_count; _kw_i++) {
             *          if (<condition>) _kw_N++;
             *      }
             *      _kw_N; })
             * Since we don't have full collection types in Stage 0,
             * emit a filter loop pattern with a result count. */
            static int keep_where_counter = 0;
            int kwid = keep_where_counter++;
            cg_fmt(g, "({ /* keep where */ int _kw_%d = 0; ", kwid);
            cg_str(g, "/* filter: iterate ");
            cg_expr(g, expr->left);
            cg_str(g, ", test (");
            cg_expr(g, expr->right->left);
            cg_fmt(g, "), count matches */ _kw_%d; })", kwid);
        } else if (expr->right && expr->right->kind == AST_EACH) {
            /* items |> each <field>
             * Desugar to GCC statement-expression extracting field from each element.
             * Since we don't have full collections, emit a map pattern. */
            static int each_counter = 0;
            int eid = each_counter++;
            cg_fmt(g, "({ /* each %s */ int _each_%d = 0; ",
                   expr->right->name ? expr->right->name : "_", eid);
            cg_str(g, "/* map: iterate ");
            cg_expr(g, expr->left);
            cg_fmt(g, ", extract .%s */ _each_%d; })",
                   expr->right->name ? expr->right->name : "_", eid);
        } else {
            /* Standard pipe: a |> b → b(a) */
            cg_expr(g, expr->right);
            cg_str(g, "(");
            cg_expr(g, expr->left);
            cg_str(g, ")");
        }
        break;

    case AST_TRY:
        /* expr? → simplified: just evaluate expr */
        cg_expr(g, expr->left);
        break;

    case AST_TRY_OTHERWISE: {
        /* try expr otherwise fallback
         * Desugar to: ({ LcnLlmOutput _r = expr; if (_r.kind == LCN_LLM_ERROR) { _r = fallback; } _r; })
         * For general expressions: ({ typeof(expr) _r = expr; ... }) — simplified as LcnLlmOutput */
        static int try_ow_counter = 0;
        int toid = try_ow_counter++;
        cg_fmt(g, "({ LcnLlmOutput _to_%d = ", toid);
        if (expr->left) cg_expr(g, expr->left);
        else cg_str(g, "(LcnLlmOutput){0}");
        cg_fmt(g, "; if (_to_%d.kind == LCN_LLM_ERROR) { _to_%d = ", toid, toid);
        if (expr->right) cg_expr(g, expr->right);
        else cg_str(g, "(LcnLlmOutput){0}");
        cg_fmt(g, "; } _to_%d; })", toid);
        break;
    }

    case AST_KEEP_WHERE: {
        /* keep where <condition>
         * Used in pipe: items |> keep where score > 0.5
         * When used as RHS of a pipe, the LHS is the input collection.
         * Since this appears in expr context (via pipe), we need a GCC statement-expression.
         * But keep where doesn't know the input — it's the pipe that feeds it.
         * We emit a filter helper comment; the pipe codegen handles the wiring.
         * Standalone: emit as a comment placeholder.
         *
         * In pipe context (AST_PIPE with RHS=AST_KEEP_WHERE), we handle it specially
         * in the AST_PIPE case above. Here we handle the standalone case. */
        cg_str(g, "/* keep where: standalone usage unsupported */ 0");
        break;
    }

    case AST_EACH: {
        /* each <field>
         * Standalone usage is not meaningful; this is handled in pipe context.
         * See AST_PIPE special case. */
        cg_str(g, "/* each: standalone usage unsupported */ 0");
        break;
    }

    case AST_IF:
        /* Ternary for if-expressions */
        cg_str(g, "(");
        cg_expr(g, expr->left);
        cg_str(g, " ? ");
        if (expr->right) cg_expr(g, expr->right);
        else cg_str(g, "0");
        cg_str(g, " : ");
        if (expr->params) cg_expr(g, expr->params);
        else cg_str(g, "0");
        cg_str(g, ")");
        break;

    case AST_RANGE:
        /* a..b — no direct C equivalent, emit as comment */
        cg_str(g, "/* range */ 0");
        break;

    case AST_MATCH:
        /* Match as expression — emit inline using statement codegen */
        cg_str(g, "/* match expr */ 0");
        break;

    case AST_TUPLE:
        cg_str(g, "/* tuple */ 0");
        break;

    case AST_SPAWN: {
        /* Real async: emit a static wrapper function and spawn via thread pool.
         * Returns a task handle from lcn_spawn_task. */
        int sid = g->spawn_counter++;
        char spawn_fn[64];
        snprintf(spawn_fn, sizeof(spawn_fn), "__lcn_spawn_%d", sid);

        /* Build the wrapper function into closure_defs_buf */
        {
            char hdr[128];
            snprintf(hdr, sizeof(hdr),
                     "static void *%s(void *__lcn_spawn_arg) {\n"
                     "    (void)__lcn_spawn_arg;\n", spawn_fn);
            cg_closure_def_append(g, hdr);

            if (expr->left) {
                char   *saved_buf = g->buf;
                size_t  saved_len = g->len;
                size_t  saved_cap = g->cap;

                g->cap = 4096;
                g->buf = (char *)malloc(g->cap);
                g->buf[0] = '\0';
                g->len = 0;

                int saved_indent = g->indent;
                g->indent = 1;

                if (expr->left->kind == AST_BLOCK) {
                    AstNode *s = expr->left->params;
                    while (s) { cg_stmt(g, s); s = s->next; }
                } else {
                    cg_indent(g);
                    cg_expr(g, expr->left);
                    cg_str(g, ";\n");
                }

                g->indent = saved_indent;
                cg_closure_def_append(g, g->buf);

                free(g->buf);
                g->buf = saved_buf;
                g->len = saved_len;
                g->cap = saved_cap;
            }

            cg_closure_def_append(g, "    return NULL;\n}\n\n");
        }

        if (g->use_green_threads) {
            cg_fmt(g, "lcn_green_spawn(%s, NULL)", spawn_fn);
        } else {
            cg_fmt(g, "lcn_spawn_task((LcnTaskFn)%s, NULL)", spawn_fn);
        }
        break;
    }

    case AST_TASK_GROUP: {
        /* Structured concurrency: task_group { spawn expr1; spawn expr2; ... }
         * Emits a block that creates a task group, spawns all tasks,
         * awaits all, and yields the results array.
         *
         * In expression context we use GCC statement-expression ({ ... })
         * so the whole thing evaluates to the void** results. */
        int tg_id = g->spawn_counter++;
        char tg_var[64];
        snprintf(tg_var, sizeof(tg_var), "__lcn_tg_%d", tg_id);
        char tg_res[64];
        snprintf(tg_res, sizeof(tg_res), "__lcn_tg_results_%d", tg_id);

        /* Open a statement-expression */
        cg_str(g, "({\n");
        g->indent++;

        /* Create the task group */
        cg_indent(g);
        cg_fmt(g, "LcnTaskGroup *%s = lcn_task_group_new();\n", tg_var);

        /* Walk the body block; rewrite each AST_SPAWN into a task_group_spawn */
        if (expr->left && expr->left->kind == AST_BLOCK) {
            AstNode *s = expr->left->params;
            while (s) {
                if (s->kind == AST_EXPR_STMT && s->left && s->left->kind == AST_SPAWN) {
                    /* Emit wrapper function for this spawn */
                    int sid = g->spawn_counter++;
                    char spawn_fn2[64];
                    snprintf(spawn_fn2, sizeof(spawn_fn2), "__lcn_spawn_%d", sid);

                    char hdr[128];
                    snprintf(hdr, sizeof(hdr),
                             "static void *%s(void *__lcn_spawn_arg) {\n"
                             "    (void)__lcn_spawn_arg;\n", spawn_fn2);
                    cg_closure_def_append(g, hdr);

                    if (s->left->left) {
                        char   *saved_buf = g->buf;
                        size_t  saved_len = g->len;
                        size_t  saved_cap = g->cap;
                        g->cap = 4096;
                        g->buf = (char *)malloc(g->cap);
                        g->buf[0] = '\0';
                        g->len = 0;
                        int saved_indent = g->indent;
                        g->indent = 1;

                        if (s->left->left->kind == AST_BLOCK) {
                            AstNode *bs = s->left->left->params;
                            while (bs) { cg_stmt(g, bs); bs = bs->next; }
                        } else {
                            cg_indent(g);
                            cg_expr(g, s->left->left);
                            cg_str(g, ";\n");
                        }

                        g->indent = saved_indent;
                        cg_closure_def_append(g, g->buf);
                        free(g->buf);
                        g->buf = saved_buf;
                        g->len = saved_len;
                        g->cap = saved_cap;
                    }

                    cg_closure_def_append(g, "    return NULL;\n}\n\n");

                    /* Emit task_group_spawn call */
                    cg_indent(g);
                    cg_fmt(g, "lcn_task_group_spawn(%s, (LcnTaskFn)%s, NULL);\n",
                           tg_var, spawn_fn2);
                } else if (s->kind == AST_SPAWN) {
                    /* Direct spawn statement (not wrapped in EXPR_STMT) */
                    int sid = g->spawn_counter++;
                    char spawn_fn2[64];
                    snprintf(spawn_fn2, sizeof(spawn_fn2), "__lcn_spawn_%d", sid);

                    char hdr[128];
                    snprintf(hdr, sizeof(hdr),
                             "static void *%s(void *__lcn_spawn_arg) {\n"
                             "    (void)__lcn_spawn_arg;\n", spawn_fn2);
                    cg_closure_def_append(g, hdr);

                    if (s->left) {
                        char   *saved_buf = g->buf;
                        size_t  saved_len = g->len;
                        size_t  saved_cap = g->cap;
                        g->cap = 4096;
                        g->buf = (char *)malloc(g->cap);
                        g->buf[0] = '\0';
                        g->len = 0;
                        int saved_indent = g->indent;
                        g->indent = 1;

                        if (s->left->kind == AST_BLOCK) {
                            AstNode *bs = s->left->params;
                            while (bs) { cg_stmt(g, bs); bs = bs->next; }
                        } else {
                            cg_indent(g);
                            cg_expr(g, s->left);
                            cg_str(g, ";\n");
                        }

                        g->indent = saved_indent;
                        cg_closure_def_append(g, g->buf);
                        free(g->buf);
                        g->buf = saved_buf;
                        g->len = saved_len;
                        g->cap = saved_cap;
                    }

                    cg_closure_def_append(g, "    return NULL;\n}\n\n");

                    cg_indent(g);
                    cg_fmt(g, "lcn_task_group_spawn(%s, (LcnTaskFn)%s, NULL);\n",
                           tg_var, spawn_fn2);
                } else {
                    /* Non-spawn statement: emit normally */
                    cg_stmt(g, s);
                }
                s = s->next;
            }
        }

        /* Await all tasks and yield results */
        cg_indent(g);
        cg_fmt(g, "void **%s = lcn_task_group_await_all(%s);\n", tg_res, tg_var);
        cg_indent(g);
        cg_fmt(g, "lcn_task_group_free(%s);\n", tg_var);
        cg_indent(g);
        cg_fmt(g, "%s;\n", tg_res);

        g->indent--;
        cg_indent(g);
        cg_str(g, "})");
        break;
    }

    case AST_AWAIT:
        /* await handle → lcn_await_task(handle) or lcn_green_await(handle) */
        if (g->use_green_threads) {
            cg_str(g, "lcn_green_await((GreenThread *)");
        } else {
            cg_str(g, "lcn_await_task((LcnTaskHandle *)");
        }
        if (expr->left) cg_expr(g, expr->left);
        else cg_str(g, "NULL");
        cg_str(g, ")");
        break;

    case AST_ASK:
        /* ask(question) → lcn_ask_typed(endpoint, model, prompt, question, budget)
         * Returns LcnLlmOutput ADT — must be handled with match */
        if (g->in_agent_method) {
            cg_str(g, "lcn_ask_typed(self->endpoint, self->model, self->prompt, ");
            cg_expr(g, expr->left);
            if (g->current_access_policy) {
                cg_fmt(g, ", &self->budget, &lcn_policy_%s, self->api_key)",
                       g->current_access_policy);
            } else {
                cg_str(g, ", &self->budget, NULL, self->api_key)");
            }
        } else {
            cg_str(g, "lcn_ask_typed(NULL, NULL, NULL, ");
            cg_expr(g, expr->left);
            cg_str(g, ", NULL, NULL, NULL)");
        }
        break;

    case AST_TELL:
        /* tell target message → printf("AGENT → target: %s\n", message) */
        cg_str(g, "printf(\"AGENT -> ");
        if (expr->left && expr->left->kind == AST_IDENT && expr->left->name)
            cg_str(g, expr->left->name);
        else
            cg_str(g, "?");
        cg_str(g, ": %s\\n\", ");
        cg_expr(g, expr->right);
        cg_str(g, ")");
        break;

    case AST_CHANNEL:
        /* chan<T>(N) → lcn_channel_new(N, sizeof(LcnString))
         * Default elem_size is sizeof(LcnString) for string channels.
         * Type-specific channels can be added in Stage 2. */
        cg_str(g, "lcn_channel_new(");
        if (expr->left) cg_expr(g, expr->left);
        else cg_str(g, "16");
        cg_str(g, ", sizeof(LcnString))");
        break;

    case AST_SELECT: {
        /* Stage 1: real select multiplexing via lcn_select() polling.
         * select { msg from ch1 -> { ... } err from ch2 -> { ... } }
         * Generates:
         *   { LcnChannel *_sel_chs[] = { ch1, ch2 };
         *     int _sel_idx = lcn_select(_sel_chs, 2, 0);
         *     switch (_sel_idx) {
         *     case 0: { LcnString msg; lcn_channel_recv(ch1, &msg); ... break; }
         *     case 1: { LcnString err; lcn_channel_recv(ch2, &err); ... break; }
         *     } }
         */
        static int select_counter = 0;
        int sid = select_counter++;

        /* Count arms */
        int arm_count = 0;
        AstNode *arm = expr->params;
        while (arm) {
            if (arm->kind == AST_SELECT_ARM) arm_count++;
            arm = arm->next;
        }

        if (arm_count == 0) {
            cg_str(g, "/* select: no arms */ (-1)");
            break;
        }

        /* Build channel array */
        cg_fmt(g, "/* select */ ({ LcnChannel *_sel_chs_%d[] = { ", sid);
        arm = expr->params;
        {
            bool first = true;
            while (arm) {
                if (arm->kind == AST_SELECT_ARM) {
                    if (!first) cg_str(g, ", ");
                    cg_expr(g, arm->left);
                    first = false;
                }
                arm = arm->next;
            }
        }
        cg_fmt(g, " }; int _sel_idx_%d = lcn_select(_sel_chs_%d, %d, 0); ", sid, sid, arm_count);

        /* Switch on result */
        cg_fmt(g, "switch (_sel_idx_%d) { ", sid);
        arm = expr->params;
        {
            int case_idx = 0;
            while (arm) {
                if (arm->kind == AST_SELECT_ARM) {
                    cg_fmt(g, "case %d: { ", case_idx);

                    /* Declare and receive the variable */
                    if (arm->name) {
                        cg_fmt(g, "LcnString %s; lcn_channel_recv(", arm->name);
                        cg_expr(g, arm->left);
                        cg_fmt(g, ", &%s); ", arm->name);
                    }

                    /* Emit the arm body */
                    if (arm->right && arm->right->params) {
                        AstNode *s = arm->right->params;
                        while (s) { cg_stmt(g, s); s = s->next; }
                    }

                    cg_str(g, " break; } ");
                    case_idx++;
                }
                arm = arm->next;
            }
        }
        cg_fmt(g, "} _sel_idx_%d; })", sid);
        break;
    }

    case AST_CLOSURE: {
        /* Stage 1 closure implementation:
         *   1. Detect captured variables
         *   2. Emit capture struct + static function into deferred buffer
         *   3. Emit inline code to allocate env, populate, build LcnClosure
         */
        int cid = g->closure_counter++;
        AstNode *closure_params = expr->params;
        AstNode *closure_body = expr->left;

        /* Collect captured variables */
        CaptureList captures;
        captures.count = 0;
        collect_captures(closure_body, closure_params, &captures);

        /* Determine the C type for each captured variable using string_vars tracking.
         * Variables known to be strings get LcnString, others get int64_t. */
        const char *cap_types[MAX_CAPTURES];
        {
            int ci;
            for (ci = 0; ci < captures.count; ci++) {
                bool is_str = false;
                int si;
                for (si = 0; si < g->string_var_count; si++) {
                    if (strcmp(g->string_vars[si], captures.names[ci]) == 0) {
                        is_str = true;
                        break;
                    }
                }
                cap_types[ci] = is_str ? "LcnString" : "int64_t";
            }
        }

        /* Emit capture struct definition (deferred — goes before current function) */
        if (captures.count > 0) {
            cg_closure_def_fmt(g, "typedef struct {\n");
            {
                int ci;
                for (ci = 0; ci < captures.count; ci++)
                    cg_closure_def_fmt(g, "    %s %s;\n", cap_types[ci], captures.names[ci]);
            }
            cg_closure_def_fmt(g, "} _lcn_closure_%d_env;\n\n", cid);
        }

        /* Emit closure function definition (deferred) */
        /* Return type heuristic: check if body is a block with implicit return,
         * or a single expression. For Stage 1, default to LcnString for
         * string operations and int64_t for arithmetic. We use int64_t as
         * the default since closures often do arithmetic. If the body involves
         * string concat or a string literal, use LcnString. */
        bool returns_string = false;
        {
            AstNode *body_check = closure_body;
            /* If body is a block, look at the last statement */
            if (body_check && body_check->kind == AST_BLOCK) {
                AstNode *last = body_check->params;
                while (last && last->next) last = last->next;
                if (last && last->kind == AST_EXPR_STMT) body_check = last->left;
                else if (last) body_check = last;
            }
            if (body_check) {
                if (body_check->kind == AST_STRING_LIT)
                    returns_string = true;
                else if (body_check->kind == AST_BINARY &&
                         body_check->val.op == TOK_PLUS) {
                    /* Only string concat if at least one operand is a string */
                    bool lhs_str = body_check->left &&
                                   (body_check->left->kind == AST_STRING_LIT ||
                                    (body_check->left->kind == AST_IDENT &&
                                     body_check->left->name &&
                                     might_be_string_expr(g, body_check->left)));
                    bool rhs_str = body_check->right &&
                                   (body_check->right->kind == AST_STRING_LIT ||
                                    (body_check->right->kind == AST_IDENT &&
                                     body_check->right->name &&
                                     might_be_string_expr(g, body_check->right)));
                    if (lhs_str || rhs_str) returns_string = true;
                }
                else if (body_check->kind == AST_CALL &&
                         body_check->left && body_check->left->kind == AST_IDENT &&
                         body_check->left->name &&
                         !cg_is_closure_var(g, body_check->left->name)) {
                    /* Known string-returning builtins */
                    const char *cn = body_check->left->name;
                    if (strcmp(cn, "to_string") == 0 || strcmp(cn, "format") == 0 ||
                        strcmp(cn, "str_replace") == 0 || strcmp(cn, "str_trim") == 0 ||
                        strcmp(cn, "read_file") == 0)
                        returns_string = true;
                }
            }
        }
        const char *ret_type_c = returns_string ? "LcnString" : "int64_t";

        if (captures.count > 0) {
            cg_closure_def_fmt(g, "static %s _lcn_closure_%d_fn(_lcn_closure_%d_env *_env",
                               ret_type_c, cid, cid);
        } else {
            cg_closure_def_fmt(g, "static %s _lcn_closure_%d_fn(void *_env",
                               ret_type_c, cid);
        }
        {
            AstNode *pp;
            for (pp = closure_params; pp; pp = pp->next) {
                /* For Stage 1, all parameters default to int64_t unless
                 * they have a type annotation */
                const char *ptype = "int64_t";
                if (pp->type_expr && pp->type_expr->kind == AST_TYPE_NAMED &&
                    pp->type_expr->name) {
                    if (strcmp(pp->type_expr->name, "string") == 0 ||
                        strcmp(pp->type_expr->name, "String") == 0)
                        ptype = "LcnString";
                    else if (strcmp(pp->type_expr->name, "i32") == 0)
                        ptype = "int32_t";
                    else if (strcmp(pp->type_expr->name, "i64") == 0 ||
                             strcmp(pp->type_expr->name, "int") == 0)
                        ptype = "int64_t";
                    else if (strcmp(pp->type_expr->name, "f64") == 0 ||
                             strcmp(pp->type_expr->name, "float") == 0)
                        ptype = "double";
                    else if (strcmp(pp->type_expr->name, "bool") == 0)
                        ptype = "bool";
                }
                cg_closure_def_fmt(g, ", %s %s", ptype, pp->name ? pp->name : "_");
            }
        }
        cg_closure_def_append(g, ") {\n");

        /* Emit capture unpacking (only if we have captures — for readability) */
        if (captures.count > 0) {
            int ci;
            for (ci = 0; ci < captures.count; ci++) {
                cg_closure_def_fmt(g, "    %s %s = _env->%s;\n",
                                   cap_types[ci], captures.names[ci], captures.names[ci]);
            }
        }

        /* Emit body. For a single expression, wrap in return.
         * For a block, emit all statements with implicit return on last. */
        if (closure_body && closure_body->kind == AST_BLOCK) {
            /* Walk block statements, emit the last as 'return expr;' */
            AstNode *s = closure_body->params;
            while (s) {
                if (!s->next && s->kind == AST_EXPR_STMT && s->left) {
                    /* Last statement — implicit return */
                    cg_closure_def_append(g, "    return ");
                    /* We need to emit the expression to the deferred buffer.
                     * Since cg_expr writes to the main buffer, we swap buffers. */
                    {
                        char *saved_buf = g->buf;
                        size_t saved_len = g->len;
                        size_t saved_cap = g->cap;

                        g->buf = (char *)malloc(4096);
                        g->buf[0] = '\0';
                        g->len = 0;
                        g->cap = 4096;

                        cg_expr(g, s->left);

                        cg_closure_def_append(g, g->buf);
                        free(g->buf);

                        g->buf = saved_buf;
                        g->len = saved_len;
                        g->cap = saved_cap;
                    }
                    cg_closure_def_append(g, ";\n");
                } else if (!s->next && s->kind == AST_RETURN) {
                    /* Explicit return */
                    cg_closure_def_append(g, "    return ");
                    if (s->left) {
                        char *saved_buf = g->buf;
                        size_t saved_len = g->len;
                        size_t saved_cap = g->cap;

                        g->buf = (char *)malloc(4096);
                        g->buf[0] = '\0';
                        g->len = 0;
                        g->cap = 4096;

                        cg_expr(g, s->left);

                        cg_closure_def_append(g, g->buf);
                        free(g->buf);

                        g->buf = saved_buf;
                        g->len = saved_len;
                        g->cap = saved_cap;
                    } else {
                        cg_closure_def_append(g, "0");
                    }
                    cg_closure_def_append(g, ";\n");
                } else {
                    /* Non-last statement — emit normally via buffer swap */
                    char *saved_buf = g->buf;
                    size_t saved_len = g->len;
                    size_t saved_cap = g->cap;

                    g->buf = (char *)malloc(4096);
                    g->buf[0] = '\0';
                    g->len = 0;
                    g->cap = 4096;
                    {
                        int saved_indent = g->indent;
                        g->indent = 1;
                        cg_stmt(g, s);
                        g->indent = saved_indent;
                    }

                    cg_closure_def_append(g, g->buf);
                    free(g->buf);

                    g->buf = saved_buf;
                    g->len = saved_len;
                    g->cap = saved_cap;
                }
                s = s->next;
            }
        } else if (closure_body) {
            /* Single expression — wrap in return */
            cg_closure_def_append(g, "    return ");
            {
                char *saved_buf = g->buf;
                size_t saved_len = g->len;
                size_t saved_cap = g->cap;

                g->buf = (char *)malloc(4096);
                g->buf[0] = '\0';
                g->len = 0;
                g->cap = 4096;

                cg_expr(g, closure_body);

                cg_closure_def_append(g, g->buf);
                free(g->buf);

                g->buf = saved_buf;
                g->len = saved_len;
                g->cap = saved_cap;
            }
            cg_closure_def_append(g, ";\n");
        }
        cg_closure_def_append(g, "}\n\n");

        /* Now emit inline construction code at the use site.
         * This is a GNU C statement expression ({ ... }) that builds the closure. */
        cg_fmt(g, "({ ");
        if (captures.count > 0) {
            cg_fmt(g, "_lcn_closure_%d_env *_cenv_%d = (_lcn_closure_%d_env *)malloc(sizeof(_lcn_closure_%d_env)); ",
                   cid, cid, cid, cid);
            {
                int ci;
                for (ci = 0; ci < captures.count; ci++)
                    cg_fmt(g, "_cenv_%d->%s = %s; ", cid, captures.names[ci], captures.names[ci]);
            }
            cg_fmt(g, "(LcnClosure){ .fn = (void *)_lcn_closure_%d_fn, .env = (void *)_cenv_%d }; })",
                   cid, cid);
        } else {
            cg_fmt(g, "(LcnClosure){ .fn = (void *)_lcn_closure_%d_fn, .env = (void *)0 }; })",
                   cid);
        }
        break;
    }

    case AST_COMPTIME: {
        /* Compile-time evaluation: evaluate the block and emit the result as a constant */
        ComptimeValue val = comptime_evaluate(expr, g->arena);
        if (comptime_check_error(val, expr)) {
            cg_fmt(g, "0 /* comptime error: evaluation failed */");
        } else {
            cg_comptime_value(g, val);
        }
        break;
    }

    default:
        cg_fmt(g, "/* TODO: expr %s */", ast_kind_name(expr->kind));
        break;
    }
}

/* ============================================================
 * Match Statement Codegen
 * ============================================================ */

/* Detect enum type from match patterns: if patterns use "EnumName.Variant",
 * extract the enum name. Returns NULL for LcnLlmOutput patterns (Ok/Error/etc). */
static const char *cg_match_detect_enum(CodeGen *g, AstNode *match_node) {
    AstNode *arm = match_node->params;
    while (arm) {
        if (arm->kind == AST_MATCH_ARM && arm->left) {
            AstNode *pat = arm->left;
            if (pat->kind == AST_PAT_ENUM && pat->name) {
                /* Check for "EnumName.Variant" pattern (dot-qualified) */
                const char *dot = strchr(pat->name, '.');
                if (dot) {
                    /* Extract enum name before the dot */
                    size_t len = (size_t)(dot - pat->name);
                    char buf[256];
                    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
                    memcpy(buf, pat->name, len);
                    buf[len] = '\0';
                    int ei = cg_lookup_enum(g, buf);
                    if (ei >= 0 && g->enums[ei].has_data)
                        return g->enums[ei].name;
                }
            } else if (pat->kind == AST_PAT_IDENT && pat->name) {
                /* Check for bare "EnumName.Variant" (parsed as ident with dot) */
                const char *dot = strchr(pat->name, '.');
                if (dot) {
                    size_t len = (size_t)(dot - pat->name);
                    char buf[256];
                    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
                    memcpy(buf, pat->name, len);
                    buf[len] = '\0';
                    int ei = cg_lookup_enum(g, buf);
                    if (ei >= 0)
                        return g->enums[ei].name;
                }
            }
        }
        arm = arm->next;
    }
    return NULL;
}

/* Find a variant AST node within an ADT enum by variant name */
static AstNode *cg_find_variant(AstNode *en, const char *vname) {
    AstNode *v = en->params;
    while (v) {
        if (v->kind == AST_VARIANT && v->name && strcmp(v->name, vname) == 0)
            return v;
        v = v->next;
    }
    return NULL;
}

/* Detect if match arms use Ok/Error or Some/None patterns (Result/Option match) */
static int cg_match_detect_result_option(CodeGen *g, AstNode *match_node) {
    /* Returns: 1 = Result match (Ok/Error), 2 = Option match (Some/None), 0 = neither.
     *
     * IMPORTANT: Ok/Error patterns are also used for LcnLlmOutput (from ask()).
     * Only return 1 if the subject variable is known to be a Result type.
     * If we can't determine the type, default to 0 (LlmOutput handling). */
    bool has_ok = false, has_error = false;
    bool has_some = false, has_none = false;
    AstNode *arm = match_node->params;
    while (arm) {
        if (arm->kind == AST_MATCH_ARM && arm->left) {
            AstNode *pat = arm->left;
            if (pat->kind == AST_PAT_ENUM && pat->name) {
                if (strcmp(pat->name, "Ok") == 0) has_ok = true;
                else if (strcmp(pat->name, "Error") == 0 || strcmp(pat->name, "Err") == 0) has_error = true;
                else if (strcmp(pat->name, "Some") == 0) has_some = true;
                else if (strcmp(pat->name, "None") == 0) has_none = true;
                /* Check for Text/ToolCall — these are LlmOutput patterns */
                else if (strcmp(pat->name, "Text") == 0 || strcmp(pat->name, "ToolCall") == 0)
                    return 0;  /* Definitely LlmOutput, not Result */
            } else if (pat->kind == AST_PAT_IDENT && pat->name) {
                if (strcmp(pat->name, "None") == 0) has_none = true;
                else if (strcmp(pat->name, "_") == 0) { /* wildcard, ok */ }
            } else if (pat->kind == AST_PAT_WILDCARD) {
                /* ok */
            }
        }
        arm = arm->next;
    }

    if (has_ok || has_error) {
        /* Check if the subject is a known Result type variable */
        if (match_node->left && match_node->left->kind == AST_IDENT &&
            match_node->left->name) {
            const char *vname = match_node->left->name;
            int vi;
            for (vi = 0; vi < g->var_type_count; vi++) {
                if (strcmp(g->var_types[vi].var_name, vname) == 0) {
                    const char *vtype = g->var_types[vi].agent_name;
                    if (strncmp(vtype, "Result_", 7) == 0 ||
                        strcmp(vtype, "LcnResult") == 0 ||
                        strcmp(vtype, "Result") == 0)
                        return 1;  /* Confirmed Result type */
                    /* If tracked as something else (e.g. agent type), not a Result */
                    break;
                }
            }
            /* Also check if the fn_ret_types registry says this is a Result */
            /* (for calls like: let r = some_fn(); match r { ... }) */
        }
        /* Not confirmed as Result — fall back to LlmOutput handling.
         * This preserves backward compatibility with ask() results. */
        return 0;
    }
    if (has_some || has_none) return 2;
    return 0;
}

/* Emit match on a Result type using if/else on .ok */
static void cg_match_result(CodeGen *g, AstNode *match_node, int mid) {
    /* Try to determine the Result type name from the subject's type context.
     * For now, use LcnResult as the fallback temp variable type.
     * If the subject has a known typed Result, we'll use it. */
    const char *result_type = "LcnResult";
    const char *value_type = "LcnString";
    const char *error_type = "LcnString";

    /* Check if subject is an ident with known typed Result from var tracking */
    if (match_node->left && match_node->left->kind == AST_IDENT &&
        match_node->left->name) {
        const char *vname = match_node->left->name;
        int vi;
        for (vi = 0; vi < g->var_type_count; vi++) {
            if (strcmp(g->var_types[vi].var_name, vname) == 0 &&
                strncmp(g->var_types[vi].agent_name, "Result_", 7) == 0) {
                result_type = g->var_types[vi].agent_name;
                /* Extract value and error types from the mono registry */
                int mi;
                for (mi = 0; mi < g->mono_count; mi++) {
                    if (strcmp(g->mono[mi].name, result_type) == 0 && g->mono[mi].generics) {
                        value_type = mono_type_from_expr(g->mono[mi].generics);
                        if (g->mono[mi].generics->next)
                            error_type = mono_type_from_expr(g->mono[mi].generics->next);
                        break;
                    }
                }
                break;
            }
        }
    }

    cg_indent(g);
    cg_str(g, "{\n");
    g->indent++;

    /* Evaluate subject into temp */
    cg_indent(g);
    cg_fmt(g, "%s _match_%d = ", result_type, mid);
    cg_expr(g, match_node->left);
    cg_str(g, ";\n");

    /* Generate if/else chain */
    AstNode *arm = match_node->params;
    bool first = true;
    while (arm) {
        if (arm->kind == AST_MATCH_ARM && arm->left) {
            AstNode *pat = arm->left;
            AstNode *body = arm->right;

            if (pat->kind == AST_PAT_ENUM && pat->name &&
                strcmp(pat->name, "Ok") == 0) {
                cg_indent(g);
                cg_fmt(g, "%sif (_match_%d.ok) {\n", first ? "" : "} else ", mid);
                g->indent++;
                /* Bind value */
                if (pat->params && pat->params->kind == AST_PAT_IDENT && pat->params->name) {
                    cg_indent(g);
                    cg_fmt(g, "%s %s = _match_%d.value;\n",
                           value_type, pat->params->name, mid);
                }
                if (body && body->kind == AST_BLOCK) {
                    AstNode *s = body->params;
                    while (s) { cg_stmt(g, s); s = s->next; }
                } else if (body) {
                    cg_indent(g); cg_expr(g, body); cg_str(g, ";\n");
                }
                g->indent--;
                first = false;
            } else if (pat->kind == AST_PAT_ENUM && pat->name &&
                       (strcmp(pat->name, "Error") == 0 || strcmp(pat->name, "Err") == 0)) {
                cg_indent(g);
                cg_fmt(g, "%sif (!_match_%d.ok) {\n", first ? "" : "} else ", mid);
                g->indent++;
                /* Bind error */
                if (pat->params && pat->params->kind == AST_PAT_IDENT && pat->params->name) {
                    cg_indent(g);
                    cg_fmt(g, "%s %s = _match_%d.error;\n",
                           error_type, pat->params->name, mid);
                }
                if (body && body->kind == AST_BLOCK) {
                    AstNode *s = body->params;
                    while (s) { cg_stmt(g, s); s = s->next; }
                } else if (body) {
                    cg_indent(g); cg_expr(g, body); cg_str(g, ";\n");
                }
                g->indent--;
                first = false;
            } else if ((pat->kind == AST_PAT_WILDCARD) ||
                       (pat->kind == AST_PAT_IDENT && pat->name && strcmp(pat->name, "_") == 0)) {
                cg_indent(g);
                cg_str(g, "} else {\n");
                g->indent++;
                if (body && body->kind == AST_BLOCK) {
                    AstNode *s = body->params;
                    while (s) { cg_stmt(g, s); s = s->next; }
                } else if (body) {
                    cg_indent(g); cg_expr(g, body); cg_str(g, ";\n");
                }
                g->indent--;
                first = false;
            }
        }
        arm = arm->next;
    }
    if (!first) {
        cg_indent(g);
        cg_str(g, "}\n");
    }

    g->indent--;
    cg_indent(g);
    cg_str(g, "}\n");
}

/* Emit match on an Option type using if/else on .has_value */
static void cg_match_option(CodeGen *g, AstNode *match_node, int mid) {
    const char *option_type = "LcnOption";
    const char *value_type = "int64_t";

    /* Check if subject is an ident with known typed Option from var tracking */
    if (match_node->left && match_node->left->kind == AST_IDENT &&
        match_node->left->name) {
        const char *vname = match_node->left->name;
        int vi;
        for (vi = 0; vi < g->var_type_count; vi++) {
            if (strcmp(g->var_types[vi].var_name, vname) == 0 &&
                strncmp(g->var_types[vi].agent_name, "Option_", 7) == 0) {
                option_type = g->var_types[vi].agent_name;
                /* Extract value type from the mono registry */
                int mi;
                for (mi = 0; mi < g->mono_count; mi++) {
                    if (strcmp(g->mono[mi].name, option_type) == 0 && g->mono[mi].generics) {
                        value_type = mono_type_from_expr(g->mono[mi].generics);
                        break;
                    }
                }
                break;
            }
        }
    }

    cg_indent(g);
    cg_str(g, "{\n");
    g->indent++;

    cg_indent(g);
    cg_fmt(g, "%s _match_%d = ", option_type, mid);
    cg_expr(g, match_node->left);
    cg_str(g, ";\n");

    AstNode *arm = match_node->params;
    bool first = true;
    while (arm) {
        if (arm->kind == AST_MATCH_ARM && arm->left) {
            AstNode *pat = arm->left;
            AstNode *body = arm->right;

            if (pat->kind == AST_PAT_ENUM && pat->name &&
                strcmp(pat->name, "Some") == 0) {
                cg_indent(g);
                cg_fmt(g, "%sif (_match_%d.has_value) {\n", first ? "" : "} else ", mid);
                g->indent++;
                if (pat->params && pat->params->kind == AST_PAT_IDENT && pat->params->name) {
                    cg_indent(g);
                    cg_fmt(g, "%s %s = _match_%d.value;\n",
                           value_type, pat->params->name, mid);
                }
                if (body && body->kind == AST_BLOCK) {
                    AstNode *s = body->params;
                    while (s) { cg_stmt(g, s); s = s->next; }
                } else if (body) {
                    cg_indent(g); cg_expr(g, body); cg_str(g, ";\n");
                }
                g->indent--;
                first = false;
            } else if ((pat->kind == AST_PAT_ENUM && pat->name && strcmp(pat->name, "None") == 0) ||
                       (pat->kind == AST_PAT_IDENT && pat->name && strcmp(pat->name, "None") == 0)) {
                cg_indent(g);
                cg_fmt(g, "%sif (!_match_%d.has_value) {\n", first ? "" : "} else ", mid);
                g->indent++;
                if (body && body->kind == AST_BLOCK) {
                    AstNode *s = body->params;
                    while (s) { cg_stmt(g, s); s = s->next; }
                } else if (body) {
                    cg_indent(g); cg_expr(g, body); cg_str(g, ";\n");
                }
                g->indent--;
                first = false;
            } else if ((pat->kind == AST_PAT_WILDCARD) ||
                       (pat->kind == AST_PAT_IDENT && pat->name && strcmp(pat->name, "_") == 0)) {
                cg_indent(g);
                cg_str(g, "} else {\n");
                g->indent++;
                if (body && body->kind == AST_BLOCK) {
                    AstNode *s = body->params;
                    while (s) { cg_stmt(g, s); s = s->next; }
                } else if (body) {
                    cg_indent(g); cg_expr(g, body); cg_str(g, ";\n");
                }
                g->indent--;
                first = false;
            }
        }
        arm = arm->next;
    }
    if (!first) {
        cg_indent(g);
        cg_str(g, "}\n");
    }

    g->indent--;
    cg_indent(g);
    cg_str(g, "}\n");
}

/* Detect if match arms contain literal patterns (int/string/bool values).
 * Returns: 0 = no literals, 1 = int/bool literals, 2 = string literals */
static int cg_match_detect_value_kind(CodeGen *g, AstNode *match_node) {
    (void)g;
    AstNode *subj = match_node->left;
    if (subj) {
        if (subj->kind == AST_INT_LIT || subj->kind == AST_BOOL_LIT)
            return 1;
        if (subj->kind == AST_STRING_LIT)
            return 2;
    }
    bool has_literal = false;
    bool has_string_literal = false;
    AstNode *arm = match_node->params;
    while (arm) {
        if (arm->kind == AST_MATCH_ARM && arm->left) {
            AstNode *pat = arm->left;
            if (pat->kind == AST_PAT_LITERAL) {
                has_literal = true;
                uintptr_t val_as_ptr = (uintptr_t)pat->val.str_val;
                if (val_as_ptr > 4096)
                    has_string_literal = true;
            } else if (pat->kind == AST_PAT_RANGE) {
                has_literal = true;
            }
        }
        arm = arm->next;
    }
    if (!has_literal)
        return 0;
    return has_string_literal ? 2 : 1;
}

/* Emit match on int/bool values using if/else chains */
static void cg_match_int(CodeGen *g, AstNode *match_node, int mid) {
    cg_indent(g);
    cg_str(g, "{\n");
    g->indent++;
    cg_indent(g);
    cg_fmt(g, "int64_t _match_%d = ", mid);
    cg_expr(g, match_node->left);
    cg_str(g, ";\n");
    AstNode *arm = match_node->params;
    bool first = true;
    while (arm) {
        if (arm->kind == AST_MATCH_ARM && arm->left) {
            AstNode *pat = arm->left;
            AstNode *body = arm->right;
            if (pat->kind == AST_PAT_LITERAL) {
                cg_indent(g);
                cg_fmt(g, "%sif (_match_%d == %lld) {\n",
                       first ? "" : "} else ", mid,
                       (long long)pat->val.int_val);
                g->indent++;
                if (body && body->kind == AST_BLOCK) {
                    AstNode *s = body->params;
                    while (s) { cg_stmt(g, s); s = s->next; }
                } else if (body) {
                    cg_indent(g); cg_expr(g, body); cg_str(g, ";\n");
                }
                g->indent--;
                first = false;
            } else if (pat->kind == AST_PAT_RANGE) {
                cg_indent(g);
                cg_fmt(g, "%sif (_match_%d >= ", first ? "" : "} else ", mid);
                if (pat->left && pat->left->kind == AST_PAT_LITERAL)
                    cg_fmt(g, "%lld", (long long)pat->left->val.int_val);
                else
                    cg_str(g, "0");
                cg_fmt(g, " && _match_%d %s ", mid, pat->is_mut ? "<=" : "<");
                if (pat->right && pat->right->kind == AST_PAT_LITERAL)
                    cg_fmt(g, "%lld", (long long)pat->right->val.int_val);
                else
                    cg_str(g, "0");
                cg_str(g, ") {\n");
                g->indent++;
                if (body && body->kind == AST_BLOCK) {
                    AstNode *s = body->params;
                    while (s) { cg_stmt(g, s); s = s->next; }
                } else if (body) {
                    cg_indent(g); cg_expr(g, body); cg_str(g, ";\n");
                }
                g->indent--;
                first = false;
            } else if ((pat->kind == AST_PAT_WILDCARD) ||
                       (pat->kind == AST_PAT_IDENT && pat->name &&
                        strcmp(pat->name, "_") == 0)) {
                cg_indent(g);
                cg_fmt(g, "%s{\n", first ? "" : "} else ");
                g->indent++;
                if (body && body->kind == AST_BLOCK) {
                    AstNode *s = body->params;
                    while (s) { cg_stmt(g, s); s = s->next; }
                } else if (body) {
                    cg_indent(g); cg_expr(g, body); cg_str(g, ";\n");
                }
                g->indent--;
                first = false;
            } else if (pat->kind == AST_PAT_IDENT && pat->name &&
                       strcmp(pat->name, "_") != 0) {
                cg_indent(g);
                cg_fmt(g, "%s{\n", first ? "" : "} else ");
                g->indent++;
                cg_indent(g);
                cg_fmt(g, "int64_t %s = _match_%d;\n", pat->name, mid);
                if (body && body->kind == AST_BLOCK) {
                    AstNode *s = body->params;
                    while (s) { cg_stmt(g, s); s = s->next; }
                } else if (body) {
                    cg_indent(g); cg_expr(g, body); cg_str(g, ";\n");
                }
                g->indent--;
                first = false;
            }
        }
        arm = arm->next;
    }
    if (!first) { cg_indent(g); cg_str(g, "}\n"); }
    g->indent--;
    cg_indent(g);
    cg_str(g, "}\n");
}

/* Emit match on string values using strcmp if/else chains */
static void cg_match_string(CodeGen *g, AstNode *match_node, int mid) {
    cg_indent(g);
    cg_str(g, "{\n");
    g->indent++;
    cg_indent(g);
    cg_fmt(g, "LcnString _match_%d = ", mid);
    cg_expr(g, match_node->left);
    cg_str(g, ";\n");
    AstNode *arm = match_node->params;
    bool first = true;
    while (arm) {
        if (arm->kind == AST_MATCH_ARM && arm->left) {
            AstNode *pat = arm->left;
            AstNode *body = arm->right;
            if (pat->kind == AST_PAT_LITERAL && pat->val.str_val) {
                cg_indent(g);
                cg_fmt(g, "%sif (strcmp(_match_%d, ", first ? "" : "} else ", mid);
                cg_string_literal(g, pat->val.str_val);
                cg_str(g, ") == 0) {\n");
                g->indent++;
                if (body && body->kind == AST_BLOCK) {
                    AstNode *s = body->params;
                    while (s) { cg_stmt(g, s); s = s->next; }
                } else if (body) {
                    cg_indent(g); cg_expr(g, body); cg_str(g, ";\n");
                }
                g->indent--;
                first = false;
            } else if ((pat->kind == AST_PAT_WILDCARD) ||
                       (pat->kind == AST_PAT_IDENT && pat->name &&
                        strcmp(pat->name, "_") == 0)) {
                cg_indent(g);
                cg_fmt(g, "%s{\n", first ? "" : "} else ");
                g->indent++;
                if (body && body->kind == AST_BLOCK) {
                    AstNode *s = body->params;
                    while (s) { cg_stmt(g, s); s = s->next; }
                } else if (body) {
                    cg_indent(g); cg_expr(g, body); cg_str(g, ";\n");
                }
                g->indent--;
                first = false;
            } else if (pat->kind == AST_PAT_IDENT && pat->name &&
                       strcmp(pat->name, "_") != 0) {
                cg_indent(g);
                cg_fmt(g, "%s{\n", first ? "" : "} else ");
                g->indent++;
                cg_indent(g);
                cg_fmt(g, "LcnString %s = _match_%d;\n", pat->name, mid);
                if (body && body->kind == AST_BLOCK) {
                    AstNode *s = body->params;
                    while (s) { cg_stmt(g, s); s = s->next; }
                } else if (body) {
                    cg_indent(g); cg_expr(g, body); cg_str(g, ";\n");
                }
                g->indent--;
                first = false;
            }
        }
        arm = arm->next;
    }
    if (!first) { cg_indent(g); cg_str(g, "}\n"); }
    g->indent--;
    cg_indent(g);
    cg_str(g, "}\n");
}

static void cg_match(CodeGen *g, AstNode *match_node) {
    static int match_counter = 0;
    int mid = match_counter++;

    /* Check for Result/Option pattern matching before ADT/LlmOutput */
    int ro_kind = cg_match_detect_result_option(g, match_node);
    if (ro_kind == 1) {
        cg_match_result(g, match_node, mid);
        return;
    }
    if (ro_kind == 2) {
        cg_match_option(g, match_node, mid);
        return;
    }

    /* Check for value-type match (int/string/bool literal patterns) */
    int val_kind = cg_match_detect_value_kind(g, match_node);
    if (val_kind == 1) { cg_match_int(g, match_node, mid); return; }
    if (val_kind == 2) { cg_match_string(g, match_node, mid); return; }

    /* Detect if matching on a user-defined ADT enum */
    const char *adt_enum_name = cg_match_detect_enum(g, match_node);

    cg_indent(g);
    cg_str(g, "{\n");
    g->indent++;

    /* Evaluate subject into temp variable */
    cg_indent(g);
    if (adt_enum_name) {
        cg_fmt(g, "%s _match_%d = ", adt_enum_name, mid);
    } else {
        cg_fmt(g, "LcnLlmOutput _match_%d = ", mid);
    }
    cg_expr(g, match_node->left);
    cg_str(g, ";\n");

    /* Generate switch */
    cg_indent(g);
    if (adt_enum_name)
        cg_fmt(g, "switch (_match_%d._kind) {\n", mid);
    else
        cg_fmt(g, "switch (_match_%d.kind) {\n", mid);
    g->indent++;

    AstNode *arm = match_node->params;
    while (arm) {
        if (arm->kind == AST_MATCH_ARM) {
            AstNode *pat = arm->left;
            AstNode *body = arm->right;

            if (pat && pat->kind == AST_PAT_ENUM && pat->name) {
                const char *variant = pat->name;

                if (adt_enum_name) {
                    /* User-defined ADT: extract variant name after the dot */
                    const char *dot = strchr(variant, '.');
                    const char *vname = dot ? dot + 1 : variant;

                    cg_line(g, "case %s_%s: {", adt_enum_name, vname);
                    g->indent++;

                    /* Bind destructured fields by matching pattern params to variant fields */
                    int ei = cg_lookup_enum(g, adt_enum_name);
                    AstNode *vnode = (ei >= 0) ? cg_find_variant(g->enums[ei].node, vname) : NULL;
                    AstNode *pat_field = pat->params;
                    AstNode *var_field = vnode ? vnode->params : NULL;
                    {
                        int ffi = 0;
                        while (pat_field && var_field) {
                            if (pat_field->kind == AST_PAT_IDENT && pat_field->name &&
                                var_field->kind == AST_FIELD) {
                                const char *ctype = cg_type_to_c(var_field->type_expr);
                                cg_indent(g);
                                if (var_field->name) {
                                    cg_fmt(g, "%s %s = _match_%d._%s_%s;\n",
                                           ctype, pat_field->name, mid, vname, var_field->name);
                                } else {
                                    cg_fmt(g, "%s %s = _match_%d._%s_%d;\n",
                                           ctype, pat_field->name, mid, vname, ffi);
                                }
                                ffi++;
                            }
                            pat_field = pat_field->next;
                            var_field = var_field->next;
                        }
                    }
                } else {
                    /* LcnLlmOutput enum pattern (existing behavior) */
                    if (strcmp(variant, "Ok") == 0) {
                        cg_line(g, "case LCN_LLM_OUTPUT_OK: {");
                    } else if (strcmp(variant, "Text") == 0) {
                        cg_line(g, "case LCN_LLM_OUTPUT_TEXT: {");
                    } else if (strcmp(variant, "ToolCall") == 0) {
                        cg_line(g, "case LCN_LLM_OUTPUT_TOOL_CALL: {");
                    } else if (strcmp(variant, "Error") == 0) {
                        cg_line(g, "case LCN_LLM_OUTPUT_ERROR: {");
                    } else {
                        cg_fmt(g, "/* unknown variant: %s */\n", variant);
                        arm = arm->next;
                        continue;
                    }

                    g->indent++;

                    /* Bind destructured fields */
                    AstNode *field = pat->params;
                    if (field) {
                        if (strcmp(variant, "Ok") == 0 || strcmp(variant, "Text") == 0) {
                            if (field->kind == AST_PAT_IDENT && field->name) {
                                cg_indent(g);
                                cg_fmt(g, "LcnString %s = _match_%d.content;\n", field->name, mid);
                            }
                        } else if (strcmp(variant, "ToolCall") == 0) {
                            if (field->kind == AST_PAT_IDENT && field->name) {
                                cg_indent(g);
                                cg_fmt(g, "LcnString %s = _match_%d.tool_name;\n", field->name, mid);
                            }
                            AstNode *field2 = field->next;
                            if (field2 && field2->kind == AST_PAT_IDENT && field2->name) {
                                cg_indent(g);
                                cg_fmt(g, "LcnString %s = _match_%d.tool_args;\n", field2->name, mid);
                            }
                            /* CAPABILITY FENCE: validate tool call against agent's allowed tools */
                            if (g->in_agent_method && g->current_agent_name) {
                                cg_indent(g);
                                cg_fmt(g, "if (!lcn_capability_check_tool(_match_%d.tool_name, "
                                       "(const char **)_agent_%s_tools, _agent_%s_tool_count)) {\n",
                                       mid, g->current_agent_name, g->current_agent_name);
                                g->indent++;
                                cg_indent(g);
                                cg_fmt(g, "lcn_capability_violation(\"%s\", _match_%d.tool_name, "
                                       "\"tool not in agent capabilities\");\n",
                                       g->current_agent_name, mid);
                                cg_line(g, "break;");
                                g->indent--;
                                cg_line(g, "}");
                            }
                        } else if (strcmp(variant, "Error") == 0) {
                            if (field->kind == AST_PAT_IDENT && field->name) {
                                cg_indent(g);
                                cg_fmt(g, "LcnString %s = _match_%d.error;\n", field->name, mid);
                            }
                        }
                    }
                }

                /* Emit body */
                if (body && body->kind == AST_BLOCK) {
                    AstNode *s = body->params;
                    while (s) { cg_stmt(g, s); s = s->next; }
                } else if (body) {
                    cg_indent(g);
                    cg_expr(g, body);
                    cg_str(g, ";\n");
                }

                cg_line(g, "break;");
                g->indent--;
                cg_line(g, "}");

            } else if (pat && pat->kind == AST_PAT_IDENT && pat->name) {
                /* Check if it's a qualified enum variant without data: Token.Eof */
                const char *dot = strchr(pat->name, '.');
                if (dot && adt_enum_name) {
                    const char *vname = dot + 1;
                    cg_line(g, "case %s_%s: {", adt_enum_name, vname);
                    g->indent++;

                    if (body && body->kind == AST_BLOCK) {
                        AstNode *s = body->params;
                        while (s) { cg_stmt(g, s); s = s->next; }
                    } else if (body) {
                        cg_indent(g);
                        cg_expr(g, body);
                        cg_str(g, ";\n");
                    }

                    cg_line(g, "break;");
                    g->indent--;
                    cg_line(g, "}");
                } else if (strcmp(pat->name, "_") == 0) {
                    /* Wildcard */
                    cg_line(g, "default: {");
                    g->indent++;

                    if (body && body->kind == AST_BLOCK) {
                        AstNode *s = body->params;
                        while (s) { cg_stmt(g, s); s = s->next; }
                    } else if (body) {
                        cg_indent(g);
                        cg_expr(g, body);
                        cg_str(g, ";\n");
                    }

                    cg_line(g, "break;");
                    g->indent--;
                    cg_line(g, "}");
                } else {
                    /* Named catch-all: `other =>` binds the whole value */
                    cg_line(g, "default: {");
                    g->indent++;
                    cg_indent(g);
                    if (adt_enum_name)
                        cg_fmt(g, "%s %s = _match_%d;\n", adt_enum_name, pat->name, mid);
                    else
                        cg_fmt(g, "LcnLlmOutput %s = _match_%d;\n", pat->name, mid);

                    if (body && body->kind == AST_BLOCK) {
                        AstNode *s = body->params;
                        while (s) { cg_stmt(g, s); s = s->next; }
                    } else if (body) {
                        cg_indent(g);
                        cg_expr(g, body);
                        cg_str(g, ";\n");
                    }

                    cg_line(g, "break;");
                    g->indent--;
                    cg_line(g, "}");
                }
            } else if (pat && pat->kind == AST_PAT_STRUCT) {
                /* Struct destructuring pattern: Point { x, y } or Point { x: a, y: b } */
                cg_line(g, "default: {");
                g->indent++;

                AstNode *field = pat->params;
                while (field) {
                    if (field->kind == AST_PAT_IDENT && field->name) {
                        /* Simple binding: Point { x } → int64_t x = _match_N.x; */
                        cg_indent(g);
                        cg_fmt(g, "int64_t %s = _match_%d.%s;\n",
                               field->name, mid, field->name);
                    } else if (field->kind == AST_PAT_TYPED && field->name && field->type_expr) {
                        /* Renamed binding: Point { x: a } → int64_t a = _match_N.x; */
                        const char *bind_name = field->type_expr->name ? field->type_expr->name : field->name;
                        cg_indent(g);
                        cg_fmt(g, "int64_t %s = _match_%d.%s;\n",
                               bind_name, mid, field->name);
                    }
                    field = field->next;
                }

                /* Emit body */
                if (body && body->kind == AST_BLOCK) {
                    AstNode *s = body->params;
                    while (s) { cg_stmt(g, s); s = s->next; }
                } else if (body) {
                    cg_indent(g);
                    cg_expr(g, body);
                    cg_str(g, ";\n");
                }

                cg_line(g, "break;");
                g->indent--;
                cg_line(g, "}");

            } else if (pat && pat->kind == AST_PAT_TUPLE) {
                /* Tuple destructuring pattern: (a, b) → int64_t a = _match_N._0; ... */
                cg_line(g, "default: {");
                g->indent++;

                AstNode *elem = pat->params;
                int ti = 0;
                while (elem) {
                    if (elem->kind == AST_PAT_IDENT && elem->name) {
                        cg_indent(g);
                        cg_fmt(g, "int64_t %s = _match_%d._%d;\n",
                               elem->name, mid, ti);
                    }
                    elem = elem->next;
                    ti++;
                }

                /* Emit body */
                if (body && body->kind == AST_BLOCK) {
                    AstNode *s = body->params;
                    while (s) { cg_stmt(g, s); s = s->next; }
                } else if (body) {
                    cg_indent(g);
                    cg_expr(g, body);
                    cg_str(g, ";\n");
                }

                cg_line(g, "break;");
                g->indent--;
                cg_line(g, "}");

            } else if (pat && pat->kind == AST_PAT_WILDCARD) {
                /* Wildcard pattern → default case */
                cg_line(g, "default: {");
                g->indent++;

                if (body && body->kind == AST_BLOCK) {
                    AstNode *s = body->params;
                    while (s) { cg_stmt(g, s); s = s->next; }
                } else if (body) {
                    cg_indent(g);
                    cg_expr(g, body);
                    cg_str(g, ";\n");
                }

                cg_line(g, "break;");
                g->indent--;
                cg_line(g, "}");
            }
        }
        arm = arm->next;
    }

    g->indent--;
    cg_indent(g);
    cg_str(g, "}\n");

    g->indent--;
    cg_indent(g);
    cg_str(g, "}\n");
}

/* ============================================================
 * Statement Generation
 * ============================================================ */

static void cg_stmt(CodeGen *g, AstNode *stmt) {
    if (!stmt) return;

    switch (stmt->kind) {
    case AST_LET: {
        /* Task group binding: let results = task_group { spawn ...; spawn ...; }
         * Emits inline block: create group, spawn tasks, await_all, assign results. */
        if (stmt->right && stmt->right->kind == AST_TASK_GROUP) {
            int tg_id = g->spawn_counter++;
            char tg_var[64];
            snprintf(tg_var, sizeof(tg_var), "__lcn_tg_%d", tg_id);

            cg_indent(g);
            cg_fmt(g, "LcnTaskGroup *%s = lcn_task_group_new();\n", tg_var);

            /* Walk children; rewrite spawns */
            if (stmt->right->left && stmt->right->left->kind == AST_BLOCK) {
                AstNode *s = stmt->right->left->params;
                while (s) {
                    if ((s->kind == AST_EXPR_STMT && s->left && s->left->kind == AST_SPAWN) ||
                        s->kind == AST_SPAWN) {
                        AstNode *spawn_node = (s->kind == AST_SPAWN) ? s : s->left;

                        int sid = g->spawn_counter++;
                        char spawn_fn[64];
                        snprintf(spawn_fn, sizeof(spawn_fn), "__lcn_spawn_%d", sid);

                        char hdr[128];
                        snprintf(hdr, sizeof(hdr),
                                 "static void *%s(void *__lcn_spawn_arg) {\n"
                                 "    (void)__lcn_spawn_arg;\n", spawn_fn);
                        cg_closure_def_append(g, hdr);

                        if (spawn_node->left) {
                            char   *saved_buf = g->buf;
                            size_t  saved_len = g->len;
                            size_t  saved_cap = g->cap;
                            g->cap = 4096;
                            g->buf = (char *)malloc(g->cap);
                            g->buf[0] = '\0';
                            g->len = 0;
                            int saved_indent = g->indent;
                            g->indent = 1;
                            if (spawn_node->left->kind == AST_BLOCK) {
                                AstNode *bs = spawn_node->left->params;
                                while (bs) { cg_stmt(g, bs); bs = bs->next; }
                            } else {
                                cg_indent(g);
                                cg_expr(g, spawn_node->left);
                                cg_str(g, ";\n");
                            }
                            g->indent = saved_indent;
                            cg_closure_def_append(g, g->buf);
                            free(g->buf);
                            g->buf = saved_buf;
                            g->len = saved_len;
                            g->cap = saved_cap;
                        }

                        cg_closure_def_append(g, "    return NULL;\n}\n\n");

                        cg_indent(g);
                        cg_fmt(g, "lcn_task_group_spawn(%s, (LcnTaskFn)%s, NULL);\n",
                               tg_var, spawn_fn);
                    } else {
                        cg_stmt(g, s);
                    }
                    s = s->next;
                }
            }

            /* Await all and assign to the binding variable */
            cg_indent(g);
            cg_fmt(g, "void **%s = lcn_task_group_await_all(%s);\n",
                   stmt->name ? stmt->name : "_", tg_var);
            cg_indent(g);
            cg_fmt(g, "lcn_task_group_free(%s);\n", tg_var);
            break;
        }

        /* Try operator: let x = expr? → error check + early return */
        if (stmt->right && stmt->right->kind == AST_TRY && stmt->right->left) {
            int tc = g->indent; /* use indent as simple counter — unique enough */
            static int try_counter = 0;
            int tid = try_counter++;

            /* Determine the Result type for the temp variable.
             * If the let has a type annotation that's Result<T,E>, use that.
             * Otherwise fall back to LcnResult. */
            bool has_typed_result = false;
            char try_type[128];
            AstNode *try_expr_type = NULL;

            /* Check if the inner call's function has a known Result<T,E> return type */
            if (stmt->right->left->kind == AST_CALL &&
                stmt->right->left->left &&
                stmt->right->left->left->kind == AST_IDENT &&
                stmt->right->left->left->name) {
                /* Look up function return type in fn_ret_types registry */
                const char *fn_name = stmt->right->left->left->name;
                int fi;
                for (fi = 0; fi < g->fn_ret_type_count; fi++) {
                    if (strcmp(g->fn_ret_types[fi].fn_name, fn_name) == 0 &&
                        strncmp(g->fn_ret_types[fi].ret_type, "Result_", 7) == 0) {
                        strncpy(try_type, g->fn_ret_types[fi].ret_type, sizeof(try_type) - 1);
                        try_type[sizeof(try_type) - 1] = '\0';
                        has_typed_result = true;
                        break;
                    }
                }
            }

            cg_indent(g);
            if (has_typed_result)
                cg_fmt(g, "%s _try_%d = ", try_type, tid);
            else
                cg_fmt(g, "LcnResult _try_%d = ", tid);
            cg_expr(g, stmt->right->left);
            cg_str(g, ";\n");
            cg_indent(g);
            cg_fmt(g, "if (!_try_%d.ok) return _try_%d;\n", tid, tid);
            cg_indent(g);
            if (stmt->type_expr) cg_type(g, stmt->type_expr);
            else if (has_typed_result) cg_str(g, try_type);
            else cg_str(g, "LcnResult");
            cg_fmt(g, " %s = _try_%d;\n", stmt->name ? stmt->name : "_", tid);
            (void)tc;
            (void)try_expr_type;
            break;
        }

        /* Check if RHS is an agent constructor */
        const char *agent_type = NULL;
        if (stmt->right && stmt->right->kind == AST_IDENT && stmt->right->name)
            agent_type = cg_lookup_agent(g, stmt->right->name);

        cg_indent(g);
        if (agent_type) {
            /* let x = AgentName → Agent_X x = lcn_agent_X_new() */
            cg_fmt(g, "Agent_%s", agent_type);
            if (stmt->name) cg_track_var(g, stmt->name, agent_type);
        } else if (stmt->type_expr) {
            cg_type(g, stmt->type_expr);
            /* Track struct types from type annotations for method dispatch */
            if (stmt->name && stmt->type_expr->kind == AST_TYPE_NAMED &&
                stmt->type_expr->name)
                cg_track_var(g, stmt->name, stmt->type_expr->name);
        } else if (stmt->right && stmt->right->kind == AST_CLOSURE) {
            /* Closure: emit LcnClosure type and register as closure var */
            cg_str(g, "LcnClosure");
            if (stmt->name) cg_register_closure_var(g, stmt->name);
        } else if (stmt->right) {
            cg_infer_type_emit(g, stmt->right);
            /* Track struct types from struct literals: let x = Name { ... } */
            if (stmt->name && stmt->right->kind == AST_MAP &&
                stmt->right->name)
                cg_track_var(g, stmt->name, stmt->right->name);
            /* Track ADT enum types from method calls: let x = Token.Ident("hello") */
            if (stmt->name && stmt->right->kind == AST_METHOD_CALL &&
                stmt->right->left && stmt->right->left->kind == AST_IDENT &&
                stmt->right->left->name) {
                int ei = cg_lookup_enum(g, stmt->right->left->name);
                if (ei >= 0 && g->enums[ei].has_data)
                    cg_track_var(g, stmt->name, g->enums[ei].name);
            }
            /* Track ADT enum types from field access: let x = Token.Eof */
            if (stmt->name && stmt->right->kind == AST_FIELD_ACCESS &&
                stmt->right->left && stmt->right->left->kind == AST_IDENT &&
                stmt->right->left->name) {
                int ei = cg_lookup_enum(g, stmt->right->left->name);
                if (ei >= 0 && g->enums[ei].has_data)
                    cg_track_var(g, stmt->name, g->enums[ei].name);
            }
            /* Track types from function calls with known return type */
            if (stmt->name && stmt->right->kind == AST_CALL &&
                stmt->right->left && stmt->right->left->kind == AST_IDENT &&
                stmt->right->left->name) {
                const char *fn_ret = cg_lookup_fn_ret(g, stmt->right->left->name);
                if (fn_ret && (cg_is_struct(g, fn_ret) || cg_lookup_enum(g, fn_ret) >= 0 ||
                               strncmp(fn_ret, "Result_", 7) == 0 ||
                               strncmp(fn_ret, "Option_", 7) == 0 ||
                               strcmp(fn_ret, "LcnResult") == 0))
                    cg_track_var(g, stmt->name, fn_ret);
            }
        } else {
            cg_str(g, "int64_t");
        }
        cg_fmt(g, " %s", stmt->name ? stmt->name : "_");
        if (stmt->right) {
            cg_str(g, " = ");
            cg_expr(g, stmt->right);
        }
        cg_str(g, ";\n");

        /* BUG-2 fix: track string variables for concat detection */
        if (stmt->name && g->string_var_count < 256) {
            bool is_str = false;
            /* Case 1: explicit type annotation "let x: string = ..." */
            if (stmt->type_expr && stmt->type_expr->kind == AST_TYPE_NAMED &&
                stmt->type_expr->name && strcmp(stmt->type_expr->name, "string") == 0)
                is_str = true;
            /* Case 2: infer from RHS expression */
            if (!is_str && stmt->right && might_be_string_expr(g, stmt->right))
                is_str = true;
            if (is_str)
                g->string_vars[g->string_var_count++] = stmt->name;
        }
        break;
    }

    case AST_RETURN:
        /* Emit deferred expressions in LIFO order before returning.
         * We emit but do NOT clear the stack — enclosing scope will handle cleanup.
         * For return, we emit all defers from 0 (function scope) to current depth. */
        {
            int di;
            for (di = g->defer_depth - 1; di >= 0; di--) {
                cg_indent(g);
                cg_str(g, "/* defer */ ");
                cg_expr(g, g->defer_stack[di]);
                cg_str(g, ";\n");
            }
        }
        cg_indent(g);
        cg_str(g, "return");
        if (stmt->left) {
            cg_str(g, " ");
            cg_expr(g, stmt->left);
        }
        cg_str(g, ";\n");
        break;

    case AST_IF:
        cg_indent(g);
        cg_str(g, "if (");
        cg_expr(g, stmt->left);
        cg_str(g, ") ");
        cg_block(g, stmt->right);
        if (stmt->params) {
            if (stmt->params->kind == AST_IF) {
                cg_str(g, " else ");
                cg_stmt(g, stmt->params);
            } else {
                cg_str(g, " else ");
                cg_block(g, stmt->params);
            }
        }
        cg_nl(g);
        break;

    case AST_WHILE:
        cg_indent(g);
        cg_str(g, "while (");
        cg_expr(g, stmt->left);
        cg_str(g, ") ");
        cg_block(g, stmt->right);
        cg_nl(g);
        break;

    case AST_LOOP:
        cg_indent(g);
        cg_str(g, "while (1) ");
        cg_block(g, stmt->left);
        cg_nl(g);
        break;

    case AST_FOR: {
        const char *var_name = "_";
        if (stmt->left) {
            if (stmt->left->kind == AST_PAT_IDENT && stmt->left->name)
                var_name = stmt->left->name;
            else if (stmt->left->kind == AST_IDENT && stmt->left->name)
                var_name = stmt->left->name;
            else if (stmt->left->kind == AST_TUPLE && stmt->left->params &&
                     stmt->left->params->name)
                var_name = stmt->left->params->name;
        }
        cg_indent(g);
        /* Range iteration: for i in 0..N or 0..=N */
        if (stmt->params && stmt->params->kind == AST_RANGE) {
            cg_str(g, "{ int64_t ");
            cg_str(g, var_name);
            cg_str(g, "; for (");
            cg_str(g, var_name);
            cg_str(g, " = ");
            if (stmt->params->left) cg_expr(g, stmt->params->left);
            else cg_str(g, "0");
            cg_str(g, "; ");
            cg_str(g, var_name);
            cg_str(g, stmt->params->is_mut ? " <= " : " < ");
            if (stmt->params->right) cg_expr(g, stmt->params->right);
            else cg_str(g, "0");
            cg_str(g, "; ");
            cg_str(g, var_name);
            cg_str(g, "++) ");
            if (stmt->right) cg_block(g, stmt->right);
            else cg_str(g, "{ }");
            cg_str(g, " }\n");
        } else {
            /* General: assign expr to var, execute body once */
            cg_fmt(g, "{ /* for %s in ... */\n", var_name);
            g->indent++;
            if (stmt->params) {
                cg_indent(g);
                cg_str(g, "LcnString ");
                cg_str(g, var_name);
                cg_str(g, " = (LcnString)");
                cg_expr(g, stmt->params);
                cg_str(g, ";\n");
                cg_indent(g);
                cg_fmt(g, "(void)%s;\n", var_name);
            }
            if (stmt->right && stmt->right->params) {
                AstNode *s = stmt->right->params;
                while (s) { cg_stmt(g, s); s = s->next; }
            }
            g->indent--;
            cg_line(g, "}");
        }
        break;
    }

    case AST_BREAK:
        cg_line(g, "break;");
        break;

    case AST_CONTINUE:
        cg_line(g, "continue;");
        break;

    case AST_DEFER:
        /* Push to defer stack — will be emitted at scope exit in LIFO order */
        if (stmt->left) cg_defer_push(g, stmt->left);
        break;

    case AST_ASSIGN:
        /* Metrics assignment: metrics.field += N -> atomic add, metrics.field = N -> direct set */
        if (g->has_metrics && stmt->left && stmt->left->kind == AST_FIELD_ACCESS &&
            stmt->left->left && stmt->left->left->kind == AST_IDENT &&
            stmt->left->left->name && strcmp(stmt->left->left->name, "metrics") == 0 &&
            stmt->left->name) {
            const char *field = stmt->left->name;
            cg_indent(g);
            if (stmt->val.op == TOK_PLUS_EQ) {
                cg_fmt(g, "__sync_fetch_and_add(&_lcn_metrics.%s, ", field);
                cg_expr(g, stmt->right);
                cg_str(g, ");\n");
            } else if (stmt->val.op == TOK_MINUS_EQ) {
                cg_fmt(g, "__sync_fetch_and_sub(&_lcn_metrics.%s, ", field);
                cg_expr(g, stmt->right);
                cg_str(g, ");\n");
            } else {
                cg_fmt(g, "_lcn_metrics.%s = ", field);
                cg_expr(g, stmt->right);
                cg_str(g, ";\n");
            }
            break;
        }
        cg_indent(g);
        cg_expr(g, stmt->left);
        cg_fmt(g, " %s ", cg_assign_op(stmt->val.op));
        cg_expr(g, stmt->right);
        cg_str(g, ";\n");
        /* Progress reporting: emit update call after assignment to the current var */
        if (g->has_progress && g->progress_current_var &&
            stmt->left && stmt->left->kind == AST_IDENT && stmt->left->name &&
            strcmp(stmt->left->name, g->progress_current_var) == 0) {
            cg_indent(g);
            cg_fmt(g, "_lcn_progress_update(%s, %s);\n",
                   g->progress_current_var,
                   g->progress_total_var ? g->progress_total_var : "0");
        }
        break;

    case AST_EXPR_STMT:
        /* If wrapping an AST_IF or AST_MATCH, delegate to statement handler */
        if (stmt->left && stmt->left->kind == AST_IF) {
            cg_stmt(g, stmt->left);
            break;
        }
        if (stmt->left && stmt->left->kind == AST_MATCH) {
            cg_stmt(g, stmt->left);
            break;
        }
        cg_indent(g);
        cg_expr(g, stmt->left);
        cg_str(g, ";\n");
        break;

    case AST_BLOCK:
        cg_block(g, stmt);
        cg_nl(g);
        break;

    case AST_SPAWN: {
        /* Real async: emit a static wrapper function and spawn via thread pool.
         * The wrapper is emitted into the closure_defs_buf so it appears
         * before the current function in the output. */
        int sid = g->spawn_counter++;
        char spawn_fn[64];
        snprintf(spawn_fn, sizeof(spawn_fn), "__lcn_spawn_%d", sid);

        /* Build the wrapper function into closure_defs_buf */
        {
            char hdr[128];
            snprintf(hdr, sizeof(hdr),
                     "static void *%s(void *__lcn_spawn_arg) {\n"
                     "    (void)__lcn_spawn_arg;\n", spawn_fn);
            cg_closure_def_append(g, hdr);

            /* Emit body statements into closure_defs_buf via a temporary buffer swap */
            if (stmt->left) {
                /* Save current output buffer */
                char   *saved_buf = g->buf;
                size_t  saved_len = g->len;
                size_t  saved_cap = g->cap;

                /* Use a temporary buffer for the body */
                g->cap = 4096;
                g->buf = (char *)malloc(g->cap);
                g->buf[0] = '\0';
                g->len = 0;

                int saved_indent = g->indent;
                g->indent = 1;

                if (stmt->left->kind == AST_BLOCK) {
                    AstNode *s = stmt->left->params;
                    while (s) { cg_stmt(g, s); s = s->next; }
                } else {
                    cg_indent(g);
                    cg_expr(g, stmt->left);
                    cg_str(g, ";\n");
                }

                g->indent = saved_indent;

                /* Append the body to closure_defs_buf */
                cg_closure_def_append(g, g->buf);

                /* Restore original buffer */
                free(g->buf);
                g->buf = saved_buf;
                g->len = saved_len;
                g->cap = saved_cap;
            }

            cg_closure_def_append(g, "    return NULL;\n}\n\n");
        }

        /* At spawn site, call lcn_spawn_task or lcn_green_spawn */
        cg_indent(g);
        if (g->use_green_threads) {
            cg_fmt(g, "/* spawn (green) */ lcn_green_spawn(%s, NULL);\n", spawn_fn);
        } else {
            cg_fmt(g, "/* spawn */ lcn_spawn_task((LcnTaskFn)%s, NULL);\n", spawn_fn);
        }
        break;
    }

    case AST_TASK_GROUP: {
        /* Structured concurrency (statement context):
         *   task_group { spawn expr1; spawn expr2; ... }
         * All spawn children become task_group_spawn calls;
         * scope is guaranteed by await_all + free. */
        int tg_id = g->spawn_counter++;
        char tg_var[64];
        snprintf(tg_var, sizeof(tg_var), "__lcn_tg_%d", tg_id);

        cg_indent(g);
        cg_fmt(g, "{ /* task_group */\n");
        g->indent++;

        cg_indent(g);
        cg_fmt(g, "LcnTaskGroup *%s = lcn_task_group_new();\n", tg_var);

        if (stmt->left && stmt->left->kind == AST_BLOCK) {
            AstNode *s = stmt->left->params;
            while (s) {
                if ((s->kind == AST_EXPR_STMT && s->left && s->left->kind == AST_SPAWN) ||
                    s->kind == AST_SPAWN) {
                    AstNode *spawn_node = (s->kind == AST_SPAWN) ? s : s->left;

                    int sid = g->spawn_counter++;
                    char spawn_fn[64];
                    snprintf(spawn_fn, sizeof(spawn_fn), "__lcn_spawn_%d", sid);

                    char hdr[128];
                    snprintf(hdr, sizeof(hdr),
                             "static void *%s(void *__lcn_spawn_arg) {\n"
                             "    (void)__lcn_spawn_arg;\n", spawn_fn);
                    cg_closure_def_append(g, hdr);

                    if (spawn_node->left) {
                        char   *saved_buf = g->buf;
                        size_t  saved_len = g->len;
                        size_t  saved_cap = g->cap;
                        g->cap = 4096;
                        g->buf = (char *)malloc(g->cap);
                        g->buf[0] = '\0';
                        g->len = 0;
                        int saved_indent = g->indent;
                        g->indent = 1;

                        if (spawn_node->left->kind == AST_BLOCK) {
                            AstNode *bs = spawn_node->left->params;
                            while (bs) { cg_stmt(g, bs); bs = bs->next; }
                        } else {
                            cg_indent(g);
                            cg_expr(g, spawn_node->left);
                            cg_str(g, ";\n");
                        }

                        g->indent = saved_indent;
                        cg_closure_def_append(g, g->buf);
                        free(g->buf);
                        g->buf = saved_buf;
                        g->len = saved_len;
                        g->cap = saved_cap;
                    }

                    cg_closure_def_append(g, "    return NULL;\n}\n\n");

                    cg_indent(g);
                    cg_fmt(g, "lcn_task_group_spawn(%s, (LcnTaskFn)%s, NULL);\n",
                           tg_var, spawn_fn);
                } else {
                    cg_stmt(g, s);
                }
                s = s->next;
            }
        }

        cg_indent(g);
        cg_fmt(g, "lcn_task_group_await_all(%s);\n", tg_var);
        cg_indent(g);
        cg_fmt(g, "lcn_task_group_free(%s);\n", tg_var);

        g->indent--;
        cg_indent(g);
        cg_str(g, "} /* end task_group */\n");
        break;
    }

    case AST_MATCH:
        cg_match(g, stmt);
        break;

    case AST_ENSURE:
        /* ensure condition, "message" → if (!cond) { fprintf + return ERR } */
        cg_indent(g);
        cg_str(g, "if (!(");
        if (stmt->left) cg_expr(g, stmt->left);
        cg_str(g, ")) {\n");
        g->indent++;
        cg_indent(g);
        if (stmt->right && stmt->right->kind == AST_STRING_LIT) {
            cg_str(g, "fprintf(stderr, \"ensure failed: %s\\n\", ");
            cg_string_literal(g, stmt->right->val.str_val);
            cg_str(g, ");\n");
            cg_indent(g);
            cg_str(g, "return LCN_ERR(");
            cg_string_literal(g, stmt->right->val.str_val);
            cg_str(g, ");\n");
        } else {
            cg_str(g, "fprintf(stderr, \"ensure failed\\n\");\n");
            cg_indent(g);
            cg_str(g, "return LCN_ERR(\"ensure failed\");\n");
        }
        g->indent--;
        cg_indent(g);
        cg_str(g, "}\n");
        break;

    case AST_REPEAT: {
        /* repeat N times { body } → for loop */
        static int repeat_counter = 0;
        int rid = repeat_counter++;
        cg_indent(g);
        cg_fmt(g, "{ int64_t _repeat_n_%d = ", rid);
        if (stmt->left) cg_expr(g, stmt->left);
        cg_fmt(g, "; int64_t _repeat_i_%d;\n", rid);
        cg_indent(g);
        cg_fmt(g, "for (_repeat_i_%d = 0; _repeat_i_%d < _repeat_n_%d; _repeat_i_%d++) ", rid, rid, rid, rid);
        if (stmt->right) cg_block(g, stmt->right);
        else cg_str(g, "{ }\n");
        cg_indent(g);
        cg_str(g, "}\n");
        break;
    }

    case AST_WAIT_UNTIL:
        /* wait until condition → busy wait (Stage 0: no async) */
        cg_indent(g);
        cg_str(g, "while (!(");
        if (stmt->left) cg_expr(g, stmt->left);
        cg_str(g, ")) { usleep(100000); /* 100ms poll */ }\n");
        break;

    case AST_COMPTIME: {
        /* Compile-time evaluation as a statement.
         * Execute side effects (assert, etc.) at compile time.
         * If the comptime block produces a value, emit it as a comment. */
        ComptimeValue val = comptime_evaluate(stmt, g->arena);
        if (comptime_check_error(val, stmt)) {
            cg_line(g, "/* comptime error: evaluation failed */");
        } else if (val.kind != COMPTIME_NONE) {
            cg_line(g, "/* comptime evaluated */");
        }
        break;
    }

    default:
        cg_line(g, "/* TODO: stmt %s */", ast_kind_name(stmt->kind));
        break;
    }
}

static void cg_block(CodeGen *g, AstNode *block) {
    if (!block) { cg_str(g, "{ }"); return; }

    int saved_defer = g->defer_depth;

    cg_str(g, "{\n");
    g->indent++;

    AstNode *s = block->params;
    while (s) {
        cg_stmt(g, s);
        s = s->next;
    }

    /* Emit deferred expressions in LIFO order at scope exit */
    cg_defer_emit_from(g, saved_defer);

    g->indent--;
    cg_indent(g);
    cg_str(g, "}");
}

/* ============================================================
 * Declaration Generation
 * ============================================================ */

/* --- capability --- */
static void cg_capability(CodeGen *g, AstNode *cap) {
    const char *group = cap->name;
    cg_fmt(g, "/* capability %s */\n", group);

    AstNode *item = cap->params;
    while (item) {
        /* Skip access-control rule nodes — they are not bitflags */
        if (item->kind != AST_CAPABILITY_ITEM) {
            item = item->next;
            continue;
        }
        cg_register_cap(g, group, item->name);
        const char *def = cg_cap_define(g, group, item->name);
        int bit = g->cap_count - 1; /* just registered */
        cg_fmt(g, "#define %-30s ((LcnCapability)(1ULL << %d))\n", def, bit);
        item = item->next;
    }
    cg_nl(g);
}

/* --- access control policy tables --- */
static bool cg_cap_has_rules(AstNode *cap) {
    AstNode *item = cap->params;
    while (item) {
        if (item->kind == AST_CAP_ENDPOINT_RULE ||
            item->kind == AST_CAP_BINARY_RULE   ||
            item->kind == AST_CAP_PATH_RULE     ||
            item->kind == AST_CAP_DENY_RANGE    ||
            item->kind == AST_CAP_DEFAULT) {
            return true;
        }
        item = item->next;
    }
    return false;
}

static void cg_access_policies(CodeGen *g, AstNode *cap) {
    const char *group = cap->name;
    if (!cg_cap_has_rules(cap)) return;

    /* Emit endpoint rules array */
    cg_fmt(g, "/* policy: %s */\n", group);
    cg_fmt(g, "static const LcnEndpointRule lcn_policy_%s_endpoints[] = {\n", group);
    AstNode *item = cap->params;
    while (item) {
        if (item->kind == AST_CAP_ENDPOINT_RULE && item->name) {
            /* Parse host:port from item->name */
            char host[256];
            int port = 0;
            const char *colon = strrchr(item->name, ':');
            if (colon) {
                size_t hlen = (size_t)(colon - item->name);
                if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
                memcpy(host, item->name, hlen);
                host[hlen] = '\0';
                port = atoi(colon + 1);
            } else {
                size_t hlen = strlen(item->name);
                if (hlen >= sizeof(host)) hlen = sizeof(host) - 1;
                memcpy(host, item->name, hlen);
                host[hlen] = '\0';
            }
            /* Find path_glob from sub-fields if present */
            const char *path_glob = NULL;
            AstNode *sf = item->params;
            while (sf) {
                if (sf->name && strcmp(sf->name, "path") == 0 &&
                    sf->right && sf->right->kind == AST_STRING_LIT) {
                    path_glob = sf->right->val.str_val;
                }
                sf = sf->next;
            }
            if (path_glob) {
                cg_fmt(g, "    { \"%s\", %d, %s, \"%s\" },\n",
                       host, port,
                       item->is_mut ? "true" : "false",
                       path_glob);
            } else {
                cg_fmt(g, "    { \"%s\", %d, %s, NULL },\n",
                       host, port,
                       item->is_mut ? "true" : "false");
            }
        }
        item = item->next;
    }
    cg_str(g, "    { NULL, 0, false, NULL } /* sentinel */\n};\n\n");

    /* Emit binary rules array */
    cg_fmt(g, "static const LcnBinaryRule lcn_policy_%s_binaries[] = {\n", group);
    item = cap->params;
    while (item) {
        if (item->kind == AST_CAP_BINARY_RULE && item->name) {
            cg_fmt(g, "    { \"%s\", %s },\n",
                   item->name,
                   item->is_mut ? "true" : "false");
        }
        item = item->next;
    }
    cg_str(g, "    { NULL, false } /* sentinel */\n};\n\n");

    /* Emit path rules array */
    cg_fmt(g, "static const LcnPathRule lcn_policy_%s_paths[] = {\n", group);
    item = cap->params;
    while (item) {
        if (item->kind == AST_CAP_PATH_RULE && item->name) {
            /* Determine mode flags from sub-fields */
            bool cr = true, cw = true;
            AstNode *sf = item->params;
            while (sf) {
                if (sf->name && strcmp(sf->name, "mode") == 0 &&
                    sf->right && sf->right->kind == AST_ARRAY) {
                    AstNode *m;
                    cr = false;
                    cw = false;
                    for (m = sf->right->params; m; m = m->next) {
                        if (m->name && strcmp(m->name, "read") == 0)
                            cr = true;
                        if (m->name && strcmp(m->name, "write") == 0)
                            cw = true;
                    }
                }
                sf = sf->next;
            }
            cg_fmt(g, "    { \"%s\", %s, %s, %s },\n",
                   item->name,
                   item->is_mut ? "true" : "false",
                   cr ? "true" : "false",
                   cw ? "true" : "false");
        }
        item = item->next;
    }
    cg_str(g, "    { NULL, false, false, false } /* sentinel */\n};\n\n");

    /* Determine deny_private and default_deny */
    bool deny_private = false;
    bool default_deny = true;  /* safe default */
    item = cap->params;
    while (item) {
        if (item->kind == AST_CAP_DENY_RANGE) {
            deny_private = true;
        }
        if (item->kind == AST_CAP_DEFAULT) {
            /* is_mut=true means "default: allow", false means "default: deny" */
            default_deny = !item->is_mut;
        }
        item = item->next;
    }

    /* Emit the aggregate policy struct */
    cg_fmt(g, "static const LcnAccessPolicy lcn_policy_%s = {\n", group);
    cg_fmt(g, "    lcn_policy_%s_endpoints,\n", group);
    cg_fmt(g, "    lcn_policy_%s_binaries,\n", group);
    cg_fmt(g, "    lcn_policy_%s_paths,\n", group);
    cg_fmt(g, "    %s, /* deny_private */\n", deny_private ? "true" : "false");
    cg_fmt(g, "    %s  /* default_deny */\n", default_deny ? "true" : "false");
    cg_str(g, "};\n\n");
}

/* --- taint --- */
static void cg_taint(CodeGen *g, AstNode *taint) {
    cg_fmt(g, "typedef struct { LcnString value; } Tainted_%s;\n", taint->name);
    /* Constructor: wraps a string in the tainted type */
    cg_fmt(g, "static Tainted_%s lcn_taint_%s(LcnString val) {\n", taint->name, taint->name);
    cg_fmt(g, "    return (Tainted_%s){ .value = val };\n", taint->name);
    cg_str(g, "}\n");
    /* Accessor: unwraps with tracing */
    cg_fmt(g, "static LcnString lcn_untaint_%s(Tainted_%s t) {\n", taint->name, taint->name);
    cg_fmt(g, "    fprintf(stderr, \"[taint] untaint %s\\n\");\n", taint->name);
    cg_str(g, "    return t.value;\n");
    cg_str(g, "}\n\n");
}

/* --- budget --- */
static void cg_budget(CodeGen *g, AstNode *budget) {
    cg_register_budget(g, budget->name);
    cg_fmt(g, "static LcnBudget lcn_budget_%s(void) {\n", budget->name);

    if (g->use_runtime_header) {
        /* Build mode: use lcn_budget_new() which initializes start_time */
        int64_t b_tokens = 0;
        double b_cost = 0.0;
        int64_t b_duration = 0;
        AstNode *f = budget->params;
        while (f) {
            if (f->name && f->right) {
                if (strcmp(f->name, "max_tokens") == 0 && f->right->kind == AST_INT_LIT)
                    b_tokens = f->right->val.int_val;
                else if (strcmp(f->name, "max_cost") == 0) {
                    if (f->right->kind == AST_INT_LIT) b_cost = (double)f->right->val.int_val;
                    else if (f->right->kind == AST_FLOAT_LIT) b_cost = f->right->val.float_val;
                }
                else if (strcmp(f->name, "max_duration") == 0 && f->right->kind == AST_INT_LIT)
                    b_duration = f->right->val.int_val;
            }
            f = f->next;
        }
        cg_fmt(g, "    return lcn_budget_new(%lld, %g, %lld);\n",
               (long long)b_tokens, b_cost, (long long)b_duration);
    } else {
        /* Standalone mode: struct literal */
        cg_str(g, "    return (LcnBudget){\n");
        AstNode *f = budget->params;
        while (f) {
            if (f->name && f->right) {
                if (strcmp(f->name, "max_tokens") == 0) {
                    cg_str(g, "        .max_tokens = ");
                    cg_expr(g, f->right); cg_str(g, ",\n");
                } else if (strcmp(f->name, "max_cost") == 0) {
                    cg_str(g, "        .max_cost = ");
                    cg_expr(g, f->right); cg_str(g, ",\n");
                } else if (strcmp(f->name, "max_duration") == 0) {
                    cg_str(g, "        .max_duration_secs = ");
                    cg_expr(g, f->right); cg_str(g, ",\n");
                }
            }
            f = f->next;
        }
        cg_str(g, "    };\n");
    }

    cg_str(g, "}\n\n");
}

/* --- guard --- */
static void cg_guard(CodeGen *g, AstNode *guard) {
    cg_fmt(g, "static bool lcn_guard_%s(", guard->name);

    AstNode *p = guard->params;
    if (!p) {
        cg_str(g, "void");
    }
    while (p) {
        if (p->kind == AST_PARAM) {
            cg_type(g, p->type_expr);
            cg_fmt(g, " %s", p->name ? p->name : "_");
        }
        if (p->next) cg_str(g, ", ");
        p = p->next;
    }
    cg_str(g, ") ");

    /* Emit guard body */
    if (guard->left && guard->left->kind == AST_BLOCK) {
        cg_str(g, "{\n");
        g->indent++;
        AstNode *s = guard->left->params;
        while (s) {
            cg_stmt(g, s);
            s = s->next;
        }
        cg_line(g, "return true; /* guard passes by default */");
        g->indent--;
        cg_str(g, "}\n\n");
    } else {
        cg_str(g, "{\n    return true;\n}\n\n");
    }
}

/* --- struct --- */
static void cg_struct(CodeGen *g, AstNode *st) {
    cg_fmt(g, "typedef struct {\n");
    AstNode *f = st->params;
    while (f) {
        if (f->kind == AST_FIELD && f->name) {
            cg_str(g, "    ");
            cg_type(g, f->type_expr);
            cg_fmt(g, " %s;\n", f->name);
        }
        f = f->next;
    }
    cg_fmt(g, "} %s;\n\n", st->name);
}

/* --- interface/trait: vtable struct + trait object type --- */
static void cg_interface(CodeGen *g, AstNode *iface) {
    const char *name = iface->name;
    if (!name) return;

    /* Register interface */
    if (g->interface_count < 64) {
        g->interfaces[g->interface_count].name = name;
        g->interfaces[g->interface_count].node = iface;
        g->interface_count++;
    }

    cg_fmt(g, "/* interface %s — vtable */\n", name);

    /* 1. Emit vtable struct */
    cg_fmt(g, "typedef struct {\n");
    {
        AstNode *m = iface->params;
        while (m) {
            if (m->kind == AST_FN && m->name) {
                cg_str(g, "    ");
                cg_type(g, m->type_expr);
                cg_fmt(g, " (*%s)(void *self", m->name);
                AstNode *p = m->params;
                while (p) {
                    if (p->name && strcmp(p->name, "self") == 0) {
                        p = p->next;
                        continue;
                    }
                    cg_str(g, ", ");
                    cg_type(g, p->type_expr);
                    cg_fmt(g, " %s", p->name ? p->name : "_");
                    p = p->next;
                }
                cg_str(g, ");\n");
            }
            m = m->next;
        }
    }
    cg_fmt(g, "} %s_vtable;\n\n", name);

    /* 2. Emit trait object */
    cg_fmt(g, "typedef struct {\n");
    cg_fmt(g, "    void *data;\n");
    cg_fmt(g, "    %s_vtable *vt;\n", name);
    cg_fmt(g, "} %s_obj;\n\n", name);
}

/* --- impl Trait for Type: emit vtable wrapper shims + vtable instance --- */
static void cg_impl_trait(CodeGen *g, AstNode *impl_node) {
    const char *type_name = NULL;
    const char *trait_name = NULL;

    if (impl_node->left && impl_node->left->kind == AST_TYPE_NAMED &&
        impl_node->left->name) {
        type_name = impl_node->left->name;
    }
    if (impl_node->right && impl_node->right->kind == AST_TYPE_NAMED &&
        impl_node->right->name) {
        trait_name = impl_node->right->name;
    }
    if (!type_name || !trait_name) return;

    cg_fmt(g, "/* impl %s for %s */\n", trait_name, type_name);

    /* 1. Emit thin wrapper shims that cast void * -> Type * and delegate */
    AstNode *m = impl_node->params;
    while (m) {
        if (m->kind == AST_FN && m->name) {
            cg_str(g, "static ");
            cg_type(g, m->type_expr);
            cg_fmt(g, " %s_%s_%s(void *self", type_name, trait_name, m->name);

            AstNode *p = m->params;
            while (p) {
                if (p->name && strcmp(p->name, "self") == 0) {
                    p = p->next;
                    continue;
                }
                cg_str(g, ", ");
                cg_type(g, p->type_expr);
                cg_fmt(g, " %s", p->name ? p->name : "_");
                p = p->next;
            }
            cg_str(g, ") {\n");

            bool has_return_type = m->type_expr &&
                !(m->type_expr->kind == AST_TYPE_NAMED &&
                  m->type_expr->name && strcmp(m->type_expr->name, "void") == 0);
            cg_str(g, "    ");
            if (has_return_type) cg_str(g, "return ");
            cg_fmt(g, "%s_%s((%s *)self", type_name, m->name, type_name);
            p = m->params;
            while (p) {
                if (p->name && strcmp(p->name, "self") == 0) {
                    p = p->next;
                    continue;
                }
                cg_fmt(g, ", %s", p->name ? p->name : "_");
                p = p->next;
            }
            cg_str(g, ");\n");
            cg_str(g, "}\n\n");
        }
        m = m->next;
    }

    /* 2. Emit static vtable instance */
    cg_fmt(g, "static %s_vtable %s_%s_vt = {\n", trait_name, type_name, trait_name);
    {
        AstNode *method = impl_node->params;
        while (method) {
            if (method->kind == AST_FN && method->name) {
                cg_fmt(g, "    .%s = %s_%s_%s,\n",
                       method->name, type_name, trait_name, method->name);
            }
            method = method->next;
        }
    }
    cg_str(g, "};\n\n");
}

/* --- union types: type StringOrInt = string | int -> tagged union --- */
static void cg_union_type(CodeGen *g, const char *name, AstNode *type_union) {
    if (!name || !type_union || type_union->kind != AST_TYPE_UNION) return;

    /* Register for lookup */
    if (g->union_type_count < 64) {
        g->union_types[g->union_type_count].name = name;
        g->union_types[g->union_type_count].type_node = type_union;
        g->union_type_count++;
    }

    cg_fmt(g, "/* union type %s */\n", name);

    AstNode *v;

    /* 1. Emit tag enum */
    cg_fmt(g, "typedef enum {\n");
    {
        int idx = 0;
        v = type_union->params;
        while (v) {
            const char *vname = "unknown";
            if (v->kind == AST_TYPE_NAMED && v->name) vname = v->name;
            else if (v->kind == AST_TYPE_OPTIONAL && v->left &&
                     v->left->kind == AST_TYPE_NAMED && v->left->name) vname = v->left->name;
            cg_fmt(g, "    %s_TAG_%s = %d,\n", name, vname, idx++);
            v = v->next;
        }
    }
    cg_fmt(g, "} %s_Tag;\n\n", name);

    /* 2. Emit tagged union struct */
    cg_fmt(g, "typedef struct {\n");
    cg_fmt(g, "    %s_Tag tag;\n", name);
    cg_str(g, "    union {\n");
    {
        v = type_union->params;
        while (v) {
            const char *vname = "unknown";
            if (v->kind == AST_TYPE_NAMED && v->name) vname = v->name;
            else if (v->kind == AST_TYPE_OPTIONAL && v->left &&
                     v->left->kind == AST_TYPE_NAMED && v->left->name) vname = v->left->name;
            cg_str(g, "        ");
            cg_type(g, v);
            cg_fmt(g, " as_%s;\n", vname);
            v = v->next;
        }
    }
    cg_str(g, "    };\n");
    cg_fmt(g, "} %s;\n\n", name);

    /* 3. Emit constructor functions */
    {
        v = type_union->params;
        while (v) {
            const char *vname = "unknown";
            if (v->kind == AST_TYPE_NAMED && v->name) vname = v->name;
            else if (v->kind == AST_TYPE_OPTIONAL && v->left &&
                     v->left->kind == AST_TYPE_NAMED && v->left->name) vname = v->left->name;
            cg_fmt(g, "static %s %s_from_%s(", name, name, vname);
            cg_type(g, v);
            cg_str(g, " val) {\n");
            cg_fmt(g, "    %s _u;\n", name);
            cg_fmt(g, "    _u.tag = %s_TAG_%s;\n", name, vname);
            cg_fmt(g, "    _u.as_%s = val;\n", vname);
            cg_str(g, "    return _u;\n");
            cg_str(g, "}\n\n");
            v = v->next;
        }
    }

    /* 4. Emit is_type check functions */
    {
        v = type_union->params;
        while (v) {
            const char *vname = "unknown";
            if (v->kind == AST_TYPE_NAMED && v->name) vname = v->name;
            else if (v->kind == AST_TYPE_OPTIONAL && v->left &&
                     v->left->kind == AST_TYPE_NAMED && v->left->name) vname = v->left->name;
            cg_fmt(g, "static bool %s_is_%s(%s val) { return val.tag == %s_TAG_%s; }\n",
                   name, vname, name, name, vname);
            v = v->next;
        }
    }
    cg_nl(g);
}

/* --- impl blocks: impl Type { fn method(...) { ... } } --- */
static void cg_impl(CodeGen *g, AstNode *impl_node) {
    /* Get target type name */
    const char *type_name = NULL;
    if (impl_node->left && impl_node->left->kind == AST_TYPE_NAMED &&
        impl_node->left->name) {
        type_name = impl_node->left->name;
    }
    if (!type_name) return;

    cg_fmt(g, "/* impl %s */\n", type_name);

    /* Emit each method: static RetType TypeName_method(TypeName *self, params...) { body } */
    AstNode *m = impl_node->params;
    while (m) {
        if (m->kind == AST_FN && m->name) {
            cg_str(g, "static ");
            cg_type(g, m->type_expr);
            cg_fmt(g, " %s_%s(%s *self", type_name, m->name, type_name);

            AstNode *p = m->params;
            while (p) {
                /* Skip &self — already emitted as first param */
                if (p->name && strcmp(p->name, "self") == 0) {
                    p = p->next;
                    continue;
                }
                cg_str(g, ", ");
                cg_type(g, p->type_expr);
                cg_fmt(g, " %s", p->name ? p->name : "_");
                p = p->next;
            }
            cg_str(g, ") ");

            AstNode *saved_impl_fn_ret = g->current_fn_ret_type;
            bool saved_in_impl = g->in_impl_method;
            const char *saved_impl_type = g->current_impl_type;
            g->current_fn_ret_type = m->type_expr;
            g->in_impl_method = true;
            g->current_impl_type = type_name;
            if (m->left) {
                cg_block_with_implicit_return(g, m->left, m->type_expr);
            } else {
                cg_str(g, "{ }");
            }
            g->current_fn_ret_type = saved_impl_fn_ret;
            g->in_impl_method = saved_in_impl;
            g->current_impl_type = saved_impl_type;
            cg_str(g, "\n\n");
        }
        m = m->next;
    }
}

/* Emit a block with implicit return: the last expression-statement becomes
 * a return statement. Used for fn bodies and agent methods. */
/* Helper: emit the "ok" return for a Result-returning function.
 * For bare Result → LCN_OK; for Result<T,E> → Result_T_E_ok(...) or zero init. */
static void cg_result_return_ok(CodeGen *g, AstNode *ret_type) {
    if (ret_type && ret_type->generics) {
        char mono_name[128];
        mono_result_name(mono_name, sizeof(mono_name), ret_type->generics);
        /* Emit zero-value ok: { .ok = true, .value = {0}, .error = {0} } */
        const char *t_c = mono_type_from_expr(ret_type->generics);
        bool t_is_ptr = (strcmp(t_c, "LcnString") == 0);
        if (t_is_ptr) {
            cg_fmt(g, "return %s_ok(\"\");\n", mono_name);
        } else {
            cg_fmt(g, "return %s_ok((%s){0});\n", mono_name, t_c);
        }
    } else {
        cg_str(g, "return LCN_OK;\n");
    }
}

/* Helper: emit the "ok" return wrapping a value for a Result-returning function. */
static void cg_result_return_ok_value(CodeGen *g, AstNode *ret_type, AstNode *value_expr) {
    if (ret_type && ret_type->generics) {
        char mono_name[128];
        mono_result_name(mono_name, sizeof(mono_name), ret_type->generics);
        cg_fmt(g, "return %s_ok(", mono_name);
        cg_expr(g, value_expr);
        cg_str(g, ");\n");
    } else {
        cg_str(g, "return (LcnResult){ .ok = true, .value = (void *)");
        cg_expr(g, value_expr);
        cg_str(g, ", .error = NULL };\n");
    }
}

static void cg_block_with_implicit_return(CodeGen *g, AstNode *block, AstNode *ret_type) {
    if (!block) { cg_str(g, "{ }"); return; }

    int saved_defer = g->defer_depth;

    bool ret_is_result = ret_type &&
        ret_type->kind == AST_TYPE_NAMED &&
        ret_type->name && strcmp(ret_type->name, "Result") == 0;

    bool ret_is_option = ret_type &&
        ret_type->kind == AST_TYPE_NAMED &&
        ret_type->name && strcmp(ret_type->name, "Option") == 0 &&
        ret_type->generics;

    cg_str(g, "{\n");
    g->indent++;

    AstNode *s = block->params;
    while (s) {
        bool is_last = (s->next == NULL);
        if (is_last && s->kind == AST_EXPR_STMT) {
            /* Last expr-stmt → implicit return (or plain stmt for void fns).
             * Defers must execute AFTER the statement but BEFORE any return. */
            cg_indent(g);
            /* print/println as last stmt in Result fn → call, then return OK */
            if (ret_is_result && s->left && s->left->kind == AST_CALL &&
                s->left->left && s->left->left->kind == AST_IDENT && s->left->left->name &&
                (strcmp(s->left->left->name, "print") == 0 ||
                 strcmp(s->left->left->name, "println") == 0)) {
                cg_expr(g, s->left);
                cg_str(g, ";\n");
                cg_defer_emit_from(g, saved_defer);
                cg_indent(g);
                cg_result_return_ok(g, ret_type);
            } else if (ret_is_result && s->left && s->left->kind == AST_IF) {
                /* If-statement as last expr in Result fn → emit as stmt, then OK */
                cg_stmt(g, s->left);
                cg_defer_emit_from(g, saved_defer);
                cg_indent(g);
                cg_result_return_ok(g, ret_type);
            } else if (ret_is_result && s->left && s->left->kind == AST_MATCH) {
                /* Match as last expr in Result fn → emit as stmt, then OK */
                cg_stmt(g, s->left);
                cg_defer_emit_from(g, saved_defer);
                cg_indent(g);
                cg_result_return_ok(g, ret_type);
            } else if (ret_is_result && s->left &&
                (s->left->kind == AST_IDENT || s->left->kind == AST_FIELD_ACCESS)) {
                /* Bare identifier/field in a Result-returning function:
                 * pack the value into Result so unwrap() can extract it */
                cg_defer_emit_from(g, saved_defer);
                if (ret_type && ret_type->generics) {
                    cg_result_return_ok_value(g, ret_type, s->left);
                } else if (might_be_string_expr(g, s->left)) {
                    cg_str(g, "return (LcnResult){ .ok = true, .value = (void *)");
                    cg_expr(g, s->left);
                    cg_str(g, ", .error = NULL };\n");
                } else {
                    cg_str(g, "(void)");
                    cg_expr(g, s->left);
                    cg_str(g, ";\n");
                    cg_indent(g);
                    cg_str(g, "return LCN_OK;\n");
                }
            } else if (s->left && (s->left->kind == AST_MATCH || s->left->kind == AST_IF)) {
                /* Match/if as last expr-stmt → emit as statement */
                cg_stmt(g, s->left);
                cg_defer_emit_from(g, saved_defer);
            } else if (ret_is_result && ret_type && ret_type->generics) {
                /* Monomorphized Result fn: wrap any expression in ok() */
                cg_defer_emit_from(g, saved_defer);
                cg_result_return_ok_value(g, ret_type, s->left);
            } else if (ret_is_option) {
                /* Monomorphized Option fn: wrap expression in some() */
                char mono_name[128];
                cg_defer_emit_from(g, saved_defer);
                mono_option_name(mono_name, sizeof(mono_name), ret_type->generics);
                cg_fmt(g, "return %s_some(", mono_name);
                cg_expr(g, s->left);
                cg_str(g, ");\n");
            } else if (!ret_type) {
                /* Void function: emit as plain statement, then defers at scope exit */
                cg_expr(g, s->left);
                cg_str(g, ";\n");
                cg_defer_emit_from(g, saved_defer);
            } else {
                /* Non-void function: emit defers before the return */
                cg_defer_emit_from(g, saved_defer);
                cg_str(g, "return ");
                cg_expr(g, s->left);
                cg_str(g, ";\n");
            }
        } else if (is_last && s->kind == AST_IDENT) {
            /* Emit defers before the implicit return */
            cg_defer_emit_from(g, saved_defer);
            cg_indent(g);
            if (ret_is_result && ret_type && ret_type->generics) {
                cg_result_return_ok_value(g, ret_type, s);
            } else if (ret_is_result && might_be_string_expr(g, s)) {
                cg_str(g, "return (LcnResult){ .ok = true, .value = (void *)");
                cg_expr(g, s);
                cg_str(g, ", .error = NULL };\n");
            } else if (ret_is_result) {
                cg_str(g, "(void)");
                cg_expr(g, s);
                cg_str(g, ";\n");
                cg_indent(g);
                cg_str(g, "return LCN_OK;\n");
            } else {
                cg_str(g, "return ");
                cg_expr(g, s);
                cg_str(g, ";\n");
            }
        } else {
            cg_stmt(g, s);
            /* If last stmt is not a return/expr-stmt and fn returns Result, add LCN_OK */
            if (is_last && ret_is_result &&
                s->kind != AST_RETURN) {
                /* Emit defers before the implicit return */
                cg_defer_emit_from(g, saved_defer);
                cg_indent(g);
                cg_result_return_ok(g, ret_type);
            } else if (is_last && s->kind != AST_RETURN) {
                /* Emit remaining defers at scope exit
                 * (skip for explicit return — it already emitted defers) */
                cg_defer_emit_from(g, saved_defer);
            }
        }
        s = s->next;
    }

    /* If block was empty, still restore defer depth */
    g->defer_depth = saved_defer;
    g->indent--;
    cg_indent(g);
    cg_str(g, "}");
}

/* --- extern function declaration (FFI) --- */
static void cg_extern_fn(CodeGen *g, AstNode *fn) {
    cg_str(g, "/* extern */ ");
    if (fn->type_expr) cg_type(g, fn->type_expr);
    else cg_str(g, "void");
    cg_fmt(g, " %s(", fn->name ? fn->name : "anon");

    AstNode *p = fn->params;
    if (!p) cg_str(g, "void");
    {
        bool first = true;
        while (p) {
            if (!first) cg_str(g, ", ");
            if (p->type_expr) cg_type(g, p->type_expr);
            else cg_str(g, "void *");
            cg_fmt(g, " %s", p->name ? p->name : "_");
            first = false;
            p = p->next;
        }
    }
    cg_str(g, ");\n\n");
}

/* --- function --- */
static void cg_fn(CodeGen *g, AstNode *fn) {
    /* Extern function — just emit declaration, no body */
    if (fn->is_unsafe && !fn->left) {
        cg_extern_fn(g, fn);
        return;
    }

    bool is_main = fn->name && strcmp(fn->name, "main") == 0;
    if (is_main) {
        g->has_main = true;
        g->main_returns_void = (!fn->type_expr ||
            (fn->type_expr->kind == AST_TYPE_NAMED && fn->type_expr->name &&
             strcmp(fn->type_expr->name, "void") == 0));
    }

    /* Save output position — closure defs generated during body emission
     * will be inserted before this function. */
    size_t fn_start_pos = g->len;

    /* Clear deferred closure buffer before this function */
    if (g->closure_defs_buf) {
        g->closure_defs_len = 0;
        g->closure_defs_buf[0] = '\0';
    }

    /* Save closure var scope (closures defined in this function are local) */
    int saved_closure_var_count = g->closure_var_count;

    cg_str(g, "static ");
    cg_type(g, fn->type_expr);
    cg_fmt(g, " lcn_%s(", fn->name ? fn->name : "anon");

    AstNode *p = fn->params;
    if (!p) cg_str(g, "void");
    while (p) {
        cg_type(g, p->type_expr);
        cg_fmt(g, " %s", p->name ? p->name : "_");
        if (p->next) cg_str(g, ", ");
        p = p->next;
    }
    cg_str(g, ") ");

    /* BUG-2 fix: save string var scope, register string-typed params */
    int saved_string_var_count = g->string_var_count;
    {
        AstNode *sp = fn->params;
        while (sp) {
            if (sp->name && sp->type_expr &&
                sp->type_expr->kind == AST_TYPE_NAMED &&
                sp->type_expr->name &&
                strcmp(sp->type_expr->name, "string") == 0 &&
                g->string_var_count < 256) {
                g->string_vars[g->string_var_count++] = sp->name;
            }
            sp = sp->next;
        }
    }

    AstNode *saved_fn_ret_type = g->current_fn_ret_type;
    g->current_fn_ret_type = fn->type_expr;
    if (fn->left) {
        cg_block_with_implicit_return(g, fn->left, fn->type_expr);
    } else {
        cg_str(g, "{ }");
    }
    g->current_fn_ret_type = saved_fn_ret_type;
    g->string_var_count = saved_string_var_count;
    cg_str(g, "\n\n");

    /* If closures were generated during this function, insert their
     * definitions (structs + static functions) before this function. */
    if (g->closure_defs_buf && g->closure_defs_len > 0) {
        size_t fn_code_len = g->len - fn_start_pos;
        size_t defs_len = g->closure_defs_len;

        /* Ensure the main buffer has space */
        cg_grow(g, defs_len);

        /* Shift the function code forward to make room for closure defs */
        memmove(g->buf + fn_start_pos + defs_len,
                g->buf + fn_start_pos,
                fn_code_len);

        /* Copy closure defs into the gap */
        memcpy(g->buf + fn_start_pos, g->closure_defs_buf, defs_len);
        g->len += defs_len;
        g->buf[g->len] = '\0';

        /* Clear deferred buffer */
        g->closure_defs_len = 0;
        g->closure_defs_buf[0] = '\0';
    }

    g->closure_var_count = saved_closure_var_count;
}

/* --- tool --- */
static void cg_tool(CodeGen *g, AstNode *tool) {
    /* Collect requires from fields */
    const char *description = NULL;
    AstNode *requires_arr = NULL;

    AstNode *f = tool->right; /* fields */
    while (f) {
        if (f->name) {
            if (strcmp(f->name, "description") == 0 && f->right &&
                f->right->kind == AST_STRING_LIT) {
                description = f->right->val.str_val;
            }
            if (strcmp(f->name, "requires") == 0 && f->right &&
                f->right->kind == AST_ARRAY) {
                requires_arr = f->right;
            }
        }
        f = f->next;
    }

    /* Register tool for call rewriting, type inference, and capability fence */
    if (g->tool_count < 256) {
        int ti = g->tool_count;
        g->tools[ti].name = tool->name;
        g->tools[ti].return_type = tool->type_expr;
        g->tools[ti].requires_count = 0;
        /* Store requires list for capability fence tool-list computation */
        if (requires_arr && requires_arr->params) {
            AstNode *req = requires_arr->params;
            while (req && g->tools[ti].requires_count < 16) {
                if (req->name) {
                    g->tools[ti].requires[g->tools[ti].requires_count++] = req->name;
                }
                req = req->next;
            }
        }
        g->tool_count++;
    }

    /* Comment */
    cg_fmt(g, "/* tool %s", tool->name);
    if (description) cg_fmt(g, " — %s", description);
    cg_str(g, " */\n");

    /* Function signature */
    cg_str(g, "static ");
    cg_type(g, tool->type_expr);
    cg_fmt(g, " lcn_tool_%s(LcnCapability _agent_caps", tool->name);

    AstNode *p = tool->params;
    while (p) {
        cg_str(g, ", ");
        cg_type(g, p->type_expr);
        cg_fmt(g, " %s", p->name ? p->name : "_");
        p = p->next;
    }
    cg_str(g, ") {\n");
    g->indent++;

    /* Capability assertions */
    if (requires_arr && requires_arr->params) {
        AstNode *req = requires_arr->params;
        while (req) {
            if (req->name) {
                const char *cap_def = cg_lookup_cap(g, req->name);
                if (cap_def) {
                    cg_line(g, "if (!(_agent_caps & %s)) {", cap_def);
                    g->indent++;
                    cg_line(g, "fprintf(stderr, \"FATAL: agent lacks capability '%s' for tool '%s'\\n\");",
                            req->name, tool->name);
                    cg_line(g, "exit(1);");
                    g->indent--;
                    cg_line(g, "}");
                }
            }
            req = req->next;
        }
    }

    /* Check for auto-LLM body generation */
    bool has_body = (tool->left && tool->left->kind == AST_BLOCK && tool->left->params != NULL);
    bool has_llm_require = false;
    if (requires_arr && requires_arr->params) {
        AstNode *r = requires_arr->params;
        while (r) {
            if (r->name && strcmp(r->name, "llm.complete") == 0)
                has_llm_require = true;
            r = r->next;
        }
    }

    if (!has_body && has_llm_require && g->use_runtime_header) {
        /* Auto-generate LLM call using first parameter as input */
        const char *input_param = (tool->params && tool->params->name)
                                   ? tool->params->name : NULL;
        cg_line(g, "LcnLlmResult _llm = lcn_llm_call(NULL, NULL, NULL, %s, NULL, NULL);",
                input_param ? input_param : "\"\"");
        cg_line(g, "if (_llm.ok && _llm.content) {");
        g->indent++;
        cg_line(g, "printf(\"%%s\\n\", _llm.content);");
        cg_line(g, "free(_llm.content);");
        g->indent--;
        cg_line(g, "}");
        cg_line(g, "free(_llm.error);");
        cg_line(g, "return LCN_OK;");
    } else {
        /* Body statements */
        if (tool->left && tool->left->kind == AST_BLOCK) {
            AstNode *s = tool->left->params;
            while (s) {
                cg_stmt(g, s);
                s = s->next;
            }
        }

        /* Default return */
        if (tool->type_expr) {
            const char *tn = tool->type_expr->name;
            if (tn && strcmp(tn, "Result") == 0)
                cg_line(g, "return LCN_OK;");
            else if (tn && strcmp(tn, "Vec") == 0)
                cg_line(g, "return lcn_vec_new();");
            else if (tn && strcmp(tn, "void") != 0)
                cg_line(g, "return (%s){0};", tn);
        }
    }

    g->indent--;
    cg_str(g, "}\n\n");
}

/* --- supervisor --- */
static void cg_supervisor(CodeGen *g, AstNode *sup) {
    const char *name = sup->name;
    const char *strat_enum = "LCN_STRATEGY_ONE_FOR_ONE";
    int strat_val = 0; /* 0=one_for_one, 1=rest_for_all, 2=one_for_all */
    int max_restarts = 3;
    int window_seconds = 60;

    /* Collect children names from the `children:` field */
    const char *child_names[64];
    int child_count = 0;

    /* Scan fields */
    AstNode *f = sup->params;
    while (f) {
        if (f->name && f->right) {
            if (strcmp(f->name, "strategy") == 0) {
                if (f->right->kind == AST_IDENT && f->right->name) {
                    if (strcmp(f->right->name, "one_for_one") == 0) {
                        strat_enum = "LCN_STRATEGY_ONE_FOR_ONE";
                        strat_val = 0;
                    } else if (strcmp(f->right->name, "rest_for_all") == 0) {
                        strat_enum = "LCN_STRATEGY_REST_FOR_ALL";
                        strat_val = 1;
                    } else if (strcmp(f->right->name, "one_for_all") == 0) {
                        strat_enum = "LCN_STRATEGY_ONE_FOR_ALL";
                        strat_val = 2;
                    }
                }
            } else if (strcmp(f->name, "max_restarts") == 0) {
                if (f->right->kind == AST_INT_LIT)
                    max_restarts = (int)f->right->val.int_val;
            } else if (strcmp(f->name, "window") == 0) {
                if (f->right->kind == AST_INT_LIT)
                    window_seconds = (int)f->right->val.int_val;
            } else if (strcmp(f->name, "children") == 0) {
                /* children: [AgentA, AgentB, AgentC] */
                if (f->right->kind == AST_ARRAY) {
                    AstNode *elem = f->right->params;
                    while (elem && child_count < 64) {
                        if (elem->name) {
                            child_names[child_count++] = elem->name;
                        }
                        elem = elem->next;
                    }
                }
            }
        }
        f = f->next;
    }

    /* Register supervisor in codegen state */
    if (g->supervisor_count < 32) {
        int idx = g->supervisor_count;
        g->supervisors[idx].name = name;
        g->supervisors[idx].strategy = strat_val;
        g->supervisors[idx].max_restarts = max_restarts;
        g->supervisors[idx].window_seconds = window_seconds;
        g->supervisors[idx].child_count = child_count;
        for (int i = 0; i < child_count; i++)
            g->supervisors[idx].child_names[i] = child_names[i];
        g->supervisor_count++;
    }

    /* Emit placeholder start/stop functions for each child agent. */
    for (int i = 0; i < child_count; i++) {
        bool already_emitted = false;
        for (int si = 0; si < g->supervisor_count - 1 && !already_emitted; si++) {
            for (int ci = 0; ci < g->supervisors[si].child_count; ci++) {
                if (strcmp(g->supervisors[si].child_names[ci], child_names[i]) == 0) {
                    already_emitted = true;
                    break;
                }
            }
        }
        if (!already_emitted) {
            cg_fmt(g, "static void lcn_agent_%s_start(void) { /* agent start stub */ }\n",
                   child_names[i]);
            cg_fmt(g, "static void lcn_agent_%s_stop(void)  { /* agent stop stub */ }\n",
                   child_names[i]);
        }
    }
    cg_str(g, "\n");

    /* Emit supervisor global pointer */
    cg_fmt(g, "static LcnSupervisor *_lcn_sup_%s = NULL;\n\n", name);

    /* Emit supervisor initialization function */
    cg_fmt(g, "static LcnSupervisor *lcn_supervisor_%s_init(void) {\n", name);
    cg_fmt(g, "    LcnSupervisor *sup = lcn_supervisor_new(\"%s\", %s, %d, %d);\n",
           name, strat_enum, max_restarts, window_seconds);

    /* Add children */
    for (int i = 0; i < child_count; i++) {
        cg_fmt(g, "    lcn_supervisor_add_child(sup, \"%s\", lcn_agent_%s_start, lcn_agent_%s_stop, LCN_RESTART_ALWAYS);\n",
               child_names[i], child_names[i], child_names[i]);
    }

    cg_fmt(g, "    _lcn_sup_%s = sup;\n", name);
    cg_str(g, "    return sup;\n");
    cg_str(g, "}\n\n");

    /* Emit convenience: report failure by agent name */
    cg_fmt(g, "static int lcn_supervisor_%s_report_failure(const char *agent_name) {\n", name);
    cg_fmt(g, "    if (!_lcn_sup_%s) return -1;\n", name);
    cg_fmt(g, "    for (int i = 0; i < _lcn_sup_%s->child_count; i++) {\n", name);
    cg_fmt(g, "        if (strcmp(_lcn_sup_%s->children[i].name, agent_name) == 0)\n", name);
    cg_fmt(g, "            return lcn_supervisor_child_failed(_lcn_sup_%s, i);\n", name);
    cg_str(g, "    }\n");
    cg_str(g, "    return -1;\n");
    cg_str(g, "}\n\n");
}

/* --- skill --- */
static void cg_skill(CodeGen *g, AstNode *skill) {
    const char *version = "0.0.0";
    AstNode *f = skill->params;

    /* Extract metadata */
    while (f) {
        if (f->kind == AST_FIELD && f->name && f->right) {
            if (strcmp(f->name, "version") == 0 && f->right->kind == AST_STRING_LIT)
                version = f->right->val.str_val;
        }
        f = f->next;
    }

    /* Emit struct */
    cg_fmt(g, "typedef struct {\n");
    cg_str(g, "    LcnString version;\n");
    cg_str(g, "    uint64_t required_caps;\n");
    cg_fmt(g, "} Skill_%s;\n\n", skill->name);

    /* Emit constructor */
    cg_fmt(g, "static Skill_%s lcn_skill_%s(void) {\n", skill->name, skill->name);
    cg_fmt(g, "    return (Skill_%s){\n", skill->name);
    cg_fmt(g, "        .version = \"%s\",\n", version);
    cg_str(g, "        .required_caps = 0,\n");
    cg_str(g, "    };\n}\n\n");

    /* Emit tool functions that belong to this skill */
    f = skill->params;
    while (f) {
        if (f->kind == AST_TOOL) cg_tool(g, f);
        if (f->kind == AST_FN) cg_fn(g, f);
        f = f->next;
    }
}

/* --- MCP protocol import --- */
static void cg_use_mcp(CodeGen *g, AstNode *use) {
    const char *command = use->val.str_val;
    const char *alias = (use->right && use->right->name) ? use->right->name : "mcp";

    /* Register alias for method call rewriting (db.call → lcn_mcp_db_call)
     * May already be pre-registered — skip duplicates */
    {
        bool already = false;
        int ai;
        for (ai = 0; ai < g->mcp_alias_count; ai++) {
            if (strcmp(g->mcp_aliases[ai].alias, alias) == 0) { already = true; break; }
        }
        if (!already && g->mcp_alias_count < 32) {
            g->mcp_aliases[g->mcp_alias_count].alias = alias;
            g->mcp_alias_count++;
        }
    }

    cg_fmt(g, "/* MCP import: %s */\n", command ? command : "?");

    cg_fmt(g, "static LcnString lcn_mcp_%s_call(LcnString tool, LcnString args_json) {\n", alias);
    g->indent++;
    if (g->use_runtime_header) {
        cg_fmt(g, "    LcnMcpResult _r = lcn_mcp_dispatch(\"%s\", tool, args_json);\n",
               command ? command : "");
        cg_line(g, "if (_r.ok && _r.result_json) {");
        g->indent++;
        cg_line(g, "LcnString _result = _r.result_json; /* caller may free */");
        cg_line(g, "return _result;");
        g->indent--;
        cg_line(g, "}");
        cg_line(g, "fprintf(stderr, \"MCP error: %%s\\n\", _r.error ? _r.error : \"unknown\");");
        cg_line(g, "free(_r.error);");
        cg_line(g, "free(_r.result_json);");
        cg_line(g, "return \"\";");
    } else {
        cg_line(g, "(void)tool; (void)args_json;");
        cg_fmt(g, "    printf(\"[MCP:%s] %%s(%%s)\\n\", tool, args_json);\n", alias);
        cg_line(g, "return \"\";");
    }
    g->indent--;
    cg_str(g, "}\n\n");
}

/* --- A2A protocol import --- */
static void cg_use_a2a(CodeGen *g, AstNode *use) {
    const char *url = use->val.str_val;
    const char *alias = (use->right && use->right->name) ? use->right->name : "a2a";

    cg_fmt(g, "/* A2A import: %s */\n", url ? url : "?");

    cg_fmt(g, "static const char *_a2a_%s_url = \"%s\";\n", alias, url ? url : "");

    if (g->use_runtime_header) {
        /* Runtime-backed: emit A2A client instance and proper wrappers */
        cg_fmt(g, "static LcnA2AClient *_a2a_%s_client = NULL;\n\n", alias);

        /* Connect function (lazy init) */
        cg_fmt(g, "static LcnA2AClient *lcn_a2a_%s_connect(void) {\n", alias);
        g->indent++;
        cg_fmt(g, "if (!_a2a_%s_client) {\n", alias);
        g->indent++;
        cg_fmt(g, "_a2a_%s_client = lcn_a2a_connect(_a2a_%s_url);\n", alias, alias);
        g->indent--;
        cg_line(g, "}");
        cg_fmt(g, "return _a2a_%s_client;\n", alias);
        g->indent--;
        cg_str(g, "}\n\n");

        /* Legacy call wrapper (backward compatible) */
        cg_fmt(g, "static LcnResult lcn_a2a_%s_call(LcnString method, LcnString payload) {\n", alias);
        g->indent++;
        cg_fmt(g, "LcnA2AClient *c = lcn_a2a_%s_connect();\n", alias);
        cg_line(g, "if (!c) return LCN_ERR(\"A2A connection failed\");");
        cg_line(g, "(void)method;");
        cg_line(g, "char *result = lcn_a2a_send(c, payload);");
        cg_line(g, "if (!result) return LCN_ERR(\"A2A send failed\");");
        cg_line(g, "free(result);");
        cg_line(g, "return LCN_OK;");
        g->indent--;
        cg_str(g, "}\n\n");

        /* Send message wrapper (returns string) */
        cg_fmt(g, "static LcnString lcn_a2a_%s_send(LcnString message) {\n", alias);
        g->indent++;
        cg_fmt(g, "LcnA2AClient *c = lcn_a2a_%s_connect();\n", alias);
        cg_line(g, "if (!c) return \"\";");
        cg_line(g, "char *r = lcn_a2a_send(c, message);");
        cg_line(g, "return r ? r : \"\";");
        g->indent--;
        cg_str(g, "}\n\n");

        /* Send task wrapper (returns string) */
        cg_fmt(g, "static LcnString lcn_a2a_%s_send_task(LcnString task_json) {\n", alias);
        g->indent++;
        cg_fmt(g, "LcnA2AClient *c = lcn_a2a_%s_connect();\n", alias);
        cg_line(g, "if (!c) return \"\";");
        cg_line(g, "char *r = lcn_a2a_send_task(c, task_json);");
        cg_line(g, "return r ? r : \"\";");
        g->indent--;
        cg_str(g, "}\n\n");

        /* Disconnect/cleanup wrapper */
        cg_fmt(g, "static void lcn_a2a_%s_disconnect(void) {\n", alias);
        g->indent++;
        cg_fmt(g, "if (_a2a_%s_client) { lcn_a2a_disconnect(_a2a_%s_client); _a2a_%s_client = NULL; }\n",
               alias, alias, alias);
        g->indent--;
        cg_str(g, "}\n\n");
    } else {
        /* Standalone mode: printf stubs */
        cg_str(g, "\n");
        cg_fmt(g, "static LcnResult lcn_a2a_%s_call(LcnString method, LcnString payload) {\n", alias);
        g->indent++;
        cg_line(g, "(void)method; (void)payload;");
        cg_fmt(g, "printf(\"[A2A:%s] %%s(%%s)\\n\", method, payload);\n", alias);
        cg_line(g, "return LCN_OK;");
        g->indent--;
        cg_str(g, "}\n\n");

        cg_fmt(g, "static LcnString lcn_a2a_%s_send(LcnString message) {\n", alias);
        g->indent++;
        cg_fmt(g, "printf(\"[A2A:%s] send: %%s\\n\", message);\n", alias);
        cg_line(g, "return \"\";");
        g->indent--;
        cg_str(g, "}\n\n");

        cg_fmt(g, "static LcnString lcn_a2a_%s_send_task(LcnString task_json) {\n", alias);
        g->indent++;
        cg_fmt(g, "printf(\"[A2A:%s] task: %%s\\n\", task_json);\n", alias);
        cg_line(g, "return \"\";");
        g->indent--;
        cg_str(g, "}\n\n");
    }
}

/* --- driver import (native DB bindings) --- */
static void cg_use_driver(CodeGen *g, AstNode *use) {
    const char *driver_name = use->val.str_val;
    const char *alias = (use->right && use->right->name) ? use->right->name : "db";

    /* Register alias for method call rewriting (db.query → lcn_driver_db_query) */
    {
        bool already = false;
        int ai;
        for (ai = 0; ai < g->mcp_alias_count; ai++) {
            if (strcmp(g->mcp_aliases[ai].alias, alias) == 0) { already = true; break; }
        }
        if (!already && g->mcp_alias_count < 32) {
            g->mcp_aliases[g->mcp_alias_count].alias = alias;
            g->mcp_alias_count++;
        }
    }

    cg_fmt(g, "/* driver import: %s as %s */\n", driver_name ? driver_name : "?", alias);

    if (driver_name && strcmp(driver_name, "mysql") == 0) {
        /* Generate wrapper functions for db.connect(), db.query(), db.execute(), db.close() */

        /* db.connect(host, user, pass, database, port) → LcnDbConn* */
        cg_fmt(g, "static void *lcn_driver_%s_connect(LcnString host, LcnString user, LcnString pass, LcnString database, int64_t port) {\n", alias);
        g->indent++;
        if (g->use_runtime_header) {
            cg_line(g, "return (void *)lcn_db_connect(host, user, pass, database, (int)port);");
        } else {
            cg_line(g, "(void)host; (void)user; (void)pass; (void)database; (void)port;");
            cg_fmt(g, "printf(\"[driver:%s] connect(%%s, %%s, %%s)\\n\", host, user, database);\n", alias);
            cg_line(g, "return NULL;");
        }
        g->indent--;
        cg_str(g, "}\n\n");

        /* db.query(conn, sql) → LcnDbResult* as void* */
        cg_fmt(g, "static void *lcn_driver_%s_query(void *conn, LcnString sql) {\n", alias);
        g->indent++;
        if (g->use_runtime_header) {
            cg_line(g, "return (void *)lcn_db_query((LcnDbConn *)conn, sql);");
        } else {
            cg_line(g, "(void)conn;");
            cg_fmt(g, "printf(\"[driver:%s] query(%%s)\\n\", sql);\n", alias);
            cg_line(g, "return NULL;");
        }
        g->indent--;
        cg_str(g, "}\n\n");

        /* db.execute(conn, sql) → int64_t affected rows */
        cg_fmt(g, "static int64_t lcn_driver_%s_execute(void *conn, LcnString sql) {\n", alias);
        g->indent++;
        if (g->use_runtime_header) {
            cg_line(g, "return lcn_db_execute((LcnDbConn *)conn, sql);");
        } else {
            cg_line(g, "(void)conn;");
            cg_fmt(g, "printf(\"[driver:%s] execute(%%s)\\n\", sql);\n", alias);
            cg_line(g, "return 0;");
        }
        g->indent--;
        cg_str(g, "}\n\n");

        /* db.close(conn) */
        cg_fmt(g, "static void lcn_driver_%s_close(void *conn) {\n", alias);
        g->indent++;
        if (g->use_runtime_header) {
            cg_line(g, "lcn_db_close((LcnDbConn *)conn);");
        } else {
            cg_line(g, "(void)conn;");
            cg_fmt(g, "printf(\"[driver:%s] close()\\n\");\n", alias);
        }
        g->indent--;
        cg_str(g, "}\n\n");

        /* db.row_count(result) → int */
        cg_fmt(g, "static int64_t lcn_driver_%s_row_count(void *result) {\n", alias);
        g->indent++;
        if (g->use_runtime_header) {
            cg_line(g, "return (int64_t)lcn_db_row_count((LcnDbResult *)result);");
        } else {
            cg_line(g, "(void)result; return 0;");
        }
        g->indent--;
        cg_str(g, "}\n\n");

        /* db.get(result, row, col_name) → string */
        cg_fmt(g, "static LcnString lcn_driver_%s_get(void *result, int64_t row, LcnString col) {\n", alias);
        g->indent++;
        if (g->use_runtime_header) {
            cg_line(g, "return lcn_db_row_get((LcnDbResult *)result, (int)row, col);");
        } else {
            cg_line(g, "(void)result; (void)row; (void)col; return \"\";");
        }
        g->indent--;
        cg_str(g, "}\n\n");

        /* db.get_number(result, row, col_name) → int64 */
        cg_fmt(g, "static int64_t lcn_driver_%s_get_number(void *result, int64_t row, LcnString col) {\n", alias);
        g->indent++;
        if (g->use_runtime_header) {
            cg_line(g, "return lcn_db_row_get_number((LcnDbResult *)result, (int)row, col);");
        } else {
            cg_line(g, "(void)result; (void)row; (void)col; return 0;");
        }
        g->indent--;
        cg_str(g, "}\n\n");

        /* db.free(result) */
        cg_fmt(g, "static void lcn_driver_%s_free(void *result) {\n", alias);
        g->indent++;
        if (g->use_runtime_header) {
            cg_line(g, "lcn_db_result_free((LcnDbResult *)result);");
        } else {
            cg_line(g, "(void)result;");
        }
        g->indent--;
        cg_str(g, "}\n\n");
    } else if (driver_name && strcmp(driver_name, "postgres") == 0) {
        /* Generate wrapper functions for db.connect(), db.query(), db.execute(), db.close(), db.escape() */

        /* db.connect(host, port, user, pass, database) → LcnPgConn* */
        cg_fmt(g, "static void *lcn_driver_%s_connect(LcnString host, int64_t port, LcnString user, LcnString pass, LcnString database) {\n", alias);
        g->indent++;
        if (g->use_runtime_header) {
            cg_line(g, "return (void *)lcn_pg_connect(host, (int)port, user, pass, database);");
        } else {
            cg_line(g, "(void)host; (void)port; (void)user; (void)pass; (void)database;");
            cg_fmt(g, "printf(\"[driver:%s] pg_connect(%%s, %%lld, %%s)\\n\", host, (long long)port, database);\n", alias);
            cg_line(g, "return NULL;");
        }
        g->indent--;
        cg_str(g, "}\n\n");

        /* db.query(conn, sql) → LcnPgResult* as void* */
        cg_fmt(g, "static void *lcn_driver_%s_query(void *conn, LcnString sql) {\n", alias);
        g->indent++;
        if (g->use_runtime_header) {
            cg_line(g, "return (void *)lcn_pg_query((LcnPgConn *)conn, sql);");
        } else {
            cg_line(g, "(void)conn;");
            cg_fmt(g, "printf(\"[driver:%s] pg_query(%%s)\\n\", sql);\n", alias);
            cg_line(g, "return NULL;");
        }
        g->indent--;
        cg_str(g, "}\n\n");

        /* db.execute(conn, sql) → int64_t affected rows */
        cg_fmt(g, "static int64_t lcn_driver_%s_execute(void *conn, LcnString sql) {\n", alias);
        g->indent++;
        if (g->use_runtime_header) {
            cg_line(g, "return lcn_pg_execute((LcnPgConn *)conn, sql);");
        } else {
            cg_line(g, "(void)conn;");
            cg_fmt(g, "printf(\"[driver:%s] pg_execute(%%s)\\n\", sql);\n", alias);
            cg_line(g, "return 0;");
        }
        g->indent--;
        cg_str(g, "}\n\n");

        /* db.close(conn) */
        cg_fmt(g, "static void lcn_driver_%s_close(void *conn) {\n", alias);
        g->indent++;
        if (g->use_runtime_header) {
            cg_line(g, "lcn_pg_close((LcnPgConn *)conn);");
        } else {
            cg_line(g, "(void)conn;");
            cg_fmt(g, "printf(\"[driver:%s] pg_close()\\n\");\n", alias);
        }
        g->indent--;
        cg_str(g, "}\n\n");

        /* db.row_count(result) → int */
        cg_fmt(g, "static int64_t lcn_driver_%s_row_count(void *result) {\n", alias);
        g->indent++;
        if (g->use_runtime_header) {
            cg_line(g, "return (int64_t)lcn_pg_row_count((LcnPgResult *)result);");
        } else {
            cg_line(g, "(void)result; return 0;");
        }
        g->indent--;
        cg_str(g, "}\n\n");

        /* db.get(result, row, col_name) → string */
        cg_fmt(g, "static LcnString lcn_driver_%s_get(void *result, int64_t row, LcnString col) {\n", alias);
        g->indent++;
        if (g->use_runtime_header) {
            cg_line(g, "return lcn_pg_row_get((LcnPgResult *)result, (int)row, col);");
        } else {
            cg_line(g, "(void)result; (void)row; (void)col; return \"\";");
        }
        g->indent--;
        cg_str(g, "}\n\n");

        /* db.get_number(result, row, col_name) → int64 */
        cg_fmt(g, "static int64_t lcn_driver_%s_get_number(void *result, int64_t row, LcnString col) {\n", alias);
        g->indent++;
        if (g->use_runtime_header) {
            cg_line(g, "return lcn_pg_row_get_number((LcnPgResult *)result, (int)row, col);");
        } else {
            cg_line(g, "(void)result; (void)row; (void)col; return 0;");
        }
        g->indent--;
        cg_str(g, "}\n\n");

        /* db.free(result) */
        cg_fmt(g, "static void lcn_driver_%s_free(void *result) {\n", alias);
        g->indent++;
        if (g->use_runtime_header) {
            cg_line(g, "lcn_pg_result_free((LcnPgResult *)result);");
        } else {
            cg_line(g, "(void)result;");
        }
        g->indent--;
        cg_str(g, "}\n\n");

        /* db.escape(conn, str) → string (malloc'd, caller frees) */
        cg_fmt(g, "static LcnString lcn_driver_%s_escape(void *conn, LcnString str) {\n", alias);
        g->indent++;
        if (g->use_runtime_header) {
            cg_line(g, "return lcn_pg_escape((LcnPgConn *)conn, str);");
        } else {
            cg_line(g, "(void)conn; (void)str; return \"\";");
        }
        g->indent--;
        cg_str(g, "}\n\n");
    } else {
        cg_fmt(g, "/* Unknown driver: %s — no codegen */\n\n",
               driver_name ? driver_name : "?");
    }
}

/* --- model import (ONNX local inference) --- */
static void cg_use_model(CodeGen *g, AstNode *use) {
    const char *model_path = use->val.str_val;
    const char *alias = (use->right && use->right->name) ? use->right->name : "model";

    /* Register alias for method call rewriting */
    {
        bool already = false;
        int ai;
        for (ai = 0; ai < g->model_alias_count; ai++) {
            if (strcmp(g->model_aliases[ai].alias, alias) == 0) { already = true; break; }
        }
        if (!already && g->model_alias_count < 32) {
            g->model_aliases[g->model_alias_count].alias = alias;
            g->model_alias_count++;
        }
    }

    cg_fmt(g, "/* Model import: %s as %s */\n", model_path ? model_path : "?", alias);

    /* Generate: static LcnModel *_model_ALIAS = NULL; */
    cg_fmt(g, "static LcnModel *_model_%s = NULL;\n\n", alias);

    /* Generate init function */
    cg_fmt(g, "static void lcn_model_%s_init(void) {\n", alias);
    g->indent++;
    cg_fmt(g, "if (!_model_%s) {\n", alias);
    g->indent++;
    if (g->use_runtime_header) {
        cg_str(g, "{ const char *_mp = getenv(\"MODEL_PATH\");\n");
        cg_str(g, "  const char *_lm = NULL;\n");
        cg_str(g, "  if (_mp) {\n");
        cg_str(g, "    char _lm_buf[1024]; const char *_sl = strrchr(_mp, '/');\n");
        cg_str(g, "    if (_sl) { int _n = (int)(_sl - _mp); snprintf(_lm_buf, sizeof(_lm_buf), \"%.*s/labels.txt\", _n, _mp); _lm = _lm_buf; }\n");
        cg_fmt(g, "    _model_%s = lcn_model_load(_mp, _lm);\n", alias);
        cg_str(g, "  } else {\n");
        cg_fmt(g, "    _model_%s = lcn_model_load(\"%s\", NULL);\n", alias, model_path ? model_path : "");
        cg_str(g, "  } }\n");
    } else {
        cg_fmt(g, "printf(\"[model:%s] load: %s\\n\");\n", alias, model_path ? model_path : "");
    }
    g->indent--;
    cg_str(g, "}\n");
    g->indent--;
    cg_str(g, "}\n\n");

    /* Generate predict wrapper */
    cg_fmt(g, "static LcnModelResult lcn_model_%s_predict(LcnString text) {\n", alias);
    g->indent++;
    cg_fmt(g, "lcn_model_%s_init();\n", alias);
    if (g->use_runtime_header) {
        cg_fmt(g, "return lcn_model_predict(_model_%s, text);\n", alias);
    } else {
        cg_str(g, "LcnModelResult _r = {0}; _r.ok = true; _r.label = text; _r.confidence = 1.0; return _r;\n");
    }
    g->indent--;
    cg_str(g, "}\n\n");

    /* Generate info wrapper */
    cg_fmt(g, "static LcnString lcn_model_%s_info(void) {\n", alias);
    g->indent++;
    cg_fmt(g, "lcn_model_%s_init();\n", alias);
    if (g->use_runtime_header) {
        cg_fmt(g, "return lcn_model_info(_model_%s);\n", alias);
    } else {
        cg_fmt(g, "return \"%s\";\n", model_path ? model_path : "unknown");
    }
    g->indent--;
    cg_str(g, "}\n\n");
}

/* --- mesh helper: check if mesh uses route syntax --- */
static bool mesh_has_routes(AstNode *mesh) {
    for (AstNode *m = mesh->params; m; m = m->next) {
        if (m->kind == AST_MESH_ROUTE) return true;
    }
    return false;
}

/* --- mesh agent wrapper: generates a LcnString->LcnString shim --- */
static void cg_mesh_agent_wrapper(CodeGen *g, const char *mesh_name, const char *agent_name) {
    cg_fmt(g, "static LcnString lcn_mesh_%s_agent_%s(LcnString _input) {\n",
            mesh_name, agent_name);
    g->indent++;
    cg_line(g, "Agent_%s _agent = lcn_agent_%s_new();", agent_name, agent_name);
    cg_line(g, "LcnLlmOutput _out = lcn_ask_typed(NULL, _agent.model, _agent.prompt, _input, &_agent.budget, NULL, _agent.api_key);");
    cg_line(g, "if (_out.kind == LCN_LLM_OUTPUT_ERROR) {");
    g->indent++;
    cg_line(g, "fprintf(stderr, \"[mesh:%s] agent '%s' failed: %%s\\n\", _out.error ? _out.error : \"unknown\");",
            mesh_name, agent_name);
    cg_line(g, "return \"\";");
    g->indent--;
    cg_line(g, "}");
    cg_line(g, "return lcn_llm_output_unwrap(_out);");
    g->indent--;
    cg_str(g, "}\n\n");
}

/* --- mesh: collect unique agent names (skip "input"/"output" pseudo-nodes) --- */
typedef struct { const char *names[128]; int count; } MeshAgentList;

static void mesh_agent_list_add(MeshAgentList *list, const char *name) {
    if (!name) return;
    if (strcmp(name, "input") == 0 || strcmp(name, "output") == 0) return;
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->names[i], name) == 0) return;
    }
    if (list->count < 128) list->names[list->count++] = name;
}

static void mesh_collect_agents(AstNode *mesh, MeshAgentList *list) {
    list->count = 0;
    for (AstNode *m = mesh->params; m; m = m->next) {
        if (m->kind == AST_MESH_ROUTE) {
            for (AstNode *n = m->left; n; n = n->next) mesh_agent_list_add(list, n->name);
            for (AstNode *n = m->right; n; n = n->next) mesh_agent_list_add(list, n->name);
        }
    }
}

/* Helper: is this a pseudo-node (input/output)? */
static bool mesh_is_pseudo(const char *name) {
    return name && (strcmp(name, "input") == 0 || strcmp(name, "output") == 0);
}

/* Helper: emit a function pointer expression for an agent (or NULL for pseudo) */
static void cg_mesh_fn_ptr(CodeGen *g, const char *mesh_name, const char *agent_name) {
    if (mesh_is_pseudo(agent_name))
        cg_str(g, "NULL");
    else
        cg_fmt(g, "lcn_mesh_%s_agent_%s", mesh_name, agent_name);
}

/* --- mesh (pipeline — sequential or parallel) --- */
static void cg_mesh(CodeGen *g, AstNode *mesh) {
    const char *name = mesh->name;
    bool has_routes = mesh_has_routes(mesh);

    if (has_routes) {
        /* Route-based mesh (parallel fan-out/fan-in) */

        /* Step 1: emit wrapper functions for each unique agent */
        MeshAgentList agents;
        mesh_collect_agents(mesh, &agents);
        for (int i = 0; i < agents.count; i++) {
            cg_mesh_agent_wrapper(g, name, agents.names[i]);
        }

        /* Step 2: emit the mesh run function */
        cg_fmt(g, "/* mesh %s — parallel pipeline */\n", name);
        cg_fmt(g, "static LcnResult lcn_mesh_%s_run(LcnString input) {\n", name);
        g->indent++;
        cg_line(g, "LcnMesh *_mesh = lcn_mesh_new(\"%s\");", name);
        cg_nl(g);

        /* Step 3: emit route registrations */
        for (AstNode *m = mesh->params; m; m = m->next) {
            if (m->kind != AST_MESH_ROUTE) continue;

            bool src_list = m->is_mut;
            bool tgt_list = m->is_unsafe;
            int src_count = ast_list_len(m->left);
            int tgt_count = ast_list_len(m->right);

            if (!src_list && !tgt_list) {
                /* Single: A -> B */
                const char *sn = m->left ? m->left->name : "input";
                const char *tn = m->right ? m->right->name : "output";
                cg_line(g, "/* route %s -> %s */", sn, tn);
                cg_indent(g);
                cg_str(g, "lcn_mesh_add_route_single(_mesh, ");
                cg_fmt(g, "\"%s\", ", sn);
                cg_mesh_fn_ptr(g, name, sn);
                cg_fmt(g, ", \"%s\", ", tn);
                cg_mesh_fn_ptr(g, name, tn);
                cg_str(g, ");\n");

            } else if (!src_list && tgt_list) {
                /* Fan-out: A -> [B, C, D] */
                const char *sn = m->left ? m->left->name : "input";
                cg_line(g, "/* route %s -> [%d targets] (fan-out) */", sn, tgt_count);
                cg_line(g, "{");
                g->indent++;

                cg_indent(g);
                cg_fmt(g, "const char *_fo_names[] = {");
                for (AstNode *t = m->right; t; t = t->next) {
                    cg_fmt(g, "\"%s\"", t->name);
                    if (t->next) cg_str(g, ", ");
                }
                cg_str(g, "};\n");

                cg_indent(g);
                cg_str(g, "LcnMeshAgentFn _fo_fns[] = {");
                for (AstNode *t = m->right; t; t = t->next) {
                    cg_mesh_fn_ptr(g, name, t->name);
                    if (t->next) cg_str(g, ", ");
                }
                cg_str(g, "};\n");

                cg_indent(g);
                cg_fmt(g, "lcn_mesh_add_route_fan_out(_mesh, \"%s\", ", sn);
                cg_mesh_fn_ptr(g, name, sn);
                cg_fmt(g, ", _fo_names, _fo_fns, %d);\n", tgt_count);

                g->indent--;
                cg_line(g, "}");

            } else if (src_list && !tgt_list) {
                /* Fan-in: [A, B, C] -> D */
                const char *tn = m->right ? m->right->name : "output";
                cg_line(g, "/* route [%d sources] -> %s (fan-in) */", src_count, tn);
                cg_line(g, "{");
                g->indent++;

                cg_indent(g);
                cg_str(g, "const char *_fi_names[] = {");
                for (AstNode *s = m->left; s; s = s->next) {
                    cg_fmt(g, "\"%s\"", s->name);
                    if (s->next) cg_str(g, ", ");
                }
                cg_str(g, "};\n");

                cg_indent(g);
                cg_str(g, "LcnMeshAgentFn _fi_fns[] = {");
                for (AstNode *s = m->left; s; s = s->next) {
                    cg_mesh_fn_ptr(g, name, s->name);
                    if (s->next) cg_str(g, ", ");
                }
                cg_str(g, "};\n");

                cg_indent(g);
                cg_fmt(g, "lcn_mesh_add_route_fan_in(_mesh, _fi_names, _fi_fns, %d, ", src_count);
                cg_fmt(g, "\"%s\", ", tn);
                cg_mesh_fn_ptr(g, name, tn);
                cg_str(g, ");\n");

                g->indent--;
                cg_line(g, "}");
            }
            cg_nl(g);
        }

        /* Step 4: execute and return */
        cg_line(g, "LcnString _mesh_output = NULL;");
        cg_line(g, "LcnResult _mesh_result = lcn_mesh_execute(_mesh, input, &_mesh_output);");
        cg_line(g, "lcn_mesh_free(_mesh);");
        cg_line(g, "return _mesh_result;");
        g->indent--;
        cg_str(g, "}\n\n");

    } else {
        /* Legacy stage-based mesh (sequential) */
        cg_fmt(g, "/* mesh %s — sequential pipeline */\n", name);
        cg_fmt(g, "static LcnResult lcn_mesh_%s_run(LcnString input) {\n", name);
        g->indent++;
        cg_line(g, "LcnString _stage_input = input;");
        cg_line(g, "LcnLlmOutput _stage_out;");
        cg_nl(g);

        AstNode *m = mesh->params;
        while (m) {
            if (m->kind == AST_MESH_STAGE && m->name) {
                cg_line(g, "/* stage \"%s\" -> %s */",
                        m->val.str_val ? m->val.str_val : "?", m->name);
                cg_line(g, "{");
                g->indent++;
                cg_line(g, "Agent_%s _agent = lcn_agent_%s_new();", m->name, m->name);
                cg_line(g, "_stage_out = lcn_ask_typed(NULL, _agent.model, _agent.prompt, _stage_input, &_agent.budget, NULL, _agent.api_key);");
                cg_line(g, "if (_stage_out.kind == LCN_LLM_OUTPUT_ERROR) {");
                g->indent++;
                cg_line(g, "fprintf(stderr, \"mesh stage '%s' failed: %%s\\n\", _stage_out.error ? _stage_out.error : \"unknown\");",
                        m->val.str_val ? m->val.str_val : "?");
                cg_line(g, "return LCN_ERR(_stage_out.error ? _stage_out.error : \"mesh stage failed\");");
                g->indent--;
                cg_line(g, "}");
                cg_line(g, "_stage_input = lcn_llm_output_unwrap(_stage_out);");
                cg_line(g, "fprintf(stderr, \"[mesh] stage '%s' -> %%zu bytes\\n\", strlen(_stage_input));",
                        m->val.str_val ? m->val.str_val : "?");
                g->indent--;
                cg_line(g, "}");
                cg_nl(g);
            }
            m = m->next;
        }

        cg_line(g, "return LCN_OK;");
        g->indent--;
        cg_str(g, "}\n\n");
    }
}

/* --- router (model selector) --- */
static void cg_router(CodeGen *g, AstNode *router) {
    const char *name = router->name;
    const char *strategy_str = NULL;
    const char *fallback_str = NULL;
    int64_t health_interval = 0;
    AstNode *m;

    /* Pre-scan for strategy, fallback, and health_check_interval */
    m = router->params;
    while (m) {
        if (m->kind == AST_FIELD && m->name) {
            if (strcmp(m->name, "strategy") == 0 &&
                m->right && m->right->kind == AST_IDENT && m->right->name) {
                strategy_str = m->right->name;
            }
            if (strcmp(m->name, "fallback") == 0 &&
                m->right && m->right->kind == AST_STRING_LIT) {
                fallback_str = m->right->val.str_val;
            }
            if (strcmp(m->name, "health_check_interval") == 0 &&
                m->right && m->right->kind == AST_INT_LIT) {
                health_interval = m->right->val.int_val;
            }
        }
        m = m->next;
    }

    /* Legacy struct type (backward compatible) */
    cg_fmt(g, "/* router %s */\n", name);
    cg_fmt(g, "typedef struct {\n");
    cg_str(g, "    LcnString strategy;\n");
    cg_str(g, "    LcnString fallback;\n");
    cg_fmt(g, "} Router_%s;\n\n", name);

    /* Legacy tier-based select function */
    cg_fmt(g, "static LcnString lcn_router_%s_select(Router_%s *self, LcnString tier) {\n", name, name);
    g->indent++;
    cg_line(g, "(void)self;");

    m = router->params;
    {
        bool first = true;
        while (m) {
            if (m->kind == AST_ROUTE_RULE && m->val.str_val) {
                cg_indent(g);
                if (!first) cg_str(g, "else ");
                cg_fmt(g, "if (strcmp(tier, \"%s\") == 0) return ", m->val.str_val);
                if (m->params && m->params->name)
                    cg_fmt(g, "\"%s\"", m->params->name);
                else
                    cg_str(g, "\"\"");
                cg_str(g, ";\n");
                first = false;
            }
            m = m->next;
        }
    }

    if (fallback_str) {
        cg_line(g, "return \"%s\"; /* fallback */", fallback_str);
    } else {
        cg_line(g, "return \"\"; /* no matching tier */");
    }
    g->indent--;
    cg_str(g, "}\n\n");

    /* Legacy constructor */
    cg_fmt(g, "static Router_%s lcn_router_%s_new(void) {\n", name, name);
    cg_fmt(g, "    return (Router_%s){ .strategy = \"%s\", .fallback = \"%s\" };\n",
           name, strategy_str ? strategy_str : "", fallback_str ? fallback_str : "");
    cg_str(g, "}\n\n");

    /* --- Enhanced runtime router with health checking --- */
    if (g->use_runtime_header) {
        cg_fmt(g, "static LcnRouter *_lcn_rt_router_%s = NULL;\n\n", name);

        cg_fmt(g, "static void lcn_router_%s_init(void) {\n", name);
        g->indent++;
        cg_fmt(g, "_lcn_rt_router_%s = lcn_router_new(\"%s\");\n", name, name);

        if (strategy_str) {
            if (strcmp(strategy_str, "cost_aware") == 0 || strcmp(strategy_str, "cost") == 0) {
                cg_fmt(g, "lcn_router_set_strategy(_lcn_rt_router_%s, LCN_ROUTE_STRATEGY_COST);\n", name);
            } else if (strcmp(strategy_str, "round_robin") == 0) {
                cg_fmt(g, "lcn_router_set_strategy(_lcn_rt_router_%s, LCN_ROUTE_STRATEGY_ROUND_ROBIN);\n", name);
            } else if (strcmp(strategy_str, "failover") == 0) {
                cg_fmt(g, "lcn_router_set_strategy(_lcn_rt_router_%s, LCN_ROUTE_STRATEGY_FAILOVER);\n", name);
            } else {
                cg_fmt(g, "lcn_router_set_strategy(_lcn_rt_router_%s, LCN_ROUTE_STRATEGY_LATENCY);\n", name);
            }
        }

        if (health_interval > 0) {
            cg_fmt(g, "_lcn_rt_router_%s->health_check_interval_ms = %lld;\n",
                   name, (long long)health_interval);
        }

        m = router->params;
        while (m) {
            if (m->kind == AST_ROUTE_RULE && m->val.str_val) {
                AstNode *model = m->params;
                while (model) {
                    if (model->name) {
                        cg_fmt(g, "lcn_router_add_endpoint(_lcn_rt_router_%s, \"%s\", \"%s\", 0.0);\n",
                               name, model->name, model->name);
                    }
                    model = model->next;
                }
            }
            m = m->next;
        }

        g->indent--;
        cg_str(g, "}\n\n");

        cg_fmt(g, "static void lcn_router_%s_health_check(void) {\n", name);
        g->indent++;
        cg_fmt(g, "if (_lcn_rt_router_%s) lcn_router_health_check(_lcn_rt_router_%s);\n", name, name);
        g->indent--;
        cg_str(g, "}\n\n");

        cg_fmt(g, "static LcnString lcn_router_%s_select_endpoint(void) {\n", name);
        g->indent++;
        cg_fmt(g, "if (!_lcn_rt_router_%s) lcn_router_%s_init();\n", name, name);
        cg_fmt(g, "LcnRouterEndpoint *ep = lcn_router_select(_lcn_rt_router_%s);\n", name);
        cg_line(g, "if (ep) return ep->name;");
        if (fallback_str) {
            cg_line(g, "return \"%s\"; /* fallback */", fallback_str);
        } else {
            cg_line(g, "return \"\"; /* no healthy endpoint */");
        }
        g->indent--;
        cg_str(g, "}\n\n");

        cg_fmt(g, "static void lcn_router_%s_report(int idx, bool success, int64_t latency_ms) {\n", name);
        g->indent++;
        cg_fmt(g, "if (_lcn_rt_router_%s) lcn_router_report_result(_lcn_rt_router_%s, idx, success, latency_ms);\n", name, name);
        g->indent--;
        cg_str(g, "}\n\n");

        cg_fmt(g, "static void lcn_router_%s_cleanup(void) {\n", name);
        g->indent++;
        cg_fmt(g, "if (_lcn_rt_router_%s) { lcn_router_free(_lcn_rt_router_%s); _lcn_rt_router_%s = NULL; }\n", name, name, name);
        g->indent--;
        cg_str(g, "}\n\n");
    }
}

/* ============================================================
 * Progress Reporting
 * ============================================================ */

static void cg_progress(CodeGen *g, AstNode *node) {
    if (!node || node->kind != AST_PROGRESS) return;

    g->has_progress = true;

    /* Extract variable names from total and current expressions */
    if (node->left && node->left->kind == AST_IDENT && node->left->name)
        g->progress_total_var = node->left->name;
    if (node->right && node->right->kind == AST_IDENT && node->right->name)
        g->progress_current_var = node->right->name;

    cg_str(g, "/* ════════════════════════════════════════════════\n"
              " * Progress Reporting\n"
              " * ════════════════════════════════════════════════ */\n\n");

    cg_str(g,
        "static int64_t _lcn_progress_total = 0;\n"
        "static int64_t _lcn_progress_current = 0;\n"
        "static const char *_lcn_progress_file = NULL;\n\n"

        "static void _lcn_progress_init(void) {\n"
        "    _lcn_progress_file = getenv(\"LCN_PROGRESS_FILE\");\n"
        "    if (!_lcn_progress_file) _lcn_progress_file = \"/tmp/lcn_progress.json\";\n"
        "}\n\n"

        "static void _lcn_progress_update(int64_t current, int64_t total) {\n"
        "    _lcn_progress_current = current;\n"
        "    _lcn_progress_total = total;\n"
        "\n"
        "    FILE *f = fopen(_lcn_progress_file, \"w\");\n"
        "    if (f) {\n"
        "        int pct = total > 0 ? (int)(current * 100 / total) : 0;\n"
        "        fprintf(f, \"{\\\"current\\\":%lld,\\\"total\\\":%lld,\\\"percent\\\":%d}\\n\",\n"
        "                (long long)current, (long long)total, pct);\n"
        "        fclose(f);\n"
        "    }\n"
        "\n"
        "    int pct = total > 0 ? (int)(current * 100 / total) : 0;\n"
        "    if (current % 100 == 0 || current == total) {\n"
        "        fprintf(stderr, \"[progress] %lld/%lld (%d%%)\\n\",\n"
        "                (long long)current, (long long)total, pct);\n"
        "    }\n"
        "}\n\n"
    );
}

/* ============================================================
 * Prometheus Metrics Code Generation
 * ============================================================ */

static void cg_metrics(CodeGen *g, AstNode *metrics) {
    int port = (int)metrics->val.int_val;
    g->has_metrics = true;
    g->metrics_port = port;

    /* Collect metric fields into registry */
    AstNode *f = metrics->params;
    while (f && g->metrics_count < 64) {
        if (f->kind == AST_METRICS_FIELD && f->name) {
            int idx = g->metrics_count;
            g->metrics[idx].name = f->name;
            g->metrics[idx].description = f->val.str_val;
            if (f->is_pub)       g->metrics[idx].kind = 2; /* histogram */
            else if (f->is_mut)  g->metrics[idx].kind = 1; /* gauge */
            else                 g->metrics[idx].kind = 0; /* counter */
            g->metrics_count++;
        }
        f = f->next;
    }

    /* Emit the metrics struct */
    cg_str(g, "/* Prometheus-compatible metrics */\n");
    cg_str(g, "typedef struct {\n");
    for (int i = 0; i < g->metrics_count; i++) {
        if (g->metrics[i].kind == 2) {
            cg_fmt(g, "    struct {\n");
            cg_fmt(g, "        int64_t count;\n");
            cg_fmt(g, "        double sum;\n");
            cg_fmt(g, "        int64_t buckets[11];\n");
            cg_fmt(g, "    } %s;\n", g->metrics[i].name);
        } else {
            cg_fmt(g, "    int64_t %s;\n", g->metrics[i].name);
        }
    }
    cg_str(g, "} LcnMetrics;\n\n");

    cg_str(g, "static LcnMetrics _lcn_metrics = {0};\n");
    cg_str(g, "static pthread_mutex_t _lcn_metrics_lock = PTHREAD_MUTEX_INITIALIZER;\n\n");

    /* Emit histogram observe helper */
    for (int i = 0; i < g->metrics_count; i++) {
        if (g->metrics[i].kind == 2) {
            cg_fmt(g, "static void _lcn_metrics_observe_%s(double val) {\n", g->metrics[i].name);
            cg_str(g, "    static const double bounds[11] = {10,20,30,40,50,60,70,80,90,95,100};\n");
            cg_str(g, "    pthread_mutex_lock(&_lcn_metrics_lock);\n");
            cg_fmt(g, "    _lcn_metrics.%s.count++;\n", g->metrics[i].name);
            cg_fmt(g, "    _lcn_metrics.%s.sum += val;\n", g->metrics[i].name);
            cg_str(g, "    for (int _i = 0; _i < 11; _i++) {\n");
            cg_fmt(g, "        if (val <= bounds[_i]) _lcn_metrics.%s.buckets[_i]++;\n", g->metrics[i].name);
            cg_str(g, "    }\n");
            cg_str(g, "    pthread_mutex_unlock(&_lcn_metrics_lock);\n");
            cg_str(g, "}\n\n");
        }
    }

    /* Emit /metrics endpoint handler (Prometheus text format) */
    cg_str(g, "static void _lcn_metrics_format(char *buf, size_t bufsz) {\n");
    cg_str(g, "    int _off = 0;\n");
    cg_str(g, "    pthread_mutex_lock(&_lcn_metrics_lock);\n");
    for (int i = 0; i < g->metrics_count; i++) {
        const char *name = g->metrics[i].name;
        const char *desc = g->metrics[i].description ? g->metrics[i].description : "";
        int kind = g->metrics[i].kind;
        const char *type_str = kind == 0 ? "counter" : kind == 1 ? "gauge" : "histogram";

        cg_fmt(g, "    _off += snprintf(buf + _off, bufsz - _off, \"# HELP %s %s\\n\");\n", name, desc);
        cg_fmt(g, "    _off += snprintf(buf + _off, bufsz - _off, \"# TYPE %s %s\\n\");\n", name, type_str);

        if (kind == 2) {
            cg_str(g, "    {\n");
            cg_str(g, "        static const char *le[11] = {\"10\",\"20\",\"30\",\"40\",\"50\",\"60\",\"70\",\"80\",\"90\",\"95\",\"100\"};\n");
            cg_str(g, "        int64_t _cum = 0;\n");
            cg_str(g, "        for (int _i = 0; _i < 11; _i++) {\n");
            cg_fmt(g, "            _cum += _lcn_metrics.%s.buckets[_i];\n", name);
            cg_fmt(g, "            _off += snprintf(buf + _off, bufsz - _off, \"%s_bucket{le=\\\"%%s\\\"} %%lld\\n\", le[_i], (long long)_cum);\n", name);
            cg_str(g, "        }\n");
            cg_fmt(g, "        _off += snprintf(buf + _off, bufsz - _off, \"%s_bucket{le=\\\"+Inf\\\"} %%lld\\n\", (long long)_lcn_metrics.%s.count);\n", name, name);
            cg_fmt(g, "        _off += snprintf(buf + _off, bufsz - _off, \"%s_sum %%f\\n\", _lcn_metrics.%s.sum);\n", name, name);
            cg_fmt(g, "        _off += snprintf(buf + _off, bufsz - _off, \"%s_count %%lld\\n\", (long long)_lcn_metrics.%s.count);\n", name, name);
            cg_str(g, "    }\n");
        } else {
            cg_fmt(g, "    _off += snprintf(buf + _off, bufsz - _off, \"%s %%lld\\n\", (long long)_lcn_metrics.%s);\n", name, name);
        }
    }
    cg_str(g, "    pthread_mutex_unlock(&_lcn_metrics_lock);\n");
    cg_str(g, "}\n\n");

    /* Emit the metrics HTTP server thread */
    cg_fmt(g, "static void *_lcn_metrics_server(void *arg) {\n");
    cg_str(g, "    (void)arg;\n");
    cg_str(g, "    int _srv = socket(AF_INET, SOCK_STREAM, 0);\n");
    cg_str(g, "    if (_srv < 0) return NULL;\n");
    cg_str(g, "    int _opt = 1;\n");
    cg_str(g, "    setsockopt(_srv, SOL_SOCKET, SO_REUSEADDR, &_opt, sizeof(_opt));\n");
    cg_str(g, "    struct sockaddr_in _addr;\n");
    cg_str(g, "    memset(&_addr, 0, sizeof(_addr));\n");
    cg_str(g, "    _addr.sin_family = AF_INET;\n");
    cg_str(g, "    _addr.sin_addr.s_addr = INADDR_ANY;\n");
    cg_fmt(g, "    _addr.sin_port = htons(%d);\n", port);
    cg_str(g, "    if (bind(_srv, (struct sockaddr *)&_addr, sizeof(_addr)) < 0) { close(_srv); return NULL; }\n");
    cg_str(g, "    listen(_srv, 4);\n");
    cg_fmt(g, "    fprintf(stderr, \"[limceron] Metrics: http://localhost:%d/metrics\\n\");\n", port);
    cg_str(g, "    while (1) {\n");
    cg_str(g, "        int _cl = accept(_srv, NULL, NULL);\n");
    cg_str(g, "        if (_cl < 0) continue;\n");
    cg_str(g, "        char _req[1024];\n");
    cg_str(g, "        read(_cl, _req, sizeof(_req) - 1);\n");
    cg_str(g, "        char _body[16384];\n");
    cg_str(g, "        _lcn_metrics_format(_body, sizeof(_body));\n");
    cg_str(g, "        char _hdr[256];\n");
    cg_str(g, "        int _hlen = snprintf(_hdr, sizeof(_hdr),\n");
    cg_str(g, "            \"HTTP/1.1 200 OK\\r\\nContent-Type: text/plain; version=0.0.4\\r\\n\"\n");
    cg_str(g, "            \"Content-Length: %zu\\r\\nConnection: close\\r\\n\\r\\n\", strlen(_body));\n");
    cg_str(g, "        write(_cl, _hdr, _hlen);\n");
    cg_str(g, "        write(_cl, _body, strlen(_body));\n");
    cg_str(g, "        close(_cl);\n");
    cg_str(g, "    }\n");
    cg_str(g, "    return NULL;\n");
    cg_str(g, "}\n\n");
}

/* Helper: map a Limceron type to C type name (for enum field codegen) */
static const char *cg_type_to_c(AstNode *type_expr) {
    static char _type_buf[128];
    if (!type_expr || type_expr->kind != AST_TYPE_NAMED || !type_expr->name)
        return "int64_t";
    const char *n = type_expr->name;
    if (strcmp(n, "int") == 0)    return "int64_t";
    if (strcmp(n, "i32") == 0)    return "int32_t";
    if (strcmp(n, "i64") == 0)    return "int64_t";
    if (strcmp(n, "u8") == 0)     return "uint8_t";
    if (strcmp(n, "u32") == 0)    return "uint32_t";
    if (strcmp(n, "u64") == 0)    return "uint64_t";
    if (strcmp(n, "f32") == 0)    return "float";
    if (strcmp(n, "f64") == 0)    return "double";
    if (strcmp(n, "float") == 0)  return "double";
    if (strcmp(n, "bool") == 0)   return "bool";
    if (strcmp(n, "string") == 0) return "LcnString";
    if (strcmp(n, "String") == 0) return "LcnString";
    if (strcmp(n, "Result") == 0) {
        if (type_expr->generics) {
            mono_result_name(_type_buf, sizeof(_type_buf), type_expr->generics);
            return _type_buf;
        }
        return "LcnResult";
    }
    if (strcmp(n, "Option") == 0) {
        if (type_expr->generics) {
            mono_option_name(_type_buf, sizeof(_type_buf), type_expr->generics);
            return _type_buf;
        }
        return "LcnOption";
    }
    /* User-defined type — use as-is */
    return n;
}

/* --- enum (string constants + validation / ADT tagged unions) --- */
static void cg_enum(CodeGen *g, AstNode *en) {
    const char *name = en->name;

    /* Detect if any variant has data fields */
    bool has_data = false;
    AstNode *v = en->params;
    while (v) {
        if (v->kind == AST_VARIANT && v->params) { has_data = true; break; }
        v = v->next;
    }

    /* Register in the enum registry */
    cg_register_enum(g, name, has_data, en);

    if (!has_data) {
        /* Simple enum: emit string constants (existing behavior) */
        cg_fmt(g, "/* enum %s */\n", name);
        v = en->params;
        while (v) {
            if (v->kind == AST_VARIANT && v->name) {
                cg_fmt(g, "#define %s_%s \"%s\"\n", name, v->name, v->name);
            }
            v = v->next;
        }
        cg_nl(g);

        /* Emit validation function */
        cg_fmt(g, "static bool lcn_enum_%s_is_valid(LcnString val) {\n", name);
        g->indent++;
        cg_line(g, "if (!val) return false;");
        v = en->params;
        while (v) {
            if (v->kind == AST_VARIANT && v->name) {
                cg_line(g, "if (strcmp(val, \"%s\") == 0) return true;", v->name);
            }
            v = v->next;
        }
        cg_line(g, "return false;");
        g->indent--;
        cg_str(g, "}\n\n");
        return;
    }

    /* ADT enum: emit tagged union + constructors */
    cg_fmt(g, "/* ADT enum %s */\n", name);

    /* 1. Kind enum */
    cg_fmt(g, "typedef enum {\n");
    {
        int idx = 0;
        v = en->params;
        while (v) {
            if (v->kind == AST_VARIANT && v->name) {
                cg_fmt(g, "    %s_%s = %d,\n", name, v->name, idx++);
            }
            v = v->next;
        }
    }
    cg_fmt(g, "} %s_Kind;\n\n", name);

    /* 2. Tagged union struct (flat: fields prefixed with _VariantName_ to avoid collisions) */
    cg_fmt(g, "typedef struct {\n");
    cg_fmt(g, "    %s_Kind _kind;\n", name);
    v = en->params;
    while (v) {
        if (v->kind == AST_VARIANT && v->params) {
            AstNode *f = v->params;
            int fi = 0;
            while (f) {
                if (f->kind == AST_FIELD) {
                    const char *ctype = cg_type_to_c(f->type_expr);
                    if (f->name) {
                        cg_fmt(g, "    %s _%s_%s;\n", ctype, v->name, f->name);
                    } else {
                        cg_fmt(g, "    %s _%s_%d;\n", ctype, v->name, fi);
                    }
                    fi++;
                }
                f = f->next;
            }
        }
        v = v->next;
    }
    cg_fmt(g, "} %s;\n\n", name);

    /* 3. Constructor functions */
    v = en->params;
    while (v) {
        if (v->kind == AST_VARIANT && v->name) {
            if (v->params) {
                /* Constructor with fields */
                cg_fmt(g, "static %s %s_%s_new(", name, name, v->name);
                AstNode *f = v->params;
                bool first = true;
                int fi = 0;
                while (f) {
                    if (f->kind == AST_FIELD) {
                        if (!first) cg_str(g, ", ");
                        const char *ctype = cg_type_to_c(f->type_expr);
                        if (f->name) {
                            cg_fmt(g, "%s %s", ctype, f->name);
                        } else {
                            cg_fmt(g, "%s _arg%d", ctype, fi);
                        }
                        first = false;
                        fi++;
                    }
                    f = f->next;
                }
                cg_str(g, ") {\n");
                cg_fmt(g, "    %s _t = {0};\n", name);
                cg_fmt(g, "    _t._kind = %s_%s;\n", name, v->name);
                f = v->params;
                fi = 0;
                while (f) {
                    if (f->kind == AST_FIELD) {
                        if (f->name) {
                            cg_fmt(g, "    _t._%s_%s = %s;\n", v->name, f->name, f->name);
                        } else {
                            cg_fmt(g, "    _t._%s_%d = _arg%d;\n", v->name, fi, fi);
                        }
                        fi++;
                    }
                    f = f->next;
                }
                cg_str(g, "    return _t;\n}\n\n");
            } else {
                /* Constructor with no fields */
                cg_fmt(g, "static %s %s_%s_new(void) {\n", name, name, v->name);
                cg_fmt(g, "    %s _t = {0};\n", name);
                cg_fmt(g, "    _t._kind = %s_%s;\n", name, v->name);
                cg_str(g, "    return _t;\n}\n\n");
            }
        }
        v = v->next;
    }
}

/* --- invariant: wire body expression to entropy/drift runtime --- */

/* Helper: check if an AST call name matches a known invariant function */
static const char *cg_invariant_match_call(AstNode *call_node) {
    if (!call_node) return NULL;
    /* Direct call: name(...) */
    if (call_node->kind == AST_CALL && call_node->left &&
        call_node->left->kind == AST_IDENT && call_node->left->name) {
        return call_node->left->name;
    }
    return NULL;
}

/* Helper: emit a C expression for an invariant body sub-expression */
static void cg_invariant_call(CodeGen *g, AstNode *call_node) {
    const char *fn_name = cg_invariant_match_call(call_node);
    if (!fn_name) {
        /* Fallback: emit as-is using cg_expr */
        cg_expr(g, call_node);
        return;
    }

    /* Pattern-match known invariant functions to runtime calls:
     * drift(current, baseline)        → lcn_js_divergence(current, baseline, n)
     * avg_confidence(last_N)          → lcn_entropy_avg_confidence(tracker, N)
     * avg_entropy(last_N)             → lcn_entropy_avg_entropy(tracker, N)
     * low_confidence_pct(last_N, thr) → lcn_entropy_low_confidence_pct(tracker, N, thr)
     * check_budget(budget)            → lcn_entropy_check_budget(tracker, budget)
     */
    AstNode *args = call_node->params; /* first argument */

    if (strcmp(fn_name, "drift") == 0 || strstr(fn_name, "drift") != NULL) {
        /* drift(current_dist, baseline_dist) → lcn_js_divergence(current_dist, baseline_dist, _inv_n_cats) */
        cg_str(g, "lcn_js_divergence(");
        if (args) { cg_expr(g, args); args = args->next; }
        cg_str(g, ", ");
        if (args) { cg_expr(g, args); args = args->next; }
        cg_str(g, ", _inv_n_cats)");
    }
    else if (strcmp(fn_name, "avg_confidence") == 0 || strstr(fn_name, "avg_confidence") != NULL) {
        /* avg_confidence(last_N) → lcn_entropy_avg_confidence(tracker, N) */
        cg_str(g, "lcn_entropy_avg_confidence(tracker, ");
        if (args) cg_expr(g, args);
        else cg_str(g, "100");
        cg_str(g, ")");
    }
    else if (strcmp(fn_name, "avg_entropy") == 0 || strstr(fn_name, "avg_entropy") != NULL) {
        /* avg_entropy(last_N) → lcn_entropy_avg_entropy(tracker, N) */
        cg_str(g, "lcn_entropy_avg_entropy(tracker, ");
        if (args) cg_expr(g, args);
        else cg_str(g, "100");
        cg_str(g, ")");
    }
    else if (strcmp(fn_name, "low_confidence_pct") == 0) {
        /* low_confidence_pct(window, threshold) */
        cg_str(g, "lcn_entropy_low_confidence_pct(tracker, ");
        if (args) { cg_expr(g, args); args = args->next; }
        else cg_str(g, "100");
        cg_str(g, ", ");
        if (args) cg_expr(g, args);
        else cg_str(g, "0.5");
        cg_str(g, ")");
    }
    else if (strcmp(fn_name, "check_budget") == 0) {
        cg_str(g, "lcn_entropy_check_budget(tracker, ");
        if (args) cg_expr(g, args);
        else cg_str(g, "NULL");
        cg_str(g, ")");
    }
    else {
        /* Unknown function: emit as regular call */
        cg_expr(g, call_node);
    }
}

static void cg_invariant(CodeGen *g, AstNode *inv) {
    if (!inv || !inv->name) return;

    /* Register invariant name */
    if (g->invariant_count < 64)
        g->invariant_names[g->invariant_count++] = inv->name;

    cg_fmt(g, "/* invariant %s */\n", inv->name);

    /* If no body expression (old-style field invariant), emit comment only */
    if (!inv->left) {
        cg_fmt(g, "/* invariant %s — no body expression */\n\n", inv->name);
        return;
    }

    /* Generate: static bool lcn_invariant_NAME(LcnEntropyTracker *tracker) { ... } */
    cg_fmt(g, "static bool lcn_invariant_%s(LcnEntropyTracker *tracker) {\n", inv->name);
    g->indent++;

    /* Default number of categories (used by drift functions) */
    cg_indent(g);
    cg_str(g, "int _inv_n_cats = 16; /* default category count */\n");
    cg_indent(g);
    cg_str(g, "(void)_inv_n_cats;\n");

    /* Body is expected to be a comparison: call(...) < threshold or call(...) > threshold */
    AstNode *body = inv->left;
    if (body->kind == AST_BINARY &&
        (body->val.op == TOK_LT || body->val.op == TOK_GT ||
         body->val.op == TOK_LT_EQ || body->val.op == TOK_GT_EQ)) {

        /* Emit: double _val = <call>; */
        cg_indent(g);
        cg_str(g, "double _val = ");
        cg_invariant_call(g, body->left);
        cg_str(g, ";\n");

        /* Emit: return _val <op> <threshold>; */
        cg_indent(g);
        cg_str(g, "return _val ");
        switch (body->val.op) {
            case TOK_LT:    cg_str(g, "< ");  break;
            case TOK_GT:    cg_str(g, "> ");  break;
            case TOK_LT_EQ: cg_str(g, "<= "); break;
            case TOK_GT_EQ: cg_str(g, ">= "); break;
            default:        cg_str(g, "< ");  break;
        }
        cg_expr(g, body->right);
        cg_str(g, ";\n");
    } else {
        /* Generic fallback: emit the whole expression as a bool */
        cg_indent(g);
        cg_str(g, "return (bool)(");
        cg_expr(g, body);
        cg_str(g, ");\n");
    }

    g->indent--;
    cg_str(g, "}\n\n");
}

/* --- agent --- */
static void cg_agent(CodeGen *g, AstNode *agent) {
    const char *name = agent->name;
    bool has_prompt = false;
    bool has_llm_cap = false;
    bool has_run_method = false;
    bool has_memory = false;
    bool has_knowledge = false;
    bool has_entropy_budget = false;
    const char *kb_path = NULL;
    int kb_chunk_size = 500;
    int kb_chunk_overlap = 50;
    AstNode *f;
    AstNode *fn;

    /* Register agent name for constructor/method rewriting */
    cg_register_agent(g, name);

    /* Pre-scan: detect prompt, llm capability, memory, and run method */
    f = agent->params;
    while (f) {
        if (f->kind == AST_FIELD && f->name) {
            if (strcmp(f->name, "prompt") == 0 && f->right &&
                f->right->kind == AST_STRING_LIT && f->right->val.str_val)
                has_prompt = true;
            if (strcmp(f->name, "memory") == 0)
                has_memory = true;
            if (strcmp(f->name, "entropy_budget") == 0)
                has_entropy_budget = true;
            if (strcmp(f->name, "knowledge") == 0) {
                has_knowledge = true;
                /* Extract knowledge config fields */
                if (f->right && f->right->kind == AST_BLOCK) {
                    AstNode *kf = f->right->params;
                    while (kf) {
                        if (kf->name && kf->right) {
                            if (strcmp(kf->name, "path") == 0 &&
                                kf->right->kind == AST_STRING_LIT)
                                kb_path = kf->right->val.str_val;
                            else if (strcmp(kf->name, "chunk_size") == 0 &&
                                     kf->right->kind == AST_INT_LIT)
                                kb_chunk_size = (int)kf->right->val.int_val;
                            else if (strcmp(kf->name, "overlap") == 0 &&
                                     kf->right->kind == AST_INT_LIT)
                                kb_chunk_overlap = (int)kf->right->val.int_val;
                        }
                        kf = kf->next;
                    }
                }
            }
            if (strcmp(f->name, "capabilities") == 0 && f->right &&
                f->right->kind == AST_ARRAY) {
                AstNode *cap = f->right->params;
                while (cap) {
                    if (cap->name && strcmp(cap->name, "llm.complete") == 0)
                        has_llm_cap = true;
                    cap = cap->next;
                }
            }
        }
        f = f->next;
    }

    /* Track memory-enabled agents */
    if (has_memory && g->memory_agent_count < 64) {
        g->memory_agents[g->memory_agent_count++] = name;
    }

    /* Track knowledge base config */
    if (has_knowledge && !g->has_kb) {
        g->has_kb = true;
        g->kb_path = kb_path;
        g->kb_chunk_size = kb_chunk_size;
        g->kb_chunk_overlap = kb_chunk_overlap;
    }
    fn = agent->left;
    while (fn) {
        if (fn->kind == AST_FN && fn->name && strcmp(fn->name, "run") == 0)
            has_run_method = true;
        fn = fn->next;
    }

    /* 1. Agent struct */
    cg_fmt(g, "typedef struct {\n");
    cg_str(g, "    LcnCapability capabilities;\n");
    cg_str(g, "    LcnString model;\n");
    cg_str(g, "    LcnBudget budget;\n");
    if (has_prompt)
        cg_str(g, "    LcnString prompt;\n");
    if (has_memory)
        cg_str(g, "    bool memory_enabled;\n");
    cg_str(g, "    LcnString endpoint;\n");
    cg_str(g, "    LcnString api_key;\n");
    if (has_entropy_budget && g->use_runtime_header) {
        cg_str(g, "    LcnEntropyBudget entropy_budget;\n");
        cg_str(g, "    LcnEntropyTracker *_entropy_tracker;\n");
    }

    /* Extra fields from agent body */
    f = agent->params;
    while (f) {
        if (f->kind == AST_FIELD && f->name) {
            /* Skip built-in fields we already emit */
            if (strcmp(f->name, "capabilities") == 0) { f = f->next; continue; }
            if (strcmp(f->name, "model") == 0) { f = f->next; continue; }
            if (strcmp(f->name, "budget") == 0) { f = f->next; continue; }
            if (strcmp(f->name, "prompt") == 0) { f = f->next; continue; }
            if (strcmp(f->name, "memory") == 0) { f = f->next; continue; }
            if (strcmp(f->name, "knowledge") == 0) { f = f->next; continue; }
            if (strcmp(f->name, "guards") == 0) { f = f->next; continue; }
            if (strcmp(f->name, "endpoint") == 0) { f = f->next; continue; }
            if (strcmp(f->name, "api_key") == 0) { f = f->next; continue; }
            if (strcmp(f->name, "entropy_budget") == 0) { f = f->next; continue; }
            /* Custom field */
            cg_str(g, "    ");
            if (f->type_expr) cg_type(g, f->type_expr);
            else cg_str(g, "LcnString");
            cg_fmt(g, " %s;\n", f->name);
        }
        f = f->next;
    }
    cg_fmt(g, "} Agent_%s;\n\n", name);

    /* 2. Constructor */
    cg_fmt(g, "static Agent_%s lcn_agent_%s_new(void) {\n", name, name);
    cg_fmt(g, "    return (Agent_%s){\n", name);

    f = agent->params;
    while (f) {
        if (f->kind == AST_FIELD && f->name && f->right) {
            if (strcmp(f->name, "capabilities") == 0 && f->right->kind == AST_ARRAY) {
                /* Combine capability flags */
                cg_str(g, "        .capabilities = ");
                AstNode *cap = f->right->params;
                int first = 1;
                while (cap) {
                    if (!first) cg_str(g, " | ");
                    first = 0;
                    if (cap->name) {
                        const char *def = cg_lookup_cap(g, cap->name);
                        cg_str(g, def ? def : "0");
                    }
                    cap = cap->next;
                }
                if (first) cg_str(g, "0");
                cg_str(g, ",\n");
            } else if (strcmp(f->name, "model") == 0) {
                cg_str(g, "        .model = ");
                cg_expr(g, f->right);
                cg_str(g, ",\n");
            } else if (strcmp(f->name, "prompt") == 0) {
                cg_str(g, "        .prompt = ");
                cg_expr(g, f->right);
                cg_str(g, ",\n");
            } else if (strcmp(f->name, "memory") == 0) {
                cg_str(g, "        .memory_enabled = true,\n");
            } else if (strcmp(f->name, "endpoint") == 0) {
                cg_str(g, "        .endpoint = ");
                cg_expr(g, f->right);
                cg_str(g, ",\n");
            } else if (strcmp(f->name, "api_key") == 0) {
                cg_str(g, "        .api_key = ");
                cg_expr(g, f->right);
                cg_str(g, ",\n");
            } else if (strcmp(f->name, "knowledge") == 0) {
                /* knowledge is handled in main(), not the agent struct */
            } else if (strcmp(f->name, "guards") == 0) {
                /* Guards are enforced at the guard-function level, not in the struct */
            } else if (strcmp(f->name, "budget") == 0) {
                cg_str(g, "        .budget = ");
                if (f->right->kind == AST_BLOCK && g->use_runtime_header) {
                    /* Build mode: use lcn_budget_new() for proper init */
                    int64_t b_tokens = 0;
                    double b_cost = 0.0;
                    int64_t b_duration = 0;
                    AstNode *bf = f->right->params;
                    while (bf) {
                        if (bf->name && bf->right) {
                            if (strcmp(bf->name, "max_tokens") == 0 &&
                                bf->right->kind == AST_INT_LIT)
                                b_tokens = bf->right->val.int_val;
                            else if (strcmp(bf->name, "max_cost") == 0) {
                                if (bf->right->kind == AST_INT_LIT)
                                    b_cost = (double)bf->right->val.int_val;
                                else if (bf->right->kind == AST_FLOAT_LIT)
                                    b_cost = bf->right->val.float_val;
                            }
                            else if (strcmp(bf->name, "max_duration") == 0 &&
                                     bf->right->kind == AST_INT_LIT)
                                b_duration = bf->right->val.int_val;
                        }
                        bf = bf->next;
                    }
                    cg_fmt(g, "lcn_budget_new(%lld, %g, %lld),\n",
                           (long long)b_tokens, b_cost, (long long)b_duration);
                } else if (f->right->kind == AST_BLOCK) {
                    /* Standalone mode: inline struct literal */
                    cg_str(g, "(LcnBudget){ ");
                    AstNode *bf = f->right->params;
                    while (bf) {
                        if (bf->name && bf->right) {
                            if (strcmp(bf->name, "max_cost") == 0) {
                                cg_str(g, ".max_cost = ");
                                cg_expr(g, bf->right);
                            } else if (strcmp(bf->name, "max_tokens") == 0) {
                                cg_str(g, ".max_tokens = ");
                                cg_expr(g, bf->right);
                            } else if (strcmp(bf->name, "max_duration") == 0) {
                                cg_str(g, ".max_duration_secs = ");
                                cg_expr(g, bf->right);
                            }
                            if (bf->next) cg_str(g, ", ");
                        }
                        bf = bf->next;
                    }
                    cg_str(g, " },\n");
                } else {
                    cg_expr(g, f->right);
                    cg_str(g, ",\n");
                }
            } else if (strcmp(f->name, "guards") == 0) {
                /* Guards are enforced at the guard-function level, not in the struct */
            } else if (strcmp(f->name, "entropy_budget") == 0 && f->right && f->right->kind == AST_BLOCK && g->use_runtime_header) {
                /* Entropy budget: extract sub-fields into struct literal */
                cg_str(g, "        .entropy_budget = { ");
                AstNode *sub = f->right->params;
                bool eb_first = true;
                while (sub) {
                    if (sub->kind == AST_FIELD && sub->name && sub->right) {
                        if (!eb_first) cg_str(g, ", ");
                        cg_fmt(g, ".%s = ", sub->name);
                        cg_expr(g, sub->right);
                        eb_first = false;
                    }
                    sub = sub->next;
                }
                if (!eb_first) cg_str(g, ", ");
                cg_str(g, ".low_confidence_threshold = 0.5 },\n");
                cg_str(g, "        ._entropy_tracker = lcn_entropy_tracker_new(1000, 6),\n");
            } else if (strcmp(f->name, "entropy_budget") == 0) {
                /* entropy_budget in standalone mode — skip (no runtime) */
            } else {
                /* Custom field */
                cg_fmt(g, "        .%s = ", f->name);
                cg_expr(g, f->right);
                cg_str(g, ",\n");
            }
        }
        f = f->next;
    }

    cg_str(g, "    };\n}\n\n");

    /* 2b. Capability fence: emit allowed tool list for this agent.
     * Derived by matching agent's capabilities against each tool's requires. */
    {
        /* Collect agent capabilities */
        const char *agent_caps[128];
        int agent_cap_count = 0;
        AstNode *cf = agent->params;
        while (cf) {
            if (cf->kind == AST_FIELD && cf->name &&
                strcmp(cf->name, "capabilities") == 0 &&
                cf->right && cf->right->kind == AST_ARRAY) {
                AstNode *cap = cf->right->params;
                while (cap && agent_cap_count < 128) {
                    if (cap->name) {
                        agent_caps[agent_cap_count++] = cap->name;
                    }
                    cap = cap->next;
                }
                break;
            }
            cf = cf->next;
        }

        /* Find tools whose requires are a subset of agent's capabilities */
        const char *allowed_tools[256];
        int allowed_count = 0;
        int ti;
        for (ti = 0; ti < g->tool_count && allowed_count < 256; ti++) {
            if (g->tools[ti].requires_count == 0) {
                /* Tool with no requires — accessible to all agents */
                allowed_tools[allowed_count++] = g->tools[ti].name;
                continue;
            }
            /* Check all required capabilities are in agent's list */
            bool all_met = true;
            int ri;
            for (ri = 0; ri < g->tools[ti].requires_count; ri++) {
                bool found = false;
                int ci;
                for (ci = 0; ci < agent_cap_count; ci++) {
                    if (strcmp(g->tools[ti].requires[ri], agent_caps[ci]) == 0) {
                        found = true;
                        break;
                    }
                }
                if (!found) { all_met = false; break; }
            }
            if (all_met) {
                allowed_tools[allowed_count++] = g->tools[ti].name;
            }
        }

        /* Emit static tool list */
        cg_fmt(g, "/* capability fence: allowed tools for agent %s */\n", name);
        cg_fmt(g, "static const char *_agent_%s_tools[] = {", name);
        {
            int ai;
            for (ai = 0; ai < allowed_count; ai++) {
                if (ai > 0) cg_str(g, ",");
                cg_fmt(g, " \"%s\"", allowed_tools[ai]);
            }
        }
        if (allowed_count == 0) {
            cg_str(g, " NULL");
        }
        cg_str(g, " };\n");
        cg_fmt(g, "static const int _agent_%s_tool_count = %d;\n\n", name, allowed_count);

        /* Register in codegen state for ToolCall fence emission */
        if (g->agent_tool_list_count < 64) {
            int idx = g->agent_tool_list_count;
            g->agent_tool_lists[idx].agent_name = name;
            g->agent_tool_lists[idx].tool_name_count = allowed_count;
            int ai;
            for (ai = 0; ai < allowed_count && ai < 256; ai++) {
                g->agent_tool_lists[idx].tool_names[ai] = allowed_tools[ai];
            }
            g->agent_tool_list_count++;
        }
    }

    /* 3. Resolve access policy from "use" field */
    {
        const char *policy_name = NULL;
        AstNode *uf = agent->params;
        while (uf) {
            if (uf->kind == AST_FIELD && uf->name &&
                strcmp(uf->name, "use") == 0 &&
                uf->right && uf->right->kind == AST_ARRAY &&
                uf->right->params && uf->right->params->name) {
                policy_name = uf->right->params->name;
                break;
            }
            uf = uf->next;
        }
        g->current_access_policy = policy_name;
    }

    /* 3b. Existing agent methods */
    fn = agent->left;
    while (fn) {
        if (fn->kind == AST_FN) {
            cg_str(g, "static ");
            cg_type(g, fn->type_expr);
            cg_fmt(g, " lcn_agent_%s_%s(Agent_%s *self",
                   name, fn->name ? fn->name : "anon", name);

            AstNode *p = fn->params;
            while (p) {
                cg_str(g, ", ");
                cg_type(g, p->type_expr);
                cg_fmt(g, " %s", p->name ? p->name : "_");
                p = p->next;
            }
            cg_str(g, ") ");

            /* Set agent method context for tool call rewriting */
            g->in_agent_method = true;
            g->current_agent_name = name;

            /* BUG-2 fix: save string var scope, register string-typed params */
            int saved_svc = g->string_var_count;
            {
                AstNode *sp = fn->params;
                while (sp) {
                    if (sp->name && sp->type_expr &&
                        sp->type_expr->kind == AST_TYPE_NAMED &&
                        sp->type_expr->name &&
                        strcmp(sp->type_expr->name, "string") == 0 &&
                        g->string_var_count < 256) {
                        g->string_vars[g->string_var_count++] = sp->name;
                    }
                    sp = sp->next;
                }
            }

            AstNode *saved_agent_fn_ret = g->current_fn_ret_type;
            g->current_fn_ret_type = fn->type_expr;
            if (fn->left) {
                cg_block_with_implicit_return(g, fn->left, fn->type_expr);
            } else {
                cg_str(g, "{ }");
            }
            g->current_fn_ret_type = saved_agent_fn_ret;

            g->string_var_count = saved_svc;
            g->in_agent_method = false;
            g->current_agent_name = NULL;
            cg_str(g, "\n\n");
        }
        fn = fn->next;
    }
    g->current_access_policy = NULL;

    /* 4. Default LLM-calling run() when agent has prompt + llm but no run */
    if (has_prompt && has_llm_cap && !has_run_method && g->use_runtime_header) {
        cg_fmt(g, "/* auto-generated: agent %s has prompt + llm.complete */\n", name);
        cg_fmt(g, "static LcnResult lcn_agent_%s_run(Agent_%s *self, LcnString user_message) {\n", name, name);
        g->indent++;
        cg_line(g, "LcnLlmResult _llm;");

        /* RAG: declare variables for knowledge base search */
        if (has_knowledge) {
            cg_line(g, "LcnKbChunk _kb_results[5];");
            cg_line(g, "int _kb_count;");
            cg_line(g, "char *_kb_context = NULL;");
            cg_line(g, "char *_augmented_prompt = NULL;");
        }
        cg_nl(g);

        /* Store user input in memory if memory is enabled */
        if (has_memory) {
            cg_line(g, "/* Store user input in memory */");
            cg_line(g, "if (self->memory_enabled) {");
            g->indent++;
            cg_line(g, "char _mem_val[4096];");
            cg_indent(g);
            cg_str(g, "snprintf(_mem_val, sizeof(_mem_val), \"\\\"%.3900s\\\"\", user_message);\n");
            cg_indent(g);
            cg_fmt(g, "lcn_memory_insert(lcn_memory_store(), \"%s\", \"\", LCN_MEM_MESSAGE, \"user_input\", _mem_val, NULL, 0, 0);\n", name);
            g->indent--;
            cg_line(g, "}");
            cg_nl(g);
        }

        cg_line(g, "/* Check budget */");
        cg_line(g, "if (!lcn_budget_check_runtime(&self->budget)) {");
        g->indent++;
        cg_line(g, "return LCN_ERR(\"budget exhausted\");");
        g->indent--;
        cg_line(g, "}");
        cg_nl(g);
        /* RAG: search knowledge base and build augmented prompt */
        if (has_knowledge) {
            cg_line(g, "/* RAG: search knowledge base */");
            cg_line(g, "_kb_count = lcn_kb_search(lcn_kb_store(), user_message, 5, _kb_results, 5);");
            cg_line(g, "if (_kb_count > 0) {");
            g->indent++;
            cg_line(g, "_kb_context = lcn_kb_format_context(_kb_results, _kb_count);");
            cg_line(g, "if (_kb_context) {");
            g->indent++;
            cg_line(g, "size_t _plen = strlen(self->prompt) + strlen(_kb_context) + 4;");
            cg_line(g, "_augmented_prompt = (char *)malloc(_plen);");
            cg_indent(g);
            cg_str(g, "snprintf(_augmented_prompt, _plen, \"%s\\n\\n%s\", self->prompt, _kb_context);\n");
            cg_line(g, "free(_kb_context);");
            g->indent--;
            cg_line(g, "}");
            cg_line(g, "{");
            g->indent++;
            cg_line(g, "int _ki;");
            cg_line(g, "for (_ki = 0; _ki < _kb_count; _ki++) lcn_kb_chunk_free(&_kb_results[_ki]);");
            g->indent--;
            cg_line(g, "}");
            g->indent--;
            cg_line(g, "}");
            cg_nl(g);
        }

        cg_line(g, "/* Call LLM */");
        if (has_knowledge) {
            cg_line(g, "_llm = lcn_llm_call(self->endpoint, self->model, _augmented_prompt ? _augmented_prompt : self->prompt, user_message, &self->budget, self->api_key);");
            cg_line(g, "free(_augmented_prompt);");
        } else {
            cg_line(g, "_llm = lcn_llm_call(self->endpoint, self->model, self->prompt, user_message, &self->budget, self->api_key);");
        }
        cg_nl(g);
        cg_line(g, "if (!_llm.ok) {");
        g->indent++;
        cg_line(g, "fprintf(stderr, \"LLM error: %%s\\n\", _llm.error ? _llm.error : \"unknown\");");
        cg_line(g, "free(_llm.error);");
        cg_line(g, "return LCN_ERR(\"LLM call failed\");");
        g->indent--;
        cg_line(g, "}");
        cg_nl(g);
        /* Auto-record entropy if entropy_budget is configured */
        if (has_entropy_budget) {
            cg_line(g, "/* Record entropy for budget tracking */");
            cg_line(g, "if (self->_entropy_tracker) {");
            g->indent++;
            cg_line(g, "lcn_entropy_record(self->_entropy_tracker, _llm.confidence, _llm.entropy, 0);");
            cg_line(g, "const char *_eb_err = lcn_entropy_check_budget(self->_entropy_tracker, &self->entropy_budget);");
            cg_line(g, "if (_eb_err) {");
            g->indent++;
            cg_line(g, "fprintf(stderr, \"entropy budget violated: %s\\n\", _eb_err);");
            cg_line(g, "if (_llm.content) free(_llm.content);");
            cg_line(g, "return LCN_ERR(_eb_err);");
            g->indent--;
            cg_line(g, "}");
            g->indent--;
            cg_line(g, "}");
            cg_nl(g);
        }
        cg_line(g, "if (_llm.content) {");
        g->indent++;

        /* Store LLM response in memory if memory is enabled */
        if (has_memory) {
            cg_line(g, "/* Store LLM response in memory */");
            cg_line(g, "if (self->memory_enabled) {");
            g->indent++;
            cg_line(g, "char _mem_resp[4096];");
            cg_indent(g);
            cg_str(g, "snprintf(_mem_resp, sizeof(_mem_resp), \"\\\"%.3900s\\\"\", _llm.content);\n");
            cg_indent(g);
            cg_fmt(g, "lcn_memory_insert(lcn_memory_store(), \"%s\", \"\", LCN_MEM_MESSAGE, \"llm_response\", _mem_resp, NULL, 0, 0);\n", name);
            g->indent--;
            cg_line(g, "}");
        }

        cg_line(g, "printf(\"%%s\\n\", _llm.content);");
        cg_line(g, "free(_llm.content);");
        g->indent--;
        cg_line(g, "}");
        cg_nl(g);
        cg_line(g, "return LCN_OK;");
        g->indent--;
        cg_str(g, "}\n\n");

        /* Mark this agent for default main generation */
        if (!g->default_agent_name)
            g->default_agent_name = name;
    }
}

/* ============================================================
 * Runtime Preamble
 * ============================================================ */

/* Emit platform-detection guards for cross-compiled code.
 * When cross-compiling, we emit compile-time defines so that the generated C
 * code can adapt to the target platform even when the runtime header uses
 * #ifdef __linux__ / __APPLE__ / _WIN32 guards. */
static void cg_platform_guards(CodeGen *g) {
    if (!g->target || g->target->arch == LCN_ARCH_UNKNOWN) return;

    cg_str(g, "/* ════════════════════════════════════════════════\n"
              " * Cross-Compilation Target Platform Defines\n"
              " * ════════════════════════════════════════════════ */\n\n");

    cg_fmt(g, "#define LCN_TARGET_TRIPLE \"%s\"\n", g->target->triple);
    cg_fmt(g, "#define LCN_TARGET_ARCH   \"%s\"\n", lcn_arch_str(g->target->arch));
    cg_fmt(g, "#define LCN_TARGET_OS     \"%s\"\n", lcn_os_str(g->target->os));

    /* Architecture defines */
    switch (g->target->arch) {
    case LCN_ARCH_X86_64:
        cg_str(g, "#define LCN_ARCH_X86_64   1\n");
        break;
    case LCN_ARCH_AARCH64:
        cg_str(g, "#define LCN_ARCH_AARCH64  1\n");
        break;
    case LCN_ARCH_ARM:
        cg_str(g, "#define LCN_ARCH_ARM      1\n");
        break;
    default:
        break;
    }

    /* OS defines */
    switch (g->target->os) {
    case LCN_OS_LINUX:
        cg_str(g, "#define LCN_OS_LINUX      1\n");
        break;
    case LCN_OS_DARWIN:
        cg_str(g, "#define LCN_OS_DARWIN     1\n");
        break;
    case LCN_OS_WINDOWS:
        cg_str(g, "#define LCN_OS_WINDOWS    1\n");
        break;
    default:
        break;
    }

    /* ABI defines */
    switch (g->target->abi) {
    case LCN_ABI_GNU:
        cg_str(g, "#define LCN_ABI_GNU       1\n");
        break;
    case LCN_ABI_MUSL:
        cg_str(g, "#define LCN_ABI_MUSL      1\n");
        break;
    case LCN_ABI_MSVC:
        cg_str(g, "#define LCN_ABI_MSVC      1\n");
        break;
    default:
        break;
    }

    if (g->target->static_link)
        cg_str(g, "#define LCN_STATIC_LINK   1\n");

    cg_nl(g);

    /* Emit portable platform-adaptation macros */
    cg_str(g,
        "/* Platform-portable library extension and path separator */\n"
        "#if defined(LCN_OS_LINUX)\n"
        "  #define LCN_DYLIB_EXT  \".so\"\n"
        "  #define LCN_PATH_SEP   '/'\n"
        "#elif defined(LCN_OS_DARWIN)\n"
        "  #define LCN_DYLIB_EXT  \".dylib\"\n"
        "  #define LCN_PATH_SEP   '/'\n"
        "#elif defined(LCN_OS_WINDOWS)\n"
        "  #define LCN_DYLIB_EXT  \".dll\"\n"
        "  #define LCN_PATH_SEP   '\\\\'\n"
        "#else\n"
        "  #define LCN_DYLIB_EXT  \".so\"\n"
        "  #define LCN_PATH_SEP   '/'\n"
        "#endif\n\n"
    );

    /* Thread primitives portability shim */
    cg_str(g,
        "/* Thread primitives portability */\n"
        "#if defined(LCN_OS_WINDOWS)\n"
        "  #include <windows.h>\n"
        "  #define LCN_THREAD_T    HANDLE\n"
        "  #define LCN_MUTEX_T     CRITICAL_SECTION\n"
        "#else\n"
        "  #include <pthread.h>\n"
        "  #define LCN_THREAD_T    pthread_t\n"
        "  #define LCN_MUTEX_T     pthread_mutex_t\n"
        "#endif\n\n"
    );

    /* I/O multiplexing portability shim */
    cg_str(g,
        "/* I/O multiplexing portability */\n"
        "#if defined(LCN_OS_LINUX)\n"
        "  #define LCN_HAS_EPOLL   1\n"
        "#elif defined(LCN_OS_DARWIN)\n"
        "  #define LCN_HAS_KQUEUE  1\n"
        "#elif defined(LCN_OS_WINDOWS)\n"
        "  #define LCN_HAS_IOCP    1\n"
        "#endif\n\n"
    );
}

static void cg_preamble(CodeGen *g, const char *source_file) {
    cg_fmt(g, "/* Generated by Limceron Compiler %s */\n", LCN_VERSION);
    cg_fmt(g, "/* Source: %s */\n", source_file ? source_file : "<unknown>");
    if (g->target && g->target->arch != LCN_ARCH_UNKNOWN)
        cg_fmt(g, "/* Target: %s */\n", g->target->triple);
    cg_str(g, "/* DO NOT EDIT — this file is auto-generated from Limceron source */\n\n");

    /* Emit platform guards before runtime header when cross-compiling */
    if (g->target && g->target->arch != LCN_ARCH_UNKNOWN)
        cg_platform_guards(g);

    if (g->use_runtime_header) {
        /* Build mode: use the consolidated runtime header */
        cg_str(g, "#include \"lcn_runtime.h\"\n\n");
        return;
    }

    /* Standalone mode: inline all runtime types */
    cg_str(g, "#include <stdio.h>\n");
    cg_str(g, "#include <stdlib.h>\n");
    cg_str(g, "#include <stdint.h>\n");
    cg_str(g, "#include <stdbool.h>\n");
    cg_str(g, "#include <string.h>\n");
    cg_str(g, "#include <unistd.h>\n");
    cg_str(g, "#include <pthread.h>\n");
    cg_str(g, "#include <sys/socket.h>\n");
    cg_str(g, "#include <netinet/in.h>\n");
    cg_str(g, "#include <arpa/inet.h>\n\n");

    cg_str(g,
        "/* ════════════════════════════════════════════════\n"
        " * Limceron Runtime\n"
        " * ════════════════════════════════════════════════ */\n\n"

        "typedef const char *LcnString;\n\n"

        "typedef struct {\n"
        "    bool ok;\n"
        "    void *value;\n"
        "    LcnString error;\n"
        "} LcnResult;\n\n"

        "#define LCN_OK   ((LcnResult){ .ok = true,  .value = NULL, .error = NULL })\n"
        "#define LCN_ERR(msg) ((LcnResult){ .ok = false, .value = NULL, .error = (msg) })\n\n"

        "typedef struct {\n"
        "    void **items;\n"
        "    int32_t len;\n"
        "    int32_t cap;\n"
        "} LcnVec;\n\n"

        "static LcnVec lcn_vec_new(void) {\n"
        "    return (LcnVec){ .items = NULL, .len = 0, .cap = 0 };\n"
        "}\n\n"

        "static void lcn_vec_push(LcnVec *v, void *item) {\n"
        "    if (v->len >= v->cap) {\n"
        "        v->cap = v->cap ? v->cap * 2 : 8;\n"
        "        v->items = (void **)realloc(v->items, sizeof(void *) * (size_t)v->cap);\n"
        "    }\n"
        "    v->items[v->len++] = item;\n"
        "}\n\n"

        "static void *lcn_vec_get(LcnVec *v, int32_t index) {\n"
        "    if (!v || index < 0 || index >= v->len) return NULL;\n"
        "    return v->items[index];\n"
        "}\n\n"

        "static int32_t lcn_vec_len(LcnVec *v) {\n"
        "    return v ? v->len : 0;\n"
        "}\n\n"

        "static void *lcn_vec_pop(LcnVec *v) {\n"
        "    if (v->len == 0) return NULL;\n"
        "    return v->items[--v->len];\n"
        "}\n\n"

        "typedef struct {\n"
        "    bool has_value;\n"
        "    int64_t value;\n"
        "} LcnOption;\n\n"

        "typedef uint64_t LcnCapability;\n\n"

        "/* Closure (function pointer + captured environment) */\n"
        "typedef struct {\n"
        "    void *fn;\n"
        "    void *env;\n"
        "} LcnClosure;\n\n"
    );

    /* Capability delegation (Hurd-inspired) — emitted as separate string to stay under C99 limit */
    cg_str(g,
        "/* Capability delegation (Hurd-inspired) */\n"
        "typedef struct {\n"
        "    LcnCapability caps;\n"
        "    LcnCapability original;\n"
        "    int64_t revoke_after_ms;\n"
        "    int64_t created_at_ms;\n"
        "} LcnDelegation;\n\n"

        "#ifdef _WIN32\n"
        "#include <windows.h>\n"
        "static int64_t _lcn_deleg_now_ms(void) { return (int64_t)GetTickCount64(); }\n"
        "#else\n"
        "#include <sys/time.h>\n"
        "static int64_t _lcn_deleg_now_ms(void) {\n"
        "    struct timeval tv; gettimeofday(&tv, NULL);\n"
        "    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;\n"
        "}\n"
        "#endif\n\n"

        "static LcnDelegation *lcn_delegate_new(LcnCapability parent, LcnCapability req, int64_t timeout_ms) {\n"
        "    LcnDelegation *d = (LcnDelegation *)malloc(sizeof(LcnDelegation));\n"
        "    if (!d) return NULL;\n"
        "    d->caps = req & parent;\n"
        "    d->original = d->caps;\n"
        "    d->revoke_after_ms = timeout_ms;\n"
        "    d->created_at_ms = _lcn_deleg_now_ms();\n"
        "    return d;\n"
        "}\n\n"

        "static void lcn_delegate_revoke(LcnDelegation *d, LcnCapability cap) {\n"
        "    if (d) d->caps &= ~cap;\n"
        "}\n\n"

        "static void lcn_delegate_revoke_all(LcnDelegation *d) {\n"
        "    if (d) d->caps = 0;\n"
        "}\n\n"

        "static bool lcn_delegate_check_timeout(LcnDelegation *d) {\n"
        "    if (!d || d->revoke_after_ms <= 0) return false;\n"
        "    if (_lcn_deleg_now_ms() - d->created_at_ms >= d->revoke_after_ms) {\n"
        "        d->caps = 0;\n"
        "        return true;\n"
        "    }\n"
        "    return false;\n"
        "}\n\n"

        "static bool lcn_delegate_has(LcnDelegation *d, LcnCapability cap) {\n"
        "    if (!d) return false;\n"
        "    if (lcn_delegate_check_timeout(d)) return false;\n"
        "    return (d->caps & cap) != 0;\n"
        "}\n\n"

        "static void lcn_delegate_free(LcnDelegation *d) { free(d); }\n\n"
    );

    cg_str(g,
        "#include <time.h>\n\n"

        "typedef struct {\n"
        "    int64_t max_tokens;\n"
        "    double  max_cost;\n"
        "    int64_t max_duration_secs;\n"
        "    int64_t used_tokens;\n"
        "    double  used_cost;\n"
        "    time_t  start_time;\n"
        "    bool    exhausted;\n"
        "    const char *exhausted_reason;\n"
        "} LcnBudget;\n\n"

        "static bool lcn_budget_check(LcnBudget *b) {\n"
        "    if (b->exhausted) return false;\n"
        "    if (b->max_tokens > 0 && b->used_tokens >= b->max_tokens) return false;\n"
        "    if (b->max_cost > 0.0 && b->used_cost >= b->max_cost) return false;\n"
        "    if (b->max_duration_secs > 0 && difftime(time(NULL), b->start_time) >= b->max_duration_secs) return false;\n"
        "    return true;\n"
        "}\n\n"

        "typedef enum {\n"
        "    LCN_STRATEGY_ONE_FOR_ONE,\n"
        "    LCN_STRATEGY_REST_FOR_ALL,\n"
        "    LCN_STRATEGY_ONE_FOR_ALL,\n"
        "} LcnSupervisorStrategy;\n\n"
    );

    /* Supervisor runtime types (inline preamble) */
    cg_str(g,
        "#define LCN_SUPERVISOR_MAX_CHILDREN 64\n"
        "#define LCN_SUPERVISOR_MAX_RESTARTS 256\n\n"

        "typedef enum { LCN_CHILD_STOPPED, LCN_CHILD_RUNNING, LCN_CHILD_FAILED, LCN_CHILD_RESTARTING } LcnChildState;\n"
        "typedef enum { LCN_RESTART_ALWAYS, LCN_RESTART_ON_FAILURE, LCN_RESTART_NEVER } LcnRestartMode;\n\n"

        "typedef struct {\n"
        "    const char *name;\n"
        "    void (*start_fn)(void);\n"
        "    void (*stop_fn)(void);\n"
        "    LcnChildState state;\n"
        "    LcnRestartMode restart_mode;\n"
        "    int restart_count;\n"
        "    int consecutive_failures;\n"
        "} LcnSupervisorChild;\n\n"

        "typedef struct {\n"
        "    time_t timestamps[LCN_SUPERVISOR_MAX_RESTARTS];\n"
        "    int head;\n"
        "    int count;\n"
        "} LcnRestartHistory;\n\n"

        "typedef enum { LCN_SUPERVISOR_STOPPED, LCN_SUPERVISOR_RUNNING, LCN_SUPERVISOR_SHUTTING_DOWN, LCN_SUPERVISOR_ESCALATED } LcnSupervisorState;\n\n"

        "typedef struct LcnSupervisor {\n"
        "    const char *name;\n"
        "    LcnSupervisorStrategy strategy;\n"
        "    int max_restarts;\n"
        "    int window_seconds;\n"
        "    LcnSupervisorState state;\n"
        "    LcnSupervisorChild children[LCN_SUPERVISOR_MAX_CHILDREN];\n"
        "    int child_count;\n"
        "    LcnRestartHistory history;\n"
        "    int total_restarts;\n"
        "    time_t started_at;\n"
        "    time_t last_restart_at;\n"
        "    void (*on_escalate)(struct LcnSupervisor *sup);\n"
        "} LcnSupervisor;\n\n"

        "static LcnSupervisor *lcn_supervisor_new(const char *name, LcnSupervisorStrategy strategy, int max_restarts, int window_seconds) {\n"
        "    LcnSupervisor *sup = (LcnSupervisor *)calloc(1, sizeof(LcnSupervisor));\n"
        "    if (!sup) return NULL;\n"
        "    sup->name = name;\n"
        "    sup->strategy = strategy;\n"
        "    sup->max_restarts = max_restarts;\n"
        "    sup->window_seconds = window_seconds > 0 ? window_seconds : 60;\n"
        "    sup->state = LCN_SUPERVISOR_STOPPED;\n"
        "    return sup;\n"
        "}\n\n"

        "static void lcn_supervisor_add_child(LcnSupervisor *sup, const char *agent_name, void (*start_fn)(void), void (*stop_fn)(void), LcnRestartMode mode) {\n"
        "    if (!sup || sup->child_count >= LCN_SUPERVISOR_MAX_CHILDREN) return;\n"
        "    LcnSupervisorChild *c = &sup->children[sup->child_count];\n"
        "    c->name = agent_name;\n"
        "    c->start_fn = start_fn;\n"
        "    c->stop_fn = stop_fn;\n"
        "    c->state = LCN_CHILD_STOPPED;\n"
        "    c->restart_mode = mode;\n"
        "    c->restart_count = 0;\n"
        "    c->consecutive_failures = 0;\n"
        "    sup->child_count++;\n"
        "}\n\n"
    );

    /* Supervisor runtime functions continued */
    cg_str(g,
        "static void lcn_supervisor_start(LcnSupervisor *sup) {\n"
        "    if (!sup || sup->state == LCN_SUPERVISOR_RUNNING) return;\n"
        "    sup->state = LCN_SUPERVISOR_RUNNING;\n"
        "    sup->started_at = time(NULL);\n"
        "    for (int i = 0; i < sup->child_count; i++) {\n"
        "        LcnSupervisorChild *c = &sup->children[i];\n"
        "        if (c->start_fn) { c->state = LCN_CHILD_RUNNING; c->start_fn(); }\n"
        "    }\n"
        "}\n\n"

        "static void lcn_supervisor_stop(LcnSupervisor *sup) {\n"
        "    if (!sup || sup->state == LCN_SUPERVISOR_STOPPED) return;\n"
        "    sup->state = LCN_SUPERVISOR_SHUTTING_DOWN;\n"
        "    for (int i = sup->child_count - 1; i >= 0; i--) {\n"
        "        LcnSupervisorChild *c = &sup->children[i];\n"
        "        if (c->state != LCN_CHILD_STOPPED && c->stop_fn) c->stop_fn();\n"
        "        c->state = LCN_CHILD_STOPPED;\n"
        "    }\n"
        "    sup->state = LCN_SUPERVISOR_STOPPED;\n"
        "}\n\n"

        "static int lcn_supervisor_child_failed(LcnSupervisor *sup, int ci) {\n"
        "    if (!sup || ci < 0 || ci >= sup->child_count || sup->state != LCN_SUPERVISOR_RUNNING) return -1;\n"
        "    sup->children[ci].state = LCN_CHILD_FAILED;\n"
        "    sup->children[ci].consecutive_failures++;\n"
        "    { int idx = (sup->history.head + sup->history.count) %% LCN_SUPERVISOR_MAX_RESTARTS;\n"
        "      sup->history.timestamps[idx] = time(NULL);\n"
        "      if (sup->history.count < LCN_SUPERVISOR_MAX_RESTARTS) sup->history.count++;\n"
        "      else sup->history.head = (sup->history.head + 1) %% LCN_SUPERVISOR_MAX_RESTARTS; }\n"
        "    sup->total_restarts++;\n"
        "    sup->last_restart_at = time(NULL);\n"
        "    if (sup->max_restarts > 0) {\n"
        "        time_t cutoff = time(NULL) - (time_t)sup->window_seconds;\n"
        "        int recent = 0;\n"
        "        for (int i = 0; i < sup->history.count; i++) {\n"
        "            int idx = (sup->history.head + i) %% LCN_SUPERVISOR_MAX_RESTARTS;\n"
        "            if (sup->history.timestamps[idx] >= cutoff) recent++;\n"
        "        }\n"
        "        if (recent > sup->max_restarts) {\n"
        "            sup->state = LCN_SUPERVISOR_ESCALATED;\n"
        "            for (int i = sup->child_count - 1; i >= 0; i--) {\n"
        "                if (sup->children[i].stop_fn) sup->children[i].stop_fn();\n"
        "                sup->children[i].state = LCN_CHILD_STOPPED;\n"
        "            }\n"
        "            if (sup->on_escalate) sup->on_escalate(sup);\n"
        "            return -2;\n"
        "        }\n"
        "    }\n"
        "    switch (sup->strategy) {\n"
        "    case LCN_STRATEGY_ONE_FOR_ONE: {\n"
        "        LcnSupervisorChild *c = &sup->children[ci];\n"
        "        if (c->restart_mode != LCN_RESTART_NEVER) { if (c->stop_fn) c->stop_fn(); c->restart_count++; if (c->start_fn) { c->state = LCN_CHILD_RUNNING; c->start_fn(); } c->consecutive_failures = 0; }\n"
        "        else c->state = LCN_CHILD_STOPPED;\n"
        "        break; }\n"
        "    case LCN_STRATEGY_ONE_FOR_ALL:\n"
        "        for (int i = sup->child_count - 1; i >= 0; i--) { if (sup->children[i].stop_fn) sup->children[i].stop_fn(); sup->children[i].state = LCN_CHILD_STOPPED; }\n"
        "        for (int i = 0; i < sup->child_count; i++) { LcnSupervisorChild *c = &sup->children[i]; if (c->restart_mode != LCN_RESTART_NEVER) { c->restart_count++; if (c->start_fn) { c->state = LCN_CHILD_RUNNING; c->start_fn(); } c->consecutive_failures = 0; } }\n"
        "        break;\n"
        "    case LCN_STRATEGY_REST_FOR_ALL:\n"
        "        for (int i = sup->child_count - 1; i >= ci; i--) { if (sup->children[i].stop_fn) sup->children[i].stop_fn(); sup->children[i].state = LCN_CHILD_STOPPED; }\n"
        "        for (int i = ci; i < sup->child_count; i++) { LcnSupervisorChild *c = &sup->children[i]; if (c->restart_mode != LCN_RESTART_NEVER) { c->restart_count++; if (c->start_fn) { c->state = LCN_CHILD_RUNNING; c->start_fn(); } c->consecutive_failures = 0; } }\n"
        "        break;\n"
        "    }\n"
        "    return 0;\n"
        "}\n\n"

        "static void lcn_supervisor_free(LcnSupervisor *sup) {\n"
        "    if (sup) { if (sup->state == LCN_SUPERVISOR_RUNNING) lcn_supervisor_stop(sup); free(sup); }\n"
        "}\n\n"
    );

    /* Continuation of preamble: LLM output types and remaining runtime */
    cg_str(g,
        "typedef enum {\n"
        "    LCN_LLM_OUTPUT_OK,\n"
        "    LCN_LLM_OUTPUT_TEXT,\n"
        "    LCN_LLM_OUTPUT_TOOL_CALL,\n"
        "    LCN_LLM_OUTPUT_ERROR\n"
        "} LcnLlmOutputKind;\n\n"

        "typedef struct {\n"
        "    LcnLlmOutputKind kind;\n"
        "    LcnString content;\n"
        "    LcnString tool_name;\n"
        "    LcnString tool_args;\n"
        "    LcnString error;\n"
        "} LcnLlmOutput;\n\n"

        "static LcnLlmOutput lcn_llm_output_ok(LcnString c) {\n"
        "    LcnLlmOutput o = {0}; o.kind = LCN_LLM_OUTPUT_OK; o.content = c; return o;\n"
        "}\n"
        "static LcnLlmOutput lcn_llm_output_error(LcnString e) {\n"
        "    LcnLlmOutput o = {0}; o.kind = LCN_LLM_OUTPUT_ERROR; o.error = e; return o;\n"
        "}\n"
        "static LcnString lcn_llm_output_unwrap(LcnLlmOutput o) {\n"
        "    return (o.kind == LCN_LLM_OUTPUT_OK || o.kind == LCN_LLM_OUTPUT_TEXT) ? (o.content ? o.content : \"\") : \"\";\n"
        "}\n"
        "static LcnLlmOutput lcn_ask_typed(const char *ep, const char *model, const char *prompt, const char *q, void *budget, const void *policy, const char *api_key) {\n"
        "    (void)ep; (void)model; (void)prompt; (void)budget; (void)policy; (void)api_key;\n"
        "    return lcn_llm_output_ok(q);\n"
        "}\n\n"

        "/* String concatenation helper */\n"
        "static char *lcn_str_concat(const char *a, const char *b) {\n"
        "    if (!a) a = \"\";\n"
        "    if (!b) b = \"\";\n"
        "    size_t la = strlen(a);\n"
        "    size_t lb = strlen(b);\n"
        "    char *result = (char *)malloc(la + lb + 1);\n"
        "    if (!result) return (char *)\"\";\n"
        "    memcpy(result, a, la);\n"
        "    memcpy(result + la, b, lb);\n"
        "    result[la + lb] = '\\0';\n"
        "    return result;\n"
        "}\n\n"
    );

    /* Character-level string manipulation builtins */
    cg_str(g,
        "/* Character-level string manipulation */\n"
        "static LcnString lcn_char_at(LcnString s, int64_t i) {\n"
        "    if (!s || i < 0 || i >= (int64_t)strlen(s)) return \"\";\n"
        "    char *buf = (char *)malloc(2);\n"
        "    if (!buf) return \"\";\n"
        "    buf[0] = s[i]; buf[1] = '\\0';\n"
        "    return buf;\n"
        "}\n\n"

        "static int64_t lcn_char_code(LcnString s) {\n"
        "    return (s && s[0]) ? (int64_t)(unsigned char)s[0] : 0;\n"
        "}\n\n"

        "static LcnString lcn_str_from_code(int64_t code) {\n"
        "    char *buf = (char *)malloc(2);\n"
        "    if (!buf) return \"\";\n"
        "    buf[0] = (char)code; buf[1] = '\\0';\n"
        "    return buf;\n"
        "}\n\n"

        "static LcnString lcn_str_slice(LcnString s, int64_t start, int64_t end) {\n"
        "    if (!s) return \"\";\n"
        "    int64_t len = (int64_t)strlen(s);\n"
        "    if (start < 0) start = 0;\n"
        "    if (end > len) end = len;\n"
        "    if (start >= end) return \"\";\n"
        "    size_t slice_len = (size_t)(end - start);\n"
        "    char *out = (char *)malloc(slice_len + 1);\n"
        "    if (!out) return \"\";\n"
        "    memcpy(out, s + start, slice_len);\n"
        "    out[slice_len] = '\\0';\n"
        "    return out;\n"
        "}\n\n"

        "static int64_t lcn_str_find(LcnString s, LcnString needle) {\n"
        "    if (!s || !needle) return -1;\n"
        "    const char *p = strstr(s, needle);\n"
        "    return p ? (int64_t)(p - s) : -1;\n"
        "}\n\n"

        "static bool lcn_char_is_alpha(LcnString s) {\n"
        "    if (!s || !s[0]) return false;\n"
        "    char c = s[0];\n"
        "    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';\n"
        "}\n\n"

        "static bool lcn_char_is_digit(LcnString s) {\n"
        "    if (!s || !s[0]) return false;\n"
        "    return s[0] >= '0' && s[0] <= '9';\n"
        "}\n\n"

        "static bool lcn_char_is_alnum(LcnString s) {\n"
        "    return lcn_char_is_alpha(s) || lcn_char_is_digit(s);\n"
        "}\n\n"
    );

    /* CLI args globals */
    cg_str(g,
        "/* CLI args globals */\n"
        "static int _lcn_argc = 0;\n"
        "static char **_lcn_argv = NULL;\n\n"
    );

    /* Capability fence: runtime tool validation (standalone stubs) */
    cg_str(g,
        "/* capability fence: global context for runtime tool validation */\n"
        "static const char **_lcn_fence_tools = NULL;\n"
        "static int _lcn_fence_tool_count = 0;\n"
        "static const char *_lcn_fence_agent_name = NULL;\n\n"

        "static bool lcn_capability_check_tool(const char *tool_name,\n"
        "                                       const char **allowed_tools,\n"
        "                                       int tool_count) {\n"
        "    int i;\n"
        "    if (!tool_name || !allowed_tools || tool_count <= 0) return false;\n"
        "    for (i = 0; i < tool_count; i++) {\n"
        "        if (allowed_tools[i] && strcmp(tool_name, allowed_tools[i]) == 0)\n"
        "            return true;\n"
        "    }\n"
        "    return false;\n"
        "}\n\n"

        "static void lcn_capability_violation(const char *agent_name,\n"
        "                                      const char *tool_name,\n"
        "                                      const char *reason) {\n"
        "    fprintf(stderr,\n"
        "            \"SECURITY: capability violation -- agent '%s', tool '%s': %s\\n\",\n"
        "            agent_name ? agent_name : \"<unknown>\",\n"
        "            tool_name  ? tool_name  : \"<unknown>\",\n"
        "            reason     ? reason     : \"unspecified\");\n"
        "}\n\n"
    );

    /* String builder */
    cg_str(g,
        "/* String builder */\n"
        "typedef struct {\n"
        "    char *data;\n"
        "    size_t len;\n"
        "    size_t cap;\n"
        "} LcnStringBuilder;\n\n"

        "static void *lcn_sb_new(void) {\n"
        "    LcnStringBuilder *sb = (LcnStringBuilder *)calloc(1, sizeof(LcnStringBuilder));\n"
        "    sb->cap = 256;\n"
        "    sb->data = (char *)malloc(sb->cap);\n"
        "    sb->data[0] = '\\0';\n"
        "    return sb;\n"
        "}\n\n"

        "static void lcn_sb_append(void *handle, LcnString s) {\n"
        "    LcnStringBuilder *sb = (LcnStringBuilder *)handle;\n"
        "    if (!sb || !s) return;\n"
        "    size_t slen = strlen(s);\n"
        "    while (sb->len + slen + 1 > sb->cap) {\n"
        "        sb->cap *= 2;\n"
        "        sb->data = (char *)realloc(sb->data, sb->cap);\n"
        "    }\n"
        "    memcpy(sb->data + sb->len, s, slen);\n"
        "    sb->len += slen;\n"
        "    sb->data[sb->len] = '\\0';\n"
        "}\n\n"

        "static LcnString lcn_sb_to_string(void *handle) {\n"
        "    LcnStringBuilder *sb = (LcnStringBuilder *)handle;\n"
        "    if (!sb) return \"\";\n"
        "    char *result = sb->data;\n"
        "    free(sb);\n"
        "    return result;\n"
        "}\n\n"
    );

    /* Access Control Policy types and stubs (separate string to stay under C99 limit) */
    cg_str(g,
        "/* Access Control Policy types */\n"
        "typedef struct {\n"
        "    const char *host;\n"
        "    int         port;\n"
        "    bool        allow;\n"
        "    const char *path_glob;\n"
        "} LcnEndpointRule;\n\n"

        "typedef struct {\n"
        "    const char *path;\n"
        "    bool        allow;\n"
        "} LcnBinaryRule;\n\n"

        "typedef struct {\n"
        "    const char *pattern;\n"
        "    bool        allow;\n"
        "    bool        can_read;\n"
        "    bool        can_write;\n"
        "} LcnPathRule;\n\n"

        "typedef struct {\n"
        "    const LcnEndpointRule *endpoints;\n"
        "    const LcnBinaryRule   *binaries;\n"
        "    const LcnPathRule     *paths;\n"
        "    bool                   deny_private;\n"
        "    bool                   default_deny;\n"
        "} LcnAccessPolicy;\n\n"

        "/* Stub: lcn_http_get */\n"
        "static LcnString lcn_http_get(const char *url) {\n"
        "    (void)url;\n"
        "    fprintf(stderr, \"[stub] lcn_http_get: %s\\n\", url);\n"
        "    return \"\";\n"
        "}\n\n"

        "/* Stub: lcn_fetch_checked */\n"
        "static LcnResult lcn_fetch_checked(const char *url, const LcnAccessPolicy *p) {\n"
        "    (void)p;\n"
        "    return (LcnResult){ true, (void *)lcn_http_get(url), NULL };\n"
        "}\n\n"

        "/* Stub: lcn_exec_checked */\n"
        "static LcnResult lcn_exec_checked(const char *cmd, const LcnAccessPolicy *p) {\n"
        "    (void)p;\n"
        "    return (LcnResult){ system(cmd) == 0, NULL, NULL };\n"
        "}\n\n"
    );

    /* File I/O + str_from_int helpers (standalone mode) */
    cg_str(g,
        "/* File I/O helpers (standalone mode) */\n"
        "static LcnString lcn_read_file(const char *path) {\n"
        "    FILE *f = fopen(path, \"rb\");\n"
        "    if (!f) return \"\";\n"
        "    fseek(f, 0, SEEK_END);\n"
        "    long sz = ftell(f);\n"
        "    fseek(f, 0, SEEK_SET);\n"
        "    if (sz < 0 || sz > 64*1024*1024) { fclose(f); return \"\"; }\n"
        "    char *buf = (char *)malloc((size_t)sz + 1);\n"
        "    if (!buf) { fclose(f); return \"\"; }\n"
        "    size_t rd = fread(buf, 1, (size_t)sz, f);\n"
        "    fclose(f);\n"
        "    buf[rd] = '\\0';\n"
        "    return buf;\n"
        "}\n\n"

        "static bool lcn_write_file(const char *path, const char *content) {\n"
        "    FILE *f = fopen(path, \"wb\");\n"
        "    if (!f) return false;\n"
        "    size_t len = strlen(content);\n"
        "    size_t written = fwrite(content, 1, len, f);\n"
        "    fclose(f);\n"
        "    return written == len;\n"
        "}\n\n"

        "static LcnString lcn_read_line(void) {\n"
        "    static char buf[4096];\n"
        "    if (fgets(buf, sizeof(buf), stdin)) {\n"
        "        size_t len = strlen(buf);\n"
        "        if (len > 0 && buf[len-1] == '\\n') buf[len-1] = '\\0';\n"
        "        return buf;\n"
        "    }\n"
        "    return \"\";\n"
        "}\n\n"

        "/* to_string helper (standalone mode) */\n"
        "static char *lcn_str_from_int(int64_t val) {\n"
        "    char buf[32];\n"
        "    snprintf(buf, sizeof(buf), \"%lld\", (long long)val);\n"
        "    char *out = (char *)malloc(strlen(buf) + 1);\n"
        "    if (!out) return (char *)\"0\";\n"
        "    strcpy(out, buf);\n"
        "    return out;\n"
        "}\n\n"
    );

    /* File access control stubs */
    cg_str(g,
        "/* Stub: lcn_read_file_checked */\n"
        "static LcnString lcn_read_file_checked(const char *path, "
        "const LcnAccessPolicy *p) {\n"
        "    (void)p;\n"
        "    return lcn_read_file(path);\n"
        "}\n\n"

        "/* Stub: lcn_write_file_checked */\n"
        "static bool lcn_write_file_checked(const char *path, "
        "const char *content, const LcnAccessPolicy *p) {\n"
        "    (void)p;\n"
        "    return lcn_write_file(path, content);\n"
        "}\n\n"
    );

    /* SQL escape helper */
    cg_str(g,
        "/* lcn_sql_escape — escape dangerous chars for SQL */\n"
        "static LcnString lcn_sql_escape(LcnString s) {\n"
        "    if (!s) return \"\";\n"
        "    size_t len = strlen(s);\n"
        "    char *out = (char *)malloc(len * 2 + 1);\n"
        "    if (!out) return s;\n"
        "    size_t j = 0;\n"
        "    size_t i;\n"
        "    for (i = 0; i < len; i++) {\n"
        "        switch (s[i]) {\n"
        "        case '\\'': out[j++] = '\\''; out[j++] = '\\''; break;\n"
        "        case '\\\\': out[j++] = '\\\\'; out[j++] = '\\\\'; break;\n"
        "        case '\\0': break;\n"
        "        default: out[j++] = s[i]; break;\n"
        "        }\n"
        "    }\n"
        "    out[j] = '\\0';\n"
        "    return out;\n"
        "}\n\n"
    );

    /* lcn_unwrap — extract string from LcnResult, or "" on error */
    cg_str(g,
        "/* lcn_unwrap — extract string value from Result */\n"
        "static LcnString lcn_unwrap(LcnResult r) {\n"
        "    if (r.ok && r.value) return (LcnString)r.value;\n"
        "    if (r.error) fprintf(stderr, \"unwrap error: %s\\n\", r.error);\n"
        "    return \"\";\n"
        "}\n\n"
    );

    /* Channel and concurrency stubs for standalone mode */
    cg_str(g,
        "/* Channel stubs (standalone mode — real impl in channel.c) */\n"
        "typedef struct LcnChannel LcnChannel;\n"
        "static LcnChannel *lcn_channel_new(int cap, size_t elem) {\n"
        "    (void)cap; (void)elem;\n"
        "    fprintf(stderr, \"[stub] channels require build mode\\n\");\n"
        "    return NULL;\n"
        "}\n"
        "static bool lcn_channel_send(LcnChannel *ch, const void *d) { (void)ch; (void)d; return false; }\n"
        "static bool lcn_channel_recv(LcnChannel *ch, void *d) { (void)ch; (void)d; return false; }\n"
        "static bool lcn_channel_try_recv(LcnChannel *ch, void *d) { (void)ch; (void)d; return false; }\n"
        "static void lcn_channel_close(LcnChannel *ch) { (void)ch; }\n"
        "static bool lcn_channel_is_closed(LcnChannel *ch) { (void)ch; return true; }\n"
        "static int  lcn_channel_len(LcnChannel *ch) { (void)ch; return 0; }\n"
        "static void lcn_channel_free(LcnChannel *ch) { (void)ch; }\n\n"

        "/* Select stub */\n"
        "static int lcn_select(LcnChannel **chs, int n, int timeout_ms) {\n"
        "    (void)chs; (void)n; (void)timeout_ms; return -1;\n"
        "}\n\n"

        "/* Thread pool stubs */\n"
        "typedef struct LcnTaskHandle LcnTaskHandle;\n"
        "typedef void *(*LcnTaskFn)(void *arg);\n"
        "static void lcn_threadpool_init(int n) { (void)n; }\n"
        "static LcnTaskHandle *lcn_spawn_task(LcnTaskFn fn, void *arg) {\n"
        "    (void)fn; (void)arg;\n"
        "    fprintf(stderr, \"[stub] spawn_parallel requires build mode\\n\");\n"
        "    return NULL;\n"
        "}\n"
        "static void *lcn_await_task(LcnTaskHandle *h) { (void)h; return NULL; }\n"
        "static void lcn_threadpool_shutdown(void) {}\n"
        "static bool lcn_threadpool_active(void) { return false; }\n\n"
    );

    /* Green thread stubs (standalone mode) */
    cg_str(g,
        "/* Green thread stubs (standalone mode — real impl in green_threads.c) */\n"
        "typedef struct GreenThread GreenThread;\n"
        "static void lcn_green_init(void) {}\n"
        "static GreenThread *lcn_green_spawn(void *(*fn)(void *), void *arg) {\n"
        "    (void)fn; (void)arg;\n"
        "    fprintf(stderr, \"[stub] green threads require build mode\\n\");\n"
        "    return NULL;\n"
        "}\n"
        "static void lcn_green_yield(void) {}\n"
        "static void *lcn_green_await(GreenThread *gt) { (void)gt; return NULL; }\n"
        "static void lcn_green_shutdown(void) {}\n"
        "static bool lcn_green_active(void) { return false; }\n"
        "static void lcn_green_park(GreenThread *gt) { (void)gt; }\n"
        "static void lcn_green_unpark(GreenThread *gt) { (void)gt; }\n\n"
    );

    /* TaskGroup stubs for standalone mode */
    cg_str(g,
        "/* TaskGroup stubs (structured concurrency — standalone mode) */\n"
        "typedef struct LcnTaskGroup LcnTaskGroup;\n"
        "static LcnTaskGroup *lcn_task_group_new(void) { return NULL; }\n"
        "static void lcn_task_group_spawn(LcnTaskGroup *tg, LcnTaskFn fn, void *arg) {\n"
        "    (void)tg; (void)fn; (void)arg;\n"
        "    fprintf(stderr, \"[stub] task_group requires build mode\\n\");\n"
        "}\n"
        "static void **lcn_task_group_await_all(LcnTaskGroup *tg) { (void)tg; return NULL; }\n"
        "static void lcn_task_group_free(LcnTaskGroup *tg) { (void)tg; }\n\n"

        "/* Entropy tracker stubs (standalone mode) */\n"
        "typedef struct { double max_avg_entropy; double max_low_confidence; double max_drift; double low_confidence_threshold; } LcnEntropyBudget;\n"
        "typedef struct LcnEntropyTracker LcnEntropyTracker;\n"
        "static LcnEntropyTracker *lcn_entropy_tracker_new(int cap, int cats) { (void)cap; (void)cats; return NULL; }\n"
        "static void lcn_entropy_record(LcnEntropyTracker *t, double c, double e, int cat) { (void)t; (void)c; (void)e; (void)cat; }\n"
        "static double lcn_entropy_avg_confidence(LcnEntropyTracker *t, int w) { (void)t; (void)w; return 1.0; }\n"
        "static double lcn_entropy_avg_entropy(LcnEntropyTracker *t, int w) { (void)t; (void)w; return 0.0; }\n"
        "static double lcn_entropy_low_confidence_pct(LcnEntropyTracker *t, int w, double thr) { (void)t; (void)w; (void)thr; return 0.0; }\n"
        "static void lcn_entropy_distribution(LcnEntropyTracker *t, int w, double *d, int n) { (void)t; (void)w; (void)d; (void)n; }\n"
        "static const char *lcn_entropy_check_budget(LcnEntropyTracker *t, const LcnEntropyBudget *b) { (void)t; (void)b; return NULL; }\n"
        "static void lcn_entropy_tracker_free(LcnEntropyTracker *t) { (void)t; }\n\n"
        "/* Drift detection stubs (standalone mode) */\n"
        "static double lcn_kl_divergence(const double *P, const double *Q, int n) { (void)P; (void)Q; (void)n; return 0.0; }\n"
        "static double lcn_drift(const double *cur, const double *base, int n) { (void)cur; (void)base; (void)n; return 0.0; }\n"
        "static double lcn_js_divergence(const double *P, const double *Q, int n) { (void)P; (void)Q; (void)n; return 0.0; }\n\n"

        "/* ONNX Model stubs (standalone mode) */\n"
        "typedef struct LcnModel LcnModel;\n"
        "typedef struct {\n"
        "    const char *label;\n"
        "    double      confidence;\n"
        "    double      entropy;\n"
        "    int         class_id;\n"
        "    bool        ok;\n"
        "    const char *error;\n"
        "} LcnModelResult;\n"
        "static LcnModel *lcn_model_load(const char *path, const char *label_map) {\n"
        "    (void)path; (void)label_map;\n"
        "    fprintf(stderr, \"[model] load (STUB): %s\\n\", path ? path : \"?\");\n"
        "    return NULL;\n"
        "}\n"
        "static LcnModelResult lcn_model_predict(LcnModel *m, const char *text) {\n"
        "    (void)m;\n"
        "    LcnModelResult r = {0}; r.ok = true; r.label = \"STUB_PREDICTION\";\n"
        "    r.confidence = 0.99; r.entropy = 0.01;\n"
        "    fprintf(stderr, \"[model] predict (STUB): %s\\n\", text ? text : \"\");\n"
        "    return r;\n"
        "}\n"
        "static void lcn_model_free(LcnModel *m) { (void)m; }\n"
        "static const char *lcn_model_info(LcnModel *m) { (void)m; return \"stub\"; }\n"
        "static void lcn_set_local_model(LcnModel *m) { (void)m; }\n"
        "static LcnModel *lcn_get_local_model(void) { return NULL; }\n\n"
    );
}

/* ============================================================
 * MCP Server Entry Point — agent-as-service
 * ============================================================ */

static void cg_main_wrapper_serve(CodeGen *g) {
    /* Find the first agent to serve */
    const char *agent_name = NULL;
    if (g->agent_count > 0) agent_name = g->agent_names[0];
    if (!agent_name) {
        cg_str(g, "int main(void) {\n");
        cg_str(g, "    fprintf(stderr, \"error: no agent to serve\\n\");\n");
        cg_str(g, "    return 1;\n}\n");
        return;
    }

    /* Global agent instance for MCP handler access */
    cg_fmt(g, "static Agent_%s _mcp_agent;\n\n", agent_name);

    /* Tool handler: receives args JSON, returns result text */
    cg_fmt(g, "static const char *_mcp_tool_run(const char *args_json) {\n");
    g->indent++;
    cg_line(g, "LcnJsonValue *_args = lcn_json_parse(args_json, strlen(args_json));");
    cg_line(g, "const char *_input = _args ? lcn_json_get_string(_args, \"input\") : \"\";");
    cg_line(g, "if (!_input) _input = \"\";");
    cg_line(g, "LcnLlmOutput _out = lcn_ask_typed(NULL, _mcp_agent.model, _mcp_agent.prompt, _input, &_mcp_agent.budget, NULL, _mcp_agent.api_key);");
    cg_line(g, "if (_args) lcn_json_free(_args);");
    cg_line(g, "return lcn_llm_output_unwrap(_out);");
    g->indent--;
    cg_str(g, "}\n\n");

    /* Main: init agent, register tool, run server loop */
    cg_str(g, "int main(int argc, char **argv) {\n");
    cg_str(g, "    _lcn_argc = argc; _lcn_argv = argv;\n");
    g->indent++;
    cg_fmt(g, "    _mcp_agent = lcn_agent_%s_new();\n", agent_name);
    cg_nl(g);
    cg_line(g, "LcnMcpServer _server;");
    cg_fmt(g, "    lcn_mcp_server_init(&_server, \"%s\", \"1.0.0\");\n", agent_name);
    cg_line(g, "lcn_mcp_server_add_tool(&_server, \"run\",");
    cg_fmt(g, "    \"Run the %s agent\",\n", agent_name);
    cg_line(g, "    \"{\\\"type\\\":\\\"object\\\",\\\"properties\\\":{\\\"input\\\":{\\\"type\\\":\\\"string\\\"}}}\",");
    cg_line(g, "    _mcp_tool_run);");
    cg_nl(g);
    cg_line(g, "return lcn_mcp_server_run(&_server);");
    g->indent--;
    cg_str(g, "}\n");
}

/* ============================================================
 * Health Probes
 * ============================================================ */

static void cg_health(CodeGen *g, AstNode *node) {
    if (!node || node->kind != AST_HEALTH) return;

    g->has_health = true;
    g->health_port = (int)node->val.int_val;

    cg_str(g, "/* \xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\n"
              " * Health Probes (/healthz, /readyz)\n"
              " * \xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90\xe2\x95\x90 */\n\n");
    cg_str(g, "#include <sys/socket.h>\n");
    cg_str(g, "#include <netinet/in.h>\n");
    cg_str(g, "#include <pthread.h>\n\n");

    cg_str(g, "static int _lcn_health_ready = 1;\n");
    cg_str(g, "static int _lcn_health_live = 1;\n\n");

    cg_str(g,
        "static void *_lcn_health_server(void *arg) {\n"
        "    int port = *(int *)arg;\n"
        "    int fd = socket(AF_INET, SOCK_STREAM, 0);\n"
        "    int opt = 1;\n"
        "    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));\n"
        "\n"
        "    struct sockaddr_in addr;\n"
        "    memset(&addr, 0, sizeof(addr));\n"
        "    addr.sin_family = AF_INET;\n"
        "    addr.sin_addr.s_addr = INADDR_ANY;\n"
        "    addr.sin_port = htons(port);\n"
        "    bind(fd, (struct sockaddr *)&addr, sizeof(addr));\n"
        "    listen(fd, 5);\n"
        "\n"
        "    while (1) {\n"
        "        int client = accept(fd, NULL, NULL);\n"
        "        if (client < 0) continue;\n"
        "\n"
        "        char buf[512];\n"
        "        read(client, buf, sizeof(buf) - 1);\n"
        "        buf[sizeof(buf) - 1] = \'\\0\';\n"
        "\n"
        "        const char *response;\n"
        "        if (strstr(buf, \"/readyz\")) {\n"
        "            response = _lcn_health_ready\n"
        "                ? \"HTTP/1.1 200 OK\\r\\nContent-Length: 2\\r\\n\\r\\nok\"\n"
        "                : \"HTTP/1.1 503 Service Unavailable\\r\\nContent-Length: 9\\r\\n\\r\\nnot ready\";\n"
        "        } else if (strstr(buf, \"/healthz\")) {\n"
        "            response = _lcn_health_live\n"
        "                ? \"HTTP/1.1 200 OK\\r\\nContent-Length: 2\\r\\n\\r\\nok\"\n"
        "                : \"HTTP/1.1 503 Service Unavailable\\r\\nContent-Length: 8\\r\\n\\r\\nnot live\";\n"
        "        } else {\n"
        "            response = \"HTTP/1.1 404 Not Found\\r\\nContent-Length: 9\\r\\n\\r\\nnot found\";\n"
        "        }\n"
        "        write(client, response, strlen(response));\n"
        "        close(client);\n"
        "    }\n"
        "    return NULL;\n"
        "}\n\n"
    );
}

/* ============================================================
 * Main Entry Point
 * ============================================================ */

static void cg_main_wrapper(CodeGen *g) {
    if (g->has_main) {
        cg_str(g, "int main(int argc, char **argv) {\n");
        cg_str(g, "    _lcn_argc = argc; _lcn_argv = argv;\n");
        if (g->use_runtime_header) {
            cg_str(g,
                "    LcnHttpServer _dashboard;\n"
                "    if (getenv(\"LCN_DASHBOARD\") && strcmp(getenv(\"LCN_DASHBOARD\"), \"1\") == 0) {\n"
                "        lcn_httpd_init(&_dashboard, 9090, NULL);\n"
                "        lcn_dashboard_register_routes(&_dashboard);\n"
                "        lcn_httpd_start(&_dashboard);\n"
                "        fprintf(stderr, \"[limceron] Dashboard: http://localhost:9090\\n\");\n"
                "    }\n"
            );
            if (g->memory_agent_count > 0) {
                cg_str(g, "    /* Initialize persistent memory */\n");
                cg_str(g, "    lcn_memory_store();\n");
            }
        }
        /* Initialize and start supervisors before main */
        if (g->supervisor_count > 0) {
            cg_str(g, "    /* Initialize supervisors */\n");
            for (int si = 0; si < g->supervisor_count; si++) {
                cg_fmt(g, "    lcn_supervisor_%s_init();\n", g->supervisors[si].name);
                cg_fmt(g, "    lcn_supervisor_start(_lcn_sup_%s);\n", g->supervisors[si].name);
            }
        }
        /* Start health probe server on background thread */
        if (g->has_health) {
            cg_fmt(g, "    static int _health_port = %d;\n", g->health_port);
            cg_str(g, "    pthread_t _health_thread;\n");
            cg_str(g, "    pthread_create(&_health_thread, NULL, _lcn_health_server, &_health_port);\n");
            cg_fmt(g, "    fprintf(stderr, \"[limceron] Health probes: http://0.0.0.0:%d/healthz, /readyz\\n\");\n",
                   g->health_port);
        }
        /* Register local ONNX model for agents with endpoint:"local" */
        if (g->model_alias_count > 0) {
            cg_str(g, "    /* Local model registration for agent endpoint:\"local\" */\n");
            cg_fmt(g, "    lcn_model_%s_init();\n", g->model_aliases[0].alias);
            cg_fmt(g, "    lcn_set_local_model(_model_%s);\n", g->model_aliases[0].alias);
        }
        /* Initialize green thread scheduler before main */
        cg_str(g, "    lcn_green_init();\n");
        /* Initialize progress reporting if declared */
        if (g->has_progress) {
            cg_str(g, "    _lcn_progress_init();\n");
        }

        /* Start Prometheus metrics server on background thread */
        if (g->has_metrics) {
            cg_str(g, "    /* Start metrics server */\n");
            cg_str(g, "    { pthread_t _mt; pthread_create(&_mt, NULL, _lcn_metrics_server, NULL); pthread_detach(_mt); }\n");
        }

        if (g->main_returns_void) {
            cg_str(g, "    lcn_main();\n");
            cg_str(g, "    lcn_green_shutdown();\n");
            /* Stop supervisors after main */
            if (g->supervisor_count > 0) {
                cg_str(g, "    /* Cleanup supervisors */\n");
                for (int si = g->supervisor_count - 1; si >= 0; si--) {
                    cg_fmt(g, "    lcn_supervisor_stop(_lcn_sup_%s);\n", g->supervisors[si].name);
                    cg_fmt(g, "    lcn_supervisor_free(_lcn_sup_%s);\n", g->supervisors[si].name);
                }
            }
            if (g->use_runtime_header && g->memory_agent_count > 0) {
                cg_str(g, "    lcn_memory_close(lcn_memory_store());\n");
            }
            cg_str(g, "    return 0;\n}\n");
        } else {
            cg_str(g,
                "    LcnResult _r = lcn_main();\n"
                "    lcn_green_shutdown();\n"
            );
            /* Stop supervisors after main */
            if (g->supervisor_count > 0) {
                cg_str(g, "    /* Cleanup supervisors */\n");
                for (int si = g->supervisor_count - 1; si >= 0; si--) {
                    cg_fmt(g, "    lcn_supervisor_stop(_lcn_sup_%s);\n", g->supervisors[si].name);
                    cg_fmt(g, "    lcn_supervisor_free(_lcn_sup_%s);\n", g->supervisors[si].name);
                }
            }
            if (g->use_runtime_header && g->memory_agent_count > 0) {
                cg_str(g, "    lcn_memory_close(lcn_memory_store());\n");
            }
            cg_str(g,
                "    if (!_r.ok) {\n"
                "        fprintf(stderr, \"error: %s\\n\", _r.error ? _r.error : \"unknown\");\n"
                "        return 1;\n"
                "    }\n"
                "    return 0;\n"
                "}\n"
            );
        }
    } else if (g->default_agent_name) {
        /* Auto-generated main: create agent, run with argv[1] */
        cg_str(g, "int main(int argc, char **argv) {\n");
        cg_str(g, "    _lcn_argc = argc; _lcn_argv = argv;\n");
        cg_str(g, "    lcn_green_init();\n");
        if (g->has_progress) {
            cg_str(g, "    _lcn_progress_init();\n");
        }
        g->indent++;
        cg_fmt(g, "    Agent_%s _agent;\n", g->default_agent_name);
        cg_line(g, "LcnResult _r;");
        if (g->use_runtime_header) {
            cg_line(g, "LcnHttpServer _dashboard;");
            cg_nl(g);
            cg_line(g, "if (getenv(\"LCN_DASHBOARD\") && strcmp(getenv(\"LCN_DASHBOARD\"), \"1\") == 0) {");
            g->indent++;
            cg_line(g, "lcn_httpd_init(&_dashboard, 9090, NULL);");
            cg_line(g, "lcn_dashboard_register_routes(&_dashboard);");
            cg_line(g, "lcn_httpd_start(&_dashboard);");
            cg_line(g, "fprintf(stderr, \"[limceron] Dashboard: http://localhost:9090\\n\");");
            g->indent--;
            cg_line(g, "}");
            if (g->memory_agent_count > 0) {
                cg_nl(g);
                cg_line(g, "/* Initialize persistent memory */");
                cg_line(g, "lcn_memory_store();");
            }
            if (g->has_kb) {
                cg_nl(g);
                cg_line(g, "/* Initialize knowledge base */");
                cg_line(g, "{");
                g->indent++;
                cg_line(g, "LcnKnowledgeBase *_kb = lcn_kb_store();");
                cg_fmt(g, "    _kb->chunk_size = %d;\n", g->kb_chunk_size);
                cg_fmt(g, "    _kb->chunk_overlap = %d;\n", g->kb_chunk_overlap);
                cg_line(g, "if (!lcn_kb_has_data(_kb)) {");
                g->indent++;
                if (g->kb_path) {
                    cg_fmt(g, "    lcn_kb_ingest_dir(_kb, \"%s\");\n", g->kb_path);
                }
                g->indent--;
                cg_line(g, "}");
                g->indent--;
                cg_line(g, "}");
            }
        }
        cg_nl(g);
        cg_fmt(g, "    _agent = lcn_agent_%s_new();\n", g->default_agent_name);
        cg_nl(g);
        cg_line(g, "if (argc >= 2) {");
        g->indent++;
        cg_line(g, "/* Mode 1: Single message from argv */");
        cg_fmt(g, "        _r = lcn_agent_%s_run(&_agent, argv[1]);\n", g->default_agent_name);
        cg_line(g, "if (!_r.ok) {");
        g->indent++;
        cg_line(g, "fprintf(stderr, \"error: %%s\\n\", _r.error ? _r.error : \"unknown\");");
        if (g->use_runtime_header && g->has_kb) {
            cg_line(g, "lcn_kb_close(lcn_kb_store());");
        }
        if (g->use_runtime_header && g->memory_agent_count > 0) {
            cg_line(g, "lcn_memory_close(lcn_memory_store());");
        }
        cg_line(g, "return 1;");
        g->indent--;
        cg_line(g, "}");
        g->indent--;
        cg_line(g, "} else {");
        g->indent++;
        cg_line(g, "/* Mode 2: Interactive REPL */");
        cg_line(g, "char _input[4096];");
        cg_line(g, "fprintf(stderr, \"[limceron] Interactive mode. Type 'exit' to quit.\\n\\n\");");
        cg_line(g, "while (1) {");
        g->indent++;
        cg_line(g, "fprintf(stderr, \"> \");");
        cg_line(g, "if (!fgets(_input, sizeof(_input), stdin)) break;");
        cg_line(g, "/* Trim newline */");
        cg_line(g, "{ size_t _len = strlen(_input);");
        cg_line(g, "  if (_len > 0 && _input[_len-1] == '\\n') _input[_len-1] = '\\0'; }");
        cg_line(g, "if (_input[0] == '\\0') continue;");
        cg_line(g, "if (strcmp(_input, \"exit\") == 0 || strcmp(_input, \"quit\") == 0) break;");
        cg_fmt(g, "            _r = lcn_agent_%s_run(&_agent, _input);\n", g->default_agent_name);
        cg_line(g, "if (!_r.ok) {");
        g->indent++;
        cg_line(g, "fprintf(stderr, \"error: %%s\\n\", _r.error ? _r.error : \"unknown\");");
        g->indent--;
        cg_line(g, "}");
        cg_line(g, "fprintf(stderr, \"\\n\");");
        g->indent--;
        cg_line(g, "}");
        cg_line(g, "fprintf(stderr, \"[limceron] Goodbye.\\n\");");
        g->indent--;
        cg_line(g, "}");
        cg_nl(g);
        if (g->use_runtime_header && g->has_kb) {
            cg_line(g, "lcn_kb_close(lcn_kb_store());");
        }
        if (g->use_runtime_header && g->memory_agent_count > 0) {
            cg_line(g, "lcn_memory_close(lcn_memory_store());");
        }
        cg_line(g, "lcn_green_shutdown();");
        cg_line(g, "return 0;");
        g->indent--;
        cg_str(g, "}\n");
    } else {
        cg_str(g,
            "int main(int argc, char **argv) {\n"
            "    _lcn_argc = argc; _lcn_argv = argv;\n"
            "    printf(\"Limceron Agent Runtime " LCN_VERSION "\\n\");\n"
            "    return 0;\n"
            "}\n"
        );
    }
}

static char *codegen_internal_ex(AstNode *program, const char *source_file, Arena *arena,
                                 bool use_runtime_header, bool serve_mode,
                                 const LcnTarget *target) {
    if (!program || program->kind != AST_PROGRAM) return NULL;

    CodeGen g;
    memset(&g, 0, sizeof(g));
    g.arena = arena;
    g.cap = CG_INITIAL_CAP;
    g.buf = (char *)malloc(g.cap);
    g.buf[0] = '\0';
    g.use_runtime_header = use_runtime_header;
    g.serve_mode = serve_mode;
    g.target = target;

    /* Green threads: enabled via LCN_GREEN_THREADS=1 env var at compile time,
     * or always in build mode when the runtime is linked. */
    {
        const char *gt_env = getenv("LCN_GREEN_THREADS");
        g.use_green_threads = (gt_env && gt_env[0] == '1');
    }

    /* 1. Runtime preamble */
    cg_preamble(&g, source_file);

    /* 1b. Monomorphization: scan entire AST for generic type usages,
     * then emit typedefs and helpers for each specialization. */
    mono_collect_from_node(&g, program);
    {
        int mi;
        bool any_mono = false;
        for (mi = 0; mi < g.mono_count; mi++) {
            if (!g.mono[mi].emitted) {
                if (!any_mono) {
                    cg_str(&g, "/* ════════════════════════════════════════════════\n"
                               " * Monomorphized Generic Types\n"
                               " * ════════════════════════════════════════════════ */\n\n");
                    any_mono = true;
                }
                g.mono[mi].emitted = true;
                if (strcmp(g.mono[mi].base, "Result") == 0) {
                    mono_emit_result(&g, g.mono[mi].name, g.mono[mi].generics);
                } else if (strcmp(g.mono[mi].base, "Option") == 0) {
                    mono_emit_option(&g, g.mono[mi].name, g.mono[mi].generics);
                }
            }
        }
        if (any_mono) cg_nl(&g);
    }

    /* 2. Pre-registration pass: collect names so later passes know about
     * agents and budgets before their code is emitted. */
    AstNode *decl;
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_AGENT && decl->name) {
            cg_register_agent(&g, decl->name);
            /* Register all methods of this agent */
            AstNode *m = decl->left;
            while (m) {
                if (m->kind == AST_FN && m->name)
                    cg_register_agent_method(&g, decl->name, m->name);
                m = m->next;
            }
        }
        if (decl->kind == AST_BUDGET && decl->name)
            cg_register_budget(&g, decl->name);
        /* Pre-register MCP aliases for method call rewriting */
        if (decl->kind == AST_USE && decl->name && strcmp(decl->name, "mcp") == 0) {
            const char *alias = (decl->right && decl->right->name) ? decl->right->name : "mcp";
            if (g.mcp_alias_count < 32) {
                g.mcp_aliases[g.mcp_alias_count].alias = alias;
                g.mcp_alias_count++;
            }
        }
        /* Pre-register model aliases for method call rewriting */
        if (decl->kind == AST_USE && decl->name && strcmp(decl->name, "model") == 0) {
            const char *alias = (decl->right && decl->right->name) ? decl->right->name : "model";
            if (g.model_alias_count < 32) {
                g.model_aliases[g.model_alias_count].alias = alias;
                g.model_alias_count++;
            }
        }
        /* Pre-register impl methods for method call rewriting */
        if (decl->kind == AST_IMPL && decl->left &&
            decl->left->kind == AST_TYPE_NAMED && decl->left->name) {
            const char *tname = decl->left->name;
            AstNode *m = decl->params;
            while (m) {
                if (m->kind == AST_FN && m->name && g.impl_method_count < 256) {
                    g.impl_methods[g.impl_method_count].type_name = tname;
                    g.impl_methods[g.impl_method_count].method_name = m->name;
                    g.impl_method_count++;
                }
                m = m->next;
            }
        }
        /* Pre-register structs for type inference */
        if (decl->kind == AST_STRUCT && decl->name) {
            cg_register_struct(&g, decl->name);
        }
        /* Pre-register enums for type inference */
        if (decl->kind == AST_ENUM && decl->name) {
            bool has_data = false;
            AstNode *v;
            for (v = decl->params; v; v = v->next) {
                if (v->kind == AST_VARIANT && v->params) { has_data = true; break; }
            }
            cg_register_enum(&g, decl->name, has_data, decl);
        }
        /* Pre-register user-defined function return types */
        if (decl->kind == AST_FN && decl->name && decl->type_expr &&
            decl->type_expr->kind == AST_TYPE_NAMED && decl->type_expr->name) {
            const char *raw = decl->type_expr->name;
            const char *ctype = cg_type_to_c(decl->type_expr);
            /* cg_type_to_c may return a static buffer for generics;
             * arena_strdup to preserve the value */
            cg_register_fn_ret(&g, decl->name, arena_strdup(arena, ctype));
            (void)raw;
        }
    }

    /* 3. Walk declarations in order, grouped by type */

    /* Pass 0: collect link directives (AST_LINK nodes — no C output) */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_LINK && decl->val.str_val && g.link_flag_count < 64) {
            g.link_flags[g.link_flag_count++] = decl->val.str_val;
            cg_fmt(&g, "/* link: %s */\n", decl->val.str_val);
        }
    }

    /* Pass 0c: health probes (AST_HEALTH nodes) */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_HEALTH) cg_health(&g, decl);
    }

    /* Pass 1: capabilities (must come first for #defines) */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_CAPABILITY) cg_capability(&g, decl);
    }

    /* Pass 1a: access control policy tables */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_CAPABILITY) cg_access_policies(&g, decl);
    }

    /* Pass 1b: auto-register capabilities referenced in agent member lists
     * (handles .lceron.md where capabilities are inline, not top-level decls) */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_AGENT) {
            AstNode *af = decl->params;
            while (af) {
                if (af->kind == AST_FIELD && af->name &&
                    strcmp(af->name, "capabilities") == 0 &&
                    af->right && af->right->kind == AST_ARRAY) {
                    AstNode *cap = af->right->params;
                    while (cap) {
                        if (cap->name && strchr(cap->name, '.')) {
                            /* Check if already registered */
                            const char *dot = strchr(cap->name, '.');
                            size_t glen = (size_t)(dot - cap->name);
                            const char *cname = dot + 1;
                            bool found = false;
                            int ci;
                            for (ci = 0; ci < g.cap_count; ci++) {
                                if (strlen(g.caps[ci].group) == glen &&
                                    strncmp(g.caps[ci].group, cap->name, glen) == 0 &&
                                    strcmp(g.caps[ci].name, cname) == 0) {
                                    found = true;
                                    break;
                                }
                            }
                            if (!found) {
                                char group_buf[128];
                                if (glen >= sizeof(group_buf)) glen = sizeof(group_buf) - 1;
                                memcpy(group_buf, cap->name, glen);
                                group_buf[glen] = '\0';
                                cg_register_cap(&g, arena_strdup(arena, group_buf),
                                                arena_strdup(arena, cname));
                                cg_fmt(&g, "/* capability %s */\n", cap->name);
                                cg_fmt(&g, "#define %s",
                                       cg_cap_define(&g, group_buf, cname));
                                cg_fmt(&g, "               ((LcnCapability)(1ULL << %d))\n\n",
                                       g.next_cap_bit - 1);
                            }
                        }
                        cap = cap->next;
                    }
                }
                af = af->next;
            }
        }
    }

    /* Pass 2: taints */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_TAINT) cg_taint(&g, decl);
    }
    if (program->params) cg_nl(&g);

    /* Pass 3: structs */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_STRUCT) cg_struct(&g, decl);
    }

    /* Pass 3b: enums */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_ENUM) cg_enum(&g, decl);
    }

    /* Pass 3c: interfaces and traits (vtable structs + trait objects) */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_INTERFACE || decl->kind == AST_TRAIT)
            cg_interface(&g, decl);
    }

    /* Pass 3d: union types (type X = A | B) */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_TYPE_ALIAS && decl->name &&
            decl->type_expr && decl->type_expr->kind == AST_TYPE_UNION) {
            cg_union_type(&g, decl->name, decl->type_expr);
        }
    }

    /* Pass 3e: impl blocks — direct methods (after structs, before vtable wrappers) */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_IMPL) cg_impl(&g, decl);
    }

    /* Pass 3f: impl Trait for Type — vtable wrapper shims + vtable instances */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_IMPL && decl->right)
            cg_impl_trait(&g, decl);
    }

    /* Pass 4: budgets */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_BUDGET) cg_budget(&g, decl);
    }

    /* Pass 5: guards */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_GUARD) cg_guard(&g, decl);
    }

    /* Pass 5b: invariants (emitted as comments for Stage 1) */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_INVARIANT) cg_invariant(&g, decl);
    }

    /* Pass 5c: comptime blocks — execute at compile time, emit constants */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_COMPTIME) {
            ComptimeValue val = comptime_evaluate(decl, g.arena);
            if (comptime_check_error(val, decl)) {
                cg_line(&g, "/* comptime error at top level */");
            }
        }
    }

    /* Pass 5d: extern function declarations (FFI) — must precede function bodies */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_FN && decl->is_unsafe && !decl->left)
            cg_extern_fn(&g, decl);
    }

    /* Pass 5e: forward declarations for all user functions (enables mutual recursion) */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_FN && decl->name
            && !is_codegen_builtin(decl->name)
            && !(decl->is_unsafe && !decl->left)) {
            cg_str(&g, "static ");
            cg_type(&g, decl->type_expr);
            cg_fmt(&g, " lcn_%s(", decl->name);
            AstNode *fp = decl->params;
            if (!fp) cg_str(&g, "void");
            while (fp) {
                cg_type(&g, fp->type_expr);
                cg_fmt(&g, " %s", fp->name ? fp->name : "_");
                if (fp->next) cg_str(&g, ", ");
                fp = fp->next;
            }
            cg_str(&g, ");\n");
        }
    }
    cg_str(&g, "\n");

    /* Pass 6: non-main free functions (helpers) — skip builtins and externs */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_FN && !(decl->name && strcmp(decl->name, "main") == 0)
            && !is_codegen_builtin(decl->name)
            && !(decl->is_unsafe && !decl->left))
            cg_fn(&g, decl);
    }

    /* Pass 7: tools */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_TOOL) cg_tool(&g, decl);
    }

    /* Pass 8: skills (comments only) */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_SKILL) cg_skill(&g, decl);
    }

    /* Pass 8.5: MCP/A2A/model protocol imports */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_USE && decl->name) {
            if (strcmp(decl->name, "mcp") == 0) cg_use_mcp(&g, decl);
            else if (strcmp(decl->name, "a2a") == 0) cg_use_a2a(&g, decl);
            else if (strcmp(decl->name, "driver") == 0) cg_use_driver(&g, decl);
            else if (strcmp(decl->name, "model") == 0) cg_use_model(&g, decl);
        }
    }

    /* Pass 9: supervisors */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_SUPERVISOR) cg_supervisor(&g, decl);
    }

    /* Pass 10: agents */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_AGENT) cg_agent(&g, decl);
    }

    /* Pass 10.5: meshes (after agents, since they reference Agent_X types) */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_MESH) cg_mesh(&g, decl);
    }

    /* Pass 10.6: routers */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_ROUTER) cg_router(&g, decl);
    }

    /* Pass 10.7: Prometheus metrics */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_METRICS) cg_metrics(&g, decl);
    }

    /* Pass 10.8: progress reporting */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_PROGRESS) cg_progress(&g, decl);
    }

    /* Pass 11: fn main (must come last — uses agents, tools, etc.) */
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_FN && decl->name && strcmp(decl->name, "main") == 0)
            cg_fn(&g, decl);
    }

    /* Entry point */
    if (g.serve_mode)
        cg_main_wrapper_serve(&g);
    else
        cg_main_wrapper(&g);

    /* Cleanup closure defs buffer */
    if (g.closure_defs_buf) {
        free(g.closure_defs_buf);
        g.closure_defs_buf = NULL;
    }

    return g.buf;
}

/* Backward-compatible wrapper */
static char *codegen_internal(AstNode *program, const char *source_file, Arena *arena,
                              bool use_runtime_header, bool serve_mode) {
    return codegen_internal_ex(program, source_file, arena, use_runtime_header, serve_mode, NULL);
}

/* Public API: standalone mode (inline preamble) — for `emit` command */
char *codegen_generate(AstNode *program, const char *source_file, Arena *arena) {
    return codegen_internal(program, source_file, arena, false, false);
}

/* Public API: build mode (#include "lcn_runtime.h") — for `build` command */
char *codegen_generate_for_build(AstNode *program, const char *source_file, Arena *arena) {
    return codegen_internal(program, source_file, arena, true, false);
}

/* Public API: build mode with cross-compilation target */
char *codegen_generate_for_build_target(AstNode *program, const char *source_file,
                                        Arena *arena, const LcnTarget *target) {
    return codegen_internal_ex(program, source_file, arena, true, false, target);
}

/* Public API: MCP server mode — agent-as-service */
char *codegen_generate_for_serve(AstNode *program, const char *source_file, Arena *arena) {
    return codegen_internal(program, source_file, arena, true, true);
}
