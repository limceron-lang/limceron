/*
 * Limceron Compiler -- AArch64 (ARM64) Assembly Emitter
 *
 * Emits Mach-O compatible ARM64 assembly from SSA IR.
 * Follows AAPCS64 calling convention:
 *   - Args: x0-x7 (first 8 integer args)
 *   - Return: x0
 *   - Callee-saved: x19-x28, x29 (FP), x30 (LR)
 *   - Temp registers: x9-x15
 *   - Frame pointer: x29
 *   - Link register: x30
 *
 * Register allocation uses a simple linear-scan approach over
 * live ranges computed from SSA value IDs.
 */

#include "lcn.h"
#include "ir.h"
#include <string.h>
#include <stdlib.h>

/* ============================================================
 * Platform Detection
 * ============================================================ */

#ifdef __APPLE__
#define ASM_SYMBOL_PREFIX "_"
#define ASM_CSTRING_SECTION ".section __TEXT,__cstring"
#define ASM_TEXT_SECTION ".section __TEXT,__text,regular,pure_instructions"
#define ASM_DATA_SECTION ".section __DATA,__data"
#else
#define ASM_SYMBOL_PREFIX ""
#define ASM_CSTRING_SECTION ".section .rodata"
#define ASM_TEXT_SECTION ".text"
#define ASM_DATA_SECTION ".data"
#endif

/* ============================================================
 * Register Allocator
 * ============================================================ */

#define REGALLOC_MAX_VALUES  4096
#define REGALLOC_NUM_TEMPS   7     /* x9-x15 */
#define REGALLOC_FIRST_TEMP  9     /* x9 */
#define REGALLOC_MAX_SPILL   512

/* LiveRange is defined in ir.h */

typedef struct {
    LiveRange ranges[REGALLOC_MAX_VALUES];
    int       count;
    int       next_spill_offset; /* grows downward from FP */
    int       max_spill;         /* total spill area size */
} RegAllocState;

/* Compute live ranges for all SSA values in a function.
 * inst_index is assigned in linear order across all basic blocks. */
static void regalloc_compute_live_ranges(IrFunction *fn, RegAllocState *state) {
    memset(state, 0, sizeof(RegAllocState));
    state->next_spill_offset = -16; /* below saved FP/LR */

    int inst_idx = 0;
    IrBasicBlock *bb;
    IrInst *inst;

    /* First pass: record definitions (first_use) */
    for (bb = fn->entry; bb; bb = bb->next) {
        for (inst = bb->first; inst; inst = inst->next) {
            if (inst->id >= 0 && state->count < REGALLOC_MAX_VALUES) {
                int idx = state->count;
                state->ranges[idx].value_id = inst->id;
                state->ranges[idx].reg = -1;
                state->ranges[idx].spill_offset = -1;
                state->ranges[idx].first_use = inst_idx;
                state->ranges[idx].last_use = inst_idx;
                state->count++;
            }
            inst_idx++;
        }
    }

    /* Also add entries for function parameters */
    {
        int pi;
        for (pi = 0; pi < fn->param_count; pi++) {
            int vid = fn->param_value_ids[pi];
            /* Check if already tracked */
            bool found = false;
            int ri;
            for (ri = 0; ri < state->count; ri++) {
                if (state->ranges[ri].value_id == vid) {
                    found = true;
                    break;
                }
            }
            if (!found && state->count < REGALLOC_MAX_VALUES) {
                int idx = state->count;
                state->ranges[idx].value_id = vid;
                state->ranges[idx].reg = -1;
                state->ranges[idx].spill_offset = -1;
                state->ranges[idx].first_use = 0;
                state->ranges[idx].last_use = 0;
                state->count++;
            }
        }
    }

    /* Second pass: extend last_use for every reference */
    inst_idx = 0;
    for (bb = fn->entry; bb; bb = bb->next) {
        for (inst = bb->first; inst; inst = inst->next) {
            int oi;
            /* Check operands */
            for (oi = 0; oi < inst->operand_count; oi++) {
                int vid = inst->operands[oi];
                if (vid < 0) continue;
                int ri;
                for (ri = 0; ri < state->count; ri++) {
                    if (state->ranges[ri].value_id == vid) {
                        if (inst_idx > state->ranges[ri].last_use)
                            state->ranges[ri].last_use = inst_idx;
                        break;
                    }
                }
            }
            /* Check call args */
            if (inst->op == IR_CALL) {
                for (oi = 0; oi < inst->call_arg_count; oi++) {
                    int vid = inst->call_args[oi];
                    if (vid < 0) continue;
                    int ri;
                    for (ri = 0; ri < state->count; ri++) {
                        if (state->ranges[ri].value_id == vid) {
                            if (inst_idx > state->ranges[ri].last_use)
                                state->ranges[ri].last_use = inst_idx;
                            break;
                        }
                    }
                }
            }
            /* Check phi args */
            if (inst->op == IR_PHI) {
                for (oi = 0; oi < inst->phi_count; oi++) {
                    int vid = inst->phi_args[oi].value;
                    if (vid < 0) continue;
                    int ri;
                    for (ri = 0; ri < state->count; ri++) {
                        if (state->ranges[ri].value_id == vid) {
                            if (inst_idx > state->ranges[ri].last_use)
                                state->ranges[ri].last_use = inst_idx;
                            break;
                        }
                    }
                }
            }
            inst_idx++;
        }
    }
}

