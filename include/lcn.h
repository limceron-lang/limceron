/*
 * Limceron Compiler — Master Header
 * All types, enums, and function declarations for Stage 0.
 */

#ifndef LCN_H
#define LCN_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>

/* ============================================================
 * Configuration
 * ============================================================ */

#define LCN_VERSION        "0.1.0-stage0"
#define MAX_SOURCE_SIZE     (64 * 1024 * 1024)
#define ARENA_SIZE          (128 * 1024 * 1024)
#define MAX_TOKENS          (4 * 1024 * 1024)
#define MAX_ERRORS          100
#define MAX_IDENT_LEN       256
#define STRING_INTERN_CAP   65536
#define SELF_HASH_LEN       32

/* ============================================================
 * Arena Allocator
 * ============================================================ */

typedef struct {
    uint8_t *base;
    size_t   size;
    size_t   used;
} Arena;

Arena  arena_new(size_t size);
void  *arena_alloc(Arena *a, size_t bytes);
char  *arena_strdup(Arena *a, const char *s);
char  *arena_strndup(Arena *a, const char *s, size_t len);
void   arena_reset(Arena *a);
void   arena_free(Arena *a);

/* ============================================================
 * Source Location
 * ============================================================ */

typedef struct {
    const char *filename;
    uint32_t    line;
    uint32_t    column;
    uint32_t    offset;
} SourceLoc;

/* ============================================================
 * Tokens
 * ============================================================ */

typedef enum {
    /* Literals */
    TOK_INT_LIT,
    TOK_FLOAT_LIT,
    TOK_STRING_LIT,
    TOK_CHAR_LIT,
    TOK_IDENT,

    /* Keywords */
    TOK_LET,
    TOK_MUT,
    TOK_FN,
    TOK_RETURN,
    TOK_IF,
    TOK_ELSE,
    TOK_MATCH,
    TOK_FOR,
    TOK_WHILE,
    TOK_LOOP,
    TOK_BREAK,
    TOK_CONTINUE,
    TOK_STRUCT,
    TOK_ENUM,
    TOK_TRAIT,
    TOK_IMPL,
    TOK_INTERFACE,
    TOK_MOD,
    TOK_USE,
    TOK_PUB,
    TOK_PRIV,
    TOK_SPAWN,
    TOK_AWAIT,
    TOK_SELECT,
    TOK_CHAN,
    TOK_DEFER,
    TOK_UNSAFE,
    TOK_COMPTIME,
    TOK_TYPE,
    TOK_AS,
    TOK_IN,
    TOK_IS,
    TOK_TRUE,
    TOK_FALSE,
    TOK_NONE,
    TOK_CONST,

    /* Agent System Keywords */
    TOK_AGENT,
    TOK_GUARD,
    TOK_CAPABILITY,
    TOK_TAINT,
    TOK_BUDGET,
    TOK_TOOL,
    TOK_SKILL,
    TOK_PROMPT,
    TOK_SUPERVISOR,
    TOK_MESH,
    TOK_MEMORY,
    TOK_ASK,
    TOK_TELL,
    TOK_ENSURE,
    TOK_INVARIANT,
    TOK_GUARDSET,
    TOK_REQUIRES,
    TOK_OTHERWISE,
    TOK_SHOWING,
    TOK_TIMEOUT,
    TOK_CHOICES,
    TOK_REPEAT,
    TOK_TIMES,
    TOK_WAIT,
    TOK_UNTIL,
    TOK_EACH,
    TOK_KEEP,
    TOK_WHERE,
    TOK_SECRET,
    TOK_ABOUT,
    TOK_CHANNEL,
    TOK_ROUTER,
    TOK_ROUTE,
    TOK_STRATEGY,

    /* Progress Reporting */
    TOK_PROGRESS,

    /* Access Control Keywords */
    TOK_ALLOW,
    TOK_DENY,

    /* Taint annotation */
    TOK_AT,              /* @ */

    /* Operators */
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_POWER,
    TOK_AMP,
    TOK_PIPE,
    TOK_CARET,
    TOK_TILDE,
    TOK_SHL,
    TOK_SHR,
    TOK_EQ_EQ,
    TOK_NOT_EQ,
    TOK_LT,
    TOK_GT,
    TOK_LT_EQ,
    TOK_GT_EQ,
    TOK_AND_AND,
    TOK_PIPE_PIPE,
    TOK_BANG,
    TOK_EQ,
    TOK_PLUS_EQ,
    TOK_MINUS_EQ,
    TOK_STAR_EQ,
    TOK_SLASH_EQ,
    TOK_PERCENT_EQ,
    TOK_AMP_EQ,
    TOK_PIPE_EQ,
    TOK_CARET_EQ,
    TOK_SHL_EQ,
    TOK_SHR_EQ,
    TOK_QUESTION,
    TOK_QUESTION_DOT,
    TOK_DOT_DOT,
    TOK_DOT_DOT_EQ,
    TOK_ARROW,
    TOK_FAT_ARROW,
    TOK_COLON_COLON,
    TOK_PIPE_GT,

    /* Delimiters */
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_COMMA,
    TOK_DOT,
    TOK_COLON,
    TOK_SEMICOLON,
    TOK_HASH,
    TOK_NEWLINE,

    /* Special */
    TOK_EOF,
    TOK_ERROR,

    TOK_COUNT
} TokenKind;

