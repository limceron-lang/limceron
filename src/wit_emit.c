/*
 * Limceron Compiler — WIT (WebAssembly Interface Types) Emitter
 *
 * Walks the top-level AST for `AST_AGENT` nodes and emits a `.wit` file
 * declaratively describing the agent's component-model interface:
 *
 *   - For each unique capability prefix (the part before the dot in
 *     `llm.classify`, `data.read`, ...) we emit one `interface <prefix>`
 *     block whose body contains a `func` line per concrete capability with
 *     that prefix.
 *
 *   - Each agent yields a `world <agent-kebab-case>` block that imports
 *     every prefix used by the agent's capability list and exports every
 *     top-level `fn` declared inside the agent body.
 *
 * The format is the WIT subset described at
 * https://component-model.bytecodealliance.org/design/wit.html — pure text,
 * no external dependencies.
 *
 * See `INTEGRATION-WITH-AGENT-A.md` at the repo root for how this hooks
 * into the wasm build flow.
 */

#include "wit_emit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/* Static capability signature table                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *qualified;   /* e.g. "llm.classify"                       */
    const char *signature;   /* WIT body, e.g. "classify: func(...) ..."  */
} WitCapSig;

/* PoC signature table.  Keep in sync with the doc comment in wit_emit.h.
 * Unknown capabilities fall back to a placeholder signature emitted by
 * `emit_unknown_cap_sig` below. */
static const WitCapSig WIT_CAP_TABLE[] = {
    { "llm.classify",
      "classify: func(text: string) -> tuple<string, f64>" },
    { "llm.complete",
      "complete: func(prompt: string) -> string" },
    { "llm.chat",
      "chat: func(messages: list<string>) -> string" },
    { "data.read",
      "read: func(query: string) -> string" },
    { "data.write",
      "write: func(key: string, value: string) -> result<_, string>" },
    { "kb.search",
      "search: func(query: string, top-k: u32) -> list<string>" },
    { "http.fetch",
      "fetch: func(url: string) -> result<list<u8>, string>" },
    { "mcp.call",
      "call: func(tool: string, args: string) -> string" },
};
static const int WIT_CAP_TABLE_LEN =
    (int)(sizeof(WIT_CAP_TABLE) / sizeof(WIT_CAP_TABLE[0]));

/* Find a static signature for the qualified capability name; returns NULL
 * if not in the table. */