/* Simple linear-scan register allocation.
 * Assigns registers x9-x15 (7 temp registers).
 * Spills to stack when no registers are available. */
static void regalloc_assign(RegAllocState *state) {
    /* Track which registers are in use and their expiry instruction index */
    int reg_expire[REGALLOC_NUM_TEMPS]; /* last_use of the value in each reg */
    int reg_value[REGALLOC_NUM_TEMPS];  /* value_id in each reg */
    int ri;
    for (ri = 0; ri < REGALLOC_NUM_TEMPS; ri++) {
        reg_expire[ri] = -1;
        reg_value[ri] = -1;
    }

    /* Sort ranges by first_use (simple insertion sort) */
    {
        int i, j;
        for (i = 1; i < state->count; i++) {
            LiveRange tmp = state->ranges[i];
            j = i - 1;
            while (j >= 0 && state->ranges[j].first_use > tmp.first_use) {
                state->ranges[j + 1] = state->ranges[j];
                j--;
            }
            state->ranges[j + 1] = tmp;
        }
    }

    /* Allocate */
    int i;
    for (i = 0; i < state->count; i++) {
        LiveRange *lr = &state->ranges[i];

        /* Expire old ranges: free registers whose last_use < current first_use */
        for (ri = 0; ri < REGALLOC_NUM_TEMPS; ri++) {
            if (reg_expire[ri] >= 0 && reg_expire[ri] < lr->first_use) {
                reg_expire[ri] = -1;
                reg_value[ri] = -1;
            }
        }

        /* Try to find a free register */
        bool allocated = false;
        for (ri = 0; ri < REGALLOC_NUM_TEMPS; ri++) {
            if (reg_expire[ri] < 0) {
                lr->reg = REGALLOC_FIRST_TEMP + ri;
                reg_expire[ri] = lr->last_use;
                reg_value[ri] = lr->value_id;
                allocated = true;
                break;
            }
        }

        if (!allocated) {
            /* Spill: find the register with the latest expiry and spill it */
            int best_ri = 0;
            int best_expire = reg_expire[0];
            for (ri = 1; ri < REGALLOC_NUM_TEMPS; ri++) {
                if (reg_expire[ri] > best_expire) {
                    best_expire = reg_expire[ri];
                    best_ri = ri;
                }
            }

            /* If the current range ends before the best candidate, spill current */
            if (lr->last_use < best_expire) {
                /* Spill the current value */
                lr->spill_offset = state->next_spill_offset;
                state->next_spill_offset -= 8;
                lr->reg = -1;
            } else {
                /* Spill the old occupant */
                int old_vid = reg_value[best_ri];
                int oi2;
                for (oi2 = 0; oi2 < state->count; oi2++) {
                    if (state->ranges[oi2].value_id == old_vid) {
                        state->ranges[oi2].spill_offset = state->next_spill_offset;
                        state->next_spill_offset -= 8;
                        state->ranges[oi2].reg = -1;
                        break;
                    }
                }
                /* Assign register to current value */
                lr->reg = REGALLOC_FIRST_TEMP + best_ri;
                reg_expire[best_ri] = lr->last_use;
                reg_value[best_ri] = lr->value_id;
            }
        }
    }

    /* Compute max spill area (aligned to 16 bytes) */
    int spill_bytes = -(state->next_spill_offset + 16);
    if (spill_bytes < 0) spill_bytes = 0;
    state->max_spill = (spill_bytes + 15) & ~15;
}

/* Look up the register or spill offset for a given SSA value */
static LiveRange *regalloc_find(RegAllocState *state, int value_id) {
    int i;
    for (i = 0; i < state->count; i++) {
        if (state->ranges[i].value_id == value_id)
            return &state->ranges[i];
    }
    return NULL;
}

/* ============================================================
 * Emit Context
 * ============================================================ */

