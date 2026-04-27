/*
 * Limceron Compiler — Markdown Source Parser
 *
 * Parses .lceron.md files (Markdown-as-source) into the same AST
 * as the regular .lceron parser. This is the "zero learning curve"
 * entry point for Limceron — write agents in plain Markdown.
 *
 * Format:
 *   # agent Name
 *   > System prompt text.
 *   ## capabilities
 *   - kb.search
 *   ## model
 *   claude-haiku
 *   ## budget
 *   - max_cost: 1.00
 *   ## guards
 *   ### guard_name
 *   Guard description text.
 *   ## tools
 *   ### tool_name(param: Type) -> RetType
 *   Tool description.
 *   requires: [cap.name]
 *   ```limceron
 *   fn run(...) { ... }
 *   ```
 */

#include "lcn.h"

/* ============================================================
 * Section Tracking
 * ============================================================ */

typedef enum {
    MD_SECTION_NONE,
    MD_SECTION_CAPABILITIES,
    MD_SECTION_MODEL,
    MD_SECTION_BUDGET,
    MD_SECTION_GUARDS,
    MD_SECTION_TOOLS,
    MD_SECTION_MEMORY,
    MD_SECTION_KNOWLEDGE,
    MD_SECTION_ENDPOINT,
    MD_SECTION_API_KEY,
    MD_SECTION_ENTROPY_BUDGET,
    MD_SECTION_ACCESS_CONTROL,
    MD_SECTION_CAPABILITY_DECL,  /* ## capability <name> — declares a named capability */
    MD_SECTION_SKILLS,
    MD_SECTION_TAINT,            /* ## taint — declares taint labels */
    MD_SECTION_HEALTH,
    MD_SECTION_METRICS,
    MD_SECTION_SIGNAL,
    MD_SECTION_PROGRESS,
    MD_SECTION_SUPERVISOR,
    MD_SECTION_UNKNOWN
} MdSection;

/* ============================================================
 * Markdown Parser State
 * ============================================================ */

typedef struct {
    const char    *filename;
    const char    *source;
    size_t         source_len;
    size_t         pos;
    uint32_t       line;
    Arena         *arena;
    StringIntern  *intern;
    ErrorReporter *reporter;
    bool           had_error;
} MdParser;

/* ============================================================
 * Forward Declarations
 * ============================================================ */

static void md_error(MdParser *mp, const char *msg);
static void md_error_fmt(MdParser *mp, const char *fmt, ...);
static SourceLoc md_loc(const MdParser *mp);
static bool md_at_end(const MdParser *mp);
static char md_peek(const MdParser *mp);
static void md_advance(MdParser *mp);
static void md_skip_line_whitespace(MdParser *mp);
static char *md_read_line(MdParser *mp);
static char *md_trim(Arena *arena, const char *s);
static bool md_starts_with(const char *s, const char *prefix);
static MdSection md_classify_section(const char *name);

static AstNode *md_parse_capabilities(MdParser *mp);
static AstNode *md_parse_model(MdParser *mp);
static AstNode *md_parse_budget(MdParser *mp);
static AstNode *md_parse_guards(MdParser *mp, AstNode **members_tail);
static AstNode *md_parse_tools(MdParser *mp, AstNode **fns_tail);
static AstNode *md_parse_tool_decl(MdParser *mp, const char *header_line,
                                    AstNode **fns_tail);
static AstNode *md_parse_code_block(MdParser *mp);
static AstNode *md_parse_capability_decl(MdParser *mp, const char *cap_name);
static AstNode *md_parse_taint(MdParser *mp);
static AstNode *md_parse_health(MdParser *mp);
static AstNode *md_parse_metrics(MdParser *mp);
static AstNode *md_parse_signal(MdParser *mp);
static AstNode *md_parse_progress(MdParser *mp);
static AstNode *md_parse_supervisor(MdParser *mp, const char *section_name);

/* ============================================================
 * Utility Functions
 * ============================================================ */

static void md_error(MdParser *mp, const char *msg) {
    SourceLoc loc = md_loc(mp);
    report_error(mp->reporter, loc, msg, NULL);
    mp->had_error = true;
}

static void md_error_fmt(MdParser *mp, const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    md_error(mp, arena_strdup(mp->arena, buf));
}

static SourceLoc md_loc(const MdParser *mp) {
    SourceLoc loc;
    loc.filename = mp->filename;
    loc.line = mp->line;
    loc.column = 1;
    loc.offset = (uint32_t)mp->pos;
    return loc;
}

static bool md_at_end(const MdParser *mp) {
    return mp->pos >= mp->source_len;
}

static char md_peek(const MdParser *mp) {
    if (md_at_end(mp)) return '\0';
    return mp->source[mp->pos];
}

static void md_advance(MdParser *mp) {
    if (md_at_end(mp)) return;
    if (mp->source[mp->pos] == '\n') {
        mp->line++;
    }
    mp->pos++;
}

static void md_skip_line_whitespace(MdParser *mp) {
    while (!md_at_end(mp)) {
        char c = md_peek(mp);
        if (c == ' ' || c == '\t') {
            md_advance(mp);
        } else {
            break;
        }
    }
}

/*
 * Read a single line from current position.
 * Returns arena-allocated string. Advances past the newline.
 */
static char *md_read_line(MdParser *mp) {
    size_t start = mp->pos;
    while (!md_at_end(mp) && md_peek(mp) != '\n') {
        mp->pos++;
    }
    size_t len = mp->pos - start;
    /* Skip the newline itself */
    if (!md_at_end(mp) && mp->source[mp->pos] == '\n') {
        mp->pos++;
        mp->line++;
    }
    return arena_strndup(mp->arena, mp->source + start, len);
}

/*
 * Trim leading and trailing whitespace from a string.
 */
static char *md_trim(Arena *arena, const char *s) {
    if (!s) return arena_strdup(arena, "");
    while (*s == ' ' || *s == '\t' || *s == '\r') s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r')) {
        len--;
    }
    return arena_strndup(arena, s, len);
}

static bool md_starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/*
 * Check if current position starts a heading at exactly `level` hashes then space.
 */
static bool md_is_heading_at_level(const MdParser *mp, int level) {
    size_t p = mp->pos;
    int i;
    /* Must have exactly `level` hashes */
    for (i = 0; i < level; i++) {
        if (p >= mp->source_len || mp->source[p] != '#') return false;
        p++;
    }
    /* Next char must be space (not '#') */
    if (p >= mp->source_len) return false;
    return mp->source[p] == ' ';
}

/*
 * Check if we're at a heading of level <= max_level
 * (i.e., at a section boundary that should stop current section parsing).
 */
static bool md_at_section_boundary(const MdParser *mp, int max_level) {
    int lvl;
    for (lvl = 1; lvl <= max_level; lvl++) {
        if (md_is_heading_at_level(mp, lvl)) return true;
    }
    return false;
}

/*
 * Classify a ## heading name into a section enum.
 */
static MdSection md_classify_section(const char *name) {
    if (strcmp(name, "capabilities") == 0) return MD_SECTION_CAPABILITIES;
    if (strcmp(name, "model") == 0)        return MD_SECTION_MODEL;
    if (strcmp(name, "budget") == 0)       return MD_SECTION_BUDGET;
    if (strcmp(name, "guards") == 0)       return MD_SECTION_GUARDS;
    if (strcmp(name, "tools") == 0)        return MD_SECTION_TOOLS;
    if (strcmp(name, "memory") == 0)      return MD_SECTION_MEMORY;
    if (strcmp(name, "knowledge") == 0)  return MD_SECTION_KNOWLEDGE;
    if (strcmp(name, "endpoint") == 0) return MD_SECTION_ENDPOINT;
    if (strcmp(name, "api_key") == 0 || strcmp(name, "api-key") == 0) return MD_SECTION_API_KEY;
    if (strcmp(name, "entropy_budget") == 0 || strcmp(name, "entropy-budget") == 0 || strcmp(name, "entropy") == 0) return MD_SECTION_ENTROPY_BUDGET;
    if (strcmp(name, "access_control") == 0 || strcmp(name, "access-control") == 0 || strcmp(name, "access") == 0) return MD_SECTION_ACCESS_CONTROL;
    if (strcmp(name, "skills") == 0) return MD_SECTION_SKILLS;
    if (strcmp(name, "taint") == 0) return MD_SECTION_TAINT;
    /* "capability <name>" — singular form with a name declares a capability */
    if (strncmp(name, "capability ", 11) == 0 && strlen(name) > 11) return MD_SECTION_CAPABILITY_DECL;
    if (strcmp(name, "health") == 0) return MD_SECTION_HEALTH;
    if (strcmp(name, "metrics") == 0) return MD_SECTION_METRICS;
    if (strcmp(name, "signal") == 0) return MD_SECTION_SIGNAL;
    if (strcmp(name, "progress") == 0) return MD_SECTION_PROGRESS;
    if (md_starts_with(name, "supervisor")) return MD_SECTION_SUPERVISOR;
    return MD_SECTION_UNKNOWN;
}

/*
 * Skip empty lines (lines that are blank or only whitespace).
 */
static void md_skip_blank_lines(MdParser *mp) {
    while (!md_at_end(mp)) {
        size_t save_pos = mp->pos;
        uint32_t save_line = mp->line;
        md_skip_line_whitespace(mp);
        if (md_at_end(mp) || md_peek(mp) == '\n') {
            if (!md_at_end(mp)) {
                mp->pos++;
                mp->line++;
            }
        } else {
            /* Non-blank line — rewind */
            mp->pos = save_pos;
            mp->line = save_line;
            break;
        }
    }
}

/* ============================================================
 * Section Parsers
 * ============================================================ */

/*
 * Parse capabilities section:
 *   ## capabilities
 *   - kb.search
 *   - human.notify
 *
 * Returns: AST_FIELD(name="capabilities", right=AST_ARRAY of AST_IDENT)
 */
static AstNode *md_parse_capabilities(MdParser *mp) {
    SourceLoc loc = md_loc(mp);
    AstNode *field = ast_new(mp->arena, AST_FIELD, loc);
    field->name = arena_strdup(mp->arena, "capabilities");

    AstNode *arr = ast_new(mp->arena, AST_ARRAY, loc);
    AstNode *elems = NULL;

    md_skip_blank_lines(mp);

    while (!md_at_end(mp) && !md_at_section_boundary(mp, 2)) {
        char *line = md_read_line(mp);
        char *trimmed = md_trim(mp->arena, line);

        /* Skip blank lines within section */
        if (strlen(trimmed) == 0) continue;

        /* Must start with '- ' */
        if (!md_starts_with(trimmed, "- ")) {
            md_error_fmt(mp, "expected '- capability' in capabilities section, got: %s",
                         trimmed);
            continue;
        }

        /* Extract capability name (after "- ") */
        const char *cap_name = md_trim(mp->arena, trimmed + 2);
        if (strlen(cap_name) == 0) continue;

        SourceLoc eloc = loc;
        eloc.line = mp->line - 1;
        AstNode *elem = ast_new(mp->arena, AST_IDENT, eloc);
        elem->name = intern_get(mp->intern, cap_name, strlen(cap_name));
        elems = ast_append(elems, elem);
    }

    arr->params = elems;
    field->right = arr;
    return field;
}

