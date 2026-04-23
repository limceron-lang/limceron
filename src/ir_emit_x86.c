/*
 * Limceron Compiler — x86_64 Assembly Emitter
 *
 * Emits AT&T syntax x86_64 assembly from the SSA IR.
 * Target: System V AMD64 ABI (Linux/macOS).
 *
 * Calling convention:
 *   Args:       %rdi, %rsi, %rdx, %rcx, %r8, %r9 (first 6 integer args)
 *   Return:     %rax
 *   Caller-saved: %rax, %rcx, %rdx, %rsi, %rdi, %r8-%r11
 *   Callee-saved: %rbx, %r12-%r15, %rbp
 *   Stack:      16-byte aligned before CALL
 */

#include "lcn.h"
#include "ir.h"
#include <string.h>
#include <stdlib.h>

/* ============================================================
 * Internal Context
 * ============================================================ */

/* Maximum SSA values per function for register mapping */
#define X86_MAX_VALUES 4096

/* String literal table for .rodata section */
#define X86_MAX_STRINGS 512

typedef struct {
    const char *value;
    int         label_id;
} X86StringLit;

typedef struct {
    IrModule      *mod;
    IrFunction    *fn;
    FILE          *out;

    /* Register allocation mapping: value_id -> RegAlloc */
    RegAlloc       allocs[X86_MAX_VALUES];
    int            alloc_count;

    /* Stack frame info */
    int            frame_size;      /* Total stack frame size (bytes) */
    int            alloca_count;    /* Number of alloca slots */
    int            alloca_offsets[X86_MAX_VALUES];  /* alloca value_id -> rbp offset */

    /* String literal collection */
    X86StringLit   strings[X86_MAX_STRINGS];
    int            string_count;

    /* Label counter for unique labels */
    int            next_label;

    /* Whether any callee-saved registers are used */
    bool           uses_rbx;
    bool           uses_r12;
    bool           uses_r13;
    bool           uses_r14;
    bool           uses_r15;
} X86Context;

/* ============================================================
 * Helpers
 * ============================================================ */

/* System V AMD64 ABI: first 6 integer argument registers */
static const X86Reg abi_arg_regs[] = {
    REG_RDI, REG_RSI, REG_RDX, REG_RCX, REG_R8, REG_R9
};

/* Look up the register allocation for a given SSA value */
static RegAlloc *x86_find_alloc(X86Context *ctx, int value_id) {
    int i;
    for (i = 0; i < ctx->alloc_count; i++) {
        if (ctx->allocs[i].value_id == value_id)
            return &ctx->allocs[i];
    }
    return NULL;
}

/* Find an instruction by SSA value ID */
static IrInst *x86_find_inst(IrFunction *fn, int id) {
    IrBasicBlock *bb;
    for (bb = fn->entry; bb; bb = bb->next) {
        IrInst *inst;
        for (inst = bb->first; inst; inst = inst->next) {
            if (inst->id == id) return inst;
        }
    }
    return NULL;
}

/* Emit: move a value into a specific register */
static void x86_emit_move_to_reg(X86Context *ctx, int value_id, X86Reg target) {
    if (value_id < 0) {
        fprintf(ctx->out, "    movq $0, %s\n", x86_reg_name(target));
        return;
    }

    RegAlloc *ra = x86_find_alloc(ctx, value_id);
    if (ra && ra->reg == target) return;  /* Already in target register */

    if (ra && ra->reg != REG_NONE && ra->reg != REG_SPILL) {
        fprintf(ctx->out, "    movq %s, %s\n", x86_reg_name(ra->reg), x86_reg_name(target));
    } else if (ra && ra->reg == REG_SPILL) {
        fprintf(ctx->out, "    movq %d(%%rbp), %s\n", ra->spill_offset, x86_reg_name(target));
    } else if (value_id < X86_MAX_VALUES && ctx->alloca_offsets[value_id] != 0) {
        /* Load the address of the alloca slot (LEA) */
        fprintf(ctx->out, "    leaq %d(%%rbp), %s\n",
                ctx->alloca_offsets[value_id], x86_reg_name(target));
    } else {
        fprintf(ctx->out, "    movq $0, %s\n", x86_reg_name(target));
    }
}

