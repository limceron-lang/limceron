/*
 * Limceron Stage 0 Bootstrap Compiler — Entry Point
 *
 * Usage:
 *   limceron-stage0 build <file.lceron|.lceron.md> [-o <output>]   Compile to native binary
 *   limceron-stage0 run   <file.lceron|.lceron.md>                 Compile and execute
 *   limceron-stage0 emit  <file.lceron|.lceron.md> [-o <output.c>] Transpile Limceron to C
 *   limceron-stage0 parse <file.lceron|.lceron.md>                 Parse and print AST (debug)
 *   limceron-stage0 lex   <file.lceron>                          Tokenize and print (debug)
 *   limceron-stage0 version                                  Show version
 */

#include "lcn.h"
#include "package.h"
#include "ir.h"
#include <sys/stat.h>
#include <libgen.h>
#include <unistd.h>

/* ============================================================
 * File I/O
 * ============================================================ */

char *read_source_file(Arena *a, const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open file: %s (%s)\n", path, strerror(errno));
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0 || (size_t)size > MAX_SOURCE_SIZE) {
        fprintf(stderr, "error: file too large: %s (%ld bytes, max %d MB)\n",
                path, size, MAX_SOURCE_SIZE / (1024*1024));
        fclose(f);
        return NULL;
    }

    char *buf = (char *)arena_alloc(a, (size_t)size + 1);
    size_t read = fread(buf, 1, (size_t)size, f);
    fclose(f);

    buf[read] = '\0';
    *out_len = read;
    return buf;
}

/* ============================================================
 * Helpers
 * ============================================================ */

/* Check if filename ends with .lceron.md */
static bool is_lceron_md(const char *filename) {
    size_t len = strlen(filename);
    return len > 10 && strcmp(filename + len - 10, ".lceron.md") == 0;
}

/* Parse a Limceron source file (.lceron or .lceron.md).
 * Returns AST program node, or NULL on error.
 * Sets *had_error to true if parsing failed. */
static AstNode *parse_source(const char *filename, const char *source, size_t len,
                              Arena *ast_arena, Arena *intern_arena,
                              ErrorReporter *reporter, bool *had_error) {
    StringIntern intern = intern_new(intern_arena);

    if (is_lceron_md(filename)) {
        AstNode *program = parse_lceron_md(filename, source, len,
                                          ast_arena, &intern, reporter);
        *had_error = (reporter->count > 0);
        return program;
    }

    /* Standard .lceron file */
    Lexer lexer = lexer_new(filename, source, len, &intern, reporter);
    Parser parser = parser_new(&lexer, ast_arena, reporter);
    AstNode *program = parse_program(&parser);
    *had_error = parser.had_error;
    return program;
}

/* ============================================================
 * Multi-file Import Resolution
 * ============================================================ */

/* Check if a USE node is a module import (not mcp/a2a/driver/model). */
static bool is_module_use(AstNode *use_node) {
    if (use_node->kind != AST_USE) return false;
    if (!use_node->name) return false;
    if (strcmp(use_node->name, "mcp") == 0) return false;
    if (strcmp(use_node->name, "a2a") == 0) return false;
    if (strcmp(use_node->name, "driver") == 0) return false;
    if (strcmp(use_node->name, "model") == 0) return false;
    return true;
}

/* Check if a use path is a stdlib import (starts with "std."). */
static bool is_std_import(const char *name) {
    return name && strncmp(name, "std.", 4) == 0;
}

/* Convert dotted use path to filesystem path.
 * "agents.extractor" + base_dir -> "{base_dir}/agents/extractor" */
static void use_path_to_filepath(const char *use_path, const char *base_dir,
                                  char *out, size_t out_size) {
    snprintf(out, out_size, "%s/", base_dir);
    size_t off = strlen(out);
    for (size_t i = 0; use_path[i] && off < out_size - 1; i++) {
        out[off++] = (use_path[i] == '.') ? '/' : use_path[i];
    }
    out[off] = '\0';
}

/* Track which files have been imported (cycle detection). */
#define MAX_IMPORT_PATHS 64

typedef struct {
    char paths[MAX_IMPORT_PATHS][512];
    int count;
} ImportTracker;

static bool import_visited(ImportTracker *tracker, const char *path) {
    for (int i = 0; i < tracker->count; i++) {
        if (strcmp(tracker->paths[i], path) == 0) return true;
    }
    return false;
}

static void import_mark_visited(ImportTracker *tracker, const char *path) {
    if (tracker->count < MAX_IMPORT_PATHS) {
        strncpy(tracker->paths[tracker->count], path, 511);
        tracker->paths[tracker->count][511] = '\0';
        tracker->count++;
    }
}

/* Track which declarations have been seen (deduplication). */
#define MAX_DECL_ENTRIES 512

typedef struct {
    AstKind kind;
    const char *name;
} DeclEntry;

typedef struct {
    DeclEntry entries[MAX_DECL_ENTRIES];
    int count;
} DeclTracker;

static bool decl_seen(DeclTracker *tracker, AstKind kind, const char *name) {
    if (!name) return false;
    for (int i = 0; i < tracker->count; i++) {
        if (tracker->entries[i].kind == kind &&
            strcmp(tracker->entries[i].name, name) == 0) return true;
    }
    return false;
}

static void decl_mark_seen(DeclTracker *tracker, AstKind kind, const char *name) {
    if (!name || tracker->count >= MAX_DECL_ENTRIES) return;
    tracker->entries[tracker->count].kind = kind;
    tracker->entries[tracker->count].name = name;
    tracker->count++;
}

/* Resolve all `use` imports recursively.
 * target  = top-level program whose params list receives merged declarations.
 * scan    = program currently being scanned for use statements (may differ during recursion).
 * Returns the number of files successfully resolved. */
static int resolve_imports(AstNode *target, AstNode *scan, const char *base_dir,
                            const char *stdlib_dir,
                            Arena *source_arena, Arena *ast_arena, Arena *intern_arena,
                            ImportTracker *imports, DeclTracker *decls) {
    int files_resolved = 0;

    /* Process use statements in the scanned program */
    for (AstNode *d = scan->params; d; d = d->next) {
        if (!is_module_use(d)) continue;

        char rel_path[512];

        /* std.* imports resolve from stdlib directory */
        if (is_std_import(d->name)) {
            /* "std.io" -> stdlib_dir + "/io" (skip "std." prefix) */
            use_path_to_filepath(d->name + 4, stdlib_dir, rel_path, sizeof(rel_path));
        } else {
            use_path_to_filepath(d->name, base_dir, rel_path, sizeof(rel_path));
        }

        /* Try .lceron then .lceron.md */
        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s.lceron", rel_path);

        struct stat st;
        if (stat(full_path, &st) != 0) {
            snprintf(full_path, sizeof(full_path), "%s.lceron.md", rel_path);
            if (stat(full_path, &st) != 0) {
                fprintf(stderr, "  import: %s (not found, skipping)\n", d->name);
                continue;
            }
        }

        /* Cycle check */
        if (import_visited(imports, full_path)) continue;
        import_mark_visited(imports, full_path);

        /* Read and parse the imported file */
        size_t len;
        char *source = read_source_file(source_arena, full_path, &len);
        if (!source) continue;

        ErrorReporter file_reporter = reporter_new(full_path, source, len);
        bool had_error = false;
        AstNode *imported = parse_source(full_path, source, len,
                                          ast_arena, intern_arena,
                                          &file_reporter, &had_error);
        if (had_error || !imported) {
            fprintf(stderr, "  import: %s (parse error, skipping)\n", d->name);
            continue;
        }

        fprintf(stderr, "  import: %s -> %s (%d declarations)\n",
                d->name, full_path, ast_list_len(imported->params));

        /* Recurse: resolve imports in the imported file first (depth-first).
         * Always merge into `target` (the top-level program). */
        files_resolved += resolve_imports(target, imported, base_dir,
                                           stdlib_dir,
                                           source_arena, ast_arena, intern_arena,
                                           imports, decls);
        files_resolved++;

        /* Merge: append imported declarations to target program.
         * Skip AST_USE and AST_MODULE nodes; deduplicate by (kind, name). */
        AstNode *tail = target->params;
        while (tail && tail->next) tail = tail->next;

        for (AstNode *imp = imported->params; imp; ) {
            AstNode *next_imp = imp->next;

            /* Skip use and module declarations from imports */
            if (imp->kind == AST_USE || imp->kind == AST_MODULE) {
                imp = next_imp;
                continue;
            }

            /* Check for duplicates */
            if (imp->name && decl_seen(decls, imp->kind, imp->name)) {
                /* Silent skip for safe kinds (budget, enum, capability, struct, guard) */
                if (imp->kind != AST_BUDGET && imp->kind != AST_ENUM &&
                    imp->kind != AST_CAPABILITY && imp->kind != AST_STRUCT &&
                    imp->kind != AST_GUARD && imp->kind != AST_GUARDSET) {
                    fprintf(stderr, "  import: duplicate %s '%s' (conflict)\n",
                            ast_kind_name(imp->kind), imp->name);
                }
                imp = next_imp;
                continue;
            }

            /* Register and append */
            if (imp->name) decl_mark_seen(decls, imp->kind, imp->name);
            imp->next = NULL;
            if (tail) { tail->next = imp; tail = imp; }
            else { target->params = imp; tail = imp; }

            imp = next_imp;
        }
    }

    return files_resolved;
}

/* Resolve path to compiler runtime directory.
 * argv0 is the compiler binary path; returns arena-allocated string. */
static const char *find_runtime_dir(Arena *a, const char *argv0) {
    /* Strategy: runtime/ is a sibling of src/ in the source tree.
     * At install time, it would be next to the binary.
     * For development, walk up from the binary location. */
    char resolved[4096];
    char *rp = NULL;

    /* Try realpath of the binary */
    if (argv0) rp = realpath(argv0, resolved);
    if (!rp) {
        /* Fallback: assume cwd-relative */
        return arena_strdup(a, "runtime");
    }

    /* Go up from build/limceron-stage0 to project root */
    char *dir = dirname(resolved);  /* build/ */
    char *parent = dirname(dir);    /* project root */
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s/runtime", parent);

    struct stat st;
    if (stat(buf, &st) == 0 && S_ISDIR(st.st_mode))
        return arena_strdup(a, buf);

    /* Fallback: relative path */
    return arena_strdup(a, "runtime");
}

/* Resolve path to stdlib directory.
 * stdlib/ is at the project root, sibling of compiler/. */
static const char *find_stdlib_dir(Arena *a, const char *argv0) {
    char resolved[4096];
    char *rp = NULL;

    if (argv0) rp = realpath(argv0, resolved);
    if (!rp) {
        return arena_strdup(a, "stdlib");
    }

    /* Go up from build/limceron-stage0 to project root */
    char *dir = dirname(resolved);
    char *parent = dirname(dir);
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s/stdlib", parent);

    struct stat st;
    if (stat(buf, &st) == 0 && S_ISDIR(st.st_mode))
        return arena_strdup(a, buf);

    return arena_strdup(a, "stdlib");
}

/* ============================================================
 * Cross-Compilation — Targets Command
 * ============================================================ */

/* Print summary of all known targets and detected cross-compilers. */
static int cmd_targets(void) {
    LcnTarget native = lcn_native_target();

    typedef struct {
        const char *triple;
        const char *desc;
    } KnownTarget;

    KnownTarget known[] = {
        { "x86_64-linux",         "x86_64 Linux (glibc)" },
        { "x86_64-linux-musl",    "x86_64 Linux (musl, static)" },
        { "aarch64-linux",        "ARM64 Linux (glibc)" },
        { "aarch64-linux-musl",   "ARM64 Linux (musl, static)" },
        { "x86_64-darwin",        "x86_64 macOS (Intel)" },
        { "aarch64-darwin",       "ARM64 macOS (Apple Silicon)" },
        { "x86_64-windows-msvc",  "x86_64 Windows (MSVC) [stub]" },
    };
    int n_known = (int)(sizeof(known) / sizeof(known[0]));

    printf("\nLimceron Cross-Compilation Targets\n");
    printf("==================================================\n\n");
    printf("  %-26s %-10s %s\n", "TARGET", "STATUS", "COMPILER");
    printf("  %-26s %-10s %s\n", "------", "------", "--------");

    int found_count = 0;
    int ki;
    for (ki = 0; ki < n_known; ki++) {
        LcnTarget t = lcn_parse_target(known[ki].triple);
        bool is_nat = (t.arch == native.arch && t.os == native.os);
        bool has_cc = false;

        if (is_nat) {
            snprintf(t.cc, sizeof(t.cc), "cc");
            has_cc = true;
        } else {
            has_cc = lcn_find_cross_cc(&t);
        }

        const char *status;
        if (is_nat)       status = "(native)";
        else if (has_cc)  status = "ok";
        else              status = "[not found]";

        printf("  %-26s %-10s %s\n", known[ki].triple, status,
               has_cc ? t.cc : "---");

        if (has_cc) found_count++;
    }

    printf("\n  %d of %d targets available.\n\n", found_count, n_known);
    printf("  Tip: Install cross-compilers with your package manager, or use\n");
    printf("       `zig cc` for cross-compilation (install: https://ziglang.org).\n");
    printf("       You can also set LCN_CC_<ARCH>_<OS> environment variables.\n\n");

    return 0;
}

/* ============================================================
 * Commands
 * ============================================================ */