/*
 * Parse model section:
 *   ## model
 *   claude-haiku
 *
 * Returns: AST_FIELD(name="model", right=AST_STRING_LIT)
 */
static AstNode *md_parse_model(MdParser *mp) {
    SourceLoc loc = md_loc(mp);
    AstNode *field = ast_new(mp->arena, AST_FIELD, loc);
    field->name = arena_strdup(mp->arena, "model");

    md_skip_blank_lines(mp);

    /* Collect non-empty content lines until next section */
    char *model_name = NULL;
    while (!md_at_end(mp) && !md_at_section_boundary(mp, 2)) {
        char *line = md_read_line(mp);
        char *trimmed = md_trim(mp->arena, line);
        if (strlen(trimmed) == 0) continue;
        model_name = trimmed;
        break;
    }

    /* Skip any remaining lines in section */
    while (!md_at_end(mp) && !md_at_section_boundary(mp, 2)) {
        md_read_line(mp);
    }

    if (!model_name) {
        md_error(mp, "expected model name in model section");
        model_name = arena_strdup(mp->arena, "");
    }

    AstNode *str_node = ast_new(mp->arena, AST_STRING_LIT, loc);
    str_node->val.str_val = intern_get(mp->intern, model_name, strlen(model_name));
    field->right = str_node;
    return field;
}

/*
 * Parse budget section:
 *   ## budget
 *   - max_cost: 1.00
 *   - max_tokens: 100000
 *
 * Returns: AST_FIELD(name="budget", right=AST_BLOCK containing AST_FIELD sub-fields)
 */
static AstNode *md_parse_budget(MdParser *mp) {
    SourceLoc loc = md_loc(mp);
    AstNode *field = ast_new(mp->arena, AST_FIELD, loc);
    field->name = arena_strdup(mp->arena, "budget");

    AstNode *block = ast_new(mp->arena, AST_BLOCK, loc);
    AstNode *items = NULL;

    md_skip_blank_lines(mp);

    while (!md_at_end(mp) && !md_at_section_boundary(mp, 2)) {
        char *line = md_read_line(mp);
        char *trimmed = md_trim(mp->arena, line);

        if (strlen(trimmed) == 0) continue;

        /* Must start with '- ' */
        if (!md_starts_with(trimmed, "- ")) {
            md_error_fmt(mp, "expected '- key: value' in budget section, got: %s",
                         trimmed);
            continue;
        }

        /* Parse "key: value" after the "- " */
        const char *kv = trimmed + 2;
        const char *colon = strchr(kv, ':');
        if (!colon) {
            md_error_fmt(mp, "expected 'key: value' in budget item: %s", kv);
            continue;
        }

        size_t key_len = (size_t)(colon - kv);
        char *key = arena_strndup(mp->arena, kv, key_len);
        key = md_trim(mp->arena, key);

        const char *val_str = colon + 1;
        char *value = md_trim(mp->arena, val_str);

        SourceLoc floc = loc;
        floc.line = mp->line - 1;
        AstNode *sub_field = ast_new(mp->arena, AST_FIELD, floc);
        sub_field->name = intern_get(mp->intern, key, strlen(key));

        /* Try to parse value as number */
        char *endptr = NULL;
        double dval = strtod(value, &endptr);
        if (endptr && endptr != value && *endptr == '\0') {
            /* Check if it looks like an integer (no decimal point) */
            if (strchr(value, '.') == NULL) {
                AstNode *int_node = ast_new(mp->arena, AST_INT_LIT, floc);
                int_node->val.int_val = (int64_t)dval;
                sub_field->right = int_node;
            } else {
                AstNode *float_node = ast_new(mp->arena, AST_FLOAT_LIT, floc);
                float_node->val.float_val = dval;
                sub_field->right = float_node;
            }
        } else {
            /* Store as string literal */
            AstNode *str_node = ast_new(mp->arena, AST_STRING_LIT, floc);
            str_node->val.str_val = intern_get(mp->intern, value, strlen(value));
            sub_field->right = str_node;
        }

        items = ast_append(items, sub_field);
    }

    block->params = items;
    field->right = block;
    return field;
}

/*
 * Parse guards section:
 *   ## guards
 *   ### rate_limit
 *   Max 50 actions per hour.
 *   ```limceron
 *   let max_actions = 50
 *   let window_seconds = 3600
 *   ```
 *
 * Each ### becomes an AST_GUARD node. If a code block is present,
 * its contents are parsed through the Limceron parser to produce
 * real AST nodes as the guard body. Otherwise the description text
 * is stored as a simple guard with a description field.
 */
static AstNode *md_parse_guards(MdParser *mp, AstNode **members_tail) {
    md_skip_blank_lines(mp);

    AstNode *guard_list = NULL;

    while (!md_at_end(mp) && !md_at_section_boundary(mp, 2)) {
        /* Expect ### heading */
        if (md_is_heading_at_level(mp, 3)) {
            char *line = md_read_line(mp);
            /* Skip "### " prefix */
            const char *name_start = line;
            while (*name_start == '#') name_start++;
            while (*name_start == ' ') name_start++;
            char *guard_name = md_trim(mp->arena, name_start);

            SourceLoc gloc = md_loc(mp);
            gloc.line = mp->line - 1;

            AstNode *guard = ast_new(mp->arena, AST_GUARD, gloc);
            guard->name = intern_get(mp->intern, guard_name, strlen(guard_name));

            /* Collect description text and check for code blocks */
            md_skip_blank_lines(mp);
            size_t desc_start = mp->pos;
            size_t desc_end = desc_start;
            bool has_code_block = false;
            AstNode *code_body = NULL;

            while (!md_at_end(mp) && !md_at_section_boundary(mp, 3)) {
                /* Check for a code block */
                md_skip_line_whitespace(mp);
                if (md_starts_with(mp->source + mp->pos, "```limceron") ||
                    md_starts_with(mp->source + mp->pos, "```lceron")) {
                    /* Mark the end of description text before the code block */
                    desc_end = mp->pos;
                    /* Parse the code block through the Limceron parser */
                    AstNode *code_result = md_parse_code_block(mp);
                    if (code_result && code_result->params) {
                        has_code_block = true;
                        /* Build a block from the parsed statements */
                        AstNode *body = ast_new(mp->arena, AST_BLOCK, gloc);
                        body->params = code_result->params;
                        code_body = body;
                    }
                    continue;
                }
                md_read_line(mp);
                if (!has_code_block) {
                    desc_end = mp->pos;
                }
            }

            if (!has_code_block) {
                desc_end = mp->pos;
            }

            if (has_code_block && code_body) {
                /* Use the parsed code as the guard body (same as .lceron) */
                guard->left = code_body;

                /* Also store description if present */
                /* Trim trailing whitespace from description */
                size_t d_end = desc_end;
                while (d_end > desc_start &&
                       (mp->source[d_end - 1] == '\n' ||
                        mp->source[d_end - 1] == '\r' ||
                        mp->source[d_end - 1] == ' ')) {
                    d_end--;
                }
                if (d_end > desc_start) {
                    char *desc = arena_strndup(mp->arena, mp->source + desc_start,
                                               d_end - desc_start);
                    desc = md_trim(mp->arena, desc);
                    if (strlen(desc) > 0) {
                        /* Store description as an attribute on the guard for docs */
                        AstNode *desc_node = ast_new(mp->arena, AST_STRING_LIT, gloc);
                        desc_node->val.str_val = intern_get(mp->intern, desc, strlen(desc));
                        AstNode *desc_attr = ast_new(mp->arena, AST_FIELD, gloc);
                        desc_attr->name = arena_strdup(mp->arena, "description");
                        desc_attr->right = desc_node;
                        guard->right = desc_attr;
                    }
                }
            } else {
                /* No code block — use description-only behavior */
                /* Trim trailing whitespace from description */
                while (desc_end > desc_start &&
                       (mp->source[desc_end - 1] == '\n' ||
                        mp->source[desc_end - 1] == '\r' ||
                        mp->source[desc_end - 1] == ' ')) {
                    desc_end--;
                }

                if (desc_end > desc_start) {
                    char *desc = arena_strndup(mp->arena, mp->source + desc_start,
                                               desc_end - desc_start);
                    /* Store description as a prompt field on the guard's left child */
                    AstNode *desc_node = ast_new(mp->arena, AST_STRING_LIT, gloc);
                    desc_node->val.str_val = intern_get(mp->intern, desc, strlen(desc));

                    /* Wrap in a block as the guard body */
                    AstNode *body = ast_new(mp->arena, AST_BLOCK, gloc);
                    AstNode *desc_field = ast_new(mp->arena, AST_FIELD, gloc);
                    desc_field->name = arena_strdup(mp->arena, "description");
                    desc_field->right = desc_node;
                    body->params = desc_field;
                    guard->left = body;
                }
            }

            guard_list = ast_append(guard_list, guard);
        } else {
            /* Skip non-heading lines within guards section */
            md_read_line(mp);
        }
    }

    /* Build a guardset field: AST_FIELD(name="guards", right=AST_BLOCK of guards) */
    if (guard_list) {
        SourceLoc loc = md_loc(mp);
        AstNode *field = ast_new(mp->arena, AST_FIELD, loc);
        field->name = arena_strdup(mp->arena, "guards");

        AstNode *block = ast_new(mp->arena, AST_BLOCK, loc);
        block->params = guard_list;
        field->right = block;

        *members_tail = ast_append(*members_tail, field);
    }

    return guard_list;
}

/*
 * Parse a tool signature from a ### heading line.
 * Format: "tool_name(param: Type, ...) -> RetType"
 *
 * Returns AST_TOOL node.
 */
