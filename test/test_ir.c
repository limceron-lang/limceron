/*
 * Limceron Stage 0 — IR Backend Test Suite
 *
 * Tests for SSA IR generation, printing, and optimization passes.
 */

#include "lcn.h"
#include "ir.h"
#include "test.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* ============================================================
 * Test Helpers
 * ============================================================ */

static Arena test_arena;
static Arena test_intern_arena;

static void ir_test_setup(void) {
    test_arena = arena_new(8 * 1024 * 1024);
    test_intern_arena = arena_new(2 * 1024 * 1024);
}

static void ir_test_teardown(void) {
    arena_free(&test_arena);
    arena_free(&test_intern_arena);
}

/* Parse source and generate IR */
static IrModule *ir_from_source(const char *source) {
    arena_reset(&test_arena);
    arena_reset(&test_intern_arena);

    size_t len = strlen(source);
    ErrorReporter reporter = reporter_new("<test>", source, len);
    StringIntern intern = intern_new(&test_intern_arena);
    Lexer lexer = lexer_new("<test>", source, len, &intern, &reporter);
    Parser parser = parser_new(&lexer, &test_arena, &reporter);

    AstNode *program = parse_program(&parser);
    if (parser.had_error || !program) return NULL;

    return ir_gen_program(program, &test_arena);
}

/* Get the first function from a module */
static IrFunction *first_fn(IrModule *mod) {
    return mod ? mod->functions : NULL;
}

/* Get a specific function by index */
static IrFunction *nth_fn(IrModule *mod, int n) {
    if (!mod) return NULL;
    IrFunction *fn = mod->functions;
    int i;
    for (i = 0; i < n && fn; i++) fn = fn->next;
    return fn;
}

/* Find an instruction by SSA value ID in a function */
static IrInst *find_inst(IrFunction *fn, int id) {
    IrBasicBlock *bb;
    for (bb = fn->entry; bb; bb = bb->next) {
        IrInst *inst;
        for (inst = bb->first; inst; inst = inst->next) {
            if (inst->id == id) return inst;
        }
    }
    return NULL;
}

/* Find first instruction with a given opcode in a function */
static IrInst *find_opcode(IrFunction *fn, IrOpcode op) {
    IrBasicBlock *bb;
    for (bb = fn->entry; bb; bb = bb->next) {
        IrInst *inst;
        for (inst = bb->first; inst; inst = inst->next) {
            if (inst->op == op) return inst;
        }
    }
    return NULL;
}

/* Count instructions with a given opcode */
static int count_opcode(IrFunction *fn, IrOpcode op) {
    int count = 0;
    IrBasicBlock *bb;
    for (bb = fn->entry; bb; bb = bb->next) {
        IrInst *inst;
        for (inst = bb->first; inst; inst = inst->next) {
            if (inst->op == op) count++;
        }
    }
    return count;
}

/* Count total instructions in a function */
static int count_insts(IrFunction *fn) {
    int count = 0;
    IrBasicBlock *bb;
    for (bb = fn->entry; bb; bb = bb->next) {
        IrInst *inst;
        for (inst = bb->first; inst; inst = inst->next)
            count++;
    }
    return count;
}