static int cmd_lex(const char *filename) {
    Arena source_arena = arena_new(16 * 1024 * 1024);
    Arena intern_arena = arena_new(4 * 1024 * 1024);

    size_t len;
    char *source = read_source_file(&source_arena, filename, &len);
    if (!source) return 1;

    ErrorReporter reporter = reporter_new(filename, source, len);
    StringIntern intern = intern_new(&intern_arena);
    Lexer lexer = lexer_new(filename, source, len, &intern, &reporter);

    printf("%-12s %-20s %-6s %s\n", "KIND", "VALUE", "LINE", "COL");
    printf("%-12s %-20s %-6s %s\n", "----", "-----", "----", "---");

    Token tok;
    do {
        tok = lexer_next(&lexer);
        printf("%-12s ", token_kind_name(tok.kind));

        switch (tok.kind) {
        case TOK_INT_LIT:
            printf("%-20lld ", (long long)tok.value.int_val);
            break;
        case TOK_FLOAT_LIT:
            printf("%-20f ", tok.value.float_val);
            break;
        case TOK_STRING_LIT:
        case TOK_IDENT:
            printf("%-20s ", tok.value.str_val ? tok.value.str_val : "");
            break;
        default:
            printf("%-20s ", token_kind_name(tok.kind));
            break;
        }

        printf("%-6u %u\n", tok.loc.line, tok.loc.column);
    } while (tok.kind != TOK_EOF);

    printf("\nErrors: %d\n", reporter.count);

    arena_free(&source_arena);
    arena_free(&intern_arena);
    return reporter.count > 0 ? 1 : 0;
}

static int cmd_parse(const char *filename) {
    Arena source_arena = arena_new(16 * 1024 * 1024);
    Arena intern_arena = arena_new(4 * 1024 * 1024);
    Arena ast_arena = arena_new(32 * 1024 * 1024);

    size_t len;
    char *source = read_source_file(&source_arena, filename, &len);
    if (!source) return 1;

    ErrorReporter reporter = reporter_new(filename, source, len);
    bool had_error = false;
    AstNode *program = parse_source(filename, source, len,
                                     &ast_arena, &intern_arena,
                                     &reporter, &had_error);

    if (!had_error) {
        fprintf(stderr, "\n=== AST ===\n");
        ast_print(program, 0);
        fprintf(stderr, "\nParse successful: %d declarations\n",
                ast_list_len(program->params));
    } else {
        fprintf(stderr, "\nParse completed with %d error(s)\n", reporter.count);
        if (program) {
            fprintf(stderr, "\n=== Partial AST ===\n");
            ast_print(program, 0);
        }
    }

    arena_free(&source_arena);
    arena_free(&intern_arena);
    arena_free(&ast_arena);
    return reporter.count > 0 ? 1 : 0;
}

static int cmd_emit(const char *filename, const char *output_path) {
    Arena source_arena = arena_new(16 * 1024 * 1024);
    Arena intern_arena = arena_new(4 * 1024 * 1024);
    Arena ast_arena = arena_new(64 * 1024 * 1024);

    size_t len;
    char *source = read_source_file(&source_arena, filename, &len);
    if (!source) return 1;

    ErrorReporter reporter = reporter_new(filename, source, len);
    bool had_error = false;
    AstNode *program = parse_source(filename, source, len,
                                     &ast_arena, &intern_arena,
                                     &reporter, &had_error);

    if (had_error) {
        fprintf(stderr, "Parse failed with %d error(s)\n", reporter.count);
        arena_free(&source_arena);
        arena_free(&intern_arena);
        arena_free(&ast_arena);
        return 1;
    }

    /* 2b. Resolve multi-file imports */
    {
        char main_dir[512];
        strncpy(main_dir, filename, sizeof(main_dir) - 1);
        main_dir[sizeof(main_dir) - 1] = '\0';
        char *slash = strrchr(main_dir, '/');
        if (slash) *slash = '\0';
        else strcpy(main_dir, ".");

        const char *stdlib_dir = find_stdlib_dir(&source_arena, NULL);

        ImportTracker import_tracker = {0};
        DeclTracker decl_tracker = {0};

        import_mark_visited(&import_tracker, filename);

        /* Seed tracker with main program's own declarations */
        {
            AstNode *d;
            for (d = program->params; d; d = d->next) {
                if (d->name && d->kind != AST_USE && d->kind != AST_MODULE)
                    decl_mark_seen(&decl_tracker, d->kind, d->name);
            }
        }

        int resolved = resolve_imports(program, program, main_dir,
                                        stdlib_dir,
                                        &source_arena, &ast_arena, &intern_arena,
                                        &import_tracker, &decl_tracker);
        if (resolved > 0) {
            fprintf(stderr, "  Imports: resolved %d file(s), %d total declarations\n",
                    resolved, ast_list_len(program->params));
        }
    }

    /* Run type checker (warnings only — don't block emit) */
    typecheck_program(program, &reporter, &ast_arena);

    char *c_code = codegen_generate(program, filename, &ast_arena);
    if (!c_code) {
        fprintf(stderr, "error: code generation failed\n");
        arena_free(&source_arena);
        arena_free(&intern_arena);
        arena_free(&ast_arena);
        return 1;
    }

    /* Write to file or stdout */
    if (output_path) {
        FILE *out = fopen(output_path, "w");
        if (!out) {
            fprintf(stderr, "error: cannot open output file: %s\n", output_path);
            free(c_code);
            arena_free(&source_arena);
            arena_free(&intern_arena);
            arena_free(&ast_arena);
            return 1;
        }
        fputs(c_code, out);
        fclose(out);
        fprintf(stderr, "Limceron->C: %s -> %s (%d declarations)\n",
                filename, output_path, ast_list_len(program->params));
    } else {
        fputs(c_code, stdout);
    }

    free(c_code);
    arena_free(&source_arena);
    arena_free(&intern_arena);
    arena_free(&ast_arena);
    return 0;
}

static int cmd_build(const char *input, const char *output, const char *argv0,
                     bool serve_mode, const LcnTarget *target) {
    Arena source_arena = arena_new(16 * 1024 * 1024);
    Arena intern_arena = arena_new(4 * 1024 * 1024);
    Arena ast_arena = arena_new(64 * 1024 * 1024);

    if (target && !target->is_native) {
        fprintf(stderr, "Limceron Stage 0 — Cross-compiling: %s -> %s (target: %s)%s%s\n",
                input, output, target->triple,
                target->static_link ? " [static]" : "",
                serve_mode ? " (MCP server)" : "");
    } else {
        fprintf(stderr, "Limceron Stage 0 — Compiling: %s -> %s%s\n", input, output,
                serve_mode ? " (MCP server)" : "");
    }

    /* 1. Read source */
    size_t len;
    char *source = read_source_file(&source_arena, input, &len);
    if (!source) return 1;

    /* 2. Parse (.lceron or .lceron.md) */
    ErrorReporter reporter = reporter_new(input, source, len);
    bool had_error = false;
    AstNode *program = parse_source(input, source, len,
                                     &ast_arena, &intern_arena,
                                     &reporter, &had_error);
    if (had_error) {
        fprintf(stderr, "  Parse failed with %d error(s)\n", reporter.count);
        arena_free(&source_arena);
        arena_free(&intern_arena);
        arena_free(&ast_arena);
        return 1;
    }
    fprintf(stderr, "  Parsed: %d declarations\n", ast_list_len(program->params));

    /* 2b. Resolve multi-file imports */
    {
        char main_dir[512];
        strncpy(main_dir, input, sizeof(main_dir) - 1);
        main_dir[sizeof(main_dir) - 1] = '\0';
        char *slash = strrchr(main_dir, '/');
        if (slash) *slash = '\0';
        else strcpy(main_dir, ".");

        const char *stdlib_dir = find_stdlib_dir(&source_arena, argv0);

        ImportTracker import_tracker = {0};
        DeclTracker decl_tracker = {0};

        /* Mark the main file as visited */
        import_mark_visited(&import_tracker, input);

        /* Seed tracker with main program's own declarations */
        {
            AstNode *d;
            for (d = program->params; d; d = d->next) {
                if (d->name && d->kind != AST_USE && d->kind != AST_MODULE)
                    decl_mark_seen(&decl_tracker, d->kind, d->name);
            }
        }

        int resolved = resolve_imports(program, program, main_dir,
                                        stdlib_dir,
                                        &source_arena, &ast_arena, &intern_arena,
                                        &import_tracker, &decl_tracker);
        if (resolved > 0) {
            fprintf(stderr, "  Imports: resolved %d file(s), %d total declarations\n",
                    resolved, ast_list_len(program->params));
        }

        /* 2c. Resolve package dependencies (limceron.toml) */
        {
            /* Walk up from source file to find project root (dir with limceron.toml) */
            char project_dir[512];
            strncpy(project_dir, main_dir, sizeof(project_dir) - 1);
            project_dir[sizeof(project_dir) - 1] = '\0';

            LcnPackage manifest;
            bool found_manifest = false;

            /* Try current source dir, then parent dirs (up to 3 levels) */
            {
                char try_dir[512];
                strncpy(try_dir, project_dir, sizeof(try_dir) - 1);
                int levels;
                for (levels = 0; levels < 4; levels++) {
                    if (lcn_package_load(try_dir, &manifest)) {
                        strncpy(project_dir, try_dir, sizeof(project_dir) - 1);
                        found_manifest = true;
                        break;
                    }
                    /* Go up one directory */
                    char *up = strrchr(try_dir, '/');
                    if (up) *up = '\0';
                    else break;
                }
            }

            if (found_manifest && manifest.dep_count > 0) {
                char dep_paths[LCN_PKG_MAX_DEPS][LCN_PKG_MAX_PATH];
                int dep_path_count = lcn_dep_import_paths(project_dir, &manifest,
                                                           dep_paths, LCN_PKG_MAX_DEPS);
                int dep_resolved = 0;
                int di;
                for (di = 0; di < dep_path_count; di++) {
                    int r = resolve_imports(program, program, dep_paths[di],
                                             stdlib_dir,
                                             &source_arena, &ast_arena, &intern_arena,
                                             &import_tracker, &decl_tracker);
                    dep_resolved += r;
                }
                if (dep_resolved > 0) {
                    fprintf(stderr, "  Packages: resolved %d file(s) from %d dependencies\n",
                            dep_resolved, dep_path_count);
                }
            }
        }
    }

    /* 3. Type check (enforcer mode: critical errors block, advisory warnings continue) */
    int errors_before = reporter.count;
    bool tc_ok = typecheck_program(program, &reporter, &ast_arena);
    int new_errors = reporter.count - errors_before;
    if (!tc_ok) {
        fprintf(stderr, "  Type check: FAILED (%d error(s))\n", new_errors);
        arena_free(&source_arena);
        arena_free(&intern_arena);
        arena_free(&ast_arena);
        return 1;
    } else if (new_errors > 0) {
        fprintf(stderr, "  Type check: %d warning(s)\n", new_errors);
    } else {
        fprintf(stderr, "  Type check: OK\n");
    }

    /* 4. Generate C (build mode — uses #include "lcn_runtime.h") */
    char *c_code;
    if (serve_mode)
        c_code = codegen_generate_for_serve(program, input, &ast_arena);
    else if (target && !target->is_native)
        c_code = codegen_generate_for_build_target(program, input, &ast_arena, target);
    else
        c_code = codegen_generate_for_build(program, input, &ast_arena);
    if (!c_code) {
        fprintf(stderr, "error: code generation failed\n");
        arena_free(&source_arena);
        arena_free(&intern_arena);
        arena_free(&ast_arena);
        return 1;
    }

    /* 5. Write to temp file */
    char tmp_c[256];
    snprintf(tmp_c, sizeof(tmp_c), "/tmp/lcn_%d.c", (int)getpid());
    {
        FILE *f = fopen(tmp_c, "w");
        if (!f) {
            fprintf(stderr, "error: cannot write temp file: %s\n", tmp_c);
            free(c_code);
            arena_free(&source_arena);
            arena_free(&intern_arena);
            arena_free(&ast_arena);
            return 1;
        }
        fputs(c_code, f);
        fclose(f);
    }
    free(c_code);
    fprintf(stderr, "  Codegen: %s\n", tmp_c);

    /* 6. Find runtime directory */
    const char *rt_dir = find_runtime_dir(&source_arena, argv0);

    /* Determine compiler to use: cross-compiler or native */
    const char *build_cc = "cc";
    char target_cflags[1024] = "";
    char target_ldflags[1024] = "";
    if (target && target->cc[0]) {
        build_cc = target->cc;
        if (target->cflags[0]) strncpy(target_cflags, target->cflags, sizeof(target_cflags) - 1);
        if (target->ldflags[0]) strncpy(target_ldflags, target->ldflags, sizeof(target_ldflags) - 1);
    }

    /* For cross-compilation, use a separate cache dir to avoid mixing object files */
    const char *obj_prefix = "/tmp/lcn_rt";
    char cross_obj_prefix[256];
    if (target && !target->is_native) {
        snprintf(cross_obj_prefix, sizeof(cross_obj_prefix), "/tmp/lcn_rt_%s",
                 target->triple);
        /* Replace dashes with underscores for filesystem safety */
        {
            char *cp;
            for (cp = cross_obj_prefix; *cp; cp++) {
                if (*cp == '-') *cp = '_';
            }
        }
        obj_prefix = cross_obj_prefix;
    }

    /* 7. Compile runtime .o files (if not cached) */
    const char *rt_files[] = {
        "budget", "json", "http", "llm", "mcp", "channel", "threads", "select",
        "green_threads",
        "event", "httpd", "dashboard_api", "memory", "kb", "string_utils", "mcp_server", "stdlib_rt", "sqlite3",
        "mysql_driver", "access_control", "entropy", "drift", "onnx_model",
        "capability_fence", "delegation", "postgres_driver", "supervisor",
        "router", "a2a", "mesh"
    };
    int n_rt = (int)(sizeof(rt_files) / sizeof(rt_files[0]));

    char rt_objs_str[4096];
    rt_objs_str[0] = '\0';
    int ri;
    for (ri = 0; ri < n_rt; ri++) {
        char src_path[512], obj_path[256];
        snprintf(src_path, sizeof(src_path), "%s/%s.c", rt_dir, rt_files[ri]);
        snprintf(obj_path, sizeof(obj_path), "%s_%s.o", obj_prefix, rt_files[ri]);

        /* Check if .o is newer than .c */
        struct stat src_st, obj_st;
        bool need_compile = true;
        if (stat(src_path, &src_st) != 0) {
            /* Runtime source not found — skip (standalone mode) */
            continue;
        }
        if (stat(obj_path, &obj_st) == 0 &&
            obj_st.st_mtime >= src_st.st_mtime) {
            need_compile = false;
        }

        if (need_compile) {
            char cmd[2048];
            /* sqlite3 needs special flags: suppress warnings, single-threaded */
            if (strcmp(rt_files[ri], "sqlite3") == 0) {
                snprintf(cmd, sizeof(cmd),
                         "%s -std=c99 -O2 -I%s %s -DSQLITE_THREADSAFE=0 "
                         "-DSQLITE_OMIT_LOAD_EXTENSION -w -c %s -o %s 2>&1",
                         build_cc, rt_dir, target_cflags, src_path, obj_path);
            } else if (strcmp(rt_files[ri], "mysql_driver") == 0) {
                /* mysql_driver needs libmysqlclient headers */
                snprintf(cmd, sizeof(cmd),
                         "%s -std=c99 -O2 -Wall -I%s %s "
                         "-I/opt/homebrew/opt/mysql-client/include "
                         "-I/usr/include/mysql "
                         "-c %s -o %s 2>&1",
                         build_cc, rt_dir, target_cflags, src_path, obj_path);
            } else if (strcmp(rt_files[ri], "onnx_model") == 0) {
                /* onnx_model: detect libonnxruntime via pkg-config */
                char onnx_cflags[256] = "";
                {
                    FILE *p = popen("pkg-config --cflags libonnxruntime 2>/dev/null", "r");
                    if (p) {
                        if (fgets(onnx_cflags, sizeof(onnx_cflags), p))
                            onnx_cflags[strcspn(onnx_cflags, "\n")] = '\0';
                        pclose(p);
                    }
                }
                if (strlen(onnx_cflags) > 0) {
                    snprintf(cmd, sizeof(cmd),
                             "%s -std=c99 -O2 -Wall -I%s %s -DLCN_HAS_ONNXRUNTIME %s "
                             "-c %s -o %s 2>&1",
                             build_cc, rt_dir, target_cflags, onnx_cflags, src_path, obj_path);
                } else {
                    snprintf(cmd, sizeof(cmd),
                             "%s -std=c99 -O2 -Wall -I%s %s -c %s -o %s 2>&1",
                             build_cc, rt_dir, target_cflags, src_path, obj_path);
                }
            } else {
                snprintf(cmd, sizeof(cmd),
                         "%s -std=c99 -O2 -Wall -I%s %s -c %s -o %s 2>&1",
                         build_cc, rt_dir, target_cflags, src_path, obj_path);
            }
            int rc = system(cmd);
            if (rc != 0) {
                fprintf(stderr, "error: failed to compile runtime %s.c\n", rt_files[ri]);
                if (target && !target->is_native)
                    fprintf(stderr, "  (cross-compiling for %s with: %s)\n",
                            target->triple, build_cc);
                unlink(tmp_c);
                arena_free(&source_arena);
                arena_free(&intern_arena);
                arena_free(&ast_arena);
                return 1;
            }
        }

        /* Append to object list */
        size_t cur_len = strlen(rt_objs_str);
        snprintf(rt_objs_str + cur_len, sizeof(rt_objs_str) - cur_len,
                 " %s", obj_path);
    }
    if (target && !target->is_native)
        fprintf(stderr, "  Runtime: cross-compiled for %s\n", target->triple);
    else
        fprintf(stderr, "  Runtime: compiled\n");

    /* 7b. Collect link directives from AST (link "-lfoo -lbar") */
    char user_ldflags[2048] = "";
    {
        AstNode *ld;
        for (ld = program->params; ld; ld = ld->next) {
            if (ld->kind == AST_LINK && ld->val.str_val) {
                size_t cur = strlen(user_ldflags);
                snprintf(user_ldflags + cur, sizeof(user_ldflags) - cur,
                         " %s", ld->val.str_val);
            }
        }
        if (user_ldflags[0]) {
            fprintf(stderr, "  Link flags:%s\n", user_ldflags);
        }
    }

    /* 8. Compile and link */
    {
        char cmd[4096];
        if (target && !target->is_native) {
            /* Cross-compilation: use target CC and LDFLAGS, skip host-specific libs */
            snprintf(cmd, sizeof(cmd),
                     "%s -std=c99 -O2 -Wall -Wno-unused-function -Wno-unused-variable "
                     "-I%s %s %s%s -o %s %s%s 2>&1",
                     build_cc, rt_dir, target_cflags, tmp_c, rt_objs_str,
                     output, target_ldflags, user_ldflags);
        } else {
            /* Native compilation — auto-detect optional libraries */
            char extra_libs[512] = "";
            {
                FILE *p = popen("pkg-config --libs libonnxruntime 2>/dev/null", "r");
                if (p) {
                    char buf[256] = "";
                    if (fgets(buf, sizeof(buf), p) && strlen(buf) > 1) {
                        buf[strcspn(buf, "\n")] = '\0';
                        strncat(extra_libs, " ", sizeof(extra_libs) - strlen(extra_libs) - 1);
                        strncat(extra_libs, buf, sizeof(extra_libs) - strlen(extra_libs) - 1);
                    }
                    pclose(p);
                }
            }
            snprintf(cmd, sizeof(cmd),
                     "cc -std=c99 -O2 -Wall -Wno-unused-function -Wno-unused-variable -I%s %s%s -o %s"
                     " -L/opt/homebrew/opt/mysql-client/lib -lmysqlclient -lm%s%s 2>&1",
                     rt_dir, tmp_c, rt_objs_str, output, user_ldflags, extra_libs);
        }
        int rc = system(cmd);
        if (rc != 0) {
            fprintf(stderr, "error: C compilation failed\n");
            if (target && !target->is_native)
                fprintf(stderr, "  compiler: %s\n  target: %s\n", build_cc, target->triple);
            fprintf(stderr, "  command: %s -std=c99 -O2 -Wall -I%s %s ... -o %s\n",
                    build_cc, rt_dir, tmp_c, output);
            unlink(tmp_c);
            arena_free(&source_arena);
            arena_free(&intern_arena);
            arena_free(&ast_arena);
            return 1;
        }
    }

    /* 9. Cleanup temp file */
    unlink(tmp_c);

    fprintf(stderr, "  Output: %s\n", output);
    fprintf(stderr, "  Build complete.\n");

    arena_free(&source_arena);
    arena_free(&intern_arena);
    arena_free(&ast_arena);
    return 0;
}

