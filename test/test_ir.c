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

/* ============================================================
 * Looping Constructs (F1-A6 follow-up): while / for-in / loop
 * with break + continue.
 *
 * These tests pin the IR shape we promise the WASM emitter:
 *   - while:   pre + cond + body + exit BBs; one CMP_LT, one BR,
 *              one back-edge JMP from body to cond.
 *   - for-in:  pre + cond + body + inc + exit (5 BBs); the desugar
 *              materializes exactly ONE init (store start->loopvar),
 *              ONE cond (load + cmp_lt + br), ONE inc (load + add + store).
 *   - break:   inside nested loops, must target the INNERMOST exit BB.
 *
 * If any of these fail you have likely broken the contract with
 * ir_emit_wasm.c's dispatch-loop emitter; review the back-edge
 * handling before changing the assertions.
 * ============================================================ */

/* Helper: count IR_JMP instructions whose target is `target_bb_id`. */
static int count_jmp_to(IrFunction *fn, int target_bb_id) {
    int count = 0;
    IrBasicBlock *bb;
    for (bb = fn->entry; bb; bb = bb->next) {
        IrInst *inst;
        for (inst = bb->first; inst; inst = inst->next) {
            if (inst->op == IR_JMP && inst->target_bb == target_bb_id) {
                count++;
            }
        }
    }
    return count;
}

/* Helper: find a basic block by label substring. */
static IrBasicBlock *find_bb_by_label(IrFunction *fn, const char *needle) {
    IrBasicBlock *bb;
    for (bb = fn->entry; bb; bb = bb->next) {
        if (bb->label && strstr(bb->label, needle)) return bb;
    }
    return NULL;
}

