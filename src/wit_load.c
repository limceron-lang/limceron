/*
 * Limceron Compiler -- WIT Contract Loader
 *
 * Parses the canonical `include/vdag.wit` produced by hand and
 * checked into the repo. The parser handles only the subset
 * `src/wit_emit.c` already emits:
 *
 *   interface <ns> {
 *       <verb>: func(<arg>: <type>, ...) -> <ret>;
 *       ...
 *   }
 *
 * Anything else (line comments `//`, blank lines, `package` lines,
 * `world` blocks, unrecognised tokens) is silently skipped so that
 * the canonical file can carry documentation comments and future
 * shapes without breaking older compiler binaries.
 *
 * Returns 0 on success; soft failures (file missing, parse skipped
 * a malformed line) are logged but do not crash the compile.
 */

#include "wit_load.h"

#include <ctype.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

/* ------------------------------------------------------------------ */
/* Path resolution                                                    */
/* ------------------------------------------------------------------ */

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? 1 : 0;
}

/* Fallback executable-path lookup when caller did not pass argv0.
 * Returns a pointer to `out` on success, NULL on failure. */
static const char *exec_self_path(char *out, size_t out_cap) {
#ifdef __APPLE__
    uint32_t sz = (uint32_t)out_cap;
    if (_NSGetExecutablePath(out, &sz) != 0) return NULL;
    return out;
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", out, out_cap - 1);
    if (n <= 0) return NULL;
    out[n] = '\0';
    return out;
#else
    (void)out;
    (void)out_cap;
    return NULL;
#endif
}

/* Shared "find a .wit file next to the compiler" helper. The L1
 * canonical path is `include/vdag.wit`; L5 adds a sibling
 * `include/vdag.errors.wit`. Both follow the same search order. */
static const char *resolve_include_file(char *out, size_t out_cap,
                                        const char *argv0,
                                        const char *basename_in) {
    char resolved[PATH_MAX];
    char self_buf[PATH_MAX];
    const char *rp = NULL;
    if (!out || out_cap == 0 || !basename_in) return NULL;

    if (argv0) rp = realpath(argv0, resolved);
    if (!rp) {
        const char *self = exec_self_path(self_buf, sizeof(self_buf));
        if (self) rp = realpath(self, resolved);
    }
    if (!rp) {
        snprintf(out, out_cap, "include/%s", basename_in);
        return file_exists(out) ? out : NULL;
    }

    char build_dir_buf[PATH_MAX];
    strncpy(build_dir_buf, resolved, sizeof(build_dir_buf) - 1);
    build_dir_buf[sizeof(build_dir_buf) - 1] = '\0';
    char *build_dir = dirname(build_dir_buf);
    char project_dir_buf[PATH_MAX];
    strncpy(project_dir_buf, build_dir, sizeof(project_dir_buf) - 1);
    project_dir_buf[sizeof(project_dir_buf) - 1] = '\0';
    char *project_dir = dirname(project_dir_buf);

    snprintf(out, out_cap, "%s/include/%s", project_dir, basename_in);
    if (file_exists(out)) return out;
    snprintf(out, out_cap, "%s/include/%s", build_dir, basename_in);
    if (file_exists(out)) return out;
    snprintf(out, out_cap, "include/%s", basename_in);
    if (file_exists(out)) return out;
    return NULL;
}

const char *lcn_wit_default_path(char *out, size_t out_cap,
                                 const char *argv0) {
    return resolve_include_file(out, out_cap, argv0, "vdag.wit");
}

const char *lcn_wit_default_errors_path(char *out, size_t out_cap,
                                        const char *argv0) {
    return resolve_include_file(out, out_cap, argv0, "vdag.errors.wit");
}

/* ------------------------------------------------------------------ */
/* Lightweight tokenizer                                              */
/* ------------------------------------------------------------------ */

/* Skip whitespace AND `//` line comments. */
static const char *skip_ws(const char *p) {
    while (*p) {
        if (isspace((unsigned char)*p)) { p++; continue; }
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
            continue;
        }
        break;
    }
    return p;
}

/* Copy an identifier starting at *p into `out` (NUL-terminated,
 * bounded by out_cap). WIT identifiers are kebab-case [a-z0-9-].
 * Advances *p past the identifier. */
static int read_ident(const char **p, char *out, size_t out_cap) {
    size_t i = 0;
    const char *s = *p;
    if (!*s) return 0;
    while (*s && (isalnum((unsigned char)*s) || *s == '-' || *s == '_')) {
        if (i + 1 < out_cap) out[i++] = *s;
        s++;
    }
    if (i == 0) return 0;
    out[i] = '\0';
    *p = s;
    return 1;
}

