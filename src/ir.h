/*
 * Limceron Compiler — SSA Intermediate Representation
 *
 * Native SSA IR for the Limceron compiler. This is an additional backend
 * alongside the existing C99 transpiler. The IR uses Static Single Assignment
 * form with basic blocks, phi nodes, and typed values.
 *
 * IR textual format:
 *   define i64 @lcn_add(i64 %a, i64 %b) {
 *   bb0:
 *       %0 = add i64 %a, %b
 *       ret i64 %0
 *   }
 */

#ifndef LCN_IR_H
#define LCN_IR_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

/* Forward declarations — avoid redefinition when lcn.h is already included */
#ifndef LCN_H
typedef struct AstNode AstNode;
struct Arena;
typedef struct Arena Arena;
#endif

/* ============================================================
 * IR Opcodes
 * ============================================================ */

typedef enum {
    /* Constants */
    IR_CONST_INT,       /* %0 = const i64 42              */
    IR_CONST_FLOAT,     /* %0 = const f64 3.14            */
    IR_CONST_STRING,    /* %0 = const str "hello"         */
    IR_CONST_BOOL,      /* %0 = const bool true           */

    /* Integer arithmetic */
    IR_ADD,             /* %2 = add %0, %1                */
    IR_SUB,             /* %2 = sub %0, %1                */
    IR_MUL,             /* %2 = mul %0, %1                */
    IR_DIV,             /* %2 = div %0, %1                */
    IR_MOD,             /* %2 = mod %0, %1                */

    /* Float arithmetic */
    IR_FADD,            /* %2 = fadd %0, %1               */
    IR_FSUB,            /* %2 = fsub %0, %1               */
    IR_FMUL,            /* %2 = fmul %0, %1               */
    IR_FDIV,            /* %2 = fdiv %0, %1               */

    /* Unary */
    IR_NEG,             /* %1 = neg %0                    */
    IR_NOT,             /* %1 = not %0                    */

    /* Comparisons */
    IR_CMP_EQ,          /* %2 = cmp_eq %0, %1             */
    IR_CMP_NE,          /* %2 = cmp_ne %0, %1             */
    IR_CMP_LT,          /* %2 = cmp_lt %0, %1             */
    IR_CMP_GT,          /* %2 = cmp_gt %0, %1             */
    IR_CMP_LE,          /* %2 = cmp_le %0, %1             */
    IR_CMP_GE,          /* %2 = cmp_ge %0, %1             */

    /* Logical */
    IR_AND,             /* %2 = and %0, %1                */
    IR_OR,              /* %2 = or %0, %1                 */

    /* Memory */
    IR_ALLOCA,          /* %0 = alloca i64                */
    IR_LOAD,            /* %1 = load %0                   */
    IR_STORE,           /* store %value, %addr            */

    /* Control flow */
    IR_CALL,            /* %3 = call @fn(%0, %1)          */
    IR_RET,             /* ret %0                         */
    IR_BR,              /* br %cond, @true, @false        */
    IR_JMP,             /* jmp @target                    */
    IR_PHI,             /* %0 = phi [%a, @bb1], [%b, @bb2] */

    /* String operations */
    IR_STR_CONCAT,      /* %2 = str_concat %0, %1         */

    /* Builtins */
    IR_PRINT,           /* print %0                       */

    /* Type operations */
    IR_CAST,            /* %1 = cast %0 to i64            */
    IR_GEP,             /* %1 = gep %struct, field_idx    */

    /* No-op */
    IR_NOP,

    IR_OPCODE_COUNT
} IrOpcode;

/* ============================================================
 * IR Types
 * ============================================================ */

typedef enum {
    IR_TYPE_I64,
    IR_TYPE_F64,
    IR_TYPE_BOOL,
    IR_TYPE_STRING,
    IR_TYPE_VOID,
    IR_TYPE_PTR,
    IR_TYPE_STRUCT
} IrType;

/* ============================================================
 * IR Instruction
 * ============================================================ */

#define IR_MAX_OPERANDS  4
#define IR_MAX_PHI_ARGS  8

typedef struct IrInst {
    int             id;             /* SSA value number (%0, %1, ...) */
    IrOpcode        op;
    IrType          type;

    /* Operands: SSA value references (-1 = unused) */
    int             operands[IR_MAX_OPERANDS];
    int             operand_count;

    /* Immediate values (for constants) */
    int64_t         imm_int;
    double          imm_float;
    char           *imm_str;

    /* For call instructions */
    char           *fn_name;
    int             call_args[16];
    int             call_arg_count;

    /* For branch/jump targets (basic block IDs) */
    int             target_bb;
    int             false_bb;

    /* For phi nodes: incoming [value, block] pairs */
    struct {
        int         value;
        int         block;
    } phi_args[IR_MAX_PHI_ARGS];
    int             phi_count;

    /* Linked list within a basic block */
    struct IrInst  *next;
} IrInst;