static int cmd_run(const char *input, const char *argv0) {
    /* Build to a temp binary, then execute it */
    char tmp_bin[256];
    snprintf(tmp_bin, sizeof(tmp_bin), "/tmp/lcn_run_%d", (int)getpid());

    int rc = cmd_build(input, tmp_bin, argv0, false, NULL);
    if (rc != 0) return rc;

    fprintf(stderr, "\n--- Running %s ---\n\n", input);
    rc = system(tmp_bin);
    unlink(tmp_bin);

    /* system() returns the wait status; extract exit code */
    if (WIFEXITED(rc))
        return WEXITSTATUS(rc);
    return 1;
}

/* ============================================================
 * IR Command — Print SSA Intermediate Representation
 * ============================================================ */

static int cmd_ir(const char *filename) {
    Arena source_arena = arena_new(16 * 1024 * 1024);
    Arena intern_arena = arena_new(4 * 1024 * 1024);
    Arena ast_arena = arena_new(32 * 1024 * 1024);

    size_t len;
    char *source = read_source_file(&source_arena, filename, &len);
    if (!source) return 1;

    ErrorReporter reporter = reporter_new(filename, source, len);
    bool had_error = false;
    AstNode *program = parse_source(filename, source, len,
                                     &ast_arena, &intern_arena,
                                     &reporter, &had_error);

    if (had_error || !program) {
        fprintf(stderr, "Parse failed with %d error(s)\n", reporter.count);
        arena_free(&source_arena);
        arena_free(&intern_arena);
        arena_free(&ast_arena);
        return 1;
    }

    /* Generate IR from AST */
    IrModule *mod = ir_gen_program(program, &ast_arena);
    if (!mod) {
        fprintf(stderr, "error: IR generation failed\n");
        arena_free(&source_arena);
        arena_free(&intern_arena);
        arena_free(&ast_arena);
        return 1;
    }

    /* Print IR to stdout */
    ir_print_module(mod, stdout);

    arena_free(&source_arena);
    arena_free(&intern_arena);
    arena_free(&ast_arena);
    return 0;
}

/* ============================================================
 * Audit Command
 * ============================================================ */

typedef struct {
    /* Per-agent metrics */
    int agents;
    int llm_calls;
    int decision_paths;
    int guards;
    int capabilities;
    int access_rules;
    int total_fns;
    int chained_llm;
    bool has_budget;
    bool has_entropy_budget;
    int invariants;

    /* Agent names (up to 32) */
    const char *agent_names[32];
    int agent_count;

    /* Per-agent detail tracking */
    int agent_llm_calls[32];
    int agent_decision_paths[32];
    int agent_guards[32];
    int agent_access_rules[32];
    bool agent_has_budget[32];
    bool agent_has_entropy_budget[32];

    /* For chained LLM detection: track if current subtree is inside ask */
    bool in_ask;
} AuditMetrics;

static void audit_walk(AstNode *node, AuditMetrics *m, int agent_idx);

static void audit_walk(AstNode *node, AuditMetrics *m, int agent_idx) {
    if (!node) return;

    switch (node->kind) {
    case AST_AGENT:
        if (node->name && m->agent_count < 32) {
            int idx = m->agent_count;
            m->agent_names[idx] = node->name;
            m->agent_count++;
            m->agents++;

            /* Scan agent fields */
            {
                AstNode *f = node->params;
                while (f) {
                    if (f->kind == AST_FIELD && f->name) {
                        if (strcmp(f->name, "budget") == 0) {
                            m->has_budget = true;
                            m->agent_has_budget[idx] = true;
                        }
                        if (strcmp(f->name, "entropy_budget") == 0) {
                            m->has_entropy_budget = true;
                            m->agent_has_entropy_budget[idx] = true;
                        }
                        if (strcmp(f->name, "guards") == 0 && f->right && f->right->kind == AST_ARRAY) {
                            int gc = ast_list_len(f->right->params);
                            m->guards += gc;
                            m->agent_guards[idx] += gc;
                        }
                        if (strcmp(f->name, "capabilities") == 0 && f->right && f->right->kind == AST_ARRAY) {
                            m->capabilities += ast_list_len(f->right->params);
                        }
                    }
                    f = f->next;
                }
            }

            /* Walk agent methods with agent_idx */
            audit_walk(node->left, m, idx);
            /* Don't recurse params again (already scanned fields) */
            audit_walk(node->next, m, -1);
            return;
        }
        break;

    case AST_ASK:
        m->llm_calls++;
        if (agent_idx >= 0) m->agent_llm_calls[agent_idx]++;
        if (m->in_ask) m->chained_llm++;
        {
            bool was_in_ask = m->in_ask;
            m->in_ask = true;
            audit_walk(node->left, m, agent_idx);
            audit_walk(node->right, m, agent_idx);
            audit_walk(node->params, m, agent_idx);
            m->in_ask = was_in_ask;
        }
        audit_walk(node->next, m, agent_idx);
        return;

    case AST_IF:
        m->decision_paths++;
        if (agent_idx >= 0) m->agent_decision_paths[agent_idx]++;
        break;

    case AST_MATCH:
        {
            int arms = ast_list_len(node->params);
            m->decision_paths += arms;
            if (agent_idx >= 0) m->agent_decision_paths[agent_idx] += arms;
        }
        break;

    case AST_GUARD:
        m->guards++;
        break;

    case AST_CAPABILITY:
        m->capabilities++;
        /* Count access control rules */
        {
            AstNode *item = node->params;
            while (item) {
                if (item->kind == AST_CAP_ENDPOINT_RULE ||
                    item->kind == AST_CAP_BINARY_RULE ||
                    item->kind == AST_CAP_PATH_RULE ||
                    item->kind == AST_CAP_DENY_RANGE ||
                    item->kind == AST_CAP_DEFAULT) {
                    m->access_rules++;
                }
                item = item->next;
            }
        }
        break;

    case AST_FN:
        m->total_fns++;
        break;

    case AST_INVARIANT:
        m->invariants++;
        break;

    case AST_BUDGET:
        m->has_budget = true;
        break;

    default:
        break;
    }

    audit_walk(node->left, m, agent_idx);
    audit_walk(node->right, m, agent_idx);
    audit_walk(node->params, m, agent_idx);
    audit_walk(node->next, m, agent_idx);
}

static const char *entropy_rating(double score) {
    if (score <= 0.3) return "low -- well-guarded";
    if (score <= 1.0) return "medium";
    if (score <= 3.0) return "high -- review recommended";
    return "critical -- unguarded LLM paths";
}

