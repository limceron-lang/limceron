/*
 * Limceron Compiler — SSA IR Generator & Printer & Optimizer
 *
 * Walks the AST and emits SSA IR instructions organized into
 * functions and basic blocks. This is an ADDITIONAL backend
 * alongside the existing C99 transpiler.
 *
 * Architecture:
 *   1. IR Construction API — low-level instruction emission
 *   2. IR Generation — AST walker that produces IR
 *   3. IR Printer — textual IR format output
 *   4. Optimization Passes — constant folding, DCE, propagation
 */

#include "lcn.h"
#include "ir.h"
#include <string.h>
#include <stdlib.h>

/* ============================================================
 * IR Type & Opcode Names
 * ============================================================ */

const char *ir_type_name(IrType type) {
    switch (type) {
    case IR_TYPE_I64:    return "i64";
    case IR_TYPE_F64:    return "f64";
    case IR_TYPE_BOOL:   return "bool";
    case IR_TYPE_STRING: return "str";
    case IR_TYPE_VOID:   return "void";
    case IR_TYPE_PTR:    return "ptr";
    case IR_TYPE_STRUCT: return "struct";
    }
    return "???";
}

const char *ir_opcode_name(IrOpcode op) {
    switch (op) {
    case IR_CONST_INT:    return "const";
    case IR_CONST_FLOAT:  return "const";
    case IR_CONST_STRING: return "const";
    case IR_CONST_BOOL:   return "const";
    case IR_ADD:          return "add";
    case IR_SUB:          return "sub";
    case IR_MUL:          return "mul";
    case IR_DIV:          return "div";
    case IR_MOD:          return "mod";
    case IR_FADD:         return "fadd";
    case IR_FSUB:         return "fsub";
    case IR_FMUL:         return "fmul";
    case IR_FDIV:         return "fdiv";
    case IR_NEG:          return "neg";
    case IR_NOT:          return "not";
    case IR_CMP_EQ:       return "cmp_eq";
    case IR_CMP_NE:       return "cmp_ne";
    case IR_CMP_LT:       return "cmp_lt";
    case IR_CMP_GT:       return "cmp_gt";
    case IR_CMP_LE:       return "cmp_le";
    case IR_CMP_GE:       return "cmp_ge";
    case IR_AND:          return "and";
    case IR_OR:           return "or";
    case IR_ALLOCA:       return "alloca";
    case IR_LOAD:         return "load";
    case IR_STORE:        return "store";
    case IR_CALL:         return "call";
    case IR_RET:          return "ret";
    case IR_BR:           return "br";
    case IR_JMP:          return "jmp";
    case IR_PHI:          return "phi";
    case IR_STR_CONCAT:   return "str_concat";
    case IR_PRINT:        return "print";
    case IR_CAST:         return "cast";
    case IR_GEP:          return "gep";
    case IR_NOP:          return "nop";
    default:              return "???";
    }
}

/* ============================================================
 * IR Construction API
 * ============================================================ */

IrModule *ir_module_new(Arena *arena) {
    IrModule *mod = (IrModule *)arena_alloc(arena, sizeof(IrModule));
    mod->functions = NULL;
    mod->fn_count = 0;
    mod->arena = arena;
    return mod;
}

IrFunction *ir_function_new(IrModule *mod, const char *name, IrType ret_type) {
    IrFunction *fn = (IrFunction *)arena_alloc(mod->arena, sizeof(IrFunction));
    memset(fn, 0, sizeof(IrFunction));
    fn->name = arena_strdup(mod->arena, name);
    fn->return_type = ret_type;
    fn->next_value = 0;
    fn->next_bb = 0;

    /* Append to module */
    if (!mod->functions) {
        mod->functions = fn;
    } else {
        IrFunction *tail = mod->functions;
        while (tail->next) tail = tail->next;
        tail->next = fn;
    }
    mod->fn_count++;
    return fn;
}

int ir_function_add_param(IrFunction *fn, IrModule *mod, const char *name, IrType type) {
    if (fn->param_count >= IR_MAX_PARAMS) return -1;
    int idx = fn->param_count;
    fn->param_types[idx] = type;
    fn->param_names[idx] = arena_strdup(mod->arena, name);
    fn->param_value_ids[idx] = fn->next_value++;
    fn->param_count++;
    return fn->param_value_ids[idx];
}

IrBasicBlock *ir_bb_new(IrFunction *fn, IrModule *mod, const char *label) {
    IrBasicBlock *bb = (IrBasicBlock *)arena_alloc(mod->arena, sizeof(IrBasicBlock));
    memset(bb, 0, sizeof(IrBasicBlock));
    bb->id = fn->next_bb++;
    bb->label = label ? arena_strdup(mod->arena, label) : NULL;

    /* Append to function */
    if (!fn->entry) {
        fn->entry = bb;
    } else {
        IrBasicBlock *tail = fn->entry;
        while (tail->next) tail = tail->next;
        tail->next = bb;
    }
    fn->bb_count++;
    fn->current_bb = bb;
    return bb;
}

void ir_set_current_bb(IrFunction *fn, IrBasicBlock *bb) {
    fn->current_bb = bb;
}

/* Internal: allocate an instruction and append it to the current block */
static IrInst *ir_inst_new(IrFunction *fn, IrModule *mod, IrOpcode op, IrType type) {
    IrInst *inst = (IrInst *)arena_alloc(mod->arena, sizeof(IrInst));
    memset(inst, 0, sizeof(IrInst));
    inst->op = op;
    inst->type = type;
    inst->target_bb = -1;
    inst->false_bb = -1;

    int i;
    for (i = 0; i < IR_MAX_OPERANDS; i++)
        inst->operands[i] = -1;

    /* Assign SSA value ID for ops that produce a value */
    switch (op) {
    case IR_STORE:
    case IR_RET:
    case IR_BR:
    case IR_JMP:
    case IR_PRINT:
    case IR_NOP:
        inst->id = -1;  /* No result */
        break;
    default:
        inst->id = fn->next_value++;
        break;
    }

    /* Append to current basic block */
    IrBasicBlock *bb = fn->current_bb;
    if (bb) {
        if (!bb->first) {
            bb->first = inst;
            bb->last = inst;
        } else {
            bb->last->next = inst;
            bb->last = inst;
        }
        bb->inst_count++;
    }

    return inst;
}

/* ---- Constant emission ---- */

int ir_emit_const_int(IrFunction *fn, IrModule *mod, int64_t val) {
    IrInst *inst = ir_inst_new(fn, mod, IR_CONST_INT, IR_TYPE_I64);
    inst->imm_int = val;
    return inst->id;
}

int ir_emit_const_float(IrFunction *fn, IrModule *mod, double val) {
    IrInst *inst = ir_inst_new(fn, mod, IR_CONST_FLOAT, IR_TYPE_F64);
    inst->imm_float = val;
    return inst->id;
}

int ir_emit_const_string(IrFunction *fn, IrModule *mod, const char *val) {
    IrInst *inst = ir_inst_new(fn, mod, IR_CONST_STRING, IR_TYPE_STRING);
    inst->imm_str = val ? arena_strdup(mod->arena, val) : NULL;
    return inst->id;
}

int ir_emit_const_bool(IrFunction *fn, IrModule *mod, bool val) {
    IrInst *inst = ir_inst_new(fn, mod, IR_CONST_BOOL, IR_TYPE_BOOL);
    inst->imm_int = val ? 1 : 0;
    return inst->id;
}

/* ---- Binary/Unary operations ---- */

int ir_emit_binop(IrFunction *fn, IrModule *mod, IrOpcode op, IrType type,
                  int lhs, int rhs) {
    IrInst *inst = ir_inst_new(fn, mod, op, type);
    inst->operands[0] = lhs;
    inst->operands[1] = rhs;
    inst->operand_count = 2;
    return inst->id;
}

int ir_emit_unary(IrFunction *fn, IrModule *mod, IrOpcode op, IrType type, int operand) {
    IrInst *inst = ir_inst_new(fn, mod, op, type);
    inst->operands[0] = operand;
    inst->operand_count = 1;
    return inst->id;
}

