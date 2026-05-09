/*
 * Limceron Compiler -- WebAssembly (wasm32-wasip2) Backend
 *
 * Walks the SSA IR module and emits a WebAssembly text-format (WAT) file,
 * then runs `wat2wasm` to assemble it into a `.wasm` binary.
 *
 * Pipeline:
 *   AST -> ir_gen_program() -> IrModule
 *   IrModule -> emit_module() -> /tmp/lcn_<pid>.wat
 *   wat2wasm /tmp/lcn_<pid>.wat -> output .wasm
 *
 * IR -> WAT lowering rules (PoC scope):
 *
 *   IR_CONST_INT      -> i64.const N                         | local.set $rN
 *   IR_CONST_FLOAT    -> f64.const F                         | local.set $rN
 *   IR_CONST_BOOL     -> i32.const 0/1                       | local.set $rN
 *   IR_CONST_STRING   -> i32.const <ptr>                     | local.set $rN
 *                        (data segment owns the bytes; per the spec we keep
 *                         a single i32 ptr per string -- length is implicit
 *                         from the 4-byte length prefix at offset[ptr-4])
 *   IR_ADD/SUB/MUL    -> i64.{add,sub,mul}
 *   IR_DIV/MOD        -> i64.{div_s,rem_s}
 *   IR_FADD..FDIV     -> f64.{add,sub,mul,div}
 *   IR_NEG            -> (i64.sub (i64.const 0) %x)
 *   IR_NOT            -> (i32.eqz %x) for bool, (i64.eqz) -> i32 cast for int
 *   IR_CMP_*          -> i64.{eq,ne,lt_s,...}  (result is i32, stored in
 *                        the bool slot of the dest local)
 *   IR_AND/OR         -> i32.{and,or}
 *   IR_BR             -> set $bb to true_bb if cond else false_bb; br $dispatch
 *   IR_JMP            -> set $bb to target_bb; br $dispatch
 *   IR_RET            -> return value (or just return)
 *   IR_CALL           -> call $lcn_<name>
 *   IR_LOAD           -> i64.load (offset 0)
 *   IR_STORE          -> i64.store
 *   IR_ALLOCA         -> bump pointer in linear memory; return i32 ptr
 *   IR_PHI            -> handled at predecessor: each branch terminator
 *                        emits the local.set for any phi node in the target
 *                        before performing the dispatch jump.
 *
 * Control flow is structured via the well-known dispatch-loop pattern:
 *
 *   (block $exit
 *     (loop $dispatch
 *       (block $bb_N ... (block $bb_1 (block $bb_0
 *         local.get $bb
 *         br_table $bb_0 $bb_1 ... $bb_N $exit))..))
 *       <bb_0 body>
 *       <bb_1 body>
 *       ...
 *     )
 *   )
 *
 * That gives us correct semantics for arbitrary CFGs without needing a
 * full relooper -- a transition jump just sets `$bb` to the target index
 * and `br $dispatch`s back to the dispatcher.
 */

#include "lcn.h"
#include "ir.h"
#include "wit_emit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>

/* ============================================================
 * Helpers
 * ============================================================ */

#define WASM_DATA_BASE        1024     /* leave [0..1024) free for stack */
#define WASM_MAX_STRINGS      512
#define WASM_MAX_LOCALS       4096
#define WASM_BUMP_PTR_GLOBAL  "$bump_ptr"

typedef struct {
    const char *value;       /* pointer into IR (arena-owned), key for dedup */
    int         offset;      /* byte offset in linear memory */
    int         length;      /* UTF-8 byte length */
} WasmString;

typedef struct {
    FILE         *out;
    IrModule     *module;
    Arena        *arena;
    const LcnTarget *target;

    /* String table (deduped, shared across the whole module). */
    WasmString    strings[WASM_MAX_STRINGS];
    int           string_count;
    int           data_offset;     /* current end of data segment */

    /* Whether any function uses linear-memory allocas (needs bump ptr). */
    int           uses_alloca;

    /* Whether any function uses i32 stores/loads for bool/string locals. */
    int           uses_i32_load;

    /* Total bytes reserved for static data. */
    int           data_total;
} EmitCtx;

/* Per-function emission state. */
typedef struct {
    EmitCtx      *gctx;
    IrFunction   *fn;

    /* Local slot type for each SSA value id in this function.
     * value_type[id] = IR_TYPE_* (or -1 if unused). */
    IrType        value_type[WASM_MAX_LOCALS];
    int           value_used[WASM_MAX_LOCALS];

    /* Number of basic blocks (for the dispatcher). */
    int           bb_count;

    /* Highest SSA value id encountered + 1. */
    int           max_value;
} FnCtx;

/* ============================================================
 * WAT escaping for data segments and string literals
 * ============================================================ */

/* Emit a string into a (data ...) segment using \xx hex escapes. */
static void emit_data_bytes(FILE *out, const char *bytes, int len) {
    int i;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)bytes[i];
        /* Emit printable ASCII (except quote, backslash) directly; the rest
         * escaped. wat2wasm expects \HH two-char hex escapes. */
        if (c == '"' || c == '\\') {
            fprintf(out, "\\%c", c);
        } else if (c >= 0x20 && c < 0x7F) {
            fputc(c, out);
        } else {
            fprintf(out, "\\%02x", c);
        }
    }
}

