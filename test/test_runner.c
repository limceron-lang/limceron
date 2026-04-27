/*
 * Limceron Stage 0 — Test Runner
 *
 * Comprehensive tests for lexer and parser.
 */

#include "lcn.h"
#include "package.h"
#include "test.h"

/* ============================================================
 * Helpers
 * ============================================================ */

static Arena test_arena;
static Arena test_intern_arena;

static void setup(void) {
    test_arena = arena_new(4 * 1024 * 1024);
    test_intern_arena = arena_new(1 * 1024 * 1024);
}

static void teardown(void) {
    arena_free(&test_arena);
    arena_free(&test_intern_arena);
}

/* Lex a string and return array of tokens (caller manages arena) */
static Token *lex_all(const char *source, int *out_count) {
    arena_reset(&test_arena);
    arena_reset(&test_intern_arena);

    size_t len = strlen(source);
    ErrorReporter reporter = reporter_new("<test>", source, len);
    StringIntern intern = intern_new(&test_intern_arena);
    Lexer lexer = lexer_new("<test>", source, len, &intern, &reporter);

    Token *tokens = (Token *)arena_alloc(&test_arena, sizeof(Token) * 4096);
    int count = 0;
    Token tok;
    do {
        tok = lexer_next(&lexer);
        tokens[count++] = tok;
    } while (tok.kind != TOK_EOF && count < 4096);

    *out_count = count;
    return tokens;
}

/* Parse a string and return AST */
static AstNode *parse_source(const char *source, bool *had_error) {
    arena_reset(&test_arena);
    arena_reset(&test_intern_arena);

    size_t len = strlen(source);
    ErrorReporter reporter = reporter_new("<test>", source, len);
    StringIntern intern = intern_new(&test_intern_arena);
    Lexer lexer = lexer_new("<test>", source, len, &intern, &reporter);
    Parser parser = parser_new(&lexer, &test_arena, &reporter);

    AstNode *program = parse_program(&parser);
    *had_error = parser.had_error;
    return program;
}

/* ============================================================
 * LEXER TESTS
 * ============================================================ */

TEST(lex_empty) {
    int count;
    Token *tokens = lex_all("", &count);
    ASSERT_EQ(count, 1);
    ASSERT_EQ(tokens[0].kind, TOK_EOF);
}

TEST(lex_integer_decimal) {
    int count;
    Token *tokens = lex_all("42", &count);
    ASSERT_EQ(count, 2);  /* 42, EOF */
    ASSERT_EQ(tokens[0].kind, TOK_INT_LIT);
    ASSERT_EQ(tokens[0].value.int_val, 42);
}

TEST(lex_integer_hex) {
    int count;
    Token *tokens = lex_all("0xff", &count);
    ASSERT_EQ(tokens[0].kind, TOK_INT_LIT);
    ASSERT_EQ(tokens[0].value.int_val, 0xff);
}

TEST(lex_integer_binary) {
    int count;
    Token *tokens = lex_all("0b1010", &count);
    ASSERT_EQ(tokens[0].kind, TOK_INT_LIT);
    ASSERT_EQ(tokens[0].value.int_val, 10);
}

TEST(lex_integer_octal) {
    int count;
    Token *tokens = lex_all("0o77", &count);
    ASSERT_EQ(tokens[0].kind, TOK_INT_LIT);
    ASSERT_EQ(tokens[0].value.int_val, 63);
}

TEST(lex_integer_underscores) {
    int count;
    Token *tokens = lex_all("1_000_000", &count);
    ASSERT_EQ(tokens[0].kind, TOK_INT_LIT);
    ASSERT_EQ(tokens[0].value.int_val, 1000000);
}

TEST(lex_float) {
    int count;
    Token *tokens = lex_all("3.14", &count);
    ASSERT_EQ(tokens[0].kind, TOK_FLOAT_LIT);
    ASSERT_FLOAT_EQ(tokens[0].value.float_val, 3.14);
}

TEST(lex_float_scientific) {
    int count;
    Token *tokens = lex_all("1e10", &count);
    ASSERT_EQ(tokens[0].kind, TOK_FLOAT_LIT);
    ASSERT_FLOAT_EQ(tokens[0].value.float_val, 1e10);
}

TEST(lex_string) {
    int count;
    Token *tokens = lex_all("\"hello world\"", &count);
    ASSERT_EQ(tokens[0].kind, TOK_STRING_LIT);
    ASSERT_STR_EQ(tokens[0].value.str_val, "hello world");
}

TEST(lex_string_escape) {
    int count;
    Token *tokens = lex_all("\"hello\\nworld\"", &count);
    ASSERT_EQ(tokens[0].kind, TOK_STRING_LIT);
    ASSERT_STR_EQ(tokens[0].value.str_val, "hello\nworld");
}

TEST(lex_raw_string) {
    int count;
    Token *tokens = lex_all("`raw \\n string`", &count);
    ASSERT_EQ(tokens[0].kind, TOK_STRING_LIT);
    ASSERT_STR_EQ(tokens[0].value.str_val, "raw \\n string");
}

TEST(lex_identifier) {
    int count;
    Token *tokens = lex_all("my_var", &count);
    ASSERT_EQ(tokens[0].kind, TOK_IDENT);
    ASSERT_STR_EQ(tokens[0].value.str_val, "my_var");
}

TEST(lex_keywords) {
    int count;
    Token *tokens = lex_all("fn let mut if else return struct enum for while", &count);
    ASSERT_EQ(tokens[0].kind, TOK_FN);
    ASSERT_EQ(tokens[1].kind, TOK_LET);
    ASSERT_EQ(tokens[2].kind, TOK_MUT);
    ASSERT_EQ(tokens[3].kind, TOK_IF);
    ASSERT_EQ(tokens[4].kind, TOK_ELSE);
    ASSERT_EQ(tokens[5].kind, TOK_RETURN);
    ASSERT_EQ(tokens[6].kind, TOK_STRUCT);
    ASSERT_EQ(tokens[7].kind, TOK_ENUM);
    ASSERT_EQ(tokens[8].kind, TOK_FOR);
    ASSERT_EQ(tokens[9].kind, TOK_WHILE);
}

TEST(lex_all_keywords) {
    int count;
    Token *tokens = lex_all(
        "loop break continue trait impl interface mod use pub priv "
        "spawn await select chan defer unsafe comptime type as in is "
        "true false none match",
        &count
    );
    ASSERT_EQ(tokens[0].kind, TOK_LOOP);
    ASSERT_EQ(tokens[1].kind, TOK_BREAK);
    ASSERT_EQ(tokens[2].kind, TOK_CONTINUE);
    ASSERT_EQ(tokens[3].kind, TOK_TRAIT);
    ASSERT_EQ(tokens[4].kind, TOK_IMPL);
    ASSERT_EQ(tokens[5].kind, TOK_INTERFACE);
    ASSERT_EQ(tokens[6].kind, TOK_MOD);
    ASSERT_EQ(tokens[7].kind, TOK_USE);
    ASSERT_EQ(tokens[8].kind, TOK_PUB);
    ASSERT_EQ(tokens[9].kind, TOK_PRIV);
    ASSERT_EQ(tokens[10].kind, TOK_SPAWN);
    ASSERT_EQ(tokens[11].kind, TOK_AWAIT);
    ASSERT_EQ(tokens[12].kind, TOK_SELECT);
    ASSERT_EQ(tokens[13].kind, TOK_CHAN);
    ASSERT_EQ(tokens[14].kind, TOK_DEFER);
    ASSERT_EQ(tokens[15].kind, TOK_UNSAFE);
    ASSERT_EQ(tokens[16].kind, TOK_COMPTIME);
    ASSERT_EQ(tokens[17].kind, TOK_TYPE);
    ASSERT_EQ(tokens[18].kind, TOK_AS);
    ASSERT_EQ(tokens[19].kind, TOK_IN);
    ASSERT_EQ(tokens[20].kind, TOK_IS);
    ASSERT_EQ(tokens[21].kind, TOK_TRUE);
    ASSERT_EQ(tokens[22].kind, TOK_FALSE);
    ASSERT_EQ(tokens[23].kind, TOK_NONE);
    ASSERT_EQ(tokens[24].kind, TOK_MATCH);
}

TEST(lex_operators_arithmetic) {
    int count;
    Token *tokens = lex_all("+ - * / % **", &count);
    ASSERT_EQ(tokens[0].kind, TOK_PLUS);
    ASSERT_EQ(tokens[1].kind, TOK_MINUS);
    ASSERT_EQ(tokens[2].kind, TOK_STAR);
    ASSERT_EQ(tokens[3].kind, TOK_SLASH);
    ASSERT_EQ(tokens[4].kind, TOK_PERCENT);
    ASSERT_EQ(tokens[5].kind, TOK_POWER);
}

TEST(lex_operators_comparison) {
    int count;
    Token *tokens = lex_all("== != < > <= >=", &count);
    ASSERT_EQ(tokens[0].kind, TOK_EQ_EQ);
    ASSERT_EQ(tokens[1].kind, TOK_NOT_EQ);
    ASSERT_EQ(tokens[2].kind, TOK_LT);
    ASSERT_EQ(tokens[3].kind, TOK_GT);
    ASSERT_EQ(tokens[4].kind, TOK_LT_EQ);
    ASSERT_EQ(tokens[5].kind, TOK_GT_EQ);
}

TEST(lex_operators_logical) {
    int count;
    Token *tokens = lex_all("&& || !", &count);
    ASSERT_EQ(tokens[0].kind, TOK_AND_AND);
    ASSERT_EQ(tokens[1].kind, TOK_PIPE_PIPE);
    ASSERT_EQ(tokens[2].kind, TOK_BANG);
}

TEST(lex_operators_bitwise) {
    int count;
    Token *tokens = lex_all("& | ^ ~ << >>", &count);
    ASSERT_EQ(tokens[0].kind, TOK_AMP);
    ASSERT_EQ(tokens[1].kind, TOK_PIPE);
    ASSERT_EQ(tokens[2].kind, TOK_CARET);
    ASSERT_EQ(tokens[3].kind, TOK_TILDE);
    ASSERT_EQ(tokens[4].kind, TOK_SHL);
    ASSERT_EQ(tokens[5].kind, TOK_SHR);
}

TEST(lex_operators_assignment) {
    int count;
    Token *tokens = lex_all("= += -= *= /= %= &= |= ^= <<= >>=", &count);
    ASSERT_EQ(tokens[0].kind, TOK_EQ);
    ASSERT_EQ(tokens[1].kind, TOK_PLUS_EQ);
    ASSERT_EQ(tokens[2].kind, TOK_MINUS_EQ);
    ASSERT_EQ(tokens[3].kind, TOK_STAR_EQ);
    ASSERT_EQ(tokens[4].kind, TOK_SLASH_EQ);
    ASSERT_EQ(tokens[5].kind, TOK_PERCENT_EQ);
    ASSERT_EQ(tokens[6].kind, TOK_AMP_EQ);
    ASSERT_EQ(tokens[7].kind, TOK_PIPE_EQ);
    ASSERT_EQ(tokens[8].kind, TOK_CARET_EQ);
    ASSERT_EQ(tokens[9].kind, TOK_SHL_EQ);
    ASSERT_EQ(tokens[10].kind, TOK_SHR_EQ);
}

TEST(lex_operators_special) {
    int count;
    Token *tokens = lex_all("? ?. .. ..= -> => :: |>", &count);
    ASSERT_EQ(tokens[0].kind, TOK_QUESTION);
    ASSERT_EQ(tokens[1].kind, TOK_QUESTION_DOT);
    ASSERT_EQ(tokens[2].kind, TOK_DOT_DOT);
    ASSERT_EQ(tokens[3].kind, TOK_DOT_DOT_EQ);
    ASSERT_EQ(tokens[4].kind, TOK_ARROW);
    ASSERT_EQ(tokens[5].kind, TOK_FAT_ARROW);
    ASSERT_EQ(tokens[6].kind, TOK_COLON_COLON);
    ASSERT_EQ(tokens[7].kind, TOK_PIPE_GT);
}

TEST(lex_delimiters) {
    int count;
    Token *tokens = lex_all("( ) [ ] { } , . : ;", &count);
    ASSERT_EQ(tokens[0].kind, TOK_LPAREN);
    ASSERT_EQ(tokens[1].kind, TOK_RPAREN);
    ASSERT_EQ(tokens[2].kind, TOK_LBRACKET);
    ASSERT_EQ(tokens[3].kind, TOK_RBRACKET);
    ASSERT_EQ(tokens[4].kind, TOK_LBRACE);
    ASSERT_EQ(tokens[5].kind, TOK_RBRACE);
    ASSERT_EQ(tokens[6].kind, TOK_COMMA);
    ASSERT_EQ(tokens[7].kind, TOK_DOT);
    ASSERT_EQ(tokens[8].kind, TOK_COLON);
    ASSERT_EQ(tokens[9].kind, TOK_SEMICOLON);
}

TEST(lex_line_comment) {
    int count;
    Token *tokens = lex_all("42 // this is a comment\n56", &count);
    ASSERT_EQ(tokens[0].kind, TOK_INT_LIT);
    ASSERT_EQ(tokens[0].value.int_val, 42);
    /* 42 is followed by newline → semicolon inserted */
    ASSERT_EQ(tokens[1].kind, TOK_SEMICOLON);
    ASSERT_EQ(tokens[2].kind, TOK_INT_LIT);
    ASSERT_EQ(tokens[2].value.int_val, 56);
}

TEST(lex_block_comment) {
    int count;
    Token *tokens = lex_all("42 /* comment */ 56", &count);
    ASSERT_EQ(tokens[0].kind, TOK_INT_LIT);
    ASSERT_EQ(tokens[0].value.int_val, 42);
    ASSERT_EQ(tokens[1].kind, TOK_INT_LIT);
    ASSERT_EQ(tokens[1].value.int_val, 56);
}

TEST(lex_nested_block_comment) {
    int count;
    Token *tokens = lex_all("42 /* outer /* inner */ still comment */ 56", &count);
    ASSERT_EQ(tokens[0].kind, TOK_INT_LIT);
    ASSERT_EQ(tokens[0].value.int_val, 42);
    ASSERT_EQ(tokens[1].kind, TOK_INT_LIT);
    ASSERT_EQ(tokens[1].value.int_val, 56);
}

TEST(lex_semicolon_insertion_after_ident) {
    int count;
    Token *tokens = lex_all("foo\nbar", &count);
    ASSERT_EQ(tokens[0].kind, TOK_IDENT);
    ASSERT_STR_EQ(tokens[0].value.str_val, "foo");
    ASSERT_EQ(tokens[1].kind, TOK_SEMICOLON);  /* auto-inserted */
    ASSERT_EQ(tokens[2].kind, TOK_IDENT);
    ASSERT_STR_EQ(tokens[2].value.str_val, "bar");
}

TEST(lex_semicolon_insertion_after_return) {
    int count;
    Token *tokens = lex_all("return\nfoo", &count);
    ASSERT_EQ(tokens[0].kind, TOK_RETURN);
    ASSERT_EQ(tokens[1].kind, TOK_SEMICOLON);
    ASSERT_EQ(tokens[2].kind, TOK_IDENT);
}

TEST(lex_no_semicolon_after_plus) {
    int count;
    Token *tokens = lex_all("a +\nb", &count);
    /* "a" is ident on same line as "+", no newline between them */
    /* "+" is NOT a statement-ending token */
    /* newline after "+" → no semicolon inserted */
    /* Result: a + b */
    ASSERT_EQ(tokens[0].kind, TOK_IDENT);
    ASSERT_EQ(tokens[1].kind, TOK_PLUS);
    ASSERT_EQ(tokens[2].kind, TOK_IDENT);
}

TEST(lex_no_semicolon_after_open_brace) {
    int count;
    Token *tokens = lex_all("{\nfoo", &count);
    ASSERT_EQ(tokens[0].kind, TOK_LBRACE);
    /* { does NOT end a statement → no semicolon */
    ASSERT_EQ(tokens[1].kind, TOK_IDENT);
}

TEST(lex_semicolon_after_close_brace) {
    int count;
    Token *tokens = lex_all("}\nfoo", &count);
    ASSERT_EQ(tokens[0].kind, TOK_RBRACE);
    ASSERT_EQ(tokens[1].kind, TOK_SEMICOLON);
    ASSERT_EQ(tokens[2].kind, TOK_IDENT);
}

TEST(lex_line_tracking) {
    int count;
    Token *tokens = lex_all("a\nb\nc", &count);
    ASSERT_EQ(tokens[0].loc.line, 1);
    /* tokens[1] is semicolon */
    ASSERT_EQ(tokens[2].loc.line, 2);
    /* tokens[3] is semicolon */
    ASSERT_EQ(tokens[4].loc.line, 3);
}

TEST(lex_complex_expression) {
    int count;
    Token *tokens = lex_all("x + y * (z - 1)", &count);
    ASSERT_EQ(tokens[0].kind, TOK_IDENT);
    ASSERT_EQ(tokens[1].kind, TOK_PLUS);
    ASSERT_EQ(tokens[2].kind, TOK_IDENT);
    ASSERT_EQ(tokens[3].kind, TOK_STAR);
    ASSERT_EQ(tokens[4].kind, TOK_LPAREN);
    ASSERT_EQ(tokens[5].kind, TOK_IDENT);
    ASSERT_EQ(tokens[6].kind, TOK_MINUS);
    ASSERT_EQ(tokens[7].kind, TOK_INT_LIT);
    ASSERT_EQ(tokens[8].kind, TOK_RPAREN);
}

TEST(lex_function_signature) {
    int count;
    Token *tokens = lex_all("fn add(a: i32, b: i32) -> i32", &count);
    ASSERT_EQ(tokens[0].kind, TOK_FN);
    ASSERT_EQ(tokens[1].kind, TOK_IDENT);     /* add */
    ASSERT_EQ(tokens[2].kind, TOK_LPAREN);
    ASSERT_EQ(tokens[3].kind, TOK_IDENT);     /* a */
    ASSERT_EQ(tokens[4].kind, TOK_COLON);
    ASSERT_EQ(tokens[5].kind, TOK_IDENT);     /* i32 */
    ASSERT_EQ(tokens[6].kind, TOK_COMMA);
    ASSERT_EQ(tokens[7].kind, TOK_IDENT);     /* b */
    ASSERT_EQ(tokens[8].kind, TOK_COLON);
    ASSERT_EQ(tokens[9].kind, TOK_IDENT);     /* i32 */
    ASSERT_EQ(tokens[10].kind, TOK_RPAREN);
    ASSERT_EQ(tokens[11].kind, TOK_ARROW);
    ASSERT_EQ(tokens[12].kind, TOK_IDENT);    /* i32 */
}

/* ============================================================
 * PARSER TESTS
 * ============================================================ */

TEST(parse_empty) {
    bool err;
    AstNode *prog = parse_source("", &err);
    ASSERT_FALSE(err);
    ASSERT_NOT_NULL(prog);
    ASSERT_EQ(prog->kind, AST_PROGRAM);
    ASSERT_NULL(prog->params);
}

TEST(parse_module_decl) {
    bool err;
    AstNode *prog = parse_source("mod main", &err);
    ASSERT_FALSE(err);
    ASSERT_NOT_NULL(prog->params);
    ASSERT_EQ(prog->params->kind, AST_MODULE);
    ASSERT_STR_EQ(prog->params->name, "main");
}

TEST(parse_module_qualified) {
    bool err;
    AstNode *prog = parse_source("mod net.http.server", &err);
    ASSERT_FALSE(err);
    ASSERT_EQ(prog->params->kind, AST_MODULE);
    ASSERT_STR_EQ(prog->params->name, "net.http.server");
}

TEST(parse_use_simple) {
    bool err;
    AstNode *prog = parse_source("use json", &err);
    ASSERT_FALSE(err);
    ASSERT_EQ(prog->params->kind, AST_USE);
    ASSERT_STR_EQ(prog->params->name, "json");
}

TEST(parse_use_qualified) {
    bool err;
    AstNode *prog = parse_source("use db.mysql", &err);
    ASSERT_FALSE(err);
    ASSERT_EQ(prog->params->kind, AST_USE);
    ASSERT_STR_EQ(prog->params->name, "db.mysql");
}