static const char *wit_lookup_cap_sig(const char *qualified) {
    int i;
    if (!qualified) return NULL;
    for (i = 0; i < WIT_CAP_TABLE_LEN; i++) {
        if (strcmp(WIT_CAP_TABLE[i].qualified, qualified) == 0)
            return WIT_CAP_TABLE[i].signature;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Small dynamic string buffer                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} WitBuf;

static int witbuf_grow(WitBuf *b, size_t need) {
    size_t want = b->cap ? b->cap : 256;
    while (want < b->len + need + 1) {
        if (want > (size_t)1 << 28) return 1;  /* sanity cap: 256 MiB */
        want *= 2;
    }
    if (want != b->cap) {
        char *p = (char *)realloc(b->buf, want);
        if (!p) return 1;
        b->buf = p;
        b->cap = want;
    }
    return 0;
}

static int witbuf_append(WitBuf *b, const char *s) {
    size_t n;
    if (!s) return 0;
    n = strlen(s);
    if (witbuf_grow(b, n) != 0) return 1;
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = '\0';
    return 0;
}

static int witbuf_appendf(WitBuf *b, const char *fmt, ...) {
    char    tmp[1024];
    int     n;
    va_list ap;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return 1;
    if ((size_t)n >= sizeof(tmp)) {
        /* Truncate gracefully — shouldn't happen for WIT (small strings). */
        tmp[sizeof(tmp) - 1] = '\0';
    }
    return witbuf_append(b, tmp);
}

static void witbuf_free(WitBuf *b) {
    free(b->buf);
    b->buf = NULL;
    b->len = b->cap = 0;
}

/* ------------------------------------------------------------------ */
/* Identifier normalization                                           */
/* ------------------------------------------------------------------ */

/* WIT identifiers are lowercase kebab-case.  Convert a Limceron name
 * (CamelCase, snake_case, or mixed) into kebab-case in `out`.  Output
 * length is bounded by `out_cap` (NUL-terminated). */
static void to_kebab(const char *in, char *out, size_t out_cap) {
    size_t oi = 0;
    int    prev_lower = 0;
    size_t i;
    if (out_cap == 0) return;
    if (!in) {
        out[0] = '\0';
        return;
    }
    for (i = 0; in[i] != '\0' && oi + 1 < out_cap; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c >= 'A' && c <= 'Z') {
            if (oi > 0 && prev_lower && oi + 1 < out_cap) {
                out[oi++] = '-';
                if (oi + 1 >= out_cap) break;
            }
            out[oi++] = (char)(c - 'A' + 'a');
            prev_lower = 0;
        } else if (c == '_' || c == ' ' || c == '.') {
            out[oi++] = '-';
            prev_lower = 0;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[oi++] = (char)c;
            prev_lower = 1;
        } else if (c == '-') {
            out[oi++] = '-';
            prev_lower = 0;
        }
        /* Other characters silently dropped (WIT identifiers are strict). */
    }
    out[oi] = '\0';
    /* Strip leading/trailing dashes for safety. */
    while (oi > 0 && out[oi - 1] == '-') {
        out[--oi] = '\0';
    }
    if (oi == 0) {
        /* Always emit *something* — fall back to "agent". */
        snprintf(out, out_cap, "agent");
    }
}

/* ------------------------------------------------------------------ */
/* Type translation                                                   */
/* ------------------------------------------------------------------ */

/* Forward declaration. */
static int wit_type_str(const AstNode *type_expr, WitBuf *out);

/* Translate a named Limceron type (already normalized via AST_TYPE_NAMED)
 * into a WIT type token.  `name` may be qualified ("foo.Bar") — we use the
 * trailing component for matching. */
static const char *named_to_wit(const char *name) {
    const char *base;
    if (!name) return "string";
    base = strrchr(name, '.');
    base = base ? base + 1 : name;
    if (strcmp(base, "int")    == 0) return "s64";
    if (strcmp(base, "i64")    == 0) return "s64";
    if (strcmp(base, "i32")    == 0) return "s32";
    if (strcmp(base, "u64")    == 0) return "u64";
    if (strcmp(base, "u32")    == 0) return "u32";
    if (strcmp(base, "float")  == 0) return "f64";
    if (strcmp(base, "f64")    == 0) return "f64";
    if (strcmp(base, "f32")    == 0) return "f32";
    if (strcmp(base, "bool")   == 0) return "bool";
    if (strcmp(base, "string") == 0) return "string";
    if (strcmp(base, "char")   == 0) return "char";
    if (strcmp(base, "void")   == 0) return "_";
    return NULL;  /* unknown -> caller emits placeholder */
}

/* Emit the WIT representation of a Limceron type expression.  Best-effort:
 * unknown shapes degrade to `string` plus a comment marker. */
