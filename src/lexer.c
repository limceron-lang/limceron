/*
 * Limceron Compiler — Lexer
 *
 * Hand-written lexer with:
 * - UTF-8 awareness
 * - String interning
 * - Automatic semicolon insertion (Go-style)
 * - Nested block comment support
 * - Excellent error messages
 */

#include "lcn.h"

/* ============================================================
 * String Interning
 * ============================================================ */

StringIntern intern_new(Arena *arena) {
    StringIntern si;
    memset(&si, 0, sizeof(si));
    si.arena = arena;
    return si;
}

static uint32_t intern_hash(const char *s, size_t len) {
    uint32_t h = 5381;
    size_t i;
    for (i = 0; i < len; i++) {
        h = ((h << 5) + h) + (uint8_t)s[i];
    }
    return h;
}

const char *intern_get(StringIntern *si, const char *s, size_t len) {
    uint32_t idx = intern_hash(s, len) % STRING_INTERN_CAP;
    size_t i;

    for (i = 0; i < STRING_INTERN_CAP; i++) {
        size_t slot = (idx + i) % STRING_INTERN_CAP;
        if (si->strings[slot] == NULL) {
            char *copy = arena_strndup(si->arena, s, len);
            si->strings[slot] = copy;
            si->count++;
            return copy;
        }
        if (strlen(si->strings[slot]) == len &&
            memcmp(si->strings[slot], s, len) == 0) {
            return si->strings[slot];
        }
    }

    fprintf(stderr, "fatal: string intern table full (%zu entries)\n", si->count);
    exit(1);
}

/* ============================================================
 * Error Reporting
 * ============================================================ */

ErrorReporter reporter_new(const char *filename, const char *source, size_t len) {
    ErrorReporter r;
    memset(&r, 0, sizeof(r));
    r.source = source;
    r.source_len = len;
    r.filename = filename;
    return r;
}

/* Compute the width (in digits) of a number for line number padding */
static int digit_width(uint32_t n) {
    int w = 1;
    while (n >= 10) { n /= 10; w++; }
    return w;
}

/* Find the start and end offsets of the source line containing `offset` */
static void find_source_line(const char *source, size_t source_len,
                              uint32_t offset,
                              uint32_t *out_start, uint32_t *out_end) {
    uint32_t ls = offset;
    while (ls > 0 && source[ls - 1] != '\n') ls--;
    uint32_t le = offset;
    while (le < source_len && source[le] != '\n') le++;
    *out_start = ls;
    *out_end   = le;
}

/* Format a diagnostic message into a buffer (no ANSI colors).
 * Output format:
 *   error: undefined variable 'foo'
 *    --> file.lceron:12:5
 *     |
 *  12 |     let x = foo + 1
 *     |             ^^^ not found in this scope
 *     |
 */