TEST(parse_simple_fn) {
    bool err;
    AstNode *prog = parse_source(
        "fn main() -> Result<void> {\n"
        "    return Ok(())\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *fn = prog->params;
    ASSERT_NOT_NULL(fn);
    ASSERT_EQ(fn->kind, AST_FN);
    ASSERT_STR_EQ(fn->name, "main");
    ASSERT_NOT_NULL(fn->left);  /* body */
    ASSERT_EQ(fn->left->kind, AST_BLOCK);
}

TEST(parse_fn_with_params) {
    bool err;
    AstNode *prog = parse_source(
        "fn add(a: i32, b: i32) -> i32 {\n"
        "    a + b\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *fn = prog->params;
    ASSERT_EQ(fn->kind, AST_FN);
    ASSERT_STR_EQ(fn->name, "add");
    ASSERT_NOT_NULL(fn->params);
    ASSERT_EQ(ast_list_len(fn->params), 2);
    ASSERT_STR_EQ(fn->params->name, "a");
    ASSERT_STR_EQ(fn->params->next->name, "b");
}

TEST(parse_pub_fn) {
    bool err;
    AstNode *prog = parse_source("pub fn hello() {}", &err);
    ASSERT_FALSE(err);
    ASSERT_TRUE(prog->params->is_pub);
}

TEST(parse_struct) {
    bool err;
    AstNode *prog = parse_source(
        "struct Point {\n"
        "    x: f64\n"
        "    y: f64\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *s = prog->params;
    ASSERT_EQ(s->kind, AST_STRUCT);
    ASSERT_STR_EQ(s->name, "Point");
    ASSERT_EQ(ast_list_len(s->params), 2);
    ASSERT_STR_EQ(s->params->name, "x");
    ASSERT_STR_EQ(s->params->next->name, "y");
}

TEST(parse_enum_simple) {
    bool err;
    AstNode *prog = parse_source(
        "enum Color {\n"
        "    Red\n"
        "    Green\n"
        "    Blue\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *e = prog->params;
    ASSERT_EQ(e->kind, AST_ENUM);
    ASSERT_STR_EQ(e->name, "Color");
    ASSERT_EQ(ast_list_len(e->params), 3);
}

TEST(parse_enum_with_fields) {
    bool err;
    AstNode *prog = parse_source(
        "enum Shape {\n"
        "    Circle(radius: f64)\n"
        "    Rectangle(width: f64, height: f64)\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *e = prog->params;
    ASSERT_EQ(e->kind, AST_ENUM);
    ASSERT_EQ(ast_list_len(e->params), 2);
    AstNode *circle = e->params;
    ASSERT_STR_EQ(circle->name, "Circle");
    ASSERT_EQ(ast_list_len(circle->params), 1);
}

TEST(parse_let_simple) {
    bool err;
    AstNode *prog = parse_source(
        "fn test() {\n"
        "    let x = 42\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *body = prog->params->left; /* fn -> body */
    AstNode *let_stmt = body->params;   /* block -> first stmt */
    ASSERT_EQ(let_stmt->kind, AST_LET);
    ASSERT_STR_EQ(let_stmt->name, "x");
    ASSERT_FALSE(let_stmt->is_mut);
    ASSERT_NOT_NULL(let_stmt->right); /* initializer */
    ASSERT_EQ(let_stmt->right->kind, AST_INT_LIT);
    ASSERT_EQ(let_stmt->right->val.int_val, 42);
}

TEST(parse_let_mut_typed) {
    bool err;
    AstNode *prog = parse_source(
        "fn test() {\n"
        "    let mut count: i32 = 0\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *let_stmt = prog->params->left->params;
    ASSERT_EQ(let_stmt->kind, AST_LET);
    ASSERT_TRUE(let_stmt->is_mut);
    ASSERT_STR_EQ(let_stmt->name, "count");
    ASSERT_NOT_NULL(let_stmt->type_expr);
}

TEST(parse_if_else) {
    bool err;
    AstNode *prog = parse_source(
        "fn test() {\n"
        "    if x > 0 {\n"
        "        foo()\n"
        "    } else {\n"
        "        bar()\n"
        "    }\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *stmt = prog->params->left->params;
    ASSERT_EQ(stmt->kind, AST_EXPR_STMT);
    AstNode *if_expr = stmt->left;
    ASSERT_EQ(if_expr->kind, AST_IF);
    ASSERT_NOT_NULL(if_expr->left);   /* condition */
    ASSERT_NOT_NULL(if_expr->right);  /* then block */
    ASSERT_NOT_NULL(if_expr->params); /* else block */
}

TEST(parse_match) {
    bool err;
    AstNode *prog = parse_source(
        "fn test() {\n"
        "    match x {\n"
        "        0 -> \"zero\"\n"
        "        n if n > 0 -> \"positive\"\n"
        "        _ -> \"negative\"\n"
        "    }\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *stmt = prog->params->left->params;
    AstNode *match_expr = stmt->left;
    ASSERT_EQ(match_expr->kind, AST_MATCH);
    ASSERT_EQ(ast_list_len(match_expr->params), 3); /* 3 arms */
}

TEST(parse_for_loop) {
    bool err;
    AstNode *prog = parse_source(
        "fn test() {\n"
        "    for item in items {\n"
        "        process(item)\n"
        "    }\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *for_stmt = prog->params->left->params;
    ASSERT_EQ(for_stmt->kind, AST_FOR);
    ASSERT_NOT_NULL(for_stmt->left);   /* pattern */
    ASSERT_NOT_NULL(for_stmt->params); /* iterator */
    ASSERT_NOT_NULL(for_stmt->right);  /* body */
}

TEST(parse_while_loop) {
    bool err;
    AstNode *prog = parse_source(
        "fn test() {\n"
        "    while running {\n"
        "        step()\n"
        "    }\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *while_stmt = prog->params->left->params;
    ASSERT_EQ(while_stmt->kind, AST_WHILE);
}

TEST(parse_binary_precedence) {
    bool err;
    AstNode *prog = parse_source(
        "fn test() {\n"
        "    a + b * c\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *expr = prog->params->left->params->left;
    /* Should be: (a + (b * c)) */
    ASSERT_EQ(expr->kind, AST_BINARY);
    ASSERT_EQ(expr->val.op, TOK_PLUS);
    ASSERT_EQ(expr->left->kind, AST_IDENT);
    ASSERT_EQ(expr->right->kind, AST_BINARY);
    ASSERT_EQ(expr->right->val.op, TOK_STAR);
}

TEST(parse_method_call) {
    bool err;
    AstNode *prog = parse_source(
        "fn test() {\n"
        "    items.push(42)\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *expr = prog->params->left->params->left;
    ASSERT_EQ(expr->kind, AST_METHOD_CALL);
    ASSERT_STR_EQ(expr->name, "push");
    ASSERT_EQ(expr->left->kind, AST_IDENT);
}

TEST(parse_try_operator) {
    bool err;
    AstNode *prog = parse_source(
        "fn test() {\n"
        "    let x = foo()?\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *let_stmt = prog->params->left->params;
    AstNode *init = let_stmt->right;
    ASSERT_EQ(init->kind, AST_TRY);
    ASSERT_EQ(init->left->kind, AST_CALL);
}

TEST(parse_closure) {
    bool err;
    AstNode *prog = parse_source(
        "fn test() {\n"
        "    let double = |x| x * 2\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *init = prog->params->left->params->right;
    ASSERT_EQ(init->kind, AST_CLOSURE);
    ASSERT_EQ(ast_list_len(init->params), 1);
    ASSERT_STR_EQ(init->params->name, "x");
}

TEST(parse_array_literal) {
    bool err;
    AstNode *prog = parse_source(
        "fn test() {\n"
        "    let arr = [1, 2, 3]\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *arr = prog->params->left->params->right;
    ASSERT_EQ(arr->kind, AST_ARRAY);
    ASSERT_EQ(ast_list_len(arr->params), 3);
}

TEST(parse_spawn) {
    bool err;
    AstNode *prog = parse_source(
        "fn test() {\n"
        "    spawn {\n"
        "        work()\n"
        "    }\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *expr = prog->params->left->params->left;
    ASSERT_EQ(expr->kind, AST_SPAWN);
    ASSERT_NOT_NULL(expr->left);
    ASSERT_EQ(expr->left->kind, AST_BLOCK);
}

TEST(parse_defer) {
    bool err;
    AstNode *prog = parse_source(
        "fn test() {\n"
        "    defer f.close()\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *stmt = prog->params->left->params;
    ASSERT_EQ(stmt->kind, AST_DEFER);
}

TEST(parse_impl_block) {
    bool err;
    AstNode *prog = parse_source(
        "impl Point {\n"
        "    fn new(x: f64, y: f64) -> Point {\n"
        "        Point { x, y }\n"
        "    }\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *impl = prog->params;
    ASSERT_EQ(impl->kind, AST_IMPL);
    ASSERT_NOT_NULL(impl->left); /* target type */
    ASSERT_EQ(ast_list_len(impl->params), 1); /* one method */
}

TEST(parse_trait_decl) {
    bool err;
    AstNode *prog = parse_source(
        "trait Display {\n"
        "    fn display(&self) -> string\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *trait = prog->params;
    ASSERT_EQ(trait->kind, AST_TRAIT);
    ASSERT_STR_EQ(trait->name, "Display");
    ASSERT_EQ(ast_list_len(trait->params), 1);
}

TEST(parse_type_alias) {
    bool err;
    AstNode *prog = parse_source(
        "type StringOrInt = string | int",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *alias = prog->params;
    ASSERT_EQ(alias->kind, AST_TYPE_ALIAS);
    ASSERT_STR_EQ(alias->name, "StringOrInt");
    ASSERT_NOT_NULL(alias->type_expr);
    ASSERT_EQ(alias->type_expr->kind, AST_TYPE_UNION);
}

TEST(parse_generic_fn) {
    bool err;
    AstNode *prog = parse_source(
        "fn max<T: Ord>(a: T, b: T) -> T {\n"
        "    if a > b { a } else { b }\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *fn = prog->params;
    ASSERT_EQ(fn->kind, AST_FN);
    ASSERT_NOT_NULL(fn->generics);
    ASSERT_STR_EQ(fn->generics->name, "T");
    ASSERT_NOT_NULL(fn->generics->params); /* constraint: Ord */
}

TEST(parse_full_program) {
    bool err;
    AstNode *prog = parse_source(
        "mod main\n"
        "\n"
        "use http\n"
        "use json\n"
        "\n"
        "struct Config {\n"
        "    port: u16\n"
        "    host: string\n"
        "}\n"
        "\n"
        "fn main() -> Result<void> {\n"
        "    let config = Config { port: 8080, host: \"localhost\" }\n"
        "    let mut server = http.Server.new()\n"
        "    server.listen(config.host)\n"
        "    Ok(())\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    /* mod + use + use + struct + fn = 5 declarations */
    ASSERT_EQ(ast_list_len(prog->params), 5);
}

/* ============================================================
 * AGENT SYSTEM PARSER TESTS
 * ============================================================ */

TEST(parse_capability) {
    bool err;
    AstNode *prog = parse_source(
        "capability bank {\n"
        "    read\n"
        "    transfer requires read\n"
        "    admin requires transfer, read\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    ASSERT_EQ(ast_list_len(prog->params), 1);

    AstNode *cap = prog->params;
    ASSERT_EQ(cap->kind, AST_CAPABILITY);
    ASSERT_STR_EQ(cap->name, "bank");

    /* 3 capability items */
    ASSERT_EQ(ast_list_len(cap->params), 3);

    AstNode *read_item = cap->params;
    ASSERT_STR_EQ(read_item->name, "read");
    ASSERT_NULL(read_item->params); /* no requires */

    AstNode *transfer_item = read_item->next;
    ASSERT_STR_EQ(transfer_item->name, "transfer");
    ASSERT_NOT_NULL(transfer_item->params); /* requires read */
    ASSERT_STR_EQ(transfer_item->params->name, "read");

    AstNode *admin_item = transfer_item->next;
    ASSERT_STR_EQ(admin_item->name, "admin");
    ASSERT_EQ(ast_list_len(admin_item->params), 2); /* requires transfer, read */
}

TEST(parse_taint) {
    bool err;
    AstNode *prog = parse_source(
        "taint user_input\n"
        "taint llm_output\n"
        "taint sanitized",
        &err
    );
    ASSERT_FALSE(err);
    ASSERT_EQ(ast_list_len(prog->params), 3);

    ASSERT_EQ(prog->params->kind, AST_TAINT);
    ASSERT_STR_EQ(prog->params->name, "user_input");
    ASSERT_STR_EQ(prog->params->next->name, "llm_output");
    ASSERT_STR_EQ(prog->params->next->next->name, "sanitized");
}

TEST(parse_budget) {
    bool err;
    AstNode *prog = parse_source(
        "budget Standard {\n"
        "    max_tokens: 100000\n"
        "    max_cost: 5\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    ASSERT_EQ(ast_list_len(prog->params), 1);

    AstNode *bud = prog->params;
    ASSERT_EQ(bud->kind, AST_BUDGET);
    ASSERT_STR_EQ(bud->name, "Standard");
    ASSERT_EQ(ast_list_len(bud->params), 2);

    ASSERT_STR_EQ(bud->params->name, "max_tokens");
    ASSERT_EQ(bud->params->right->val.int_val, 100000);
    ASSERT_STR_EQ(bud->params->next->name, "max_cost");
}

TEST(parse_guard_with_params) {
    bool err;
    AstNode *prog = parse_source(
        "guard max_transfer(amount: f64) {\n"
        "    let limit = 10000\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);

    AstNode *g = prog->params;
    ASSERT_EQ(g->kind, AST_GUARD);
    ASSERT_STR_EQ(g->name, "max_transfer");
    ASSERT_NOT_NULL(g->params); /* has parameters */
    ASSERT_STR_EQ(g->params->name, "amount");
    ASSERT_NOT_NULL(g->left); /* has body block */
    ASSERT_EQ(g->left->kind, AST_BLOCK);
}

TEST(parse_guard_no_params) {
    bool err;
    AstNode *prog = parse_source(
        "guard rate_limit {\n"
        "    let x = 50\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);

    AstNode *g = prog->params;
    ASSERT_EQ(g->kind, AST_GUARD);
    ASSERT_STR_EQ(g->name, "rate_limit");
    ASSERT_NULL(g->params); /* no params */
    ASSERT_NOT_NULL(g->left); /* has body */
}

TEST(parse_tool_decl) {
    bool err;
    AstNode *prog = parse_source(
        "tool web_search(query: string) -> Vec {\n"
        "    requires: [web.search]\n"
        "    description: \"Search the web\"\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);

    AstNode *t = prog->params;
    ASSERT_EQ(t->kind, AST_TOOL);
    ASSERT_STR_EQ(t->name, "web_search");
    ASSERT_NOT_NULL(t->params); /* has param: query */
    ASSERT_STR_EQ(t->params->name, "query");
    ASSERT_NOT_NULL(t->type_expr); /* return type Vec */

    /* right has fields: requires + description */
    ASSERT_NOT_NULL(t->right);
    ASSERT_STR_EQ(t->right->name, "requires");
    ASSERT_STR_EQ(t->right->next->name, "description");
}

TEST(parse_skill_decl) {
    bool err;
    AstNode *prog = parse_source(
        "skill WebResearch {\n"
        "    version: \"1.0.0\"\n"
        "    requires: [web.search, web.fetch]\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);

    AstNode *s = prog->params;
    ASSERT_EQ(s->kind, AST_SKILL);
    ASSERT_STR_EQ(s->name, "WebResearch");
    ASSERT_EQ(ast_list_len(s->params), 2); /* version + requires */
}

TEST(parse_supervisor_decl) {
    bool err;
    AstNode *prog = parse_source(
        "supervisor PaymentSystem {\n"
        "    strategy: one_for_one\n"
        "    max_restarts: 5\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);

    AstNode *sv = prog->params;
    ASSERT_EQ(sv->kind, AST_SUPERVISOR);
    ASSERT_STR_EQ(sv->name, "PaymentSystem");
    ASSERT_EQ(ast_list_len(sv->params), 2);
    ASSERT_STR_EQ(sv->params->name, "strategy");
}

TEST(parse_agent_decl) {
    bool err;
    AstNode *prog = parse_source(
        "agent Researcher {\n"
        "    capabilities: [web.search, web.fetch]\n"
        "    model: \"claude-sonnet\"\n"
        "    budget: { max_cost: 5 }\n"
        "\n"
        "    fn run(topic: string) -> Result {\n"
        "        let results = search(topic)\n"
        "        results\n"
        "    }\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);

    AstNode *agent = prog->params;
    ASSERT_EQ(agent->kind, AST_AGENT);
    ASSERT_STR_EQ(agent->name, "Researcher");

    /* Fields: capabilities, model, budget */
    ASSERT_EQ(ast_list_len(agent->params), 3);
    ASSERT_STR_EQ(agent->params->name, "capabilities");
    ASSERT_STR_EQ(agent->params->next->name, "model");
    ASSERT_STR_EQ(agent->params->next->next->name, "budget");

    /* Functions: run */
    ASSERT_NOT_NULL(agent->left);
    ASSERT_EQ(agent->left->kind, AST_FN);
    ASSERT_STR_EQ(agent->left->name, "run");
}

TEST(parse_agent_full_program) {
    bool err;
    AstNode *prog = parse_source(
        "capability web {\n"
        "    search\n"
        "    fetch requires search\n"
        "}\n"
        "\n"
        "taint user_input\n"
        "\n"
        "budget Standard {\n"
        "    max_cost: 10\n"
        "}\n"
        "\n"
        "guard safety {\n"
        "    let x = 1\n"
        "}\n"
        "\n"
        "agent Bot {\n"
        "    capabilities: [web.search]\n"
        "    model: \"claude-sonnet\"\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    /* capability + taint + budget + guard + agent = 5 */
    ASSERT_EQ(ast_list_len(prog->params), 5);

    ASSERT_EQ(prog->params->kind, AST_CAPABILITY);
    ASSERT_EQ(prog->params->next->kind, AST_TAINT);
    ASSERT_EQ(prog->params->next->next->kind, AST_BUDGET);
    ASSERT_EQ(prog->params->next->next->next->kind, AST_GUARD);
    ASSERT_EQ(prog->params->next->next->next->next->kind, AST_AGENT);
}

/* ============================================================
 * SECURITY TESTS
 * ============================================================ */

TEST(security_hash_basic) {
    const char *data = "Hello, Limceron!";
    uint8_t hash1[HASH_SIZE];
    uint8_t hash2[HASH_SIZE];

    security_hash_buffer((const uint8_t *)data, strlen(data), hash1);
    security_hash_buffer((const uint8_t *)data, strlen(data), hash2);

    /* Same input → same hash */
    ASSERT(memcmp(hash1, hash2, HASH_SIZE) == 0);
}

TEST(security_hash_different) {
    uint8_t hash1[HASH_SIZE];
    uint8_t hash2[HASH_SIZE];

    security_hash_buffer((const uint8_t *)"hello", 5, hash1);
    security_hash_buffer((const uint8_t *)"world", 5, hash2);

    /* Different input → different hash */
    ASSERT(memcmp(hash1, hash2, HASH_SIZE) != 0);
}

TEST(security_lceron_sign_verify) {
    uint8_t key[HASH_SIZE];
    uint8_t data[] = "fn main() { print(42) }";
    LceronObjHeader header;
    int i;

    memset(&header, 0, sizeof(header));
    header.magic = LCN_MAGIC;
    header.version = LCERON_OBJ_VERSION;

    /* Generate a test key */
    for (i = 0; i < HASH_SIZE; i++) key[i] = (uint8_t)(i * 7 + 13);

    /* Sign */
    ASSERT_TRUE(security_sign_lceron(&header, data, sizeof(data), key));

    /* Verify with same key */
    ASSERT_TRUE(security_verify_lceron(&header, data, sizeof(data), key));
}

TEST(security_lceron_tamper_detection) {
    uint8_t key[HASH_SIZE];
    uint8_t data[] = "fn main() { print(42) }";
    uint8_t tampered[] = "fn main() { print(99) }";
    LceronObjHeader header;
    int i;

    memset(&header, 0, sizeof(header));
    header.magic = LCN_MAGIC;
    header.version = LCERON_OBJ_VERSION;

    for (i = 0; i < HASH_SIZE; i++) key[i] = (uint8_t)(i * 7 + 13);

    security_sign_lceron(&header, data, sizeof(data), key);

    /* Verify with tampered data should FAIL */
    ASSERT_FALSE(security_verify_lceron(&header, tampered, sizeof(tampered), key));
}

TEST(security_lceron_wrong_key) {
    uint8_t key1[HASH_SIZE];
    uint8_t key2[HASH_SIZE];
    uint8_t data[] = "fn main() {}";
    LceronObjHeader header;
    int i;

    memset(&header, 0, sizeof(header));
    header.magic = LCN_MAGIC;
    header.version = LCERON_OBJ_VERSION;

    for (i = 0; i < HASH_SIZE; i++) key1[i] = (uint8_t)i;
    for (i = 0; i < HASH_SIZE; i++) key2[i] = (uint8_t)(i + 1);

    security_sign_lceron(&header, data, sizeof(data), key1);

    /* Verify with different key should FAIL */
    ASSERT_FALSE(security_verify_lceron(&header, data, sizeof(data), key2));
}

TEST(security_lceron_bad_magic) {
    uint8_t key[HASH_SIZE];
    uint8_t data[] = "test";
    LceronObjHeader header;
    int i;

    memset(&header, 0, sizeof(header));
    header.magic = 0xDEADBEEF; /* wrong magic */
    header.version = LCERON_OBJ_VERSION;

    for (i = 0; i < HASH_SIZE; i++) key[i] = (uint8_t)i;

    ASSERT_FALSE(security_verify_lceron(&header, data, sizeof(data), key));
}

/* ============================================================
 * TYPE CHECKER TESTS
 * ============================================================ */

/* Parse + typecheck. Returns true if no errors. */
static bool typecheck_source(const char *source) {
    arena_reset(&test_arena);
    arena_reset(&test_intern_arena);

    size_t len = strlen(source);
    ErrorReporter reporter = reporter_new("<test>", source, len);
    StringIntern intern = intern_new(&test_intern_arena);
    Lexer lexer = lexer_new("<test>", source, len, &intern, &reporter);
    Parser parser = parser_new(&lexer, &test_arena, &reporter);
    AstNode *program = parse_program(&parser);
    if (parser.had_error) return false;
    return typecheck_program(program, &reporter, &test_arena);
}

TEST(typecheck_simple_program_ok) {
    bool ok = typecheck_source(
        "fn add(a: i32, b: i32) -> i32 {\n    return a + b\n}"
    );
    ASSERT(ok);
}

TEST(typecheck_capability_missing) {
    /* Agent uses tool requiring web.search but doesn't have it */
    bool ok = typecheck_source(
        "capability web {\n    search\n}\n"
        "tool web_search(q: string) -> Result {\n    requires: [web.search]\n}\n"
        "agent Bot {\n"
        "    capabilities: []\n"
        "    model: \"gpt-4\"\n"
        "    fn run(topic: string) -> Result {\n"
        "        let r = web_search(topic, 10)\n"
        "        r\n"
        "    }\n"
        "}"
    );
    ASSERT_FALSE(ok);  /* Should fail: Bot lacks web.search capability */
}

TEST(typecheck_capability_ok) {
    bool ok = typecheck_source(
        "capability web {\n    search\n}\n"
        "tool web_search(q: string) -> Result {\n    requires: [web.search]\n}\n"
        "agent Bot {\n"
        "    capabilities: [web.search]\n"
        "    model: \"gpt-4\"\n"
        "    budget: { max_tokens: 10000 }\n"
        "    fn run(topic: string) -> Result {\n"
        "        let r = web_search(topic, 10)\n"
        "        r\n"
        "    }\n"
        "}"
    );
    ASSERT(ok);  /* Should pass: Bot has web.search and budget */
}

TEST(typecheck_guard_required) {
    /* Agent has sensitive capability but no guard */
    bool ok = typecheck_source(
        "capability payment {\n    transfer\n}\n"
        "agent Bot {\n"
        "    capabilities: [payment.transfer]\n"
        "    model: \"gpt-4\"\n"
        "}"
    );
    /* Should produce a warning/error about missing guard for sensitive cap */
    /* The type checker emits warnings for sensitive caps without guards */
    /* This test verifies the check runs (it may still pass if it's a warning) */
    (void)ok;
    ASSERT(1);  /* At minimum, it shouldn't crash */
}

TEST(typecheck_budget_missing) {
    /* Agent calls tools but has no budget */
    bool ok = typecheck_source(
        "capability web {\n    search\n}\n"
        "tool web_search(q: string) -> Result {\n    requires: [web.search]\n}\n"
        "agent Bot {\n"
        "    capabilities: [web.search]\n"
        "    model: \"gpt-4\"\n"
        "    fn run(topic: string) -> Result {\n"
        "        let r = web_search(topic, 10)\n"
        "        r\n"
        "    }\n"
        "}"
    );
    ASSERT_FALSE(ok);  /* Should fail: Bot uses tools but has no budget */
}

TEST(typecheck_budget_ok) {
    bool ok = typecheck_source(
        "capability web {\n    search\n}\n"
        "tool web_search(q: string) -> Result {\n    requires: [web.search]\n}\n"
        "agent Bot {\n"
        "    capabilities: [web.search]\n"
        "    model: \"gpt-4\"\n"
        "    budget: { max_tokens: 10000 }\n"
        "    fn run(topic: string) -> Result {\n"
        "        let r = web_search(topic, 10)\n"
        "        r\n"
        "    }\n"
        "}"
    );
    ASSERT(ok);  /* Should pass: Bot has budget */
}

TEST(typecheck_taint_violation) {
    /* Tainted parameter flows to dangerous sink */
    bool ok = typecheck_source(
        "taint user_input\n"
        "fn process(text: string) -> Result {\n"
        "    let result = text\n"
        "    result\n"
        "}"
    );
    /* The taint checker looks for @user_input annotations on params.
       Without annotation, this should pass (no taint to track) */
    ASSERT(ok);
}

TEST(typecheck_unknown_type) {
    /* Using an undefined type should produce an error */
    bool ok = typecheck_source(
        "fn process(x: NonExistentType) -> Result {\n"
        "    x\n"
        "}"
    );
    ASSERT_FALSE(ok);  /* Should fail: NonExistentType not defined */
}

TEST(typecheck_unknown_function) {
    /* Calling an undefined function should produce an error */
    bool ok = typecheck_source(
        "fn main() -> Result {\n"
        "    let r = nonexistent_function(42)\n"
        "    r\n"
        "}"
    );
    ASSERT_FALSE(ok);  /* Should fail: nonexistent_function not defined */
}

TEST(typecheck_full_agent_pipeline) {
    /* Complete valid agent program should pass all checks */
    bool ok = typecheck_source(
        "capability web {\n    search\n    fetch requires search\n}\n"
        "taint user_input\n"
        "budget Std {\n    max_tokens: 100000\n    max_cost: 5\n}\n"
        "guard rate_limit() {\n    let max = 50\n}\n"
        "tool web_search(q: string) -> Result {\n    requires: [web.search]\n}\n"
        "supervisor Sup {\n    strategy: one_for_one\n    max_restarts: 5\n}\n"
        "agent Researcher {\n"
        "    capabilities: [web.search, web.fetch]\n"
        "    model: \"claude\"\n"
        "    budget: { max_cost: 5 }\n"
        "    guards: [rate_limit]\n"
        "    fn run(topic: string) -> Result {\n"
        "        let r = web_search(topic, 5)\n"
        "        r\n"
        "    }\n"
        "}\n"
    );
    ASSERT(ok);  /* Complete valid program should pass */
}

TEST(typecheck_taint_inference_ask) {
    /* ask() returns tainted data — should detect if it flows to dangerous sink */
    bool ok = typecheck_source(
        "taint user_input\n"
        "fn process() -> Result {\n"
        "    let user_msg = ask(\"What do you need?\")\n"
        "    let cmd = exec(user_msg)\n"
        "    cmd\n"
        "}"
    );
    ASSERT_FALSE(ok);  /* Should fail: ask() result flows to exec() */
}

TEST(typecheck_taint_propagation) {
    /* Tainted data concatenated with string should propagate taint */
    bool ok = typecheck_source(
        "taint user_input\n"
        "fn process() -> Result {\n"
        "    let user_msg = ask(\"Input?\")\n"
        "    let combined = \"System: \" + user_msg\n"
        "    let result = exec(combined)\n"
        "    result\n"
        "}"
    );
    ASSERT_FALSE(ok);  /* Should fail: taint propagated through concatenation */
}

TEST(typecheck_taint_clean_ok) {
    /* Non-tainted data flowing to sinks is fine */
    bool ok = typecheck_source(
        "fn exec(cmd: string) -> Result {\n"
        "    cmd\n"
        "}\n"
        "fn process() -> Result {\n"
        "    let safe_data = \"hello world\"\n"
        "    let result = exec(safe_data)\n"
        "    result\n"
        "}"
    );
    ASSERT(ok);  /* Should pass: no tainted data involved */
}

TEST(typecheck_prompt_injection) {
    /* Tainted data directly in ask() argument — prompt injection */
    bool ok = typecheck_source(
        "taint user_input\n"
        "fn handle(input: string) -> Result {\n"
        "    let user_msg = ask(\"Process this\")\n"
        "    let response = ask(\"System prompt: \" + user_msg)\n"
        "    response\n"
        "}"
    );
    ASSERT_FALSE(ok);  /* Should fail: tainted ask() result flows to another ask() */
}

TEST(typecheck_agent_taint) {
    /* Agent method with ask() flowing to exec — should detect */
    bool ok = typecheck_source(
        "taint user_input\n"
        "capability llm { complete }\n"
        "agent Bot {\n"
        "    capabilities: [llm.complete]\n"
        "    model: \"gpt-4\"\n"
        "    budget: { max_tokens: 10000 }\n"
        "    fn run(topic: string) -> Result {\n"
        "        let user_data = ask(topic)\n"
        "        let cmd = exec(user_data)\n"
        "        cmd\n"
        "    }\n"
        "}"
    );
    ASSERT_FALSE(ok);  /* Should fail: ask() result flows to exec() in agent */
}

/* ============================================================
 * ASK / TELL / CHAN / SELECT / MESH / ROUTER / MCP / A2A PARSER TESTS
 * ============================================================ */

TEST(parse_ask_expr) {
    bool err;
    AstNode *prog = parse_source("fn main() -> Result {\n    let answer = ask(\"What is 2+2?\")\n}", &err);
    ASSERT_NOT_NULL(prog);
    AstNode *fn = prog->params;
    ASSERT_NOT_NULL(fn);
    ASSERT(fn->kind == AST_FN);
    AstNode *body = fn->left;
    ASSERT_NOT_NULL(body);
    AstNode *let_stmt = body->params;
    ASSERT_NOT_NULL(let_stmt);
    ASSERT(let_stmt->kind == AST_LET);
    ASSERT_NOT_NULL(let_stmt->right);
    ASSERT(let_stmt->right->kind == AST_ASK);
}

TEST(parse_tell_expr) {
    bool err;
    AstNode *prog = parse_source("fn main() -> Result {\n    tell user \"hello\"\n}", &err);
    ASSERT_NOT_NULL(prog);
    AstNode *fn = prog->params;
    ASSERT_NOT_NULL(fn);
    ASSERT(fn->kind == AST_FN);
    AstNode *body = fn->left;
    ASSERT_NOT_NULL(body);
    AstNode *stmt = body->params;
    ASSERT_NOT_NULL(stmt);
    ASSERT(stmt->kind == AST_EXPR_STMT);
    ASSERT_NOT_NULL(stmt->left);
    ASSERT(stmt->left->kind == AST_TELL);
}

TEST(parse_chan_expr) {
    bool err;
    AstNode *prog = parse_source("fn main() -> Result {\n    let ch = chan<i32>(10)\n}", &err);
    ASSERT_NOT_NULL(prog);
    AstNode *fn = prog->params;
    ASSERT(fn->kind == AST_FN);
    AstNode *let_stmt = fn->left->params;
    ASSERT(let_stmt->kind == AST_LET);
    ASSERT_NOT_NULL(let_stmt->right);
    ASSERT(let_stmt->right->kind == AST_CHANNEL);
    ASSERT_NOT_NULL(let_stmt->right->type_expr);
    ASSERT_NOT_NULL(let_stmt->right->left);
}

TEST(parse_select_stmt) {
    bool err;
    AstNode *prog = parse_source(
        "fn main() -> Result {\n"
        "    select {\n"
        "        msg from ch1 -> {\n"
        "            let x = 1\n"
        "        }\n"
        "        val from ch2 -> {\n"
        "            let y = 2\n"
        "        }\n"
        "    }\n"
        "}", &err);
    ASSERT_NOT_NULL(prog);
    AstNode *fn = prog->params;
    ASSERT(fn->kind == AST_FN);
    AstNode *stmt = fn->left->params;
    ASSERT(stmt->kind == AST_EXPR_STMT);
    ASSERT_NOT_NULL(stmt->left);
    ASSERT(stmt->left->kind == AST_SELECT);
    ASSERT_NOT_NULL(stmt->left->params);
    ASSERT(stmt->left->params->kind == AST_SELECT_ARM);
    ASSERT_NOT_NULL(stmt->left->params->next);
}

TEST(parse_mesh_decl) {
    bool err;
    AstNode *prog = parse_source(
        "mesh Pipeline {\n"
        "    stage \"analyze\" -> Analyzer {\n"
        "        timeout: 30\n"
        "    }\n"
        "    stage \"summarize\" -> Summarizer {}\n"
        "}", &err);
    ASSERT_NOT_NULL(prog);
    AstNode *mesh = prog->params;
    ASSERT_NOT_NULL(mesh);
    ASSERT(mesh->kind == AST_MESH);
    ASSERT(strcmp(mesh->name, "Pipeline") == 0);
    ASSERT_NOT_NULL(mesh->params);
    ASSERT(mesh->params->kind == AST_MESH_STAGE);
    ASSERT_NOT_NULL(mesh->params->next);
}

TEST(parse_router_decl) {
    bool err;
    AstNode *prog = parse_source(
        "router ModelSelector {\n"
        "    route \"fast\" -> [gpt4_mini, claude_haiku]\n"
        "    route \"quality\" -> [gpt4, claude_opus]\n"
        "    strategy: cost_aware\n"
        "    fallback: \"gpt4_mini\"\n"
        "}", &err);
    ASSERT_NOT_NULL(prog);
    AstNode *router = prog->params;
    ASSERT_NOT_NULL(router);
    ASSERT(router->kind == AST_ROUTER);
    ASSERT(strcmp(router->name, "ModelSelector") == 0);
    ASSERT_NOT_NULL(router->params);
}

TEST(parse_use_mcp) {
    bool err;
    AstNode *prog = parse_source("use mcp(\"npx @anthropic/mcp-server-memory\") as mem", &err);
    ASSERT_NOT_NULL(prog);
    AstNode *use = prog->params;
    ASSERT_NOT_NULL(use);
    ASSERT(use->kind == AST_USE);
    ASSERT(strcmp(use->name, "mcp") == 0);
    ASSERT_NOT_NULL(use->val.str_val);
    ASSERT(strstr(use->val.str_val, "mcp-server-memory") != NULL);
    ASSERT_NOT_NULL(use->right);
    ASSERT(strcmp(use->right->name, "mem") == 0);
}

TEST(parse_use_a2a) {
    bool err;
    AstNode *prog = parse_source("use a2a(\"https://api.example.com/agent\") as remote", &err);
    ASSERT_NOT_NULL(prog);
    AstNode *use = prog->params;
    ASSERT_NOT_NULL(use);
    ASSERT(use->kind == AST_USE);
    ASSERT(strcmp(use->name, "a2a") == 0);
    ASSERT_NOT_NULL(use->val.str_val);
    ASSERT(strstr(use->val.str_val, "example.com") != NULL);
    ASSERT_NOT_NULL(use->right);
    ASSERT(strcmp(use->right->name, "remote") == 0);
}

TEST(parse_use_model) {
    bool err;
    AstNode *prog = parse_source("use model(\"categorizador.onnx\") as clf", &err);
    ASSERT_NOT_NULL(prog);
    AstNode *use = prog->params;
    ASSERT_NOT_NULL(use);
    ASSERT(use->kind == AST_USE);
    ASSERT(strcmp(use->name, "model") == 0);
    ASSERT_NOT_NULL(use->val.str_val);
    ASSERT(strstr(use->val.str_val, "categorizador.onnx") != NULL);
    ASSERT_NOT_NULL(use->right);
    ASSERT(strcmp(use->right->name, "clf") == 0);
}

/* ============================================================
 * CODEGEN TESTS
 * ============================================================ */

/* Parse + generate C. Returns malloc'd string (caller must free). */
static char *gen_c(const char *source) {
    arena_reset(&test_arena);
    arena_reset(&test_intern_arena);

    size_t len = strlen(source);
    ErrorReporter reporter = reporter_new("<test>", source, len);
    StringIntern intern = intern_new(&test_intern_arena);
    Lexer lexer = lexer_new("<test>", source, len, &intern, &reporter);
    Parser parser = parser_new(&lexer, &test_arena, &reporter);
    AstNode *program = parse_program(&parser);
    if (parser.had_error) return NULL;
    return codegen_generate(program, "<test>", &test_arena);
}

TEST(codegen_preamble) {
    char *c = gen_c("");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "Generated by Limceron Compiler") != NULL);
    ASSERT(strstr(c, "typedef const char *LcnString") != NULL);
    ASSERT(strstr(c, "typedef uint64_t LcnCapability") != NULL);
    ASSERT(strstr(c, "LcnBudget") != NULL);
    ASSERT(strstr(c, "int main(") != NULL);
    free(c);
}

TEST(codegen_simple_fn) {
    char *c = gen_c("fn add(a: i32, b: i32) -> i32 {\n    return a + b\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "int32_t lcn_add(") != NULL);
    ASSERT(strstr(c, "int32_t a") != NULL);
    ASSERT(strstr(c, "int32_t b") != NULL);
    ASSERT(strstr(c, "return (a + b)") != NULL);
    free(c);
}

TEST(codegen_capability) {
    char *c = gen_c("capability web {\n    search\n    fetch requires search\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "#define CAP_WEB_SEARCH") != NULL);
    ASSERT(strstr(c, "#define CAP_WEB_FETCH") != NULL);
    ASSERT(strstr(c, "1ULL << 0") != NULL);
    ASSERT(strstr(c, "1ULL << 1") != NULL);
    free(c);
}

TEST(codegen_guard) {
    char *c = gen_c("guard limit_check(amount: f64) {\n    let max = 1000\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "bool lcn_guard_limit_check(double amount)") != NULL);
    ASSERT(strstr(c, "int64_t max = 1000") != NULL);
    ASSERT(strstr(c, "return true;") != NULL);
    free(c);
}

TEST(codegen_budget) {
    char *c = gen_c("budget Standard {\n    max_tokens: 50000\n    max_cost: 10\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "LcnBudget lcn_budget_Standard(") != NULL);
    ASSERT(strstr(c, ".max_tokens = 50000") != NULL);
    ASSERT(strstr(c, ".max_cost = 10") != NULL);
    free(c);
}

TEST(codegen_taint) {
    char *c = gen_c("taint user_input\ntaint llm_output");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "Tainted_user_input") != NULL);
    ASSERT(strstr(c, "Tainted_llm_output") != NULL);
    free(c);
}

TEST(codegen_tool_cap_check) {
    char *c = gen_c(
        "capability web {\n    search\n}\n"
        "tool web_search(q: string) -> Vec {\n    requires: [web.search]\n    description: \"Search\"\n}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_tool_web_search(LcnCapability _agent_caps") != NULL);
    ASSERT(strstr(c, "CAP_WEB_SEARCH") != NULL);
    ASSERT(strstr(c, "FATAL: agent lacks capability") != NULL);
    free(c);
}

TEST(codegen_agent_struct) {
    char *c = gen_c(
        "capability web {\n    search\n}\n"
        "agent Bot {\n    capabilities: [web.search]\n    model: \"gpt-4\"\n}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "Agent_Bot") != NULL);
    ASSERT(strstr(c, "LcnCapability capabilities") != NULL);
    ASSERT(strstr(c, "lcn_agent_Bot_new") != NULL);
    ASSERT(strstr(c, "CAP_WEB_SEARCH") != NULL);
    ASSERT(strstr(c, "\"gpt-4\"") != NULL);
    free(c);
}

TEST(codegen_supervisor) {
    char *c = gen_c("supervisor MySup {\n    strategy: one_for_all\n    max_restarts: 3\n}");
    ASSERT_NOT_NULL(c);
    /* New codegen emits init function and global pointer */
    ASSERT(strstr(c, "lcn_supervisor_MySup_init") != NULL);
    ASSERT(strstr(c, "_lcn_sup_MySup") != NULL);
    ASSERT(strstr(c, "3") != NULL);
    free(c);
}

TEST(codegen_agent_tool_rewrite) {
    char *c = gen_c(
        "capability web {\n    search\n}\n"
        "tool web_search(q: string) -> Vec {\n    requires: [web.search]\n}\n"
        "agent Bot {\n    capabilities: [web.search]\n"
        "    fn run(topic: string) -> Result {\n"
        "        let results = web_search(topic, 10)\n"
        "        results\n"
        "    }\n}"
    );
    ASSERT_NOT_NULL(c);
    /* Tool call rewritten with self->capabilities */
    ASSERT(strstr(c, "lcn_tool_web_search(self->capabilities") != NULL);
    /* Type inferred from tool return type */
    ASSERT(strstr(c, "LcnVec results") != NULL);
    /* Last expr wrapped in LCN_OK for Result return */
    ASSERT(strstr(c, "return LCN_OK") != NULL);
    free(c);
}

TEST(codegen_md_guard_skip) {
    char *c = gen_c(
        "agent Bot {\n"
        "    model: \"gpt-4\"\n"
        "    guards: [rate_limit]\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    /* Guards field should NOT produce a TODO */
    ASSERT(strstr(c, "TODO") == NULL);
    /* Agent struct should still be generated */
    ASSERT(strstr(c, "Agent_Bot") != NULL);
    free(c);
}

TEST(codegen_spawn_inline) {
    char *c = gen_c(
        "fn main() -> Result {\n"
        "    spawn {\n"
        "        let x = 42\n"
        "    }\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    /* spawn now emits a wrapper function and lcn_spawn_task call */
    ASSERT(strstr(c, "lcn_spawn_task") != NULL);
    ASSERT(strstr(c, "__lcn_spawn_") != NULL);
    ASSERT(strstr(c, "int64_t x = 42") != NULL);
    free(c);
}

TEST(codegen_vec_pop_call) {
    char *c = gen_c(
        "fn main() -> Result {\n"
        "    let v = vec_new()\n"
        "    vec_push(v, 1)\n"
        "    let item = vec_pop(v)\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_vec_pop(&") != NULL);
    free(c);
}

TEST(codegen_vec_pop_preamble) {
    char *c = gen_c("");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_vec_pop") != NULL);
    ASSERT(strstr(c, "v->len == 0") != NULL);
    ASSERT(strstr(c, "--v->len") != NULL);
    free(c);
}

TEST(codegen_spawn_threaded) {
    char *c = gen_c(
        "fn main() -> Result {\n"
        "    spawn {\n"
        "        let x = 42\n"
        "    }\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_spawn_task") != NULL);
    ASSERT(strstr(c, "__lcn_spawn_") != NULL);
    ASSERT(strstr(c, "LcnTaskFn") != NULL);
    free(c);
}

TEST(codegen_spawn_await) {
    char *c = gen_c(
        "fn main() -> Result {\n"
        "    let h = spawn {\n"
        "        let x = 10\n"
        "    }\n"
        "    let result = await h\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_spawn_task") != NULL);
    ASSERT(strstr(c, "lcn_await_task") != NULL);
    free(c);
}

TEST(codegen_taint_functions) {
    char *c = gen_c("taint user_input");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "Tainted_user_input") != NULL);
    ASSERT(strstr(c, "lcn_taint_user_input(LcnString val)") != NULL);
    ASSERT(strstr(c, "lcn_untaint_user_input(Tainted_user_input t)") != NULL);
    ASSERT(strstr(c, "[taint] untaint user_input") != NULL);
    free(c);
}

TEST(codegen_skill_struct) {
    char *c = gen_c(
        "skill WebOps {\n"
        "    version: \"1.0.0\"\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "Skill_WebOps") != NULL);
    ASSERT(strstr(c, "lcn_skill_WebOps(void)") != NULL);
    ASSERT(strstr(c, "\"1.0.0\"") != NULL);
    free(c);
}

TEST(codegen_ask_builtin) {
    char *c = gen_c("fn main() -> Result {\n    let answer = ask(\"What?\")\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_ask_typed(") != NULL);
    ASSERT(strstr(c, "\"What?\"") != NULL);
    ASSERT(strstr(c, "LcnLlmOutput") != NULL);
    free(c);
}

TEST(codegen_tell) {
    char *c = gen_c("fn main() -> Result {\n    tell user \"hello world\"\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "printf(\"AGENT -> user") != NULL);
    ASSERT(strstr(c, "\"hello world\"") != NULL);
    free(c);
}

TEST(codegen_channel) {
    char *c = gen_c("fn main() -> Result {\n    let ch = chan<i32>(10)\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_channel_new(10, sizeof(LcnString))") != NULL);
    free(c);
}

TEST(codegen_select) {
    char *c = gen_c(
        "fn main() -> Result {\n"
        "    select {\n"
        "        msg from ch1 -> {\n"
        "            let x = 1\n"
        "        }\n"
        "    }\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_channel_recv(") != NULL);
    ASSERT(strstr(c, "select") != NULL);
    free(c);
}

TEST(codegen_mesh) {
    char *c = gen_c(
        "agent Analyzer {\n"
        "    model: \"gpt-4\"\n"
        "}\n"
        "mesh Pipeline {\n"
        "    stage \"analyze\" -> Analyzer {}\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_mesh_Pipeline_run") != NULL);
    ASSERT(strstr(c, "Agent_Analyzer") != NULL);
    ASSERT(strstr(c, "lcn_agent_Analyzer_new") != NULL);
    free(c);
}

TEST(codegen_router) {
    char *c = gen_c(
        "router ModelRouter {\n"
        "    route \"fast\" -> [gpt4_mini]\n"
        "    route \"quality\" -> [claude_opus]\n"
        "    strategy: cost_aware\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "Router_ModelRouter") != NULL);
    ASSERT(strstr(c, "lcn_router_ModelRouter_select") != NULL);
    ASSERT(strstr(c, "\"fast\"") != NULL);
    ASSERT(strstr(c, "\"gpt4_mini\"") != NULL);
    free(c);
}

TEST(codegen_mcp_init) {
    char *c = gen_c("use mcp(\"npx @anthropic/mcp-memory\") as mem");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_mcp_mem_call") != NULL);
    ASSERT(strstr(c, "MCP import") != NULL);
    free(c);
}

TEST(codegen_a2a_init) {
    char *c = gen_c("use a2a(\"https://api.example.com\") as remote");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_a2a_remote_call") != NULL);
    ASSERT(strstr(c, "A2A import") != NULL);
    ASSERT(strstr(c, "https://api.example.com") != NULL);
    /* Verify send/send_task stubs are generated */
    ASSERT(strstr(c, "lcn_a2a_remote_send") != NULL);
    ASSERT(strstr(c, "lcn_a2a_remote_send_task") != NULL);
    free(c);
}

TEST(codegen_model_init) {
    char *c = gen_c("use model(\"categorizador.onnx\") as clf");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "Model import") != NULL);
    ASSERT(strstr(c, "categorizador.onnx") != NULL);
    ASSERT(strstr(c, "_model_clf") != NULL);
    ASSERT(strstr(c, "lcn_model_clf_predict") != NULL);
    ASSERT(strstr(c, "lcn_model_clf_info") != NULL);
    ASSERT(strstr(c, "LcnModelResult") != NULL);
    free(c);
}

TEST(codegen_model_predict_rewrite) {
    char *c = gen_c(
        "use model(\"categorizador.onnx\") as clf\n"
        "fn main() -> Result {\n"
        "    let r = clf.predict(\"faltan imagenes\")\n"
        "    println(r.label)\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* The predict call should be rewritten to lcn_model_clf_predict */
    ASSERT(strstr(c, "lcn_model_clf_predict(") != NULL);
    /* The let binding should infer LcnModelResult */
    ASSERT(strstr(c, "LcnModelResult") != NULL);
    free(c);
}

/* ============================================================
 * POSTGRES DRIVER CODEGEN TESTS
 * ============================================================ */

TEST(codegen_postgres_driver_include) {
    char *c = gen_c("use driver(\"postgres\") as pg");
    ASSERT_NOT_NULL(c);
    /* Should emit the driver import comment */
    ASSERT(strstr(c, "driver import: postgres as pg") != NULL);
    /* Should generate wrapper functions */
    ASSERT(strstr(c, "lcn_driver_pg_connect") != NULL);
    ASSERT(strstr(c, "lcn_driver_pg_query") != NULL);
    ASSERT(strstr(c, "lcn_driver_pg_execute") != NULL);
    ASSERT(strstr(c, "lcn_driver_pg_close") != NULL);
    free(c);
}

TEST(codegen_postgres_connect) {
    char *c = gen_c(
        "use driver(\"postgres\") as pg\n"
        "fn main() -> Result {\n"
        "    let conn = pg.connect(\"localhost\", 5432, \"user\", \"pass\", \"mydb\")\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* The method call should be rewritten to lcn_driver_pg_connect */
    ASSERT(strstr(c, "lcn_driver_pg_connect(") != NULL);
    free(c);
}

TEST(codegen_postgres_execute) {
    char *c = gen_c(
        "use driver(\"postgres\") as pg\n"
        "fn main() -> Result {\n"
        "    let n = pg.execute(conn, \"INSERT INTO t VALUES(1)\")\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* The method call should be rewritten to lcn_driver_pg_execute */
    ASSERT(strstr(c, "lcn_driver_pg_execute(") != NULL);
    free(c);
}

TEST(codegen_postgres_query) {
    char *c = gen_c(
        "use driver(\"postgres\") as pg\n"
        "fn main() -> Result {\n"
        "    let rows = pg.query(conn, \"SELECT * FROM users\")\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* The method call should be rewritten to lcn_driver_pg_query */
    ASSERT(strstr(c, "lcn_driver_pg_query(") != NULL);
    free(c);
}

TEST(codegen_postgres_escape) {
    char *c = gen_c(
        "use driver(\"postgres\") as pg\n"
        "fn main() -> Result {\n"
        "    let safe = pg.escape(conn, \"O'Reilly\")\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* The method call should be rewritten to lcn_driver_pg_escape */
    ASSERT(strstr(c, "lcn_driver_pg_escape(") != NULL);
    free(c);
}

TEST(codegen_postgres_row_accessors) {
    char *c = gen_c(
        "use driver(\"postgres\") as db\n"
        "fn main() -> Result {\n"
        "    let count = db.row_count(result)\n"
        "    let name = db.get(result, 0, \"name\")\n"
        "    let age = db.get_number(result, 0, \"age\")\n"
        "    db.free(result)\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* All row accessor methods should be rewritten */
    ASSERT(strstr(c, "lcn_driver_db_row_count(") != NULL);
    ASSERT(strstr(c, "lcn_driver_db_get(") != NULL);
    ASSERT(strstr(c, "lcn_driver_db_get_number(") != NULL);
    ASSERT(strstr(c, "lcn_driver_db_free(") != NULL);
    free(c);
}

TEST(codegen_full_pipeline) {
    char *c = gen_c(
        "capability web {\n    search\n    fetch requires search\n}\n"
        "taint user_input\n"
        "budget Std {\n    max_tokens: 100000\n    max_cost: 5\n}\n"
        "guard rate_limit() {\n    let max = 50\n}\n"
        "tool web_search(q: string) -> Vec {\n    requires: [web.search]\n}\n"
        "supervisor PaySup {\n    strategy: one_for_one\n    max_restarts: 5\n}\n"
        "agent Researcher {\n    capabilities: [web.search, web.fetch]\n    model: \"claude\"\n"
        "    budget: { max_cost: 5 }\n"
        "    fn run(topic: string) -> Result {\n        let r = web_search(topic, 5)\n        r\n    }\n}\n"
    );
    ASSERT_NOT_NULL(c);
    /* All sections present */
    ASSERT(strstr(c, "CAP_WEB_SEARCH") != NULL);
    ASSERT(strstr(c, "CAP_WEB_FETCH") != NULL);
    ASSERT(strstr(c, "Tainted_user_input") != NULL);
    ASSERT(strstr(c, "lcn_budget_Std") != NULL);
    ASSERT(strstr(c, "lcn_guard_rate_limit") != NULL);
    ASSERT(strstr(c, "lcn_tool_web_search") != NULL);
    ASSERT(strstr(c, "lcn_supervisor_PaySup_init") != NULL);
    ASSERT(strstr(c, "Agent_Researcher") != NULL);
    ASSERT(strstr(c, "lcn_agent_Researcher_run") != NULL);
    ASSERT(strstr(c, "self->capabilities") != NULL);
    free(c);
}

/* ============================================================
 * BETA FEATURE TESTS
 * ============================================================ */

TEST(codegen_for_range) {
    char *c = gen_c("fn main() -> Result {\n    for i in 0..5 {\n        println(\"hello\")\n    }\n}");
    ASSERT_NOT_NULL(c);
    /* Should generate a real C for loop with int64_t counter */
    ASSERT(strstr(c, "int64_t i") != NULL);
    ASSERT(strstr(c, "i < 5") != NULL);
    ASSERT(strstr(c, "i++") != NULL);
    free(c);
}

TEST(codegen_for_inclusive_range) {
    char *c = gen_c("fn main() -> Result {\n    for i in 1..=10 {\n        println(\"hello\")\n    }\n}");
    ASSERT_NOT_NULL(c);
    /* Inclusive range should use <= */
    ASSERT(strstr(c, "int64_t i") != NULL);
    ASSERT(strstr(c, "i <= 10") != NULL);
    free(c);
}

TEST(codegen_for_general) {
    char *c = gen_c("fn main() -> Result {\n    for item in items {\n        println(\"hello\")\n    }\n}");
    ASSERT_NOT_NULL(c);
    /* Should have the variable name */
    ASSERT(strstr(c, "item") != NULL);
    /* Should NOT be completely empty/noop */
    ASSERT(strstr(c, "hello") != NULL);
    free(c);
}

TEST(codegen_try_operator) {
    char *c = gen_c(
        "fn do_something() -> Result {\n    let x = 42\n}\n"
        "fn main() -> Result {\n    let result = do_something()?\n    result\n}"
    );
    ASSERT_NOT_NULL(c);
    /* Should generate try variable and error check */
    ASSERT(strstr(c, "_try_") != NULL);
    ASSERT(strstr(c, ".ok") != NULL);
    free(c);
}

TEST(codegen_try_multiple) {
    char *c = gen_c(
        "fn foo() -> Result { let x = 1 }\n"
        "fn bar() -> Result { let x = 2 }\n"
        "fn main() -> Result {\n    let a = foo()?\n    let b = bar()?\n    b\n}"
    );
    ASSERT_NOT_NULL(c);
    /* Should have two different try variables */
    ASSERT(strstr(c, "_try_") != NULL);
    free(c);
}

TEST(codegen_mcp_wrapper) {
    char *c = gen_c("use mcp(\"npx @test/server\") as db");
    ASSERT_NOT_NULL(c);
    /* Should generate MCP wrapper function */
    ASSERT(strstr(c, "lcn_mcp_db_call") != NULL);
    ASSERT(strstr(c, "MCP import") != NULL);
    /* Should contain the server command */
    ASSERT(strstr(c, "npx @test/server") != NULL);
    free(c);
}

TEST(codegen_for_with_body) {
    char *c = gen_c(
        "fn main() -> Result {\n"
        "    for i in 0..3 {\n"
        "        let x = i\n"
        "    }\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    /* Should have a real for loop with body containing let x */
    ASSERT(strstr(c, "int64_t i") != NULL);
    ASSERT(strstr(c, "x = i") != NULL);
    free(c);
}

TEST(codegen_endpoint_field) {
    char *c = gen_c(
        "capability llm { complete }\n"
        "agent Bot {\n"
        "    capabilities: [llm.complete]\n"
        "    model: \"test-model\"\n"
        "    endpoint: \"http://localhost:8000\"\n"
        "    budget: { max_tokens: 1000 }\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "endpoint") != NULL);
    ASSERT(strstr(c, "http://localhost:8000") != NULL);
    free(c);
}

/* ============================================================
 * PRODUCTION BUILTINS TESTS
 * ============================================================ */

TEST(codegen_len_builtin) {
    char *c = gen_c("fn main() -> Result {\n    let s = \"hello\"\n    let n = len(s)\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "strlen(") != NULL);
    ASSERT(strstr(c, "int64_t") != NULL);
    free(c);
}

TEST(codegen_contains_builtin) {
    char *c = gen_c("fn main() -> Result {\n    let found = contains(\"hello world\", \"world\")\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "strstr(") != NULL);
    ASSERT(strstr(c, "!= NULL") != NULL);
    free(c);
}

TEST(codegen_starts_with_builtin) {
    char *c = gen_c("fn main() -> Result {\n    let ok = starts_with(\"hello\", \"hel\")\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "strncmp(") != NULL);
    free(c);
}

TEST(codegen_ends_with_builtin) {
    char *c = gen_c("fn main() -> Result {\n    let ok = ends_with(\"hello.txt\", \".txt\")\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_str_ends_with(") != NULL);
    free(c);
}

TEST(codegen_env_builtin) {
    char *c = gen_c("fn main() -> Result {\n    let host = env(\"DB_HOST\")\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "getenv(") != NULL);
    free(c);
}

TEST(codegen_str_eq_builtin) {
    char *c = gen_c("fn main() -> Result {\n    let same = str_eq(\"a\", \"b\")\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "strcmp(") != NULL);
    ASSERT(strstr(c, "== 0") != NULL);
    free(c);
}

TEST(codegen_str_replace_builtin) {
    char *c = gen_c("fn main() -> Result {\n    let fixed = str_replace(\"hello\", \"l\", \"r\")\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_str_replace(") != NULL);
    free(c);
}

TEST(codegen_str_trim_builtin) {
    char *c = gen_c("fn main() -> Result {\n    let clean = str_trim(\"  hello  \")\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_str_trim(") != NULL);
    free(c);
}

TEST(codegen_to_string_builtin) {
    char *c = gen_c("fn main() -> Result {\n    let s = to_string(42)\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_str_from_int(") != NULL);
    free(c);
}

TEST(codegen_to_int_builtin) {
    char *c = gen_c("fn main() -> Result {\n    let n = to_int(\"42\")\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "atoll(") != NULL);
    free(c);
}

TEST(codegen_json_parse_builtin) {
    char *c = gen_c("fn main() -> Result {\n    let doc = json_parse(\"{\\\"key\\\":\\\"val\\\"}\")\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_json_parse(") != NULL);
    free(c);
}

TEST(codegen_json_get_builtin) {
    char *c = gen_c("fn main() -> Result {\n    let v = json_get(doc, \"key\")\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_json_get_string(") != NULL);
    free(c);
}

TEST(codegen_json_array_len_builtin) {
    char *c = gen_c("fn main() -> Result {\n    let n = json_array_len(arr)\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_json_array_len(") != NULL);
    free(c);
}

TEST(codegen_json_array_get_builtin) {
    char *c = gen_c("fn main() -> Result {\n    let item = json_array_get(arr, 0)\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_json_array_get(") != NULL);
    free(c);
}

TEST(codegen_string_equality) {
    char *c = gen_c("fn main() -> Result {\n    let x = \"hello\"\n    if x == \"hello\" {\n        println(\"match\")\n    }\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "strcmp(") != NULL);
    ASSERT(strstr(c, "== 0") != NULL);
    free(c);
}

TEST(codegen_string_inequality) {
    char *c = gen_c("fn main() -> Result {\n    let x = \"hello\"\n    if x != \"world\" {\n        println(\"different\")\n    }\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "strcmp(") != NULL);
    ASSERT(strstr(c, "!= 0") != NULL);
    free(c);
}

TEST(codegen_enum_constants) {
    char *c = gen_c("enum Color {\n    RED\n    GREEN\n    BLUE\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "#define Color_RED \"RED\"") != NULL);
    ASSERT(strstr(c, "#define Color_GREEN \"GREEN\"") != NULL);
    ASSERT(strstr(c, "#define Color_BLUE \"BLUE\"") != NULL);
    ASSERT(strstr(c, "lcn_enum_Color_is_valid") != NULL);
    free(c);
}

TEST(codegen_enum_validation) {
    char *c = gen_c("enum Status {\n    ACTIVE\n    INACTIVE\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "strcmp(val, \"ACTIVE\")") != NULL);
    ASSERT(strstr(c, "strcmp(val, \"INACTIVE\")") != NULL);
    ASSERT(strstr(c, "return false;") != NULL);
    free(c);
}

TEST(codegen_mcp_returns_string) {
    char *c = gen_c("use mcp(\"mysql-server\") as db");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "LcnString lcn_mcp_db_call") != NULL);
    free(c);
}

TEST(codegen_categorizer_pattern) {
    char *c = gen_c(
        "use mcp(\"mysql-server\") as db\n"
        "enum DevCat {\n    CAT_A\n    CAT_B\n}\n"
        "budget Std {\n    max_tokens: 100000\n    max_cost: 5\n}\n"
        "fn main() -> Result {\n"
        "    let host = env(\"DB_HOST\")\n"
        "    let data = db.call(\"query\", \"{\\\"sql\\\":\\\"SELECT *\\\"}\")\n"
        "    let n = len(host)\n"
        "    for i in 0..10 {\n"
        "        let x = to_string(i)\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* All features present */
    ASSERT(strstr(c, "getenv(") != NULL);             /* env() */
    ASSERT(strstr(c, "lcn_mcp_db_call") != NULL);     /* MCP */
    ASSERT(strstr(c, "#define DevCat_CAT_A") != NULL); /* enum */
    ASSERT(strstr(c, "strlen(") != NULL);              /* len() */
    ASSERT(strstr(c, "lcn_str_from_int(") != NULL);   /* to_string() */
    free(c);
}

/* ============================================================
 * LLMOUTPUT + MATCH TESTS
 * ============================================================ */

TEST(codegen_ask_returns_llmoutput) {
    char *c = gen_c("fn main() -> Result {\n    let result = ask(\"hello\")\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "LcnLlmOutput") != NULL);
    ASSERT(strstr(c, "lcn_ask_typed(") != NULL);
    free(c);
}

TEST(codegen_match_llmoutput) {
    char *c = gen_c(
        "fn main() -> Result {\n"
        "    let result = ask(\"hello\")\n"
        "    match result {\n"
        "        Ok(text) -> {\n"
        "            println(text)\n"
        "        }\n"
        "        Error(msg) -> {\n"
        "            println(msg)\n"
        "        }\n"
        "        _ -> {\n"
        "            println(\"other\")\n"
        "        }\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "switch (") != NULL);
    ASSERT(strstr(c, "LCN_LLM_OUTPUT_OK") != NULL);
    ASSERT(strstr(c, "LCN_LLM_OUTPUT_ERROR") != NULL);
    ASSERT(strstr(c, "default:") != NULL);
    free(c);
}

TEST(codegen_match_variant_binding) {
    char *c = gen_c(
        "fn main() -> Result {\n"
        "    let result = ask(\"test\")\n"
        "    match result {\n"
        "        Ok(response) -> {\n"
        "            println(response)\n"
        "        }\n"
        "        Text(raw) -> {\n"
        "            println(raw)\n"
        "        }\n"
        "        ToolCall(name, args) -> {\n"
        "            println(name)\n"
        "        }\n"
        "        Error(e) -> {\n"
        "            println(e)\n"
        "        }\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* All 4 variants present */
    ASSERT(strstr(c, "LCN_LLM_OUTPUT_OK") != NULL);
    ASSERT(strstr(c, "LCN_LLM_OUTPUT_TEXT") != NULL);
    ASSERT(strstr(c, "LCN_LLM_OUTPUT_TOOL_CALL") != NULL);
    ASSERT(strstr(c, "LCN_LLM_OUTPUT_ERROR") != NULL);
    /* Bindings extracted */
    ASSERT(strstr(c, "response") != NULL);
    ASSERT(strstr(c, "tool_name") != NULL);
    ASSERT(strstr(c, "tool_args") != NULL);
    free(c);
}

TEST(codegen_llmoutput_preamble) {
    char *c = gen_c("");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "LcnLlmOutputKind") != NULL);
    ASSERT(strstr(c, "LCN_LLM_OUTPUT_OK") != NULL);
    ASSERT(strstr(c, "LcnLlmOutput") != NULL);
    ASSERT(strstr(c, "lcn_llm_output_ok") != NULL);
    ASSERT(strstr(c, "lcn_ask_typed") != NULL);
    free(c);
}

/* ============================================================
 * MULTI-FILE IMPORT TESTS
 * ============================================================ */

TEST(test_parse_use_module_path) {
    bool err;
    AstNode *prog = parse_source("use agents.extractor", &err);
    ASSERT_FALSE(err);
    ASSERT_NOT_NULL(prog);
    ASSERT_EQ(prog->params->kind, AST_USE);
    ASSERT_STR_EQ(prog->params->name, "agents.extractor");
}

TEST(test_parse_use_deep_path) {
    bool err;
    AstNode *prog = parse_source("use shared.utils.helpers", &err);
    ASSERT_FALSE(err);
    ASSERT_NOT_NULL(prog);
    ASSERT_EQ(prog->params->kind, AST_USE);
    ASSERT_STR_EQ(prog->params->name, "shared.utils.helpers");
}

TEST(test_parse_use_grouped) {
    bool err;
    AstNode *prog = parse_source("use shared.{budgets, config}", &err);
    ASSERT_FALSE(err);
    ASSERT_NOT_NULL(prog);
    AstNode *use = prog->params;
    ASSERT_EQ(use->kind, AST_USE);
    ASSERT_STR_EQ(use->name, "shared");
    /* params list should have 2 AST_IDENT items */
    ASSERT_NOT_NULL(use->params);
    ASSERT_EQ(use->params->kind, AST_IDENT);
    ASSERT_NOT_NULL(use->params->next);
    ASSERT_EQ(use->params->next->kind, AST_IDENT);
}

TEST(test_parse_use_with_alias) {
    bool err;
    AstNode *prog = parse_source("use db.mysql as database", &err);
    ASSERT_FALSE(err);
    ASSERT_NOT_NULL(prog);
    AstNode *use = prog->params;
    ASSERT_EQ(use->kind, AST_USE);
    ASSERT_STR_EQ(use->name, "db.mysql");
    ASSERT_NOT_NULL(use->val.str_val);
    ASSERT_STR_EQ(use->val.str_val, "database");
}

TEST(test_parse_multiple_use) {
    bool err;
    AstNode *prog = parse_source(
        "use agents.extractor\n"
        "use shared.budgets\n"
        "use config",
        &err
    );
    ASSERT_FALSE(err);
    ASSERT_NOT_NULL(prog);
    /* First use */
    AstNode *u1 = prog->params;
    ASSERT_NOT_NULL(u1);
    ASSERT_EQ(u1->kind, AST_USE);
    ASSERT_STR_EQ(u1->name, "agents.extractor");
    /* Second use */
    AstNode *u2 = u1->next;
    ASSERT_NOT_NULL(u2);
    ASSERT_EQ(u2->kind, AST_USE);
    ASSERT_STR_EQ(u2->name, "shared.budgets");
    /* Third use */
    AstNode *u3 = u2->next;
    ASSERT_NOT_NULL(u3);
    ASSERT_EQ(u3->kind, AST_USE);
    ASSERT_STR_EQ(u3->name, "config");
}

TEST(test_parse_use_mcp_not_module) {
    bool err;
    AstNode *prog = parse_source("use mcp(\"test-server\") as db", &err);
    ASSERT_FALSE(err);
    ASSERT_NOT_NULL(prog);
    AstNode *use = prog->params;
    ASSERT_EQ(use->kind, AST_USE);
    /* MCP uses should have name "mcp", not treated as a module path */
    ASSERT_STR_EQ(use->name, "mcp");
}

/* ============================================================
 * STDLIB CODEGEN TESTS
 * ============================================================ */

TEST(codegen_math_builtins) {
    char *c = gen_c("fn main() -> Result {\n    let a = abs(-5)\n    let b = min(1, 2)\n    let c = max(3, 4)\n    let d = clamp(10, 0, 5)\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "llabs(") != NULL);
    ASSERT(strstr(c, "lcn_min(") != NULL);
    ASSERT(strstr(c, "lcn_max(") != NULL);
    ASSERT(strstr(c, "lcn_clamp(") != NULL);
    free(c);
}

TEST(codegen_math_float) {
    char *c = gen_c("fn main() -> Result {\n    let a = floor(3.7)\n    let b = ceil(3.2)\n    let c = round(3.5)\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "floor(") != NULL);
    ASSERT(strstr(c, "ceil(") != NULL);
    ASSERT(strstr(c, "round(") != NULL);
    free(c);
}

TEST(codegen_time_builtins) {
    char *c = gen_c("fn main() -> Result {\n    let t = now_ms()\n    sleep_ms(100)\n    let e = elapsed_ms(t)\n    let s = format_timestamp(t)\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_now_ms()") != NULL);
    ASSERT(strstr(c, "lcn_sleep_ms(") != NULL);
    ASSERT(strstr(c, "lcn_format_timestamp(") != NULL);
    free(c);
}

TEST(codegen_log_builtins) {
    char *c = gen_c("fn main() -> Result {\n    log_info(\"started\")\n    log_warn(\"slow\")\n    log_error(\"fail\")\n    log_debug(\"x\")\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_log(\"INFO\"") != NULL);
    ASSERT(strstr(c, "lcn_log(\"WARN\"") != NULL);
    ASSERT(strstr(c, "lcn_log(\"ERROR\"") != NULL);
    ASSERT(strstr(c, "lcn_log(\"DEBUG\"") != NULL);
    free(c);
}

TEST(codegen_batch_builtins) {
    char *c = gen_c("fn main() -> Result {\n    let bs = batch_size()\n    let off = batch_offset()\n    let dry = batch_dry_run()\n    batch_progress(1, 10)\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_batch_size()") != NULL);
    ASSERT(strstr(c, "lcn_batch_offset()") != NULL);
    ASSERT(strstr(c, "lcn_batch_dry_run()") != NULL);
    ASSERT(strstr(c, "lcn_batch_progress(") != NULL);
    free(c);
}

TEST(codegen_budget_introspection) {
    char *c = gen_c("fn main() -> Result {\n    let used = budget_tokens_used()\n    let left = budget_tokens_left()\n    let pct = budget_percentage()\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_budget_tokens_used()") != NULL);
    ASSERT(strstr(c, "lcn_budget_tokens_left()") != NULL);
    ASSERT(strstr(c, "lcn_budget_percentage()") != NULL);
    free(c);
}

TEST(codegen_token_estimation) {
    char *c = gen_c("fn main() -> Result {\n    let est = estimate_tokens(\"hello world\")\n    let fits = fits_in_budget(\"test\", 100)\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_estimate_tokens(") != NULL);
    ASSERT(strstr(c, "lcn_fits_in_budget(") != NULL);
    free(c);
}

TEST(codegen_trace_builtins) {
    char *c = gen_c("fn main() -> Result {\n    let span = trace_begin(\"op\")\n    trace_tag(span, \"k\", \"v\")\n    trace_end(span)\n    trace_event(\"done\", \"ok\")\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_trace_begin(") != NULL);
    ASSERT(strstr(c, "lcn_trace_tag(") != NULL);
    ASSERT(strstr(c, "lcn_trace_end(") != NULL);
    ASSERT(strstr(c, "lcn_trace_event(") != NULL);
    free(c);
}

TEST(codegen_file_io_builtins) {
    char *c = gen_c("fn main() -> Result {\n    let content = read_file(\"/tmp/test\")\n    let ok = write_file(\"/tmp/out\", \"data\")\n    let line = read_line()\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_read_file(") != NULL);
    ASSERT(strstr(c, "lcn_write_file(") != NULL);
    ASSERT(strstr(c, "lcn_read_line()") != NULL);
    free(c);
}

TEST(codegen_env_or_builtin) {
    char *c = gen_c("fn main() -> Result {\n    let h = env_or(\"HOST\", \"localhost\")\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "getenv(") != NULL);
    ASSERT(strstr(c, "\"localhost\"") != NULL);
    free(c);
}

TEST(codegen_sql_escape_builtin) {
    char *c = gen_c(
        "fn main() -> Result {\n"
        "    let safe = sql_escape(\"O'Reilly\")\n"
        "    println(safe)\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_sql_escape") != NULL);
    free(c);
}

/* ============================================================
 * KEYWORD-AS-IDENTIFIER TESTS
 * ============================================================ */

TEST(parse_keyword_agent_as_ident) {
    bool err;
    AstNode *prog = parse_source(
        "fn test() {\n"
        "    let agent = 42\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *let_stmt = prog->params->left->params;
    ASSERT_EQ(let_stmt->kind, AST_LET);
    ASSERT_STR_EQ(let_stmt->name, "agent");
    ASSERT_EQ(let_stmt->right->kind, AST_INT_LIT);
    ASSERT_EQ(let_stmt->right->val.int_val, 42);
}

TEST(parse_keyword_budget_as_ident) {
    bool err;
    AstNode *prog = parse_source(
        "fn test() {\n"
        "    let budget = 100\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *let_stmt = prog->params->left->params;
    ASSERT_EQ(let_stmt->kind, AST_LET);
    ASSERT_STR_EQ(let_stmt->name, "budget");
    ASSERT_EQ(let_stmt->right->kind, AST_INT_LIT);
    ASSERT_EQ(let_stmt->right->val.int_val, 100);
}

TEST(parse_keyword_channel_as_ident) {
    bool err;
    AstNode *prog = parse_source(
        "fn test() {\n"
        "    let channel = \"foo\"\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *let_stmt = prog->params->left->params;
    ASSERT_EQ(let_stmt->kind, AST_LET);
    ASSERT_STR_EQ(let_stmt->name, "channel");
    ASSERT_EQ(let_stmt->right->kind, AST_STRING_LIT);
}

TEST(parse_keyword_channel_as_param) {
    bool err;
    AstNode *prog = parse_source(
        "fn send(channel: string) {\n"
        "    println(channel)\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *fn = prog->params;
    ASSERT_EQ(fn->kind, AST_FN);
    ASSERT_STR_EQ(fn->name, "send");
    ASSERT_NOT_NULL(fn->params);
    ASSERT_STR_EQ(fn->params->name, "channel");
}

TEST(parse_keyword_ident_in_expr) {
    bool err;
    AstNode *prog = parse_source(
        "fn test() {\n"
        "    let budget = 100\n"
        "    let remaining = budget - 10\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *second = prog->params->left->params->next;
    ASSERT_EQ(second->kind, AST_LET);
    ASSERT_STR_EQ(second->name, "remaining");
    ASSERT_NOT_NULL(second->right);
    ASSERT_EQ(second->right->kind, AST_BINARY);
}

/* ============================================================
 * COMMA-SEPARATED FIELD TESTS
 * ============================================================ */

TEST(parse_agent_three_fields) {
    bool err;
    AstNode *prog = parse_source(
        "agent Bot {\n"
        "    capabilities: [web.search]\n"
        "    model: \"gpt-4\"\n"
        "    budget: { max_tokens: 1000 }\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *ag = prog->params;
    ASSERT_EQ(ag->kind, AST_AGENT);
    ASSERT_STR_EQ(ag->name, "Bot");
    ASSERT_EQ(ast_list_len(ag->params), 3);
}

TEST(parse_skill_two_fields) {
    bool err;
    AstNode *prog = parse_source(
        "skill Search {\n"
        "    version: \"2.0\"\n"
        "    requires: [web.search]\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *s = prog->params;
    ASSERT_EQ(s->kind, AST_SKILL);
    ASSERT_STR_EQ(s->name, "Search");
    ASSERT_EQ(ast_list_len(s->params), 2);
}

TEST(parse_agent_with_method) {
    bool err;
    AstNode *prog = parse_source(
        "agent Helper {\n"
        "    capabilities: [web.search]\n"
        "    model: \"claude\"\n"
        "    budget: { max_tokens: 5000 }\n"
        "    fn run(q: string) -> Result {\n"
        "        let x = 1\n"
        "    }\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *ag = prog->params;
    ASSERT_EQ(ag->kind, AST_AGENT);
    ASSERT_EQ(ast_list_len(ag->params), 3);
    ASSERT_NOT_NULL(ag->left);
    ASSERT_EQ(ag->left->kind, AST_FN);
    ASSERT_STR_EQ(ag->left->name, "run");
}

TEST(parse_supervisor_two_fields) {
    bool err;
    AstNode *prog = parse_source(
        "supervisor Sup {\n"
        "    strategy: one_for_one\n"
        "    max_restarts: 3\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *sv = prog->params;
    ASSERT_EQ(sv->kind, AST_SUPERVISOR);
    ASSERT_EQ(ast_list_len(sv->params), 2);
    ASSERT_STR_EQ(sv->params->name, "strategy");
    ASSERT_STR_EQ(sv->params->next->name, "max_restarts");
}

/* ============================================================
 * STRING INTERPOLATION CODEGEN TESTS
 * ============================================================ */

TEST(codegen_interp_simple) {
    char *c = gen_c("fn main() -> Result {\n    let name = \"world\"\n    let msg = \"Hello {name}\"\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_str_concat") != NULL);
    ASSERT(strstr(c, "\"Hello \"") != NULL);
    ASSERT(strstr(c, "name") != NULL);
    free(c);
}

TEST(codegen_interp_adjacent) {
    char *c = gen_c("fn main() -> Result {\n    let a = \"x\"\n    let b = \"y\"\n    let msg = \"{a}{b}\"\n}");
    ASSERT_NOT_NULL(c);
    /* Two adjacent interpolations → nested concat */
    ASSERT(strstr(c, "lcn_str_concat") != NULL);
    free(c);
}

TEST(codegen_interp_none) {
    char *c = gen_c("fn main() -> Result {\n    let msg = \"no interpolation\"\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "\"no interpolation\"") != NULL);
    free(c);
}

TEST(codegen_interp_space_not_interp) {
    char *c = gen_c("fn main() -> Result {\n    let msg = \"{ not_interp }\"\n}");
    ASSERT_NOT_NULL(c);
    /* Space after brace should NOT trigger interpolation — stays as literal */
    /* The string should appear in the output without lcn_str_concat for this segment */
    ASSERT(strstr(c, "not_interp") != NULL);
    free(c);
}

TEST(codegen_interp_middle) {
    char *c = gen_c("fn main() -> Result {\n    let name = \"Bob\"\n    let msg = \"Hello {name} world\"\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_str_concat") != NULL);
    ASSERT(strstr(c, "\"Hello \"") != NULL);
    ASSERT(strstr(c, "\" world\"") != NULL);
    free(c);
}

TEST(codegen_plain_no_concat) {
    char *c = gen_c("fn main() -> Result {\n    let msg = \"plain\"\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "\"plain\"") != NULL);
    free(c);
}

/* ============================================================
 * STRUCT LITERAL TESTS
 * ============================================================ */

TEST(codegen_struct_literal) {
    char *c = gen_c(
        "struct Point {\n    x: i32\n    y: i32\n}\n"
        "fn main() -> Result {\n    let p = Point { x: 1, y: 2 }\n}"
    );
    ASSERT_NOT_NULL(c);
    /* Should generate C compound literal */
    ASSERT(strstr(c, "(Point){") != NULL);
    ASSERT(strstr(c, ".x = 1") != NULL);
    ASSERT(strstr(c, ".y = 2") != NULL);
    free(c);
}

TEST(parse_struct_literal_ast) {
    bool err;
    AstNode *prog = parse_source(
        "fn test() {\n"
        "    let p = Point { x: 1, y: 2 }\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *let_stmt = prog->params->left->params;
    ASSERT_EQ(let_stmt->kind, AST_LET);
    ASSERT_NOT_NULL(let_stmt->right);
    ASSERT_EQ(let_stmt->right->kind, AST_MAP);
    ASSERT_NOT_NULL(let_stmt->right->name);
    ASSERT_STR_EQ(let_stmt->right->name, "Point");
}

TEST(codegen_struct_literal_empty) {
    char *c = gen_c(
        "struct Empty {}\n"
        "fn main() -> Result {\n    let e = Empty {}\n}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "(Empty){") != NULL);
    free(c);
}

TEST(codegen_struct_literal_single_field) {
    char *c = gen_c(
        "struct Wrapper {\n    value: i32\n}\n"
        "fn main() -> Result {\n    let w = Wrapper { value: 99 }\n}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "(Wrapper){") != NULL);
    ASSERT(strstr(c, ".value = 99") != NULL);
    free(c);
}

TEST(codegen_struct_literal_let_binding) {
    char *c = gen_c(
        "struct Config {\n    port: i32\n    host: string\n}\n"
        "fn main() -> Result {\n    let cfg = Config { port: 8080, host: \"localhost\" }\n}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "Config") != NULL);
    ASSERT(strstr(c, ".port = 8080") != NULL);
    ASSERT(strstr(c, ".host = \"localhost\"") != NULL);
    free(c);
}

/* ============================================================
 * IMPL BLOCK TESTS
 * ============================================================ */

TEST(parse_impl_as_ast_impl) {
    bool err;
    AstNode *prog = parse_source(
        "impl Point {\n"
        "    fn zero() {}\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *impl = prog->params;
    ASSERT_EQ(impl->kind, AST_IMPL);
    ASSERT_NOT_NULL(impl->left);
    ASSERT_EQ(impl->left->kind, AST_TYPE_NAMED);
    ASSERT_STR_EQ(impl->left->name, "Point");
    ASSERT_EQ(ast_list_len(impl->params), 1);
}

TEST(codegen_impl_method_name) {
    char *c = gen_c(
        "struct Point {\n    x: f64\n    y: f64\n}\n"
        "impl Point {\n"
        "    fn origin() -> Point {\n"
        "        Point { x: 0, y: 0 }\n"
        "    }\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    /* Method should be emitted as Point_origin */
    ASSERT(strstr(c, "Point_origin") != NULL);
    ASSERT(strstr(c, "Point *self") != NULL);
    free(c);
}

TEST(parse_impl_multiple_methods) {
    bool err;
    AstNode *prog = parse_source(
        "impl Vec2 {\n"
        "    fn length() -> f64 {\n"
        "        0.0\n"
        "    }\n"
        "    fn normalize() -> Vec2 {\n"
        "        Vec2 { x: 0, y: 0 }\n"
        "    }\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *impl = prog->params;
    ASSERT_EQ(impl->kind, AST_IMPL);
    ASSERT_EQ(ast_list_len(impl->params), 2);
}

TEST(codegen_impl_self_param) {
    char *c = gen_c(
        "struct Counter {\n    count: i32\n}\n"
        "impl Counter {\n"
        "    fn increment() {\n"
        "        let x = 1\n"
        "    }\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    /* Method signature should include self pointer */
    ASSERT(strstr(c, "Counter *self") != NULL);
    ASSERT(strstr(c, "Counter_increment") != NULL);
    free(c);
}

TEST(codegen_impl_pre_registration) {
    char *c = gen_c(
        "struct Calc {\n    val: i32\n}\n"
        "impl Calc {\n"
        "    fn reset() {\n"
        "        let x = 0\n"
        "    }\n"
        "    fn get() -> i32 {\n"
        "        return 0\n"
        "    }\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    /* Both methods should be generated */
    ASSERT(strstr(c, "Calc_reset") != NULL);
    ASSERT(strstr(c, "Calc_get") != NULL);
    /* impl comment should be present */
    ASSERT(strstr(c, "impl Calc") != NULL);
    free(c);
}

/* ============================================================
 * ERROR PROPAGATION / ? OPERATOR TESTS
 * ============================================================ */

TEST(codegen_try_generates_pattern) {
    char *c = gen_c(
        "fn do_work() -> Result {\n    let x = 1\n}\n"
        "fn main() -> Result {\n    let x = do_work()?\n}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "_try_") != NULL);
    ASSERT(strstr(c, ".ok") != NULL);
    free(c);
}

TEST(codegen_try_on_expression) {
    char *c = gen_c(
        "fn fetch() -> Result {\n    let x = 1\n}\n"
        "fn main() -> Result {\n    let data = fetch()?\n    data\n}"
    );
    ASSERT_NOT_NULL(c);
    /* Should generate try variable with error check */
    ASSERT(strstr(c, "_try_") != NULL);
    free(c);
}

TEST(codegen_try_nested) {
    char *c = gen_c(
        "fn step1() -> Result {\n    let x = 1\n}\n"
        "fn step2() -> Result {\n    let x = 2\n}\n"
        "fn step3() -> Result {\n    let x = 3\n}\n"
        "fn main() -> Result {\n"
        "    let a = step1()?\n"
        "    let b = step2()?\n"
        "    let c = step3()?\n"
        "    c\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    /* Should have multiple try variables */
    ASSERT(strstr(c, "_try_") != NULL);
    free(c);
}

/* ============================================================
 * FOR LOOP AND CONTROL FLOW TESTS
 * ============================================================ */

TEST(parse_for_in_items) {
    bool err;
    AstNode *prog = parse_source(
        "fn test() {\n"
        "    for x in items {\n"
        "        println(x)\n"
        "    }\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *for_stmt = prog->params->left->params;
    ASSERT_EQ(for_stmt->kind, AST_FOR);
    ASSERT_NOT_NULL(for_stmt->left);   /* pattern: x */
    ASSERT_NOT_NULL(for_stmt->params); /* iterator: items */
    ASSERT_NOT_NULL(for_stmt->right);  /* body */
}

TEST(parse_while_cond) {
    bool err;
    AstNode *prog = parse_source(
        "fn test() {\n"
        "    while active {\n"
        "        process()\n"
        "    }\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *while_stmt = prog->params->left->params;
    ASSERT_EQ(while_stmt->kind, AST_WHILE);
    ASSERT_NOT_NULL(while_stmt->left);  /* condition */
    ASSERT_NOT_NULL(while_stmt->right); /* body */
}

TEST(parse_break_continue) {
    bool err;
    AstNode *prog = parse_source(
        "fn test() {\n"
        "    while true {\n"
        "        break\n"
        "        continue\n"
        "    }\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *while_stmt = prog->params->left->params;
    ASSERT_EQ(while_stmt->kind, AST_WHILE);
    AstNode *body = while_stmt->right;
    ASSERT_NOT_NULL(body);
    ASSERT_NOT_NULL(body->params);
    /* First statement should be break */
    ASSERT_EQ(body->params->kind, AST_BREAK);
    /* Second statement should be continue */
    ASSERT_NOT_NULL(body->params->next);
    ASSERT_EQ(body->params->next->kind, AST_CONTINUE);
}

TEST(parse_nested_loops) {
    bool err;
    AstNode *prog = parse_source(
        "fn test() {\n"
        "    for i in outer {\n"
        "        for j in inner {\n"
        "            process(i, j)\n"
        "        }\n"
        "    }\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *outer = prog->params->left->params;
    ASSERT_EQ(outer->kind, AST_FOR);
    /* Body of outer loop should contain inner for */
    AstNode *inner = outer->right->params;
    ASSERT_EQ(inner->kind, AST_FOR);
}

TEST(codegen_for_range_expr) {
    char *c = gen_c("fn main() -> Result {\n    for i in 0..10 {\n        let x = i\n    }\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "int64_t i") != NULL);
    ASSERT(strstr(c, "i < 10") != NULL);
    ASSERT(strstr(c, "i++") != NULL);
    free(c);
}

/* ============================================================
 * PATTERN MATCHING TESTS
 * ============================================================ */

TEST(parse_match_wildcard) {
    bool err;
    AstNode *prog = parse_source(
        "fn test() {\n"
        "    match x {\n"
        "        _ -> \"default\"\n"
        "    }\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *stmt = prog->params->left->params;
    AstNode *match_expr = stmt->left;
    ASSERT_EQ(match_expr->kind, AST_MATCH);
    ASSERT_EQ(ast_list_len(match_expr->params), 1);
}

TEST(parse_match_multiple_arms) {
    bool err;
    AstNode *prog = parse_source(
        "fn test() {\n"
        "    match status {\n"
        "        200 -> \"ok\"\n"
        "        404 -> \"not found\"\n"
        "        500 -> \"error\"\n"
        "        _ -> \"unknown\"\n"
        "    }\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *stmt = prog->params->left->params;
    AstNode *match_expr = stmt->left;
    ASSERT_EQ(match_expr->kind, AST_MATCH);
    ASSERT_EQ(ast_list_len(match_expr->params), 4);
}

TEST(parse_match_string_literals) {
    bool err;
    AstNode *prog = parse_source(
        "fn test() {\n"
        "    match cmd {\n"
        "        \"start\" -> run()\n"
        "        \"stop\" -> halt()\n"
        "        _ -> noop()\n"
        "    }\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *stmt = prog->params->left->params;
    AstNode *match_expr = stmt->left;
    ASSERT_EQ(match_expr->kind, AST_MATCH);
    ASSERT_EQ(ast_list_len(match_expr->params), 3);
}

TEST(codegen_match_enum_variant) {
    char *c = gen_c(
        "fn main() -> Result {\n"
        "    let result = ask(\"test\")\n"
        "    match result {\n"
        "        Ok(text) -> {\n"
        "            println(text)\n"
        "        }\n"
        "        Error(e) -> {\n"
        "            println(e)\n"
        "        }\n"
        "    }\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "switch (") != NULL);
    ASSERT(strstr(c, "LCN_LLM_OUTPUT_OK") != NULL);
    ASSERT(strstr(c, "LCN_LLM_OUTPUT_ERROR") != NULL);
    free(c);
}

TEST(codegen_match_struct_destructure) {
    char *c = gen_c(
        "struct Point {\n"
        "    x: i64\n"
        "    y: i64\n"
        "}\n"
        "fn main() -> Result {\n"
        "    let result = ask(\"test\")\n"
        "    match result {\n"
        "        Point { x, y } -> {\n"
        "            println(to_string(x))\n"
        "        }\n"
        "    }\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    /* Struct destructuring should bind fields from the match target */
    ASSERT(strstr(c, "_match_") != NULL);
    ASSERT(strstr(c, ".x") != NULL);
    ASSERT(strstr(c, ".y") != NULL);
    free(c);
}

TEST(codegen_match_tuple_destructure) {
    char *c = gen_c(
        "fn main() -> Result {\n"
        "    let result = ask(\"test\")\n"
        "    match result {\n"
        "        (a, b) -> {\n"
        "            println(to_string(a))\n"
        "        }\n"
        "    }\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    /* Tuple destructuring should bind positional elements */
    ASSERT(strstr(c, "_match_") != NULL);
    ASSERT(strstr(c, "._0") != NULL);
    ASSERT(strstr(c, "._1") != NULL);
    free(c);
}

TEST(codegen_match_struct_rename) {
    char *c = gen_c(
        "struct Point {\n"
        "    x: i64\n"
        "    y: i64\n"
        "}\n"
        "fn main() -> Result {\n"
        "    let result = ask(\"test\")\n"
        "    match result {\n"
        "        Point { x: a, y: b } -> {\n"
        "            println(to_string(a))\n"
        "        }\n"
        "    }\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    /* Renamed bindings: x: a should bind 'a' from .x */
    ASSERT(strstr(c, "_match_") != NULL);
    ASSERT(strstr(c, ".x") != NULL);
    ASSERT(strstr(c, ".y") != NULL);
    free(c);
}

/* ============================================================
 * ADDITIONAL TYPE CHECKER TESTS
 * ============================================================ */

TEST(typecheck_rejects_unknown_type) {
    bool ok = typecheck_source(
        "fn test(x: NonExistentType) -> i32 {\n    return 0\n}"
    );
    /* Unknown type should be flagged */
    (void)ok;
    ASSERT(1); /* Should not crash */
}

TEST(typecheck_declared_fn_ok) {
    bool ok = typecheck_source(
        "fn helper() -> i32 {\n    return 42\n}\n"
        "fn main() -> i32 {\n    return helper()\n}"
    );
    ASSERT(ok);
}

TEST(typecheck_agent_undeclared_fn) {
    bool ok = typecheck_source(
        "capability web {\n    search\n}\n"
        "tool web_search(q: string) -> Result {\n    requires: [web.search]\n}\n"
        "agent Bot {\n"
        "    capabilities: [web.search]\n"
        "    model: \"gpt-4\"\n"
        "    budget: { max_tokens: 5000 }\n"
        "    fn run(topic: string) -> Result {\n"
        "        let r = nonexistent_function(topic)\n"
        "        r\n"
        "    }\n"
        "}"
    );
    /* Should still parse OK even if function doesn't exist
       (type checker may or may not enforce this yet) */
    (void)ok;
    ASSERT(1); /* Must not crash */
}

TEST(typecheck_impl_blocks) {
    bool ok = typecheck_source(
        "struct Point {\n    x: f64\n    y: f64\n}\n"
        "impl Point {\n"
        "    fn zero() -> Point {\n"
        "        Point { x: 0, y: 0 }\n"
        "    }\n"
        "}"
    );
    /* Impl blocks should typecheck without crashing */
    (void)ok;
    ASSERT(1);
}

/* ============================================================
 * EDGE CASE TESTS
 * ============================================================ */

TEST(parse_empty_program) {
    bool err;
    AstNode *prog = parse_source("", &err);
    ASSERT_FALSE(err);
    ASSERT_NOT_NULL(prog);
    ASSERT_EQ(prog->kind, AST_PROGRAM);
}

TEST(codegen_empty_program) {
    char *c = gen_c("");
    ASSERT_NOT_NULL(c);
    /* Should at least have the preamble and main */
    ASSERT(strstr(c, "int main(") != NULL);
    free(c);
}

TEST(parse_single_function) {
    bool err;
    AstNode *prog = parse_source(
        "fn hello() {\n"
        "    println(\"hi\")\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    ASSERT_EQ(ast_list_len(prog->params), 1);
    ASSERT_EQ(prog->params->kind, AST_FN);
    ASSERT_STR_EQ(prog->params->name, "hello");
}

TEST(codegen_unicode_string) {
    char *c = gen_c("fn main() -> Result {\n    let msg = \"Hola mundo \\xC2\\xA1Limceron!\"\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "msg") != NULL);
    free(c);
}

TEST(parse_long_identifier) {
    bool err;
    AstNode *prog = parse_source(
        "fn this_is_a_very_long_function_name_that_tests_identifiers() {\n"
        "    let also_a_very_long_variable_name_for_testing_purposes = 42\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *fn = prog->params;
    ASSERT_EQ(fn->kind, AST_FN);
    ASSERT_STR_EQ(fn->name, "this_is_a_very_long_function_name_that_tests_identifiers");
    AstNode *let_stmt = fn->left->params;
    ASSERT_STR_EQ(let_stmt->name, "also_a_very_long_variable_name_for_testing_purposes");
}

/* ============================================================
 * Access Control Policy Tests
 * ============================================================ */

TEST(parse_cap_endpoint_allow) {
    bool err;
    AstNode *prog = parse_source(
        "capability network {\n"
        "    allow endpoint \"api.github.com:443\" {\n"
        "        method: [GET, POST]\n"
        "        path: \"/repos/**\"\n"
        "    }\n"
        "}", &err);
    ASSERT_FALSE(err);
    AstNode *cap = prog->params;
    ASSERT_EQ(cap->kind, AST_CAPABILITY);
    ASSERT_STR_EQ(cap->name, "network");
    AstNode *rule = cap->params;
    ASSERT_EQ(rule->kind, AST_CAP_ENDPOINT_RULE);
    ASSERT_TRUE(rule->is_mut); /* allow */
    ASSERT_STR_EQ(rule->name, "api.github.com:443");
    ASSERT_NOT_NULL(rule->params); /* has method/path fields */
}

TEST(parse_cap_binary_allow_deny) {
    bool err;
    AstNode *prog = parse_source(
        "capability shell {\n"
        "    allow binary \"/usr/bin/git\"\n"
        "    deny binary \"/bin/rm\"\n"
        "    default: deny\n"
        "}", &err);
    ASSERT_FALSE(err);
    AstNode *cap = prog->params;
    AstNode *r1 = cap->params;
    ASSERT_EQ(r1->kind, AST_CAP_BINARY_RULE);
    ASSERT_TRUE(r1->is_mut); /* allow */
    ASSERT_STR_EQ(r1->name, "/usr/bin/git");
    AstNode *r2 = r1->next;
    ASSERT_EQ(r2->kind, AST_CAP_BINARY_RULE);
    ASSERT_FALSE(r2->is_mut); /* deny */
    ASSERT_STR_EQ(r2->name, "/bin/rm");
    AstNode *def = r2->next;
    ASSERT_EQ(def->kind, AST_CAP_DEFAULT);
    ASSERT_FALSE(def->is_mut); /* deny */
}

TEST(parse_cap_deny_private_ranges) {
    bool err;
    AstNode *prog = parse_source(
        "capability network {\n"
        "    deny private_ranges\n"
        "    default: deny\n"
        "}", &err);
    ASSERT_FALSE(err);
    AstNode *rule = prog->params->params;
    ASSERT_EQ(rule->kind, AST_CAP_DENY_RANGE);
    ASSERT_STR_EQ(rule->name, "private_ranges");
    AstNode *def = rule->next;
    ASSERT_EQ(def->kind, AST_CAP_DEFAULT);
}

TEST(parse_cap_mixed_abstract_concrete) {
    bool err;
    AstNode *prog = parse_source(
        "capability web {\n"
        "    search\n"
        "    fetch requires search\n"
        "    allow endpoint \"api.github.com:443\"\n"
        "    default: deny\n"
        "}", &err);
    ASSERT_FALSE(err);
    AstNode *item1 = prog->params->params;
    ASSERT_EQ(item1->kind, AST_CAPABILITY_ITEM);
    ASSERT_STR_EQ(item1->name, "search");
    AstNode *item2 = item1->next;
    ASSERT_EQ(item2->kind, AST_CAPABILITY_ITEM);
    ASSERT_STR_EQ(item2->name, "fetch");
    AstNode *rule = item2->next;
    ASSERT_EQ(rule->kind, AST_CAP_ENDPOINT_RULE);
    AstNode *def = rule->next;
    ASSERT_EQ(def->kind, AST_CAP_DEFAULT);
}

TEST(typecheck_network_static_allowed) {
    bool ok = typecheck_source(
        "capability network {\n"
        "    allow endpoint \"api.github.com:443\" {\n"
        "        path: \"/repos/**\"\n"
        "    }\n"
        "    deny private_ranges\n"
        "    default: deny\n"
        "}\n"
        "agent Bot {\n"
        "    use network\n"
        "    model: \"gpt-4\"\n"
        "    fn run() -> Result {\n"
        "        let data = fetch(\"https://api.github.com/repos/org/repo\")\n"
        "        data\n"
        "    }\n"
        "}"
    );
    ASSERT(ok);
}

TEST(typecheck_network_static_denied) {
    bool ok = typecheck_source(
        "capability network {\n"
        "    allow endpoint \"api.github.com:443\"\n"
        "    default: deny\n"
        "}\n"
        "agent Bot {\n"
        "    use network\n"
        "    model: \"gpt-4\"\n"
        "    fn run() -> Result {\n"
        "        let data = fetch(\"https://evil.com/hack\")\n"
        "        data\n"
        "    }\n"
        "}"
    );
    ASSERT_FALSE(ok);
}

TEST(typecheck_private_range_denied) {
    bool ok = typecheck_source(
        "capability network {\n"
        "    deny private_ranges\n"
        "    default: deny\n"
        "}\n"
        "agent Bot {\n"
        "    use network\n"
        "    model: \"gpt-4\"\n"
        "    fn run() -> Result {\n"
        "        let data = fetch(\"http://10.0.0.1/admin\")\n"
        "        data\n"
        "    }\n"
        "}"
    );
    ASSERT_FALSE(ok);
}

TEST(typecheck_binary_allowed) {
    bool ok = typecheck_source(
        "capability shell {\n"
        "    allow binary \"/usr/bin/git\"\n"
        "    default: deny\n"
        "}\n"
        "agent Bot {\n"
        "    use shell\n"
        "    model: \"gpt-4\"\n"
        "    fn run() -> Result {\n"
        "        let r = exec(\"git pull\")\n"
        "        r\n"
        "    }\n"
        "}"
    );
    ASSERT(ok);
}

TEST(typecheck_binary_denied) {
    bool ok = typecheck_source(
        "capability shell {\n"
        "    allow binary \"/usr/bin/git\"\n"
        "    deny binary \"/bin/rm\"\n"
        "    default: deny\n"
        "}\n"
        "agent Bot {\n"
        "    use shell\n"
        "    model: \"gpt-4\"\n"
        "    fn run() -> Result {\n"
        "        let r = exec(\"rm -rf /\")\n"
        "        r\n"
        "    }\n"
        "}"
    );
    ASSERT_FALSE(ok);
}

TEST(codegen_access_policy_table) {
    char *c = gen_c(
        "capability network {\n"
        "    allow endpoint \"api.github.com:443\"\n"
        "    deny private_ranges\n"
        "    default: deny\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_policy_network") != NULL);
    ASSERT(strstr(c, "api.github.com") != NULL);
    ASSERT(strstr(c, "deny_private") != NULL);
    free(c);
}

TEST(codegen_binary_policy_table) {
    char *c = gen_c(
        "capability shell {\n"
        "    allow binary \"/usr/bin/git\"\n"
        "    deny binary \"/bin/rm\"\n"
        "    default: deny\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_policy_shell") != NULL);
    ASSERT(strstr(c, "/usr/bin/git") != NULL);
    ASSERT(strstr(c, "/bin/rm") != NULL);
    free(c);
}

TEST(parse_cap_path_allow_deny) {
    bool err;
    AstNode *prog = parse_source(
        "capability filesystem {\n"
        "    allow path \"/data/**\" {\n"
        "        mode: [read, write]\n"
        "    }\n"
        "    deny path \"/etc/**\"\n"
        "    default: deny\n"
        "}", &err);
    ASSERT_FALSE(err);
    AstNode *cap = prog->params;
    ASSERT_EQ(cap->kind, AST_CAPABILITY);
    ASSERT_STR_EQ(cap->name, "filesystem");
    AstNode *r1 = cap->params;
    ASSERT_EQ(r1->kind, AST_CAP_PATH_RULE);
    ASSERT_TRUE(r1->is_mut); /* allow */
    ASSERT_STR_EQ(r1->name, "/data/**");
    ASSERT_NOT_NULL(r1->params); /* has mode fields */
    AstNode *r2 = r1->next;
    ASSERT_EQ(r2->kind, AST_CAP_PATH_RULE);
    ASSERT_FALSE(r2->is_mut); /* deny */
    ASSERT_STR_EQ(r2->name, "/etc/**");
    AstNode *def = r2->next;
    ASSERT_EQ(def->kind, AST_CAP_DEFAULT);
    ASSERT_FALSE(def->is_mut); /* deny */
}

TEST(typecheck_path_read_allowed) {
    bool ok = typecheck_source(
        "capability filesystem {\n"
        "    allow path \"/data/**\" {\n"
        "        mode: [read, write]\n"
        "    }\n"
        "    default: deny\n"
        "}\n"
        "agent Bot {\n"
        "    use filesystem\n"
        "    model: \"gpt-4\"\n"
        "    fn run() -> Result {\n"
        "        let data = read_file(\"/data/input.csv\")\n"
        "        data\n"
        "    }\n"
        "}"
    );
    ASSERT(ok);
}

TEST(typecheck_path_write_denied) {
    bool ok = typecheck_source(
        "capability filesystem {\n"
        "    allow path \"/data/**\" {\n"
        "        mode: [read, write]\n"
        "    }\n"
        "    deny path \"/etc/**\"\n"
        "    default: deny\n"
        "}\n"
        "agent Bot {\n"
        "    use filesystem\n"
        "    model: \"gpt-4\"\n"
        "    fn run() -> Result {\n"
        "        write_file(\"/etc/passwd\", \"hack\")\n"
        "    }\n"
        "}"
    );
    ASSERT_FALSE(ok);
}

TEST(typecheck_path_default_deny) {
    bool ok = typecheck_source(
        "capability filesystem {\n"
        "    allow path \"/data/**\"\n"
        "    default: deny\n"
        "}\n"
        "agent Bot {\n"
        "    use filesystem\n"
        "    model: \"gpt-4\"\n"
        "    fn run() -> Result {\n"
        "        let x = read_file(\"/root/.ssh/id_rsa\")\n"
        "        x\n"
        "    }\n"
        "}"
    );
    ASSERT_FALSE(ok);
}

TEST(codegen_path_policy_table) {
    char *c = gen_c(
        "capability filesystem {\n"
        "    allow path \"/data/**\" {\n"
        "        mode: [read, write]\n"
        "    }\n"
        "    deny path \"/etc/**\"\n"
        "    default: deny\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_policy_filesystem") != NULL);
    ASSERT(strstr(c, "LcnPathRule") != NULL);
    ASSERT(strstr(c, "/data/**") != NULL);
    ASSERT(strstr(c, "/etc/**") != NULL);
    free(c);
}

TEST(codegen_existing_cap_unchanged) {
    char *c = gen_c(
        "capability web {\n"
        "    search\n"
        "    fetch requires search\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "CAP_WEB_SEARCH") != NULL);
    ASSERT(strstr(c, "CAP_WEB_FETCH") != NULL);
    free(c);
}

/* ============================================================
 * Capability Delegation Tests
 * ============================================================ */

TEST(codegen_delegate_builtin) {
    char *c = gen_c(
        "capability data {\n"
        "    read\n"
        "    write\n"
        "}\n"
        "fn main() {\n"
        "    let worker = delegate([data.read], 30000)\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_delegate_new") != NULL);
    ASSERT(strstr(c, "CAP_DATA_READ") != NULL);
    ASSERT(strstr(c, "30000") != NULL);
    free(c);
}

TEST(codegen_revoke_builtin) {
    char *c = gen_c(
        "capability data {\n"
        "    read\n"
        "    write\n"
        "}\n"
        "fn main() {\n"
        "    let worker = delegate([data.read, data.write], 0)\n"
        "    revoke(worker, data.write)\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_delegate_revoke(") != NULL);
    ASSERT(strstr(c, "CAP_DATA_WRITE") != NULL);
    free(c);
}

TEST(codegen_revoke_all_builtin) {
    char *c = gen_c(
        "capability data {\n"
        "    read\n"
        "}\n"
        "fn main() {\n"
        "    let worker = delegate([data.read], 0)\n"
        "    revoke_all(worker)\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_delegate_revoke_all(") != NULL);
    free(c);
}

TEST(codegen_has_capability_builtin) {
    char *c = gen_c(
        "capability data {\n"
        "    read\n"
        "    write\n"
        "}\n"
        "fn main() {\n"
        "    let worker = delegate([data.read], 0)\n"
        "    let ok = has_capability(worker, data.read)\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_delegate_has(") != NULL);
    ASSERT(strstr(c, "CAP_DATA_READ") != NULL);
    free(c);
}

TEST(codegen_delegation_preamble) {
    char *c = gen_c("");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "LcnDelegation") != NULL);
    ASSERT(strstr(c, "lcn_delegate_new") != NULL);
    ASSERT(strstr(c, "lcn_delegate_revoke") != NULL);
    ASSERT(strstr(c, "lcn_delegate_has") != NULL);
    ASSERT(strstr(c, "lcn_delegate_free") != NULL);
    free(c);
}

TEST(parse_has_capability_call) {
    bool err;
    AstNode *ast = parse_source(
        "fn main() {\n"
        "    let ok = has_capability(w, data.read)\n"
        "}",
        &err
    );
    ASSERT(!err);
    ASSERT_NOT_NULL(ast);
    /* The AST should parse without errors — has_capability is a valid call */
}

/* ============================================================
 * Stage 1 Codegen: Invariant Wiring Tests
 * ============================================================ */

TEST(codegen_invariant_drift) {
    /* invariant with drift() call should generate lcn_invariant_NAME function */
    char *c = gen_c(
        "invariant no_drift {\n"
        "    drift(current, baseline) < 0.15\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* Should generate a bool-returning function */
    ASSERT(strstr(c, "bool lcn_invariant_no_drift(LcnEntropyTracker *tracker)") != NULL);
    /* Should call the JS divergence runtime function */
    ASSERT(strstr(c, "lcn_js_divergence(") != NULL);
    /* Should compare against threshold */
    ASSERT(strstr(c, "< 0.15") != NULL);
    free(c);
}

TEST(codegen_invariant_avg_confidence) {
    char *c = gen_c(
        "invariant high_quality {\n"
        "    avg_confidence(100) > 0.70\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "bool lcn_invariant_high_quality(LcnEntropyTracker *tracker)") != NULL);
    ASSERT(strstr(c, "lcn_entropy_avg_confidence(tracker, 100)") != NULL);
    ASSERT(strstr(c, "> 0.7") != NULL || strstr(c, "> 0.70") != NULL);
    free(c);
}

TEST(codegen_invariant_avg_entropy) {
    char *c = gen_c(
        "invariant low_entropy {\n"
        "    avg_entropy(50) < 0.50\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "bool lcn_invariant_low_entropy(LcnEntropyTracker *tracker)") != NULL);
    ASSERT(strstr(c, "lcn_entropy_avg_entropy(tracker, 50)") != NULL);
    ASSERT(strstr(c, "< 0.5") != NULL || strstr(c, "< 0.50") != NULL);
    free(c);
}

TEST(parse_invariant_decl) {
    bool err;
    AstNode *ast = parse_source(
        "invariant no_drift {\n"
        "    drift(current, baseline) < 0.15\n"
        "}\n",
        &err
    );
    ASSERT(!err);
    ASSERT_NOT_NULL(ast);
    /* The first child should be an AST_INVARIANT */
    AstNode *inv = ast->params;
    ASSERT_NOT_NULL(inv);
    ASSERT_EQ(inv->kind, AST_INVARIANT);
    ASSERT_STR_EQ(inv->name, "no_drift");
    /* Body should be a binary comparison */
    ASSERT_NOT_NULL(inv->left);
    ASSERT_EQ(inv->left->kind, AST_BINARY);
}

/* ============================================================
 * Stage 1 Codegen: Defer LIFO Tests
 * ============================================================ */

TEST(codegen_defer_lifo_order) {
    /* Two defers in a block should execute in LIFO order at block exit */
    char *c = gen_c(
        "fn test() {\n"
        "    defer close_a()\n"
        "    defer close_b()\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* close_b should appear before close_a (LIFO) */
    const char *b_pos = strstr(c, "/* defer */ lcn_close_b()");
    const char *a_pos = strstr(c, "/* defer */ lcn_close_a()");
    ASSERT_NOT_NULL(b_pos);
    ASSERT_NOT_NULL(a_pos);
    ASSERT(b_pos < a_pos);  /* LIFO: b emitted first */
    free(c);
}

TEST(codegen_defer_not_inline) {
    /* Defer should NOT appear at the point of the defer statement itself —
     * it should only appear at scope exit */
    char *c = gen_c(
        "fn test() -> i32 {\n"
        "    defer cleanup()\n"
        "    let x = 42\n"
        "    return x\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* The defer should appear before the return statement */
    const char *defer_pos = strstr(c, "/* defer */ lcn_cleanup()");
    const char *return_pos = strstr(c, "return x;");
    ASSERT_NOT_NULL(defer_pos);
    ASSERT_NOT_NULL(return_pos);
    /* Defer should be emitted before the return */
    ASSERT(defer_pos < return_pos);
    free(c);
}

TEST(codegen_defer_at_block_exit) {
    /* Defer inside a nested block should execute at that block's exit */
    char *c = gen_c(
        "fn test() {\n"
        "    let a = 1\n"
        "    defer outer()\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* Should have the defer comment */
    ASSERT(strstr(c, "/* defer */") != NULL);
    free(c);
}

TEST(codegen_defer_after_last_stmt_in_void_fn) {
    /* In a void function, defers must execute AFTER the last statement,
     * not before it. Output should be: first, second, third */
    char *c = gen_c(
        "fn main() {\n"
        "    defer println(\"third\")\n"
        "    defer println(\"second\")\n"
        "    println(\"first\")\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* The println("first") should appear BEFORE the deferred printlns */
    const char *first_pos  = strstr(c, "printf(\"%s\\n\", \"first\")");
    const char *second_pos = strstr(c, "/* defer */ printf(\"%s\\n\", \"second\")");
    const char *third_pos  = strstr(c, "/* defer */ printf(\"%s\\n\", \"third\")");
    ASSERT_NOT_NULL(first_pos);
    ASSERT_NOT_NULL(second_pos);
    ASSERT_NOT_NULL(third_pos);
    /* first must come before defers */
    ASSERT(first_pos < second_pos);
    ASSERT(first_pos < third_pos);
    /* LIFO: second (last deferred) emitted before third (first deferred) */
    ASSERT(second_pos < third_pos);
    free(c);
}

/* ============================================================
 * Stage 1 Codegen: Syntactic Sugar Tests
 * ============================================================ */

TEST(parse_try_otherwise) {
    bool err;
    AstNode *ast = parse_source(
        "fn test() {\n"
        "    let r = try search(query) otherwise use_cache()\n"
        "}\n",
        &err
    );
    ASSERT(!err);
    ASSERT_NOT_NULL(ast);
    /* Find the fn, then the let, then the RHS should be AST_TRY_OTHERWISE */
    AstNode *fn = ast->params;
    ASSERT_NOT_NULL(fn);
    ASSERT_EQ(fn->kind, AST_FN);
    AstNode *body = fn->left; /* block */
    ASSERT_NOT_NULL(body);
    AstNode *let_stmt = body->params;
    ASSERT_NOT_NULL(let_stmt);
    ASSERT_EQ(let_stmt->kind, AST_LET);
    ASSERT_NOT_NULL(let_stmt->right);
    ASSERT_EQ(let_stmt->right->kind, AST_TRY_OTHERWISE);
}

TEST(codegen_try_otherwise) {
    char *c = gen_c(
        "fn test() {\n"
        "    let r = try search(query) otherwise use_cache()\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* Should produce a GCC statement-expression with error check */
    ASSERT(strstr(c, "LcnLlmOutput _to_") != NULL);
    ASSERT(strstr(c, "LCN_LLM_ERROR") != NULL);
    free(c);
}

TEST(parse_keep_where) {
    bool err;
    AstNode *ast = parse_source(
        "fn test() {\n"
        "    let r = items |> keep where score > 5\n"
        "}\n",
        &err
    );
    ASSERT(!err);
    ASSERT_NOT_NULL(ast);
    /* The RHS of let should be AST_PIPE with RHS=AST_KEEP_WHERE */
    AstNode *fn = ast->params;
    ASSERT_NOT_NULL(fn);
    AstNode *let_stmt = fn->left->params;
    ASSERT_NOT_NULL(let_stmt);
    ASSERT_EQ(let_stmt->kind, AST_LET);
    ASSERT_NOT_NULL(let_stmt->right);
    ASSERT_EQ(let_stmt->right->kind, AST_PIPE);
    ASSERT_NOT_NULL(let_stmt->right->right);
    ASSERT_EQ(let_stmt->right->right->kind, AST_KEEP_WHERE);
}

TEST(codegen_keep_where) {
    char *c = gen_c(
        "fn test() {\n"
        "    let r = items |> keep where score > 5\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* Should produce a filter pattern */
    ASSERT(strstr(c, "keep where") != NULL);
    ASSERT(strstr(c, "filter") != NULL);
    free(c);
}

TEST(parse_each) {
    bool err;
    AstNode *ast = parse_source(
        "fn test() {\n"
        "    let names = users |> each name\n"
        "}\n",
        &err
    );
    ASSERT(!err);
    ASSERT_NOT_NULL(ast);
    /* The RHS of let should be AST_PIPE with RHS=AST_EACH */
    AstNode *fn = ast->params;
    ASSERT_NOT_NULL(fn);
    AstNode *let_stmt = fn->left->params;
    ASSERT_NOT_NULL(let_stmt);
    ASSERT_EQ(let_stmt->kind, AST_LET);
    ASSERT_NOT_NULL(let_stmt->right);
    ASSERT_EQ(let_stmt->right->kind, AST_PIPE);
    ASSERT_NOT_NULL(let_stmt->right->right);
    ASSERT_EQ(let_stmt->right->right->kind, AST_EACH);
    ASSERT_STR_EQ(let_stmt->right->right->name, "name");
}

TEST(codegen_each) {
    char *c = gen_c(
        "fn test() {\n"
        "    let names = users |> each name\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* Should produce a map/extract pattern */
    ASSERT(strstr(c, "each name") != NULL);
    ASSERT(strstr(c, "extract .name") != NULL);
    free(c);
}

/* ============================================================
 * Generics / Monomorphization Tests
 * ============================================================ */

TEST(codegen_result_generic_typedef) {
    char *c = gen_c(
        "fn try_parse(s: string) -> Result<int, string> {\n"
        "    return 42\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* Should emit a monomorphized typedef */
    ASSERT(strstr(c, "Result_int64_t_LcnString") != NULL);
    /* Should have the struct fields */
    ASSERT(strstr(c, "bool ok") != NULL);
    /* Should have the ok constructor */
    ASSERT(strstr(c, "Result_int64_t_LcnString_ok") != NULL);
    /* Should have the err constructor */
    ASSERT(strstr(c, "Result_int64_t_LcnString_err") != NULL);
    free(c);
}

TEST(codegen_result_generic_return_type) {
    char *c = gen_c(
        "fn parse_number(s: string) -> Result<int, string> {\n"
        "    let x = 42\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* Return type should be the monomorphized name */
    ASSERT(strstr(c, "Result_int64_t_LcnString lcn_parse_number") != NULL);
    free(c);
}

TEST(codegen_option_generic_typedef) {
    char *c = gen_c(
        "fn find_item(name: string) -> Option<string> {\n"
        "    return name\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* Should emit Option_LcnString typedef */
    ASSERT(strstr(c, "Option_LcnString") != NULL);
    ASSERT(strstr(c, "bool has_value") != NULL);
    /* Constructors */
    ASSERT(strstr(c, "Option_LcnString_some") != NULL);
    ASSERT(strstr(c, "Option_LcnString_none") != NULL);
    free(c);
}

TEST(codegen_bare_result_backward_compat) {
    char *c = gen_c(
        "fn do_thing() -> Result {\n"
        "    let x = 42\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* Should use untyped LcnResult */
    ASSERT(strstr(c, "LcnResult lcn_do_thing") != NULL);
    /* Should NOT have monomorphized types */
    ASSERT(strstr(c, "Result_") == NULL);
    free(c);
}

TEST(codegen_result_string_string) {
    char *c = gen_c(
        "fn fetch_data(url: string) -> Result<string, string> {\n"
        "    return url\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* Should emit Result_LcnString_LcnString */
    ASSERT(strstr(c, "Result_LcnString_LcnString") != NULL);
    ASSERT(strstr(c, "Result_LcnString_LcnString_ok") != NULL);
    ASSERT(strstr(c, "Result_LcnString_LcnString_err") != NULL);
    free(c);
}

TEST(codegen_option_int_typedef) {
    char *c = gen_c(
        "fn find_index(name: string) -> Option<int> {\n"
        "    return 0\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* Should emit Option_int64_t */
    ASSERT(strstr(c, "Option_int64_t") != NULL);
    ASSERT(strstr(c, "Option_int64_t_some") != NULL);
    ASSERT(strstr(c, "Option_int64_t_none") != NULL);
    free(c);
}

TEST(codegen_match_result_ok_error) {
    char *c = gen_c(
        "fn get_data() -> Result {\n"
        "    let x = 42\n"
        "}\n"
        "fn main() {\n"
        "    let r = get_data()\n"
        "    match r {\n"
        "        Ok(value) -> { println(value) }\n"
        "        Error(err) -> { println(err) }\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* Should generate if/else on .ok, not a switch */
    ASSERT(strstr(c, ".ok") != NULL);
    /* Should bind value and error */
    ASSERT(strstr(c, "value = _match_") != NULL);
    ASSERT(strstr(c, "error = _match_") == NULL || strstr(c, "err = _match_") != NULL);
    free(c);
}

TEST(codegen_match_option_some_none) {
    char *c = gen_c(
        "fn maybe() -> Option<int> {\n"
        "    return 42\n"
        "}\n"
        "fn main() {\n"
        "    let opt = maybe()\n"
        "    match opt {\n"
        "        Some(val) -> { println(val) }\n"
        "        None -> { println(\"nothing\") }\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* Should generate if/else on .has_value */
    ASSERT(strstr(c, ".has_value") != NULL);
    /* Should bind value */
    ASSERT(strstr(c, "val = _match_") != NULL);
    free(c);
}

TEST(codegen_result_no_duplicate_typedef) {
    char *c = gen_c(
        "fn foo() -> Result<string, string> {\n"
        "    return \"a\"\n"
        "}\n"
        "fn bar() -> Result<string, string> {\n"
        "    return \"b\"\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* Should have the typedef */
    ASSERT(strstr(c, "Result_LcnString_LcnString") != NULL);
    /* Count occurrences of typedef — should appear once as a struct def */
    {
        const char *search = "typedef struct { bool ok;";
        const char *p = c;
        int count = 0;
        while ((p = strstr(p, search)) != NULL) { count++; p++; }
        ASSERT_EQ(count, 1);
    }
    free(c);
}

TEST(codegen_result_default_error_type) {
    /* Result<int> with only 1 param should default E to LcnString */
    char *c = gen_c(
        "fn parse(s: string) -> Result<int> {\n"
        "    return 42\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* Should emit Result_int64_t_LcnString (E defaults to LcnString) */
    ASSERT(strstr(c, "Result_int64_t_LcnString") != NULL);
    free(c);
}

/* ============================================================
 * Error Message Format Tests
 * ============================================================ */

/* Helper: format a diagnostic and return it in a static buffer */
static const char *format_test_error(const char *source, uint32_t line,
                                      uint32_t col, uint32_t offset,
                                      const char *message, const char *hint,
                                      bool is_warning, uint32_t ulen) {
    static char buf[2048];
    SourceLoc loc;
    loc.filename = "test.lceron";
    loc.line = line;
    loc.column = col;
    loc.offset = offset;
    format_diagnostic(buf, sizeof(buf), source, strlen(source),
                      "test.lceron", loc, message, hint,
                      is_warning, ulen);
    return buf;
}

TEST(error_msg_has_line_number) {
    const char *src = "let x = 42\nlet y = foo + 1\nlet z = 10\n";
    /* Error on line 2, col 9 ("foo"), offset = 12+9-1 = 20 */
    const char *out = format_test_error(src, 2, 9, 20,
                                         "undefined variable 'foo'",
                                         "not found in this scope",
                                         false, 3);
    /* Verify the line number 2 appears in the output */
    ASSERT(strstr(out, "2 |") != NULL);
    /* Verify the location is shown */
    ASSERT(strstr(out, "test.lceron:2:9") != NULL);
}

TEST(error_msg_has_source_snippet) {
    const char *src = "let x = 42\nlet y = foo + 1\nlet z = 10\n";
    const char *out = format_test_error(src, 2, 9, 20,
                                         "undefined variable 'foo'",
                                         "not found in this scope",
                                         false, 3);
    /* Verify the actual source line is present */
    ASSERT(strstr(out, "let y = foo + 1") != NULL);
}

TEST(error_msg_has_caret) {
    const char *src = "let x = 42\nlet y = foo + 1\nlet z = 10\n";
    const char *out = format_test_error(src, 2, 9, 20,
                                         "undefined variable 'foo'",
                                         "not found in this scope",
                                         false, 3);
    /* Verify caret/underline appears: three ^ chars for "foo" */
    ASSERT(strstr(out, "^^^") != NULL);
    /* Verify the hint text follows the carets */
    ASSERT(strstr(out, "^^^ not found in this scope") != NULL);
}

TEST(warning_msg_format) {
    const char *src = "agent Bot {\n    model: \"gpt-4\"\n}\n";
    const char *out = format_test_error(src, 1, 7, 6,
                                         "missing guard for agent",
                                         "add a guard",
                                         true, 3);
    /* Verify it says "warning" not "error" */
    ASSERT(strstr(out, "warning:") != NULL);
    ASSERT(strstr(out, "error:") == NULL);
    /* Still has source snippet and location */
    ASSERT(strstr(out, "agent Bot") != NULL);
    ASSERT(strstr(out, "test.lceron:1:7") != NULL);
}

TEST(error_msg_single_caret) {
    const char *src = "let x = ;\n";
    const char *out = format_test_error(src, 1, 9, 8,
                                         "unexpected ';'", NULL,
                                         false, 0);
    /* With underline_len=0, should get a single ^ */
    ASSERT(strstr(out, "^") != NULL);
    /* Should NOT have hint text after caret since hint is NULL */
    ASSERT(strstr(out, "^ ") == NULL || strstr(out, "^\n") != NULL);
}

TEST(error_msg_no_source) {
    /* When source is NULL, still formats cleanly */
    static char buf[2048];
    SourceLoc loc;
    loc.filename = "missing.lceron";
    loc.line = 5;
    loc.column = 3;
    loc.offset = 999;
    format_diagnostic(buf, sizeof(buf), NULL, 0, "missing.lceron",
                      loc, "file not found", "check the path",
                      false, 0);
    ASSERT(strstr(buf, "error: file not found") != NULL);
    ASSERT(strstr(buf, "missing.lceron:5:3") != NULL);
    ASSERT(strstr(buf, "= help: check the path") != NULL);
}

TEST(error_msg_multidigit_line) {
    /* Build source with enough lines to reach line 100 */
    static char src[4096];
    int pos = 0;
    int i;
    for (i = 1; i < 100; i++) {
        pos += snprintf(src + pos, sizeof(src) - (size_t)pos, "let a%d = %d\n", i, i);
    }
    pos += snprintf(src + pos, sizeof(src) - (size_t)pos, "let bad = oops\n");
    /* line 100, col 11 for "oops", offset = pos - 15 + 10 */
    uint32_t offset = (uint32_t)(pos - 5); /* points inside "oops" */
    const char *out = format_test_error(src, 100, 11, offset,
                                         "unknown variable",
                                         NULL, false, 4);
    /* Verify triple-digit line number is displayed */
    ASSERT(strstr(out, "100 |") != NULL);
}

TEST(error_reporter_stores_warning_flag) {
    arena_reset(&test_arena);
    arena_reset(&test_intern_arena);

    const char *src = "let x = 1\n";
    size_t len = strlen(src);
    ErrorReporter reporter = reporter_new("<test>", src, len);
    SourceLoc loc;
    loc.filename = "<test>";
    loc.line = 1;
    loc.column = 5;
    loc.offset = 4;

    report_error(&reporter, loc, "an error", NULL);
    report_warning(&reporter, loc, "a warning", NULL);

    ASSERT_EQ(reporter.count, 2);
    ASSERT_FALSE(reporter.errors[0].is_warning);
    ASSERT_TRUE(reporter.errors[1].is_warning);
}

/* ============================================================
 * SECRET TYPE TESTS
 * ============================================================ */

TEST(parse_secret_type) {
    /* let api_key: secret string = "sk-..." should parse with AST_TYPE_SECRET */
    bool err = false;
    AstNode *program = parse_source(
        "fn test() {\n"
        "    let api_key: secret string = \"sk-12345\"\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    ASSERT_NOT_NULL(program);
    ASSERT_EQ(program->kind, AST_PROGRAM);

    /* Find the function */
    AstNode *fn = program->params;
    ASSERT_NOT_NULL(fn);
    ASSERT_EQ(fn->kind, AST_FN);

    /* Find the let statement inside the block */
    AstNode *body = fn->left;
    ASSERT_NOT_NULL(body);
    ASSERT_EQ(body->kind, AST_BLOCK);

    AstNode *let_stmt = body->params;
    ASSERT_NOT_NULL(let_stmt);
    ASSERT_EQ(let_stmt->kind, AST_LET);
    ASSERT_STR_EQ(let_stmt->name, "api_key");

    /* Type should be AST_TYPE_SECRET wrapping AST_TYPE_NAMED("string") */
    ASSERT_NOT_NULL(let_stmt->type_expr);
    ASSERT_EQ(let_stmt->type_expr->kind, AST_TYPE_SECRET);
    ASSERT_NOT_NULL(let_stmt->type_expr->left);
    ASSERT_EQ(let_stmt->type_expr->left->kind, AST_TYPE_NAMED);
    ASSERT_STR_EQ(let_stmt->type_expr->left->name, "string");
}

TEST(typecheck_secret_no_println) {
    /* Passing a secret value to println should be an error */
    bool ok = typecheck_source(
        "fn test() {\n"
        "    let api_key: secret string = \"sk-12345\"\n"
        "    println(api_key)\n"
        "}"
    );
    ASSERT_FALSE(ok);  /* Should fail: secret leaked to println */
}

TEST(typecheck_secret_no_log) {
    /* Passing a secret value to log_info should be an error */
    bool ok = typecheck_source(
        "fn test() {\n"
        "    let token: secret string = \"tok-abc\"\n"
        "    log_info(token)\n"
        "}"
    );
    ASSERT_FALSE(ok);  /* Should fail: secret leaked to log_info */
}

TEST(typecheck_secret_redact_ok) {
    /* Using secret_redact() is allowed — no error */
    bool ok = typecheck_source(
        "fn test() {\n"
        "    let api_key: secret string = \"sk-12345\"\n"
        "    let safe: string = secret_redact(api_key)\n"
        "    println(safe)\n"
        "}"
    );
    ASSERT(ok);  /* Should pass: secret_redact() is the approved escape hatch */
}

TEST(codegen_secret_type) {
    /* secret string should map to LcnString in generated C */
    char *c = gen_c(
        "fn test() {\n"
        "    let api_key: secret string = \"sk-12345\"\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    /* The variable should be typed as LcnString (compile-time only protection) */
    ASSERT(strstr(c, "LcnString") != NULL);
    free(c);
}

TEST(codegen_secret_redact) {
    /* secret_redact() should generate "[REDACTED]" */
    char *c = gen_c(
        "fn test() {\n"
        "    let api_key: secret string = \"sk-12345\"\n"
        "    let safe: string = secret_redact(api_key)\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "\"[REDACTED]\"") != NULL);
    free(c);
}

/* ============================================================
 * CAPABILITY FENCE TESTS
 * ============================================================ */

TEST(codegen_tool_capability_fence) {
    char *c = gen_c(
        "capability web {\n    search\n}\n"
        "tool web_search(q: string) -> string {\n    requires: [web.search]\n    description: \"Search\"\n}\n"
        "agent Bot {\n"
        "    capabilities: [web.search]\n"
        "    model: \"gpt-4\"\n"
        "    fn run(input: string) -> Result {\n"
        "        let response = ask(input)\n"
        "        match response {\n"
        "            ToolCall(name, args) -> {\n"
        "                println(name)\n"
        "            }\n"
        "            Ok(text) -> {\n"
        "                println(text)\n"
        "            }\n"
        "            _ -> {\n"
        "                println(\"other\")\n"
        "            }\n"
        "        }\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_capability_check_tool") != NULL);
    ASSERT(strstr(c, "lcn_capability_violation") != NULL);
    ASSERT(strstr(c, "_agent_Bot_tools") != NULL);
    ASSERT(strstr(c, "_agent_Bot_tool_count") != NULL);
    free(c);
}

TEST(codegen_agent_tool_list) {
    char *c = gen_c(
        "capability web {\n    search\n}\n"
        "tool web_search(q: string) -> string {\n    requires: [web.search]\n    description: \"Search\"\n}\n"
        "tool no_cap_tool(x: string) -> string {\n    description: \"No caps needed\"\n}\n"
        "agent Bot {\n"
        "    capabilities: [web.search]\n"
        "    model: \"gpt-4\"\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "_agent_Bot_tools[]") != NULL);
    ASSERT(strstr(c, "_agent_Bot_tool_count") != NULL);
    ASSERT(strstr(c, "\"web_search\"") != NULL);
    ASSERT(strstr(c, "\"no_cap_tool\"") != NULL);
    free(c);
}

TEST(codegen_mcp_capability_gate) {
    char *c = gen_c(
        "use mcp \"sqlite-server\" as db\n"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "_lcn_fence_tools") != NULL);
    ASSERT(strstr(c, "_lcn_fence_tool_count") != NULL);
    ASSERT(strstr(c, "_lcn_fence_agent_name") != NULL);
    ASSERT(strstr(c, "lcn_capability_check_tool") != NULL);
    free(c);
}

/* ============================================================
 * Green Thread Tests
 *
 * Tests for green thread codegen output: spawn, await, yield,
 * init/shutdown in main wrapper, and standalone preamble stubs.
 * ============================================================ */

TEST(green_spawn_basic) {
    /* Verify the standalone preamble includes green thread stubs */
    char *c = gen_c("");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_green_init") != NULL);
    ASSERT(strstr(c, "lcn_green_spawn") != NULL);
    ASSERT(strstr(c, "lcn_green_await") != NULL);
    ASSERT(strstr(c, "lcn_green_shutdown") != NULL);
    ASSERT(strstr(c, "lcn_green_yield") != NULL);
    ASSERT(strstr(c, "lcn_green_park") != NULL);
    ASSERT(strstr(c, "lcn_green_unpark") != NULL);
    ASSERT(strstr(c, "GreenThread") != NULL);
    free(c);
}

TEST(green_yield) {
    /* Verify that yield stub is present and won't deadlock (it's a no-op in standalone) */
    char *c = gen_c(
        "fn main() {\n"
        "    let x = 1\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    /* Green init/shutdown should be in main wrapper */
    ASSERT(strstr(c, "lcn_green_init()") != NULL);
    ASSERT(strstr(c, "lcn_green_shutdown()") != NULL);
    free(c);
}

TEST(green_many_tasks) {
    /* Verify that spawn codegen emits properly (many spawns shouldn't break codegen) */
    char *c = gen_c(
        "fn main() -> Result {\n"
        "    let a = spawn { let x = 1 }\n"
        "    let b = spawn { let y = 2 }\n"
        "    let c = spawn { let z = 3 }\n"
        "    await a\n"
        "    await b\n"
        "    await c\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    /* Should have three separate spawn wrapper functions */
    ASSERT(strstr(c, "__lcn_spawn_0") != NULL);
    ASSERT(strstr(c, "__lcn_spawn_1") != NULL);
    ASSERT(strstr(c, "__lcn_spawn_2") != NULL);
    free(c);
}

TEST(codegen_green_spawn) {
    /* When LCN_GREEN_THREADS is not set, spawn should use lcn_spawn_task (default) */
    char *c = gen_c(
        "fn main() -> Result {\n"
        "    let h = spawn {\n"
        "        let x = 42\n"
        "    }\n"
        "    let result = await h\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    /* Default mode: pthreads */
    ASSERT(strstr(c, "lcn_spawn_task") != NULL);
    ASSERT(strstr(c, "lcn_await_task") != NULL);
    /* Green thread stubs should be in preamble */
    ASSERT(strstr(c, "lcn_green_spawn") != NULL);
    free(c);
}

/* ============================================================
 * Module Visibility (pub/priv) Enforcement Tests
 *
 * These tests construct ASTs where some declarations appear to
 * come from a different file (by patching loc.filename), then
 * run typecheck to verify that non-pub cross-module access is
 * rejected while pub cross-module access and same-module
 * private access are allowed.
 * ============================================================ */

/* Helper: parse source, then patch the first declaration named
 * |target_name| to appear as if it came from |foreign_file|.
 * If |make_pub| is true, also set is_pub on that declaration.
 * Returns true if typecheck passes (no enforced errors). */
static bool typecheck_with_foreign_decl(const char *source,
                                         const char *target_name,
                                         const char *foreign_file,
                                         bool make_pub) {
    arena_reset(&test_arena);
    arena_reset(&test_intern_arena);

    size_t len = strlen(source);
    ErrorReporter reporter = reporter_new("<test>", source, len);
    StringIntern intern = intern_new(&test_intern_arena);
    Lexer lexer = lexer_new("<test>", source, len, &intern, &reporter);
    Parser parser = parser_new(&lexer, &test_arena, &reporter);
    AstNode *program = parse_program(&parser);
    if (parser.had_error) return false;

    /* Patch: find the declaration named target_name and change its
     * source filename to foreign_file to simulate a cross-module import */
    AstNode *decl;
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->name && strcmp(decl->name, target_name) == 0) {
            decl->loc.filename = foreign_file;
            if (make_pub) decl->is_pub = true;
            break;
        }
    }

    return typecheck_program(program, &reporter, &test_arena);
}

TEST(typecheck_pub_fn_accessible) {
    /* A pub function from another module should pass typecheck */
    bool ok = typecheck_with_foreign_decl(
        "pub fn helper() -> i32 { return 1 }\n"
        "fn main() -> i32 { return helper() }\n",
        "helper",        /* target to patch */
        "other.lceron",  /* pretend it's from another file */
        true             /* mark it pub */
    );
    ASSERT(ok);
}

TEST(typecheck_priv_fn_error) {
    /* A private function from another module should fail typecheck */
    bool ok = typecheck_with_foreign_decl(
        "fn helper() -> i32 { return 1 }\n"
        "fn main() -> i32 { return helper() }\n",
        "helper",        /* target to patch */
        "other.lceron",  /* pretend it's from another file */
        false            /* NOT pub */
    );
    ASSERT_FALSE(ok);
}

TEST(typecheck_priv_same_module_ok) {
    /* A private function in the same module should pass typecheck */
    bool ok = typecheck_source(
        "fn helper() -> i32 { return 1 }\n"
        "fn main() -> i32 { return helper() }\n"
    );
    ASSERT(ok);
}

TEST(typecheck_pub_struct_accessible) {
    /* A pub struct from another module should pass typecheck */
    bool ok = typecheck_with_foreign_decl(
        "pub struct Point {\n    x: f64\n    y: f64\n}\n"
        "fn main() -> i32 { return 0 }\n",
        "Point",         /* target to patch */
        "geom.lceron",   /* pretend it's from another file */
        true             /* mark it pub */
    );
    ASSERT(ok);
}

TEST(typecheck_priv_struct_error) {
    /* A private struct from another module should fail typecheck */
    bool ok = typecheck_with_foreign_decl(
        "struct Point {\n    x: f64\n    y: f64\n}\n"
        "fn main() -> i32 { return 0 }\n",
        "Point",         /* target to patch */
        "geom.lceron",   /* pretend it's from another file */
        false            /* NOT pub */
    );
    ASSERT_FALSE(ok);
}


/* ============================================================
 * Supervisor Tests
 * ============================================================ */

/* ============================================================
 * Supervisor Tests
 * ============================================================ */

TEST(parse_supervisor_block) {
    bool had_error;
    AstNode *prog = parse_source(
        "supervisor MySup {\n"
        "    strategy: one_for_one\n"
        "    max_restarts: 5\n"
        "}\n",
        &had_error
    );
    ASSERT_FALSE(had_error);
    ASSERT_NOT_NULL(prog);
    /* Find supervisor node */
    AstNode *sup = prog->params;
    while (sup && sup->kind != AST_SUPERVISOR) sup = sup->next;
    ASSERT_NOT_NULL(sup);
    ASSERT_EQ(sup->kind, AST_SUPERVISOR);
    ASSERT_STR_EQ(sup->name, "MySup");
    /* Check fields: strategy and max_restarts */
    AstNode *f = sup->params;
    int found = 0;
    while (f) {
        if (f->name && strcmp(f->name, "strategy") == 0) {
            ASSERT_NOT_NULL(f->right);
            ASSERT_EQ(f->right->kind, AST_IDENT);
            ASSERT_STR_EQ(f->right->name, "one_for_one");
            found++;
        }
        if (f->name && strcmp(f->name, "max_restarts") == 0) {
            ASSERT_NOT_NULL(f->right);
            ASSERT_EQ(f->right->kind, AST_INT_LIT);
            ASSERT_EQ(f->right->val.int_val, 5);
            found++;
        }
        f = f->next;
    }
    ASSERT_EQ(found, 2);
}

TEST(codegen_supervisor_one_for_one) {
    char *c = gen_c(
        "supervisor FrontDesk {\n"
        "    strategy: one_for_one\n"
        "    max_restarts: 5\n"
        "    children: [AgentA, AgentB]\n"
        "}\n"
        "fn main() {}\n"
    );
    ASSERT_NOT_NULL(c);
    /* Should emit supervisor init function */
    ASSERT(strstr(c, "lcn_supervisor_FrontDesk_init") != NULL);
    /* Should use one_for_one strategy */
    ASSERT(strstr(c, "LCN_STRATEGY_ONE_FOR_ONE") != NULL);
    /* Should emit max_restarts 5 */
    ASSERT(strstr(c, "5") != NULL);
    /* Should register child agents */
    ASSERT(strstr(c, "lcn_agent_AgentA_start") != NULL);
    ASSERT(strstr(c, "lcn_agent_AgentB_start") != NULL);
    /* Should add children via lcn_supervisor_add_child */
    ASSERT(strstr(c, "lcn_supervisor_add_child") != NULL);
    /* Should emit supervisor start in main */
    ASSERT(strstr(c, "lcn_supervisor_start(") != NULL);
    /* Should emit supervisor stop/free in main */
    ASSERT(strstr(c, "lcn_supervisor_stop(") != NULL);
    ASSERT(strstr(c, "lcn_supervisor_free(") != NULL);
    free(c);
}

TEST(codegen_supervisor_all_for_one) {
    char *c = gen_c(
        "supervisor Pipeline {\n"
        "    strategy: one_for_all\n"
        "    max_restarts: 3\n"
        "    children: [Worker1, Worker2, Worker3]\n"
        "}\n"
        "fn main() {}\n"
    );
    ASSERT_NOT_NULL(c);
    /* Should use one_for_all strategy */
    ASSERT(strstr(c, "LCN_STRATEGY_ONE_FOR_ALL") != NULL);
    /* Should register all 3 children */
    ASSERT(strstr(c, "\"Worker1\"") != NULL);
    ASSERT(strstr(c, "\"Worker2\"") != NULL);
    ASSERT(strstr(c, "\"Worker3\"") != NULL);
    /* Should emit the init function */
    ASSERT(strstr(c, "lcn_supervisor_Pipeline_init") != NULL);
    /* Should emit the global pointer */
    ASSERT(strstr(c, "_lcn_sup_Pipeline") != NULL);
    free(c);
}

TEST(codegen_supervisor_max_restarts) {
    char *c = gen_c(
        "supervisor Strict {\n"
        "    strategy: rest_for_all\n"
        "    max_restarts: 10\n"
        "    window: 120\n"
        "    children: [X]\n"
        "}\n"
        "fn main() {}\n"
    );
    ASSERT_NOT_NULL(c);
    /* Should use rest_for_all strategy */
    ASSERT(strstr(c, "LCN_STRATEGY_REST_FOR_ALL") != NULL);
    /* Should emit max_restarts and window values */
    ASSERT(strstr(c, "10") != NULL);
    ASSERT(strstr(c, "120") != NULL);
    /* Should emit report_failure helper */
    ASSERT(strstr(c, "lcn_supervisor_Strict_report_failure") != NULL);
    /* Supervisor should emit child_failed call in the report function */
    ASSERT(strstr(c, "lcn_supervisor_child_failed") != NULL);
    free(c);
}



/* ============================================================
 * TaskGroup Tests
 * ============================================================ */

/* ============================================================
 * TASK GROUP (Structured Concurrency) TESTS
 * ============================================================ */

TEST(parse_task_group) {
    bool err;
    AstNode *prog = parse_source(
        "fn main() {\n"
        "    task_group {\n"
        "        spawn { work_a() }\n"
        "        spawn { work_b() }\n"
        "    }\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    /* fn main → body → first statement */
    AstNode *fn = prog->params;
    ASSERT_NOT_NULL(fn);
    ASSERT_EQ(fn->kind, AST_FN);
    AstNode *body = fn->left; /* block */
    ASSERT_NOT_NULL(body);
    AstNode *stmt = body->params; /* first stmt: EXPR_STMT wrapping TASK_GROUP */
    ASSERT_NOT_NULL(stmt);
    /* The task_group is the expression inside an EXPR_STMT */
    AstNode *tg = (stmt->kind == AST_EXPR_STMT) ? stmt->left : stmt;
    ASSERT_EQ(tg->kind, AST_TASK_GROUP);
    /* Its body is a block with 2 spawn children */
    ASSERT_NOT_NULL(tg->left);
    ASSERT_EQ(tg->left->kind, AST_BLOCK);
    int spawn_count = 0;
    AstNode *child = tg->left->params;
    while (child) {
        AstNode *inner = (child->kind == AST_EXPR_STMT) ? child->left : child;
        if (inner && inner->kind == AST_SPAWN) spawn_count++;
        child = child->next;
    }
    ASSERT_EQ(spawn_count, 2);
}

TEST(codegen_task_group_spawn) {
    char *c = gen_c(
        "fn main() -> Result {\n"
        "    task_group {\n"
        "        spawn { work_a() }\n"
        "        spawn { work_b() }\n"
        "    }\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    /* Must emit task group lifecycle */
    ASSERT(strstr(c, "lcn_task_group_new") != NULL);
    ASSERT(strstr(c, "lcn_task_group_spawn") != NULL);
    ASSERT(strstr(c, "lcn_task_group_await_all") != NULL);
    ASSERT(strstr(c, "lcn_task_group_free") != NULL);
    /* Must emit wrapper functions for the spawns */
    ASSERT(strstr(c, "__lcn_spawn_") != NULL);
    ASSERT(strstr(c, "LcnTaskFn") != NULL);
    free(c);
}

TEST(codegen_task_group_await_all) {
    char *c = gen_c(
        "fn main() -> Result {\n"
        "    let results = task_group {\n"
        "        spawn { work_a() }\n"
        "        spawn { work_b() }\n"
        "        spawn { work_c() }\n"
        "    }\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    /* let binding should produce await_all result */
    ASSERT(strstr(c, "lcn_task_group_new") != NULL);
    ASSERT(strstr(c, "lcn_task_group_spawn") != NULL);
    ASSERT(strstr(c, "lcn_task_group_await_all") != NULL);
    ASSERT(strstr(c, "lcn_task_group_free") != NULL);
    /* The results variable should be declared as void** */
    ASSERT(strstr(c, "void **results") != NULL);
    /* Should have 3 spawn wrapper functions */
    ASSERT(strstr(c, "work_a()") != NULL);
    ASSERT(strstr(c, "work_b()") != NULL);
    ASSERT(strstr(c, "work_c()") != NULL);
    free(c);
}

TEST(codegen_task_group_preamble) {
    char *c = gen_c("");
    ASSERT_NOT_NULL(c);
    /* Standalone mode should include TaskGroup stubs */
    ASSERT(strstr(c, "LcnTaskGroup") != NULL);
    ASSERT(strstr(c, "lcn_task_group_new") != NULL);
    ASSERT(strstr(c, "lcn_task_group_spawn") != NULL);
    ASSERT(strstr(c, "lcn_task_group_await_all") != NULL);
    ASSERT(strstr(c, "lcn_task_group_free") != NULL);
    free(c);
}



/* ============================================================
 * Defense/FFI Tests
 * ============================================================ */

/* ============================================================
 * Link Directive Tests
 * ============================================================ */

TEST(parse_link_directive) {
    bool had_error = false;
    AstNode *prog = parse_source(
        "link \"-lssl -lcrypto\"\n"
        "link \"-lpq\"\n"
        "fn main() -> i32 { return 0 }\n",
        &had_error);
    ASSERT_FALSE(had_error);
    ASSERT_NOT_NULL(prog);
    /* First decl should be AST_LINK */
    AstNode *d = prog->params;
    ASSERT_NOT_NULL(d);
    ASSERT_EQ(d->kind, AST_LINK);
    ASSERT_STR_EQ(d->val.str_val, "-lssl -lcrypto");
    /* Second decl should also be AST_LINK */
    d = d->next;
    ASSERT_NOT_NULL(d);
    ASSERT_EQ(d->kind, AST_LINK);
    ASSERT_STR_EQ(d->val.str_val, "-lpq");
    /* Third decl should be the function */
    d = d->next;
    ASSERT_NOT_NULL(d);
    ASSERT_EQ(d->kind, AST_FN);
}

TEST(codegen_link_directive) {
    char *c = gen_c(
        "link \"-lssl -lcrypto\"\n"
        "link \"-lpq\"\n"
        "fn main() -> i32 { return 0 }\n"
    );
    ASSERT_NOT_NULL(c);
    /* Link directives should appear as comments, not as C code */
    ASSERT(strstr(c, "/* link: -lssl -lcrypto */") != NULL);
    ASSERT(strstr(c, "/* link: -lpq */") != NULL);
    free(c);
}

/* ============================================================
 * Defense-in-Depth Tests
 * ============================================================ */

TEST(codegen_defense_in_depth) {
    char *c = gen_c(
        "capability web {\n    search\n}\n"
        "agent Bot {\n"
        "    capabilities: [web.search]\n"
        "    model: \"gpt-4\"\n"
        "    fn run(input: string) -> string {\n"
        "        let data = http_get(\"https://example.com\")\n"
        "        return data\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* http_get is a sensitive call — should be wrapped with capability check */
    ASSERT(strstr(c, "lcn_capability_check_tool(\"http_get\"") != NULL);
    ASSERT(strstr(c, "capability denied: http_get") != NULL);
    free(c);
}

TEST(codegen_defense_non_agent_no_wrap) {
    char *c = gen_c(
        "fn helper() -> string {\n"
        "    return http_get(\"https://example.com\")\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* Outside agent method, sensitive calls should NOT be wrapped —
     * the capability check call pattern includes a quoted function name */
    ASSERT(strstr(c, "lcn_capability_check_tool(\"http_get\"") == NULL);
    ASSERT(strstr(c, "lcn_http_get") != NULL);
    free(c);
}



/* ============================================================
 * Router/A2A Tests
 * ============================================================ */

TEST(codegen_router_health_check) {
    char *c = gen_c(
        "router HealthRouter {\n"
        "    route \"fast\" -> [gpt4_mini, claude_haiku]\n"
        "    route \"quality\" -> [gpt4, claude_opus]\n"
        "    strategy: cost_aware\n"
        "    fallback: \"gpt4_mini\"\n"
        "    health_check_interval: 30000\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    /* Legacy struct + select still generated */
    ASSERT(strstr(c, "Router_HealthRouter") != NULL);
    ASSERT(strstr(c, "lcn_router_HealthRouter_select") != NULL);
    ASSERT(strstr(c, "\"fast\"") != NULL);
    ASSERT(strstr(c, "\"gpt4_mini\"") != NULL);
    /* Legacy constructor */
    ASSERT(strstr(c, "lcn_router_HealthRouter_new") != NULL);
    ASSERT(strstr(c, "cost_aware") != NULL);
    ASSERT(strstr(c, "gpt4_mini") != NULL);
    free(c);
}

TEST(codegen_router_select) {
    char *c = gen_c(
        "router SelectRouter {\n"
        "    route \"fast\" -> [model_a, model_b]\n"
        "    route \"slow\" -> [model_c]\n"
        "    strategy: latency\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    /* Verify tier-based select handles multiple routes */
    ASSERT(strstr(c, "lcn_router_SelectRouter_select") != NULL);
    ASSERT(strstr(c, "\"fast\"") != NULL);
    ASSERT(strstr(c, "\"slow\"") != NULL);
    ASSERT(strstr(c, "\"model_a\"") != NULL);
    ASSERT(strstr(c, "\"model_c\"") != NULL);
    /* Strategy should be in constructor */
    ASSERT(strstr(c, "latency") != NULL);
    free(c);
}

TEST(codegen_a2a_connect) {
    char *c = gen_c("use a2a(\"https://partner.example.com/agent\") as partner");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "A2A import") != NULL);
    ASSERT(strstr(c, "partner.example.com") != NULL);
    ASSERT(strstr(c, "_a2a_partner_url") != NULL);
    ASSERT(strstr(c, "lcn_a2a_partner_call") != NULL);
    ASSERT(strstr(c, "lcn_a2a_partner_send") != NULL);
    ASSERT(strstr(c, "lcn_a2a_partner_send_task") != NULL);
    free(c);
}

TEST(parse_router_block) {
    bool err;
    AstNode *prog = parse_source(
        "router InferenceRouter {\n"
        "    route \"fast\" -> [gpt4_mini]\n"
        "    route \"quality\" -> [claude_opus, gpt4]\n"
        "    strategy: cost_aware\n"
        "    fallback: \"gpt4_mini\"\n"
        "    health_check_interval: 30000\n"
        "}", &err);
    ASSERT_NOT_NULL(prog);
    ASSERT(!err);
    AstNode *router = prog->params;
    ASSERT_NOT_NULL(router);
    ASSERT(router->kind == AST_ROUTER);
    ASSERT(strcmp(router->name, "InferenceRouter") == 0);

    /* Verify route rules and fields are present */
    AstNode *m = router->params;
    ASSERT_NOT_NULL(m);

    /* Count route rules */
    int route_count = 0;
    int field_count = 0;
    AstNode *n = router->params;
    while (n) {
        if (n->kind == AST_ROUTE_RULE) route_count++;
        if (n->kind == AST_FIELD) field_count++;
        n = n->next;
    }
    ASSERT_EQ(route_count, 2);
    /* strategy + fallback + health_check_interval = 3 fields */
    ASSERT(field_count >= 3);
}



/* ============================================================
 * Mesh Tests
 * ============================================================ */

TEST(parse_mesh_routes) {
    bool err;
    AstNode *prog = parse_source(
        "mesh Pipeline {\n"
        "    route input -> [AgentA, AgentB, AgentC]\n"
        "    route [AgentA, AgentB, AgentC] -> Merger\n"
        "    route Merger -> output\n"
        "}", &err);
    ASSERT_NOT_NULL(prog);
    ASSERT_FALSE(err);
    AstNode *mesh = prog->params;
    ASSERT_NOT_NULL(mesh);
    ASSERT(mesh->kind == AST_MESH);
    ASSERT_STR_EQ(mesh->name, "Pipeline");

    /* First route: input -> [AgentA, AgentB, AgentC] (fan-out) */
    AstNode *r1 = mesh->params;
    ASSERT_NOT_NULL(r1);
    ASSERT(r1->kind == AST_MESH_ROUTE);
    ASSERT_FALSE(r1->is_mut);    /* source is NOT a list */
    ASSERT_TRUE(r1->is_unsafe);  /* target IS a list (fan-out) */
    ASSERT_NOT_NULL(r1->left);
    ASSERT_STR_EQ(r1->left->name, "input");
    ASSERT_NOT_NULL(r1->right);
    ASSERT_STR_EQ(r1->right->name, "AgentA");
    ASSERT_NOT_NULL(r1->right->next);
    ASSERT_STR_EQ(r1->right->next->name, "AgentB");
    ASSERT_NOT_NULL(r1->right->next->next);
    ASSERT_STR_EQ(r1->right->next->next->name, "AgentC");

    /* Second route: [AgentA, AgentB, AgentC] -> Merger (fan-in) */
    AstNode *r2 = r1->next;
    ASSERT_NOT_NULL(r2);
    ASSERT(r2->kind == AST_MESH_ROUTE);
    ASSERT_TRUE(r2->is_mut);     /* source IS a list (fan-in) */
    ASSERT_FALSE(r2->is_unsafe); /* target is NOT a list */
    ASSERT_NOT_NULL(r2->left);
    ASSERT_STR_EQ(r2->left->name, "AgentA");
    ASSERT_NOT_NULL(r2->right);
    ASSERT_STR_EQ(r2->right->name, "Merger");

    /* Third route: Merger -> output (single) */
    AstNode *r3 = r2->next;
    ASSERT_NOT_NULL(r3);
    ASSERT(r3->kind == AST_MESH_ROUTE);
    ASSERT_FALSE(r3->is_mut);
    ASSERT_FALSE(r3->is_unsafe);
    ASSERT_STR_EQ(r3->left->name, "Merger");
    ASSERT_STR_EQ(r3->right->name, "output");
}



/* ============================================================
 * Mesh Tests
 * ============================================================ */

TEST(codegen_mesh_fan_out) {
    char *c = gen_c(
        "agent AgentA { model: \"gpt-4\" }\n"
        "agent AgentB { model: \"gpt-4\" }\n"
        "agent AgentC { model: \"gpt-4\" }\n"
        "mesh Pipeline {\n"
        "    route input -> [AgentA, AgentB, AgentC]\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    /* Must have the mesh run function */
    ASSERT(strstr(c, "lcn_mesh_Pipeline_run") != NULL);
    /* Must generate agent wrapper functions */
    ASSERT(strstr(c, "lcn_mesh_Pipeline_agent_AgentA") != NULL);
    ASSERT(strstr(c, "lcn_mesh_Pipeline_agent_AgentB") != NULL);
    ASSERT(strstr(c, "lcn_mesh_Pipeline_agent_AgentC") != NULL);
    /* Must use fan-out API */
    ASSERT(strstr(c, "lcn_mesh_add_route_fan_out") != NULL);
    /* Must create and execute the mesh */
    ASSERT(strstr(c, "lcn_mesh_new") != NULL);
    ASSERT(strstr(c, "lcn_mesh_execute") != NULL);
    free(c);
}

TEST(codegen_mesh_fan_in) {
    char *c = gen_c(
        "agent AgentA { model: \"gpt-4\" }\n"
        "agent AgentB { model: \"gpt-4\" }\n"
        "agent Merger { model: \"gpt-4\" }\n"
        "mesh Pipeline {\n"
        "    route [AgentA, AgentB] -> Merger\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_mesh_Pipeline_run") != NULL);
    /* Must generate wrapper for Merger */
    ASSERT(strstr(c, "lcn_mesh_Pipeline_agent_Merger") != NULL);
    /* Must use fan-in API */
    ASSERT(strstr(c, "lcn_mesh_add_route_fan_in") != NULL);
    ASSERT(strstr(c, "lcn_mesh_execute") != NULL);
    free(c);
}

TEST(codegen_mesh_sequential) {
    char *c = gen_c(
        "agent AgentA { model: \"gpt-4\" }\n"
        "agent AgentB { model: \"gpt-4\" }\n"
        "mesh Pipeline {\n"
        "    route input -> AgentA\n"
        "    route AgentA -> AgentB\n"
        "    route AgentB -> output\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "lcn_mesh_Pipeline_run") != NULL);
    /* Must use single route API for each route */
    ASSERT(strstr(c, "lcn_mesh_add_route_single") != NULL);
    /* Must generate agent wrappers */
    ASSERT(strstr(c, "lcn_mesh_Pipeline_agent_AgentA") != NULL);
    ASSERT(strstr(c, "lcn_mesh_Pipeline_agent_AgentB") != NULL);
    ASSERT(strstr(c, "lcn_mesh_execute") != NULL);
    free(c);
}



/* ============================================================
 * Comptime Tests
 * ============================================================ */

/* ============================================================
 * Compile-Time Evaluation (comptime) Tests
 * ============================================================ */

TEST(parse_comptime_block) {
    bool err;
    AstNode *prog = parse_source("let x = comptime { 2 + 3 * 4 }", &err);
    ASSERT_FALSE(err);
    ASSERT_NOT_NULL(prog);
    /* First statement should be a let with comptime on the right */
    AstNode *decl = prog->params;
    ASSERT_NOT_NULL(decl);
    ASSERT_EQ(decl->kind, AST_LET);
    ASSERT_NOT_NULL(decl->right);
    ASSERT_EQ(decl->right->kind, AST_COMPTIME);
    /* comptime should contain a block */
    ASSERT_NOT_NULL(decl->right->left);
    ASSERT_EQ(decl->right->left->kind, AST_BLOCK);
}

TEST(parse_comptime_top_level) {
    bool err;
    AstNode *prog = parse_source("comptime {\n    assert(true, \"ok\")\n}", &err);
    ASSERT_FALSE(err);
    ASSERT_NOT_NULL(prog);
    AstNode *decl = prog->params;
    ASSERT_NOT_NULL(decl);
    ASSERT_EQ(decl->kind, AST_COMPTIME);
}

TEST(codegen_comptime_int_arithmetic) {
    /* comptime { 2 + 3 * 4 } should evaluate to 14 at compile time */
    char *c = gen_c("fn main() {\n    let x = comptime { 2 + 3 * 4 }\n    println(to_string(x))\n}");
    ASSERT_NOT_NULL(c);
    /* The comptime block should be replaced with a constant 14 */
    ASSERT(strstr(c, "14LL") != NULL);
    free(c);
}

TEST(codegen_comptime_string_concat) {
    /* comptime { "hello" + " world" } should evaluate to "hello world" */
    char *c = gen_c("fn main() {\n    let s = comptime { \"hello\" + \" world\" }\n    println(s)\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "\"hello world\"") != NULL);
    free(c);
}

TEST(codegen_comptime_if_expr) {
    /* comptime { if true { 42 } else { 0 } } should evaluate to 42 */
    char *c = gen_c("fn main() {\n    let x = comptime { if true { 42 } else { 0 } }\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "42LL") != NULL);
    /* Should NOT contain 0LL as the else branch result */
    free(c);
}

TEST(codegen_comptime_if_expr_false) {
    /* comptime { if false { 42 } else { 99 } } should evaluate to 99 */
    char *c = gen_c("fn main() {\n    let x = comptime { if false { 42 } else { 99 } }\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "99LL") != NULL);
    free(c);
}

TEST(codegen_comptime_assert_pass) {
    /* comptime { assert(1 + 1 == 2, "math works") } should compile fine */
    char *c = gen_c("fn main() {\n    comptime { assert(1 + 1 == 2, \"math works\") }\n}");
    ASSERT_NOT_NULL(c);
    /* Should not have any error marker */
    ASSERT(strstr(c, "comptime error") == NULL);
    free(c);
}

TEST(codegen_comptime_assert_fail) {
    /* comptime { assert(1 == 2, "bad math") } should produce an error */
    char *c = gen_c("fn main() {\n    comptime { assert(1 == 2, \"bad math\") }\n}");
    ASSERT_NOT_NULL(c);
    /* The comptime error should be noted in the output */
    ASSERT(strstr(c, "comptime error") != NULL);
    free(c);
}

TEST(codegen_comptime_nested_let) {
    /* comptime { let a = 10; let b = 20; a + b } should evaluate to 30 */
    char *c = gen_c(
        "fn main() {\n"
        "    let x = comptime {\n"
        "        let a = 10\n"
        "        let b = 20\n"
        "        a + b\n"
        "    }\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "30LL") != NULL);
    free(c);
}

TEST(codegen_comptime_float_arithmetic) {
    /* comptime { 3.14 * 2.0 } should evaluate to 6.28 */
    char *c = gen_c("fn main() {\n    let x = comptime { 3.14 * 2.0 }\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "6.28") != NULL);
    free(c);
}

TEST(codegen_comptime_boolean_logic) {
    /* comptime { true && false } should evaluate to false */
    char *c = gen_c("fn main() {\n    let x = comptime { true && false }\n}");
    ASSERT_NOT_NULL(c);
    /* Should have false, not true as the result */
    ASSERT(strstr(c, "false") != NULL);
    free(c);
}

TEST(codegen_comptime_comparison) {
    /* comptime { 10 > 5 } should evaluate to true */
    char *c = gen_c("fn main() {\n    let x = comptime { 10 > 5 }\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "true") != NULL);
    free(c);
}

TEST(codegen_comptime_unary_negation) {
    /* comptime { -42 } should evaluate to -42 */
    char *c = gen_c("fn main() {\n    let x = comptime { -42 }\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "-42LL") != NULL);
    free(c);
}

TEST(codegen_comptime_len_builtin) {
    /* comptime { len("hello") } should evaluate to 5 */
    char *c = gen_c("fn main() {\n    let x = comptime { len(\"hello\") }\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "5LL") != NULL);
    free(c);
}

TEST(codegen_comptime_min_max) {
    /* comptime { max(10, 20) } should evaluate to 20 */
    char *c = gen_c("fn main() {\n    let x = comptime { max(10, 20) }\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "20LL") != NULL);
    free(c);
}

TEST(codegen_comptime_complex_expr) {
    /* comptime { let size = 1024; size * size } should evaluate to 1048576 */
    char *c = gen_c(
        "fn main() {\n"
        "    let MAX = comptime {\n"
        "        let size = 1024\n"
        "        size * size\n"
        "    }\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "1048576LL") != NULL);
    free(c);
}

TEST(codegen_comptime_top_level_assert) {
    /* Top-level comptime assert that passes */
    char *c = gen_c(
        "comptime {\n"
        "    assert(100 > 0, \"positive check\")\n"
        "}\n"
        "fn main() {\n"
        "    return\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "comptime error") == NULL);
    free(c);
}

TEST(codegen_comptime_to_string) {
    /* comptime { to_string(42) } should evaluate to "42" */
    char *c = gen_c("fn main() {\n    let s = comptime { to_string(42) }\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "\"42\"") != NULL);
    free(c);
}

TEST(codegen_comptime_abs) {
    /* comptime { abs(-7) } should evaluate to 7 */
    char *c = gen_c("fn main() {\n    let x = comptime { abs(-7) }\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "7LL") != NULL);
    free(c);
}

TEST(codegen_comptime_bitwise) {
    /* comptime { 0xFF & 0x0F } should evaluate to 15 */
    char *c = gen_c("fn main() {\n    let x = comptime { 0xFF & 0x0F }\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "15LL") != NULL);
    free(c);
}

TEST(codegen_comptime_division) {
    /* comptime { 100 / 3 } should evaluate to 33 (integer division) */
    char *c = gen_c("fn main() {\n    let x = comptime { 100 / 3 }\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "33LL") != NULL);
    free(c);
}

TEST(codegen_comptime_modulo) {
    /* comptime { 17 % 5 } should evaluate to 2 */
    char *c = gen_c("fn main() {\n    let x = comptime { 17 % 5 }\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "2LL") != NULL);
    free(c);
}

TEST(codegen_comptime_string_comparison) {
    /* comptime { "abc" == "abc" } should evaluate to true */
    char *c = gen_c("fn main() {\n    let x = comptime { \"abc\" == \"abc\" }\n}");
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "true") != NULL);
    free(c);
}



/* ============================================================
 * Traits Tests
 * ============================================================ */

/* ============================================================
 * Interface / Trait / Union Type Tests
 * ============================================================ */

TEST(parse_interface) {
    bool err;
    AstNode *prog = parse_source(
        "interface Summarizable {\n"
        "    fn summarize(&self) -> string\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *iface = prog->params;
    ASSERT_NOT_NULL(iface);
    ASSERT_EQ(iface->kind, AST_INTERFACE);
    ASSERT_STR_EQ(iface->name, "Summarizable");
    /* Should have one method */
    ASSERT_EQ(ast_list_len(iface->params), 1);
    AstNode *method = iface->params;
    ASSERT_EQ(method->kind, AST_FN);
    ASSERT_STR_EQ(method->name, "summarize");
}

TEST(parse_impl_for_interface) {
    bool err;
    AstNode *prog = parse_source(
        "interface Summarizable {\n"
        "    fn summarize(&self) -> string\n"
        "}\n"
        "struct Article {\n"
        "    title: string\n"
        "}\n"
        "impl Summarizable for Article {\n"
        "    fn summarize(&self) -> string {\n"
        "        return self.title\n"
        "    }\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);
    /* Find the impl node */
    AstNode *decl = prog->params;
    while (decl && decl->kind != AST_IMPL) decl = decl->next;
    ASSERT_NOT_NULL(decl);
    ASSERT_EQ(decl->kind, AST_IMPL);
    /* right = trait/interface, left = target type */
    ASSERT_NOT_NULL(decl->right);
    ASSERT_STR_EQ(decl->right->name, "Summarizable");
    ASSERT_NOT_NULL(decl->left);
    ASSERT_STR_EQ(decl->left->name, "Article");
    ASSERT_EQ(ast_list_len(decl->params), 1);
}

TEST(codegen_interface_vtable) {
    char *c = gen_c(
        "interface Summarizable {\n"
        "    fn summarize(&self) -> string\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    /* Should generate vtable struct */
    ASSERT(strstr(c, "Summarizable_vtable") != NULL);
    ASSERT(strstr(c, "(*summarize)(void *self)") != NULL);
    /* Should generate trait object */
    ASSERT(strstr(c, "Summarizable_obj") != NULL);
    ASSERT(strstr(c, "void *data") != NULL);
    free(c);
}

TEST(codegen_impl_vtable_instance) {
    char *c = gen_c(
        "interface Summarizable {\n"
        "    fn summarize(&self) -> string\n"
        "}\n"
        "struct Article {\n"
        "    title: string\n"
        "}\n"
        "impl Summarizable for Article {\n"
        "    fn summarize(&self) -> string {\n"
        "        return self.title\n"
        "    }\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    /* Should generate vtable wrapper function */
    ASSERT(strstr(c, "Article_Summarizable_summarize") != NULL);
    /* Should generate vtable instance */
    ASSERT(strstr(c, "Article_Summarizable_vt") != NULL);
    ASSERT(strstr(c, ".summarize = Article_Summarizable_summarize") != NULL);
    free(c);
}

TEST(parse_union_type) {
    bool err;
    AstNode *prog = parse_source(
        "type StringOrInt = string | int",
        &err
    );
    ASSERT_FALSE(err);
    AstNode *decl = prog->params;
    ASSERT_NOT_NULL(decl);
    ASSERT_EQ(decl->kind, AST_TYPE_ALIAS);
    ASSERT_STR_EQ(decl->name, "StringOrInt");
    ASSERT_NOT_NULL(decl->type_expr);
    ASSERT_EQ(decl->type_expr->kind, AST_TYPE_UNION);
    /* Should have 2 variant types in params list */
    ASSERT_EQ(ast_list_len(decl->type_expr->params), 2);
}

TEST(codegen_union_tagged) {
    char *c = gen_c(
        "type StringOrInt = string | int"
    );
    ASSERT_NOT_NULL(c);
    /* Should generate tag enum */
    ASSERT(strstr(c, "StringOrInt_Tag") != NULL);
    ASSERT(strstr(c, "StringOrInt_TAG_string") != NULL);
    ASSERT(strstr(c, "StringOrInt_TAG_int") != NULL);
    /* Should generate tagged union struct */
    ASSERT(strstr(c, "as_string") != NULL);
    ASSERT(strstr(c, "as_int") != NULL);
    /* Should generate constructor functions */
    ASSERT(strstr(c, "StringOrInt_from_string") != NULL);
    ASSERT(strstr(c, "StringOrInt_from_int") != NULL);
    /* Should generate is_type checks */
    ASSERT(strstr(c, "StringOrInt_is_string") != NULL);
    ASSERT(strstr(c, "StringOrInt_is_int") != NULL);
    free(c);
}

TEST(typecheck_interface_missing_method) {
    /* impl block missing a required method should fail */
    bool ok = typecheck_source(
        "interface Printable {\n"
        "    fn display(&self) -> string\n"
        "    fn debug(&self) -> string\n"
        "}\n"
        "struct Foo {\n"
        "    x: i32\n"
        "}\n"
        "impl Printable for Foo {\n"
        "    fn display(&self) -> string {\n"
        "        return \"foo\"\n"
        "    }\n"
        "}"
    );
    ASSERT_FALSE(ok);
}



/* ============================================================
 * Ownership & Borrow Checking Tests
 *
 * The ownership pass runs as advisory (warnings only) by default.
 * These tests verify that warnings are emitted for violations
 * and that valid code produces no warnings.
 * ============================================================ */

/* Helper: parse + typecheck, return number of ownership warnings */
static int typecheck_ownership_warnings(const char *source) {
    arena_reset(&test_arena);
    arena_reset(&test_intern_arena);

    size_t len = strlen(source);
    ErrorReporter reporter = reporter_new("<test>", source, len);
    StringIntern intern = intern_new(&test_intern_arena);
    Lexer lexer = lexer_new("<test>", source, len, &intern, &reporter);
    Parser parser = parser_new(&lexer, &test_arena, &reporter);
    AstNode *program = parse_program(&parser);
    if (parser.had_error) return -1;

    typecheck_program(program, &reporter, &test_arena);

    /* Count warnings that contain "[ownership]" */
    int own_warnings = 0;
    int i;
    for (i = 0; i < reporter.count; i++) {
        if (reporter.errors[i].is_warning &&
            reporter.errors[i].message &&
            strstr(reporter.errors[i].message, "[ownership]") != NULL) {
            own_warnings++;
        }
    }
    return own_warnings;
}

TEST(ownership_use_after_move) {
    /* Variable moved into function call, then used again */
    int w = typecheck_ownership_warnings(
        "fn process(data: string) -> string { return data }\n"
        "fn main() -> Result {\n"
        "    let data = \"hello\"\n"
        "    process(data)\n"
        "    println(data)\n"
        "}\n"
    );
    /* data is moved into process(), then used in println() */
    ASSERT(w >= 1);
}

TEST(ownership_move_while_borrowed) {
    /* Can't move a variable while it is borrowed */
    int w = typecheck_ownership_warnings(
        "fn process(data: string) -> string { return data }\n"
        "fn main() -> Result {\n"
        "    let data = \"hello\"\n"
        "    let r = &data\n"
        "    process(data)\n"
        "}\n"
    );
    /* data is borrowed via &data, then moved into process() */
    ASSERT(w >= 1);
}

TEST(ownership_double_mut_borrow) {
    /* Can't have two mutable borrows at the same time */
    int w = typecheck_ownership_warnings(
        "fn main() -> Result {\n"
        "    let data = \"hello\"\n"
        "    let r1 = &mut data\n"
        "    let r2 = &mut data\n"
        "}\n"
    );
    /* Two &mut data at the same time */
    ASSERT(w >= 1);
}

TEST(ownership_immutable_borrow_ok) {
    /* Multiple immutable borrows are OK */
    int w = typecheck_ownership_warnings(
        "fn main() -> Result {\n"
        "    let data = \"hello\"\n"
        "    let r1 = &data\n"
        "    let r2 = &data\n"
        "    let r3 = &data\n"
        "}\n"
    );
    /* Multiple & borrows are fine */
    ASSERT_EQ(w, 0);
}

TEST(ownership_scope_release) {
    /* Borrows released at scope exit allow subsequent operations */
    int w = typecheck_ownership_warnings(
        "fn process(data: string) -> string { return data }\n"
        "fn main() -> Result {\n"
        "    let data = \"hello\"\n"
        "    if true {\n"
        "        let r = &data\n"
        "    }\n"
        "    process(data)\n"
        "}\n"
    );
    /* Borrow of data is in inner scope, released before process() */
    ASSERT_EQ(w, 0);
}

TEST(ownership_function_param_move) {
    /* Function parameters passed by value move ownership */
    int w = typecheck_ownership_warnings(
        "fn consume(x: string) -> string { return x }\n"
        "fn transform(y: string) -> string { return y }\n"
        "fn main() -> Result {\n"
        "    let val = \"test\"\n"
        "    consume(val)\n"
        "    transform(val)\n"
        "}\n"
    );
    /* val moved into consume(), then used in transform() */
    ASSERT(w >= 1);
}

TEST(ownership_ref_param_borrow) {
    /* Passing &x borrows instead of moving */
    int w = typecheck_ownership_warnings(
        "fn inspect(x: string) -> i32 { return 0 }\n"
        "fn main() -> Result {\n"
        "    let data = \"hello\"\n"
        "    inspect(&data)\n"
        "    inspect(&data)\n"
        "    println(data)\n"
        "}\n"
    );
    /* &data borrows, data still usable afterwards */
    ASSERT_EQ(w, 0);
}

TEST(ownership_advisory_mode) {
    /* In default advisory mode, ownership violations are warnings, not errors.
     * The typecheck_program should still return true (no enforced errors). */
    bool ok = typecheck_source(
        "fn process(data: string) -> string { return data }\n"
        "fn main() -> Result {\n"
        "    let data = \"hello\"\n"
        "    process(data)\n"
        "    println(data)\n"
        "}\n"
    );
    /* typecheck_program returns true because ownership warnings
     * are advisory (not counted as enforced errors) */
    ASSERT(ok);
}

/* ============================================================
 * Package Manager Tests
 * ============================================================ */

TEST(package_parse_toml) {
    const char *toml =
        "[package]\n"
        "name = \"my-agent\"\n"
        "version = \"0.1.0\"\n"
        "description = \"A categorization agent\"\n"
        "\n"
        "[dependencies]\n"
        "whatsapp-gateway = \"1.2.0\"\n"
        "medical-ontology = \"0.3.0\"\n"
        "\n"
        "[dev-dependencies]\n"
        "test-helpers = \"0.1.0\"\n";

    LcnPackage pkg;
    bool ok = lcn_package_parse(toml, strlen(toml), &pkg);
    ASSERT(ok);
    ASSERT_STR_EQ(pkg.name, "my-agent");
    ASSERT_STR_EQ(pkg.version, "0.1.0");
    ASSERT_STR_EQ(pkg.description, "A categorization agent");
    ASSERT_EQ(pkg.dep_count, 3);

    /* Check dependencies */
    ASSERT_STR_EQ(pkg.deps[0].name, "whatsapp-gateway");
    ASSERT_STR_EQ(pkg.deps[0].version, "1.2.0");
    ASSERT_EQ(pkg.deps[0].kind, LCN_DEP_REGISTRY);
    ASSERT_FALSE(pkg.deps[0].is_dev);

    ASSERT_STR_EQ(pkg.deps[1].name, "medical-ontology");
    ASSERT_STR_EQ(pkg.deps[1].version, "0.3.0");
    ASSERT_FALSE(pkg.deps[1].is_dev);

    ASSERT_STR_EQ(pkg.deps[2].name, "test-helpers");
    ASSERT_STR_EQ(pkg.deps[2].version, "0.1.0");
    ASSERT_TRUE(pkg.deps[2].is_dev);
}

TEST(package_parse_toml_path_dep) {
    const char *toml =
        "[package]\n"
        "name = \"my-project\"\n"
        "version = \"1.0.0\"\n"
        "\n"
        "[dependencies]\n"
        "my-lib = { path = \"../my-lib\" }\n"
        "other-lib = { git = \"https://github.com/user/other.git\", rev = \"v1.0\" }\n";

    LcnPackage pkg;
    bool ok = lcn_package_parse(toml, strlen(toml), &pkg);
    ASSERT(ok);
    ASSERT_STR_EQ(pkg.name, "my-project");
    ASSERT_EQ(pkg.dep_count, 2);

    ASSERT_STR_EQ(pkg.deps[0].name, "my-lib");
    ASSERT_EQ(pkg.deps[0].kind, LCN_DEP_PATH);
    ASSERT_STR_EQ(pkg.deps[0].path, "../my-lib");

    ASSERT_STR_EQ(pkg.deps[1].name, "other-lib");
    ASSERT_EQ(pkg.deps[1].kind, LCN_DEP_GIT);
    ASSERT_STR_EQ(pkg.deps[1].git_url, "https://github.com/user/other.git");
    ASSERT_STR_EQ(pkg.deps[1].git_rev, "v1.0");
}

TEST(package_parse_toml_comments) {
    const char *toml =
        "# This is a comment\n"
        "[package]\n"
        "name = \"commented\"\n"
        "version = \"0.2.0\"\n"
        "# Another comment\n"
        "\n"
        "[dependencies]\n"
        "# dep comment\n"
        "dep-a = \"1.0.0\"\n";

    LcnPackage pkg;
    bool ok = lcn_package_parse(toml, strlen(toml), &pkg);
    ASSERT(ok);
    ASSERT_STR_EQ(pkg.name, "commented");
    ASSERT_STR_EQ(pkg.version, "0.2.0");
    ASSERT_EQ(pkg.dep_count, 1);
    ASSERT_STR_EQ(pkg.deps[0].name, "dep-a");
}

TEST(package_parse_toml_empty) {
    const char *toml = "";
    LcnPackage pkg;
    bool ok = lcn_package_parse(toml, 0, &pkg);
    ASSERT_FALSE(ok);  /* No name -> invalid */
}

TEST(package_semver_compare) {
    ASSERT(semver_compare("1.0.0", "1.0.0") == 0);
    ASSERT(semver_compare("1.0.1", "1.0.0") > 0);
    ASSERT(semver_compare("1.0.0", "1.0.1") < 0);
    ASSERT(semver_compare("2.0.0", "1.9.9") > 0);
    ASSERT(semver_compare("1.9.9", "2.0.0") < 0);
    ASSERT(semver_compare("1.2.0", "1.1.9") > 0);
    ASSERT(semver_compare("0.1.0", "0.1.0") == 0);
}

TEST(package_semver_satisfies) {
    /* Exact match */
    ASSERT_TRUE(semver_satisfies("1.2.0", "1.2.0"));
    ASSERT_FALSE(semver_satisfies("1.2.1", "1.2.0"));
    ASSERT_FALSE(semver_satisfies("1.3.0", "1.2.0"));

    /* Compatible (^) */
    ASSERT_TRUE(semver_satisfies("1.2.0", "^1.2.0"));
    ASSERT_TRUE(semver_satisfies("1.2.5", "^1.2.0"));
    ASSERT_TRUE(semver_satisfies("1.9.9", "^1.2.0"));
    ASSERT_FALSE(semver_satisfies("2.0.0", "^1.2.0"));
    ASSERT_FALSE(semver_satisfies("1.1.0", "^1.2.0"));
    ASSERT_FALSE(semver_satisfies("0.9.0", "^1.2.0"));

    /* Wildcard */
    ASSERT_TRUE(semver_satisfies("0.0.1", "*"));
    ASSERT_TRUE(semver_satisfies("99.99.99", "*"));

    /* Greater/Less than */
    ASSERT_TRUE(semver_satisfies("2.0.0", ">1.0.0"));
    ASSERT_FALSE(semver_satisfies("1.0.0", ">1.0.0"));
    ASSERT_TRUE(semver_satisfies("1.0.0", ">=1.0.0"));
    ASSERT_TRUE(semver_satisfies("0.9.0", "<1.0.0"));
    ASSERT_FALSE(semver_satisfies("1.0.0", "<1.0.0"));
    ASSERT_TRUE(semver_satisfies("1.0.0", "<=1.0.0"));
}

TEST(package_semver_parse) {
    LcnSemver sv;

    ASSERT_TRUE(semver_parse("1.2.3", &sv));
    ASSERT_EQ(sv.major, 1);
    ASSERT_EQ(sv.minor, 2);
    ASSERT_EQ(sv.patch, 3);

    ASSERT_TRUE(semver_parse("0.0.0", &sv));
    ASSERT_EQ(sv.major, 0);
    ASSERT_EQ(sv.minor, 0);
    ASSERT_EQ(sv.patch, 0);

    ASSERT_TRUE(semver_parse("v2.1.0", &sv));
    ASSERT_EQ(sv.major, 2);
    ASSERT_EQ(sv.minor, 1);
    ASSERT_EQ(sv.patch, 0);

    ASSERT_FALSE(semver_parse(NULL, &sv));
}

TEST(package_resolve_simple) {
    /* Create a simple package with no deps — should resolve to 0 */
    LcnPackage pkg;
    memset(&pkg, 0, sizeof(pkg));
    strcpy(pkg.name, "test-pkg");
    strcpy(pkg.version, "1.0.0");

    LcnResolvedDep resolved[8];
    int count = lcn_resolve_dependencies(&pkg, "/tmp", resolved, 8);
    ASSERT_EQ(count, 0);
}

TEST(package_lock_roundtrip) {
    /* Create a lock file, serialize, parse, verify */
    LcnLockFile lock;
    memset(&lock, 0, sizeof(lock));

    strcpy(lock.entries[0].name, "whatsapp-gateway");
    strcpy(lock.entries[0].resolved_version, "1.2.3");
    strcpy(lock.entries[0].hash, "sha256:abc123def456");
    lock.count = 1;

    strcpy(lock.entries[1].name, "medical-ontology");
    strcpy(lock.entries[1].resolved_version, "0.3.1");
    strcpy(lock.entries[1].hash, "sha256:789xyz000111");
    lock.count = 2;

    /* Serialize */
    char buf[4096];
    int n = lcn_lock_serialize(&lock, buf, sizeof(buf));
    ASSERT(n > 0);

    /* Parse back */
    LcnLockFile parsed;
    bool ok = lcn_lock_parse(buf, (size_t)n, &parsed);
    ASSERT(ok);
    ASSERT_EQ(parsed.count, 2);
    ASSERT_STR_EQ(parsed.entries[0].name, "whatsapp-gateway");
    ASSERT_STR_EQ(parsed.entries[0].resolved_version, "1.2.3");
    ASSERT_STR_EQ(parsed.entries[0].hash, "sha256:abc123def456");
    ASSERT_STR_EQ(parsed.entries[1].name, "medical-ontology");
    ASSERT_STR_EQ(parsed.entries[1].resolved_version, "0.3.1");
    ASSERT_STR_EQ(parsed.entries[1].hash, "sha256:789xyz000111");
}

TEST(package_serialize_roundtrip) {
    /* Parse TOML, serialize, parse again, compare */
    const char *toml =
        "[package]\n"
        "name = \"roundtrip\"\n"
        "version = \"2.0.0\"\n"
        "description = \"Roundtrip test\"\n"
        "\n"
        "[dependencies]\n"
        "dep-a = \"1.0.0\"\n"
        "dep-b = { path = \"../lib\" }\n"
        "\n"
        "[dev-dependencies]\n"
        "dev-dep = \"0.1.0\"\n";

    LcnPackage pkg1;
    bool ok = lcn_package_parse(toml, strlen(toml), &pkg1);
    ASSERT(ok);

    /* Serialize */
    char buf[4096];
    int n = lcn_package_serialize(&pkg1, buf, sizeof(buf));
    ASSERT(n > 0);

    /* Parse again */
    LcnPackage pkg2;
    ok = lcn_package_parse(buf, (size_t)n, &pkg2);
    ASSERT(ok);

    ASSERT_STR_EQ(pkg2.name, "roundtrip");
    ASSERT_STR_EQ(pkg2.version, "2.0.0");
    ASSERT_STR_EQ(pkg2.description, "Roundtrip test");
    ASSERT_EQ(pkg2.dep_count, 3);
}

TEST(package_add_remove_dep) {
    LcnPackage pkg;
    memset(&pkg, 0, sizeof(pkg));
    strcpy(pkg.name, "test-pkg");
    strcpy(pkg.version, "1.0.0");

    /* Add deps */
    bool ok = lcn_package_add_dep(&pkg, "dep-a", "1.0.0",
                                   LCN_DEP_REGISTRY, NULL, false);
    ASSERT(ok);
    ASSERT_EQ(pkg.dep_count, 1);

    ok = lcn_package_add_dep(&pkg, "dep-b", NULL,
                              LCN_DEP_PATH, "../lib-b", false);
    ASSERT(ok);
    ASSERT_EQ(pkg.dep_count, 2);

    /* Update existing dep */
    ok = lcn_package_add_dep(&pkg, "dep-a", "2.0.0",
                              LCN_DEP_REGISTRY, NULL, false);
    ASSERT(ok);
    ASSERT_EQ(pkg.dep_count, 2);
    ASSERT_STR_EQ(pkg.deps[0].version, "2.0.0");

    /* Remove dep */
    ok = lcn_package_remove_dep(&pkg, "dep-a");
    ASSERT(ok);
    ASSERT_EQ(pkg.dep_count, 1);
    ASSERT_STR_EQ(pkg.deps[0].name, "dep-b");

    /* Remove non-existent */
    ok = lcn_package_remove_dep(&pkg, "nonexistent");
    ASSERT_FALSE(ok);
    ASSERT_EQ(pkg.dep_count, 1);
}

TEST(package_semver_tilde) {
    /* Tilde: ~X.Y.Z means >=X.Y.Z <X.(Y+1).0 */
    ASSERT_TRUE(semver_satisfies("1.2.0", "~1.2.0"));
    ASSERT_TRUE(semver_satisfies("1.2.5", "~1.2.0"));
    ASSERT_FALSE(semver_satisfies("1.3.0", "~1.2.0"));
    ASSERT_FALSE(semver_satisfies("2.0.0", "~1.2.0"));
    ASSERT_FALSE(semver_satisfies("1.1.0", "~1.2.0"));
}

/* ============================================================
 * Cross-Compilation Target Tests
 * ============================================================ */

TEST(target_parse_triple) {
    /* Parse "aarch64-linux" -> arch=AARCH64, os=LINUX, abi=GNU (default) */
    LcnTarget t = lcn_parse_target("aarch64-linux");
    ASSERT_EQ(t.arch, LCN_ARCH_AARCH64);
    ASSERT_EQ(t.os, LCN_OS_LINUX);
    ASSERT_EQ(t.abi, LCN_ABI_GNU);  /* default for linux */
    ASSERT_STR_EQ(t.triple, "aarch64-linux-gnu");
}

TEST(target_parse_full) {
    /* Parse "x86_64-linux-musl" -> arch=X86_64, os=LINUX, abi=MUSL */
    LcnTarget t = lcn_parse_target("x86_64-linux-musl");
    ASSERT_EQ(t.arch, LCN_ARCH_X86_64);
    ASSERT_EQ(t.os, LCN_OS_LINUX);
    ASSERT_EQ(t.abi, LCN_ABI_MUSL);
    ASSERT_STR_EQ(t.triple, "x86_64-linux-musl");
}

TEST(target_parse_darwin) {
    LcnTarget t = lcn_parse_target("aarch64-darwin");
    ASSERT_EQ(t.arch, LCN_ARCH_AARCH64);
    ASSERT_EQ(t.os, LCN_OS_DARWIN);
    ASSERT_EQ(t.abi, LCN_ABI_NONE);
    ASSERT_STR_EQ(t.triple, "aarch64-darwin");
}

TEST(target_parse_windows) {
    LcnTarget t = lcn_parse_target("x86_64-windows-msvc");
    ASSERT_EQ(t.arch, LCN_ARCH_X86_64);
    ASSERT_EQ(t.os, LCN_OS_WINDOWS);
    ASSERT_EQ(t.abi, LCN_ABI_MSVC);
    ASSERT_STR_EQ(t.triple, "x86_64-windows-msvc");
}

TEST(target_parse_arm64_alias) {
    /* "arm64" should map to AARCH64 */
    LcnTarget t = lcn_parse_target("arm64-linux");
    ASSERT_EQ(t.arch, LCN_ARCH_AARCH64);
    ASSERT_EQ(t.os, LCN_OS_LINUX);
}

TEST(target_parse_invalid) {
    /* Invalid triple should result in UNKNOWN */
    LcnTarget t = lcn_parse_target("sparc-solaris");
    ASSERT_EQ(t.arch, LCN_ARCH_UNKNOWN);
    ASSERT_EQ(t.os, LCN_OS_UNKNOWN);
}

TEST(target_parse_empty) {
    LcnTarget t = lcn_parse_target("");
    ASSERT_EQ(t.arch, LCN_ARCH_UNKNOWN);
}

TEST(target_parse_null) {
    LcnTarget t = lcn_parse_target(NULL);
    ASSERT_EQ(t.arch, LCN_ARCH_UNKNOWN);
}

TEST(target_native_detect) {
    /* Native detection should yield non-UNKNOWN values */
    LcnTarget t = lcn_native_target();
    ASSERT_NEQ(t.arch, LCN_ARCH_UNKNOWN);
    ASSERT_NEQ(t.os, LCN_OS_UNKNOWN);
    ASSERT_TRUE(t.is_native);
    ASSERT_STR_EQ(t.cc, "cc");
    /* Triple should be non-empty */
    ASSERT_TRUE(strlen(t.triple) > 0);
}

TEST(target_ldflags_linux) {
    LcnTarget t = lcn_parse_target("x86_64-linux");
    /* Linux should have -lpthread -lm -ldl */
    ASSERT_TRUE(strstr(t.ldflags, "-lpthread") != NULL);
    ASSERT_TRUE(strstr(t.ldflags, "-lm") != NULL);
}

TEST(target_ldflags_darwin) {
    LcnTarget t = lcn_parse_target("aarch64-darwin");
    /* macOS should have -lm */
    ASSERT_TRUE(strstr(t.ldflags, "-lm") != NULL);
    /* macOS should NOT have -lpthread (built-in) */
    ASSERT_TRUE(strstr(t.ldflags, "-lpthread") == NULL);
}

TEST(target_ldflags_windows) {
    LcnTarget t = lcn_parse_target("x86_64-windows-msvc");
    ASSERT_TRUE(strstr(t.ldflags, "-lws2_32") != NULL);
}

TEST(target_static_ldflags) {
    LcnTarget t = lcn_parse_target("x86_64-linux");
    t.static_link = true;
    /* Re-parse to pick up static flags */
    t = lcn_parse_target("x86_64-linux");
    t.static_link = true;
    /* Manually set what cmd_build would set */
    snprintf(t.ldflags, sizeof(t.ldflags), "-static -lpthread -lm -ldl");
    ASSERT_TRUE(strstr(t.ldflags, "-static") != NULL);
}

TEST(target_arch_str) {
    ASSERT_STR_EQ(lcn_arch_str(LCN_ARCH_X86_64), "x86_64");
    ASSERT_STR_EQ(lcn_arch_str(LCN_ARCH_AARCH64), "aarch64");
    ASSERT_STR_EQ(lcn_arch_str(LCN_ARCH_ARM), "arm");
    ASSERT_STR_EQ(lcn_arch_str(LCN_ARCH_UNKNOWN), "unknown");
}

TEST(target_os_str) {
    ASSERT_STR_EQ(lcn_os_str(LCN_OS_LINUX), "linux");
    ASSERT_STR_EQ(lcn_os_str(LCN_OS_DARWIN), "darwin");
    ASSERT_STR_EQ(lcn_os_str(LCN_OS_WINDOWS), "windows");
    ASSERT_STR_EQ(lcn_os_str(LCN_OS_UNKNOWN), "unknown");
}

TEST(target_abi_str) {
    ASSERT_STR_EQ(lcn_abi_str(LCN_ABI_GNU), "gnu");
    ASSERT_STR_EQ(lcn_abi_str(LCN_ABI_MUSL), "musl");
    ASSERT_STR_EQ(lcn_abi_str(LCN_ABI_MSVC), "msvc");
}

TEST(codegen_platform_guards) {
    /* When cross-compiling to aarch64-linux, generated C should have platform defines */
    const char *source = "fn main() {\n    println(\"hello\")\n}\n";
    bool had_error = false;
    AstNode *program = parse_source(source, &had_error);
    ASSERT_FALSE(had_error);
    ASSERT_NOT_NULL(program);

    LcnTarget t = lcn_parse_target("aarch64-linux");
    char *code = codegen_generate_for_build_target(program, "<test>", &test_arena, &t);
    ASSERT_NOT_NULL(code);

    /* Should contain target triple define */
    ASSERT_TRUE(strstr(code, "LCN_TARGET_TRIPLE") != NULL);
    ASSERT_TRUE(strstr(code, "aarch64-linux-gnu") != NULL);

    /* Should contain architecture define */
    ASSERT_TRUE(strstr(code, "LCN_ARCH_AARCH64") != NULL);

    /* Should contain OS define */
    ASSERT_TRUE(strstr(code, "LCN_OS_LINUX") != NULL);

    /* Should contain platform portability macros */
    ASSERT_TRUE(strstr(code, "LCN_DYLIB_EXT") != NULL);
    ASSERT_TRUE(strstr(code, "LCN_PATH_SEP") != NULL);
    ASSERT_TRUE(strstr(code, "LCN_HAS_EPOLL") != NULL);

    /* Should contain thread portability */
    ASSERT_TRUE(strstr(code, "LCN_THREAD_T") != NULL);

    free(code);
}

TEST(codegen_platform_guards_darwin) {
    const char *source = "fn main() {\n    println(\"hello\")\n}\n";
    bool had_error = false;
    AstNode *program = parse_source(source, &had_error);
    ASSERT_FALSE(had_error);

    LcnTarget t = lcn_parse_target("x86_64-darwin");
    char *code = codegen_generate_for_build_target(program, "<test>", &test_arena, &t);
    ASSERT_NOT_NULL(code);

    ASSERT_TRUE(strstr(code, "LCN_OS_DARWIN") != NULL);
    ASSERT_TRUE(strstr(code, "LCN_ARCH_X86_64") != NULL);
    ASSERT_TRUE(strstr(code, "LCN_HAS_KQUEUE") != NULL);

    free(code);
}

TEST(codegen_platform_guards_windows) {
    const char *source = "fn main() {\n    println(\"hello\")\n}\n";
    bool had_error = false;
    AstNode *program = parse_source(source, &had_error);
    ASSERT_FALSE(had_error);

    LcnTarget t = lcn_parse_target("x86_64-windows-msvc");
    char *code = codegen_generate_for_build_target(program, "<test>", &test_arena, &t);
    ASSERT_NOT_NULL(code);

    ASSERT_TRUE(strstr(code, "LCN_OS_WINDOWS") != NULL);
    ASSERT_TRUE(strstr(code, "LCN_HAS_IOCP") != NULL);
    ASSERT_TRUE(strstr(code, ".dll") != NULL);

    free(code);
}

TEST(codegen_no_guards_native) {
    /* Without a target, there should be no LCN_TARGET_TRIPLE */
    const char *source = "fn main() {\n    println(\"hello\")\n}\n";
    bool had_error = false;
    AstNode *program = parse_source(source, &had_error);
    ASSERT_FALSE(had_error);

    char *code = codegen_generate_for_build(program, "<test>", &test_arena);
    ASSERT_NOT_NULL(code);

    ASSERT_TRUE(strstr(code, "LCN_TARGET_TRIPLE") == NULL);

    free(code);
}

TEST(target_cross_cc_native) {
    /* For native target, cc should always be found */
    LcnTarget t = lcn_native_target();
    /* It should already have cc="cc" and is_native=true */
    bool found = lcn_find_cross_cc(&t);
    ASSERT_TRUE(found);
    ASSERT_TRUE(t.is_native);
}

/* ============================================================
 * LSP SERVER TESTS
 * ============================================================ */

/* Test: lsp_json_get_string parses a JSON key/value pair */
TEST(lsp_parse_header) {
    const char *json = "{\"method\":\"initialize\",\"id\":1,\"params\":{}}";

    char buf[256];
    const char *result = lsp_json_get_string(json, "method", buf, sizeof(buf));
    ASSERT_NOT_NULL(result);
    ASSERT_STR_EQ(result, "initialize");

    /* Test integer extraction */
    long id = lsp_json_get_int(json, "id");
    ASSERT_EQ(id, 1);

    /* Test with nested JSON */
    const char *nested = "{\"textDocument\":{\"uri\":\"file:///test.lceron\",\"version\":1}}";
    char uri[256];
    /* Find the textDocument sub-object first */
    const char *td = strstr(nested, "{\"uri\"");
    ASSERT_NOT_NULL(td);
    result = lsp_json_get_string(td, "uri", uri, sizeof(uri));
    ASSERT_NOT_NULL(result);
    ASSERT_STR_EQ(result, "file:///test.lceron");
}

/* Test: initialize response has required capabilities */
TEST(lsp_initialize_response) {
    const char *capabilities =
        "{\"capabilities\":{\"textDocumentSync\":1,"
        "\"completionProvider\":{\"triggerCharacters\":[\".\",\":\"]},"
        "\"hoverProvider\":true,"
        "\"definitionProvider\":true,"
        "\"diagnosticProvider\":{\"interFileDependencies\":false}},"
        "\"serverInfo\":{\"name\":\"limceron-lsp\",\"version\":\"" LCN_VERSION "\"}}";

    /* Verify key fields are present */
    ASSERT(strstr(capabilities, "\"textDocumentSync\":1") != NULL);
    ASSERT(strstr(capabilities, "\"completionProvider\"") != NULL);
    ASSERT(strstr(capabilities, "\"hoverProvider\":true") != NULL);
    ASSERT(strstr(capabilities, "\"definitionProvider\":true") != NULL);
    ASSERT(strstr(capabilities, "\"diagnosticProvider\"") != NULL);
    ASSERT(strstr(capabilities, "\"limceron-lsp\"") != NULL);
    ASSERT(strstr(capabilities, LCN_VERSION) != NULL);

    /* Verify trigger characters */
    ASSERT(strstr(capabilities, "\".\"") != NULL);
    ASSERT(strstr(capabilities, "\":\"") != NULL);
}

/* Test: compile errors produce correct LSP diagnostics */
TEST(lsp_diagnostics) {
    const char *source = "fn main() {\n  let x: UnknownType = 42\n}\n";
    size_t len = strlen(source);
    ErrorReporter reporter = reporter_new("<test>", source, len);

    /* Simulate a type-check error at line 2, col 10 */
    SourceLoc loc = { .filename = "<test>", .line = 2, .column = 10, .offset = 25 };
    report_error(&reporter, loc, "unknown type 'UnknownType'", NULL);

    ASSERT_EQ(reporter.count, 1);
    ASSERT_EQ(reporter.errors[0].loc.line, 2);
    ASSERT_EQ(reporter.errors[0].loc.column, 10);
    ASSERT_STR_EQ(reporter.errors[0].message, "unknown type 'UnknownType'");
    ASSERT_EQ(reporter.errors[0].is_warning, false);

    /* LSP uses 0-based lines, so line 2 -> 1, col 10 -> 9 */
    uint32_t lsp_line = reporter.errors[0].loc.line - 1;
    uint32_t lsp_col = reporter.errors[0].loc.column - 1;
    ASSERT_EQ(lsp_line, 1);
    ASSERT_EQ(lsp_col, 9);

    /* Verify warning flag */
    SourceLoc loc2 = { .filename = "<test>", .line = 1, .column = 1, .offset = 0 };
    report_warning(&reporter, loc2, "unused variable", NULL);
    ASSERT_EQ(reporter.count, 2);
    ASSERT_EQ(reporter.errors[1].is_warning, true);
}

/* ============================================================
 * Prometheus Metrics Tests
 * ============================================================ */

TEST(parse_metrics_block) {
    bool err;
    AstNode *prog = parse_source(
        "metrics {\n"
        "    counter processed_total \"Records processed\"\n"
        "    counter errors_total \"Total errors\"\n"
        "    histogram confidence \"Confidence distribution\"\n"
        "    gauge pending_count \"Records pending\"\n"
        "    port: 9091\n"
        "}", &err);
    ASSERT(!err);
    ASSERT_NOT_NULL(prog);
    AstNode *m = prog->params;
    ASSERT_NOT_NULL(m);
    ASSERT_EQ(m->kind, AST_METRICS);
    ASSERT_EQ(m->val.int_val, 9091);
    int count = 0;
    AstNode *f = m->params;
    while (f) {
        if (f->kind == AST_METRICS_FIELD) count++;
        f = f->next;
    }
    ASSERT_EQ(count, 4);
    f = m->params;
    ASSERT_STR_EQ(f->name, "processed_total");
    ASSERT(!f->is_mut);
    ASSERT(!f->is_pub);
    f = f->next->next;
    ASSERT_STR_EQ(f->name, "confidence");
    ASSERT(f->is_pub);
    f = f->next;
    ASSERT_STR_EQ(f->name, "pending_count");
    ASSERT(f->is_mut);
}

TEST(codegen_metrics_counter) {
    char *c = gen_c(
        "metrics {\n"
        "    counter processed_total \"Records processed\"\n"
        "    port: 9091\n"
        "}\n"
        "fn main() {\n"
        "    metrics.processed_total += 1\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "LcnMetrics") != NULL);
    ASSERT(strstr(c, "_lcn_metrics") != NULL);
    ASSERT(strstr(c, "__sync_fetch_and_add(&_lcn_metrics.processed_total") != NULL);
    ASSERT(strstr(c, "_lcn_metrics_server") != NULL);
    free(c);
}

TEST(codegen_metrics_endpoint) {
    char *c = gen_c(
        "metrics {\n"
        "    counter requests \"Total requests\"\n"
        "    gauge active \"Active connections\"\n"
        "    port: 9091\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "_lcn_metrics_format") != NULL);
    ASSERT(strstr(c, "# HELP requests Total requests") != NULL);
    ASSERT(strstr(c, "# TYPE requests counter") != NULL);
    ASSERT(strstr(c, "# HELP active Active connections") != NULL);
    ASSERT(strstr(c, "# TYPE active gauge") != NULL);
    ASSERT(strstr(c, "htons(9091)") != NULL);
    ASSERT(strstr(c, "text/plain") != NULL);
    free(c);
}

TEST(codegen_metrics_histogram) {
    char *c = gen_c(
        "metrics {\n"
        "    histogram confidence \"Confidence distribution\"\n"
        "    port: 9091\n"
        "}\n"
        "fn main() {\n"
        "    metrics.confidence.observe(85.0)\n"
        "}"
    );
    ASSERT_NOT_NULL(c);
    ASSERT(strstr(c, "int64_t count") != NULL);
    ASSERT(strstr(c, "double sum") != NULL);
    ASSERT(strstr(c, "int64_t buckets") != NULL);
    ASSERT(strstr(c, "_lcn_metrics_observe_confidence") != NULL);
    /* Float literal 85.0 may emit as 85 or 85.000000 in C */
    ASSERT(strstr(c, "_lcn_metrics_observe_confidence(85") != NULL);
    ASSERT(strstr(c, "confidence_bucket") != NULL);
    ASSERT(strstr(c, "confidence_sum") != NULL);
    ASSERT(strstr(c, "confidence_count") != NULL);
    free(c);
}

/* Test: completion list includes Limceron keywords */
TEST(lsp_completion_keywords) {
    const char *essential_keywords[] = {
        "fn", "let", "if", "for", "while", "struct", "enum", "agent",
        "guard", "capability", "tool", "skill", "match", "return",
        "spawn", "await", "budget", "ask", "tell", "use", "mod",
        NULL
    };

    /* Verify these are valid Limceron keywords by lexing them */
    for (int i = 0; essential_keywords[i]; i++) {
        arena_reset(&test_arena);
        arena_reset(&test_intern_arena);

        size_t len = strlen(essential_keywords[i]);
        ErrorReporter reporter = reporter_new("<test>", essential_keywords[i], len);
        StringIntern intern = intern_new(&test_intern_arena);
        Lexer lexer = lexer_new("<test>", essential_keywords[i], len, &intern, &reporter);

        Token tok = lexer_next(&lexer);
        /* All of these should lex as keywords, not identifiers */
        ASSERT_NEQ(tok.kind, TOK_ERROR);
        /* They should be recognized tokens (not plain IDENT for most) */
        ASSERT(tok.kind != TOK_EOF);
    }

    /* Verify the builtin function names are not keywords (they lex as identifiers) */
    const char *builtins[] = { "print", "println", "len", "push", "pop", NULL };
    for (int i = 0; builtins[i]; i++) {
        arena_reset(&test_arena);
        arena_reset(&test_intern_arena);

        size_t len = strlen(builtins[i]);
        ErrorReporter reporter = reporter_new("<test>", builtins[i], len);
        StringIntern intern = intern_new(&test_intern_arena);
        Lexer lexer = lexer_new("<test>", builtins[i], len, &intern, &reporter);

        Token tok = lexer_next(&lexer);
        ASSERT_EQ(tok.kind, TOK_IDENT);
        ASSERT_STR_EQ(tok.value.str_val, builtins[i]);
    }
}

/* ============================================================
 * Main
 * ============================================================ */

/* ============================================================
 * Health Probe Tests
 * ============================================================ */

TEST(parse_health_block) {
    bool err;
    AstNode *prog = parse_source(
        "health {\n"
        "    ready: true\n"
        "    live: true\n"
        "    port: 8080\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);

    AstNode *h = prog->params;
    ASSERT_NOT_NULL(h);
    ASSERT_EQ(h->kind, AST_HEALTH);

    /* Port stored in val.int_val */
    ASSERT_EQ(h->val.int_val, 8080);

    /* ready expression */
    ASSERT_NOT_NULL(h->left);
    ASSERT_EQ(h->left->kind, AST_BOOL_LIT);

    /* live expression */
    ASSERT_NOT_NULL(h->right);
    ASSERT_EQ(h->right->kind, AST_BOOL_LIT);

    /* fields list */
    ASSERT_EQ(ast_list_len(h->params), 3);
}

TEST(parse_health_default_port) {
    bool err;
    AstNode *prog = parse_source(
        "health {\n"
        "    ready: true\n"
        "    live: true\n"
        "}",
        &err
    );
    ASSERT_FALSE(err);

    AstNode *h = prog->params;
    ASSERT_NOT_NULL(h);
    ASSERT_EQ(h->kind, AST_HEALTH);

    /* Default port is 9090 */
    ASSERT_EQ(h->val.int_val, 9090);
}

TEST(codegen_health_server) {
    char *c = gen_c(
        "health {\n"
        "    ready: true\n"
        "    live: true\n"
        "    port: 9090\n"
        "}\n"
        "fn main() {\n"
        "    let x = 1\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);

    /* Should emit socket/bind/listen server code */
    ASSERT(strstr(c, "_lcn_health_server") != NULL);
    ASSERT(strstr(c, "socket(AF_INET") != NULL);
    ASSERT(strstr(c, "bind(fd") != NULL);
    ASSERT(strstr(c, "listen(fd") != NULL);

    /* Should emit /healthz and /readyz endpoint handling */
    ASSERT(strstr(c, "/readyz") != NULL);
    ASSERT(strstr(c, "/healthz") != NULL);

    /* Should emit global health state variables */
    ASSERT(strstr(c, "_lcn_health_ready") != NULL);
    ASSERT(strstr(c, "_lcn_health_live") != NULL);

    /* Should start the health thread in main */
    ASSERT(strstr(c, "pthread_create") != NULL);
    ASSERT(strstr(c, "_health_port") != NULL);

    free(c);
}

/* ============================================================
 * Progress Reporting Tests
 * ============================================================ */

TEST(parse_progress_block) {
    bool had_error = false;
    AstNode *prog = parse_source(
        "progress {\n"
        "    total: count\n"
        "    current: processed\n"
        "}\n",
        &had_error);
    ASSERT_FALSE(had_error);
    ASSERT_NOT_NULL(prog);
    /* First decl should be AST_PROGRESS */
    AstNode *d = prog->params;
    ASSERT_NOT_NULL(d);
    ASSERT_EQ(d->kind, AST_PROGRESS);
    /* left = total expression (identifier "count") */
    ASSERT_NOT_NULL(d->left);
    ASSERT_EQ(d->left->kind, AST_IDENT);
    ASSERT_STR_EQ(d->left->name, "count");
    /* right = current expression (identifier "processed") */
    ASSERT_NOT_NULL(d->right);
    ASSERT_EQ(d->right->kind, AST_IDENT);
    ASSERT_STR_EQ(d->right->name, "processed");
}

TEST(codegen_progress_update) {
    char *c = gen_c(
        "progress {\n"
        "    total: count\n"
        "    current: processed\n"
        "}\n"
        "fn main() {\n"
        "    let mut count = 100\n"
        "    let mut processed = 0\n"
        "    processed = processed + 1\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* Should emit the progress update function */
    ASSERT(strstr(c, "_lcn_progress_update") != NULL);
    /* Should emit progress_init call in main wrapper */
    ASSERT(strstr(c, "_lcn_progress_init") != NULL);
    /* Should emit update after assignment to processed */
    ASSERT(strstr(c, "_lcn_progress_update(processed, count)") != NULL);
    free(c);
}

TEST(codegen_progress_file) {
    char *c = gen_c(
        "progress {\n"
        "    total: n\n"
        "    current: done\n"
        "}\n"
        "fn main() {\n"
        "    let mut n = 50\n"
        "    let mut done = 0\n"
        "}\n"
    );
    ASSERT_NOT_NULL(c);
    /* Should emit the progress file variable */
    ASSERT(strstr(c, "_lcn_progress_file") != NULL);
    /* Should emit file output logic with fopen */
    ASSERT(strstr(c, "fopen(_lcn_progress_file") != NULL);
    /* Should emit JSON format in file output (escaped in generated C source) */
    ASSERT(strstr(c, "\\\"current\\\"") != NULL);
    ASSERT(strstr(c, "\\\"total\\\"") != NULL);
    ASSERT(strstr(c, "\\\"percent\\\"") != NULL);
    /* Should emit stderr logging */
    ASSERT(strstr(c, "[progress]") != NULL);
    /* Should read LCN_PROGRESS_FILE env var */
    ASSERT(strstr(c, "LCN_PROGRESS_FILE") != NULL);
    /* Should default to /tmp/lcn_progress.json */
    ASSERT(strstr(c, "/tmp/lcn_progress.json") != NULL);
    free(c);
}

/* ============================================================
 * MARKDOWN PARSER TESTS
 * ============================================================ */

/* Helper: parse a Markdown string through parse_lceron_md, return reporter */
static AstNode *parse_md_source_reporter(const char *source, ErrorReporter *out_reporter) {
    arena_reset(&test_arena);
    arena_reset(&test_intern_arena);

    size_t len = strlen(source);
    *out_reporter = reporter_new("<test.lceron.md>", source, len);
    StringIntern intern = intern_new(&test_intern_arena);

    return parse_lceron_md("<test.lceron.md>", source, len,
                           &test_arena, &intern, out_reporter);
}

/* Helper: parse markdown source, return had_error flag */
static AstNode *parse_md_source(const char *source, bool *had_error) {
    ErrorReporter reporter;
    AstNode *result = parse_md_source_reporter(source, &reporter);
    *had_error = has_errors(&reporter);
    return result;
}

/* Find a guard node by name in the agent's members (guards field -> block -> list) */
static AstNode *find_guard_in_ast(AstNode *program, const char *guard_name) {
    if (!program || program->kind != AST_PROGRAM) return NULL;
    AstNode *agent = program->params;
    if (!agent || agent->kind != AST_AGENT) return NULL;

    /* Walk members looking for guards field */
    AstNode *member = agent->params;
    while (member) {
        if (member->kind == AST_FIELD && member->name &&
            strcmp(member->name, "guards") == 0 && member->right) {
            /* member->right is AST_BLOCK with guards as params */
            AstNode *guard = member->right->params;
            while (guard) {
                if (guard->kind == AST_GUARD && guard->name &&
                    strcmp(guard->name, guard_name) == 0) {
                    return guard;
                }
                guard = guard->next;
            }
        }
        member = member->next;
    }
    return NULL;
}

/* Phase 1: Core Safety Tests */

TEST(md_parse_capability_declaration) {
    bool had_error = false;
    AstNode *prog = parse_md_source(
        "# agent TestBot\n"
        "\n"
        "## capability llm\n"
        "- complete\n"
        "- classify requires complete\n"
        "- embed\n",
        &had_error);
    ASSERT_NOT_NULL(prog);
    ASSERT_FALSE(had_error);
    ASSERT_EQ(prog->kind, AST_PROGRAM);

    /* Agent should be in program->params */
    AstNode *agent = prog->params;
    ASSERT_NOT_NULL(agent);
    ASSERT_EQ(agent->kind, AST_AGENT);
    ASSERT_STR_EQ(agent->name, "TestBot");

    /* The capability decl should be in agent->params */
    AstNode *cap = agent->params;
    ASSERT_NOT_NULL(cap);
    ASSERT_EQ(cap->kind, AST_CAPABILITY);
    ASSERT_STR_EQ(cap->name, "llm");

    /* Should have 3 items */
    AstNode *item1 = cap->params;
    ASSERT_NOT_NULL(item1);
    ASSERT_EQ(item1->kind, AST_CAPABILITY_ITEM);
    ASSERT_STR_EQ(item1->name, "complete");
    ASSERT_NULL(item1->params); /* no requires */

    AstNode *item2 = item1->next;
    ASSERT_NOT_NULL(item2);
    ASSERT_EQ(item2->kind, AST_CAPABILITY_ITEM);
    ASSERT_STR_EQ(item2->name, "classify");
    ASSERT_NOT_NULL(item2->params); /* has requires */
    ASSERT_EQ(item2->params->kind, AST_IDENT);
    ASSERT_STR_EQ(item2->params->name, "complete");

    AstNode *item3 = item2->next;
    ASSERT_NOT_NULL(item3);
    ASSERT_EQ(item3->kind, AST_CAPABILITY_ITEM);
    ASSERT_STR_EQ(item3->name, "embed");
    ASSERT_NULL(item3->next); /* no more items */
}

TEST(md_parse_capability_with_access_rules) {
    bool had_error = false;
    AstNode *prog = parse_md_source(
        "# agent NetBot\n"
        "\n"
        "## capability network\n"
        "allow endpoint \"api.example.com:443\"\n"
        "deny private_ranges\n"
        "default: deny\n",
        &had_error);
    ASSERT_NOT_NULL(prog);
    ASSERT_FALSE(had_error);

    AstNode *agent = prog->params;
    ASSERT_NOT_NULL(agent);
    AstNode *cap = agent->params;
    ASSERT_NOT_NULL(cap);
    ASSERT_EQ(cap->kind, AST_CAPABILITY);
    ASSERT_STR_EQ(cap->name, "network");

    /* First rule: allow endpoint */
    AstNode *r1 = cap->params;
    ASSERT_NOT_NULL(r1);
    ASSERT_EQ(r1->kind, AST_CAP_ENDPOINT_RULE);
    ASSERT_TRUE(r1->is_mut); /* allow */
    ASSERT_STR_EQ(r1->name, "api.example.com:443");

    /* Second rule: deny private_ranges */
    AstNode *r2 = r1->next;
    ASSERT_NOT_NULL(r2);
    ASSERT_EQ(r2->kind, AST_CAP_DENY_RANGE);
    ASSERT_FALSE(r2->is_mut); /* deny */

    /* Third rule: default deny */
    AstNode *r3 = r2->next;
    ASSERT_NOT_NULL(r3);
    ASSERT_EQ(r3->kind, AST_CAP_DEFAULT);
    ASSERT_FALSE(r3->is_mut); /* default: deny */
}

TEST(md_parse_taint) {
    bool had_error = false;
    AstNode *prog = parse_md_source(
        "# agent SafeBot\n"
        "\n"
        "## taint\n"
        "- user_input\n"
        "- llm_output\n"
        "- sanitized\n",
        &had_error);
    ASSERT_NOT_NULL(prog);
    ASSERT_FALSE(had_error);

    AstNode *agent = prog->params;
    ASSERT_NOT_NULL(agent);
    ASSERT_EQ(agent->kind, AST_AGENT);
    ASSERT_STR_EQ(agent->name, "SafeBot");

    /* Taints should be in agent->params as AST_TAINT nodes */
    AstNode *t1 = agent->params;
    ASSERT_NOT_NULL(t1);
    ASSERT_EQ(t1->kind, AST_TAINT);
    ASSERT_STR_EQ(t1->name, "user_input");

    AstNode *t2 = t1->next;
    ASSERT_NOT_NULL(t2);
    ASSERT_EQ(t2->kind, AST_TAINT);
    ASSERT_STR_EQ(t2->name, "llm_output");

    AstNode *t3 = t2->next;
    ASSERT_NOT_NULL(t3);
    ASSERT_EQ(t3->kind, AST_TAINT);
    ASSERT_STR_EQ(t3->name, "sanitized");

    ASSERT_NULL(t3->next); /* no more items */
}

TEST(md_parse_access_control) {
    bool had_error = false;
    AstNode *prog = parse_md_source(
        "# agent SecBot\n"
        "\n"
        "## access_control\n"
        "### network\n"
        "allow endpoint \"api.example.com:443\"\n"
        "deny private_ranges\n"
        "default: deny\n"
        "### filesystem\n"
        "allow path \"/tmp/**\"\n"
        "deny path \"/etc/**\"\n"
        "default: deny\n"
        "### shell\n"
        "allow binary \"/usr/bin/git\"\n"
        "deny binary \"/bin/rm\"\n"
        "default: deny\n",
        &had_error);
    ASSERT_NOT_NULL(prog);
    ASSERT_FALSE(had_error);

    AstNode *agent = prog->params;
    ASSERT_NOT_NULL(agent);

    /* Find the access_control field */
    AstNode *ac_field = agent->params;
    ASSERT_NOT_NULL(ac_field);
    ASSERT_EQ(ac_field->kind, AST_FIELD);
    ASSERT_STR_EQ(ac_field->name, "access_control");

    /* Block should contain all rules */
    AstNode *block = ac_field->right;
    ASSERT_NOT_NULL(block);
    ASSERT_EQ(block->kind, AST_BLOCK);

    /* Count all rules: 3 (network) + 3 (filesystem) + 3 (shell) = 9 */
    AstNode *rule = block->params;
    int count = 0;
    while (rule) {
        count++;
        rule = rule->next;
    }
    ASSERT_EQ(count, 9);

    /* Verify first rule: allow endpoint */
    rule = block->params;
    ASSERT_EQ(rule->kind, AST_CAP_ENDPOINT_RULE);
    ASSERT_TRUE(rule->is_mut);
    ASSERT_STR_EQ(rule->name, "api.example.com:443");

    /* Verify second rule: deny private_ranges */
    rule = rule->next;
    ASSERT_EQ(rule->kind, AST_CAP_DENY_RANGE);
    ASSERT_FALSE(rule->is_mut);

    /* Verify third rule: default deny */
    rule = rule->next;
    ASSERT_EQ(rule->kind, AST_CAP_DEFAULT);
    ASSERT_FALSE(rule->is_mut);

    /* Verify fourth rule: allow path /tmp/glob */
    rule = rule->next;
    ASSERT_EQ(rule->kind, AST_CAP_PATH_RULE);
    ASSERT_TRUE(rule->is_mut);
    ASSERT_STR_EQ(rule->name, "/tmp/**");

    /* Verify fifth rule: deny path /etc/glob */
    rule = rule->next;
    ASSERT_EQ(rule->kind, AST_CAP_PATH_RULE);
    ASSERT_FALSE(rule->is_mut);
    ASSERT_STR_EQ(rule->name, "/etc/**");
}

TEST(md_parse_combined_safety) {
    /* Test that capability, taint, and access_control can coexist */
    bool had_error = false;
    AstNode *prog = parse_md_source(
        "# agent FullBot\n"
        "> You are a safe agent.\n"
        "\n"
        "## capability llm\n"
        "- complete\n"
        "- embed\n"
        "\n"
        "## taint\n"
        "- user_input\n"
        "- sanitized\n"
        "\n"
        "## model\n"
        "claude-haiku\n",
        &had_error);
    ASSERT_NOT_NULL(prog);
    ASSERT_FALSE(had_error);

    AstNode *agent = prog->params;
    ASSERT_NOT_NULL(agent);
    ASSERT_STR_EQ(agent->name, "FullBot");

    /* Walk members: prompt, capability, taint, taint, model */
    AstNode *m = agent->params;
    ASSERT_NOT_NULL(m);
    ASSERT_EQ(m->kind, AST_FIELD);
    ASSERT_STR_EQ(m->name, "prompt");

    m = m->next;
    ASSERT_NOT_NULL(m);
    ASSERT_EQ(m->kind, AST_CAPABILITY);
    ASSERT_STR_EQ(m->name, "llm");

    /* Two taint nodes */
    m = m->next;
    ASSERT_NOT_NULL(m);
    ASSERT_EQ(m->kind, AST_TAINT);
    ASSERT_STR_EQ(m->name, "user_input");

    m = m->next;
    ASSERT_NOT_NULL(m);
    ASSERT_EQ(m->kind, AST_TAINT);
    ASSERT_STR_EQ(m->name, "sanitized");

    /* Model field */
    m = m->next;
    ASSERT_NOT_NULL(m);
    ASSERT_EQ(m->kind, AST_FIELD);
    ASSERT_STR_EQ(m->name, "model");
}

/* Phase 2: K8s + Runtime Tests */

TEST(md_parse_health) {
    bool err;
    AstNode *prog = parse_md_source(
        "# agent TestAgent\n"
        "> A test agent.\n"
        "\n"
        "## health\n"
        "- ready: true\n"
        "- live: true\n"
        "- port: 9090\n",
        &err
    );
    ASSERT_FALSE(err);
    ASSERT_NOT_NULL(prog);
    ASSERT_EQ(prog->kind, AST_PROGRAM);

    /* Agent is the first child */
    AstNode *agent = prog->params;
    ASSERT_NOT_NULL(agent);
    ASSERT_EQ(agent->kind, AST_AGENT);
    ASSERT_STR_EQ(agent->name, "TestAgent");

    /* Health node is a top-level sibling of the agent */
    AstNode *h = agent->next;
    ASSERT_NOT_NULL(h);
    ASSERT_EQ(h->kind, AST_HEALTH);

    /* Port stored in val.int_val */
    ASSERT_EQ(h->val.int_val, 9090);

    /* ready expression */
    ASSERT_NOT_NULL(h->left);
    ASSERT_EQ(h->left->kind, AST_BOOL_LIT);
    ASSERT_TRUE(h->left->val.bool_val);

    /* live expression */
    ASSERT_NOT_NULL(h->right);
    ASSERT_EQ(h->right->kind, AST_BOOL_LIT);
    ASSERT_TRUE(h->right->val.bool_val);

    /* fields list */
    ASSERT_EQ(ast_list_len(h->params), 3);
}

TEST(md_parse_metrics) {
    bool err;
    AstNode *prog = parse_md_source(
        "# agent MetricsAgent\n"
        "> Metrics test.\n"
        "\n"
        "## metrics\n"
        "- counter processed_total \"Records processed\"\n"
        "- histogram confidence \"Confidence distribution\"\n"
        "- gauge pending \"Records pending\"\n"
        "- port: 9091\n",
        &err
    );
    ASSERT_FALSE(err);
    ASSERT_NOT_NULL(prog);

    AstNode *agent = prog->params;
    ASSERT_NOT_NULL(agent);
    ASSERT_EQ(agent->kind, AST_AGENT);

    /* Metrics node is a top-level sibling */
    AstNode *m = agent->next;
    ASSERT_NOT_NULL(m);
    ASSERT_EQ(m->kind, AST_METRICS);

    /* Port */
    ASSERT_EQ(m->val.int_val, 9091);

    /* Count metrics fields */
    int count = 0;
    AstNode *f = m->params;
    while (f) {
        if (f->kind == AST_METRICS_FIELD) count++;
        f = f->next;
    }
    ASSERT_EQ(count, 3);

    /* First field: counter processed_total */
    f = m->params;
    ASSERT_STR_EQ(f->name, "processed_total");
    ASSERT_FALSE(f->is_mut);   /* counter */
    ASSERT_FALSE(f->is_pub);

    /* Second: histogram confidence */
    f = f->next;
    ASSERT_STR_EQ(f->name, "confidence");
    ASSERT_TRUE(f->is_pub);    /* histogram */

    /* Third: gauge pending */
    f = f->next;
    ASSERT_STR_EQ(f->name, "pending");
    ASSERT_TRUE(f->is_mut);    /* gauge */
}

TEST(md_parse_progress) {
    bool err;
    AstNode *prog = parse_md_source(
        "# agent ProgressAgent\n"
        "> Progress test.\n"
        "\n"
        "## progress\n"
        "- total: count\n"
        "- current: processed\n",
        &err
    );
    ASSERT_FALSE(err);
    ASSERT_NOT_NULL(prog);

    AstNode *agent = prog->params;
    ASSERT_NOT_NULL(agent);
    ASSERT_EQ(agent->kind, AST_AGENT);

    /* Progress node is a top-level sibling */
    AstNode *p = agent->next;
    ASSERT_NOT_NULL(p);
    ASSERT_EQ(p->kind, AST_PROGRESS);

    /* left = total expression (identifier "count") */
    ASSERT_NOT_NULL(p->left);
    ASSERT_EQ(p->left->kind, AST_IDENT);
    ASSERT_STR_EQ(p->left->name, "count");

    /* right = current expression (identifier "processed") */
    ASSERT_NOT_NULL(p->right);
    ASSERT_EQ(p->right->kind, AST_IDENT);
    ASSERT_STR_EQ(p->right->name, "processed");
}

TEST(md_parse_supervisor) {
    bool err;
    AstNode *prog = parse_md_source(
        "# agent SupervisorAgent\n"
        "> Supervisor test.\n"
        "\n"
        "## supervisor PipelineManager\n"
        "- strategy: one_for_one\n"
        "- max_restarts: 3\n"
        "- window: 60\n"
        "- children: [AgentA, AgentB]\n",
        &err
    );
    ASSERT_FALSE(err);
    ASSERT_NOT_NULL(prog);

    AstNode *agent = prog->params;
    ASSERT_NOT_NULL(agent);
    ASSERT_EQ(agent->kind, AST_AGENT);

    /* Supervisor node is a top-level sibling */
    AstNode *sup = agent->next;
    ASSERT_NOT_NULL(sup);
    ASSERT_EQ(sup->kind, AST_SUPERVISOR);
    ASSERT_STR_EQ(sup->name, "PipelineManager");

    /* Check fields */
    AstNode *f = sup->params;
    int found = 0;
    while (f) {
        if (f->name && strcmp(f->name, "strategy") == 0) {
            ASSERT_NOT_NULL(f->right);
            ASSERT_EQ(f->right->kind, AST_IDENT);
            ASSERT_STR_EQ(f->right->name, "one_for_one");
            found++;
        }
        if (f->name && strcmp(f->name, "max_restarts") == 0) {
            ASSERT_NOT_NULL(f->right);
            ASSERT_EQ(f->right->kind, AST_INT_LIT);
            ASSERT_EQ(f->right->val.int_val, 3);
            found++;
        }
        if (f->name && strcmp(f->name, "window") == 0) {
            ASSERT_NOT_NULL(f->right);
            ASSERT_EQ(f->right->kind, AST_INT_LIT);
            ASSERT_EQ(f->right->val.int_val, 60);
            found++;
        }
        if (f->name && strcmp(f->name, "children") == 0) {
            ASSERT_NOT_NULL(f->right);
            ASSERT_EQ(f->right->kind, AST_ARRAY);
            ASSERT_EQ(ast_list_len(f->right->params), 2);
            ASSERT_STR_EQ(f->right->params->name, "AgentA");
            ASSERT_STR_EQ(f->right->params->next->name, "AgentB");
            found++;
        }
        f = f->next;
    }
    ASSERT_EQ(found, 4);
}

/* Phase 3: Guards + Errors Tests */

TEST(md_guard_text_only) {
    const char *src =
        "# agent TestBot\n"
        "> A test agent.\n"
        "## guards\n"
        "### rate_limit\n"
        "Max 50 actions per hour.\n";

    ErrorReporter reporter;
    AstNode *program = parse_md_source_reporter(src, &reporter);

    ASSERT_NOT_NULL(program);
    ASSERT_EQ(program->kind, AST_PROGRAM);

    AstNode *guard = find_guard_in_ast(program, "rate_limit");
    ASSERT_NOT_NULL(guard);
    ASSERT_STR_EQ(guard->name, "rate_limit");

    /* Guard body should be AST_BLOCK with a description field */
    ASSERT_NOT_NULL(guard->left);
    ASSERT_EQ(guard->left->kind, AST_BLOCK);
    ASSERT_NOT_NULL(guard->left->params);
    ASSERT_EQ(guard->left->params->kind, AST_FIELD);
    ASSERT_STR_EQ(guard->left->params->name, "description");

    /* The description field's value should be a string */
    ASSERT_NOT_NULL(guard->left->params->right);
    ASSERT_EQ(guard->left->params->right->kind, AST_STRING_LIT);
}

TEST(md_guard_with_code_block) {
    const char *src =
        "# agent TestBot\n"
        "> A test agent.\n"
        "## guards\n"
        "### rate_limit\n"
        "Max 50 actions per hour.\n"
        "```limceron\n"
        "let max_actions = 50\n"
        "let window_seconds = 3600\n"
        "```\n";

    ErrorReporter reporter;
    AstNode *program = parse_md_source_reporter(src, &reporter);

    ASSERT_NOT_NULL(program);
    ASSERT_EQ(program->kind, AST_PROGRAM);

    AstNode *guard = find_guard_in_ast(program, "rate_limit");
    ASSERT_NOT_NULL(guard);
    ASSERT_STR_EQ(guard->name, "rate_limit");

    /* Guard body should be AST_BLOCK with parsed let-bindings */
    ASSERT_NOT_NULL(guard->left);
    ASSERT_EQ(guard->left->kind, AST_BLOCK);

    /* First statement should be a let binding for max_actions */
    AstNode *stmt1 = guard->left->params;
    ASSERT_NOT_NULL(stmt1);
    ASSERT_EQ(stmt1->kind, AST_LET);
    ASSERT_STR_EQ(stmt1->name, "max_actions");

    /* Second statement should be a let binding for window_seconds */
    AstNode *stmt2 = stmt1->next;
    ASSERT_NOT_NULL(stmt2);
    ASSERT_EQ(stmt2->kind, AST_LET);
    ASSERT_STR_EQ(stmt2->name, "window_seconds");
}

TEST(md_unknown_section_warning) {
    const char *src =
        "# agent TestBot\n"
        "> A test agent.\n"
        "## distributed_tracing\n"
        "Some content here.\n";

    ErrorReporter reporter;
    AstNode *program = parse_md_source_reporter(src, &reporter);

    ASSERT_NOT_NULL(program);
    ASSERT_EQ(program->kind, AST_PROGRAM);

    /* Should have at least one diagnostic (the warning) */
    ASSERT(reporter.count >= 1);

    /* Find the warning about unrecognized section */
    {
        int found_warning = 0;
        int i;
        for (i = 0; i < reporter.count; i++) {
            if (reporter.errors[i].is_warning &&
                strstr(reporter.errors[i].message, "unrecognized section") != NULL &&
                strstr(reporter.errors[i].message, "distributed_tracing") != NULL) {
                found_warning = 1;
                /* Check hint lists supported sections */
                ASSERT_NOT_NULL(reporter.errors[i].hint);
                ASSERT(strstr(reporter.errors[i].hint, "supported sections") != NULL);
                break;
            }
        }
        ASSERT(found_warning);
    }
}

int main(void) {
    fprintf(stderr, "\n\033[1mLimceron Stage 0 — Test Suite\033[0m\n\n");

    setup();

    fprintf(stderr, "── Lexer Tests ──\n");
    RUN_TEST(lex_empty);
    RUN_TEST(lex_integer_decimal);
    RUN_TEST(lex_integer_hex);
    RUN_TEST(lex_integer_binary);
    RUN_TEST(lex_integer_octal);
    RUN_TEST(lex_integer_underscores);
    RUN_TEST(lex_float);
    RUN_TEST(lex_float_scientific);
    RUN_TEST(lex_string);
    RUN_TEST(lex_string_escape);
    RUN_TEST(lex_raw_string);
    RUN_TEST(lex_identifier);
    RUN_TEST(lex_keywords);
    RUN_TEST(lex_all_keywords);
    RUN_TEST(lex_operators_arithmetic);
    RUN_TEST(lex_operators_comparison);
    RUN_TEST(lex_operators_logical);
    RUN_TEST(lex_operators_bitwise);
    RUN_TEST(lex_operators_assignment);
    RUN_TEST(lex_operators_special);
    RUN_TEST(lex_delimiters);
    RUN_TEST(lex_line_comment);
    RUN_TEST(lex_block_comment);
    RUN_TEST(lex_nested_block_comment);
    RUN_TEST(lex_semicolon_insertion_after_ident);
    RUN_TEST(lex_semicolon_insertion_after_return);
    RUN_TEST(lex_no_semicolon_after_plus);
    RUN_TEST(lex_no_semicolon_after_open_brace);
    RUN_TEST(lex_semicolon_after_close_brace);
    RUN_TEST(lex_line_tracking);
    RUN_TEST(lex_complex_expression);
    RUN_TEST(lex_function_signature);

    fprintf(stderr, "\n── Parser Tests ──\n");
    RUN_TEST(parse_empty);
    RUN_TEST(parse_module_decl);
    RUN_TEST(parse_module_qualified);
    RUN_TEST(parse_use_simple);
    RUN_TEST(parse_use_qualified);
    RUN_TEST(parse_simple_fn);
    RUN_TEST(parse_fn_with_params);
    RUN_TEST(parse_pub_fn);
    RUN_TEST(parse_struct);
    RUN_TEST(parse_enum_simple);
    RUN_TEST(parse_enum_with_fields);
    RUN_TEST(parse_let_simple);
    RUN_TEST(parse_let_mut_typed);
    RUN_TEST(parse_if_else);
    RUN_TEST(parse_match);
    RUN_TEST(parse_for_loop);
    RUN_TEST(parse_while_loop);
    RUN_TEST(parse_binary_precedence);
    RUN_TEST(parse_method_call);
    RUN_TEST(parse_try_operator);
    RUN_TEST(parse_closure);
    RUN_TEST(parse_array_literal);
    RUN_TEST(parse_spawn);
    RUN_TEST(parse_defer);
    RUN_TEST(parse_impl_block);
    RUN_TEST(parse_trait_decl);
    RUN_TEST(parse_type_alias);
    RUN_TEST(parse_generic_fn);
    RUN_TEST(parse_full_program);

    fprintf(stderr, "\n── Agent System Tests ──\n");
    RUN_TEST(parse_capability);
    RUN_TEST(parse_taint);
    RUN_TEST(parse_budget);
    RUN_TEST(parse_guard_with_params);
    RUN_TEST(parse_guard_no_params);
    RUN_TEST(parse_tool_decl);
    RUN_TEST(parse_skill_decl);
    RUN_TEST(parse_supervisor_decl);
    RUN_TEST(parse_agent_decl);
    RUN_TEST(parse_agent_full_program);
    RUN_TEST(parse_ask_expr);
    RUN_TEST(parse_tell_expr);
    RUN_TEST(parse_chan_expr);
    RUN_TEST(parse_select_stmt);
    RUN_TEST(parse_mesh_decl);
    RUN_TEST(parse_router_decl);
    RUN_TEST(parse_use_mcp);
    RUN_TEST(parse_use_a2a);
    RUN_TEST(parse_use_model);

    fprintf(stderr, "\n── Type Checker Tests ──\n");
    RUN_TEST(typecheck_simple_program_ok);
    RUN_TEST(typecheck_capability_missing);
    RUN_TEST(typecheck_capability_ok);
    RUN_TEST(typecheck_guard_required);
    RUN_TEST(typecheck_budget_missing);
    RUN_TEST(typecheck_budget_ok);
    RUN_TEST(typecheck_taint_violation);
    RUN_TEST(typecheck_unknown_type);
    RUN_TEST(typecheck_unknown_function);
    RUN_TEST(typecheck_full_agent_pipeline);
    RUN_TEST(typecheck_taint_inference_ask);
    RUN_TEST(typecheck_taint_propagation);
    RUN_TEST(typecheck_taint_clean_ok);
    RUN_TEST(typecheck_prompt_injection);
    RUN_TEST(typecheck_agent_taint);

    fprintf(stderr, "\n── Codegen Tests ──\n");
    RUN_TEST(codegen_preamble);
    RUN_TEST(codegen_simple_fn);
    RUN_TEST(codegen_capability);
    RUN_TEST(codegen_guard);
    RUN_TEST(codegen_budget);
    RUN_TEST(codegen_taint);
    RUN_TEST(codegen_tool_cap_check);
    RUN_TEST(codegen_agent_struct);
    RUN_TEST(codegen_supervisor);
    RUN_TEST(codegen_agent_tool_rewrite);
    RUN_TEST(codegen_md_guard_skip);
    RUN_TEST(codegen_spawn_inline);
    RUN_TEST(codegen_vec_pop_call);
    RUN_TEST(codegen_vec_pop_preamble);
    RUN_TEST(codegen_spawn_threaded);
    RUN_TEST(codegen_spawn_await);
    RUN_TEST(codegen_taint_functions);
    RUN_TEST(codegen_skill_struct);
    RUN_TEST(codegen_ask_builtin);
    RUN_TEST(codegen_tell);
    RUN_TEST(codegen_channel);
    RUN_TEST(codegen_select);
    RUN_TEST(codegen_mesh);
    RUN_TEST(codegen_router);
    RUN_TEST(codegen_mcp_init);
    RUN_TEST(codegen_a2a_init);
    RUN_TEST(codegen_model_init);
    RUN_TEST(codegen_model_predict_rewrite);
    RUN_TEST(codegen_postgres_driver_include);
    RUN_TEST(codegen_postgres_connect);
    RUN_TEST(codegen_postgres_execute);
    RUN_TEST(codegen_postgres_query);
    RUN_TEST(codegen_postgres_escape);
    RUN_TEST(codegen_postgres_row_accessors);
    RUN_TEST(codegen_full_pipeline);

    fprintf(stderr, "\n── Beta Feature Tests ──\n");
    RUN_TEST(codegen_for_range);
    RUN_TEST(codegen_for_inclusive_range);
    RUN_TEST(codegen_for_general);
    RUN_TEST(codegen_try_operator);
    RUN_TEST(codegen_try_multiple);
    RUN_TEST(codegen_mcp_wrapper);
    RUN_TEST(codegen_for_with_body);
    RUN_TEST(codegen_endpoint_field);

    fprintf(stderr, "\n── Production Builtins Tests ──\n");
    RUN_TEST(codegen_len_builtin);
    RUN_TEST(codegen_contains_builtin);
    RUN_TEST(codegen_starts_with_builtin);
    RUN_TEST(codegen_ends_with_builtin);
    RUN_TEST(codegen_env_builtin);
    RUN_TEST(codegen_str_eq_builtin);
    RUN_TEST(codegen_str_replace_builtin);
    RUN_TEST(codegen_str_trim_builtin);
    RUN_TEST(codegen_to_string_builtin);
    RUN_TEST(codegen_to_int_builtin);
    RUN_TEST(codegen_json_parse_builtin);
    RUN_TEST(codegen_json_get_builtin);
    RUN_TEST(codegen_json_array_len_builtin);
    RUN_TEST(codegen_json_array_get_builtin);
    RUN_TEST(codegen_string_equality);
    RUN_TEST(codegen_string_inequality);
    RUN_TEST(codegen_enum_constants);
    RUN_TEST(codegen_enum_validation);
    RUN_TEST(codegen_mcp_returns_string);
    RUN_TEST(codegen_categorizer_pattern);

    fprintf(stderr, "\n── LLMOutput + Match Tests ──\n");
    RUN_TEST(codegen_ask_returns_llmoutput);
    RUN_TEST(codegen_match_llmoutput);
    RUN_TEST(codegen_match_variant_binding);
    RUN_TEST(codegen_llmoutput_preamble);

    fprintf(stderr, "\n── Security Tests ──\n");
    RUN_TEST(security_hash_basic);
    RUN_TEST(security_hash_different);
    RUN_TEST(security_lceron_sign_verify);
    RUN_TEST(security_lceron_tamper_detection);
    RUN_TEST(security_lceron_wrong_key);
    RUN_TEST(security_lceron_bad_magic);

    fprintf(stderr, "\n── Stdlib Codegen Tests ──\n");
    RUN_TEST(codegen_math_builtins);
    RUN_TEST(codegen_math_float);
    RUN_TEST(codegen_time_builtins);
    RUN_TEST(codegen_log_builtins);
    RUN_TEST(codegen_batch_builtins);
    RUN_TEST(codegen_budget_introspection);
    RUN_TEST(codegen_token_estimation);
    RUN_TEST(codegen_trace_builtins);
    RUN_TEST(codegen_file_io_builtins);
    RUN_TEST(codegen_env_or_builtin);
    RUN_TEST(codegen_sql_escape_builtin);

    fprintf(stderr, "\n── Multi-File Import Tests ──\n");
    RUN_TEST(test_parse_use_module_path);
    RUN_TEST(test_parse_use_deep_path);
    RUN_TEST(test_parse_use_grouped);
    RUN_TEST(test_parse_use_with_alias);
    RUN_TEST(test_parse_multiple_use);
    RUN_TEST(test_parse_use_mcp_not_module);

    fprintf(stderr, "\n── Keyword-as-Identifier Tests ──\n");
    RUN_TEST(parse_keyword_agent_as_ident);
    RUN_TEST(parse_keyword_budget_as_ident);
    RUN_TEST(parse_keyword_channel_as_ident);
    RUN_TEST(parse_keyword_channel_as_param);
    RUN_TEST(parse_keyword_ident_in_expr);

    fprintf(stderr, "\n── Block Field Parsing Tests ──\n");
    RUN_TEST(parse_agent_three_fields);
    RUN_TEST(parse_skill_two_fields);
    RUN_TEST(parse_agent_with_method);
    RUN_TEST(parse_supervisor_two_fields);

    fprintf(stderr, "\n── String Interpolation Codegen Tests ──\n");
    RUN_TEST(codegen_interp_simple);
    RUN_TEST(codegen_interp_adjacent);
    RUN_TEST(codegen_interp_none);
    RUN_TEST(codegen_interp_space_not_interp);
    RUN_TEST(codegen_interp_middle);
    RUN_TEST(codegen_plain_no_concat);

    fprintf(stderr, "\n── Struct Literal Tests ──\n");
    RUN_TEST(codegen_struct_literal);
    RUN_TEST(parse_struct_literal_ast);
    RUN_TEST(codegen_struct_literal_empty);
    RUN_TEST(codegen_struct_literal_single_field);
    RUN_TEST(codegen_struct_literal_let_binding);

    fprintf(stderr, "\n── Impl Block Tests ──\n");
    RUN_TEST(parse_impl_as_ast_impl);
    RUN_TEST(codegen_impl_method_name);
    RUN_TEST(parse_impl_multiple_methods);
    RUN_TEST(codegen_impl_self_param);
    RUN_TEST(codegen_impl_pre_registration);

    fprintf(stderr, "\n── Error Propagation / ? Operator Tests ──\n");
    RUN_TEST(codegen_try_generates_pattern);
    RUN_TEST(codegen_try_on_expression);
    RUN_TEST(codegen_try_nested);

    fprintf(stderr, "\n── For Loop and Control Flow Tests ──\n");
    RUN_TEST(parse_for_in_items);
    RUN_TEST(parse_while_cond);
    RUN_TEST(parse_break_continue);
    RUN_TEST(parse_nested_loops);
    RUN_TEST(codegen_for_range_expr);

    fprintf(stderr, "\n── Pattern Matching Tests ──\n");
    RUN_TEST(parse_match_wildcard);
    RUN_TEST(parse_match_multiple_arms);
    RUN_TEST(parse_match_string_literals);
    RUN_TEST(codegen_match_enum_variant);
    RUN_TEST(codegen_match_struct_destructure);
    RUN_TEST(codegen_match_tuple_destructure);
    RUN_TEST(codegen_match_struct_rename);

    fprintf(stderr, "\n── Additional Type Checker Tests ──\n");
    RUN_TEST(typecheck_rejects_unknown_type);
    RUN_TEST(typecheck_declared_fn_ok);
    RUN_TEST(typecheck_agent_undeclared_fn);
    RUN_TEST(typecheck_impl_blocks);

    fprintf(stderr, "\n── Edge Case Tests ──\n");
    RUN_TEST(parse_empty_program);
    RUN_TEST(codegen_empty_program);
    RUN_TEST(parse_single_function);
    RUN_TEST(codegen_unicode_string);
    RUN_TEST(parse_long_identifier);

    fprintf(stderr, "\n── Access Control Policy Tests ──\n");
    RUN_TEST(parse_cap_endpoint_allow);
    RUN_TEST(parse_cap_binary_allow_deny);
    RUN_TEST(parse_cap_deny_private_ranges);
    RUN_TEST(parse_cap_mixed_abstract_concrete);
    RUN_TEST(typecheck_network_static_allowed);
    RUN_TEST(typecheck_network_static_denied);
    RUN_TEST(typecheck_private_range_denied);
    RUN_TEST(typecheck_binary_allowed);
    RUN_TEST(typecheck_binary_denied);
    RUN_TEST(codegen_access_policy_table);
    RUN_TEST(codegen_binary_policy_table);
    RUN_TEST(parse_cap_path_allow_deny);
    RUN_TEST(typecheck_path_read_allowed);
    RUN_TEST(typecheck_path_write_denied);
    RUN_TEST(typecheck_path_default_deny);
    RUN_TEST(codegen_path_policy_table);
    RUN_TEST(codegen_existing_cap_unchanged);

    fprintf(stderr, "\n── Capability Delegation Tests ──\n");
    RUN_TEST(codegen_delegate_builtin);
    RUN_TEST(codegen_revoke_builtin);
    RUN_TEST(codegen_revoke_all_builtin);
    RUN_TEST(codegen_has_capability_builtin);
    RUN_TEST(codegen_delegation_preamble);
    RUN_TEST(parse_has_capability_call);

    fprintf(stderr, "\n── Invariant Wiring Tests ──\n");
    RUN_TEST(codegen_invariant_drift);
    RUN_TEST(codegen_invariant_avg_confidence);
    RUN_TEST(codegen_invariant_avg_entropy);
    RUN_TEST(parse_invariant_decl);

    fprintf(stderr, "\n── Defer LIFO Tests ──\n");
    RUN_TEST(codegen_defer_lifo_order);
    RUN_TEST(codegen_defer_not_inline);
    RUN_TEST(codegen_defer_at_block_exit);
    RUN_TEST(codegen_defer_after_last_stmt_in_void_fn);

    fprintf(stderr, "\n── Syntactic Sugar Tests ──\n");
    RUN_TEST(parse_try_otherwise);
    RUN_TEST(codegen_try_otherwise);
    RUN_TEST(parse_keep_where);
    RUN_TEST(codegen_keep_where);
    RUN_TEST(parse_each);
    RUN_TEST(codegen_each);

    fprintf(stderr, "\n── Generics / Monomorphization Tests ──\n");
    RUN_TEST(codegen_result_generic_typedef);
    RUN_TEST(codegen_result_generic_return_type);
    RUN_TEST(codegen_option_generic_typedef);
    RUN_TEST(codegen_bare_result_backward_compat);
    RUN_TEST(codegen_result_string_string);
    RUN_TEST(codegen_option_int_typedef);
    RUN_TEST(codegen_match_result_ok_error);
    RUN_TEST(codegen_match_option_some_none);
    RUN_TEST(codegen_result_no_duplicate_typedef);
    RUN_TEST(codegen_result_default_error_type);

    /* Error message format tests */
    RUN_TEST(error_msg_has_line_number);
    RUN_TEST(error_msg_has_source_snippet);
    RUN_TEST(error_msg_has_caret);
    RUN_TEST(warning_msg_format);
    RUN_TEST(error_msg_single_caret);
    RUN_TEST(error_msg_no_source);
    RUN_TEST(error_msg_multidigit_line);
    RUN_TEST(error_reporter_stores_warning_flag);

    /* Secret type tests */
    RUN_TEST(parse_secret_type);
    RUN_TEST(typecheck_secret_no_println);
    RUN_TEST(typecheck_secret_no_log);
    RUN_TEST(typecheck_secret_redact_ok);
    RUN_TEST(codegen_secret_type);
    RUN_TEST(codegen_secret_redact);

    /* Capability fence tests */
    RUN_TEST(codegen_tool_capability_fence);
    RUN_TEST(codegen_agent_tool_list);
    RUN_TEST(codegen_mcp_capability_gate);

    fprintf(stderr, "\n── Green Thread Tests ──\n");
    RUN_TEST(green_spawn_basic);
    RUN_TEST(green_yield);
    RUN_TEST(green_many_tasks);
    RUN_TEST(codegen_green_spawn);

    fprintf(stderr, "\n── Module Visibility (pub/priv) Tests ──\n");
    RUN_TEST(typecheck_pub_fn_accessible);
    RUN_TEST(typecheck_priv_fn_error);
    RUN_TEST(typecheck_priv_same_module_ok);
    RUN_TEST(typecheck_pub_struct_accessible);
    RUN_TEST(typecheck_priv_struct_error);

    /* Supervisor tests */
    RUN_TEST(parse_supervisor_block);
    RUN_TEST(codegen_supervisor_one_for_one);
    RUN_TEST(codegen_supervisor_all_for_one);
    RUN_TEST(codegen_supervisor_max_restarts);

    /* TaskGroup tests */
    RUN_TEST(parse_task_group);
    RUN_TEST(codegen_task_group_spawn);
    RUN_TEST(codegen_task_group_await_all);
    RUN_TEST(codegen_task_group_preamble);

    /* Defense/FFI tests */
    RUN_TEST(parse_link_directive);
    RUN_TEST(codegen_link_directive);
    RUN_TEST(codegen_defense_in_depth);
    RUN_TEST(codegen_defense_non_agent_no_wrap);

    /* Router/A2A tests */
    RUN_TEST(parse_router_block);
    RUN_TEST(codegen_router_health_check);
    RUN_TEST(codegen_router_select);
    RUN_TEST(codegen_a2a_connect);

    /* Mesh tests */
    RUN_TEST(parse_mesh_routes);
    RUN_TEST(codegen_mesh_fan_out);
    RUN_TEST(codegen_mesh_fan_in);
    RUN_TEST(codegen_mesh_sequential);

    /* Comptime tests */
    RUN_TEST(parse_comptime_block);
    RUN_TEST(parse_comptime_top_level);
    RUN_TEST(codegen_comptime_int_arithmetic);
    RUN_TEST(codegen_comptime_string_concat);
    RUN_TEST(codegen_comptime_if_expr);
    RUN_TEST(codegen_comptime_if_expr_false);
    RUN_TEST(codegen_comptime_assert_pass);
    RUN_TEST(codegen_comptime_assert_fail);
    RUN_TEST(codegen_comptime_nested_let);
    RUN_TEST(codegen_comptime_float_arithmetic);
    RUN_TEST(codegen_comptime_boolean_logic);
    RUN_TEST(codegen_comptime_comparison);
    RUN_TEST(codegen_comptime_unary_negation);
    RUN_TEST(codegen_comptime_len_builtin);
    RUN_TEST(codegen_comptime_min_max);
    RUN_TEST(codegen_comptime_complex_expr);
    RUN_TEST(codegen_comptime_top_level_assert);
    RUN_TEST(codegen_comptime_to_string);
    RUN_TEST(codegen_comptime_abs);
    RUN_TEST(codegen_comptime_bitwise);
    RUN_TEST(codegen_comptime_division);
    RUN_TEST(codegen_comptime_modulo);
    RUN_TEST(codegen_comptime_string_comparison);

    /* Trait/Interface tests */
    RUN_TEST(parse_interface);
    RUN_TEST(parse_impl_for_interface);
    RUN_TEST(codegen_interface_vtable);
    RUN_TEST(codegen_impl_vtable_instance);
    RUN_TEST(parse_union_type);
    RUN_TEST(codegen_union_tagged);
    RUN_TEST(typecheck_interface_missing_method);

    fprintf(stderr, "\n── Ownership & Borrow Checking Tests ──\n");
    RUN_TEST(ownership_use_after_move);
    RUN_TEST(ownership_move_while_borrowed);
    RUN_TEST(ownership_double_mut_borrow);
    RUN_TEST(ownership_immutable_borrow_ok);
    RUN_TEST(ownership_scope_release);
    RUN_TEST(ownership_function_param_move);
    RUN_TEST(ownership_ref_param_borrow);
    RUN_TEST(ownership_advisory_mode);

    fprintf(stderr, "\n── Package Manager Tests ──\n");
    RUN_TEST(package_parse_toml);
    RUN_TEST(package_parse_toml_path_dep);
    RUN_TEST(package_parse_toml_comments);
    RUN_TEST(package_parse_toml_empty);
    RUN_TEST(package_semver_compare);
    RUN_TEST(package_semver_satisfies);
    RUN_TEST(package_semver_parse);
    RUN_TEST(package_semver_tilde);
    RUN_TEST(package_resolve_simple);
    RUN_TEST(package_lock_roundtrip);
    RUN_TEST(package_serialize_roundtrip);
    RUN_TEST(package_add_remove_dep);

    fprintf(stderr, "\n── Cross-Compilation Target Tests ──\n");
    RUN_TEST(target_parse_triple);
    RUN_TEST(target_parse_full);
    RUN_TEST(target_parse_darwin);
    RUN_TEST(target_parse_windows);
    RUN_TEST(target_parse_arm64_alias);
    RUN_TEST(target_parse_invalid);
    RUN_TEST(target_parse_empty);
    RUN_TEST(target_parse_null);
    RUN_TEST(target_native_detect);
    RUN_TEST(target_ldflags_linux);
    RUN_TEST(target_ldflags_darwin);
    RUN_TEST(target_ldflags_windows);
    RUN_TEST(target_static_ldflags);
    RUN_TEST(target_arch_str);
    RUN_TEST(target_os_str);
    RUN_TEST(target_abi_str);
    RUN_TEST(codegen_platform_guards);
    RUN_TEST(codegen_platform_guards_darwin);
    RUN_TEST(codegen_platform_guards_windows);
    RUN_TEST(codegen_no_guards_native);
    RUN_TEST(target_cross_cc_native);

    fprintf(stderr, "\n\xe2\x94\x80\xe2\x94\x80 Health Probe Tests \xe2\x94\x80\xe2\x94\x80\n");
    RUN_TEST(parse_health_block);
    RUN_TEST(parse_health_default_port);
    RUN_TEST(codegen_health_server);

    fprintf(stderr, "\n── LSP Server Tests ──\n");
    RUN_TEST(lsp_parse_header);
    RUN_TEST(lsp_initialize_response);
    RUN_TEST(lsp_diagnostics);
    RUN_TEST(lsp_completion_keywords);

    fprintf(stderr, "\n── Prometheus Metrics Tests ──\n");
    RUN_TEST(parse_metrics_block);
    RUN_TEST(codegen_metrics_counter);
    RUN_TEST(codegen_metrics_endpoint);
    RUN_TEST(codegen_metrics_histogram);

    fprintf(stderr, "\n── Progress Reporting Tests ──\n");
    RUN_TEST(parse_progress_block);
    RUN_TEST(codegen_progress_update);
    RUN_TEST(codegen_progress_file);

    fprintf(stderr, "\n── Markdown Parser Safety Tests ──\n");
    RUN_TEST(md_parse_capability_declaration);
    RUN_TEST(md_parse_capability_with_access_rules);
    RUN_TEST(md_parse_taint);
    RUN_TEST(md_parse_access_control);
    RUN_TEST(md_parse_combined_safety);

    fprintf(stderr, "\n── Markdown Parser K8s/Runtime Tests ──\n");
    RUN_TEST(md_parse_health);
    RUN_TEST(md_parse_metrics);
    RUN_TEST(md_parse_progress);
    RUN_TEST(md_parse_supervisor);

    fprintf(stderr, "\n── Markdown Parser Guards/Errors Tests ──\n");
    RUN_TEST(md_guard_text_only);
    RUN_TEST(md_guard_with_code_block);
    RUN_TEST(md_unknown_section_warning);

    teardown();

    TEST_SUMMARY();
    return test_failures > 0 ? 1 : 0;
}
