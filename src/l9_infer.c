/*
 * Limceron Compiler -- L9 bidirectional type inference (Pass 10).
 *
 * Each `fn` body is its own unification scope. Synthesis walks an
 * expression bottom-up to deduce a type; checking compares a
 * synthesised type against an expected type. Diagnostics surface the
 * offending site with the two clashing types.
 *
 * The pass is intentionally conservative: it only flags clear conflicts
 * between primitive types it can resolve confidently. Any type it
 * cannot pin down stays as a fresh LCN_TYPE_VAR and the pass walks
 * past it without complaint. Existing typecheck passes (capability,
 * taint, ownership, ...) keep their own walkers and are untouched.
 *
 * Public entry: `lcn_l9_infer_types`, called from typecheck.c after the
 * ownership pass. The LcnType/LcnUnifyTable IR + the unify/resolve
 * helpers live here too because they are L9-specific.
 *
 * Compiles cleanly under: -std=c99 -Wall -Wextra -Werror -pedantic.
 */

#include "lcn.h"

#include <stdio.h>
#include <string.h>

/* The L9 walker needs to look up callee return types in the same
 * symbol table the typecheck pipeline built. We deliberately keep a
 * minimal mirror of typecheck.c's SymKind / Symbol / SymbolTable
 * layout here so we don't have to extend the public header surface
 * with implementation-only structs. The two layouts MUST stay in
 * sync -- if you reorder SymKind in typecheck.c, do the same here.
 */
typedef enum {
    L9_SYM_FN,
    L9_SYM_AGENT,
    L9_SYM_TOOL,
    L9_SYM_CAPABILITY,
    L9_SYM_GUARD,
    L9_SYM_GUARDSET,
    L9_SYM_BUDGET,
    L9_SYM_TAINT,
    L9_SYM_STRUCT,
    L9_SYM_ENUM,
    L9_SYM_TRAIT,
    L9_SYM_INTERFACE,
    L9_SYM_CONST,
    L9_SYM_SUPERVISOR,
    L9_SYM_SKILL,
    L9_SYM_PROMPT,
    L9_SYM_MESH,
    L9_SYM_MEMORY,
    L9_SYM_CHANNEL,
    L9_SYM_ROUTER,
    L9_SYM_STRATEGY,
    L9_SYM_LET,
    L9_SYM_TYPE_ALIAS
} L9SymKind;

#define L9_MAX_SYMBOLS 4096

typedef struct {
    const char *name;
    int         kind;       /* L9SymKind, kept as int to dodge enum-width warnings */
    AstNode    *node;
    SourceLoc   loc;
} L9Symbol;

typedef struct {
    L9Symbol entries[L9_MAX_SYMBOLS];
    int      count;
} L9SymbolTable;

/* ---- LcnType infrastructure -------------------------------------- */

LcnUnifyTable lcn_unify_table_new(Arena *arena) {
    LcnUnifyTable tbl;
    memset(&tbl, 0, sizeof(tbl));
    tbl.arena = arena;
    return tbl;
}

LcnType *lcn_type_fresh_var(LcnUnifyTable *tbl) {
    LcnType *t;
    if (!tbl || tbl->count >= LCN_INFER_MAX_VARS) return NULL;
    t = (LcnType *)arena_alloc(tbl->arena, sizeof(LcnType));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    t->kind   = LCN_TYPE_VAR;
    t->var_id = tbl->count;
    tbl->bindings[tbl->count++] = NULL;
    return t;
}

LcnType *lcn_type_lit(Arena *arena, LcnLitKind lit) {
    LcnType *t = (LcnType *)arena_alloc(arena, sizeof(LcnType));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));
    t->kind = LCN_TYPE_LIT;
    t->lit  = lit;
    return t;
}

LcnType *lcn_type_resolve(LcnUnifyTable *tbl, LcnType *t) {
    if (!t || !tbl) return t;
    while (t->kind == LCN_TYPE_VAR &&
           t->var_id >= 0 && t->var_id < tbl->count &&
           tbl->bindings[t->var_id]) {
        t = tbl->bindings[t->var_id];
    }
    return t;
}