/* Encode a 32-bit little-endian integer as four \HH escapes. */
static void emit_data_u32_le(FILE *out, uint32_t v) {
    fprintf(out, "\\%02x\\%02x\\%02x\\%02x",
            (unsigned)(v & 0xFF),
            (unsigned)((v >> 8) & 0xFF),
            (unsigned)((v >> 16) & 0xFF),
            (unsigned)((v >> 24) & 0xFF));
}

/* ============================================================
 * String interning
 * ============================================================ */

/* Look up or insert a string. Returns the offset of the UTF-8 bytes
 * (the 4-byte length prefix sits at offset-4). Stores out_len on success. */
static int intern_string(EmitCtx *ctx, const char *s, int *out_len) {
    if (!s) s = "";
    int slen = (int)strlen(s);
    int i;

    /* Dedup: linear search is fine (PoC). */
    for (i = 0; i < ctx->string_count; i++) {
        if (ctx->strings[i].length == slen &&
            (ctx->strings[i].value == s ||
             (ctx->strings[i].value &&
              strcmp(ctx->strings[i].value, s) == 0))) {
            if (out_len) *out_len = slen;
            return ctx->strings[i].offset;
        }
    }

    if (ctx->string_count >= WASM_MAX_STRINGS) {
        if (out_len) *out_len = slen;
        return WASM_DATA_BASE; /* fallback; cannot intern more */
    }

    /* Layout: [u32 length][UTF-8 bytes][padding to 8-byte alignment]
     * The "offset" we return points at the bytes (length prefix at off-4). */
    int prefix_off = ctx->data_offset;
    int bytes_off  = prefix_off + 4;
    int next_off   = bytes_off + slen;
    /* Align next allocation to 8 bytes. */
    next_off = (next_off + 7) & ~7;

    ctx->strings[ctx->string_count].value  = s;
    ctx->strings[ctx->string_count].offset = bytes_off;
    ctx->strings[ctx->string_count].length = slen;
    ctx->string_count++;
    ctx->data_offset = next_off;

    if (out_len) *out_len = slen;
    return bytes_off;
}

/* ============================================================
 * Type helpers
 * ============================================================ */

/* Return the WASM type name string for an IR type. */
static const char *wasm_type_name(IrType t) {
    switch (t) {
    case IR_TYPE_I64:    return "i64";
    case IR_TYPE_F64:    return "f64";
    case IR_TYPE_BOOL:   return "i32";
    case IR_TYPE_STRING: return "i32";   /* pointer into linear memory */
    case IR_TYPE_PTR:    return "i32";
    case IR_TYPE_VOID:   return "";
    case IR_TYPE_STRUCT: return "i32";
    }
    return "i64";
}

/* ============================================================
 * Pre-pass: gather per-SSA-value types
 *
 * We need to know each value's type so we can declare the correct
 * `(local ...)` slot for it. The IrInst.type field holds the result
 * type for value-producing instructions. We also track param types.
 * ============================================================ */

static void scan_function_values(FnCtx *fctx) {
    int i;
    for (i = 0; i < WASM_MAX_LOCALS; i++) {
        fctx->value_type[i] = IR_TYPE_VOID;
        fctx->value_used[i] = 0;
    }
    fctx->max_value = 0;

    /* Function parameters first. */
    for (i = 0; i < fctx->fn->param_count; i++) {
        int vid = fctx->fn->param_value_ids[i];
        if (vid >= 0 && vid < WASM_MAX_LOCALS) {
            fctx->value_type[vid] = fctx->fn->param_types[i];
            fctx->value_used[vid] = 1;
            if (vid >= fctx->max_value) fctx->max_value = vid + 1;
        }
    }

    /* Walk instructions. */
    fctx->bb_count = 0;
    IrBasicBlock *bb;
    for (bb = fctx->fn->entry; bb; bb = bb->next) {
        if (bb->id + 1 > fctx->bb_count) fctx->bb_count = bb->id + 1;
        IrInst *inst;
        for (inst = bb->first; inst; inst = inst->next) {
            int vid = inst->id;
            /* Many instructions don't produce values (store, ret, br, jmp). */
            int produces = 1;
            switch (inst->op) {
            case IR_STORE:
            case IR_RET:
            case IR_BR:
            case IR_JMP:
            case IR_PRINT:
            case IR_NOP:
                produces = 0;
                break;
            default:
                produces = (vid >= 0);
                break;
            }
            if (produces && vid >= 0 && vid < WASM_MAX_LOCALS) {
                IrType t = inst->type;
                /* Allocas store the slot index as i32 ptr. */
                if (inst->op == IR_ALLOCA) t = IR_TYPE_PTR;
                fctx->value_type[vid] = t;
                fctx->value_used[vid] = 1;
                if (vid >= fctx->max_value) fctx->max_value = vid + 1;
            }
        }
    }
}

/* ============================================================
 * Emit local declarations for an IR function
 *
 * Each used SSA value id maps to a WASM local "$rN" of the appropriate
 * type. Function parameters are declared inline in the (param ...) list,
 * so we skip them here.
 *
 * We also reserve:
 *   $bb       i32   -- current basic-block index (for the dispatcher)
 *   $tmp_i32  i32   -- one general-purpose i32 scratch
 *   $tmp_i64  i64   -- one general-purpose i64 scratch
 * ============================================================ */

static int is_param_value(FnCtx *fctx, int vid) {
    int i;
    for (i = 0; i < fctx->fn->param_count; i++) {
        if (fctx->fn->param_value_ids[i] == vid) return 1;
    }
    return 0;
}

