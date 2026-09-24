# L11 -- entropy_budget runtime fence

The Limceron compiler turns each agent's `entropy_budget: <bits>` declaration
into a wasm-level runtime fence. Every entropy-consuming host call decrements
a module-global bit counter and traps with a new `HostError` sentinel if the
remaining budget would go negative. The fence is structurally local to each
host-call site, so it composes with the existing `Result<T, HostError>` /
`?` / `try-catch` machinery without any source-level changes.

## Samples

| File | Budget | Calls | Outcome |
|------|-------:|------:|---------|
| `01_within_budget.lceron` | 10 bits | 2 x `llm.classify` (cost 2) | `Ok(N)` -- non-negative |
| `02_exceeds_budget.lceron` | 1 bit  | 2 x `llm.classify` (cost 2) | `Err(-9)` on the second call |

Build and inspect:

```sh
./build/limceron-stage0 build examples/wasm/entropy/01_within_budget.lceron \
    --target wasm32-wasi-preview2 -o /tmp/within.wasm

./build/limceron-stage0 build examples/wasm/entropy/02_exceeds_budget.lceron \
    --target wasm32-wasi-preview2 -o /tmp/exceeds.wasm
```

## Emitted shape

The wasm module gets a single mutable i32 global initialised to the declared
budget (or `INT32_MAX` if none was declared):

```wat
(global $entropy_remaining (mut i32) (i32.const 10))
```

At every entropy-tagged host-call site, the compiler emits a fence right
before the actual `call $hi_<ns>_<fn>`:

```wat
;; --- entropy fence: cost=1 ---
global.get $entropy_remaining
i32.const 1
i32.lt_s
if
  i64.const -9  ;; HostError.EntropyExceeded
  return
end
global.get $entropy_remaining
i32.const 1
i32.sub
global.set $entropy_remaining
```

The sentinel push (`i64.const -9` above) is adapted to the enclosing
function's return type: `f64.const` for float-returning fns, `i32.const`
for bool/string/ptr fns, plain `return` for void.

## Host-call entropy cost table

The compiler maps each qualified host-call name to a per-call cost in
bits. The table lives in `src/ir_emit_wasm.c::entropy_cost_for`.

| Qualified name | Cost (bits) | Notes |
|----------------|------------:|-------|
| `llm.classify` | 1 | Default; refined later when we measure log-prob. |
| `llm.chat`     | 4 | Longer responses, higher uncertainty. |
| `http.fetch`   | 0 | Network response timing is out of scope today. |
| `kb.search`    | 0 | Passive vector lookup. |
| `data.read`    | 0 | Read-only relational query. |
| `json.*`       | 0 | Deterministic byte manipulation. |
| `vdag:agent.*` | 0 | Per-agent override; default no charge. |

Costs are stage0-hardcoded; promoting the table to a per-target
configuration is tracked separately.

## HostError variants

Mirrors `Visual-DAG/internal/nodes/code/imports.go`:

| Sentinel | Value | Source |
|----------|------:|--------|
| `Generic`         | -1 | All hosts |
| `URLNotAllowed`   | -2 | http.fetch |
| `BudgetExceeded`  | -3 | budget tracker |
| `NoTenant`        | -4 | tenant context |
| `DMLRejected`     | -5 | data.read |
| `TooLarge`        | -6 | out-buffer overflow |
| `CapMissing`      | -7 | capability check |
| `InvalidArg`      | -8 | argument validation |
| **`EntropyExceeded`** | **-9** | **L11 entropy fence (new)** |

The Limceron-side encoding is unchanged from L5: positive i64 = `Ok(v)`,
negative i64 = `Err(sentinel)`. `?` on a negative value short-circuits
the enclosing function (or innermost `try/catch`).

## Visual-DAG side gap

`internal/nodes/code/imports.go` must learn about `EntropyExceeded = -9`
and propagate it through the `code.limceron` Activity's output so an
upstream workflow can branch on it. Tracked as the follow-up to this
commit; see the commit message for details.

## What ADR this unblocks

Visual-DAG ADR-0016 specifies two contracts that depend on the fence
shipping in the compiled wasm:

- `entropy.fence@1.0` -- "agent must respect a declared bit budget"
- `replay.deterministic@1.0` -- "replays of an entropy-exceeding step
  must produce the same `Err(-9)`"

Both now have the wasm-level invariant they assume.