static AstNode *md_parse_tool_signature(MdParser *mp, const char *sig) {
    SourceLoc loc = md_loc(mp);
    loc.line = mp->line > 0 ? mp->line - 1 : mp->line;

    AstNode *tool = ast_new(mp->arena, AST_TOOL, loc);

    /* Find '(' to separate name from params */
    const char *paren = strchr(sig, '(');
    if (!paren) {
        /* No parameters — just a name */
        char *name = md_trim(mp->arena, sig);
        tool->name = intern_get(mp->intern, name, strlen(name));
        return tool;
    }

    /* Extract tool name */
    size_t name_len = (size_t)(paren - sig);
    char *name = arena_strndup(mp->arena, sig, name_len);
    name = md_trim(mp->arena, name);
    tool->name = intern_get(mp->intern, name, strlen(name));

    /* Parse parameters between ( and ) */
    const char *params_start = paren + 1;
    const char *params_end = strchr(params_start, ')');
    if (!params_end) {
        md_error(mp, "expected ')' in tool signature");
        return tool;
    }

    AstNode *param_list = NULL;

    if (params_end > params_start) {
        /* Parse comma-separated params: "name: Type, name2: Type2" */
        size_t params_len = (size_t)(params_end - params_start);
        char *params_str = arena_strndup(mp->arena, params_start, params_len);

        char *saveptr = NULL;
        char *param_tok = params_str;
        char *piece;

        /* Manual comma splitting (strtok_r not portable in C99 pedantic) */
        while (*param_tok != '\0') {
            /* Find next comma or end */
            piece = param_tok;
            char *comma = strchr(param_tok, ',');
            size_t piece_len;
            if (comma) {
                piece_len = (size_t)(comma - param_tok);
                param_tok = comma + 1;
            } else {
                piece_len = strlen(param_tok);
                param_tok = param_tok + piece_len;
            }

            char *param_piece = arena_strndup(mp->arena, piece, piece_len);
            param_piece = md_trim(mp->arena, param_piece);
            if (strlen(param_piece) == 0) continue;

            /* Split on ':' for name and type */
            const char *col = strchr(param_piece, ':');
            AstNode *param = ast_new(mp->arena, AST_PARAM, loc);

            if (col) {
                size_t pname_len = (size_t)(col - param_piece);
                char *pname = arena_strndup(mp->arena, param_piece, pname_len);
                pname = md_trim(mp->arena, pname);
                param->name = intern_get(mp->intern, pname, strlen(pname));

                const char *type_str = col + 1;
                char *type_name = md_trim(mp->arena, type_str);
                if (strlen(type_name) > 0) {
                    AstNode *type_node = ast_new(mp->arena, AST_TYPE_NAMED, loc);
                    type_node->name = intern_get(mp->intern, type_name, strlen(type_name));
                    param->type_expr = type_node;
                }
            } else {
                param->name = intern_get(mp->intern, param_piece, strlen(param_piece));
            }

            param_list = ast_append(param_list, param);
        }

        (void)saveptr;
    }

    tool->params = param_list;

    /* Check for return type: -> Type */
    const char *arrow = strstr(params_end + 1, "->");
    if (arrow) {
        const char *ret_start = arrow + 2;
        char *ret_type = md_trim(mp->arena, ret_start);
        if (strlen(ret_type) > 0) {
            AstNode *ret_node = ast_new(mp->arena, AST_TYPE_NAMED, loc);
            ret_node->name = intern_get(mp->intern, ret_type, strlen(ret_type));
            tool->type_expr = ret_node;
        }
    }

    return tool;
}

/*
 * Parse a single tool declaration under ## tools:
 *   ### search_kb(query: string) -> Result
 *   Searches the knowledge base.
 *   requires: [kb.search]
 *
 * Also parses embedded code blocks.
 */
static AstNode *md_parse_tool_decl(MdParser *mp, const char *header_line,
                                    AstNode **fns_tail) {
    /* Skip "### " prefix */
    const char *sig_start = header_line;
    while (*sig_start == '#') sig_start++;
    while (*sig_start == ' ') sig_start++;

    AstNode *tool = md_parse_tool_signature(mp, sig_start);

    /* Collect body lines: description, requires, and code blocks */
    AstNode *body_fields = NULL;
    size_t desc_start = 0;
    size_t desc_end = 0;
    bool has_desc = false;

    md_skip_blank_lines(mp);

    while (!md_at_end(mp) && !md_at_section_boundary(mp, 3)) {
        /* Check for code block */
        md_skip_line_whitespace(mp);

        if (md_starts_with(mp->source + mp->pos, "```limceron") ||
            md_starts_with(mp->source + mp->pos, "```lceron")) {
            /* Parse code block through Limceron lexer/parser */
            AstNode *code_result = md_parse_code_block(mp);
            if (code_result && fns_tail) {
                /* Merge function declarations from code block */
                AstNode *decl = code_result->params;
                while (decl) {
                    AstNode *next = decl->next;
                    decl->next = NULL;
                    if (decl->kind == AST_FN) {
                        *fns_tail = ast_append(*fns_tail, decl);
                    }
                    decl = next;
                }
            }
            continue;
        }

        /* Check for "requires: [...]" line */
        size_t save_pos = mp->pos;
        char *line = md_read_line(mp);
        char *trimmed = md_trim(mp->arena, line);

        if (strlen(trimmed) == 0) continue;

        if (md_starts_with(trimmed, "requires:")) {
            const char *req_val = trimmed + 9;
            while (*req_val == ' ') req_val++;

            /* Parse [cap1, cap2, ...] */
            if (*req_val == '[') {
                req_val++;
                const char *bracket_end = strchr(req_val, ']');
                if (!bracket_end) {
                    md_error(mp, "expected ']' in requires list");
                    continue;
                }

                SourceLoc rloc = md_loc(mp);
                rloc.line = mp->line - 1;
                AstNode *req_field = ast_new(mp->arena, AST_FIELD, rloc);
                req_field->name = arena_strdup(mp->arena, "requires");

                AstNode *req_arr = ast_new(mp->arena, AST_ARRAY, rloc);
                AstNode *req_elems = NULL;

                size_t list_len = (size_t)(bracket_end - req_val);
                char *list_str = arena_strndup(mp->arena, req_val, list_len);

                /* Split by comma */
                char *tok_ptr = list_str;
                while (*tok_ptr != '\0') {
                    char *comma = strchr(tok_ptr, ',');
                    size_t tok_len;
                    if (comma) {
                        tok_len = (size_t)(comma - tok_ptr);
                    } else {
                        tok_len = strlen(tok_ptr);
                    }

                    char *cap = arena_strndup(mp->arena, tok_ptr, tok_len);
                    cap = md_trim(mp->arena, cap);
                    if (strlen(cap) > 0) {
                        AstNode *cap_node = ast_new(mp->arena, AST_IDENT, rloc);
                        cap_node->name = intern_get(mp->intern, cap, strlen(cap));
                        req_elems = ast_append(req_elems, cap_node);
                    }

                    if (comma) {
                        tok_ptr = comma + 1;
                    } else {
                        break;
                    }
                }

                req_arr->params = req_elems;
                req_field->right = req_arr;
                body_fields = ast_append(body_fields, req_field);
            }
            continue;
        }

        /* Otherwise it's description text */
        if (!has_desc) {
            desc_start = save_pos;
            has_desc = true;
        }
        desc_end = mp->pos;
    }

    /* Store description as a field */
    if (has_desc && desc_end > desc_start) {
        /* Trim trailing whitespace */
        while (desc_end > desc_start &&
               (mp->source[desc_end - 1] == '\n' ||
                mp->source[desc_end - 1] == '\r' ||
                mp->source[desc_end - 1] == ' ')) {
            desc_end--;
        }
        char *desc = arena_strndup(mp->arena, mp->source + desc_start,
                                   desc_end - desc_start);
        desc = md_trim(mp->arena, desc);
        if (strlen(desc) > 0) {
            SourceLoc dloc = md_loc(mp);
            AstNode *desc_field = ast_new(mp->arena, AST_FIELD, dloc);
            desc_field->name = arena_strdup(mp->arena, "description");
            AstNode *desc_node = ast_new(mp->arena, AST_STRING_LIT, dloc);
            desc_node->val.str_val = intern_get(mp->intern, desc, strlen(desc));
            desc_field->right = desc_node;
            body_fields = ast_append(body_fields, desc_field);
        }
    }

    /* Attach fields to tool */
    tool->right = body_fields;

    return tool;
}

/*
 * Parse tools section:
 *   ## tools
 *   ### tool_name(param: Type) -> RetType
 *   ...
 *
 * Appends tool nodes to members, and any code-block fn decls to fns.
 */
static AstNode *md_parse_tools(MdParser *mp, AstNode **fns_tail) {
    AstNode *tool_list = NULL;

    md_skip_blank_lines(mp);

    while (!md_at_end(mp) && !md_at_section_boundary(mp, 2)) {
        if (md_is_heading_at_level(mp, 3)) {
            char *line = md_read_line(mp);
            AstNode *tool = md_parse_tool_decl(mp, line, fns_tail);
            if (tool) {
                tool_list = ast_append(tool_list, tool);
            }
        } else {
            /* Check for standalone code blocks outside tool subsections */
            md_skip_line_whitespace(mp);
            if (md_starts_with(mp->source + mp->pos, "```limceron") ||
                md_starts_with(mp->source + mp->pos, "```lceron")) {
                AstNode *code_result = md_parse_code_block(mp);
                if (code_result && fns_tail) {
                    AstNode *decl = code_result->params;
                    while (decl) {
                        AstNode *next = decl->next;
                        decl->next = NULL;
                        if (decl->kind == AST_FN) {
                            *fns_tail = ast_append(*fns_tail, decl);
                        }
                        decl = next;
                    }
                }
            } else {
                md_read_line(mp);
            }
        }
    }

    return tool_list;
}

/*
 * Parse a fenced code block:
 *   ```limceron
 *   fn run(topic: string) -> Result { ... }
 *   ```
 *
 * Feeds the extracted code through the Limceron lexer and parser
 * to produce real AST nodes.
 */
static AstNode *md_parse_code_block(MdParser *mp) {
    /* We're at the ``` line — read it to skip the fence */
    char *fence_line = md_read_line(mp);
    (void)fence_line;

    /* Collect code lines until closing ``` */
    size_t code_start = mp->pos;
    uint32_t code_start_line = mp->line;

    while (!md_at_end(mp)) {
        md_skip_line_whitespace(mp);
        if (md_starts_with(mp->source + mp->pos, "```")) {
            break;
        }
        /* Skip this line */
        while (!md_at_end(mp) && md_peek(mp) != '\n') {
            mp->pos++;
        }
        if (!md_at_end(mp)) {
            mp->pos++;
            mp->line++;
        }
    }

    size_t code_end = mp->pos;

    /* Skip closing ``` */
    if (!md_at_end(mp) && md_starts_with(mp->source + mp->pos, "```")) {
        md_read_line(mp);
    }

    if (code_end <= code_start) {
        return NULL;
    }

    /* Trim trailing whitespace from code */
    while (code_end > code_start &&
           (mp->source[code_end - 1] == '\n' ||
            mp->source[code_end - 1] == '\r' ||
            mp->source[code_end - 1] == ' ')) {
        code_end--;
    }

    size_t code_len = code_end - code_start;
    if (code_len == 0) return NULL;

    /* Copy code into a null-terminated buffer */
    char *code = arena_strndup(mp->arena, mp->source + code_start, code_len);

    /* Feed through the Limceron lexer/parser */
    ErrorReporter sub_reporter = reporter_new(mp->filename, code, code_len);
    /* Adjust line numbers to match position in the markdown file */
    (void)code_start_line;

    Lexer lexer = lexer_new(mp->filename, code, code_len, mp->intern, &sub_reporter);
    Parser parser = parser_new(&lexer, mp->arena, &sub_reporter);
    AstNode *sub_program = parse_program(&parser);

    if (parser.had_error) {
        /* Forward errors from sub-parse */
        int i;
        for (i = 0; i < sub_reporter.count && i < MAX_ERRORS; i++) {
            SourceLoc eloc = sub_reporter.errors[i].loc;
            /* Adjust line numbers to be relative to the markdown file */
            eloc.line += code_start_line - 1;
            report_error(mp->reporter, eloc,
                        sub_reporter.errors[i].message,
                        sub_reporter.errors[i].hint);
        }
        mp->had_error = true;
    }

    return sub_program;
}