static void emit_locals(FnCtx *fctx) {
    FILE *out = fctx->gctx->out;
    int vid;

    fprintf(out, "    (local $bb i32)\n");
    fprintf(out, "    (local $tmp_i32 i32)\n");
    fprintf(out, "    (local $tmp_i64 i64)\n");

    for (vid = 0; vid < fctx->max_value; vid++) {
        if (!fctx->value_used[vid]) continue;
        if (is_param_value(fctx, vid)) continue;
        const char *tname = wasm_type_name(fctx->value_type[vid]);
        if (!tname || !*tname) tname = "i64"; /* safety */
        fprintf(out, "    (local $r%d %s)\n", vid, tname);
    }
}

/* ============================================================
 * Operand emission
 *
 * For an SSA value `vid`, push its value on the stack:
 *   - Function param: local.get $name_or_id (we use $rN convention always)
 *   - Else: local.get $rN
 * ============================================================ */

static void emit_get_value(FnCtx *fctx, int vid) {
    FILE *out = fctx->gctx->out;
    if (vid < 0) {
        /* Defensive: emit a zero. */
        fprintf(out, "      i64.const 0  ;; <invalid value id>\n");
        return;
    }
    /* Parameter: we declared params by name, but reference them by $rN
     * since ir_gen assigns SSA value ids 0..param_count-1 to params. */
    fprintf(out, "      local.get $r%d\n", vid);
}

/* Set the local slot for an SSA value from the top of stack. */
static void emit_set_value(FnCtx *fctx, int vid) {
    FILE *out = fctx->gctx->out;
    if (vid < 0) {
        fprintf(out, "      drop  ;; <invalid value id>\n");
        return;
    }
    fprintf(out, "      local.set $r%d\n", vid);
}

/* ============================================================
 * Phi handling
 *
 * For each terminator (BR/JMP) that targets a block containing PHI nodes,
 * we need to copy the appropriate incoming value into the phi's local
 * BEFORE jumping. We resolve this by scanning the target block for PHIs
 * and looking up the (value, predecessor) pair that matches the current
 * source block.
 * ============================================================ */

static IrBasicBlock *find_bb(IrFunction *fn, int id) {
    IrBasicBlock *bb;
    for (bb = fn->entry; bb; bb = bb->next) {
        if (bb->id == id) return bb;
    }
    return NULL;
}

static void emit_phi_assignments(FnCtx *fctx, int src_bb_id, int target_bb_id) {
    IrBasicBlock *target = find_bb(fctx->fn, target_bb_id);
    if (!target) return;
    IrInst *inst;
    for (inst = target->first; inst; inst = inst->next) {
        if (inst->op != IR_PHI) continue;
        /* Find the phi entry from src_bb_id. */
        int matched_value = -1;
        int i;
        for (i = 0; i < inst->phi_count; i++) {
            if (inst->phi_args[i].block == src_bb_id) {
                matched_value = inst->phi_args[i].value;
                break;
            }
        }
        if (matched_value < 0) continue;
        emit_get_value(fctx, matched_value);
        emit_set_value(fctx, inst->id);
    }
}

/* ============================================================
 * Instruction emission
 * ============================================================ */