/* ---- Memory operations ---- */

int ir_emit_alloca(IrFunction *fn, IrModule *mod, IrType type) {
    IrInst *inst = ir_inst_new(fn, mod, IR_ALLOCA, IR_TYPE_PTR);
    /* Store the element type in operands[0] as an encoded type marker */
    inst->imm_int = (int64_t)type;
    return inst->id;
}

int ir_emit_load(IrFunction *fn, IrModule *mod, IrType type, int addr) {
    IrInst *inst = ir_inst_new(fn, mod, IR_LOAD, type);
    inst->operands[0] = addr;
    inst->operand_count = 1;
    return inst->id;
}

void ir_emit_store(IrFunction *fn, IrModule *mod, int value, int addr) {
    IrInst *inst = ir_inst_new(fn, mod, IR_STORE, IR_TYPE_VOID);
    inst->operands[0] = value;
    inst->operands[1] = addr;
    inst->operand_count = 2;
}

/* ---- Control flow ---- */

int ir_emit_call(IrFunction *fn, IrModule *mod, const char *callee,
                 IrType ret_type, int *args, int arg_count) {
    IrInst *inst = ir_inst_new(fn, mod, IR_CALL, ret_type);
    inst->fn_name = arena_strdup(mod->arena, callee);
    int i;
    for (i = 0; i < arg_count && i < 16; i++)
        inst->call_args[i] = args[i];
    inst->call_arg_count = arg_count;
    return inst->id;
}

void ir_emit_ret(IrFunction *fn, IrModule *mod, int value) {
    IrInst *inst = ir_inst_new(fn, mod, IR_RET, IR_TYPE_VOID);
    inst->operands[0] = value;
    inst->operand_count = 1;
}

void ir_emit_ret_void(IrFunction *fn, IrModule *mod) {
    IrInst *inst = ir_inst_new(fn, mod, IR_RET, IR_TYPE_VOID);
    inst->operand_count = 0;
}

void ir_emit_br(IrFunction *fn, IrModule *mod, int cond, int true_bb, int false_bb) {
    IrInst *inst = ir_inst_new(fn, mod, IR_BR, IR_TYPE_VOID);
    inst->operands[0] = cond;
    inst->operand_count = 1;
    inst->target_bb = true_bb;
    inst->false_bb = false_bb;
}

void ir_emit_jmp(IrFunction *fn, IrModule *mod, int target_bb) {
    IrInst *inst = ir_inst_new(fn, mod, IR_JMP, IR_TYPE_VOID);
    inst->target_bb = target_bb;
}

int ir_emit_phi(IrFunction *fn, IrModule *mod, IrType type) {
    IrInst *inst = ir_inst_new(fn, mod, IR_PHI, type);
    inst->phi_count = 0;
    return inst->id;
}

void ir_phi_add_incoming(IrInst *phi, int value, int block) {
    if (phi->phi_count < IR_MAX_PHI_ARGS) {
        phi->phi_args[phi->phi_count].value = value;
        phi->phi_args[phi->phi_count].block = block;
        phi->phi_count++;
    }
}

/* ---- String / Builtin operations ---- */

int ir_emit_str_concat(IrFunction *fn, IrModule *mod, int lhs, int rhs) {
    IrInst *inst = ir_inst_new(fn, mod, IR_STR_CONCAT, IR_TYPE_STRING);
    inst->operands[0] = lhs;
    inst->operands[1] = rhs;
    inst->operand_count = 2;
    return inst->id;
}

void ir_emit_print(IrFunction *fn, IrModule *mod, int value) {
    IrInst *inst = ir_inst_new(fn, mod, IR_PRINT, IR_TYPE_VOID);
    inst->operands[0] = value;
    inst->operand_count = 1;
}

int ir_emit_cast(IrFunction *fn, IrModule *mod, IrType target, int value) {
    IrInst *inst = ir_inst_new(fn, mod, IR_CAST, target);
    inst->operands[0] = value;
    inst->operand_count = 1;
    return inst->id;
}

int ir_emit_gep(IrFunction *fn, IrModule *mod, IrType type, int base, int index) {
    IrInst *inst = ir_inst_new(fn, mod, IR_GEP, type);
    inst->operands[0] = base;
    inst->operands[1] = index;
    inst->operand_count = 2;
    return inst->id;
}

/* ============================================================
 * IR Generation Context
 * ============================================================ */

/* Variable scope for SSA value tracking.
 * Maps variable names to their current SSA alloca addresses. */
#define IR_GEN_MAX_VARS  512
#define IR_GEN_MAX_SCOPE 32

typedef struct {
    const char *name;
    int         addr_id;    /* SSA value of the alloca */
    IrType      type;
} IrGenVar;

typedef struct {
    IrGenVar    vars[IR_GEN_MAX_VARS];
    int         var_count;
    int         scope_starts[IR_GEN_MAX_SCOPE];
    int         scope_depth;
} IrGenScope;

typedef struct {
    IrModule   *mod;
    IrFunction *current_fn;
    IrGenScope  scope;

    /* Function name registry for call target resolution */
    struct {
        const char *name;
        IrType      ret_type;
    } fn_registry[256];
    int fn_reg_count;
} IrGenContext;

static void irgen_scope_init(IrGenScope *s) {
    s->var_count = 0;
    s->scope_depth = 0;
}

static void irgen_scope_push(IrGenScope *s) {
    if (s->scope_depth < IR_GEN_MAX_SCOPE) {
        s->scope_starts[s->scope_depth] = s->var_count;
        s->scope_depth++;
    }
}

static void irgen_scope_pop(IrGenScope *s) {
    if (s->scope_depth > 0) {
        s->scope_depth--;
        s->var_count = s->scope_starts[s->scope_depth];
    }
}

static void irgen_scope_add(IrGenScope *s, const char *name, int addr_id, IrType type) {
    if (s->var_count < IR_GEN_MAX_VARS) {
        s->vars[s->var_count].name = name;
        s->vars[s->var_count].addr_id = addr_id;
        s->vars[s->var_count].type = type;
        s->var_count++;
    }
}

/* Look up variable by name (search backwards for most-recent binding) */
static IrGenVar *irgen_scope_lookup(IrGenScope *s, const char *name) {
    int i;
    for (i = s->var_count - 1; i >= 0; i--) {
        if (strcmp(s->vars[i].name, name) == 0)
            return &s->vars[i];
    }
    return NULL;
}

/* Register a function in the context for call-site type inference */
static void irgen_register_fn(IrGenContext *ctx, const char *name, IrType ret_type) {
    if (ctx->fn_reg_count < 256) {
        ctx->fn_registry[ctx->fn_reg_count].name = name;
        ctx->fn_registry[ctx->fn_reg_count].ret_type = ret_type;
        ctx->fn_reg_count++;
    }
}

static IrType irgen_lookup_fn_ret(IrGenContext *ctx, const char *name) {
    int i;
    for (i = 0; i < ctx->fn_reg_count; i++) {
        if (strcmp(ctx->fn_registry[i].name, name) == 0)
            return ctx->fn_registry[i].ret_type;
    }
    return IR_TYPE_VOID;
}

/* ============================================================
 * AST Type to IR Type Conversion
 * ============================================================ */

static IrType ast_type_to_ir(AstNode *type_expr) {
    if (!type_expr) return IR_TYPE_VOID;

    if (type_expr->kind == AST_TYPE_NAMED && type_expr->name) {
        if (strcmp(type_expr->name, "int") == 0 || strcmp(type_expr->name, "i64") == 0)
            return IR_TYPE_I64;
        if (strcmp(type_expr->name, "float") == 0 || strcmp(type_expr->name, "f64") == 0)
            return IR_TYPE_F64;
        if (strcmp(type_expr->name, "bool") == 0)
            return IR_TYPE_BOOL;
        if (strcmp(type_expr->name, "string") == 0 || strcmp(type_expr->name, "str") == 0)
            return IR_TYPE_STRING;
        if (strcmp(type_expr->name, "void") == 0)
            return IR_TYPE_VOID;
        /* Default: treat unknown named types as struct/ptr */
        return IR_TYPE_PTR;
    }

    if (type_expr->kind == AST_TYPE_REF || type_expr->kind == AST_TYPE_PTR)
        return IR_TYPE_PTR;

    return IR_TYPE_VOID;
}