int format_diagnostic(char *buf, size_t bufsize,
                      const char *source, size_t source_len,
                      const char *filename,
                      SourceLoc loc, const char *message,
                      const char *hint, bool is_warning,
                      uint32_t underline_len) {
    char tmp[2048];
    int  pos = 0;
    int  cap = (int)sizeof(tmp) - 1;
    const char *label = is_warning ? "warning" : "error";
    const char *fn = filename ? filename :
                     (loc.filename ? loc.filename : "<unknown>");

    /* Line 1: level: message */
    pos += snprintf(tmp + pos, (size_t)(cap - pos),
                    "%s: %s\n", label, message);

    /* Line 2:  --> file:line:col */
    pos += snprintf(tmp + pos, (size_t)(cap - pos),
                    " --> %s:%u:%u\n", fn, loc.line, loc.column);

    /* Source snippet */
    if (source && loc.offset < source_len) {
        uint32_t line_start, line_end;
        find_source_line(source, source_len, loc.offset,
                         &line_start, &line_end);

        int lw = digit_width(loc.line);
        uint32_t ulen = underline_len > 0 ? underline_len : 1;
        uint32_t col;

        /* Empty separator line */
        {
            int pad;
            for (pad = 0; pad < lw + 1; pad++)
                pos += snprintf(tmp + pos, (size_t)(cap - pos), " ");
        }
        pos += snprintf(tmp + pos, (size_t)(cap - pos), "|\n");

        /* Source line with line number */
        pos += snprintf(tmp + pos, (size_t)(cap - pos),
                        " %u | %.*s\n",
                        loc.line,
                        (int)(line_end - line_start),
                        source + line_start);

        /* Caret/underline line */
        {
            int pad;
            for (pad = 0; pad < lw + 1; pad++)
                pos += snprintf(tmp + pos, (size_t)(cap - pos), " ");
        }
        pos += snprintf(tmp + pos, (size_t)(cap - pos), "| ");

        for (col = 0; col < loc.column - 1 && col < 200; col++)
            pos += snprintf(tmp + pos, (size_t)(cap - pos), " ");

        {
            uint32_t ci;
            for (ci = 0; ci < ulen && ci < 200; ci++)
                pos += snprintf(tmp + pos, (size_t)(cap - pos), "^");
        }

        if (hint) {
            pos += snprintf(tmp + pos, (size_t)(cap - pos), " %s", hint);
        }
        pos += snprintf(tmp + pos, (size_t)(cap - pos), "\n");

        /* Closing separator */
        {
            int pad;
            for (pad = 0; pad < lw + 1; pad++)
                pos += snprintf(tmp + pos, (size_t)(cap - pos), " ");
        }
        pos += snprintf(tmp + pos, (size_t)(cap - pos), "|\n");
    } else if (hint) {
        /* No source available, still show hint */
        pos += snprintf(tmp + pos, (size_t)(cap - pos),
                        " = help: %s\n", hint);
    }

    tmp[pos] = '\0';

    if (buf && bufsize > 0) {
        size_t copy = ((size_t)pos < bufsize - 1) ? (size_t)pos : bufsize - 1;
        memcpy(buf, tmp, copy);
        buf[copy] = '\0';
    }
    return pos;
}

/* Internal: emit a diagnostic (shared by error and warning paths) */
static void emit_diagnostic(ErrorReporter *r, SourceLoc loc,
                             const char *message, const char *hint,
                             bool is_warning, uint32_t underline_len) {
    if (r->count >= MAX_ERRORS) return;

    CompileError *e = &r->errors[r->count++];
    e->loc = loc;
    e->message = message;
    e->hint = hint;
    e->is_warning = is_warning;
    e->underline_len = underline_len;

    /* Color codes */
    const char *color = is_warning ? "\033[1;33m" : "\033[1;31m";
    const char *reset = "\033[0m";
    const char *blue  = "\033[1;34m";
    const char *label = is_warning ? "warning" : "error";

    fprintf(stderr, "%s%s%s: %s\n", color, label, reset, message);
    fprintf(stderr, "  %s-->%s %s:%u:%u\n",
            blue, reset,
            loc.filename ? loc.filename : "<unknown>",
            loc.line, loc.column);

    if (r->source && loc.offset < r->source_len) {
        uint32_t line_start, line_end;
        find_source_line(r->source, r->source_len, loc.offset,
                         &line_start, &line_end);

        int lw = digit_width(loc.line);
        uint32_t ulen = underline_len > 0 ? underline_len : 1;

        /* Empty separator */
        fprintf(stderr, " %*s %s|%s\n", lw, "", blue, reset);

        /* Source line */
        fprintf(stderr, " %s%u%s %s|%s %.*s\n",
                blue, loc.line, reset,
                blue, reset,
                (int)(line_end - line_start), r->source + line_start);

        /* Caret/underline line */
        fprintf(stderr, " %*s %s|%s ", lw, "", blue, reset);
        {
            uint32_t col;
            for (col = 0; col < loc.column - 1; col++)
                fputc(' ', stderr);
        }
        {
            uint32_t ci;
            for (ci = 0; ci < ulen; ci++)
                fprintf(stderr, "%s^%s", color, reset);
        }
        if (hint) {
            fprintf(stderr, " %s", hint);
        }
        fprintf(stderr, "\n");

        /* Closing separator */
        fprintf(stderr, " %*s %s|%s\n", lw, "", blue, reset);
    } else if (hint) {
        fprintf(stderr, "   %s= help%s: %s\n", blue, reset, hint);
    }

    fprintf(stderr, "\n");
}

void report_error(ErrorReporter *r, SourceLoc loc,
                  const char *message, const char *hint) {
    emit_diagnostic(r, loc, message, hint, false, 1);
}