static void emit_instruction(FnCtx *fctx, IrBasicBlock *bb, IrInst *inst) {
    FILE *out = fctx->gctx->out;
    if (!inst) return;

    switch (inst->op) {

    case IR_CONST_INT: {
        fprintf(out, "      i64.const %lld\n", (long long)inst->imm_int);
        emit_set_value(fctx, inst->id);
        break;
    }

    case IR_CONST_BOOL: {
        fprintf(out, "      i32.const %d\n", inst->imm_int ? 1 : 0);
        emit_set_value(fctx, inst->id);
        break;
    }

    case IR_CONST_FLOAT: {
        /* WAT supports fractional literals; print with enough precision. */
        fprintf(out, "      f64.const %.17g\n", inst->imm_float);
        emit_set_value(fctx, inst->id);
        break;
    }

    case IR_CONST_STRING: {
        int slen = 0;
        int off = intern_string(fctx->gctx, inst->imm_str, &slen);
        /* Push pointer (i32). The length is recoverable from the 4-byte
         * little-endian prefix at (off - 4). */
        fprintf(out, "      i32.const %d  ;; \"", off);
        emit_data_bytes(out, inst->imm_str ? inst->imm_str : "",
                        inst->imm_str ? (int)strlen(inst->imm_str) : 0);
        fprintf(out, "\" (len=%d)\n", slen);
        emit_set_value(fctx, inst->id);
        break;
    }

    case IR_ADD:
    case IR_SUB:
    case IR_MUL:
    case IR_DIV:
    case IR_MOD: {
        const char *op =
            inst->op == IR_ADD ? "i64.add" :
            inst->op == IR_SUB ? "i64.sub" :
            inst->op == IR_MUL ? "i64.mul" :
            inst->op == IR_DIV ? "i64.div_s" :
            "i64.rem_s";
        emit_get_value(fctx, inst->operands[0]);
        emit_get_value(fctx, inst->operands[1]);
        fprintf(out, "      %s\n", op);
        emit_set_value(fctx, inst->id);
        break;
    }

    case IR_FADD:
    case IR_FSUB:
    case IR_FMUL:
    case IR_FDIV: {
        const char *op =
            inst->op == IR_FADD ? "f64.add" :
            inst->op == IR_FSUB ? "f64.sub" :
            inst->op == IR_FMUL ? "f64.mul" :
            "f64.div";
        emit_get_value(fctx, inst->operands[0]);
        emit_get_value(fctx, inst->operands[1]);
        fprintf(out, "      %s\n", op);
        emit_set_value(fctx, inst->id);
        break;
    }

    case IR_NEG: {
        /* (i64.sub 0, x) */
        fprintf(out, "      i64.const 0\n");
        emit_get_value(fctx, inst->operands[0]);
        fprintf(out, "      i64.sub\n");
        emit_set_value(fctx, inst->id);
        break;
    }

    case IR_NOT: {
        /* Booleans are i32; use i32.eqz. For ints we conservatively
         * compare against 0 and set i32 result. */
        IrType src_t = (inst->operands[0] >= 0 &&
                        inst->operands[0] < WASM_MAX_LOCALS)
            ? fctx->value_type[inst->operands[0]] : IR_TYPE_BOOL;
        emit_get_value(fctx, inst->operands[0]);
        if (src_t == IR_TYPE_I64) {
            fprintf(out, "      i64.eqz\n");
        } else {
            fprintf(out, "      i32.eqz\n");
        }
        emit_set_value(fctx, inst->id);
        break;
    }

    case IR_CMP_EQ:
    case IR_CMP_NE:
    case IR_CMP_LT:
    case IR_CMP_GT:
    case IR_CMP_LE:
    case IR_CMP_GE: {
        /* Pick i32 ops if both operands are i32-typed, else i64. */
        IrType lhs_t = (inst->operands[0] >= 0 &&
                        inst->operands[0] < WASM_MAX_LOCALS)
            ? fctx->value_type[inst->operands[0]] : IR_TYPE_I64;
        int is_i32 = (lhs_t == IR_TYPE_BOOL ||
                      lhs_t == IR_TYPE_PTR ||
                      lhs_t == IR_TYPE_STRING);
        int is_f64 = (lhs_t == IR_TYPE_F64);
        const char *op;
        if (is_f64) {
            op =
                inst->op == IR_CMP_EQ ? "f64.eq" :
                inst->op == IR_CMP_NE ? "f64.ne" :
                inst->op == IR_CMP_LT ? "f64.lt" :
                inst->op == IR_CMP_GT ? "f64.gt" :
                inst->op == IR_CMP_LE ? "f64.le" :
                "f64.ge";
        } else if (is_i32) {
            op =
                inst->op == IR_CMP_EQ ? "i32.eq" :
                inst->op == IR_CMP_NE ? "i32.ne" :
                inst->op == IR_CMP_LT ? "i32.lt_s" :
                inst->op == IR_CMP_GT ? "i32.gt_s" :
                inst->op == IR_CMP_LE ? "i32.le_s" :
                "i32.ge_s";
        } else {
            op =
                inst->op == IR_CMP_EQ ? "i64.eq" :
                inst->op == IR_CMP_NE ? "i64.ne" :
                inst->op == IR_CMP_LT ? "i64.lt_s" :
                inst->op == IR_CMP_GT ? "i64.gt_s" :
                inst->op == IR_CMP_LE ? "i64.le_s" :
                "i64.ge_s";
        }
        emit_get_value(fctx, inst->operands[0]);
        emit_get_value(fctx, inst->operands[1]);
        fprintf(out, "      %s\n", op);
        emit_set_value(fctx, inst->id);
        break;
    }

    case IR_AND:
    case IR_OR: {
        /* These are always boolean (i32). */
        const char *op = (inst->op == IR_AND) ? "i32.and" : "i32.or";
        emit_get_value(fctx, inst->operands[0]);
        emit_get_value(fctx, inst->operands[1]);
        fprintf(out, "      %s\n", op);
        emit_set_value(fctx, inst->id);
        break;
    }

    case IR_ALLOCA: {
        /* Bump-allocate 8 bytes from the global $bump_ptr. */
        fctx->gctx->uses_alloca = 1;
        fprintf(out, "      global.get %s\n", WASM_BUMP_PTR_GLOBAL);
        fprintf(out, "      local.tee $r%d\n", inst->id);
        fprintf(out, "      i32.const 8\n");
        fprintf(out, "      i32.add\n");
        fprintf(out, "      global.set %s\n", WASM_BUMP_PTR_GLOBAL);
        break;
    }

    case IR_LOAD: {
        /* Load i64 from the address (an i32 ptr). */
        fctx->gctx->uses_i32_load = 1;
        emit_get_value(fctx, inst->operands[0]);
        if (inst->type == IR_TYPE_F64) {
            fprintf(out, "      f64.load\n");
        } else if (inst->type == IR_TYPE_BOOL ||
                   inst->type == IR_TYPE_PTR ||
                   inst->type == IR_TYPE_STRING) {
            fprintf(out, "      i32.load\n");
        } else {
            fprintf(out, "      i64.load\n");
        }
        emit_set_value(fctx, inst->id);
        break;
    }

    case IR_STORE: {
        /* operands[0] = value, operands[1] = addr (i32 ptr). */
        int val_id  = inst->operands[0];
        int addr_id = inst->operands[1];
        IrType vt = (val_id >= 0 && val_id < WASM_MAX_LOCALS)
            ? fctx->value_type[val_id] : IR_TYPE_I64;
        emit_get_value(fctx, addr_id);
        emit_get_value(fctx, val_id);
        if (vt == IR_TYPE_F64) {
            fprintf(out, "      f64.store\n");
        } else if (vt == IR_TYPE_BOOL ||
                   vt == IR_TYPE_PTR ||
                   vt == IR_TYPE_STRING) {
            fprintf(out, "      i32.store\n");
        } else {
            fprintf(out, "      i64.store\n");
        }
        break;
    }

    case IR_CALL: {
        /* Push args, then call $lcn_<name> if internal, or $<name>. */
        int i;
        for (i = 0; i < inst->call_arg_count; i++) {
            emit_get_value(fctx, inst->call_args[i]);
        }
        const char *fname = inst->fn_name ? inst->fn_name : "unknown";
        /* Internal IR functions all have name `lcn_<userName>` already.
         * If the IR call sites use the bare user name we prepend lcn_ to
         * match. We check for the prefix to be safe. */
        if (strncmp(fname, "lcn_", 4) == 0) {
            fprintf(out, "      call $%s\n", fname);
        } else {
            fprintf(out, "      call $lcn_%s\n", fname);
        }
        if (inst->id >= 0 && inst->type != IR_TYPE_VOID) {
            emit_set_value(fctx, inst->id);
        }
        break;
    }

    case IR_RET: {
        if (inst->operand_count > 0 && inst->operands[0] >= 0) {
            emit_get_value(fctx, inst->operands[0]);
        }
        fprintf(out, "      return\n");
        break;
    }

    case IR_BR: {
        /* Conditional: target_bb (true) / false_bb (false). */
        emit_get_value(fctx, inst->operands[0]);
        fprintf(out, "      if\n");
        emit_phi_assignments(fctx, bb->id, inst->target_bb);
        fprintf(out, "        i32.const %d\n", inst->target_bb);
        fprintf(out, "        local.set $bb\n");
        fprintf(out, "      else\n");
        emit_phi_assignments(fctx, bb->id, inst->false_bb);
        fprintf(out, "        i32.const %d\n", inst->false_bb);
        fprintf(out, "        local.set $bb\n");
        fprintf(out, "      end\n");
        fprintf(out, "      br $dispatch\n");
        break;
    }

    case IR_JMP: {
        emit_phi_assignments(fctx, bb->id, inst->target_bb);
        fprintf(out, "      i32.const %d\n", inst->target_bb);
        fprintf(out, "      local.set $bb\n");
        fprintf(out, "      br $dispatch\n");
        break;
    }

    case IR_PHI: {
        /* Phi nodes are resolved at the predecessor side via
         * emit_phi_assignments. Nothing to do here. */
        fprintf(out, "      ;; phi $r%d (resolved at predecessors)\n",
                inst->id);
        break;
    }

    case IR_PRINT: {
        /* No native runtime in PoC. Stub as comment + drop the value
         * cleanly (don't push it -- nothing to drop). */
        fprintf(out, "      ;; UNSUPPORTED IR_PRINT (stubbed)\n");
        break;
    }

    case IR_STR_CONCAT: {
        /* No runtime helper available in WASM PoC -- return lhs verbatim. */
        fprintf(out, "      ;; UNSUPPORTED IR_STR_CONCAT (returning lhs)\n");
        emit_get_value(fctx, inst->operands[0]);
        emit_set_value(fctx, inst->id);
        break;
    }

    case IR_CAST: {
        /* Cast: handle i64<->i32 reasonably; otherwise pass through. */
        IrType src_t = (inst->operands[0] >= 0 &&
                        inst->operands[0] < WASM_MAX_LOCALS)
            ? fctx->value_type[inst->operands[0]] : IR_TYPE_I64;
        IrType dst_t = inst->type;
        emit_get_value(fctx, inst->operands[0]);
        const char *dst_w = wasm_type_name(dst_t);
        const char *src_w = wasm_type_name(src_t);
        if (strcmp(src_w, "i64") == 0 && strcmp(dst_w, "i32") == 0) {
            fprintf(out, "      i32.wrap_i64\n");
        } else if (strcmp(src_w, "i32") == 0 && strcmp(dst_w, "i64") == 0) {
            fprintf(out, "      i64.extend_i32_s\n");
        } else if (strcmp(src_w, "i64") == 0 && strcmp(dst_w, "f64") == 0) {
            fprintf(out, "      f64.convert_i64_s\n");
        } else if (strcmp(src_w, "f64") == 0 && strcmp(dst_w, "i64") == 0) {
            fprintf(out, "      i64.trunc_f64_s\n");
        }
        /* else: same-shape cast, no instruction needed */
        emit_set_value(fctx, inst->id);
        break;
    }

    case IR_GEP: {
        /* base + index*8 in i32 land. */
        emit_get_value(fctx, inst->operands[0]);
        emit_get_value(fctx, inst->operands[1]);
        /* index is likely i64; truncate to i32 then multiply. */
        IrType idx_t = (inst->operands[1] >= 0 &&
                        inst->operands[1] < WASM_MAX_LOCALS)
            ? fctx->value_type[inst->operands[1]] : IR_TYPE_I64;
        if (idx_t == IR_TYPE_I64) {
            fprintf(out, "      i32.wrap_i64\n");
        }
        fprintf(out, "      i32.const 8\n");
        fprintf(out, "      i32.mul\n");
        fprintf(out, "      i32.add\n");
        emit_set_value(fctx, inst->id);
        break;
    }

    case IR_NOP: {
        fprintf(out, "      nop\n");
        break;
    }

    default: {
        fprintf(out, "      ;; UNSUPPORTED %s (op=%d)\n",
                ir_opcode_name(inst->op), (int)inst->op);
        fprintf(stderr,
                "  WASM emit: warning: opcode %s not yet implemented in fn %s\n",
                ir_opcode_name(inst->op),
                fctx->fn->name ? fctx->fn->name : "?");
        break;
    }
    }
}