bool lcn_unify(LcnUnifyTable *tbl, LcnType *a, LcnType *b) {
    if (!a || !b) return true;
    a = lcn_type_resolve(tbl, a);
    b = lcn_type_resolve(tbl, b);
    if (!a || !b) return true;
    if (a == b) return true;

    if (a->kind == LCN_TYPE_VAR) {
        if (a->var_id >= 0 && a->var_id < tbl->count) {
            tbl->bindings[a->var_id] = b;
        }
        return true;
    }
    if (b->kind == LCN_TYPE_VAR) {
        if (b->var_id >= 0 && b->var_id < tbl->count) {
            tbl->bindings[b->var_id] = a;
        }
        return true;
    }
    if (a->kind == LCN_TYPE_LIT && b->kind == LCN_TYPE_LIT) {
        if (a->lit == b->lit) {
            if (a->lit == LCN_LIT_OPAQUE) {
                if (!a->opaque_name || !b->opaque_name) return true;
                return strcmp(a->opaque_name, b->opaque_name) == 0;
            }
            return true;
        }
        /* L9 coercion: int <-> float. bool -> string still requires an
         * explicit `as string` cast (handled by AST_CAST). */
        if (a->lit == LCN_LIT_INT   && b->lit == LCN_LIT_FLOAT) return true;
        if (a->lit == LCN_LIT_FLOAT && b->lit == LCN_LIT_INT)   return true;
        return false;
    }
    if (a->kind == LCN_TYPE_FUNC && b->kind == LCN_TYPE_FUNC) {
        return lcn_unify(tbl, a->func_ret, b->func_ret);
    }
    return false;
}

const char *lcn_type_to_string(LcnUnifyTable *tbl, Arena *arena, LcnType *t) {
    char buf[64];
    t = lcn_type_resolve(tbl, t);
    if (!t) return "?";
    if (t->kind == LCN_TYPE_VAR) {
        snprintf(buf, sizeof(buf), "T#%d", t->var_id);
        return arena_strdup(arena, buf);
    }
    if (t->kind == LCN_TYPE_FUNC) return "<fn>";
    switch (t->lit) {
    case LCN_LIT_INT:    return "int";
    case LCN_LIT_FLOAT:  return "float";
    case LCN_LIT_BOOL:   return "bool";
    case LCN_LIT_STRING: return "string";
    case LCN_LIT_UNIT:   return "()";
    case LCN_LIT_OPAQUE: return t->opaque_name ? t->opaque_name : "<opaque>";
    case LCN_LIT_NONE:   return "?";
    }
    return "?";
}

/* ---- L9 inference walker ----------------------------------------- */

#define L9_MAX_LOCALS 256

typedef struct {
    LcnUnifyTable  tbl;
    L9SymbolTable *symtab;
    ErrorReporter *reporter;
    Arena         *arena;
    const char    *local_names[L9_MAX_LOCALS];
    LcnType       *local_types[L9_MAX_LOCALS];
    int            local_count;
    LcnType       *expected_ret;
} L9Ctx;

static LcnLitKind l9_named_type_to_lit(const char *name) {
    if (!name) return LCN_LIT_NONE;
    if (strcmp(name, "int")    == 0) return LCN_LIT_INT;
    if (strcmp(name, "i32")    == 0) return LCN_LIT_INT;
    if (strcmp(name, "i64")    == 0) return LCN_LIT_INT;
    if (strcmp(name, "float")  == 0) return LCN_LIT_FLOAT;
    if (strcmp(name, "f64")    == 0) return LCN_LIT_FLOAT;
    if (strcmp(name, "f32")    == 0) return LCN_LIT_FLOAT;
    if (strcmp(name, "bool")   == 0) return LCN_LIT_BOOL;
    if (strcmp(name, "string") == 0) return LCN_LIT_STRING;
    if (strcmp(name, "str")    == 0) return LCN_LIT_STRING;
    if (strcmp(name, "String") == 0) return LCN_LIT_STRING;
    if (strcmp(name, "void")   == 0) return LCN_LIT_UNIT;
    if (strcmp(name, "()")     == 0) return LCN_LIT_UNIT;
    return LCN_LIT_NONE;
}