const char *token_kind_name(TokenKind kind);
bool token_ends_stmt(TokenKind kind);

typedef struct {
    TokenKind   kind;
    SourceLoc   loc;
    union {
        int64_t     int_val;
        double      float_val;
        const char *str_val;
    } value;
    uint32_t    len;
} Token;

/* ============================================================
 * String Interning
 * ============================================================ */

typedef struct {
    char   *strings[STRING_INTERN_CAP];
    size_t  count;
    Arena  *arena;
} StringIntern;

StringIntern    intern_new(Arena *arena);
const char     *intern_get(StringIntern *si, const char *s, size_t len);

/* ============================================================
 * Error Reporting
 * ============================================================ */

typedef struct {
    SourceLoc   loc;
    const char *message;
    const char *hint;
    bool        is_warning;
    uint32_t    underline_len;  /* length of ^^^ underline (0 = single ^) */
} CompileError;

typedef struct {
    CompileError errors[MAX_ERRORS];
    int          count;
    const char  *source;
    size_t       source_len;
    const char  *filename;
} ErrorReporter;

ErrorReporter reporter_new(const char *filename, const char *source, size_t len);
void          report_error(ErrorReporter *r, SourceLoc loc,
                           const char *message, const char *hint);
void          report_error_fmt(ErrorReporter *r, SourceLoc loc,
                               const char *hint, const char *fmt, ...);
void          report_warning(ErrorReporter *r, SourceLoc loc,
                              const char *message, const char *hint);
void          report_warning_fmt(ErrorReporter *r, SourceLoc loc,
                                  const char *hint, const char *fmt, ...);
bool          has_errors(const ErrorReporter *r);

/* Format a diagnostic message into a buffer (no ANSI colors).
 * Returns the number of characters written (excluding NUL).
 * If buf is NULL or bufsize is 0, returns the required size. */
int format_diagnostic(char *buf, size_t bufsize,
                      const char *source, size_t source_len,
                      const char *filename,
                      SourceLoc loc, const char *message,
                      const char *hint, bool is_warning,
                      uint32_t underline_len);

/* ============================================================
 * Lexer
 * ============================================================ */

typedef struct {
    const char     *source;
    size_t          source_len;
    size_t          pos;
    uint32_t        line;
    uint32_t        column;
    const char     *filename;
    StringIntern   *intern;
    ErrorReporter  *reporter;
    Token           prev_token;
    bool            at_line_start;
} Lexer;

Lexer   lexer_new(const char *filename, const char *source, size_t len,
                  StringIntern *intern, ErrorReporter *reporter);
Token   lexer_next_raw(Lexer *l);
Token   lexer_next(Lexer *l);

/* ============================================================
 * AST
 * ============================================================ */