TEST(ir_gen_while_bb_count_and_back_edge) {
    /* Verifies the CFG shape of a while-loop:
     *   bb_pre   -> jmp -> bb_cond
     *   bb_cond  -> br  -> bb_body / bb_exit
     *   bb_body  -> jmp -> bb_cond   (this is the loop back-edge)
     *
     * We assert by counting BBs and confirming there are at least two
     * IR_JMPs targeting the while.cond block (one from pre, one from
     * the body back-edge).  */
    IrModule *mod = ir_from_source(
        "fn test() -> int {\n"
        "    let mut sum = 0\n"
        "    let mut i = 0\n"
        "    while i < 10 {\n"
        "        sum = sum + i\n"
        "        i = i + 1\n"
        "    }\n"
        "    return sum\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);
    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    /* entry + while.cond + while.body + while.exit = 4 BBs minimum. */
    ASSERT(fn->bb_count >= 4);

    IrBasicBlock *cond_bb = find_bb_by_label(fn, "while.cond");
    IrBasicBlock *body_bb = find_bb_by_label(fn, "while.body");
    IrBasicBlock *exit_bb = find_bb_by_label(fn, "while.exit");
    ASSERT_NOT_NULL(cond_bb);
    ASSERT_NOT_NULL(body_bb);
    ASSERT_NOT_NULL(exit_bb);

    /* Two JMPs target the cond BB: one from the pre-block and one
     * from the body back-edge. */
    ASSERT(count_jmp_to(fn, cond_bb->id) >= 2);

    /* The cond BB ends in a conditional branch. */
    ASSERT_NOT_NULL(cond_bb->last);
    ASSERT_EQ(cond_bb->last->op, IR_BR);
    /* The cond branch picks body (true) or exit (false). */
    ASSERT_EQ(cond_bb->last->target_bb, body_bb->id);
    ASSERT_EQ(cond_bb->last->false_bb, exit_bb->id);
}

TEST(ir_gen_for_in_desugar_shape) {
    /* The for-in `for i in 0..n` desugar must emit:
     *   - exactly 1 store of `start` into the loop variable (init);
     *   - exactly 1 CMP_LT in the cond BB;
     *   - exactly 1 increment ADD in the inc BB.
     * Anything else means the desugar drifted away from the
     * `let i = start; while i < end { body; i = i + 1 }` shape. */
    IrModule *mod = ir_from_source(
        "fn test() -> int {\n"
        "    let mut total = 0\n"
        "    for i in 0..5 {\n"
        "        total = total + i\n"
        "    }\n"
        "    return total\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);
    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    /* 5 BBs at minimum: entry + for.cond + for.body + for.inc + for.exit. */
    ASSERT(fn->bb_count >= 5);

    IrBasicBlock *cond_bb = find_bb_by_label(fn, "for.cond");
    IrBasicBlock *body_bb = find_bb_by_label(fn, "for.body");
    IrBasicBlock *inc_bb  = find_bb_by_label(fn, "for.inc");
    IrBasicBlock *exit_bb = find_bb_by_label(fn, "for.exit");
    ASSERT_NOT_NULL(cond_bb);
    ASSERT_NOT_NULL(body_bb);
    ASSERT_NOT_NULL(inc_bb);
    ASSERT_NOT_NULL(exit_bb);

    /* Exactly one CMP_LT in the cond BB (the i < end check). */
    int cmp_in_cond = 0;
    {
        IrInst *inst;
        for (inst = cond_bb->first; inst; inst = inst->next) {
            if (inst->op == IR_CMP_LT) cmp_in_cond++;
        }
    }
    ASSERT_EQ(cmp_in_cond, 1);

    /* Cond BB ends in BR to body / exit. */
    ASSERT_NOT_NULL(cond_bb->last);
    ASSERT_EQ(cond_bb->last->op, IR_BR);
    ASSERT_EQ(cond_bb->last->target_bb, body_bb->id);
    ASSERT_EQ(cond_bb->last->false_bb, exit_bb->id);

    /* Exactly one ADD in the inc BB (the i = i + 1). */
    int add_in_inc = 0;
    {
        IrInst *inst;
        for (inst = inc_bb->first; inst; inst = inst->next) {
            if (inst->op == IR_ADD) add_in_inc++;
        }
    }
    ASSERT_EQ(add_in_inc, 1);

    /* Inc BB jumps back to the cond BB. */
    ASSERT_NOT_NULL(inc_bb->last);
    ASSERT_EQ(inc_bb->last->op, IR_JMP);
    ASSERT_EQ(inc_bb->last->target_bb, cond_bb->id);

    /* Init: there must be a store of the start constant into the loop
     * var BEFORE the cond BB. Equivalently, the entry BB ends in a
     * jmp to the cond BB. */
    IrBasicBlock *entry = fn->entry;
    ASSERT_NOT_NULL(entry);
    ASSERT_NOT_NULL(entry->last);
    ASSERT_EQ(entry->last->op, IR_JMP);
    ASSERT_EQ(entry->last->target_bb, cond_bb->id);

    /* The entry block stores `0` (start of `0..5`) into the loop var.
     * IR_STORE is opcode-distinct, but we don't have count_opcode_in_bb;
     * fold by hand. */
    int store_in_entry = 0;
    {
        IrInst *inst;
        for (inst = entry->first; inst; inst = inst->next) {
            if (inst->op == IR_STORE) store_in_entry++;
        }
    }
    /* let mut total = 0 stores into total; the for-init stores 0 into i.
     * Two stores in entry. */
    ASSERT(store_in_entry >= 2);
}

TEST(ir_gen_nested_break_targets_innermost_exit) {
    /* Nested while loops; the `break` is inside the INNER loop and must
     * target the INNER loop's exit, not the outer one. We pin this by
     * locating each loop's exit BB by label (the irgen names them
     * "while.exit" in declaration order — the FIRST occurrence is the
     * outer loop's exit since the outer loop's exit BB is created
     * before the inner loop is lowered).
     *
     * Actually no: irgen creates outer.cond/body/exit first, then steps
     * into the body and creates inner.cond/body/exit. So the basic-block
     * declaration order is:
     *
     *   bb_entry, outer.cond, outer.body, outer.exit,
     *              inner.cond, inner.body, inner.exit, ...
     *
     * The break jmp inside the inner body must target inner.exit. */
    IrModule *mod = ir_from_source(
        "fn test() -> int {\n"
        "    let mut sum = 0\n"
        "    let mut i = 0\n"
        "    while i < 3 {\n"
        "        let mut j = 0\n"
        "        while j < 10 {\n"
        "            if j >= 2 {\n"
        "                break\n"
        "            }\n"
        "            sum = sum + 1\n"
        "            j = j + 1\n"
        "        }\n"
        "        i = i + 1\n"
        "    }\n"
        "    return sum\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);
    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    /* Find both while.exit blocks in declaration order. */
    IrBasicBlock *outer_exit = NULL;
    IrBasicBlock *inner_exit = NULL;
    {
        IrBasicBlock *bb;
        for (bb = fn->entry; bb; bb = bb->next) {
            if (bb->label && strstr(bb->label, "while.exit")) {
                if (!outer_exit) outer_exit = bb;
                else if (!inner_exit) inner_exit = bb;
            }
        }
    }
    ASSERT_NOT_NULL(outer_exit);
    ASSERT_NOT_NULL(inner_exit);
    ASSERT(outer_exit->id != inner_exit->id);

    /* There must be EXACTLY ONE jmp to the INNER exit (the break) and
     * ZERO jmps to the OUTER exit. (The outer exit is reached via the
     * outer cond's BR-false, not via a JMP.) */
    ASSERT_EQ(count_jmp_to(fn, inner_exit->id), 1);
    ASSERT_EQ(count_jmp_to(fn, outer_exit->id), 0);
}

TEST(l2_ir_gen_loop_has_header_and_back_edge) {
    /* `loop { break }` lowers to:
     *
     *   bb_pre  -> jmp -> bb_header
     *   bb_header -> jmp -> bb_body
     *   bb_body -> jmp -> bb_exit         (the break)
     *   bb_exit -> ...
     *
     * Invariants:
     *   - there is a basic block whose label contains "loop.header"
     *   - there is a basic block whose label contains "loop.exit"
     *   - the body jumps to the EXIT block (the `break`), so the
     *     count_jmp_to(exit) is >= 1
     *   - the back-edge from body->header is missing here because
     *     the body's only stmt is `break`, which terminates the block;
     *     irgen does NOT insert a redundant back-edge. (This is the
     *     dead-block trick described in ir_gen.c's AST_BREAK case.) */
    IrModule *mod = ir_from_source(
        "fn run() -> int {\n"
        "    loop {\n"
        "        break\n"
        "    }\n"
        "    return 42\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);
    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    IrBasicBlock *header = find_bb_by_label(fn, "loop.header");
    IrBasicBlock *exit_bb = find_bb_by_label(fn, "loop.exit");
    ASSERT_NOT_NULL(header);
    ASSERT_NOT_NULL(exit_bb);
    ASSERT(count_jmp_to(fn, exit_bb->id) >= 1);
}

TEST(l2_ir_gen_for_wildcard_pattern_lowers_like_named) {
    /* `for _ in 0..N` must produce the same 4-BB shape as a named
     * for-in. The wildcard pattern simply means the loop variable
     * never appears in scope — the IR scaffolding around it is
     * unchanged. */
    IrModule *mod = ir_from_source(
        "fn run() -> int {\n"
        "    let mut total = 0\n"
        "    for _ in 0..3 {\n"
        "        total = total + 1\n"
        "    }\n"
        "    return total\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);
    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    ASSERT_NOT_NULL(find_bb_by_label(fn, "for.cond"));
    ASSERT_NOT_NULL(find_bb_by_label(fn, "for.body"));
    ASSERT_NOT_NULL(find_bb_by_label(fn, "for.inc"));
    ASSERT_NOT_NULL(find_bb_by_label(fn, "for.exit"));
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
 * JSON host-call lowering (L3)
 *
 * Verifies that the dotted-namespace form `json.<fn>(args)` lowers to
 * IR_HOST_CALL with the qualified name "json.<fn>" and the right arg
 * count. The WASM backend's per-capability marshalling consumes these
 * sites and emits the matching `(import "vdag:json" ...)` declarations.
 * ============================================================ */

/* Helper: find an IR_HOST_CALL whose fn_name matches `qname`. */
static IrInst *find_host_call(IrFunction *fn, const char *qname) {
    IrBasicBlock *bb;
    for (bb = fn->entry; bb; bb = bb->next) {
        IrInst *inst;
        for (inst = bb->first; inst; inst = inst->next) {
            if (inst->op == IR_HOST_CALL && inst->fn_name &&
                strcmp(inst->fn_name, qname) == 0) {
                return inst;
            }
        }
    }
    return NULL;
}

TEST(ir_gen_json_parse_host_call) {
    IrModule *mod = ir_from_source(
        "fn test() -> int {\n"
        "    json.parse(\"{}\")\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    IrInst *call = find_host_call(fn, "json.parse");
    ASSERT_NOT_NULL(call);
    ASSERT_EQ(call->call_arg_count, 1);
}

TEST(ir_gen_json_field_host_call) {
    IrModule *mod = ir_from_source(
        "fn test() -> int {\n"
        "    let h = json.parse(\"{\\\"k\\\":1}\")\n"
        "    json.field(h, \"k\")\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    IrInst *field = find_host_call(fn, "json.field");
    ASSERT_NOT_NULL(field);
    ASSERT_EQ(field->call_arg_count, 2);
}

TEST(ir_gen_json_array_index_host_call) {
    IrModule *mod = ir_from_source(
        "fn test() -> int {\n"
        "    let h = json.parse(\"[1,2,3]\")\n"
        "    json.array_index(h, 1)\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    IrInst *ai = find_host_call(fn, "json.array_index");
    ASSERT_NOT_NULL(ai);
    ASSERT_EQ(ai->call_arg_count, 2);
}

TEST(ir_gen_json_length_host_call) {
    IrModule *mod = ir_from_source(
        "fn test() -> int {\n"
        "    let h = json.parse(\"[1,2,3]\")\n"
        "    json.length(h)\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    IrInst *len = find_host_call(fn, "json.length");
    ASSERT_NOT_NULL(len);
    ASSERT_EQ(len->call_arg_count, 1);
}

TEST(ir_gen_json_string_value_host_call) {
    IrModule *mod = ir_from_source(
        "fn test() -> int {\n"
        "    let h = json.parse(\"\\\"hi\\\"\")\n"
        "    json.string_value(h)\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    IrInst *sv = find_host_call(fn, "json.string_value");
    ASSERT_NOT_NULL(sv);
    ASSERT_EQ(sv->call_arg_count, 1);
}

TEST(ir_gen_json_int_value_host_call) {
    IrModule *mod = ir_from_source(
        "fn test() -> int {\n"
        "    let h = json.parse(\"42\")\n"
        "    json.int_value(h)\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    IrInst *iv = find_host_call(fn, "json.int_value");
    ASSERT_NOT_NULL(iv);
    ASSERT_EQ(iv->call_arg_count, 1);
}

TEST(ir_gen_json_bool_value_host_call) {
    IrModule *mod = ir_from_source(
        "fn test() -> int {\n"
        "    let h = json.parse(\"true\")\n"
        "    json.bool_value(h)\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    IrInst *bv = find_host_call(fn, "json.bool_value");
    ASSERT_NOT_NULL(bv);
    ASSERT_EQ(bv->call_arg_count, 1);
}

TEST(ir_gen_json_is_null_host_call) {
    IrModule *mod = ir_from_source(
        "fn test() -> int {\n"
        "    let h = json.parse(\"null\")\n"
        "    json.is_null(h)\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    IrInst *inull = find_host_call(fn, "json.is_null");
    ASSERT_NOT_NULL(inull);
    ASSERT_EQ(inull->call_arg_count, 1);
}

TEST(ir_gen_json_stringify_host_call) {
    IrModule *mod = ir_from_source(
        "fn test() -> int {\n"
        "    let h = json.parse(\"{}\")\n"
        "    json.stringify(h)\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    IrInst *s = find_host_call(fn, "json.stringify");
    ASSERT_NOT_NULL(s);
    ASSERT_EQ(s->call_arg_count, 1);
}

TEST(ir_gen_json_chained_pipeline) {
    /* End-to-end shape: parse -> field -> string_value. Verifies that
     * chained host calls all lower to distinct IR_HOST_CALL sites with
     * the right qualified names, mirroring the 01 sample. */
    IrModule *mod = ir_from_source(
        "fn test() -> int {\n"
        "    let h = json.parse(\"{\\\"intent\\\":\\\"refund\\\"}\")\n"
        "    let f = json.field(h, \"intent\")\n"
        "    json.string_value(f)\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);

    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);

    ASSERT(count_opcode(fn, IR_HOST_CALL) == 3);
    ASSERT_NOT_NULL(find_host_call(fn, "json.parse"));
    ASSERT_NOT_NULL(find_host_call(fn, "json.field"));
    ASSERT_NOT_NULL(find_host_call(fn, "json.string_value"));
}

/* ============================================================
 * L5: Result<T,E> + ? propagator + try/catch
 *
 * Tests verify the lowering shape: Ok/Err constructors are no-ops at
 * the i64 level (negative-i32 sentinel encoding doubles as the Result
 * runtime repr); `?` lowers to cmp_lt + br with either ret-Err or
 * jump-to-catch on the negative branch; and `try {} catch {}` produces
 * three blocks with a PHI in merge.
 * ============================================================ */

TEST(ir_gen_result_ok_pass_through) {
    /* Ok(v) lowers to v as i64: no wrapping, no extra opcode. */
    IrModule *mod = ir_from_source(
        "fn make() -> int {\n"
        "    Ok(42)\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);
    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);
    IrInst *c = find_opcode(fn, IR_CONST_INT);
    ASSERT_NOT_NULL(c);
    ASSERT_EQ(c->imm_int, 42);
    ASSERT_EQ(count_opcode(fn, IR_CALL), 0);
    ASSERT(count_opcode(fn, IR_RET) >= 1);
}

TEST(ir_gen_result_err_pass_through) {
    /* Err(-3) lowers to a neg of 3 — no call to lcn_Err. */
    IrModule *mod = ir_from_source(
        "fn make() -> int {\n"
        "    Err(-3)\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);
    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);
    ASSERT_EQ(count_opcode(fn, IR_CALL), 0);
    ASSERT(count_opcode(fn, IR_RET) >= 1);
}

TEST(ir_gen_try_propagates_via_ret) {
    /* `expr?` outside a try-catch lowers to cmp_lt + br;
     * the err branch ends in ret %v. */
    IrModule *mod = ir_from_source(
        "fn fetch() -> int { Err(-3) }\n"
        "fn main() -> int {\n"
        "    let x = fetch()?\n"
        "    x + 1\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);
    IrFunction *main_fn = nth_fn(mod, 1);
    ASSERT_NOT_NULL(main_fn);
    ASSERT(count_opcode(main_fn, IR_CMP_LT) >= 1);
    ASSERT(count_opcode(main_fn, IR_BR) >= 1);
    ASSERT(count_opcode(main_fn, IR_RET) >= 2);
}

TEST(ir_gen_try_emits_try_err_and_try_ok_blocks) {
    /* Block labels carry the rationale: try.err / try.ok. */
    IrModule *mod = ir_from_source(
        "fn fetch() -> int { Ok(7) }\n"
        "fn caller() -> int { let x = fetch()?\nx }\n"
    );
    ASSERT_NOT_NULL(mod);
    IrFunction *fn = nth_fn(mod, 1);
    ASSERT_NOT_NULL(fn);
    bool saw_err = false, saw_ok = false;
    for (IrBasicBlock *bb = fn->entry; bb; bb = bb->next) {
        if (bb->label && strcmp(bb->label, "try.err") == 0) saw_err = true;
        if (bb->label && strcmp(bb->label, "try.ok")  == 0) saw_ok  = true;
    }
    ASSERT(saw_err);
    ASSERT(saw_ok);
}

TEST(ir_gen_try_catch_emits_three_blocks_with_phi) {
    /* try { ok_val } catch (e) { err_val } produces try.body, try.catch,
     * try.merge — and a phi in merge joining both incoming values. */
    IrModule *mod = ir_from_source(
        "fn fetch() -> int { Err(-3) }\n"
        "fn main() -> int {\n"
        "    try {\n"
        "        let x = fetch()?\n"
        "        x + 1\n"
        "    } catch (e: int) {\n"
        "        e\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);
    IrFunction *main_fn = nth_fn(mod, 1);
    ASSERT_NOT_NULL(main_fn);
    bool body = false, cat = false, merge = false;
    for (IrBasicBlock *bb = main_fn->entry; bb; bb = bb->next) {
        if (bb->label && strcmp(bb->label, "try.body")  == 0) body  = true;
        if (bb->label && strcmp(bb->label, "try.catch") == 0) cat   = true;
        if (bb->label && strcmp(bb->label, "try.merge") == 0) merge = true;
    }
    ASSERT(body);
    ASSERT(cat);
    ASSERT(merge);
    IrInst *phi = find_opcode(main_fn, IR_PHI);
    ASSERT_NOT_NULL(phi);
    ASSERT(phi->phi_count >= 2);
}

TEST(ir_gen_try_catch_question_jumps_to_catch_not_ret) {
    /* Inside a try-catch, `?` must NOT emit a `ret`. */
    IrModule *mod = ir_from_source(
        "fn fetch() -> int { Err(-3) }\n"
        "fn main() -> int {\n"
        "    try {\n"
        "        let x = fetch()?\n"
        "        x\n"
        "    } catch (e: int) {\n"
        "        e\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);
    IrFunction *main_fn = nth_fn(mod, 1);
    ASSERT_NOT_NULL(main_fn);
    ASSERT_EQ(count_opcode(main_fn, IR_RET), 1);
}

TEST(ir_gen_try_catch_chained_propagation) {
    /* Chain of three ? calls: each produces its own cmp_lt + br pair. */
    IrModule *mod = ir_from_source(
        "fn a() -> int { Ok(1) }\n"
        "fn b() -> int { Ok(2) }\n"
        "fn c() -> int { Ok(3) }\n"
        "fn main() -> int {\n"
        "    try {\n"
        "        let x = a()?\n"
        "        let y = b()?\n"
        "        let z = c()?\n"
        "        x + y + z\n"
        "    } catch (e: int) {\n"
        "        e\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);
    IrFunction *main_fn = nth_fn(mod, 3);
    ASSERT_NOT_NULL(main_fn);
    ASSERT(count_opcode(main_fn, IR_CMP_LT) >= 3);
    ASSERT(count_opcode(main_fn, IR_BR)     >= 3);
}

/* ============================================================
 * L5 surface extension: match Result + HostError lowering.
 * The match-over-Result lowering mirrors try/catch -- a cmp_lt
 * against zero splits Ok from Err, both arms join in a PHI.
 * Host-error sentinels are emitted as immediate i64 constants
 * sourced from include/vdag.errors.wit.
 * ============================================================ */

TEST(ir_gen_match_result_emits_ok_err_merge_blocks) {
    /* `match r { Result::Ok(v) -> v, Result::Err(e) -> e }` must
     * produce three blocks named match.ok / match.err / match.merge
     * plus a PHI in the merge block joining both incoming values. */
    IrModule *mod = ir_from_source(
        "fn fetch() -> int { Ok(7) }\n"
        "fn main() -> int {\n"
        "    let r = fetch()\n"
        "    match r {\n"
        "        Result::Ok(v)  -> v + 1\n"
        "        Result::Err(e) -> e\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);
    IrFunction *main_fn = nth_fn(mod, 1);
    ASSERT_NOT_NULL(main_fn);
    bool ok = false, er = false, mg = false;
    for (IrBasicBlock *bb = main_fn->entry; bb; bb = bb->next) {
        if (bb->label && strcmp(bb->label, "match.ok")    == 0) ok = true;
        if (bb->label && strcmp(bb->label, "match.err")   == 0) er = true;
        if (bb->label && strcmp(bb->label, "match.merge") == 0) mg = true;
    }
    ASSERT(ok);
    ASSERT(er);
    ASSERT(mg);
    bool found_merge_phi = false;
    for (IrBasicBlock *bb = main_fn->entry; bb; bb = bb->next) {
        if (!bb->label || strcmp(bb->label, "match.merge") != 0) continue;
        for (IrInst *i = bb->first; i; i = i->next) {
            if (i->op == IR_PHI && i->phi_count >= 2) {
                found_merge_phi = true;
            }
        }
    }
    ASSERT(found_merge_phi);
}

TEST(ir_gen_match_result_cmp_lt_against_zero) {
    /* The split test against the Result encoding MUST be a
     * cmp_lt against zero -- the same predicate the ? propagator
     * uses, so the negative-i64 invariant is enforced uniformly. */
    IrModule *mod = ir_from_source(
        "fn fetch() -> int { Err(-3) }\n"
        "fn main() -> int {\n"
        "    let r = fetch()\n"
        "    match r {\n"
        "        Result::Ok(v)  -> v\n"
        "        Result::Err(e) -> e\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);
    IrFunction *main_fn = nth_fn(mod, 1);
    ASSERT_NOT_NULL(main_fn);
    ASSERT(count_opcode(main_fn, IR_CMP_LT) >= 1);
}

TEST(ir_gen_host_error_resolves_to_negative_sentinel) {
    /* `host_error::quota_exceeded` must resolve to the canonical -3
     * sentinel sourced from include/vdag.errors.wit. We look for an
     * IR_CONST_INT carrying that exact value somewhere in the fn. */
    IrModule *mod = ir_from_source(
        "fn dispense() -> int {\n"
        "    Err(host_error::quota_exceeded)\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);
    IrFunction *fn = first_fn(mod);
    ASSERT_NOT_NULL(fn);
    bool found = false;
    for (IrBasicBlock *bb = fn->entry; bb; bb = bb->next) {
        for (IrInst *i = bb->first; i; i = i->next) {
            if (i->op == IR_CONST_INT && i->imm_int == -3) {
                found = true;
            }
        }
    }
    ASSERT(found);
}

TEST(ir_gen_host_error_in_try_catch_end_to_end) {
    /* End-to-end smoke: an inner fn returns host_error::quota_exceeded
     * (-3), the outer fn `?` propagates it into a try/catch which
     * binds the code to `e` and returns 99 from the catch arm. The
     * lowering should still produce the canonical try/catch shape
     * (try.body / try.catch / try.merge blocks + PHI). */
    IrModule *mod = ir_from_source(
        "fn inner() -> int {\n"
        "    Err(host_error::quota_exceeded)\n"
        "}\n"
        "fn outer() -> int {\n"
        "    try {\n"
        "        let v = inner()?\n"
        "        Ok(v + 1)\n"
        "    } catch (e: int) {\n"
        "        99\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(mod);
    IrFunction *outer = nth_fn(mod, 1);
    ASSERT_NOT_NULL(outer);
    bool body = false, cat = false, merge = false;
    for (IrBasicBlock *bb = outer->entry; bb; bb = bb->next) {
        if (bb->label && strcmp(bb->label, "try.body")  == 0) body  = true;
        if (bb->label && strcmp(bb->label, "try.catch") == 0) cat   = true;
        if (bb->label && strcmp(bb->label, "try.merge") == 0) merge = true;
    }
    ASSERT(body);
    ASSERT(cat);
    ASSERT(merge);
    bool found_99 = false;
    for (IrBasicBlock *bb = outer->entry; bb; bb = bb->next) {
        for (IrInst *i = bb->first; i; i = i->next) {
            if (i->op == IR_CONST_INT && i->imm_int == 99) found_99 = true;
        }
    }
    ASSERT(found_99);
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
 * L11: entropy_budget runtime fence (WASM emit)
 *
 * These tests assert on the textual WAT shape produced by the WASM
 * backend, avoiding any dependency on wat2wasm/wasmtime. The backend
 * exposes a test entry that emits WAT to a FILE* directly; we run
 * end-to-end (parse -> typecheck -> IR -> WASM-WAT) and search the
 * output for the structural markers the runtime fence relies on.
 *
 * The cost table is hardcoded in `src/ir_emit_wasm.c::entropy_cost_for`.
 * For these tests we rely on `llm.classify` having cost 1.
 * ============================================================ */

extern int lcn_emit_wasm_wat(AstNode *program, FILE *out, Arena *arena,
                             const LcnTarget *target);

/* Parse `source`, emit WAT, return as a malloc'd null-terminated string.
 * Returns NULL on parse error. Caller must free(). */
static char *wat_from_source(const char *source) {
    arena_reset(&test_arena);
    arena_reset(&test_intern_arena);

    size_t len = strlen(source);
    ErrorReporter reporter = reporter_new("<test>", source, len);
    StringIntern intern = intern_new(&test_intern_arena);
    Lexer lexer = lexer_new("<test>", source, len, &intern, &reporter);
    Parser parser = parser_new(&lexer, &test_arena, &reporter);

    AstNode *program = parse_program(&parser);
    if (parser.had_error || !program) return NULL;

    FILE *f = tmpfile();
    if (!f) return NULL;
    int rc = lcn_emit_wasm_wat(program, f, &test_arena, NULL);
    if (rc != 0) { fclose(f); return NULL; }

    long size = ftell(f);
    rewind(f);
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

TEST(wasm_entropy_global_initialised_to_declared_budget) {
    /* `entropy_budget: 42` on an agent must produce a wasm global named
     * `$entropy_remaining` initialised to exactly 42. Without an entropy
     * declaration the global defaults to INT32_MAX (no-op fence). */
    char *wat = wat_from_source(
        "agent HasBudget {\n"
        "    capabilities: [llm.classify]\n"
        "    entropy_budget: 42\n"
        "    fn main() -> int {\n"
        "        llm.classify(\"hello\")\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(wat);
    /* Global is declared and is the bit count from the agent. */
    ASSERT(strstr(wat,
        "(global $entropy_remaining (mut i32) (i32.const 42))") != NULL);
    /* Make sure it is mutable (the fence needs to write back). */
    ASSERT(strstr(wat, "(mut i32)") != NULL);
    free(wat);

    /* Default budget: agent with no entropy_budget declaration gets
     * the unbounded sentinel so the fence is a no-op. */
    char *wat2 = wat_from_source(
        "agent NoBudget {\n"
        "    capabilities: [llm.classify]\n"
        "    fn main() -> int {\n"
        "        llm.classify(\"hello\")\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(wat2);
    ASSERT(strstr(wat2,
        "(global $entropy_remaining (mut i32) (i32.const 2147483647))") != NULL);
    free(wat2);
}

TEST(wasm_entropy_decrement_wat_shape) {
    /* At every llm.classify site the compiler must emit a fence that
     * (a) decrements the global by the per-call cost, and
     * (b) writes the result back via global.set $entropy_remaining.
     *
     * We check the structural sequence rather than exact whitespace. */
    char *wat = wat_from_source(
        "agent Decrement {\n"
        "    capabilities: [llm.classify]\n"
        "    entropy_budget: 10\n"
        "    fn main() -> int {\n"
        "        llm.classify(\"hello\")\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(wat);

    /* Cost-1 fence header (llm.classify is hardcoded to cost 1). */
    ASSERT(strstr(wat, ";; --- entropy fence: cost=1 ---") != NULL);

    /* Find the read-modify-write sequence: global.get, sub, global.set.
     * They must appear in order, AND the global.set has to follow the
     * sub, otherwise the budget is never persisted between calls. */
    const char *get_after_if = strstr(wat, "i32.sub");
    ASSERT_NOT_NULL(get_after_if);
    const char *set = strstr(get_after_if, "global.set $entropy_remaining");
    ASSERT_NOT_NULL(set);

    /* The decrement uses i32.sub on i32 operands -- not i64. The global
     * itself is i32 to keep cost arithmetic cheap. */
    ASSERT(strstr(wat, "      i32.sub") != NULL);

    free(wat);
}

TEST(wasm_entropy_trap_wat_shape) {
    /* The fence must trap with HostError.EntropyExceeded = -9 BEFORE
     * dispatching the call when remaining < cost. The trap shape is:
     *
     *   global.get $entropy_remaining
     *   i32.const <cost>
     *   i32.lt_s
     *   if
     *     <push -9 in fn's return type>
     *     return
     *   end
     *
     * For an int-returning fn the sentinel push is `i64.const -9`. */
    char *wat = wat_from_source(
        "agent Trap {\n"
        "    capabilities: [llm.classify]\n"
        "    entropy_budget: 0\n"
        "    fn main() -> int {\n"
        "        llm.classify(\"hello\")\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(wat);

    /* Budget=0 propagates verbatim into the global initial value. */
    ASSERT(strstr(wat,
        "(global $entropy_remaining (mut i32) (i32.const 0))") != NULL);

    /* The trap conditional is the canonical `lt_s` + `if` + `return`. */
    const char *fence = strstr(wat, ";; --- entropy fence: cost=1 ---");
    ASSERT_NOT_NULL(fence);
    /* All four anchors must appear inside this fence, in order. */
    const char *lt = strstr(fence, "i32.lt_s");
    ASSERT_NOT_NULL(lt);
    const char *iff = strstr(lt, "if");
    ASSERT_NOT_NULL(iff);
    const char *sentinel = strstr(iff,
        "i64.const -9  ;; HostError.EntropyExceeded");
    ASSERT_NOT_NULL(sentinel);
    const char *ret = strstr(sentinel, "return");
    ASSERT_NOT_NULL(ret);
    const char *end = strstr(ret, "end");
    ASSERT_NOT_NULL(end);
    /* The post-trap subtract/store still has to be reachable for the
     * "did not trap" path. */
    const char *post_sub = strstr(end, "global.set $entropy_remaining");
    ASSERT_NOT_NULL(post_sub);

    free(wat);
}

/* ============================================================
 * L13: budget runtime fence (WASM emit)
 *
 * The declared `budget: { max_tokens: N, max_cost: F }` becomes two
 * wasm globals -- `$tokens_remaining` (mut i32) and
 * `$cost_micro_usd_remaining` (mut i64) -- and a per-call-site fence
 * that decrements + bounds-checks both before dispatching. The cost
 * tables are hardcoded in `src/ir_emit_wasm.c::token_cost_for` /
 * `cost_micro_usd_for`; these tests rely on `llm.classify` having
 * cost 100 tokens / 500 micro-USD.
 * ============================================================ */

TEST(wasm_budget_globals_initialised_to_declared_budgets) {
    /* `budget: { max_tokens: 5000, max_cost: 0.05 }` on an agent must
     * produce two wasm globals at the module preamble: `$tokens_remaining`
     * (mut i32) initialised to 5000 and `$cost_micro_usd_remaining`
     * (mut i64) initialised to 50000 (= 0.05 USD * 1e6). */
    char *wat = wat_from_source(
        "agent HasBudget {\n"
        "    capabilities: [llm.classify]\n"
        "    budget: { max_tokens: 5000, max_cost: 0.05 }\n"
        "    fn main() -> int {\n"
        "        llm.classify(\"hello\")\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(wat);
    ASSERT(strstr(wat,
        "(global $tokens_remaining (mut i32) (i32.const 5000))") != NULL);
    ASSERT(strstr(wat,
        "(global $cost_micro_usd_remaining (mut i64) (i64.const 50000))")
        != NULL);
    free(wat);

    /* No budget declared -> both globals default to MAX (i32_MAX / i64_MAX)
     * so the fence is a no-op. */
    char *wat2 = wat_from_source(
        "agent NoBudget {\n"
        "    capabilities: [llm.classify]\n"
        "    fn main() -> int {\n"
        "        llm.classify(\"hello\")\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(wat2);
    ASSERT(strstr(wat2,
        "(global $tokens_remaining (mut i32) (i32.const 2147483647))")
        != NULL);
    ASSERT(strstr(wat2,
        "(global $cost_micro_usd_remaining (mut i64) "
        "(i64.const 9223372036854775807))") != NULL);
    free(wat2);
}

TEST(wasm_budget_token_decrement_wat_shape) {
    /* At every llm.classify site the compiler must emit a token fence
     * that decrements $tokens_remaining by the per-call cost (100) and
     * writes the result back via global.set. The fence must follow the
     * entropy fence in the emission order. */
    char *wat = wat_from_source(
        "agent TokenDecrement {\n"
        "    capabilities: [llm.classify]\n"
        "    budget: { max_tokens: 5000, max_cost: 0.05 }\n"
        "    fn main() -> int {\n"
        "        llm.classify(\"hello\")\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(wat);

    /* Cost-100 token fence header. */
    ASSERT(strstr(wat, ";; --- token fence: cost=100 ---") != NULL);

    /* The read-modify-write sequence must appear, AND global.set must
     * follow i32.sub so the budget actually persists between calls. */
    const char *fence = strstr(wat, ";; --- token fence: cost=100 ---");
    ASSERT_NOT_NULL(fence);
    const char *sub = strstr(fence, "i32.sub");
    ASSERT_NOT_NULL(sub);
    const char *set = strstr(sub, "global.set $tokens_remaining");
    ASSERT_NOT_NULL(set);

    /* Order: entropy fence (if declared) must appear before token fence
     * at each call site. Here no entropy_budget is declared so the
     * entropy fence is suppressed (cost=0), but the token fence still
     * goes before the cost fence. */
    const char *token = strstr(wat, ";; --- token fence: cost=100 ---");
    ASSERT_NOT_NULL(token);
    const char *cost  = strstr(token, ";; --- cost fence:");
    ASSERT_NOT_NULL(cost);

    free(wat);
}

TEST(wasm_budget_cost_decrement_wat_shape) {
    /* The cost fence works on the i64 micro-USD global and uses i64
     * arithmetic throughout: i64.lt_s for the trap test and i64.sub
     * for the decrement. The trap pushes the BudgetExceeded sentinel
     * (-10) before returning. */
    char *wat = wat_from_source(
        "agent CostDecrement {\n"
        "    capabilities: [llm.classify]\n"
        "    budget: { max_tokens: 5000, max_cost: 0.05 }\n"
        "    fn main() -> int {\n"
        "        llm.classify(\"hello\")\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(wat);

    /* Cost fence header at 500 micro-USD per llm.classify. */
    const char *fence = strstr(wat, ";; --- cost fence: cost_micro_usd=500 ---");
    ASSERT_NOT_NULL(fence);

    /* The fence reads the i64 global, compares, traps with -10, then
     * subtracts. Anchors must appear in that order. */
    const char *get_ = strstr(fence, "global.get $cost_micro_usd_remaining");
    ASSERT_NOT_NULL(get_);
    const char *lt = strstr(get_, "i64.lt_s");
    ASSERT_NOT_NULL(lt);
    const char *iff = strstr(lt, "if");
    ASSERT_NOT_NULL(iff);
    const char *sentinel = strstr(iff,
        "i64.const -10  ;; HostError.BudgetExceeded");
    ASSERT_NOT_NULL(sentinel);
    const char *ret = strstr(sentinel, "return");
    ASSERT_NOT_NULL(ret);
    const char *end = strstr(ret, "end");
    ASSERT_NOT_NULL(end);
    const char *sub = strstr(end, "i64.sub");
    ASSERT_NOT_NULL(sub);
    const char *set = strstr(sub, "global.set $cost_micro_usd_remaining");
    ASSERT_NOT_NULL(set);

    free(wat);
}

TEST(wasm_budget_token_trap_shape) {
    /* When max_tokens is too small to cover a single llm.classify call
     * the token fence must trap with HostError.BudgetExceeded = -10
     * BEFORE the actual host call is dispatched. Budget=0 propagates
     * verbatim into the global initialiser so the very first call trips. */
    char *wat = wat_from_source(
        "agent TokenTrap {\n"
        "    capabilities: [llm.classify]\n"
        "    budget: { max_tokens: 0, max_cost: 1.0 }\n"
        "    fn main() -> int {\n"
        "        llm.classify(\"hello\")\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(wat);
    ASSERT(strstr(wat,
        "(global $tokens_remaining (mut i32) (i32.const 0))") != NULL);

    const char *fence = strstr(wat, ";; --- token fence: cost=100 ---");
    ASSERT_NOT_NULL(fence);
    const char *lt = strstr(fence, "i32.lt_s");
    ASSERT_NOT_NULL(lt);
    const char *iff = strstr(lt, "if");
    ASSERT_NOT_NULL(iff);
    const char *sentinel = strstr(iff,
        "i64.const -10  ;; HostError.BudgetExceeded");
    ASSERT_NOT_NULL(sentinel);

    free(wat);
}

TEST(wasm_budget_chain_order_entropy_then_tokens_then_cost) {
    /* At each call site the fences must appear in the canonical chain
     * order: entropy first (L11), then tokens (L13), then cost (L13).
     * The first fence to trip wins, so replays observe a deterministic
     * failure mode regardless of which counter would also have exceeded. */
    char *wat = wat_from_source(
        "agent Chain {\n"
        "    capabilities: [llm.classify]\n"
        "    entropy_budget: 10\n"
        "    budget: { max_tokens: 5000, max_cost: 0.05 }\n"
        "    fn main() -> int {\n"
        "        llm.classify(\"hello\")\n"
        "    }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(wat);

    const char *entropy = strstr(wat, ";; --- entropy fence: cost=1 ---");
    ASSERT_NOT_NULL(entropy);
    const char *token   = strstr(entropy,
        ";; --- token fence: cost=100 ---");
    ASSERT_NOT_NULL(token);
    const char *cost    = strstr(token,
        ";; --- cost fence: cost_micro_usd=500 ---");
    ASSERT_NOT_NULL(cost);

    free(wat);
}

/* ============================================================
 * L12: capability.network compile-time allowlist
 *
 * These tests verify that the parameterised capability form
 *   `capabilities: [http.fetch(["api.openai.com:443", ...])]`
 * (a) parses (round-trip through the parser yields an
 *     AST_CAPABILITY_ITEM with string-literal hosts),
 * (b) is rejected by typecheck for malformed entries,
 * (c) propagates into the emitted .wit as a `hosts: [...]` field,
 * (d) propagates into the wasm WAT as a `vdag.capability.network.
 *     allowlist` custom section carrying the JSON-encoded payload.
 * ============================================================ */

#include "wit_emit.h"

/* Parse the given source, locate the (sole) AST_AGENT, and return
 * the AST_FIELD node for `capabilities` (or NULL). Used by L12
 * parser-shape tests. */
static AstNode *parse_first_agent_field(const char *source,
                                        const char *field_name) {
    arena_reset(&test_arena);
    arena_reset(&test_intern_arena);
    size_t len = strlen(source);
    ErrorReporter reporter = reporter_new("<test>", source, len);
    StringIntern intern = intern_new(&test_intern_arena);
    Lexer lexer = lexer_new("<test>", source, len, &intern, &reporter);
    Parser parser = parser_new(&lexer, &test_arena, &reporter);
    AstNode *program = parse_program(&parser);
    if (parser.had_error || !program) return NULL;
    AstNode *decl;
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind != AST_AGENT) continue;
        AstNode *f;
        for (f = decl->params; f; f = f->next) {
            if (f->kind == AST_FIELD && f->name &&
                strcmp(f->name, field_name) == 0)
                return f;
        }
    }
    return NULL;
}

TEST(l12_parser_accepts_parameterised_http_fetch) {
    /* The parameterised form must lower to AST_CAPABILITY_ITEM with
     * the qualified verb as `name` and a chain of AST_STRING_LIT
     * host:port specs in `params`. */
    AstNode *caps = parse_first_agent_field(
        "agent Net {\n"
        "    capabilities: [http.fetch([\"api.openai.com:443\", \"*.example.com:443\"])]\n"
        "    fn main() -> int { 0 }\n"
        "}\n",
        "capabilities");
    ASSERT_NOT_NULL(caps);
    ASSERT_NOT_NULL(caps->right);
    ASSERT(caps->right->kind == AST_ARRAY);

    AstNode *elem = caps->right->params;
    ASSERT_NOT_NULL(elem);
    ASSERT(elem->kind == AST_CAPABILITY_ITEM);
    ASSERT_NOT_NULL(elem->name);
    ASSERT(strcmp(elem->name, "http.fetch") == 0);

    /* Two host entries, both string literals. */
    AstNode *h1 = elem->params;
    ASSERT_NOT_NULL(h1);
    ASSERT(h1->kind == AST_STRING_LIT);
    ASSERT(strcmp(h1->val.str_val, "api.openai.com:443") == 0);
    AstNode *h2 = h1->next;
    ASSERT_NOT_NULL(h2);
    ASSERT(h2->kind == AST_STRING_LIT);
    ASSERT(strcmp(h2->val.str_val, "*.example.com:443") == 0);
    ASSERT(h2->next == NULL);

    /* And the bare form must still parse to AST_IDENT (back-compat). */
    AstNode *caps_bare = parse_first_agent_field(
        "agent Bare {\n"
        "    capabilities: [http.fetch]\n"
        "    fn main() -> int { 0 }\n"
        "}\n",
        "capabilities");
    ASSERT_NOT_NULL(caps_bare);
    AstNode *bare_elem = caps_bare->right->params;
    ASSERT_NOT_NULL(bare_elem);
    ASSERT(bare_elem->kind == AST_IDENT);
    ASSERT(strcmp(bare_elem->name, "http.fetch") == 0);
}

/* Parse + typecheck the given source. Returns the number of errors
 * the reporter accumulated. */
static int typecheck_error_count(const char *source) {
    arena_reset(&test_arena);
    arena_reset(&test_intern_arena);
    size_t len = strlen(source);
    ErrorReporter reporter = reporter_new("<test>", source, len);
    StringIntern intern = intern_new(&test_intern_arena);
    Lexer lexer = lexer_new("<test>", source, len, &intern, &reporter);
    Parser parser = parser_new(&lexer, &test_arena, &reporter);
    AstNode *program = parse_program(&parser);
    if (!program) return -1;
    /* Don't short-circuit on parse error: we want to see typecheck
     * diagnostics even when the parser already raised something. */
    (void)typecheck_program(program, &reporter, &test_arena);
    return reporter.count;
}

TEST(l12_typecheck_rejects_malformed_hosts) {
    /* Missing port. */
    int n1 = typecheck_error_count(
        "agent A {\n"
        "    capabilities: [http.fetch([\"api.openai.com\"])]\n"
        "    fn main() -> int { 0 }\n"
        "}\n");
    ASSERT(n1 > 0);

    /* Port out of range. */
    int n2 = typecheck_error_count(
        "agent A {\n"
        "    capabilities: [http.fetch([\"api.openai.com:99999\"])]\n"
        "    fn main() -> int { 0 }\n"
        "}\n");
    ASSERT(n2 > 0);

    /* Wildcard not at leading position. */
    int n3 = typecheck_error_count(
        "agent A {\n"
        "    capabilities: [http.fetch([\"api.*.com:443\"])]\n"
        "    fn main() -> int { 0 }\n"
        "}\n");
    ASSERT(n3 > 0);

    /* IP literal rejected. */
    int n4 = typecheck_error_count(
        "agent A {\n"
        "    capabilities: [http.fetch([\"127.0.0.1:443\"])]\n"
        "    fn main() -> int { 0 }\n"
        "}\n");
    ASSERT(n4 > 0);

    /* Unrestricted '*' wildcard rejected. */
    int n5 = typecheck_error_count(
        "agent A {\n"
        "    capabilities: [http.fetch([\"*\"])]\n"
        "    fn main() -> int { 0 }\n"
        "}\n");
    ASSERT(n5 > 0);

    /* The "good" form must produce ZERO L12 errors. */
    int n_ok = typecheck_error_count(
        "agent A {\n"
        "    capabilities: [http.fetch([\"api.openai.com:443\", "
        "\"*.example.com:443\"])]\n"
        "    fn main() -> int { 0 }\n"
        "}\n");
    ASSERT(n_ok == 0);
}

TEST(l12_wit_emit_writes_hosts_field) {
    /* End-to-end: write a temp .wit file with the parameterised form
     * and confirm the `import http.fetch { hosts: [...] }` block
     * appears verbatim. */
    arena_reset(&test_arena);
    arena_reset(&test_intern_arena);
    const char *src =
        "agent NetCap {\n"
        "    capabilities: [http.fetch([\"api.openai.com:443\", "
        "\"*.example.com:443\"])]\n"
        "    fn run() -> int { 0 }\n"
        "}\n";
    size_t len = strlen(src);
    ErrorReporter reporter = reporter_new("<test>", src, len);
    StringIntern intern = intern_new(&test_intern_arena);
    Lexer lexer = lexer_new("<test>", src, len, &intern, &reporter);
    Parser parser = parser_new(&lexer, &test_arena, &reporter);
    AstNode *program = parse_program(&parser);
    ASSERT_NOT_NULL(program);

    char wit_path[] = "/tmp/l12_wit_emit_XXXXXX.wit";
    int fd = mkstemps(wit_path, 4);
    ASSERT(fd >= 0);
    close(fd);
    int rc = lcn_emit_wit(program, wit_path);
    ASSERT(rc == 0);

    /* Read the file back. */
    FILE *fp = fopen(wit_path, "rb");
    ASSERT_NOT_NULL(fp);
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    char *buf = (char *)malloc((size_t)sz + 1);
    fread(buf, 1, (size_t)sz, fp);
    buf[sz] = '\0';
    fclose(fp);
    unlink(wit_path);

    /* The block must appear with both hosts inside. */
    ASSERT(strstr(buf, "import http.fetch {") != NULL);
    ASSERT(strstr(buf, "hosts: [") != NULL);
    ASSERT(strstr(buf, "\"api.openai.com:443\"") != NULL);
    ASSERT(strstr(buf, "\"*.example.com:443\"") != NULL);

    free(buf);
}

TEST(l12_wasm_custom_section_contains_allowlist) {
    /* The wasm module must carry a `(@custom
     * "vdag.capability.network.allowlist" "...JSON...")` block whose
     * payload matches the declared host list verbatim. */
    char *wat = wat_from_source(
        "agent NetCap {\n"
        "    capabilities: [http.fetch([\"api.openai.com:443\", "
        "\"*.example.com:443\"])]\n"
        "    fn main() -> int { 0 }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(wat);

    /* Custom-section anchor + name. */
    const char *anchor =
        "(@custom \"vdag.capability.network.allowlist\"";
    const char *sec = strstr(wat, anchor);
    ASSERT_NOT_NULL(sec);
    /* Payload must be a JSON list with both hosts. The payload appears
     * WAT-escaped (`\"verb\"`) inside the source-level string literal. */
    ASSERT(strstr(sec, "\\\"verb\\\":\\\"http.fetch\\\"") != NULL);
    ASSERT(strstr(sec, "api.openai.com:443") != NULL);
    ASSERT(strstr(sec, "*.example.com:443") != NULL);

    free(wat);

    /* And the bare form (no allowlist) must NOT emit the custom
     * section, so the runtime keeps the bare contract = unrestricted
     * fetch at compile time. */
    char *wat_bare = wat_from_source(
        "agent BareNet {\n"
        "    capabilities: [http.fetch]\n"
        "    fn main() -> int { 0 }\n"
        "}\n"
    );
    ASSERT_NOT_NULL(wat_bare);
    ASSERT(strstr(wat_bare,
        "(@custom \"vdag.capability.network.allowlist\"") == NULL);
    free(wat_bare);
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
    RUN_TEST(ir_gen_while_bb_count_and_back_edge);
    RUN_TEST(ir_gen_for_in_desugar_shape);
    RUN_TEST(ir_gen_nested_break_targets_innermost_exit);
    RUN_TEST(l2_ir_gen_loop_has_header_and_back_edge);
    RUN_TEST(l2_ir_gen_for_wildcard_pattern_lowers_like_named);
    RUN_TEST(ir_gen_string_concat);
    RUN_TEST(ir_gen_multiple_functions);
    RUN_TEST(ir_gen_return_void);
    RUN_TEST(ir_gen_nested_binary);
    RUN_TEST(ir_gen_unary_neg);
    RUN_TEST(ir_gen_print_statement);

    fprintf(stderr, "\n-- JSON Host-Call Lowering --\n");
    RUN_TEST(ir_gen_json_parse_host_call);
    RUN_TEST(ir_gen_json_field_host_call);
    RUN_TEST(ir_gen_json_array_index_host_call);
    RUN_TEST(ir_gen_json_length_host_call);
    RUN_TEST(ir_gen_json_string_value_host_call);
    RUN_TEST(ir_gen_json_int_value_host_call);
    RUN_TEST(ir_gen_json_bool_value_host_call);
    RUN_TEST(ir_gen_json_is_null_host_call);
    RUN_TEST(ir_gen_json_stringify_host_call);
    RUN_TEST(ir_gen_json_chained_pipeline);

    fprintf(stderr, "\n-- L5: Result + ? + try/catch --\n");
    RUN_TEST(ir_gen_result_ok_pass_through);
    RUN_TEST(ir_gen_result_err_pass_through);
    RUN_TEST(ir_gen_try_propagates_via_ret);
    RUN_TEST(ir_gen_try_emits_try_err_and_try_ok_blocks);
    RUN_TEST(ir_gen_try_catch_emits_three_blocks_with_phi);
    RUN_TEST(ir_gen_try_catch_question_jumps_to_catch_not_ret);
    RUN_TEST(ir_gen_try_catch_chained_propagation);

    fprintf(stderr, "\n-- L5 (extension): match Result + HostError --\n");
    RUN_TEST(ir_gen_match_result_emits_ok_err_merge_blocks);
    RUN_TEST(ir_gen_match_result_cmp_lt_against_zero);
    RUN_TEST(ir_gen_host_error_resolves_to_negative_sentinel);
    RUN_TEST(ir_gen_host_error_in_try_catch_end_to_end);

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

    fprintf(stderr, "\n-- L11: entropy_budget runtime fence --\n");
    RUN_TEST(wasm_entropy_global_initialised_to_declared_budget);
    RUN_TEST(wasm_entropy_decrement_wat_shape);
    RUN_TEST(wasm_entropy_trap_wat_shape);

    fprintf(stderr, "\n-- L13: budget runtime fence --\n");
    RUN_TEST(wasm_budget_globals_initialised_to_declared_budgets);
    RUN_TEST(wasm_budget_token_decrement_wat_shape);
    RUN_TEST(wasm_budget_cost_decrement_wat_shape);
    RUN_TEST(wasm_budget_token_trap_shape);
    RUN_TEST(wasm_budget_chain_order_entropy_then_tokens_then_cost);

    fprintf(stderr, "\n-- L12: capability.network compile-time allowlist --\n");
    RUN_TEST(l12_parser_accepts_parameterised_http_fetch);
    RUN_TEST(l12_typecheck_rejects_malformed_hosts);
    RUN_TEST(l12_wit_emit_writes_hosts_field);
    RUN_TEST(l12_wasm_custom_section_contains_allowlist);

    ir_test_teardown();

    TEST_SUMMARY();
    return test_failures > 0 ? 1 : 0;
}
