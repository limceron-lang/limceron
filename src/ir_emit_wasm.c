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

/* --emit-wit / --no-emit-wit toggle. Defaults to ON; cmd_build flips
 * it OFF for non-wasm targets unless the operator passes --emit-wit
 * explicitly. The setter has C linkage so main.c can call it without
 * pulling ir_emit_wasm's full header surface. */
static int g_emit_wit_enabled = 1;
void lcn_set_emit_wit(int on) { g_emit_wit_enabled = on ? 1 : 0; }
int  lcn_emit_wit_enabled(void) { return g_emit_wit_enabled; }

/* ============================================================
 * Helpers
 * ============================================================ */

#define WASM_DATA_BASE        1024     /* leave [0..1024) free for stack */
#define WASM_MAX_STRINGS      512
#define WASM_MAX_LOCALS       4096
#define WASM_BUMP_PTR_GLOBAL  "$bump_ptr"

/* Host-call scratch region.
 *
 * Host imports follow a buffer-protocol ABI (see imports.go): the caller
 * passes input ptr/len pairs and an output buffer ptr+max. We carve a
 * per-call-site slab out of linear memory at a fixed offset above the
 * static-data area so each call has stable scratch addresses we can
 * embed as i32 literals.
 *
 * Layout per host-call site (1 KiB total):
 *   [base + 0    .. base + 1024):  primary out-buffer (label, body, etc.)
 *   [base + 1024 .. base + 1032):  confidence f64 (only used by llm.classify)
 *   [base + 1032 .. base + 1040):  status i32 / reserved
 *
 * We round per-site stride to 1088 bytes so successive sites stay 64-byte
 * aligned. The base of the scratch region is HOST_SCRATCH_BASE; the
 * string data segment is positioned below (it grows up from
 * WASM_DATA_BASE and is bounded by HOST_SCRATCH_BASE). */
#define WASM_HOST_SCRATCH_BASE   8192
#define WASM_HOST_SCRATCH_STRIDE 1088
#define WASM_HOST_OUTBUF_MAX     1024
#define WASM_MAX_HOST_CALLS      64

/* Entropy-budget runtime fence (L11).
 *
 * Each agent may declare `entropy_budget: <bits>` in its agent block.
 * When present, the compiler emits a wasm `(global $entropy_remaining
 * (mut i32))` initialised to that many bits, and decrements it before
 * every entropy-consuming host call. If the remaining budget would
 * drop below the call's cost, the function returns the sentinel
 * `HostError.EntropyExceeded = -9` instead of dispatching the call.
 *
 * The cost table is per host-call qualified name and is currently
 * hardcoded (refined later when we measure log-prob). Capabilities
 * that don't consume entropy (network/db/json) return 0 here and
 * incur no fence overhead at the emission site.
 *
 * If the agent does NOT declare entropy_budget (or the form is the
 * legacy block-of-knobs `entropy_budget: { ... }` rather than a
 * scalar bit count), the global is still emitted but initialised to
 * INT32_MAX so the fence is effectively a no-op. */
#define WASM_ENTROPY_GLOBAL          "$entropy_remaining"
#define WASM_HOST_ERR_ENTROPY_EXC    (-9)
#define WASM_ENTROPY_UNBOUNDED       2147483647

/* Budget runtime fence (L13).
 *
 * Each agent may declare a `budget: { max_tokens: <int>, max_cost: <float> }`
 * block. When present, the compiler emits two wasm globals --
 * `$tokens_remaining` (mut i32, init = declared max_tokens) and
 * `$cost_micro_usd_remaining` (mut i64, init = declared max_cost * 1e6) --
 * and decrements both at every host call site that consumes tokens or
 * cost. A breach (either counter would go negative) short-circuits the
 * enclosing function with `HostError.BudgetExceeded = -10`.
 *
 * The token/cost cost tables are per qualified host-call name and are
 * hardcoded for stage0. Costs are intentionally conservative per-call
 * estimates -- the authoritative reconciliation happens in Visual-DAG's
 * `cost.Guard`. The wasm fence is the inner hard cap that lets a single
 * Limceron program refuse to dispatch a call that would obviously
 * overshoot the declared envelope.
 *
 * If the agent does NOT declare a budget block (or declares it without
 * one of the recognised fields), the corresponding global is initialised
 * to INT*_MAX so the fence is effectively a no-op. Chain order at each
 * call site mirrors the natural failure precedence:
 *   1. entropy fence  (L11)  -> HostError.EntropyExceeded  = -9
 *   2. token  fence  (L13)  -> HostError.BudgetExceeded   = -10
 *   3. cost   fence  (L13)  -> HostError.BudgetExceeded   = -10
 * The first to trip wins. */
#define WASM_TOKENS_GLOBAL              "$tokens_remaining"
#define WASM_COST_GLOBAL                "$cost_micro_usd_remaining"
#define WASM_HOST_ERR_BUDGET_EXC        (-10)
#define WASM_TOKENS_UNBOUNDED           2147483647
/* i64 MAX (= 9223372036854775807). Stay inside i64 literal range. */
#define WASM_COST_UNBOUNDED             9223372036854775807LL

typedef struct {
    const char *value;       /* pointer into IR (arena-owned), key for dedup */
    int         offset;      /* byte offset in linear memory */
    int         length;      /* UTF-8 byte length */
} WasmString;

/* One entry per IR_HOST_CALL site. Each is a tuple of the qualified
 * capability name plus the linear-memory scratch slot we allocated. The
 * scratch slot is shared by all call sites with the same qualified
 * name (they cannot interleave in stage0's sequential model). */
typedef struct {
    const char *qualified;   /* e.g. "llm.classify"                  */
    const char *ns;          /* e.g. "llm" (arena-owned)             */
    const char *fn;          /* e.g. "classify" (arena-owned)        */
    int         scratch_off; /* offset of this site's 1 KiB out-buf  */
} HostCallSite;

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

    /* Host-call sites (deduped by qualified name). */
    HostCallSite  host_calls[WASM_MAX_HOST_CALLS];
    int           host_call_count;

    /* Entropy budget (L11). `entropy_budget_bits` is the scalar bit count
     * the agent declared; if no agent declared it (or only the legacy
     * block form was used), this stays at WASM_ENTROPY_UNBOUNDED so the
     * fence is a no-op. `entropy_budget_declared` records whether we
     * found an explicit scalar form, used for diagnostic comments in
     * the emitted WAT. */
    int           entropy_budget_bits;
    int           entropy_budget_declared;

    /* Token / cost budget (L13). `tokens_budget` is the declared
     * `budget: { max_tokens: ... }` or WASM_TOKENS_UNBOUNDED if not
     * declared. `cost_micro_usd_budget` is the declared `max_cost`
     * converted to micro-USD (cost * 1_000_000, rounded to nearest)
     * or WASM_COST_UNBOUNDED if not declared. `*_declared` flags
     * drive only the diagnostic preamble comment. */
    int           tokens_budget;
    int           tokens_budget_declared;
    int64_t       cost_micro_usd_budget;
    int           cost_budget_declared;

    /* L12: capability.network compile-time allowlist.
     *
     * If any agent declares a parameterised `capabilities:
     * [http.fetch(["host:port", ...])]` entry we capture the list
     * here so the module emit can write a `vdag.capability.network
     * .allowlist` custom section carrying the JSON-encoded payload.
     * The runtime (wazero) reads this section at instantiate time
     * and rejects the module if any declared host:port is not in
     * the worker's runtime allowlist. If no parameterised form is
     * declared, `net_allow_count` stays at 0 and no custom section
     * is emitted (bare form = no compile-time fence). */