typedef enum {
    /* Top-level declarations */
    AST_PROGRAM,
    AST_MODULE,
    AST_USE,
    AST_FN,
    AST_STRUCT,
    AST_ENUM,
    AST_VARIANT,
    AST_TRAIT,
    AST_INTERFACE,
    AST_IMPL,
    AST_TYPE_ALIAS,
    AST_CONST,
    AST_FIELD,
    AST_PARAM,

    /* Statements */
    AST_BLOCK,
    AST_LET,
    AST_RETURN,
    AST_DEFER,
    AST_ASSIGN,
    AST_EXPR_STMT,
    AST_FOR,
    AST_WHILE,
    AST_LOOP,
    AST_BREAK,
    AST_CONTINUE,

    /* Expressions */
    AST_INT_LIT,
    AST_FLOAT_LIT,
    AST_STRING_LIT,
    AST_BOOL_LIT,
    AST_NONE_LIT,
    AST_IDENT,
    AST_BINARY,
    AST_UNARY,
    AST_CALL,
    AST_FIELD_ACCESS,
    AST_INDEX,
    AST_METHOD_CALL,
    AST_IF,
    AST_MATCH,
    AST_MATCH_ARM,
    AST_CLOSURE,
    AST_ARRAY,
    AST_MAP,
    AST_MAP_ENTRY,
    AST_TUPLE,
    AST_CAST,
    AST_IS_EXPR,
    AST_RANGE,
    AST_PIPE,
    AST_SPAWN,
    AST_AWAIT,
    AST_SELECT,
    AST_SELECT_ARM,
    AST_TRY,
    AST_UNSAFE_BLOCK,
    AST_COMPTIME,
    AST_REF,
    AST_DEREF,

    /* Type expressions */
    AST_TYPE_NAMED,
    AST_TYPE_REF,
    AST_TYPE_PTR,
    AST_TYPE_ARRAY,
    AST_TYPE_SLICE,
    AST_TYPE_TUPLE,
    AST_TYPE_FN,
    AST_TYPE_UNION,
    AST_TYPE_OPTIONAL,
    AST_TYPE_INFER,

    /* Patterns */
    AST_PAT_WILDCARD,
    AST_PAT_IDENT,
    AST_PAT_LITERAL,
    AST_PAT_TYPED,
    AST_PAT_ENUM,
    AST_PAT_STRUCT,
    AST_PAT_TUPLE,
    AST_PAT_RANGE,
    AST_PAT_OR,

    /* Generic parameters & attributes */
    AST_GENERIC_PARAM,
    AST_ATTRIBUTE,

    /* Agent System declarations */
    AST_AGENT,
    AST_CAPABILITY,
    AST_CAPABILITY_ITEM,
    AST_GUARD,
    AST_GUARDSET,
    AST_INVARIANT,
    AST_TAINT,
    AST_BUDGET,
    AST_BUDGET_FIELD,
    AST_TOOL,
    AST_SKILL,
    AST_PROMPT,
    AST_PROMPT_SECTION,
    AST_PROMPT_PARAMS,
    AST_SUPERVISOR,
    AST_SUPERVISOR_CHILD,
    AST_MESH,
    AST_MESH_STAGE,
    AST_MESH_ROUTE,        /* route X -> Y | route X -> [A,B] | route [A,B] -> X */
    AST_MEMORY,
    AST_CHANNEL,

    /* Agent System expressions */
    AST_ASK,
    AST_TELL,
    AST_ENSURE,
    AST_REPEAT,
    AST_WAIT_UNTIL,
    AST_TRY_OTHERWISE,
    AST_KEEP_WHERE,
    AST_EACH,

    /* Taint-annotated type */
    AST_TYPE_TAINTED,

    /* Secret-annotated type (compile-time confidentiality) */
    AST_TYPE_SECRET,

    /* LLM Inference Layer */
    AST_ROUTER,
    AST_ROUTE_RULE,
    AST_STRATEGY,

    /* Structured Concurrency */
    AST_TASK_GROUP,          /* task_group { spawn expr1; spawn expr2; ... } */

    /* Access Control Policy rules */
    AST_CAP_ENDPOINT_RULE,   /* allow/deny endpoint "host:port" { method: [...], path: "..." } */
    AST_CAP_BINARY_RULE,     /* allow/deny binary "/path"                                      */
    AST_CAP_PATH_RULE,       /* allow/deny path "/pattern" { mode: [read, write] }             */
    AST_CAP_DENY_RANGE,      /* deny private_ranges                                            */
    AST_CAP_DEFAULT,         /* default: allow | default: deny                                 */

    /* FFI link directive */
    AST_LINK,                /* link "-lssl -lcrypto"  — linker flags for C library consumption */

    /* Prometheus-compatible metrics */
    AST_METRICS,             /* metrics { counter/histogram/gauge fields, port: N } */
    AST_METRICS_FIELD,       /* counter/histogram/gauge NAME "description"          */

    /* Progress reporting */
    AST_PROGRESS,            /* progress { total: expr, current: expr }             */

    /* Health probe declaration */
    AST_HEALTH,              /* health { ready: expr, live: expr, port: N } */

    /* Signal handler declaration */
    AST_SIGNAL,              /* signal SIGTERM { handler_body } */

    AST_COUNT
} AstKind;

