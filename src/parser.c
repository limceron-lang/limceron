/*
 * Limceron Compiler — Parser
 *
 * Recursive descent parser for declarations/statements.
 * Pratt parser (precedence climbing) for expressions.
 *
 * Error recovery: on syntax error, skip to next sync point and continue.
 */

#include "lcn.h"

/* ============================================================
 * AST Construction Helpers
 * ============================================================ */

const char *ast_kind_name(AstKind kind) {
    static const char *names[AST_COUNT] = {
        [AST_PROGRAM]       = "Program",
        [AST_MODULE]        = "Module",
        [AST_USE]           = "Use",
        [AST_FN]            = "Fn",
        [AST_STRUCT]        = "Struct",
        [AST_ENUM]          = "Enum",
        [AST_VARIANT]       = "Variant",
        [AST_TRAIT]         = "Trait",
        [AST_INTERFACE]     = "Interface",
        [AST_IMPL]          = "Impl",
        [AST_TYPE_ALIAS]    = "TypeAlias",
        [AST_CONST]         = "Const",
        [AST_FIELD]         = "Field",
        [AST_PARAM]         = "Param",
        [AST_BLOCK]         = "Block",
        [AST_LET]           = "Let",
        [AST_RETURN]        = "Return",
        [AST_DEFER]         = "Defer",
        [AST_ASSIGN]        = "Assign",
        [AST_EXPR_STMT]     = "ExprStmt",
        [AST_FOR]           = "For",
        [AST_WHILE]         = "While",
        [AST_LOOP]          = "Loop",
        [AST_BREAK]         = "Break",
        [AST_CONTINUE]      = "Continue",
        [AST_INT_LIT]       = "IntLit",
        [AST_FLOAT_LIT]     = "FloatLit",
        [AST_STRING_LIT]    = "StringLit",
        [AST_BOOL_LIT]      = "BoolLit",
        [AST_NONE_LIT]      = "NoneLit",
        [AST_IDENT]         = "Ident",
        [AST_BINARY]        = "Binary",
        [AST_UNARY]         = "Unary",
        [AST_CALL]          = "Call",
        [AST_FIELD_ACCESS]  = "FieldAccess",
        [AST_INDEX]         = "Index",
        [AST_METHOD_CALL]   = "MethodCall",
        [AST_IF]            = "If",
        [AST_MATCH]         = "Match",
        [AST_MATCH_ARM]     = "MatchArm",
        [AST_CLOSURE]       = "Closure",
        [AST_ARRAY]         = "Array",
        [AST_MAP]           = "Map",
        [AST_MAP_ENTRY]     = "MapEntry",
        [AST_TUPLE]         = "Tuple",
        [AST_CAST]          = "Cast",
        [AST_IS_EXPR]       = "IsExpr",
        [AST_RANGE]         = "Range",
        [AST_PIPE]          = "Pipe",
        [AST_SPAWN]         = "Spawn",
        [AST_AWAIT]         = "Await",
        [AST_SELECT]        = "Select",
        [AST_SELECT_ARM]    = "SelectArm",
        [AST_TRY]           = "Try",
        [AST_UNSAFE_BLOCK]  = "UnsafeBlock",
        [AST_COMPTIME]      = "Comptime",
        [AST_REF]           = "Ref",
        [AST_DEREF]         = "Deref",
        [AST_TYPE_NAMED]    = "TypeNamed",
        [AST_TYPE_REF]      = "TypeRef",
        [AST_TYPE_PTR]      = "TypePtr",
        [AST_TYPE_ARRAY]    = "TypeArray",
        [AST_TYPE_SLICE]    = "TypeSlice",
        [AST_TYPE_TUPLE]    = "TypeTuple",
        [AST_TYPE_FN]       = "TypeFn",
        [AST_TYPE_UNION]    = "TypeUnion",
        [AST_TYPE_OPTIONAL] = "TypeOptional",
        [AST_TYPE_INFER]    = "TypeInfer",
        [AST_PAT_WILDCARD]  = "PatWildcard",
        [AST_PAT_IDENT]     = "PatIdent",
        [AST_PAT_LITERAL]   = "PatLiteral",
        [AST_PAT_TYPED]     = "PatTyped",
        [AST_PAT_ENUM]      = "PatEnum",
        [AST_PAT_STRUCT]    = "PatStruct",
        [AST_PAT_TUPLE]     = "PatTuple",
        [AST_PAT_RANGE]     = "PatRange",
        [AST_PAT_OR]        = "PatOr",
        [AST_GENERIC_PARAM] = "GenericParam",
        [AST_ATTRIBUTE]     = "Attribute",
        /* Agent System */
        [AST_AGENT]           = "Agent",
        [AST_CAPABILITY]      = "Capability",
        [AST_CAPABILITY_ITEM] = "CapabilityItem",
        [AST_GUARD]           = "Guard",
        [AST_GUARDSET]        = "Guardset",
        [AST_INVARIANT]       = "Invariant",
        [AST_TAINT]           = "Taint",
        [AST_BUDGET]          = "Budget",
        [AST_BUDGET_FIELD]    = "BudgetField",
        [AST_TOOL]            = "Tool",
        [AST_SKILL]           = "Skill",
        [AST_PROMPT]          = "Prompt",
        [AST_PROMPT_SECTION]  = "PromptSection",
        [AST_PROMPT_PARAMS]   = "PromptParams",
        [AST_SUPERVISOR]      = "Supervisor",
        [AST_SUPERVISOR_CHILD]= "SupervisorChild",
        [AST_MESH]            = "Mesh",
        [AST_MESH_STAGE]      = "MeshStage",
        [AST_MESH_ROUTE]      = "MeshRoute",
        [AST_MEMORY]          = "Memory",
        [AST_CHANNEL]         = "Channel",
        [AST_ASK]             = "Ask",
        [AST_TELL]            = "Tell",
        [AST_ENSURE]          = "Ensure",
        [AST_REPEAT]          = "Repeat",
        [AST_WAIT_UNTIL]      = "WaitUntil",
        [AST_TRY_OTHERWISE]   = "TryOtherwise",
        [AST_KEEP_WHERE]      = "KeepWhere",
        [AST_EACH]            = "Each",
        [AST_TYPE_TAINTED]    = "TypeTainted",
        [AST_TYPE_SECRET]     = "TypeSecret",
        [AST_ROUTER]          = "Router",
        [AST_ROUTE_RULE]      = "RouteRule",
        [AST_STRATEGY]        = "Strategy",
        /* Structured Concurrency */
        [AST_TASK_GROUP]          = "TaskGroup",
        /* Access Control Policy */
        [AST_CAP_ENDPOINT_RULE] = "CapEndpointRule",
        [AST_CAP_BINARY_RULE]   = "CapBinaryRule",
        [AST_CAP_PATH_RULE]     = "CapPathRule",
        [AST_CAP_DENY_RANGE]    = "CapDenyRange",
        [AST_CAP_DEFAULT]       = "CapDefault",
        /* FFI link directive */
        [AST_LINK]              = "Link",
        /* Prometheus metrics */
        [AST_METRICS]           = "Metrics",
        [AST_METRICS_FIELD]     = "MetricsField",
        /* Progress reporting */
        [AST_PROGRESS]          = "Progress",
        /* Health probes */
        [AST_HEALTH]            = "Health",
    };
    if (kind >= 0 && kind < AST_COUNT && names[kind]) return names[kind];
    return "Unknown";
}

AstNode *ast_new(Arena *a, AstKind kind, SourceLoc loc) {
    AstNode *n = (AstNode *)arena_alloc(a, sizeof(AstNode));
    n->kind = kind;
    n->loc = loc;
    return n;
}

AstNode *ast_append(AstNode *list, AstNode *node) {
    if (!list) return node;
    AstNode *tail = list;
    while (tail->next) tail = tail->next;
    tail->next = node;
    return list;
}

int ast_list_len(const AstNode *list) {
    int n = 0;
    while (list) { n++; list = list->next; }
    return n;
}

static void indent_print(int indent) {
    int i;
    for (i = 0; i < indent; i++) fprintf(stderr, "  ");
}

void ast_print(const AstNode *node, int indent) {
    if (!node) return;
    indent_print(indent);
    fprintf(stderr, "%s", ast_kind_name(node->kind));
    if (node->name) fprintf(stderr, " \"%s\"", node->name);
    if (node->kind == AST_INT_LIT) fprintf(stderr, " %lld", (long long)node->val.int_val);
    if (node->kind == AST_FLOAT_LIT) fprintf(stderr, " %f", node->val.float_val);
    if (node->kind == AST_STRING_LIT && node->val.str_val) fprintf(stderr, " \"%s\"", node->val.str_val);
    if (node->kind == AST_BOOL_LIT) fprintf(stderr, " %s", node->val.bool_val ? "true" : "false");
    if (node->kind == AST_BINARY || node->kind == AST_UNARY || node->kind == AST_ASSIGN)
        fprintf(stderr, " [%s]", token_kind_name(node->val.op));
    if (node->is_pub) fprintf(stderr, " pub");
    if (node->is_mut) fprintf(stderr, " mut");
    fprintf(stderr, " @%u:%u", node->loc.line, node->loc.column);
    fprintf(stderr, "\n");

    if (node->attributes) { indent_print(indent+1); fprintf(stderr, "attrs:\n"); ast_print(node->attributes, indent+2); }
    if (node->generics) { indent_print(indent+1); fprintf(stderr, "generics:\n"); ast_print(node->generics, indent+2); }
    if (node->type_expr) { indent_print(indent+1); fprintf(stderr, "type:\n"); ast_print(node->type_expr, indent+2); }
    if (node->left) { indent_print(indent+1); fprintf(stderr, "left:\n"); ast_print(node->left, indent+2); }
    if (node->right) { indent_print(indent+1); fprintf(stderr, "right:\n"); ast_print(node->right, indent+2); }
    if (node->params) { indent_print(indent+1); fprintf(stderr, "params:\n"); ast_print(node->params, indent+2); }

    if (node->next) ast_print(node->next, indent);
}

/* ============================================================
 * Parser Core
 * ============================================================ */

Parser parser_new(Lexer *lexer, Arena *arena, ErrorReporter *reporter) {
    Parser p;
    memset(&p, 0, sizeof(p));
    p.lexer = lexer;
    p.arena = arena;
    p.reporter = reporter;
    p.had_error = false;
    p.panic_mode = false;
    /* Prime the parser with first token */
    p.current = lexer_next(lexer);
    return p;
}

static Token parser_advance(Parser *p) {
    p->previous = p->current;
    for (;;) {
        p->current = lexer_next(p->lexer);
        if (p->current.kind != TOK_ERROR) break;
        /* lexer already reported the error */
        p->had_error = true;
    }
    return p->previous;
}

static bool parser_check(Parser *p, TokenKind kind) {
    return p->current.kind == kind;
}

static bool parser_match(Parser *p, TokenKind kind) {
    if (!parser_check(p, kind)) return false;
    parser_advance(p);
    return true;
}

static Token parser_expect(Parser *p, TokenKind kind, const char *context) {
    if (parser_check(p, kind)) {
        return parser_advance(p);
    }
    char msg[256];
    snprintf(msg, sizeof(msg), "expected '%s' %s, found '%s'",
             token_kind_name(kind), context, token_kind_name(p->current.kind));
    report_error(p->reporter, p->current.loc, msg, NULL);
    p->had_error = true;
    p->panic_mode = true;
    /* Return a synthetic token to keep going */
    {
        Token t;
        memset(&t, 0, sizeof(t));
        t.kind = kind;
        t.loc = p->current.loc;
        return t;
    }
}

static void parser_synchronize(Parser *p) {
    p->panic_mode = false;
    while (p->current.kind != TOK_EOF) {
        if (p->previous.kind == TOK_SEMICOLON) return;
        switch (p->current.kind) {
        case TOK_FN:
        case TOK_STRUCT:
        case TOK_ENUM:
        case TOK_TRAIT:
        case TOK_INTERFACE:
        case TOK_IMPL:
        case TOK_LET:
        case TOK_FOR:
        case TOK_WHILE:
        case TOK_LOOP:
        case TOK_IF:
        case TOK_MATCH:
        case TOK_RETURN:
        case TOK_MOD:
        case TOK_USE:
        case TOK_PUB:
        case TOK_TYPE:
        case TOK_CONST:
        case TOK_AGENT:
        case TOK_CAPABILITY:
        case TOK_GUARD:
        case TOK_TAINT:
        case TOK_BUDGET:
        case TOK_TOOL:
        case TOK_SKILL:
        case TOK_SUPERVISOR:
        case TOK_MESH:
        case TOK_ROUTER:
        case TOK_PROGRESS:
            return;
        default:
            parser_advance(p);
            break;
        }
    }
}

static void skip_semis(Parser *p) {
    while (parser_match(p, TOK_SEMICOLON)) { }
}

/* ============================================================
 * Forward Declarations
 * ============================================================ */

static AstNode *parse_expr(Parser *p, Precedence min_prec);
static AstNode *parse_statement(Parser *p);
static AstNode *parse_block(Parser *p);
static AstNode *parse_pattern(Parser *p);
static bool is_ident_or_keyword(TokenKind kind);
static const char *consume_ident_name(Parser *p);

AstNode *parse_expression(Parser *p) {
    return parse_expr(p, PREC_NONE);
}

/* ============================================================
 * Type Expression Parser
 * ============================================================ */