/* ============================================================
 * Function body emission
 *
 * We use the dispatch-loop CFG idiom. After emitting all blocks the
 * function falls off the end; we add a default `return` of a zero
 * value of the appropriate type as a safety net.
 * ============================================================ */

static int last_inst_terminates(IrBasicBlock *bb) {
    IrInst *last = bb->last;
    if (!last) return 0;
    return last->op == IR_RET ||
           last->op == IR_BR  ||
           last->op == IR_JMP;
}

static void emit_function_body(FnCtx *fctx) {
    FILE *out = fctx->gctx->out;
    int n = fctx->bb_count;
    int i;

    /* Initialize $bb to entry (which is fn->entry->id, normally 0). */
    int entry_id = fctx->fn->entry ? fctx->fn->entry->id : 0;
    fprintf(out, "    i32.const %d\n", entry_id);
    fprintf(out, "    local.set $bb\n");

    /* Open the dispatch outer block + loop. */
    fprintf(out, "    (block $exit\n");
    fprintf(out, "    (loop $dispatch\n");

    /* Open one nested (block $bb_i) per basic block, with $bb_0 INNERMOST.
     * `br $bb_0` then lands just past the innermost block, which is where
     * we emit the bb0 body (the FIRST close). $bb_{n-1} is the OUTERMOST
     * block: `br $bb_{n-1}` lands past it (the LAST close), where bb{n-1}
     * body is emitted. */
    for (i = n - 1; i >= 0; i--) {
        fprintf(out, "    (block $bb_%d\n", i);
    }
    /* Innermost: br_table dispatch. */
    fprintf(out, "      local.get $bb\n");
    fprintf(out, "      br_table");
    for (i = 0; i < n; i++) {
        fprintf(out, " $bb_%d", i);
    }
    fprintf(out, " $exit\n");

    /* Close each inner block and emit that block's body in order. */
    /* The innermost (block $bb_0) closes first: close it, then bb_0 body
     * appears outside it (because br $bb_0 jumps past the block end). */
    /* Iterate i from 0..n-1: close (block $bb_i) and emit its body. */
    for (i = 0; i < n; i++) {
        fprintf(out, "    )  ;; close $bb_%d, fall into bb%d body\n", i, i);
        IrBasicBlock *bb = find_bb(fctx->fn, i);
        if (bb) {
            fprintf(out, "    ;; ===== bb%d", bb->id);
            if (bb->label) fprintf(out, " (%s)", bb->label);
            fprintf(out, " =====\n");
            IrInst *inst;
            for (inst = bb->first; inst; inst = inst->next) {
                emit_instruction(fctx, bb, inst);
            }
            /* If the block has no terminator, fall through to the next
             * block (i.e., implicit jmp to bb i+1). The natural code
             * order makes that work: we just keep going. But to be
             * safe, if it's the last block and didn't terminate, we
             * fall through to the loop end => $dispatch repeats. */
            if (!last_inst_terminates(bb)) {
                /* Continue to next block in declaration order if any. */
                if (i + 1 < n) {
                    fprintf(out, "      i32.const %d\n", i + 1);
                    fprintf(out, "      local.set $bb\n");
                    fprintf(out, "      br $dispatch\n");
                } else {
                    /* No more blocks; go to exit. */
                    fprintf(out, "      br $exit\n");
                }
            }
        } else {
            /* No such block: just exit to be safe. */
            fprintf(out, "      br $exit\n");
        }
    }

    /* Close (loop $dispatch) and (block $exit). */
    fprintf(out, "    )  ;; close loop $dispatch\n");
    fprintf(out, "    )  ;; close block $exit\n");

    /* Default return value. */
    if (fctx->fn->return_type != IR_TYPE_VOID) {
        switch (fctx->fn->return_type) {
        case IR_TYPE_I64:    fprintf(out, "    i64.const 0\n"); break;
        case IR_TYPE_F64:    fprintf(out, "    f64.const 0\n"); break;
        case IR_TYPE_BOOL:
        case IR_TYPE_PTR:
        case IR_TYPE_STRING:
        case IR_TYPE_STRUCT: fprintf(out, "    i32.const 0\n"); break;
        default:             fprintf(out, "    i64.const 0\n"); break;
        }
    }
}

