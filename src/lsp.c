/*
 * Limceron Compiler -- LSP (Language Server Protocol) Server
 *
 * Implements a minimal LSP server over stdin/stdout using JSON-RPC 2.0.
 * Provides diagnostics, completion, hover, and go-to-definition for
 * .lceron source files.
 *
 * Protocol wire format:
 *   Content-Length: N\r\n\r\n{...JSON...}
 *
 * Compiles cleanly under: -std=c99 -Wall -Wextra -Werror -pedantic
 */

#include "lcn.h"
#include <ctype.h>

/* ============================================================
 * Configuration
 * ============================================================ */

#define LSP_MAX_MSG_SIZE    (4 * 1024 * 1024)
#define LSP_MAX_FIELD_LEN   4096
#define LSP_MAX_URI_LEN     1024
#define LSP_MAX_DOCS        64
#define LSP_MAX_COMPLETIONS 256
#define LSP_MAX_RESPONSE    (1 * 1024 * 1024)

/* ============================================================
 * Minimal JSON helpers (self-contained, no external deps)
 * ============================================================ */

/* Skip whitespace in JSON string */
static const char *json_skip_ws(const char *p) {
    while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    return p;
}

/* Extract a string value for a given key from a JSON object.
 * Writes into buf[bufsz]. Returns buf on success, NULL on failure.
 * Only handles simple flat/nested key lookups. */
const char *lsp_json_get_string(const char *json, const char *key, char *buf, size_t bufsz) {
    if (!json || !key || !buf || bufsz == 0) return NULL;
    buf[0] = '\0';

    /* Search for "key" : "value" */
    char search[512];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return NULL;

    p += strlen(search);
    p = json_skip_ws(p);
    if (*p != ':') return NULL;
    p++;
    p = json_skip_ws(p);

    if (*p != '"') return NULL;
    p++; /* skip opening quote */

    size_t i = 0;
    while (*p && *p != '"' && i < bufsz - 1) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
            case '"':  buf[i++] = '"';  break;
            case '\\': buf[i++] = '\\'; break;
            case '/':  buf[i++] = '/';  break;
            case 'n':  buf[i++] = '\n'; break;
            case 't':  buf[i++] = '\t'; break;
            case 'r':  buf[i++] = '\r'; break;
            default:   buf[i++] = *p;   break;
            }
        } else {
            buf[i++] = *p;
        }
        p++;
    }
    buf[i] = '\0';
    return buf;
}

/* Extract an integer value for a given key. Returns -1 on failure. */
long lsp_json_get_int(const char *json, const char *key) {
    if (!json || !key) return -1;

    char search[512];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return -1;

    p += strlen(search);
    p = json_skip_ws(p);
    if (*p != ':') return -1;
    p++;
    p = json_skip_ws(p);

    /* Parse integer (possibly negative) */
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        return strtol(p, NULL, 10);
    }
    return -1;
}

/* Find the "params" sub-object. Returns pointer into json, or NULL. */
static const char *json_find_params(const char *json) {
    const char *p = strstr(json, "\"params\"");
    if (!p) return NULL;
    p += 8; /* strlen("\"params\"") */
    p = json_skip_ws(p);
    if (*p != ':') return NULL;
    p++;
    p = json_skip_ws(p);
    return p;
}

/* Find a nested object by key. Returns pointer to the '{'. */
static const char *json_find_object(const char *json, const char *key) {
    if (!json || !key) return NULL;
    char search[512];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return NULL;
    p += strlen(search);
    p = json_skip_ws(p);
    if (*p != ':') return NULL;
    p++;
    p = json_skip_ws(p);
    if (*p == '{') return p;
    return NULL;
}

/* ============================================================
 * LSP message I/O
 * ============================================================ */

/* Read one LSP message from stdin. Returns malloc'd JSON string, or NULL on EOF.
 * Caller must free the returned string. */
char *lsp_read_message(void) {
    /* Read headers until empty line */
    int content_length = -1;
    char header_line[1024];

    while (1) {
        if (!fgets(header_line, sizeof(header_line), stdin)) {
            return NULL; /* EOF */
        }

        /* Check for empty line (end of headers) */
        if (strcmp(header_line, "\r\n") == 0 || strcmp(header_line, "\n") == 0) {
            break;
        }

        /* Parse Content-Length */
        if (strncmp(header_line, "Content-Length:", 15) == 0) {
            content_length = atoi(header_line + 15);
        }
    }

    if (content_length <= 0 || content_length > LSP_MAX_MSG_SIZE) {
        return NULL;
    }

    char *body = (char *)malloc((size_t)content_length + 1);
    if (!body) return NULL;

    size_t total_read = 0;
    while ((int)total_read < content_length) {
        size_t n = fread(body + total_read, 1,
                         (size_t)(content_length - (int)total_read), stdin);
        if (n == 0) {
            free(body);
            return NULL;
        }
        total_read += n;
    }
    body[content_length] = '\0';
    return body;
}

/* Send an LSP message to stdout. */
void lsp_send_message(const char *json) {
    size_t len = strlen(json);
    fprintf(stdout, "Content-Length: %zu\r\n\r\n%s", len, json);
    fflush(stdout);
}

/* ============================================================
 * LSP response builders
 * ============================================================ */

/* Send a JSON-RPC result response for a given request id. */
static void lsp_send_result(long id, const char *result_json) {
    char buf[LSP_MAX_RESPONSE];
    snprintf(buf, sizeof(buf),
             "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":%s}", id, result_json);
    lsp_send_message(buf);
}

/* Send a JSON-RPC null result (for shutdown, etc.) */
static void lsp_send_null_result(long id) {
    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"result\":null}", id);
    lsp_send_message(buf);
}

/* Send a JSON-RPC error response. */
static void lsp_send_error(long id, int code, const char *message) {
    char buf[1024];
    snprintf(buf, sizeof(buf),
             "{\"jsonrpc\":\"2.0\",\"id\":%ld,\"error\":{\"code\":%d,\"message\":\"%s\"}}",
             id, code, message);
    lsp_send_message(buf);
}

/* Send a notification (no id). */
static void lsp_send_notification(const char *method, const char *params_json) {
    char buf[LSP_MAX_RESPONSE];
    snprintf(buf, sizeof(buf),
             "{\"jsonrpc\":\"2.0\",\"method\":\"%s\",\"params\":%s}",
             method, params_json);
    lsp_send_message(buf);
}

/* ============================================================
 * Document store — tracks open files
 * ============================================================ */