typedef struct {
    FILE           *out;
    IrModule       *mod;
    IrFunction     *fn;
    RegAllocState   regalloc;
    int             frame_size;     /* total stack frame size */
    int             alloca_offset;  /* current alloca offset from FP */
    int             string_count;   /* string literal counter */

    /* Map alloca value IDs to their stack offsets */
    struct {
        int value_id;
        int offset;
    } alloca_map[512];
    int alloca_count;
} Arm64EmitCtx;

/* ============================================================
 * Helper: emit register name for an SSA value
 *
 * Returns the register name (e.g., "x9"). If spilled, emits
 * a load from the spill slot into x16 (scratch) and returns "x16".
 * ============================================================ */

static const char *emit_load_value(Arm64EmitCtx *ctx, int value_id) {
    static char buf[8];

    /* Check function parameters first -- they arrive in x0-x7 */
    if (ctx->fn) {
        int pi;
        for (pi = 0; pi < ctx->fn->param_count && pi < 8; pi++) {
            if (ctx->fn->param_value_ids[pi] == value_id) {
                snprintf(buf, sizeof(buf), "x%d", pi);
                return buf;
            }
        }
    }

    LiveRange *lr = regalloc_find(&ctx->regalloc, value_id);
    if (!lr) {
        /* Fallback: use x16 scratch and assume it's on stack */
        return "x16";
    }

    if (lr->reg >= 0) {
        snprintf(buf, sizeof(buf), "x%d", lr->reg);
        return buf;
    }

    /* Spilled: load from stack into x16 */
    fprintf(ctx->out, "    ldr x16, [x29, #%d]\n", lr->spill_offset);
    return "x16";
}

/* Like emit_load_value but uses x17 as alternate scratch */
static const char *emit_load_value_alt(Arm64EmitCtx *ctx, int value_id) {
    static char buf[8];

    if (ctx->fn) {
        int pi;
        for (pi = 0; pi < ctx->fn->param_count && pi < 8; pi++) {
            if (ctx->fn->param_value_ids[pi] == value_id) {
                snprintf(buf, sizeof(buf), "x%d", pi);
                return buf;
            }
        }
    }

    LiveRange *lr = regalloc_find(&ctx->regalloc, value_id);
    if (!lr) {
        return "x17";
    }

    if (lr->reg >= 0) {
        snprintf(buf, sizeof(buf), "x%d", lr->reg);
        return buf;
    }

    fprintf(ctx->out, "    ldr x17, [x29, #%d]\n", lr->spill_offset);
    return "x17";
}

/* Get the destination register name for an SSA value.
 * If spilled, returns "x16" and the caller must store it after. */
static const char *emit_dest_reg(Arm64EmitCtx *ctx, int value_id) {
    static char buf[8];

    LiveRange *lr = regalloc_find(&ctx->regalloc, value_id);
    if (!lr || lr->reg < 0) {
        return "x16";
    }
    snprintf(buf, sizeof(buf), "x%d", lr->reg);
    return buf;
}

/* If an SSA value is spilled, emit a store from x16 to its slot */
static void emit_store_if_spilled(Arm64EmitCtx *ctx, int value_id) {
    LiveRange *lr = regalloc_find(&ctx->regalloc, value_id);
    if (lr && lr->reg < 0 && lr->spill_offset != -1) {
        fprintf(ctx->out, "    str x16, [x29, #%d]\n", lr->spill_offset);
    }
}

/* Look up the alloca stack offset for a given alloca value ID */
static int alloca_offset_for(Arm64EmitCtx *ctx, int alloca_id) {
    int i;
    for (i = 0; i < ctx->alloca_count; i++) {
        if (ctx->alloca_map[i].value_id == alloca_id)
            return ctx->alloca_map[i].offset;
    }
    return -128; /* fallback */
}

/* ============================================================
 * Large Immediate Handling
 * ============================================================ */

/* Emit a mov of a 64-bit immediate into a register.
 * For small values uses mov; for large values uses movz/movk. */
static void emit_mov_imm64(FILE *out, const char *reg, int64_t val) {
    uint64_t uval = (uint64_t)val;

    /* Small positive values: single mov */
    if (val >= 0 && val <= 0xFFFF) {
        fprintf(out, "    mov %s, #%lld\n", reg, (long long)val);
        return;
    }

    /* Small negative values: single movn */
    if (val < 0 && val >= -0x10000) {
        fprintf(out, "    mov %s, #%lld\n", reg, (long long)val);
        return;
    }

    /* General case: movz + movk sequence */
    fprintf(out, "    movz %s, #%u, lsl #0\n", reg,
            (unsigned)(uval & 0xFFFF));

    if ((uval >> 16) & 0xFFFF) {
        fprintf(out, "    movk %s, #%u, lsl #16\n", reg,
                (unsigned)((uval >> 16) & 0xFFFF));
    }
    if ((uval >> 32) & 0xFFFF) {
        fprintf(out, "    movk %s, #%u, lsl #32\n", reg,
                (unsigned)((uval >> 32) & 0xFFFF));
    }
    if ((uval >> 48) & 0xFFFF) {
        fprintf(out, "    movk %s, #%u, lsl #48\n", reg,
                (unsigned)((uval >> 48) & 0xFFFF));
    }
}