/* ============================================================
 * Expression IR Generation
 * ============================================================ */

/* Forward declaration */
static int irgen_expr(IrGenContext *ctx, AstNode *expr);
static void irgen_stmt(IrGenContext *ctx, AstNode *stmt);

/* Determine the IR type of a literal or expression */
static IrType irgen_infer_type(AstNode *expr) {
    if (!expr) return IR_TYPE_VOID;
    switch (expr->kind) {
    case AST_INT_LIT:    return IR_TYPE_I64;
    case AST_FLOAT_LIT:  return IR_TYPE_F64;
    case AST_STRING_LIT: return IR_TYPE_STRING;
    case AST_BOOL_LIT:   return IR_TYPE_BOOL;
    case AST_NONE_LIT:   return IR_TYPE_PTR;
    case AST_BINARY: {
        /* Comparisons return bool */
        TokenKind op = expr->val.op;
        if (op == TOK_EQ_EQ || op == TOK_NOT_EQ ||
            op == TOK_LT || op == TOK_GT ||
            op == TOK_LT_EQ || op == TOK_GT_EQ ||
            op == TOK_AND_AND || op == TOK_PIPE_PIPE)
            return IR_TYPE_BOOL;
        /* Arithmetic follows LHS type */
        return irgen_infer_type(expr->left);
    }
    case AST_UNARY:
        if (expr->val.op == TOK_BANG) return IR_TYPE_BOOL;
        return irgen_infer_type(expr->left);
    case AST_CALL:
        /* Default: assume call returns i64; actual type resolved at emit time */
        return IR_TYPE_I64;
    default:
        return IR_TYPE_I64;
    }
}

/* Determine the actual type of a generated SSA value */
static IrType irgen_value_type(IrGenContext *ctx, int val_id) {
    if (val_id < 0) return IR_TYPE_VOID;
    IrBasicBlock *bb;
    for (bb = ctx->current_fn->entry; bb; bb = bb->next) {
        IrInst *inst;
        for (inst = bb->first; inst; inst = inst->next) {
            if (inst->id == val_id) return inst->type;
        }
    }
    return IR_TYPE_I64;
}

/* Generate IR for a binary expression */
static int irgen_binary(IrGenContext *ctx, AstNode *expr) {
    int lhs = irgen_expr(ctx, expr->left);
    int rhs = irgen_expr(ctx, expr->right);
    IrType lhs_type = irgen_value_type(ctx, lhs);
    IrType rhs_type = irgen_value_type(ctx, rhs);
    bool is_float = (lhs_type == IR_TYPE_F64 || rhs_type == IR_TYPE_F64);
    IrType result_type = irgen_infer_type(expr);
    /* Promote result type if children are known to be i64 but inference said void */
    if (result_type == IR_TYPE_VOID || result_type == IR_TYPE_I64) {
        if (lhs_type == IR_TYPE_F64 || rhs_type == IR_TYPE_F64)
            result_type = IR_TYPE_F64;
        else if (lhs_type == IR_TYPE_I64 || rhs_type == IR_TYPE_I64)
            result_type = IR_TYPE_I64;
    }

    switch (expr->val.op) {
    case TOK_PLUS:
        /* Check for string concatenation */
        if (irgen_infer_type(expr->left) == IR_TYPE_STRING ||
            irgen_infer_type(expr->right) == IR_TYPE_STRING) {
            return ir_emit_str_concat(ctx->current_fn, ctx->mod, lhs, rhs);
        }
        return ir_emit_binop(ctx->current_fn, ctx->mod,
                             is_float ? IR_FADD : IR_ADD, result_type, lhs, rhs);
    case TOK_MINUS:
        return ir_emit_binop(ctx->current_fn, ctx->mod,
                             is_float ? IR_FSUB : IR_SUB, result_type, lhs, rhs);
    case TOK_STAR:
        return ir_emit_binop(ctx->current_fn, ctx->mod,
                             is_float ? IR_FMUL : IR_MUL, result_type, lhs, rhs);
    case TOK_SLASH:
        return ir_emit_binop(ctx->current_fn, ctx->mod,
                             is_float ? IR_FDIV : IR_DIV, result_type, lhs, rhs);
    case TOK_PERCENT:
        return ir_emit_binop(ctx->current_fn, ctx->mod, IR_MOD, result_type, lhs, rhs);
    case TOK_EQ_EQ:
        return ir_emit_binop(ctx->current_fn, ctx->mod, IR_CMP_EQ, IR_TYPE_BOOL, lhs, rhs);
    case TOK_NOT_EQ:
        return ir_emit_binop(ctx->current_fn, ctx->mod, IR_CMP_NE, IR_TYPE_BOOL, lhs, rhs);
    case TOK_LT:
        return ir_emit_binop(ctx->current_fn, ctx->mod, IR_CMP_LT, IR_TYPE_BOOL, lhs, rhs);
    case TOK_GT:
        return ir_emit_binop(ctx->current_fn, ctx->mod, IR_CMP_GT, IR_TYPE_BOOL, lhs, rhs);
    case TOK_LT_EQ:
        return ir_emit_binop(ctx->current_fn, ctx->mod, IR_CMP_LE, IR_TYPE_BOOL, lhs, rhs);
    case TOK_GT_EQ:
        return ir_emit_binop(ctx->current_fn, ctx->mod, IR_CMP_GE, IR_TYPE_BOOL, lhs, rhs);
    case TOK_AND_AND:
        return ir_emit_binop(ctx->current_fn, ctx->mod, IR_AND, IR_TYPE_BOOL, lhs, rhs);
    case TOK_PIPE_PIPE:
        return ir_emit_binop(ctx->current_fn, ctx->mod, IR_OR, IR_TYPE_BOOL, lhs, rhs);
    default:
        /* Fallback: treat as add */
        return ir_emit_binop(ctx->current_fn, ctx->mod, IR_ADD, IR_TYPE_I64, lhs, rhs);
    }
}

/* Generate IR for a unary expression */
static int irgen_unary(IrGenContext *ctx, AstNode *expr) {
    int operand = irgen_expr(ctx, expr->left);
    IrType type = irgen_infer_type(expr->left);

    switch (expr->val.op) {
    case TOK_MINUS:
        return ir_emit_unary(ctx->current_fn, ctx->mod, IR_NEG, type, operand);
    case TOK_BANG:
        return ir_emit_unary(ctx->current_fn, ctx->mod, IR_NOT, IR_TYPE_BOOL, operand);
    default:
        return operand;
    }
}

/* Generate IR for a function call */
static int irgen_call(IrGenContext *ctx, AstNode *expr) {
    /* Get the callee name */
    const char *callee = NULL;
    if (expr->left && expr->left->kind == AST_IDENT) {
        callee = expr->left->name;
    }
    if (!callee) callee = "unknown";

    /* Special handling for print/println */
    if (strcmp(callee, "print") == 0 || strcmp(callee, "println") == 0) {
        if (expr->params) {
            int arg = irgen_expr(ctx, expr->params);
            ir_emit_print(ctx->current_fn, ctx->mod, arg);
        }
        return -1; /* void */
    }

    /* General function call */
    int args[16];
    int arg_count = 0;
    AstNode *arg_node;
    for (arg_node = expr->params; arg_node && arg_count < 16; arg_node = arg_node->next) {
        args[arg_count++] = irgen_expr(ctx, arg_node);
    }

    /* Determine return type from registry */
    IrType ret_type = irgen_lookup_fn_ret(ctx, callee);

    /* Build qualified name: lcn_<name> */
    char fn_name[256];
    snprintf(fn_name, sizeof(fn_name), "lcn_%s", callee);

    return ir_emit_call(ctx->current_fn, ctx->mod, fn_name, ret_type, args, arg_count);
}

