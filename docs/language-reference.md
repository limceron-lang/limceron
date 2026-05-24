# Limceron language reference

Covers every syntactic form stage0 supports as of L13 + L2 (2026-05-13).
The reference is grouped by feature; each section names the
implementing files in `src/`.

## Top-level shape

A Limceron source file declares one or more `agent` blocks. Each
agent ships its own capability declaration, budget header, optional
entropy_budget, and a collection of `fn` definitions.

```limceron
agent Classifier {
    capabilities: [llm.classify, data.read]
    budget: { max_tokens: 100, max_cost: 0.01 }
    entropy_budget: 4

    fn classify(text: string) -> int {
        llm.classify(text)?
    }

    fn main() -> int {
        classify("hello world")
    }
}
```

Parser: `src/parser.c`. AST kinds: `AST_AGENT_DECL`, `AST_FN_DECL`.

## Capabilities

The `capabilities:` field declares the host imports an agent may
issue.

### Bare form

```limceron
capabilities: [http.fetch, llm.classify, kb.search]
```

Recognised identifiers (the host-import verbs):

```
llm.classify, llm.chat
http.fetch, http.request
kb.search
data.read
json.parse                   // binds all 9 vdag:json functions
mcp.tool
```

Plus the agent verbs from ADR-0017 (future):
`agent.call.<id>`, `agent.spawn.<id>`, `agent.broadcast`,
`agent.handoff.<id>`, `agent.observe.<id>`, `agent.veto.<id>`.

### Parameterised form (L12)

```limceron
capabilities: [
    http.fetch(["api.openai.com:443", "*.example.com:443"]),
    llm.classify
]
```

Pins a compile-time host:port allowlist. Validated at typecheck
pass 2c (`check_capability_allowlists`):

- `host:port` required.
- Leading `*.<rest>` is the only glob form.
- Port decimal, in (0, 65535].
- IPs / `*` / `*:*` / empty rejected.
- Empty list rejected.

The compiler emits a wasm custom section
`vdag.capability.network.allowlist` carrying the JSON payload. See
[ADR-0005](adr/0005-capability-network-compile-time-allowlist.md).

## `budget` block

```limceron
budget: { max_tokens: 1000, max_cost: 0.03 }
```

Both fields optional; default `INT32_MAX` / float-equivalent (no cap).

`max_tokens` is i32; `max_cost` is a literal whole-USD float (converted
to micro-USD at compile time).

Runtime fence (L13) decrements both counters at every cost-tagged host
call and traps with `Err(-10)` when either would go negative. See
[ADR-0004](adr/0004-budget-runtime-fence-chain-order.md).

## `entropy_budget`

```limceron
entropy_budget: 4
```

Scalar i32 bit count. Optional; default `INT32_MAX` (no cap). The
block form `entropy_budget: { ... }` is intentionally treated as
"no declared budget".

Runtime fence (L11) decrements at every entropy-consuming host call;
`llm.classify=1`, `llm.chat=4`, everything else `0`. Trap with
`Err(-9)`. See
[ADR-0003](adr/0003-entropy-budget-runtime-fence.md).

## Function declarations

```limceron
fn name(arg1: type1, arg2: type2) -> ret_type {
    body
}
```

Types: `int` (i64), `string`, `bool`, `float` (f64), `void`.

`main()` is the conventional entry point; `wasmtime --invoke main`
expects it.

### Tail-expression returns

The last expression of a fn body is implicitly returned:

```limceron
fn double(x: int) -> int {
    x * 2          // returned
}
```

`return <expr>` is also legal for early exit:

```limceron
fn classify(score: int) -> string {
    if score >= 90 { return "A" }
    if score >= 80 { return "B" }
    return "F"
}
```

## Let bindings

```limceron
let x = 42
let mut y = 0
y = y + 1
```

`let mut` is required for assignment. Type inferred from initialiser.

## Operators

Arithmetic: `+`, `-`, `*`, `/`, `%`.
Comparison: `<`, `<=`, `>`, `>=`, `==`, `!=`.
Logical: `&&`, `||`, `!` (boolean only).
String concatenation: `+`.