/* ============================================================
 * Pre-scan: Count allocas and compute frame layout
 * ============================================================ */

static void emit_prescan_function(Arm64EmitCtx *ctx) {
    ctx->alloca_count = 0;
    ctx->alloca_offset = -16; /* start below FP/LR save area */

    /* Skip past the regalloc spill area */
    ctx->alloca_offset -= ctx->regalloc.max_spill;

    IrBasicBlock *bb;
    IrInst *inst;
    for (bb = ctx->fn->entry; bb; bb = bb->next) {
        for (inst = bb->first; inst; inst = inst->next) {
            if (inst->op == IR_ALLOCA && ctx->alloca_count < 512) {
                ctx->alloca_offset -= 8;
                ctx->alloca_map[ctx->alloca_count].value_id = inst->id;
                ctx->alloca_map[ctx->alloca_count].offset = ctx->alloca_offset;
                ctx->alloca_count++;
            }
        }
    }

    /* Frame size = 16 (FP/LR) + spill area + alloca area, aligned to 16 */
    int total = 16 + ctx->regalloc.max_spill + (ctx->alloca_count * 8);
    ctx->frame_size = (total + 15) & ~15;
    if (ctx->frame_size < 16) ctx->frame_size = 16;
}

/* ============================================================
 * Instruction Emission
 * ============================================================ */