static LcnType *l9_from_type_expr(L9Ctx *ctx, AstNode *texpr) {
    if (!texpr) return lcn_type_fresh_var(&ctx->tbl);
    if (texpr->kind == AST_TYPE_NAMED && texpr->name) {
        LcnLitKind k = l9_named_type_to_lit(texpr->name);
        if (k != LCN_LIT_NONE) return lcn_type_lit(ctx->arena, k);
        LcnType *t = lcn_type_lit(ctx->arena, LCN_LIT_OPAQUE);
        if (t) t->opaque_name = texpr->name;
        return t;
    }
    return lcn_type_fresh_var(&ctx->tbl);
}

static void l9_push_local(L9Ctx *ctx, const char *name, LcnType *t) {
    if (!name || !t) return;
    if (ctx->local_count >= L9_MAX_LOCALS) return;
    ctx->local_names[ctx->local_count] = name;
    ctx->local_types[ctx->local_count] = t;
    ctx->local_count++;
}

static LcnType *l9_lookup_local(L9Ctx *ctx, const char *name) {
    int i;
    if (!name) return NULL;
    for (i = ctx->local_count - 1; i >= 0; i--) {
        if (ctx->local_names[i] && strcmp(ctx->local_names[i], name) == 0)
            return ctx->local_types[i];
    }
    return NULL;
}

static AstNode *l9_find_fn(L9Ctx *ctx, const char *name) {
    int i;
    if (!name || !ctx->symtab) return NULL;
    for (i = 0; i < ctx->symtab->count; i++) {
        if ((ctx->symtab->entries[i].kind == (int)L9_SYM_FN ||
             ctx->symtab->entries[i].kind == (int)L9_SYM_TOOL) &&
            ctx->symtab->entries[i].name &&
            strcmp(ctx->symtab->entries[i].name, name) == 0)
            return ctx->symtab->entries[i].node;
    }
    return NULL;
}

static LcnType *l9_syn_expr(L9Ctx *ctx, AstNode *expr);
static LcnType *l9_syn_block(L9Ctx *ctx, AstNode *block);

static LcnType *l9_syn_block(L9Ctx *ctx, AstNode *block) {
    AstNode *s;
    int saved = ctx->local_count;
    LcnType *result = lcn_type_lit(ctx->arena, LCN_LIT_UNIT);

    if (!block) return result;

    if (block->kind == AST_BLOCK) {
        for (s = block->params; s; s = s->next) {
            if (s->kind == AST_LET && s->name) {
                LcnType *rhs = l9_syn_expr(ctx, s->right);
                if (s->type_expr) {
                    LcnType *declared = l9_from_type_expr(ctx, s->type_expr);
                    if (declared && rhs &&
                        !lcn_unify(&ctx->tbl, rhs, declared)) {
                        LcnType *r_rhs = lcn_type_resolve(&ctx->tbl, rhs);
                        LcnType *r_dec = lcn_type_resolve(&ctx->tbl, declared);
                        if (r_rhs && r_dec &&
                            r_rhs->kind == LCN_TYPE_LIT &&
                            r_dec->kind == LCN_TYPE_LIT &&
                            r_rhs->lit != LCN_LIT_OPAQUE &&
                            r_dec->lit != LCN_LIT_OPAQUE) {
                            report_error_fmt(ctx->reporter, s->loc,
                                "remove the annotation or change the initializer",
                                "cannot unify %s and %s in `let %s`",
                                lcn_type_to_string(&ctx->tbl, ctx->arena, r_rhs),
                                lcn_type_to_string(&ctx->tbl, ctx->arena, r_dec),
                                s->name);
                        }
                    }
                    l9_push_local(ctx, s->name, declared);
                } else {
                    l9_push_local(ctx, s->name,
                                  rhs ? rhs : lcn_type_fresh_var(&ctx->tbl));
                }
            } else if (s->kind == AST_EXPR_STMT) {
                (void)l9_syn_expr(ctx, s->left);
            } else if (s->kind == AST_IF) {
                (void)l9_syn_expr(ctx, s);
            }
            if (!s->next) {
                if (s->kind == AST_EXPR_STMT)
                    result = l9_syn_expr(ctx, s->left);
                else if (s->kind == AST_IF)
                    result = l9_syn_expr(ctx, s);
            }
        }
    } else {
        result = l9_syn_expr(ctx, block);
    }

    ctx->local_count = saved;
    return result;
}