/* Read a WIT type token. The WIT grammar admits generic shapes
 * like `list<string>` and `result<_, string>` -- for the canonical
 * vdag.wit we only need scalar names, so we accept identifier-shaped
 * tokens plus the `_` placeholder. Trailing generic forms are
 * captured verbatim into the same buffer. */
static int read_type_token(const char **p, char *out, size_t out_cap) {
    size_t i = 0;
    const char *s = *p;
    if (!*s) return 0;
    if (*s == '_') {
        if (out_cap > 1) { out[0] = '_'; out[1] = '\0'; }
        *p = s + 1;
        return 1;
    }
    while (*s && (isalnum((unsigned char)*s) || *s == '-' || *s == '_')) {
        if (i + 1 < out_cap) out[i++] = *s;
        s++;
    }
    /* Optional `<...>` generic body. */
    if (*s == '<') {
        int depth = 0;
        do {
            if (i + 1 < out_cap) out[i++] = *s;
            if (*s == '<') depth++;
            else if (*s == '>') depth--;
            s++;
        } while (*s && depth > 0);
    }
    if (i == 0) return 0;
    out[i] = '\0';
    *p = s;
    return 1;
}

/* Match a single literal character (skipping leading whitespace).
 * Advances *p past the character on success. */
static int match_char(const char **p, char c) {
    const char *s = skip_ws(*p);
    if (*s != c) return 0;
    *p = s + 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Parser                                                             */
/* ------------------------------------------------------------------ */

/* Append a fully-resolved LcnWitFunc to the contract. Silently
 * drops entries past LCN_WIT_MAX_FUNCS (logged once at load). */
static void contract_push(LcnWitContract *c, const LcnWitFunc *fn) {
    if (c->count >= LCN_WIT_MAX_FUNCS) return;
    c->funcs[c->count++] = *fn;
}

/* Parse one `interface <ns> { ... }` body, accumulating funcs
 * into the contract. Returns the pointer past the closing brace
 * (or end-of-input on error). */
static const char *parse_interface(const char *p, const char *ns,
                                   LcnWitContract *c) {
    p = skip_ws(p);
    if (!match_char(&p, '{')) {
        return p;
    }
    while (1) {
        p = skip_ws(p);
        if (!*p) return p;
        if (*p == '}') return p + 1;

        char verb[LCN_WIT_MAX_NAME];
        if (!read_ident(&p, verb, sizeof(verb))) {
            /* Skip to next line and try again. */
            while (*p && *p != '\n') p++;
            continue;
        }
        p = skip_ws(p);
        if (!match_char(&p, ':')) {
            while (*p && *p != ';') p++;
            if (*p == ';') p++;
            continue;
        }
        p = skip_ws(p);
        /* Expect `func`. */
        char kw[LCN_WIT_MAX_NAME];
        if (!read_ident(&p, kw, sizeof(kw)) || strcmp(kw, "func") != 0) {
            while (*p && *p != ';') p++;
            if (*p == ';') p++;
            continue;
        }
        p = skip_ws(p);
        if (!match_char(&p, '(')) {
            while (*p && *p != ';') p++;
            if (*p == ';') p++;
            continue;
        }

        LcnWitFunc fn;
        memset(&fn, 0, sizeof(fn));
        snprintf(fn.qualified, sizeof(fn.qualified), "%s.%s", ns, verb);

        /* Param list. */
        while (1) {
            p = skip_ws(p);
            if (*p == ')') { p++; break; }
            char pname[LCN_WIT_MAX_NAME];
            if (!read_ident(&p, pname, sizeof(pname))) break;
            p = skip_ws(p);
            if (!match_char(&p, ':')) break;
            p = skip_ws(p);
            char ptype[LCN_WIT_MAX_TYPE];
            if (!read_type_token(&p, ptype, sizeof(ptype))) break;
            if (fn.param_count < LCN_WIT_MAX_PARAMS) {
                strncpy(fn.params[fn.param_count].name, pname,
                        sizeof(fn.params[0].name) - 1);
                strncpy(fn.params[fn.param_count].type, ptype,
                        sizeof(fn.params[0].type) - 1);
                fn.param_count++;
            }
            p = skip_ws(p);
            if (*p == ',') { p++; continue; }
            if (*p == ')') { p++; break; }
        }

        /* Optional `-> <ret>` arrow. */
        p = skip_ws(p);
        if (p[0] == '-' && p[1] == '>') {
            p += 2;
            p = skip_ws(p);
            read_type_token(&p, fn.ret_type, sizeof(fn.ret_type));
        } else {
            strncpy(fn.ret_type, "_", sizeof(fn.ret_type) - 1);
        }
        p = skip_ws(p);
        if (*p == ';') p++;

        contract_push(c, &fn);
    }
}

int lcn_wit_load(LcnWitContract *contract, const char *path) {
    if (!contract) return 1;
    memset(contract, 0, sizeof(*contract));
    contract->loaded = 1;
    if (!path) return 1;
    strncpy(contract->source_path, path,
            sizeof(contract->source_path) - 1);

    FILE *fp = fopen(path, "rb");
    if (!fp) return 1;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return 1; }
    rewind(fp);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return 1; }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[got] = '\0';

    const char *p = buf;
    while (*p) {
        p = skip_ws(p);
        if (!*p) break;
        char kw[LCN_WIT_MAX_NAME];
        const char *save = p;
        if (!read_ident(&p, kw, sizeof(kw))) { p++; continue; }
        if (strcmp(kw, "interface") == 0) {
            p = skip_ws(p);
            char ns[LCN_WIT_MAX_NAME];
            if (!read_ident(&p, ns, sizeof(ns))) continue;
            p = parse_interface(p, ns, contract);
        } else {
            /* Skip `package` / `world` / unknown -- advance past
             * the next `;` or `}` boundary. */
            (void)save;
            while (*p && *p != ';' && *p != '}') {
                if (*p == '{') {
                    int depth = 1;
                    p++;
                    while (*p && depth > 0) {
                        if (*p == '{') depth++;
                        else if (*p == '}') depth--;
                        p++;
                    }
                    break;
                }
                p++;
            }
            if (*p) p++;
        }
    }

    free(buf);
    contract->load_ok = 1;
    return 0;
}