#define WASM_MAX_NET_ALLOW   64
    const char   *net_allow_hosts[WASM_MAX_NET_ALLOW];
    int           net_allow_count;
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
 * Host-call site registration
 *
 * Each unique IR_HOST_CALL qualified name (e.g. "llm.classify") gets
 * one entry. The entry owns a slab of linear memory used as the
 * out-buffer + confidence/status scratch for every invocation of that
 * capability. Different capabilities never share a slab so the host
 * cannot accidentally clobber one call's payload with another's. */

static int host_call_register(EmitCtx *ctx, const char *qualified) {
    if (!qualified) return -1;
    int i;
    for (i = 0; i < ctx->host_call_count; i++) {
        if (strcmp(ctx->host_calls[i].qualified, qualified) == 0) {
            return i;
        }
    }
    if (ctx->host_call_count >= WASM_MAX_HOST_CALLS) {
        fprintf(stderr,
                "  WASM emit: too many distinct host-call sites (max %d)\n",
                WASM_MAX_HOST_CALLS);
        return -1;
    }
    /* Split "ns.fn" into ns + fn. We own arena-style copies through the
     * IR module's arena. */
    const char *dot = strchr(qualified, '.');
    if (!dot) return -1;
    int idx = ctx->host_call_count++;
    HostCallSite *site = &ctx->host_calls[idx];
    site->qualified = arena_strdup(ctx->arena, qualified);
    /* Build ns and fn copies. */
    size_t ns_len = (size_t)(dot - qualified);
    char *ns_buf = (char *)arena_alloc(ctx->arena, ns_len + 1);
    memcpy(ns_buf, qualified, ns_len);
    ns_buf[ns_len] = '\0';
    site->ns = ns_buf;
    site->fn = arena_strdup(ctx->arena, dot + 1);
    site->scratch_off = WASM_HOST_SCRATCH_BASE
                        + idx * WASM_HOST_SCRATCH_STRIDE;
    return idx;
}

/* Find the registered host-call site by qualified name. Returns NULL if
 * the call was not pre-registered (which should never happen if the
 * pre-pass ran). */
static const HostCallSite *host_call_lookup(EmitCtx *ctx,
                                             const char *qualified) {
    int i;
    if (!qualified) return NULL;
    for (i = 0; i < ctx->host_call_count; i++) {
        if (strcmp(ctx->host_calls[i].qualified, qualified) == 0) {
            return &ctx->host_calls[i];
        }
    }
    return NULL;
}

/* Pre-pass: walk every IR_HOST_CALL in the module and ensure each
 * unique qualified name has a registered scratch slot. */
static void preregister_host_calls(EmitCtx *ctx) {
    IrFunction *fn;
    for (fn = ctx->module->functions; fn; fn = fn->next) {
        IrBasicBlock *bb;
        for (bb = fn->entry; bb; bb = bb->next) {
            IrInst *inst;
            for (inst = bb->first; inst; inst = inst->next) {
                if (inst->op == IR_HOST_CALL && inst->fn_name) {
                    (void)host_call_register(ctx, inst->fn_name);
                }
            }
        }
    }
}

/* ============================================================
 * Entropy fence (L11)
 *
 * Map a qualified host-call name to its entropy cost in bits. The
 * table is intentionally small and hardcoded for stage0: capabilities
 * that probe a non-deterministic external service (an LLM) consume
 * entropy; passive readers (kb.search, data.read, http.fetch — yes,
 * network responses are non-deterministic too but timing-only
 * entropy is out of scope) do not. Per-agent `vdag:agent` imports
 * default to 0 and can be raised later by per-agent declaration.
 *
 * Cost values:
 *   llm.classify  -> 1   (default; refined when we measure log-prob)
 *   llm.chat      -> 4   (longer responses, higher uncertainty)
 *   http.fetch    -> 0   (network-response timing not in scope yet)
 *   kb.search     -> 0
 *   data.read     -> 0
 *   json.x        -> 0   (deterministic byte manipulation)
 *   vdag:agent    -> 0   (per-agent override default)
 * ============================================================ */
static int entropy_cost_for(const char *qname) {
    if (!qname) return 0;
    if (strcmp(qname, "llm.classify") == 0) return 1;
    if (strcmp(qname, "llm.chat")     == 0) return 4;
    return 0;
}

/* Walk an AST_AGENT looking for an `entropy_budget: <int>` field and
 * return the integer literal value if found. Returns -1 if the agent
 * doesn't declare entropy_budget in scalar form. The legacy block
 * shape (`entropy_budget: { max_avg_entropy: ... }`) is intentionally
 * not interpreted here — it carries no bit-count and predates the
 * runtime fence, so we treat it as "no declared budget". */
static int agent_entropy_budget_bits(AstNode *agent) {
    if (!agent || agent->kind != AST_AGENT) return -1;
    AstNode *f;
    for (f = agent->params; f; f = f->next) {
        if (f->kind != AST_FIELD || !f->name) continue;
        if (strcmp(f->name, "entropy_budget") != 0) continue;
        if (!f->right) return -1;
        /* Scalar form: `entropy_budget: 10` -> AST_INT_LIT */
        if (f->right->kind == AST_INT_LIT) {
            int64_t v = f->right->val.int_val;
            if (v < 0)                       return 0;
            if (v > WASM_ENTROPY_UNBOUNDED)  return WASM_ENTROPY_UNBOUNDED;
            return (int)v;
        }
        /* Block form: legacy, no scalar bit count -- skip. */
        return -1;
    }
    return -1;
}

/* Walk the program-level AST and capture the first agent's
 * entropy_budget scalar. Stage0 supports a single agent per module
 * for the WASM emit path; if multiple agents exist we take the first
 * one that declares a scalar budget. */