static LcnType *l9_syn_expr(L9Ctx *ctx, AstNode *expr) {
    if (!expr) return lcn_type_fresh_var(&ctx->tbl);

    switch (expr->kind) {
    case AST_INT_LIT:
        return lcn_type_lit(ctx->arena, LCN_LIT_INT);
    case AST_FLOAT_LIT:
        return lcn_type_lit(ctx->arena, LCN_LIT_FLOAT);
    case AST_STRING_LIT:
    case AST_INTERP_STRING:
        return lcn_type_lit(ctx->arena, LCN_LIT_STRING);
    case AST_BOOL_LIT:
        return lcn_type_lit(ctx->arena, LCN_LIT_BOOL);

    case AST_IDENT: {
        LcnType *t = l9_lookup_local(ctx, expr->name);
        if (t) return t;
        return lcn_type_fresh_var(&ctx->tbl);
    }

    case AST_BINARY: {
        LcnType *l = l9_syn_expr(ctx, expr->left);
        LcnType *r = l9_syn_expr(ctx, expr->right);
        TokenKind op = expr->val.op;
        if (op == TOK_EQ_EQ  || op == TOK_NOT_EQ ||
            op == TOK_LT     || op == TOK_GT     ||
            op == TOK_LT_EQ  || op == TOK_GT_EQ  ||
            op == TOK_AND_AND|| op == TOK_PIPE_PIPE) {
            (void)l; (void)r;
            return lcn_type_lit(ctx->arena, LCN_LIT_BOOL);
        }
        LcnType *lr = lcn_type_resolve(&ctx->tbl, l);
        LcnType *rr = lcn_type_resolve(&ctx->tbl, r);
        if (op == TOK_PLUS &&
            lr && rr &&
            lr->kind == LCN_TYPE_LIT && rr->kind == LCN_TYPE_LIT &&
            (lr->lit == LCN_LIT_STRING || rr->lit == LCN_LIT_STRING)) {
            return lcn_type_lit(ctx->arena, LCN_LIT_STRING);
        }
        if (lr && rr &&
            lr->kind == LCN_TYPE_LIT && rr->kind == LCN_TYPE_LIT) {
            if (lr->lit == LCN_LIT_FLOAT || rr->lit == LCN_LIT_FLOAT)
                return lcn_type_lit(ctx->arena, LCN_LIT_FLOAT);
            if (lr->lit == LCN_LIT_INT && rr->lit == LCN_LIT_INT)
                return lcn_type_lit(ctx->arena, LCN_LIT_INT);
        }
        return l ? l : lcn_type_fresh_var(&ctx->tbl);
    }

    case AST_UNARY: {
        TokenKind op = expr->val.op;
        if (op == TOK_BANG) return lcn_type_lit(ctx->arena, LCN_LIT_BOOL);
        return l9_syn_expr(ctx, expr->left);
    }

    case AST_CALL: {
        AstNode *arg;
        for (arg = expr->params; arg; arg = arg->next)
            (void)l9_syn_expr(ctx, arg);
        if (expr->left && expr->left->kind == AST_IDENT && expr->left->name) {
            AstNode *callee = l9_find_fn(ctx, expr->left->name);
            if (callee && callee->type_expr)
                return l9_from_type_expr(ctx, callee->type_expr);
        }
        return lcn_type_fresh_var(&ctx->tbl);
    }

    case AST_HOST_CALL: {
        AstNode *arg;
        for (arg = expr->params; arg; arg = arg->next)
            (void)l9_syn_expr(ctx, arg);
        return lcn_type_fresh_var(&ctx->tbl);
    }

    case AST_IF: {
        LcnType *t1 = l9_syn_block(ctx, expr->right);
        LcnType *t2 = expr->params ? l9_syn_block(ctx, expr->params)
                                   : lcn_type_lit(ctx->arena, LCN_LIT_UNIT);
        LcnType *r1 = lcn_type_resolve(&ctx->tbl, t1);
        LcnType *r2 = lcn_type_resolve(&ctx->tbl, t2);
        if (r1 && r2 &&
            r1->kind == LCN_TYPE_LIT && r2->kind == LCN_TYPE_LIT &&
            r1->lit != LCN_LIT_OPAQUE && r2->lit != LCN_LIT_OPAQUE &&
            r1->lit != LCN_LIT_NONE   && r2->lit != LCN_LIT_NONE   &&
            r1->lit != LCN_LIT_UNIT   && r2->lit != LCN_LIT_UNIT) {
            if (!lcn_unify(&ctx->tbl, r1, r2)) {
                report_error_fmt(ctx->reporter, expr->loc,
                    "both branches of an `if` expression must agree "
                    "(or coerce via int -> float)",
                    "cannot unify %s and %s -- `if` branches disagree",
                    lcn_type_to_string(&ctx->tbl, ctx->arena, r1),
                    lcn_type_to_string(&ctx->tbl, ctx->arena, r2));
                return lcn_type_fresh_var(&ctx->tbl);
            }
        }
        return t1 ? t1 : t2;
    }

    case AST_CAST:
        if (expr->type_expr) return l9_from_type_expr(ctx, expr->type_expr);
        return lcn_type_fresh_var(&ctx->tbl);

    case AST_REF:
    case AST_DEREF:
    case AST_TRY:
    case AST_AWAIT:
    case AST_SPAWN:
    case AST_RESULT_OK:
    case AST_RESULT_ERR:
        (void)l9_syn_expr(ctx, expr->left);
        return lcn_type_fresh_var(&ctx->tbl);

    default: {
        AstNode *p;
        (void)l9_syn_expr(ctx, expr->left);
        (void)l9_syn_expr(ctx, expr->right);
        for (p = expr->params; p; p = p->next) (void)l9_syn_expr(ctx, p);
        return lcn_type_fresh_var(&ctx->tbl);
    }
    }
}

