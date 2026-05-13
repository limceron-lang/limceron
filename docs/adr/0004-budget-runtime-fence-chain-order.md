# ADR 0004 — Budget runtime fence + entropy/tokens/cost chain order (L13)

## Status
Accepted (2026-05-13). Implemented in commit `017e7b2`.

## Context

ADR-0003 introduced the entropy fence. L13 mirrors that shape for the
two budget dimensions stage0 already tracks at the language level:

```limceron
budget: { max_tokens: 1000, max_cost: 0.03 }
```

Pre-L13 these fields parsed but did not enforce anything at runtime.
The semantic intent:

> Every host call decrements `max_tokens` by a per-call token cost
> and `max_cost` by a per-call cost-in-USD. When the next call would
> drive either counter negative, the call refuses with
> `HostError.BudgetExceeded = -10`.

Constraints inherited from L11:

- No new IR opcodes.
- Determinism: counters must be module-scoped, observable only via
  the host-import boundary.
- Composition: an agent that declared `entropy_budget` AND `budget`
  must fence in a deterministic order so replay reproduces the same
  failure mode.

## Decision

**Two new wasm globals per agent — `$tokens_remaining (i32)` and
`$cost_micro_usd_remaining (i64)`. Per-call decrement-and-bounds-check
fences chain after the L11 entropy fence in the order
entropy → tokens → cost. First counter to trip wins.**

### Globals

```wat
(global $tokens_remaining (mut i32) (i32.const <max_tokens>))
(global $cost_micro_usd_remaining (mut i64) (i64.const <max_cost*1e6>))
```

`max_cost` (whole USD float like `0.05`) is converted to micro-USD at
compile time so the fence stays in i64 land and never executes float
arithmetic.

Both globals default to `INT*_MAX` sentinels when undeclared.

### Per-site fences

```wat
;; --- token fence: cost=<N> ---
global.get $tokens_remaining
i32.const <N>
i32.lt_s
if
  i64.const -10  ;; HostError.BudgetExceeded
  return
end
global.get $tokens_remaining
i32.const <N>
i32.sub
global.set $tokens_remaining

;; --- cost fence: cost_micro_usd=<C> ---
global.get $cost_micro_usd_remaining
i64.const <C>
i64.lt_s
if
  i64.const -10
  return
end
global.get $cost_micro_usd_remaining
i64.const <C>
i64.sub
global.set $cost_micro_usd_remaining
```

Sentinel push is return-type-aware (i32 / i64 / f64 / void) to match
the enclosing fn's signature, identical to the L11 fence.

### Cost tables

```c
qname            tokens  cost_micro_usd
llm.classify      100     500       (~$0.0005)
llm.chat         1000   30000       (~$0.03)
http.fetch          0       0
kb.search           0       0
data.read           0       0
json.*              0       0
```

Zero-cost capabilities skip fence emission entirely.

### Chain order (entropy → tokens → cost)

At every cost-tagged host-call site the fences emit in this order:

1. L11 entropy fence → `Err(-9)` on trip.
2. L13 token fence → `Err(-10)` on trip.
3. L13 cost fence → `Err(-10)` on trip.

The first counter to trip wins. Replays reproduce the failure mode
deterministically regardless of which counter would also have
exceeded.

This is asserted by `test_ir.c::wasm_budget_chain_order_entropy_then_tokens_then_cost`.

## Trade-offs

**Eases**

- Same shape as L11. Reviewer reads one fence pattern.
- Token + cost dimensions decouple — a token-cheap-but-cost-expensive
  call (e.g. premium model) is gated by the cost fence even if tokens
  are abundant.
- Inner hard cap is mechanical; the authoritative billing
  reconciliation remains at Visual-DAG's `cost.Guard`.

**Hardens**

- Now three fences per call site for entropy/tokens/cost agents.
  ~18 wasm ops per call. Still below per-call noise but worth
  noting.
- Cost tables hard-coded; promoting to per-target config is
  follow-up.
- `-10` and the existing `HostErrBudgetExceeded = -3` (the legacy
  cost-ledger sentinel in `imports.go`) coexist. We document both
  rows in the HostError table so the audit trail is unambiguous.
  Visual-DAG follow-up F36c maps `-10` to non-retryable separately.

## Consequences

- Five new IR tests pin the global init, the i32 fence, the i64
  fence, the trap shape, and the chain order.
- ADR-0016 `agent.compiled@1.0` contract now has the hard-cap budget
  enforcement it assumed. Three-counter replay (entropy + tokens +
  cost) holds simultaneously.

## Alternatives considered

**A. Single combined fence.** Add tokens and cost together in one
fence block. Loses the per-dimension trip granularity that the
`Err(-10)` payload could eventually carry.

**B. Float cost arithmetic.** Keep `cost_micro_usd` as f64. Rejected
because floating-point comparisons under repeated `f64.sub` accumulate
rounding error; replay determinism would not hold.

**C. Runtime-side host enforcement.** The host imports decrement a
host-managed counter. Same problem as ADR-0003 alternative A — the
counter is no longer guest-observable.

## Open questions

- **Refilling counters.** Today, once a counter trips, it stays
  tripped for the remainder of the wasm instance's lifetime. A
  long-lived agent that wants to refill on a wallclock interval
  needs host-side reset — not in scope for v1.
- **Per-tenant cost-table override** — see ADR-0003 open questions.

## Implementation order

1. Two new globals + init from agent header.
2. `emit_*_fence` shared with L11 — chain order asserted.
3. Cost tables + `tokens_cost_for` + `cost_micro_usd_for`.
4. Five IR tests covering globals, decrement shape, trap shape,
   chain order, and zero-default behaviour.
5. Two wasm examples (within budget, exceeds budget).

## Cross-references

- [language reference: budget block](../language-reference.md#budget)
- [ADR-0002](0002-result-as-negative-i64-union.md)
- [ADR-0003](0003-entropy-budget-runtime-fence.md)
- Examples: `examples/wasm/budget/`.