static void emit_inst(Arm64EmitCtx *ctx, IrInst *inst) {
    if (!inst) return;

    switch (inst->op) {

    case IR_CONST_INT: {
        const char *dst = emit_dest_reg(ctx, inst->id);
        emit_mov_imm64(ctx->out, dst, inst->imm_int);
        emit_store_if_spilled(ctx, inst->id);
        break;
    }

    case IR_CONST_BOOL: {
        const char *dst = emit_dest_reg(ctx, inst->id);
        fprintf(ctx->out, "    mov %s, #%lld\n", dst,
                (long long)inst->imm_int);
        emit_store_if_spilled(ctx, inst->id);
        break;
    }

    case IR_CONST_FLOAT: {
        /* Store float constant in data section, load via adrp/add + ldr */
        const char *dst = emit_dest_reg(ctx, inst->id);
        fprintf(ctx->out, "    ; load f64 %g (via integer reinterpret)\n",
                inst->imm_float);
        /* Reinterpret float bits as integer for simplicity */
        union { double d; uint64_t u; } conv;
        conv.d = inst->imm_float;
        emit_mov_imm64(ctx->out, dst, (int64_t)conv.u);
        emit_store_if_spilled(ctx, inst->id);
        break;
    }

    case IR_CONST_STRING: {
        /* String literal: reference is handled in string data section.
         * Here we load the address. */
        const char *dst = emit_dest_reg(ctx, inst->id);
        int str_id = ctx->string_count++;
        (void)str_id; /* Will be patched in a second pass -- for now use label */
        /* We use the instruction's id to create a unique label */
        fprintf(ctx->out, "    adrp %s, .Lstr_%d@PAGE\n", dst, inst->id);
        fprintf(ctx->out, "    add %s, %s, .Lstr_%d@PAGEOFF\n", dst, dst, inst->id);
        emit_store_if_spilled(ctx, inst->id);
        break;
    }

    case IR_ADD: {
        const char *lhs = emit_load_value(ctx, inst->operands[0]);
        const char *rhs = emit_load_value_alt(ctx, inst->operands[1]);
        const char *dst = emit_dest_reg(ctx, inst->id);
        fprintf(ctx->out, "    add %s, %s, %s\n", dst, lhs, rhs);
        emit_store_if_spilled(ctx, inst->id);
        break;
    }

    case IR_SUB: {
        const char *lhs = emit_load_value(ctx, inst->operands[0]);
        const char *rhs = emit_load_value_alt(ctx, inst->operands[1]);
        const char *dst = emit_dest_reg(ctx, inst->id);
        fprintf(ctx->out, "    sub %s, %s, %s\n", dst, lhs, rhs);
        emit_store_if_spilled(ctx, inst->id);
        break;
    }

    case IR_MUL: {
        const char *lhs = emit_load_value(ctx, inst->operands[0]);
        const char *rhs = emit_load_value_alt(ctx, inst->operands[1]);
        const char *dst = emit_dest_reg(ctx, inst->id);
        fprintf(ctx->out, "    mul %s, %s, %s\n", dst, lhs, rhs);
        emit_store_if_spilled(ctx, inst->id);
        break;
    }

    case IR_DIV: {
        const char *lhs = emit_load_value(ctx, inst->operands[0]);
        const char *rhs = emit_load_value_alt(ctx, inst->operands[1]);
        const char *dst = emit_dest_reg(ctx, inst->id);
        fprintf(ctx->out, "    sdiv %s, %s, %s\n", dst, lhs, rhs);
        emit_store_if_spilled(ctx, inst->id);
        break;
    }

    case IR_MOD: {
        /* ARM64 has no mod instruction; use sdiv + msub */
        const char *lhs = emit_load_value(ctx, inst->operands[0]);
        const char *rhs = emit_load_value_alt(ctx, inst->operands[1]);
        const char *dst = emit_dest_reg(ctx, inst->id);
        fprintf(ctx->out, "    sdiv x18, %s, %s\n", lhs, rhs);
        fprintf(ctx->out, "    msub %s, x18, %s, %s\n", dst, rhs, lhs);
        emit_store_if_spilled(ctx, inst->id);
        break;
    }

    case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV: {
        /* Float ops: for now, emit as integer ops with a comment.
         * Full float support would use d0-d7 NEON registers. */
        const char *op_name = "fadd";
        if (inst->op == IR_FSUB) op_name = "fsub";
        if (inst->op == IR_FMUL) op_name = "fmul";
        if (inst->op == IR_FDIV) op_name = "fdiv";
        fprintf(ctx->out, "    ; %s (float op via integer regs -- placeholder)\n",
                op_name);
        /* Emit as a call to a runtime helper */
        const char *lhs = emit_load_value(ctx, inst->operands[0]);
        const char *rhs = emit_load_value_alt(ctx, inst->operands[1]);
        const char *dst = emit_dest_reg(ctx, inst->id);
        fprintf(ctx->out, "    ; %s = %s %s, %s\n", dst, op_name, lhs, rhs);
        /* Placeholder: just move LHS to dest */
        if (strcmp(dst, lhs) != 0)
            fprintf(ctx->out, "    mov %s, %s\n", dst, lhs);
        emit_store_if_spilled(ctx, inst->id);
        break;
    }

    case IR_NEG: {
        const char *src = emit_load_value(ctx, inst->operands[0]);
        const char *dst = emit_dest_reg(ctx, inst->id);
        fprintf(ctx->out, "    neg %s, %s\n", dst, src);
        emit_store_if_spilled(ctx, inst->id);
        break;
    }

    case IR_NOT: {
        const char *src = emit_load_value(ctx, inst->operands[0]);
        const char *dst = emit_dest_reg(ctx, inst->id);
        fprintf(ctx->out, "    cmp %s, #0\n", src);
        fprintf(ctx->out, "    cset %s, eq\n", dst);
        emit_store_if_spilled(ctx, inst->id);
        break;
    }

    case IR_CMP_EQ: case IR_CMP_NE:
    case IR_CMP_LT: case IR_CMP_GT:
    case IR_CMP_LE: case IR_CMP_GE: {
        const char *lhs = emit_load_value(ctx, inst->operands[0]);
        const char *rhs = emit_load_value_alt(ctx, inst->operands[1]);
        const char *dst = emit_dest_reg(ctx, inst->id);
        const char *cond = "eq";
        switch (inst->op) {
        case IR_CMP_EQ: cond = "eq"; break;
        case IR_CMP_NE: cond = "ne"; break;
        case IR_CMP_LT: cond = "lt"; break;
        case IR_CMP_GT: cond = "gt"; break;
        case IR_CMP_LE: cond = "le"; break;
        case IR_CMP_GE: cond = "ge"; break;
        default: break;
        }
        fprintf(ctx->out, "    cmp %s, %s\n", lhs, rhs);
        fprintf(ctx->out, "    cset %s, %s\n", dst, cond);
        emit_store_if_spilled(ctx, inst->id);
        break;
    }

    case IR_AND: {
        const char *lhs = emit_load_value(ctx, inst->operands[0]);
        const char *rhs = emit_load_value_alt(ctx, inst->operands[1]);
        const char *dst = emit_dest_reg(ctx, inst->id);
        fprintf(ctx->out, "    and %s, %s, %s\n", dst, lhs, rhs);
        emit_store_if_spilled(ctx, inst->id);
        break;
    }

    case IR_OR: {
        const char *lhs = emit_load_value(ctx, inst->operands[0]);
        const char *rhs = emit_load_value_alt(ctx, inst->operands[1]);
        const char *dst = emit_dest_reg(ctx, inst->id);
        fprintf(ctx->out, "    orr %s, %s, %s\n", dst, lhs, rhs);
        emit_store_if_spilled(ctx, inst->id);
        break;
    }

    case IR_ALLOCA: {
        /* Allocas are pre-scanned; here we just load the address into the dest reg */
        int offset = alloca_offset_for(ctx, inst->id);
        const char *dst = emit_dest_reg(ctx, inst->id);
        if (offset >= 0 && offset <= 4095) {
            fprintf(ctx->out, "    add %s, x29, #%d\n", dst, offset);
        } else if (offset < 0 && offset >= -4095) {
            fprintf(ctx->out, "    sub %s, x29, #%d\n", dst, -offset);
        } else {
            /* Large offset: load into register, then add */
            emit_mov_imm64(ctx->out, dst, (int64_t)offset);
            fprintf(ctx->out, "    add %s, x29, %s\n", dst, dst);
        }
        emit_store_if_spilled(ctx, inst->id);
        break;
    }

    case IR_LOAD: {
        /* Load from an address (which is the result of an alloca or param) */
        int addr_id = inst->operands[0];
        const char *dst = emit_dest_reg(ctx, inst->id);

        /* Check if the address is an alloca -- load directly from known offset */
        int offset = alloca_offset_for(ctx, addr_id);
        if (offset != -128) {
            fprintf(ctx->out, "    ldr %s, [x29, #%d]\n", dst, offset);
        } else {
            /* Address is in a register */
            const char *addr_reg = emit_load_value(ctx, addr_id);
            fprintf(ctx->out, "    ldr %s, [%s]\n", dst, addr_reg);
        }
        emit_store_if_spilled(ctx, inst->id);
        break;
    }

    case IR_STORE: {
        /* Store value to address */
        int val_id = inst->operands[0];
        int addr_id = inst->operands[1];

        const char *val_reg = emit_load_value(ctx, val_id);

        /* Check if address is an alloca */
        int offset = alloca_offset_for(ctx, addr_id);
        if (offset != -128) {
            fprintf(ctx->out, "    str %s, [x29, #%d]\n", val_reg, offset);
        } else {
            const char *addr_reg = emit_load_value_alt(ctx, addr_id);
            fprintf(ctx->out, "    str %s, [%s]\n", val_reg, addr_reg);
        }
        break;
    }

    case IR_CALL: {
        /* Move arguments into x0-x7 */
        int ai;
        for (ai = 0; ai < inst->call_arg_count && ai < 8; ai++) {
            const char *src = emit_load_value(ctx, inst->call_args[ai]);
            if (strcmp(src, "x16") == 0 || strcmp(src, "x17") == 0) {
                fprintf(ctx->out, "    mov x%d, %s\n", ai, src);
            } else {
                int src_reg = -1;
                if (src[0] == 'x') src_reg = atoi(src + 1);
                if (src_reg != ai) {
                    fprintf(ctx->out, "    mov x%d, %s\n", ai, src);
                }
            }
        }

        /* Call the function */
        fprintf(ctx->out, "    bl %s%s\n",
                ASM_SYMBOL_PREFIX,
                inst->fn_name ? inst->fn_name : "_unknown");

        /* Result is in x0; move to destination register */
        if (inst->id >= 0 && inst->type != IR_TYPE_VOID) {
            const char *dst = emit_dest_reg(ctx, inst->id);
            if (strcmp(dst, "x0") != 0) {
                fprintf(ctx->out, "    mov %s, x0\n", dst);
            }
            emit_store_if_spilled(ctx, inst->id);
        }
        break;
    }

    case IR_RET: {
        if (inst->operand_count > 0 && inst->operands[0] >= 0) {
            const char *src = emit_load_value(ctx, inst->operands[0]);
            if (strcmp(src, "x0") != 0) {
                fprintf(ctx->out, "    mov x0, %s\n", src);
            }
        }
        /* Epilogue */
        fprintf(ctx->out, "    ldp x29, x30, [sp], #%d\n", ctx->frame_size);
        fprintf(ctx->out, "    ret\n");
        break;
    }

    case IR_BR: {
        /* Conditional branch: br %cond, @true_bb, @false_bb */
        const char *cond_reg = emit_load_value(ctx, inst->operands[0]);
        fprintf(ctx->out, "    cbz %s, .LBB%s_%d\n",
                cond_reg, ctx->fn->name, inst->false_bb);
        fprintf(ctx->out, "    b .LBB%s_%d\n",
                ctx->fn->name, inst->target_bb);
        break;
    }

    case IR_JMP: {
        fprintf(ctx->out, "    b .LBB%s_%d\n",
                ctx->fn->name, inst->target_bb);
        break;
    }

    case IR_PHI: {
        /* Phi nodes are resolved during SSA destruction.
         * For our simple emitter, they become mov from the predecessor's value.
         * Since we don't do SSA destruction, emit as nop with comment. */
        fprintf(ctx->out, "    ; phi %%%d (resolved by predecessor moves)\n",
                inst->id);
        break;
    }

    case IR_STR_CONCAT: {
        /* Call a runtime helper: lcn_str_concat(x0=lhs, x1=rhs) -> x0 */
        const char *lhs = emit_load_value(ctx, inst->operands[0]);
        fprintf(ctx->out, "    mov x0, %s\n", lhs);
        const char *rhs = emit_load_value_alt(ctx, inst->operands[1]);
        fprintf(ctx->out, "    mov x1, %s\n", rhs);
        fprintf(ctx->out, "    bl %slcn_str_concat\n", ASM_SYMBOL_PREFIX);
        if (inst->id >= 0) {
            const char *dst = emit_dest_reg(ctx, inst->id);
            if (strcmp(dst, "x0") != 0)
                fprintf(ctx->out, "    mov %s, x0\n", dst);
            emit_store_if_spilled(ctx, inst->id);
        }
        break;
    }

    case IR_PRINT: {
        /* Call runtime: lcn_print(x0=value) or printf */
        const char *src = emit_load_value(ctx, inst->operands[0]);
        fprintf(ctx->out, "    mov x0, %s\n", src);
        fprintf(ctx->out, "    bl %slcn_print\n", ASM_SYMBOL_PREFIX);
        break;
    }

    case IR_CAST: {
        /* Simple move for now (type punning) */
        const char *src = emit_load_value(ctx, inst->operands[0]);
        const char *dst = emit_dest_reg(ctx, inst->id);
        if (strcmp(dst, src) != 0)
            fprintf(ctx->out, "    mov %s, %s\n", dst, src);
        emit_store_if_spilled(ctx, inst->id);
        break;
    }

    case IR_GEP: {
        /* Get element pointer: base + index * 8 */
        const char *base = emit_load_value(ctx, inst->operands[0]);
        const char *idx = emit_load_value_alt(ctx, inst->operands[1]);
        const char *dst = emit_dest_reg(ctx, inst->id);
        fprintf(ctx->out, "    add %s, %s, %s, lsl #3\n", dst, base, idx);
        emit_store_if_spilled(ctx, inst->id);
        break;
    }

    case IR_NOP:
        fprintf(ctx->out, "    nop\n");
        break;

    default:
        fprintf(ctx->out, "    ; unhandled opcode %d\n", inst->op);
        break;
    }
}