static int cmd_audit(const char *filename) {
    Arena source_arena = arena_new(16 * 1024 * 1024);
    Arena intern_arena = arena_new(4 * 1024 * 1024);
    Arena ast_arena = arena_new(32 * 1024 * 1024);

    size_t len;
    char *source = read_source_file(&source_arena, filename, &len);
    if (!source) return 1;

    ErrorReporter reporter = reporter_new(filename, source, len);
    bool had_error = false;
    AstNode *program = parse_source(filename, source, len,
                                     &ast_arena, &intern_arena,
                                     &reporter, &had_error);

    if (had_error || !program) {
        fprintf(stderr, "Parse failed with %d error(s)\n", reporter.count);
        arena_free(&source_arena);
        arena_free(&intern_arena);
        arena_free(&ast_arena);
        return 1;
    }

    /* Walk AST and collect metrics */
    AuditMetrics m;
    memset(&m, 0, sizeof(m));
    audit_walk(program->params, &m, -1);

    /* Compute entropy score: decision_paths * llm_calls / (guards + 1) */
    double entropy_score = (double)(m.decision_paths * m.llm_calls) / (double)(m.guards + 1);

    /* Print report */
    printf("\nLimceron Audit -- %s\n", filename);
    printf("========================================\n\n");

    if (m.agent_count > 0) {
        printf("Agents:\n");
        int ai;
        for (ai = 0; ai < m.agent_count; ai++) {
            printf("  %s\n", m.agent_names[ai]);
            printf("    LLM calls:         %d\n", m.agent_llm_calls[ai]);
            printf("    Decision paths:    %d\n", m.agent_decision_paths[ai]);
            printf("    Guards:            %d\n", m.agent_guards[ai]);
            printf("    Budget:            %s\n", m.agent_has_budget[ai] ? "yes" : "no");
            printf("    Entropy budget:    %s\n", m.agent_has_entropy_budget[ai] ? "yes" : "no");
            /* Per-agent entropy score */
            {
                double agent_entropy = (double)(m.agent_decision_paths[ai] * m.agent_llm_calls[ai])
                                       / (double)(m.agent_guards[ai] + 1);
                printf("    Entropy score:     %.2f (%s)\n", agent_entropy,
                       entropy_rating(agent_entropy));
            }
            printf("\n");
        }
    }

    printf("Summary:\n");
    printf("  Total agents:        %d\n", m.agents);
    printf("  Total LLM calls:     %d\n", m.llm_calls);
    printf("  Total guards:        %d\n", m.guards);
    printf("  Total functions:     %d\n", m.total_fns);
    printf("  Decision paths:      %d\n", m.decision_paths);
    printf("  Capabilities:        %d\n", m.capabilities);
    printf("  Access control:      %d rule(s)\n", m.access_rules);
    printf("  Invariants:          %d\n", m.invariants);
    printf("  Chained LLM calls:   %d\n", m.chained_llm);
    printf("  Budget declared:     %s\n", m.has_budget ? "yes" : "no");
    printf("  Entropy budget:      %s\n", m.has_entropy_budget ? "yes" : "no");
    printf("  Overall entropy:     %.2f (%s)\n", entropy_score, entropy_rating(entropy_score));
    printf("\n");

    arena_free(&source_arena);
    arena_free(&intern_arena);
    arena_free(&ast_arena);
    return 0;
}


/* ============================================================
 * Fmt Command — Code Formatter
 * ============================================================ */

typedef struct {
    FILE *out;
    int indent;
} FmtCtx;

static void fmt_indent(FmtCtx *ctx) {
    int i;
    for (i = 0; i < ctx->indent; i++) fprintf(ctx->out, "    ");
}

static void fmt_type_expr(FmtCtx *ctx, AstNode *t);
static void fmt_expr(FmtCtx *ctx, AstNode *e);
static void fmt_stmt(FmtCtx *ctx, AstNode *s);
static void fmt_block(FmtCtx *ctx, AstNode *block);
static void fmt_decl(FmtCtx *ctx, AstNode *d);

/* Emit a type expression */
static void fmt_type_expr(FmtCtx *ctx, AstNode *t) {
    if (!t) return;
    switch (t->kind) {
    case AST_TYPE_NAMED:
        fprintf(ctx->out, "%s", t->name ? t->name : "?");
        if (t->generics) {
            fprintf(ctx->out, "<");
            AstNode *g;
            for (g = t->generics; g; g = g->next) {
                fmt_type_expr(ctx, g);
                if (g->next) fprintf(ctx->out, ", ");
            }
            fprintf(ctx->out, ">");
        }
        break;
    case AST_TYPE_REF:
        fprintf(ctx->out, "&");
        if (t->is_mut) fprintf(ctx->out, "mut ");
        fmt_type_expr(ctx, t->left);
        break;
    case AST_TYPE_PTR:
        fprintf(ctx->out, "*");
        if (t->is_mut) fprintf(ctx->out, "mut ");
        fmt_type_expr(ctx, t->left);
        break;
    case AST_TYPE_ARRAY:
        fprintf(ctx->out, "[");
        fmt_type_expr(ctx, t->left);
        if (t->right) {
            fprintf(ctx->out, "; ");
            fmt_expr(ctx, t->right);
        }
        fprintf(ctx->out, "]");
        break;
    case AST_TYPE_SLICE:
        fprintf(ctx->out, "[");
        fmt_type_expr(ctx, t->left);
        fprintf(ctx->out, "]");
        break;
    case AST_TYPE_OPTIONAL:
        fmt_type_expr(ctx, t->left);
        fprintf(ctx->out, "?");
        break;
    case AST_TYPE_TUPLE:
        fprintf(ctx->out, "(");
        {
            AstNode *p;
            for (p = t->params; p; p = p->next) {
                fmt_type_expr(ctx, p);
                if (p->next) fprintf(ctx->out, ", ");
            }
        }
        fprintf(ctx->out, ")");
        break;
    case AST_TYPE_FN:
        fprintf(ctx->out, "fn(");
        {
            AstNode *p;
            for (p = t->params; p; p = p->next) {
                fmt_type_expr(ctx, p);
                if (p->next) fprintf(ctx->out, ", ");
            }
        }
        fprintf(ctx->out, ")");
        if (t->type_expr) {
            fprintf(ctx->out, " -> ");
            fmt_type_expr(ctx, t->type_expr);
        }
        break;
    case AST_TYPE_UNION:
        {
            AstNode *v;
            for (v = t->params; v; v = v->next) {
                fmt_type_expr(ctx, v);
                if (v->next) fprintf(ctx->out, " | ");
            }
        }
        break;
    case AST_TYPE_TAINTED:
        fprintf(ctx->out, "@%s ", t->name ? t->name : "?");
        fmt_type_expr(ctx, t->left);
        break;
    case AST_TYPE_SECRET:
        fprintf(ctx->out, "secret ");
        fmt_type_expr(ctx, t->left);
        break;
    default:
        if (t->name) fprintf(ctx->out, "%s", t->name);
        else fprintf(ctx->out, "?");
        break;
    }
}

/* Emit an expression */
static void fmt_expr(FmtCtx *ctx, AstNode *e) {
    if (!e) return;
    switch (e->kind) {
    case AST_INT_LIT:
        fprintf(ctx->out, "%lld", (long long)e->val.int_val);
        break;
    case AST_FLOAT_LIT:
        fprintf(ctx->out, "%g", e->val.float_val);
        break;
    case AST_STRING_LIT:
        if (e->val.str_val) {
            /* Check if it contains newlines (use backtick for multiline) */
            const char *s = e->val.str_val;
            bool has_newline = false;
            for (const char *p = s; *p; p++) {
                if (*p == '\n') { has_newline = true; break; }
            }
            if (has_newline) {
                fprintf(ctx->out, "`%s`", s);
            } else {
                /* Escape double-quote strings */
                fprintf(ctx->out, "\"");
                for (const char *p = s; *p; p++) {
                    if (*p == '"') fprintf(ctx->out, "\\\"");
                    else if (*p == '\\') fprintf(ctx->out, "\\\\");
                    else if (*p == '\t') fprintf(ctx->out, "\\t");
                    else if (*p == '\r') fprintf(ctx->out, "\\r");
                    else fputc(*p, ctx->out);
                }
                fprintf(ctx->out, "\"");
            }
        } else {
            fprintf(ctx->out, "\"\"");
        }
        break;
    case AST_BOOL_LIT:
        fprintf(ctx->out, "%s", e->val.bool_val ? "true" : "false");
        break;
    case AST_NONE_LIT:
        fprintf(ctx->out, "none");
        break;
    case AST_IDENT:
        fprintf(ctx->out, "%s", e->name ? e->name : "_");
        break;
    case AST_BINARY:
        fmt_expr(ctx, e->left);
        fprintf(ctx->out, " %s ", token_kind_name(e->val.op));
        fmt_expr(ctx, e->right);
        break;
    case AST_UNARY:
        fprintf(ctx->out, "%s", token_kind_name(e->val.op));
        fmt_expr(ctx, e->left);
        break;
    case AST_CALL:
        fmt_expr(ctx, e->left);
        fprintf(ctx->out, "(");
        {
            AstNode *a;
            for (a = e->params; a; a = a->next) {
                fmt_expr(ctx, a);
                if (a->next) fprintf(ctx->out, ", ");
            }
        }
        fprintf(ctx->out, ")");
        break;
    case AST_METHOD_CALL:
        fmt_expr(ctx, e->left);
        fprintf(ctx->out, ".%s(", e->name ? e->name : "?");
        {
            AstNode *a;
            for (a = e->params; a; a = a->next) {
                fmt_expr(ctx, a);
                if (a->next) fprintf(ctx->out, ", ");
            }
        }
        fprintf(ctx->out, ")");
        break;
    case AST_FIELD_ACCESS:
        fmt_expr(ctx, e->left);
        fprintf(ctx->out, ".%s", e->name ? e->name : "?");
        break;
    case AST_INDEX:
        fmt_expr(ctx, e->left);
        fprintf(ctx->out, "[");
        fmt_expr(ctx, e->right);
        fprintf(ctx->out, "]");
        break;
    case AST_ARRAY:
        fprintf(ctx->out, "[");
        {
            AstNode *el;
            for (el = e->params; el; el = el->next) {
                fmt_expr(ctx, el);
                if (el->next) fprintf(ctx->out, ", ");
            }
        }
        fprintf(ctx->out, "]");
        break;
    case AST_MAP:
        fprintf(ctx->out, "{");
        {
            AstNode *ent;
            for (ent = e->params; ent; ent = ent->next) {
                if (ent->kind == AST_MAP_ENTRY) {
                    fmt_expr(ctx, ent->left);
                    fprintf(ctx->out, ": ");
                    fmt_expr(ctx, ent->right);
                } else {
                    fmt_expr(ctx, ent);
                }
                if (ent->next) fprintf(ctx->out, ", ");
            }
        }
        fprintf(ctx->out, "}");
        break;
    case AST_TUPLE:
        fprintf(ctx->out, "(");
        {
            AstNode *el;
            for (el = e->params; el; el = el->next) {
                fmt_expr(ctx, el);
                if (el->next) fprintf(ctx->out, ", ");
            }
        }
        fprintf(ctx->out, ")");
        break;
    case AST_RANGE:
        fmt_expr(ctx, e->left);
        fprintf(ctx->out, e->is_mut ? "..=" : "..");
        fmt_expr(ctx, e->right);
        break;
    case AST_CAST:
        fmt_expr(ctx, e->left);
        fprintf(ctx->out, " as ");
        fmt_type_expr(ctx, e->type_expr);
        break;
    case AST_IS_EXPR:
        fmt_expr(ctx, e->left);
        fprintf(ctx->out, " is ");
        fmt_type_expr(ctx, e->type_expr);
        break;
    case AST_PIPE:
        fmt_expr(ctx, e->left);
        fprintf(ctx->out, " |> ");
        fmt_expr(ctx, e->right);
        break;
    case AST_SPAWN:
        fprintf(ctx->out, "spawn ");
        fmt_expr(ctx, e->left);
        break;
    case AST_AWAIT:
        fprintf(ctx->out, "await ");
        fmt_expr(ctx, e->left);
        break;
    case AST_TRY:
        fmt_expr(ctx, e->left);
        fprintf(ctx->out, "?");
        break;
    case AST_REF:
        fprintf(ctx->out, "&");
        if (e->is_mut) fprintf(ctx->out, "mut ");
        fmt_expr(ctx, e->left);
        break;
    case AST_DEREF:
        fprintf(ctx->out, "*");
        fmt_expr(ctx, e->left);
        break;
    case AST_CLOSURE:
        fprintf(ctx->out, "|");
        {
            AstNode *p;
            for (p = e->params; p; p = p->next) {
                fprintf(ctx->out, "%s", p->name ? p->name : "_");
                if (p->type_expr) {
                    fprintf(ctx->out, ": ");
                    fmt_type_expr(ctx, p->type_expr);
                }
                if (p->next) fprintf(ctx->out, ", ");
            }
        }
        fprintf(ctx->out, "| ");
        if (e->left && e->left->kind == AST_BLOCK)
            fmt_block(ctx, e->left);
        else
            fmt_expr(ctx, e->left);
        break;
    case AST_ASK:
        fprintf(ctx->out, "ask(");
        if (e->left) fmt_expr(ctx, e->left);
        if (e->params) {
            AstNode *a;
            for (a = e->params; a; a = a->next) {
                fprintf(ctx->out, ", ");
                fmt_expr(ctx, a);
            }
        }
        fprintf(ctx->out, ")");
        break;
    case AST_TELL:
        fprintf(ctx->out, "tell ");
        fmt_expr(ctx, e->left);
        break;
    case AST_ENSURE:
        fprintf(ctx->out, "ensure ");
        fmt_expr(ctx, e->left);
        break;
    case AST_IF:
        /* If used as an expression */
        fprintf(ctx->out, "if ");
        fmt_expr(ctx, e->left);
        fprintf(ctx->out, " ");
        fmt_block(ctx, e->right);
        if (e->params) {
            fprintf(ctx->out, " else ");
            if (e->params->kind == AST_IF) {
                fmt_expr(ctx, e->params);
            } else {
                fmt_block(ctx, e->params);
            }
        }
        break;
    case AST_MATCH:
        fprintf(ctx->out, "match ");
        fmt_expr(ctx, e->left);
        fprintf(ctx->out, " {\n");
        ctx->indent++;
        {
            AstNode *arm;
            for (arm = e->params; arm; arm = arm->next) {
                if (arm->kind == AST_MATCH_ARM) {
                    fmt_indent(ctx);
                    fmt_expr(ctx, arm->left);  /* pattern */
                    fprintf(ctx->out, " -> ");
                    if (arm->right && arm->right->kind == AST_BLOCK)
                        fmt_block(ctx, arm->right);
                    else
                        fmt_expr(ctx, arm->right);
                    fprintf(ctx->out, "\n");
                }
            }
        }
        ctx->indent--;
        fmt_indent(ctx);
        fprintf(ctx->out, "}");
        break;
    case AST_SELECT:
        fprintf(ctx->out, "select {\n");
        ctx->indent++;
        {
            AstNode *arm;
            for (arm = e->params; arm; arm = arm->next) {
                if (arm->kind == AST_SELECT_ARM) {
                    fmt_indent(ctx);
                    fmt_expr(ctx, arm->left);
                    fprintf(ctx->out, " -> ");
                    if (arm->right && arm->right->kind == AST_BLOCK)
                        fmt_block(ctx, arm->right);
                    else
                        fmt_expr(ctx, arm->right);
                    fprintf(ctx->out, "\n");
                }
            }
        }
        ctx->indent--;
        fmt_indent(ctx);
        fprintf(ctx->out, "}");
        break;
    case AST_UNSAFE_BLOCK:
        fprintf(ctx->out, "unsafe ");
        fmt_block(ctx, e->left);
        break;
    case AST_COMPTIME:
        fprintf(ctx->out, "comptime ");
        fmt_block(ctx, e->left);
        break;
    /* Patterns */
    case AST_PAT_WILDCARD:
        fprintf(ctx->out, "_");
        break;
    case AST_PAT_IDENT:
        fprintf(ctx->out, "%s", e->name ? e->name : "_");
        break;
    case AST_PAT_LITERAL:
        fmt_expr(ctx, e->left);
        break;
    case AST_PAT_TYPED:
        fprintf(ctx->out, "%s", e->name ? e->name : "_");
        if (e->type_expr) {
            fprintf(ctx->out, ": ");
            fmt_type_expr(ctx, e->type_expr);
        }
        break;
    case AST_PAT_ENUM:
        fprintf(ctx->out, "%s", e->name ? e->name : "?");
        if (e->params) {
            fprintf(ctx->out, "(");
            AstNode *p;
            for (p = e->params; p; p = p->next) {
                fmt_expr(ctx, p);
                if (p->next) fprintf(ctx->out, ", ");
            }
            fprintf(ctx->out, ")");
        }
        break;
    case AST_PAT_TUPLE:
        fprintf(ctx->out, "(");
        {
            AstNode *p;
            for (p = e->params; p; p = p->next) {
                fmt_expr(ctx, p);
                if (p->next) fprintf(ctx->out, ", ");
            }
        }
        fprintf(ctx->out, ")");
        break;
    case AST_PAT_RANGE:
        fmt_expr(ctx, e->left);
        fprintf(ctx->out, e->is_mut ? "..=" : "..");
        fmt_expr(ctx, e->right);
        break;
    case AST_PAT_OR:
        fmt_expr(ctx, e->left);
        fprintf(ctx->out, " | ");
        fmt_expr(ctx, e->right);
        break;
    default:
        /* Fallback: print the identifier name if available */
        if (e->name) fprintf(ctx->out, "%s", e->name);
        break;
    }
}