static AstNode *parse_type_primary(Parser *p) {
    SourceLoc loc = p->current.loc;

    /* secret T — compile-time confidentiality wrapper */
    if (parser_match(p, TOK_SECRET)) {
        AstNode *node = ast_new(p->arena, AST_TYPE_SECRET, loc);
        node->left = parse_type_primary(p);
        return node;
    }

    /* &T or &mut T */
    if (parser_match(p, TOK_AMP)) {
        AstNode *node = ast_new(p->arena, AST_TYPE_REF, loc);
        node->is_mut = parser_match(p, TOK_MUT);
        node->left = parse_type_primary(p);
        return node;
    }

    /* *const T or *mut T */
    if (parser_match(p, TOK_STAR)) {
        AstNode *node = ast_new(p->arena, AST_TYPE_PTR, loc);
        if (parser_match(p, TOK_MUT)) {
            node->is_mut = true;
        } else {
            /* *const or just * (default const) */
            if (p->current.kind == TOK_IDENT && p->current.value.str_val &&
                strcmp(p->current.value.str_val, "const") == 0) {
                parser_advance(p);
            }
        }
        node->left = parse_type_primary(p);
        return node;
    }

    /* [T] or [T; N] */
    if (parser_match(p, TOK_LBRACKET)) {
        AstNode *elem = parse_type_expr(p);
        if (parser_match(p, TOK_SEMICOLON)) {
            AstNode *node = ast_new(p->arena, AST_TYPE_ARRAY, loc);
            node->left = elem;
            node->right = parse_expression(p);
            parser_expect(p, TOK_RBRACKET, "after array type size");
            return node;
        }
        parser_expect(p, TOK_RBRACKET, "after slice type");
        {
            AstNode *node = ast_new(p->arena, AST_TYPE_SLICE, loc);
            node->left = elem;
            return node;
        }
    }

    /* (T, T, ...) — tuple type or grouped type */
    if (parser_match(p, TOK_LPAREN)) {
        AstNode *first = parse_type_expr(p);
        if (parser_match(p, TOK_COMMA)) {
            AstNode *node = ast_new(p->arena, AST_TYPE_TUPLE, loc);
            node->params = first;
            while (!parser_check(p, TOK_RPAREN) && !parser_check(p, TOK_EOF)) {
                AstNode *elem = parse_type_expr(p);
                first = ast_append(first, elem);
                if (!parser_match(p, TOK_COMMA)) break;
            }
            parser_expect(p, TOK_RPAREN, "after tuple type");
            return node;
        }
        parser_expect(p, TOK_RPAREN, "after grouped type");
        return first;
    }

    /* fn(T, T) -> T */
    if (parser_match(p, TOK_FN)) {
        AstNode *node = ast_new(p->arena, AST_TYPE_FN, loc);
        parser_expect(p, TOK_LPAREN, "after 'fn' in function type");
        AstNode *param_list = NULL;
        while (!parser_check(p, TOK_RPAREN) && !parser_check(p, TOK_EOF)) {
            AstNode *pt = parse_type_expr(p);
            param_list = ast_append(param_list, pt);
            if (!parser_match(p, TOK_COMMA)) break;
        }
        parser_expect(p, TOK_RPAREN, "after function type parameters");
        node->params = param_list;
        if (parser_match(p, TOK_ARROW)) {
            node->type_expr = parse_type_expr(p);
        }
        return node;
    }

    /* Named type: ident<T, U> */
    if (parser_check(p, TOK_IDENT)) {
        AstNode *node = ast_new(p->arena, AST_TYPE_NAMED, loc);
        node->name = parser_advance(p).value.str_val;

        /* Qualified name: a.b.c */
        while (parser_match(p, TOK_DOT)) {
            if (parser_check(p, TOK_IDENT)) {
                const char *next = parser_advance(p).value.str_val;
                /* Build qualified name */
                size_t len1 = strlen(node->name);
                size_t len2 = strlen(next);
                char *qn = (char *)arena_alloc(p->arena, len1 + 1 + len2 + 1);
                memcpy(qn, node->name, len1);
                qn[len1] = '.';
                memcpy(qn + len1 + 1, next, len2);
                qn[len1 + 1 + len2] = '\0';
                node->name = qn;
            }
        }

        /* Generic arguments <T, U> */
        if (parser_match(p, TOK_LT)) {
            AstNode *args = NULL;
            while (!parser_check(p, TOK_GT) && !parser_check(p, TOK_EOF)) {
                AstNode *arg = parse_type_expr(p);
                args = ast_append(args, arg);
                if (!parser_match(p, TOK_COMMA)) break;
            }
            parser_expect(p, TOK_GT, "after generic arguments");
            node->generics = args;
        }

        return node;
    }

    /* void, never — treated as named types */
    report_error(p->reporter, p->current.loc, "expected type expression", NULL);
    p->had_error = true;
    return ast_new(p->arena, AST_TYPE_INFER, loc);
}

AstNode *parse_type_expr(Parser *p) {
    AstNode *ty = parse_type_primary(p);

    /* T? → Optional<T> */
    if (parser_match(p, TOK_QUESTION)) {
        SourceLoc loc = p->previous.loc;
        AstNode *opt = ast_new(p->arena, AST_TYPE_OPTIONAL, loc);
        opt->left = ty;
        ty = opt;
    }

    /* T | U | V → Union type */
    if (parser_check(p, TOK_PIPE)) {
        SourceLoc loc = p->current.loc;
        AstNode *un = ast_new(p->arena, AST_TYPE_UNION, loc);
        un->params = ty;
        while (parser_match(p, TOK_PIPE)) {
            AstNode *variant = parse_type_primary(p);
            if (parser_match(p, TOK_QUESTION)) {
                AstNode *opt = ast_new(p->arena, AST_TYPE_OPTIONAL, p->previous.loc);
                opt->left = variant;
                variant = opt;
            }
            ty = ast_append(ty, variant);
        }
        return un;
    }

    return ty;
}

/* ============================================================
 * Generic Parameter Parser
 * ============================================================ */

static AstNode *parse_generic_params(Parser *p) {
    if (!parser_match(p, TOK_LT)) return NULL;

    AstNode *params = NULL;
    while (!parser_check(p, TOK_GT) && !parser_check(p, TOK_EOF)) {
        SourceLoc loc = p->current.loc;
        AstNode *gp = ast_new(p->arena, AST_GENERIC_PARAM, loc);

        /* "region" keyword for lifetime-like params */
        if (parser_check(p, TOK_IDENT) && p->current.value.str_val &&
            strcmp(p->current.value.str_val, "region") == 0) {
            parser_advance(p);
            gp->name = parser_expect(p, TOK_IDENT, "after 'region'").value.str_val;
            gp->is_mut = true;  /* flag: this is a region param */
        } else {
            gp->name = parser_expect(p, TOK_IDENT, "in generic parameter").value.str_val;
            /* Constraints: T: Foo + Bar */
            if (parser_match(p, TOK_COLON)) {
                AstNode *bounds = NULL;
                AstNode *bound = parse_type_primary(p);
                bounds = ast_append(bounds, bound);
                while (parser_match(p, TOK_PLUS)) {
                    bound = parse_type_primary(p);
                    bounds = ast_append(bounds, bound);
                }
                gp->params = bounds;
            }
        }

        params = ast_append(params, gp);
        if (!parser_match(p, TOK_COMMA)) break;
    }
    parser_expect(p, TOK_GT, "after generic parameters");
    return params;
}

/* ============================================================
 * Attribute Parser
 * ============================================================ */

static AstNode *parse_attributes(Parser *p) {
    AstNode *attrs = NULL;
    while (parser_check(p, TOK_HASH)) {
        SourceLoc loc = p->current.loc;
        parser_advance(p);
        parser_expect(p, TOK_LBRACKET, "after '#'");
        AstNode *attr = ast_new(p->arena, AST_ATTRIBUTE, loc);
        attr->name = parser_expect(p, TOK_IDENT, "in attribute").value.str_val;
        /* Parse attribute arguments if present */
        if (parser_match(p, TOK_LPAREN)) {
            /* Skip attribute arguments for now — just consume until ) */
            int depth = 1;
            while (depth > 0 && !parser_check(p, TOK_EOF)) {
                if (parser_check(p, TOK_LPAREN)) depth++;
                if (parser_check(p, TOK_RPAREN)) depth--;
                if (depth > 0) parser_advance(p);
            }
            parser_expect(p, TOK_RPAREN, "after attribute arguments");
        }
        parser_expect(p, TOK_RBRACKET, "after attribute");
        attrs = ast_append(attrs, attr);
        skip_semis(p);
    }
    return attrs;
}

/* ============================================================
 * Pattern Parser
 * ============================================================ */

static AstNode *parse_pattern(Parser *p) {
    SourceLoc loc = p->current.loc;

    /* _ wildcard */
    if (parser_check(p, TOK_IDENT) && p->current.value.str_val &&
        strcmp(p->current.value.str_val, "_") == 0) {
        parser_advance(p);
        return ast_new(p->arena, AST_PAT_WILDCARD, loc);
    }

    /* Literal patterns */
    if (parser_check(p, TOK_INT_LIT)) {
        AstNode *n = ast_new(p->arena, AST_PAT_LITERAL, loc);
        n->val.int_val = parser_advance(p).value.int_val;
        /* Range pattern: 1..=9 or 1..9 */
        if (parser_check(p, TOK_DOT_DOT) || parser_check(p, TOK_DOT_DOT_EQ)) {
            bool inclusive = parser_check(p, TOK_DOT_DOT_EQ);
            parser_advance(p);
            AstNode *range = ast_new(p->arena, AST_PAT_RANGE, loc);
            range->left = n;
            range->right = parse_pattern(p);
            range->is_mut = inclusive;
            return range;
        }
        return n;
    }
    if (parser_check(p, TOK_STRING_LIT)) {
        AstNode *n = ast_new(p->arena, AST_PAT_LITERAL, loc);
        n->val.str_val = parser_advance(p).value.str_val;
        return n;
    }
    if (parser_match(p, TOK_TRUE)) {
        AstNode *n = ast_new(p->arena, AST_PAT_LITERAL, loc);
        n->val.bool_val = true;
        return n;
    }
    if (parser_match(p, TOK_FALSE)) {
        AstNode *n = ast_new(p->arena, AST_PAT_LITERAL, loc);
        n->val.bool_val = false;
        return n;
    }
    if (parser_match(p, TOK_NONE)) {
        return ast_new(p->arena, AST_PAT_LITERAL, loc);
    }

    /* Tuple pattern: (a, b, c) */
    if (parser_match(p, TOK_LPAREN)) {
        AstNode *pat = ast_new(p->arena, AST_PAT_TUPLE, loc);
        AstNode *elems = NULL;
        while (!parser_check(p, TOK_RPAREN) && !parser_check(p, TOK_EOF)) {
            AstNode *elem = parse_pattern(p);
            elems = ast_append(elems, elem);
            if (!parser_match(p, TOK_COMMA)) break;
        }
        parser_expect(p, TOK_RPAREN, "after tuple pattern");
        pat->params = elems;
        return pat;
    }

    /* Identifier pattern, possibly typed or enum */
    if (parser_check(p, TOK_IDENT)) {
        AstNode *n = ast_new(p->arena, AST_PAT_IDENT, loc);
        n->name = parser_advance(p).value.str_val;

        /* Qualified: Enum.Variant */
        while (parser_match(p, TOK_DOT)) {
            if (parser_check(p, TOK_IDENT)) {
                const char *next = parser_advance(p).value.str_val;
                size_t len1 = strlen(n->name);
                size_t len2 = strlen(next);
                char *qn = (char *)arena_alloc(p->arena, len1 + 1 + len2 + 1);
                memcpy(qn, n->name, len1);
                qn[len1] = '.';
                memcpy(qn + len1 + 1, next, len2);
                qn[len1 + 1 + len2] = '\0';
                n->name = qn;
            }
        }

        /* Enum pattern: Variant(a, b) */
        if (parser_match(p, TOK_LPAREN)) {
            n->kind = AST_PAT_ENUM;
            AstNode *fields = NULL;
            while (!parser_check(p, TOK_RPAREN) && !parser_check(p, TOK_EOF)) {
                AstNode *f = parse_pattern(p);
                fields = ast_append(fields, f);
                if (!parser_match(p, TOK_COMMA)) break;
            }
            parser_expect(p, TOK_RPAREN, "after enum pattern");
            n->params = fields;
            return n;
        }

        /* Struct pattern: Name { a, b: c } */
        if (parser_match(p, TOK_LBRACE)) {
            n->kind = AST_PAT_STRUCT;
            AstNode *fields = NULL;
            while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
                AstNode *f = parse_pattern(p);
                fields = ast_append(fields, f);
                if (!parser_match(p, TOK_COMMA)) break;
            }
            parser_expect(p, TOK_RBRACE, "after struct pattern");
            n->params = fields;
            return n;
        }

        /* Typed pattern: name: Type */
        if (parser_match(p, TOK_COLON)) {
            n->kind = AST_PAT_TYPED;
            n->type_expr = parse_type_expr(p);
            return n;
        }

        /* Or pattern: a | b */
        if (parser_check(p, TOK_PIPE)) {
            AstNode *or_pat = ast_new(p->arena, AST_PAT_OR, loc);
            or_pat->left = n;
            while (parser_match(p, TOK_PIPE)) {
                AstNode *rhs = parse_pattern(p);
                or_pat->left = ast_append(or_pat->left, rhs);
            }
            return or_pat;
        }

        return n;
    }

    report_error(p->reporter, p->current.loc, "expected pattern", NULL);
    p->had_error = true;
    return ast_new(p->arena, AST_PAT_WILDCARD, loc);
}

/* ============================================================
 * Expression Parser (Pratt)
 * ============================================================ */

/* Forward declarations for parse functions */
static AstNode *parse_prefix(Parser *p);
static AstNode *parse_infix(Parser *p, AstNode *left, TokenKind op);

static Precedence get_precedence(TokenKind kind) {
    switch (kind) {
    case TOK_PIPE_GT:                           return PREC_PIPE_OP;
    case TOK_PIPE_PIPE:                         return PREC_OR;
    case TOK_AND_AND:                           return PREC_AND;
    case TOK_PIPE:                              return PREC_BIT_OR;
    case TOK_CARET:                             return PREC_BIT_XOR;
    case TOK_AMP:                               return PREC_BIT_AND;
    case TOK_EQ_EQ: case TOK_NOT_EQ:           return PREC_EQUALITY;
    case TOK_LT: case TOK_GT:
    case TOK_LT_EQ: case TOK_GT_EQ:            return PREC_COMPARISON;
    case TOK_SHL: case TOK_SHR:                return PREC_SHIFT;
    case TOK_DOT_DOT: case TOK_DOT_DOT_EQ:     return PREC_RANGE;
    case TOK_PLUS: case TOK_MINUS:              return PREC_TERM;
    case TOK_STAR: case TOK_SLASH:
    case TOK_PERCENT:                           return PREC_FACTOR;
    case TOK_POWER:                             return PREC_POWER;
    case TOK_DOT: case TOK_LPAREN:
    case TOK_LBRACKET: case TOK_QUESTION:
    case TOK_AS: case TOK_IS:                   return PREC_POSTFIX;
    default:                                    return PREC_NONE;
    }
}