/* ============================================================
 * Capability Declaration Parser (## capability <name>)
 * ============================================================ */

/*
 * Parse a capability declaration section:
 *   ## capability llm
 *   - complete
 *   - classify requires complete
 *   - embed
 *
 *   ## capability network
 *   allow endpoint "api.example.com:443" { method: [GET, POST] }
 *   deny private_ranges
 *   default: deny
 *
 * Returns: AST_CAPABILITY node with name and child items.
 * Items may be AST_CAPABILITY_ITEM (abstract) or access control rule nodes.
 */
static AstNode *md_parse_capability_decl(MdParser *mp, const char *cap_name) {
    SourceLoc loc = md_loc(mp);
    AstNode *node = ast_new(mp->arena, AST_CAPABILITY, loc);
    node->name = intern_get(mp->intern, cap_name, strlen(cap_name));

    AstNode *items = NULL;

    md_skip_blank_lines(mp);

    while (!md_at_end(mp) && !md_at_section_boundary(mp, 2)) {
        char *line = md_read_line(mp);
        char *trimmed = md_trim(mp->arena, line);

        if (strlen(trimmed) == 0) continue;

        SourceLoc iloc = md_loc(mp);
        iloc.line = mp->line > 0 ? mp->line - 1 : mp->line;

        /* allow endpoint/binary/path rule */
        if (md_starts_with(trimmed, "allow ")) {
            const char *rest = trimmed + 6;
            char *kind_end = (char *)rest;
            while (*kind_end && *kind_end != ' ') kind_end++;
            size_t kind_len = (size_t)(kind_end - rest);
            char *kind = arena_strndup(mp->arena, rest, kind_len);

            if (strcmp(kind, "endpoint") == 0) {
                AstNode *rule = ast_new(mp->arena, AST_CAP_ENDPOINT_RULE, iloc);
                rule->is_mut = true; /* allow */
                /* Extract quoted value */
                const char *q1 = strchr(kind_end, '"');
                if (q1) {
                    q1++;
                    const char *q2 = strchr(q1, '"');
                    if (q2) {
                        char *val = arena_strndup(mp->arena, q1, (size_t)(q2 - q1));
                        rule->name = intern_get(mp->intern, val, strlen(val));
                    }
                }
                items = ast_append(items, rule);
            } else if (strcmp(kind, "binary") == 0) {
                AstNode *rule = ast_new(mp->arena, AST_CAP_BINARY_RULE, iloc);
                rule->is_mut = true; /* allow */
                const char *q1 = strchr(kind_end, '"');
                if (q1) {
                    q1++;
                    const char *q2 = strchr(q1, '"');
                    if (q2) {
                        char *val = arena_strndup(mp->arena, q1, (size_t)(q2 - q1));
                        rule->name = intern_get(mp->intern, val, strlen(val));
                    }
                }
                items = ast_append(items, rule);
            } else if (strcmp(kind, "path") == 0) {
                AstNode *rule = ast_new(mp->arena, AST_CAP_PATH_RULE, iloc);
                rule->is_mut = true; /* allow */
                const char *q1 = strchr(kind_end, '"');
                if (q1) {
                    q1++;
                    const char *q2 = strchr(q1, '"');
                    if (q2) {
                        char *val = arena_strndup(mp->arena, q1, (size_t)(q2 - q1));
                        rule->name = intern_get(mp->intern, val, strlen(val));
                    }
                }
                items = ast_append(items, rule);
            } else {
                md_error_fmt(mp, "unknown allow rule kind: %s", kind);
            }
            continue;
        }

        /* deny endpoint/binary/path/private_ranges rule */
        if (md_starts_with(trimmed, "deny ")) {
            const char *rest = trimmed + 5;
            char *kind_end = (char *)rest;
            while (*kind_end && *kind_end != ' ') kind_end++;
            size_t kind_len = (size_t)(kind_end - rest);
            char *kind = arena_strndup(mp->arena, rest, kind_len);

            if (strcmp(kind, "private_ranges") == 0) {
                AstNode *rule = ast_new(mp->arena, AST_CAP_DENY_RANGE, iloc);
                rule->is_mut = false; /* deny */
                rule->name = intern_get(mp->intern, "private_ranges", 14);
                items = ast_append(items, rule);
            } else if (strcmp(kind, "endpoint") == 0) {
                AstNode *rule = ast_new(mp->arena, AST_CAP_ENDPOINT_RULE, iloc);
                rule->is_mut = false; /* deny */
                const char *q1 = strchr(kind_end, '"');
                if (q1) {
                    q1++;
                    const char *q2 = strchr(q1, '"');
                    if (q2) {
                        char *val = arena_strndup(mp->arena, q1, (size_t)(q2 - q1));
                        rule->name = intern_get(mp->intern, val, strlen(val));
                    }
                }
                items = ast_append(items, rule);
            } else if (strcmp(kind, "binary") == 0) {
                AstNode *rule = ast_new(mp->arena, AST_CAP_BINARY_RULE, iloc);
                rule->is_mut = false; /* deny */
                const char *q1 = strchr(kind_end, '"');
                if (q1) {
                    q1++;
                    const char *q2 = strchr(q1, '"');
                    if (q2) {
                        char *val = arena_strndup(mp->arena, q1, (size_t)(q2 - q1));
                        rule->name = intern_get(mp->intern, val, strlen(val));
                    }
                }
                items = ast_append(items, rule);
            } else if (strcmp(kind, "path") == 0) {
                AstNode *rule = ast_new(mp->arena, AST_CAP_PATH_RULE, iloc);
                rule->is_mut = false; /* deny */
                const char *q1 = strchr(kind_end, '"');
                if (q1) {
                    q1++;
                    const char *q2 = strchr(q1, '"');
                    if (q2) {
                        char *val = arena_strndup(mp->arena, q1, (size_t)(q2 - q1));
                        rule->name = intern_get(mp->intern, val, strlen(val));
                    }
                }
                items = ast_append(items, rule);
            } else {
                md_error_fmt(mp, "unknown deny rule kind: %s", kind);
            }
            continue;
        }

        /* default: allow | default: deny */
        if (md_starts_with(trimmed, "default:")) {
            const char *val_str = trimmed + 8;
            char *dval = md_trim(mp->arena, val_str);
            AstNode *def = ast_new(mp->arena, AST_CAP_DEFAULT, iloc);
            if (strcmp(dval, "allow") == 0) {
                def->is_mut = true;  /* default allow */
            } else {
                def->is_mut = false; /* default deny */
            }
            items = ast_append(items, def);
            continue;
        }

        /* List item: "- name" or "- name requires dep1, dep2" */
        if (md_starts_with(trimmed, "- ")) {
            const char *item_text = md_trim(mp->arena, trimmed + 2);
            if (strlen(item_text) == 0) continue;

            AstNode *item = ast_new(mp->arena, AST_CAPABILITY_ITEM, iloc);

            /* Check for "requires" clause */
            const char *req = strstr(item_text, " requires ");
            if (req) {
                /* Name is everything before " requires " */
                size_t name_len = (size_t)(req - item_text);
                char *iname = arena_strndup(mp->arena, item_text, name_len);
                iname = md_trim(mp->arena, iname);
                item->name = intern_get(mp->intern, iname, strlen(iname));

                /* Dependencies after "requires " */
                const char *deps_str = req + 10; /* skip " requires " */
                AstNode *deps = NULL;
                /* Split by comma */
                while (*deps_str != '\0') {
                    const char *comma = strchr(deps_str, ',');
                    size_t dep_len;
                    if (comma) {
                        dep_len = (size_t)(comma - deps_str);
                    } else {
                        dep_len = strlen(deps_str);
                    }
                    char *dep_name = arena_strndup(mp->arena, deps_str, dep_len);
                    dep_name = md_trim(mp->arena, dep_name);
                    if (strlen(dep_name) > 0) {
                        AstNode *dep = ast_new(mp->arena, AST_IDENT, iloc);
                        dep->name = intern_get(mp->intern, dep_name, strlen(dep_name));
                        deps = ast_append(deps, dep);
                    }
                    if (comma) {
                        deps_str = comma + 1;
                    } else {
                        break;
                    }
                }
                item->params = deps;
            } else {
                item->name = intern_get(mp->intern, item_text, strlen(item_text));
            }

            items = ast_append(items, item);
            continue;
        }

        /* Unknown line — skip with warning */
        md_error_fmt(mp, "unexpected line in capability declaration: %s", trimmed);
    }

    node->params = items;
    return node;
}

/*
 * Parse taint section:
 *   ## taint
 *   - user_input
 *   - llm_output
 *   - sanitized
 *
 * Returns: A list of AST_TAINT nodes (one per taint label).
 * Caller should append each to program->params as top-level declarations.
 */
static AstNode *md_parse_taint(MdParser *mp) {
    AstNode *taint_list = NULL;

    md_skip_blank_lines(mp);

    while (!md_at_end(mp) && !md_at_section_boundary(mp, 2)) {
        char *line = md_read_line(mp);
        char *trimmed = md_trim(mp->arena, line);

        if (strlen(trimmed) == 0) continue;

        /* Must start with '- ' */
        if (!md_starts_with(trimmed, "- ")) {
            md_error_fmt(mp, "expected '- taint_name' in taint section, got: %s",
                         trimmed);
            continue;
        }

        const char *taint_name = md_trim(mp->arena, trimmed + 2);
        if (strlen(taint_name) == 0) continue;

        SourceLoc tloc = md_loc(mp);
        tloc.line = mp->line > 0 ? mp->line - 1 : mp->line;
        AstNode *taint = ast_new(mp->arena, AST_TAINT, tloc);
        taint->name = intern_get(mp->intern, taint_name, strlen(taint_name));

        taint_list = ast_append(taint_list, taint);
    }

    return taint_list;
}

