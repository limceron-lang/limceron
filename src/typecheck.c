/*
 * Limceron Compiler -- Type Checker & Semantic Analysis
 *
 * Runs after parsing, before codegen.  Performs:
 *   1. Symbol table construction (top-level declarations)
 *   2. Capability verification  (agent caps vs. tool requires)
 *   3. Guard enforcement         (sensitive caps without guards)
 *   4. Taint tracking            (@user_input flow to system contexts)
 *   5. Budget presence           (agents with tool calls need budgets)
 *   6. Basic type checking       (callee exists, binary operands)
 *
 * Every public function is declared in lcn.h.
 * Compiles cleanly under: -std=c99 -Wall -Wextra -Werror -pedantic
 */

#include "lcn.h"

/* ============================================================
 * Configuration
 * ============================================================ */

#define MAX_SYMBOLS         4096
#define MAX_CAP_ITEMS       512
#define MAX_AGENT_CAPS      128
#define MAX_TOOL_REQUIRES   64
#define MAX_TAINTED_PARAMS  256
#define MAX_GUARD_NAMES     256
#define MAX_SENSITIVE_CAPS  64
#define MAX_OWNED_VARS      512
#define MAX_OWN_SCOPES      64

/* ============================================================
 * Symbol Kinds
 * ============================================================ */

typedef enum {
    SYM_FN,
    SYM_AGENT,
    SYM_TOOL,
    SYM_CAPABILITY,
    SYM_GUARD,
    SYM_GUARDSET,
    SYM_BUDGET,
    SYM_TAINT,
    SYM_STRUCT,
    SYM_ENUM,
    SYM_TRAIT,
    SYM_INTERFACE,
    SYM_CONST,
    SYM_SUPERVISOR,
    SYM_SKILL,
    SYM_PROMPT,
    SYM_MESH,
    SYM_MEMORY,
    SYM_CHANNEL,
    SYM_ROUTER,
    SYM_STRATEGY,
    SYM_LET,
    SYM_TYPE_ALIAS
} SymKind;

/* ============================================================
 * Symbol Table
 * ============================================================ */

typedef struct {
    const char  *name;
    SymKind      kind;
    AstNode     *node;
    SourceLoc    loc;
} Symbol;

typedef struct {
    Symbol  entries[MAX_SYMBOLS];
    int     count;
} SymbolTable;

/* ============================================================
 * Capability Item  (group.name pair)
 * ============================================================ */

typedef struct {
    const char *group;
    const char *name;
    const char *qualified;   /* "group.name" */
} CapItem;

typedef struct {
    CapItem  items[MAX_CAP_ITEMS];
    int      count;
} CapRegistry;

/* ============================================================
 * Access Control Policy structures
 * ============================================================ */

#define MAX_ENDPOINT_RULES 64
#define MAX_BINARY_RULES   64
#define MAX_POLICIES       32
#define MAX_EP_METHODS     8

typedef struct {
    const char *host;
    int         port;
    bool        allow;
    const char *methods[MAX_EP_METHODS];
    int         method_count;
    const char *path_glob;
} EndpointRule;

typedef struct {
    const char *path;
    bool        allow;
} BinaryRule;

typedef struct {
    const char *pattern;    /* glob pattern, e.g. /data/... */
    bool        allow;
    bool        can_read;
    bool        can_write;
} PathRule;

#define MAX_PATH_RULES 64

typedef struct {
    const char   *group_name;
    EndpointRule  endpoints[MAX_ENDPOINT_RULES];
    int           endpoint_count;
    BinaryRule    binaries[MAX_BINARY_RULES];
    int           binary_count;
    PathRule      paths[MAX_PATH_RULES];
    int           path_count;
    bool          deny_private;
    bool          default_deny;
    bool          has_default;
    bool          is_concrete;
} ConcretePolicy;

typedef struct {
    ConcretePolicy policies[MAX_POLICIES];
    int            count;
} AccessPolicyRegistry;

/* ============================================================
 * Forward declarations
 * ============================================================ */

static void register_declarations(SymbolTable *st, AstNode *program,
                                  ErrorReporter *reporter, Arena *arena);

static void build_cap_registry(CapRegistry *cr, AstNode *program, Arena *arena);

static void check_capabilities(SymbolTable *st, CapRegistry *cr,
                               AstNode *program, ErrorReporter *reporter,
                               Arena *arena);

static void check_guards(SymbolTable *st, CapRegistry *cr,
                         AstNode *program, ErrorReporter *reporter,
                         Arena *arena);

static void check_taint(SymbolTable *st, AstNode *program,
                        ErrorReporter *reporter, Arena *arena);

static void check_secrets(SymbolTable *st, AstNode *program,
                          ErrorReporter *reporter, Arena *arena);

static void check_budgets(SymbolTable *st, AstNode *program,
                          ErrorReporter *reporter, Arena *arena);

static void check_types(SymbolTable *st, AstNode *program,
                        ErrorReporter *reporter, Arena *arena);

static void build_access_policies(AccessPolicyRegistry *apr,
                                  AstNode *program, Arena *arena);
static int  check_access_policies(SymbolTable *st,
                                  AccessPolicyRegistry *apr,
                                  AstNode *program,
                                  ErrorReporter *reporter, Arena *arena);

/* ============================================================
 * Ownership & Borrow Checking — Data Structures
 * ============================================================ */

typedef enum {
    OWN_OWNED,          /* variable owns its value */
    OWN_MOVED,          /* value was moved to another owner */
    OWN_BORROWED,       /* immutable borrow active */
    OWN_MUT_BORROWED    /* mutable borrow active */
} OwnershipState;

typedef struct {
    const char     *name;
    OwnershipState  state;
    int             borrow_count;       /* active immutable borrows */
    bool            has_mut_borrow;     /* exclusive mutable borrow active */
    bool            is_copy;            /* Copy type: int, bool, float, handles — never moved */
    const char     *moved_to;           /* where it was moved (for errors) */
    int             move_line;          /* line where move happened */
    int             borrow_line;        /* line where first borrow happened */
    int             mut_borrow_line;    /* line where mut borrow happened */
    int             scope_depth;        /* scope level where variable lives */
} VarOwnership;

typedef struct {
    VarOwnership vars[MAX_OWNED_VARS];
    int          count;
    int          scope_depth;
    bool         strict;    /* true = errors, false = warnings */
} OwnershipCtx;

static void check_ownership(AstNode *program, ErrorReporter *reporter,
                             Arena *arena, bool strict);

/* ============================================================
 * Utility: safe strcmp that handles NULL
 * ============================================================ */

static bool str_eq(const char *a, const char *b) {
    if (!a || !b) return false;
    return strcmp(a, b) == 0;
}

/* ============================================================
 * Utility: build a "group.name" qualified string in the arena
 * ============================================================ */

static const char *make_qualified(Arena *arena, const char *group,
                                  const char *name) {
    size_t glen = group ? strlen(group) : 0;
    size_t nlen = name  ? strlen(name)  : 0;
    char *buf = (char *)arena_alloc(arena, glen + 1 + nlen + 1);
    size_t off = 0;
    if (group) { memcpy(buf, group, glen); off += glen; }
    buf[off++] = '.';
    if (name) { memcpy(buf + off, name, nlen); off += nlen; }
    buf[off] = '\0';
    return buf;
}

/* ============================================================
 * Symbol Table Operations
 * ============================================================ */

static Symbol *symtab_find(SymbolTable *st, const char *name) {
    int i;
    for (i = 0; i < st->count; i++) {
        if (str_eq(st->entries[i].name, name))
            return &st->entries[i];
    }
    return NULL;
}

static Symbol *symtab_find_kind(SymbolTable *st, const char *name,
                                SymKind kind) {
    int i;
    for (i = 0; i < st->count; i++) {
        if (st->entries[i].kind == kind && str_eq(st->entries[i].name, name))
            return &st->entries[i];
    }
    return NULL;
}

static bool symtab_add(SymbolTable *st, const char *name, SymKind kind,
                       AstNode *node, SourceLoc loc, ErrorReporter *reporter) {
    if (!name) return false;

    /* Check for duplicates within same kind */
    Symbol *existing = symtab_find_kind(st, name, kind);
    if (existing) {
        report_error_fmt(reporter, loc,
                         "previous declaration was here",
                         "duplicate declaration: '%s'", name);
        return false;
    }

    if (st->count >= MAX_SYMBOLS) {
        report_error(reporter, loc,
                     "too many declarations (symbol table full)",
                     "reduce the number of top-level declarations");
        return false;
    }

    Symbol *s = &st->entries[st->count++];
    s->name = name;
    s->kind = kind;
    s->node = node;
    s->loc  = loc;
    return true;
}

/* ============================================================
 * Pass 1: Register Declarations
 * ============================================================ */

static SymKind ast_kind_to_sym_kind(AstKind k) {
    switch (k) {
    case AST_FN:          return SYM_FN;
    case AST_AGENT:       return SYM_AGENT;
    case AST_TOOL:        return SYM_TOOL;
    case AST_CAPABILITY:  return SYM_CAPABILITY;
    case AST_GUARD:       return SYM_GUARD;
    case AST_GUARDSET:    return SYM_GUARDSET;
    case AST_BUDGET:      return SYM_BUDGET;
    case AST_TAINT:       return SYM_TAINT;
    case AST_STRUCT:      return SYM_STRUCT;
    case AST_ENUM:        return SYM_ENUM;
    case AST_TRAIT:       return SYM_TRAIT;
    case AST_INTERFACE:   return SYM_INTERFACE;
    case AST_CONST:       return SYM_CONST;
    case AST_SUPERVISOR:  return SYM_SUPERVISOR;
    case AST_SKILL:       return SYM_SKILL;
    case AST_PROMPT:      return SYM_PROMPT;
    case AST_MESH:        return SYM_MESH;
    case AST_MEMORY:      return SYM_MEMORY;
    case AST_CHANNEL:     return SYM_CHANNEL;
    case AST_ROUTER:      return SYM_ROUTER;
    case AST_STRATEGY:    return SYM_STRATEGY;
    case AST_LET:         return SYM_LET;
    case AST_TYPE_ALIAS:  return SYM_TYPE_ALIAS;
    default:              return SYM_FN;  /* fallback */
    }
}

static void register_declarations(SymbolTable *st, AstNode *program,
                                  ErrorReporter *reporter, Arena *arena) {
    AstNode *decl;
    (void)arena;

    if (!program || program->kind != AST_PROGRAM) return;

    for (decl = program->params; decl; decl = decl->next) {
        switch (decl->kind) {
        case AST_FN:
        case AST_AGENT:
        case AST_TOOL:
        case AST_CAPABILITY:
        case AST_GUARD:
        case AST_GUARDSET:
        case AST_BUDGET:
        case AST_TAINT:
        case AST_STRUCT:
        case AST_ENUM:
        case AST_TRAIT:
        case AST_INTERFACE:
        case AST_CONST:
        case AST_SUPERVISOR:
        case AST_SKILL:
        case AST_PROMPT:
        case AST_MESH:
        case AST_MEMORY:
        case AST_CHANNEL:
        case AST_ROUTER:
        case AST_STRATEGY:
        case AST_TYPE_ALIAS:
            symtab_add(st, decl->name,
                       ast_kind_to_sym_kind(decl->kind),
                       decl, decl->loc, reporter);
            break;

        case AST_LET:
            /* Top-level let binding */
            symtab_add(st, decl->name, SYM_LET,
                       decl, decl->loc, reporter);
            break;

        /* Skip everything else (use, impl, module, ...) */
        default:
            break;
        }
    }
}

/* ============================================================
 * Capability Registry Builder
 * ============================================================ */

static void build_cap_registry(CapRegistry *cr, AstNode *program,
                               Arena *arena) {
    AstNode *decl;
    cr->count = 0;

    if (!program || program->kind != AST_PROGRAM) return;

    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind != AST_CAPABILITY) continue;

        const char *group = decl->name;
        AstNode *item = decl->params;  /* linked list of AST_CAPABILITY_ITEM */
        while (item) {
            if (item->name && cr->count < MAX_CAP_ITEMS) {
                CapItem *ci = &cr->items[cr->count++];
                ci->group = group;
                ci->name  = item->name;
                ci->qualified = make_qualified(arena, group, item->name);
            }
            item = item->next;
        }
    }
}

/* Check whether a qualified capability name (e.g. "web.search") exists
 * in the registry. */
static bool cap_registry_has(CapRegistry *cr, const char *qualified) {
    int i;
    for (i = 0; i < cr->count; i++) {
        if (str_eq(cr->items[i].qualified, qualified))
            return true;
    }
    return false;
}

/* ============================================================
 * Collect Agent Capabilities
 *
 * Agent fields are in agent->params (linked list of AST_FIELD).
 * Look for the "capabilities" field whose value is an AST_ARRAY.
 * Each element is an AST_IDENT whose name is "group.name".
 * ============================================================ */

static int collect_agent_caps(AstNode *agent, const char *out[],
                              int max_out) {
    int count = 0;
    AstNode *field = agent->params;
    while (field) {
        if (field->kind == AST_FIELD && str_eq(field->name, "capabilities")) {
            AstNode *arr = field->right;
            if (arr && arr->kind == AST_ARRAY) {
                AstNode *elem = arr->params;
                while (elem && count < max_out) {
                    if (elem->name) {
                        out[count++] = elem->name;
                    } else if (elem->kind == AST_IDENT && elem->val.str_val) {
                        out[count++] = elem->val.str_val;
                    }
                    elem = elem->next;
                }
            }
            break;
        }
        field = field->next;
    }
    return count;
}

/* ============================================================
 * Collect Tool Requires
 *
 * Tool fields are in tool->right (linked list of AST_FIELD).
 * Look for "requires" field whose value is an AST_ARRAY.
 * ============================================================ */

static int collect_tool_requires(AstNode *tool, const char *out[],
                                 int max_out) {
    int count = 0;
    AstNode *field = tool->right;
    while (field) {
        if (field->kind == AST_FIELD && str_eq(field->name, "requires")) {
            AstNode *arr = field->right;
            if (arr && arr->kind == AST_ARRAY) {
                AstNode *elem = arr->params;
                while (elem && count < max_out) {
                    if (elem->name) {
                        out[count++] = elem->name;
                    } else if (elem->kind == AST_IDENT && elem->val.str_val) {
                        out[count++] = elem->val.str_val;
                    }
                    elem = elem->next;
                }
            }
            break;
        }
        field = field->next;
    }
    return count;
}

/* ============================================================
 * Check whether a capability is in a list
 * ============================================================ */

static bool cap_list_contains(const char *caps[], int ncaps,
                              const char *target) {
    int i;
    for (i = 0; i < ncaps; i++) {
        if (str_eq(caps[i], target))
            return true;
    }
    return false;
}

/* ============================================================
 * AST Traversal: Find tool calls in a subtree
 *
 * We look for AST_CALL nodes where the callee is an AST_IDENT
 * that matches a known tool name.
 * ============================================================ */

typedef struct {
    const char *tool_names[256];
    int         tool_count;
} ToolCallCtx;

static void find_tool_calls_in_expr(AstNode *expr, ToolCallCtx *ctx,
                                    void (*on_call)(AstNode *call,
                                                    const char *tool_name,
                                                    void *user),
                                    void *user);

static void find_tool_calls_in_block(AstNode *block, ToolCallCtx *ctx,
                                     void (*on_call)(AstNode *call,
                                                     const char *tool_name,
                                                     void *user),
                                     void *user);

static bool is_known_tool(ToolCallCtx *ctx, const char *name) {
    int i;
    for (i = 0; i < ctx->tool_count; i++) {
        if (str_eq(ctx->tool_names[i], name))
            return true;
    }
    return false;
}