static AstNode *parse_expr(Parser *p, Precedence min_prec) {
    AstNode *left = parse_prefix(p);
    if (!left) return NULL;

    while (!parser_check(p, TOK_EOF)) {
        TokenKind op = p->current.kind;
        Precedence prec = get_precedence(op);

        if (prec <= min_prec) break;

        /* Postfix operators */
        if (op == TOK_DOT) {
            parser_advance(p);
            SourceLoc loc = p->previous.loc;
            const char *field = parser_expect(p, TOK_IDENT, "after '.'").value.str_val;

            /* Method call: expr.method(args) */
            if (parser_match(p, TOK_LPAREN)) {
                AstNode *node = ast_new(p->arena, AST_METHOD_CALL, loc);
                node->left = left;
                node->name = field;
                AstNode *args = NULL;
                while (!parser_check(p, TOK_RPAREN) && !parser_check(p, TOK_EOF)) {
                    AstNode *arg = parse_expr(p, PREC_NONE);
                    args = ast_append(args, arg);
                    if (!parser_match(p, TOK_COMMA)) break;
                }
                parser_expect(p, TOK_RPAREN, "after method arguments");
                node->params = args;
                left = node;
            } else {
                /* Field access */
                AstNode *node = ast_new(p->arena, AST_FIELD_ACCESS, loc);
                node->left = left;
                node->name = field;
                left = node;
            }
            continue;
        }

        if (op == TOK_LPAREN) {
            /* Function call */
            parser_advance(p);
            SourceLoc loc = p->previous.loc;
            AstNode *node = ast_new(p->arena, AST_CALL, loc);
            node->left = left;
            AstNode *args = NULL;
            while (!parser_check(p, TOK_RPAREN) && !parser_check(p, TOK_EOF)) {
                /* Named argument: name: value */
                AstNode *arg;
                if (parser_check(p, TOK_IDENT) && /* lookahead for colon */
                    p->current.kind == TOK_IDENT) {
                    /* Save state to check for named args */
                    arg = parse_expr(p, PREC_NONE);
                }
                else {
                    arg = parse_expr(p, PREC_NONE);
                }
                args = ast_append(args, arg);
                if (!parser_match(p, TOK_COMMA)) break;
            }
            parser_expect(p, TOK_RPAREN, "after function arguments");
            node->params = args;
            left = node;
            continue;
        }

        if (op == TOK_LBRACKET) {
            /* Index: expr[index] */
            parser_advance(p);
            SourceLoc loc = p->previous.loc;
            AstNode *node = ast_new(p->arena, AST_INDEX, loc);
            node->left = left;
            node->right = parse_expr(p, PREC_NONE);
            parser_expect(p, TOK_RBRACKET, "after index expression");
            left = node;
            continue;
        }

        if (op == TOK_QUESTION) {
            /* Try: expr? */
            parser_advance(p);
            SourceLoc loc = p->previous.loc;
            AstNode *node = ast_new(p->arena, AST_TRY, loc);
            node->left = left;
            left = node;
            continue;
        }

        if (op == TOK_AS) {
            /* Cast: expr as Type */
            parser_advance(p);
            SourceLoc loc = p->previous.loc;
            AstNode *node = ast_new(p->arena, AST_CAST, loc);
            node->left = left;
            node->type_expr = parse_type_primary(p);
            left = node;
            continue;
        }

        if (op == TOK_IS) {
            /* Type check: expr is Type */
            parser_advance(p);
            SourceLoc loc = p->previous.loc;
            AstNode *node = ast_new(p->arena, AST_IS_EXPR, loc);
            node->left = left;
            node->type_expr = parse_type_primary(p);
            left = node;
            continue;
        }

        /* Infix binary operators */
        left = parse_infix(p, left, op);
    }

    return left;
}

static AstNode *parse_infix(Parser *p, AstNode *left, TokenKind op) {
    SourceLoc loc = p->current.loc;
    Precedence prec = get_precedence(op);
    parser_advance(p);

    /* Right-associative for ** */
    Precedence next_prec = (op == TOK_POWER) ? (Precedence)(prec - 1) : prec;

    /* Range operators */
    if (op == TOK_DOT_DOT || op == TOK_DOT_DOT_EQ) {
        AstNode *node = ast_new(p->arena, AST_RANGE, loc);
        node->left = left;
        node->is_mut = (op == TOK_DOT_DOT_EQ);
        /* Right side is optional for open ranges */
        if (!parser_check(p, TOK_RPAREN) && !parser_check(p, TOK_RBRACKET) &&
            !parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_COMMA) &&
            !parser_check(p, TOK_SEMICOLON) && !parser_check(p, TOK_EOF) &&
            !parser_check(p, TOK_LBRACE)) {
            node->right = parse_expr(p, next_prec);
        }
        return node;
    }

    /* Pipe operator */
    if (op == TOK_PIPE_GT) {
        AstNode *node = ast_new(p->arena, AST_PIPE, loc);
        node->left = left;
        node->right = parse_expr(p, next_prec);
        return node;
    }

    /* Standard binary */
    AstNode *node = ast_new(p->arena, AST_BINARY, loc);
    node->val.op = op;
    node->left = left;
    node->right = parse_expr(p, next_prec);
    return node;
}

static AstNode *parse_prefix(Parser *p) {
    SourceLoc loc = p->current.loc;

    /* Unary operators: - ! ~ */
    if (parser_check(p, TOK_MINUS) || parser_check(p, TOK_BANG) || parser_check(p, TOK_TILDE)) {
        TokenKind op = p->current.kind;
        parser_advance(p);
        AstNode *node = ast_new(p->arena, AST_UNARY, loc);
        node->val.op = op;
        node->left = parse_expr(p, PREC_UNARY);
        return node;
    }

    /* Reference: &expr or &mut expr */
    if (parser_match(p, TOK_AMP)) {
        AstNode *node = ast_new(p->arena, AST_REF, loc);
        node->is_mut = parser_match(p, TOK_MUT);
        node->left = parse_expr(p, PREC_UNARY);
        return node;
    }

    /* Dereference: *expr */
    if (parser_match(p, TOK_STAR)) {
        AstNode *node = ast_new(p->arena, AST_DEREF, loc);
        node->left = parse_expr(p, PREC_UNARY);
        return node;
    }

    /* Integer literal */
    if (parser_check(p, TOK_INT_LIT)) {
        AstNode *node = ast_new(p->arena, AST_INT_LIT, loc);
        node->val.int_val = parser_advance(p).value.int_val;
        return node;
    }

    /* Float literal */
    if (parser_check(p, TOK_FLOAT_LIT)) {
        AstNode *node = ast_new(p->arena, AST_FLOAT_LIT, loc);
        node->val.float_val = parser_advance(p).value.float_val;
        return node;
    }

    /* String literal */
    if (parser_check(p, TOK_STRING_LIT)) {
        AstNode *node = ast_new(p->arena, AST_STRING_LIT, loc);
        node->val.str_val = parser_advance(p).value.str_val;
        return node;
    }

    /* Bool literals */
    if (parser_match(p, TOK_TRUE)) {
        AstNode *node = ast_new(p->arena, AST_BOOL_LIT, loc);
        node->val.bool_val = true;
        return node;
    }
    if (parser_match(p, TOK_FALSE)) {
        AstNode *node = ast_new(p->arena, AST_BOOL_LIT, loc);
        node->val.bool_val = false;
        return node;
    }

    /* None literal */
    if (parser_match(p, TOK_NONE)) {
        return ast_new(p->arena, AST_NONE_LIT, loc);
    }

    /* Grouped expression / Tuple / Unit: () or (expr) or (expr, expr, ...) */
    if (parser_match(p, TOK_LPAREN)) {
        /* Empty tuple / unit: () */
        if (parser_match(p, TOK_RPAREN)) {
            AstNode *tup = ast_new(p->arena, AST_TUPLE, loc);
            return tup;
        }
        AstNode *first = parse_expr(p, PREC_NONE);
        if (parser_match(p, TOK_COMMA)) {
            AstNode *tup = ast_new(p->arena, AST_TUPLE, loc);
            tup->params = first;
            while (!parser_check(p, TOK_RPAREN) && !parser_check(p, TOK_EOF)) {
                AstNode *elem = parse_expr(p, PREC_NONE);
                first = ast_append(first, elem);
                if (!parser_match(p, TOK_COMMA)) break;
            }
            parser_expect(p, TOK_RPAREN, "after tuple");
            return tup;
        }
        parser_expect(p, TOK_RPAREN, "after grouped expression");
        return first;
    }

    /* Array literal: [a, b, c] */
    if (parser_match(p, TOK_LBRACKET)) {
        AstNode *node = ast_new(p->arena, AST_ARRAY, loc);
        AstNode *elems = NULL;
        while (!parser_check(p, TOK_RBRACKET) && !parser_check(p, TOK_EOF)) {
            AstNode *elem = parse_expr(p, PREC_NONE);
            elems = ast_append(elems, elem);
            if (!parser_match(p, TOK_COMMA)) break;
        }
        parser_expect(p, TOK_RBRACKET, "after array literal");
        node->params = elems;
        return node;
    }

    /* Map literal: #{ key: value, ... } */
    if (parser_match(p, TOK_HASH)) {
        if (parser_match(p, TOK_LBRACE)) {
            AstNode *node = ast_new(p->arena, AST_MAP, loc);
            AstNode *entries = NULL;
            while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
                SourceLoc eloc = p->current.loc;
                AstNode *entry = ast_new(p->arena, AST_MAP_ENTRY, eloc);
                entry->left = parse_expr(p, PREC_NONE);
                parser_expect(p, TOK_COLON, "in map entry");
                entry->right = parse_expr(p, PREC_NONE);
                entries = ast_append(entries, entry);
                if (!parser_match(p, TOK_COMMA)) break;
            }
            parser_expect(p, TOK_RBRACE, "after map literal");
            node->params = entries;
            return node;
        }
    }

    /* If expression */
    if (parser_match(p, TOK_IF)) {
        AstNode *node = ast_new(p->arena, AST_IF, loc);
        node->left = parse_expr(p, PREC_NONE);
        node->right = parse_block(p);
        if (parser_match(p, TOK_ELSE)) {
            if (parser_check(p, TOK_IF)) {
                /* else if — recursive */
                AstNode *else_if = parse_prefix(p);  /* will hit the if branch */
                node->params = else_if;
            } else {
                node->params = parse_block(p);
            }
        }
        return node;
    }

    /* Match expression */
    if (parser_match(p, TOK_MATCH)) {
        AstNode *node = ast_new(p->arena, AST_MATCH, loc);
        node->left = parse_expr(p, PREC_NONE);
        parser_expect(p, TOK_LBRACE, "after match subject");
        skip_semis(p);
        AstNode *arms = NULL;
        while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
            SourceLoc arm_loc = p->current.loc;
            AstNode *arm = ast_new(p->arena, AST_MATCH_ARM, arm_loc);
            arm->left = parse_pattern(p);
            /* Guard: if condition */
            if (parser_match(p, TOK_IF)) {
                arm->params = parse_expr(p, PREC_NONE);
            }
            parser_expect(p, TOK_ARROW, "in match arm");
            if (parser_check(p, TOK_LBRACE)) {
                arm->right = parse_block(p);
            } else {
                arm->right = parse_expr(p, PREC_NONE);
            }
            arms = ast_append(arms, arm);
            skip_semis(p);
        }
        parser_expect(p, TOK_RBRACE, "after match body");
        node->params = arms;
        return node;
    }

    /* Closure: |params| expr or |params| { block } */
    if (parser_match(p, TOK_PIPE)) {
        AstNode *node = ast_new(p->arena, AST_CLOSURE, loc);
        AstNode *params = NULL;
        if (!parser_check(p, TOK_PIPE)) {
            while (!parser_check(p, TOK_PIPE) && !parser_check(p, TOK_EOF)) {
                SourceLoc ploc = p->current.loc;
                AstNode *param = ast_new(p->arena, AST_PARAM, ploc);
                param->name = parser_expect(p, TOK_IDENT, "in closure parameter").value.str_val;
                if (parser_match(p, TOK_COLON)) {
                    param->type_expr = parse_type_expr(p);
                }
                params = ast_append(params, param);
                if (!parser_match(p, TOK_COMMA)) break;
            }
        }
        parser_expect(p, TOK_PIPE, "after closure parameters");
        node->params = params;
        if (parser_check(p, TOK_LBRACE)) {
            node->left = parse_block(p);
        } else {
            node->left = parse_expr(p, PREC_NONE);
        }
        return node;
    }

    /* Closure with || (empty params) */
    if (parser_match(p, TOK_PIPE_PIPE)) {
        AstNode *node = ast_new(p->arena, AST_CLOSURE, loc);
        if (parser_check(p, TOK_LBRACE)) {
            node->left = parse_block(p);
        } else {
            node->left = parse_expr(p, PREC_NONE);
        }
        return node;
    }

    /* Spawn: spawn { ... } */
    if (parser_match(p, TOK_SPAWN)) {
        AstNode *node = ast_new(p->arena, AST_SPAWN, loc);
        node->left = parse_block(p);
        return node;
    }

    /* Keep where: keep where <condition>
     * Used in pipe chains: items |> keep where score > 0.5
     * The condition is an expression that refers to the pipe input element.
     * We parse with PREC_PIPE_OP so we capture the full condition but stop
     * before another pipe operator. */
    if (parser_match(p, TOK_KEEP)) {
        AstNode *node = ast_new(p->arena, AST_KEEP_WHERE, loc);
        parser_expect(p, TOK_WHERE, "after 'keep' (expected 'keep where')");
        node->left = parse_expr(p, PREC_NONE);
        return node;
    }

    /* Each: each <field_name>
     * Used in pipe chains: users |> each name
     * Extracts a single field from each element. */
    if (parser_match(p, TOK_EACH)) {
        AstNode *node = ast_new(p->arena, AST_EACH, loc);
        node->name = consume_ident_name(p);
        return node;
    }

    /* Task group: task_group { spawn expr1; spawn expr2; ... }
     * Structured concurrency: all spawned tasks must complete before scope exits.
     * Returns an array of results. */
    if (parser_check(p, TOK_IDENT) && p->current.value.str_val &&
        strcmp(p->current.value.str_val, "task_group") == 0) {
        parser_advance(p); /* consume 'task_group' */
        AstNode *node = ast_new(p->arena, AST_TASK_GROUP, loc);
        /* Parse block body — contains spawn statements */
        node->left = parse_block(p);
        return node;
    }

    /* Try-otherwise: try <expr> otherwise <fallback>
     * Evaluates expr; if it errors, evaluates fallback instead.
     * 'try' is detected as an identifier (not a keyword). */
    if (parser_check(p, TOK_IDENT) && p->current.value.str_val &&
        strcmp(p->current.value.str_val, "try") == 0) {
        parser_advance(p); /* consume 'try' */
        AstNode *node = ast_new(p->arena, AST_TRY_OTHERWISE, loc);
        node->left = parse_expr(p, PREC_PIPE_OP);
        if (parser_match(p, TOK_OTHERWISE)) {
            node->right = parse_expr(p, PREC_PIPE_OP);
        }
        return node;
    }

    /* Ask: ask(question) or ask(question, context) */
    if (parser_match(p, TOK_ASK)) {
        AstNode *node = ast_new(p->arena, AST_ASK, loc);
        parser_expect(p, TOK_LPAREN, "after 'ask'");
        node->left = parse_expr(p, PREC_NONE);
        if (parser_match(p, TOK_COMMA)) {
            node->right = parse_expr(p, PREC_NONE);
        }
        parser_expect(p, TOK_RPAREN, "after ask arguments");
        return node;
    }

    /* Tell: tell target message */
    if (parser_match(p, TOK_TELL)) {
        AstNode *node = ast_new(p->arena, AST_TELL, loc);
        node->left = parse_expr(p, PREC_UNARY);
        node->right = parse_expr(p, PREC_NONE);
        return node;
    }

    /* Await: await expr */
    if (parser_match(p, TOK_AWAIT)) {
        AstNode *node = ast_new(p->arena, AST_AWAIT, loc);
        node->left = parse_expr(p, PREC_NONE);
        return node;
    }

    /* Channel: chan<T>(buffer) or chan<T>() */
    if (parser_match(p, TOK_CHAN)) {
        AstNode *node = ast_new(p->arena, AST_CHANNEL, loc);
        /* Optional type parameter: chan<T> */
        if (parser_match(p, TOK_LT)) {
            node->type_expr = parse_type_expr(p);
            parser_expect(p, TOK_GT, "after channel type parameter");
        }
        /* Optional buffer size: chan<T>(N) */
        if (parser_match(p, TOK_LPAREN)) {
            if (!parser_check(p, TOK_RPAREN)) {
                node->left = parse_expr(p, PREC_NONE);
            }
            parser_expect(p, TOK_RPAREN, "after channel buffer size");
        }
        return node;
    }

    /* Select: select { var from ch -> { body }, ... } */
    if (parser_match(p, TOK_SELECT)) {
        AstNode *node = ast_new(p->arena, AST_SELECT, loc);
        parser_expect(p, TOK_LBRACE, "after 'select'");
        skip_semis(p);
        AstNode *arms = NULL;
        while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
            SourceLoc arm_loc = p->current.loc;
            AstNode *arm = ast_new(p->arena, AST_SELECT_ARM, arm_loc);
            /* var from channel_expr */
            arm->name = parser_expect(p, TOK_IDENT, "in select arm").value.str_val;
            /* expect "from" keyword — it's just an ident in the lexer */
            if (parser_check(p, TOK_IDENT) && p->current.value.str_val &&
                strcmp(p->current.value.str_val, "from") == 0) {
                parser_advance(p);
            } else {
                parser_expect(p, TOK_IDENT, "expected 'from' in select arm");
            }
            arm->left = parse_expr(p, PREC_NONE);
            parser_expect(p, TOK_ARROW, "in select arm");
            arm->right = parse_block(p);
            arms = ast_append(arms, arm);
            skip_semis(p);
        }
        parser_expect(p, TOK_RBRACE, "after select body");
        node->params = arms;
        return node;
    }

    /* Unsafe block: unsafe { ... } */
    if (parser_match(p, TOK_UNSAFE)) {
        AstNode *node = ast_new(p->arena, AST_UNSAFE_BLOCK, loc);
        node->left = parse_block(p);
        return node;
    }

    /* Comptime block: comptime { ... } */
    if (parser_match(p, TOK_COMPTIME)) {
        AstNode *node = ast_new(p->arena, AST_COMPTIME, loc);
        node->left = parse_block(p);
        return node;
    }

    /* Block expression: { ... } */
    if (parser_check(p, TOK_LBRACE)) {
        return parse_block(p);
    }

    /* Identifier — possibly struct literal Name { ... } */
    if (parser_check(p, TOK_IDENT)) {
        AstNode *node = ast_new(p->arena, AST_IDENT, loc);
        node->name = parser_advance(p).value.str_val;

        /* Check for struct literal: Name { field: value, ... }
         * We look for IDENT { IDENT : which distinguishes from a block */
        if (parser_check(p, TOK_LBRACE)) {
            /* Heuristic: if the next tokens are "ident :" or "}", it's a struct literal.
             * Otherwise it's a block expression (which starts with statements). */
            /* For now, we peek two tokens ahead (IDENT COLON or just RBRACE) */
            /* Since we can't easily peek ahead in a recursive descent parser,
             * we use a simple approach: uppercase first letter = struct literal */
            if (node->name[0] >= 'A' && node->name[0] <= 'Z') {
                parser_advance(p); /* consume { */
                skip_semis(p);
                AstNode *map = ast_new(p->arena, AST_MAP, loc);
                map->name = node->name; /* store struct name */
                AstNode *entries = NULL;
                while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
                    SourceLoc eloc = p->current.loc;
                    AstNode *entry = ast_new(p->arena, AST_MAP_ENTRY, eloc);
                    AstNode *key = ast_new(p->arena, AST_IDENT, eloc);
                    key->name = parser_expect(p, TOK_IDENT, "in struct literal").value.str_val;
                    entry->left = key;
                    if (parser_match(p, TOK_COLON)) {
                        entry->right = parse_expr(p, PREC_NONE);
                    } else {
                        /* Shorthand: { x } means { x: x } */
                        AstNode *val = ast_new(p->arena, AST_IDENT, eloc);
                        val->name = key->name;
                        entry->right = val;
                    }
                    entries = ast_append(entries, entry);
                    if (!parser_match(p, TOK_COMMA)) {
                        skip_semis(p);
                        if (!parser_check(p, TOK_RBRACE)) {
                            /* Allow newline-separated entries */
                        }
                    } else {
                        skip_semis(p);
                    }
                }
                parser_expect(p, TOK_RBRACE, "after struct literal");
                map->params = entries;
                return map;
            }
        }

        return node;
    }

    /* Agent-system keywords used as identifiers in expressions (e.g. `agent.run(...)`) */
    if (is_ident_or_keyword(p->current.kind) && p->current.kind != TOK_IDENT) {
        AstNode *node = ast_new(p->arena, AST_IDENT, loc);
        node->name = consume_ident_name(p);
        return node;
    }

    report_error(p->reporter, p->current.loc, "expected expression", NULL);
    p->had_error = true;
    parser_advance(p);
    return ast_new(p->arena, AST_NONE_LIT, loc);
}