/* Emit: store a register value into the destination for a SSA value */
static void x86_emit_store_from_reg(X86Context *ctx, int value_id, X86Reg src) {
    RegAlloc *ra = x86_find_alloc(ctx, value_id);
    if (!ra) return;

    if (ra->reg != REG_NONE && ra->reg != REG_SPILL) {
        if (ra->reg != src) {
            fprintf(ctx->out, "    movq %s, %s\n", x86_reg_name(src), x86_reg_name(ra->reg));
        }
    } else if (ra->reg == REG_SPILL) {
        fprintf(ctx->out, "    movq %s, %d(%%rbp)\n", x86_reg_name(src), ra->spill_offset);
    }
}

/* Register a string literal and return its label ID */
static int x86_add_string(X86Context *ctx, const char *str) {
    /* Check if already registered */
    int i;
    for (i = 0; i < ctx->string_count; i++) {
        if (ctx->strings[i].value && str && strcmp(ctx->strings[i].value, str) == 0)
            return ctx->strings[i].label_id;
    }
    if (ctx->string_count >= X86_MAX_STRINGS) return 0;

    int label = ctx->next_label++;
    ctx->strings[ctx->string_count].value = str;
    ctx->strings[ctx->string_count].label_id = label;
    ctx->string_count++;
    return label;
}

/* ============================================================
 * Compute Frame Layout
 * ============================================================ */

static void x86_compute_frame(X86Context *ctx) {
    /* Walk all instructions to find allocas and compute stack layout */
    ctx->alloca_count = 0;
    memset(ctx->alloca_offsets, 0, sizeof(ctx->alloca_offsets));

    int stack_offset = 0;  /* Offset from RBP (negative) */

    IrBasicBlock *bb;
    IrInst *inst;
    for (bb = ctx->fn->entry; bb; bb = bb->next) {
        for (inst = bb->first; inst; inst = inst->next) {
            if (inst->op == IR_ALLOCA && inst->id >= 0 && inst->id < X86_MAX_VALUES) {
                stack_offset -= 8;
                ctx->alloca_offsets[inst->id] = stack_offset;
                ctx->alloca_count++;
            }
        }
    }

    /* Account for spilled registers */
    {
        int i;
        for (i = 0; i < ctx->alloc_count; i++) {
            if (ctx->allocs[i].reg == REG_SPILL) {
                /* Spill offsets are already negative from the allocator,
                 * but we need them below the alloca area. */
                ctx->allocs[i].spill_offset = stack_offset + ctx->allocs[i].spill_offset;
                stack_offset -= 8;
            }
        }
    }

    /* Check callee-saved register usage */
    ctx->uses_rbx = false;
    ctx->uses_r12 = false;
    ctx->uses_r13 = false;
    ctx->uses_r14 = false;
    ctx->uses_r15 = false;
    {
        int i;
        for (i = 0; i < ctx->alloc_count; i++) {
            switch (ctx->allocs[i].reg) {
            case REG_RBX: ctx->uses_rbx = true; break;
            case REG_R12: ctx->uses_r12 = true; break;
            case REG_R13: ctx->uses_r13 = true; break;
            case REG_R14: ctx->uses_r14 = true; break;
            case REG_R15: ctx->uses_r15 = true; break;
            default: break;
            }
        }
    }

    /* Count callee-saved pushes */
    int callee_saved_pushes = 0;
    if (ctx->uses_rbx) callee_saved_pushes++;
    if (ctx->uses_r12) callee_saved_pushes++;
    if (ctx->uses_r13) callee_saved_pushes++;
    if (ctx->uses_r14) callee_saved_pushes++;
    if (ctx->uses_r15) callee_saved_pushes++;

    /* Frame size: stack vars + spills, aligned to 16 bytes.
     * After pushq %rbp (and callee-saved pushes), RSP must be 16-byte aligned
     * before any CALL instruction. */
    int raw_size = -stack_offset;  /* Convert negative offset to positive size */
    if (raw_size < 0) raw_size = 0;

    /* +1 for pushq %rbp itself, plus callee-saved pushes.
     * Total pushes = 1 (rbp) + callee_saved_pushes.
     * If total pushes is odd, we need raw_size to be 8 mod 16,
     * if even, we need raw_size to be 0 mod 16. */
    int total_pushes = 1 + callee_saved_pushes;
    if (total_pushes % 2 == 0) {
        /* Even pushes: need frame to be 0 mod 16 */
        ctx->frame_size = (raw_size + 15) & ~15;
    } else {
        /* Odd pushes: need frame to be 8 mod 16, or 0 mod 16 if already aligned */
        if (raw_size == 0) {
            ctx->frame_size = 0;
        } else {
            ctx->frame_size = ((raw_size + 15) & ~15);
            /* Ensure 16-byte alignment at CALL:
             * (total_pushes * 8 + frame_size) must be 0 mod 16 */
            if ((total_pushes * 8 + ctx->frame_size) % 16 != 0)
                ctx->frame_size += 8;
        }
    }
    if (ctx->frame_size == 0 && raw_size > 0)
        ctx->frame_size = 16;
}