/* Emit a block { ... } */
static void fmt_block(FmtCtx *ctx, AstNode *block) {
    if (!block) {
        fprintf(ctx->out, "{ }");
        return;
    }
    AstNode *stmts = (block->kind == AST_BLOCK) ? block->params : block;
    if (!stmts) {
        fprintf(ctx->out, "{ }");
        return;
    }
    fprintf(ctx->out, "{\n");
    ctx->indent++;
    AstNode *s;
    for (s = stmts; s; s = s->next) {
        fmt_stmt(ctx, s);
    }
    ctx->indent--;
    fmt_indent(ctx);
    fprintf(ctx->out, "}");
}

/* Emit a single statement */
static void fmt_stmt(FmtCtx *ctx, AstNode *s) {
    if (!s) return;
    switch (s->kind) {
    case AST_LET:
        fmt_indent(ctx);
        fprintf(ctx->out, "let ");
        if (s->is_mut) fprintf(ctx->out, "mut ");
        fprintf(ctx->out, "%s", s->name ? s->name : "_");
        if (s->type_expr) {
            fprintf(ctx->out, ": ");
            fmt_type_expr(ctx, s->type_expr);
        }
        if (s->right) {
            fprintf(ctx->out, " = ");
            fmt_expr(ctx, s->right);
        }
        fprintf(ctx->out, "\n");
        break;
    case AST_RETURN:
        fmt_indent(ctx);
        fprintf(ctx->out, "return");
        if (s->left) {
            fprintf(ctx->out, " ");
            fmt_expr(ctx, s->left);
        }
        fprintf(ctx->out, "\n");
        break;
    case AST_DEFER:
        fmt_indent(ctx);
        fprintf(ctx->out, "defer ");
        fmt_expr(ctx, s->left);
        fprintf(ctx->out, "\n");
        break;
    case AST_ASSIGN:
        fmt_indent(ctx);
        fmt_expr(ctx, s->left);
        fprintf(ctx->out, " %s ", token_kind_name(s->val.op));
        fmt_expr(ctx, s->right);
        fprintf(ctx->out, "\n");
        break;
    case AST_IF:
        fmt_indent(ctx);
        fprintf(ctx->out, "if ");
        fmt_expr(ctx, s->left);
        fprintf(ctx->out, " ");
        fmt_block(ctx, s->right);
        if (s->params) {
            fprintf(ctx->out, " else ");
            if (s->params->kind == AST_IF) {
                /* else if — inline */
                fprintf(ctx->out, "if ");
                fmt_expr(ctx, s->params->left);
                fprintf(ctx->out, " ");
                fmt_block(ctx, s->params->right);
                if (s->params->params) {
                    fprintf(ctx->out, " else ");
                    fmt_block(ctx, s->params->params);
                }
            } else {
                fmt_block(ctx, s->params);
            }
        }
        fprintf(ctx->out, "\n");
        break;
    case AST_FOR:
        fmt_indent(ctx);
        fprintf(ctx->out, "for ");
        if (s->left) {
            fmt_expr(ctx, s->left);
        } else if (s->name) {
            fprintf(ctx->out, "%s", s->name);
        }
        fprintf(ctx->out, " in ");
        fmt_expr(ctx, s->params);
        fprintf(ctx->out, " ");
        fmt_block(ctx, s->right);
        fprintf(ctx->out, "\n");
        break;
    case AST_WHILE:
        fmt_indent(ctx);
        fprintf(ctx->out, "while ");
        fmt_expr(ctx, s->left);
        fprintf(ctx->out, " ");
        fmt_block(ctx, s->right);
        fprintf(ctx->out, "\n");
        break;
    case AST_LOOP:
        fmt_indent(ctx);
        fprintf(ctx->out, "loop ");
        fmt_block(ctx, s->left);
        fprintf(ctx->out, "\n");
        break;
    case AST_BREAK:
        fmt_indent(ctx);
        fprintf(ctx->out, "break\n");
        break;
    case AST_CONTINUE:
        fmt_indent(ctx);
        fprintf(ctx->out, "continue\n");
        break;
    case AST_MATCH:
        fmt_indent(ctx);
        fmt_expr(ctx, s);
        fprintf(ctx->out, "\n");
        break;
    case AST_EXPR_STMT:
        fmt_indent(ctx);
        fmt_expr(ctx, s->left);
        fprintf(ctx->out, "\n");
        break;
    default:
        /* Other expressions used as statements */
        fmt_indent(ctx);
        fmt_expr(ctx, s);
        fprintf(ctx->out, "\n");
        break;
    }
}

/* Emit generics <T, U: Trait> */
static void fmt_generics(FmtCtx *ctx, AstNode *generics) {
    if (!generics) return;
    fprintf(ctx->out, "<");
    AstNode *g;
    for (g = generics; g; g = g->next) {
        fprintf(ctx->out, "%s", g->name ? g->name : "?");
        if (g->params) {
            fprintf(ctx->out, ": ");
            AstNode *b;
            for (b = g->params; b; b = b->next) {
                fmt_type_expr(ctx, b);
                if (b->next) fprintf(ctx->out, " + ");
            }
        }
        if (g->next) fprintf(ctx->out, ", ");
    }
    fprintf(ctx->out, ">");
}

/* Emit parameter list (a: int, b: string) */
static void fmt_params(FmtCtx *ctx, AstNode *params) {
    fprintf(ctx->out, "(");
    AstNode *p;
    for (p = params; p; p = p->next) {
        if (p->kind == AST_PARAM || p->kind == AST_FIELD) {
            fprintf(ctx->out, "%s", p->name ? p->name : "_");
            if (p->type_expr) {
                fprintf(ctx->out, ": ");
                fmt_type_expr(ctx, p->type_expr);
            }
            if (p->right) {
                fprintf(ctx->out, " = ");
                fmt_expr(ctx, p->right);
            }
        } else {
            fmt_expr(ctx, p);
        }
        if (p->next) fprintf(ctx->out, ", ");
    }
    fprintf(ctx->out, ")");
}