static int wit_type_str(const AstNode *type_expr, WitBuf *out) {
    if (!type_expr) {
        return witbuf_append(out, "string");
    }
    switch (type_expr->kind) {
    case AST_TYPE_NAMED: {
        const char *base;
        const char *mapped;
        if (!type_expr->name) {
            return witbuf_append(out, "string");
        }
        base = strrchr(type_expr->name, '.');
        base = base ? base + 1 : type_expr->name;
        /* Generic constructors: list<T>, Result<T,E>, Option<T> */
        if ((strcmp(base, "list") == 0 || strcmp(base, "List") == 0)
            && type_expr->generics) {
            if (witbuf_append(out, "list<") != 0) return 1;
            if (wit_type_str(type_expr->generics, out) != 0) return 1;
            return witbuf_append(out, ">");
        }
        if ((strcmp(base, "Result") == 0 || strcmp(base, "result") == 0)
            && type_expr->generics) {
            const AstNode *ok = type_expr->generics;
            const AstNode *err = ok ? ok->next : NULL;
            if (witbuf_append(out, "result<") != 0) return 1;
            if (ok) {
                if (wit_type_str(ok, out) != 0) return 1;
            } else {
                if (witbuf_append(out, "_") != 0) return 1;
            }
            if (witbuf_append(out, ", ") != 0) return 1;
            if (err) {
                if (wit_type_str(err, out) != 0) return 1;
            } else {
                if (witbuf_append(out, "string") != 0) return 1;
            }
            return witbuf_append(out, ">");
        }
        if ((strcmp(base, "Option") == 0 || strcmp(base, "option") == 0)
            && type_expr->generics) {
            if (witbuf_append(out, "option<") != 0) return 1;
            if (wit_type_str(type_expr->generics, out) != 0) return 1;
            return witbuf_append(out, ">");
        }
        mapped = named_to_wit(type_expr->name);
        if (mapped) {
            return witbuf_append(out, mapped);
        }
        /* Unknown user-defined type — placeholder.  We can't safely emit
         * an inline comment here because not every WIT location accepts
         * one (e.g. inside a return-type position).  Caller-side line
         * comments handle the explanatory text where appropriate. */
        (void)base;
        return witbuf_append(out, "string");
    }
    case AST_TYPE_OPTIONAL:
        if (witbuf_append(out, "option<") != 0) return 1;
        if (wit_type_str(type_expr->left, out) != 0) return 1;
        return witbuf_append(out, ">");
    case AST_TYPE_SLICE:
    case AST_TYPE_ARRAY:
        if (witbuf_append(out, "list<") != 0) return 1;
        if (wit_type_str(type_expr->left, out) != 0) return 1;
        return witbuf_append(out, ">");
    case AST_TYPE_REF:
    case AST_TYPE_PTR:
        /* References collapse to the underlying type for the purposes of
         * WIT — there is no notion of pointer in the component model. */
        return wit_type_str(type_expr->left, out);
    case AST_TYPE_INFER:
        return witbuf_append(out, "string");
    default:
        return witbuf_append(out, "string");
    }
}

/* ------------------------------------------------------------------ */
/* Capability collection                                              */
/* ------------------------------------------------------------------ */

#define WIT_MAX_CAPS_PER_AGENT  64
#define WIT_MAX_PREFIXES        32

/* Collect the qualified capability names declared in `agent` into `out`
 * (mirrors `collect_agent_caps` in typecheck.c, kept duplicate to avoid
 * cross-translation-unit coupling).  Returns the number written. */
static int wit_collect_caps(const AstNode *agent,
                            const char *out[],
                            int max_out) {
    int count = 0;
    const AstNode *field;
    if (!agent) return 0;
    for (field = agent->params; field != NULL; field = field->next) {
        const AstNode *arr;
        const AstNode *elem;
        if (field->kind != AST_FIELD) continue;
        if (!field->name || strcmp(field->name, "capabilities") != 0) continue;
        arr = field->right;
        if (!arr || arr->kind != AST_ARRAY) break;
        for (elem = arr->params; elem != NULL && count < max_out;
             elem = elem->next) {
            if (elem->name && elem->name[0] != '\0') {
                out[count++] = elem->name;
            } else if (elem->kind == AST_IDENT && elem->val.str_val
                       && elem->val.str_val[0] != '\0') {
                out[count++] = elem->val.str_val;
            }
        }
        break;
    }
    return count;
}

/* Extract the prefix (text before the first '.') of a qualified name into
 * `prefix_out` (NUL-terminated, bounded by `cap`).  Returns the suffix
 * (text after the dot), or the full name if there was no dot. */