/* Main expression IR generator */
static int irgen_expr(IrGenContext *ctx, AstNode *expr) {
    if (!expr) return -1;

    switch (expr->kind) {
    case AST_INT_LIT:
        return ir_emit_const_int(ctx->current_fn, ctx->mod, expr->val.int_val);

    case AST_FLOAT_LIT:
        return ir_emit_const_float(ctx->current_fn, ctx->mod, expr->val.float_val);

    case AST_STRING_LIT:
        return ir_emit_const_string(ctx->current_fn, ctx->mod,
                                     expr->val.str_val ? expr->val.str_val : "");

    case AST_BOOL_LIT:
        return ir_emit_const_bool(ctx->current_fn, ctx->mod, expr->val.bool_val);

    case AST_NONE_LIT:
        return ir_emit_const_int(ctx->current_fn, ctx->mod, 0);

    case AST_IDENT: {
        if (!expr->name) return -1;
        IrGenVar *var = irgen_scope_lookup(&ctx->scope, expr->name);
        if (var) {
            return ir_emit_load(ctx->current_fn, ctx->mod, var->type, var->addr_id);
        }
        /* Unknown identifier: could be a global or param, emit as load from name.
         * For now, search params of current function. */
        if (ctx->current_fn) {
            int i;
            for (i = 0; i < ctx->current_fn->param_count; i++) {
                if (ctx->current_fn->param_names[i] &&
                    strcmp(ctx->current_fn->param_names[i], expr->name) == 0) {
                    return ctx->current_fn->param_value_ids[i];
                }
            }
        }
        /* Fallback: emit a const 0 as placeholder */
        return ir_emit_const_int(ctx->current_fn, ctx->mod, 0);
    }

    case AST_BINARY:
        return irgen_binary(ctx, expr);

    case AST_UNARY:
        return irgen_unary(ctx, expr);

    case AST_CALL:
        return irgen_call(ctx, expr);

    case AST_CAST: {
        int val = irgen_expr(ctx, expr->left);
        IrType target = ast_type_to_ir(expr->type_expr);
        return ir_emit_cast(ctx->current_fn, ctx->mod, target, val);
    }

    case AST_IF: {
        /* if/else as expression: returns a value via phi node */
        IrBasicBlock *pre_bb = ctx->current_fn->current_bb;
        int cond = irgen_expr(ctx, expr->left);

        IrBasicBlock *then_bb = ir_bb_new(ctx->current_fn, ctx->mod, "if.then");
        IrBasicBlock *else_bb = ir_bb_new(ctx->current_fn, ctx->mod, "if.else");
        IrBasicBlock *merge_bb = ir_bb_new(ctx->current_fn, ctx->mod, "if.merge");

        /* Emit branch in the block that evaluated the condition */
        ir_set_current_bb(ctx->current_fn, pre_bb);
        ir_emit_br(ctx->current_fn, ctx->mod, cond, then_bb->id, else_bb->id);

        /* Generate then block */
        ir_set_current_bb(ctx->current_fn, then_bb);
        irgen_scope_push(&ctx->scope);
        if (expr->right) {
            /* then body */
            AstNode *stmt;
            if (expr->right->kind == AST_BLOCK) {
                for (stmt = expr->right->params; stmt; stmt = stmt->next)
                    irgen_stmt(ctx, stmt);
            } else {
                irgen_stmt(ctx, expr->right);
            }
        }
        ir_emit_jmp(ctx->current_fn, ctx->mod, merge_bb->id);
        irgen_scope_pop(&ctx->scope);

        /* Generate else block */
        ir_set_current_bb(ctx->current_fn, else_bb);
        irgen_scope_push(&ctx->scope);
        if (expr->params) {
            /* else body (AST_IF: params=else_body) */
            AstNode *stmt;
            if (expr->params->kind == AST_BLOCK) {
                for (stmt = expr->params->params; stmt; stmt = stmt->next)
                    irgen_stmt(ctx, stmt);
            } else {
                irgen_stmt(ctx, expr->params);
            }
        }
        ir_emit_jmp(ctx->current_fn, ctx->mod, merge_bb->id);
        irgen_scope_pop(&ctx->scope);

        /* Continue in merge block */
        ir_set_current_bb(ctx->current_fn, merge_bb);
        return -1; /* If used as expression, would need phi */
    }

    default:
        /* Unsupported expression kind: emit NOP */
        return -1;
    }
}

/* ============================================================
 * Statement IR Generation
 * ============================================================ */