static void find_tool_calls_in_expr(AstNode *expr, ToolCallCtx *ctx,
                                    void (*on_call)(AstNode *call,
                                                    const char *tool_name,
                                                    void *user),
                                    void *user) {
    if (!expr) return;

    switch (expr->kind) {
    case AST_CALL:
        /* Check if callee is a tool */
        if (expr->left && expr->left->kind == AST_IDENT &&
            expr->left->name && is_known_tool(ctx, expr->left->name)) {
            on_call(expr, expr->left->name, user);
        }
        /* Also recurse into callee and args */
        find_tool_calls_in_expr(expr->left, ctx, on_call, user);
        {
            AstNode *arg = expr->params;
            while (arg) {
                find_tool_calls_in_expr(arg, ctx, on_call, user);
                arg = arg->next;
            }
        }
        break;

    case AST_BINARY:
        find_tool_calls_in_expr(expr->left,  ctx, on_call, user);
        find_tool_calls_in_expr(expr->right, ctx, on_call, user);
        break;

    case AST_UNARY:
        find_tool_calls_in_expr(expr->left, ctx, on_call, user);
        break;

    case AST_FIELD_ACCESS:
        find_tool_calls_in_expr(expr->left, ctx, on_call, user);
        break;

    case AST_METHOD_CALL:
        find_tool_calls_in_expr(expr->left, ctx, on_call, user);
        {
            AstNode *arg = expr->params;
            while (arg) {
                find_tool_calls_in_expr(arg, ctx, on_call, user);
                arg = arg->next;
            }
        }
        break;

    case AST_INDEX:
        find_tool_calls_in_expr(expr->left,  ctx, on_call, user);
        find_tool_calls_in_expr(expr->right, ctx, on_call, user);
        break;

    case AST_IF:
        find_tool_calls_in_expr(expr->left,   ctx, on_call, user);
        find_tool_calls_in_block(expr->right,  ctx, on_call, user);
        find_tool_calls_in_block(expr->params, ctx, on_call, user);
        break;

    case AST_PIPE:
        find_tool_calls_in_expr(expr->left,  ctx, on_call, user);
        find_tool_calls_in_expr(expr->right, ctx, on_call, user);
        break;

    case AST_CAST:
    case AST_IS_EXPR:
    case AST_REF:
    case AST_DEREF:
    case AST_TRY:
    case AST_AWAIT:
    case AST_SPAWN:
        find_tool_calls_in_expr(expr->left, ctx, on_call, user);
        break;

    case AST_CLOSURE:
        find_tool_calls_in_block(expr->left, ctx, on_call, user);
        break;

    case AST_ARRAY:
    case AST_TUPLE: {
        AstNode *el = expr->params;
        while (el) {
            find_tool_calls_in_expr(el, ctx, on_call, user);
            el = el->next;
        }
        break;
    }

    case AST_MAP: {
        AstNode *entry = expr->params;
        while (entry) {
            if (entry->kind == AST_MAP_ENTRY) {
                find_tool_calls_in_expr(entry->left,  ctx, on_call, user);
                find_tool_calls_in_expr(entry->right, ctx, on_call, user);
            }
            entry = entry->next;
        }
        break;
    }

    case AST_RANGE:
        find_tool_calls_in_expr(expr->left,  ctx, on_call, user);
        find_tool_calls_in_expr(expr->right, ctx, on_call, user);
        break;

    case AST_TRY_OTHERWISE:
        find_tool_calls_in_expr(expr->left,  ctx, on_call, user);
        find_tool_calls_in_expr(expr->right, ctx, on_call, user);
        break;

    case AST_ASK:
    case AST_TELL:
    case AST_ENSURE:
        find_tool_calls_in_expr(expr->left, ctx, on_call, user);
        break;

    case AST_MATCH:
        find_tool_calls_in_expr(expr->left, ctx, on_call, user);
        {
            AstNode *arm = expr->params;
            while (arm) {
                if (arm->kind == AST_MATCH_ARM) {
                    find_tool_calls_in_block(arm->right, ctx, on_call, user);
                }
                arm = arm->next;
            }
        }
        break;

    case AST_EACH:
    case AST_KEEP_WHERE:
    case AST_REPEAT:
    case AST_WAIT_UNTIL:
        find_tool_calls_in_expr(expr->left,  ctx, on_call, user);
        find_tool_calls_in_expr(expr->right, ctx, on_call, user);
        break;

    case AST_SELECT: {
        AstNode *arm = expr->params;
        while (arm) {
            if (arm->kind == AST_SELECT_ARM) {
                find_tool_calls_in_expr(arm->left,  ctx, on_call, user);
                find_tool_calls_in_block(arm->right, ctx, on_call, user);
            }
            arm = arm->next;
        }
        break;
    }

    /* Leaves: no children to recurse into */
    case AST_INT_LIT:
    case AST_FLOAT_LIT:
    case AST_STRING_LIT:
    case AST_BOOL_LIT:
    case AST_NONE_LIT:
    case AST_IDENT:
        break;

    default:
        /* Best effort: try common child pointers */
        find_tool_calls_in_expr(expr->left,  ctx, on_call, user);
        find_tool_calls_in_expr(expr->right, ctx, on_call, user);
        break;
    }
}

static void find_tool_calls_in_stmt(AstNode *stmt, ToolCallCtx *ctx,
                                    void (*on_call)(AstNode *call,
                                                    const char *tool_name,
                                                    void *user),
                                    void *user) {
    if (!stmt) return;

    switch (stmt->kind) {
    case AST_LET:
        find_tool_calls_in_expr(stmt->right, ctx, on_call, user);
        break;

    case AST_RETURN:
        find_tool_calls_in_expr(stmt->left, ctx, on_call, user);
        break;

    case AST_DEFER:
        find_tool_calls_in_expr(stmt->left, ctx, on_call, user);
        break;

    case AST_ASSIGN:
        find_tool_calls_in_expr(stmt->left,  ctx, on_call, user);
        find_tool_calls_in_expr(stmt->right, ctx, on_call, user);
        break;

    case AST_EXPR_STMT:
        find_tool_calls_in_expr(stmt->left, ctx, on_call, user);
        break;

    case AST_IF:
        find_tool_calls_in_expr(stmt->left, ctx, on_call, user);
        find_tool_calls_in_block(stmt->right,  ctx, on_call, user);
        find_tool_calls_in_block(stmt->params, ctx, on_call, user);
        break;

    case AST_WHILE:
        find_tool_calls_in_expr(stmt->left, ctx, on_call, user);
        find_tool_calls_in_block(stmt->right, ctx, on_call, user);
        break;

    case AST_LOOP:
        find_tool_calls_in_block(stmt->left, ctx, on_call, user);
        break;

    case AST_FOR:
        find_tool_calls_in_expr(stmt->params, ctx, on_call, user);
        find_tool_calls_in_block(stmt->right, ctx, on_call, user);
        break;

    case AST_BLOCK:
        find_tool_calls_in_block(stmt, ctx, on_call, user);
        break;

    case AST_MATCH:
        find_tool_calls_in_expr(stmt->left, ctx, on_call, user);
        {
            AstNode *arm = stmt->params;
            while (arm) {
                if (arm->kind == AST_MATCH_ARM) {
                    find_tool_calls_in_block(arm->right, ctx, on_call, user);
                }
                arm = arm->next;
            }
        }
        break;

    case AST_BREAK:
    case AST_CONTINUE:
        break;

    default:
        /* Best effort */
        find_tool_calls_in_expr(stmt->left,  ctx, on_call, user);
        find_tool_calls_in_expr(stmt->right, ctx, on_call, user);
        break;
    }
}

static void find_tool_calls_in_block(AstNode *block, ToolCallCtx *ctx,
                                     void (*on_call)(AstNode *call,
                                                     const char *tool_name,
                                                     void *user),
                                     void *user) {
    if (!block) return;

    /* A block node has statements in block->params */
    if (block->kind == AST_BLOCK) {
        AstNode *s = block->params;
        while (s) {
            find_tool_calls_in_stmt(s, ctx, on_call, user);
            s = s->next;
        }
    } else {
        /* Might be a single expression/statement used as block */
        find_tool_calls_in_stmt(block, ctx, on_call, user);
    }
}

/* Convenience: search all methods of an agent */
static void find_tool_calls_in_agent(AstNode *agent, ToolCallCtx *ctx,
                                     void (*on_call)(AstNode *call,
                                                     const char *tool_name,
                                                     void *user),
                                     void *user) {
    AstNode *method = agent->left;
    while (method) {
        if (method->kind == AST_FN && method->left) {
            find_tool_calls_in_block(method->left, ctx, on_call, user);
        }
        method = method->next;
    }
}

/* ============================================================
 * Build ToolCallCtx from the symbol table
 * ============================================================ */

static void build_tool_ctx(SymbolTable *st, ToolCallCtx *ctx) {
    int i;
    ctx->tool_count = 0;
    for (i = 0; i < st->count; i++) {
        if (st->entries[i].kind == SYM_TOOL &&
            ctx->tool_count < 256) {
            ctx->tool_names[ctx->tool_count++] = st->entries[i].name;
        }
    }
}

/* ============================================================
 * Pass 2: Capability Verification
 *
 * For each agent:
 *   - Collect its declared capabilities
 *   - Find all tool calls in its methods
 *   - For each tool call, look up the tool's "requires" list
 *   - Error if agent lacks a required capability
 *   - Also verify referenced capabilities exist in the registry
 * ============================================================ */

typedef struct {
    const char     *agent_name;
    const char    **agent_caps;
    int             agent_cap_count;
    SymbolTable    *st;
    CapRegistry    *cr;
    ErrorReporter  *reporter;
} CapCheckCtx;

static void on_cap_check_tool_call(AstNode *call, const char *tool_name,
                                   void *user) {
    CapCheckCtx *cctx = (CapCheckCtx *)user;

    /* Find the tool's AST node */
    Symbol *tool_sym = symtab_find_kind(cctx->st, tool_name, SYM_TOOL);
    if (!tool_sym) return;  /* unknown tool -- caught in type check */

    /* Collect tool's requires */
    const char *requires[MAX_TOOL_REQUIRES];
    int nreq = collect_tool_requires(tool_sym->node, requires,
                                     MAX_TOOL_REQUIRES);

    int i;
    for (i = 0; i < nreq; i++) {
        /* Check the required cap exists in registry */
        if (!cap_registry_has(cctx->cr, requires[i])) {
            report_error_fmt(cctx->reporter, call->loc,
                             "declare this capability in a 'capability' block",
                             "tool '%s' requires unknown capability '%s'",
                             tool_name, requires[i]);
        }

        /* Check agent has the required cap */
        if (!cap_list_contains(cctx->agent_caps, cctx->agent_cap_count,
                               requires[i])) {
            report_error_fmt(cctx->reporter, call->loc,
                             "add this capability to the agent's "
                             "capabilities list",
                             "agent '%s' calls tool '%s' but lacks "
                             "required capability '%s'",
                             cctx->agent_name, tool_name, requires[i]);
        }
    }
}

static void check_capabilities(SymbolTable *st, CapRegistry *cr,
                               AstNode *program, ErrorReporter *reporter,
                               Arena *arena) {
    AstNode *decl;
    ToolCallCtx tctx;

    (void)arena;

    if (!program || program->kind != AST_PROGRAM) return;

    build_tool_ctx(st, &tctx);
    if (tctx.tool_count == 0) return;  /* no tools, nothing to check */

    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind != AST_AGENT) continue;

        /* Collect this agent's capabilities */
        const char *agent_caps[MAX_AGENT_CAPS];
        int ncaps = collect_agent_caps(decl, agent_caps, MAX_AGENT_CAPS);

        /* Verify each declared capability exists in the registry */
        int ci;
        for (ci = 0; ci < ncaps; ci++) {
            if (!cap_registry_has(cr, agent_caps[ci])) {
                report_error_fmt(reporter, decl->loc,
                                 "declare this capability in a "
                                 "'capability' block",
                                 "agent '%s' references unknown "
                                 "capability '%s'",
                                 decl->name ? decl->name : "<anon>",
                                 agent_caps[ci]);
            }
        }

        /* Set up context and scan agent methods for tool calls */
        CapCheckCtx cctx;
        cctx.agent_name     = decl->name ? decl->name : "<anon>";
        cctx.agent_caps     = agent_caps;
        cctx.agent_cap_count = ncaps;
        cctx.st             = st;
        cctx.cr             = cr;
        cctx.reporter       = reporter;

        find_tool_calls_in_agent(decl, &tctx,
                                 on_cap_check_tool_call, &cctx);
    }
}

/* ============================================================
 * Pass 3: Guard Enforcement
 *
 * "Sensitive" capabilities are heuristically detected by name
 * patterns: anything containing "write", "delete", "execute",
 * "admin", "payment", "transfer", or "secret".
 *
 * For each agent that holds sensitive caps, warn if no guard or
 * guardset is referenced in its declarations.
 * ============================================================ */

static const char *sensitive_patterns[] = {
    "write",
    "delete",
    "execute",
    "exec",
    "admin",
    "payment",
    "transfer",
    "secret",
    "destroy",
    "remove",
    "drop",
    "sudo",
    "root",
    NULL
};

