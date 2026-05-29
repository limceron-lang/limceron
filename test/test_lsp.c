/*
 * Limceron Stage 0 -- LSP test suite (L10).
 *
 * Covers:
 *   1) JSON-RPC framing helpers (lsp_json_get_string / lsp_json_get_int).
 *   2) Diagnostic transformation -- feed a known-bad source and assert
 *      the JSON payload that publishDiagnostics would emit.
 *   3) Hover -- feed a known source + cursor and assert the hover text
 *      matches the AST-derived declaration.
 *   4) Completion -- feed a cursor in scope and assert keywords +
 *      scope identifiers + host-module member set are surfaced.
 *
 * The helpers under test live in src/lsp.c and are exposed via
 * include/lcn.h (lsp_test_*_for_source family). The full request /
 * response loop (cmd_lsp) is not unit-tested here -- that path is
 * exercised by editor integration smoke tests.
 */

#include "lcn.h"
#include "test.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * 1) JSON-RPC framing helpers
 * ============================================================ */

TEST(lsp_json_get_string_basic) {
    char buf[64];
    const char *src =
        "{\"jsonrpc\":\"2.0\",\"method\":\"initialize\",\"id\":7}";
    const char *r = lsp_json_get_string(src, "method", buf, sizeof(buf));
    ASSERT_NOT_NULL(r);
    ASSERT_STR_EQ(buf, "initialize");
}

TEST(lsp_json_get_string_with_escape) {
    char buf[64];
    const char *src = "{\"text\":\"hello\\nworld\"}";
    const char *r = lsp_json_get_string(src, "text", buf, sizeof(buf));
    ASSERT_NOT_NULL(r);
    /* lsp_json_get_string decodes \n -> newline */
    ASSERT_STR_EQ(buf, "hello\nworld");
}

TEST(lsp_json_get_string_missing_key) {
    char buf[64];
    const char *src = "{\"a\":\"b\"}";
    const char *r = lsp_json_get_string(src, "missing", buf, sizeof(buf));
    ASSERT_NULL(r);
}

TEST(lsp_json_get_int_basic) {
    const char *src = "{\"id\":42,\"line\":5}";
    long id = lsp_json_get_int(src, "id");
    long line = lsp_json_get_int(src, "line");
    ASSERT_EQ(id, 42);
    ASSERT_EQ(line, 5);
}

TEST(lsp_json_get_int_missing_returns_neg1) {
    const char *src = "{\"id\":42}";
    long n = lsp_json_get_int(src, "absent");
    ASSERT_EQ(n, -1);
}

TEST(lsp_json_get_int_handles_whitespace) {
    const char *src = "{\"line\"   :    13}";
    long n = lsp_json_get_int(src, "line");
    ASSERT_EQ(n, 13);
}

/* ============================================================
 * 2) Diagnostic transformation
 * ============================================================ */

/* A well-formed source produces an empty diagnostics array. */
TEST(lsp_diagnostics_clean_source_is_empty) {
    char buf[4096];
    /* Minimal valid Limceron fn -- no errors expected. */
    int n = lsp_test_diagnostics_for_source(
        "file:///clean.lceron",
        "fn main() {\n  return 0;\n}\n",
        buf, sizeof(buf));
    ASSERT(n > 0);
    /* Payload shape: {"uri":"...","diagnostics":[]} */
    ASSERT_NOT_NULL(strstr(buf, "\"diagnostics\":[]"));
    ASSERT_NOT_NULL(strstr(buf, "\"uri\":\"file:///clean.lceron\""));
}

/* A source with a parse-level mistake produces at least one diagnostic
 * carrying the limceron source tag + a LCN-NNN code. */
TEST(lsp_diagnostics_bad_source_reports_error) {
    char buf[4096];
    /* Open brace without function header -- guaranteed parse error. */
    int n = lsp_test_diagnostics_for_source(
        "file:///bad.lceron",
        "fn main( {\n  return 0;\n}\n",
        buf, sizeof(buf));
    ASSERT(n > 0);
    /* The diagnostic array should not be empty. */
    ASSERT_NULL(strstr(buf, "\"diagnostics\":[]"));
    /* The payload must include limceron source attribution and an LCN
     * code -- editors key off both. */
    ASSERT_NOT_NULL(strstr(buf, "\"source\":\"limceron\""));
    ASSERT_NOT_NULL(strstr(buf, "\"code\":\"LCN-"));
    /* Severity 1 = Error. */
    ASSERT_NOT_NULL(strstr(buf, "\"severity\":1"));
}

/* ============================================================
 * 3) Hover
 * ============================================================ */

TEST(lsp_hover_keyword_returns_description) {
    char buf[1024];
    /* Cursor on the `fn` keyword at (0,0). */
    int ok = lsp_test_hover_for_source("fn main() {}\n", 0, 0,
                                        buf, sizeof(buf));
    ASSERT_EQ(ok, 1);
    /* The keyword description format starts with **<word>**. */
    ASSERT_NOT_NULL(strstr(buf, "**fn**"));
}

TEST(lsp_hover_user_fn_returns_signature) {
    char buf[2048];
    /* `greet` declared on line 0. Cursor on its name. */
    const char *src = "fn greet(name: string) {\n  return;\n}\n";
    int ok = lsp_test_hover_for_source(src, 0, 5, buf, sizeof(buf));
    ASSERT_EQ(ok, 1);
    /* The hover should embed the function signature inside a fenced
     * code block. */
    ASSERT_NOT_NULL(strstr(buf, "fn greet"));
}