static void irgen_stmt(IrGenContext *ctx, AstNode *stmt) {
    if (!stmt) return;

    switch (stmt->kind) {
    case AST_LET: {
        /* Variable declaration: alloca + optional store */
        IrType var_type = ast_type_to_ir(stmt->type_expr);
        if (var_type == IR_TYPE_VOID) {
            /* Infer from initializer */
            if (stmt->right) {
                var_type = irgen_infer_type(stmt->right);
                /* For call expressions, try the function registry */
                if (stmt->right->kind == AST_CALL && stmt->right->left &&
                    stmt->right->left->kind == AST_IDENT && stmt->right->left->name) {
                    IrType call_ret = irgen_lookup_fn_ret(ctx, stmt->right->left->name);
                    if (call_ret != IR_TYPE_VOID) var_type = call_ret;
                }
            } else {
                var_type = IR_TYPE_I64; /* default */
            }
        }

        int addr = ir_emit_alloca(ctx->current_fn, ctx->mod, var_type);

        if (stmt->name) {
            irgen_scope_add(&ctx->scope, stmt->name, addr, var_type);
        }

        if (stmt->right) {
            int val = irgen_expr(ctx, stmt->right);
            if (val >= 0) {
                ir_emit_store(ctx->current_fn, ctx->mod, val, addr);
            }
        }
        break;
    }

    case AST_ASSIGN: {
        /* Assignment: evaluate RHS, store to LHS address */
        if (stmt->left && stmt->left->kind == AST_IDENT && stmt->left->name) {
            IrGenVar *var = irgen_scope_lookup(&ctx->scope, stmt->left->name);
            if (var && stmt->right) {
                int val = irgen_expr(ctx, stmt->right);
                if (val >= 0) {
                    ir_emit_store(ctx->current_fn, ctx->mod, val, var->addr_id);
                }
            }
        }
        break;
    }

    case AST_RETURN: {
        if (stmt->left) {
            int val = irgen_expr(ctx, stmt->left);
            ir_emit_ret(ctx->current_fn, ctx->mod, val);
        } else {
            ir_emit_ret_void(ctx->current_fn, ctx->mod);
        }
        break;
    }

    case AST_EXPR_STMT: {
        if (stmt->left) {
            irgen_expr(ctx, stmt->left);
        }
        break;
    }

    case AST_BLOCK: {
        irgen_scope_push(&ctx->scope);
        AstNode *s;
        for (s = stmt->params; s; s = s->next)
            irgen_stmt(ctx, s);
        irgen_scope_pop(&ctx->scope);
        break;
    }

    case AST_IF: {
        /* If statement (not expression) */
        IrBasicBlock *pre_bb = ctx->current_fn->current_bb;
        int cond = irgen_expr(ctx, stmt->left);

        /* Create basic blocks */
        IrBasicBlock *then_bb = ir_bb_new(ctx->current_fn, ctx->mod, "if.then");
        IrBasicBlock *else_bb = stmt->params ?
            ir_bb_new(ctx->current_fn, ctx->mod, "if.else") : NULL;
        IrBasicBlock *merge_bb = ir_bb_new(ctx->current_fn, ctx->mod, "if.merge");

        /* Emit branch in the condition block */
        ir_set_current_bb(ctx->current_fn, pre_bb);
        ir_emit_br(ctx->current_fn, ctx->mod, cond,
                   then_bb->id, else_bb ? else_bb->id : merge_bb->id);

        /* Then block */
        ir_set_current_bb(ctx->current_fn, then_bb);
        irgen_scope_push(&ctx->scope);
        if (stmt->right) {
            if (stmt->right->kind == AST_BLOCK) {
                AstNode *s;
                for (s = stmt->right->params; s; s = s->next)
                    irgen_stmt(ctx, s);
            } else {
                irgen_stmt(ctx, stmt->right);
            }
        }
        ir_emit_jmp(ctx->current_fn, ctx->mod, merge_bb->id);
        irgen_scope_pop(&ctx->scope);

        /* Else block */
        if (else_bb) {
            ir_set_current_bb(ctx->current_fn, else_bb);
            irgen_scope_push(&ctx->scope);
            if (stmt->params) {
                if (stmt->params->kind == AST_BLOCK) {
                    AstNode *s;
                    for (s = stmt->params->params; s; s = s->next)
                        irgen_stmt(ctx, s);
                } else {
                    irgen_stmt(ctx, stmt->params);
                }
            }
            ir_emit_jmp(ctx->current_fn, ctx->mod, merge_bb->id);
            irgen_scope_pop(&ctx->scope);
        }

        /* Continue in merge block */
        ir_set_current_bb(ctx->current_fn, merge_bb);
        break;
    }

    case AST_FOR: {
        /*
         * For loop: for <pattern> in <iterator> { body }
         * AST_FOR: left=pattern, right=body, params=iterator
         *
         * IR layout:
         *   bb_init:  evaluate iterator, init loop var
         *   bb_cond:  check condition, br to body or exit
         *   bb_body:  loop body
         *   bb_inc:   increment, jmp to cond
         *   bb_exit:  continue
         *
         * For range-based loops (e.g. for i in 0..10):
         *   We check if the iterator is an AST_RANGE and generate
         *   a classic counted loop.
         */
        AstNode *pattern = stmt->left;
        AstNode *body = stmt->right;
        AstNode *iterator = stmt->params;

        /* Check for range-based loop */
        bool is_range = iterator && iterator->kind == AST_RANGE;

        if (is_range) {
            /* Range loop: for i in start..end */
            int start_val = irgen_expr(ctx, iterator->left);
            int end_val = irgen_expr(ctx, iterator->right);

            /* Allocate loop variable */
            int loop_var = ir_emit_alloca(ctx->current_fn, ctx->mod, IR_TYPE_I64);
            ir_emit_store(ctx->current_fn, ctx->mod, start_val, loop_var);

            if (pattern && pattern->name) {
                irgen_scope_push(&ctx->scope);
                irgen_scope_add(&ctx->scope, pattern->name, loop_var, IR_TYPE_I64);
            }

            IrBasicBlock *init_bb = ctx->current_fn->current_bb;
            IrBasicBlock *cond_bb = ir_bb_new(ctx->current_fn, ctx->mod, "for.cond");
            IrBasicBlock *body_bb = ir_bb_new(ctx->current_fn, ctx->mod, "for.body");
            IrBasicBlock *inc_bb = ir_bb_new(ctx->current_fn, ctx->mod, "for.inc");
            IrBasicBlock *exit_bb = ir_bb_new(ctx->current_fn, ctx->mod, "for.exit");

            /* Jump from init to cond */
            ir_set_current_bb(ctx->current_fn, init_bb);
            ir_emit_jmp(ctx->current_fn, ctx->mod, cond_bb->id);

            /* Condition block: i < end */
            ir_set_current_bb(ctx->current_fn, cond_bb);
            int cur_val = ir_emit_load(ctx->current_fn, ctx->mod, IR_TYPE_I64, loop_var);
            int cmp = ir_emit_binop(ctx->current_fn, ctx->mod, IR_CMP_LT, IR_TYPE_BOOL,
                                    cur_val, end_val);
            ir_emit_br(ctx->current_fn, ctx->mod, cmp, body_bb->id, exit_bb->id);

            /* Body block */
            ir_set_current_bb(ctx->current_fn, body_bb);
            if (body) {
                if (body->kind == AST_BLOCK) {
                    AstNode *s;
                    for (s = body->params; s; s = s->next)
                        irgen_stmt(ctx, s);
                } else {
                    irgen_stmt(ctx, body);
                }
            }
            ir_emit_jmp(ctx->current_fn, ctx->mod, inc_bb->id);

            /* Increment block: i = i + 1 */
            ir_set_current_bb(ctx->current_fn, inc_bb);
            int cur_val2 = ir_emit_load(ctx->current_fn, ctx->mod, IR_TYPE_I64, loop_var);
            int one = ir_emit_const_int(ctx->current_fn, ctx->mod, 1);
            int next_val = ir_emit_binop(ctx->current_fn, ctx->mod, IR_ADD, IR_TYPE_I64,
                                         cur_val2, one);
            ir_emit_store(ctx->current_fn, ctx->mod, next_val, loop_var);
            ir_emit_jmp(ctx->current_fn, ctx->mod, cond_bb->id);

            /* Exit block */
            ir_set_current_bb(ctx->current_fn, exit_bb);

            if (pattern && pattern->name) {
                irgen_scope_pop(&ctx->scope);
            }
        } else {
            /* Generic for-in loop: emit as iterator call (simplified).
             * For now, just generate the body once as a placeholder. */
            if (body) {
                irgen_scope_push(&ctx->scope);
                if (body->kind == AST_BLOCK) {
                    AstNode *s;
                    for (s = body->params; s; s = s->next)
                        irgen_stmt(ctx, s);
                } else {
                    irgen_stmt(ctx, body);
                }
                irgen_scope_pop(&ctx->scope);
            }
        }
        break;
    }

    case AST_WHILE: {
        /* While loop: while <cond> { body }
         * AST_WHILE: left=condition, right=body */
        IrBasicBlock *pre_bb = ctx->current_fn->current_bb;
        IrBasicBlock *cond_bb = ir_bb_new(ctx->current_fn, ctx->mod, "while.cond");
        IrBasicBlock *body_bb = ir_bb_new(ctx->current_fn, ctx->mod, "while.body");
        IrBasicBlock *exit_bb = ir_bb_new(ctx->current_fn, ctx->mod, "while.exit");

        /* Jump from current block to cond */
        ir_set_current_bb(ctx->current_fn, pre_bb);
        ir_emit_jmp(ctx->current_fn, ctx->mod, cond_bb->id);

        /* Condition */
        ir_set_current_bb(ctx->current_fn, cond_bb);
        int cond = irgen_expr(ctx, stmt->left);
        ir_emit_br(ctx->current_fn, ctx->mod, cond, body_bb->id, exit_bb->id);

        /* Body */
        ir_set_current_bb(ctx->current_fn, body_bb);
        irgen_scope_push(&ctx->scope);
        if (stmt->right) {
            if (stmt->right->kind == AST_BLOCK) {
                AstNode *s;
                for (s = stmt->right->params; s; s = s->next)
                    irgen_stmt(ctx, s);
            } else {
                irgen_stmt(ctx, stmt->right);
            }
        }
        ir_emit_jmp(ctx->current_fn, ctx->mod, cond_bb->id);
        irgen_scope_pop(&ctx->scope);

        /* Exit */
        ir_set_current_bb(ctx->current_fn, exit_bb);
        break;
    }

    default:
        /* For expression-level statements, try to generate IR for the node itself */
        irgen_expr(ctx, stmt);
        break;
    }
}

/* ============================================================
 * Top-Level Declaration IR Generation
 * ============================================================ */

