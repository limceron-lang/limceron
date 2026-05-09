/*
 * Limceron Compiler — WebAssembly (wasm32-wasip2) Backend (STUB)
 *
 * STAGE: PoC scaffold. This file is the entry point handed to agent A
 * who will implement the full SSA IR -> WAT translator (15 opcodes minimum:
 * CONST_INT/STRING, ADD/SUB/MUL, CMP_EQ/LT, BR/BR_IF, CALL, RET,
 * LOAD/STORE/ALLOCA, PHI). See docs/POC-WASM.md for the spec.
 *
 * This stub exists so the rest of the build flow links cleanly while the
 * real emitter is being authored. Calling it today emits a minimal valid
 * WAT module (single export `main` that returns 0) and runs wat2wasm to
 * produce a real .wasm — that proves the pipeline shape works end-to-end
 * before agent A fills in the per-opcode emit logic.
 */

#include "lcn.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Default to a placeholder WAT module so the toolchain pipeline can be
 * exercised end-to-end (parser→typecheck→emit→wat2wasm) before the real
 * opcode emitter lands. Once agent A implements the SSA walk, replace
 * emit_placeholder_wat with the real translator entry point. */
static int emit_placeholder_wat(FILE *out) {
    /* Minimal valid WASI command module: exports `_start` and `main`,
     * imports nothing, returns exit code 0. */
    fputs(
        ";; Limceron WASM placeholder — agent A replaces with SSA IR walk.\n"
        "(module\n"
        "  (func $main (result i32)\n"
        "    i32.const 0\n"
        "    return)\n"
        "  (export \"main\" (func $main))\n"
        "  (memory 1)\n"
        "  (export \"memory\" (memory 0)))\n",
        out);
    return 0;
}

/* Run wat2wasm to convert the .wat to a real .wasm. Returns 0 on success. */
static int run_wat2wasm(const char *wat_path, const char *wasm_path) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "wat2wasm --debug-names -o %s %s 2>&1",
             wasm_path, wat_path);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "  WASM emit: wat2wasm failed (exit %d)\n", rc);
        fprintf(stderr, "  Hint: brew install wabt   (or: cargo install wasm-tools)\n");
        return 1;
    }
    return 0;
}

/* Public entry called by main.c when the active target is wasm32-wasi-preview2. */
int lcn_emit_wasm(AstNode *program, const char *input,
                  const char *output, Arena *arena,
                  const LcnTarget *target) {
    (void)program;
    (void)arena;
    (void)target;

    if (!input || !output) {
        fprintf(stderr, "error: lcn_emit_wasm called with null input/output\n");
        return 1;
    }

    /* Write .wat to a temp file. */
    char wat_path[512];
    snprintf(wat_path, sizeof(wat_path), "/tmp/lcn_%d.wat", (int)getpid());
    FILE *out = fopen(wat_path, "w");
    if (!out) {
        fprintf(stderr, "error: cannot write %s\n", wat_path);
        return 1;
    }
    int rc = emit_placeholder_wat(out);
    fclose(out);
    if (rc != 0) return rc;

    fprintf(stderr, "  WASM emit (placeholder): %s\n", wat_path);

    /* Run wat2wasm to produce final binary. */
    rc = run_wat2wasm(wat_path, output);
    if (rc != 0) return rc;

    /* Report final size. */
    {
        FILE *fp = fopen(output, "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long sz = ftell(fp);
            fclose(fp);
            fprintf(stderr, "  WASM emit: %s (%ld bytes)\n", output, sz);
        }
    }

    /* Cleanup temp WAT (optional — keep for inspection). */
    /* unlink(wat_path); */

    return 0;
}