/* Case-insensitive substring search */
static bool contains_ci(const char *haystack, const char *needle) {
    size_t hlen, nlen, i, j;
    if (!haystack || !needle) return false;
    hlen = strlen(haystack);
    nlen = strlen(needle);
    if (nlen > hlen) return false;
    for (i = 0; i <= hlen - nlen; i++) {
        bool match = true;
        for (j = 0; j < nlen; j++) {
            char h = haystack[i + j];
            char n = needle[j];
            if (h >= 'A' && h <= 'Z') h = (char)(h + 32);
            if (n >= 'A' && n <= 'Z') n = (char)(n + 32);
            if (h != n) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

static bool is_sensitive_cap(const char *qualified) {
    int i;
    if (!qualified) return false;
    for (i = 0; sensitive_patterns[i]; i++) {
        if (contains_ci(qualified, sensitive_patterns[i]))
            return true;
    }
    return false;
}

/* Check if an agent references any guard or guardset */
static bool agent_has_guard_ref(AstNode *agent) {
    AstNode *field = agent->params;
    while (field) {
        if (field->kind == AST_FIELD) {
            if (str_eq(field->name, "guard") ||
                str_eq(field->name, "guards") ||
                str_eq(field->name, "guardset")) {
                return true;
            }
        }
        field = field->next;
    }

    /* Also check if any method has a guard attribute */
    AstNode *method = agent->left;
    while (method) {
        if (method->kind == AST_FN) {
            AstNode *attr = method->attributes;
            while (attr) {
                if (attr->kind == AST_ATTRIBUTE &&
                    (str_eq(attr->name, "guard") ||
                     str_eq(attr->name, "guarded"))) {
                    return true;
                }
                attr = attr->next;
            }
        }
        method = method->next;
    }

    return false;
}

/* Check if any guards are defined in the program for a given cap */
static bool has_guard_for_cap(SymbolTable *st, const char *cap_qualified) {
    int i;
    (void)cap_qualified;  /* heuristic: any guard at all is ok for now */
    for (i = 0; i < st->count; i++) {
        if (st->entries[i].kind == SYM_GUARD ||
            st->entries[i].kind == SYM_GUARDSET) {
            return true;
        }
    }
    return false;
}

static void check_guards(SymbolTable *st, CapRegistry *cr,
                         AstNode *program, ErrorReporter *reporter,
                         Arena *arena) {
    AstNode *decl;

    (void)arena;
    (void)cr;

    if (!program || program->kind != AST_PROGRAM) return;

    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind != AST_AGENT) continue;

        const char *agent_caps[MAX_AGENT_CAPS];
        int ncaps = collect_agent_caps(decl, agent_caps, MAX_AGENT_CAPS);

        /* Check if any cap is sensitive */
        bool has_sensitive = false;
        const char *first_sensitive = NULL;
        int ci;
        for (ci = 0; ci < ncaps; ci++) {
            if (is_sensitive_cap(agent_caps[ci])) {
                has_sensitive = true;
                first_sensitive = agent_caps[ci];
                break;
            }
        }

        if (!has_sensitive) continue;

        /* Agent has sensitive caps -- check for guards */
        bool has_guard = agent_has_guard_ref(decl);
        bool program_has_guards = has_guard_for_cap(st, first_sensitive);

        if (!has_guard && !program_has_guards) {
            report_warning_fmt(reporter, decl->loc,
                               "add a guard or guardset to protect "
                               "sensitive operations",
                               "agent '%s' uses sensitive capability "
                               "'%s' without any guard",
                               decl->name ? decl->name : "<anon>",
                               first_sensitive);
        } else if (!has_guard && program_has_guards) {
            /* Guards exist but aren't referenced by this agent --
             * this is a warning, not an error. */
            report_warning_fmt(reporter, decl->loc,
                               "consider referencing a guard in this agent's "
                               "declaration (guards exist in this program)",
                               "agent '%s' uses sensitive capability "
                               "'%s' but does not reference a guard",
                               decl->name ? decl->name : "<anon>",
                               first_sensitive);
        }
    }
}

/* ============================================================
 * Pass 4: Taint Checking
 *
 * Basic flow: detect parameters annotated with taint labels
 * (AST_TYPE_TAINTED) and warn if they flow into certain
 * dangerous contexts (system calls, exec, eval patterns).
 *
 * Strategy:
 *   - Collect all taint labels declared in the program
 *   - For each function/method, collect tainted parameters
 *   - Walk the body looking for calls to "dangerous" builtins
 *     where a tainted parameter is passed directly
 * ============================================================ */

static const char *dangerous_sinks[] = {
    "exec",
    "system",
    "eval",
    "shell",
    "run_command",
    "execute",
    "sql",
    "query",
    /* LLM/prompt sinks — tainted data should not flow here unsanitized */
    "llm_call",
    "lcn_llm_call",
    "llm_complete",
    "build_prompt",
    "system_prompt",
    NULL
};

static bool is_dangerous_sink(const char *name) {
    int i;
    if (!name) return false;
    for (i = 0; dangerous_sinks[i]; i++) {
        if (contains_ci(name, dangerous_sinks[i]))
            return true;
    }
    return false;
}

/* Collect names of tainted params from a function's parameter list */
static int collect_tainted_params(AstNode *param_list,
                                  const char *out[], int max_out) {
    int count = 0;
    AstNode *p = param_list;
    while (p && count < max_out) {
        if (p->type_expr && p->type_expr->kind == AST_TYPE_TAINTED) {
            if (p->name) {
                out[count++] = p->name;
            }
        }
        p = p->next;
    }
    return count;
}

/* Check if an expression directly references a tainted parameter */
static bool expr_uses_tainted(AstNode *expr, const char *tainted[],
                              int ntainted) {
    if (!expr) return false;

    if (expr->kind == AST_IDENT && expr->name) {
        int i;
        for (i = 0; i < ntainted; i++) {
            if (str_eq(expr->name, tainted[i]))
                return true;
        }
    }

    /* Recurse into sub-expressions */
    if (expr_uses_tainted(expr->left, tainted, ntainted))  return true;
    if (expr_uses_tainted(expr->right, tainted, ntainted)) return true;

    /* Check args */
    {
        AstNode *a = expr->params;
        while (a) {
            if (expr_uses_tainted(a, tainted, ntainted)) return true;
            a = a->next;
        }
    }

    return false;
}

/* Check if an expression is a taint source (produces tainted data) */
static bool is_taint_source(AstNode *expr) {
    if (!expr) return false;

    /* ask(...) produces @llm_output — tainted */
    if (expr->kind == AST_ASK) return true;

    /* channel.recv() produces @user_input — tainted */
    if (expr->kind == AST_METHOD_CALL && expr->name &&
        str_eq(expr->name, "recv")) return true;

    /* tell target msg — the target is user input context */
    if (expr->kind == AST_TELL) return true;

    /* Binary expression where either side is a taint source */
    if (expr->kind == AST_BINARY) {
        return is_taint_source(expr->left) || is_taint_source(expr->right);
    }

    return false;
}

/* Collect variables that are assigned from taint sources (inference) */
static int collect_inferred_tainted(AstNode *block, const char *out[],
                                     int max_out,
                                     const char *existing[], int nexisting) {
    int count = 0;
    AstNode *stmt;

    if (!block || block->kind != AST_BLOCK) return 0;

    for (stmt = block->params; stmt && count < max_out; stmt = stmt->next) {
        if (stmt->kind == AST_LET && stmt->name && stmt->right) {
            /* Check if RHS is a taint source */
            if (is_taint_source(stmt->right)) {
                out[count++] = stmt->name;
                continue;
            }

            /* Check if RHS references an already-tainted variable */
            if (expr_uses_tainted(stmt->right, existing, nexisting) ||
                (count > 0 && expr_uses_tainted(stmt->right, out, count))) {
                out[count++] = stmt->name;
                continue;
            }

            /* Check if RHS is a binary expression with tainted operand */
            if (stmt->right->kind == AST_BINARY) {
                if (expr_uses_tainted(stmt->right->left, existing, nexisting) ||
                    expr_uses_tainted(stmt->right->right, existing, nexisting) ||
                    (count > 0 &&
                     (expr_uses_tainted(stmt->right->left, out, count) ||
                      expr_uses_tainted(stmt->right->right, out, count)))) {
                    out[count++] = stmt->name;
                    continue;
                }
            }
        }
    }
    return count;
}

/* Check if an expression is an LLM call (ask, llm.complete, etc.) */
static bool is_llm_call(AstNode *expr) {
    if (!expr) return false;
    if (expr->kind == AST_ASK) return true;
    if (expr->kind == AST_CALL && expr->left &&
        expr->left->kind == AST_IDENT) {
        const char *name = expr->left->name;
        if (name && (str_eq(name, "ask") ||
                     contains_ci(name, "llm") ||
                     contains_ci(name, "complete") ||
                     contains_ci(name, "prompt"))) {
            return true;
        }
    }
    if (expr->kind == AST_METHOD_CALL && expr->name &&
        (str_eq(expr->name, "complete") || str_eq(expr->name, "call"))) {
        return true;
    }
    return false;
}

/* Check for prompt injection pattern: tainted data concatenated into string
   that is used as argument to an LLM call */
static void check_prompt_injection(AstNode *stmt, const char *all_tainted[],
                                    int ntainted, const char *fn_context,
                                    ErrorReporter *reporter) {
    AstNode *expr;
    if (!stmt || ntainted == 0) return;

    switch (stmt->kind) {
    case AST_EXPR_STMT:
        expr = stmt->left;
        if (expr && is_llm_call(expr)) {
            /* Check if any arg to the LLM call contains tainted data.
               For AST_ASK, args are in left/right, not params. */
            if (expr->kind == AST_ASK) {
                if (expr_uses_tainted(expr->left, all_tainted, ntainted) ||
                    expr_uses_tainted(expr->right, all_tainted, ntainted)) {
                    report_error_fmt(reporter, expr->loc,
                        "sanitize tainted input before passing to LLM — "
                        "this is a potential prompt injection vector",
                        "tainted data flows to LLM call in '%s'",
                        fn_context ? fn_context : "<fn>");
                }
            } else {
                AstNode *arg = expr->params;
                while (arg) {
                    if (expr_uses_tainted(arg, all_tainted, ntainted)) {
                        report_error_fmt(reporter, expr->loc,
                            "sanitize tainted input before passing to LLM — "
                            "this is a potential prompt injection vector",
                            "tainted data flows to LLM call in '%s'",
                            fn_context ? fn_context : "<fn>");
                        break;
                    }
                    arg = arg->next;
                }
            }
        }
        break;

    case AST_LET:
        /* let x = ask(tainted_var) or let x = llm_call(..., tainted) */
        if (stmt->right && is_llm_call(stmt->right)) {
            if (stmt->right->kind == AST_ASK) {
                if (expr_uses_tainted(stmt->right->left, all_tainted,
                                      ntainted) ||
                    expr_uses_tainted(stmt->right->right, all_tainted,
                                      ntainted)) {
                    report_error_fmt(reporter, stmt->right->loc,
                        "sanitize tainted input before passing to LLM — "
                        "this is a potential prompt injection vector",
                        "tainted data flows to LLM call in '%s'",
                        fn_context ? fn_context : "<fn>");
                }
            } else {
                AstNode *arg = stmt->right->params;
                while (arg) {
                    if (expr_uses_tainted(arg, all_tainted, ntainted)) {
                        report_error_fmt(reporter, stmt->right->loc,
                            "sanitize tainted input before passing to LLM — "
                            "this is a potential prompt injection vector",
                            "tainted data flows to LLM call in '%s'",
                            fn_context ? fn_context : "<fn>");
                        break;
                    }
                    arg = arg->next;
                }
            }
        }
        break;

    case AST_RETURN:
        /* Recurse into return value */
        if (stmt->left && is_llm_call(stmt->left)) {
            if (stmt->left->kind == AST_ASK) {
                if (expr_uses_tainted(stmt->left->left, all_tainted,
                                      ntainted) ||
                    expr_uses_tainted(stmt->left->right, all_tainted,
                                      ntainted)) {
                    report_error_fmt(reporter, stmt->left->loc,
                        "sanitize tainted input before passing to LLM",
                        "tainted data flows to LLM call in '%s'",
                        fn_context ? fn_context : "<fn>");
                }
            } else {
                AstNode *arg = stmt->left->params;
                while (arg) {
                    if (expr_uses_tainted(arg, all_tainted, ntainted)) {
                        report_error_fmt(reporter, stmt->left->loc,
                            "sanitize tainted input before passing to LLM",
                            "tainted data flows to LLM call in '%s'",
                            fn_context ? fn_context : "<fn>");
                        break;
                    }
                    arg = arg->next;
                }
            }
        }
        break;

    case AST_BLOCK: {
        AstNode *s = stmt->params;
        while (s) {
            check_prompt_injection(s, all_tainted, ntainted,
                                   fn_context, reporter);
            s = s->next;
        }
        break;
    }

    case AST_IF:
        check_prompt_injection(stmt->right, all_tainted, ntainted,
                               fn_context, reporter);
        check_prompt_injection(stmt->params, all_tainted, ntainted,
                               fn_context, reporter);
        break;

    default:
        break;
    }
}

/* Walk statements looking for dangerous calls with tainted args */
static void check_taint_in_stmt(AstNode *stmt, const char *tainted[],
                                int ntainted, const char *fn_context,
                                ErrorReporter *reporter) {
    if (!stmt || ntainted == 0) return;

    switch (stmt->kind) {
    case AST_EXPR_STMT:
    case AST_RETURN:
    case AST_DEFER: {
        AstNode *expr = (stmt->kind == AST_EXPR_STMT) ? stmt->left :
                        (stmt->kind == AST_RETURN)    ? stmt->left :
                        stmt->left;
        if (expr && expr->kind == AST_CALL &&
            expr->left && expr->left->kind == AST_IDENT &&
            is_dangerous_sink(expr->left->name)) {
            /* Check if any arg uses tainted data */
            AstNode *arg = expr->params;
            while (arg) {
                if (expr_uses_tainted(arg, tainted, ntainted)) {
                    report_error_fmt(reporter, expr->loc,
                                     "sanitize tainted input before "
                                     "passing to this function",
                                     "tainted data flows to dangerous "
                                     "sink '%s' in '%s'",
                                     expr->left->name,
                                     fn_context ? fn_context : "<fn>");
                    break;
                }
                arg = arg->next;
            }
        }
        break;
    }

    case AST_LET:
        /* A let that calls a dangerous sink */
        if (stmt->right && stmt->right->kind == AST_CALL &&
            stmt->right->left && stmt->right->left->kind == AST_IDENT &&
            is_dangerous_sink(stmt->right->left->name)) {
            AstNode *arg = stmt->right->params;
            while (arg) {
                if (expr_uses_tainted(arg, tainted, ntainted)) {
                    report_error_fmt(reporter, stmt->right->loc,
                                     "sanitize tainted input before "
                                     "passing to this function",
                                     "tainted data flows to dangerous "
                                     "sink '%s' in '%s'",
                                     stmt->right->left->name,
                                     fn_context ? fn_context : "<fn>");
                    break;
                }
                arg = arg->next;
            }
        }
        break;

    case AST_IF:
        check_taint_in_stmt(stmt->right,  tainted, ntainted,
                            fn_context, reporter);
        check_taint_in_stmt(stmt->params, tainted, ntainted,
                            fn_context, reporter);
        break;

    case AST_WHILE:
        check_taint_in_stmt(stmt->right, tainted, ntainted,
                            fn_context, reporter);
        break;

    case AST_LOOP:
        check_taint_in_stmt(stmt->left, tainted, ntainted,
                            fn_context, reporter);
        break;

    case AST_FOR:
        check_taint_in_stmt(stmt->right, tainted, ntainted,
                            fn_context, reporter);
        break;

    case AST_BLOCK: {
        AstNode *s = stmt->params;
        while (s) {
            check_taint_in_stmt(s, tainted, ntainted,
                                fn_context, reporter);
            s = s->next;
        }
        break;
    }

    case AST_ASSIGN:
        if (stmt->right && stmt->right->kind == AST_CALL &&
            stmt->right->left && stmt->right->left->kind == AST_IDENT &&
            is_dangerous_sink(stmt->right->left->name)) {
            AstNode *arg = stmt->right->params;
            while (arg) {
                if (expr_uses_tainted(arg, tainted, ntainted)) {
                    report_error_fmt(reporter, stmt->right->loc,
                                     "sanitize tainted input before "
                                     "passing to this function",
                                     "tainted data flows to dangerous "
                                     "sink '%s' in '%s'",
                                     stmt->right->left->name,
                                     fn_context ? fn_context : "<fn>");
                    break;
                }
                arg = arg->next;
            }
        }
        break;

    case AST_MATCH: {
        AstNode *arm = stmt->params;
        while (arm) {
            if (arm->kind == AST_MATCH_ARM) {
                check_taint_in_stmt(arm->right, tainted, ntainted,
                                    fn_context, reporter);
            }
            arm = arm->next;
        }
        break;
    }

    default:
        break;
    }
}

static void check_taint_in_fn(AstNode *fn, const char *context_name,
                              ErrorReporter *reporter) {
    const char *tainted[MAX_TAINTED_PARAMS];
    const char *inferred[MAX_TAINTED_PARAMS];
    const char *all_tainted[MAX_TAINTED_PARAMS * 2];
    int ntainted, ninferred, nall, i;

    /* Collect explicitly annotated tainted params */
    ntainted = collect_tainted_params(fn->params, tainted,
                                      MAX_TAINTED_PARAMS);

    /* Collect inferred tainted variables from function body */
    ninferred = 0;
    if (fn->left) {
        ninferred = collect_inferred_tainted(fn->left, inferred,
                                             MAX_TAINTED_PARAMS,
                                             tainted, ntainted);
    }

    /* Merge both lists */
    nall = 0;
    for (i = 0; i < ntainted && nall < MAX_TAINTED_PARAMS * 2; i++)
        all_tainted[nall++] = tainted[i];
    for (i = 0; i < ninferred && nall < MAX_TAINTED_PARAMS * 2; i++)
        all_tainted[nall++] = inferred[i];

    if (nall == 0) return;

    /* Original check: tainted data flowing to dangerous sinks */
    if (fn->left) {
        check_taint_in_stmt(fn->left, all_tainted, nall,
                            context_name, reporter);
    }

    /* NEW: Check for prompt injection patterns */
    if (fn->left) {
        check_prompt_injection(fn->left, all_tainted, nall,
                               context_name, reporter);
    }
}

static void check_taint(SymbolTable *st, AstNode *program,
                        ErrorReporter *reporter, Arena *arena) {
    AstNode *decl;

    (void)st;
    (void)arena;

    if (!program || program->kind != AST_PROGRAM) return;

    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_FN) {
            check_taint_in_fn(decl, decl->name, reporter);
        }

        if (decl->kind == AST_AGENT) {
            /* Check agent methods */
            AstNode *method = decl->left;
            while (method) {
                if (method->kind == AST_FN) {
                    /* Build context name "AgentName.method_name" */
                    char ctx_buf[512];
                    int n = snprintf(ctx_buf, sizeof(ctx_buf), "%s.%s",
                                     decl->name ? decl->name : "<agent>",
                                     method->name ? method->name : "<fn>");
                    if (n < 0 || (size_t)n >= sizeof(ctx_buf)) {
                        ctx_buf[sizeof(ctx_buf) - 1] = '\0';
                    }
                    check_taint_in_fn(method, ctx_buf, reporter);
                }
                method = method->next;
            }
        }

        if (decl->kind == AST_TOOL) {
            /* Tool implementations can also have tainted params */
            check_taint_in_fn(decl, decl->name, reporter);
        }
    }
}

/* ============================================================
 * Pass 4b: Secret Tracking  (compile-time confidentiality)
 *
 * Variables typed as `secret T` must not leak to output sinks:
 *   - println, print, log_info, log_warn, log_error, log_debug
 *   - the prompt argument of ask()
 *   - assignment to a non-secret variable
 *   - return from a function without secret return type
 *
 * secret_redact(v)  is the approved escape hatch (returns "[REDACTED]").
 * secret_unwrap(v)  explicitly extracts the value (marks intent).
 *
 * Design: compile-time only — same approach as taint tracking.
 * ============================================================ */

#define MAX_SECRET_VARS 256

/* Output sinks that must never receive secret data */
static bool is_secret_output_sink(const char *name) {
    if (!name) return false;
    return str_eq(name, "println") || str_eq(name, "print") ||
           str_eq(name, "log_info") || str_eq(name, "log_warn") ||
           str_eq(name, "log_error") || str_eq(name, "log_debug");
}

/* Check whether a type expression contains `secret` */
static bool type_is_secret(AstNode *type_expr) {
    if (!type_expr) return false;
    return type_expr->kind == AST_TYPE_SECRET;
}

/* Collect names of secret-typed variables from let bindings and params */
static int collect_secret_params(AstNode *param_list,
                                 const char *out[], int max) {
    int count = 0;
    AstNode *p;
    for (p = param_list; p && count < max; p = p->next) {
        if (p->type_expr && type_is_secret(p->type_expr) && p->name)
            out[count++] = p->name;
    }
    return count;
}

/* Collect secret-typed variables from let statements in a block */
static int collect_secret_lets(AstNode *block, const char *out[], int max) {
    int count = 0;
    AstNode *stmt;
    if (!block) return 0;
    for (stmt = (block->kind == AST_BLOCK) ? block->params : block;
         stmt && count < max; stmt = stmt->next) {
        if (stmt->kind == AST_LET && stmt->type_expr &&
            type_is_secret(stmt->type_expr) && stmt->name)
            out[count++] = stmt->name;
    }
    return count;
}