static void irgen_function(IrGenContext *ctx, AstNode *fn_ast) {
    if (!fn_ast || fn_ast->kind != AST_FN) return;

    const char *name = fn_ast->name ? fn_ast->name : "anon";

    /* Determine return type */
    IrType ret_type = ast_type_to_ir(fn_ast->type_expr);

    /* Build function name: lcn_<name> */
    char fn_name[256];
    snprintf(fn_name, sizeof(fn_name), "lcn_%s", name);

    IrFunction *fn = ir_function_new(ctx->mod, fn_name, ret_type);

    /* Register for call resolution */
    irgen_register_fn(ctx, name, ret_type);

    /* Add parameters */
    AstNode *param;
    for (param = fn_ast->params; param; param = param->next) {
        if (param->kind == AST_PARAM && param->name) {
            IrType ptype = ast_type_to_ir(param->type_expr);
            ir_function_add_param(fn, ctx->mod, param->name, ptype);
        }
    }

    ctx->current_fn = fn;
    irgen_scope_push(&ctx->scope);

    /* Create entry block */
    IrBasicBlock *entry = ir_bb_new(fn, ctx->mod, "entry");
    (void)entry;

    /* Generate body */
    AstNode *body = fn_ast->left;
    if (body) {
        if (body->kind == AST_BLOCK) {
            AstNode *s;
            for (s = body->params; s; s = s->next)
                irgen_stmt(ctx, s);
        } else {
            irgen_stmt(ctx, body);
        }
    }

    /* Add implicit return void if the last instruction isn't a terminator */
    {
        IrBasicBlock *last_bb = fn->current_bb;
        if (last_bb) {
            IrInst *last = last_bb->last;
            if (!last || (last->op != IR_RET && last->op != IR_JMP && last->op != IR_BR)) {
                ir_emit_ret_void(fn, ctx->mod);
            }
        }
    }

    irgen_scope_pop(&ctx->scope);
    ctx->current_fn = NULL;
}

/* ============================================================
 * Program-Level IR Generation
 * ============================================================ */

IrModule *ir_gen_program(AstNode *program, Arena *arena) {
    if (!program || program->kind != AST_PROGRAM) return NULL;

    IrModule *mod = ir_module_new(arena);

    IrGenContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.mod = mod;
    irgen_scope_init(&ctx.scope);

    /* First pass: register all function names for forward references */
    AstNode *decl;
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind == AST_FN && decl->name) {
            IrType ret_type = ast_type_to_ir(decl->type_expr);
            irgen_register_fn(&ctx, decl->name, ret_type);
        }
    }

    /* Second pass: generate IR for each function */
    for (decl = program->params; decl; decl = decl->next) {
        switch (decl->kind) {
        case AST_FN:
            irgen_function(&ctx, decl);
            break;

        /* Skip non-function declarations for now (structs, enums, agents, etc.).
         * They don't produce IR directly in this foundation phase. */
        default:
            break;
        }
    }

    return mod;
}

/* ============================================================
 * IR Printer
 * ============================================================ */

static void ir_print_value(int id, FILE *out) {
    if (id >= 0) {
        fprintf(out, "%%%d", id);
    }
}

static void ir_print_inst(IrInst *inst, IrFunction *fn, FILE *out) {
    if (!inst) return;

    fprintf(out, "    ");

    switch (inst->op) {
    case IR_CONST_INT:
        fprintf(out, "%%%d = const %s %lld\n",
                inst->id, ir_type_name(inst->type), (long long)inst->imm_int);
        break;

    case IR_CONST_FLOAT:
        fprintf(out, "%%%d = const %s %g\n",
                inst->id, ir_type_name(inst->type), inst->imm_float);
        break;

    case IR_CONST_STRING:
        fprintf(out, "%%%d = const %s \"%s\"\n",
                inst->id, ir_type_name(inst->type),
                inst->imm_str ? inst->imm_str : "");
        break;

    case IR_CONST_BOOL:
        fprintf(out, "%%%d = const %s %s\n",
                inst->id, ir_type_name(inst->type),
                inst->imm_int ? "true" : "false");
        break;

    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV:
    case IR_CMP_EQ: case IR_CMP_NE: case IR_CMP_LT: case IR_CMP_GT:
    case IR_CMP_LE: case IR_CMP_GE:
    case IR_AND: case IR_OR:
    case IR_STR_CONCAT:
        fprintf(out, "%%%d = %s %s %%%d, %%%d\n",
                inst->id, ir_opcode_name(inst->op), ir_type_name(inst->type),
                inst->operands[0], inst->operands[1]);
        break;

    case IR_NEG: case IR_NOT:
        fprintf(out, "%%%d = %s %s %%%d\n",
                inst->id, ir_opcode_name(inst->op), ir_type_name(inst->type),
                inst->operands[0]);
        break;

    case IR_ALLOCA:
        fprintf(out, "%%%d = alloca %s\n",
                inst->id, ir_type_name((IrType)inst->imm_int));
        break;

    case IR_LOAD:
        fprintf(out, "%%%d = load %s %%%d\n",
                inst->id, ir_type_name(inst->type), inst->operands[0]);
        break;

    case IR_STORE:
        fprintf(out, "store %%%d, %%%d\n",
                inst->operands[0], inst->operands[1]);
        break;

    case IR_CALL: {
        if (inst->id >= 0 && inst->type != IR_TYPE_VOID) {
            fprintf(out, "%%%d = call %s @%s(",
                    inst->id, ir_type_name(inst->type),
                    inst->fn_name ? inst->fn_name : "???");
        } else {
            fprintf(out, "call void @%s(",
                    inst->fn_name ? inst->fn_name : "???");
        }
        int i;
        for (i = 0; i < inst->call_arg_count; i++) {
            if (i > 0) fprintf(out, ", ");
            ir_print_value(inst->call_args[i], out);
        }
        fprintf(out, ")\n");
        break;
    }

    case IR_RET:
        if (inst->operand_count > 0 && inst->operands[0] >= 0) {
            /* Find the function return type from context */
            fprintf(out, "ret %s %%%d\n", ir_type_name(fn->return_type), inst->operands[0]);
        } else {
            fprintf(out, "ret void\n");
        }
        break;

    case IR_BR:
        fprintf(out, "br %%%d, @bb%d, @bb%d\n",
                inst->operands[0], inst->target_bb, inst->false_bb);
        break;

    case IR_JMP:
        fprintf(out, "jmp @bb%d\n", inst->target_bb);
        break;

    case IR_PHI:
        fprintf(out, "%%%d = phi %s", inst->id, ir_type_name(inst->type));
        {
            int i;
            for (i = 0; i < inst->phi_count; i++) {
                fprintf(out, " [%%%d, @bb%d]",
                        inst->phi_args[i].value, inst->phi_args[i].block);
                if (i < inst->phi_count - 1) fprintf(out, ",");
            }
        }
        fprintf(out, "\n");
        break;

    case IR_PRINT:
        fprintf(out, "print %%%d\n", inst->operands[0]);
        break;

    case IR_CAST:
        fprintf(out, "%%%d = cast %s %%%d\n",
                inst->id, ir_type_name(inst->type), inst->operands[0]);
        break;

    case IR_GEP:
        fprintf(out, "%%%d = gep %s %%%d, %%%d\n",
                inst->id, ir_type_name(inst->type),
                inst->operands[0], inst->operands[1]);
        break;

    case IR_NOP:
        fprintf(out, "nop\n");
        break;

    default:
        fprintf(out, "; unknown opcode %d\n", inst->op);
        break;
    }
}

void ir_print_function(IrFunction *fn, FILE *out) {
    if (!fn) return;

    /* Function signature */
    fprintf(out, "define %s @%s(", ir_type_name(fn->return_type), fn->name);
    {
        int i;
        for (i = 0; i < fn->param_count; i++) {
            if (i > 0) fprintf(out, ", ");
            fprintf(out, "%s %%%s", ir_type_name(fn->param_types[i]),
                    fn->param_names[i] ? fn->param_names[i] : "?");
        }
    }
    fprintf(out, ") {\n");

    /* Basic blocks */
    IrBasicBlock *bb;
    for (bb = fn->entry; bb; bb = bb->next) {
        if (bb->label) {
            fprintf(out, "bb%d:  ; %s\n", bb->id, bb->label);
        } else {
            fprintf(out, "bb%d:\n", bb->id);
        }

        IrInst *inst;
        for (inst = bb->first; inst; inst = inst->next) {
            ir_print_inst(inst, fn, out);
        }

        /* Blank line between blocks (except last) */
        if (bb->next) fprintf(out, "\n");
    }

    fprintf(out, "}\n");
}