/* ============================================================
 * Function Emission
 * ============================================================ */

static void emit_function(Arm64EmitCtx *ctx, IrFunction *fn) {
    ctx->fn = fn;
    ctx->string_count = 0;

    /* Run register allocation */
    regalloc_compute_live_ranges(fn, &ctx->regalloc);
    regalloc_assign(&ctx->regalloc);

    /* Pre-scan for allocas and compute frame layout */
    emit_prescan_function(ctx);

    /* Function header */
    fprintf(ctx->out, "    .globl %s%s\n", ASM_SYMBOL_PREFIX, fn->name);
    fprintf(ctx->out, "    .p2align 2\n");
    fprintf(ctx->out, "%s%s:\n", ASM_SYMBOL_PREFIX, fn->name);

    /* Prologue: save FP/LR, set up frame */
    fprintf(ctx->out, "    stp x29, x30, [sp, #-%d]!\n", ctx->frame_size);
    fprintf(ctx->out, "    mov x29, sp\n");

    /* Emit basic blocks */
    IrBasicBlock *bb;
    for (bb = fn->entry; bb; bb = bb->next) {
        /* Block label */
        fprintf(ctx->out, ".LBB%s_%d:\n", fn->name, bb->id);
        if (bb->label) {
            fprintf(ctx->out, "    ; %s\n", bb->label);
        }

        /* Instructions */
        IrInst *inst;
        for (inst = bb->first; inst; inst = inst->next) {
            emit_inst(ctx, inst);
        }
    }

    /* If the function doesn't end with a ret, add one as safety net */
    {
        IrBasicBlock *last_bb = fn->entry;
        while (last_bb && last_bb->next) last_bb = last_bb->next;
        if (last_bb) {
            IrInst *last = last_bb->last;
            if (!last || last->op != IR_RET) {
                fprintf(ctx->out, "    ; implicit return\n");
                fprintf(ctx->out, "    mov x0, #0\n");
                fprintf(ctx->out, "    ldp x29, x30, [sp], #%d\n",
                        ctx->frame_size);
                fprintf(ctx->out, "    ret\n");
            }
        }
    }

    fprintf(ctx->out, "\n");
}