/* ============================================================
 * Basic Block
 * ============================================================ */

typedef struct IrBasicBlock {
    int             id;             /* Block number (@bb0, @bb1, ...) */
    char           *label;          /* Optional label */

    IrInst         *first;
    IrInst         *last;
    int             inst_count;

    struct IrBasicBlock *next;      /* Next block in function */
} IrBasicBlock;

/* ============================================================
 * Function
 * ============================================================ */

#define IR_MAX_PARAMS    16

typedef struct IrFunction {
    char           *name;
    IrType          return_type;

    int             param_count;
    IrType          param_types[IR_MAX_PARAMS];
    char           *param_names[IR_MAX_PARAMS];
    int             param_value_ids[IR_MAX_PARAMS]; /* SSA value IDs for params */

    IrBasicBlock   *entry;          /* First basic block */
    IrBasicBlock   *current_bb;     /* Block being built */
    int             bb_count;

    int             next_value;     /* SSA value counter */
    int             next_bb;        /* Block counter */

    struct IrFunction *next;        /* Next function in module */
} IrFunction;

/* ============================================================
 * Module (top-level IR unit)
 * ============================================================ */

typedef struct {
    IrFunction     *functions;
    int             fn_count;
    Arena          *arena;          /* Memory arena for allocations */
} IrModule;

/* ============================================================
 * IR Construction API
 * ============================================================ */

/* Create a new IR module */
IrModule       *ir_module_new(Arena *arena);

/* Create a new function and add it to the module */
IrFunction     *ir_function_new(IrModule *mod, const char *name, IrType ret_type);

/* Add a parameter to a function (before adding blocks) */
int             ir_function_add_param(IrFunction *fn, IrModule *mod,
                                      const char *name, IrType type);

/* Create a new basic block and add it to the function */
IrBasicBlock   *ir_bb_new(IrFunction *fn, IrModule *mod, const char *label);

/* Set the current basic block for instruction emission */
void            ir_set_current_bb(IrFunction *fn, IrBasicBlock *bb);

/* Emit instructions into the current basic block.
 * Returns the SSA value ID of the result (-1 for void ops like store/ret). */
int  ir_emit_const_int(IrFunction *fn, IrModule *mod, int64_t val);
int  ir_emit_const_float(IrFunction *fn, IrModule *mod, double val);
int  ir_emit_const_string(IrFunction *fn, IrModule *mod, const char *val);
int  ir_emit_const_bool(IrFunction *fn, IrModule *mod, bool val);

int  ir_emit_binop(IrFunction *fn, IrModule *mod, IrOpcode op, IrType type,
                   int lhs, int rhs);
int  ir_emit_unary(IrFunction *fn, IrModule *mod, IrOpcode op, IrType type, int operand);

int  ir_emit_alloca(IrFunction *fn, IrModule *mod, IrType type);
int  ir_emit_load(IrFunction *fn, IrModule *mod, IrType type, int addr);
void ir_emit_store(IrFunction *fn, IrModule *mod, int value, int addr);

int  ir_emit_call(IrFunction *fn, IrModule *mod, const char *callee,
                  IrType ret_type, int *args, int arg_count);
void ir_emit_ret(IrFunction *fn, IrModule *mod, int value);
void ir_emit_ret_void(IrFunction *fn, IrModule *mod);
void ir_emit_br(IrFunction *fn, IrModule *mod, int cond, int true_bb, int false_bb);
void ir_emit_jmp(IrFunction *fn, IrModule *mod, int target_bb);
int  ir_emit_phi(IrFunction *fn, IrModule *mod, IrType type);
void ir_phi_add_incoming(IrInst *phi, int value, int block);

int  ir_emit_str_concat(IrFunction *fn, IrModule *mod, int lhs, int rhs);
void ir_emit_print(IrFunction *fn, IrModule *mod, int value);
int  ir_emit_cast(IrFunction *fn, IrModule *mod, IrType target, int value);
int  ir_emit_gep(IrFunction *fn, IrModule *mod, IrType type, int base, int index);

/* ============================================================
 * IR Generation from AST
 * ============================================================ */