typedef struct {
    char     uri[LSP_MAX_URI_LEN];
    char    *content;       /* malloc'd source text */
    size_t   content_len;
    AstNode *ast;           /* parsed AST (arena-allocated) */
    Arena    arena;         /* per-document arena */
    Arena    intern_arena;  /* per-document intern arena */
    bool     valid;         /* true if parse succeeded */
} LspDocument;

static LspDocument lsp_docs[LSP_MAX_DOCS];
static int lsp_doc_count = 0;

static LspDocument *lsp_find_doc(const char *uri) {
    for (int i = 0; i < lsp_doc_count; i++) {
        if (strcmp(lsp_docs[i].uri, uri) == 0) {
            return &lsp_docs[i];
        }
    }
    return NULL;
}

static LspDocument *lsp_open_doc(const char *uri) {
    LspDocument *doc = lsp_find_doc(uri);
    if (doc) return doc;
    if (lsp_doc_count >= LSP_MAX_DOCS) return NULL;

    doc = &lsp_docs[lsp_doc_count++];
    memset(doc, 0, sizeof(*doc));
    strncpy(doc->uri, uri, LSP_MAX_URI_LEN - 1);
    doc->arena = arena_new(8 * 1024 * 1024);
    doc->intern_arena = arena_new(2 * 1024 * 1024);
    return doc;
}

static void lsp_close_doc(const char *uri) {
    for (int i = 0; i < lsp_doc_count; i++) {
        if (strcmp(lsp_docs[i].uri, uri) == 0) {
            arena_free(&lsp_docs[i].arena);
            arena_free(&lsp_docs[i].intern_arena);
            free(lsp_docs[i].content);
            /* Swap with last */
            lsp_docs[i] = lsp_docs[lsp_doc_count - 1];
            memset(&lsp_docs[lsp_doc_count - 1], 0, sizeof(LspDocument));
            lsp_doc_count--;
            return;
        }
    }
}

/* ============================================================
 * URI helpers
 * ============================================================ */

/* Convert file:///path/to/file to /path/to/file */
static const char *uri_to_path(const char *uri) {
    if (strncmp(uri, "file://", 7) == 0) {
        return uri + 7;
    }
    return uri;
}