/* ============================================================
 * String Literal Section
 *
 * Collect all IR_CONST_STRING instructions across the module
 * and emit them in a __cstring section.
 * ============================================================ */

static void emit_string_literals(Arm64EmitCtx *ctx) {
    bool has_strings = false;

    /* Check if there are any string constants */
    IrFunction *fn;
    for (fn = ctx->mod->functions; fn; fn = fn->next) {
        IrBasicBlock *bb;
        for (bb = fn->entry; bb; bb = bb->next) {
            IrInst *inst;
            for (inst = bb->first; inst; inst = inst->next) {
                if (inst->op == IR_CONST_STRING) {
                    has_strings = true;
                    break;
                }
            }
            if (has_strings) break;
        }
        if (has_strings) break;
    }

    if (!has_strings) return;

    fprintf(ctx->out, "%s\n", ASM_CSTRING_SECTION);

    for (fn = ctx->mod->functions; fn; fn = fn->next) {
        IrBasicBlock *bb;
        for (bb = fn->entry; bb; bb = bb->next) {
            IrInst *inst;
            for (inst = bb->first; inst; inst = inst->next) {
                if (inst->op == IR_CONST_STRING) {
                    fprintf(ctx->out, ".Lstr_%d:\n", inst->id);
                    fprintf(ctx->out, "    .asciz \"%s\"\n",
                            inst->imm_str ? inst->imm_str : "");
                }
            }
        }
    }

    fprintf(ctx->out, "\n");
}