/* ============================================================
 * Statement Parser
 * ============================================================ */

static AstNode *parse_block(Parser *p) {
    SourceLoc loc = p->current.loc;
    parser_expect(p, TOK_LBRACE, "to begin block");
    skip_semis(p);

    AstNode *block = ast_new(p->arena, AST_BLOCK, loc);
    AstNode *stmts = NULL;

    while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
        AstNode *stmt = parse_statement(p);
        if (stmt) {
            stmts = ast_append(stmts, stmt);
        }
        skip_semis(p);
        if (p->panic_mode) parser_synchronize(p);
    }

    parser_expect(p, TOK_RBRACE, "to end block");
    block->params = stmts;
    return block;
}

static AstNode *parse_statement(Parser *p) {
    SourceLoc loc = p->current.loc;

    /* Let statement */
    if (parser_match(p, TOK_LET)) {
        AstNode *node = ast_new(p->arena, AST_LET, loc);
        node->is_mut = parser_match(p, TOK_MUT);
        /* Allow keywords (agent, guard, budget, etc.) as variable names */
        if (is_ident_or_keyword(p->current.kind)) {
            node->name = consume_ident_name(p);
        } else {
            node->name = parser_expect(p, TOK_IDENT, "in let binding").value.str_val;
        }
        if (parser_match(p, TOK_COLON)) {
            node->type_expr = parse_type_expr(p);
        }
        if (parser_match(p, TOK_EQ)) {
            node->right = parse_expr(p, PREC_NONE);
        }
        return node;
    }

    /* Return */
    if (parser_match(p, TOK_RETURN)) {
        AstNode *node = ast_new(p->arena, AST_RETURN, loc);
        if (!parser_check(p, TOK_SEMICOLON) && !parser_check(p, TOK_RBRACE) &&
            !parser_check(p, TOK_EOF)) {
            node->left = parse_expr(p, PREC_NONE);
        }
        return node;
    }

    /* Break */
    if (parser_match(p, TOK_BREAK)) {
        AstNode *node = ast_new(p->arena, AST_BREAK, loc);
        if (!parser_check(p, TOK_SEMICOLON) && !parser_check(p, TOK_RBRACE)) {
            node->left = parse_expr(p, PREC_NONE);
        }
        return node;
    }

    /* Continue */
    if (parser_match(p, TOK_CONTINUE)) {
        return ast_new(p->arena, AST_CONTINUE, loc);
    }

    /* Defer */
    if (parser_match(p, TOK_DEFER)) {
        AstNode *node = ast_new(p->arena, AST_DEFER, loc);
        node->left = parse_expr(p, PREC_NONE);
        return node;
    }

    /* For loop */
    if (parser_match(p, TOK_FOR)) {
        AstNode *node = ast_new(p->arena, AST_FOR, loc);
        node->left = parse_pattern(p);
        /* Optional second pattern (index): for i, item in ... */
        if (parser_match(p, TOK_COMMA)) {
            AstNode *second = parse_pattern(p);
            /* Store both patterns — left is first, right->left is second */
            AstNode *wrap = ast_new(p->arena, AST_TUPLE, loc);
            wrap->params = node->left;
            node->left->next = second;
            node->left = wrap;
        }
        parser_expect(p, TOK_IN, "in for loop");
        node->params = parse_expr(p, PREC_NONE);
        node->right = parse_block(p);
        return node;
    }

    /* While loop */
    if (parser_match(p, TOK_WHILE)) {
        AstNode *node = ast_new(p->arena, AST_WHILE, loc);
        node->left = parse_expr(p, PREC_NONE);
        node->right = parse_block(p);
        return node;
    }

    /* Loop (infinite) */
    if (parser_match(p, TOK_LOOP)) {
        AstNode *node = ast_new(p->arena, AST_LOOP, loc);
        node->left = parse_block(p);
        return node;
    }

    /* Ensure statement: ensure condition, "message" */
    if (parser_match(p, TOK_ENSURE)) {
        AstNode *node = ast_new(p->arena, AST_ENSURE, loc);
        node->left = parse_expr(p, PREC_NONE);  /* condition */
        if (parser_match(p, TOK_COMMA)) {
            node->right = parse_expr(p, PREC_NONE);  /* message */
        }
        return node;
    }

    /* Repeat statement: repeat N times { body } */
    if (parser_match(p, TOK_REPEAT)) {
        AstNode *node = ast_new(p->arena, AST_REPEAT, loc);
        node->left = parse_expr(p, PREC_NONE);  /* count */
        if (parser_check(p, TOK_TIMES)) {
            parser_advance(p);  /* consume 'times' */
        }
        node->right = parse_block(p);  /* body */
        return node;
    }

    /* Wait until statement: wait until condition */
    if (parser_match(p, TOK_WAIT)) {
        AstNode *node = ast_new(p->arena, AST_WAIT_UNTIL, loc);
        if (parser_check(p, TOK_UNTIL)) {
            parser_advance(p);  /* consume 'until' */
        }
        node->left = parse_expr(p, PREC_NONE);  /* condition */
        return node;
    }

    /* Expression statement — may also be assignment */
    {
        AstNode *expr = parse_expr(p, PREC_NONE);

        /* Check for assignment operators */
        TokenKind assign_op = p->current.kind;
        if (assign_op == TOK_EQ || assign_op == TOK_PLUS_EQ || assign_op == TOK_MINUS_EQ ||
            assign_op == TOK_STAR_EQ || assign_op == TOK_SLASH_EQ || assign_op == TOK_PERCENT_EQ ||
            assign_op == TOK_AMP_EQ || assign_op == TOK_PIPE_EQ || assign_op == TOK_CARET_EQ ||
            assign_op == TOK_SHL_EQ || assign_op == TOK_SHR_EQ) {
            parser_advance(p);
            AstNode *node = ast_new(p->arena, AST_ASSIGN, loc);
            node->val.op = assign_op;
            node->left = expr;
            node->right = parse_expr(p, PREC_NONE);
            return node;
        }

        /* Plain expression statement */
        AstNode *node = ast_new(p->arena, AST_EXPR_STMT, loc);
        node->left = expr;
        return node;
    }
}

/* ============================================================
 * Declaration Parser
 * ============================================================ */