/* Check if an expression directly references a secret variable */
static bool expr_uses_secret(AstNode *expr, const char *secrets[],
                             int nsecrets) {
    int i;
    AstNode *a;
    if (!expr || nsecrets == 0) return false;

    if (expr->kind == AST_IDENT && expr->name) {
        for (i = 0; i < nsecrets; i++) {
            if (str_eq(expr->name, secrets[i]))
                return true;
        }
    }

    /* Recurse into sub-expressions */
    if (expr_uses_secret(expr->left, secrets, nsecrets))  return true;
    if (expr_uses_secret(expr->right, secrets, nsecrets)) return true;

    /* Check call arguments (but exclude secret_redact / secret_unwrap) */
    if (expr->kind == AST_CALL && expr->left &&
        expr->left->kind == AST_IDENT && expr->left->name) {
        if (str_eq(expr->left->name, "secret_redact") ||
            str_eq(expr->left->name, "secret_unwrap"))
            return false;  /* these are approved escape hatches */
    }

    for (a = expr->params; a; a = a->next) {
        if (expr_uses_secret(a, secrets, nsecrets)) return true;
    }
    return false;
}

/* Walk a function body checking for secret leaks */
static void check_secret_in_stmt(AstNode *stmt, const char *secrets[],
                                  int nsecrets, const char *fn_context,
                                  ErrorReporter *reporter) {
    if (!stmt || nsecrets == 0) return;

    switch (stmt->kind) {
    case AST_EXPR_STMT:
    case AST_DEFER: {
        AstNode *expr = stmt->left;
        if (expr && expr->kind == AST_CALL &&
            expr->left && expr->left->kind == AST_IDENT) {
            const char *fn = expr->left->name;
            /* Check output sinks */
            if (is_secret_output_sink(fn)) {
                AstNode *arg = expr->params;
                while (arg) {
                    if (expr_uses_secret(arg, secrets, nsecrets)) {
                        report_error_fmt(reporter, expr->loc,
                            "use secret_redact() to safely log secret data",
                            "secret value leaked to output sink '%s' in '%s'",
                            fn, fn_context ? fn_context : "<fn>");
                        break;
                    }
                    arg = arg->next;
                }
            }
            /* Check ask() — secret must not flow to prompt argument */
            if (str_eq(fn, "ask")) {
                AstNode *arg = expr->params;
                while (arg) {
                    if (expr_uses_secret(arg, secrets, nsecrets)) {
                        report_error_fmt(reporter, expr->loc,
                            "use secret_redact() before passing to ask()",
                            "secret value leaked to ask() prompt in '%s'",
                            fn_context ? fn_context : "<fn>");
                        break;
                    }
                    arg = arg->next;
                }
            }
        }
        break;
    }

    case AST_LET:
        /* Check assignment to non-secret variable from secret expression */
        if (stmt->right && stmt->name && !type_is_secret(stmt->type_expr)) {
            if (expr_uses_secret(stmt->right, secrets, nsecrets)) {
                /* Allow secret_redact() and secret_unwrap() calls */
                if (stmt->right->kind == AST_CALL && stmt->right->left &&
                    stmt->right->left->kind == AST_IDENT &&
                    (str_eq(stmt->right->left->name, "secret_redact") ||
                     str_eq(stmt->right->left->name, "secret_unwrap"))) {
                    break;  /* OK — explicit extraction */
                }
                report_error_fmt(reporter, stmt->loc,
                    "declare the target as 'secret' type or use secret_unwrap()",
                    "secret value assigned to non-secret variable '%s' in '%s'",
                    stmt->name, fn_context ? fn_context : "<fn>");
            }
        }
        /* Check if let RHS calls an output sink with secret args */
        if (stmt->right && stmt->right->kind == AST_CALL &&
            stmt->right->left && stmt->right->left->kind == AST_IDENT) {
            const char *fn = stmt->right->left->name;
            if (is_secret_output_sink(fn)) {
                AstNode *arg = stmt->right->params;
                while (arg) {
                    if (expr_uses_secret(arg, secrets, nsecrets)) {
                        report_error_fmt(reporter, stmt->right->loc,
                            "use secret_redact() to safely log secret data",
                            "secret value leaked to output sink '%s' in '%s'",
                            fn, fn_context ? fn_context : "<fn>");
                        break;
                    }
                    arg = arg->next;
                }
            }
            if (str_eq(fn, "ask")) {
                AstNode *arg = stmt->right->params;
                while (arg) {
                    if (expr_uses_secret(arg, secrets, nsecrets)) {
                        report_error_fmt(reporter, stmt->right->loc,
                            "use secret_redact() before passing to ask()",
                            "secret value leaked to ask() prompt in '%s'",
                            fn_context ? fn_context : "<fn>");
                        break;
                    }
                    arg = arg->next;
                }
            }
        }
        break;

    case AST_RETURN:
        /* Secret value returned from function without secret return type
         * (checked at function level, not here) */
        break;

    case AST_IF:
        check_secret_in_stmt(stmt->right,  secrets, nsecrets,
                             fn_context, reporter);
        check_secret_in_stmt(stmt->params, secrets, nsecrets,
                             fn_context, reporter);
        break;

    case AST_WHILE:
        check_secret_in_stmt(stmt->right, secrets, nsecrets,
                             fn_context, reporter);
        break;

    case AST_LOOP:
        check_secret_in_stmt(stmt->left, secrets, nsecrets,
                             fn_context, reporter);
        break;

    case AST_FOR:
        check_secret_in_stmt(stmt->right, secrets, nsecrets,
                             fn_context, reporter);
        break;

    case AST_BLOCK: {
        AstNode *s = stmt->params;
        while (s) {
            check_secret_in_stmt(s, secrets, nsecrets,
                                 fn_context, reporter);
            s = s->next;
        }
        break;
    }

    case AST_ASSIGN:
        if (stmt->right && stmt->right->kind == AST_CALL &&
            stmt->right->left && stmt->right->left->kind == AST_IDENT) {
            const char *fn = stmt->right->left->name;
            if (is_secret_output_sink(fn)) {
                AstNode *arg = stmt->right->params;
                while (arg) {
                    if (expr_uses_secret(arg, secrets, nsecrets)) {
                        report_error_fmt(reporter, stmt->right->loc,
                            "use secret_redact() to safely log secret data",
                            "secret value leaked to output sink '%s' in '%s'",
                            fn, fn_context ? fn_context : "<fn>");
                        break;
                    }
                    arg = arg->next;
                }
            }
        }
        break;

    case AST_MATCH: {
        AstNode *arm = stmt->params;
        while (arm) {
            if (arm->kind == AST_MATCH_ARM) {
                check_secret_in_stmt(arm->right, secrets, nsecrets,
                                     fn_context, reporter);
            }
            arm = arm->next;
        }
        break;
    }

    default:
        break;
    }
}

/* Check a single function for secret leaks */
static void check_secret_in_fn(AstNode *fn, const char *context_name,
                                ErrorReporter *reporter) {
    const char *from_params[MAX_SECRET_VARS];
    const char *from_lets[MAX_SECRET_VARS];
    const char *all_secrets[MAX_SECRET_VARS * 2];
    int np, nl, nall, i;

    /* Collect secret-typed parameters */
    np = collect_secret_params(fn->params, from_params, MAX_SECRET_VARS);

    /* Collect secret-typed let bindings in body */
    nl = 0;
    if (fn->left) {
        nl = collect_secret_lets(fn->left, from_lets, MAX_SECRET_VARS);
    }

    /* Merge */
    nall = 0;
    for (i = 0; i < np && nall < MAX_SECRET_VARS * 2; i++)
        all_secrets[nall++] = from_params[i];
    for (i = 0; i < nl && nall < MAX_SECRET_VARS * 2; i++)
        all_secrets[nall++] = from_lets[i];

    if (nall == 0) return;

    /* Check function body for leaks */
    if (fn->left) {
        check_secret_in_stmt(fn->left, all_secrets, nall,
                             context_name, reporter);
    }
}

/* Top-level driver for secret checking */
static void check_secrets(SymbolTable *st, AstNode *program,
                          ErrorReporter *reporter, Arena *arena) {
    AstNode *decl;

    (void)st;
    (void)arena;

    if (!program || program->kind != AST_PROGRAM) return;

    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_FN) {
            check_secret_in_fn(decl, decl->name, reporter);
        }

        if (decl->kind == AST_AGENT) {
            AstNode *method = decl->left;
            while (method) {
                if (method->kind == AST_FN) {
                    char ctx_buf[512];
                    int n = snprintf(ctx_buf, sizeof(ctx_buf), "%s.%s",
                                     decl->name ? decl->name : "<agent>",
                                     method->name ? method->name : "<fn>");
                    if (n < 0 || (size_t)n >= sizeof(ctx_buf)) {
                        ctx_buf[sizeof(ctx_buf) - 1] = '\0';
                    }
                    check_secret_in_fn(method, ctx_buf, reporter);
                }
                method = method->next;
            }
        }

        if (decl->kind == AST_TOOL) {
            check_secret_in_fn(decl, decl->name, reporter);
        }
    }
}

/* ============================================================
 * Pass 5: Budget Presence
 *
 * Every agent that makes tool calls must have a budget field.
 * Without a budget, an agent could consume unlimited resources.
 * ============================================================ */

typedef struct {
    bool found_tool_call;
} BudgetScanCtx;

static void on_budget_scan_tool_call(AstNode *call, const char *tool_name,
                                     void *user) {
    BudgetScanCtx *bctx = (BudgetScanCtx *)user;
    (void)call;
    (void)tool_name;
    bctx->found_tool_call = true;
}

static bool agent_has_budget_field(AstNode *agent) {
    AstNode *field = agent->params;
    while (field) {
        if (field->kind == AST_FIELD && str_eq(field->name, "budget"))
            return true;
        field = field->next;
    }
    return false;
}

static void check_budgets(SymbolTable *st, AstNode *program,
                          ErrorReporter *reporter, Arena *arena) {
    AstNode *decl;
    ToolCallCtx tctx;

    (void)arena;

    if (!program || program->kind != AST_PROGRAM) return;

    build_tool_ctx(st, &tctx);
    if (tctx.tool_count == 0) return;

    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind != AST_AGENT) continue;

        /* Scan for tool calls */
        BudgetScanCtx bctx;
        bctx.found_tool_call = false;
        find_tool_calls_in_agent(decl, &tctx,
                                 on_budget_scan_tool_call, &bctx);

        if (bctx.found_tool_call && !agent_has_budget_field(decl)) {
            report_error_fmt(reporter, decl->loc,
                             "add a 'budget' field to limit resource "
                             "consumption",
                             "agent '%s' calls tools but has no budget",
                             decl->name ? decl->name : "<anon>");
        }
    }
}

/* ============================================================
 * Pass 6: Basic Type Checking
 *
 * - AST_CALL: verify callee name exists in symbol table
 * - AST_BINARY: verify operands are both present
 * - AST_FIELD_ACCESS: verify object is present
 * - AST_METHOD_CALL: verify object is present
 * ============================================================ */

/* Is a name a language builtin that does not need declaration? */
static bool is_builtin_name(const char *name) {
    static const char *builtins[] = {
        "print", "println", "printf", "sprintf", "format",
        "len", "append", "push", "pop", "insert", "remove",
        "contains", "keys", "values",
        "starts_with", "ends_with", "env",
        "json_parse", "json_get", "json_get_number", "json_array_len", "json_array_get", "json_stringify",
        "str_eq", "str_replace", "str_trim", "str_substring", "str_split",
        "to_string", "to_int", "to_float", "parse",
        "read", "write", "open", "close",
        "sleep", "now", "elapsed",
        "assert", "panic", "unreachable",
        "Some", "None", "Ok", "Err",
        "true", "false",
        "min", "max", "abs", "sqrt", "pow",
        "map", "filter", "reduce", "fold",
        "sort", "reverse", "zip", "enumerate",
        "range", "type_of", "size_of",
        "channel", "select",
        "fetch", "exec",
        "read_file", "write_file", "read_line",
        "log_info", "log_warn", "log_error", "log_debug",
        "sql_escape", "unwrap",
        "vec_new", "vec_push", "vec_get", "vec_len",
        "char_at", "char_code", "str_from_code", "str_slice",
        "str_len", "str_find", "str_char_is_alpha", "str_char_is_digit",
        "str_char_is_alnum", "to_char_code",
        "arg", "arg_count",
        "sb_new", "sb_append", "sb_to_string", "sb_peek",
        "delegate", "revoke", "revoke_all", "has_capability",
        "secret_redact", "secret_unwrap",
        NULL
    };
    int i;
    if (!name) return false;
    for (i = 0; builtins[i]; i++) {
        if (strcmp(name, builtins[i]) == 0)
            return true;
    }
    return false;
}

/* Is a name a builtin type? */
static bool is_builtin_type(const char *name) {
    static const char *types[] = {
        "i8", "i16", "i32", "i64", "i128",
        "u8", "u16", "u32", "u64", "u128",
        "f32", "f64",
        "int", "float",
        "bool", "void", "string", "String",
        "Result", "Vec", "List", "Option", "Map", "Set",
        "char", "byte", "usize", "isize",
        NULL
    };
    int i;
    if (!name) return false;
    for (i = 0; types[i]; i++) {
        if (strcmp(name, types[i]) == 0)
            return true;
    }
    return false;
}

/* Check a type expression references a known type */
static void check_type_expr(SymbolTable *st, AstNode *type_expr,
                            ErrorReporter *reporter, Arena *arena) {
    if (!type_expr) return;

    switch (type_expr->kind) {
    case AST_TYPE_NAMED:
        if (type_expr->name && !is_builtin_type(type_expr->name)) {
            /* Check for user-defined type */
            Symbol *sym = symtab_find(st, type_expr->name);
            if (!sym ||
                (sym->kind != SYM_STRUCT &&
                 sym->kind != SYM_ENUM &&
                 sym->kind != SYM_TRAIT &&
                 sym->kind != SYM_INTERFACE &&
                 sym->kind != SYM_TYPE_ALIAS &&
                 sym->kind != SYM_AGENT &&
                 sym->kind != SYM_TAINT)) {
                report_error_fmt(reporter, type_expr->loc,
                                 "check spelling or add an import",
                                 "unknown type '%s'",
                                 type_expr->name);
            }
        }
        /* Check generic args and validate arity for built-in generic types */
        if (type_expr->name && type_expr->generics) {
            int gcount = ast_list_len(type_expr->generics);

            if (strcmp(type_expr->name, "Result") == 0) {
                if (gcount > 2) {
                    report_error_fmt(reporter, type_expr->loc,
                                     "Result takes at most 2 type parameters: Result<T, E>",
                                     "too many type parameters for Result (got %d, max 2)",
                                     gcount);
                }
                /* Result<T> with 1 param → default E to string.
                 * Add a synthetic AST_TYPE_NAMED "string" node as the second generic arg. */
                if (gcount == 1) {
                    SourceLoc sloc = type_expr->generics->loc;
                    AstNode *default_e = ast_new(arena, AST_TYPE_NAMED, sloc);
                    default_e->name = "string";
                    type_expr->generics->next = default_e;
                }
            } else if (strcmp(type_expr->name, "Option") == 0) {
                if (gcount > 1) {
                    report_error_fmt(reporter, type_expr->loc,
                                     "Option takes exactly 1 type parameter: Option<T>",
                                     "too many type parameters for Option (got %d, max 1)",
                                     gcount);
                }
            } else if (strcmp(type_expr->name, "Vec") == 0 ||
                       strcmp(type_expr->name, "List") == 0) {
                if (gcount > 1) {
                    report_error_fmt(reporter, type_expr->loc,
                                     "Vec takes at most 1 type parameter: Vec<T>",
                                     "too many type parameters for %s (got %d, max 1)",
                                     type_expr->name, gcount);
                }
            }
        }
        /* Check each generic arg recursively */
        {
            AstNode *ga = type_expr->generics;
            while (ga) {
                check_type_expr(st, ga, reporter, arena);
                ga = ga->next;
            }
        }
        break;

    case AST_TYPE_REF:
    case AST_TYPE_PTR:
    case AST_TYPE_OPTIONAL:
    case AST_TYPE_SLICE:
        check_type_expr(st, type_expr->left, reporter, arena);
        break;

    case AST_TYPE_ARRAY:
        check_type_expr(st, type_expr->left, reporter, arena);
        break;

    case AST_TYPE_TUPLE:
    case AST_TYPE_UNION: {
        AstNode *v = type_expr->params;
        while (v) {
            check_type_expr(st, v, reporter, arena);
            v = v->next;
        }
        break;
    }

    case AST_TYPE_FN:
        check_type_expr(st, type_expr->type_expr, reporter, arena);
        {
            AstNode *pt = type_expr->params;
            while (pt) {
                check_type_expr(st, pt, reporter, arena);
                pt = pt->next;
            }
        }
        break;

    case AST_TYPE_TAINTED:
        check_type_expr(st, type_expr->left, reporter, arena);
        break;

    case AST_TYPE_INFER:
        /* Nothing to check */
        break;

    default:
        break;
    }
}