static void scan_entropy_budget(EmitCtx *ctx, AstNode *program) {
    ctx->entropy_budget_bits     = WASM_ENTROPY_UNBOUNDED;
    ctx->entropy_budget_declared = 0;
    if (!program || program->kind != AST_PROGRAM) return;
    AstNode *decl;
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind != AST_AGENT) continue;
        int bits = agent_entropy_budget_bits(decl);
        if (bits >= 0) {
            ctx->entropy_budget_bits     = bits;
            ctx->entropy_budget_declared = 1;
            return;
        }
    }
}

/* Emit the per-call-site fence: load $entropy_remaining, compare against
 * the cost, return EntropyExceeded if the call would underflow, otherwise
 * subtract and proceed. The fence is structurally local to the host-call
 * site so it composes with the existing dispatch-loop CFG without
 * needing a new IR opcode or basic block.
 *
 * `ret_type` is the enclosing function's return type so we can push the
 * sentinel as the correct WASM value type before `return`. Limceron
 * host-call sites in practice produce i64 (their result flows through
 * Limceron int math), but the fence has to be polymorphic enough that
 * a `fn foo() -> bool { llm.classify(x)? }` still validates. */
static void emit_entropy_fence(EmitCtx *ctx, int cost, IrType ret_type) {
    FILE *out = ctx->out;
    if (cost <= 0) return;
    /* Push the sentinel using the function's return type. For void
     * returns we emit a bare `return` (no value). */
    const char *sentinel_push;
    char buf[64];
    switch (ret_type) {
    case IR_TYPE_F64:
        snprintf(buf, sizeof(buf),
                 "        f64.const %d  ;; HostError.EntropyExceeded",
                 WASM_HOST_ERR_ENTROPY_EXC);
        sentinel_push = buf;
        break;
    case IR_TYPE_BOOL:
    case IR_TYPE_PTR:
    case IR_TYPE_STRING:
    case IR_TYPE_STRUCT:
        snprintf(buf, sizeof(buf),
                 "        i32.const %d  ;; HostError.EntropyExceeded",
                 WASM_HOST_ERR_ENTROPY_EXC);
        sentinel_push = buf;
        break;
    case IR_TYPE_VOID:
        sentinel_push = NULL;
        break;
    case IR_TYPE_I64:
    default:
        snprintf(buf, sizeof(buf),
                 "        i64.const %d  ;; HostError.EntropyExceeded",
                 WASM_HOST_ERR_ENTROPY_EXC);
        sentinel_push = buf;
        break;
    }
    fprintf(out,
        "      ;; --- entropy fence: cost=%d ---\n"
        "      global.get %s\n"
        "      i32.const %d\n"
        "      i32.lt_s\n"
        "      if\n",
        cost, WASM_ENTROPY_GLOBAL, cost);
    if (sentinel_push) {
        fprintf(out, "%s\n", sentinel_push);
    }
    fprintf(out,
        "        return\n"
        "      end\n"
        "      global.get %s\n"
        "      i32.const %d\n"
        "      i32.sub\n"
        "      global.set %s\n",
        WASM_ENTROPY_GLOBAL, cost, WASM_ENTROPY_GLOBAL);
}

/* ============================================================
 * Budget fence (L13)
 *
 * Maps each qualified host-call name to a per-call token estimate and a
 * per-call cost estimate in micro-USD. Both tables are stage0-hardcoded
 * and intentionally conservative:
 *
 *   llm.classify -> 100 tokens, 500   micro-USD (~$0.0005)
 *   llm.chat     -> 1000 tokens, 30000 micro-USD (~$0.03)
 *   others       -> 0 tokens, 0 micro-USD       (no fence overhead)
 *
 * Actual reconciliation lives in Visual-DAG's cost.Guard; the wasm
 * fence is the inner hard cap so a runaway loop refuses to dispatch
 * once the declared envelope is exhausted.
 * ============================================================ */

static int token_cost_for(const char *qname) {
    if (!qname) return 0;
    if (strcmp(qname, "llm.classify") == 0) return 100;
    if (strcmp(qname, "llm.chat")     == 0) return 1000;
    return 0;
}

static int64_t cost_micro_usd_for(const char *qname) {
    if (!qname) return 0;
    if (strcmp(qname, "llm.classify") == 0) return 500;
    if (strcmp(qname, "llm.chat")     == 0) return 30000;
    return 0;
}

/* Extract a scalar numeric value from a `budget` block field. Accepts
 * both AST_INT_LIT (e.g. `max_tokens: 5000`) and AST_FLOAT_LIT (e.g.
 * `max_cost: 0.05`) and returns the value as a double. Returns 0 on
 * any unsupported shape; callers check `*found` to distinguish
 * "declared as 0" from "not declared". */
static double budget_field_value(AstNode *field, int *found) {
    *found = 0;
    if (!field || !field->right) return 0.0;
    if (field->right->kind == AST_INT_LIT) {
        *found = 1;
        return (double)field->right->val.int_val;
    }
    if (field->right->kind == AST_FLOAT_LIT) {
        *found = 1;
        return field->right->val.float_val;
    }
    return 0.0;
}

/* Walk an AST_AGENT looking for a `budget: { max_tokens: ..., max_cost:
 * ... }` field. Populates the four out parameters describing what was
 * found. The legacy form `budget: BudgetName` (identifier reference)
 * is treated as "no inline declaration" -- a follow-up could resolve
 * the named budget but stage0 keeps the fence scoped to the inline
 * block form. */
static void agent_token_cost_budget(AstNode *agent,
                                    int *tokens, int *tokens_found,
                                    int64_t *cost_micro, int *cost_found) {
    *tokens = 0;
    *tokens_found = 0;
    *cost_micro = 0;
    *cost_found = 0;
    if (!agent || agent->kind != AST_AGENT) return;

    AstNode *f;
    for (f = agent->params; f; f = f->next) {
        if (f->kind != AST_FIELD || !f->name) continue;
        if (strcmp(f->name, "budget") != 0) continue;
        if (!f->right || f->right->kind != AST_BLOCK) return;

        AstNode *bf;
        for (bf = f->right->params; bf; bf = bf->next) {
            if (bf->kind != AST_FIELD || !bf->name) continue;
            int got = 0;
            double v = budget_field_value(bf, &got);
            if (!got) continue;
            if (strcmp(bf->name, "max_tokens") == 0) {
                if (v < 0) v = 0;
                if (v > (double)WASM_TOKENS_UNBOUNDED) v = WASM_TOKENS_UNBOUNDED;
                *tokens = (int)v;
                *tokens_found = 1;
            } else if (strcmp(bf->name, "max_cost") == 0) {
                /* max_cost is in whole USD (e.g. 0.05 = 5 cents). Convert
                 * to micro-USD for cheap i64 arithmetic at runtime. */
                double micro = v * 1000000.0;
                if (micro < 0) micro = 0;
                if (micro > (double)WASM_COST_UNBOUNDED)
                    micro = (double)WASM_COST_UNBOUNDED;
                *cost_micro = (int64_t)(micro + 0.5);
                *cost_found = 1;
            }
        }
        return; /* Only the first `budget:` field is honoured. */
    }
}