static const char *split_qualified(const char *qualified,
                                   char *prefix_out, size_t cap) {
    const char *dot;
    size_t      n;
    if (!qualified) {
        if (cap > 0) prefix_out[0] = '\0';
        return "";
    }
    dot = strchr(qualified, '.');
    if (!dot) {
        n = strlen(qualified);
        if (n >= cap) n = cap - 1;
        memcpy(prefix_out, qualified, n);
        prefix_out[n] = '\0';
        return qualified + strlen(qualified);
    }
    n = (size_t)(dot - qualified);
    if (n >= cap) n = cap - 1;
    memcpy(prefix_out, qualified, n);
    prefix_out[n] = '\0';
    return dot + 1;
}

/* ------------------------------------------------------------------ */
/* Per-agent emission                                                 */
/* ------------------------------------------------------------------ */

/* Emit one `func` line for a capability, indented two spaces under an
 * `interface` block.  `qualified` is the full "prefix.name" key. */
static int emit_cap_func(WitBuf *out, const char *qualified) {
    const char *sig = wit_lookup_cap_sig(qualified);
    char        prefix[64];
    const char *suffix = split_qualified(qualified, prefix, sizeof(prefix));
    char        kebab_suffix[128];

    if (sig) {
        return witbuf_appendf(out, "    %s;\n", sig);
    }
    /* Unknown capability — placeholder. */
    to_kebab(suffix, kebab_suffix, sizeof(kebab_suffix));
    if (kebab_suffix[0] == '\0') {
        snprintf(kebab_suffix, sizeof(kebab_suffix), "op");
    }
    return witbuf_appendf(out,
        "    %s: func() -> string;  // unknown capability — placeholder signature\n",
        kebab_suffix);
}

/* Emit `interface <prefix> { ... }` containing every capability with that
 * prefix in the agent's list. */
static int emit_interface(WitBuf *out,
                          const char *prefix,
                          const char *caps[],
                          int cap_count) {
    int i;
    if (witbuf_appendf(out, "interface %s {\n", prefix) != 0) return 1;
    for (i = 0; i < cap_count; i++) {
        char this_prefix[64];
        (void)split_qualified(caps[i], this_prefix, sizeof(this_prefix));
        if (strcmp(this_prefix, prefix) != 0) continue;
        if (emit_cap_func(out, caps[i]) != 0) return 1;
    }
    return witbuf_append(out, "}\n\n");
}

/* L12: locate the parameterised capability entry for a qualified verb in
 * the agent's `capabilities:` array, if any. The bare form is encoded as
 * AST_IDENT and returns NULL here; only AST_CAPABILITY_ITEM nodes carry
 * a host:port allowlist payload via their `params` chain of
 * AST_STRING_LIT. */
static const AstNode *find_param_cap_item(const AstNode *agent,
                                          const char *qualified) {
    const AstNode *field, *elem;
    if (!agent || !qualified) return NULL;
    for (field = agent->params; field != NULL; field = field->next) {
        if (field->kind != AST_FIELD) continue;
        if (!field->name || strcmp(field->name, "capabilities") != 0)
            continue;
        if (!field->right || field->right->kind != AST_ARRAY) break;
        for (elem = field->right->params; elem != NULL; elem = elem->next) {
            if (elem->kind != AST_CAPABILITY_ITEM) continue;
            if (!elem->name) continue;
            if (strcmp(elem->name, qualified) == 0) return elem;
        }
        break;
    }
    return NULL;
}

/* Emit a WIT-style `import <verb> { hosts: [...] }` block when the
 * agent's capability list pins a host allowlist on a network-shaped
 * verb. The shape mirrors the documented L12 advertisement:
 *
 *   import http.fetch {
 *       hosts: ["api.openai.com:443", "*.example.com:443"]
 *   }
 *
 * Returns 0 on success, 1 on buffer error, and -1 if no parameterised
 * allowlist exists (caller should emit the bare `import` line). */