/* ============================================================
 * Function signature
 * ============================================================ */

static void emit_function_signature(FnCtx *fctx) {
    FILE *out = fctx->gctx->out;
    IrFunction *fn = fctx->fn;

    fprintf(out, "  (func $%s", fn->name ? fn->name : "anon");

    /* Parameters. */
    int i;
    for (i = 0; i < fn->param_count; i++) {
        const char *t = wasm_type_name(fn->param_types[i]);
        if (!t || !*t) t = "i64";
        int vid = fn->param_value_ids[i];
        if (vid >= 0) {
            fprintf(out, " (param $r%d %s)", vid, t);
        } else {
            fprintf(out, " (param %s)", t);
        }
    }

    /* Result. */
    if (fn->return_type != IR_TYPE_VOID) {
        const char *rt = wasm_type_name(fn->return_type);
        if (rt && *rt) {
            fprintf(out, " (result %s)", rt);
        }
    }

    fprintf(out, "\n");
}

/* ============================================================
 * Module emission
 * ============================================================ */

static void emit_function(EmitCtx *ctx, IrFunction *fn) {
    FnCtx fctx;
    memset(&fctx, 0, sizeof(fctx));
    fctx.gctx = ctx;
    fctx.fn   = fn;

    scan_function_values(&fctx);

    emit_function_signature(&fctx);
    emit_locals(&fctx);
    emit_function_body(&fctx);
    fprintf(ctx->out, "  )\n\n");
}