## Control flow

### `if` / `else`

```limceron
if cond { then_branch } else { else_branch }
```

`if` is BOTH a statement and an expression — when the last form of a
fn body, the value of the chosen branch becomes the return value.

### `while`

```limceron
let mut i = 0
while i < 10 {
    sum = sum + i
    i = i + 1
}
```

Condition checked at the top. Body must not be the tail expression.

### `for` (half-open range)

```limceron
for i in 0..10 {
    sum = sum + i
}
```

Half-open: `0..10` iterates `i = 0..9`. Inclusive `..=`, step, and
reverse ranges are future work.

Desugared at IR-gen to:

```limceron
let i = start
while i < end {
    <body>
    i = i + 1
}
```

### `loop`

```limceron
loop {
    if done() { break }
    work()
}
```

Infinite loop; exits via `break` only.

### `break` / `continue`

`break` exits the nearest enclosing loop. `continue` jumps to the
next iteration. Order matters when both gates are present in a body
— `break` is evaluated first:

```limceron
for i in 0..100 {
    if i >= 10 && (i % 17) == 0 { break }     // 1st gate
    if (i % 2) == 1 { continue }              // 2nd gate
    sum = sum + i
}
```

`break` or `continue` outside any loop body is a typecheck error
(`` `break` used outside of a loop body``). The check walks into
`if` and `match` arms while preserving the enclosing loop frame, so
`if cond { break }` inside a loop is accepted.

### Loop-carried `let mut` bindings (L2)

A `let mut` declared *outside* a loop and reassigned *inside* the
body has its mutation surface to the next iteration:

```limceron
let mut sum = 0
let mut i = 0
while i < 10 {
    sum = sum + i      // sum at iter N+1 = sum at iter N + i
    i = i + 1
}
// sum == 45, i == 10
```

The compiler lowers each loop-carried binding to an `alloca` slot in
the entry block with `load`/`store` at each access. There are no PHI
nodes at the loop header in stage 0 — the alloca address is itself
single-assignment, which keeps the wasm backend emission simple. See
[ADR-0007](adr/0007-loops-and-loop-carried-bindings.md) for the full
rationale and the deferred alternatives (header phis, mem2reg,
iterator protocols).

The wildcard `_` pattern is accepted in `for-in` and skips binding
the counter into scope:

```limceron
for _ in 0..MAX {
    work()             // counter unused
}
```

Both shapes — named (`for i in 0..N`) and wildcard (`for _ in 0..N`)
— lower to the same 4-BB CFG (init → cond → body → inc → exit). See
[ADR-0007](adr/0007-loops-and-loop-carried-bindings.md).

## Result, `?` and `try/catch` (L5)

### `Result<T, E>`

In stage0, `Result<i64, HostError>` is the only concrete Result type
and is encoded as a single i64 where:

- Non-negative values are `Ok(v)`.
- Negative values are `Err(code)`.

The encoding matches the existing `HostErr*` sentinel space, so every
`vdag:*` host call is already a Result.

### Constructors

```limceron
Ok(42)
Err(-7)                            // HostError.CapMissing (legacy short form)

Result::Ok(42)                     // L5 qualified form -- preferred
Result::Err(host_error::quota_exceeded)
```

Both spellings parse to the same AST kinds (`AST_RESULT_OK`,
`AST_RESULT_ERR`). The qualified form documents intent and pairs
with `match Result::Ok / Result::Err` arm patterns.

### `?` propagator

```limceron
fn reason() -> int {
    let label_len = llm.classify("react-stage1-reason")?
    label_len
}
```

`expr?` short-circuits the current fn (or the enclosing `try` body)
when `expr` is `Err(...)`. Already shipped in L1b -- L5 documents
it under the new error-handling surface but does not redefine its
lowering. See ADR-0002 for the IR shape (cmp_lt + br + ret/jmp).

### `try` / `catch`

```limceron
fn safe_call() -> int {
    try {
        llm.chat("draft", "make a recommendation")?
    } catch (e: HostError) {
        // recoverable: degraded answer
        -1
    }
}
```