/* WIT identifiers are kebab-case (`field-get`); the Limceron front-end
 * uses snake_case (`field_get`) because `-` is not a valid identifier
 * character there. Compare with `-` and `_` treated as equivalent so
 * the contract round-trips cleanly in both directions. */
static int qualified_eq_canonical(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca == '-') ca = '_';
        if (cb == '-') cb = '_';
        if (ca != cb) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

const LcnWitFunc *lcn_wit_lookup_signature(const LcnWitContract *contract,
                                           const char *qualified) {
    int i;
    if (!contract || !qualified) return NULL;
    if (!contract->load_ok) return NULL;
    for (i = 0; i < contract->count; i++) {
        if (qualified_eq_canonical(contract->funcs[i].qualified, qualified))
            return &contract->funcs[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* L5 host-error enum loader                                          */
/* ------------------------------------------------------------------ */

/* WIT identifiers are kebab-case (`quota-exceeded`); the Limceron
 * front-end carries snake_case in identifier strings (`-` is not a
 * valid identifier char there). Compare with `-` and `_` treated as
 * equivalent so author-written `host_error::quota_exceeded` matches
 * the canonical `quota-exceeded` enum entry. */
static int kebab_snake_eq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca == '-') ca = '_';
        if (cb == '-') cb = '_';
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

/* Read a signed decimal integer at *p, advancing past it. Returns 1
 * on success, 0 on failure. Hex / octal not needed -- the canonical
 * sentinels are plain negative decimals. */
static int read_signed_int(const char **p, int64_t *out) {
    const char *s = *p;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }
    if (!isdigit((unsigned char)*s)) return 0;
    int64_t v = 0;
    while (isdigit((unsigned char)*s)) {
        v = v * 10 + (*s - '0');
        s++;
    }
    *out = neg ? -v : v;
    *p = s;
    return 1;
}

/* Parse one `enum <name> { variant = value, ... }` body. */
static const char *parse_enum(const char *p, const char *enum_name,
                              LcnWitContract *c) {
    p = skip_ws(p);
    if (!match_char(&p, '{')) return p;
    if (c->enum_count >= LCN_WIT_MAX_ENUMS) {
        int depth = 1;
        while (*p && depth > 0) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            p++;
        }
        return p;
    }
    LcnWitEnum *e = &c->enums[c->enum_count++];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, enum_name, sizeof(e->name) - 1);

    while (1) {
        p = skip_ws(p);
        if (!*p) return p;
        if (*p == '}') return p + 1;

        char vname[LCN_WIT_MAX_NAME];
        if (!read_ident(&p, vname, sizeof(vname))) {
            while (*p && *p != ',' && *p != '}') p++;
            if (*p == ',') p++;
            continue;
        }
        p = skip_ws(p);
        int64_t value = 0;
        if (*p == '=') {
            p++;
            p = skip_ws(p);
            (void)read_signed_int(&p, &value);
        }
        if (e->variant_count < LCN_WIT_MAX_VARIANTS) {
            LcnWitEnumVariant *v = &e->variants[e->variant_count++];
            strncpy(v->name, vname, sizeof(v->name) - 1);
            v->value = value;
        }
        p = skip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p == '}') return p + 1;
    }
}