/* Emit the (data ...) segment for all interned strings.
 * Layout: each string's 4-byte LE length prefix sits 4 bytes BEFORE its
 *         pointer. We emit one continuous chunk starting at WASM_DATA_BASE.
 *
 * We re-walk the string table in offset order and emit prefix+bytes+pad
 * in sequence. */
static void emit_data_segment(EmitCtx *ctx) {
    FILE *out = ctx->out;
    if (ctx->string_count == 0) return;

    /* Sort strings by offset (insertion sort, n is small). */
    int n = ctx->string_count;
    int order[WASM_MAX_STRINGS];
    int i, j;
    for (i = 0; i < n; i++) order[i] = i;
    for (i = 1; i < n; i++) {
        int key = order[i];
        j = i - 1;
        while (j >= 0 && ctx->strings[order[j]].offset > ctx->strings[key].offset) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = key;
    }

    fprintf(out, "  ;; --- string data (%d entries) ---\n", n);
    fprintf(out, "  (data (i32.const %d)\n", WASM_DATA_BASE);

    /* Walk in offset order: each entry's prefix is at offset-4 (which
     * equals the previous entry's pad-aligned end). */
    int cursor = WASM_DATA_BASE;
    for (i = 0; i < n; i++) {
        WasmString *s = &ctx->strings[order[i]];
        int prefix_off = s->offset - 4;
        int bytes_off  = s->offset;

        /* Pad with zeros from cursor up to prefix_off. */
        if (prefix_off > cursor) {
            fprintf(out, "    \"");
            int pad;
            for (pad = 0; pad < (prefix_off - cursor); pad++) {
                fprintf(out, "\\00");
            }
            fprintf(out, "\"\n");
        }
        cursor = prefix_off;

        /* Emit length prefix. */
        fprintf(out, "    \"");
        emit_data_u32_le(out, (uint32_t)s->length);
        fprintf(out, "\"\n");
        cursor += 4;

        /* Emit bytes. */
        fprintf(out, "    \"");
        emit_data_bytes(out, s->value ? s->value : "", s->length);
        fprintf(out, "\"\n");
        cursor = bytes_off + s->length;
    }
    fprintf(out, "  )\n");
}

/* Pre-pass: walk every IR_CONST_STRING in the module to populate the
 * string table BEFORE we emit function bodies. This way we know all
 * offsets at body-emit time. */
static void preintern_strings(EmitCtx *ctx) {
    IrFunction *fn;
    for (fn = ctx->module->functions; fn; fn = fn->next) {
        IrBasicBlock *bb;
        for (bb = fn->entry; bb; bb = bb->next) {
            IrInst *inst;
            for (inst = bb->first; inst; inst = inst->next) {
                if (inst->op == IR_CONST_STRING) {
                    intern_string(ctx, inst->imm_str, NULL);
                }
            }
        }
    }
}

/* Pre-pass: detect alloca usage, so we know whether to emit the bump
 * pointer global. */
static void scan_module_features(EmitCtx *ctx) {
    IrFunction *fn;
    for (fn = ctx->module->functions; fn; fn = fn->next) {
        IrBasicBlock *bb;
        for (bb = fn->entry; bb; bb = bb->next) {
            IrInst *inst;
            for (inst = bb->first; inst; inst = inst->next) {
                if (inst->op == IR_ALLOCA) {
                    ctx->uses_alloca = 1;
                }
            }
        }
    }
}

/* Strip the "lcn_" prefix used by ir_gen and return the user-visible
 * function name. Returns the input string if no prefix matches.
 * No allocation -- the result aliases `name`. */
static const char *user_name_for(const char *name) {
    if (!name) return "";
    if (strncmp(name, "lcn_", 4) == 0) return name + 4;
    return name;
}