/* ============================================================
 * Health Probe Parser (## health)
 * ============================================================ */

/*
 * Parse health section:
 *   ## health
 *   - ready: true
 *   - live: true
 *   - port: 9090
 *
 * Returns: AST_HEALTH node
 *   left  = ready expression (AST_BOOL_LIT or AST_IDENT)
 *   right = live expression  (AST_BOOL_LIT or AST_IDENT)
 *   val.int_val = port number (default 9090)
 *   params = raw field list
 */
static AstNode *md_parse_health(MdParser *mp) {
    SourceLoc loc = md_loc(mp);
    AstNode *node = ast_new(mp->arena, AST_HEALTH, loc);
    node->val.int_val = 9090;  /* default port */

    AstNode *fields = NULL;

    md_skip_blank_lines(mp);

    while (!md_at_end(mp) && !md_at_section_boundary(mp, 2)) {
        char *line = md_read_line(mp);
        char *trimmed = md_trim(mp->arena, line);

        if (strlen(trimmed) == 0) continue;

        /* Must start with '- ' */
        if (!md_starts_with(trimmed, "- ")) continue;

        /* Parse "key: value" after the "- " */
        const char *kv = trimmed + 2;
        const char *colon = strchr(kv, ':');
        if (!colon) continue;

        size_t key_len = (size_t)(colon - kv);
        char *key = arena_strndup(mp->arena, kv, key_len);
        key = md_trim(mp->arena, key);

        const char *val_str = colon + 1;
        char *value = md_trim(mp->arena, val_str);

        SourceLoc floc = loc;
        floc.line = mp->line - 1;

        AstNode *field = ast_new(mp->arena, AST_FIELD, floc);
        field->name = intern_get(mp->intern, key, strlen(key));

        /* Parse value: boolean, integer, or identifier */
        if (strcmp(value, "true") == 0) {
            AstNode *val_node = ast_new(mp->arena, AST_BOOL_LIT, floc);
            val_node->val.bool_val = true;
            field->right = val_node;
        } else if (strcmp(value, "false") == 0) {
            AstNode *val_node = ast_new(mp->arena, AST_BOOL_LIT, floc);
            val_node->val.bool_val = false;
            field->right = val_node;
        } else {
            char *endptr = NULL;
            double dval = strtod(value, &endptr);
            if (endptr && endptr != value && *endptr == '\0') {
                AstNode *int_node = ast_new(mp->arena, AST_INT_LIT, floc);
                int_node->val.int_val = (int64_t)dval;
                field->right = int_node;
            } else {
                AstNode *id_node = ast_new(mp->arena, AST_IDENT, floc);
                id_node->name = intern_get(mp->intern, value, strlen(value));
                field->right = id_node;
            }
        }

        /* Set node-level fields based on key */
        if (strcmp(key, "ready") == 0) {
            node->left = field->right;
        } else if (strcmp(key, "live") == 0) {
            node->right = field->right;
        } else if (strcmp(key, "port") == 0) {
            if (field->right && field->right->kind == AST_INT_LIT) {
                node->val.int_val = field->right->val.int_val;
            }
        }

        fields = ast_append(fields, field);
    }

    node->params = fields;
    return node;
}

/* ============================================================
 * Metrics Parser (## metrics)
 * ============================================================ */

/*
 * Parse metrics section:
 *   ## metrics
 *   - counter processed_total "Records processed"
 *   - histogram confidence "Confidence distribution"
 *   - gauge pending "Records pending"
 *   - port: 9091
 *
 * Returns: AST_METRICS node
 *   params = linked list of AST_METRICS_FIELD nodes
 *   val.int_val = port number (default 9091)
 *
 * AST_METRICS_FIELD:
 *   name = metric name
 *   val.str_val = description string
 *   is_mut = true for gauge
 *   is_pub = true for histogram
 */
static AstNode *md_parse_metrics(MdParser *mp) {
    SourceLoc loc = md_loc(mp);
    AstNode *node = ast_new(mp->arena, AST_METRICS, loc);
    node->val.int_val = 9091;  /* default port */

    AstNode *fields = NULL;

    md_skip_blank_lines(mp);

    while (!md_at_end(mp) && !md_at_section_boundary(mp, 2)) {
        char *line = md_read_line(mp);
        char *trimmed = md_trim(mp->arena, line);

        if (strlen(trimmed) == 0) continue;

        /* Must start with '- ' */
        if (!md_starts_with(trimmed, "- ")) continue;

        const char *content = trimmed + 2;

        SourceLoc floc = loc;
        floc.line = mp->line - 1;

        /* Check for "port: N" */
        if (md_starts_with(content, "port:")) {
            const char *port_val = content + 5;
            char *pv = md_trim(mp->arena, port_val);
            char *endptr = NULL;
            long port = strtol(pv, &endptr, 10);
            if (endptr && endptr != pv && *endptr == '\0') {
                node->val.int_val = (int64_t)port;
            }
            continue;
        }

        /* Parse "type name \"description\"" */
        /* Find first space to get type */
        const char *sp1 = strchr(content, ' ');
        if (!sp1) continue;

        size_t type_len = (size_t)(sp1 - content);
        char *type_str = arena_strndup(mp->arena, content, type_len);

        const char *rest = sp1 + 1;
        while (*rest == ' ') rest++;

        /* Find the metric name (next token before quote or space) */
        const char *sp2 = strchr(rest, ' ');
        const char *quote = strchr(rest, '"');
        const char *name_end = sp2;
        if (!name_end || (quote && quote < name_end)) name_end = quote;
        if (!name_end) name_end = rest + strlen(rest);

        size_t name_len = (size_t)(name_end - rest);
        char *metric_name = arena_strndup(mp->arena, rest, name_len);
        metric_name = md_trim(mp->arena, metric_name);

        if (strlen(metric_name) == 0) continue;

        AstNode *mf = ast_new(mp->arena, AST_METRICS_FIELD, floc);
        mf->name = intern_get(mp->intern, metric_name, strlen(metric_name));

        if (strcmp(type_str, "gauge") == 0)     mf->is_mut = true;
        if (strcmp(type_str, "histogram") == 0) mf->is_pub = true;

        /* Extract description from quotes */
        if (quote) {
            const char *desc_start = quote + 1;
            const char *desc_end = strchr(desc_start, '"');
            if (desc_end) {
                size_t desc_len = (size_t)(desc_end - desc_start);
                char *desc = arena_strndup(mp->arena, desc_start, desc_len);
                mf->val.str_val = intern_get(mp->intern, desc, strlen(desc));
            }
        }

        fields = ast_append(fields, mf);
    }

    node->params = fields;
    return node;
}

/* ============================================================
 * Signal Parser (## signal)
 * ============================================================ */

/*
 * Parse signal section:
 *   ## signal
 *   SIGTERM
 *
 * Optionally followed by a code block for the handler body.
 *
 * Returns: AST_SIGNAL node
 *   name = signal name (e.g. "SIGTERM", "SIGINT")
 *   left = handler body (if code block present)
 */
static AstNode *md_parse_signal(MdParser *mp) {
    SourceLoc loc = md_loc(mp);
    AstNode *node = ast_new(mp->arena, AST_SIGNAL, loc);

    md_skip_blank_lines(mp);

    /* Read the signal name */
    char *sig_name = NULL;
    while (!md_at_end(mp) && !md_at_section_boundary(mp, 2)) {
        md_skip_line_whitespace(mp);

        /* Check for code block */
        if (md_starts_with(mp->source + mp->pos, "```limceron") ||
            md_starts_with(mp->source + mp->pos, "```lceron")) {
            AstNode *code_result = md_parse_code_block(mp);
            if (code_result) {
                node->left = code_result;
            }
            continue;
        }

        char *line = md_read_line(mp);
        char *trimmed = md_trim(mp->arena, line);

        if (strlen(trimmed) == 0) continue;

        if (!sig_name) {
            sig_name = trimmed;
        }
    }

    if (sig_name) {
        node->name = intern_get(mp->intern, sig_name, strlen(sig_name));
    } else {
        node->name = intern_get(mp->intern, "SIGTERM", 7);
    }

    return node;
}

/* ============================================================
 * Progress Parser (## progress)
 * ============================================================ */

/*
 * Parse progress section:
 *   ## progress
 *   - total: count
 *   - current: processed
 *
 * Returns: AST_PROGRESS node
 *   left  = total expression (AST_IDENT)
 *   right = current expression (AST_IDENT)
 */
static AstNode *md_parse_progress(MdParser *mp) {
    SourceLoc loc = md_loc(mp);
    AstNode *node = ast_new(mp->arena, AST_PROGRESS, loc);

    md_skip_blank_lines(mp);

    while (!md_at_end(mp) && !md_at_section_boundary(mp, 2)) {
        char *line = md_read_line(mp);
        char *trimmed = md_trim(mp->arena, line);

        if (strlen(trimmed) == 0) continue;

        /* Must start with '- ' */
        if (!md_starts_with(trimmed, "- ")) continue;

        /* Parse "key: value" */
        const char *kv = trimmed + 2;
        const char *colon = strchr(kv, ':');
        if (!colon) continue;

        size_t key_len = (size_t)(colon - kv);
        char *key = arena_strndup(mp->arena, kv, key_len);
        key = md_trim(mp->arena, key);

        const char *val_str = colon + 1;
        char *value = md_trim(mp->arena, val_str);

        SourceLoc floc = loc;
        floc.line = mp->line - 1;

        AstNode *val_node = ast_new(mp->arena, AST_IDENT, floc);
        val_node->name = intern_get(mp->intern, value, strlen(value));

        if (strcmp(key, "total") == 0) {
            node->left = val_node;
        } else if (strcmp(key, "current") == 0) {
            node->right = val_node;
        }
    }

    return node;
}

/* ============================================================
 * Supervisor Parser (## supervisor <Name>)
 * ============================================================ */

/*
 * Parse supervisor section:
 *   ## supervisor PipelineManager
 *   - strategy: one_for_one
 *   - max_restarts: 3
 *   - window: 60
 *   - children: [AgentA, AgentB]
 *
 * Returns: AST_SUPERVISOR node
 *   name = supervisor name
 *   params = linked list of AST_FIELD nodes
 *     For children: right = AST_ARRAY of AST_IDENT
 *     For others: right = AST_INT_LIT, AST_IDENT, or AST_STRING_LIT
 */