void ir_print_module(IrModule *mod, FILE *out) {
    if (!mod) return;

    fprintf(out, "; Limceron SSA IR\n");
    fprintf(out, "; Functions: %d\n\n", mod->fn_count);

    IrFunction *fn;
    for (fn = mod->functions; fn; fn = fn->next) {
        ir_print_function(fn, out);
        if (fn->next) fprintf(out, "\n");
    }
}

/* ============================================================
 * Optimization Pass: Constant Folding
 * ============================================================ */

/* Internal: find an instruction by its SSA value ID within a function */
static IrInst *ir_find_inst(IrFunction *fn, int id) {
    IrBasicBlock *bb;
    for (bb = fn->entry; bb; bb = bb->next) {
        IrInst *inst;
        for (inst = bb->first; inst; inst = inst->next) {
            if (inst->id == id) return inst;
        }
    }
    return NULL;
}

int ir_opt_constant_fold(IrModule *mod) {
    int folded = 0;

    IrFunction *fn;
    for (fn = mod->functions; fn; fn = fn->next) {
        IrBasicBlock *bb;
        for (bb = fn->entry; bb; bb = bb->next) {
            IrInst *inst;
            for (inst = bb->first; inst; inst = inst->next) {
                /* Only fold binary operations on two constants */
                if (inst->operand_count != 2) continue;

                IrInst *lhs = ir_find_inst(fn, inst->operands[0]);
                IrInst *rhs = ir_find_inst(fn, inst->operands[1]);
                if (!lhs || !rhs) continue;

                /* Integer constant folding */
                if (lhs->op == IR_CONST_INT && rhs->op == IR_CONST_INT) {
                    int64_t a = lhs->imm_int;
                    int64_t b = rhs->imm_int;
                    int64_t result;
                    bool did_fold = true;

                    switch (inst->op) {
                    case IR_ADD: result = a + b; break;
                    case IR_SUB: result = a - b; break;
                    case IR_MUL: result = a * b; break;
                    case IR_DIV:
                        if (b == 0) { did_fold = false; break; }
                        result = a / b;
                        break;
                    case IR_MOD:
                        if (b == 0) { did_fold = false; break; }
                        result = a % b;
                        break;
                    default:
                        did_fold = false;
                        break;
                    }

                    if (did_fold) {
                        /* Replace instruction with a constant */
                        inst->op = IR_CONST_INT;
                        inst->type = IR_TYPE_I64;
                        inst->imm_int = result;
                        inst->operand_count = 0;
                        inst->operands[0] = -1;
                        inst->operands[1] = -1;
                        folded++;
                    }
                }

                /* Boolean constant folding for comparisons */
                if (lhs->op == IR_CONST_INT && rhs->op == IR_CONST_INT) {
                    int64_t a = lhs->imm_int;
                    int64_t b = rhs->imm_int;
                    bool result;
                    bool did_fold = true;

                    switch (inst->op) {
                    case IR_CMP_EQ: result = (a == b); break;
                    case IR_CMP_NE: result = (a != b); break;
                    case IR_CMP_LT: result = (a < b);  break;
                    case IR_CMP_GT: result = (a > b);  break;
                    case IR_CMP_LE: result = (a <= b); break;
                    case IR_CMP_GE: result = (a >= b); break;
                    default: did_fold = false; break;
                    }

                    if (did_fold) {
                        inst->op = IR_CONST_BOOL;
                        inst->type = IR_TYPE_BOOL;
                        inst->imm_int = result ? 1 : 0;
                        inst->operand_count = 0;
                        inst->operands[0] = -1;
                        inst->operands[1] = -1;
                        folded++;
                    }
                }

                /* Float constant folding */
                if (lhs->op == IR_CONST_FLOAT && rhs->op == IR_CONST_FLOAT) {
                    double a = lhs->imm_float;
                    double b = rhs->imm_float;
                    double result;
                    bool did_fold = true;

                    switch (inst->op) {
                    case IR_FADD: result = a + b; break;
                    case IR_FSUB: result = a - b; break;
                    case IR_FMUL: result = a * b; break;
                    case IR_FDIV:
                        if (b == 0.0) { did_fold = false; break; }
                        result = a / b;
                        break;
                    default:
                        did_fold = false;
                        break;
                    }

                    if (did_fold) {
                        inst->op = IR_CONST_FLOAT;
                        inst->type = IR_TYPE_F64;
                        inst->imm_float = result;
                        inst->operand_count = 0;
                        inst->operands[0] = -1;
                        inst->operands[1] = -1;
                        folded++;
                    }
                }

                /* String constant folding: concat two string literals */
                if (inst->op == IR_STR_CONCAT &&
                    lhs->op == IR_CONST_STRING && rhs->op == IR_CONST_STRING) {
                    const char *sa = lhs->imm_str ? lhs->imm_str : "";
                    const char *sb = rhs->imm_str ? rhs->imm_str : "";
                    size_t la = strlen(sa);
                    size_t lb = strlen(sb);
                    char *combined = (char *)arena_alloc(mod->arena, la + lb + 1);
                    memcpy(combined, sa, la);
                    memcpy(combined + la, sb, lb);
                    combined[la + lb] = '\0';

                    inst->op = IR_CONST_STRING;
                    inst->type = IR_TYPE_STRING;
                    inst->imm_str = combined;
                    inst->operand_count = 0;
                    inst->operands[0] = -1;
                    inst->operands[1] = -1;
                    folded++;
                }
            }
        }
    }

    return folded;
}

/* ============================================================
 * Optimization Pass: Dead Code Elimination
 * ============================================================ */

/* Check if a given SSA value ID is used by any instruction in the function */
static bool ir_value_used(IrFunction *fn, int id) {
    IrBasicBlock *bb;
    for (bb = fn->entry; bb; bb = bb->next) {
        IrInst *inst;
        for (inst = bb->first; inst; inst = inst->next) {
            /* Check operands */
            int i;
            for (i = 0; i < inst->operand_count; i++) {
                if (inst->operands[i] == id) return true;
            }
            /* Check call args */
            if (inst->op == IR_CALL) {
                for (i = 0; i < inst->call_arg_count; i++) {
                    if (inst->call_args[i] == id) return true;
                }
            }
            /* Check phi args */
            if (inst->op == IR_PHI) {
                for (i = 0; i < inst->phi_count; i++) {
                    if (inst->phi_args[i].value == id) return true;
                }
            }
        }
    }
    return false;
}

int ir_opt_dead_code_elim(IrModule *mod) {
    int eliminated = 0;

    IrFunction *fn;
    for (fn = mod->functions; fn; fn = fn->next) {
        IrBasicBlock *bb;
        for (bb = fn->entry; bb; bb = bb->next) {
            IrInst *inst, *prev = NULL;
            for (inst = bb->first; inst; ) {
                IrInst *next = inst->next;

                /* Skip side-effectful instructions */
                bool has_side_effects = false;
                switch (inst->op) {
                case IR_STORE: case IR_RET: case IR_BR: case IR_JMP:
                case IR_PRINT: case IR_CALL:
                    has_side_effects = true;
                    break;
                default:
                    break;
                }

                /* Remove instruction if it produces a value that nobody uses */
                if (!has_side_effects && inst->id >= 0 && !ir_value_used(fn, inst->id)) {
                    /* Unlink from list */
                    if (prev) {
                        prev->next = next;
                    } else {
                        bb->first = next;
                    }
                    if (inst == bb->last) {
                        bb->last = prev;
                    }
                    bb->inst_count--;
                    eliminated++;
                    /* Don't advance prev */
                } else {
                    prev = inst;
                }

                inst = next;
            }
        }
    }

    return eliminated;
}

/* ============================================================
 * Optimization Pass: Constant Propagation
 * ============================================================ */