/* Forward declarations for mutually recursive check_expr / check_stmt */
static void check_stmt(SymbolTable *st, AstNode *stmt,
                       ErrorReporter *reporter, Arena *arena);
static void check_expr(SymbolTable *st, AstNode *expr,
                       ErrorReporter *reporter, Arena *arena);

/* Check an expression for type errors */
static void check_expr(SymbolTable *st, AstNode *expr,
                       ErrorReporter *reporter, Arena *arena) {
    if (!expr) return;

    switch (expr->kind) {
    case AST_CALL:
        /* Verify callee exists */
        if (expr->left && expr->left->kind == AST_IDENT &&
            expr->left->name) {
            const char *callee = expr->left->name;
            if (!is_builtin_name(callee) &&
                !symtab_find(st, callee)) {
                report_error_fmt(reporter, expr->loc,
                                 "check spelling or add a declaration",
                                 "call to undeclared function '%s'",
                                 callee);
            }
        }
        /* Check args */
        check_expr(st, expr->left, reporter, arena);
        {
            AstNode *arg = expr->params;
            while (arg) {
                check_expr(st, arg, reporter, arena);
                arg = arg->next;
            }
        }
        break;

    case AST_BINARY:
        if (!expr->left) {
            report_error(reporter, expr->loc,
                         "binary operator missing left operand",
                         "add an expression before the operator");
        }
        if (!expr->right) {
            report_error(reporter, expr->loc,
                         "binary operator missing right operand",
                         "add an expression after the operator");
        }
        check_expr(st, expr->left,  reporter, arena);
        check_expr(st, expr->right, reporter, arena);
        break;

    case AST_UNARY:
        if (!expr->left) {
            report_error(reporter, expr->loc,
                         "unary operator missing operand",
                         "add an expression after the operator");
        }
        check_expr(st, expr->left, reporter, arena);
        break;

    case AST_FIELD_ACCESS:
        if (!expr->left) {
            report_error(reporter, expr->loc,
                         "field access missing object",
                         "add an expression before '.'");
        }
        check_expr(st, expr->left, reporter, arena);
        break;

    case AST_METHOD_CALL:
        if (!expr->left) {
            report_error(reporter, expr->loc,
                         "method call missing object",
                         "add an expression before '.'");
        }
        check_expr(st, expr->left, reporter, arena);
        {
            AstNode *arg = expr->params;
            while (arg) {
                check_expr(st, arg, reporter, arena);
                arg = arg->next;
            }
        }
        break;

    case AST_INDEX:
        if (!expr->left) {
            report_error(reporter, expr->loc,
                         "index expression missing object",
                         "add an expression before '['");
        }
        if (!expr->right) {
            report_error(reporter, expr->loc,
                         "index expression missing index",
                         "add an expression inside '[]'");
        }
        check_expr(st, expr->left,  reporter, arena);
        check_expr(st, expr->right, reporter, arena);
        break;

    case AST_IF:
        check_expr(st, expr->left, reporter, arena);
        check_stmt(st, expr->right, reporter, arena);
        check_stmt(st, expr->params, reporter, arena);
        break;

    case AST_CAST:
    case AST_IS_EXPR:
        check_expr(st, expr->left, reporter, arena);
        check_type_expr(st, expr->type_expr, reporter, arena);
        break;

    case AST_REF:
    case AST_DEREF:
    case AST_TRY:
    case AST_AWAIT:
    case AST_SPAWN:
        check_expr(st, expr->left, reporter, arena);
        break;

    case AST_PIPE:
        check_expr(st, expr->left,  reporter, arena);
        check_expr(st, expr->right, reporter, arena);
        break;

    case AST_ARRAY:
    case AST_TUPLE: {
        AstNode *el = expr->params;
        while (el) {
            check_expr(st, el, reporter, arena);
            el = el->next;
        }
        break;
    }

    case AST_MAP: {
        AstNode *entry = expr->params;
        while (entry) {
            if (entry->kind == AST_MAP_ENTRY) {
                check_expr(st, entry->left,  reporter, arena);
                check_expr(st, entry->right, reporter, arena);
            }
            entry = entry->next;
        }
        break;
    }

    case AST_RANGE:
        check_expr(st, expr->left,  reporter, arena);
        check_expr(st, expr->right, reporter, arena);
        break;

    case AST_CLOSURE:
        /* Check param types */
        {
            AstNode *cp = expr->params;
            while (cp) {
                check_type_expr(st, cp->type_expr, reporter, arena);
                cp = cp->next;
            }
        }
        check_stmt(st, expr->left, reporter, arena);
        break;

    case AST_MATCH:
        check_expr(st, expr->left, reporter, arena);
        {
            AstNode *arm = expr->params;
            while (arm) {
                if (arm->kind == AST_MATCH_ARM) {
                    check_stmt(st, arm->right, reporter, arena);
                }
                arm = arm->next;
            }
        }
        break;

    case AST_ASK:
    case AST_TELL:
    case AST_ENSURE:
        check_expr(st, expr->left, reporter, arena);
        break;

    case AST_TRY_OTHERWISE:
        check_expr(st, expr->left,  reporter, arena);
        check_expr(st, expr->right, reporter, arena);
        break;

    case AST_EACH:
    case AST_KEEP_WHERE:
    case AST_REPEAT:
    case AST_WAIT_UNTIL:
        check_expr(st, expr->left,  reporter, arena);
        check_expr(st, expr->right, reporter, arena);
        break;

    case AST_SELECT: {
        AstNode *arm = expr->params;
        while (arm) {
            if (arm->kind == AST_SELECT_ARM) {
                check_expr(st, arm->left,  reporter, arena);
                check_stmt(st, arm->right, reporter, arena);
            }
            arm = arm->next;
        }
        break;
    }

    /* Leaves */
    case AST_INT_LIT:
    case AST_FLOAT_LIT:
    case AST_STRING_LIT:
    case AST_BOOL_LIT:
    case AST_NONE_LIT:
    case AST_IDENT:
        break;

    default:
        /* Best effort for unknown nodes */
        check_expr(st, expr->left,  reporter, arena);
        check_expr(st, expr->right, reporter, arena);
        break;
    }
}

static void check_stmt(SymbolTable *st, AstNode *stmt,
                       ErrorReporter *reporter, Arena *arena) {
    if (!stmt) return;

    switch (stmt->kind) {
    case AST_LET:
        check_type_expr(st, stmt->type_expr, reporter, arena);
        check_expr(st, stmt->right, reporter, arena);
        break;

    case AST_RETURN:
        check_expr(st, stmt->left, reporter, arena);
        break;

    case AST_DEFER:
        check_expr(st, stmt->left, reporter, arena);
        break;

    case AST_ASSIGN:
        check_expr(st, stmt->left,  reporter, arena);
        check_expr(st, stmt->right, reporter, arena);
        break;

    case AST_EXPR_STMT:
        check_expr(st, stmt->left, reporter, arena);
        break;

    case AST_IF:
        check_expr(st, stmt->left, reporter, arena);
        check_stmt(st, stmt->right,  reporter, arena);
        check_stmt(st, stmt->params, reporter, arena);
        break;

    case AST_WHILE:
        check_expr(st, stmt->left, reporter, arena);
        check_stmt(st, stmt->right, reporter, arena);
        break;

    case AST_LOOP:
        check_stmt(st, stmt->left, reporter, arena);
        break;

    case AST_FOR:
        check_expr(st, stmt->params, reporter, arena);
        check_stmt(st, stmt->right,  reporter, arena);
        break;

    case AST_BLOCK: {
        AstNode *s = stmt->params;
        while (s) {
            check_stmt(st, s, reporter, arena);
            s = s->next;
        }
        break;
    }

    case AST_MATCH:
        check_expr(st, stmt->left, reporter, arena);
        {
            AstNode *arm = stmt->params;
            while (arm) {
                if (arm->kind == AST_MATCH_ARM) {
                    check_stmt(st, arm->right, reporter, arena);
                }
                arm = arm->next;
            }
        }
        break;

    case AST_BREAK:
    case AST_CONTINUE:
        break;

    default:
        /* Best effort */
        check_expr(st, stmt->left,  reporter, arena);
        check_expr(st, stmt->right, reporter, arena);
        break;
    }
}

/* Check function body */
static void check_fn_body(SymbolTable *st, AstNode *fn,
                          ErrorReporter *reporter, Arena *arena) {
    if (!fn) return;

    /* Check parameter types */
    AstNode *p = fn->params;
    while (p) {
        check_type_expr(st, p->type_expr, reporter, arena);
        p = p->next;
    }

    /* Check return type */
    check_type_expr(st, fn->type_expr, reporter, arena);

    /* Check body */
    if (fn->left) {
        check_stmt(st, fn->left, reporter, arena);
    }
}

/* Check struct field types */
static void check_struct_fields(SymbolTable *st, AstNode *strct,
                                ErrorReporter *reporter, Arena *arena) {
    AstNode *field = strct->params;
    while (field) {
        if (field->kind == AST_FIELD) {
            check_type_expr(st, field->type_expr, reporter, arena);
        }
        field = field->next;
    }
}

/* Check tool declaration */
static void check_tool_decl(SymbolTable *st, AstNode *tool,
                            ErrorReporter *reporter, Arena *arena) {
    /* Check parameter types */
    AstNode *p = tool->params;
    while (p) {
        check_type_expr(st, p->type_expr, reporter, arena);
        p = p->next;
    }

    /* Check return type */
    check_type_expr(st, tool->type_expr, reporter, arena);

    /* Check body */
    if (tool->left) {
        check_stmt(st, tool->left, reporter, arena);
    }
}

static void check_types(SymbolTable *st, AstNode *program,
                        ErrorReporter *reporter, Arena *arena) {
    AstNode *decl;

    (void)arena;

    if (!program || program->kind != AST_PROGRAM) return;

    for (decl = program->params; decl; decl = decl->next) {
        switch (decl->kind) {
        case AST_FN:
            check_fn_body(st, decl, reporter, arena);
            break;

        case AST_STRUCT:
            check_struct_fields(st, decl, reporter, arena);
            break;

        case AST_AGENT: {
            /* Check agent field types */
            AstNode *field = decl->params;
            while (field) {
                if (field->kind == AST_FIELD) {
                    check_type_expr(st, field->type_expr, reporter, arena);
                }
                field = field->next;
            }

            /* Check agent methods */
            AstNode *method = decl->left;
            while (method) {
                if (method->kind == AST_FN) {
                    check_fn_body(st, method, reporter, arena);
                }
                method = method->next;
            }
            break;
        }

        case AST_TOOL:
            check_tool_decl(st, decl, reporter, arena);
            break;

        case AST_GUARD:
            check_fn_body(st, decl, reporter, arena);
            break;

        case AST_TRAIT:
        case AST_INTERFACE: {
            AstNode *method = decl->params;
            while (method) {
                if (method->kind == AST_FN) {
                    /* Check param types and return type */
                    AstNode *p = method->params;
                    while (p) {
                        check_type_expr(st, p->type_expr, reporter, arena);
                        p = p->next;
                    }
                    check_type_expr(st, method->type_expr, reporter, arena);
                }
                method = method->next;
            }
            break;
        }

        case AST_IMPL: {
            /* Check target type */
            check_type_expr(st, decl->left, reporter, arena);
            /* Check trait (if impl Trait for Type) */
            check_type_expr(st, decl->right, reporter, arena);
            /* Check methods */
            AstNode *method = decl->params;
            while (method) {
                if (method->kind == AST_FN) {
                    check_fn_body(st, method, reporter, arena);
                }
                method = method->next;
            }
            /* Verify interface satisfaction: all required methods implemented */
            if (decl->right && decl->right->name) {
                const char *trait_name = decl->right->name;
                /* Find the interface/trait declaration via symbol table */
                AstNode *trait_decl = NULL;
                for (int si = 0; si < st->count; si++) {
                    if ((st->entries[si].kind == SYM_INTERFACE || st->entries[si].kind == SYM_TRAIT) &&
                        st->entries[si].node &&
                        st->entries[si].name && strcmp(st->entries[si].name, trait_name) == 0) {
                        trait_decl = st->entries[si].node;
                        break;
                    }
                }
                if (trait_decl) {
                    AstNode *req = trait_decl->params;
                    while (req) {
                        if (req->kind == AST_FN && req->name && !req->left) {
                            bool found = false;
                            AstNode *imp = decl->params;
                            while (imp) {
                                if (imp->kind == AST_FN && imp->name &&
                                    strcmp(imp->name, req->name) == 0) {
                                    found = true;
                                    break;
                                }
                                imp = imp->next;
                            }
                            if (!found) {
                                report_error_fmt(reporter, decl->loc,
                                    "add the missing method to the impl block",
                                    "impl %s for %s is missing method '%s'",
                                    trait_name,
                                    decl->left && decl->left->name ? decl->left->name : "?",
                                    req->name);
                            }
                        }
                        req = req->next;
                    }
                }
            }
            break;
        }

        case AST_ENUM: {
            /* Check variant field types */
            AstNode *variant = decl->params;
            while (variant) {
                if (variant->kind == AST_VARIANT) {
                    AstNode *field = variant->params;
                    while (field) {
                        check_type_expr(st, field->type_expr, reporter, arena);
                        field = field->next;
                    }
                }
                variant = variant->next;
            }
            break;
        }

        case AST_LET:
            check_type_expr(st, decl->type_expr, reporter, arena);
            check_expr(st, decl->right, reporter, arena);
            break;

        case AST_CONST:
            check_type_expr(st, decl->type_expr, reporter, arena);
            check_expr(st, decl->right, reporter, arena);
            break;

        case AST_TYPE_ALIAS:
            check_type_expr(st, decl->type_expr, reporter, arena);
            break;

        /* Declarations that don't need deep type checking */
        case AST_USE:
        case AST_MODULE:
        case AST_CAPABILITY:
        case AST_TAINT:
        case AST_BUDGET:
        case AST_GUARDSET:
        case AST_INVARIANT:
        case AST_SKILL:
        case AST_PROMPT:
        case AST_SUPERVISOR:
        case AST_MESH:
        case AST_MEMORY:
        case AST_CHANNEL:
        case AST_ROUTER:
        case AST_STRATEGY:
            break;

        default:
            break;
        }
    }
}

/* ============================================================
 * Pass 7: Enum LLM constraint detection
 * ============================================================ */

static void check_enum_constraints(SymbolTable *st, AstNode *program,
                                    ErrorReporter *reporter) {
    AstNode *decl;

    (void)reporter;

    if (!program || program->kind != AST_PROGRAM) return;

    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind != AST_AGENT) continue;

        /* Check agent methods for enum return types */
        AstNode *method = decl->left;
        while (method) {
            if (method->kind == AST_FN && method->type_expr) {
                /* Check if return type is an enum */
                if (method->type_expr->kind == AST_TYPE_NAMED &&
                    method->type_expr->name) {
                    /* Look up the return type in symbol table */
                    int i;
                    for (i = 0; i < st->count; i++) {
                        if (st->entries[i].kind == SYM_ENUM &&
                            str_eq(st->entries[i].name,
                                   method->type_expr->name)) {
                            /* Found: method returns an enum type.
                             * In the future, this will generate a
                             * structured output constraint for the LLM. */
                            /* For now, just note it silently. */
                            break;
                        }
                    }
                }
            }
            method = method->next;
        }
    }
}

/* ============================================================
 * Pass 2b: Access Control Policy Enforcement
 *
 * Validates fetch() and exec() calls against declared endpoint
 * and binary allow/deny rules in capability blocks.
 *
 * Strategy:
 *   - Build a registry of concrete policies from capability blocks
 *   - For each agent, resolve its "use" field to concrete policies
 *   - Walk agent methods looking for fetch()/exec() calls
 *   - Check URLs and binaries against the policy rules
 *   - Flag violations or mark dynamic args as is_unsafe
 * ============================================================ */

/* ── Helper: parse "host:port" into host string and port number ── */

static void parse_host_port(const char *spec, char *host_out,
                            size_t host_sz, int *port_out) {
    const char *colon;
    size_t hlen;

    if (!spec || !host_out || !port_out) return;

    *port_out = 443;  /* default */
    colon = strrchr(spec, ':');

    if (colon && colon != spec) {
        hlen = (size_t)(colon - spec);
        if (hlen >= host_sz) hlen = host_sz - 1;
        memcpy(host_out, spec, hlen);
        host_out[hlen] = '\0';
        *port_out = atoi(colon + 1);
        if (*port_out <= 0) *port_out = 443;
    } else {
        hlen = strlen(spec);
        if (hlen >= host_sz) hlen = host_sz - 1;
        memcpy(host_out, spec, hlen);
        host_out[hlen] = '\0';
    }
}

/* ── Helper: check if a dotted-quad IP is in a private range ── */

