# L13 -- budget runtime fence

The Limceron compiler turns each agent's
`budget: { max_tokens: <int>, max_cost: <float> }` declaration into
two wasm-level runtime fences. Every cost-tagged host call decrements
a token counter and a micro-USD counter, and traps with a new
`HostError` sentinel if either counter would go negative. The fences
are structurally local to each host-call site, so they compose with
the existing `Result<T, HostError>` / `?` / `try-catch` machinery
without any source-level changes.

The L13 fence chains with L11's entropy fence: at every call site the
emission order is **entropy first, then tokens, then cost**. The first
counter to trip wins and short-circuits the function. That gives
replays a deterministic failure-mode ordering regardless of which
counter would also have exceeded.

## Samples

| File | Budget | Calls | Outcome |
|------|-------|------:|---------|
| `01_within_budget.lceron` | `max_tokens: 5000`, `max_cost: 0.05` | 2 x `llm.classify` (cost 200 tokens / 1000 micro-USD) | `Ok(N)` -- non-negative |
| `02_exceeds_budget.lceron` | `max_tokens: 50`, `max_cost: 0.001` | 1 x `llm.classify` (would cost 100 tokens) | `Err(-10)` on the first call |

Build and inspect:

```sh
./build/limceron-stage0 build examples/wasm/budget/01_within_budget.lceron \
    --target wasm32-wasi-preview2 -o /tmp/within.wasm

./build/limceron-stage0 build examples/wasm/budget/02_exceeds_budget.lceron \
    --target wasm32-wasi-preview2 -o /tmp/exceeds.wasm
```

## Emitted shape

The wasm module gets two mutable globals initialised to the declared
budgets (or `INT*_MAX` sentinels if none was declared):

```wat
(global $tokens_remaining (mut i32) (i32.const 5000))
(global $cost_micro_usd_remaining (mut i64) (i64.const 50000))
```

`max_cost` is converted from whole USD to micro-USD at compile time
(`0.05` -> `50000`) so the runtime counter stays in i64 land and
avoids float arithmetic inside the fence.

At every cost-tagged host-call site the compiler emits both fences
immediately after the entropy fence, before the actual
`call $hi_<ns>_<fn>`:

```wat
;; --- entropy fence: cost=1 ---     ;; L11
...

;; --- token fence: cost=100 ---     ;; L13
global.get $tokens_remaining
i32.const 100
i32.lt_s
if
  i64.const -10  ;; HostError.BudgetExceeded
  return
end
global.get $tokens_remaining
i32.const 100
i32.sub
global.set $tokens_remaining

;; --- cost fence: cost_micro_usd=500 ---   ;; L13
global.get $cost_micro_usd_remaining
i64.const 500
i64.lt_s
if
  i64.const -10  ;; HostError.BudgetExceeded
  return
end
global.get $cost_micro_usd_remaining
i64.const 500
i64.sub
global.set $cost_micro_usd_remaining
```

The sentinel push (`i64.const -10` above) is adapted to the enclosing
function's return type: `f64.const` for float-returning fns,
`i32.const` for bool/string/ptr fns, plain `return` for void.

## Host-call cost tables

The compiler maps each qualified host-call name to a per-call token
estimate and a per-call cost estimate in micro-USD. Both tables live in
`src/ir_emit_wasm.c::token_cost_for` and `cost_micro_usd_for`.

| Qualified name | Tokens (per call) | Cost (micro-USD per call) | Notes |
|----------------|------------------:|--------------------------:|-------|
| `llm.classify` | 100 | 500 (~$0.0005) | Conservative input+output combined; refine later. |
| `llm.chat`     | 1000 | 30000 (~$0.03) | Longer responses; refine later. |
| `http.fetch`   | 0 | 0 | Network IO does not consume tokens / direct LLM cost. |
| `kb.search`    | 0 | 0 | Passive vector lookup. |
| `data.read`    | 0 | 0 | Read-only relational query. |
| `json.*`       | 0 | 0 | Deterministic byte manipulation. |

These are the inner hard cap. The authoritative reconciliation
(against the actual provider invoice) happens in Visual-DAG's
`cost.Guard`; the wasm fence simply guarantees that a single
Limceron program refuses to dispatch a call that would obviously
overshoot the declared envelope.

Costs are stage0-hardcoded; promoting the tables to per-target
configuration is tracked separately.

## HostError variants

Mirrors `Visual-DAG/internal/nodes/code/imports.go`:

| Sentinel | Value | Source |
|----------|------:|--------|
| `Generic`         | -1 | All hosts |
| `URLNotAllowed`   | -2 | http.fetch |
| `BudgetExceeded`  | -3 | budget tracker (legacy) |
| `NoTenant`        | -4 | tenant context |
| `DMLRejected`     | -5 | data.read |
| `TooLarge`        | -6 | out-buffer overflow |
| `CapMissing`      | -7 | capability check |
| `InvalidArg`      | -8 | argument validation |
| `EntropyExceeded` | -9 | L11 entropy fence |
| **`BudgetExceeded`** | **-10** | **L13 token / cost fence (new)** |

> Note on the `-3 BudgetExceeded` legacy slot: that variant predates the
> compiled wasm fence and refers to the runtime's outer budget tracker
> for non-wasm execution. The L13 wasm fence introduces a distinct
> sentinel `-10` so wasm-internal hard-cap breaches are
> distinguishable from outer tracker decisions.

The Limceron-side encoding is unchanged from L5: positive i64 = `Ok(v)`,
negative i64 = `Err(sentinel)`. `?` on a negative value short-circuits
the enclosing function (or innermost `try/catch`).

## Chain-with-L11 emission order

At each cost-tagged host-call site the compiler emits fences in this
order:

1. **Entropy fence** (L11) -- trips first on
   `HostError.EntropyExceeded = -9`
2. **Token fence** (L13) -- trips next on
   `HostError.BudgetExceeded = -10`
3. **Cost fence** (L13) -- trips last on
   `HostError.BudgetExceeded = -10`

The first counter whose remaining value is less than the call's per-call
estimate short-circuits the function with the corresponding sentinel.
This ordering is asserted by
`test/test_ir.c::wasm_budget_chain_order_entropy_then_tokens_then_cost`.

## Visual-DAG side gap

`internal/nodes/code/imports.go` must learn about the new
`HostErrBudgetExceeded = -10` sentinel and propagate it through the
`code.limceron` Activity's output as a non-retryable error code so an
upstream workflow can branch on it without burning retries. Tracked
as the follow-up to this commit; see the commit message for details.

## What ADR this unblocks

Visual-DAG ADR-0016 specifies `agent.compiled@1.0` which mandates
hard-cap budget enforcement at the wasm boundary. The L13 fence
satisfies the wasm-level invariant that contract assumes:

- A compiled agent **must** refuse to dispatch a host call whose
  per-call estimate would overshoot the declared `max_tokens` or
  `max_cost`.
- A breach **must** surface as a deterministic sentinel
  (`HostError.BudgetExceeded = -10`) so replays observe the same
  failure mode as the original run.
