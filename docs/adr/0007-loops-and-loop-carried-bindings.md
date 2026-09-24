# ADR 0007 — `while` / `for` / `loop` and loop-carried bindings (L2)

## Status
Accepted (2026-05-13). Implements L2 of the wasm-target ROADMAP. Builds
on commits `9593b84` (original loop landing) and `b6b9d4f` (L1b).

## Context

Limceron stage0 inherited `while` / `for-in` / `loop` from the Limceron
language surface; the C99 transpiler had emitted them since the
self-hosted days. The wasm-target work (ADR-0001) reused the existing
AST nodes (`AST_WHILE`, `AST_FOR`, `AST_LOOP`, `AST_BREAK`,
`AST_CONTINUE`) and added IR-gen + wasm-emit support, but **never
formalised the surface that callers of the wasm backend may rely on**.

Three downstream consumers now depend on stable loop semantics:

1. **Visual-DAG’s bounded ReAct executor** issues a `for _ in 0..MAX`
   loop in the guest to bound the number of LLM round-trips per
   decision. If the loop's termination invariant changed, the runtime
   budget fence (L13, ADR-0004) would be the *only* defence against an
   infinite host-call chain.
2. **The L11 entropy fence** (ADR-0003) and L13 budget fence
   (ADR-0004) decrement counters at every cost-tagged host call. Loops
   amplify call counts linearly; a missed back-edge means the fence
   trips at the wrong iteration and the operator's audit trail
   misreports the trip point.
3. **L5's `?` propagator** (ADR-0002) lowers `host_call(...)?` to a
   short-circuit `ret` from the enclosing fn (or jump to the enclosing
   catch handler). Inside a loop body this MUST exit the loop too —
   silently continuing would mask the error.

## Decision

**L2 lifts `while` / `for` / `loop` (and their `break` / `continue`)
into a load-bearing surface with three stable invariants and one
explicit non-feature.**

### Invariant 1 — half-open integer-range for-in is the only iterable

```limceron
for i in 0..N  { … }     // i takes 0, 1, …, N-1
for _ in 0..N  { … }     // wildcard pattern: counter not in scope
```

- `start` and `end` must be integer expressions.
- The range is **half-open**. Inclusive (`..=`), step (`0..N by 2`),
  reverse (`N..0`), and iterator-protocol (`xs.iter()`) forms are
  intentionally out of scope for L2 — they will land alongside L3 JSON
  list iteration (`for x in json.array(h)`).
- A wildcard pattern (`_`) is accepted; the loop variable is
  materialised in IR and used by the cond/inc blocks but is not bound
  in lexical scope.

### Invariant 2 — `loop` body must contain a `break` (advisory)

```limceron
loop {
    if done() { break }
    work()
}
```

A `loop { }` whose lexical body has no `break` is provably
non-terminating in the wasm sandbox. The compiler emits a
**warning** (not error) — the runtime budget fence will eventually
trap, but flagging the typo at compile time saves a debug round-trip.

### Invariant 3 — loop-carried `let mut` bindings via alloca-store

A `let mut x = init` declared **outside** a loop and reassigned
**inside** the body has its mutation surface to the next iteration.

```limceron
let mut sum = 0
let mut i = 0
while i < 10 {
    sum = sum + i      // sum read at iter N+1 is the value written at iter N
    i = i + 1
}
```

**Lowering shape (current stage0):** each mutated binding is an
`alloca` slot in the entry block; reads are `load`, writes are `store`.
The loop header re-loads on every iteration. **No PHI nodes are emitted
at the loop header** — alloca is functionally equivalent under SSA
because the address itself (the `alloca` result) is single-assignment.

ADR-0002 documented a header-PHI pattern for `if` as an expression.
That pattern works at merge points where the number of predecessors is
fixed at lowering time; at a loop header the back-edge predecessor BB
isn't yet known when the header is created, which would require a
two-pass walk or a placeholder-phi-then-patch dance. Stage 0 avoids
that complexity by routing through alloca. mem2reg in a future SSA
optimisation pass can promote these slots if needed; for stage 0 the
wasm backend lowers `alloca` to a `(local …)` slot anyway, so there is
no observable cost.

### CFG and wasm emission

`AST_WHILE` lowers to four basic blocks:

```
bb_pre   -> jmp  -> bb_cond
bb_cond  -> br   -> bb_body / bb_exit
bb_body  -> jmp  -> bb_cond              (the back-edge)
bb_exit  -> (continuation)
```

`AST_FOR` (range form) adds an explicit increment block so `continue`
still advances the counter:

```
bb_init -> jmp -> bb_cond
bb_cond -> br  -> bb_body / bb_exit
bb_body -> jmp -> bb_inc                 (or to bb_exit on break)
bb_inc  -> jmp -> bb_cond                (the back-edge; continue lands here)
bb_exit -> (continuation)
```