const char *ast_kind_name(AstKind kind);

typedef struct AstNode AstNode;

struct AstNode {
    AstKind     kind;
    SourceLoc   loc;
    AstNode    *next;           /* sibling in a list */

    /* Common fields */
    const char *name;           /* identifier name */
    AstNode    *type_expr;      /* type annotation */
    bool        is_pub;
    bool        is_mut;
    bool        is_unsafe;

    /* Children — meaning depends on kind:
     *
     * AST_FN:       left=body, params=params, type_expr=return_type, generics=generics
     * AST_STRUCT:   params=fields, generics=generics
     * AST_ENUM:     params=variants, generics=generics
     * AST_VARIANT:  params=fields
     * AST_IMPL:     left=target_type, right=trait, params=methods, generics=generics
     * AST_TRAIT:    params=methods, generics=generics
     * AST_INTERFACE:params=methods, generics=generics
     * AST_LET:      right=initializer, type_expr=type
     * AST_RETURN:   left=value
     * AST_DEFER:    left=expr
     * AST_ASSIGN:   left=target, right=value, val.op=operator
     * AST_FOR:      left=pattern, right=body, params=iterator
     * AST_WHILE:    left=condition, right=body
     * AST_LOOP:     left=body
     * AST_IF:       left=condition, right=then_body, params=else_body
     * AST_MATCH:    left=subject, params=arms
     * AST_MATCH_ARM:left=pattern, right=body, params=guard
     * AST_BINARY:   left=lhs, right=rhs, val.op=operator
     * AST_UNARY:    left=operand, val.op=operator
     * AST_CALL:     left=callee, params=args
     * AST_FIELD_ACCESS: left=object, name=field
     * AST_INDEX:    left=object, right=index
     * AST_METHOD_CALL: left=object, name=method, params=args
     * AST_CLOSURE:  left=body, params=params
     * AST_ARRAY:    params=elements
     * AST_MAP:      params=entries
     * AST_MAP_ENTRY:left=key, right=value
     * AST_RANGE:    left=start, right=end, is_mut=inclusive
     * AST_CAST:     left=expr, type_expr=target
     * AST_IS_EXPR:  left=expr, type_expr=target
     * AST_SPAWN:    left=body
     * AST_AWAIT:    left=expr
     * AST_TRY:      left=expr  (the ? operator)
     * AST_REF:      left=expr, is_mut=is_mut_ref
     * AST_TYPE_NAMED: name=name, generics=generic_args
     * AST_TYPE_REF:   left=inner, is_mut=is_mut
     * AST_TYPE_ARRAY: left=element, right=size_expr
     * AST_TYPE_SLICE: left=element
     * AST_TYPE_UNION: params=variants
     * AST_TYPE_OPTIONAL: left=inner
     * AST_TYPE_FN:  params=param_types, type_expr=return_type
     * AST_PAT_TYPED: name=name, type_expr=type
     * AST_PAT_ENUM: name=variant_path, params=fields
     * AST_GENERIC_PARAM: name=name, params=bounds
     */
    AstNode    *left;
    AstNode    *right;
    AstNode    *params;
    AstNode    *generics;
    AstNode    *attributes;