static bool l9_strict_return_type(LcnType *ret) {
    if (!ret || ret->kind != LCN_TYPE_LIT) return false;
    return ret->lit == LCN_LIT_INT    || ret->lit == LCN_LIT_FLOAT ||
           ret->lit == LCN_LIT_BOOL   || ret->lit == LCN_LIT_STRING;
}

static AstNode *l9_block_tail(AstNode *block) {
    AstNode *s, *last = NULL;
    if (!block) return NULL;
    if (block->kind == AST_BLOCK) {
        for (s = block->params; s; s = s->next) last = s;
    } else {
        last = block;
    }
    if (!last) return NULL;
    if (last->kind == AST_EXPR_STMT) return last->left;
    if (last->kind == AST_INT_LIT || last->kind == AST_FLOAT_LIT ||
        last->kind == AST_STRING_LIT || last->kind == AST_BOOL_LIT ||
        last->kind == AST_IDENT || last->kind == AST_BINARY ||
        last->kind == AST_CALL || last->kind == AST_IF) {
        return last;
    }
    return NULL;
}

static void l9_check_fn(L9Ctx *ctx, AstNode *fn) {
    AstNode *p;
    LcnType *ret_ty;
    LcnType *body_ty;
    int saved;

    if (!fn || !fn->left) return;

    saved = ctx->local_count;

    for (p = fn->params; p; p = p->next) {
        if (p->name && p->type_expr) {
            l9_push_local(ctx, p->name, l9_from_type_expr(ctx, p->type_expr));
        }
    }

    ret_ty = fn->type_expr ? l9_from_type_expr(ctx, fn->type_expr)
                           : lcn_type_lit(ctx->arena, LCN_LIT_UNIT);
    ctx->expected_ret = ret_ty;

    body_ty = l9_syn_block(ctx, fn->left);

    if (l9_strict_return_type(lcn_type_resolve(&ctx->tbl, ret_ty))) {
        AstNode *tail = l9_block_tail(fn->left);
        LcnType *tail_ty = NULL;
        if (tail) {
            int restore = ctx->local_count;
            AstNode *s;
            for (s = fn->left->params; s; s = s->next) {
                if (s->kind == AST_LET && s->name) {
                    LcnType *t = s->type_expr
                        ? l9_from_type_expr(ctx, s->type_expr)
                        : l9_syn_expr(ctx, s->right);
                    l9_push_local(ctx, s->name, t);
                }
                if (s == tail || (s->kind == AST_EXPR_STMT && s->left == tail))
                    break;
            }
            tail_ty = l9_syn_expr(ctx, tail);
            ctx->local_count = restore;
        } else {
            tail_ty = body_ty;
        }
        {
            LcnType *rret  = lcn_type_resolve(&ctx->tbl, ret_ty);
            LcnType *rtail = lcn_type_resolve(&ctx->tbl, tail_ty);
            if (rret && rtail &&
                rret->kind == LCN_TYPE_LIT && rtail->kind == LCN_TYPE_LIT &&
                rtail->lit != LCN_LIT_NONE && rtail->lit != LCN_LIT_OPAQUE &&
                rtail->lit != LCN_LIT_UNIT) {
                if (!lcn_unify(&ctx->tbl, rtail, rret)) {
                    report_error_fmt(ctx->reporter,
                        tail ? tail->loc : fn->loc,
                        "change the expression or relax the function "
                        "return type -- function boundaries still require "
                        "explicit annotations",
                        "cannot unify %s and %s in return of fn '%s'",
                        lcn_type_to_string(&ctx->tbl, ctx->arena, rtail),
                        lcn_type_to_string(&ctx->tbl, ctx->arena, rret),
                        fn->name ? fn->name : "<anon>");
                }
            }
        }
    }

    ctx->local_count = saved;
    ctx->expected_ret = NULL;
}