TEST(lsp_hover_unannotated_let_shows_l9_pending_stub) {
    char buf[2048];
    /* Top-level let with no annotation -- L9 inference results are
     * not exposed on AstNode yet, so hover should fall back to the
     * documented stub. */
    const char *src = "let x = 1;\n";
    int ok = lsp_test_hover_for_source(src, 0, 4, buf, sizeof(buf));
    if (ok) {
        /* If the parser surfaces the let at top-level, the L9-pending
         * stub must be in the hover text. */
        ASSERT_NOT_NULL(strstr(buf, "L9"));
    }
}

/* ============================================================
 * 4) Completion
 * ============================================================ */

TEST(lsp_completion_in_scope_surfaces_keywords) {
    char buf[16384];
    int n = lsp_test_completion_for_source("fn main() {\n  \n}\n",
                                            1, 2, buf, sizeof(buf));
    ASSERT(n > 0);
    /* Keywords + builtins are always in the in-scope completion list. */
    ASSERT_NOT_NULL(strstr(buf, "\"label\":\"fn\""));
    ASSERT_NOT_NULL(strstr(buf, "\"label\":\"let\""));
    ASSERT_NOT_NULL(strstr(buf, "\"label\":\"return\""));
    /* Builtins like println must also appear. */
    ASSERT_NOT_NULL(strstr(buf, "\"label\":\"println\""));
}

TEST(lsp_completion_after_host_module_dot_surfaces_members) {
    char buf[16384];
    /* Cursor sits immediately after `llm.` on line 1.
     *
     * `  llm.` -- chars at columns 0,1 are spaces, then 'l','l','m','.'
     * lives at column 5. Cursor immediately after the dot is at
     * character index 6 (the LSP convention is "the cursor is before
     * the byte at `character`"). The trigger detector inspects the
     * byte at `character - 1`. */
    const char *src = "fn main() {\n  llm.\n}\n";
    int n = lsp_test_completion_for_source(src, 1, 6, buf, sizeof(buf));
    ASSERT(n > 0);
    /* Host module members must be surfaced; keywords/builtins must NOT. */
    ASSERT_NOT_NULL(strstr(buf, "\"label\":\"classify\""));
    ASSERT_NOT_NULL(strstr(buf, "\"detail\":\"host:llm\""));
    ASSERT_NULL(strstr(buf, "\"label\":\"return\""));
}

TEST(lsp_completion_after_double_colon_surfaces_enum_variants) {
    char buf[16384];
    /* Cursor inside `Result::Ok` between the `::` and `Ok`. Picking
     * a fully-parseable token keeps the parser happy -- a bare
     * `Result::` would emit a parse error and confuse the test. */
    const char *src = "fn main() {\n  let x = Result::Ok;\n}\n";
    /* `  let x = Result::Ok` -- the second `:` sits at column 17,
     * cursor immediately after at character=18 (before the `O`). */
    int n = lsp_test_completion_for_source(src, 1, 18, buf, sizeof(buf));
    ASSERT(n > 0);
    /* `Result::` should yield `Ok` and `Err` as variant entries. */
    ASSERT_NOT_NULL(strstr(buf, "\"label\":\"Ok\""));
    ASSERT_NOT_NULL(strstr(buf, "\"label\":\"Err\""));
    ASSERT_NOT_NULL(strstr(buf, "\"detail\":\"variant\""));
}

TEST(lsp_completion_in_scope_surfaces_user_identifiers) {
    char buf[16384];
    /* User-declared fn `compute` should appear in the completion list. */
    const char *src =
        "fn compute(a: int) {\n"
        "  return a + 1;\n"
        "}\n"
        "fn main() {\n"
        "  \n"
        "}\n";
    int n = lsp_test_completion_for_source(src, 4, 2, buf, sizeof(buf));
    ASSERT(n > 0);
    ASSERT_NOT_NULL(strstr(buf, "\"label\":\"compute\""));
}

/* ============================================================
 * Entry point
 * ============================================================ */

int main(void) {
    fprintf(stderr, "\n=== LSP test suite ===\n");

    /* JSON-RPC framing */
    RUN_TEST(lsp_json_get_string_basic);
    RUN_TEST(lsp_json_get_string_with_escape);
    RUN_TEST(lsp_json_get_string_missing_key);
    RUN_TEST(lsp_json_get_int_basic);
    RUN_TEST(lsp_json_get_int_missing_returns_neg1);
    RUN_TEST(lsp_json_get_int_handles_whitespace);

    /* Diagnostics */
    RUN_TEST(lsp_diagnostics_clean_source_is_empty);
    RUN_TEST(lsp_diagnostics_bad_source_reports_error);

    /* Hover */
    RUN_TEST(lsp_hover_keyword_returns_description);
    RUN_TEST(lsp_hover_user_fn_returns_signature);
    RUN_TEST(lsp_hover_unannotated_let_shows_l9_pending_stub);

    /* Completion */
    RUN_TEST(lsp_completion_in_scope_surfaces_keywords);
    RUN_TEST(lsp_completion_after_host_module_dot_surfaces_members);
    RUN_TEST(lsp_completion_after_double_colon_surfaces_enum_variants);
    RUN_TEST(lsp_completion_in_scope_surfaces_user_identifiers);

    TEST_SUMMARY();
    return test_failures > 0 ? 1 : 0;
}