    /* Literal/operator values */
    union {
        int64_t     int_val;
        double      float_val;
        const char *str_val;
        bool        bool_val;
        TokenKind   op;
    } val;
};

AstNode    *ast_new(Arena *a, AstKind kind, SourceLoc loc);
AstNode    *ast_append(AstNode *list, AstNode *node);
int         ast_list_len(const AstNode *list);
void        ast_print(const AstNode *node, int indent);

/* ============================================================
 * Parser
 * ============================================================ */

typedef enum {
    PREC_NONE,
    PREC_ASSIGN,        /* = += -= *= /= %= etc. */
    PREC_PIPE_OP,       /* |> */
    PREC_OR,            /* || */
    PREC_AND,           /* && */
    PREC_BIT_OR,        /* | */
    PREC_BIT_XOR,       /* ^ */
    PREC_BIT_AND,       /* & */
    PREC_EQUALITY,      /* == != */
    PREC_COMPARISON,    /* < > <= >= */
    PREC_SHIFT,         /* << >> */
    PREC_RANGE,         /* .. ..= */
    PREC_TERM,          /* + - */
    PREC_FACTOR,        /* * / % */
    PREC_POWER,         /* ** */
    PREC_UNARY,         /* ! - ~ & * */
    PREC_POSTFIX,       /* . () [] ? as is */
    PREC_PRIMARY
} Precedence;

typedef struct Parser Parser;

typedef AstNode *(*PrefixParseFn)(Parser *p);
typedef AstNode *(*InfixParseFn)(Parser *p, AstNode *left);

typedef struct {
    PrefixParseFn prefix;
    InfixParseFn  infix;
    Precedence    precedence;
} ParseRule;

struct Parser {
    Lexer          *lexer;
    Token           current;
    Token           previous;
    Arena          *arena;
    ErrorReporter  *reporter;
    bool            had_error;
    bool            panic_mode;
};

Parser      parser_new(Lexer *lexer, Arena *arena, ErrorReporter *reporter);
AstNode    *parse_program(Parser *p);
AstNode    *parse_expression(Parser *p);
AstNode    *parse_type_expr(Parser *p);

/* ============================================================
 * Markdown Source Parser (.lceron.md)
 * ============================================================ */

AstNode    *parse_lceron_md(const char *filename, const char *source, size_t source_len,
                          Arena *arena, StringIntern *intern, ErrorReporter *reporter);

/* ============================================================
 * Security — .lceron signing & self-verification
 * ============================================================ */

#define LCN_MAGIC          0x4C494D43  /* "LIMC" */
#define LCERON_OBJ_VERSION        1
#define SIGNATURE_SIZE      64
#define HASH_SIZE           32

/* .lceron file header */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t section_count;
    uint64_t code_offset;
    uint64_t code_size;
    uint64_t data_offset;
    uint64_t data_size;
    uint64_t symtab_offset;
    uint64_t symtab_size;
    uint64_t reloc_offset;
    uint64_t reloc_size;
    uint64_t debug_offset;
    uint64_t debug_size;
    uint8_t  content_hash[HASH_SIZE];
    uint8_t  signature[SIGNATURE_SIZE];
} LceronObjHeader;

/* Section types in .lceron */
typedef enum {
    LCERON_OBJ_SECTION_CODE,
    LCERON_OBJ_SECTION_DATA,
    LCERON_OBJ_SECTION_RODATA,
    LCERON_OBJ_SECTION_BSS,
    LCERON_OBJ_SECTION_SYMTAB,
    LCERON_OBJ_SECTION_RELOC,
    LCERON_OBJ_SECTION_DEBUG,
    LCERON_OBJ_SECTION_META,
} LceronObjSectionKind;

/* Self-verification */
bool    security_self_verify(const char *exe_path);
void    security_hash_buffer(const uint8_t *data, size_t len, uint8_t out[HASH_SIZE]);
bool    security_sign_lceron(LceronObjHeader *header, const uint8_t *data, size_t len,
                           const uint8_t private_key[HASH_SIZE]);
bool    security_verify_lceron(const LceronObjHeader *header, const uint8_t *data, size_t len,
                             const uint8_t public_key[HASH_SIZE]);

/* ============================================================
 * Compiler Pipeline (future phases)
 * ============================================================ */