void report_error_fmt(ErrorReporter *r, SourceLoc loc,
                      const char *hint, const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    emit_diagnostic(r, loc, buf, hint, false, 1);
}

void report_warning(ErrorReporter *r, SourceLoc loc,
                    const char *message, const char *hint) {
    emit_diagnostic(r, loc, message, hint, true, 1);
}

void report_warning_fmt(ErrorReporter *r, SourceLoc loc,
                        const char *hint, const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    emit_diagnostic(r, loc, buf, hint, true, 1);
}

bool has_errors(const ErrorReporter *r) {
    return r->count > 0;
}

/* ============================================================
 * Token utilities
 * ============================================================ */

const char *token_kind_name(TokenKind kind) {
    switch (kind) {
    case TOK_INT_LIT:     return "integer";
    case TOK_FLOAT_LIT:   return "float";
    case TOK_STRING_LIT:  return "string";
    case TOK_CHAR_LIT:    return "char";
    case TOK_IDENT:       return "identifier";
    case TOK_LET:         return "let";
    case TOK_MUT:         return "mut";
    case TOK_FN:          return "fn";
    case TOK_RETURN:      return "return";
    case TOK_IF:          return "if";
    case TOK_ELSE:        return "else";
    case TOK_MATCH:       return "match";
    case TOK_FOR:         return "for";
    case TOK_WHILE:       return "while";
    case TOK_LOOP:        return "loop";
    case TOK_BREAK:       return "break";
    case TOK_CONTINUE:    return "continue";
    case TOK_STRUCT:      return "struct";
    case TOK_ENUM:        return "enum";
    case TOK_TRAIT:       return "trait";
    case TOK_IMPL:        return "impl";
    case TOK_INTERFACE:   return "interface";
    case TOK_MOD:         return "mod";
    case TOK_USE:         return "use";
    case TOK_PUB:         return "pub";
    case TOK_PRIV:        return "priv";
    case TOK_SPAWN:       return "spawn";
    case TOK_AWAIT:       return "await";
    case TOK_SELECT:      return "select";
    case TOK_CHAN:         return "chan";
    case TOK_DEFER:       return "defer";
    case TOK_UNSAFE:      return "unsafe";
    case TOK_COMPTIME:    return "comptime";
    case TOK_TYPE:        return "type";
    case TOK_AS:          return "as";
    case TOK_IN:          return "in";
    case TOK_IS:          return "is";
    case TOK_TRUE:        return "true";
    case TOK_FALSE:       return "false";
    case TOK_NONE:        return "none";
    case TOK_CONST:       return "const";
    /* Agent System Keywords */
    case TOK_AGENT:       return "agent";
    case TOK_GUARD:       return "guard";
    case TOK_CAPABILITY:  return "capability";
    case TOK_TAINT:       return "taint";
    case TOK_BUDGET:      return "budget";
    case TOK_TOOL:        return "tool";
    case TOK_SKILL:       return "skill";
    case TOK_PROMPT:      return "prompt";
    case TOK_SUPERVISOR:  return "supervisor";
    case TOK_MESH:        return "mesh";
    case TOK_MEMORY:      return "memory";
    case TOK_ASK:         return "ask";
    case TOK_TELL:        return "tell";
    case TOK_ENSURE:      return "ensure";
    case TOK_INVARIANT:   return "invariant";
    case TOK_GUARDSET:    return "guardset";
    case TOK_REQUIRES:    return "requires";
    case TOK_OTHERWISE:   return "otherwise";
    case TOK_SHOWING:     return "showing";
    case TOK_TIMEOUT:     return "timeout";
    case TOK_CHOICES:     return "choices";
    case TOK_REPEAT:      return "repeat";
    case TOK_TIMES:       return "times";
    case TOK_WAIT:        return "wait";
    case TOK_UNTIL:       return "until";
    case TOK_EACH:        return "each";
    case TOK_KEEP:        return "keep";
    case TOK_WHERE:       return "where";
    case TOK_SECRET:      return "secret";
    case TOK_ABOUT:       return "about";
    case TOK_CHANNEL:     return "channel";
    case TOK_ROUTER:      return "router";
    case TOK_ROUTE:       return "route";
    case TOK_STRATEGY:    return "strategy";
    /* Progress Reporting */
    case TOK_PROGRESS:    return "progress";
    /* Access Control Keywords */
    case TOK_ALLOW:       return "allow";
    case TOK_DENY:        return "deny";
    case TOK_AT:          return "@";
    case TOK_PLUS:        return "+";
    case TOK_MINUS:       return "-";
    case TOK_STAR:        return "*";
    case TOK_SLASH:       return "/";
    case TOK_PERCENT:     return "%";
    case TOK_POWER:       return "**";
    case TOK_AMP:         return "&";
    case TOK_PIPE:        return "|";
    case TOK_CARET:       return "^";
    case TOK_TILDE:       return "~";
    case TOK_SHL:         return "<<";
    case TOK_SHR:         return ">>";
    case TOK_EQ_EQ:       return "==";
    case TOK_NOT_EQ:      return "!=";
    case TOK_LT:          return "<";
    case TOK_GT:          return ">";
    case TOK_LT_EQ:       return "<=";
    case TOK_GT_EQ:       return ">=";
    case TOK_AND_AND:     return "&&";
    case TOK_PIPE_PIPE:   return "||";
    case TOK_BANG:        return "!";
    case TOK_EQ:          return "=";
    case TOK_PLUS_EQ:     return "+=";
    case TOK_MINUS_EQ:    return "-=";
    case TOK_STAR_EQ:     return "*=";
    case TOK_SLASH_EQ:    return "/=";
    case TOK_PERCENT_EQ:  return "%=";
    case TOK_AMP_EQ:      return "&=";
    case TOK_PIPE_EQ:     return "|=";
    case TOK_CARET_EQ:    return "^=";
    case TOK_SHL_EQ:      return "<<=";
    case TOK_SHR_EQ:      return ">>=";
    case TOK_QUESTION:    return "?";
    case TOK_QUESTION_DOT:return "?.";
    case TOK_DOT_DOT:     return "..";
    case TOK_DOT_DOT_EQ:  return "..=";
    case TOK_ARROW:       return "->";
    case TOK_FAT_ARROW:   return "=>";
    case TOK_COLON_COLON: return "::";
    case TOK_PIPE_GT:     return "|>";
    case TOK_LPAREN:      return "(";
    case TOK_RPAREN:      return ")";
    case TOK_LBRACKET:    return "[";
    case TOK_RBRACKET:    return "]";
    case TOK_LBRACE:      return "{";
    case TOK_RBRACE:      return "}";
    case TOK_COMMA:       return ",";
    case TOK_DOT:         return ".";
    case TOK_COLON:       return ":";
    case TOK_SEMICOLON:   return ";";
    case TOK_HASH:        return "#";
    case TOK_NEWLINE:     return "newline";
    case TOK_EOF:         return "end of file";
    case TOK_ERROR:       return "error";
    default:              return "unknown";
    }
}