/* ============================================================
 * Instruction Emission
 * ============================================================ */

static void x86_emit_inst(X86Context *ctx, IrInst *inst) {
    if (!inst) return;

    switch (inst->op) {

    case IR_CONST_INT: {
        RegAlloc *ra = x86_find_alloc(ctx, inst->id);
        if (!ra) break;
        if (ra->reg != REG_NONE && ra->reg != REG_SPILL) {
            fprintf(ctx->out, "    movq $%lld, %s\n",
                    (long long)inst->imm_int, x86_reg_name(ra->reg));
        } else if (ra->reg == REG_SPILL) {
            fprintf(ctx->out, "    movq $%lld, %d(%%rbp)\n",
                    (long long)inst->imm_int, ra->spill_offset);
        }
        break;
    }

    case IR_CONST_BOOL: {
        RegAlloc *ra = x86_find_alloc(ctx, inst->id);
        if (!ra) break;
        if (ra->reg != REG_NONE && ra->reg != REG_SPILL) {
            fprintf(ctx->out, "    movq $%lld, %s\n",
                    (long long)inst->imm_int, x86_reg_name(ra->reg));
        } else if (ra->reg == REG_SPILL) {
            fprintf(ctx->out, "    movq $%lld, %d(%%rbp)\n",
                    (long long)inst->imm_int, ra->spill_offset);
        }
        break;
    }

    case IR_CONST_STRING: {
        int label = x86_add_string(ctx, inst->imm_str ? inst->imm_str : "");
        RegAlloc *ra = x86_find_alloc(ctx, inst->id);
        if (!ra) break;
        if (ra->reg != REG_NONE && ra->reg != REG_SPILL) {
            fprintf(ctx->out, "    leaq .Lstr_%d(%%rip), %s\n",
                    label, x86_reg_name(ra->reg));
        } else if (ra->reg == REG_SPILL) {
            fprintf(ctx->out, "    leaq .Lstr_%d(%%rip), %%rax\n", label);
            fprintf(ctx->out, "    movq %%rax, %d(%%rbp)\n", ra->spill_offset);
        }
        break;
    }

    case IR_CONST_FLOAT: {
        /* Float constants: store to .rodata and load via movsd.
         * For simplicity in integer-only mode, just load the bit pattern. */
        RegAlloc *ra = x86_find_alloc(ctx, inst->id);
        if (!ra) break;
        /* Treat float as integer bit pattern for now (simplified) */
        union { double d; int64_t i; } u;
        u.d = inst->imm_float;
        if (ra->reg != REG_NONE && ra->reg != REG_SPILL) {
            fprintf(ctx->out, "    movabsq $%lld, %s  # float %g\n",
                    (long long)u.i, x86_reg_name(ra->reg), inst->imm_float);
        } else if (ra->reg == REG_SPILL) {
            fprintf(ctx->out, "    movabsq $%lld, %%rax  # float %g\n",
                    (long long)u.i, inst->imm_float);
            fprintf(ctx->out, "    movq %%rax, %d(%%rbp)\n", ra->spill_offset);
        }
        break;
    }

    case IR_ADD: {
        /* dst = lhs + rhs: movq lhs, dst; addq rhs, dst */
        RegAlloc *ra = x86_find_alloc(ctx, inst->id);
        if (!ra) break;
        X86Reg dst = (ra->reg != REG_NONE && ra->reg != REG_SPILL) ? ra->reg : REG_RAX;

        x86_emit_move_to_reg(ctx, inst->operands[0], dst);
        /* Load rhs to a scratch register if needed */
        RegAlloc *rhs_ra = x86_find_alloc(ctx, inst->operands[1]);
        if (rhs_ra && rhs_ra->reg != REG_NONE && rhs_ra->reg != REG_SPILL) {
            fprintf(ctx->out, "    addq %s, %s\n",
                    x86_reg_name(rhs_ra->reg), x86_reg_name(dst));
        } else {
            X86Reg scratch = (dst == REG_R11) ? REG_R10 : REG_R11;
            x86_emit_move_to_reg(ctx, inst->operands[1], scratch);
            fprintf(ctx->out, "    addq %s, %s\n",
                    x86_reg_name(scratch), x86_reg_name(dst));
        }
        if (dst == REG_RAX && ra->reg == REG_SPILL) {
            x86_emit_store_from_reg(ctx, inst->id, REG_RAX);
        }
        break;
    }

    case IR_SUB: {
        RegAlloc *ra = x86_find_alloc(ctx, inst->id);
        if (!ra) break;
        X86Reg dst = (ra->reg != REG_NONE && ra->reg != REG_SPILL) ? ra->reg : REG_RAX;

        x86_emit_move_to_reg(ctx, inst->operands[0], dst);
        RegAlloc *rhs_ra = x86_find_alloc(ctx, inst->operands[1]);
        if (rhs_ra && rhs_ra->reg != REG_NONE && rhs_ra->reg != REG_SPILL) {
            fprintf(ctx->out, "    subq %s, %s\n",
                    x86_reg_name(rhs_ra->reg), x86_reg_name(dst));
        } else {
            X86Reg scratch = (dst == REG_R11) ? REG_R10 : REG_R11;
            x86_emit_move_to_reg(ctx, inst->operands[1], scratch);
            fprintf(ctx->out, "    subq %s, %s\n",
                    x86_reg_name(scratch), x86_reg_name(dst));
        }
        if (dst == REG_RAX && ra->reg == REG_SPILL) {
            x86_emit_store_from_reg(ctx, inst->id, REG_RAX);
        }
        break;
    }

    case IR_MUL: {
        RegAlloc *ra = x86_find_alloc(ctx, inst->id);
        if (!ra) break;
        X86Reg dst = (ra->reg != REG_NONE && ra->reg != REG_SPILL) ? ra->reg : REG_RAX;

        x86_emit_move_to_reg(ctx, inst->operands[0], dst);
        RegAlloc *rhs_ra = x86_find_alloc(ctx, inst->operands[1]);
        if (rhs_ra && rhs_ra->reg != REG_NONE && rhs_ra->reg != REG_SPILL) {
            fprintf(ctx->out, "    imulq %s, %s\n",
                    x86_reg_name(rhs_ra->reg), x86_reg_name(dst));
        } else {
            X86Reg scratch = (dst == REG_R11) ? REG_R10 : REG_R11;
            x86_emit_move_to_reg(ctx, inst->operands[1], scratch);
            fprintf(ctx->out, "    imulq %s, %s\n",
                    x86_reg_name(scratch), x86_reg_name(dst));
        }
        if (dst == REG_RAX && ra->reg == REG_SPILL) {
            x86_emit_store_from_reg(ctx, inst->id, REG_RAX);
        }
        break;
    }

    case IR_DIV:
    case IR_MOD: {
        /* idivq uses RAX:RDX as dividend, divisor in any other register.
         * Quotient -> RAX, remainder -> RDX. */
        RegAlloc *ra = x86_find_alloc(ctx, inst->id);
        if (!ra) break;

        x86_emit_move_to_reg(ctx, inst->operands[0], REG_RAX);
        fprintf(ctx->out, "    cqto\n");  /* Sign-extend RAX -> RDX:RAX */

        /* Load divisor to a register that is not RAX or RDX */
        RegAlloc *rhs_ra = x86_find_alloc(ctx, inst->operands[1]);
        X86Reg div_reg = REG_R11;
        if (rhs_ra && rhs_ra->reg != REG_NONE && rhs_ra->reg != REG_SPILL
            && rhs_ra->reg != REG_RAX && rhs_ra->reg != REG_RDX) {
            div_reg = rhs_ra->reg;
        } else {
            x86_emit_move_to_reg(ctx, inst->operands[1], REG_R11);
        }
        fprintf(ctx->out, "    idivq %s\n", x86_reg_name(div_reg));

        /* Result: quotient in RAX for DIV, remainder in RDX for MOD */
        X86Reg result_reg = (inst->op == IR_DIV) ? REG_RAX : REG_RDX;
        x86_emit_store_from_reg(ctx, inst->id, result_reg);
        break;
    }

    case IR_NEG: {
        RegAlloc *ra = x86_find_alloc(ctx, inst->id);
        if (!ra) break;
        X86Reg dst = (ra->reg != REG_NONE && ra->reg != REG_SPILL) ? ra->reg : REG_RAX;

        x86_emit_move_to_reg(ctx, inst->operands[0], dst);
        fprintf(ctx->out, "    negq %s\n", x86_reg_name(dst));
        if (dst == REG_RAX && ra->reg == REG_SPILL) {
            x86_emit_store_from_reg(ctx, inst->id, REG_RAX);
        }
        break;
    }

    case IR_NOT: {
        RegAlloc *ra = x86_find_alloc(ctx, inst->id);
        if (!ra) break;
        X86Reg dst = (ra->reg != REG_NONE && ra->reg != REG_SPILL) ? ra->reg : REG_RAX;

        x86_emit_move_to_reg(ctx, inst->operands[0], dst);
        fprintf(ctx->out, "    xorq $1, %s\n", x86_reg_name(dst));
        if (dst == REG_RAX && ra->reg == REG_SPILL) {
            x86_emit_store_from_reg(ctx, inst->id, REG_RAX);
        }
        break;
    }

    case IR_CMP_EQ: case IR_CMP_NE: case IR_CMP_LT:
    case IR_CMP_GT: case IR_CMP_LE: case IR_CMP_GE: {
        RegAlloc *ra = x86_find_alloc(ctx, inst->id);
        if (!ra) break;

        /* cmpq %b, %a  (AT&T: cmpq src, dst compares dst - src) */
        x86_emit_move_to_reg(ctx, inst->operands[0], REG_RAX);
        x86_emit_move_to_reg(ctx, inst->operands[1], REG_R11);
        fprintf(ctx->out, "    cmpq %%r11, %%rax\n");

        /* Set condition byte */
        const char *setcc;
        switch (inst->op) {
        case IR_CMP_EQ: setcc = "sete";  break;
        case IR_CMP_NE: setcc = "setne"; break;
        case IR_CMP_LT: setcc = "setl";  break;
        case IR_CMP_GT: setcc = "setg";  break;
        case IR_CMP_LE: setcc = "setle"; break;
        case IR_CMP_GE: setcc = "setge"; break;
        default:        setcc = "sete";  break;
        }
        fprintf(ctx->out, "    %s %%al\n", setcc);
        fprintf(ctx->out, "    movzbq %%al, %%rax\n");

        x86_emit_store_from_reg(ctx, inst->id, REG_RAX);
        break;
    }

    case IR_AND: {
        RegAlloc *ra = x86_find_alloc(ctx, inst->id);
        if (!ra) break;
        X86Reg dst = (ra->reg != REG_NONE && ra->reg != REG_SPILL) ? ra->reg : REG_RAX;

        x86_emit_move_to_reg(ctx, inst->operands[0], dst);
        x86_emit_move_to_reg(ctx, inst->operands[1], REG_R11);
        fprintf(ctx->out, "    andq %%r11, %s\n", x86_reg_name(dst));
        if (dst == REG_RAX && ra->reg == REG_SPILL) {
            x86_emit_store_from_reg(ctx, inst->id, REG_RAX);
        }
        break;
    }

    case IR_OR: {
        RegAlloc *ra = x86_find_alloc(ctx, inst->id);
        if (!ra) break;
        X86Reg dst = (ra->reg != REG_NONE && ra->reg != REG_SPILL) ? ra->reg : REG_RAX;

        x86_emit_move_to_reg(ctx, inst->operands[0], dst);
        x86_emit_move_to_reg(ctx, inst->operands[1], REG_R11);
        fprintf(ctx->out, "    orq %%r11, %s\n", x86_reg_name(dst));
        if (dst == REG_RAX && ra->reg == REG_SPILL) {
            x86_emit_store_from_reg(ctx, inst->id, REG_RAX);
        }
        break;
    }

    case IR_ALLOCA: {
        /* Alloca space is pre-computed in the frame layout.
         * The "value" of an alloca is the address of its stack slot.
         * We just need to make sure the register (if any) holds the address. */
        RegAlloc *ra = x86_find_alloc(ctx, inst->id);
        if (!ra) break;
        int offset = ctx->alloca_offsets[inst->id];
        if (ra->reg != REG_NONE && ra->reg != REG_SPILL) {
            fprintf(ctx->out, "    leaq %d(%%rbp), %s\n", offset, x86_reg_name(ra->reg));
        }
        /* For spilled allocas, we use the offset directly when loading */
        break;
    }

    case IR_LOAD: {
        /* Load from an address (the operand is an alloca or pointer) */
        RegAlloc *ra = x86_find_alloc(ctx, inst->id);
        if (!ra) break;
        X86Reg dst = (ra->reg != REG_NONE && ra->reg != REG_SPILL) ? ra->reg : REG_RAX;

        int addr_id = inst->operands[0];
        /* Check if the address is an alloca -- use direct stack offset */
        if (addr_id >= 0 && addr_id < X86_MAX_VALUES && ctx->alloca_offsets[addr_id] != 0) {
            fprintf(ctx->out, "    movq %d(%%rbp), %s\n",
                    ctx->alloca_offsets[addr_id], x86_reg_name(dst));
        } else {
            /* General case: load address into scratch, then load from it */
            x86_emit_move_to_reg(ctx, addr_id, REG_R11);
            fprintf(ctx->out, "    movq (%%r11), %s\n", x86_reg_name(dst));
        }
        if (dst == REG_RAX && ra->reg == REG_SPILL) {
            x86_emit_store_from_reg(ctx, inst->id, REG_RAX);
        }
        break;
    }

    case IR_STORE: {
        /* Store value to address */
        int val_id = inst->operands[0];
        int addr_id = inst->operands[1];

        /* Load the value into RAX */
        x86_emit_move_to_reg(ctx, val_id, REG_RAX);

        /* Check if address is an alloca -- use direct stack offset */
        if (addr_id >= 0 && addr_id < X86_MAX_VALUES && ctx->alloca_offsets[addr_id] != 0) {
            fprintf(ctx->out, "    movq %%rax, %d(%%rbp)\n",
                    ctx->alloca_offsets[addr_id]);
        } else {
            x86_emit_move_to_reg(ctx, addr_id, REG_R11);
            fprintf(ctx->out, "    movq %%rax, (%%r11)\n");
        }
        break;
    }

    case IR_CALL: {
        /* System V AMD64 ABI: args in rdi, rsi, rdx, rcx, r8, r9 */
        int i;
        for (i = 0; i < inst->call_arg_count && i < 6; i++) {
            x86_emit_move_to_reg(ctx, inst->call_args[i], abi_arg_regs[i]);
        }
        /* Stack args for > 6 arguments (push in reverse order) */
        if (inst->call_arg_count > 6) {
            for (i = inst->call_arg_count - 1; i >= 6; i--) {
                x86_emit_move_to_reg(ctx, inst->call_args[i], REG_RAX);
                fprintf(ctx->out, "    pushq %%rax\n");
            }
        }

        /* Emit call.  On macOS, C functions need underscore prefix. */
#ifdef __APPLE__
        fprintf(ctx->out, "    callq _%s\n", inst->fn_name ? inst->fn_name : "unknown");
#else
        fprintf(ctx->out, "    callq %s\n", inst->fn_name ? inst->fn_name : "unknown");
#endif

        /* Clean up stack args */
        if (inst->call_arg_count > 6) {
            int stack_args = inst->call_arg_count - 6;
            fprintf(ctx->out, "    addq $%d, %%rsp\n", stack_args * 8);
        }

        /* Result is in RAX */
        if (inst->id >= 0) {
            x86_emit_store_from_reg(ctx, inst->id, REG_RAX);
        }
        break;
    }

    case IR_RET: {
        if (inst->operand_count > 0 && inst->operands[0] >= 0) {
            x86_emit_move_to_reg(ctx, inst->operands[0], REG_RAX);
        }

        /* Restore callee-saved registers (in reverse order of save) */
        if (ctx->uses_r15) fprintf(ctx->out, "    popq %%r15\n");
        if (ctx->uses_r14) fprintf(ctx->out, "    popq %%r14\n");
        if (ctx->uses_r13) fprintf(ctx->out, "    popq %%r13\n");
        if (ctx->uses_r12) fprintf(ctx->out, "    popq %%r12\n");
        if (ctx->uses_rbx) fprintf(ctx->out, "    popq %%rbx\n");

        fprintf(ctx->out, "    leave\n");
        fprintf(ctx->out, "    ret\n");
        break;
    }

    case IR_BR: {
        /* Conditional branch: test condition, branch to true/false blocks */
        x86_emit_move_to_reg(ctx, inst->operands[0], REG_RAX);
        fprintf(ctx->out, "    testq %%rax, %%rax\n");
        fprintf(ctx->out, "    je .L%s_bb%d\n", ctx->fn->name, inst->false_bb);
        fprintf(ctx->out, "    jmp .L%s_bb%d\n", ctx->fn->name, inst->target_bb);
        break;
    }

    case IR_JMP: {
        fprintf(ctx->out, "    jmp .L%s_bb%d\n", ctx->fn->name, inst->target_bb);
        break;
    }

    case IR_PHI: {
        /* PHI nodes are resolved during SSA destruction (not yet implemented).
         * For now, emit a comment. */
        fprintf(ctx->out, "    # phi node %%%d (SSA resolution pending)\n", inst->id);
        break;
    }

    case IR_PRINT: {
        /* Determine the type of the value being printed */
        IrInst *val_inst = x86_find_inst(ctx->fn, inst->operands[0]);
        IrType vtype = val_inst ? val_inst->type : IR_TYPE_I64;

        const char *fmt;
        if (vtype == IR_TYPE_STRING) {
            fmt = "%s\n";
        } else if (vtype == IR_TYPE_F64) {
            fmt = "%g\n";
        } else if (vtype == IR_TYPE_BOOL) {
            /* For bools, we'll print 0/1 as int for simplicity */
            fmt = "%lld\n";
        } else {
            fmt = "%lld\n";
        }

        int fmt_label = x86_add_string(ctx, fmt);

        /* Move the value to RSI (second argument) */
        x86_emit_move_to_reg(ctx, inst->operands[0], REG_RSI);
        /* Move format string to RDI (first argument) */
        fprintf(ctx->out, "    leaq .Lstr_%d(%%rip), %%rdi\n", fmt_label);
        /* Clear AL for varargs (no floating point args in SSE) */
        fprintf(ctx->out, "    xorl %%eax, %%eax\n");
#ifdef __APPLE__
        fprintf(ctx->out, "    callq _printf\n");
#else
        fprintf(ctx->out, "    callq printf\n");
#endif
        break;
    }

    case IR_STR_CONCAT: {
        /* Call lcn_str_concat(lhs, rhs) */
        x86_emit_move_to_reg(ctx, inst->operands[0], REG_RDI);
        x86_emit_move_to_reg(ctx, inst->operands[1], REG_RSI);
#ifdef __APPLE__
        fprintf(ctx->out, "    callq _lcn_str_concat\n");
#else
        fprintf(ctx->out, "    callq lcn_str_concat\n");
#endif
        if (inst->id >= 0) {
            x86_emit_store_from_reg(ctx, inst->id, REG_RAX);
        }
        break;
    }

    case IR_CAST: {
        /* Simple cast: just move the value */
        RegAlloc *ra = x86_find_alloc(ctx, inst->id);
        if (!ra) break;
        X86Reg dst = (ra->reg != REG_NONE && ra->reg != REG_SPILL) ? ra->reg : REG_RAX;
        x86_emit_move_to_reg(ctx, inst->operands[0], dst);
        if (dst == REG_RAX && ra->reg == REG_SPILL) {
            x86_emit_store_from_reg(ctx, inst->id, REG_RAX);
        }
        break;
    }

    case IR_GEP: {
        /* GEP: base + index * 8 (simplified pointer arithmetic) */
        RegAlloc *ra = x86_find_alloc(ctx, inst->id);
        if (!ra) break;
        X86Reg dst = (ra->reg != REG_NONE && ra->reg != REG_SPILL) ? ra->reg : REG_RAX;

        x86_emit_move_to_reg(ctx, inst->operands[0], dst);
        x86_emit_move_to_reg(ctx, inst->operands[1], REG_R11);
        fprintf(ctx->out, "    imulq $8, %%r11\n");
        fprintf(ctx->out, "    addq %%r11, %s\n", x86_reg_name(dst));
        if (dst == REG_RAX && ra->reg == REG_SPILL) {
            x86_emit_store_from_reg(ctx, inst->id, REG_RAX);
        }
        break;
    }

    /* Float arithmetic: simplified — treat as integer ops for now */
    case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV: {
        fprintf(ctx->out, "    # float op %%%d (not yet implemented, treating as NOP)\n",
                inst->id);
        break;
    }

    case IR_NOP:
        fprintf(ctx->out, "    nop\n");
        break;

    default:
        fprintf(ctx->out, "    # unhandled opcode %s\n", ir_opcode_name(inst->op));
        break;
    }
}