/* Read a source file into an arena-allocated buffer */
char *read_source_file(Arena *a, const char *path, size_t *out_len);

/* ============================================================
 * Type Checker & Semantic Analysis
 * ============================================================ */

/* Run all type-checking passes on a parsed program.
 * Returns true if no errors were found. */
bool typecheck_program(AstNode *program, ErrorReporter *reporter, Arena *arena);

/* ============================================================
 * Cross-Compilation Target
 * ============================================================ */

/* Target triple: ARCH-OS[-ABI] */
typedef enum {
    LCN_ARCH_UNKNOWN = 0,
    LCN_ARCH_X86_64,
    LCN_ARCH_AARCH64,
    LCN_ARCH_ARM
} LcnArch;

typedef enum {
    LCN_OS_UNKNOWN = 0,
    LCN_OS_LINUX,
    LCN_OS_DARWIN,
    LCN_OS_WINDOWS
} LcnOS;

typedef enum {
    LCN_ABI_NONE = 0,
    LCN_ABI_GNU,
    LCN_ABI_MUSL,
    LCN_ABI_MSVC
} LcnABI;

typedef struct {
    LcnArch  arch;
    LcnOS    os;
    LcnABI   abi;
    char     triple[128];    /* original triple string, e.g. "aarch64-linux-gnu" */
    char     cc[512];        /* resolved cross-compiler path */
    char     cflags[1024];   /* target-specific CFLAGS */
    char     ldflags[1024];  /* target-specific LDFLAGS */
    bool     is_native;      /* true if target matches host */
    bool     static_link;    /* true if --static requested */
} LcnTarget;

/* Parse a target triple string into an LcnTarget struct.
 * Returns a zero-initialized target with arch=UNKNOWN on failure. */
LcnTarget lcn_parse_target(const char *triple);

/* Detect the native (host) target. */
LcnTarget lcn_native_target(void);

/* Find a suitable cross-compiler for the given target.
 * Populates target->cc. Returns true if found. */
bool lcn_find_cross_cc(LcnTarget *target);

/* Get the canonical triple string for a target (e.g. "x86_64-linux-gnu"). */
const char *lcn_target_triple_str(const LcnTarget *target);

/* Get the architecture name as a string. */
const char *lcn_arch_str(LcnArch arch);

/* Get the OS name as a string. */
const char *lcn_os_str(LcnOS os);

/* Get the ABI name as a string. */
const char *lcn_abi_str(LcnABI abi);

/* ============================================================
 * SSA Intermediate Representation
 * ============================================================ */

/* See ir.h for full IR type definitions. The ir command is:
 *   limceron-stage0 ir <file.lceron>  -- Print SSA IR (debug) */

/* ============================================================
 * Code Generation (Limceron→C Transpiler)
 * ============================================================ */

/* Generate C code from parsed AST. Returns malloc'd string (caller must free).
 * Standalone mode: inlines all runtime types (output is self-contained .c). */
char *codegen_generate(AstNode *program, const char *source_file, Arena *arena);

/* Generate C code for build mode: emits #include "lcn_runtime.h" instead of
 * inlining runtime types. Output must be compiled with -I<runtime-dir> and
 * linked against runtime .o files. */
char *codegen_generate_for_build(AstNode *program, const char *source_file, Arena *arena);

/* Generate C code for build mode with a specific cross-compilation target. */
char *codegen_generate_for_build_target(AstNode *program, const char *source_file,
                                        Arena *arena, const LcnTarget *target);

/* Public: MCP server mode — agent as MCP service over stdin/stdout */
char *codegen_generate_for_serve(AstNode *program, const char *source_file, Arena *arena);

/* ============================================================
 * LSP Server
 * ============================================================ */

/* Start the LSP server on stdin/stdout (JSON-RPC 2.0). */
int cmd_lsp(void);

/* LSP internal helpers (exposed for testing) */
char       *lsp_read_message(void);
void         lsp_send_message(const char *json);
const char  *lsp_json_get_string(const char *json, const char *key, char *buf, size_t bufsz);
long         lsp_json_get_int(const char *json, const char *key);

#endif /* LCN_H */
