# ADR 0003 — entropy_budget runtime fence (L11)

## Status
Accepted (2026-05-12). Implemented in commit `0d9e889`.

## Context

`entropy_budget: N` has parsed in agent headers since stage0 but did
not enforce anything at runtime. The semantics in the spec call for:

> Non-deterministic operations (LLM with temp > 0, RNG, time-dependent
> ops) deduct from a per-agent counter; when the counter would go
> negative, the next entropy-consuming operation refuses and the
> agent halts.

The compile-time piece is satisfied (the field parses); the runtime
piece is the gap.

Constraints:

- Stage0 has no new IR opcodes — the fence must lower to existing
  arithmetic + branches.
- The fence must not perturb the cold-start budget. Per the ADR-0001
  measurements (70-205 µs cold start) we have budget for ~50 µs of
  fence-related overhead at instantiate.
- Replay determinism: two invocations on the same input with the same
  declared budget must produce the same `$entropy_remaining`
  trajectory.

## Decision

**Each agent's `entropy_budget: <bits>` becomes a module-level i32
wasm global. Every entropy-consuming host call site emits a
decrement-and-bounds-check fence before invoking the host import.
A fence trip emits `Err(-9)` (`HostError.EntropyExceeded`).**

### Module-level global

```wat
(global $entropy_remaining (mut i32) (i32.const N))
```

where `N` is the agent's declared bit count, or
`INT32_MAX = 2147483647` when no agent declared a scalar budget.

The block form `entropy_budget: { ... }` is intentionally treated as
"no declared budget" because it carries no scalar bit count.

### Per-site fence

```wat
;; --- entropy fence: cost=N ---
global.get $entropy_remaining
i32.const N
i32.lt_s
if
  i64.const -9  ;; HostError.EntropyExceeded — sentinel push is
                ;; return-type-aware (i32 / i64 / f64 / void).
  return
end
global.get $entropy_remaining
i32.const N
i32.sub
global.set $entropy_remaining
```

### Cost table

```c
entropy_cost_for("llm.classify")  → 1
entropy_cost_for("llm.chat")      → 4
entropy_cost_for("http.fetch")    → 0
entropy_cost_for("kb.search")     → 0
entropy_cost_for("data.read")     → 0
entropy_cost_for("json.*")        → 0
entropy_cost_for("vdag:agent.*")  → 0
```

Zero-cost capabilities skip fence emission entirely. `llm.classify`'s
cost should refine when we have log-prob telemetry; today it's
fixed.

## Trade-offs

**Eases**

- Lower at emit time, not at IR-gen — no new IR opcode.
- Sentinel `-9` composes with L5 `?` propagator: a guest writes
  `let x = llm.chat(...)?` and a fence trip propagates out cleanly.
- Replay determinism trivially holds because the global is
  module-scoped i32 with no externally-visible side effects.

**Hardens**

- The fence adds ~6 wasm ops per entropy-consuming call. Per a
  back-of-envelope: 4 LLM calls per agent run × 6 ops/fence × ~5 ns
  per op = 120 ns total. Below noise.
- The cost table is hard-coded in `entropy_cost_for`. Per-target
  config promotion is follow-up; v1 acceptable because the call
  surface is small.

## Consequences

- Visual-DAG `internal/nodes/code/imports.go` registers
  `HostErrEntropyExceeded = -9` (shipped in F36b) and the runtime
  follow-up (F36c) wires the value to a non-retryable
  Temporal application error.
- L13 budget fence (ADR-0004) reuses the fence-emit machinery
  (`emit_*_fence`) so chain order is deterministic.
- The L11 invariants for ADR-0016 `entropy.fence@1.0` and
  `replay.deterministic@1.0` are satisfied at the Limceron emitter.

## Alternatives considered

**A. Runtime-side counter (host).** The host tracks per-instance
entropy spend, refusing a host call when the budget runs out.
Rejected: loses the determinism property (the host's view of
"entropy" is policy, not the wasm state).

**B. Per-call counter passed as i32 arg.** The host import takes
`entropy_remaining` as an additional parameter, returns the new
value. Rejected: every call site needs marshalling; couples ABI
across all entropy-consuming verbs.

**C. New `IR_ENTROPY_FENCE` opcode.** Cleaner separation but
introduces a new IR shape every backend must lower. Rejected per
the "no new ops" design constraint.

## Open questions

- **Per-target cost-table override.** Today the table is static.
  Visual-DAG operators may want to set `llm.chat = 8` when running
  at temperature > 1.0; this needs a build-time config plumbed into
  `lcn_emit_wasm`.
- **Log-prob-derived entropy cost.** The "right" cost of an LLM call
  is the response's per-token log-prob sum (high prob → low entropy
  consumed). Wiring requires a follow-up host-side feedback channel.

## Implementation order

1. Module-level global emission (`scan_entropy_budget` +
   `emit_module`).
2. Per-site fence emission (`emit_entropy_fence` called from
   `IR_HOST_CALL` case).
3. Cost table.
4. Examples (within-budget + exceeds-budget) + 3 IR tests.

## Cross-references

- [language reference: entropy_budget](../language-reference.md#entropy_budget)
- [ADR-0002](0002-result-as-negative-i64-union.md) — `?` propagation.
- [ADR-0004](0004-budget-runtime-fence-chain-order.md) — L13 reuses the fence machinery.
- Examples: `examples/wasm/entropy/`.