/* ============================================================
 * Module Emission (Public API)
 * ============================================================ */

void ir_emit_arm64(IrModule *mod, FILE *out) {
    if (!mod || !out) return;

    Arm64EmitCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.out = out;
    ctx.mod = mod;

    /* Header comment */
    fprintf(out, "; Limceron AArch64 Assembly\n");
    fprintf(out, "; Generated by limceron-stage0 IR backend\n");
    fprintf(out, "; Target: aarch64-%s\n\n",
#ifdef __APPLE__
            "apple-macos"
#else
            "linux-gnu"
#endif
    );

    /* Text section */
    fprintf(out, "%s\n\n", ASM_TEXT_SECTION);

    /* Emit all functions */
    IrFunction *fn;
    bool has_lcn_main = false;
    for (fn = mod->functions; fn; fn = fn->next) {
        emit_function(&ctx, fn);
        if (fn->name && strcmp(fn->name, "lcn_main") == 0)
            has_lcn_main = true;
    }

    /* Emit a _main entry point that calls lcn_main, if present.
     * This allows the generated binary to link as a standalone executable. */
    if (has_lcn_main) {
        fprintf(out, "    ; Entry point trampoline\n");
        fprintf(out, "    .globl %smain\n", ASM_SYMBOL_PREFIX);
        fprintf(out, "    .p2align 2\n");
        fprintf(out, "%smain:\n", ASM_SYMBOL_PREFIX);
        fprintf(out, "    stp x29, x30, [sp, #-16]!\n");
        fprintf(out, "    mov x29, sp\n");
        fprintf(out, "    bl %slcn_main\n", ASM_SYMBOL_PREFIX);
        fprintf(out, "    ldp x29, x30, [sp], #16\n");
        fprintf(out, "    ret\n\n");
    }

    /* String literals */
    emit_string_literals(&ctx);
}

/* ============================================================
 * Register Allocator Public API (shared interface)
 * ============================================================ */

void ir_compute_live_ranges(IrFunction *fn, LiveRange *ranges,
                            int *count, int max_ranges) {
    RegAllocState state;
    regalloc_compute_live_ranges(fn, &state);

    int i;
    int n = state.count < max_ranges ? state.count : max_ranges;
    for (i = 0; i < n; i++) {
        ranges[i] = state.ranges[i];
    }
    *count = n;
}

void ir_alloc_registers(LiveRange *ranges, int count, int num_regs) {
    /* Build a temporary state and run allocation */
    RegAllocState state;
    memset(&state, 0, sizeof(state));
    state.next_spill_offset = -16;

    int n = count < REGALLOC_MAX_VALUES ? count : REGALLOC_MAX_VALUES;
    int i;
    for (i = 0; i < n; i++) {
        state.ranges[i] = ranges[i];
    }
    state.count = n;

    /* Override num temps for the allocation */
    /* Note: this uses the global REGALLOC settings, num_regs is informational */
    (void)num_regs;
    regalloc_assign(&state);

    for (i = 0; i < n; i++) {
        ranges[i] = state.ranges[i];
    }
}