static AstNode *parse_fn(Parser *p, bool is_pub) {
    SourceLoc loc = p->previous.loc;
    AstNode *node = ast_new(p->arena, AST_FN, loc);
    node->is_pub = is_pub;

    node->name = parser_expect(p, TOK_IDENT, "after 'fn'").value.str_val;
    node->generics = parse_generic_params(p);

    /* Parameters */
    parser_expect(p, TOK_LPAREN, "after function name");
    AstNode *params = NULL;
    while (!parser_check(p, TOK_RPAREN) && !parser_check(p, TOK_EOF)) {
        SourceLoc ploc = p->current.loc;
        AstNode *param = ast_new(p->arena, AST_PARAM, ploc);

        /* &self or &mut self */
        if (parser_check(p, TOK_AMP)) {
            parser_advance(p);
            param->is_mut = parser_match(p, TOK_MUT);
            if (parser_check(p, TOK_IDENT) && p->current.value.str_val &&
                strcmp(p->current.value.str_val, "self") == 0) {
                param->name = parser_advance(p).value.str_val;
                params = ast_append(params, param);
                if (!parser_match(p, TOK_COMMA)) break;
                continue;
            }
            /* Not self — it was a ref type in parameter */
            if (is_ident_or_keyword(p->current.kind))
                param->name = consume_ident_name(p);
            else
                param->name = parser_expect(p, TOK_IDENT, "in parameter").value.str_val;
            /* We already consumed & so the type must handle it */
        } else {
            if (is_ident_or_keyword(p->current.kind))
                param->name = consume_ident_name(p);
            else
                param->name = parser_expect(p, TOK_IDENT, "in parameter").value.str_val;
        }
        parser_expect(p, TOK_COLON, "after parameter name");
        param->type_expr = parse_type_expr(p);
        params = ast_append(params, param);
        if (!parser_match(p, TOK_COMMA)) break;
    }
    parser_expect(p, TOK_RPAREN, "after parameters");
    node->params = params;

    /* Return type */
    if (parser_match(p, TOK_ARROW)) {
        node->type_expr = parse_type_expr(p);
    }

    /* Body (or just declaration for traits) */
    if (parser_check(p, TOK_LBRACE)) {
        node->left = parse_block(p);
    }

    return node;
}

static AstNode *parse_struct(Parser *p, bool is_pub) {
    SourceLoc loc = p->previous.loc;
    AstNode *node = ast_new(p->arena, AST_STRUCT, loc);
    node->is_pub = is_pub;
    node->name = parser_expect(p, TOK_IDENT, "after 'struct'").value.str_val;
    node->generics = parse_generic_params(p);

    parser_expect(p, TOK_LBRACE, "after struct name");
    skip_semis(p);
    AstNode *fields = NULL;
    while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
        SourceLoc floc = p->current.loc;
        AstNode *attrs = parse_attributes(p);
        AstNode *field = ast_new(p->arena, AST_FIELD, floc);
        field->attributes = attrs;

        if (parser_match(p, TOK_PUB)) field->is_pub = true;
        if (parser_match(p, TOK_PRIV)) field->is_pub = false;

        field->name = parser_expect(p, TOK_IDENT, "in struct field").value.str_val;
        parser_expect(p, TOK_COLON, "after field name");
        field->type_expr = parse_type_expr(p);

        /* Default value */
        if (parser_match(p, TOK_EQ)) {
            field->right = parse_expr(p, PREC_NONE);
        }

        fields = ast_append(fields, field);
        skip_semis(p);
    }
    parser_expect(p, TOK_RBRACE, "after struct body");
    node->params = fields;
    return node;
}

static AstNode *parse_enum(Parser *p, bool is_pub) {
    SourceLoc loc = p->previous.loc;
    AstNode *node = ast_new(p->arena, AST_ENUM, loc);
    node->is_pub = is_pub;
    node->name = parser_expect(p, TOK_IDENT, "after 'enum'").value.str_val;
    node->generics = parse_generic_params(p);

    parser_expect(p, TOK_LBRACE, "after enum name");
    skip_semis(p);
    AstNode *variants = NULL;
    while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
        SourceLoc vloc = p->current.loc;
        AstNode *variant = ast_new(p->arena, AST_VARIANT, vloc);
        variant->name = parser_expect(p, TOK_IDENT, "in enum variant").value.str_val;

        if (parser_match(p, TOK_LPAREN)) {
            AstNode *vfields = NULL;
            while (!parser_check(p, TOK_RPAREN) && !parser_check(p, TOK_EOF)) {
                SourceLoc ffloc = p->current.loc;
                AstNode *f = ast_new(p->arena, AST_FIELD, ffloc);
                /* Check if it's "name: Type" or just "Type" */
                if (parser_check(p, TOK_IDENT)) {
                    const char *maybe_name = p->current.value.str_val;
                    parser_advance(p);
                    if (parser_match(p, TOK_COLON)) {
                        f->name = maybe_name;
                        f->type_expr = parse_type_expr(p);
                    } else {
                        /* It was just a type name */
                        AstNode *ty = ast_new(p->arena, AST_TYPE_NAMED, ffloc);
                        ty->name = maybe_name;
                        f->type_expr = ty;
                    }
                } else {
                    f->type_expr = parse_type_expr(p);
                }
                vfields = ast_append(vfields, f);
                if (!parser_match(p, TOK_COMMA)) break;
            }
            parser_expect(p, TOK_RPAREN, "after variant fields");
            variant->params = vfields;
        }

        variants = ast_append(variants, variant);
        skip_semis(p);
    }
    parser_expect(p, TOK_RBRACE, "after enum body");
    node->params = variants;
    return node;
}

static AstNode *parse_trait(Parser *p, bool is_pub) {
    SourceLoc loc = p->previous.loc;
    AstNode *node = ast_new(p->arena, AST_TRAIT, loc);
    node->is_pub = is_pub;
    node->name = parser_expect(p, TOK_IDENT, "after 'trait'").value.str_val;
    node->generics = parse_generic_params(p);

    /* Super-traits: trait Foo: Bar + Baz */
    if (parser_match(p, TOK_COLON)) {
        AstNode *supers = NULL;
        AstNode *s = parse_type_primary(p);
        supers = ast_append(supers, s);
        while (parser_match(p, TOK_PLUS)) {
            s = parse_type_primary(p);
            supers = ast_append(supers, s);
        }
        node->type_expr = supers;
    }

    parser_expect(p, TOK_LBRACE, "after trait name");
    skip_semis(p);
    AstNode *methods = NULL;
    while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
        AstNode *attrs = parse_attributes(p);
        bool method_pub = parser_match(p, TOK_PUB);
        parser_expect(p, TOK_FN, "in trait body");
        AstNode *method = parse_fn(p, method_pub);
        method->attributes = attrs;
        methods = ast_append(methods, method);
        skip_semis(p);
    }
    parser_expect(p, TOK_RBRACE, "after trait body");
    node->params = methods;
    return node;
}

static AstNode *parse_interface(Parser *p, bool is_pub) {
    SourceLoc loc = p->previous.loc;
    AstNode *node = ast_new(p->arena, AST_INTERFACE, loc);
    node->is_pub = is_pub;
    node->name = parser_expect(p, TOK_IDENT, "after 'interface'").value.str_val;
    node->generics = parse_generic_params(p);

    /* Interface = A + B (type combination) */
    if (parser_match(p, TOK_EQ)) {
        AstNode *types = NULL;
        AstNode *t = parse_type_primary(p);
        types = ast_append(types, t);
        while (parser_match(p, TOK_PLUS)) {
            t = parse_type_primary(p);
            types = ast_append(types, t);
        }
        node->type_expr = types;
        return node;
    }

    parser_expect(p, TOK_LBRACE, "after interface name");
    skip_semis(p);
    AstNode *methods = NULL;
    while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
        parser_expect(p, TOK_FN, "in interface body");
        AstNode *method = parse_fn(p, true);
        methods = ast_append(methods, method);
        skip_semis(p);
    }
    parser_expect(p, TOK_RBRACE, "after interface body");
    node->params = methods;
    return node;
}

static AstNode *parse_impl(Parser *p) {
    SourceLoc loc = p->previous.loc;
    AstNode *node = ast_new(p->arena, AST_IMPL, loc);
    node->generics = parse_generic_params(p);

    /* impl Type { ... } or impl Trait for Type { ... } */
    AstNode *first_type = parse_type_primary(p);

    if (parser_match(p, TOK_FOR)) {
        node->right = first_type;  /* trait */
        node->left = parse_type_primary(p);  /* target type */
    } else {
        node->left = first_type;  /* just target type */
    }

    parser_expect(p, TOK_LBRACE, "after impl target");
    skip_semis(p);
    AstNode *methods = NULL;
    while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
        AstNode *attrs = parse_attributes(p);
        bool method_pub = parser_match(p, TOK_PUB);
        parser_expect(p, TOK_FN, "in impl body");
        AstNode *method = parse_fn(p, method_pub);
        method->attributes = attrs;
        methods = ast_append(methods, method);
        skip_semis(p);
    }
    parser_expect(p, TOK_RBRACE, "after impl body");
    node->params = methods;
    return node;
}

static AstNode *parse_type_alias(Parser *p, bool is_pub) {
    SourceLoc loc = p->previous.loc;
    AstNode *node = ast_new(p->arena, AST_TYPE_ALIAS, loc);
    node->is_pub = is_pub;
    node->name = parser_expect(p, TOK_IDENT, "after 'type'").value.str_val;
    node->generics = parse_generic_params(p);
    parser_expect(p, TOK_EQ, "in type alias");
    node->type_expr = parse_type_expr(p);
    return node;
}

static AstNode *parse_const_decl(Parser *p, bool is_pub) {
    SourceLoc loc = p->previous.loc;
    AstNode *node = ast_new(p->arena, AST_CONST, loc);
    node->is_pub = is_pub;
    if (parser_check(p, TOK_IDENT)) {
        node->name = parser_advance(p).value.str_val;
    }
    if (parser_match(p, TOK_COLON)) {
        node->type_expr = parse_type_expr(p);
    }
    parser_expect(p, TOK_EQ, "in const declaration");
    node->right = parse_expr(p, PREC_NONE);
    return node;
}

static AstNode *parse_module_decl(Parser *p) {
    SourceLoc loc = p->previous.loc;
    AstNode *node = ast_new(p->arena, AST_MODULE, loc);
    node->name = parser_expect(p, TOK_IDENT, "after 'mod'").value.str_val;
    /* Qualified: mod a.b.c */
    while (parser_match(p, TOK_DOT)) {
        const char *next = parser_expect(p, TOK_IDENT, "in module path").value.str_val;
        size_t len1 = strlen(node->name);
        size_t len2 = strlen(next);
        char *qn = (char *)arena_alloc(p->arena, len1 + 1 + len2 + 1);
        memcpy(qn, node->name, len1);
        qn[len1] = '.';
        memcpy(qn + len1 + 1, next, len2);
        qn[len1 + 1 + len2] = '\0';
        node->name = qn;
    }
    return node;
}

static AstNode *parse_use_decl(Parser *p) {
    SourceLoc loc = p->previous.loc;
    AstNode *node = ast_new(p->arena, AST_USE, loc);

    /* Build path: use a.b.c or use a.b.{c, d} */
    const char *path = parser_expect(p, TOK_IDENT, "after 'use'").value.str_val;

    /* MCP import: use mcp("server_command") as alias */
    if (path && strcmp(path, "mcp") == 0 && parser_check(p, TOK_LPAREN)) {
        parser_advance(p);
        node->name = "mcp";
        node->val.str_val = parser_expect(p, TOK_STRING_LIT, "MCP server command").value.str_val;
        parser_expect(p, TOK_RPAREN, "after MCP command");
        if (parser_match(p, TOK_AS)) {
            node->right = ast_new(p->arena, AST_IDENT, loc);
            node->right->name = parser_expect(p, TOK_IDENT, "after 'as'").value.str_val;
        }
        return node;
    }

    /* Driver import: use driver("mysql") as db */
    if (path && strcmp(path, "driver") == 0 && parser_check(p, TOK_LPAREN)) {
        parser_advance(p);
        node->name = "driver";
        node->val.str_val = parser_expect(p, TOK_STRING_LIT,
                                          "driver name (e.g. \"mysql\")").value.str_val;
        parser_expect(p, TOK_RPAREN, "after driver name");
        if (parser_match(p, TOK_AS)) {
            node->right = ast_new(p->arena, AST_IDENT, loc);
            node->right->name = parser_expect(p, TOK_IDENT, "after 'as'").value.str_val;
        }
        return node;
    }

    /* A2A import: use a2a("endpoint_url") as alias */
    if (path && strcmp(path, "a2a") == 0 && parser_check(p, TOK_LPAREN)) {
        parser_advance(p);
        node->name = "a2a";
        node->val.str_val = parser_expect(p, TOK_STRING_LIT, "A2A endpoint URL").value.str_val;
        parser_expect(p, TOK_RPAREN, "after A2A URL");
        if (parser_match(p, TOK_AS)) {
            node->right = ast_new(p->arena, AST_IDENT, loc);
            node->right->name = parser_expect(p, TOK_IDENT, "after 'as'").value.str_val;
        }
        return node;
    }

    /* Model import: use model("path.onnx") as classifier */
    if (path && strcmp(path, "model") == 0 && parser_check(p, TOK_LPAREN)) {
        parser_advance(p);
        node->name = "model";
        node->val.str_val = parser_expect(p, TOK_STRING_LIT, "model path").value.str_val;
        parser_expect(p, TOK_RPAREN, "after model path");
        if (parser_match(p, TOK_AS)) {
            node->right = ast_new(p->arena, AST_IDENT, loc);
            node->right->name = parser_expect(p, TOK_IDENT, "after 'as'").value.str_val;
        }
        return node;
    }

    while (parser_match(p, TOK_DOT)) {
        if (parser_match(p, TOK_LBRACE)) {
            /* use a.b.{c, d, e} */
            node->name = path;
            AstNode *imports = NULL;
            while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
                SourceLoc iloc = p->current.loc;
                AstNode *imp = ast_new(p->arena, AST_IDENT, iloc);
                imp->name = parser_expect(p, TOK_IDENT, "in import list").value.str_val;
                imports = ast_append(imports, imp);
                if (!parser_match(p, TOK_COMMA)) break;
            }
            parser_expect(p, TOK_RBRACE, "after import list");
            node->params = imports;
            return node;
        }
        if (parser_check(p, TOK_IDENT)) {
            const char *next = parser_advance(p).value.str_val;
            size_t len1 = strlen(path);
            size_t len2 = strlen(next);
            char *qn = (char *)arena_alloc(p->arena, len1 + 1 + len2 + 1);
            memcpy(qn, path, len1);
            qn[len1] = '.';
            memcpy(qn + len1 + 1, next, len2);
            qn[len1 + 1 + len2] = '\0';
            path = qn;
        }
    }
    node->name = path;

    /* Alias: use foo.bar as baz */
    if (parser_match(p, TOK_AS)) {
        node->val.str_val = parser_expect(p, TOK_IDENT, "after 'as'").value.str_val;
    }

    return node;
}