/* Emit a top-level declaration */
static void fmt_decl(FmtCtx *ctx, AstNode *d) {
    if (!d) return;
    switch (d->kind) {
    case AST_USE:
        fprintf(ctx->out, "use %s", d->name ? d->name : "?");
        /* Special use forms: use driver("mysql") as db, use mcp("cmd") as alias, etc.
         * The string argument is stored in val.str_val, alias in right->name */
        if (d->val.str_val && (
            (d->name && strcmp(d->name, "driver") == 0) ||
            (d->name && strcmp(d->name, "mcp") == 0) ||
            (d->name && strcmp(d->name, "a2a") == 0) ||
            (d->name && strcmp(d->name, "model") == 0))) {
            fprintf(ctx->out, "(\"%s\")", d->val.str_val);
            if (d->right && d->right->name) {
                fprintf(ctx->out, " as %s", d->right->name);
            }
        }
        /* Grouped imports: use a.b.{c, d} */
        else if (d->params) {
            fprintf(ctx->out, ".{");
            AstNode *a;
            for (a = d->params; a; a = a->next) {
                fprintf(ctx->out, "%s", a->name ? a->name : "?");
                if (a->next) fprintf(ctx->out, ", ");
            }
            fprintf(ctx->out, "}");
        }
        /* Simple alias: use foo.bar as baz (alias stored in val.str_val) */
        else if (d->val.str_val && !d->right) {
            fprintf(ctx->out, " as %s", d->val.str_val);
        }
        fprintf(ctx->out, "\n");
        break;

    case AST_MODULE:
        fprintf(ctx->out, "mod %s\n", d->name ? d->name : "?");
        break;

    case AST_FN:
        if (d->is_pub) fprintf(ctx->out, "pub ");
        fprintf(ctx->out, "fn %s", d->name ? d->name : "?");
        fmt_generics(ctx, d->generics);
        fmt_params(ctx, d->params);
        if (d->type_expr) {
            fprintf(ctx->out, " -> ");
            fmt_type_expr(ctx, d->type_expr);
        }
        fprintf(ctx->out, " ");
        fmt_block(ctx, d->left);
        fprintf(ctx->out, "\n");
        break;

    case AST_STRUCT:
        if (d->is_pub) fprintf(ctx->out, "pub ");
        fprintf(ctx->out, "struct %s", d->name ? d->name : "?");
        fmt_generics(ctx, d->generics);
        fprintf(ctx->out, " {\n");
        ctx->indent++;
        {
            AstNode *f;
            for (f = d->params; f; f = f->next) {
                fmt_indent(ctx);
                if (f->is_pub) fprintf(ctx->out, "pub ");
                fprintf(ctx->out, "%s", f->name ? f->name : "?");
                if (f->type_expr) {
                    fprintf(ctx->out, ": ");
                    fmt_type_expr(ctx, f->type_expr);
                }
                if (f->right) {
                    fprintf(ctx->out, " = ");
                    fmt_expr(ctx, f->right);
                }
                fprintf(ctx->out, "\n");
            }
        }
        ctx->indent--;
        fprintf(ctx->out, "}\n");
        break;

    case AST_ENUM:
        if (d->is_pub) fprintf(ctx->out, "pub ");
        fprintf(ctx->out, "enum %s", d->name ? d->name : "?");
        fmt_generics(ctx, d->generics);
        fprintf(ctx->out, " {\n");
        ctx->indent++;
        {
            AstNode *v;
            for (v = d->params; v; v = v->next) {
                fmt_indent(ctx);
                fprintf(ctx->out, "%s", v->name ? v->name : "?");
                if (v->params) {
                    fprintf(ctx->out, "(");
                    AstNode *vf;
                    for (vf = v->params; vf; vf = vf->next) {
                        fmt_type_expr(ctx, vf->type_expr ? vf->type_expr : vf);
                        if (vf->next) fprintf(ctx->out, ", ");
                    }
                    fprintf(ctx->out, ")");
                }
                fprintf(ctx->out, "\n");
            }
        }
        ctx->indent--;
        fprintf(ctx->out, "}\n");
        break;

    case AST_AGENT:
        fprintf(ctx->out, "agent %s {\n", d->name ? d->name : "?");
        ctx->indent++;
        {
            /* Agent fields (capabilities, model, budget, prompt, guards, etc.) */
            AstNode *f;
            for (f = d->params; f; f = f->next) {
                if (f->kind == AST_FIELD) {
                    fmt_indent(ctx);
                    fprintf(ctx->out, "%s: ", f->name ? f->name : "?");
                    if (f->right) {
                        fmt_expr(ctx, f->right);
                    }
                    fprintf(ctx->out, "\n");
                } else if (f->kind == AST_USE) {
                    fmt_indent(ctx);
                    fprintf(ctx->out, "use %s", f->name ? f->name : "?");
                    /* Handle comma-separated use list in agent */
                    AstNode *u;
                    for (u = f->next; u && u->kind == AST_USE; ) {
                        fprintf(ctx->out, ", %s", u->name ? u->name : "?");
                        AstNode *skip = u;
                        u = u->next;
                        /* Advance the outer loop past these */
                        f = skip;
                    }
                    fprintf(ctx->out, "\n");
                }
            }
            /* Agent methods (stored in left) */
            if (d->left) {
                AstNode *m = (d->left->kind == AST_BLOCK) ? d->left->params : d->left;
                for (; m; m = m->next) {
                    fprintf(ctx->out, "\n");
                    fmt_indent(ctx);
                    if (m->kind == AST_FN) {
                        if (m->is_pub) fprintf(ctx->out, "pub ");
                        fprintf(ctx->out, "fn %s", m->name ? m->name : "?");
                        fmt_generics(ctx, m->generics);
                        fmt_params(ctx, m->params);
                        if (m->type_expr) {
                            fprintf(ctx->out, " -> ");
                            fmt_type_expr(ctx, m->type_expr);
                        }
                        fprintf(ctx->out, " ");
                        fmt_block(ctx, m->left);
                        fprintf(ctx->out, "\n");
                    } else {
                        fmt_decl(ctx, m);
                    }
                }
            }
        }
        ctx->indent--;
        fprintf(ctx->out, "}\n");
        break;

    case AST_CAPABILITY:
        fprintf(ctx->out, "capability %s {\n", d->name ? d->name : "?");
        ctx->indent++;
        {
            AstNode *item;
            for (item = d->params; item; item = item->next) {
                fmt_indent(ctx);
                switch (item->kind) {
                case AST_CAPABILITY_ITEM:
                    fprintf(ctx->out, "%s", item->name ? item->name : "?");
                    if (item->params) {
                        fprintf(ctx->out, " requires ");
                        AstNode *r;
                        for (r = item->params; r; r = r->next) {
                            fprintf(ctx->out, "%s", r->name ? r->name : "?");
                            if (r->next) fprintf(ctx->out, ", ");
                        }
                    }
                    fprintf(ctx->out, "\n");
                    break;
                case AST_CAP_ENDPOINT_RULE:
                    fprintf(ctx->out, "%s endpoint ", item->is_mut ? "allow" : "deny");
                    if (item->name) fprintf(ctx->out, "\"%s\"", item->name);
                    if (item->params) {
                        fprintf(ctx->out, " {\n");
                        ctx->indent++;
                        AstNode *rf;
                        for (rf = item->params; rf; rf = rf->next) {
                            fmt_indent(ctx);
                            fprintf(ctx->out, "%s: ", rf->name ? rf->name : "?");
                            fmt_expr(ctx, rf->right);
                            fprintf(ctx->out, "\n");
                        }
                        ctx->indent--;
                        fmt_indent(ctx);
                        fprintf(ctx->out, "}\n");
                    } else {
                        fprintf(ctx->out, "\n");
                    }
                    break;
                case AST_CAP_BINARY_RULE:
                    fprintf(ctx->out, "%s binary ", item->is_mut ? "allow" : "deny");
                    if (item->name) fprintf(ctx->out, "\"%s\"", item->name);
                    fprintf(ctx->out, "\n");
                    break;
                case AST_CAP_PATH_RULE:
                    fprintf(ctx->out, "%s path ", item->is_mut ? "allow" : "deny");
                    if (item->name) fprintf(ctx->out, "\"%s\"", item->name);
                    if (item->params) {
                        fprintf(ctx->out, " { ");
                        AstNode *rf;
                        for (rf = item->params; rf; rf = rf->next) {
                            fprintf(ctx->out, "%s: ", rf->name ? rf->name : "?");
                            fmt_expr(ctx, rf->right);
                            if (rf->next) fprintf(ctx->out, ", ");
                        }
                        fprintf(ctx->out, " }");
                    }
                    fprintf(ctx->out, "\n");
                    break;
                case AST_CAP_DENY_RANGE:
                    fprintf(ctx->out, "deny %s\n", item->name ? item->name : "private_ranges");
                    break;
                case AST_CAP_DEFAULT:
                    fprintf(ctx->out, "default: %s\n", item->name ? item->name : "deny");
                    break;
                default:
                    if (item->name) fprintf(ctx->out, "%s\n", item->name);
                    break;
                }
            }
        }
        ctx->indent--;
        fprintf(ctx->out, "}\n");
        break;

    case AST_BUDGET:
        fprintf(ctx->out, "budget %s {\n", d->name ? d->name : "?");
        ctx->indent++;
        {
            AstNode *bf;
            for (bf = d->params; bf; bf = bf->next) {
                fmt_indent(ctx);
                fprintf(ctx->out, "%s: ", bf->name ? bf->name : "?");
                if (bf->right) fmt_expr(ctx, bf->right);
                fprintf(ctx->out, "\n");
            }
        }
        ctx->indent--;
        fprintf(ctx->out, "}\n");
        break;

    case AST_GUARD:
        fprintf(ctx->out, "guard %s", d->name ? d->name : "?");
        fmt_params(ctx, d->params);
        fprintf(ctx->out, " ");
        fmt_block(ctx, d->left);
        fprintf(ctx->out, "\n");
        break;

    case AST_TAINT:
        fprintf(ctx->out, "taint %s\n", d->name ? d->name : "?");
        break;

    case AST_TOOL:
        fprintf(ctx->out, "tool %s", d->name ? d->name : "?");
        fmt_params(ctx, d->params);
        if (d->type_expr) {
            fprintf(ctx->out, " -> ");
            fmt_type_expr(ctx, d->type_expr);
        }
        fprintf(ctx->out, " {\n");
        ctx->indent++;
        {
            AstNode *tf;
            for (tf = d->left ? (d->left->kind == AST_BLOCK ? d->left->params : d->left) : NULL;
                 tf; tf = tf->next) {
                fmt_indent(ctx);
                if (tf->kind == AST_FIELD) {
                    fprintf(ctx->out, "%s: ", tf->name ? tf->name : "?");
                    if (tf->right) fmt_expr(ctx, tf->right);
                    fprintf(ctx->out, "\n");
                } else {
                    fmt_stmt(ctx, tf);
                }
            }
        }
        ctx->indent--;
        fprintf(ctx->out, "}\n");
        break;

    case AST_SKILL:
        fprintf(ctx->out, "skill %s {\n", d->name ? d->name : "?");
        ctx->indent++;
        {
            AstNode *sf;
            for (sf = d->params; sf; sf = sf->next) {
                fmt_indent(ctx);
                fprintf(ctx->out, "%s: ", sf->name ? sf->name : "?");
                if (sf->right) fmt_expr(ctx, sf->right);
                fprintf(ctx->out, "\n");
            }
        }
        ctx->indent--;
        fprintf(ctx->out, "}\n");
        break;

    case AST_SUPERVISOR:
        fprintf(ctx->out, "supervisor %s {\n", d->name ? d->name : "?");
        ctx->indent++;
        {
            AstNode *sf;
            for (sf = d->params; sf; sf = sf->next) {
                fmt_indent(ctx);
                if (sf->kind == AST_SUPERVISOR_CHILD) {
                    fprintf(ctx->out, "child %s", sf->name ? sf->name : "?");
                    if (sf->params) {
                        fprintf(ctx->out, " {\n");
                        ctx->indent++;
                        AstNode *cf;
                        for (cf = sf->params; cf; cf = cf->next) {
                            fmt_indent(ctx);
                            fprintf(ctx->out, "%s: ", cf->name ? cf->name : "?");
                            if (cf->right) fmt_expr(ctx, cf->right);
                            fprintf(ctx->out, "\n");
                        }
                        ctx->indent--;
                        fmt_indent(ctx);
                        fprintf(ctx->out, "}\n");
                    } else {
                        fprintf(ctx->out, "\n");
                    }
                } else if (sf->kind == AST_FIELD) {
                    fprintf(ctx->out, "%s: ", sf->name ? sf->name : "?");
                    if (sf->right) fmt_expr(ctx, sf->right);
                    fprintf(ctx->out, "\n");
                } else {
                    fmt_stmt(ctx, sf);
                }
            }
        }
        ctx->indent--;
        fprintf(ctx->out, "}\n");
        break;

    case AST_MESH:
        fprintf(ctx->out, "mesh %s {\n", d->name ? d->name : "?");
        ctx->indent++;
        {
            AstNode *mf;
            for (mf = d->params; mf; mf = mf->next) {
                fmt_indent(ctx);
                if (mf->kind == AST_MESH_STAGE) {
                    fprintf(ctx->out, "stage %s", mf->name ? mf->name : "?");
                    if (mf->params) {
                        fprintf(ctx->out, " {\n");
                        ctx->indent++;
                        AstNode *sf;
                        for (sf = mf->params; sf; sf = sf->next) {
                            fmt_indent(ctx);
                            fprintf(ctx->out, "%s: ", sf->name ? sf->name : "?");
                            if (sf->right) fmt_expr(ctx, sf->right);
                            fprintf(ctx->out, "\n");
                        }
                        ctx->indent--;
                        fmt_indent(ctx);
                        fprintf(ctx->out, "}\n");
                    } else {
                        fprintf(ctx->out, "\n");
                    }
                } else if (mf->kind == AST_FIELD) {
                    fprintf(ctx->out, "%s: ", mf->name ? mf->name : "?");
                    if (mf->right) fmt_expr(ctx, mf->right);
                    fprintf(ctx->out, "\n");
                }
            }
        }
        ctx->indent--;
        fprintf(ctx->out, "}\n");
        break;

    case AST_MEMORY:
        fprintf(ctx->out, "memory %s {\n", d->name ? d->name : "?");
        ctx->indent++;
        {
            AstNode *mf;
            for (mf = d->params; mf; mf = mf->next) {
                fmt_indent(ctx);
                fprintf(ctx->out, "%s: ", mf->name ? mf->name : "?");
                if (mf->right) fmt_expr(ctx, mf->right);
                fprintf(ctx->out, "\n");
            }
        }
        ctx->indent--;
        fprintf(ctx->out, "}\n");
        break;

    case AST_CHANNEL:
        fprintf(ctx->out, "channel %s", d->name ? d->name : "?");
        if (d->type_expr) {
            fprintf(ctx->out, ": ");
            fmt_type_expr(ctx, d->type_expr);
        }
        if (d->params) {
            fprintf(ctx->out, " {\n");
            ctx->indent++;
            AstNode *cf;
            for (cf = d->params; cf; cf = cf->next) {
                fmt_indent(ctx);
                fprintf(ctx->out, "%s: ", cf->name ? cf->name : "?");
                if (cf->right) fmt_expr(ctx, cf->right);
                fprintf(ctx->out, "\n");
            }
            ctx->indent--;
            fprintf(ctx->out, "}\n");
        } else {
            fprintf(ctx->out, "\n");
        }
        break;

    case AST_GUARDSET:
        fprintf(ctx->out, "guardset %s {\n", d->name ? d->name : "?");
        ctx->indent++;
        {
            AstNode *gf;
            for (gf = d->params; gf; gf = gf->next) {
                fmt_indent(ctx);
                fprintf(ctx->out, "%s\n", gf->name ? gf->name : "?");
            }
        }
        ctx->indent--;
        fprintf(ctx->out, "}\n");
        break;

    case AST_INVARIANT:
        fprintf(ctx->out, "invariant %s {\n", d->name ? d->name : "?");
        ctx->indent++;
        if (d->left) {
            AstNode *stmts = (d->left->kind == AST_BLOCK) ? d->left->params : d->left;
            AstNode *s;
            for (s = stmts; s; s = s->next) {
                fmt_stmt(ctx, s);
            }
        }
        ctx->indent--;
        fprintf(ctx->out, "}\n");
        break;

    case AST_TRAIT:
    case AST_INTERFACE:
        if (d->is_pub) fprintf(ctx->out, "pub ");
        fprintf(ctx->out, "%s %s", d->kind == AST_TRAIT ? "trait" : "interface",
                d->name ? d->name : "?");
        fmt_generics(ctx, d->generics);
        fprintf(ctx->out, " {\n");
        ctx->indent++;
        {
            AstNode *m;
            for (m = d->params; m; m = m->next) {
                fmt_indent(ctx);
                fmt_decl(ctx, m);
            }
        }
        ctx->indent--;
        fprintf(ctx->out, "}\n");
        break;

    case AST_IMPL:
        fprintf(ctx->out, "impl");
        fmt_generics(ctx, d->generics);
        fprintf(ctx->out, " ");
        if (d->right) {
            fmt_type_expr(ctx, d->right);
            fprintf(ctx->out, " for ");
        }
        fmt_type_expr(ctx, d->left);
        fprintf(ctx->out, " {\n");
        ctx->indent++;
        {
            AstNode *m;
            for (m = d->params; m; m = m->next) {
                fmt_indent(ctx);
                fmt_decl(ctx, m);
                fprintf(ctx->out, "\n");
            }
        }
        ctx->indent--;
        fprintf(ctx->out, "}\n");
        break;

    case AST_CONST:
        if (d->is_pub) fprintf(ctx->out, "pub ");
        fprintf(ctx->out, "const %s", d->name ? d->name : "?");
        if (d->type_expr) {
            fprintf(ctx->out, ": ");
            fmt_type_expr(ctx, d->type_expr);
        }
        if (d->right) {
            fprintf(ctx->out, " = ");
            fmt_expr(ctx, d->right);
        }
        fprintf(ctx->out, "\n");
        break;

    case AST_TYPE_ALIAS:
        if (d->is_pub) fprintf(ctx->out, "pub ");
        fprintf(ctx->out, "type %s", d->name ? d->name : "?");
        fmt_generics(ctx, d->generics);
        fprintf(ctx->out, " = ");
        fmt_type_expr(ctx, d->type_expr);
        fprintf(ctx->out, "\n");
        break;

    case AST_PROMPT:
        fprintf(ctx->out, "prompt %s {\n", d->name ? d->name : "?");
        ctx->indent++;
        {
            AstNode *ps;
            for (ps = d->params; ps; ps = ps->next) {
                fmt_indent(ctx);
                fprintf(ctx->out, "%s: ", ps->name ? ps->name : "?");
                if (ps->right) fmt_expr(ctx, ps->right);
                fprintf(ctx->out, "\n");
            }
        }
        ctx->indent--;
        fprintf(ctx->out, "}\n");
        break;

    case AST_ROUTER:
        fprintf(ctx->out, "router %s {\n", d->name ? d->name : "?");
        ctx->indent++;
        {
            AstNode *rr;
            for (rr = d->params; rr; rr = rr->next) {
                fmt_indent(ctx);
                if (rr->kind == AST_ROUTE_RULE) {
                    fprintf(ctx->out, "route %s", rr->name ? rr->name : "?");
                    if (rr->params) {
                        fprintf(ctx->out, " {\n");
                        ctx->indent++;
                        AstNode *rf;
                        for (rf = rr->params; rf; rf = rf->next) {
                            fmt_indent(ctx);
                            fprintf(ctx->out, "%s: ", rf->name ? rf->name : "?");
                            if (rf->right) fmt_expr(ctx, rf->right);
                            fprintf(ctx->out, "\n");
                        }
                        ctx->indent--;
                        fmt_indent(ctx);
                        fprintf(ctx->out, "}\n");
                    } else {
                        fprintf(ctx->out, "\n");
                    }
                } else if (rr->kind == AST_STRATEGY) {
                    fprintf(ctx->out, "strategy %s\n", rr->name ? rr->name : "?");
                } else if (rr->kind == AST_FIELD) {
                    fprintf(ctx->out, "%s: ", rr->name ? rr->name : "?");
                    if (rr->right) fmt_expr(ctx, rr->right);
                    fprintf(ctx->out, "\n");
                }
            }
        }
        ctx->indent--;
        fprintf(ctx->out, "}\n");
        break;

    default:
        /* Fallback: print as comment if unrecognized */
        fprintf(ctx->out, "// (unformatted: %s)\n", ast_kind_name(d->kind));
        break;
    }
}

