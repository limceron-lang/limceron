# ADR 0002 — Result<T, E> as the negative-i64 union (L5)

## Status
Accepted (2026-05-12). Implemented in commit `e47e1ba`.

## Context

Limceron stage0 had no error-handling shape at the language level. The
existing host imports (`vdag:llm/classify`, `vdag:http/fetch`, ...)
already encoded errors as **negative i64** sentinels (`-1` generic,
`-2` URL not allowed, ..., `-8` invalid arg). Every guest function
that consumed those calls had to manually compare against `< 0` and
branch — boilerplate that hides real intent.

Three pressures pushed for a real Result type:

1. **`?` propagation.** A common pattern is "call the host; if it
   returned an error, return that error from the current fn". Without
   a Result type, this is a four-line `if signal < 0 { return signal }`
   block at every call site.
2. **`try/catch` for recoverable host calls.** A `policy.guard` agent
   may want to attempt an LLM classify and degrade gracefully on
   budget breach. Stage0 had no syntax for "treat this error path as
   a recoverable branch".
3. **Composition with ADR-0016 substrate.** The compiled DAG → .wasm
   path needs the wasm guest to surface host errors as user-visible
   failures, not as silent negative-int propagation through arithmetic.

## Decision

**`Result<T, HostError>` IS a single i64 at runtime. Negative values
are `Err(code)`, non-negative are `Ok(v)`. The same encoding the host
imports already use becomes the canonical Result repr — no
marshalling, no tag bit, no boxing.**

### AST additions

```c
AST_RESULT_OK    Ok(expr)          left = inner expression
AST_RESULT_ERR   Err(code)         left = (negative) error code
AST_TRY_CATCH    try { } catch ()  left = body, right = handler,
                                    name = err binding, type_expr = err type
```

`?` reuses the existing `AST_TRY` AST kind (which had no IR-gen
handler before L5). No new IR opcodes introduced — the lowering uses
existing `IR_CMP_LT`, `IR_BR`, `IR_RET`.

### `?` propagator lowering

```
bbN:  %v = <expr>
      %z = const i64 0
      %neg = cmp_lt %v, %z
      br %neg, @try.err, @try.ok
try.err:  ret %v             ; (no enclosing catch)
try.ok:   ; value is %v
```

Inside a `try { } catch { }`, `try.err` becomes
`store %v, %err_addr; jmp @try.catch` — controlled by an
`IrGenCatchCtx` stack in `IrGenContext`.

### try/catch lowering

```
pre:        %err_addr = alloca i64
            jmp @try.body
try.body:   <body lowered with (catch_bb, err_addr) on stack>
            <tail expr -> ok_val>
            jmp @try.merge
try.catch:  %ev = load %err_addr
            %e_slot = alloca i64
            store %ev, %e_slot   ; binds `e` in scope
            <handler lowered>
            <tail expr -> catch_val>
            jmp @try.merge
try.merge:  %r = phi [ok_val, body_pred] [catch_val, catch_pred]
```

### Example

```limceron
fn reason() -> int {
    let label_len = llm.classify("react-stage1-reason")?
    label_len
}

fn safe_call() -> int {
    try {
        llm.chat("draft", "make a recommendation")?
    } catch (e: HostError) {
        // recoverable: produce a degraded answer
        Err(e)?  // re-raises
    }
}
```

## Trade-offs

**Eases**

- Every existing `vdag:*` host call automatically composes with `?`
  — no marshalling layer.
- IR opcode footprint stays small (no new ops).
- WASM emit doesn't change for non-Result code paths.
- Native (ARM64 / x86_64) backends handle the new AST kinds for
  free — they only see the existing IR opcodes.

**Hardens**

- **i64 is the only Result-carrier in v1.** A `Result<string, E>` or
  `Result<Foo, E>` requires a different repr (tagged union). Stage0
  doesn't have generic Result yet; `match Ok(v) / Err(e)` is not
  exhaustiveness-checked at v1.
- The sentinel space is shared between guest-raised `Err(...)` and
  host-raised negative codes. We document the codes (`-1` through
  `-10` so far) as a stable contract; new sentinels must avoid
  collision.

## Consequences

- The host-call frontend (F18, commit `adba2aa`) keeps its existing
  shape. The negative-int return becomes "the Err variant of the
  Result"; the positive return becomes "the Ok variant".
- ADR-0017 multi-agent verbs reuse this encoding for `agent.call`
  return codes.
- L11 / L13 runtime fences emit `Err(-9)` / `Err(-10)` via the same
  encoding when budgets trip.
- The Visual-DAG side `react_loop.wasm` SHA shifted in this commit
  (because `?` adds compare-branch blocks); both repos updated.

## Alternatives considered

**A. Boxed `Result<T, E>` tagged union.** Standard ML/Rust shape.
Rejected for v1 because it requires a new repr, marshalling for host
calls, and a heap allocator that stage0 doesn't have.

**B. Out-parameter convention** (the C / Go shape). Rejected because
it doesn't compose with `?`; every call would need explicit `ok_ptr`
plumbing.

**C. Exceptions.** Rejected — stage0 has no unwinder; would require
re-implementing wasm exception-handling proposal which is not on the
target.

## Open questions

- **Generic `Result<T, E>`.** Stage1 work. Requires tagged-union repr
  + match-exhaustiveness checking.
- **`match Ok(v) / Err(e)` syntax.** Not in v1; the if-let-style
  pattern via `try/catch` covers the v1 use cases.

## Implementation order

1. Parser: detect `Ok(...)`, `Err(...)`, `try { } catch (e: T) { }`.
2. AST: three new kinds.
3. IR-gen: lower `?` via existing cmp_lt + br + ret.
4. WASM emit: no changes (uses existing opcodes).
5. Examples + tests (7 IR tests + 1 wasm example).

## Cross-references

- [language reference: try / catch / ?](../language-reference.md#try-catch-and-)
- [ADR-0003](0003-entropy-budget-runtime-fence.md) — L11 fence emits Err(-9) through this encoding.
- [ADR-0004](0004-budget-runtime-fence-chain-order.md) — L13 fence emits Err(-10).
- Examples: `examples/wasm/errors/01_basic_ok_err.lceron`, `examples/wasm/errors/03_try_catch.lceron`.