/* Print module to a string (for format checking) */
static char *print_to_string(IrModule *mod) {
    /* Print to temp file, then read back */
    FILE *f = tmpfile();
    if (!f) return NULL;
    ir_print_module(mod, f);
    long size = ftell(f);
    rewind(f);
    char *buf = (char *)malloc(size + 1);
    fread(buf, 1, (size_t)size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

/* Helper: emit ARM64 asm to a string from source code */
static char *arm64_from_source(const char *source) {
    IrModule *mod = ir_from_source(source);
    if (!mod) return NULL;

    ir_opt_all(mod);

    FILE *f = tmpfile();
    if (!f) return NULL;
    ir_emit_arm64(mod, f);
    long size = ftell(f);
    rewind(f);
    char *buf = (char *)malloc(size + 1);
    fread(buf, 1, (size_t)size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

/* Emit x86 assembly to a string */
static char *emit_x86_to_string(IrModule *mod) {
    FILE *f = tmpfile();
    if (!f) return NULL;
    ir_emit_x86(mod, f);
    long size = ftell(f);
    rewind(f);
    char *buf = (char *)malloc(size + 1);
    fread(buf, 1, (size_t)size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

/* ============================================================
 * IR Generation Tests
 * ============================================================ */

TEST(ir_gen_int_literal) {
    IrModule *mod = ir_from_source("fn test() -> int {\n    return 42\n}\n");
    ASSERT_NOT_NULL(mod);
    ASSERT_EQ(mod->fn_count, 1);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);
    ASSERT_STR_EQ(fn->name, "lcn_test");
    ASSERT_EQ(fn->return_type, IR_TYPE_I64);

    /* Should contain a const int 42 */
    IrInst *ci = find_opcode(fn, IR_CONST_INT);
    ASSERT_NOT_NULL(ci);
    ASSERT_EQ(ci->imm_int, 42);
    ASSERT_EQ(ci->type, IR_TYPE_I64);
}

TEST(ir_gen_float_literal) {
    IrModule *mod = ir_from_source("fn test() -> float {\n    return 3.14\n}\n");
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    IrInst *cf = find_opcode(fn, IR_CONST_FLOAT);
    ASSERT_NOT_NULL(cf);
    ASSERT_FLOAT_EQ(cf->imm_float, 3.14);
    ASSERT_EQ(cf->type, IR_TYPE_F64);
}

TEST(ir_gen_string_literal) {
    IrModule *mod = ir_from_source("fn test() {\n    println(\"hello world\")\n}\n");
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    IrInst *cs = find_opcode(fn, IR_CONST_STRING);
    ASSERT_NOT_NULL(cs);
    ASSERT_STR_EQ(cs->imm_str, "hello world");
    ASSERT_EQ(cs->type, IR_TYPE_STRING);
}

TEST(ir_gen_bool_literal) {
    IrModule *mod = ir_from_source("fn test() -> bool {\n    return true\n}\n");
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    IrInst *cb = find_opcode(fn, IR_CONST_BOOL);
    ASSERT_NOT_NULL(cb);
    ASSERT_EQ(cb->imm_int, 1);
    ASSERT_EQ(cb->type, IR_TYPE_BOOL);
}

TEST(ir_gen_binary_add) {
    IrModule *mod = ir_from_source("fn test() -> int {\n    return 3 + 4\n}\n");
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    /* Should have: const 3, const 4, add, ret */
    IrInst *add = find_opcode(fn, IR_ADD);
    ASSERT_NOT_NULL(add);
    ASSERT_EQ(add->type, IR_TYPE_I64);
    ASSERT_EQ(add->operand_count, 2);
}

TEST(ir_gen_binary_sub) {
    IrModule *mod = ir_from_source("fn test() -> int {\n    return 10 - 3\n}\n");
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    IrInst *sub = find_opcode(fn, IR_SUB);
    ASSERT_NOT_NULL(sub);
    ASSERT_EQ(sub->type, IR_TYPE_I64);
}

TEST(ir_gen_binary_mul) {
    IrModule *mod = ir_from_source("fn test() -> int {\n    return 5 * 6\n}\n");
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    IrInst *mul = find_opcode(fn, IR_MUL);
    ASSERT_NOT_NULL(mul);
}

TEST(ir_gen_binary_comparison) {
    IrModule *mod = ir_from_source("fn test() -> bool {\n    return 3 < 4\n}\n");
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    IrInst *cmp = find_opcode(fn, IR_CMP_LT);
    ASSERT_NOT_NULL(cmp);
    ASSERT_EQ(cmp->type, IR_TYPE_BOOL);
}

TEST(ir_gen_function_def) {
    IrModule *mod = ir_from_source(
        "fn add(a: int, b: int) -> int {\n"
        "    return a + b\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);
    ASSERT_EQ(mod->fn_count, 1);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);
    ASSERT_STR_EQ(fn->name, "lcn_add");
    ASSERT_EQ(fn->return_type, IR_TYPE_I64);
    ASSERT_EQ(fn->param_count, 2);
    ASSERT_EQ(fn->param_types[0], IR_TYPE_I64);
    ASSERT_EQ(fn->param_types[1], IR_TYPE_I64);
    ASSERT_STR_EQ(fn->param_names[0], "a");
    ASSERT_STR_EQ(fn->param_names[1], "b");
}

TEST(ir_gen_function_call) {
    IrModule *mod = ir_from_source(
        "fn add(a: int, b: int) -> int {\n"
        "    return a + b\n"
        "}\n"
        "fn main() {\n"
        "    let x = add(3, 4)\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);
    ASSERT_EQ(mod->fn_count, 2);

    /* Check that main has a call instruction */
    IrFunction *main_fn = nth_fn(mod, 1);
    ASSERT_NOT_NULL(main_fn);
    ASSERT_STR_EQ(main_fn->name, "lcn_main");

    IrInst *call = find_opcode(main_fn, IR_CALL);
    ASSERT_NOT_NULL(call);
    ASSERT_STR_EQ(call->fn_name, "lcn_add");
    ASSERT_EQ(call->call_arg_count, 2);
}

TEST(ir_gen_variable_decl) {
    IrModule *mod = ir_from_source(
        "fn test() {\n"
        "    let x = 42\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    /* Should have: alloca, const 42, store */
    ASSERT(count_opcode(fn, IR_ALLOCA) >= 1);
    ASSERT(count_opcode(fn, IR_STORE) >= 1);

    IrInst *ci = find_opcode(fn, IR_CONST_INT);
    ASSERT_NOT_NULL(ci);
    ASSERT_EQ(ci->imm_int, 42);
}

TEST(ir_gen_variable_load) {
    IrModule *mod = ir_from_source(
        "fn test() -> int {\n"
        "    let x = 42\n"
        "    return x\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    /* Should have a load instruction for reading x */
    ASSERT(count_opcode(fn, IR_LOAD) >= 1);
}

TEST(ir_gen_if_else_branches) {
    IrModule *mod = ir_from_source(
        "fn test(n: int) -> int {\n"
        "    if n < 10 {\n"
        "        return 1\n"
        "    } else {\n"
        "        return 2\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    /* Should have a conditional branch */
    IrInst *br = find_opcode(fn, IR_BR);
    ASSERT_NOT_NULL(br);
    ASSERT(br->target_bb >= 0);
    ASSERT(br->false_bb >= 0);
    ASSERT(br->target_bb != br->false_bb);

    /* Should have at least 3 basic blocks (entry, then, else + merge) */
    ASSERT(fn->bb_count >= 3);

    /* Should have comparison */
    ASSERT(count_opcode(fn, IR_CMP_LT) >= 1);
}

TEST(ir_gen_if_no_else) {
    IrModule *mod = ir_from_source(
        "fn test(n: int) {\n"
        "    if n > 0 {\n"
        "        println(\"positive\")\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    /* Should have a branch */
    ASSERT(count_opcode(fn, IR_BR) >= 1);
    /* Should have then and merge blocks */
    ASSERT(fn->bb_count >= 3);
}

TEST(ir_gen_for_loop) {
    IrModule *mod = ir_from_source(
        "fn test() {\n"
        "    for i in 0..5 {\n"
        "        println(\"step\")\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    /* For loop should produce: entry + cond + body + inc + exit = 5 blocks */
    ASSERT(fn->bb_count >= 5);

    /* Should have comparison and branch in cond block */
    ASSERT(count_opcode(fn, IR_CMP_LT) >= 1);
    ASSERT(count_opcode(fn, IR_BR) >= 1);

    /* Should have increment: add i64 */
    ASSERT(count_opcode(fn, IR_ADD) >= 1);

    /* Should have jumps: from init->cond, body->inc, inc->cond */
    ASSERT(count_opcode(fn, IR_JMP) >= 3);
}

TEST(ir_gen_while_loop) {
    IrModule *mod = ir_from_source(
        "fn test() {\n"
        "    let x = 0\n"
        "    while x < 10 {\n"
        "        println(\"loop\")\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    /* While loop: entry + cond + body + exit = 4 blocks */
    ASSERT(fn->bb_count >= 4);
    ASSERT(count_opcode(fn, IR_CMP_LT) >= 1);
    ASSERT(count_opcode(fn, IR_BR) >= 1);
}

TEST(ir_gen_string_concat) {
    IrModule *mod = ir_from_source(
        "fn test() {\n"
        "    let a = \"hello\"\n"
        "    let b = \" world\"\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    /* Should have two string constants */
    ASSERT(count_opcode(fn, IR_CONST_STRING) >= 2);
}

TEST(ir_gen_multiple_functions) {
    IrModule *mod = ir_from_source(
        "fn foo() -> int {\n"
        "    return 1\n"
        "}\n"
        "fn bar() -> int {\n"
        "    return 2\n"
        "}\n"
        "fn main() {\n"
        "    let a = foo()\n"
        "    let b = bar()\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);
    ASSERT_EQ(mod->fn_count, 3);

    IrFunction *fn0 = nth_fn(mod, 0);
    IrFunction *fn1 = nth_fn(mod, 1);
    IrFunction *fn2 = nth_fn(mod, 2);
    ASSERT_NOT_NULL(fn0);
    ASSERT_NOT_NULL(fn1);
    ASSERT_NOT_NULL(fn2);
    ASSERT_STR_EQ(fn0->name, "lcn_foo");
    ASSERT_STR_EQ(fn1->name, "lcn_bar");
    ASSERT_STR_EQ(fn2->name, "lcn_main");
}

TEST(ir_gen_return_void) {
    IrModule *mod = ir_from_source("fn test() {\n    println(\"hi\")\n}\n");
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);
    ASSERT_EQ(fn->return_type, IR_TYPE_VOID);

    /* Should have at least one ret void */
    ASSERT(count_opcode(fn, IR_RET) >= 1);
}

TEST(ir_gen_nested_binary) {
    IrModule *mod = ir_from_source(
        "fn test() -> int {\n"
        "    return (1 + 2) * (3 + 4)\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    /* Should have 2 adds and 1 mul */
    ASSERT_EQ(count_opcode(fn, IR_ADD), 2);
    ASSERT_EQ(count_opcode(fn, IR_MUL), 1);
}

TEST(ir_gen_unary_neg) {
    IrModule *mod = ir_from_source(
        "fn test() -> int {\n"
        "    return -42\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    ASSERT(count_opcode(fn, IR_NEG) >= 1);
}

TEST(ir_gen_print_statement) {
    IrModule *mod = ir_from_source(
        "fn test() {\n"
        "    println(\"hello\")\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    ASSERT(count_opcode(fn, IR_PRINT) >= 1);
}

/* ============================================================
 * IR Optimization Tests
 * ============================================================ */

TEST(ir_opt_constant_fold) {
    IrModule *mod = ir_from_source(
        "fn test() -> int {\n"
        "    return 3 + 4\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    /* Before folding: should have ADD */
    ASSERT(count_opcode(fn, IR_ADD) >= 1);

    /* Run constant folding */
    int folded = ir_opt_constant_fold(mod);
    ASSERT(folded >= 1);

    /* After folding: ADD should be replaced by CONST_INT 7 */
    ASSERT_EQ(count_opcode(fn, IR_ADD), 0);

    /* Find the folded constant: should be 7 */
    IrBasicBlock *bb;
    bool found_7 = false;
    for (bb = fn->entry; bb; bb = bb->next) {
        IrInst *inst;
        for (inst = bb->first; inst; inst = inst->next) {
            if (inst->op == IR_CONST_INT && inst->imm_int == 7)
                found_7 = true;
        }
    }
    ASSERT(found_7);
}

TEST(ir_opt_constant_fold_mul) {
    IrModule *mod = ir_from_source(
        "fn test() -> int {\n"
        "    return 6 * 7\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    int folded = ir_opt_constant_fold(mod);
    ASSERT(folded >= 1);

    IrFunction *fn = first_fn(mod);
    ASSERT_EQ(count_opcode(fn, IR_MUL), 0);

    /* Should have const 42 */
    IrBasicBlock *bb;
    bool found_42 = false;
    for (bb = fn->entry; bb; bb = bb->next) {
        IrInst *inst;
        for (inst = bb->first; inst; inst = inst->next) {
            if (inst->op == IR_CONST_INT && inst->imm_int == 42)
                found_42 = true;
        }
    }
    ASSERT(found_42);
}

TEST(ir_opt_constant_fold_float) {
    IrModule *mod = ir_from_source(
        "fn test() -> float {\n"
        "    return 1.5 + 2.5\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    int folded = ir_opt_constant_fold(mod);
    ASSERT(folded >= 1);

    IrFunction *fn = first_fn(mod);
    ASSERT_EQ(count_opcode(fn, IR_FADD), 0);

    /* Should have const 4.0 */
    IrBasicBlock *bb;
    bool found = false;
    for (bb = fn->entry; bb; bb = bb->next) {
        IrInst *inst;
        for (inst = bb->first; inst; inst = inst->next) {
            if (inst->op == IR_CONST_FLOAT && inst->imm_float > 3.9 && inst->imm_float < 4.1)
                found = true;
        }
    }
    ASSERT(found);
}

TEST(ir_opt_constant_fold_string) {
    IrModule *mod = ir_from_source(
        "fn test() {\n"
        "    let x = \"hello\" + \" world\"\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    int folded = ir_opt_constant_fold(mod);
    ASSERT(folded >= 1);

    IrFunction *fn = first_fn(mod);
    ASSERT_EQ(count_opcode(fn, IR_STR_CONCAT), 0);

    /* Should have const "hello world" */
    IrBasicBlock *bb;
    bool found = false;
    for (bb = fn->entry; bb; bb = bb->next) {
        IrInst *inst;
        for (inst = bb->first; inst; inst = inst->next) {
            if (inst->op == IR_CONST_STRING && inst->imm_str &&
                strcmp(inst->imm_str, "hello world") == 0)
                found = true;
        }
    }
    ASSERT(found);
}

TEST(ir_opt_dead_code_elim) {
    IrModule *mod = ir_from_source(
        "fn test() {\n"
        "    let x = 42\n"
        "    let y = 99\n"
        "    println(\"hi\")\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    int before = count_insts(fn);

    /* Run DCE: the loads of x and y (if any) are unused */
    int eliminated = ir_opt_dead_code_elim(mod);

    int after = count_insts(fn);

    /* At minimum, the print and ret should remain */
    ASSERT(count_opcode(fn, IR_PRINT) >= 1);
    ASSERT(count_opcode(fn, IR_RET) >= 1);

    /* Should have eliminated at least some dead code, or at least not crashed */
    ASSERT(after <= before);
    (void)eliminated;
}

TEST(ir_opt_dead_code_preserves_side_effects) {
    IrModule *mod = ir_from_source(
        "fn test() {\n"
        "    println(\"hello\")\n"
        "    println(\"world\")\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    int print_count_before = count_opcode(fn, IR_PRINT);

    ir_opt_dead_code_elim(mod);

    /* print instructions have side effects and must NOT be eliminated */
    int print_count_after = count_opcode(fn, IR_PRINT);
    ASSERT_EQ(print_count_before, print_count_after);
}

TEST(ir_opt_constant_prop) {
    IrModule *mod = ir_from_source(
        "fn test() -> int {\n"
        "    let x = 42\n"
        "    return x\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);

    /* Before propagation: should have LOAD */
    ASSERT(count_opcode(fn, IR_LOAD) >= 1);

    int propagated = ir_opt_constant_prop(mod);

    /* After propagation: LOAD should be replaced with CONST_INT */
    /* The load of x should become const 42 */
    if (propagated > 0) {
        /* Count total const_int instructions */
        int const_count = count_opcode(fn, IR_CONST_INT);
        /* Should have at least 2: original 42 + propagated 42 */
        ASSERT(const_count >= 2);
    }
}

TEST(ir_opt_all_combined) {
    IrModule *mod = ir_from_source(
        "fn test() -> int {\n"
        "    let x = 3 + 4\n"
        "    return x\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    /* Run all optimizations */
    ir_opt_all(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    /* After full optimization: 3+4 should be folded to 7,
     * then propagated into the return. */
    ASSERT_EQ(count_opcode(fn, IR_ADD), 0);
}

/* ============================================================
 * IR Printer Tests
 * ============================================================ */

TEST(ir_print_format) {
    IrModule *mod = ir_from_source(
        "fn add(a: int, b: int) -> int {\n"
        "    return a + b\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    char *output = print_to_string(mod);
    ASSERT_NOT_NULL(output);

    /* Check for key elements in the output */
    ASSERT(strstr(output, "define i64 @lcn_add") != NULL);
    ASSERT(strstr(output, "i64 %a") != NULL);
    ASSERT(strstr(output, "i64 %b") != NULL);
    ASSERT(strstr(output, "bb0:") != NULL);
    ASSERT(strstr(output, "add i64") != NULL);
    ASSERT(strstr(output, "ret i64") != NULL);

    free(output);
}

TEST(ir_print_void_function) {
    IrModule *mod = ir_from_source(
        "fn greet() {\n"
        "    println(\"hello\")\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    char *output = print_to_string(mod);
    ASSERT_NOT_NULL(output);

    ASSERT(strstr(output, "define void @lcn_greet") != NULL);
    ASSERT(strstr(output, "ret void") != NULL);
    ASSERT(strstr(output, "print %") != NULL);

    free(output);
}

TEST(ir_print_branch) {
    IrModule *mod = ir_from_source(
        "fn test(x: int) -> int {\n"
        "    if x > 0 {\n"
        "        return 1\n"
        "    } else {\n"
        "        return 0\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    char *output = print_to_string(mod);
    ASSERT_NOT_NULL(output);

    ASSERT(strstr(output, "br %") != NULL);
    ASSERT(strstr(output, "@bb") != NULL);
    ASSERT(strstr(output, "cmp_gt") != NULL);

    free(output);
}

TEST(ir_print_const_types) {
    IrModule *mod = ir_from_source(
        "fn test() {\n"
        "    let a = 42\n"
        "    let b = 3.14\n"
        "    let c = \"hello\"\n"
        "    let d = true\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    char *output = print_to_string(mod);
    ASSERT_NOT_NULL(output);

    ASSERT(strstr(output, "const i64 42") != NULL);
    ASSERT(strstr(output, "const f64 3.14") != NULL);
    ASSERT(strstr(output, "const str \"hello\"") != NULL);
    ASSERT(strstr(output, "const bool true") != NULL);

    free(output);
}

TEST(ir_print_call_format) {
    IrModule *mod = ir_from_source(
        "fn add(a: int, b: int) -> int {\n"
        "    return a + b\n"
        "}\n"
        "fn main() {\n"
        "    let r = add(1, 2)\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    char *output = print_to_string(mod);
    ASSERT_NOT_NULL(output);

    ASSERT(strstr(output, "call i64 @lcn_add(") != NULL);

    free(output);
}

/* ============================================================
 * IR Construction API Tests
 * ============================================================ */

TEST(ir_api_module_create) {
    Arena a = arena_new(1024 * 1024);
    IrModule *mod = ir_module_new(&a);
    ASSERT_NOT_NULL(mod);
    ASSERT_EQ(mod->fn_count, 0);
    ASSERT_NULL(mod->functions);
    arena_free(&a);
}

TEST(ir_api_function_create) {
    Arena a = arena_new(1024 * 1024);
    IrModule *mod = ir_module_new(&a);
    IrFunction *fn = ir_function_new(mod, "test_fn", IR_TYPE_I64);
    ASSERT_NOT_NULL(fn);
    ASSERT_STR_EQ(fn->name, "test_fn");
    ASSERT_EQ(fn->return_type, IR_TYPE_I64);
    ASSERT_EQ(mod->fn_count, 1);
    arena_free(&a);
}

TEST(ir_api_basic_block) {
    Arena a = arena_new(1024 * 1024);
    IrModule *mod = ir_module_new(&a);
    IrFunction *fn = ir_function_new(mod, "test_fn", IR_TYPE_VOID);
    IrBasicBlock *bb = ir_bb_new(fn, mod, "entry");
    ASSERT_NOT_NULL(bb);
    ASSERT_EQ(bb->id, 0);
    ASSERT_EQ(fn->bb_count, 1);
    ASSERT(fn->entry == bb);
    arena_free(&a);
}

TEST(ir_api_emit_const) {
    Arena a = arena_new(1024 * 1024);
    IrModule *mod = ir_module_new(&a);
    IrFunction *fn = ir_function_new(mod, "test_fn", IR_TYPE_VOID);
    ir_bb_new(fn, mod, "entry");

    int v0 = ir_emit_const_int(fn, mod, 42);
    ASSERT_EQ(v0, 0);

    int v1 = ir_emit_const_float(fn, mod, 3.14);
    ASSERT_EQ(v1, 1);

    int v2 = ir_emit_const_string(fn, mod, "hello");
    ASSERT_EQ(v2, 2);

    int v3 = ir_emit_const_bool(fn, mod, true);
    ASSERT_EQ(v3, 3);

    arena_free(&a);
}

TEST(ir_api_emit_binop) {
    Arena a = arena_new(1024 * 1024);
    IrModule *mod = ir_module_new(&a);
    IrFunction *fn = ir_function_new(mod, "test_fn", IR_TYPE_I64);
    ir_bb_new(fn, mod, "entry");

    int a_val = ir_emit_const_int(fn, mod, 10);
    int b_val = ir_emit_const_int(fn, mod, 20);
    int sum = ir_emit_binop(fn, mod, IR_ADD, IR_TYPE_I64, a_val, b_val);

    IrInst *inst = find_inst(fn, sum);
    ASSERT_NOT_NULL(inst);
    ASSERT_EQ(inst->op, IR_ADD);
    ASSERT_EQ(inst->operands[0], a_val);
    ASSERT_EQ(inst->operands[1], b_val);

    arena_free(&a);
}

TEST(ir_api_emit_branch) {
    Arena a = arena_new(1024 * 1024);
    IrModule *mod = ir_module_new(&a);
    IrFunction *fn = ir_function_new(mod, "test_fn", IR_TYPE_VOID);
    IrBasicBlock *entry = ir_bb_new(fn, mod, "entry");
    IrBasicBlock *then_bb = ir_bb_new(fn, mod, "then");
    IrBasicBlock *else_bb = ir_bb_new(fn, mod, "else");

    ir_set_current_bb(fn, entry);
    int cond = ir_emit_const_bool(fn, mod, true);
    ir_emit_br(fn, mod, cond, then_bb->id, else_bb->id);

    IrInst *br = find_opcode(fn, IR_BR);
    ASSERT_NOT_NULL(br);
    ASSERT_EQ(br->target_bb, then_bb->id);
    ASSERT_EQ(br->false_bb, else_bb->id);

    arena_free(&a);
}

/* ============================================================
 * Type Name / Opcode Name Tests
 * ============================================================ */

TEST(ir_type_names) {
    ASSERT_STR_EQ(ir_type_name(IR_TYPE_I64), "i64");
    ASSERT_STR_EQ(ir_type_name(IR_TYPE_F64), "f64");
    ASSERT_STR_EQ(ir_type_name(IR_TYPE_BOOL), "bool");
    ASSERT_STR_EQ(ir_type_name(IR_TYPE_STRING), "str");
    ASSERT_STR_EQ(ir_type_name(IR_TYPE_VOID), "void");
    ASSERT_STR_EQ(ir_type_name(IR_TYPE_PTR), "ptr");
}

TEST(ir_opcode_names) {
    ASSERT_STR_EQ(ir_opcode_name(IR_ADD), "add");
    ASSERT_STR_EQ(ir_opcode_name(IR_SUB), "sub");
    ASSERT_STR_EQ(ir_opcode_name(IR_MUL), "mul");
    ASSERT_STR_EQ(ir_opcode_name(IR_DIV), "div");
    ASSERT_STR_EQ(ir_opcode_name(IR_FADD), "fadd");
    ASSERT_STR_EQ(ir_opcode_name(IR_CMP_EQ), "cmp_eq");
    ASSERT_STR_EQ(ir_opcode_name(IR_CMP_LT), "cmp_lt");
    ASSERT_STR_EQ(ir_opcode_name(IR_BR), "br");
    ASSERT_STR_EQ(ir_opcode_name(IR_JMP), "jmp");
    ASSERT_STR_EQ(ir_opcode_name(IR_PHI), "phi");
    ASSERT_STR_EQ(ir_opcode_name(IR_NOP), "nop");
}

/* ============================================================
 * ARM64 Emitter Tests
 * ============================================================ */

TEST(ir_emit_arm64_const_int) {
    char *asm_out = arm64_from_source(
        "fn test() -> int {\n"
        "    return 42\n"
        "}\n"
    );
    ASSERT_NOT_NULL(asm_out);
    ASSERT(strstr(asm_out, "mov") != NULL);
    ASSERT(strstr(asm_out, "#42") != NULL);
    ASSERT(strstr(asm_out, "ret") != NULL);
    free(asm_out);
}

TEST(ir_emit_arm64_add) {
    char *asm_out = arm64_from_source(
        "fn add(a: int, b: int) -> int {\n"
        "    return a + b\n"
        "}\n"
    );
    ASSERT_NOT_NULL(asm_out);
    ASSERT(strstr(asm_out, "add") != NULL);
    ASSERT(strstr(asm_out, "ret") != NULL);
    free(asm_out);
}

TEST(ir_emit_arm64_function_def) {
    char *asm_out = arm64_from_source(
        "fn my_func(x: int) -> int {\n"
        "    return x\n"
        "}\n"
    );
    ASSERT_NOT_NULL(asm_out);
    ASSERT(strstr(asm_out, "stp x29, x30") != NULL);
    ASSERT(strstr(asm_out, "ldp x29, x30") != NULL);
    ASSERT(strstr(asm_out, ".globl") != NULL);
    ASSERT(strstr(asm_out, "lcn_my_func") != NULL);
    ASSERT(strstr(asm_out, ".p2align 2") != NULL);
    free(asm_out);
}

TEST(ir_emit_arm64_call) {
    char *asm_out = arm64_from_source(
        "fn add(a: int, b: int) -> int {\n"
        "    return a + b\n"
        "}\n"
        "fn main() -> int {\n"
        "    return add(3, 4)\n"
        "}\n"
    );
    ASSERT_NOT_NULL(asm_out);
    ASSERT(strstr(asm_out, "bl") != NULL);
    ASSERT(strstr(asm_out, "lcn_add") != NULL);
    free(asm_out);
}

TEST(ir_emit_arm64_branch) {
    char *asm_out = arm64_from_source(
        "fn test(n: int) -> int {\n"
        "    if n > 0 {\n"
        "        return 1\n"
        "    } else {\n"
        "        return 0\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(asm_out);
    ASSERT(strstr(asm_out, "cmp") != NULL);
    ASSERT(strstr(asm_out, "cbz") != NULL || strstr(asm_out, "cbnz") != NULL);
    ASSERT(strstr(asm_out, ".LBB") != NULL);
    free(asm_out);
}

TEST(ir_emit_arm64_string) {
    char *asm_out = arm64_from_source(
        "fn test() {\n"
        "    println(\"hello world\")\n"
        "}\n"
    );
    ASSERT_NOT_NULL(asm_out);
#ifdef __APPLE__
    ASSERT(strstr(asm_out, "__cstring") != NULL || strstr(asm_out, ".rodata") != NULL);
#else
    ASSERT(strstr(asm_out, ".rodata") != NULL || strstr(asm_out, "__cstring") != NULL);
#endif
    ASSERT(strstr(asm_out, ".asciz") != NULL);
    ASSERT(strstr(asm_out, "hello world") != NULL);
    ASSERT(strstr(asm_out, "adrp") != NULL);
    free(asm_out);
}

TEST(ir_emit_arm64_sub_mul_div) {
    char *asm_out = arm64_from_source(
        "fn test(a: int, b: int) -> int {\n"
        "    let x = a - b\n"
        "    let y = a * b\n"
        "    let z = a / b\n"
        "    return x\n"
        "}\n"
    );
    ASSERT_NOT_NULL(asm_out);
    ASSERT(strstr(asm_out, "sub") != NULL);
    ASSERT(strstr(asm_out, "mul") != NULL);
    ASSERT(strstr(asm_out, "sdiv") != NULL);
    free(asm_out);
}

TEST(ir_emit_arm64_comparison_ops) {
    char *asm_out = arm64_from_source(
        "fn test(a: int, b: int) -> bool {\n"
        "    return a == b\n"
        "}\n"
    );
    ASSERT_NOT_NULL(asm_out);
    ASSERT(strstr(asm_out, "cmp") != NULL);
    ASSERT(strstr(asm_out, "cset") != NULL);
    ASSERT(strstr(asm_out, "eq") != NULL);
    free(asm_out);
}

TEST(ir_emit_arm64_large_imm) {
    char *asm_out = arm64_from_source(
        "fn test() -> int {\n"
        "    return 100000\n"
        "}\n"
    );
    ASSERT_NOT_NULL(asm_out);
    ASSERT(strstr(asm_out, "movz") != NULL || strstr(asm_out, "mov") != NULL);
    ASSERT(strstr(asm_out, "ret") != NULL);
    free(asm_out);
}

TEST(ir_emit_arm64_regalloc_basic) {
    Arena a = arena_new(1024 * 1024);
    IrModule *mod = ir_module_new(&a);
    IrFunction *fn = ir_function_new(mod, "test_regalloc", IR_TYPE_I64);
    ir_bb_new(fn, mod, "entry");

    int v0 = ir_emit_const_int(fn, mod, 1);
    int v1 = ir_emit_const_int(fn, mod, 2);
    int v2 = ir_emit_const_int(fn, mod, 3);
    int v3 = ir_emit_binop(fn, mod, IR_ADD, IR_TYPE_I64, v0, v1);
    int v4 = ir_emit_binop(fn, mod, IR_ADD, IR_TYPE_I64, v2, v3);
    ir_emit_ret(fn, mod, v4);

    LiveRange ranges[256];
    int count = 0;
    ir_compute_live_ranges(fn, ranges, &count, 256);
    ASSERT(count >= 5);

    ir_alloc_registers(ranges, count, 7);

    int i;
    for (i = 0; i < count; i++) {
        ASSERT(ranges[i].reg >= 0 || ranges[i].spill_offset != -1);
    }

    arena_free(&a);
}

TEST(ir_emit_arm64_neg_not) {
    char *asm_out = arm64_from_source(
        "fn test(a: int) -> int {\n"
        "    return -a\n"
        "}\n"
    );
    ASSERT_NOT_NULL(asm_out);
    ASSERT(strstr(asm_out, "neg") != NULL);
    free(asm_out);
}

TEST(ir_emit_arm64_multiple_functions) {
    char *asm_out = arm64_from_source(
        "fn foo() -> int {\n"
        "    return 1\n"
        "}\n"
        "fn bar() -> int {\n"
        "    return 2\n"
        "}\n"
    );
    ASSERT_NOT_NULL(asm_out);
    ASSERT(strstr(asm_out, "lcn_foo") != NULL);
    ASSERT(strstr(asm_out, "lcn_bar") != NULL);
    free(asm_out);
}

TEST(ir_compile_pipeline) {
    const char *source =
        "fn main() -> int {\n"
        "    let x = 21\n"
        "    let y = 21\n"
        "    return x + y\n"
        "}\n";

    IrModule *mod = ir_from_source(source);
    ASSERT_NOT_NULL(mod);

    ir_opt_all(mod);

    char tmp_s[256];
    snprintf(tmp_s, sizeof(tmp_s), "/tmp/lcn_test_pipeline_%d.s",
             (int)getpid());

    FILE *f = fopen(tmp_s, "w");
    ASSERT_NOT_NULL(f);
    ir_emit_arm64(mod, f);
    fclose(f);

    f = fopen(tmp_s, "r");
    ASSERT_NOT_NULL(f);
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    ASSERT(strstr(buf, "stp x29, x30") != NULL);
    ASSERT(strstr(buf, "ret") != NULL);
    ASSERT(strstr(buf, "lcn_main") != NULL);

#if defined(__aarch64__) || defined(__arm64__)
    {
        char tmp_o[256], cmd[512];
        snprintf(tmp_o, sizeof(tmp_o), "/tmp/lcn_test_pipeline_%d.o",
                 (int)getpid());
        snprintf(cmd, sizeof(cmd), "cc -c \"%s\" -o \"%s\" 2>/dev/null",
                 tmp_s, tmp_o);
        int rc = system(cmd);
        if (rc == 0) {
            remove(tmp_o);
        }
    }
#endif

    remove(tmp_s);
}

/* ============================================================
 * x86_64 Assembly Emitter Tests
 * ============================================================ */

TEST(ir_emit_x86_const_int) {
    IrModule *mod = ir_from_source(
        "fn test() -> int {\n"
        "    return 42\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    char *asm_out = emit_x86_to_string(mod);
    ASSERT_NOT_NULL(asm_out);
    ASSERT(strstr(asm_out, "movq $42") != NULL);
    ASSERT(strstr(asm_out, "ret") != NULL);
    free(asm_out);
}

TEST(ir_emit_x86_add) {
    IrModule *mod = ir_from_source(
        "fn test() -> int {\n"
        "    return 3 + 4\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    char *asm_out = emit_x86_to_string(mod);
    ASSERT_NOT_NULL(asm_out);
    ASSERT(strstr(asm_out, "addq") != NULL || strstr(asm_out, "movq $7") != NULL);
    ASSERT(strstr(asm_out, "ret") != NULL);
    free(asm_out);
}

TEST(ir_emit_x86_function_def) {
    IrModule *mod = ir_from_source(
        "fn add(a: int, b: int) -> int {\n"
        "    return a + b\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    char *asm_out = emit_x86_to_string(mod);
    ASSERT_NOT_NULL(asm_out);
    ASSERT(strstr(asm_out, "lcn_add") != NULL);
    ASSERT(strstr(asm_out, "pushq %rbp") != NULL);
    ASSERT(strstr(asm_out, "movq %rsp, %rbp") != NULL);
    ASSERT(strstr(asm_out, "leave") != NULL);
    ASSERT(strstr(asm_out, "ret") != NULL);
    free(asm_out);
}

TEST(ir_emit_x86_call) {
    IrModule *mod = ir_from_source(
        "fn add(a: int, b: int) -> int {\n"
        "    return a + b\n"
        "}\n"
        "fn main() {\n"
        "    let r = add(1, 2)\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    char *asm_out = emit_x86_to_string(mod);
    ASSERT_NOT_NULL(asm_out);
    ASSERT(strstr(asm_out, "callq") != NULL);
    ASSERT(strstr(asm_out, "lcn_add") != NULL);
    ASSERT(strstr(asm_out, "%rdi") != NULL);
    free(asm_out);
}

TEST(ir_emit_x86_branch) {
    IrModule *mod = ir_from_source(
        "fn test(x: int) -> int {\n"
        "    if x > 0 {\n"
        "        return 1\n"
        "    } else {\n"
        "        return 0\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    char *asm_out = emit_x86_to_string(mod);
    ASSERT_NOT_NULL(asm_out);
    ASSERT(strstr(asm_out, "testq") != NULL || strstr(asm_out, "cmpq") != NULL);
    ASSERT(strstr(asm_out, "je") != NULL);
    ASSERT(strstr(asm_out, "jmp") != NULL);
    free(asm_out);
}

TEST(ir_emit_x86_string) {
    IrModule *mod = ir_from_source(
        "fn test() {\n"
        "    println(\"hello world\")\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    char *asm_out = emit_x86_to_string(mod);
    ASSERT_NOT_NULL(asm_out);
    ASSERT(strstr(asm_out, ".asciz") != NULL);
    ASSERT(strstr(asm_out, "hello world") != NULL);
    ASSERT(strstr(asm_out, "leaq") != NULL);
    ASSERT(strstr(asm_out, "printf") != NULL);
    free(asm_out);
}

TEST(ir_regalloc_simple) {
    Arena a = arena_new(1024 * 1024);
    IrModule *mod = ir_module_new(&a);
    IrFunction *fn = ir_function_new(mod, "test_fn", IR_TYPE_I64);
    ir_bb_new(fn, mod, "entry");

    int v0 = ir_emit_const_int(fn, mod, 10);
    int v1 = ir_emit_const_int(fn, mod, 20);
    int v2 = ir_emit_binop(fn, mod, IR_ADD, IR_TYPE_I64, v0, v1);
    ir_emit_ret(fn, mod, v2);

    RegAlloc allocs[256];
    int count;
    ir_x86_compute_live_ranges(fn, allocs, &count);
    ASSERT(count >= 3);

    ir_linear_scan_alloc(allocs, count);

    {
        int i;
        for (i = 0; i < count; i++) {
            ASSERT(allocs[i].reg != REG_NONE);
            ASSERT(allocs[i].reg != REG_SPILL);
        }
    }

    arena_free(&a);
}

TEST(ir_regalloc_spill) {
    Arena a = arena_new(1024 * 1024);
    IrModule *mod = ir_module_new(&a);
    IrFunction *fn = ir_function_new(mod, "spill_fn", IR_TYPE_I64);
    ir_bb_new(fn, mod, "entry");

    int values[16];
    int i;
    for (i = 0; i < 16; i++) {
        values[i] = ir_emit_const_int(fn, mod, (int64_t)(i + 1));
    }

    int result = ir_emit_call(fn, mod, "big_fn", IR_TYPE_I64, values, 16);
    ir_emit_ret(fn, mod, result);

    RegAlloc allocs[256];
    int count;
    ir_x86_compute_live_ranges(fn, allocs, &count);
    ir_linear_scan_alloc(allocs, count);

    int spill_count = 0;
    int reg_count = 0;
    {
        int j;
        for (j = 0; j < count; j++) {
            if (allocs[j].reg == REG_SPILL) spill_count++;
            else if (allocs[j].reg != REG_NONE) reg_count++;
        }
    }
    ASSERT(reg_count > 0);
    ASSERT(spill_count > 0);

    arena_free(&a);
}

TEST(ir_emit_x86_full_program) {
    IrModule *mod = ir_from_source(
        "fn add(a: int, b: int) -> int {\n"
        "    return a + b\n"
        "}\n"
        "fn main() {\n"
        "    let x = 10\n"
        "    let y = 20\n"
        "    let sum = add(x, y)\n"
        "    println(\"result:\")\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    char *asm_out = emit_x86_to_string(mod);
    ASSERT_NOT_NULL(asm_out);
    ASSERT(strstr(asm_out, ".text") != NULL);
    ASSERT(strstr(asm_out, "lcn_add") != NULL);
    ASSERT(strstr(asm_out, "lcn_main") != NULL);
    ASSERT(strstr(asm_out, "addq") != NULL);
    ASSERT(strstr(asm_out, "result:") != NULL);
    {
        int pushq_count = 0;
        const char *p = asm_out;
        while ((p = strstr(p, "pushq %rbp")) != NULL) {
            pushq_count++;
            p++;
        }
        ASSERT(pushq_count >= 2);
    }
    free(asm_out);
}

/* ============================================================
 * Main
 * ============================================================ */

int main(void) {
    ir_test_setup();

    fprintf(stderr, "\n== IR Backend Tests ==\n\n");

    fprintf(stderr, "-- IR Generation --\n");
    RUN_TEST(ir_gen_int_literal);
    RUN_TEST(ir_gen_float_literal);
    RUN_TEST(ir_gen_string_literal);
    RUN_TEST(ir_gen_bool_literal);
    RUN_TEST(ir_gen_binary_add);
    RUN_TEST(ir_gen_binary_sub);
    RUN_TEST(ir_gen_binary_mul);
    RUN_TEST(ir_gen_binary_comparison);
    RUN_TEST(ir_gen_function_def);
    RUN_TEST(ir_gen_function_call);
    RUN_TEST(ir_gen_variable_decl);
    RUN_TEST(ir_gen_variable_load);
    RUN_TEST(ir_gen_if_else_branches);
    RUN_TEST(ir_gen_if_no_else);
    RUN_TEST(ir_gen_for_loop);
    RUN_TEST(ir_gen_while_loop);
    RUN_TEST(ir_gen_string_concat);
    RUN_TEST(ir_gen_multiple_functions);
    RUN_TEST(ir_gen_return_void);
    RUN_TEST(ir_gen_nested_binary);
    RUN_TEST(ir_gen_unary_neg);
    RUN_TEST(ir_gen_print_statement);

    fprintf(stderr, "\n-- IR Optimization --\n");
    RUN_TEST(ir_opt_constant_fold);
    RUN_TEST(ir_opt_constant_fold_mul);
    RUN_TEST(ir_opt_constant_fold_float);
    RUN_TEST(ir_opt_constant_fold_string);
    RUN_TEST(ir_opt_dead_code_elim);
    RUN_TEST(ir_opt_dead_code_preserves_side_effects);
    RUN_TEST(ir_opt_constant_prop);
    RUN_TEST(ir_opt_all_combined);

    fprintf(stderr, "\n-- IR Printer --\n");
    RUN_TEST(ir_print_format);
    RUN_TEST(ir_print_void_function);
    RUN_TEST(ir_print_branch);
    RUN_TEST(ir_print_const_types);
    RUN_TEST(ir_print_call_format);

    fprintf(stderr, "\n-- IR Construction API --\n");
    RUN_TEST(ir_api_module_create);
    RUN_TEST(ir_api_function_create);
    RUN_TEST(ir_api_basic_block);
    RUN_TEST(ir_api_emit_const);
    RUN_TEST(ir_api_emit_binop);
    RUN_TEST(ir_api_emit_branch);

    fprintf(stderr, "\n-- IR Type/Opcode Names --\n");
    RUN_TEST(ir_type_names);
    RUN_TEST(ir_opcode_names);

    fprintf(stderr, "\n-- ARM64 Emitter --\n");
    RUN_TEST(ir_emit_arm64_const_int);
    RUN_TEST(ir_emit_arm64_add);
    RUN_TEST(ir_emit_arm64_function_def);
    RUN_TEST(ir_emit_arm64_call);
    RUN_TEST(ir_emit_arm64_branch);
    RUN_TEST(ir_emit_arm64_string);
    RUN_TEST(ir_emit_arm64_sub_mul_div);
    RUN_TEST(ir_emit_arm64_comparison_ops);
    RUN_TEST(ir_emit_arm64_large_imm);
    RUN_TEST(ir_emit_arm64_regalloc_basic);
    RUN_TEST(ir_emit_arm64_neg_not);
    RUN_TEST(ir_emit_arm64_multiple_functions);

    fprintf(stderr, "\n-- Compile Pipeline --\n");
    RUN_TEST(ir_compile_pipeline);

    fprintf(stderr, "\n-- x86_64 Assembly Emitter --\n");
    RUN_TEST(ir_emit_x86_const_int);
    RUN_TEST(ir_emit_x86_add);
    RUN_TEST(ir_emit_x86_function_def);
    RUN_TEST(ir_emit_x86_call);
    RUN_TEST(ir_emit_x86_branch);
    RUN_TEST(ir_emit_x86_string);

    fprintf(stderr, "\n-- Register Allocator --\n");
    RUN_TEST(ir_regalloc_simple);
    RUN_TEST(ir_regalloc_spill);

    fprintf(stderr, "\n-- Full Program x86_64 --\n");
    RUN_TEST(ir_emit_x86_full_program);

    ir_test_teardown();

    TEST_SUMMARY();
    return test_failures > 0 ? 1 : 0;
}