/* ============================================================
 * Agent System Helpers
 * ============================================================ */

/*
 * Check if the current token can be used as a field/identifier name.
 * Agent system keywords (strategy, requires, budget, etc.) are allowed
 * as field names in declaration bodies.
 */
static bool is_ident_or_keyword(TokenKind kind) {
    switch (kind) {
    case TOK_IDENT:
    /* Agent keywords usable as field names */
    case TOK_AGENT:     case TOK_GUARD:      case TOK_CAPABILITY:
    case TOK_TAINT:     case TOK_BUDGET:     case TOK_TOOL:
    case TOK_SKILL:     case TOK_PROMPT:     case TOK_SUPERVISOR:
    case TOK_MESH:      case TOK_MEMORY:     case TOK_REQUIRES:
    case TOK_STRATEGY:  case TOK_ROUTE:      case TOK_ROUTER:
    case TOK_CHANNEL:   case TOK_SECRET:     case TOK_INVARIANT:
    case TOK_GUARDSET:  case TOK_OTHERWISE:  case TOK_SHOWING:
    case TOK_TIMEOUT:   case TOK_CHOICES:    case TOK_REPEAT:
    case TOK_TIMES:     case TOK_WAIT:       case TOK_UNTIL:
    case TOK_EACH:      case TOK_KEEP:       case TOK_WHERE:
    case TOK_ABOUT:     case TOK_ASK:        case TOK_TELL:
    case TOK_ENSURE:    case TOK_ALLOW:      case TOK_DENY:
    case TOK_PROGRESS:
        return true;
    default:
        return false;
    }
}

/* Consume current token and return its name string (works for ident and keywords) */
static const char *consume_ident_name(Parser *p) {
    if (is_ident_or_keyword(p->current.kind)) {
        const char *name;
        if (p->current.kind == TOK_IDENT) {
            name = p->current.value.str_val;
        } else {
            name = arena_strdup(p->arena, token_kind_name(p->current.kind));
        }
        parser_advance(p);
        return name;
    }
    /* Fall back to expect ident — will produce error */
    return parser_expect(p, TOK_IDENT, "in field").value.str_val;
}

/* Parse a qualified identifier: ident or ident.ident.ident */
static const char *parse_qualified_ident(Parser *p) {
    const char *name = consume_ident_name(p);
    while (parser_match(p, TOK_DOT)) {
        const char *next = consume_ident_name(p);
        size_t len1 = name ? strlen(name) : 0;
        size_t len2 = next ? strlen(next) : 0;
        char *qn = (char *)arena_alloc(p->arena, len1 + 1 + len2 + 1);
        if (name) memcpy(qn, name, len1);
        qn[len1] = '.';
        if (next) memcpy(qn + len1 + 1, next, len2);
        qn[len1 + 1 + len2] = '\0';
        name = qn;
    }
    return name;
}

/* Parse a field assignment: ident: expression */
static AstNode *parse_decl_field(Parser *p) {
    SourceLoc loc = p->current.loc;
    AstNode *field = ast_new(p->arena, AST_FIELD, loc);
    field->name = consume_ident_name(p);
    parser_expect(p, TOK_COLON, "after field name");

    /* Array value: [item, item, ...] — parse as identifiers (qualified) */
    if (parser_check(p, TOK_LBRACKET)) {
        parser_advance(p);
        AstNode *arr = ast_new(p->arena, AST_ARRAY, loc);
        AstNode *elems = NULL;
        while (!parser_check(p, TOK_RBRACKET) && !parser_check(p, TOK_EOF)) {
            SourceLoc eloc = p->current.loc;
            AstNode *elem = ast_new(p->arena, AST_IDENT, eloc);
            elem->name = parse_qualified_ident(p);
            elems = ast_append(elems, elem);
            if (!parser_match(p, TOK_COMMA)) break;
        }
        parser_expect(p, TOK_RBRACKET, "after array");
        arr->params = elems;
        field->right = arr;
    }
    /* Block value: { key: value, ... } */
    else if (parser_check(p, TOK_LBRACE)) {
        parser_advance(p);
        skip_semis(p);
        AstNode *block = ast_new(p->arena, AST_BLOCK, loc);
        AstNode *items = NULL;
        while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
            AstNode *item = parse_decl_field(p);
            items = ast_append(items, item);
            /* Accept both commas and semicolons/newlines as separators */
            if (!parser_match(p, TOK_COMMA)) skip_semis(p);
        }
        parser_expect(p, TOK_RBRACE, "after block field");
        block->params = items;
        field->right = block;
    }
    /* Otherwise: expression */
    else {
        field->right = parse_expr(p, PREC_NONE);
    }
    return field;
}

/* ============================================================
 * Agent System Parsers
 * ============================================================ */

/* parse_agent: agent Name { ... } */
static AstNode *parse_agent(Parser *p, bool is_pub) {
    SourceLoc loc = p->previous.loc;
    AstNode *node = ast_new(p->arena, AST_AGENT, loc);
    node->is_pub = is_pub;
    node->name = parser_expect(p, TOK_IDENT, "after 'agent'").value.str_val;

    parser_expect(p, TOK_LBRACE, "after agent name");
    skip_semis(p);

    AstNode *members = NULL;
    AstNode *fns = NULL;

    while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
        /* Function declaration inside agent */
        if (parser_check(p, TOK_FN)) {
            parser_advance(p);
            AstNode *fn = parse_fn(p, false);
            fns = ast_append(fns, fn);
        }
        /* pub fn */
        else if (parser_check(p, TOK_PUB)) {
            parser_advance(p);
            if (parser_match(p, TOK_FN)) {
                AstNode *fn = parse_fn(p, true);
                fns = ast_append(fns, fn);
            } else {
                AstNode *field = parse_decl_field(p);
                field->is_pub = true;
                members = ast_append(members, field);
            }
        }
        /* use capability1, capability2, ... (access control policy binding) */
        else if (parser_check(p, TOK_USE)) {
            SourceLoc uloc = p->current.loc;
            parser_advance(p);
            AstNode *field = ast_new(p->arena, AST_FIELD, uloc);
            field->name = "use";
            AstNode *arr = ast_new(p->arena, AST_ARRAY, p->current.loc);
            AstNode *elems = NULL;
            do {
                AstNode *elem = ast_new(p->arena, AST_IDENT, p->current.loc);
                elem->name = consume_ident_name(p);
                elems = ast_append(elems, elem);
            } while (parser_match(p, TOK_COMMA));
            arr->params = elems;
            field->right = arr;
            members = ast_append(members, field);
        }
        /* Field: ident: value (keywords like budget, model allowed as field names) */
        else if (is_ident_or_keyword(p->current.kind)) {
            AstNode *field = parse_decl_field(p);
            members = ast_append(members, field);
        }
        else {
            report_error(p->reporter, p->current.loc,
                        "expected field or function in agent body", NULL);
            p->had_error = true;
            parser_advance(p);
        }
        skip_semis(p);
    }
    parser_expect(p, TOK_RBRACE, "after agent body");

    node->params = members;
    node->left = fns;
    return node;
}

/* ── Access Control Rule Parsers ────────────────────────────── */

/* parse_cap_allow_rule: allow endpoint "host:port" { ... } | allow binary "/path" */
static AstNode *parse_cap_allow_rule(Parser *p) {
    SourceLoc loc = p->previous.loc;
    const char *kind = consume_ident_name(p);

    if (strcmp(kind, "endpoint") == 0) {
        AstNode *node = ast_new(p->arena, AST_CAP_ENDPOINT_RULE, loc);
        node->is_mut = true; /* allow */
        node->name = parser_expect(p, TOK_STRING_LIT,
                                   "endpoint host:port string").value.str_val;
        /* Optional block { method: [...], path: "..." } */
        if (parser_check(p, TOK_LBRACE)) {
            parser_advance(p);
            skip_semis(p);
            AstNode *fields = NULL;
            while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
                AstNode *field = parse_decl_field(p);
                fields = ast_append(fields, field);
                skip_semis(p);
            }
            parser_expect(p, TOK_RBRACE, "after endpoint rule block");
            node->params = fields;
        }
        return node;
    }
    else if (strcmp(kind, "binary") == 0) {
        AstNode *node = ast_new(p->arena, AST_CAP_BINARY_RULE, loc);
        node->is_mut = true; /* allow */
        node->name = parser_expect(p, TOK_STRING_LIT,
                                   "binary path string").value.str_val;
        return node;
    }
    else if (strcmp(kind, "path") == 0) {
        AstNode *node = ast_new(p->arena, AST_CAP_PATH_RULE, loc);
        node->is_mut = true; /* allow */
        node->name = parser_expect(p, TOK_STRING_LIT,
                                   "path pattern string").value.str_val;
        /* Optional block { mode: [read, write] } */
        if (parser_check(p, TOK_LBRACE)) {
            parser_advance(p);
            skip_semis(p);
            AstNode *fields = NULL;
            while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
                AstNode *field = parse_decl_field(p);
                fields = ast_append(fields, field);
                skip_semis(p);
            }
            parser_expect(p, TOK_RBRACE, "after path rule block");
            node->params = fields;
        }
        return node;
    }
    else {
        report_error(p->reporter, loc,
                     "expected 'endpoint', 'binary', or 'path' after 'allow'",
                     NULL);
        p->had_error = true;
        return ast_new(p->arena, AST_NONE_LIT, loc);
    }
}

/* parse_cap_deny_rule: deny endpoint "..." | deny binary "..." | deny private_ranges */
static AstNode *parse_cap_deny_rule(Parser *p) {
    SourceLoc loc = p->previous.loc;

    if (!is_ident_or_keyword(p->current.kind)) {
        report_error(p->reporter, loc,
                     "expected 'endpoint', 'binary', 'path', or "
                     "'private_ranges' after 'deny'",
                     NULL);
        p->had_error = true;
        return ast_new(p->arena, AST_NONE_LIT, loc);
    }

    const char *kind = consume_ident_name(p);

    if (strcmp(kind, "private_ranges") == 0) {
        AstNode *node = ast_new(p->arena, AST_CAP_DENY_RANGE, loc);
        node->name = "private_ranges";
        return node;
    }
    else if (strcmp(kind, "endpoint") == 0) {
        AstNode *node = ast_new(p->arena, AST_CAP_ENDPOINT_RULE, loc);
        node->is_mut = false; /* deny */
        node->name = parser_expect(p, TOK_STRING_LIT,
                                   "endpoint host:port string").value.str_val;
        return node;
    }
    else if (strcmp(kind, "binary") == 0) {
        AstNode *node = ast_new(p->arena, AST_CAP_BINARY_RULE, loc);
        node->is_mut = false; /* deny */
        node->name = parser_expect(p, TOK_STRING_LIT,
                                   "binary path string").value.str_val;
        return node;
    }
    else if (strcmp(kind, "path") == 0) {
        AstNode *node = ast_new(p->arena, AST_CAP_PATH_RULE, loc);
        node->is_mut = false; /* deny */
        node->name = parser_expect(p, TOK_STRING_LIT,
                                   "path pattern string").value.str_val;
        return node;
    }
    else {
        report_error(p->reporter, loc,
                     "expected 'endpoint', 'binary', 'path', or "
                     "'private_ranges' after 'deny'",
                     NULL);
        p->had_error = true;
        return ast_new(p->arena, AST_NONE_LIT, loc);
    }
}

/* parse_cap_default: default: allow | default: deny */
static AstNode *parse_cap_default(Parser *p) {
    SourceLoc loc = p->current.loc;
    parser_advance(p); /* consume "default" identifier */
    parser_expect(p, TOK_COLON, "after 'default'");

    AstNode *node = ast_new(p->arena, AST_CAP_DEFAULT, loc);
    const char *policy = consume_ident_name(p);
    if (strcmp(policy, "allow") == 0) {
        node->is_mut = true;
    } else if (strcmp(policy, "deny") == 0) {
        node->is_mut = false;
    } else {
        report_error(p->reporter, loc,
                     "expected 'allow' or 'deny' after 'default:'", NULL);
        p->had_error = true;
    }
    return node;
}

/* parse_capability: capability name { ... }
 * Supports both abstract items (search, fetch requires search)
 * and concrete access rules (allow endpoint, deny binary, etc.) */
static AstNode *parse_capability(Parser *p, bool is_pub) {
    SourceLoc loc = p->previous.loc;
    AstNode *node = ast_new(p->arena, AST_CAPABILITY, loc);
    node->is_pub = is_pub;
    node->name = parser_expect(p, TOK_IDENT, "after 'capability'").value.str_val;

    parser_expect(p, TOK_LBRACE, "after capability name");
    skip_semis(p);

    AstNode *items = NULL;
    while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
        /* allow endpoint/binary rule */
        if (parser_match(p, TOK_ALLOW)) {
            items = ast_append(items, parse_cap_allow_rule(p));
        }
        /* deny endpoint/binary/private_ranges rule */
        else if (parser_match(p, TOK_DENY)) {
            items = ast_append(items, parse_cap_deny_rule(p));
        }
        /* default: allow | default: deny */
        else if (parser_check(p, TOK_IDENT) && p->current.value.str_val &&
                 strcmp(p->current.value.str_val, "default") == 0) {
            items = ast_append(items, parse_cap_default(p));
        }
        /* Abstract capability item (backward compatible) */
        else if (is_ident_or_keyword(p->current.kind)) {
            SourceLoc iloc = p->current.loc;
            AstNode *item = ast_new(p->arena, AST_CAPABILITY_ITEM, iloc);
            item->name = consume_ident_name(p);

            /* Optional: requires dep1, dep2, ... */
            if (parser_match(p, TOK_REQUIRES)) {
                AstNode *deps = NULL;
                AstNode *dep = ast_new(p->arena, AST_IDENT, p->current.loc);
                dep->name = parser_expect(p, TOK_IDENT,
                                          "after 'requires'").value.str_val;
                deps = ast_append(deps, dep);
                while (parser_match(p, TOK_COMMA)) {
                    AstNode *d = ast_new(p->arena, AST_IDENT, p->current.loc);
                    d->name = parser_expect(p, TOK_IDENT,
                                            "in requires list").value.str_val;
                    deps = ast_append(deps, d);
                }
                item->params = deps;
            }

            items = ast_append(items, item);
        }
        else {
            report_error(p->reporter, p->current.loc,
                         "expected capability item, 'allow', 'deny', or 'default'",
                         NULL);
            p->had_error = true;
            parser_advance(p);
        }
        skip_semis(p);
    }
    parser_expect(p, TOK_RBRACE, "after capability body");

    node->params = items;
    return node;
}