static int cmd_fmt(const char *filename, bool write_in_place) {
    Arena source_arena = arena_new(16 * 1024 * 1024);
    Arena intern_arena = arena_new(4 * 1024 * 1024);
    Arena ast_arena = arena_new(32 * 1024 * 1024);

    size_t len;
    char *source = read_source_file(&source_arena, filename, &len);
    if (!source) return 1;

    ErrorReporter reporter = reporter_new(filename, source, len);
    bool had_error = false;
    AstNode *program = parse_source(filename, source, len,
                                     &ast_arena, &intern_arena,
                                     &reporter, &had_error);

    if (had_error || !program) {
        fprintf(stderr, "error: cannot format file with parse errors (%d error(s))\n",
                reporter.count);
        arena_free(&source_arena);
        arena_free(&intern_arena);
        arena_free(&ast_arena);
        return 1;
    }

    FILE *out;
    char tmp_path[512];
    tmp_path[0] = '\0';

    if (write_in_place) {
        /* Write to a temp file first, then rename */
        snprintf(tmp_path, sizeof(tmp_path), "%s.fmt.tmp", filename);
        out = fopen(tmp_path, "w");
        if (!out) {
            fprintf(stderr, "error: cannot create temp file: %s\n", tmp_path);
            arena_free(&source_arena);
            arena_free(&intern_arena);
            arena_free(&ast_arena);
            return 1;
        }
    } else {
        out = stdout;
    }

    FmtCtx ctx;
    ctx.out = out;
    ctx.indent = 0;

    /* Walk all top-level declarations */
    AstNode *d;
    AstNode *prev = NULL;
    for (d = program->params; d; d = d->next) {
        /* One blank line between top-level declarations (except use/module at top) */
        if (prev) {
            bool prev_is_header = (prev->kind == AST_USE || prev->kind == AST_MODULE ||
                                   prev->kind == AST_TAINT);
            bool curr_is_header = (d->kind == AST_USE || d->kind == AST_MODULE ||
                                   d->kind == AST_TAINT);
            if (prev_is_header && curr_is_header) {
                /* No blank line between consecutive use/taint declarations */
            } else {
                fprintf(out, "\n");
            }
        }
        fmt_decl(&ctx, d);
        prev = d;
    }

    if (write_in_place) {
        fclose(out);
        if (rename(tmp_path, filename) != 0) {
            fprintf(stderr, "error: cannot rename %s -> %s: %s\n",
                    tmp_path, filename, strerror(errno));
            unlink(tmp_path);
            arena_free(&source_arena);
            arena_free(&intern_arena);
            arena_free(&ast_arena);
            return 1;
        }
        fprintf(stderr, "Formatted: %s\n", filename);
    }

    arena_free(&source_arena);
    arena_free(&intern_arena);
    arena_free(&ast_arena);
    return 0;
}

/* ============================================================
 * Doctor Command — Build Environment Diagnostics
 * ============================================================ */

/* Run a command and capture the first line of output into buf.
 * Returns the exit code (0 = success). */
static int run_capture(const char *cmd, char *buf, size_t bufsz) {
    buf[0] = '\0';
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    if (fgets(buf, (int)bufsz, p)) {
        /* Trim trailing newline */
        size_t blen = strlen(buf);
        while (blen > 0 && (buf[blen-1] == '\n' || buf[blen-1] == '\r'))
            buf[--blen] = '\0';
    }
    return pclose(p);
}