static bool is_private_ip(const char *host) {
    unsigned int a, b, c, d;

    if (!host) return false;
    if (sscanf(host, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
        return false;

    /* 10.0.0.0/8 */
    if (a == 10) return true;
    /* 172.16.0.0/12 */
    if (a == 172 && b >= 16 && b <= 31) return true;
    /* 192.168.0.0/16 */
    if (a == 192 && b == 168) return true;
    /* 127.0.0.0/8 */
    if (a == 127) return true;
    /* 169.254.0.0/16 */
    if (a == 169 && b == 254) return true;

    return false;
}

/* ── Helper: simple URL parser ── */

static void parse_url_simple(const char *url,
                             char *scheme_out, size_t scheme_sz,
                             char *host_out,   size_t host_sz,
                             int  *port_out,
                             char *path_out,   size_t path_sz) {
    const char *p;
    const char *host_start;
    const char *host_end;
    const char *path_start;
    size_t len;

    if (!url) return;
    if (scheme_out) scheme_out[0] = '\0';
    if (host_out)   host_out[0]   = '\0';
    if (path_out)   path_out[0]   = '\0';
    if (port_out)   *port_out     = 443;

    p = url;

    /* Parse scheme */
    if (strncmp(p, "https://", 8) == 0) {
        if (scheme_out) {
            len = 5;
            if (len >= scheme_sz) len = scheme_sz - 1;
            memcpy(scheme_out, "https", len);
            scheme_out[len] = '\0';
        }
        if (port_out) *port_out = 443;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        if (scheme_out) {
            len = 4;
            if (len >= scheme_sz) len = scheme_sz - 1;
            memcpy(scheme_out, "http", len);
            scheme_out[len] = '\0';
        }
        if (port_out) *port_out = 80;
        p += 7;
    }

    /* Parse host[:port] */
    host_start = p;
    host_end = p;
    while (*host_end && *host_end != '/' && *host_end != '?') {
        host_end++;
    }
    path_start = host_end;

    /* Check for port in host segment */
    {
        const char *colon = host_start;
        while (colon < host_end && *colon != ':') colon++;
        if (colon < host_end) {
            /* Has port */
            len = (size_t)(colon - host_start);
            if (host_out) {
                if (len >= host_sz) len = host_sz - 1;
                memcpy(host_out, host_start, len);
                host_out[len] = '\0';
            }
            if (port_out) {
                *port_out = atoi(colon + 1);
                if (*port_out <= 0) *port_out = 443;
            }
        } else {
            /* No port */
            len = (size_t)(host_end - host_start);
            if (host_out) {
                if (len >= host_sz) len = host_sz - 1;
                memcpy(host_out, host_start, len);
                host_out[len] = '\0';
            }
        }
    }

    /* Parse path */
    if (path_out && *path_start) {
        len = strlen(path_start);
        if (len >= path_sz) len = path_sz - 1;
        memcpy(path_out, path_start, len);
        path_out[len] = '\0';
    }
}

/* ── Helper: simple glob matching ── */

static bool glob_match(const char *pattern, const char *str) {
    if (!pattern || !str) return false;

    while (*pattern && *str) {
        if (*pattern == '*') {
            if (*(pattern + 1) == '*') {
                /* ** matches anything including / */
                pattern += 2;
                if (*pattern == '/') pattern++;  /* skip trailing / */
                if (*pattern == '\0') return true;
                while (*str) {
                    if (glob_match(pattern, str)) return true;
                    str++;
                }
                return glob_match(pattern, str);
            }
            /* single * matches non-/ chars */
            pattern++;
            while (*str && *str != '/') {
                if (glob_match(pattern, str)) return true;
                str++;
            }
            return glob_match(pattern, str);
        }

        if (*pattern == '?') {
            if (*str == '/') return false;
            pattern++;
            str++;
            continue;
        }

        if (*pattern != *str) return false;
        pattern++;
        str++;
    }

    /* Handle trailing *'s */
    while (*pattern == '*') pattern++;

    return (*pattern == '\0' && *str == '\0');
}

/* ── Helper: extract first whitespace-delimited word from command ── */

static const char *extract_binary_name(const char *cmd, char *out,
                                       size_t out_sz) {
    size_t i = 0;
    if (!cmd || !out || out_sz == 0) return "";

    /* Skip leading whitespace */
    while (*cmd == ' ' || *cmd == '\t') cmd++;

    while (*cmd && *cmd != ' ' && *cmd != '\t' && i < out_sz - 1) {
        out[i++] = *cmd++;
    }
    out[i] = '\0';
    return out;
}

/* ── Build access policies from capability declarations ── */

static void build_access_policies(AccessPolicyRegistry *apr,
                                  AstNode *program, Arena *arena) {
    AstNode *decl;

    (void)arena;

    apr->count = 0;
    if (!program || program->kind != AST_PROGRAM) return;

    for (decl = program->params; decl; decl = decl->next) {
        AstNode *child;
        bool has_rules;

        if (decl->kind != AST_CAPABILITY) continue;
        if (apr->count >= MAX_POLICIES) break;

        /* Check if this capability has any access control rules */
        has_rules = false;
        for (child = decl->params; child; child = child->next) {
            if (child->kind == AST_CAP_ENDPOINT_RULE ||
                child->kind == AST_CAP_BINARY_RULE ||
                child->kind == AST_CAP_PATH_RULE ||
                child->kind == AST_CAP_DENY_RANGE ||
                child->kind == AST_CAP_DEFAULT) {
                has_rules = true;
                break;
            }
        }
        if (!has_rules) continue;

        {
            ConcretePolicy *pol = &apr->policies[apr->count++];
            memset(pol, 0, sizeof(*pol));
            pol->group_name = decl->name;
            pol->is_concrete = true;

            for (child = decl->params; child; child = child->next) {
                if (child->kind == AST_CAP_ENDPOINT_RULE &&
                    pol->endpoint_count < MAX_ENDPOINT_RULES) {
                    EndpointRule *er;
                    AstNode *sub;
                    char host_buf[256];
                    int port;

                    er = &pol->endpoints[pol->endpoint_count++];
                    memset(er, 0, sizeof(*er));
                    er->allow = child->is_mut; /* is_mut=true→allow */

                    parse_host_port(child->name, host_buf,
                                    sizeof(host_buf), &port);
                    er->host = child->name
                               ? arena_strdup(arena, host_buf)
                               : NULL;
                    er->port = port;

                    /* Extract method/path from sub-fields */
                    for (sub = child->params; sub; sub = sub->next) {
                        if (sub->kind != AST_FIELD) continue;
                        if (str_eq(sub->name, "method") && sub->right &&
                            sub->right->kind == AST_ARRAY) {
                            AstNode *m = sub->right->params;
                            while (m && er->method_count < MAX_EP_METHODS) {
                                if (m->name) {
                                    er->methods[er->method_count++] = m->name;
                                }
                                m = m->next;
                            }
                        }
                        if (str_eq(sub->name, "path") && sub->right &&
                            sub->right->kind == AST_STRING_LIT) {
                            er->path_glob = sub->right->val.str_val;
                        }
                    }
                }

                if (child->kind == AST_CAP_BINARY_RULE &&
                    pol->binary_count < MAX_BINARY_RULES) {
                    BinaryRule *br = &pol->binaries[pol->binary_count++];
                    br->path = child->name;
                    br->allow = child->is_mut; /* is_mut=true→allow */
                }

                if (child->kind == AST_CAP_PATH_RULE &&
                    pol->path_count < MAX_PATH_RULES) {
                    PathRule *pr = &pol->paths[pol->path_count++];
                    AstNode *sub;
                    pr->pattern = child->name;
                    pr->allow = child->is_mut; /* is_mut=true→allow */
                    pr->can_read = true;   /* default: both */
                    pr->can_write = true;

                    /* Extract mode from sub-fields if present */
                    for (sub = child->params; sub; sub = sub->next) {
                        if (sub->kind != AST_FIELD) continue;
                        if (str_eq(sub->name, "mode") && sub->right &&
                            sub->right->kind == AST_ARRAY) {
                            AstNode *m;
                            pr->can_read = false;
                            pr->can_write = false;
                            for (m = sub->right->params; m; m = m->next) {
                                if (m->name && strcmp(m->name, "read") == 0)
                                    pr->can_read = true;
                                if (m->name && strcmp(m->name, "write") == 0)
                                    pr->can_write = true;
                            }
                        }
                    }
                }

                if (child->kind == AST_CAP_DENY_RANGE) {
                    pol->deny_private = true;
                }

                if (child->kind == AST_CAP_DEFAULT) {
                    pol->has_default = true;
                    pol->default_deny = !child->is_mut;
                }
            }
        }
    }
}

/* ── Look up a concrete policy by group name ── */

static ConcretePolicy *find_policy(AccessPolicyRegistry *apr,
                                   const char *name) {
    int i;
    for (i = 0; i < apr->count; i++) {
        if (str_eq(apr->policies[i].group_name, name))
            return &apr->policies[i];
    }
    return NULL;
}

/* ── Extract basename from a path (last component after /) ── */

static const char *basename_of(const char *path) {
    const char *last;
    if (!path) return NULL;
    last = strrchr(path, '/');
    return last ? last + 1 : path;
}

/* ── Check a fetch() call against endpoint policies ── */

static int check_fetch_call(ErrorReporter *reporter,
                            AccessPolicyRegistry *apr,
                            const char **agent_policies,
                            int agent_policy_count,
                            AstNode *call_node) {
    int errors = 0;
    int pi;
    AstNode *first_arg;
    char scheme[16];
    char host[256];
    int  port;
    char path[1024];

    if (!call_node || !call_node->params) return 0;
    first_arg = call_node->params;

    if (first_arg->kind != AST_STRING_LIT) {
        /* Dynamic URL — cannot check statically, flag for runtime */
        call_node->is_unsafe = true;
        return 0;
    }

    scheme[0] = '\0'; host[0] = '\0'; path[0] = '\0';
    port = 443;
    parse_url_simple(first_arg->val.str_val,
                     scheme, sizeof(scheme),
                     host,   sizeof(host),
                     &port,
                     path,   sizeof(path));

    for (pi = 0; pi < agent_policy_count; pi++) {
        ConcretePolicy *pol = find_policy(apr, agent_policies[pi]);
        if (!pol || !pol->is_concrete) continue;
        /* Skip policies that have no endpoint rules (e.g. shell policy) */
        if (pol->endpoint_count == 0 && !pol->deny_private) continue;

        /* Check private IP ranges */
        if (pol->deny_private && is_private_ip(host)) {
            report_error_fmt(reporter, call_node->loc,
                "remove the fetch or adjust the capability's "
                "network policy",
                "access denied: fetch to private IP range '%s' "
                "is blocked by 'deny private_ranges'",
                host);
            errors++;
            continue;
        }

        /* Check endpoint allow/deny rules */
        {
            int ei;
            bool matched = false;
            for (ei = 0; ei < pol->endpoint_count; ei++) {
                EndpointRule *er = &pol->endpoints[ei];
                bool host_match = false;

                /* Match host */
                if (er->host && host[0]) {
                    /* Try exact match or glob */
                    if (str_eq(er->host, host) ||
                        glob_match(er->host, host)) {
                        host_match = true;
                    }
                }
                /* Match port (0 means any) */
                if (host_match && er->port != 0 && er->port != port) {
                    host_match = false;
                }

                if (!host_match) continue;

                /* Match path if specified */
                if (er->path_glob && path[0]) {
                    if (!glob_match(er->path_glob, path)) continue;
                }

                matched = true;
                if (!er->allow) {
                    report_error_fmt(reporter, call_node->loc,
                        "this endpoint is explicitly denied by "
                        "the capability policy",
                        "access denied: endpoint '%s:%d' denied by "
                        "capability '%s'",
                        host, port,
                        pol->group_name ? pol->group_name : "<cap>");
                    errors++;
                }
                break;
            }

            if (!matched && pol->has_default && pol->default_deny) {
                report_error_fmt(reporter, call_node->loc,
                    "add an allow rule for this endpoint or change "
                    "the default policy",
                    "access denied: endpoint '%s:%d' not in allow "
                    "list for capability '%s' (default: deny)",
                    host, port,
                    pol->group_name ? pol->group_name : "<cap>");
                errors++;
            }
        }
    }

    return errors;
}

/* ── Check an exec() call against binary policies ── */

static int check_exec_call(ErrorReporter *reporter,
                           AccessPolicyRegistry *apr,
                           const char **agent_policies,
                           int agent_policy_count,
                           AstNode *call_node) {
    int errors = 0;
    int pi;
    AstNode *first_arg;
    char bin_buf[512];
    const char *binary;
    const char *bin_base;

    if (!call_node || !call_node->params) return 0;
    first_arg = call_node->params;

    if (first_arg->kind != AST_STRING_LIT) {
        call_node->is_unsafe = true;
        return 0;
    }

    extract_binary_name(first_arg->val.str_val, bin_buf, sizeof(bin_buf));
    binary = bin_buf;
    bin_base = basename_of(binary);

    for (pi = 0; pi < agent_policy_count; pi++) {
        ConcretePolicy *pol = find_policy(apr, agent_policies[pi]);
        if (!pol || !pol->is_concrete) continue;
        /* Skip policies that have no binary rules (e.g. network policy) */
        if (pol->binary_count == 0) continue;

        {
            int bi;
            bool matched = false;
            for (bi = 0; bi < pol->binary_count; bi++) {
                BinaryRule *br = &pol->binaries[bi];
                const char *rule_base;

                if (!br->path) continue;
                rule_base = basename_of(br->path);

                /* Match by full path or basename */
                if (str_eq(br->path, binary) ||
                    str_eq(rule_base, bin_base) ||
                    str_eq(br->path, bin_base)) {
                    matched = true;
                    if (!br->allow) {
                        report_error_fmt(reporter, call_node->loc,
                            "this binary is explicitly denied by "
                            "the capability policy",
                            "access denied: binary '%s' explicitly "
                            "denied by capability '%s'",
                            binary,
                            pol->group_name ? pol->group_name : "<cap>");
                        errors++;
                    }
                    break;
                }
            }

            if (!matched && pol->has_default && pol->default_deny) {
                report_error_fmt(reporter, call_node->loc,
                    "add an allow rule for this binary or change "
                    "the default policy",
                    "access denied: binary '%s' not in allow list "
                    "for capability '%s' (default: deny)",
                    binary,
                    pol->group_name ? pol->group_name : "<cap>");
                errors++;
            }
        }
    }

    return errors;
}

/* ── Check a read_file()/write_file() call against path policies ── */

static int check_file_call(ErrorReporter *reporter,
                           AccessPolicyRegistry *apr,
                           const char **agent_policies,
                           int agent_policy_count,
                           AstNode *call_node,
                           bool is_write) {
    int errors = 0;
    int pi;
    AstNode *first_arg;
    const char *file_path;
    const char *op_name = is_write ? "write_file" : "read_file";

    if (!call_node || !call_node->params) return 0;
    first_arg = call_node->params;

    if (first_arg->kind != AST_STRING_LIT) {
        /* Dynamic path — cannot check statically, flag for runtime */
        call_node->is_unsafe = true;
        return 0;
    }

    file_path = first_arg->val.str_val;

    for (pi = 0; pi < agent_policy_count; pi++) {
        ConcretePolicy *pol = find_policy(apr, agent_policies[pi]);
        if (!pol || !pol->is_concrete) continue;
        /* Skip policies that have no path rules */
        if (pol->path_count == 0) continue;

        {
            int ri;
            bool matched = false;
            for (ri = 0; ri < pol->path_count; ri++) {
                PathRule *pr = &pol->paths[ri];

                if (!pr->pattern) continue;
                if (!glob_match(pr->pattern, file_path)) continue;

                matched = true;
                if (!pr->allow) {
                    report_error_fmt(reporter, call_node->loc,
                        "this path is explicitly denied by "
                        "the capability policy",
                        "access denied: %s('%s') denied by "
                        "capability '%s'",
                        op_name, file_path,
                        pol->group_name ? pol->group_name : "<cap>");
                    errors++;
                } else {
                    /* Check mode permissions */
                    if (!is_write && !pr->can_read) {
                        report_error_fmt(reporter, call_node->loc,
                            "this path does not allow read access",
                            "access denied: read_file('%s') not "
                            "permitted (no read mode) in '%s'",
                            file_path,
                            pol->group_name ? pol->group_name
                                            : "<cap>");
                        errors++;
                    }
                    if (is_write && !pr->can_write) {
                        report_error_fmt(reporter, call_node->loc,
                            "this path does not allow write access",
                            "access denied: write_file('%s') not "
                            "permitted (no write mode) in '%s'",
                            file_path,
                            pol->group_name ? pol->group_name
                                            : "<cap>");
                        errors++;
                    }
                }
                break;
            }

            if (!matched && pol->has_default && pol->default_deny) {
                report_error_fmt(reporter, call_node->loc,
                    "add an allow rule for this path or change "
                    "the default policy",
                    "access denied: %s('%s') not in allow list "
                    "for capability '%s' (default: deny)",
                    op_name, file_path,
                    pol->group_name ? pol->group_name : "<cap>");
                errors++;
            }
        }
    }

    return errors;
}

/* ── Recursive statement walker for access policy checks ── */

static int walk_stmts_for_access(ErrorReporter *reporter,
                                 AccessPolicyRegistry *apr,
                                 const char **policies,
                                 int policy_count,
                                 AstNode *node);

static int walk_expr_for_access(ErrorReporter *reporter,
                                AccessPolicyRegistry *apr,
                                const char **policies,
                                int policy_count,
                                AstNode *expr) {
    int errors = 0;
    if (!expr) return 0;

    if (expr->kind == AST_CALL && expr->left &&
        expr->left->kind == AST_IDENT && expr->left->name) {
        if (str_eq(expr->left->name, "fetch")) {
            errors += check_fetch_call(reporter, apr, policies,
                                       policy_count, expr);
        } else if (str_eq(expr->left->name, "exec")) {
            errors += check_exec_call(reporter, apr, policies,
                                      policy_count, expr);
        } else if (str_eq(expr->left->name, "read_file")) {
            errors += check_file_call(reporter, apr, policies,
                                      policy_count, expr, false);
        } else if (str_eq(expr->left->name, "write_file")) {
            errors += check_file_call(reporter, apr, policies,
                                      policy_count, expr, true);
        }
    }

    errors += walk_expr_for_access(reporter, apr, policies,
                                   policy_count, expr->left);
    errors += walk_expr_for_access(reporter, apr, policies,
                                   policy_count, expr->right);
    {
        AstNode *p = expr->params;
        while (p) {
            errors += walk_expr_for_access(reporter, apr, policies,
                                           policy_count, p);
            p = p->next;
        }
    }
    return errors;
}

static int walk_stmts_for_access(ErrorReporter *reporter,
                                 AccessPolicyRegistry *apr,
                                 const char **policies,
                                 int policy_count,
                                 AstNode *node) {
    int errors = 0;
    if (!node) return 0;

    switch (node->kind) {
    case AST_BLOCK: {
        AstNode *s = node->params;
        while (s) {
            errors += walk_stmts_for_access(reporter, apr, policies,
                                            policy_count, s);
            s = s->next;
        }
        break;
    }

    case AST_LET:
        errors += walk_expr_for_access(reporter, apr, policies,
                                       policy_count, node->right);
        break;

    case AST_RETURN:
        errors += walk_expr_for_access(reporter, apr, policies,
                                       policy_count, node->left);
        break;

    case AST_IF:
        errors += walk_expr_for_access(reporter, apr, policies,
                                       policy_count, node->left);
        errors += walk_stmts_for_access(reporter, apr, policies,
                                        policy_count, node->right);
        errors += walk_stmts_for_access(reporter, apr, policies,
                                        policy_count, node->params);
        break;

    case AST_FOR:
        errors += walk_stmts_for_access(reporter, apr, policies,
                                        policy_count, node->right);
        break;

    case AST_WHILE:
        errors += walk_expr_for_access(reporter, apr, policies,
                                       policy_count, node->left);
        errors += walk_stmts_for_access(reporter, apr, policies,
                                        policy_count, node->right);
        break;

    case AST_LOOP:
        errors += walk_stmts_for_access(reporter, apr, policies,
                                        policy_count, node->left);
        break;

    case AST_EXPR_STMT:
        errors += walk_expr_for_access(reporter, apr, policies,
                                       policy_count, node->left);
        break;

    case AST_ASSIGN:
        errors += walk_expr_for_access(reporter, apr, policies,
                                       policy_count, node->left);
        errors += walk_expr_for_access(reporter, apr, policies,
                                       policy_count, node->right);
        break;

    case AST_DEFER:
        errors += walk_expr_for_access(reporter, apr, policies,
                                       policy_count, node->left);
        break;

    case AST_MATCH: {
        AstNode *arm;
        errors += walk_expr_for_access(reporter, apr, policies,
                                       policy_count, node->left);
        for (arm = node->params; arm; arm = arm->next) {
            if (arm->kind == AST_MATCH_ARM) {
                errors += walk_stmts_for_access(reporter, apr, policies,
                                                policy_count, arm->right);
            }
        }
        break;
    }

    default:
        errors += walk_expr_for_access(reporter, apr, policies,
                                       policy_count, node->left);
        errors += walk_expr_for_access(reporter, apr, policies,
                                       policy_count, node->right);
        break;
    }

    return errors;
}

/* ── Collect agent's "use" field policy names ── */

static int collect_agent_policies(AstNode *agent, const char *out[],
                                  int max_out) {
    int count = 0;
    AstNode *field = agent->params;
    while (field) {
        if (field->kind == AST_FIELD && str_eq(field->name, "use")) {
            AstNode *arr = field->right;
            if (arr && arr->kind == AST_ARRAY) {
                AstNode *elem = arr->params;
                while (elem && count < max_out) {
                    if (elem->kind == AST_IDENT && elem->name) {
                        out[count++] = elem->name;
                    }
                    elem = elem->next;
                }
            }
            break;
        }
        field = field->next;
    }
    return count;
}

/* ── Main access policy check pass ── */

static int check_access_policies(SymbolTable *st,
                                 AccessPolicyRegistry *apr,
                                 AstNode *program,
                                 ErrorReporter *reporter, Arena *arena) {
    int errors = 0;
    AstNode *decl;

    (void)st;
    (void)arena;

    if (!program || program->kind != AST_PROGRAM) return 0;
    if (apr->count == 0) return 0;  /* no concrete policies at all */

    for (decl = program->params; decl; decl = decl->next) {
        const char *use_names[MAX_POLICIES];
        int nuse;
        AstNode *method;

        if (decl->kind != AST_AGENT) continue;

        nuse = collect_agent_policies(decl, use_names, MAX_POLICIES);
        if (nuse == 0) continue;  /* backward compatible: skip */

        /* Walk agent methods */
        for (method = decl->left; method; method = method->next) {
            if (method->kind == AST_FN && method->left) {
                errors += walk_stmts_for_access(reporter, apr,
                                                use_names, nuse,
                                                method->left);
            }
        }
    }

    return errors;
}

/* ============================================================
 * Pass 9: Ownership & Borrow Checking — advisory by default
 *
 * Tracks variable ownership through the AST:
 *   - Every `let x = expr` registers x as OWNED
 *   - Passing x by value to a function MOVEs ownership
 *   - `&x` creates an immutable borrow (multiple OK)
 *   - `&mut x` creates an exclusive mutable borrow
 *   - Borrows are released at scope exit
 *
 * Rules enforced:
 *   1. Use after move: error
 *   2. Move while borrowed: error
 *   3. Mutable borrow while immutably borrowed: error
 *   4. Double mutable borrow: error
 *
 * Default mode: warnings with [ownership] prefix.
 * --strict-ownership flag promotes them to hard errors.
 * ============================================================ */

/* ── Ownership context helpers ── */

static void own_ctx_init(OwnershipCtx *ctx, bool strict) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->strict = strict;
}

static VarOwnership *own_find(OwnershipCtx *ctx, const char *name) {
    int i;
    if (!name) return NULL;
    /* Search backwards to find innermost scope first */
    for (i = ctx->count - 1; i >= 0; i--) {
        if (ctx->vars[i].name && strcmp(ctx->vars[i].name, name) == 0)
            return &ctx->vars[i];
    }
    return NULL;
}

static VarOwnership *own_register(OwnershipCtx *ctx, const char *name,
                                   int line) {
    VarOwnership *v;
    if (!name || ctx->count >= MAX_OWNED_VARS) return NULL;
    v = &ctx->vars[ctx->count++];
    memset(v, 0, sizeof(*v));
    v->name = name;
    v->state = OWN_OWNED;
    v->scope_depth = ctx->scope_depth;
    v->borrow_line = line;
    return v;
}

static void own_push_scope(OwnershipCtx *ctx) {
    ctx->scope_depth++;
}

static void own_pop_scope(OwnershipCtx *ctx) {
    /* Release borrows for variables in the exiting scope and
     * reset borrow state for outer-scope variables whose borrows
     * were created at this depth. */
    int i;
    int depth = ctx->scope_depth;

    /* Collect names of variables being removed (they may hold borrows
     * of outer-scope variables). We track which outer variables were
     * borrowed by let-bindings in this scope. */
    const char *leaving_names[MAX_OWNED_VARS];
    int leaving_count = 0;
    for (i = ctx->count - 1; i >= 0; i--) {
        if (ctx->vars[i].scope_depth >= depth) {
            leaving_names[leaving_count++] = ctx->vars[i].name;
        } else {
            break;
        }
    }

    /* Remove variables declared at this scope depth */
    while (ctx->count > 0 &&
           ctx->vars[ctx->count - 1].scope_depth >= depth) {
        ctx->count--;
    }

    /* For remaining (outer-scope) vars, release borrows that were
     * created while this scope was active. This implements the rule
     * that borrows end when the borrowing variable goes out of scope. */
    for (i = 0; i < ctx->count; i++) {
        VarOwnership *v = &ctx->vars[i];
        if (v->state == OWN_BORROWED && v->borrow_count > 0) {
            /* Decrement borrows — borrows from the exiting scope are released.
             * In a full implementation we'd track per-borrow scope depth. */
            v->borrow_count--;
            if (v->borrow_count == 0) {
                v->state = OWN_OWNED;
            }
        }
        if (v->state == OWN_MUT_BORROWED && v->has_mut_borrow) {
            /* Mut borrow released at scope exit */
            v->has_mut_borrow = false;
            v->state = OWN_OWNED;
        }
    }

    ctx->scope_depth--;
}

/* Report an ownership violation (warning or error based on strict mode) */
static void own_report(OwnershipCtx *ctx, ErrorReporter *reporter,
                        SourceLoc loc, const char *hint,
                        const char *fmt, ...) {
    char msg[512];
    char tagged[544];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    snprintf(tagged, sizeof(tagged), "[ownership] %s", msg);

    if (ctx->strict) {
        report_error(reporter, loc, tagged, hint);
    } else {
        report_warning(reporter, loc, tagged, hint);
    }
}

/* ── AST Walking for ownership analysis ── */

static void own_check_expr(OwnershipCtx *ctx, AstNode *expr,
                            ErrorReporter *reporter, Arena *arena);
static void own_check_stmt(OwnershipCtx *ctx, AstNode *stmt,
                            ErrorReporter *reporter, Arena *arena);

/* Check if an expression is a simple identifier reference */
static const char *own_expr_ident_name(AstNode *expr) {
    if (!expr) return NULL;
    if (expr->kind == AST_IDENT && expr->name) return expr->name;
    return NULL;
}

/* Check an identifier usage — verifies it hasn't been moved */
static void own_check_use(OwnershipCtx *ctx, AstNode *expr,
                           ErrorReporter *reporter) {
    const char *name;
    VarOwnership *v;

    if (!expr) return;
    name = own_expr_ident_name(expr);
    if (!name) return;

    v = own_find(ctx, name);
    if (!v) return;  /* not a tracked variable */

    if (v->state == OWN_MOVED) {
        own_report(ctx, reporter, expr->loc,
                   v->moved_to
                       ? "value was moved into a function call"
                       : "value was previously moved",
                   "use of moved value '%s' (moved at line %d)",
                   name, v->move_line);
    }
}

/* Mark a variable as moved (e.g., passed by value to a function) */
static void own_mark_move(OwnershipCtx *ctx, const char *name,
                           const char *moved_to, int line,
                           ErrorReporter *reporter, SourceLoc loc) {
    VarOwnership *v;
    if (!name) return;

    v = own_find(ctx, name);
    if (!v) return;

    /* Copy types (int, bool, float, handles) are never moved */
    if (v->is_copy) return;

    /* Can't move if already moved */
    if (v->state == OWN_MOVED) {
        own_report(ctx, reporter, loc,
                   "value was already moved",
                   "use of moved value '%s' (moved at line %d)",
                   name, v->move_line);
        return;
    }

    /* Can't move while borrowed */
    if (v->state == OWN_BORROWED || v->borrow_count > 0) {
        own_report(ctx, reporter, loc,
                   "release the borrow before moving",
                   "cannot move '%s' while it is borrowed (borrow at line %d)",
                   name, v->borrow_line);
        return;
    }

    if (v->state == OWN_MUT_BORROWED || v->has_mut_borrow) {
        own_report(ctx, reporter, loc,
                   "release the mutable borrow before moving",
                   "cannot move '%s' while mutably borrowed (borrow at line %d)",
                   name, v->mut_borrow_line);
        return;
    }

    v->state = OWN_MOVED;
    v->moved_to = moved_to;
    v->move_line = line;
}

/* Process a borrow expression: &x or &mut x */
static void own_process_borrow(OwnershipCtx *ctx, AstNode *ref_expr,
                                ErrorReporter *reporter) {
    const char *name;
    VarOwnership *v;
    bool is_mut;

    if (!ref_expr || ref_expr->kind != AST_REF) return;
    if (!ref_expr->left) return;

    name = own_expr_ident_name(ref_expr->left);
    if (!name) return;

    v = own_find(ctx, name);
    if (!v) return;

    is_mut = ref_expr->is_mut;

    /* Can't borrow a moved value */
    if (v->state == OWN_MOVED) {
        own_report(ctx, reporter, ref_expr->loc,
                   "value was previously moved",
                   "cannot borrow moved value '%s' (moved at line %d)",
                   name, v->move_line);
        return;
    }

    if (is_mut) {
        /* Mutable borrow: no other borrows allowed */
        if (v->borrow_count > 0) {
            own_report(ctx, reporter, ref_expr->loc,
                       "release immutable borrows first",
                       "cannot mutably borrow '%s' while immutably borrowed "
                       "(borrow at line %d)",
                       name, v->borrow_line);
            return;
        }
        if (v->has_mut_borrow) {
            own_report(ctx, reporter, ref_expr->loc,
                       "only one &mut borrow allowed at a time",
                       "cannot mutably borrow '%s': already mutably borrowed "
                       "(at line %d)",
                       name, v->mut_borrow_line);
            return;
        }
        v->state = OWN_MUT_BORROWED;
        v->has_mut_borrow = true;
        v->mut_borrow_line = (int)ref_expr->loc.line;
    } else {
        /* Immutable borrow: no mut borrows allowed */
        if (v->has_mut_borrow) {
            own_report(ctx, reporter, ref_expr->loc,
                       "release the mutable borrow first",
                       "cannot immutably borrow '%s' while mutably borrowed "
                       "(at line %d)",
                       name, v->mut_borrow_line);
            return;
        }
        v->state = OWN_BORROWED;
        v->borrow_count++;
        if (v->borrow_count == 1) {
            v->borrow_line = (int)ref_expr->loc.line;
        }
    }
}

/* Check a function call: arguments passed by value are moved,
 * arguments passed by reference are temporarily borrowed for
 * the duration of the call. */
static void own_check_call(OwnershipCtx *ctx, AstNode *call_expr,
                            ErrorReporter *reporter, Arena *arena) {
    AstNode *arg;
    const char *callee_name = NULL;
    /* Track which variables were temporarily borrowed for this call */
    const char *temp_borrows[32];
    bool        temp_is_mut[32];
    int         temp_count = 0;

    if (!call_expr || call_expr->kind != AST_CALL) return;

    /* Get callee name for error messages */
    if (call_expr->left && call_expr->left->kind == AST_IDENT) {
        callee_name = call_expr->left->name;
    }

    /* Check each argument */
    for (arg = call_expr->params; arg; arg = arg->next) {
        if (arg->kind == AST_REF) {
            /* &x or &mut x: temporary borrow for the call duration */
            const char *ref_name = own_expr_ident_name(arg->left);
            own_process_borrow(ctx, arg, reporter);
            /* Track for release after the call */
            if (ref_name && temp_count < 32) {
                temp_borrows[temp_count] = ref_name;
                temp_is_mut[temp_count] = arg->is_mut;
                temp_count++;
            }
        } else {
            const char *arg_name = own_expr_ident_name(arg);
            if (arg_name) {
                /* Plain identifier passed by value: move */
                own_mark_move(ctx, arg_name, callee_name,
                              (int)arg->loc.line, reporter, arg->loc);
            } else {
                /* Recurse into complex sub-expressions of the arg */
                own_check_expr(ctx, arg, reporter, arena);
            }
        }
    }

    /* Also check the callee expression */
    if (call_expr->left) {
        /* Don't re-check if callee is a simple ident (would trigger
         * false use-after-move for the callee name) */
        if (call_expr->left->kind != AST_IDENT) {
            own_check_expr(ctx, call_expr->left, reporter, arena);
        }
    }

    /* Release temporary borrows from &x arguments.
     * Function-call borrows only last for the call duration. */
    {
        int i;
        for (i = 0; i < temp_count; i++) {
            VarOwnership *v = own_find(ctx, temp_borrows[i]);
            if (!v) continue;
            if (temp_is_mut[i]) {
                if (v->has_mut_borrow) {
                    v->has_mut_borrow = false;
                    v->state = OWN_OWNED;
                }
            } else {
                if (v->borrow_count > 0) {
                    v->borrow_count--;
                    if (v->borrow_count == 0 && v->state == OWN_BORROWED) {
                        v->state = OWN_OWNED;
                    }
                }
            }
        }
    }
}

/* Walk an expression for ownership tracking */
static void own_check_expr(OwnershipCtx *ctx, AstNode *expr,
                            ErrorReporter *reporter, Arena *arena) {
    if (!expr) return;

    (void)arena;

    switch (expr->kind) {
    case AST_IDENT:
        own_check_use(ctx, expr, reporter);
        break;

    case AST_REF:
        own_process_borrow(ctx, expr, reporter);
        break;

    case AST_DEREF:
        own_check_expr(ctx, expr->left, reporter, arena);
        break;

    case AST_CALL:
        own_check_call(ctx, expr, reporter, arena);
        break;

    case AST_METHOD_CALL:
        /* Object is used but not moved for method calls */
        own_check_use(ctx, expr->left, reporter);
        /* Check arguments — by value = move */
        {
            AstNode *arg;
            for (arg = expr->params; arg; arg = arg->next) {
                if (arg->kind == AST_REF) {
                    own_process_borrow(ctx, arg, reporter);
                } else {
                    const char *arg_name = own_expr_ident_name(arg);
                    if (arg_name) {
                        own_mark_move(ctx, arg_name, expr->name,
                                      (int)arg->loc.line, reporter, arg->loc);
                    }
                }
                own_check_expr(ctx, arg, reporter, arena);
            }
        }
        break;

    case AST_BINARY:
        own_check_expr(ctx, expr->left,  reporter, arena);
        own_check_expr(ctx, expr->right, reporter, arena);
        break;

    case AST_UNARY:
        own_check_expr(ctx, expr->left, reporter, arena);
        break;

    case AST_FIELD_ACCESS:
        own_check_expr(ctx, expr->left, reporter, arena);
        break;

    case AST_INDEX:
        own_check_expr(ctx, expr->left,  reporter, arena);
        own_check_expr(ctx, expr->right, reporter, arena);
        break;

    case AST_IF:
        own_check_expr(ctx, expr->left, reporter, arena);
        own_check_stmt(ctx, expr->right,  reporter, arena);
        own_check_stmt(ctx, expr->params, reporter, arena);
        break;

    case AST_MATCH:
        own_check_expr(ctx, expr->left, reporter, arena);
        {
            AstNode *arm = expr->params;
            while (arm) {
                if (arm->kind == AST_MATCH_ARM) {
                    own_check_stmt(ctx, arm->right, reporter, arena);
                }
                arm = arm->next;
            }
        }
        break;

    case AST_CLOSURE:
        /* Closures capture variables — don't walk into body for now
         * (future: track captured variables) */
        break;

    case AST_ARRAY:
    case AST_TUPLE: {
        AstNode *el = expr->params;
        while (el) {
            own_check_expr(ctx, el, reporter, arena);
            el = el->next;
        }
        break;
    }

    case AST_MAP: {
        AstNode *entry = expr->params;
        while (entry) {
            if (entry->kind == AST_MAP_ENTRY) {
                own_check_expr(ctx, entry->left,  reporter, arena);
                own_check_expr(ctx, entry->right, reporter, arena);
            }
            entry = entry->next;
        }
        break;
    }

    case AST_PIPE:
        own_check_expr(ctx, expr->left,  reporter, arena);
        own_check_expr(ctx, expr->right, reporter, arena);
        break;

    case AST_CAST:
    case AST_IS_EXPR:
        own_check_expr(ctx, expr->left, reporter, arena);
        break;

    case AST_TRY:
    case AST_AWAIT:
    case AST_SPAWN:
        own_check_expr(ctx, expr->left, reporter, arena);
        break;

    case AST_RANGE:
        own_check_expr(ctx, expr->left,  reporter, arena);
        own_check_expr(ctx, expr->right, reporter, arena);
        break;

    case AST_ASK:
    case AST_TELL:
    case AST_ENSURE:
        own_check_expr(ctx, expr->left, reporter, arena);
        break;

    case AST_TRY_OTHERWISE:
    case AST_EACH:
    case AST_KEEP_WHERE:
    case AST_REPEAT:
    case AST_WAIT_UNTIL:
        own_check_expr(ctx, expr->left,  reporter, arena);
        own_check_expr(ctx, expr->right, reporter, arena);
        break;

    case AST_SELECT: {
        AstNode *arm = expr->params;
        while (arm) {
            if (arm->kind == AST_SELECT_ARM) {
                own_check_expr(ctx, arm->left,  reporter, arena);
                own_check_stmt(ctx, arm->right, reporter, arena);
            }
            arm = arm->next;
        }
        break;
    }

    default:
        /* Best effort for remaining node types */
        own_check_expr(ctx, expr->left,  reporter, arena);
        own_check_expr(ctx, expr->right, reporter, arena);
        break;
    }
}

/* Walk a statement for ownership tracking */
static void own_check_stmt(OwnershipCtx *ctx, AstNode *stmt,
                            ErrorReporter *reporter, Arena *arena) {
    if (!stmt) return;

    switch (stmt->kind) {
    case AST_LET: {
        /* Register the new variable as owned */
        if (stmt->name) {
            VarOwnership *ov = own_register(ctx, stmt->name, (int)stmt->loc.line);
            /* Detect Copy types — primitives, handles, and function results
             * are always safe to reuse without moving */
            if (ov && stmt->right) {
                AstKind rk = stmt->right->kind;
                /* Literals are always Copy */
                if (rk == AST_INT_LIT || rk == AST_FLOAT_LIT || rk == AST_BOOL_LIT ||
                    rk == AST_NONE_LIT) {
                    ov->is_copy = true;
                }
                /* Function calls returning primitives or handles */
                if (rk == AST_CALL && stmt->right->left && stmt->right->left->name) {
                    const char *fn = stmt->right->left->name;
                    if (strcmp(fn, "to_int") == 0 || strcmp(fn, "len") == 0 ||
                        strcmp(fn, "str_len") == 0 || strcmp(fn, "str_find") == 0 ||
                        strcmp(fn, "env") == 0 || strcmp(fn, "env_or") == 0 ||
                        strcmp(fn, "to_string") == 0 || strcmp(fn, "str_eq") == 0)
                        ov->is_copy = true;
                }
                /* Method calls (db.connect, db.query, db.get, etc.) */
                if (rk == AST_METHOD_CALL)
                    ov->is_copy = true;  /* handles + method results are Copy */
                /* Binary expressions produce primitives */
                if (rk == AST_BINARY)
                    ov->is_copy = true;
                /* Explicit type annotations */
                if (stmt->type_expr && stmt->type_expr->name) {
                    const char *tn = stmt->type_expr->name;
                    if (strcmp(tn, "int") == 0 || strcmp(tn, "bool") == 0 ||
                        strcmp(tn, "float") == 0 || strcmp(tn, "string") == 0)
                        ov->is_copy = true;
                }
            }
        }
        /* Check the initializer expression */
        if (stmt->right) {
            /* If the initializer is a simple ident, it's a move */
            const char *rhs_name = own_expr_ident_name(stmt->right);
            if (rhs_name && stmt->right->kind == AST_IDENT) {
                VarOwnership *rv = own_find(ctx, rhs_name);
                if (rv && rv->state == OWN_OWNED) {
                    /* If it's a ref expression, it's a borrow not a move */
                    /* but plain ident = move (for non-Copy types).
                     * In advisory mode, we track this for complex types;
                     * for now, we don't move simple let-bindings since
                     * many are Copy types (int, bool, etc). */
                }
            }
            /* If the initializer is a &x or &mut x, process the borrow */
            if (stmt->right->kind == AST_REF) {
                own_process_borrow(ctx, stmt->right, reporter);
            } else {
                own_check_expr(ctx, stmt->right, reporter, arena);
            }
        }
        break;
    }

    case AST_RETURN:
        own_check_expr(ctx, stmt->left, reporter, arena);
        break;

    case AST_DEFER:
        own_check_expr(ctx, stmt->left, reporter, arena);
        break;

    case AST_ASSIGN:
        own_check_expr(ctx, stmt->left,  reporter, arena);
        own_check_expr(ctx, stmt->right, reporter, arena);
        break;

    case AST_EXPR_STMT:
        own_check_expr(ctx, stmt->left, reporter, arena);
        break;

    case AST_IF:
        own_check_expr(ctx, stmt->left, reporter, arena);
        /* Then branch */
        if (stmt->right) {
            own_push_scope(ctx);
            own_check_stmt(ctx, stmt->right, reporter, arena);
            own_pop_scope(ctx);
        }
        /* Else branch */
        if (stmt->params) {
            own_push_scope(ctx);
            own_check_stmt(ctx, stmt->params, reporter, arena);
            own_pop_scope(ctx);
        }
        break;

    case AST_WHILE:
        own_check_expr(ctx, stmt->left, reporter, arena);
        if (stmt->right) {
            own_push_scope(ctx);
            own_check_stmt(ctx, stmt->right, reporter, arena);
            own_pop_scope(ctx);
        }
        break;

    case AST_LOOP:
        if (stmt->left) {
            own_push_scope(ctx);
            own_check_stmt(ctx, stmt->left, reporter, arena);
            own_pop_scope(ctx);
        }
        break;

    case AST_FOR: {
        /* Register loop variable as Copy (always int) */
        own_push_scope(ctx);
        if (stmt->name) {
            VarOwnership *fv = own_register(ctx, stmt->name, (int)stmt->loc.line);
            if (fv) fv->is_copy = true;
        } else if (stmt->left && stmt->left->name) {
            VarOwnership *fv = own_register(ctx, stmt->left->name, (int)stmt->loc.line);
            if (fv) fv->is_copy = true;
        }
        own_check_expr(ctx, stmt->left, reporter, arena);
        own_check_expr(ctx, stmt->right, reporter, arena);
        if (stmt->params) own_check_stmt(ctx, stmt->params, reporter, arena);
        own_pop_scope(ctx);
        break;
    }

    case AST_BLOCK: {
        AstNode *s;
        own_push_scope(ctx);
        for (s = stmt->params; s; s = s->next) {
            own_check_stmt(ctx, s, reporter, arena);
        }
        own_pop_scope(ctx);
        break;
    }

    case AST_MATCH:
        own_check_expr(ctx, stmt->left, reporter, arena);
        {
            AstNode *arm = stmt->params;
            while (arm) {
                if (arm->kind == AST_MATCH_ARM) {
                    own_push_scope(ctx);
                    own_check_stmt(ctx, arm->right, reporter, arena);
                    own_pop_scope(ctx);
                }
                arm = arm->next;
            }
        }
        break;

    case AST_BREAK:
    case AST_CONTINUE:
        break;

    default:
        /* Best effort */
        own_check_expr(ctx, stmt->left,  reporter, arena);
        own_check_expr(ctx, stmt->right, reporter, arena);
        break;
    }
}

/* Check ownership in a function body */
static void own_check_fn(OwnershipCtx *ctx, AstNode *fn,
                          ErrorReporter *reporter, Arena *arena) {
    AstNode *p;

    if (!fn) return;

    own_push_scope(ctx);

    /* Register function parameters as owned variables */
    for (p = fn->params; p; p = p->next) {
        if (p->kind == AST_PARAM && p->name) {
            own_register(ctx, p->name, (int)p->loc.line);
        }
    }

    /* Check function body */
    if (fn->left) {
        own_check_stmt(ctx, fn->left, reporter, arena);
    }

    own_pop_scope(ctx);
}

/* Main entry: walk all declarations and check ownership */
static void check_ownership(AstNode *program, ErrorReporter *reporter,
                             Arena *arena, bool strict) {
    AstNode *decl;
    OwnershipCtx ctx;

    if (!program || program->kind != AST_PROGRAM) return;

    own_ctx_init(&ctx, strict);

    for (decl = program->params; decl; decl = decl->next) {
        switch (decl->kind) {
        case AST_FN:
            own_check_fn(&ctx, decl, reporter, arena);
            break;

        case AST_AGENT: {
            /* Check agent methods */
            AstNode *method = decl->left;
            while (method) {
                if (method->kind == AST_FN) {
                    own_check_fn(&ctx, method, reporter, arena);
                }
                method = method->next;
            }
            break;
        }

        case AST_TOOL:
            own_check_fn(&ctx, decl, reporter, arena);
            break;

        case AST_GUARD:
            own_check_fn(&ctx, decl, reporter, arena);
            break;

        case AST_IMPL: {
            AstNode *method = decl->params;
            while (method) {
                if (method->kind == AST_FN) {
                    own_check_fn(&ctx, method, reporter, arena);
                }
                method = method->next;
            }
            break;
        }

        default:
            break;
        }
    }
}

/* ============================================================
 * Pass 8: Module Visibility (pub/priv) — enforced
 *
 * When multi-file imports merge declarations from other files,
 * any declaration that lacks `pub` is private to its module.
 * Accessing a non-pub declaration from another module is a
 * hard error that blocks compilation.
 * ============================================================ */

static int check_module_visibility(AstNode *program,
                                    ErrorReporter *reporter) {
    AstNode *decl;
    const char *main_file = NULL;
    int errors = 0;

    if (!program || program->kind != AST_PROGRAM) return 0;

    /* Determine the main file: it is the filename from the reporter. */
    main_file = reporter->filename;
    if (!main_file) return 0;

    for (decl = program->params; decl; decl = decl->next) {
        /* Only check named declarations that have a source location */
        if (!decl->name) continue;
        if (!decl->loc.filename) continue;

        /* Skip declarations from the main file — they are local */
        if (strcmp(decl->loc.filename, main_file) == 0) continue;

        /* Skip kinds that don't support pub annotation meaningfully */
        if (decl->kind == AST_USE || decl->kind == AST_MODULE ||
            decl->kind == AST_IMPL || decl->kind == AST_LET) continue;

        /* If the declaration is not pub, error — private symbols
         * cannot be accessed from another module */
        if (!decl->is_pub) {
            report_error_fmt(reporter, decl->loc,
                             "add 'pub' to export this declaration",
                             "imported declaration '%s' is private "
                             "(not marked pub)",
                             decl->name);
            errors++;
        }
    }

    return errors;
}

/* ============================================================
 * Public API
 * ============================================================ */

bool typecheck_program(AstNode *program, ErrorReporter *reporter,
                       Arena *arena) {
    SymbolTable st;
    CapRegistry cr;

    if (!program || !reporter || !arena) return false;
    if (program->kind != AST_PROGRAM) return false;

    memset(&st, 0, sizeof(st));
    memset(&cr, 0, sizeof(cr));

    /* Pass 1: Build symbol table (enforced — duplicates are hard errors) */
    register_declarations(&st, program, reporter, arena);

    /* Pass 1b: Build capability registry */
    build_cap_registry(&cr, program, arena);

    /* Pass 2: Capability verification (enforced — security critical) */
    check_capabilities(&st, &cr, program, reporter, arena);

    /* Pass 2b: Access control enforcement (network endpoints + binary) */
    {
        AccessPolicyRegistry apr;
        memset(&apr, 0, sizeof(apr));
        build_access_policies(&apr, program, arena);
        check_access_policies(&st, &apr, program, reporter, arena);
    }

    /* Pass 3: Guard enforcement (advisory — suggestions only) */
    int guard_start = reporter->count;
    check_guards(&st, &cr, program, reporter, arena);
    int guard_warnings = reporter->count - guard_start;

    /* Pass 4: Taint checking (enforced — security critical) */
    check_taint(&st, program, reporter, arena);

    /* Pass 4b: Secret tracking (enforced — confidentiality critical) */
    check_secrets(&st, program, reporter, arena);

    /* Pass 5: Budget presence (enforced — agents using tools must have budgets) */
    check_budgets(&st, program, reporter, arena);

    /* Pass 6: Basic type checking (enforced) */
    check_types(&st, program, reporter, arena);

    /* Pass 7: Enum LLM constraint detection (advisory) */
    int enum_start = reporter->count;
    check_enum_constraints(&st, program, reporter);
    int enum_warnings = reporter->count - enum_start;

    /* Pass 8: Module visibility — pub/priv enforcement (enforced) */
    check_module_visibility(program, reporter);

    /* Pass 9: Ownership & borrow checking (advisory — warnings only) */
    int own_start = reporter->count;
    check_ownership(program, reporter, arena, false);
    int own_warnings = reporter->count - own_start;

    /* Total errors minus advisory warnings = enforced errors */
    int total_errors = reporter->count;
    int advisory = guard_warnings + enum_warnings + own_warnings;
    int enforced = total_errors - advisory;

    return enforced == 0;
}