static AstNode *md_parse_supervisor(MdParser *mp, const char *section_name) {
    SourceLoc loc = md_loc(mp);
    AstNode *node = ast_new(mp->arena, AST_SUPERVISOR, loc);

    /* Extract supervisor name from section heading (after "supervisor ") */
    const char *name_start = section_name + 10;  /* skip "supervisor" */
    while (*name_start == ' ') name_start++;
    if (strlen(name_start) > 0) {
        char *name = md_trim(mp->arena, name_start);
        node->name = intern_get(mp->intern, name, strlen(name));
    } else {
        node->name = intern_get(mp->intern, "Unnamed", 7);
    }

    AstNode *fields = NULL;

    md_skip_blank_lines(mp);

    while (!md_at_end(mp) && !md_at_section_boundary(mp, 2)) {
        char *line = md_read_line(mp);
        char *trimmed = md_trim(mp->arena, line);

        if (strlen(trimmed) == 0) continue;

        /* Must start with '- ' */
        if (!md_starts_with(trimmed, "- ")) continue;

        /* Parse "key: value" */
        const char *kv = trimmed + 2;
        const char *colon = strchr(kv, ':');
        if (!colon) continue;

        size_t key_len = (size_t)(colon - kv);
        char *key = arena_strndup(mp->arena, kv, key_len);
        key = md_trim(mp->arena, key);

        const char *val_str = colon + 1;
        char *value = md_trim(mp->arena, val_str);

        SourceLoc floc = loc;
        floc.line = mp->line - 1;

        AstNode *sub_field = ast_new(mp->arena, AST_FIELD, floc);
        sub_field->name = intern_get(mp->intern, key, strlen(key));

        /* Handle children: [AgentA, AgentB] */
        if (strcmp(key, "children") == 0 && value[0] == '[') {
            AstNode *arr = ast_new(mp->arena, AST_ARRAY, floc);
            AstNode *elems = NULL;

            /* Find closing bracket */
            const char *bracket_start = value + 1;
            const char *bracket_end = strchr(bracket_start, ']');
            if (!bracket_end) bracket_end = bracket_start + strlen(bracket_start);

            size_t list_len = (size_t)(bracket_end - bracket_start);
            char *list_str = arena_strndup(mp->arena, bracket_start, list_len);

            /* Split by comma */
            char *tok_ptr = list_str;
            while (*tok_ptr != '\0') {
                char *comma = strchr(tok_ptr, ',');
                size_t tok_len;
                if (comma) {
                    tok_len = (size_t)(comma - tok_ptr);
                } else {
                    tok_len = strlen(tok_ptr);
                }

                char *child_name = arena_strndup(mp->arena, tok_ptr, tok_len);
                child_name = md_trim(mp->arena, child_name);
                if (strlen(child_name) > 0) {
                    AstNode *child = ast_new(mp->arena, AST_IDENT, floc);
                    child->name = intern_get(mp->intern, child_name, strlen(child_name));
                    elems = ast_append(elems, child);
                }

                if (comma) {
                    tok_ptr = comma + 1;
                } else {
                    break;
                }
            }

            arr->params = elems;
            sub_field->right = arr;
        } else {
            /* Try to parse value as number */
            char *endptr = NULL;
            double dval = strtod(value, &endptr);
            if (endptr && endptr != value && *endptr == '\0') {
                if (strchr(value, '.') == NULL) {
                    AstNode *int_node = ast_new(mp->arena, AST_INT_LIT, floc);
                    int_node->val.int_val = (int64_t)dval;
                    sub_field->right = int_node;
                } else {
                    AstNode *float_node = ast_new(mp->arena, AST_FLOAT_LIT, floc);
                    float_node->val.float_val = dval;
                    sub_field->right = float_node;
                }
            } else {
                /* Store as identifier (for strategy: one_for_one, etc.) */
                AstNode *id_node = ast_new(mp->arena, AST_IDENT, floc);
                id_node->name = intern_get(mp->intern, value, strlen(value));
                sub_field->right = id_node;
            }
        }

        fields = ast_append(fields, sub_field);
    }

    node->params = fields;
    return node;
}

/* ============================================================
 * System Prompt Parser
 * ============================================================ */

/*
 * Parse blockquote lines as system prompt:
 *   > You help customers with their questions.
 *   > Multi-line prompt continues here.
 *
 * Returns: AST_FIELD(name="prompt", right=AST_STRING_LIT)
 */
static AstNode *md_parse_prompt(MdParser *mp) {
    SourceLoc loc = md_loc(mp);

    /* Collect all consecutive blockquote lines */
    size_t buf_cap = 1024;
    size_t buf_len = 0;
    char *buf = (char *)arena_alloc(mp->arena, buf_cap);

    while (!md_at_end(mp)) {
        /* Check if line starts with '>' */
        md_skip_line_whitespace(mp);
        if (md_at_end(mp) || md_peek(mp) != '>') break;

        size_t line_start = mp->pos;
        char *line = md_read_line(mp);
        (void)line_start;

        /* Strip leading '> ' or '>' */
        const char *content = line;
        if (*content == '>') content++;
        if (*content == ' ') content++;

        size_t content_len = strlen(content);

        /* Append to buffer with newline separator */
        if (buf_len > 0) {
            if (buf_len + 1 < buf_cap) {
                buf[buf_len] = '\n';
                buf_len++;
            }
        }
        if (buf_len + content_len < buf_cap) {
            memcpy(buf + buf_len, content, content_len);
            buf_len += content_len;
        }
    }
    buf[buf_len] = '\0';

    if (buf_len == 0) return NULL;

    char *prompt_text = md_trim(mp->arena, buf);
    if (strlen(prompt_text) == 0) return NULL;

    AstNode *field = ast_new(mp->arena, AST_FIELD, loc);
    field->name = arena_strdup(mp->arena, "prompt");

    AstNode *str_node = ast_new(mp->arena, AST_STRING_LIT, loc);
    str_node->val.str_val = intern_get(mp->intern, prompt_text, strlen(prompt_text));
    field->right = str_node;

    return field;
}

/* ============================================================
 * Top-Level Heading Parser
 * ============================================================ */

/*
 * Parse the `# agent Name` heading.
 * Returns the agent name, or NULL on error.
 */
static const char *md_parse_agent_heading(MdParser *mp) {
    md_skip_blank_lines(mp);

    if (md_at_end(mp)) {
        md_error(mp, "expected '# agent Name' heading");
        return NULL;
    }

    if (!md_is_heading_at_level(mp, 1)) {
        md_error(mp, "expected '# agent Name' as first heading");
        /* Try to recover by reading lines until we find it */
        while (!md_at_end(mp) && !md_is_heading_at_level(mp, 1)) {
            md_read_line(mp);
        }
        if (md_at_end(mp)) return NULL;
    }

    char *heading = md_read_line(mp);

    /* Skip "# " prefix */
    const char *text = heading;
    while (*text == '#') text++;
    while (*text == ' ') text++;

    /* Expect "agent Name" */
    if (!md_starts_with(text, "agent ")) {
        md_error_fmt(mp, "expected '# agent Name', got '# %s'", text);
        /* Use text as agent name anyway */
        char *name = md_trim(mp->arena, text);
        return intern_get(mp->intern, name, strlen(name));
    }

    const char *name_start = text + 6;  /* skip "agent " */
    char *name = md_trim(mp->arena, name_start);
    if (strlen(name) == 0) {
        md_error(mp, "agent name is empty");
        return intern_get(mp->intern, "Unnamed", 7);
    }

    return intern_get(mp->intern, name, strlen(name));
}

/* ============================================================
 * Main Entry Point
 * ============================================================ */

/*
 * Parse a .lceron.md file into the same AST as the .lceron parser would produce.
 *
 * Returns: AST_PROGRAM with AST_AGENT as child
 *          The agent node has:
 *            - name: agent name from heading
 *            - params: member fields (capabilities, model, budget, guards, tools)
 *            - left: function declarations (from code blocks)
 */