static int cmd_doctor(const char *argv0) {
    int checks_total = 0;
    int checks_ok = 0;
    int checks_warn = 0;
    char buf[1024];

    printf("Limceron Doctor\n");
    printf("===============\n");

    /* 1. System info */
    {
        char os_buf[128] = "unknown";
        char arch_buf[128] = "unknown";
        run_capture("uname -s 2>/dev/null", os_buf, sizeof(os_buf));
        run_capture("uname -m 2>/dev/null", arch_buf, sizeof(arch_buf));
        printf("[OK] System: %s %s\n", os_buf, arch_buf);
        checks_total++; checks_ok++;
    }

    /* 2. C compiler */
    checks_total++;
    {
        int rc = run_capture("cc --version 2>&1 | head -1", buf, sizeof(buf));
        if (rc == 0 && buf[0] != '\0') {
            printf("[OK] C compiler: %s\n", buf);
            checks_ok++;
        } else {
            printf("[!!] C compiler: not found (cc --version failed)\n");
        }
    }

    /* 3. Runtime directory */
    checks_total++;
    {
        const char *rt_dir = NULL;
        Arena tmp_arena = arena_new(4096);
        rt_dir = find_runtime_dir(&tmp_arena, argv0);
        struct stat st;
        if (rt_dir && stat(rt_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
            /* Count .c files */
            char count_cmd[512];
            snprintf(count_cmd, sizeof(count_cmd),
                     "ls '%s'/*.c 2>/dev/null | wc -l", rt_dir);
            run_capture(count_cmd, buf, sizeof(buf));
            int n = atoi(buf);
            printf("[OK] Runtime: %d files found (%s)\n", n, rt_dir);
            checks_ok++;
        } else {
            printf("[!!] Runtime: directory not found\n");
        }
        arena_free(&tmp_arena);
    }

    /* 4. Stdlib directory */
    checks_total++;
    {
        const char *std_dir = NULL;
        Arena tmp_arena = arena_new(4096);
        std_dir = find_stdlib_dir(&tmp_arena, argv0);
        struct stat st;
        if (std_dir && stat(std_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
            char count_cmd[512];
            snprintf(count_cmd, sizeof(count_cmd),
                     "ls '%s'/*.lceron 2>/dev/null | wc -l", std_dir);
            run_capture(count_cmd, buf, sizeof(buf));
            int n = atoi(buf);
            printf("[OK] Stdlib: %d modules found (%s)\n", n, std_dir);
            checks_ok++;
        } else {
            printf("[!!] Stdlib: directory not found\n");
        }
        arena_free(&tmp_arena);
    }

    /* 5. Build directory writable */
    checks_total++;
    {
        char tmp_file[256];
        snprintf(tmp_file, sizeof(tmp_file), "/tmp/lcn_doctor_%d.tmp", (int)getpid());
        FILE *f = fopen(tmp_file, "w");
        if (f) {
            fprintf(f, "ok");
            fclose(f);
            unlink(tmp_file);
            printf("[OK] Build directory: writable (/tmp)\n");
            checks_ok++;
        } else {
            printf("[!!] Build directory: /tmp not writable\n");
        }
    }

    /* 6. Optional: PostgreSQL (libpq) */
    checks_total++;
    {
        int rc = run_capture("pkg-config --exists libpq 2>/dev/null && echo found || "
                             "(pg_config --version 2>/dev/null && echo found || echo missing)",
                             buf, sizeof(buf));
        /* Check if we got something useful */
        bool found = false;
        if (rc == 0 && strstr(buf, "found")) found = true;
        if (!found) {
            /* Try pg_config directly */
            rc = run_capture("pg_config --version 2>/dev/null", buf, sizeof(buf));
            if (rc == 0 && buf[0] != '\0') found = true;
        }
        if (found) {
            printf("[OK] PostgreSQL: libpq found\n");
            checks_ok++;
        } else {
            printf("[--] PostgreSQL: not found (optional)\n");
            checks_warn++;
        }
    }

    /* 7. Optional: ONNX Runtime */
    checks_total++;
    {
        int rc = run_capture("pkg-config --exists libonnxruntime 2>/dev/null && echo found || echo missing",
                             buf, sizeof(buf));
        if (rc == 0 && strstr(buf, "found")) {
            printf("[OK] ONNX Runtime: found\n");
            checks_ok++;
        } else {
            printf("[--] ONNX Runtime: not found (optional)\n");
            checks_warn++;
        }
    }

    /* 8. Optional: MySQL */
    checks_total++;
    {
        int rc = run_capture("mysql_config --version 2>/dev/null", buf, sizeof(buf));
        if (rc == 0 && buf[0] != '\0') {
            printf("[OK] MySQL: mysql_config found (v%s)\n", buf);
            checks_ok++;
        } else {
            printf("[--] MySQL: not found (optional)\n");
            checks_warn++;
        }
    }

    /* 9. Self-test: compile and run hello world */
    checks_total++;
    {
        char tmp_c[256], tmp_bin[256];
        snprintf(tmp_c, sizeof(tmp_c), "/tmp/lcn_doctor_%d.c", (int)getpid());
        snprintf(tmp_bin, sizeof(tmp_bin), "/tmp/lcn_doctor_%d", (int)getpid());

        FILE *f = fopen(tmp_c, "w");
        if (f) {
            fprintf(f,
                "#include <stdio.h>\n"
                "int main(void) { printf(\"hello world\\n\"); return 0; }\n");
            fclose(f);

            char cmd[512];
            snprintf(cmd, sizeof(cmd), "cc -o '%s' '%s' 2>/dev/null", tmp_bin, tmp_c);
            int rc = system(cmd);
            if (rc == 0) {
                char run_cmd[512];
                snprintf(run_cmd, sizeof(run_cmd), "'%s' 2>/dev/null", tmp_bin);
                run_capture(run_cmd, buf, sizeof(buf));
                if (strcmp(buf, "hello world") == 0) {
                    printf("[OK] Self-test: hello world compiled and ran\n");
                    checks_ok++;
                } else {
                    printf("[!!] Self-test: compiled but unexpected output: %s\n", buf);
                }
            } else {
                printf("[!!] Self-test: compilation failed\n");
            }
            unlink(tmp_c);
            unlink(tmp_bin);
        } else {
            printf("[!!] Self-test: cannot create temp file\n");
        }
    }

    /* Summary */
    printf("\n");
    int checks_fail = checks_total - checks_ok - checks_warn;
    if (checks_fail == 0) {
        printf("All checks passed");
        if (checks_warn > 0)
            printf(" (%d optional dependenc%s not found)", checks_warn,
                   checks_warn == 1 ? "y" : "ies");
        printf(".\n");
    } else {
        printf("%d of %d checks failed.\n", checks_fail, checks_total);
    }

    return checks_fail > 0 ? 1 : 0;
}

/* ============================================================
 * Init Command — Project Scaffolding
 * ============================================================ */

static int do_init(const char *name) {
    char path[512];

    /* Create project directory */
    snprintf(path, sizeof(path), "%s", name);
    mkdir(path, 0755);

    /* Create src/ directory */
    snprintf(path, sizeof(path), "%s/src", name);
    mkdir(path, 0755);

    /* Create tests/ directory */
    snprintf(path, sizeof(path), "%s/tests", name);
    mkdir(path, 0755);

    /* Create limceron.toml */
    {
        LcnPackage pkg;
        memset(&pkg, 0, sizeof(pkg));
        strncpy(pkg.name, name, LCN_PKG_MAX_NAME - 1);
        strncpy(pkg.version, "0.1.0", LCN_PKG_MAX_VERSION - 1);
        snprintf(pkg.description, LCN_PKG_MAX_DESC, "A Limceron project");

        char proj_dir[512];
        snprintf(proj_dir, sizeof(proj_dir), "%s", name);
        lcn_package_save(proj_dir, &pkg);
    }

    /* Create src/main.lceron */
    snprintf(path, sizeof(path), "%s/src/main.lceron", name);
    {
        FILE *f = fopen(path, "w");
        if (!f) { fprintf(stderr, "error: cannot create %s\n", path); return 1; }
        fprintf(f,
            "capability llm {\n"
            "    complete\n"
            "}\n\n"
            "budget DefaultBudget {\n"
            "    max_tokens: 10000\n"
            "    max_cost: 1.00\n"
            "    max_duration: 60\n"
            "}\n\n"
            "agent %s {\n"
            "    capabilities: [llm.complete]\n"
            "    model: \"llama3\"\n"
            "    budget: DefaultBudget\n"
            "    prompt: \"You are a helpful assistant.\"\n\n"
            "    fn run(input: string) -> Result {\n"
            "        let result = ask(input)\n"
            "        match result {\n"
            "            Ok(text) -> { println(text) }\n"
            "            Error(msg) -> { println(msg) }\n"
            "            _ -> { println(\"unexpected\") }\n"
            "        }\n"
            "    }\n"
            "}\n\n"
            "fn main() -> Result {\n"
            "    let agent = %s\n"
            "    agent.run(\"Hello, world!\")\n"
            "    println(\"done\")\n"
            "}\n",
            name, name
        );
        fclose(f);
    }

    /* Create tests/test_main.lceron */
    snprintf(path, sizeof(path), "%s/tests/test_main.lceron", name);
    {
        FILE *f = fopen(path, "w");
        if (!f) { fprintf(stderr, "error: cannot create %s\n", path); return 1; }
        fprintf(f,
            "use %s\n\n"
            "fn test_hello() -> Result {\n"
            "    println(\"test passed\")\n"
            "}\n\n"
            "fn main() -> Result {\n"
            "    test_hello()\n"
            "}\n",
            name
        );
        fclose(f);
    }

    fprintf(stderr, "Created project: %s/\n", name);
    fprintf(stderr, "  %s/limceron.toml        (package manifest)\n", name);
    fprintf(stderr, "  %s/src/main.lceron      (entry point)\n", name);
    fprintf(stderr, "  %s/tests/test_main.lceron (test file)\n", name);
    fprintf(stderr, "\nBuild:  limceron build %s/src/main.lceron -o %s\n", name, name);
    fprintf(stderr, "Run:    limceron run %s/src/main.lceron\n", name);
    fprintf(stderr, "Add:    limceron add <package> [version]\n");

    return 0;
}

/* ============================================================
 * Add Command — Add dependency to limceron.toml
 * ============================================================ */

static int cmd_add(const char *pkg_name, const char *version, bool is_path,
                   bool is_git, bool is_dev) {
    LcnPackage pkg;
    if (!lcn_package_load(".", &pkg)) {
        fprintf(stderr, "error: no limceron.toml found in current directory\n");
        fprintf(stderr, "  Run `limceron init <name>` first.\n");
        return 1;
    }

    LcnDepKind kind = LCN_DEP_REGISTRY;
    const char *path_or_url = NULL;

    if (is_path) {
        kind = LCN_DEP_PATH;
        path_or_url = version;  /* version arg is the path for path deps */
        version = NULL;
    } else if (is_git) {
        kind = LCN_DEP_GIT;
        path_or_url = version;  /* version arg is the URL for git deps */
        version = NULL;
    }

    if (!version && kind == LCN_DEP_REGISTRY) {
        version = "*";
    }

    if (!lcn_package_add_dep(&pkg, pkg_name, version, kind, path_or_url, is_dev)) {
        fprintf(stderr, "error: too many dependencies (max %d)\n", LCN_PKG_MAX_DEPS);
        return 1;
    }

    if (!lcn_package_save(".", &pkg)) {
        fprintf(stderr, "error: cannot write limceron.toml\n");
        return 1;
    }

    fprintf(stderr, "Added %s %s to %s\n",
            pkg_name,
            version ? version : (path_or_url ? path_or_url : ""),
            is_dev ? "[dev-dependencies]" : "[dependencies]");

    /* Try to install immediately */
    LcnDependency *dep = NULL;
    for (int i = 0; i < pkg.dep_count; i++) {
        if (strcmp(pkg.deps[i].name, pkg_name) == 0) {
            dep = &pkg.deps[i];
            break;
        }
    }
    if (dep) {
        lcn_install_dep(".", dep);
    }

    return 0;
}

/* ============================================================
 * Install Command — Install all dependencies
 * ============================================================ */

static int cmd_install(void) {
    LcnPackage pkg;
    if (!lcn_package_load(".", &pkg)) {
        fprintf(stderr, "error: no limceron.toml found in current directory\n");
        return 1;
    }

    fprintf(stderr, "Installing dependencies for %s %s ...\n",
            pkg.name, pkg.version);

    if (pkg.dep_count == 0) {
        fprintf(stderr, "  No dependencies to install.\n");
        return 0;
    }

    int installed = lcn_install_all(".", &pkg);
    fprintf(stderr, "\nInstalled %d of %d dependencies.\n",
            installed, pkg.dep_count);

    return installed >= 0 ? 0 : 1;
}

/* ============================================================
 * Publish Command — Package for distribution
 * ============================================================ */

static int cmd_publish(void) {
    LcnPackage pkg;
    if (!lcn_package_load(".", &pkg)) {
        fprintf(stderr, "error: no limceron.toml found in current directory\n");
        return 1;
    }

    char tarball_path[LCN_PKG_MAX_PATH];
    if (!lcn_publish_package(".", tarball_path, sizeof(tarball_path))) {
        return 1;
    }

    fprintf(stderr, "\nPackage created: %s\n", tarball_path);
    fprintf(stderr, "\nTo share this package:\n");
    fprintf(stderr, "  1. Host the tarball at a URL or git repository\n");
    fprintf(stderr, "  2. Others can add it with:\n");
    fprintf(stderr, "     limceron add %s --git <repo-url>\n", pkg.name);
    fprintf(stderr, "     limceron add %s --path <local-path>\n", pkg.name);

    return 0;
}

/* ============================================================
 * Main
 * ============================================================ */

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Limceron Programming Language — Stage 0 Bootstrap Compiler\n"
        "Version: %s\n"
        "\n"
        "Usage:\n"
        "  %s build   <file> [-o <output>] [--target <triple>] [--static]   Compile to binary\n"
        "  %s run     <file>                                     Compile and execute\n"
        "  %s emit    <file> [-o <output.c>]                     Transpile to C (standalone)\n"
        "  %s fmt     <file> [-w]                                Format source code\n"
        "  %s parse   <file>                                     Parse and print AST (debug)\n"
        "  %s ir      <file>                                     Print SSA IR (debug)\n"
        "  %s audit   <file>                                     Audit: entropy, guards, LLM paths\n"
        "  %s targets                                            List cross-compilation targets\n"
        "  %s doctor                                             Check build environment\n"
        "  %s init    [name]                                     Create new project scaffold\n"
        "  %s add     <package> [version]                        Add dependency\n"
        "  %s install                                            Install all dependencies\n"
        "  %s publish                                            Package for distribution\n"
        "  %s lsp                                                Start LSP server (stdin/stdout)\n"
        "  %s lex     <file>                                     Tokenize and print (debug)\n"
        "  %s version                                            Show version\n"
        "  %s help                                               Show this help\n"
        "\n"
        "Cross-Compilation:\n"
        "  --target <triple>   Target triple: ARCH-OS[-ABI]\n"
        "                      Examples: aarch64-linux, x86_64-linux-musl, aarch64-darwin\n"
        "  --static            Link statically (Linux targets, for containers/Lambda)\n"
        "\n"
        "Package commands:\n"
        "  add <pkg> <version>        Add registry dependency\n"
        "  add <pkg> --path <dir>     Add local path dependency\n"
        "  add <pkg> --git <url>      Add git dependency\n"
        "  add <pkg> --dev            Add as dev-dependency\n"
        "\n"
        "Formats:\n"
        "  .lceron        Full Limceron syntax\n"
        "  .lceron.md     Markdown-as-source (same output as .lceron)\n"
        "\n",
        LCN_VERSION, prog, prog, prog, prog, prog, prog, prog, prog, prog,
        prog, prog, prog, prog, prog, prog, prog, prog);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "version") == 0) {
        printf("Limceron %s\n", LCN_VERSION);
        return 0;
    }

    if (strcmp(argv[1], "help") == 0) {
        print_usage(argv[0]);
        return 0;
    }

    if (strcmp(argv[1], "lex") == 0) {
        if (argc < 3) {
            fprintf(stderr, "error: 'lex' requires a source file\n");
            return 1;
        }
        return cmd_lex(argv[2]);
    }

    if (strcmp(argv[1], "parse") == 0) {
        if (argc < 3) {
            fprintf(stderr, "error: 'parse' requires a source file\n");
            return 1;
        }
        return cmd_parse(argv[2]);
    }

    if (strcmp(argv[1], "emit") == 0) {
        if (argc < 3) {
            fprintf(stderr, "error: 'emit' requires a source file\n");
            return 1;
        }
        const char *out = NULL;
        int i;
        for (i = 3; i < argc - 1; i++) {
            if (strcmp(argv[i], "-o") == 0) {
                out = argv[i + 1];
                i++;
            }
        }
        return cmd_emit(argv[2], out);
    }

    if (strcmp(argv[1], "build") == 0) {
        if (argc < 3) {
            fprintf(stderr, "error: 'build' requires a source file\n");
            return 1;
        }
        const char *input = NULL;
        const char *output = "a.out";
        bool serve_mode = false;
        const char *target_triple = NULL;
        bool static_link = false;
        int i;
        for (i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                output = argv[i + 1];
                i++;
            } else if (strcmp(argv[i], "--serve") == 0) {
                serve_mode = true;
            } else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) {
                target_triple = argv[i + 1];
                i++;
            } else if (strcmp(argv[i], "--static") == 0) {
                static_link = true;
            } else if (argv[i][0] != '-' && !input) {
                /* First non-flag argument is the input file */
                input = argv[i];
            }
        }
        if (!input) {
            fprintf(stderr, "error: 'build' requires a source file\n");
            return 1;
        }

        /* Resolve cross-compilation target */
        LcnTarget target;
        LcnTarget *target_ptr = NULL;
        if (target_triple) {
            target = lcn_parse_target(target_triple);
            target.static_link = static_link;

            if (target.arch == LCN_ARCH_UNKNOWN) {
                fprintf(stderr, "error: unknown architecture in target '%s'\n", target_triple);
                fprintf(stderr, "  Supported: x86_64, aarch64, arm\n");
                return 1;
            }
            if (target.os == LCN_OS_UNKNOWN) {
                fprintf(stderr, "error: unknown OS in target '%s'\n", target_triple);
                fprintf(stderr, "  Supported: linux, darwin, windows\n");
                return 1;
            }
            if (target.os == LCN_OS_WINDOWS) {
                fprintf(stderr, "warning: Windows target is a stub; compilation may fail.\n");
            }

            /* Recompute LDFLAGS with static_link set */
            if (static_link) {
                target = lcn_parse_target(target_triple);
                target.static_link = true;
                if (target.os == LCN_OS_LINUX) {
                    snprintf(target.ldflags, sizeof(target.ldflags),
                             "-static -lpthread -lm -ldl");
                } else if (target.os == LCN_OS_DARWIN) {
                    fprintf(stderr, "warning: --static is not supported on macOS; ignoring.\n");
                }
            }

            /* Find cross-compiler */
            if (!lcn_find_cross_cc(&target)) {
                fprintf(stderr, "error: no cross-compiler found for target '%s'\n",
                        target.triple);
                fprintf(stderr, "  Install a cross-compiler or zig, or set LCN_CC_%s_%s\n",
                        lcn_arch_str(target.arch), lcn_os_str(target.os));
                fprintf(stderr, "  Run '%s targets' to see available targets.\n", argv[0]);
                return 1;
            }
            fprintf(stderr, "  Cross-compiler: %s\n", target.cc);
            target_ptr = &target;
        } else if (static_link) {
            /* --static without --target: use native target */
            target = lcn_native_target();
            target.static_link = true;
            if (target.os == LCN_OS_LINUX)
                snprintf(target.ldflags, sizeof(target.ldflags),
                         "-static -lpthread -lm -ldl");
            target_ptr = &target;
        }

        return cmd_build(input, output, argv[0], serve_mode, target_ptr);
    }

    if (strcmp(argv[1], "targets") == 0) {
        return cmd_targets();
    }

    if (strcmp(argv[1], "run") == 0) {
        if (argc < 3) {
            fprintf(stderr, "error: 'run' requires a source file\n");
            return 1;
        }
        return cmd_run(argv[2], argv[0]);
    }

    if (strcmp(argv[1], "audit") == 0) {
        if (argc < 3) {
            fprintf(stderr, "error: 'audit' requires a source file\n");
            return 1;
        }
        return cmd_audit(argv[2]);
    }

    if (strcmp(argv[1], "fmt") == 0) {
        if (argc < 3) {
            fprintf(stderr, "error: 'fmt' requires a source file\n");
            return 1;
        }
        bool write_in_place = false;
        int i;
        for (i = 3; i < argc; i++) {
            if (strcmp(argv[i], "-w") == 0) {
                write_in_place = true;
            }
        }
        return cmd_fmt(argv[2], write_in_place);
    }

    if (strcmp(argv[1], "doctor") == 0) {
        return cmd_doctor(argv[0]);
    }

    if (strcmp(argv[1], "init") == 0) {
        return do_init(argc > 2 ? argv[2] : "my-agent");
    }

    if (strcmp(argv[1], "ir") == 0) {
        if (argc < 3) {
            fprintf(stderr, "error: 'ir' requires a source file\n");
            return 1;
        }
        return cmd_ir(argv[2]);
    }

    if (strcmp(argv[1], "lsp") == 0) {
        return cmd_lsp();
    }

    if (strcmp(argv[1], "add") == 0) {
        if (argc < 3) {
            fprintf(stderr, "error: 'add' requires a package name\n");
            fprintf(stderr, "Usage: limceron add <package> [version]\n");
            fprintf(stderr, "       limceron add <package> --path <dir>\n");
            fprintf(stderr, "       limceron add <package> --git <url>\n");
            return 1;
        }
        const char *pkg_name = argv[2];
        const char *version = NULL;
        bool is_path = false;
        bool is_git = false;
        bool is_dev = false;
        int i;
        for (i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) {
                is_path = true;
                version = argv[++i];
            } else if (strcmp(argv[i], "--git") == 0 && i + 1 < argc) {
                is_git = true;
                version = argv[++i];
            } else if (strcmp(argv[i], "--dev") == 0) {
                is_dev = true;
            } else if (!version) {
                version = argv[i];
            }
        }
        return cmd_add(pkg_name, version, is_path, is_git, is_dev);
    }

    if (strcmp(argv[1], "install") == 0) {
        return cmd_install();
    }

    if (strcmp(argv[1], "publish") == 0) {
        return cmd_publish();
    }

    fprintf(stderr, "error: unknown command '%s'\n", argv[1]);
    fprintf(stderr, "Run '%s help' for usage.\n", argv[0]);
    return 1;
}