/* Program-level scan: take the first agent's budget block as the module
 * budget. Stage0 supports a single agent per module for the WASM emit
 * path. If no agent declares a budget, both globals init to MAX so the
 * fence is a no-op. */
static void scan_budget(EmitCtx *ctx, AstNode *program) {
    ctx->tokens_budget          = WASM_TOKENS_UNBOUNDED;
    ctx->tokens_budget_declared = 0;
    ctx->cost_micro_usd_budget  = WASM_COST_UNBOUNDED;
    ctx->cost_budget_declared   = 0;
    if (!program || program->kind != AST_PROGRAM) return;

    AstNode *decl;
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind != AST_AGENT) continue;
        int tokens = 0, tokens_found = 0;
        int64_t cost_micro = 0;
        int cost_found = 0;
        agent_token_cost_budget(decl, &tokens, &tokens_found,
                                &cost_micro, &cost_found);
        if (tokens_found) {
            ctx->tokens_budget          = tokens;
            ctx->tokens_budget_declared = 1;
        }
        if (cost_found) {
            ctx->cost_micro_usd_budget  = cost_micro;
            ctx->cost_budget_declared   = 1;
        }
        if (tokens_found || cost_found) return;
    }
}

/* ============================================================
 * L12: capability.network compile-time allowlist scan + emit
 *
 * Walks the program AST for any agent whose `capabilities:` list
 * contains a parameterised `http.fetch(["host:port", ...])` entry,
 * collects the host specs into ctx->net_allow_hosts, and -- if any
 * were declared -- emits a wasm custom section
 * `vdag.capability.network.allowlist` whose payload is the
 * UTF-8 JSON `[{"verb":"http.fetch","hosts":[...]}]`.
 *
 * The custom section is module-level metadata: wat2wasm preserves
 * `(@custom "name" "payload")` blocks verbatim into the binary's
 * custom-section vector, where wazero's instantiate-time policy
 * reader picks them up (see Visual-DAG follow-up F36c).
 * ============================================================ */

static void scan_capability_network_allowlist(EmitCtx *ctx,
                                              AstNode *program) {
    ctx->net_allow_count = 0;
    if (!program || program->kind != AST_PROGRAM) return;

    AstNode *decl;
    for (decl = program->params; decl; decl = decl->next) {
        if (decl->kind != AST_AGENT) continue;
        AstNode *field;
        for (field = decl->params; field; field = field->next) {
            if (field->kind != AST_FIELD || !field->name) continue;
            if (strcmp(field->name, "capabilities") != 0) continue;
            if (!field->right || field->right->kind != AST_ARRAY) break;
            AstNode *elem;
            for (elem = field->right->params; elem; elem = elem->next) {
                if (elem->kind != AST_CAPABILITY_ITEM) continue;
                if (!elem->name) continue;
                if (strcmp(elem->name, "http.fetch") != 0) continue;
                AstNode *h;
                for (h = elem->params; h; h = h->next) {
                    if (h->kind != AST_STRING_LIT) continue;
                    if (!h->val.str_val) continue;
                    if (ctx->net_allow_count >= WASM_MAX_NET_ALLOW) break;
                    ctx->net_allow_hosts[ctx->net_allow_count++] =
                        h->val.str_val;
                }
            }
            break;
        }
        /* First agent that declares a parameterised http.fetch wins
         * (stage0 supports a single agent per wasm module). */
        if (ctx->net_allow_count > 0) return;
    }
}

/* Emit the module-level custom section. Caller must be inside the
 * `(module ...)` block. Section is omitted when no parameterised
 * allowlist was declared (bare form keeps the runtime fence as the
 * only enforcement layer).
 *
 * The JSON payload contains `"` characters which must be escaped as
 * `\"` inside the WAT string literal; wat2wasm un-escapes them on
 * its way into the binary's custom-section byte vector, so the
 * actual payload bytes are valid JSON. We render the WAT-escaped
 * form directly to ctx->out to keep the source readable. */
static void emit_capability_network_custom_section(EmitCtx *ctx) {
    FILE *out = ctx->out;
    if (ctx->net_allow_count <= 0) return;

    fprintf(out,
        "  ;; --- L12: capability.network compile-time allowlist ---\n"
        "  ;; %d host(s) declared via http.fetch([...])\n"
        "  (@custom \"vdag.capability.network.allowlist\" "
        "\"[{\\\"verb\\\":\\\"http.fetch\\\",\\\"hosts\\\":[",
        ctx->net_allow_count);
    int i;
    for (i = 0; i < ctx->net_allow_count; i++) {
        /* Host strings are validated upstream (typecheck) so they
         * are guaranteed not to contain `"` or `\` -- straight
         * concatenation is safe here. */
        fprintf(out, "%s\\\"%s\\\"",
                i == 0 ? "" : ",", ctx->net_allow_hosts[i]);
    }
    fprintf(out, "]}]\")\n");
}

/* Emit the per-call-site token fence: load $tokens_remaining, compare
 * against the per-call cost, return BudgetExceeded if it would underflow,
 * otherwise subtract and proceed. Mirrors `emit_entropy_fence` shape. */
static void emit_token_fence(EmitCtx *ctx, int cost, IrType ret_type) {
    FILE *out = ctx->out;
    if (cost <= 0) return;

    const char *sentinel_push;
    char buf[80];
    switch (ret_type) {
    case IR_TYPE_F64:
        snprintf(buf, sizeof(buf),
                 "        f64.const %d  ;; HostError.BudgetExceeded",
                 WASM_HOST_ERR_BUDGET_EXC);
        sentinel_push = buf;
        break;
    case IR_TYPE_BOOL:
    case IR_TYPE_PTR:
    case IR_TYPE_STRING:
    case IR_TYPE_STRUCT:
        snprintf(buf, sizeof(buf),
                 "        i32.const %d  ;; HostError.BudgetExceeded",
                 WASM_HOST_ERR_BUDGET_EXC);
        sentinel_push = buf;
        break;
    case IR_TYPE_VOID:
        sentinel_push = NULL;
        break;
    case IR_TYPE_I64:
    default:
        snprintf(buf, sizeof(buf),
                 "        i64.const %d  ;; HostError.BudgetExceeded",
                 WASM_HOST_ERR_BUDGET_EXC);
        sentinel_push = buf;
        break;
    }
    fprintf(out,
        "      ;; --- token fence: cost=%d ---\n"
        "      global.get %s\n"
        "      i32.const %d\n"
        "      i32.lt_s\n"
        "      if\n",
        cost, WASM_TOKENS_GLOBAL, cost);
    if (sentinel_push) {
        fprintf(out, "%s\n", sentinel_push);
    }
    fprintf(out,
        "        return\n"
        "      end\n"
        "      global.get %s\n"
        "      i32.const %d\n"
        "      i32.sub\n"
        "      global.set %s\n",
        WASM_TOKENS_GLOBAL, cost, WASM_TOKENS_GLOBAL);
}