/* ============================================================
 * Function Emission
 * ============================================================ */

void ir_emit_x86_function(IrFunction *fn, IrModule *mod, FILE *out) {
    if (!fn) return;

    X86Context ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mod = mod;
    ctx.fn = fn;
    ctx.out = out;
    ctx.next_label = 0;

    /* Run register allocation */
    ir_x86_compute_live_ranges(fn, ctx.allocs, &ctx.alloc_count);
    ir_linear_scan_alloc(ctx.allocs, ctx.alloc_count);

    /* Compute stack frame layout */
    x86_compute_frame(&ctx);

    /* Copy params from ABI registers to their allocated locations.
     * This needs to happen after we know where params live. */

    /* Function label */
#ifdef __APPLE__
    fprintf(out, "    .globl _%s\n", fn->name);
    fprintf(out, "_%s:\n", fn->name);
#else
    fprintf(out, "    .globl %s\n", fn->name);
    fprintf(out, "%s:\n", fn->name);
#endif

    /* Prologue */
    fprintf(out, "    pushq %%rbp\n");
    fprintf(out, "    movq %%rsp, %%rbp\n");

    /* Save callee-saved registers */
    if (ctx.uses_rbx) fprintf(out, "    pushq %%rbx\n");
    if (ctx.uses_r12) fprintf(out, "    pushq %%r12\n");
    if (ctx.uses_r13) fprintf(out, "    pushq %%r13\n");
    if (ctx.uses_r14) fprintf(out, "    pushq %%r14\n");
    if (ctx.uses_r15) fprintf(out, "    pushq %%r15\n");

    /* Allocate stack frame */
    if (ctx.frame_size > 0) {
        fprintf(out, "    subq $%d, %%rsp\n", ctx.frame_size);
    }

    /* Move parameters from ABI registers to their allocated locations */
    {
        int i;
        for (i = 0; i < fn->param_count && i < 6; i++) {
            int vid = fn->param_value_ids[i];
            RegAlloc *ra = x86_find_alloc(&ctx, vid);
            if (ra && ra->reg != REG_NONE && ra->reg != REG_SPILL) {
                if (ra->reg != abi_arg_regs[i]) {
                    fprintf(out, "    movq %s, %s\n",
                            x86_reg_name(abi_arg_regs[i]), x86_reg_name(ra->reg));
                }
            } else if (ra && ra->reg == REG_SPILL) {
                fprintf(out, "    movq %s, %d(%%rbp)\n",
                        x86_reg_name(abi_arg_regs[i]), ra->spill_offset);
            }
        }
    }

    /* Emit basic blocks */
    IrBasicBlock *bb;
    for (bb = fn->entry; bb; bb = bb->next) {
        fprintf(out, ".L%s_bb%d:\n", fn->name, bb->id);

        IrInst *inst;
        for (inst = bb->first; inst; inst = inst->next) {
            x86_emit_inst(&ctx, inst);
        }
    }

    /* If the function doesn't end with a ret, add a safety epilogue */
    {
        IrBasicBlock *last_bb = fn->entry;
        while (last_bb && last_bb->next) last_bb = last_bb->next;
        if (last_bb) {
            IrInst *last = last_bb->last;
            if (!last || (last->op != IR_RET)) {
                fprintf(out, "    xorl %%eax, %%eax\n");
                if (ctx.uses_r15) fprintf(out, "    popq %%r15\n");
                if (ctx.uses_r14) fprintf(out, "    popq %%r14\n");
                if (ctx.uses_r13) fprintf(out, "    popq %%r13\n");
                if (ctx.uses_r12) fprintf(out, "    popq %%r12\n");
                if (ctx.uses_rbx) fprintf(out, "    popq %%rbx\n");
                fprintf(out, "    leave\n");
                fprintf(out, "    ret\n");
            }
        }
    }

    /* Emit string literals in .rodata */
    if (ctx.string_count > 0) {
        fprintf(out, "\n    .section __TEXT,__cstring,cstring_literals\n");
        int i;
        for (i = 0; i < ctx.string_count; i++) {
            fprintf(out, ".Lstr_%d:\n", ctx.strings[i].label_id);
            fprintf(out, "    .asciz \"%s\"\n",
                    ctx.strings[i].value ? ctx.strings[i].value : "");
        }
    }
}

/* ============================================================
 * Module Emission
 * ============================================================ */

void ir_emit_x86(IrModule *mod, FILE *out) {
    if (!mod) return;

    fprintf(out, "# Limceron x86_64 Assembly — generated by limceron compile\n");
    fprintf(out, "# Target: x86_64 (System V AMD64 ABI)\n\n");
    fprintf(out, "    .text\n\n");

    IrFunction *fn;
    for (fn = mod->functions; fn; fn = fn->next) {
        ir_emit_x86_function(fn, mod, out);
        fprintf(out, "\n");
    }
}