IrModule       *ir_gen_program(AstNode *program, Arena *arena);

/* ============================================================
 * IR Printer
 * ============================================================ */

/* Print the entire module in textual IR format */
void            ir_print_module(IrModule *mod, FILE *out);

/* Print a single function */
void            ir_print_function(IrFunction *fn, FILE *out);

/* Return the name of an IR type */
const char     *ir_type_name(IrType type);

/* Return the name of an IR opcode */
const char     *ir_opcode_name(IrOpcode op);

/* ============================================================
 * Optimization Passes
 * ============================================================ */

/* Constant folding: evaluate constant expressions at compile time.
 * Returns the number of instructions folded. */
int             ir_opt_constant_fold(IrModule *mod);

/* Dead code elimination: remove instructions whose results are unused.
 * Returns the number of instructions eliminated. */
int             ir_opt_dead_code_elim(IrModule *mod);

/* Constant propagation: replace uses of known constants.
 * Returns the number of propagations performed. */
int             ir_opt_constant_prop(IrModule *mod);

/* Run all optimization passes (fold + propagate + DCE). */
void            ir_opt_all(IrModule *mod);

/* ============================================================
 * Register Allocator — AArch64 (shared interface)
 * ============================================================ */

typedef struct {
    int value_id;
    int reg;            /* architecture-agnostic register number, -1 if spilled */
    int spill_offset;   /* offset from FP, -1 if not spilled */
    int first_use;      /* instruction index of first definition */
    int last_use;       /* instruction index of last use */
} LiveRange;

/* Compute live ranges for all SSA values in a function.
 * Fills `ranges` array up to `max_ranges` entries.
 * Sets *count to the number of ranges written. */
void ir_compute_live_ranges(IrFunction *fn, LiveRange *ranges,
                            int *count, int max_ranges);

/* Run linear-scan register allocation over the given live ranges.
 * `num_regs` is the number of available temporary registers. */
void ir_alloc_registers(LiveRange *ranges, int count, int num_regs);

/* ============================================================
 * AArch64 (ARM64) Assembly Emitter
 * ============================================================ */

/* Emit AArch64 assembly for the entire module to the given FILE. */
void ir_emit_arm64(IrModule *mod, FILE *out);

/* ============================================================
 * Register Allocator — x86_64
 * ============================================================ */

typedef enum {
    REG_RAX, REG_RCX, REG_RDX, REG_RSI, REG_RDI,
    REG_R8, REG_R9, REG_R10, REG_R11,
    /* Callee-saved registers */
    REG_RBX, REG_R12, REG_R13, REG_R14, REG_R15,
    REG_NONE,   /* Unassigned */
    REG_SPILL   /* Spilled to stack */
} X86Reg;

#define X86_NUM_REGS 14  /* RAX..R15 (allocatable) */

typedef struct {
    int         value_id;       /* SSA value */
    X86Reg      reg;            /* Assigned register, or REG_SPILL */
    int         spill_offset;   /* Stack offset if spilled */
    int         start;          /* Live range start (instruction index) */
    int         end;            /* Live range end (instruction index) */
} RegAlloc;

/* Compute live ranges for each SSA value in a function (x86_64).
 * Fills allocs[] and sets *count. allocs must be large enough. */
void ir_x86_compute_live_ranges(IrFunction *fn, RegAlloc *allocs, int *count);

/* Assign registers using linear scan algorithm (x86_64). */
void ir_linear_scan_alloc(RegAlloc *allocs, int count);

/* Return the AT&T syntax name for a register (e.g., "%rax"). */
const char *x86_reg_name(X86Reg reg);

/* ============================================================
 * x86_64 Assembly Emitter
 * ============================================================ */

/* Emit the entire module as AT&T syntax x86_64 assembly. */
void ir_emit_x86(IrModule *mod, FILE *out);

/* Emit a single function as x86_64 assembly. */
void ir_emit_x86_function(IrFunction *fn, IrModule *mod, FILE *out);

/* ============================================================
 * Native Compilation Pipeline
 * ============================================================ */

/* Detect the host architecture. Returns "arm64" or "x86_64". */
const char *ir_detect_arch(void);

/* Compile an IR module to a native binary.
 *   mod       - optimized IR module
 *   output    - path for the output binary (or .s for emit-asm)
 *   emit_asm  - if true, only emit assembly (don't assemble/link)
 * Returns 0 on success, non-zero on failure. */
int ir_compile_native(IrModule *mod, const char *output, bool emit_asm);

#endif /* LCN_IR_H */