/* Emit the per-call-site cost fence: same shape as the token fence but
 * the counter is i64 (micro-USD) so we use i64.lt_s / i64.sub. */
static void emit_cost_fence(EmitCtx *ctx, int64_t cost, IrType ret_type) {
    FILE *out = ctx->out;
    if (cost <= 0) return;

    const char *sentinel_push;
    char buf[80];
    switch (ret_type) {
    case IR_TYPE_F64:
        snprintf(buf, sizeof(buf),
                 "        f64.const %d  ;; HostError.BudgetExceeded",
                 WASM_HOST_ERR_BUDGET_EXC);
        sentinel_push = buf;
        break;
    case IR_TYPE_BOOL:
    case IR_TYPE_PTR:
    case IR_TYPE_STRING:
    case IR_TYPE_STRUCT:
        snprintf(buf, sizeof(buf),
                 "        i32.const %d  ;; HostError.BudgetExceeded",
                 WASM_HOST_ERR_BUDGET_EXC);
        sentinel_push = buf;
        break;
    case IR_TYPE_VOID:
        sentinel_push = NULL;
        break;
    case IR_TYPE_I64:
    default:
        snprintf(buf, sizeof(buf),
                 "        i64.const %d  ;; HostError.BudgetExceeded",
                 WASM_HOST_ERR_BUDGET_EXC);
        sentinel_push = buf;
        break;
    }
    fprintf(out,
        "      ;; --- cost fence: cost_micro_usd=%lld ---\n"
        "      global.get %s\n"
        "      i64.const %lld\n"
        "      i64.lt_s\n"
        "      if\n",
        (long long)cost, WASM_COST_GLOBAL, (long long)cost);
    if (sentinel_push) {
        fprintf(out, "%s\n", sentinel_push);
    }
    fprintf(out,
        "        return\n"
        "      end\n"
        "      global.get %s\n"
        "      i64.const %lld\n"
        "      i64.sub\n"
        "      global.set %s\n",
        WASM_COST_GLOBAL, (long long)cost, WASM_COST_GLOBAL);
}

/* Emit the `(import "vdag:<ns>" "<fn>" (func ...))` declarations for
 * every registered host call. Must run BEFORE any (func ...) body in
 * the module — the WAT grammar requires imports first. */