`try { body } catch (e: T) { handler }` is an expression. The result
is `body` when no `?` tripped, or `handler` when one did. The bound
name (`e`) is in scope in the handler with the captured error code.

`try` and `catch` are soft identifiers (not keywords); detected by
lookahead at the `{` following `try`.

### `match` over Result

```limceron
fn classify_or_default(r: int) -> int {
    match r {
        Result::Ok(v)  -> v
        Result::Err(e) -> if e == host_error::quota_exceeded {
                              fallback()
                          } else {
                              -1
                          }
    }
}
```

The L5 typechecker enforces exhaustiveness on Result-shaped matches:
both `Result::Ok(_)` and `Result::Err(_)` arms must be present.
A single-arm match raises `ERR_MATCH_INEXHAUSTIVE`. Wildcard /
catch-all patterns (`_ -> ...`) are L6 territory and are NOT
recognised as a substitute for the missing arm today.

The lowering mirrors `try/catch`: one `cmp_lt %r, 0` split, two
arm blocks (`match.ok`, `match.err`), and a join block
(`match.merge`) holding a PHI of both arm values.

### Canonical HostError enum

The negative-sentinel space is now formally declared in
`include/vdag.errors.wit`:

```wit
enum host-error {
    not-found              = -1,
    permission-denied      = -2,
    quota-exceeded         = -3,
    cost-budget-exceeded   = -4,
    dml-rejected           = -5,
    sandbox-violation      = -6,
    json-field-missing     = -7,
    json-type-mismatch     = -8,
    entropy-budget-exceeded = -9,
    limceron-budget-exceeded = -10,
    network-not-allowed    = -11,
    invalid-arg            = -12,
}
```

Reference a variant from Limceron source as
`host_error::<name>` (snake_case at the call site -- the loader
treats `-` and `_` as equivalent). The typechecker validates the
variant name against the canonical file; an unknown name raises
`ERR_HOST_ERROR_UNKNOWN_VARIANT` before codegen runs. Resolution
emits the integer sentinel as a `const i64` so the `?` propagator
and `try/catch` recognise it through the existing negative-i64
encoding -- no new IR opcodes.

The numbers are an ABI contract: changing one is a release-blocking
break because Visual-DAG's `internal/nodes/code/imports.go`
returns the same numbers from every host call.

### Host-call failure surface

A host call that returns a negative sentinel **is** a
`Result::Err(host_error::*)` value at the language level. The `?`
propagator catches it; an enclosing `try/catch` recovers it; a
`match` arm dispatches on the specific sentinel. No host call traps
the wasm module on its own -- traps are reserved for the L11/L13
budget fences (which themselves return -9 / -10 through the same
encoding).

## Host calls

```limceron
let len = llm.classify(prompt)
let intent_h = json.parse(blob)?
let blob = json.field(intent_h, "intent")?
```

Host calls follow `<namespace>.<verb>(args...)` syntax where
`<namespace>` is one of `llm`, `http`, `kb`, `data`, `json`, `mcp`,
`vdag.json` etc. The compiler emits a `(import "vdag:<namespace>" "<verb>" ...)`
WIT declaration matching the call.

Return values are i64 in the Result encoding (positive = bytes
written / handle / value; negative = `HostErr*`).

`json.int_value(h)` is the one exception — returns i64 directly
(not extended from i32) because the value semantics require the full
i64 range.

See [ADR-0006](adr/0006-vdag-json-host-module.md) for the JSON host
module shape.

### Standard host modules

| Module | Functions |
|---|---|
| `vdag:llm` | `classify`, `chat` |
| `vdag:http` | `fetch`, `request` |
| `vdag:kb` | `search` |
| `vdag:data` | `read` |
| `vdag:json` | `parse`, `field`, `array_index`, `length`, `string_value`, `int_value`, `bool_value`, `is_null`, `stringify` |
| `vdag:mcp` | `tool` |

## `agent` block reference

```limceron
agent Name {
    capabilities: [<list>]
    budget: { max_tokens: <int>, max_cost: <float> }
    entropy_budget: <int>     // optional

    fn ... { ... }
    fn ... { ... }
}
```