bool token_ends_stmt(TokenKind kind) {
    switch (kind) {
    case TOK_IDENT:
    case TOK_INT_LIT:
    case TOK_FLOAT_LIT:
    case TOK_STRING_LIT:
    case TOK_CHAR_LIT:
    case TOK_RETURN:
    case TOK_BREAK:
    case TOK_CONTINUE:
    case TOK_TRUE:
    case TOK_FALSE:
    case TOK_NONE:
    case TOK_RPAREN:
    case TOK_RBRACKET:
    case TOK_RBRACE:
    case TOK_QUESTION:
        return true;
    default:
        return false;
    }
}

/* ============================================================
 * Lexer Implementation
 * ============================================================ */

Lexer lexer_new(const char *filename, const char *source, size_t len,
                StringIntern *intern, ErrorReporter *reporter) {
    Lexer l;
    memset(&l, 0, sizeof(l));
    l.source = source;
    l.source_len = len;
    l.pos = 0;
    l.line = 1;
    l.column = 1;
    l.filename = filename;
    l.intern = intern;
    l.reporter = reporter;
    l.at_line_start = true;
    return l;
}

static char peek(const Lexer *l) {
    if (l->pos >= l->source_len) return '\0';
    return l->source[l->pos];
}

static char peek_next(const Lexer *l) {
    if (l->pos + 1 >= l->source_len) return '\0';
    return l->source[l->pos + 1];
}

static char advance(Lexer *l) {
    if (l->pos >= l->source_len) return '\0';
    char c = l->source[l->pos++];
    if (c == '\n') {
        l->line++;
        l->column = 1;
    } else {
        l->column++;
    }
    return c;
}