static int emit_param_cap_import(WitBuf *out,
                                 const AstNode *agent,
                                 const char *qualified) {
    const AstNode *item = find_param_cap_item(agent, qualified);
    if (!item) return -1;
    const AstNode *host;
    int first = 1;
    if (witbuf_appendf(out, "    import %s {\n", qualified) != 0) return 1;
    if (witbuf_append(out, "        hosts: [") != 0) return 1;
    for (host = item->params; host != NULL; host = host->next) {
        const char *spec = NULL;
        if (host->kind == AST_STRING_LIT) spec = host->val.str_val;
        if (!spec) continue;
        if (!first) {
            if (witbuf_append(out, ", ") != 0) return 1;
        }
        first = 0;
        if (witbuf_appendf(out, "\"%s\"", spec) != 0) return 1;
    }
    if (witbuf_append(out, "]\n    }\n") != 0) return 1;
    return 0;
}

/* Emit `export <fn>: func(<params>) -> <ret>;` lines for every fn in the
 * agent's body (`agent->left` is the linked list of AST_FN nodes set up
 * by `parse_agent`). */
static int emit_agent_exports(WitBuf *out, const AstNode *agent) {
    const AstNode *fn;
    if (!agent) return 0;
    for (fn = agent->left; fn != NULL; fn = fn->next) {
        const AstNode *param;
        char           kebab_name[128];
        int            first_param = 1;
        if (fn->kind != AST_FN || !fn->name) continue;
        to_kebab(fn->name, kebab_name, sizeof(kebab_name));
        if (witbuf_appendf(out, "    export %s: func(", kebab_name) != 0)
            return 1;
        for (param = fn->params; param != NULL; param = param->next) {
            char kebab_param[128];
            if (param->kind != AST_PARAM) continue;
            /* Skip `&self` / `self`. */
            if (param->name && strcmp(param->name, "self") == 0) continue;
            if (!first_param) {
                if (witbuf_append(out, ", ") != 0) return 1;
            }
            first_param = 0;
            to_kebab(param->name ? param->name : "arg",
                     kebab_param, sizeof(kebab_param));
            if (witbuf_appendf(out, "%s: ", kebab_param) != 0) return 1;
            if (wit_type_str(param->type_expr, out) != 0) return 1;
        }
        if (witbuf_append(out, ")") != 0) return 1;
        if (fn->type_expr) {
            if (witbuf_append(out, " -> ") != 0) return 1;
            if (wit_type_str(fn->type_expr, out) != 0) return 1;
        }
        if (witbuf_append(out, ";\n") != 0) return 1;
    }
    return 0;
}