static void emit_host_imports(EmitCtx *ctx) {
    int i;
    if (ctx->host_call_count == 0) return;
    fprintf(ctx->out, "  ;; --- host imports (%d) ---\n",
            ctx->host_call_count);
    for (i = 0; i < ctx->host_call_count; i++) {
        const HostCallSite *s = &ctx->host_calls[i];
        /* Per-capability signature — must match imports.go exactly.
         * The Limceron compiler currently supports the llm.classify
         * shape; other capabilities are emitted as their declared
         * ABI but only llm.classify is exercised by 05_host_call. */
        const char *param_list;
        if (strcmp(s->qualified, "llm.classify") == 0) {
            /* (text_ptr, text_len, label_buf, label_max, conf_out) */
            param_list = "(param i32 i32 i32 i32 i32) (result i32)";
        } else if (strcmp(s->qualified, "llm.chat") == 0) {
            /* (prompt, prompt_len, system, system_len, out_buf, out_max) */
            param_list = "(param i32 i32 i32 i32 i32 i32) (result i32)";
        } else if (strcmp(s->qualified, "http.fetch") == 0) {
            /* (url, url_len, method, method_len, body, body_len,
             *  out_buf, out_max, status_out) */
            param_list = "(param i32 i32 i32 i32 i32 i32 i32 i32 i32) "
                         "(result i32)";
        } else if (strcmp(s->qualified, "kb.search") == 0) {
            /* (coll, coll_len, q, q_len, top_k, out_buf, out_max) */
            param_list = "(param i32 i32 i32 i32 i32 i32 i32) (result i32)";
        } else if (strcmp(s->qualified, "data.read") == 0) {
            /* (query, query_len, params, params_len, out_buf, out_max) */
            param_list = "(param i32 i32 i32 i32 i32 i32) (result i32)";
        } else if (strcmp(s->qualified, "json.parse") == 0) {
            /* (bytes_ptr, bytes_len, out_ptr, out_cap) -> handle/HostErr */
            param_list = "(param i32 i32 i32 i32) (result i32)";
        } else if (strcmp(s->qualified, "json.field") == 0) {
            /* (handle, key_ptr, key_len, out_ptr, out_cap) -> sub-handle */
            param_list = "(param i32 i32 i32 i32 i32) (result i32)";
        } else if (strcmp(s->qualified, "json.array_index") == 0) {
            /* (handle, idx, out_ptr, out_cap) -> sub-handle */
            param_list = "(param i32 i32 i32 i32) (result i32)";
        } else if (strcmp(s->qualified, "json.length") == 0) {
            /* (handle) -> length or HostErr */
            param_list = "(param i32) (result i32)";
        } else if (strcmp(s->qualified, "json.string_value") == 0) {
            /* (handle, out_ptr, out_cap) -> byte count */
            param_list = "(param i32 i32 i32) (result i32)";
        } else if (strcmp(s->qualified, "json.int_value") == 0) {
            /* (handle) -> i64 value (HostErr encoded negatively) */
            param_list = "(param i32) (result i64)";
        } else if (strcmp(s->qualified, "json.bool_value") == 0) {
            /* (handle) -> 0/1 (HostErr negative) */
            param_list = "(param i32) (result i32)";
        } else if (strcmp(s->qualified, "json.is_null") == 0) {
            /* (handle) -> 1 if null, 0 otherwise */
            param_list = "(param i32) (result i32)";
        } else if (strcmp(s->qualified, "json.stringify") == 0) {
            /* (handle, out_ptr, out_cap) -> byte count */
            param_list = "(param i32 i32 i32) (result i32)";
        } else {
            /* Unknown capability: emit a 4-arg/i32-result placeholder so
             * the wasm at least validates. wazero will fail to link the
             * unknown import, which is the correct failure mode. */
            param_list = "(param i32 i32 i32 i32) (result i32)";
        }
        fprintf(ctx->out,
                "  (import \"vdag:%s\" \"%s\""
                " (func $hi_%s_%s %s))\n",
                s->ns, s->fn, s->ns, s->fn, param_list);
    }
    fprintf(ctx->out, "\n");
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

    case IR_HOST_CALL: {
        /* Host-call lowering.
         *
         * Before any marshalling we emit the L11 entropy fence for
         * capabilities that consume entropy. The fence reads the
         * module-level `$entropy_remaining` global, compares it
         * against the per-capability cost from `entropy_cost_for`,
         * and either traps with HostError.EntropyExceeded (-9) or
         * subtracts the cost and proceeds.
         *
         * The buffer-protocol ABI (see imports.go) is built around
         * (ptr, len) input pairs and a (ptr, max) output pair, plus a
         * scalar status slot for some capabilities (confidence f64 for
         * llm.classify, status i32 for http.fetch). The pre-pass
         * (preregister_host_calls) gave us a stable scratch base for
         * this site; the per-capability marshalling below pushes the
         * appropriate set of i32 operands and then calls the imported
         * function.
         *
         * The host fn returns an i32: positive = bytes written into
         * the out buffer, negative = HostErr* code. We sign-extend
         * into the i64 SSA slot the IR allocated for us so the value
         * composes with the rest of Limceron's integer math without
         * an explicit cast.
         */
        const char *qname = inst->fn_name ? inst->fn_name : "?";
        const HostCallSite *s = host_call_lookup(fctx->gctx, qname);
        if (!s) {
            /* Should never happen: pre-pass registers every site. */
            fprintf(out,
                    "      ;; UNRESOLVED host call %s -- emitting drop\n",
                    qname);
            fprintf(out, "      i64.const -1\n");
            emit_set_value(fctx, inst->id);
            break;
        }

        /* L11: entropy fence -- decrement and bounds-check the
         * agent-declared bit budget before dispatching the call. */
        emit_entropy_fence(fctx->gctx,
                           entropy_cost_for(qname),
                           fctx->fn->return_type);

        /* L13: token + cost fences. Order matters: entropy first,
         * then tokens, then cost. The first counter to underflow
         * short-circuits the function so callers see a consistent
         * failure-mode ordering across replays. */
        emit_token_fence(fctx->gctx,
                         token_cost_for(qname),
                         fctx->fn->return_type);
        emit_cost_fence(fctx->gctx,
                        cost_micro_usd_for(qname),
                        fctx->fn->return_type);

        int out_buf = s->scratch_off;
        int conf_off = s->scratch_off + WASM_HOST_OUTBUF_MAX;
        int status_off = conf_off + 8;
        (void)status_off;

        if (strcmp(qname, "llm.classify") == 0 &&
            inst->call_arg_count >= 1) {
            /* (text_ptr, text_len, label_buf, label_max, conf_out) */
            int text_arg = inst->call_args[0];
            emit_get_value(fctx, text_arg);             /* prompt ptr (i32) */
            emit_get_value(fctx, text_arg);             /* dup for len load */
            fprintf(out, "      i32.const 4\n");
            fprintf(out, "      i32.sub\n");
            fprintf(out, "      i32.load\n");           /* prompt len from prefix */
            fprintf(out, "      i32.const %d\n", out_buf);
            fprintf(out, "      i32.const %d\n", WASM_HOST_OUTBUF_MAX);
            fprintf(out, "      i32.const %d\n", conf_off);
            fprintf(out, "      call $hi_%s_%s\n", s->ns, s->fn);
        } else if (strcmp(qname, "llm.chat") == 0 &&
                   inst->call_arg_count >= 2) {
            /* (prompt, prompt_len, system, system_len, out_buf, out_max) */
            int p_arg = inst->call_args[0];
            int sys_arg = inst->call_args[1];
            emit_get_value(fctx, p_arg);
            emit_get_value(fctx, p_arg);
            fprintf(out, "      i32.const 4\n      i32.sub\n      i32.load\n");
            emit_get_value(fctx, sys_arg);
            emit_get_value(fctx, sys_arg);
            fprintf(out, "      i32.const 4\n      i32.sub\n      i32.load\n");
            fprintf(out, "      i32.const %d\n", out_buf);
            fprintf(out, "      i32.const %d\n", WASM_HOST_OUTBUF_MAX);
            fprintf(out, "      call $hi_%s_%s\n", s->ns, s->fn);
        } else if (strcmp(qname, "kb.search") == 0 &&
                   inst->call_arg_count >= 3) {
            /* (coll, coll_len, q, q_len, top_k, out_buf, out_max) */
            int coll_arg = inst->call_args[0];
            int q_arg = inst->call_args[1];
            int k_arg = inst->call_args[2];
            emit_get_value(fctx, coll_arg);
            emit_get_value(fctx, coll_arg);
            fprintf(out, "      i32.const 4\n      i32.sub\n      i32.load\n");
            emit_get_value(fctx, q_arg);
            emit_get_value(fctx, q_arg);
            fprintf(out, "      i32.const 4\n      i32.sub\n      i32.load\n");
            emit_get_value(fctx, k_arg);
            /* top_k arrives as i64 in Limceron; truncate to i32. */
            fprintf(out, "      i32.wrap_i64\n");
            fprintf(out, "      i32.const %d\n", out_buf);
            fprintf(out, "      i32.const %d\n", WASM_HOST_OUTBUF_MAX);
            fprintf(out, "      call $hi_%s_%s\n", s->ns, s->fn);
        } else if (strcmp(qname, "data.read") == 0 &&
                   inst->call_arg_count >= 2) {
            /* (query, query_len, params, params_len, out_buf, out_max) */
            int q_arg = inst->call_args[0];
            int p_arg = inst->call_args[1];
            emit_get_value(fctx, q_arg);
            emit_get_value(fctx, q_arg);
            fprintf(out, "      i32.const 4\n      i32.sub\n      i32.load\n");
            emit_get_value(fctx, p_arg);
            emit_get_value(fctx, p_arg);
            fprintf(out, "      i32.const 4\n      i32.sub\n      i32.load\n");
            fprintf(out, "      i32.const %d\n", out_buf);
            fprintf(out, "      i32.const %d\n", WASM_HOST_OUTBUF_MAX);
            fprintf(out, "      call $hi_%s_%s\n", s->ns, s->fn);
        } else if (strcmp(qname, "http.fetch") == 0 &&
                   inst->call_arg_count >= 3) {
            /* (url, url_len, method, method_len, body, body_len,
             *  out_buf, out_max, status_out) */
            int url_arg = inst->call_args[0];
            int meth_arg = inst->call_args[1];
            int body_arg = inst->call_args[2];
            emit_get_value(fctx, url_arg);
            emit_get_value(fctx, url_arg);
            fprintf(out, "      i32.const 4\n      i32.sub\n      i32.load\n");
            emit_get_value(fctx, meth_arg);
            emit_get_value(fctx, meth_arg);
            fprintf(out, "      i32.const 4\n      i32.sub\n      i32.load\n");
            emit_get_value(fctx, body_arg);
            emit_get_value(fctx, body_arg);
            fprintf(out, "      i32.const 4\n      i32.sub\n      i32.load\n");
            fprintf(out, "      i32.const %d\n", out_buf);
            fprintf(out, "      i32.const %d\n", WASM_HOST_OUTBUF_MAX);
            fprintf(out, "      i32.const %d\n", conf_off);
            fprintf(out, "      call $hi_%s_%s\n", s->ns, s->fn);
        } else if (strcmp(qname, "json.parse") == 0 &&
                   inst->call_arg_count >= 1) {
            /* (bytes_ptr, bytes_len, out_ptr, out_cap) -> handle/HostErr.
             * The bytes come from a Limceron string (ptr + len-prefix). */
            int b_arg = inst->call_args[0];
            emit_get_value(fctx, b_arg);
            emit_get_value(fctx, b_arg);
            fprintf(out, "      i32.const 4\n      i32.sub\n      i32.load\n");
            fprintf(out, "      i32.const %d\n", out_buf);
            fprintf(out, "      i32.const %d\n", WASM_HOST_OUTBUF_MAX);
            fprintf(out, "      call $hi_%s_%s\n", s->ns, s->fn);
            fprintf(out, "      i64.extend_i32_s\n");
            emit_set_value(fctx, inst->id);
            break;
        } else if (strcmp(qname, "json.field") == 0 &&
                   inst->call_arg_count >= 2) {
            /* (handle, key_ptr, key_len, out_ptr, out_cap) -> sub-handle.
             * Handle arrives as i64 (Limceron int); truncate to i32. */
            int h_arg = inst->call_args[0];
            int k_arg = inst->call_args[1];
            emit_get_value(fctx, h_arg);
            fprintf(out, "      i32.wrap_i64\n");
            emit_get_value(fctx, k_arg);
            emit_get_value(fctx, k_arg);
            fprintf(out, "      i32.const 4\n      i32.sub\n      i32.load\n");
            fprintf(out, "      i32.const %d\n", out_buf);
            fprintf(out, "      i32.const %d\n", WASM_HOST_OUTBUF_MAX);
            fprintf(out, "      call $hi_%s_%s\n", s->ns, s->fn);
            fprintf(out, "      i64.extend_i32_s\n");
            emit_set_value(fctx, inst->id);
            break;
        } else if (strcmp(qname, "json.array_index") == 0 &&
                   inst->call_arg_count >= 2) {
            /* (handle, idx, out_ptr, out_cap) -> sub-handle.
             * Both handle and idx are i64 in Limceron; truncate. */
            int h_arg = inst->call_args[0];
            int i_arg = inst->call_args[1];
            emit_get_value(fctx, h_arg);
            fprintf(out, "      i32.wrap_i64\n");
            emit_get_value(fctx, i_arg);
            fprintf(out, "      i32.wrap_i64\n");
            fprintf(out, "      i32.const %d\n", out_buf);
            fprintf(out, "      i32.const %d\n", WASM_HOST_OUTBUF_MAX);
            fprintf(out, "      call $hi_%s_%s\n", s->ns, s->fn);
            fprintf(out, "      i64.extend_i32_s\n");
            emit_set_value(fctx, inst->id);
            break;
        } else if (strcmp(qname, "json.length") == 0 &&
                   inst->call_arg_count >= 1) {
            /* (handle) -> length or HostErr. */
            int h_arg = inst->call_args[0];
            emit_get_value(fctx, h_arg);
            fprintf(out, "      i32.wrap_i64\n");
            fprintf(out, "      call $hi_%s_%s\n", s->ns, s->fn);
            fprintf(out, "      i64.extend_i32_s\n");
            emit_set_value(fctx, inst->id);
            break;
        } else if (strcmp(qname, "json.string_value") == 0 &&
                   inst->call_arg_count >= 1) {
            /* (handle, out_ptr, out_cap) -> byte count or HostErr. */
            int h_arg = inst->call_args[0];
            emit_get_value(fctx, h_arg);
            fprintf(out, "      i32.wrap_i64\n");
            fprintf(out, "      i32.const %d\n", out_buf);
            fprintf(out, "      i32.const %d\n", WASM_HOST_OUTBUF_MAX);
            fprintf(out, "      call $hi_%s_%s\n", s->ns, s->fn);
            fprintf(out, "      i64.extend_i32_s\n");
            emit_set_value(fctx, inst->id);
            break;
        } else if (strcmp(qname, "json.int_value") == 0 &&
                   inst->call_arg_count >= 1) {
            /* (handle) -> i64 value directly (NO sign-extend). */
            int h_arg = inst->call_args[0];
            emit_get_value(fctx, h_arg);
            fprintf(out, "      i32.wrap_i64\n");
            fprintf(out, "      call $hi_%s_%s\n", s->ns, s->fn);
            /* Result is already i64 — drop into the SSA slot as-is. */
            emit_set_value(fctx, inst->id);
            break;
        } else if (strcmp(qname, "json.bool_value") == 0 &&
                   inst->call_arg_count >= 1) {
            /* (handle) -> 0/1 or negative HostErr. */
            int h_arg = inst->call_args[0];
            emit_get_value(fctx, h_arg);
            fprintf(out, "      i32.wrap_i64\n");
            fprintf(out, "      call $hi_%s_%s\n", s->ns, s->fn);
            fprintf(out, "      i64.extend_i32_s\n");
            emit_set_value(fctx, inst->id);
            break;
        } else if (strcmp(qname, "json.is_null") == 0 &&
                   inst->call_arg_count >= 1) {
            /* (handle) -> 1 if null, 0 otherwise. */
            int h_arg = inst->call_args[0];
            emit_get_value(fctx, h_arg);
            fprintf(out, "      i32.wrap_i64\n");
            fprintf(out, "      call $hi_%s_%s\n", s->ns, s->fn);
            fprintf(out, "      i64.extend_i32_s\n");
            emit_set_value(fctx, inst->id);
            break;
        } else if (strcmp(qname, "json.stringify") == 0 &&
                   inst->call_arg_count >= 1) {
            /* (handle, out_ptr, out_cap) -> byte count of serialised form. */
            int h_arg = inst->call_args[0];
            emit_get_value(fctx, h_arg);
            fprintf(out, "      i32.wrap_i64\n");
            fprintf(out, "      i32.const %d\n", out_buf);
            fprintf(out, "      i32.const %d\n", WASM_HOST_OUTBUF_MAX);
            fprintf(out, "      call $hi_%s_%s\n", s->ns, s->fn);
            fprintf(out, "      i64.extend_i32_s\n");
            emit_set_value(fctx, inst->id);
            break;
        } else {
            /* Unknown / mismatched arity: emit best-effort (push each
             * arg as-is and trust the import declaration). */
            int i;
            for (i = 0; i < inst->call_arg_count; i++) {
                emit_get_value(fctx, inst->call_args[i]);
            }
            fprintf(out, "      call $hi_%s_%s\n", s->ns, s->fn);
        }

        /* Result is i32; the SSA slot is i64. Sign-extend so negative
         * HostErr* codes propagate correctly through Limceron's
         * integer comparisons. */
        fprintf(out, "      i64.extend_i32_s\n");
        emit_set_value(fctx, inst->id);
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

    /* Host imports must appear before any other (func ...) declaration
     * per the WAT grammar. The (import ...) entries map the Limceron
     * "vdag:<ns>" / "<fn>" pairs to host fns provided by wazero (see
     * imports.go for the matching wiring). */
    emit_host_imports(ctx);

    /* Linear memory: one 64 KiB page, exported. */
    fprintf(out, "  (memory (export \"memory\") 1)\n");

    /* Bump-pointer global for ALLOCA, if any function uses it. We
     * declare it unconditionally -- it's a tiny overhead and keeps
     * the WAT shape stable across modules. The initial value points
     * just past the static-data area AND any host-call scratch
     * region we reserved. */
    int bump_init = ctx->data_offset > 0 ? ctx->data_offset : WASM_DATA_BASE;
    if (ctx->host_call_count > 0) {
        int scratch_end = WASM_HOST_SCRATCH_BASE
                          + ctx->host_call_count * WASM_HOST_SCRATCH_STRIDE;
        if (scratch_end > bump_init) bump_init = scratch_end;
    }
    /* Round up to 16-byte alignment for tidy heap base. */
    bump_init = (bump_init + 15) & ~15;
    fprintf(out, "  (global %s (mut i32) (i32.const %d))\n",
            WASM_BUMP_PTR_GLOBAL, bump_init);

    /* L11: entropy budget global. Always emitted so the fence has a
     * stable target; defaults to INT32_MAX (no-op) when the agent did
     * not declare a scalar budget. */
    if (ctx->entropy_budget_declared) {
        fprintf(out,
                "  ;; agent-declared entropy budget: %d bits\n",
                ctx->entropy_budget_bits);
    } else {
        fprintf(out,
                "  ;; no entropy_budget declared -- fence is a no-op\n");
    }
    fprintf(out, "  (global %s (mut i32) (i32.const %d))\n",
            WASM_ENTROPY_GLOBAL, ctx->entropy_budget_bits);

    /* L13: token + cost budget globals. Same no-op-on-undeclared shape
     * as the entropy global. The token global is i32, the cost global
     * is i64 (micro-USD) so cents-level granularity fits without
     * float-arithmetic in the fence. */
    if (ctx->tokens_budget_declared) {
        fprintf(out,
                "  ;; agent-declared token budget: %d tokens\n",
                ctx->tokens_budget);
    } else {
        fprintf(out,
                "  ;; no max_tokens declared -- token fence is a no-op\n");
    }
    fprintf(out, "  (global %s (mut i32) (i32.const %d))\n",
            WASM_TOKENS_GLOBAL, ctx->tokens_budget);

    if (ctx->cost_budget_declared) {
        fprintf(out,
                "  ;; agent-declared cost budget: %lld micro-USD\n",
                (long long)ctx->cost_micro_usd_budget);
    } else {
        fprintf(out,
                "  ;; no max_cost declared -- cost fence is a no-op\n");
    }
    fprintf(out, "  (global %s (mut i64) (i64.const %lld))\n",
            WASM_COST_GLOBAL, (long long)ctx->cost_micro_usd_budget);

    /* L12: capability.network compile-time allowlist custom section.
     * Emitted only if the agent used the parameterised form
     * `http.fetch(["host:port", ...])`. The runtime reads this
     * section at instantiate time and refuses the module if any
     * declared host:port is not in its allowlist. */
    emit_capability_network_custom_section(ctx);

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
    int has_agent_main = 0;
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
        if (strcmp(uname, "agent_main") == 0) has_agent_main = 1;
    }

    /* Agent runtime ABI: Visual-DAG's TestReactLoopE2E invokes wazero
     * with EntryPoint="agent_main", so for every agent-scoped `fn main`
     * we also expose an `agent_main` export aliasing `lcn_main`. The
     * Limceron trampoline (irgen_emit_main_trampoline) makes lcn_main
     * the canonical entrypoint regardless of the original agent's name.
     */
    if (has_main && !has_agent_main) {
        fprintf(out, "  (export \"agent_main\" (func $lcn_main))\n");
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
    /* `--enable-annotations` is required for `(@custom "name" "data")`
     * annotation blocks (e.g. the L12 capability.network allowlist) to
     * be propagated into the binary's custom-section vector. The flag
     * is no-op for modules that don't use any annotations, so we set
     * it unconditionally. */
    snprintf(cmd, sizeof(cmd),
             "wat2wasm --debug-names --enable-annotations -o %s %s 2>&1",
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
    preregister_host_calls(&ctx);
    scan_entropy_budget(&ctx, program);
    scan_budget(&ctx, program);
    scan_capability_network_allowlist(&ctx, program);
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
     * metadata: a missing-agents source shouldn't fail the wasm build.
     *
     * Gated by lcn_set_emit_wit (default ON for wasm32-wasi-preview2
     * via cmd_build; can be flipped with --no-emit-wit). The L1b
     * requirement is "always co-emit alongside output.wasm" -- the
     * unconditional invocation lives here so the wasm path always
     * produces both artefacts unless explicitly disabled. */
    if (lcn_emit_wit_enabled()) {
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

/* ============================================================
 * Test helper: emit WAT to a FILE* (no wat2wasm step).
 *
 * Used by test_ir.c to assert on the textual WAT shape (e.g. that
 * the entropy fence emits the expected sequence at host-call sites)
 * without depending on wat2wasm or wasmtime being installed.
 *
 * Mirrors lcn_emit_wasm's pre-passes verbatim so the WAT we
 * produce here is byte-identical to what wat2wasm would consume.
 * ============================================================ */
int lcn_emit_wasm_wat(AstNode *program, FILE *out, Arena *arena,
                      const LcnTarget *target) {
    if (!program || !out || !arena) return 1;

    IrModule *mod = ir_gen_program(program, arena);
    if (!mod) return 1;

    EmitCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.module      = mod;
    ctx.arena       = arena;
    ctx.target      = target;
    ctx.data_offset = WASM_DATA_BASE;
    ctx.out         = out;

    scan_module_features(&ctx);
    preintern_strings(&ctx);
    preregister_host_calls(&ctx);
    scan_entropy_budget(&ctx, program);
    scan_budget(&ctx, program);
    scan_capability_network_allowlist(&ctx, program);
    ctx.data_total  = ctx.data_offset;

    emit_module(out, mod, &ctx);
    return 0;
}