static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_alnum(char c) {
    return is_alpha(c) || is_digit(c);
}

static bool is_hex(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static void skip_whitespace_no_newline(Lexer *l) {
    while (l->pos < l->source_len) {
        char c = peek(l);
        if (c == ' ' || c == '\t' || c == '\r') {
            advance(l);
        } else if (c == '/' && peek_next(l) == '/') {
            while (l->pos < l->source_len && peek(l) != '\n') {
                advance(l);
            }
        } else if (c == '/' && peek_next(l) == '*') {
            advance(l); advance(l);
            int depth = 1;
            while (l->pos < l->source_len && depth > 0) {
                if (peek(l) == '/' && peek_next(l) == '*') {
                    advance(l); advance(l); depth++;
                } else if (peek(l) == '*' && peek_next(l) == '/') {
                    advance(l); advance(l); depth--;
                } else {
                    advance(l);
                }
            }
        } else {
            break;
        }
    }
}

static TokenKind keyword_lookup(const char *s, size_t len) {
    /* Sorted by frequency for faster average lookup */
    static const struct { const char *kw; TokenKind kind; } keywords[] = {
        {"let",       TOK_LET},
        {"fn",        TOK_FN},
        {"if",        TOK_IF},
        {"else",      TOK_ELSE},
        {"return",    TOK_RETURN},
        {"for",       TOK_FOR},
        {"in",        TOK_IN},
        {"mut",       TOK_MUT},
        {"struct",    TOK_STRUCT},
        {"enum",      TOK_ENUM},
        {"match",     TOK_MATCH},
        {"while",     TOK_WHILE},
        {"loop",      TOK_LOOP},
        {"break",     TOK_BREAK},
        {"continue",  TOK_CONTINUE},
        {"trait",     TOK_TRAIT},
        {"impl",      TOK_IMPL},
        {"interface", TOK_INTERFACE},
        {"mod",       TOK_MOD},
        {"use",       TOK_USE},
        {"pub",       TOK_PUB},
        {"priv",      TOK_PRIV},
        {"spawn",     TOK_SPAWN},
        {"await",     TOK_AWAIT},
        {"select",    TOK_SELECT},
        {"chan",       TOK_CHAN},
        {"defer",     TOK_DEFER},
        {"unsafe",    TOK_UNSAFE},
        {"comptime",  TOK_COMPTIME},
        {"type",      TOK_TYPE},
        {"as",        TOK_AS},
        {"is",        TOK_IS},
        {"true",      TOK_TRUE},
        {"false",     TOK_FALSE},
        {"none",      TOK_NONE},
        {"const",     TOK_CONST},
        /* Agent System Keywords */
        {"agent",      TOK_AGENT},
        {"guard",      TOK_GUARD},
        {"capability", TOK_CAPABILITY},
        {"taint",      TOK_TAINT},
        {"budget",     TOK_BUDGET},
        {"tool",       TOK_TOOL},
        {"skill",      TOK_SKILL},
        {"prompt",     TOK_PROMPT},
        {"supervisor", TOK_SUPERVISOR},
        {"mesh",       TOK_MESH},
        {"memory",     TOK_MEMORY},
        {"ask",        TOK_ASK},
        {"tell",       TOK_TELL},
        {"ensure",     TOK_ENSURE},
        {"invariant",  TOK_INVARIANT},
        {"guardset",   TOK_GUARDSET},
        {"requires",   TOK_REQUIRES},
        {"otherwise",  TOK_OTHERWISE},
        {"showing",    TOK_SHOWING},
        {"timeout",    TOK_TIMEOUT},
        {"choices",    TOK_CHOICES},
        {"repeat",     TOK_REPEAT},
        {"times",      TOK_TIMES},
        {"wait",       TOK_WAIT},
        {"until",      TOK_UNTIL},
        {"each",       TOK_EACH},
        {"keep",       TOK_KEEP},
        {"where",      TOK_WHERE},
        {"secret",     TOK_SECRET},
        {"about",      TOK_ABOUT},
        {"channel",    TOK_CHANNEL},
        {"router",     TOK_ROUTER},
        {"route",      TOK_ROUTE},
        {"strategy",   TOK_STRATEGY},
        /* Progress Reporting */
        {"progress",   TOK_PROGRESS},
        /* Access Control Keywords */
        {"allow",      TOK_ALLOW},
        {"deny",       TOK_DENY},
        {NULL, 0},
    };

    int i;
    for (i = 0; keywords[i].kw; i++) {
        if (strlen(keywords[i].kw) == len &&
            memcmp(keywords[i].kw, s, len) == 0) {
            return keywords[i].kind;
        }
    }
    return TOK_IDENT;
}

Token lexer_next_raw(Lexer *l) {
    Token tok;
    char c;

    skip_whitespace_no_newline(l);

    memset(&tok, 0, sizeof(tok));
    tok.loc.filename = l->filename;
    tok.loc.line = l->line;
    tok.loc.column = l->column;
    tok.loc.offset = (uint32_t)l->pos;

    if (l->pos >= l->source_len) {
        tok.kind = TOK_EOF;
        return tok;
    }

    c = peek(l);

    /* Newline */
    if (c == '\n') {
        advance(l);
        tok.kind = TOK_NEWLINE;
        tok.len = 1;
        return tok;
    }

    /* Identifiers and keywords */
    if (is_alpha(c)) {
        size_t start = l->pos;
        while (l->pos < l->source_len && is_alnum(peek(l))) {
            advance(l);
        }
        size_t len = l->pos - start;
        tok.kind = keyword_lookup(l->source + start, len);
        tok.value.str_val = intern_get(l->intern, l->source + start, len);
        tok.len = (uint32_t)len;
        return tok;
    }

    /* Numbers */
    if (is_digit(c)) {
        size_t start = l->pos;
        bool is_float = false;

        if (c == '0' && l->pos + 1 < l->source_len) {
            char next = peek_next(l);
            if (next == 'x' || next == 'X') {
                advance(l); advance(l);
                while (l->pos < l->source_len && (is_hex(peek(l)) || peek(l) == '_')) {
                    advance(l);
                }
                tok.kind = TOK_INT_LIT;
                tok.value.int_val = strtoll(l->source + start, NULL, 16);
                tok.len = (uint32_t)(l->pos - start);
                return tok;
            }
            if (next == 'o' || next == 'O') {
                advance(l); advance(l);
                while (l->pos < l->source_len && ((peek(l) >= '0' && peek(l) <= '7') || peek(l) == '_')) {
                    advance(l);
                }
                tok.kind = TOK_INT_LIT;
                tok.value.int_val = strtoll(l->source + start + 2, NULL, 8);
                tok.len = (uint32_t)(l->pos - start);
                return tok;
            }
            if (next == 'b' || next == 'B') {
                advance(l); advance(l);
                while (l->pos < l->source_len && (peek(l) == '0' || peek(l) == '1' || peek(l) == '_')) {
                    advance(l);
                }
                tok.kind = TOK_INT_LIT;
                tok.value.int_val = strtoll(l->source + start + 2, NULL, 2);
                tok.len = (uint32_t)(l->pos - start);
                return tok;
            }
        }

        while (l->pos < l->source_len && (is_digit(peek(l)) || peek(l) == '_')) {
            advance(l);
        }

        if (peek(l) == '.' && is_digit(peek_next(l))) {
            is_float = true;
            advance(l);
            while (l->pos < l->source_len && (is_digit(peek(l)) || peek(l) == '_')) {
                advance(l);
            }
        }

        if (peek(l) == 'e' || peek(l) == 'E') {
            is_float = true;
            advance(l);
            if (peek(l) == '+' || peek(l) == '-') advance(l);
            while (l->pos < l->source_len && is_digit(peek(l))) {
                advance(l);
            }
        }

        /* Strip underscores for parsing numeric value */
        {
            char numbuf[128];
            size_t ni = 0;
            size_t si;
            size_t slen = l->pos - start;
            if (slen >= sizeof(numbuf)) slen = sizeof(numbuf) - 1;
            for (si = 0; si < slen && ni < sizeof(numbuf) - 1; si++) {
                if (l->source[start + si] != '_') {
                    numbuf[ni++] = l->source[start + si];
                }
            }
            numbuf[ni] = '\0';

            if (is_float) {
                tok.kind = TOK_FLOAT_LIT;
                tok.value.float_val = strtod(numbuf, NULL);
            } else {
                tok.kind = TOK_INT_LIT;
                tok.value.int_val = strtoll(numbuf, NULL, 10);
            }
        }
        tok.len = (uint32_t)(l->pos - start);
        return tok;
    }

    /* Strings */
    if (c == '"') {
        advance(l);
        /* Process escape sequences into a temporary buffer */
        char str_buf[8192];
        size_t si = 0;
        while (l->pos < l->source_len && peek(l) != '"') {
            if (peek(l) == '\\') {
                advance(l); /* consume backslash */
                if (l->pos >= l->source_len) break;
                char esc = peek(l);
                switch (esc) {
                case 'n':  str_buf[si++] = '\n'; break;
                case 't':  str_buf[si++] = '\t'; break;
                case 'r':  str_buf[si++] = '\r'; break;
                case '0':  str_buf[si++] = '\0'; break;
                case '\\': str_buf[si++] = '\\'; break;
                case '"':  str_buf[si++] = '"';  break;
                case '{':  str_buf[si++] = '{';  break;  /* escape interpolation */
                default:   str_buf[si++] = '\\'; str_buf[si++] = esc; break;
                }
                advance(l);
            } else if (peek(l) == '\n') {
                SourceLoc loc = tok.loc;
                report_error(l->reporter, loc, "unterminated string literal",
                            "string literals cannot span multiple lines; use a raw string `...` instead");
                tok.kind = TOK_ERROR;
                return tok;
            } else {
                if (si < sizeof(str_buf) - 1) str_buf[si++] = peek(l);
                advance(l);
            }
        }
        if (l->pos < l->source_len) {
            advance(l); /* consume closing quote */
        } else {
            report_error(l->reporter, tok.loc, "unterminated string literal",
                        "add a closing '\"'");
        }
        tok.kind = TOK_STRING_LIT;
        tok.value.str_val = intern_get(l->intern, str_buf, si);
        tok.len = (uint32_t)(l->pos - (tok.loc.offset));
        return tok;
    }

    /* Raw strings */
    if (c == '`') {
        advance(l);
        size_t start = l->pos;
        while (l->pos < l->source_len && peek(l) != '`') {
            advance(l);
        }
        size_t len = l->pos - start;
        if (l->pos < l->source_len) {
            advance(l);
        } else {
            report_error(l->reporter, tok.loc, "unterminated raw string",
                        "add a closing '`'");
        }
        tok.kind = TOK_STRING_LIT;
        tok.value.str_val = intern_get(l->intern, l->source + start, len);
        tok.len = (uint32_t)(len + 2);
        return tok;
    }

    /* Character literals */
    if (c == '\'') {
        advance(l);
        size_t start = l->pos;
        if (peek(l) == '\\') {
            advance(l);
        }
        advance(l);
        size_t len = l->pos - start;
        if (peek(l) == '\'') {
            advance(l);
        } else {
            report_error(l->reporter, tok.loc, "unterminated character literal",
                        "add a closing '\\''");
        }
        tok.kind = TOK_CHAR_LIT;
        tok.value.str_val = intern_get(l->intern, l->source + start, len);
        tok.len = (uint32_t)(len + 2);
        return tok;
    }

    /* Operators and delimiters */
    advance(l);

    switch (c) {
    case '(': tok.kind = TOK_LPAREN; break;
    case ')': tok.kind = TOK_RPAREN; break;
    case '[': tok.kind = TOK_LBRACKET; break;
    case ']': tok.kind = TOK_RBRACKET; break;
    case '{': tok.kind = TOK_LBRACE; break;
    case '}': tok.kind = TOK_RBRACE; break;
    case ',': tok.kind = TOK_COMMA; break;
    case ';': tok.kind = TOK_SEMICOLON; break;
    case '~': tok.kind = TOK_TILDE; break;
    case '#': tok.kind = TOK_HASH; break;
    case '@': tok.kind = TOK_AT; break;
    case ':':
        if (peek(l) == ':') { advance(l); tok.kind = TOK_COLON_COLON; }
        else { tok.kind = TOK_COLON; }
        break;
    case '.':
        if (peek(l) == '.') {
            advance(l);
            if (peek(l) == '=') { advance(l); tok.kind = TOK_DOT_DOT_EQ; }
            else { tok.kind = TOK_DOT_DOT; }
        } else { tok.kind = TOK_DOT; }
        break;
    case '+':
        if (peek(l) == '=') { advance(l); tok.kind = TOK_PLUS_EQ; }
        else { tok.kind = TOK_PLUS; }
        break;
    case '-':
        if (peek(l) == '>') { advance(l); tok.kind = TOK_ARROW; }
        else if (peek(l) == '=') { advance(l); tok.kind = TOK_MINUS_EQ; }
        else { tok.kind = TOK_MINUS; }
        break;
    case '*':
        if (peek(l) == '*') { advance(l); tok.kind = TOK_POWER; }
        else if (peek(l) == '=') { advance(l); tok.kind = TOK_STAR_EQ; }
        else { tok.kind = TOK_STAR; }
        break;
    case '/':
        if (peek(l) == '=') { advance(l); tok.kind = TOK_SLASH_EQ; }
        else { tok.kind = TOK_SLASH; }
        break;
    case '%':
        if (peek(l) == '=') { advance(l); tok.kind = TOK_PERCENT_EQ; }
        else { tok.kind = TOK_PERCENT; }
        break;
    case '&':
        if (peek(l) == '&') { advance(l); tok.kind = TOK_AND_AND; }
        else if (peek(l) == '=') { advance(l); tok.kind = TOK_AMP_EQ; }
        else { tok.kind = TOK_AMP; }
        break;
    case '|':
        if (peek(l) == '|') { advance(l); tok.kind = TOK_PIPE_PIPE; }
        else if (peek(l) == '>') { advance(l); tok.kind = TOK_PIPE_GT; }
        else if (peek(l) == '=') { advance(l); tok.kind = TOK_PIPE_EQ; }
        else { tok.kind = TOK_PIPE; }
        break;
    case '^':
        if (peek(l) == '=') { advance(l); tok.kind = TOK_CARET_EQ; }
        else { tok.kind = TOK_CARET; }
        break;
    case '=':
        if (peek(l) == '=') { advance(l); tok.kind = TOK_EQ_EQ; }
        else if (peek(l) == '>') { advance(l); tok.kind = TOK_FAT_ARROW; }
        else { tok.kind = TOK_EQ; }
        break;
    case '!':
        if (peek(l) == '=') { advance(l); tok.kind = TOK_NOT_EQ; }
        else { tok.kind = TOK_BANG; }
        break;
    case '<':
        if (peek(l) == '=') { advance(l); tok.kind = TOK_LT_EQ; }
        else if (peek(l) == '<') {
            advance(l);
            if (peek(l) == '=') { advance(l); tok.kind = TOK_SHL_EQ; }
            else { tok.kind = TOK_SHL; }
        }
        else { tok.kind = TOK_LT; }
        break;
    case '>':
        if (peek(l) == '=') { advance(l); tok.kind = TOK_GT_EQ; }
        else if (peek(l) == '>') {
            advance(l);
            if (peek(l) == '=') { advance(l); tok.kind = TOK_SHR_EQ; }
            else { tok.kind = TOK_SHR; }
        }
        else { tok.kind = TOK_GT; }
        break;
    case '?':
        if (peek(l) == '.') { advance(l); tok.kind = TOK_QUESTION_DOT; }
        else { tok.kind = TOK_QUESTION; }
        break;
    default:
        tok.kind = TOK_ERROR;
        {
            char msg[64];
            snprintf(msg, sizeof(msg), "unexpected character: '%c' (0x%02x)", c, (unsigned char)c);
            report_error(l->reporter, tok.loc, msg, NULL);
        }
        break;
    }

    tok.len = (uint32_t)(l->pos - tok.loc.offset);
    return tok;
}

Token lexer_next(Lexer *l) {
    /*
     * Automatic semicolon insertion (Go-style):
     * When a newline is encountered, and the previous token could end
     * a statement, insert a synthetic semicolon.
     */
    Token tok;

    for (;;) {
        tok = lexer_next_raw(l);

        if (tok.kind == TOK_NEWLINE) {
            if (token_ends_stmt(l->prev_token.kind)) {
                /* Insert semicolon */
                tok.kind = TOK_SEMICOLON;
                tok.len = 0;
                l->prev_token = tok;
                return tok;
            }
            /* Skip newline and continue */
            continue;
        }

        l->prev_token = tok;
        return tok;
    }
}