Multiple agents per file is permitted at the parser level; stage0's
WASM emit path supports a single agent per module. The first agent
with a scalar `entropy_budget` declaration wins for that field.

## HostError sentinel space

| Const | Value | Meaning |
|---|---:|---|
| `HostErr.Generic` | `-1` | catch-all |
| `HostErr.URLNotAllowed` | `-2` | host:port not in worker allowlist |
| `HostErr.BudgetExceeded` (legacy) | `-3` | outer cost-ledger trip |
| `HostErr.NoTenant` | `-4` | no tenant context |
| `HostErr.DMLRejected` | `-5` | `data.read` saw a DML statement |
| `HostErr.TooLarge` | `-6` | payload exceeds out-buffer |
| `HostErr.CapMissing` | `-7` | declared capability missing at call site |
| `HostErr.InvalidArg` | `-8` | malformed input |
| `HostErr.EntropyExceeded` | `-9` | **L11 fence trip — non-retryable** |
| `HostErr.BudgetExceeded` (L13) | `-10` | **L13 fence trip — non-retryable** |

L5 `Err(<code>)` accepts any negative-int sentinel; users can also
mint custom sentinels in `[-100, -1]` for application-level errors.

## Compilation targets

```bash
./build/limceron-stage0 build --target wasm32-wasi-preview2 \
    -o /tmp/out.wasm path/to/source.lceron

./build/limceron-stage0 build --target c99 \
    -o /tmp/out.c   path/to/source.lceron

./build/limceron-stage0 build --target arm64 \
    -o /tmp/out.asm path/to/source.lceron

./build/limceron-stage0 build --target x86_64 \
    -o /tmp/out.asm path/to/source.lceron
```

Per [ADR-0001](adr/0001-wasm-target.md), `wasm32-wasi-preview2` is
the primary target for SaaS/multi-tenant deployments. C99 + native
backends remain available for embedded and single-tenant edge.

## What v1 does NOT support yet

- Generic `Result<T, E>` for `T != i64` — needs tagged-union repr.
- `match` over Result variants — covered by if-let-style via
  `try/catch` instead.
- Inclusive `..=` or reverse ranges.
- Closures / higher-order functions.
- `struct` / `enum` declarations beyond the implicit `Result`.
- Pattern matching beyond `try/catch` binding.

## Worked examples

| Path | Demonstrates |
|---|---|
| `examples/wasm/poc/01_arith_budget.lceron` | Arithmetic + comparisons + budget header. |
| `examples/wasm/poc/04_react_loop.lceron` | Unrolled ReAct pipeline. |
| `examples/wasm/loops/01_while_count.lceron` | `while` loop. |
| `examples/wasm/loops/02_for_range.lceron` | `for i in 0..N`. |
| `examples/wasm/loops/03_break_continue.lceron` | `break` / `continue` ordering. |
| `examples/wasm/errors/01_basic_ok_err.lceron` | `Ok` / `Err` constructors. |
| `examples/wasm/errors/03_try_catch.lceron` | `try` / `catch` shape. |
| `examples/wasm/entropy/01_within_budget.lceron` | `entropy_budget` runtime fence. |
| `examples/wasm/budget/02_exceeds_budget.lceron` | budget fence trip. |
| `examples/wasm/capabilities/02_restricted_fetch.lceron` | parameterised capability. |
| `examples/wasm/json/01_parse_and_field.lceron` | `vdag:json` host call. |

## Cross-references

- [ADR-0001](adr/0001-wasm-target.md) — wasm target.
- [ADR-0002](adr/0002-result-as-negative-i64-union.md) — Result repr.
- [ADR-0003](adr/0003-entropy-budget-runtime-fence.md) — L11.
- [ADR-0004](adr/0004-budget-runtime-fence-chain-order.md) — L13.
- [ADR-0005](adr/0005-capability-network-compile-time-allowlist.md) — L12.
- [ADR-0006](adr/0006-vdag-json-host-module.md) — L3.
- Visual-DAG: [ADR-0016](../../../Visual-DAG/docs/adr/0016-limceron-execution-substrate.md)
  — execution substrate context for these features.