/* parse_guard: guard name(...) { ... } */
static AstNode *parse_guard(Parser *p, bool is_pub) {
    SourceLoc loc = p->previous.loc;
    AstNode *node = ast_new(p->arena, AST_GUARD, loc);
    node->is_pub = is_pub;
    node->name = parser_expect(p, TOK_IDENT, "after 'guard'").value.str_val;

    /* Optional parameter list */
    if (parser_match(p, TOK_LPAREN)) {
        AstNode *params = NULL;
        while (!parser_check(p, TOK_RPAREN) && !parser_check(p, TOK_EOF)) {
            SourceLoc ploc = p->current.loc;
            AstNode *param = ast_new(p->arena, AST_PARAM, ploc);
            param->name = parser_expect(p, TOK_IDENT, "in guard parameter").value.str_val;
            if (parser_match(p, TOK_COLON)) {
                param->type_expr = parse_type_expr(p);
            }
            params = ast_append(params, param);
            if (!parser_match(p, TOK_COMMA)) break;
        }
        parser_expect(p, TOK_RPAREN, "after guard parameters");
        node->params = params;
    }

    /* Body */
    node->left = parse_block(p);
    return node;
}

/* parse_taint_decl: taint name */
static AstNode *parse_taint_decl(Parser *p) {
    SourceLoc loc = p->previous.loc;
    AstNode *node = ast_new(p->arena, AST_TAINT, loc);
    node->name = parser_expect(p, TOK_IDENT, "after 'taint'").value.str_val;
    return node;
}

/* parse_budget: budget Name { ... } */
static AstNode *parse_budget(Parser *p, bool is_pub) {
    SourceLoc loc = p->previous.loc;
    AstNode *node = ast_new(p->arena, AST_BUDGET, loc);
    node->is_pub = is_pub;
    node->name = parser_expect(p, TOK_IDENT, "after 'budget'").value.str_val;

    parser_expect(p, TOK_LBRACE, "after budget name");
    skip_semis(p);

    AstNode *fields = NULL;
    while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
        AstNode *field = parse_decl_field(p);
        fields = ast_append(fields, field);
        skip_semis(p);
    }
    parser_expect(p, TOK_RBRACE, "after budget body");

    node->params = fields;
    return node;
}

/* parse_tool: tool name(...) -> Type { ... } */
static AstNode *parse_tool(Parser *p, bool is_pub) {
    SourceLoc loc = p->previous.loc;
    AstNode *node = ast_new(p->arena, AST_TOOL, loc);
    node->is_pub = is_pub;
    node->name = parser_expect(p, TOK_IDENT, "after 'tool'").value.str_val;

    /* Parameter list */
    parser_expect(p, TOK_LPAREN, "after tool name");
    {
        AstNode *params = NULL;
        while (!parser_check(p, TOK_RPAREN) && !parser_check(p, TOK_EOF)) {
            SourceLoc ploc = p->current.loc;
            AstNode *param = ast_new(p->arena, AST_PARAM, ploc);
            if (is_ident_or_keyword(p->current.kind))
                param->name = consume_ident_name(p);
            else
                param->name = parser_expect(p, TOK_IDENT, "in tool parameter").value.str_val;
            if (parser_match(p, TOK_COLON)) {
                param->type_expr = parse_type_expr(p);
            }
            params = ast_append(params, param);
            if (!parser_match(p, TOK_COMMA)) break;
        }
        parser_expect(p, TOK_RPAREN, "after tool parameters");
        node->params = params;
    }

    /* Optional return type */
    if (parser_match(p, TOK_ARROW)) {
        node->type_expr = parse_type_expr(p);
    }

    /* Body: { requires: [...], description: "...", statements... } */
    parser_expect(p, TOK_LBRACE, "after tool signature");
    skip_semis(p);
    {
        AstNode *body_fields = NULL;
        AstNode *body_stmts = NULL;
        while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
            /* requires: [...] or description: "..." — field assignment */
            if (is_ident_or_keyword(p->current.kind) && p->current.kind != TOK_LET
                && p->current.kind != TOK_RETURN && p->current.kind != TOK_IF
                && p->current.kind != TOK_FOR && p->current.kind != TOK_WHILE
                && p->current.kind != TOK_LOOP && p->current.kind != TOK_MATCH) {
                /* Peek ahead: is this `ident:` (field) or expression? */
                /* Save state — if next is colon, treat as field */
                Token saved = p->current;
                parser_advance(p);
                if (parser_check(p, TOK_COLON)) {
                    /* It's a field: rewind name, parse as decl field */
                    AstNode *field = ast_new(p->arena, AST_FIELD, saved.loc);
                    if (saved.kind == TOK_IDENT) {
                        field->name = saved.value.str_val;
                    } else {
                        field->name = arena_strdup(p->arena, token_kind_name(saved.kind));
                    }
                    parser_advance(p); /* consume colon */
                    if (parser_check(p, TOK_LBRACKET)) {
                        /* Array value */
                        parser_advance(p);
                        AstNode *arr = ast_new(p->arena, AST_ARRAY, saved.loc);
                        AstNode *elems = NULL;
                        while (!parser_check(p, TOK_RBRACKET) && !parser_check(p, TOK_EOF)) {
                            SourceLoc eloc = p->current.loc;
                            AstNode *elem = ast_new(p->arena, AST_IDENT, eloc);
                            elem->name = parse_qualified_ident(p);
                            elems = ast_append(elems, elem);
                            if (!parser_match(p, TOK_COMMA)) break;
                        }
                        parser_expect(p, TOK_RBRACKET, "after array");
                        arr->params = elems;
                        field->right = arr;
                    } else {
                        field->right = parse_expr(p, PREC_NONE);
                    }
                    body_fields = ast_append(body_fields, field);
                } else {
                    /* Not a field — it was an expression starting with that token.
                     * We already consumed it, so we need to handle this.
                     * For simplicity, treat remaining body as block. */
                    /* This is a statement starting with an ident — parse rest as expr */
                    /* We already advanced past the first token — just parse as statement from here */
                    AstNode *stmt = parse_statement(p);
                    body_stmts = ast_append(body_stmts, stmt);
                }
            } else {
                AstNode *stmt = parse_statement(p);
                body_stmts = ast_append(body_stmts, stmt);
            }
            skip_semis(p);
        }
        parser_expect(p, TOK_RBRACE, "after tool body");

        /* Store fields in right, statements in a block on left */
        node->right = body_fields;
        if (body_stmts) {
            AstNode *block = ast_new(p->arena, AST_BLOCK, loc);
            block->params = body_stmts;
            node->left = block;
        }
    }
    return node;
}

/* parse_skill: skill Name { ... } */
static AstNode *parse_skill(Parser *p, bool is_pub) {
    SourceLoc loc = p->previous.loc;
    AstNode *node = ast_new(p->arena, AST_SKILL, loc);
    node->is_pub = is_pub;
    node->name = parser_expect(p, TOK_IDENT, "after 'skill'").value.str_val;

    parser_expect(p, TOK_LBRACE, "after skill name");
    skip_semis(p);

    AstNode *members = NULL;
    while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
        /* use imports */
        if (parser_match(p, TOK_USE)) {
            AstNode *use = parse_use_decl(p);
            members = ast_append(members, use);
        }
        /* tool declarations */
        else if (parser_match(p, TOK_TOOL)) {
            AstNode *tool = parse_tool(p, false);
            members = ast_append(members, tool);
        }
        /* pub tool */
        else if (parser_check(p, TOK_PUB)) {
            parser_advance(p);
            if (parser_match(p, TOK_TOOL)) {
                AstNode *tool = parse_tool(p, true);
                members = ast_append(members, tool);
            } else {
                AstNode *field = parse_decl_field(p);
                field->is_pub = true;
                members = ast_append(members, field);
            }
        }
        /* fn declarations */
        else if (parser_match(p, TOK_FN)) {
            AstNode *fn = parse_fn(p, false);
            members = ast_append(members, fn);
        }
        /* field: value (keywords allowed as field names) */
        else if (is_ident_or_keyword(p->current.kind)) {
            AstNode *field = parse_decl_field(p);
            members = ast_append(members, field);
        }
        else {
            report_error(p->reporter, p->current.loc,
                        "expected field, use, tool, or fn in skill body", NULL);
            p->had_error = true;
            parser_advance(p);
        }
        skip_semis(p);
    }
    parser_expect(p, TOK_RBRACE, "after skill body");

    node->params = members;
    return node;
}

/* parse_progress: progress { total: expr, current: expr }
 * Parses a progress reporting block.
 * AST_PROGRESS: left = total expression, right = current expression
 *   The field names (total/current) refer to variable identifiers whose
 *   assignments will trigger progress update calls in codegen. */
static AstNode *parse_progress(Parser *p) {
    SourceLoc loc = p->previous.loc;
    AstNode *node = ast_new(p->arena, AST_PROGRESS, loc);

    parser_expect(p, TOK_LBRACE, "after 'progress'");
    skip_semis(p);

    /* Parse fields: total: expr, current: expr */
    while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
        const char *fname = consume_ident_name(p);
        parser_expect(p, TOK_COLON, "after field name in progress block");
        AstNode *val = parse_expr(p, PREC_NONE);
        if (strcmp(fname, "total") == 0) {
            node->left = val;
        } else if (strcmp(fname, "current") == 0) {
            node->right = val;
        }
        /* Accept both commas and semicolons/newlines as separators */
        if (!parser_match(p, TOK_COMMA)) skip_semis(p);
    }
    parser_expect(p, TOK_RBRACE, "after progress body");

    return node;
}

/* parse_supervisor: supervisor Name { ... } */
static AstNode *parse_supervisor(Parser *p, bool is_pub) {
    SourceLoc loc = p->previous.loc;
    AstNode *node = ast_new(p->arena, AST_SUPERVISOR, loc);
    node->is_pub = is_pub;
    node->name = parser_expect(p, TOK_IDENT, "after 'supervisor'").value.str_val;

    parser_expect(p, TOK_LBRACE, "after supervisor name");
    skip_semis(p);

    AstNode *fields = NULL;
    while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
        AstNode *field = parse_decl_field(p);
        fields = ast_append(fields, field);
        skip_semis(p);
    }
    parser_expect(p, TOK_RBRACE, "after supervisor body");

    node->params = fields;
    return node;
}

/* parse_mesh_ident_list: parse [A, B, C] or single ident, returns linked AST_IDENT list */
static AstNode *parse_mesh_ident_list(Parser *p, bool *is_list) {
    *is_list = false;
    if (parser_match(p, TOK_LBRACKET)) {
        *is_list = true;
        AstNode *list = NULL;
        while (!parser_check(p, TOK_RBRACKET) && !parser_check(p, TOK_EOF)) {
            SourceLoc eloc = p->current.loc;
            AstNode *ident = ast_new(p->arena, AST_IDENT, eloc);
            ident->name = parser_expect(p, TOK_IDENT, "agent name in mesh route list").value.str_val;
            list = ast_append(list, ident);
            if (!parser_match(p, TOK_COMMA)) break;
        }
        parser_expect(p, TOK_RBRACKET, "after mesh route list");
        return list;
    }
    /* Single identifier */
    SourceLoc eloc = p->current.loc;
    AstNode *ident = ast_new(p->arena, AST_IDENT, eloc);
    ident->name = parser_expect(p, TOK_IDENT, "agent name in mesh route").value.str_val;
    return ident;
}

/* parse_mesh: mesh Name { route/stage ..., ... }
 *   stage "label" -> AgentName { config }     (legacy sequential)
 *   route Source -> Target                     (single route)
 *   route Source -> [A, B, C]                  (fan-out / parallel)
 *   route [A, B, C] -> Target                  (fan-in / collect)
 *
 * AST_MESH_ROUTE node layout:
 *   left  = source ident list (AST_IDENT linked)
 *   right = target ident list (AST_IDENT linked)
 *   is_mut = true if source is a list (fan-in)
 *   is_pub = true if target is a list (fan-out)  [reusing fields]
 */
static AstNode *parse_mesh(Parser *p, bool is_pub) {
    SourceLoc loc = p->previous.loc;
    AstNode *node = ast_new(p->arena, AST_MESH, loc);
    node->is_pub = is_pub;
    node->name = parser_expect(p, TOK_IDENT, "after 'mesh'").value.str_val;

    parser_expect(p, TOK_LBRACE, "after mesh name");
    skip_semis(p);

    AstNode *members = NULL;
    while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
        SourceLoc mloc = p->current.loc;
        /* route ... -> ... */
        if (parser_match(p, TOK_ROUTE)) {
            AstNode *route = ast_new(p->arena, AST_MESH_ROUTE, mloc);
            bool src_is_list = false;
            route->left = parse_mesh_ident_list(p, &src_is_list);
            route->is_mut = src_is_list;  /* reuse: is_mut = source is list (fan-in) */
            parser_expect(p, TOK_ARROW, "after mesh route source");
            bool tgt_is_list = false;
            route->right = parse_mesh_ident_list(p, &tgt_is_list);
            route->is_unsafe = tgt_is_list;  /* reuse: is_unsafe = target is list (fan-out) */
            members = ast_append(members, route);
        }
        /* legacy: stage "label" -> AgentName { config } */
        else if (parser_check(p, TOK_IDENT) && p->current.value.str_val &&
            strcmp(p->current.value.str_val, "stage") == 0) {
            parser_advance(p);
            AstNode *stage = ast_new(p->arena, AST_MESH_STAGE, mloc);
            stage->val.str_val = parser_expect(p, TOK_STRING_LIT, "stage name").value.str_val;
            parser_expect(p, TOK_ARROW, "after stage name");
            stage->name = parser_expect(p, TOK_IDENT, "agent name in mesh stage").value.str_val;
            if (parser_check(p, TOK_LBRACE)) {
                parser_advance(p);
                skip_semis(p);
                AstNode *config = NULL;
                while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
                    AstNode *field = parse_decl_field(p);
                    config = ast_append(config, field);
                    skip_semis(p);
                }
                parser_expect(p, TOK_RBRACE, "after stage config");
                stage->params = config;
            }
            members = ast_append(members, stage);
        }
        else if (is_ident_or_keyword(p->current.kind)) {
            AstNode *field = parse_decl_field(p);
            members = ast_append(members, field);
        }
        else {
            report_error(p->reporter, p->current.loc,
                        "expected 'route', 'stage', or field in mesh body", NULL);
            p->had_error = true;
            parser_advance(p);
        }
        skip_semis(p);
    }
    parser_expect(p, TOK_RBRACE, "after mesh body");

    node->params = members;
    return node;
}