`AST_LOOP` is identical to `AST_WHILE` minus the conditional:

```
bb_pre    -> jmp -> bb_header
bb_header -> jmp -> bb_body
bb_body   -> jmp -> bb_header            (back-edge; break -> bb_exit)
bb_exit   -> (continuation)
```

`AST_BREAK` / `AST_CONTINUE` are scoped against an `irgen_loop_stack`
in `IrGenContext`; nesting just pushes a fresh frame, so the inner
`break` targets the innermost exit BB.

The wasm backend (`ir_emit_wasm.c`) uses the existing
**dispatch-loop** idiom:

```
(block $exit
  (loop $dispatch
    (block $bb_N … (block $bb_1 (block $bb_0
      local.get $bb
      br_table $bb_0 $bb_1 … $bb_N $exit
    ) ... ) ... )
    ;; bb_i bodies set $bb := next and br $dispatch
  )
)
```

Each basic-block terminator sets the dispatcher’s `$bb` local to the
next block index and `br`s back to the outer `$dispatch` loop. The
verifier accepts the resulting structured CFG without any of the
relooper machinery.

### Type-check additions

- `break` outside any loop body → `error: \`break\` used outside of a
  loop body`. Same shape for `continue`.
- The check is a small per-fn walker (`check_loop_scope_stmt`)
  carrying a `loop_depth` counter; it’s called after `check_stmt`
  finishes. The walker descends into `if` / `match` arms while
  preserving the enclosing loop frame, so `if cond { break }` inside
  a loop is accepted.

### Tests

- `test/test_runner.c` — five new `l2_*` tests:
  - `l2_parse_loop_keyword_infinite`
  - `l2_parse_for_wildcard_pattern`
  - `l2_typecheck_rejects_break_outside_loop`
  - `l2_typecheck_rejects_continue_outside_loop`
  - `l2_typecheck_accepts_break_inside_if_inside_loop`
- `test/test_ir.c` — two new `l2_*` tests:
  - `l2_ir_gen_loop_has_header_and_back_edge`
  - `l2_ir_gen_for_wildcard_pattern_lowers_like_named`
- `examples/wasm/poc/06_loops.lceron` — end-to-end PoC sample: a
  bounded ReAct loop over `llm.classify` with the L5 `?` propagator.
  Validates through `wasm-validate`.

## Consequences

### Positive

- Visual-DAG can rely on bounded `for _ in 0..MAX_ITER` loops without
  worrying that the compiler will silently miss the back-edge.
- The L11 / L13 fences correctly accumulate per-iteration host-call
  costs because the loop body is a real CFG cycle, not unrolled.
- The L5 `?` propagator inside a loop body correctly exits the
  *enclosing function*, not the loop — matching Limceron’s `return`-
  semantics for early exit.

### Negative

- Iterator-protocol `for x in xs.iter()` deferred to L3.
- Labelled break (`break 'outer`) deferred indefinitely. The
  innermost-loop semantics are sufficient for the bounded-retry
  patterns that motivate L2.
- The alloca-store loop-carried lowering wastes one wasm local per
  mutated binding. A future mem2reg pass can promote.

### Neutral

- The dispatch-loop wasm emission style is unchanged from L1; the
  back-edge naturally falls out of setting `$bb := header_index` at
  the body terminator.

## Alternatives considered

**A. True SSA PHI at the loop header.** Mainstream compilers (LLVM,
Cranelift) use header phis. Implementing requires either a two-pass
walk or placeholder-phi-then-patch. The win is one less load per
iteration when the wasm backend promotes locals — measured impact for
the bounded ReAct workload is <0.001 % of wall time (LLM round-trips
dominate). Rejected as premature.

**B. Relooper algorithm for arbitrary irreducible CFGs.** Required if
we ever want to compile `goto`-style flow. Stage 0 emits only reducible
CFGs, so the dispatch-loop idiom suffices.

**C. Loop-unrolling at IR.** Tempting for the bounded ReAct case (the
bound is a literal `0..3`), but breaks the L11 / L13 fence semantics —
each iteration must hit the runtime decrement, otherwise the budget
audit trail is wrong. Rejected.

## Related ADRs

- [ADR-0001](0001-wasm-target.md) — wasm32-wasi-preview2 target choice.
- [ADR-0002](0002-result-as-negative-i64-union.md) — L5 `?` propagator
  semantics inside a loop body.
- [ADR-0003](0003-entropy-budget-runtime-fence.md) — L11 fence
  decrements per iteration.
- [ADR-0004](0004-budget-runtime-fence-chain-order.md) — L13 fence
  chain order under loops.