/* Guess filename for error reporter from URI */
static const char *uri_to_filename(const char *uri) {
    const char *path = uri_to_path(uri);
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* ============================================================
 * Parse & publish diagnostics
 * ============================================================ */

/* Escape a string for JSON output. Writes into buf, returns buf. */
static char *json_escape_string(const char *s, char *buf, size_t bufsz) {
    size_t i = 0;
    buf[0] = '\0';
    if (!s) return buf;
    while (*s && i < bufsz - 6) { /* leave room for \uXXXX */
        switch (*s) {
        case '"':  buf[i++] = '\\'; buf[i++] = '"';  break;
        case '\\': buf[i++] = '\\'; buf[i++] = '\\'; break;
        case '\n': buf[i++] = '\\'; buf[i++] = 'n';  break;
        case '\r': buf[i++] = '\\'; buf[i++] = 'r';  break;
        case '\t': buf[i++] = '\\'; buf[i++] = 't';  break;
        default:
            if ((unsigned char)*s < 0x20) {
                i += (size_t)snprintf(buf + i, bufsz - i, "\\u%04x", (unsigned char)*s);
            } else {
                buf[i++] = *s;
            }
            break;
        }
        s++;
    }
    buf[i] = '\0';
    return buf;
}

/* Parse document content and send diagnostics. */
static void lsp_parse_and_diagnose(LspDocument *doc) {
    if (!doc || !doc->content) return;

    /* Reset arenas for re-parse */
    arena_reset(&doc->arena);
    arena_reset(&doc->intern_arena);

    const char *filename = uri_to_filename(doc->uri);
    ErrorReporter reporter = reporter_new(filename, doc->content, doc->content_len);
    StringIntern intern = intern_new(&doc->intern_arena);
    Lexer lexer = lexer_new(filename, doc->content, doc->content_len, &intern, &reporter);
    Parser parser = parser_new(&lexer, &doc->arena, &reporter);

    doc->ast = parse_program(&parser);
    doc->valid = !parser.had_error;

    /* Also run type checker for additional diagnostics */
    if (doc->ast && doc->valid) {
        typecheck_program(doc->ast, &reporter, &doc->arena);
    }

    /* Build diagnostics JSON */
    char diag_buf[LSP_MAX_RESPONSE];
    int off = 0;
    char esc[2048];

    off += snprintf(diag_buf + off, sizeof(diag_buf) - (size_t)off,
                    "{\"uri\":\"%s\",\"diagnostics\":[", doc->uri);

    for (int i = 0; i < reporter.count && (size_t)off < sizeof(diag_buf) - 512; i++) {
        CompileError *e = &reporter.errors[i];
        uint32_t line = e->loc.line > 0 ? e->loc.line - 1 : 0; /* LSP is 0-based */
        uint32_t col  = e->loc.column > 0 ? e->loc.column - 1 : 0;
        int severity  = e->is_warning ? 2 : 1; /* 1=Error, 2=Warning */

        json_escape_string(e->message, esc, sizeof(esc));

        if (i > 0) diag_buf[off++] = ',';
        off += snprintf(diag_buf + off, sizeof(diag_buf) - (size_t)off,
                        "{\"range\":{\"start\":{\"line\":%u,\"character\":%u},"
                        "\"end\":{\"line\":%u,\"character\":%u}},"
                        "\"severity\":%d,"
                        "\"source\":\"limceron\","
                        "\"message\":\"%s\"}",
                        line, col, line, col + (e->underline_len > 0 ? e->underline_len : 1),
                        severity, esc);
    }

    off += snprintf(diag_buf + off, sizeof(diag_buf) - (size_t)off, "]}");

    lsp_send_notification("textDocument/publishDiagnostics", diag_buf);
}

/* ============================================================
 * initialize
 * ============================================================ */

static void lsp_handle_initialize(long id) {
    const char *result =
        "{"
            "\"capabilities\":{"
                "\"textDocumentSync\":{\"openClose\":true,\"change\":1,\"save\":{\"includeText\":true}},"
                "\"completionProvider\":{\"triggerCharacters\":[\".\",\":\"]},"
                "\"hoverProvider\":true,"
                "\"definitionProvider\":true,"
                "\"diagnosticProvider\":{\"interFileDependencies\":false}"
            "},"
            "\"serverInfo\":{"
                "\"name\":\"limceron-lsp\","
                "\"version\":\"" LCN_VERSION "\""
            "}"
        "}";
    lsp_send_result(id, result);
}

/* ============================================================
 * textDocument/didOpen
 * ============================================================ */

static void lsp_handle_did_open(const char *params) {
    const char *td = json_find_object(params, "textDocument");
    if (!td) return;

    char uri[LSP_MAX_URI_LEN];
    if (!lsp_json_get_string(td, "uri", uri, sizeof(uri))) return;

    char text_buf[LSP_MAX_MSG_SIZE];
    if (!lsp_json_get_string(td, "text", text_buf, sizeof(text_buf))) return;

    LspDocument *doc = lsp_open_doc(uri);
    if (!doc) return;

    /* Store content */
    free(doc->content);
    doc->content_len = strlen(text_buf);
    doc->content = (char *)malloc(doc->content_len + 1);
    if (doc->content) {
        memcpy(doc->content, text_buf, doc->content_len + 1);
    }

    lsp_parse_and_diagnose(doc);
}

/* ============================================================
 * textDocument/didChange (full sync, textDocumentSync=1)
 * ============================================================ */

static void lsp_handle_did_change(const char *params) {
    const char *td = json_find_object(params, "textDocument");
    if (!td) return;

    char uri[LSP_MAX_URI_LEN];
    if (!lsp_json_get_string(td, "uri", uri, sizeof(uri))) return;

    LspDocument *doc = lsp_find_doc(uri);
    if (!doc) return;

    /* Find contentChanges array — extract text from first element.
     * With textDocumentSync=1 (Full), there's one element with full text. */
    const char *changes = strstr(params, "\"contentChanges\"");
    if (!changes) return;
    /* Find the first { in the array */
    const char *arr = strchr(changes, '[');
    if (!arr) return;
    const char *obj = strchr(arr, '{');
    if (!obj) return;

    char text_buf[LSP_MAX_MSG_SIZE];
    if (!lsp_json_get_string(obj, "text", text_buf, sizeof(text_buf))) return;

    free(doc->content);
    doc->content_len = strlen(text_buf);
    doc->content = (char *)malloc(doc->content_len + 1);
    if (doc->content) {
        memcpy(doc->content, text_buf, doc->content_len + 1);
    }

    lsp_parse_and_diagnose(doc);
}

/* ============================================================
 * textDocument/didSave (L10: re-run typecheck on save)
 * ============================================================ */

static void lsp_handle_did_save(const char *params) {
    const char *td = json_find_object(params, "textDocument");
    if (!td) return;
    char uri[LSP_MAX_URI_LEN];
    if (!lsp_json_get_string(td, "uri", uri, sizeof(uri))) return;
    LspDocument *doc = lsp_find_doc(uri);
    if (!doc) return;
    char text_buf[LSP_MAX_MSG_SIZE];
    if (lsp_json_get_string(params, "text", text_buf, sizeof(text_buf))) {
        free(doc->content);
        doc->content_len = strlen(text_buf);
        doc->content = (char *)malloc(doc->content_len + 1);
        if (doc->content) memcpy(doc->content, text_buf, doc->content_len + 1);
    }
    lsp_parse_and_diagnose(doc);
}

/* ============================================================
 * textDocument/didClose
 * ============================================================ */

static void lsp_handle_did_close(const char *params) {
    const char *td = json_find_object(params, "textDocument");
    if (!td) return;

    char uri[LSP_MAX_URI_LEN];
    if (!lsp_json_get_string(td, "uri", uri, sizeof(uri))) return;

    lsp_close_doc(uri);
}

/* ============================================================
 * textDocument/completion
 * ============================================================ */

/* Limceron keywords for completion */
static const char *lsp_keywords[] = {
    "fn", "let", "mut", "if", "else", "return", "for", "while", "loop",
    "break", "continue", "struct", "enum", "trait", "impl", "interface",
    "mod", "use", "pub", "priv", "match", "spawn", "await", "select",
    "chan", "defer", "unsafe", "comptime", "type", "as", "in", "is",
    "true", "false", "none", "const",
    /* Agent system keywords */
    "agent", "guard", "capability", "taint", "budget", "tool", "skill",
    "prompt", "supervisor", "mesh", "memory", "ask", "tell", "ensure",
    "invariant", "guardset", "requires", "otherwise", "showing",
    "timeout", "choices", "repeat", "times", "wait", "until",
    "each", "keep", "where", "secret", "about", "channel", "router",
    "route", "strategy",
    NULL
};

/* Limceron builtin functions for completion */
static const char *lsp_builtins[] = {
    "print", "println", "len", "contains", "starts_with", "ends_with",
    "push", "pop", "env", "env_or", "str_eq", "str_replace", "str_trim",
    "str_split", "str_upper", "str_lower", "str_len", "str_find",
    "to_string", "to_int", "to_float",
    "json_parse", "json_get", "json_array_len", "json_array_get",
    "json_stringify", "json_set",
    "math_abs", "math_min", "math_max", "math_sqrt", "math_pow",
    "math_floor", "math_ceil",
    "time_now", "time_sleep", "time_format",
    "log_info", "log_warn", "log_error", "log_debug",
    "file_read", "file_write", "file_exists", "file_delete",
    "sql_escape",
    "delegate", "revoke", "revoke_all", "has_capability",
    "batch", "trace_start", "trace_end",
    NULL
};

/* Collect identifiers declared in the AST (fn names, let bindings, params, structs, etc.)
 * before a given line. Appends to completions array. */
static int lsp_collect_scope_identifiers(AstNode *node, uint32_t cursor_line,
                                         char *buf, size_t bufsz, int off) {
    if (!node) return off;

    for (AstNode *d = (node->kind == AST_PROGRAM) ? node->params : node; d; d = d->next) {
        if (!d->name) continue;

        /* Only include declarations before or at cursor */
        if (d->loc.line > cursor_line) continue;

        /* Determine completion kind (LSP CompletionItemKind) */
        int kind = 6; /* Variable */
        const char *detail = "";
        switch (d->kind) {
        case AST_FN:
            kind = 3; /* Function */
            detail = "fn";
            break;
        case AST_STRUCT:
            kind = 22; /* Struct */
            detail = "struct";
            break;
        case AST_ENUM:
            kind = 13; /* Enum */
            detail = "enum";
            break;
        case AST_AGENT:
            kind = 7; /* Class — closest to agent */
            detail = "agent";
            break;
        case AST_TRAIT:
        case AST_INTERFACE:
            kind = 8; /* Interface */
            detail = "trait";
            break;
        case AST_LET:
            kind = 6; /* Variable */
            detail = "let";
            break;
        case AST_PARAM:
            kind = 6;
            detail = "param";
            break;
        case AST_TOOL:
            kind = 3;
            detail = "tool";
            break;
        case AST_SKILL:
            kind = 3;
            detail = "skill";
            break;
        case AST_CAPABILITY:
            kind = 11; /* Value — closest for cap */
            detail = "capability";
            break;
        case AST_GUARD:
            kind = 3;
            detail = "guard";
            break;
        case AST_BUDGET:
            kind = 11;
            detail = "budget";
            break;
        default:
            continue;
        }

        char esc_name[512];
        json_escape_string(d->name, esc_name, sizeof(esc_name));

        if (off > 0 && (size_t)off < bufsz - 1) {
            buf[off++] = ',';
        }
        off += snprintf(buf + off, bufsz - (size_t)off,
                        "{\"label\":\"%s\",\"kind\":%d,\"detail\":\"%s\"}",
                        esc_name, kind, detail);

        if ((size_t)off >= bufsz - 256) break;

        /* Also scan fn params */
        if (d->kind == AST_FN && d->params) {
            for (AstNode *p = d->params; p; p = p->next) {
                if (p->name && p->kind == AST_PARAM) {
                    char esc_pname[512];
                    json_escape_string(p->name, esc_pname, sizeof(esc_pname));
                    if ((size_t)off < bufsz - 256) {
                        buf[off++] = ',';
                        off += snprintf(buf + off, bufsz - (size_t)off,
                                        "{\"label\":\"%s\",\"kind\":6,\"detail\":\"param\"}",
                                        esc_pname);
                    }
                }
            }
        }
    }
    return off;
}

/* Collect struct field names for member completion (after '.') */
static int lsp_collect_struct_fields(AstNode *program, const char *struct_name,
                                     char *buf, size_t bufsz, int off) {
    if (!program || !struct_name) return off;

    for (AstNode *d = program->params; d; d = d->next) {
        if (d->kind == AST_STRUCT && d->name && strcmp(d->name, struct_name) == 0) {
            for (AstNode *f = d->params; f; f = f->next) {
                if (f->kind == AST_FIELD && f->name) {
                    char esc[512];
                    json_escape_string(f->name, esc, sizeof(esc));
                    if (off > 0 && (size_t)off < bufsz - 1) buf[off++] = ',';
                    off += snprintf(buf + off, bufsz - (size_t)off,
                                    "{\"label\":\"%s\",\"kind\":5,\"detail\":\"field\"}",
                                    esc);
                }
            }
            break;
        }
        /* Also handle agent fields */
        if (d->kind == AST_AGENT && d->name && strcmp(d->name, struct_name) == 0) {
            for (AstNode *f = d->params; f; f = f->next) {
                if (f->kind == AST_FIELD && f->name) {
                    char esc[512];
                    json_escape_string(f->name, esc, sizeof(esc));
                    if (off > 0 && (size_t)off < bufsz - 1) buf[off++] = ',';
                    off += snprintf(buf + off, bufsz - (size_t)off,
                                    "{\"label\":\"%s\",\"kind\":5,\"detail\":\"field\"}",
                                    esc);
                }
            }
            break;
        }
    }
    return off;
}

static void lsp_handle_completion(long id, const char *params) {
    const char *td = json_find_object(params, "textDocument");
    const char *pos = json_find_object(params, "position");

    char uri[LSP_MAX_URI_LEN];
    if (td) lsp_json_get_string(td, "uri", uri, sizeof(uri));
    else uri[0] = '\0';

    long line = pos ? lsp_json_get_int(pos, "line") : 0;
    long character = pos ? lsp_json_get_int(pos, "character") : 0;
    (void)character;

    LspDocument *doc = lsp_find_doc(uri);

    /* Check if cursor is after a '.' (member completion) */
    bool is_member_completion = false;
    if (doc && doc->content && character > 0) {
        /* Find the position in the source */
        const char *src = doc->content;
        long cur_line = 0;
        long cur_col = 0;
        const char *p = src;
        while (*p && (cur_line < line || (cur_line == line && cur_col < character - 1))) {
            if (*p == '\n') {
                cur_line++;
                cur_col = 0;
            } else {
                cur_col++;
            }
            p++;
        }
        if (*p == '.') is_member_completion = true;
    }

    /* Build completion items */
    char items[LSP_MAX_RESPONSE];
    int off = 0;

    if (!is_member_completion) {
        /* Keywords */
        for (int i = 0; lsp_keywords[i]; i++) {
            if (off > 0) items[off++] = ',';
            off += snprintf(items + off, sizeof(items) - (size_t)off,
                            "{\"label\":\"%s\",\"kind\":14,\"detail\":\"keyword\"}",
                            lsp_keywords[i]);
            if ((size_t)off >= sizeof(items) - 256) break;
        }

        /* Builtins */
        for (int i = 0; lsp_builtins[i] && (size_t)off < sizeof(items) - 256; i++) {
            if (off > 0) items[off++] = ',';
            off += snprintf(items + off, sizeof(items) - (size_t)off,
                            "{\"label\":\"%s\",\"kind\":3,\"detail\":\"builtin\"}",
                            lsp_builtins[i]);
        }

        /* Scope identifiers from AST */
        if (doc && doc->ast) {
            off = lsp_collect_scope_identifiers(doc->ast, (uint32_t)(line + 1),
                                                items, sizeof(items), off);
        }
    } else {
        /* Member completion: for now, provide fields from all structs/agents.
         * A more advanced implementation would resolve the type of the expression. */
        if (doc && doc->ast) {
            for (AstNode *d = doc->ast->params; d; d = d->next) {
                if ((d->kind == AST_STRUCT || d->kind == AST_AGENT) && d->name) {
                    off = lsp_collect_struct_fields(doc->ast, d->name,
                                                   items, sizeof(items), off);
                }
                if ((size_t)off >= sizeof(items) - 256) break;
            }
        }
    }

    char result[LSP_MAX_RESPONSE];
    snprintf(result, sizeof(result), "{\"isIncomplete\":false,\"items\":[%s]}", items);
    lsp_send_result(id, result);
}

/* ============================================================
 * textDocument/hover
 * ============================================================ */

/* Build a hover signature for a function node */
static int lsp_build_fn_signature(AstNode *fn_node, char *buf, size_t bufsz) {
    if (!fn_node || fn_node->kind != AST_FN) return 0;

    int off = 0;
    off += snprintf(buf + off, bufsz - (size_t)off, "fn %s(", fn_node->name ? fn_node->name : "?");

    /* Parameters */
    int first = 1;
    for (AstNode *p = fn_node->params; p; p = p->next) {
        if (p->kind != AST_PARAM) continue;
        if (!first) off += snprintf(buf + off, bufsz - (size_t)off, ", ");
        first = 0;
        off += snprintf(buf + off, bufsz - (size_t)off, "%s", p->name ? p->name : "_");
        if (p->type_expr && p->type_expr->kind == AST_TYPE_NAMED && p->type_expr->name) {
            off += snprintf(buf + off, bufsz - (size_t)off, ": %s", p->type_expr->name);
        }
    }

    off += snprintf(buf + off, bufsz - (size_t)off, ")");

    /* Return type */
    if (fn_node->type_expr && fn_node->type_expr->kind == AST_TYPE_NAMED && fn_node->type_expr->name) {
        off += snprintf(buf + off, bufsz - (size_t)off, " -> %s", fn_node->type_expr->name);
    }

    return off;
}

/* Keyword descriptions */
static const char *lsp_keyword_description(const char *word) {
    if (!word) return NULL;
    if (strcmp(word, "fn") == 0)       return "Function declaration";
    if (strcmp(word, "let") == 0)      return "Variable binding";
    if (strcmp(word, "mut") == 0)      return "Mutable modifier";
    if (strcmp(word, "if") == 0)       return "Conditional expression";
    if (strcmp(word, "else") == 0)     return "Alternative branch";
    if (strcmp(word, "return") == 0)   return "Return from function";
    if (strcmp(word, "for") == 0)      return "For loop (iteration)";
    if (strcmp(word, "while") == 0)    return "While loop (conditional)";
    if (strcmp(word, "loop") == 0)     return "Infinite loop";
    if (strcmp(word, "match") == 0)    return "Pattern matching expression";
    if (strcmp(word, "struct") == 0)   return "Struct type declaration";
    if (strcmp(word, "enum") == 0)     return "Enum type declaration";
    if (strcmp(word, "trait") == 0)    return "Trait declaration (interface with methods)";
    if (strcmp(word, "impl") == 0)     return "Implementation block for a type";
    if (strcmp(word, "use") == 0)      return "Import module or symbol";
    if (strcmp(word, "mod") == 0)      return "Module declaration";
    if (strcmp(word, "pub") == 0)      return "Public visibility modifier";
    if (strcmp(word, "priv") == 0)     return "Private visibility modifier";
    if (strcmp(word, "agent") == 0)    return "Agent declaration (autonomous unit)";
    if (strcmp(word, "guard") == 0)    return "Guard function (safety check)";
    if (strcmp(word, "capability") == 0) return "Capability set declaration";
    if (strcmp(word, "taint") == 0)    return "Taint tracking annotation";
    if (strcmp(word, "budget") == 0)   return "Budget constraint (token/cost limits)";
    if (strcmp(word, "tool") == 0)     return "Tool declaration (agent action)";
    if (strcmp(word, "skill") == 0)    return "Skill declaration (reusable behavior)";
    if (strcmp(word, "prompt") == 0)   return "Prompt template declaration";
    if (strcmp(word, "supervisor") == 0) return "Supervisor declaration (manages child agents)";
    if (strcmp(word, "mesh") == 0)     return "Mesh pipeline (multi-stage agent pipeline)";
    if (strcmp(word, "memory") == 0)   return "Memory store declaration";
    if (strcmp(word, "ask") == 0)      return "LLM inference call";
    if (strcmp(word, "tell") == 0)     return "Send message to agent";
    if (strcmp(word, "spawn") == 0)    return "Spawn concurrent task";
    if (strcmp(word, "await") == 0)    return "Await async result";
    if (strcmp(word, "select") == 0)   return "Select from multiple channels";
    if (strcmp(word, "chan") == 0)     return "Channel type (inter-agent communication)";
    if (strcmp(word, "defer") == 0)    return "Deferred execution (LIFO at scope exit)";
    if (strcmp(word, "secret") == 0)   return "Secret type (compile-time confidentiality)";
    if (strcmp(word, "ensure") == 0)   return "Runtime assertion with rollback";
    if (strcmp(word, "invariant") == 0) return "Invariant declaration (continuous monitoring)";
    return NULL;
}

/* Find the word under the cursor in source text */
static bool lsp_get_word_at(const char *source, size_t source_len,
                             long line, long col,
                             char *word_out, size_t word_bufsz) {
    if (!source) return false;
    word_out[0] = '\0';

    /* Find position in source */
    long cur_line = 0;
    size_t pos = 0;
    while (pos < source_len && cur_line < line) {
        if (source[pos] == '\n') cur_line++;
        pos++;
    }
    /* Now pos is at beginning of target line */
    pos += (size_t)col;
    if (pos >= source_len) return false;

    /* Find word boundaries */
    size_t start = pos;
    while (start > 0 && (isalnum((unsigned char)source[start - 1]) || source[start - 1] == '_')) {
        start--;
    }
    size_t end = pos;
    while (end < source_len && (isalnum((unsigned char)source[end]) || source[end] == '_')) {
        end++;
    }
    if (end <= start) return false;

    size_t wlen = end - start;
    if (wlen >= word_bufsz) wlen = word_bufsz - 1;
    memcpy(word_out, source + start, wlen);
    word_out[wlen] = '\0';
    return true;
}

/* Find a declaration by name in the AST */
static AstNode *lsp_find_decl(AstNode *program, const char *name) {
    if (!program || !name) return NULL;
    for (AstNode *d = program->params; d; d = d->next) {
        if (d->name && strcmp(d->name, name) == 0) return d;

        /* Search inside function bodies for let bindings */
        if (d->kind == AST_FN && d->left) {
            /* d->left is the body block */
            AstNode *body = d->left;
            if (body && body->kind == AST_BLOCK) {
                for (AstNode *s = body->params; s; s = s->next) {
                    if (s->kind == AST_LET && s->name && strcmp(s->name, name) == 0)
                        return s;
                }
            }
        }
    }
    return NULL;
}

static void lsp_handle_hover(long id, const char *params) {
    const char *td = json_find_object(params, "textDocument");
    const char *pos = json_find_object(params, "position");

    char uri[LSP_MAX_URI_LEN];
    if (td) lsp_json_get_string(td, "uri", uri, sizeof(uri));
    else uri[0] = '\0';

    long line = pos ? lsp_json_get_int(pos, "line") : 0;
    long col = pos ? lsp_json_get_int(pos, "character") : 0;

    LspDocument *doc = lsp_find_doc(uri);
    if (!doc || !doc->content) {
        lsp_send_null_result(id);
        return;
    }

    /* Get word at cursor */
    char word[512];
    if (!lsp_get_word_at(doc->content, doc->content_len, line, col, word, sizeof(word))) {
        lsp_send_null_result(id);
        return;
    }

    char hover_text[4096];
    hover_text[0] = '\0';

    /* Check if it's a keyword */
    const char *kw_desc = lsp_keyword_description(word);
    if (kw_desc) {
        snprintf(hover_text, sizeof(hover_text), "**%s** -- %s", word, kw_desc);
    }

    /* Check if it's a known declaration in the AST */
    if (hover_text[0] == '\0' && doc->ast) {
        AstNode *decl = lsp_find_decl(doc->ast, word);
        if (decl) {
            switch (decl->kind) {
            case AST_FN: {
                char sig[2048];
                lsp_build_fn_signature(decl, sig, sizeof(sig));
                snprintf(hover_text, sizeof(hover_text), "```limceron\\n%s\\n```", sig);
                break;
            }
            case AST_STRUCT:
                snprintf(hover_text, sizeof(hover_text),
                         "```limceron\\nstruct %s\\n```", decl->name);
                break;
            case AST_ENUM:
                snprintf(hover_text, sizeof(hover_text),
                         "```limceron\\nenum %s\\n```", decl->name);
                break;
            case AST_AGENT:
                snprintf(hover_text, sizeof(hover_text),
                         "```limceron\\nagent %s\\n```", decl->name);
                break;
            case AST_LET:
                if (decl->type_expr && decl->type_expr->kind == AST_TYPE_NAMED && decl->type_expr->name) {
                    snprintf(hover_text, sizeof(hover_text),
                             "```limceron\\nlet %s: %s\\n```",
                             decl->name, decl->type_expr->name);
                } else {
                    snprintf(hover_text, sizeof(hover_text),
                             "```limceron\\nlet %s\\n```", decl->name);
                }
                break;
            case AST_TOOL:
                snprintf(hover_text, sizeof(hover_text),
                         "```limceron\\ntool %s\\n```", decl->name);
                break;
            case AST_CAPABILITY:
                snprintf(hover_text, sizeof(hover_text),
                         "```limceron\\ncapability %s\\n```", decl->name);
                break;
            default:
                snprintf(hover_text, sizeof(hover_text), "%s `%s`",
                         ast_kind_name(decl->kind), decl->name);
                break;
            }
        }
    }

    /* Check if it's a builtin */
    if (hover_text[0] == '\0') {
        for (int i = 0; lsp_builtins[i]; i++) {
            if (strcmp(word, lsp_builtins[i]) == 0) {
                snprintf(hover_text, sizeof(hover_text),
                         "**%s** -- builtin function", word);
                break;
            }
        }
    }

    if (hover_text[0] == '\0') {
        lsp_send_null_result(id);
        return;
    }

    char esc[4096];
    json_escape_string(hover_text, esc, sizeof(esc));

    char result[8192];
    snprintf(result, sizeof(result),
             "{\"contents\":{\"kind\":\"markdown\",\"value\":\"%s\"}}", esc);
    lsp_send_result(id, result);
}

/* ============================================================
 * textDocument/definition
 * ============================================================ */

static void lsp_handle_definition(long id, const char *params) {
    const char *td = json_find_object(params, "textDocument");
    const char *pos = json_find_object(params, "position");

    char uri[LSP_MAX_URI_LEN];
    if (td) lsp_json_get_string(td, "uri", uri, sizeof(uri));
    else uri[0] = '\0';

    long line = pos ? lsp_json_get_int(pos, "line") : 0;
    long col = pos ? lsp_json_get_int(pos, "character") : 0;

    LspDocument *doc = lsp_find_doc(uri);
    if (!doc || !doc->content || !doc->ast) {
        lsp_send_null_result(id);
        return;
    }

    char word[512];
    if (!lsp_get_word_at(doc->content, doc->content_len, line, col, word, sizeof(word))) {
        lsp_send_null_result(id);
        return;
    }

    /* Find declaration */
    AstNode *decl = lsp_find_decl(doc->ast, word);
    if (!decl) {
        lsp_send_null_result(id);
        return;
    }

    uint32_t def_line = decl->loc.line > 0 ? decl->loc.line - 1 : 0;
    uint32_t def_col = decl->loc.column > 0 ? decl->loc.column - 1 : 0;

    char result[2048];
    snprintf(result, sizeof(result),
             "{\"uri\":\"%s\",\"range\":{\"start\":{\"line\":%u,\"character\":%u},"
             "\"end\":{\"line\":%u,\"character\":%u}}}",
             uri, def_line, def_col, def_line, def_col + (uint32_t)strlen(word));
    lsp_send_result(id, result);
}

/* ============================================================
 * Main LSP event loop
 * ============================================================ */

/* L10 additions appended below */

/* Host module + enum variant tables for completion. */
typedef struct { const char *module; const char **members; } LspHostModule;
static const char *lsp_host_llm_m[]    = { "classify", "complete", "embed", "rerank", NULL };
static const char *lsp_host_json_m[]   = { "parse", "get", "set", "array_len", "array_get", "stringify", NULL };
static const char *lsp_host_http_m[]   = { "fetch", "post", "get", NULL };
static const char *lsp_host_mcp_m[]    = { "call", "list_tools", NULL };
static const char *lsp_host_log_m[]    = { "info", "warn", "error", "debug", NULL };
static const char *lsp_host_time_m[]   = { "now", "sleep", "format", "since", NULL };
static const char *lsp_host_math_m[]   = { "abs", "min", "max", "sqrt", "pow", "floor", "ceil", "clamp", NULL };
static const char *lsp_host_string_m[] = { "len", "upper", "lower", "contains", "starts_with",
                                            "ends_with", "split", "trim", "replace", "find", NULL };
static const LspHostModule lsp_host_modules_tbl[] = {
    { "llm", lsp_host_llm_m }, { "json", lsp_host_json_m }, { "http", lsp_host_http_m },
    { "mcp", lsp_host_mcp_m }, { "log", lsp_host_log_m }, { "time", lsp_host_time_m },
    { "math", lsp_host_math_m }, { "string", lsp_host_string_m }, { NULL, NULL },
};

typedef struct { const char *type_name; const char **variants; } LspEnumVariants;
static const char *lsp_var_host_err_v[] = {
    "BudgetExceeded", "EntropyExceeded", "InvalidArgument", "RuntimeError",
    "Timeout", "NetworkError", "PermissionDenied", "Unavailable", NULL,
};
static const char *lsp_var_result_v[] = { "Ok", "Err", NULL };
static const char *lsp_var_option_v[] = { "Some", "None", NULL };
static const LspEnumVariants lsp_enum_table_tbl[] = {
    { "host-error", lsp_var_host_err_v }, { "HostError", lsp_var_host_err_v },
    { "Result", lsp_var_result_v }, { "Option", lsp_var_option_v }, { NULL, NULL },
};

int lsp_test_diagnostics_for_source(const char *uri, const char *source,
                                     char *out, size_t out_sz) {
    if (!source || !out || out_sz == 0) return 0;
    out[0] = '\0';
    LspDocument doc; memset(&doc, 0, sizeof(doc));
    strncpy(doc.uri, uri ? uri : "file:///test.lceron", LSP_MAX_URI_LEN - 1);
    doc.arena        = arena_new(2 * 1024 * 1024);
    doc.intern_arena = arena_new(512 * 1024);
    doc.content_len  = strlen(source);
    doc.content      = (char *)malloc(doc.content_len + 1);
    memcpy(doc.content, source, doc.content_len + 1);
    const char *filename = uri_to_filename(doc.uri);
    ErrorReporter reporter = reporter_new(filename, doc.content, doc.content_len);
    StringIntern intern = intern_new(&doc.intern_arena);
    Lexer lexer = lexer_new(filename, doc.content, doc.content_len, &intern, &reporter);
    Parser parser = parser_new(&lexer, &doc.arena, &reporter);
    doc.ast = parse_program(&parser);
    if (doc.ast && !parser.had_error) typecheck_program(doc.ast, &reporter, &doc.arena);
    int off = 0;
    char esc[2048];
    off += snprintf(out + off, out_sz - (size_t)off,
                    "{\"uri\":\"%s\",\"diagnostics\":[", doc.uri);
    for (int i = 0; i < reporter.count && (size_t)off < out_sz - 512; i++) {
        CompileError *e = &reporter.errors[i];
        uint32_t line = e->loc.line > 0 ? e->loc.line - 1 : 0;
        uint32_t col  = e->loc.column > 0 ? e->loc.column - 1 : 0;
        int severity  = e->is_warning ? 2 : 1;
        char *p = esc; const char *s = e->message ? e->message : "";
        size_t k = 0;
        while (*s && k < sizeof(esc) - 6) {
            if (*s == '"') { p[k++]='\\'; p[k++]='"'; }
            else if (*s == '\\') { p[k++]='\\'; p[k++]='\\'; }
            else if (*s == '\n') { p[k++]='\\'; p[k++]='n'; }
            else p[k++] = *s;
            s++;
        }
        p[k] = '\0';
        if (i > 0) out[off++] = ',';
        off += snprintf(out + off, out_sz - (size_t)off,
                        "{\"range\":{\"start\":{\"line\":%u,\"character\":%u},"
                        "\"end\":{\"line\":%u,\"character\":%u}},"
                        "\"severity\":%d,\"code\":\"LCN-%03d\","
                        "\"source\":\"limceron\",\"message\":\"%s\"}",
                        line, col, line,
                        col + (e->underline_len > 0 ? e->underline_len : 1),
                        severity, i + 1, esc);
    }
    off += snprintf(out + off, out_sz - (size_t)off, "]}");
    free(doc.content); arena_free(&doc.arena); arena_free(&doc.intern_arena);
    return off;
}

int lsp_test_hover_for_source(const char *source, long line, long col,
                               char *out, size_t out_sz) {
    if (!source || !out || out_sz < 4) return 0;
    out[0] = '\0';
    LspDocument doc; memset(&doc, 0, sizeof(doc));
    strncpy(doc.uri, "file:///test.lceron", LSP_MAX_URI_LEN - 1);
    doc.arena        = arena_new(2 * 1024 * 1024);
    doc.intern_arena = arena_new(512 * 1024);
    doc.content_len  = strlen(source);
    doc.content      = (char *)malloc(doc.content_len + 1);
    memcpy(doc.content, source, doc.content_len + 1);
    ErrorReporter reporter = reporter_new("test.lceron", doc.content, doc.content_len);
    StringIntern intern = intern_new(&doc.intern_arena);
    Lexer lexer = lexer_new("test.lceron", doc.content, doc.content_len, &intern, &reporter);
    Parser parser = parser_new(&lexer, &doc.arena, &reporter);
    doc.ast = parse_program(&parser);
    char word[512];
    int produced = 0;
    if (lsp_get_word_at(doc.content, doc.content_len, line, col, word, sizeof(word))) {
        const char *kw = lsp_keyword_description(word);
        if (kw) {
            snprintf(out, out_sz, "**%s** -- %s", word, kw);
            produced = 1;
        }
        if (!produced && doc.ast) {
            AstNode *decl = lsp_find_decl(doc.ast, word);
            if (decl) {
                if (decl->kind == AST_FN) {
                    char sig[2048]; lsp_build_fn_signature(decl, sig, sizeof(sig));
                    snprintf(out, out_sz, "```limceron\n%s\n```", sig);
                } else if (decl->kind == AST_LET) {
                    if (decl->type_expr && decl->type_expr->kind == AST_TYPE_NAMED && decl->type_expr->name) {
                        snprintf(out, out_sz, "```limceron\nlet %s: %s\n```",
                                 decl->name, decl->type_expr->name);
                    } else {
                        snprintf(out, out_sz,
                                 "```limceron\nlet %s\n```\ntype info pending L9 close",
                                 decl->name);
                    }
                } else if (decl->name) {
                    snprintf(out, out_sz, "%s `%s`", ast_kind_name(decl->kind), decl->name);
                }
                produced = out[0] != '\0';
            }
        }
        if (!produced) {
            for (int i = 0; lsp_builtins[i]; i++) {
                if (strcmp(word, lsp_builtins[i]) == 0) {
                    snprintf(out, out_sz, "**%s** -- builtin function", word);
                    produced = 1;
                    break;
                }
            }
        }
    }
    free(doc.content); arena_free(&doc.arena); arena_free(&doc.intern_arena);
    return produced;
}

int lsp_test_completion_for_source(const char *source, long line, long character,
                                    char *out, size_t out_sz) {
    if (!source || !out || out_sz < 2) return 0;
    out[0] = '\0';
    LspDocument doc; memset(&doc, 0, sizeof(doc));
    strncpy(doc.uri, "file:///test.lceron", LSP_MAX_URI_LEN - 1);
    doc.arena        = arena_new(2 * 1024 * 1024);
    doc.intern_arena = arena_new(512 * 1024);
    doc.content_len  = strlen(source);
    doc.content      = (char *)malloc(doc.content_len + 1);
    memcpy(doc.content, source, doc.content_len + 1);
    ErrorReporter reporter = reporter_new("test.lceron", doc.content, doc.content_len);
    StringIntern intern = intern_new(&doc.intern_arena);
    Lexer lexer = lexer_new("test.lceron", doc.content, doc.content_len, &intern, &reporter);
    Parser parser = parser_new(&lexer, &doc.arena, &reporter);
    doc.ast = parse_program(&parser);
    int off = 0;
    int is_member = 0, is_enum_var = 0;
    char recv[128]; recv[0] = '\0';
    if (doc.content && character > 0) {
        const char *src2 = doc.content;
        long cl = 0, cc = 0;
        const char *p = src2;
        while (*p && (cl < line || (cl == line && cc < character - 1))) {
            if (*p == '\n') { cl++; cc = 0; } else { cc++; }
            p++;
        }
        if (*p == '.') is_member = 1;
        else if (*p == ':' && p > src2 && *(p - 1) == ':') is_enum_var = 1;
        const char *q = p;
        if (is_enum_var && q > src2 && *(q - 1) == ':') q--;
        if (q > src2 && (*q == '.' || *q == ':')) q--;
        const char *end = q + 1;
        while (q > src2 && (isalnum((unsigned char)*q) || *q == '_' || *q == '-')) q--;
        if (!isalnum((unsigned char)*q) && *q != '_' && *q != '-') q++;
        size_t rlen = (size_t)(end - q);
        if (rlen > 0 && rlen < sizeof(recv)) { memcpy(recv, q, rlen); recv[rlen] = '\0'; }
    }
    int handled = 0;
    if (is_enum_var && recv[0]) {
        for (int i = 0; lsp_enum_table_tbl[i].type_name; i++) {
            if (strcmp(recv, lsp_enum_table_tbl[i].type_name) != 0) continue;
            const char **vars = lsp_enum_table_tbl[i].variants;
            for (int j = 0; vars[j]; j++) {
                if (off > 0) out[off++] = ',';
                off += snprintf(out + off, out_sz - (size_t)off,
                                "{\"label\":\"%s\",\"kind\":20,\"detail\":\"variant\"}", vars[j]);
                if ((size_t)off >= out_sz - 256) break;
            }
            handled = 1; break;
        }
    }
    if (!handled && is_member && recv[0]) {
        for (int i = 0; lsp_host_modules_tbl[i].module; i++) {
            if (strcmp(recv, lsp_host_modules_tbl[i].module) != 0) continue;
            const char **mems = lsp_host_modules_tbl[i].members;
            for (int j = 0; mems[j]; j++) {
                if (off > 0) out[off++] = ',';
                off += snprintf(out + off, out_sz - (size_t)off,
                                "{\"label\":\"%s\",\"kind\":3,\"detail\":\"host:%s\"}",
                                mems[j], recv);
                if ((size_t)off >= out_sz - 256) break;
            }
            handled = 1; break;
        }
    }
    if (!handled && !is_member) {
        for (int i = 0; lsp_keywords[i]; i++) {
            if (off > 0) out[off++] = ',';
            off += snprintf(out + off, out_sz - (size_t)off,
                            "{\"label\":\"%s\",\"kind\":14,\"detail\":\"keyword\"}",
                            lsp_keywords[i]);
            if ((size_t)off >= out_sz - 256) break;
        }
        for (int i = 0; lsp_builtins[i] && (size_t)off < out_sz - 256; i++) {
            if (off > 0) out[off++] = ',';
            off += snprintf(out + off, out_sz - (size_t)off,
                            "{\"label\":\"%s\",\"kind\":3,\"detail\":\"builtin\"}",
                            lsp_builtins[i]);
        }
        if (doc.ast) {
            off = lsp_collect_scope_identifiers(doc.ast, (uint32_t)(line + 1),
                                                out, out_sz, off);
        }
    }
    free(doc.content); arena_free(&doc.arena); arena_free(&doc.intern_arena);
    return off;
}
int cmd_lsp(void) {
    /* LSP communicates over stdin/stdout. Log to stderr. */
    fprintf(stderr, "limceron-lsp: starting (version %s)\n", LCN_VERSION);

    bool running = true;
    bool shutdown_received = false;

    while (running) {
        char *msg = lsp_read_message();
        if (!msg) {
            /* EOF — editor closed the connection */
            break;
        }

        /* Extract method and id */
        char method[256];
        if (!lsp_json_get_string(msg, "method", method, sizeof(method))) {
            free(msg);
            continue;
        }

        long id = lsp_json_get_int(msg, "id"); /* -1 for notifications */

        const char *params = json_find_params(msg);

        /* Dispatch */
        if (strcmp(method, "initialize") == 0) {
            lsp_handle_initialize(id);
        }
        else if (strcmp(method, "initialized") == 0) {
            /* No-op notification */
        }
        else if (strcmp(method, "shutdown") == 0) {
            shutdown_received = true;
            lsp_send_null_result(id);
        }
        else if (strcmp(method, "exit") == 0) {
            running = false;
        }
        else if (strcmp(method, "textDocument/didOpen") == 0) {
            if (params) lsp_handle_did_open(params);
        }
        else if (strcmp(method, "textDocument/didChange") == 0) {
            if (params) lsp_handle_did_change(params);
        }
        else if (strcmp(method, "textDocument/didSave") == 0) {
            if (params) lsp_handle_did_save(params);
        }
        else if (strcmp(method, "textDocument/didClose") == 0) {
            if (params) lsp_handle_did_close(params);
        }
        else if (strcmp(method, "textDocument/completion") == 0) {
            lsp_handle_completion(id, params ? params : "{}");
        }
        else if (strcmp(method, "textDocument/hover") == 0) {
            lsp_handle_hover(id, params ? params : "{}");
        }
        else if (strcmp(method, "textDocument/definition") == 0) {
            lsp_handle_definition(id, params ? params : "{}");
        }
        else {
            /* Unknown method — if it has an id, respond with MethodNotFound */
            if (id >= 0) {
                lsp_send_error(id, -32601, "Method not found");
            }
        }

        free(msg);
    }

    /* Clean up all open documents */
    for (int i = 0; i < lsp_doc_count; i++) {
        arena_free(&lsp_docs[i].arena);
        arena_free(&lsp_docs[i].intern_arena);
        free(lsp_docs[i].content);
    }
    lsp_doc_count = 0;

    fprintf(stderr, "limceron-lsp: shutdown (clean=%s)\n",
            shutdown_received ? "yes" : "no");
    return shutdown_received ? 0 : 1;
}