int lcn_wit_load_errors(LcnWitContract *contract, const char *path) {
    if (!contract) return 1;
    /* This is an extension load on top of a previously parsed contract.
     * Do NOT memset -- the func table from lcn_wit_load must survive. */
    contract->loaded = 1;
    if (!path) return 1;

    FILE *fp = fopen(path, "rb");
    if (!fp) return 1;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return 1; }
    rewind(fp);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return 1; }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[got] = '\0';

    const char *p = buf;
    while (*p) {
        p = skip_ws(p);
        if (!*p) break;
        char kw[LCN_WIT_MAX_NAME];
        if (!read_ident(&p, kw, sizeof(kw))) { p++; continue; }
        if (strcmp(kw, "enum") == 0) {
            p = skip_ws(p);
            char ename[LCN_WIT_MAX_NAME];
            if (!read_ident(&p, ename, sizeof(ename))) continue;
            p = parse_enum(p, ename, contract);
        } else {
            /* Skip `package` / unknown -- advance past the next `;`
             * or `}` boundary, recursing into `{...}` bodies. */
            while (*p && *p != ';' && *p != '}') {
                if (*p == '{') {
                    int depth = 1;
                    p++;
                    while (*p && depth > 0) {
                        if (*p == '{') depth++;
                        else if (*p == '}') depth--;
                        p++;
                    }
                    break;
                }
                p++;
            }
            if (*p) p++;
        }
    }

    free(buf);
    contract->load_ok = 1;
    return 0;
}

const LcnWitEnum *lcn_wit_lookup_enum(const LcnWitContract *contract,
                                      const char *name) {
    int i;
    if (!contract || !name) return NULL;
    for (i = 0; i < contract->enum_count; i++) {
        if (kebab_snake_eq(contract->enums[i].name, name))
            return &contract->enums[i];
    }
    return NULL;
}

const LcnWitEnumVariant *
lcn_wit_lookup_enum_variant(const LcnWitEnum *e, const char *name) {
    int i;
    if (!e || !name) return NULL;
    for (i = 0; i < e->variant_count; i++) {
        if (kebab_snake_eq(e->variants[i].name, name))
            return &e->variants[i];
    }
    return NULL;
}

/* Process-wide HostError table. Both typecheck and ir_gen share this
 * via lcn_wit_resolve_host_error so that the canonical contract is
 * only parsed once per compile run. */
static LcnWitContract g_host_err_contract;
static int            g_host_err_loaded = 0;

int lcn_wit_resolve_host_error(const char *enum_name,
                               const char *variant_name,
                               int64_t *out_value) {
    if (!enum_name || !variant_name || !out_value) return 0;
    if (!g_host_err_loaded) {
        char path[1024];
        const char *p = lcn_wit_default_errors_path(path, sizeof(path), NULL);
        if (p) (void)lcn_wit_load_errors(&g_host_err_contract, p);
        g_host_err_loaded = 1;
    }
    const LcnWitEnum *e = lcn_wit_lookup_enum(&g_host_err_contract, enum_name);
    if (!e) return 0;
    const LcnWitEnumVariant *v = lcn_wit_lookup_enum_variant(e, variant_name);
    if (!v) return 0;
    *out_value = v->value;
    return 1;
}

/* L6: enum-table accessor used by the L6 typecheck exhaustiveness
 * pass. Triggers the same lazy load as lcn_wit_resolve_host_error
 * so callers don't have to plumb a contract pointer through the AST
 * walker. */
const LcnWitEnum *lcn_wit_shared_enum(const char *enum_name) {
    if (!enum_name) return NULL;
    if (!g_host_err_loaded) {
        char path[1024];
        const char *p = lcn_wit_default_errors_path(path, sizeof(path), NULL);
        if (p) (void)lcn_wit_load_errors(&g_host_err_contract, p);
        g_host_err_loaded = 1;
    }
    return lcn_wit_lookup_enum(&g_host_err_contract, enum_name);
}