/* parse_router: router Name { route "tier" -> [model1, model2]; strategy: cost_aware } */
static AstNode *parse_router(Parser *p, bool is_pub) {
    SourceLoc loc = p->previous.loc;
    AstNode *node = ast_new(p->arena, AST_ROUTER, loc);
    node->is_pub = is_pub;
    node->name = parser_expect(p, TOK_IDENT, "after 'router'").value.str_val;

    parser_expect(p, TOK_LBRACE, "after router name");
    skip_semis(p);

    AstNode *members = NULL;
    while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
        SourceLoc mloc = p->current.loc;
        if (parser_match(p, TOK_ROUTE)) {
            AstNode *rule = ast_new(p->arena, AST_ROUTE_RULE, mloc);
            rule->val.str_val = parser_expect(p, TOK_STRING_LIT, "route tier name").value.str_val;
            parser_expect(p, TOK_ARROW, "after route tier");
            parser_expect(p, TOK_LBRACKET, "in route model list");
            AstNode *models = NULL;
            while (!parser_check(p, TOK_RBRACKET) && !parser_check(p, TOK_EOF)) {
                SourceLoc eloc = p->current.loc;
                AstNode *model = ast_new(p->arena, AST_IDENT, eloc);
                model->name = parser_expect(p, TOK_IDENT, "model name").value.str_val;
                models = ast_append(models, model);
                if (!parser_match(p, TOK_COMMA)) break;
            }
            parser_expect(p, TOK_RBRACKET, "after model list");
            rule->params = models;
            members = ast_append(members, rule);
        }
        else if (is_ident_or_keyword(p->current.kind)) {
            AstNode *field = parse_decl_field(p);
            members = ast_append(members, field);
        }
        else {
            report_error(p->reporter, p->current.loc,
                        "expected 'route' or field in router body", NULL);
            p->had_error = true;
            parser_advance(p);
        }
        skip_semis(p);
    }
    parser_expect(p, TOK_RBRACE, "after router body");

    node->params = members;
    return node;
}

/* parse_metrics: metrics { counter/histogram/gauge NAME "desc"; port: N }
 *
 * AST_METRICS node layout:
 *   params = linked list of AST_METRICS_FIELD nodes
 *   val.int_val = port number (default 9091)
 *
 * AST_METRICS_FIELD node layout:
 *   name = metric name (e.g. "processed_total")
 *   val.str_val = description string
 *   is_mut = true for gauge
 *   is_pub = true for histogram
 */
static AstNode *parse_metrics(Parser *p) {
    SourceLoc loc = p->previous.loc;
    AstNode *node = ast_new(p->arena, AST_METRICS, loc);
    node->val.int_val = 9091; /* default port */

    parser_expect(p, TOK_LBRACE, "after 'metrics'");
    skip_semis(p);

    AstNode *fields = NULL;
    while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
        SourceLoc floc = p->current.loc;

        if (parser_check(p, TOK_IDENT) && p->current.value.str_val) {
            const char *kw = p->current.value.str_val;
            if (strcmp(kw, "counter") == 0 || strcmp(kw, "gauge") == 0 ||
                strcmp(kw, "histogram") == 0) {
                parser_advance(p);
                AstNode *mf = ast_new(p->arena, AST_METRICS_FIELD, floc);
                if (strcmp(kw, "gauge") == 0)     mf->is_mut = true;
                if (strcmp(kw, "histogram") == 0) mf->is_pub = true;
                mf->name = parser_expect(p, TOK_IDENT, "metric name").value.str_val;
                mf->val.str_val = parser_expect(p, TOK_STRING_LIT, "metric description").value.str_val;
                fields = ast_append(fields, mf);
            }
            else if (strcmp(kw, "port") == 0) {
                parser_advance(p);
                parser_expect(p, TOK_COLON, "after 'port'");
                if (parser_check(p, TOK_INT_LIT)) {
                    node->val.int_val = p->current.value.int_val;
                    parser_advance(p);
                } else {
                    report_error(p->reporter, p->current.loc,
                                "expected integer port number", NULL);
                    p->had_error = true;
                    parser_advance(p);
                }
            }
            else {
                AstNode *field = parse_decl_field(p);
                fields = ast_append(fields, field);
            }
        } else {
            report_error(p->reporter, p->current.loc,
                        "expected 'counter', 'gauge', 'histogram', or 'port' in metrics body", NULL);
            p->had_error = true;
            parser_advance(p);
        }
        skip_semis(p);
    }
    parser_expect(p, TOK_RBRACE, "after metrics body");

    node->params = fields;
    return node;
}


/* parse_health: health { ready: expr, live: expr, port: N }
 * AST_HEALTH node:
 *   left  = ready expression
 *   right = live expression
 *   val.int_val = port number (default 9090)
 *   params = raw field list for extensibility */
static AstNode *parse_health(Parser *p) {
    SourceLoc loc = p->current.loc;
    parser_advance(p);  /* consume "health" */

    AstNode *node = ast_new(p->arena, AST_HEALTH, loc);
    node->val.int_val = 9090;  /* default port */

    parser_expect(p, TOK_LBRACE, "after 'health'");
    skip_semis(p);

    AstNode *fields = NULL;
    while (!parser_check(p, TOK_RBRACE) && !parser_check(p, TOK_EOF)) {
        AstNode *field = parse_decl_field(p);
        if (field && field->name) {
            if (strcmp(field->name, "ready") == 0) {
                node->left = field->right;
            } else if (strcmp(field->name, "live") == 0) {
                node->right = field->right;
            } else if (strcmp(field->name, "port") == 0) {
                if (field->right && field->right->kind == AST_INT_LIT) {
                    node->val.int_val = field->right->val.int_val;
                }
            }
        }
        fields = ast_append(fields, field);
        skip_semis(p);
    }
    parser_expect(p, TOK_RBRACE, "after health body");

    node->params = fields;
    return node;
}

static AstNode *parse_declaration(Parser *p) {
    AstNode *attrs = parse_attributes(p);
    bool is_pub = parser_match(p, TOK_PUB);
    AstNode *decl = NULL;

    if (parser_match(p, TOK_FN)) {
        decl = parse_fn(p, is_pub);
    } else if (parser_match(p, TOK_STRUCT)) {
        decl = parse_struct(p, is_pub);
    } else if (parser_match(p, TOK_ENUM)) {
        decl = parse_enum(p, is_pub);
    } else if (parser_match(p, TOK_TRAIT)) {
        decl = parse_trait(p, is_pub);
    } else if (parser_match(p, TOK_INTERFACE)) {
        decl = parse_interface(p, is_pub);
    } else if (parser_match(p, TOK_IMPL)) {
        decl = parse_impl(p);
    } else if (parser_match(p, TOK_TYPE)) {
        decl = parse_type_alias(p, is_pub);
    } else if (parser_match(p, TOK_CONST)) {
        decl = parse_const_decl(p, is_pub);
    } else if (parser_match(p, TOK_MOD)) {
        decl = parse_module_decl(p);
    } else if (parser_match(p, TOK_USE)) {
        decl = parse_use_decl(p);
    } else if (parser_match(p, TOK_AGENT)) {
        decl = parse_agent(p, is_pub);
    } else if (parser_match(p, TOK_CAPABILITY)) {
        decl = parse_capability(p, is_pub);
    } else if (parser_match(p, TOK_GUARD)) {
        decl = parse_guard(p, is_pub);
    } else if (parser_match(p, TOK_TAINT)) {
        decl = parse_taint_decl(p);
    } else if (parser_match(p, TOK_BUDGET)) {
        decl = parse_budget(p, is_pub);
    } else if (parser_match(p, TOK_TOOL)) {
        decl = parse_tool(p, is_pub);
    } else if (parser_match(p, TOK_SKILL)) {
        decl = parse_skill(p, is_pub);
    } else if (parser_match(p, TOK_SUPERVISOR)) {
        decl = parse_supervisor(p, is_pub);
    } else if (parser_match(p, TOK_MESH)) {
        decl = parse_mesh(p, is_pub);
    } else if (parser_match(p, TOK_ROUTER)) {
        decl = parse_router(p, is_pub);
    } else if (parser_match(p, TOK_PROGRESS)) {
        decl = parse_progress(p);
    } else if (parser_match(p, TOK_COMPTIME)) {
        /* comptime { block } — compile-time evaluation at top level */
        SourceLoc cloc = p->previous.loc;
        decl = ast_new(p->arena, AST_COMPTIME, cloc);
        decl->is_pub = is_pub;
        decl->left = parse_block(p);
    } else if (parser_match(p, TOK_INVARIANT)) {
        /* invariant Name { expression }
         * Parses a named invariant with a body expression.
         * AST: name = invariant name, left = body expression (binary comparison) */
        SourceLoc iloc = p->previous.loc;
        decl = ast_new(p->arena, AST_INVARIANT, iloc);
        decl->is_pub = is_pub;
        decl->name = parser_expect(p, TOK_IDENT, "after 'invariant'").value.str_val;
        parser_expect(p, TOK_LBRACE, "after invariant name");
        skip_semis(p);
        decl->left = parse_expr(p, PREC_NONE);
        skip_semis(p);
        parser_expect(p, TOK_RBRACE, "after invariant body");
    } else if (parser_check(p, TOK_IDENT) && p->current.value.str_val &&
               strcmp(p->current.value.str_val, "health") == 0) {
        /* health { ready: expr, live: expr, port: N } */
        decl = parse_health(p);
    } else if (parser_check(p, TOK_IDENT) && p->current.value.str_val &&
               strcmp(p->current.value.str_val, "metrics") == 0) {
        /* metrics { counter/histogram/gauge NAME "desc"; port: N } */
        parser_advance(p);  /* consume "metrics" */
        decl = parse_metrics(p);
    } else if (parser_check(p, TOK_IDENT) && p->current.value.str_val &&
               strcmp(p->current.value.str_val, "link") == 0) {
        /* link "-lssl -lcrypto"  — FFI link directive (linker flags) */
        SourceLoc lloc = p->current.loc;
        parser_advance(p);  /* consume "link" */
        decl = ast_new(p->arena, AST_LINK, lloc);
        decl->val.str_val = parser_expect(p, TOK_STRING_LIT, "link flags string").value.str_val;
    } else if (parser_check(p, TOK_IDENT) && p->current.value.str_val &&
               strcmp(p->current.value.str_val, "extern") == 0) {
        /* extern fn name(params) -> Type  — FFI declaration (no body) */
        SourceLoc eloc = p->current.loc;
        parser_advance(p);  /* consume "extern" */
        parser_expect(p, TOK_FN, "after 'extern'");

        decl = ast_new(p->arena, AST_FN, eloc);
        decl->is_pub = is_pub;
        decl->is_unsafe = true;  /* marks as extern — no body */
        decl->name = parser_expect(p, TOK_IDENT, "function name").value.str_val;

        /* Parameters */
        parser_expect(p, TOK_LPAREN, "after function name");
        AstNode *eparams = NULL;
        while (!parser_check(p, TOK_RPAREN) && !parser_check(p, TOK_EOF)) {
            SourceLoc ploc = p->current.loc;
            AstNode *param = ast_new(p->arena, AST_PARAM, ploc);
            if (is_ident_or_keyword(p->current.kind))
                param->name = consume_ident_name(p);
            else
                param->name = parser_expect(p, TOK_IDENT, "in parameter").value.str_val;
            parser_expect(p, TOK_COLON, "after parameter name");
            param->type_expr = parse_type_expr(p);
            eparams = ast_append(eparams, param);
            if (!parser_match(p, TOK_COMMA)) break;
        }
        parser_expect(p, TOK_RPAREN, "after parameters");
        decl->params = eparams;

        /* Optional return type */
        if (parser_match(p, TOK_ARROW)) {
            decl->type_expr = parse_type_expr(p);
        }

        /* NO body — extern functions have no implementation */
        decl->left = NULL;
    } else {
        /* Not a declaration — parse as statement */
        if (is_pub) {
            report_error(p->reporter, p->current.loc,
                        "'pub' can only be used before fn, struct, enum, trait, interface, type, const, "
                        "agent, capability, guard, budget, tool, skill, or supervisor",
                        NULL);
            p->had_error = true;
        }
        if (attrs) {
            /* Attributes on a non-declaration — that's an error but we keep going */
        }
        return parse_statement(p);
    }

    if (decl && attrs) {
        decl->attributes = attrs;
    }
    return decl;
}

/* ============================================================
 * Top-Level: Parse Program
 * ============================================================ */

AstNode *parse_program(Parser *p) {
    SourceLoc loc = p->current.loc;
    AstNode *program = ast_new(p->arena, AST_PROGRAM, loc);
    AstNode *decls = NULL;

    skip_semis(p);
    while (!parser_check(p, TOK_EOF)) {
        AstNode *decl = parse_declaration(p);
        if (decl) {
            decls = ast_append(decls, decl);
        }
        skip_semis(p);
        if (p->panic_mode) {
            parser_synchronize(p);
        }
    }

    program->params = decls;
    return program;
}