static void emit_module(FILE *out, IrModule *mod, EmitCtx *ctx) {
    fprintf(out, ";; Limceron WebAssembly module\n");
    fprintf(out, ";; Generated by limceron-stage0 WASM backend\n");
    fprintf(out, ";; Functions: %d\n", mod->fn_count);
    fprintf(out, "(module\n");

    /* Linear memory: one 64 KiB page, exported. */
    fprintf(out, "  (memory (export \"memory\") 1)\n");

    /* Bump-pointer global for ALLOCA, if any function uses it. We
     * declare it unconditionally -- it's a tiny overhead and keeps
     * the WAT shape stable across modules. The initial value points
     * just past the static-data area we reserve. */
    int bump_init = ctx->data_offset > 0 ? ctx->data_offset : WASM_DATA_BASE;
    /* Round up to 16-byte alignment for tidy heap base. */
    bump_init = (bump_init + 15) & ~15;
    fprintf(out, "  (global %s (mut i32) (i32.const %d))\n",
            WASM_BUMP_PTR_GLOBAL, bump_init);

    /* String data segment. */
    emit_data_segment(ctx);

    fprintf(out, "\n");

    /* Emit all functions. */
    IrFunction *fn;
    for (fn = mod->functions; fn; fn = fn->next) {
        emit_function(ctx, fn);
    }

    /* Exports.
     *
     * We export every IR function as both `lcn_<name>` (raw IR symbol)
     * and `<name>` (user-visible name) so that `wasmtime --invoke main`
     * works without requiring callers to know the lcn_ prefix.
     *
     * Special handling for the entrypoint: if there's a `lcn_main`,
     * also export it as `_start` (the standard WASI entry symbol)
     * with a wrapper that ignores the i64 return value, so a generic
     * WASI host can run the module. We keep that wrapper minimal.
     */
    int has_main = 0;
    for (fn = mod->functions; fn; fn = fn->next) {
        if (!fn->name) continue;
        const char *uname = user_name_for(fn->name);
        fprintf(out, "  (export \"%s\" (func $%s))\n", uname, fn->name);
        /* Also export the raw lcn_-prefixed symbol for stable ABI. */
        if (strcmp(uname, fn->name) != 0) {
            fprintf(out, "  (export \"%s\" (func $%s))\n",
                    fn->name, fn->name);
        }
        if (strcmp(uname, "main") == 0) has_main = 1;
    }

    /* Optional WASI _start wrapper. We only emit it if `main` exists
     * and does NOT already collide with WASI's expected void()->void
     * signature. For PoC, our `main` returns i64; we wrap by calling
     * it and dropping the result. */
    if (has_main) {
        fprintf(out, "  (func $_start\n");
        fprintf(out, "    call $lcn_main\n");
        /* If lcn_main produced a value, drop it. We always emit drop
         * for non-void main. */
        IrFunction *m;
        for (m = mod->functions; m; m = m->next) {
            if (m->name && strcmp(m->name, "lcn_main") == 0) {
                if (m->return_type != IR_TYPE_VOID) {
                    fprintf(out, "    drop\n");
                }
                break;
            }
        }
        fprintf(out, "  )\n");
        fprintf(out, "  (export \"_start\" (func $_start))\n");
    }

    fprintf(out, ")\n");
}

/* ============================================================
 * Driver: wat2wasm
 * ============================================================ */

static int run_wat2wasm(const char *wat_path, const char *wasm_path) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "wat2wasm --debug-names -o %s %s 2>&1",
             wasm_path, wat_path);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "  WASM emit: wat2wasm failed (exit %d)\n", rc);
        fprintf(stderr,
                "  Hint: brew install wabt   (or: cargo install wasm-tools)\n");
        return 1;
    }
    return 0;
}

/* ============================================================
 * Public entry: lcn_emit_wasm
 * ============================================================ */

int lcn_emit_wasm(AstNode *program, const char *input,
                  const char *output, Arena *arena,
                  const LcnTarget *target) {
    (void)input;

    if (!program || !output || !arena) {
        fprintf(stderr, "  WASM emit: null program/output/arena\n");
        return 1;
    }

    /* 1. Lower AST -> SSA IR module. */
    IrModule *mod = ir_gen_program(program, arena);
    if (!mod) {
        fprintf(stderr, "  WASM emit: ir_gen_program returned NULL\n");
        return 1;
    }

    /* 2. Build the emit context. */
    EmitCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.module      = mod;
    ctx.arena       = arena;
    ctx.target      = target;
    ctx.data_offset = WASM_DATA_BASE;

    /* Pre-passes. */
    scan_module_features(&ctx);
    preintern_strings(&ctx);
    ctx.data_total = ctx.data_offset;

    /* 3. Open temp .wat file. */
    char wat_path[512];
    snprintf(wat_path, sizeof(wat_path), "/tmp/lcn_%d.wat", (int)getpid());
    FILE *out = fopen(wat_path, "w");
    if (!out) {
        fprintf(stderr, "  WASM emit: cannot open %s for writing\n", wat_path);
        return 1;
    }
    ctx.out = out;

    /* 4. Emit the module. */
    emit_module(out, mod, &ctx);
    fclose(out);

    fprintf(stderr, "  WASM emit: %s (%d fn(s), %d string(s), %d data bytes)\n",
            wat_path, mod->fn_count, ctx.string_count,
            ctx.data_total - WASM_DATA_BASE);

    /* 5. Run wat2wasm. */
    int rc = run_wat2wasm(wat_path, output);
    if (rc != 0) {
        fprintf(stderr, "  WASM emit: see %s for the failing WAT\n", wat_path);
        return 1;
    }

    /* 6. Report final size. */
    {
        FILE *fp = fopen(output, "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long sz = ftell(fp);
            fclose(fp);
            fprintf(stderr, "  WASM emit: %s (%ld bytes)\n", output, sz);
        }
    }

    /* Sibling .wit emit. Replace the .wasm extension; if there is none,
     * append .wit so we always produce an artifact. WIT is auxiliary
     * metadata: a missing-agents source shouldn't fail the wasm build. */
    {
        char wit_path[1024];
        size_t n = strlen(output);
        const char *dot = strrchr(output, '.');
        if (dot && strcmp(dot, ".wasm") == 0) {
            size_t prefix = (size_t)(dot - output);
            if (prefix + 5 < sizeof(wit_path)) {
                memcpy(wit_path, output, prefix);
                memcpy(wit_path + prefix, ".wit", 5);
                (void)lcn_emit_wit(program, wit_path);
            }
        } else if (n + 5 < sizeof(wit_path)) {
            memcpy(wit_path, output, n);
            memcpy(wit_path + n, ".wit", 5);
            (void)lcn_emit_wit(program, wit_path);
        }
    }

    /* Leave the .wat file for inspection; do not unlink. */
    return 0;
}
