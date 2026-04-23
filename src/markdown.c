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
    MD_SECTION_SKILLS,
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
 *
 * Each ### becomes an AST_GUARD node with the description stored
 * as a prompt field. Returns a list of guard fields to append to
 * the agent's members.
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

            /* Collect description text until next ### or ## */
            md_skip_blank_lines(mp);
            size_t desc_start = mp->pos;
            while (!md_at_end(mp) && !md_at_section_boundary(mp, 3)) {
                md_read_line(mp);
            }
            size_t desc_end = mp->pos;

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
            char *section_name = md_trim(mp.arena, text);

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
                /* ## access_control — too complex for markdown, skip with comment */
                while (!md_at_end(&mp) && !md_at_section_boundary(&mp, 2)) {
                    md_read_line(&mp);
                }
                /* TODO: access_control parsing not yet implemented for markdown */
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
                break;
            }
            case MD_SECTION_UNKNOWN:
                /* Unknown section — skip until next ## */
                while (!md_at_end(&mp) && !md_at_section_boundary(&mp, 2)) {
                    md_read_line(&mp);
                }
                break;
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

    program->params = agent;
    return program;
}