/* Public entry. typecheck.c declares this with `extern void` and calls
 * it as Pass 10 after the ownership pass. The opaque void *symtab is the
 * typecheck pipeline's SymbolTable -- structurally identical to
 * L9SymbolTable above. */
void lcn_l9_infer_types(void *symtab, AstNode *program,
                        ErrorReporter *reporter, Arena *arena);

void lcn_l9_infer_types(void *symtab, AstNode *program,
                        ErrorReporter *reporter, Arena *arena) {
    AstNode *decl, *method;
    L9Ctx ctx;

    if (!program || program->kind != AST_PROGRAM) return;

    memset(&ctx, 0, sizeof(ctx));
    ctx.symtab   = (L9SymbolTable *)symtab;
    ctx.reporter = reporter;
    ctx.arena    = arena;

    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_FN) {
            ctx.tbl = lcn_unify_table_new(arena);
            ctx.local_count = 0;
            l9_check_fn(&ctx, decl);
        } else if (decl->kind == AST_AGENT) {
            for (method = decl->left; method; method = method->next) {
                if (method->kind == AST_FN) {
                    ctx.tbl = lcn_unify_table_new(arena);
                    ctx.local_count = 0;
                    l9_check_fn(&ctx, method);
                }
            }
            for (method = decl->params; method; method = method->next) {
                if (method->kind == AST_FN) {
                    ctx.tbl = lcn_unify_table_new(arena);
                    ctx.local_count = 0;
                    l9_check_fn(&ctx, method);
                }
            }
        } else if (decl->kind == AST_IMPL) {
            for (method = decl->params; method; method = method->next) {
                if (method->kind == AST_FN) {
                    ctx.tbl = lcn_unify_table_new(arena);
                    ctx.local_count = 0;
                    l9_check_fn(&ctx, method);
                }
            }
        }
    }
}