AstNode *parse_lceron_md(const char *filename, const char *source, size_t source_len,
                       Arena *arena, StringIntern *intern, ErrorReporter *reporter) {
    MdParser mp;
    mp.filename = filename;
    mp.source = source;
    mp.source_len = source_len;
    mp.pos = 0;
    mp.line = 1;
    mp.arena = arena;
    mp.intern = intern;
    mp.reporter = reporter;
    mp.had_error = false;

    SourceLoc prog_loc;
    prog_loc.filename = filename;
    prog_loc.line = 1;
    prog_loc.column = 1;
    prog_loc.offset = 0;

    AstNode *program = ast_new(arena, AST_PROGRAM, prog_loc);

    /* Parse "# agent Name" heading */
    const char *agent_name = md_parse_agent_heading(&mp);
    if (!agent_name) {
        return program;
    }

    SourceLoc agent_loc = md_loc(&mp);
    agent_loc.line = 1;
    AstNode *agent = ast_new(arena, AST_AGENT, agent_loc);
    agent->name = agent_name;

    AstNode *members = NULL;
    AstNode *fns = NULL;
    AstNode *top_level = NULL;  /* health, metrics, signal, progress, supervisor */

    /* Parse remaining sections */
    while (!md_at_end(&mp)) {
        md_skip_blank_lines(&mp);
        if (md_at_end(&mp)) break;

        /* Blockquote — system prompt */
        if (md_peek(&mp) == '>') {
            AstNode *prompt = md_parse_prompt(&mp);
            if (prompt) {
                members = ast_append(members, prompt);
            }
            continue;
        }

        /* ## Section heading */
        if (md_is_heading_at_level(&mp, 2)) {
            char *heading = md_read_line(&mp);
            const char *text = heading;
            while (*text == '#') text++;
            while (*text == ' ') text++;
            char *section_name_orig = md_trim(mp.arena, text);
            char *section_name = arena_strdup(mp.arena, section_name_orig);

            /* Convert to lowercase for matching */
            {
                char *p = section_name;
                while (*p) {
                    if (*p >= 'A' && *p <= 'Z') {
                        *p = (char)(*p + ('a' - 'A'));
                    }
                    p++;
                }
            }

            MdSection section = md_classify_section(section_name);

            switch (section) {
            case MD_SECTION_CAPABILITIES: {
                AstNode *cap = md_parse_capabilities(&mp);
                if (cap) members = ast_append(members, cap);
                break;
            }
            case MD_SECTION_MODEL: {
                AstNode *model = md_parse_model(&mp);
                if (model) members = ast_append(members, model);
                break;
            }
            case MD_SECTION_BUDGET: {
                AstNode *budget = md_parse_budget(&mp);
                if (budget) members = ast_append(members, budget);
                break;
            }
            case MD_SECTION_GUARDS: {
                md_parse_guards(&mp, &members);
                break;
            }
            case MD_SECTION_TOOLS: {
                AstNode *tools = md_parse_tools(&mp, &fns);
                /* Add each tool as a member of the agent */
                while (tools) {
                    AstNode *next = tools->next;
                    tools->next = NULL;
                    members = ast_append(members, tools);
                    tools = next;
                }
                break;
            }
            case MD_SECTION_MEMORY: {
                /* ## memory → Field "memory" with value "true" */
                SourceLoc memloc = md_loc(&mp);
                md_skip_blank_lines(&mp);
                if (!md_at_end(&mp) && !md_at_section_boundary(&mp, 2)) {
                    AstNode *mem_field = ast_new(mp.arena, AST_FIELD, memloc);
                    mem_field->name = arena_strdup(mp.arena, "memory");
                    AstNode *mem_val = ast_new(mp.arena, AST_IDENT, memloc);
                    mem_val->name = arena_strdup(mp.arena, "true");
                    mem_field->right = mem_val;
                    members = ast_append(members, mem_field);
                    md_read_line(&mp);  /* consume the "true" line */
                }
                break;
            }
            case MD_SECTION_KNOWLEDGE: {
                /* ## knowledge → Field "knowledge" with block of key:value pairs
                 * Parsed same as budget: - path: ./docs, - chunk_size: 500, etc. */
                SourceLoc kbloc = md_loc(&mp);
                AstNode *kb_field = ast_new(mp.arena, AST_FIELD, kbloc);
                kb_field->name = arena_strdup(mp.arena, "knowledge");
                AstNode *kb_block = ast_new(mp.arena, AST_BLOCK, kbloc);
                AstNode *kb_items = NULL;

                md_skip_blank_lines(&mp);
                while (!md_at_end(&mp) && !md_at_section_boundary(&mp, 2)) {
                    char *kbline = md_read_line(&mp);
                    char *kbtrimmed = md_trim(mp.arena, kbline);
                    if (strlen(kbtrimmed) == 0) continue;
                    if (!md_starts_with(kbtrimmed, "- ")) continue;
                    {
                        const char *kv = kbtrimmed + 2;
                        const char *colon = strchr(kv, ':');
                        if (!colon) continue;
                        {
                            size_t klen = (size_t)(colon - kv);
                            char *kkey = arena_strndup(mp.arena, kv, klen);
                            const char *vstr = colon + 1;
                            char *vval;
                            SourceLoc kfloc = kbloc;
                            AstNode *sf;
                            char *endp = NULL;
                            double dv;
                            kkey = md_trim(mp.arena, kkey);
                            vval = md_trim(mp.arena, vstr);
                            kfloc.line = mp.line - 1;
                            sf = ast_new(mp.arena, AST_FIELD, kfloc);
                            sf->name = intern_get(mp.intern, kkey, strlen(kkey));
                            dv = strtod(vval, &endp);
                            if (endp && endp != vval && *endp == '\0') {
                                if (strchr(vval, '.') == NULL) {
                                    AstNode *n = ast_new(mp.arena, AST_INT_LIT, kfloc);
                                    n->val.int_val = (int64_t)dv;
                                    sf->right = n;
                                } else {
                                    AstNode *n = ast_new(mp.arena, AST_FLOAT_LIT, kfloc);
                                    n->val.float_val = dv;
                                    sf->right = n;
                                }
                            } else {
                                AstNode *n = ast_new(mp.arena, AST_STRING_LIT, kfloc);
                                n->val.str_val = arena_strdup(mp.arena, vval);
                                sf->right = n;
                            }
                            kb_items = ast_append(kb_items, sf);
                        }
                    }
                }
                kb_block->params = kb_items;
                kb_field->right = kb_block;
                members = ast_append(members, kb_field);
                break;
            }
            case MD_SECTION_ENDPOINT: {
                /* ## endpoint
                 * http://localhost:8000 */
                SourceLoc eploc = md_loc(&mp);
                md_skip_blank_lines(&mp);
                char *ep_value = NULL;
                while (!md_at_end(&mp) && !md_at_section_boundary(&mp, 2)) {
                    char *epline = md_read_line(&mp);
                    char *eptrimmed = md_trim(mp.arena, epline);
                    if (strlen(eptrimmed) == 0) continue;
                    ep_value = eptrimmed;
                    break;
                }
                /* Skip remaining lines in section */
                while (!md_at_end(&mp) && !md_at_section_boundary(&mp, 2)) {
                    md_read_line(&mp);
                }
                if (ep_value) {
                    AstNode *ep_field = ast_new(mp.arena, AST_FIELD, eploc);
                    ep_field->name = arena_strdup(mp.arena, "endpoint");
                    AstNode *ep_str = ast_new(mp.arena, AST_STRING_LIT, eploc);
                    ep_str->val.str_val = intern_get(mp.intern, ep_value, strlen(ep_value));
                    ep_field->right = ep_str;
                    members = ast_append(members, ep_field);
                }
                break;
            }
            case MD_SECTION_API_KEY: {
                /* ## api_key
                 * sk-abc123... */
                SourceLoc akloc = md_loc(&mp);
                md_skip_blank_lines(&mp);
                char *ak_value = NULL;
                while (!md_at_end(&mp) && !md_at_section_boundary(&mp, 2)) {
                    char *akline = md_read_line(&mp);
                    char *aktrimmed = md_trim(mp.arena, akline);
                    if (strlen(aktrimmed) == 0) continue;
                    ak_value = aktrimmed;
                    break;
                }
                /* Skip remaining lines in section */
                while (!md_at_end(&mp) && !md_at_section_boundary(&mp, 2)) {
                    md_read_line(&mp);
                }
                if (ak_value) {
                    AstNode *ak_field = ast_new(mp.arena, AST_FIELD, akloc);
                    ak_field->name = arena_strdup(mp.arena, "api_key");
                    AstNode *ak_str = ast_new(mp.arena, AST_STRING_LIT, akloc);
                    ak_str->val.str_val = intern_get(mp.intern, ak_value, strlen(ak_value));
                    ak_field->right = ak_str;
                    members = ast_append(members, ak_field);
                }
                break;
            }
            case MD_SECTION_ENTROPY_BUDGET: {
                /* ## entropy_budget
                 * - max_avg_entropy: 0.7
                 * - max_low_confidence: 0.20
                 * - max_drift: 0.15
                 * Parsed like budget: key:value pairs */
                SourceLoc ebloc = md_loc(&mp);
                AstNode *eb_field = ast_new(mp.arena, AST_FIELD, ebloc);
                eb_field->name = arena_strdup(mp.arena, "entropy_budget");
                AstNode *eb_block = ast_new(mp.arena, AST_BLOCK, ebloc);
                AstNode *eb_items = NULL;

                md_skip_blank_lines(&mp);
                while (!md_at_end(&mp) && !md_at_section_boundary(&mp, 2)) {
                    char *ebline = md_read_line(&mp);
                    char *ebtrimmed = md_trim(mp.arena, ebline);
                    if (strlen(ebtrimmed) == 0) continue;
                    if (!md_starts_with(ebtrimmed, "- ")) {
                        md_error_fmt(&mp, "expected '- key: value' in entropy_budget section, got: %s",
                                     ebtrimmed);
                        continue;
                    }
                    {
                        const char *kv = ebtrimmed + 2;
                        const char *colon = strchr(kv, ':');
                        if (!colon) {
                            md_error_fmt(&mp, "expected 'key: value' in entropy_budget item: %s", kv);
                            continue;
                        }
                        {
                            size_t klen = (size_t)(colon - kv);
                            char *kkey = arena_strndup(mp.arena, kv, klen);
                            const char *vstr = colon + 1;
                            char *vval;
                            SourceLoc efloc = ebloc;
                            AstNode *sf;
                            char *endp = NULL;
                            double dv;
                            kkey = md_trim(mp.arena, kkey);
                            vval = md_trim(mp.arena, vstr);
                            efloc.line = mp.line - 1;
                            sf = ast_new(mp.arena, AST_FIELD, efloc);
                            sf->name = intern_get(mp.intern, kkey, strlen(kkey));
                            dv = strtod(vval, &endp);
                            if (endp && endp != vval && *endp == '\0') {
                                if (strchr(vval, '.') == NULL) {
                                    AstNode *n = ast_new(mp.arena, AST_INT_LIT, efloc);
                                    n->val.int_val = (int64_t)dv;
                                    sf->right = n;
                                } else {
                                    AstNode *n = ast_new(mp.arena, AST_FLOAT_LIT, efloc);
                                    n->val.float_val = dv;
                                    sf->right = n;
                                }
                            } else {
                                AstNode *n = ast_new(mp.arena, AST_STRING_LIT, efloc);
                                n->val.str_val = arena_strdup(mp.arena, vval);
                                sf->right = n;
                            }
                            eb_items = ast_append(eb_items, sf);
                        }
                    }
                }
                eb_block->params = eb_items;
                eb_field->right = eb_block;
                members = ast_append(members, eb_field);
                break;
            }
            case MD_SECTION_ACCESS_CONTROL: {
                /* ## access_control — parse subsections for network, filesystem, shell */
                SourceLoc acloc = md_loc(&mp);
                AstNode *ac_list = NULL;
                const char *current_subsection = NULL;

                md_skip_blank_lines(&mp);

                while (!md_at_end(&mp) && !md_at_section_boundary(&mp, 2)) {
                    /* Check for ### subsection heading */
                    if (md_is_heading_at_level(&mp, 3)) {
                        char *acline = md_read_line(&mp);
                        const char *sub_text = acline;
                        while (*sub_text == '#') sub_text++;
                        while (*sub_text == ' ') sub_text++;
                        current_subsection = md_trim(mp.arena, sub_text);
                        /* Lowercase the subsection name */
                        {
                            char *pp = (char *)current_subsection;
                            while (*pp) {
                                if (*pp >= 'A' && *pp <= 'Z') {
                                    *pp = (char)(*pp + ('a' - 'A'));
                                }
                                pp++;
                            }
                        }
                        md_skip_blank_lines(&mp);
                        continue;
                    }

                    char *acline = md_read_line(&mp);
                    char *actrimmed = md_trim(mp.arena, acline);
                    if (strlen(actrimmed) == 0) continue;

                    if (!current_subsection) {
                        md_error_fmt(&mp,
                            "access_control rules must be under a ### subsection "
                            "(network, filesystem, shell), got: %s", actrimmed);
                        continue;
                    }

                    SourceLoc rloc = acloc;
                    rloc.line = mp.line > 0 ? mp.line - 1 : mp.line;

                    /* Parse: allow|deny <kind> <value> or default: allow|deny */
                    if (md_starts_with(actrimmed, "allow ")) {
                        const char *rest = actrimmed + 6;
                        char *kend = (char *)rest;
                        while (*kend && *kend != ' ') kend++;
                        size_t klen = (size_t)(kend - rest);
                        char *kind = arena_strndup(mp.arena, rest, klen);

                        if (strcmp(kind, "endpoint") == 0) {
                            AstNode *rule = ast_new(mp.arena, AST_CAP_ENDPOINT_RULE, rloc);
                            rule->is_mut = true;
                            const char *q1 = strchr(kend, '"');
                            if (q1) { q1++;
                                const char *q2 = strchr(q1, '"');
                                if (q2) {
                                    char *val = arena_strndup(mp.arena, q1, (size_t)(q2 - q1));
                                    rule->name = intern_get(mp.intern, val, strlen(val));
                                }
                            }
                            ac_list = ast_append(ac_list, rule);
                        } else if (strcmp(kind, "path") == 0) {
                            AstNode *rule = ast_new(mp.arena, AST_CAP_PATH_RULE, rloc);
                            rule->is_mut = true;
                            const char *q1 = strchr(kend, '"');
                            if (q1) { q1++;
                                const char *q2 = strchr(q1, '"');
                                if (q2) {
                                    char *val = arena_strndup(mp.arena, q1, (size_t)(q2 - q1));
                                    rule->name = intern_get(mp.intern, val, strlen(val));
                                }
                            }
                            ac_list = ast_append(ac_list, rule);
                        } else if (strcmp(kind, "binary") == 0) {
                            AstNode *rule = ast_new(mp.arena, AST_CAP_BINARY_RULE, rloc);
                            rule->is_mut = true;
                            const char *q1 = strchr(kend, '"');
                            if (q1) { q1++;
                                const char *q2 = strchr(q1, '"');
                                if (q2) {
                                    char *val = arena_strndup(mp.arena, q1, (size_t)(q2 - q1));
                                    rule->name = intern_get(mp.intern, val, strlen(val));
                                }
                            }
                            ac_list = ast_append(ac_list, rule);
                        } else {
                            md_error_fmt(&mp, "unknown allow rule kind in access_control: %s", kind);
                        }
                    } else if (md_starts_with(actrimmed, "deny ")) {
                        const char *rest = actrimmed + 5;
                        char *kend = (char *)rest;
                        while (*kend && *kend != ' ') kend++;
                        size_t klen = (size_t)(kend - rest);
                        char *kind = arena_strndup(mp.arena, rest, klen);

                        if (strcmp(kind, "private_ranges") == 0) {
                            AstNode *rule = ast_new(mp.arena, AST_CAP_DENY_RANGE, rloc);
                            rule->is_mut = false;
                            rule->name = intern_get(mp.intern, "private_ranges", 14);
                            ac_list = ast_append(ac_list, rule);
                        } else if (strcmp(kind, "endpoint") == 0) {
                            AstNode *rule = ast_new(mp.arena, AST_CAP_ENDPOINT_RULE, rloc);
                            rule->is_mut = false;
                            const char *q1 = strchr(kend, '"');
                            if (q1) { q1++;
                                const char *q2 = strchr(q1, '"');
                                if (q2) {
                                    char *val = arena_strndup(mp.arena, q1, (size_t)(q2 - q1));
                                    rule->name = intern_get(mp.intern, val, strlen(val));
                                }
                            }
                            ac_list = ast_append(ac_list, rule);
                        } else if (strcmp(kind, "path") == 0) {
                            AstNode *rule = ast_new(mp.arena, AST_CAP_PATH_RULE, rloc);
                            rule->is_mut = false;
                            const char *q1 = strchr(kend, '"');
                            if (q1) { q1++;
                                const char *q2 = strchr(q1, '"');
                                if (q2) {
                                    char *val = arena_strndup(mp.arena, q1, (size_t)(q2 - q1));
                                    rule->name = intern_get(mp.intern, val, strlen(val));
                                }
                            }
                            ac_list = ast_append(ac_list, rule);
                        } else if (strcmp(kind, "binary") == 0) {
                            AstNode *rule = ast_new(mp.arena, AST_CAP_BINARY_RULE, rloc);
                            rule->is_mut = false;
                            const char *q1 = strchr(kend, '"');
                            if (q1) { q1++;
                                const char *q2 = strchr(q1, '"');
                                if (q2) {
                                    char *val = arena_strndup(mp.arena, q1, (size_t)(q2 - q1));
                                    rule->name = intern_get(mp.intern, val, strlen(val));
                                }
                            }
                            ac_list = ast_append(ac_list, rule);
                        } else {
                            md_error_fmt(&mp, "unknown deny rule kind in access_control: %s", kind);
                        }
                    } else if (md_starts_with(actrimmed, "default:")) {
                        const char *dv = md_trim(mp.arena, actrimmed + 8);
                        AstNode *def = ast_new(mp.arena, AST_CAP_DEFAULT, rloc);
                        def->is_mut = (strcmp(dv, "allow") == 0);
                        ac_list = ast_append(ac_list, def);
                    } else {
                        md_error_fmt(&mp,
                            "expected 'allow', 'deny', or 'default:' in access_control, got: %s",
                            actrimmed);
                    }
                }

                /* Wrap all access control rules into capability nodes per subsection */
                if (ac_list) {
                    /* Store as a single access_control field containing all rules */
                    AstNode *ac_field = ast_new(mp.arena, AST_FIELD, acloc);
                    ac_field->name = arena_strdup(mp.arena, "access_control");
                    AstNode *ac_block = ast_new(mp.arena, AST_BLOCK, acloc);
                    ac_block->params = ac_list;
                    ac_field->right = ac_block;
                    members = ast_append(members, ac_field);
                }
                break;
            }
            case MD_SECTION_CAPABILITY_DECL: {
                /* Extract capability name from "capability <name>" */
                const char *cap_decl_name = section_name + 11; /* skip "capability " */
                AstNode *cap_decl = md_parse_capability_decl(&mp, cap_decl_name);
                if (cap_decl) {
                    /* Add as top-level declaration on program, not inside agent */
                    /* We store in a temporary list and attach later */
                    members = ast_append(members, cap_decl);
                }
                break;
            }
            case MD_SECTION_TAINT: {
                AstNode *taint_list = md_parse_taint(&mp);
                /* Add each taint as a member of the agent (like capabilities) */
                while (taint_list) {
                    AstNode *next = taint_list->next;
                    taint_list->next = NULL;
                    members = ast_append(members, taint_list);
                    taint_list = next;
                }
                break;
            }
            case MD_SECTION_SKILLS: {
                /* ## skills
                 * - summarize
                 * - translate
                 * Parsed like capabilities: list of identifiers */
                SourceLoc skloc = md_loc(&mp);
                AstNode *sk_field = ast_new(mp.arena, AST_FIELD, skloc);
                sk_field->name = arena_strdup(mp.arena, "skills");
                AstNode *sk_arr = ast_new(mp.arena, AST_ARRAY, skloc);
                AstNode *sk_elems = NULL;

                md_skip_blank_lines(&mp);
                while (!md_at_end(&mp) && !md_at_section_boundary(&mp, 2)) {
                    char *skline = md_read_line(&mp);
                    char *sktrimmed = md_trim(mp.arena, skline);
                    if (strlen(sktrimmed) == 0) continue;
                    if (!md_starts_with(sktrimmed, "- ")) {
                        md_error_fmt(&mp, "expected '- skill_name' in skills section, got: %s",
                                     sktrimmed);
                        continue;
                    }
                    {
                        const char *sk_name = md_trim(mp.arena, sktrimmed + 2);
                        if (strlen(sk_name) == 0) continue;
                        SourceLoc seloc = skloc;
                        seloc.line = mp.line - 1;
                        AstNode *elem = ast_new(mp.arena, AST_IDENT, seloc);
                        elem->name = intern_get(mp.intern, sk_name, strlen(sk_name));
                        sk_elems = ast_append(sk_elems, elem);
                    }
                }
                sk_arr->params = sk_elems;
                sk_field->right = sk_arr;
                members = ast_append(members, sk_field);

                /* Emit a note for each listed skill — they must be defined externally */
                if (sk_elems) {
                    AstNode *sk_elem = sk_elems;
                    while (sk_elem) {
                        SourceLoc snloc = sk_elem->loc;
                        report_warning(mp.reporter, snloc,
                            "skill listed in markdown must be defined in a "
                            ".lceron file or package",
                            "verify that this skill is defined and importable");
                        sk_elem = sk_elem->next;
                    }
                }
                break;
            }
            case MD_SECTION_HEALTH: {
                AstNode *health = md_parse_health(&mp);
                if (health) top_level = ast_append(top_level, health);
                break;
            }
            case MD_SECTION_METRICS: {
                AstNode *metrics = md_parse_metrics(&mp);
                if (metrics) top_level = ast_append(top_level, metrics);
                break;
            }
            case MD_SECTION_SIGNAL: {
                AstNode *sig = md_parse_signal(&mp);
                if (sig) top_level = ast_append(top_level, sig);
                break;
            }
            case MD_SECTION_PROGRESS: {
                AstNode *prog_node = md_parse_progress(&mp);
                if (prog_node) top_level = ast_append(top_level, prog_node);
                break;
            }
            case MD_SECTION_SUPERVISOR: {
                AstNode *sup = md_parse_supervisor(&mp, section_name_orig);
                if (sup) top_level = ast_append(top_level, sup);
                break;
            }
            case MD_SECTION_UNKNOWN: {
                /* Unknown section — warn and skip until next ## */
                SourceLoc wloc = md_loc(&mp);
                wloc.line = mp.line > 0 ? mp.line - 1 : mp.line;
                report_warning_fmt(mp.reporter, wloc,
                    "supported sections: capabilities, model, budget, guards, tools, "
                    "memory, knowledge, endpoint, api_key, entropy_budget, "
                    "access_control, skills",
                    "unrecognized section '## %s' -- section will be ignored",
                    section_name);
                while (!md_at_end(&mp) && !md_at_section_boundary(&mp, 2)) {
                    md_read_line(&mp);
                }
                break;
            }
            case MD_SECTION_NONE:
                break;
            }
            continue;
        }

        /* Standalone code blocks outside any section */
        md_skip_line_whitespace(&mp);
        if (!md_at_end(&mp) &&
            (md_starts_with(mp.source + mp.pos, "```limceron") ||
             md_starts_with(mp.source + mp.pos, "```lceron"))) {
            AstNode *code_result = md_parse_code_block(&mp);
            if (code_result) {
                AstNode *decl = code_result->params;
                while (decl) {
                    AstNode *next = decl->next;
                    decl->next = NULL;
                    if (decl->kind == AST_FN) {
                        fns = ast_append(fns, decl);
                    }
                    decl = next;
                }
            }
            continue;
        }

        /* Skip any other content (paragraphs, HTML comments, etc.) */
        md_read_line(&mp);
    }

    agent->params = members;
    agent->left = fns;

    /* Chain: agent first, then top-level declarations (health, metrics, etc.) */
    agent->next = top_level;

    program->params = agent;
    return program;
}