int ir_opt_constant_prop(IrModule *mod) {
    int propagated = 0;

    IrFunction *fn;
    for (fn = mod->functions; fn; fn = fn->next) {
        IrBasicBlock *bb;
        for (bb = fn->entry; bb; bb = bb->next) {
            IrInst *inst;
            for (inst = bb->first; inst; inst = inst->next) {
                /* For LOAD instructions: if the address was a recent ALLOCA
                 * and there's exactly one STORE to it with a constant value,
                 * replace the LOAD result with the constant. */
                if (inst->op == IR_LOAD && inst->operand_count == 1) {
                    int addr = inst->operands[0];

                    /* Find the store to this address with a constant value */
                    IrInst *store_val = NULL;
                    int store_count = 0;
                    IrBasicBlock *sb;
                    for (sb = fn->entry; sb; sb = sb->next) {
                        IrInst *si;
                        for (si = sb->first; si; si = si->next) {
                            if (si->op == IR_STORE && si->operand_count >= 2 &&
                                si->operands[1] == addr) {
                                store_count++;
                                IrInst *val_inst = ir_find_inst(fn, si->operands[0]);
                                if (val_inst && (val_inst->op == IR_CONST_INT ||
                                                  val_inst->op == IR_CONST_FLOAT ||
                                                  val_inst->op == IR_CONST_STRING ||
                                                  val_inst->op == IR_CONST_BOOL)) {
                                    store_val = val_inst;
                                }
                            }
                        }
                    }

                    /* Only propagate if there's exactly one store with a constant */
                    if (store_count == 1 && store_val) {
                        /* Replace LOAD with a copy of the constant */
                        inst->op = store_val->op;
                        inst->type = store_val->type;
                        inst->imm_int = store_val->imm_int;
                        inst->imm_float = store_val->imm_float;
                        inst->imm_str = store_val->imm_str;
                        inst->operand_count = 0;
                        inst->operands[0] = -1;
                        propagated++;
                    }
                }
            }
        }
    }

    return propagated;
}

/* ============================================================
 * Combined Optimization
 * ============================================================ */

void ir_opt_all(IrModule *mod) {
    /* Run passes iteratively until no more changes */
    int rounds = 0;
    int max_rounds = 10;
    while (rounds < max_rounds) {
        int changes = 0;
        changes += ir_opt_constant_fold(mod);
        changes += ir_opt_constant_prop(mod);
        changes += ir_opt_dead_code_elim(mod);
        if (changes == 0) break;
        rounds++;
    }
}

/* ============================================================
 * Register Allocator — Linear Scan for x86_64
 * ============================================================ */

const char *x86_reg_name(X86Reg reg) {
    switch (reg) {
    case REG_RAX: return "%rax";
    case REG_RCX: return "%rcx";
    case REG_RDX: return "%rdx";
    case REG_RSI: return "%rsi";
    case REG_RDI: return "%rdi";
    case REG_R8:  return "%r8";
    case REG_R9:  return "%r9";
    case REG_R10: return "%r10";
    case REG_R11: return "%r11";
    case REG_RBX: return "%rbx";
    case REG_R12: return "%r12";
    case REG_R13: return "%r13";
    case REG_R14: return "%r14";
    case REG_R15: return "%r15";
    case REG_NONE:  return "(none)";
    case REG_SPILL: return "(spill)";
    }
    return "(?)";
}

void ir_x86_compute_live_ranges(IrFunction *fn, RegAlloc *allocs, int *count) {
    *count = 0;
    if (!fn) return;

    /* First pass: assign instruction indices and collect all value definitions */
    int inst_idx = 0;
    IrBasicBlock *bb;
    IrInst *inst;

    /* Also count params as live values */
    {
        int i;
        for (i = 0; i < fn->param_count; i++) {
            int vid = fn->param_value_ids[i];
            allocs[*count].value_id = vid;
            allocs[*count].reg = REG_NONE;
            allocs[*count].spill_offset = 0;
            allocs[*count].start = 0;
            allocs[*count].end = 0;
            (*count)++;
        }
    }

    for (bb = fn->entry; bb; bb = bb->next) {
        for (inst = bb->first; inst; inst = inst->next) {
            /* If this instruction produces a value, record its definition point */
            if (inst->id >= 0) {
                int idx = *count;
                allocs[idx].value_id = inst->id;
                allocs[idx].reg = REG_NONE;
                allocs[idx].spill_offset = 0;
                allocs[idx].start = inst_idx;
                allocs[idx].end = inst_idx;
                (*count)++;
            }
            inst_idx++;
        }
    }

    /* Second pass: extend live ranges to last use */
    inst_idx = 0;
    for (bb = fn->entry; bb; bb = bb->next) {
        for (inst = bb->first; inst; inst = inst->next) {
            int i;
            /* Check operands */
            for (i = 0; i < inst->operand_count; i++) {
                int vid = inst->operands[i];
                if (vid < 0) continue;
                int j;
                for (j = 0; j < *count; j++) {
                    if (allocs[j].value_id == vid) {
                        if (inst_idx > allocs[j].end)
                            allocs[j].end = inst_idx;
                        break;
                    }
                }
            }
            /* Check call args */
            if (inst->op == IR_CALL) {
                for (i = 0; i < inst->call_arg_count; i++) {
                    int vid = inst->call_args[i];
                    if (vid < 0) continue;
                    int j;
                    for (j = 0; j < *count; j++) {
                        if (allocs[j].value_id == vid) {
                            if (inst_idx > allocs[j].end)
                                allocs[j].end = inst_idx;
                            break;
                        }
                    }
                }
            }
            /* Check phi args */
            if (inst->op == IR_PHI) {
                for (i = 0; i < inst->phi_count; i++) {
                    int vid = inst->phi_args[i].value;
                    if (vid < 0) continue;
                    int j;
                    for (j = 0; j < *count; j++) {
                        if (allocs[j].value_id == vid) {
                            if (inst_idx > allocs[j].end)
                                allocs[j].end = inst_idx;
                            break;
                        }
                    }
                }
            }
            inst_idx++;
        }
    }
}

/* Sort allocs by start point (insertion sort for simplicity) */
static void regalloc_sort_by_start(RegAlloc *allocs, int count) {
    int i, j;
    for (i = 1; i < count; i++) {
        RegAlloc tmp = allocs[i];
        j = i - 1;
        while (j >= 0 && allocs[j].start > tmp.start) {
            allocs[j + 1] = allocs[j];
            j--;
        }
        allocs[j + 1] = tmp;
    }
}

void ir_linear_scan_alloc(RegAlloc *allocs, int count) {
    if (count == 0) return;

    regalloc_sort_by_start(allocs, count);

    /* Available registers for allocation.
     * We skip RAX (used for return values / division),
     * and allocate from caller-saved first, then callee-saved. */
    static const X86Reg alloc_order[] = {
        REG_RCX, REG_RDX, REG_RSI, REG_RDI,
        REG_R8, REG_R9, REG_R10, REG_R11,
        REG_RBX, REG_R12, REG_R13, REG_R14, REG_R15
    };
    int n_alloc_regs = 13;

    /* Track which registers are currently in use and when they become free */
    int reg_free_at[16];  /* Index by X86Reg enum value */
    {
        int i;
        for (i = 0; i < 16; i++) reg_free_at[i] = -1;
    }

    int next_spill_offset = -8;  /* Stack grows downward from %rbp */

    int i;
    for (i = 0; i < count; i++) {
        /* Expire old intervals: free registers whose live range has ended */
        {
            int r;
            for (r = 0; r < n_alloc_regs; r++) {
                X86Reg reg = alloc_order[r];
                if (reg_free_at[reg] >= 0 && reg_free_at[reg] < allocs[i].start) {
                    reg_free_at[reg] = -1;
                }
            }
        }

        /* Try to find a free register */
        bool allocated = false;
        {
            int r;
            for (r = 0; r < n_alloc_regs; r++) {
                X86Reg reg = alloc_order[r];
                if (reg_free_at[reg] < 0) {
                    allocs[i].reg = reg;
                    reg_free_at[reg] = allocs[i].end;
                    allocated = true;
                    break;
                }
            }
        }

        /* No free register: spill */
        if (!allocated) {
            allocs[i].reg = REG_SPILL;
            allocs[i].spill_offset = next_spill_offset;
            next_spill_offset -= 8;
        }
    }
}