/* Top-level: emit a single agent's interfaces and world block into `out`. */
static int emit_agent(WitBuf *out, const AstNode *agent) {
    const char *caps[WIT_MAX_CAPS_PER_AGENT];
    char        prefixes[WIT_MAX_PREFIXES][64];
    int         prefix_count = 0;
    int         cap_count;
    int         i;
    char        kebab_agent[128];

    cap_count = wit_collect_caps(agent, caps, WIT_MAX_CAPS_PER_AGENT);

    /* Build the unique prefix list, preserving first-seen order. */
    for (i = 0; i < cap_count; i++) {
        char this_prefix[64];
        int  j;
        int  seen = 0;
        (void)split_qualified(caps[i], this_prefix, sizeof(this_prefix));
        if (this_prefix[0] == '\0') continue;
        for (j = 0; j < prefix_count; j++) {
            if (strcmp(prefixes[j], this_prefix) == 0) { seen = 1; break; }
        }
        if (!seen && prefix_count < WIT_MAX_PREFIXES) {
            size_t n = strlen(this_prefix);
            if (n >= sizeof(prefixes[0])) n = sizeof(prefixes[0]) - 1;
            memcpy(prefixes[prefix_count], this_prefix, n);
            prefixes[prefix_count][n] = '\0';
            prefix_count++;
        }
    }

    /* One `interface <prefix> { ... }` block per unique prefix. */
    for (i = 0; i < prefix_count; i++) {
        if (emit_interface(out, prefixes[i], caps, cap_count) != 0) return 1;
    }

    /* `world <agent-kebab> { ... }` */
    to_kebab(agent->name, kebab_agent, sizeof(kebab_agent));
    if (witbuf_appendf(out, "world %s {\n", kebab_agent) != 0) return 1;
    for (i = 0; i < prefix_count; i++) {
        if (witbuf_appendf(out, "    import %s;\n", prefixes[i]) != 0)
            return 1;
    }
    /* L12: for every parameterised capability entry emit a richer
     * `import <verb> { hosts: [...] }` block carrying the compile-time
     * allowlist alongside the bare prefix import. The runtime side
     * (wazero) reads the same shape out of the wasm custom section --
     * the WIT block is the human-readable advertisement of the same
     * contract. */
    for (i = 0; i < cap_count; i++) {
        int rc = emit_param_cap_import(out, agent, caps[i]);
        if (rc == 1) return 1;
        /* rc == -1 means the cap is bare; nothing to emit here. */
    }
    if (prefix_count > 0) {
        if (witbuf_append(out, "\n") != 0) return 1;
    }
    if (emit_agent_exports(out, agent) != 0) return 1;
    if (witbuf_append(out, "}\n") != 0) return 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                        */
/* ------------------------------------------------------------------ */

int lcn_emit_wit(AstNode *program, const char *wit_path) {
    WitBuf     buf;
    AstNode   *node;
    int        agent_count = 0;
    char       package_name[128];
    const AstNode *first_agent = NULL;
    FILE      *fp;
    size_t     wrote;

    if (!program || !wit_path) {
        fprintf(stderr, "error: lcn_emit_wit called with null argument\n");
        return 1;
    }

    /* Top-level declarations live in `program->params` (see parse_program
     * in parser.c).  Walk that list to count agents and pick the first
     * one for the package name. */
    for (node = program->params; node != NULL; node = node->next) {
        if (node->kind == AST_AGENT) {
            if (!first_agent) first_agent = node;
            agent_count++;
        }
    }
    if (agent_count == 0) {
        fprintf(stderr,
                "  WIT emit: no agent declarations found, skipping %s\n",
                wit_path);
        return 1;
    }

    memset(&buf, 0, sizeof(buf));
    to_kebab(first_agent->name, package_name, sizeof(package_name));
    if (package_name[0] == '\0') {
        snprintf(package_name, sizeof(package_name), "agent");
    }

    if (witbuf_appendf(&buf,
            "// Auto-generated by Limceron %s. Do not edit.\n"
            "package limceron:%s;\n\n",
            LCN_VERSION, package_name) != 0) {
        witbuf_free(&buf);
        return 1;
    }

    /* Emit each agent (interfaces first, then world). */
    for (node = program->params; node != NULL; node = node->next) {
        if (node->kind != AST_AGENT) continue;
        if (emit_agent(&buf, node) != 0) {
            witbuf_free(&buf);
            return 1;
        }
        /* Blank line between agents in a multi-agent file. */
        if (node->next) {
            const AstNode *peek;
            for (peek = node->next; peek != NULL; peek = peek->next) {
                if (peek->kind == AST_AGENT) {
                    if (witbuf_append(&buf, "\n") != 0) {
                        witbuf_free(&buf);
                        return 1;
                    }
                    break;
                }
            }
        }
    }

    /* Write to file. */
    fp = fopen(wit_path, "w");
    if (!fp) {
        fprintf(stderr, "error: cannot write %s\n", wit_path);
        witbuf_free(&buf);
        return 1;
    }
    wrote = fwrite(buf.buf, 1, buf.len, fp);
    fclose(fp);
    if (wrote != buf.len) {
        fprintf(stderr, "error: short write to %s\n", wit_path);
        witbuf_free(&buf);
        return 1;
    }
    fprintf(stderr, "  WIT emit: %s (%zu bytes, %d agent%s)\n",
            wit_path, buf.len, agent_count, agent_count == 1 ? "" : "s");
    witbuf_free(&buf);
    return 0;
}
