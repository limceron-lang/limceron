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

An optional annotation is still accepted (`let x: int = 42`) for cases
where the author wants the binding to read as a contract — but it is no
longer required for stage0 to infer the type. See §Type Inference (L9)
below for the synthesis / checking rules and the diagnostics raised when
the annotation conflicts with the initialiser.

## Type Inference (L9)

Stage0 performs **bidirectional type inference** scoped to each `fn`
body. Synthesis (⇑) deduces a type from an expression; checking (⇓)
compares a synthesised type against an expected one. Implementation:
`src/l9_infer.c`, exposed as `lcn_l9_infer_types` and exercised under
the `l9_*` test group.

### Synthesis rules

| Form | Inferred type |
|---|---|
| Integer literal (`42`, `0xff`) | `int` |
| Float literal (`1.5`) | `float` |
| Boolean literal (`true`, `false`) | `bool` |
| String literal (`"x"`) and interpolated string | `string` |
| `f(...)` -- top-level fn / tool call | callee's declared return type |
| `a + b`, `a * b`, ... | max of operand types (`int + float` -> `float`) |
| `a + b` when either side is `string` | `string` (concat) |
| `a == b`, `a < b`, `a && b`, ... | `bool` |
| `!x` | `bool` |
| `if c { a } else { b }` | unified type of both branches |
| `expr as T` | `T` |

Anything the synthesiser cannot pin down (calls to undeclared
identifiers, generic references, struct field access, `Result`, `Json`,
...) becomes a fresh **type variable**. Type variables live in the
function's `LcnUnifyTable` and are resolved on demand by `lcn_unify`.

### Checking rules

When a context expects a specific type -- e.g. the function's declared
return type, or the LHS of a `let x: T = ...` -- the inferred RHS is
checked against the expected type. The check succeeds when:

- both sides resolve to the same primitive (`int == int`), or
- one side is `int` and the other is `float` (single-step coercion in
  either direction), or
- one or both sides are still a type variable (it gets bound to the
  other side), or
- one or both sides are opaque (user-defined / non-primitive) -- the
  pass is deliberately conservative for v1 and accepts these.

`bool -> string` requires an explicit `as string` cast; the same holds
for `int -> string`. The cast is the only way to bridge non-numeric
primitives.

### Where annotations are still required

Function boundaries keep their explicit annotations -- the
wasm/WIT signature is derived from them, so they cannot be inferred
without breaking the host contract:

```limceron
fn classify(text: string) -> int { ... }   // params + return REQUIRED
```

The return type may be omitted only when the function returns unit
(`()`). Top-level `const` / `let` declarations still require an
annotation (they participate in cross-file imports). Generic
parameters (`fn id<T>(x: T) -> T`) are deferred to L9b.

### Diagnostics

Two conflicts are reported by the L9 pass:

- *`cannot unify <A> and <B> -- `if` branches disagree*: the two
  branches of an `if` expression infer to incompatible primitives.
  Either coerce one side (`a as float`) or restructure the branches.
- *`cannot unify <A> and <B> in return of fn '<name>'`*: the tail
  expression of the body infers to a primitive that does not match the
  declared return type. Either change the body or change the return
  annotation -- function boundaries are not inferred.

Both diagnostics print the unified-or-not types as `int`, `float`,
`bool`, `string`, `()`, or `T#N` (an unresolved type variable). Opaque
types (`Result`, `Json`, `MyStruct`, ...) are printed by name.

## Operators

Arithmetic: `+`, `-`, `*`, `/`, `%`.
Comparison: `<`, `<=`, `>`, `>=`, `==`, `!=`.
Logical: `&&`, `||`, `!` (boolean only).
String concatenation: `+`.

## String Interpolation (L4)

A `"..."` string literal may embed one or more `${expr}` placeholders.
At lex time the literal is split into alternating *literal* and
*expression* segments; the parser materialises them as an
`AST_INTERP_STRING` whose `params` list is the interleaved sequence
`literal[0], expr[0], literal[1], expr[1], ..., literal[N]`. IR-gen
lowers each interpolation site into a left-folded chain of
`vdag:string.concat(prev, next)` host calls.

```limceron
let prompt = "summarise ${title} in ${n} bullets"
```

### Coercion rules

Each `${expr}` segment is normalised to `string` before it joins the
concat chain. The table below names the host verb each source type
lowers through; mismatches that fall outside the table raise
`ERR_INTERP_NOT_COERCIBLE` at typecheck pass 6c.

| Source type | Lowering |
|---|---|
| `string` | passed through verbatim |
| `int` (`s64`) | `vdag:string.from-int` |
| `bool` | `vdag:string.from-bool` |
| `float` (`f64`) | `vdag:string.from-float` |
| `Json` | requires `${value as string}`, then `vdag:json.as-string` |
| anything else | `ERR_INTERP_NOT_COERCIBLE` |

`Json` is the one type that v1 refuses to coerce implicitly — the
intent is to make the call to `vdag:json.as-string` visible to the
author, because that verb walks the JSON DOM and may surface a host
error. Wrap the value in `${doc as string}` to opt in.

### Escaping `$`

`\$` produces a literal `$` in the output and does NOT enter
interpolation mode. So `"price: \$${cost}"` reads "price: $42" when
`cost = 42` at runtime. `\{` continues to produce a literal `{` for
backwards compatibility with the legacy `{name}` interpolation handled
by the C99 transpiler.

### Nested interpolation is forbidden in v1

A `${...${...}...}` pattern — whether the inner `${` is in the outer
expression body or inside a string literal embedded therein — raises
`ERR_NESTED_INTERPOLATION` at parse time. Split the inner expression
into a `let` and reference it from the outer string instead:

```limceron
let inner = "${x}"
let outer = "[ ${inner} ]"          // OK
```

### Deferred features

Format specifiers (`${value:.2f}`), multi-line interpolation
indentation handling, and lazy evaluation are intentionally NOT
supported in v1 -- they are tracked separately for a future L4
extension row.

Implementing files: lexer in `src/lexer.c`, parser hook in
`src/parser.c`, coercibility check in `src/typecheck.c::pass 6c`,
IR-gen lowering in `src/ir_gen.c::AST_INTERP_STRING`. The host
interface lives at `include/vdag.wit::interface string`.

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
A single-arm match raises `ERR_MATCH_INEXHAUSTIVE`. L6 relaxes this:
a wildcard / catch-all arm (`_ -> ...`) now satisfies exhaustiveness
on a Result-shaped match -- see the "Pattern matching" section
below.

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

## Pattern matching (L6)

L6 extends `match` from the L5 two-arm Result form to a general
decision tree. The supported pattern surface is:

| Pattern | Example | Notes |
|---|---|---|
| Wildcard | `_` | Matches anything. No binding. |
| Wildcard with binding | `_v` | Matches anything, binds to `v` (leading `_` stripped). |
| Identifier binding | `n` | Matches anything, binds to `n`. |
| Integer literal | `0`, `42` | Tagged with `pat->name = "int"`. |
| Bool literal | `true`, `false` | Tagged with `pat->name = "true" / "false"`. |
| String literal | `"start"` | Tagged with `pat->name = "str"`. C transpiler only -- IR backend defers. |
| Tagged-union | `host_error::quota_exceeded` | Resolved to the negative sentinel from `vdag.errors.wit`. |
| Nested enum | `Result::Ok(0)` | Outer enum + nested payload pattern. |
| Guarded | `n if n > 100 -> ...` | Predicate `if <expr>` after a binding pattern. |

### Exhaustiveness

The typechecker enforces exhaustiveness depending on the scrutinee
shape inferred from the arm patterns:

- **Result family** (any arm spelled `Result::Ok` / `Result::Err`):
  both arms required OR a `_` catch-all.
- **host-error enum** (heads `host_error::` / `host-error::`):
  every variant from `vdag.errors.wit` required OR a `_` catch-all.
  The diagnostic lists the missing variants.
- **Bool** (any arm spelled `true` or `false`): both arms required
  OR a `_` catch-all.
- **Open literal types** (`int` / `string`): a `_` catch-all is
  mandatory -- the value space cannot be enumerated.

The bare error is `ERR_MATCH_INEXHAUSTIVE` with the kind-specific
hint (the missing Result arm, the missing host-error variants, the
missing bool arm, or "add a `_` catch-all").

### Reachability

An arm after an unguarded catch-all is dead -- the L6 pass raises
`ERR_MATCH_UNREACHABLE_ARM` with the offending arm number. Arms
after a *guarded* catch-all (`n if <pred> -> ...`) are NOT flagged:
the guard may fail at runtime, so a subsequent fallback is the
intended shape.

### Guards

```limceron
fn classify(x: int) -> int {
    match x {
        n if n > 100 -> 999    // guarded: extra runtime predicate
        n            -> n      // unguarded fallback
    }
}
```

A guarded arm fires only when both the pattern matches AND the
guard expression returns `true`. The lowering emits the guard inside
a sub-block; the conditional branch jumps to the body on `true` or
to the next arm on `false`.

### Lowering shape

The IR backend lowers an N-arm match to a chain of predicate blocks
joined by a single `match.merge` PHI:

```
pre:           %v = <subject>
               jmp @match.arm0

match.arm0:    %p0 = cmp_eq %v, <pat0>     ; (no predicate for catch-all)
               br %p0, @match.arm0.body, @match.arm0.next
match.arm0.body:
               <arm body lowered ; tail -> val0>
               jmp @match.merge
match.arm0.next:
               jmp @match.arm1
...
match.merge:
               %r = phi [val0, pred0] [val1, pred1] ...
```

Each non-catch-all literal arm contributes a `cmp_eq` predicate.
Tagged-union arms (`host_error::<v>`) resolve to a `cmp_eq` against
the negative sentinel. The L5 two-arm `Ok` / `Err` shape still
lowers via the `cmp_lt %v, 0` short-circuit -- the L6 general path
kicks in for any non-Result-shaped match.

### Deferred from L6

- Range patterns (`1..=10`) -- parsed since L2, no IR-gen support.
- Struct / tuple destructuring patterns.
- `Option<T>` patterns -- gated on generic ADTs (L9).

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
| `vdag:math` | `sqrt`, `sin`, `cos`, `tan`, `log`, `exp`, `pow` (L7) |
| `vdag:string` | `trim`, `contains`, `starts_with`, `ends_with`, `to_upper`, `to_lower`, `split`/`join` (L7; list shape TODO) |
| `vdag:time` | `now`, `now_millis`, `format`, `parse` (L7) |

## Standard Library (L7)

The L7 stdlib pins a small minimum of math / string / time primitives
that are reachable from every agent. The integer math helpers are
pure-Limceron (zero host-call cost); every other verb is host-backed
through the matching `vdag:<ns>` interface declared above.

Authors call the stdlib with the bare-prefix syntax already used for
`llm.*` / `json.*` / `mcp.*`:

```limceron
let bucket = math.clamp(secs, 0, 60)         // pure builtin, inline
let sq     = math.sqrt(2.0)                  // host: vdag:math.sqrt
let now    = time.now()                      // host: vdag:time.now
let alert  = string.contains(label, "alert") // host: vdag:string.contains
```

The L8 cross-file module system is deferred, but the parser already
accepts `use stdlib::math;` (and friends) as a no-op stub so authors
who write the L8-style import today do not trip a parse error. The
bare-prefix shortcut is reserved at the parser level
(parser.c::`is_host_ns`).

### Standard Library -- `math`

| Verb | Signature | Lowering |
|---|---|---|
| `min(a, b)` | `(int, int) -> int` | **builtin** -- inline phi-fed merge |
| `max(a, b)` | `(int, int) -> int` | **builtin** |
| `clamp(x, lo, hi)` | `(int, int, int) -> int` | **builtin** -- `min(max(x,lo), hi)` |
| `abs(x)` | `(int) -> int` | **builtin** |
| `sign(x)` | `(int) -> int` | **builtin** -- returns -1 / 0 / 1 |
| `sqrt(f)` | `(float) -> float` | host: `vdag:math.sqrt` |
| `sin(f)` | `(float) -> float` | host: `vdag:math.sin` |
| `cos(f)` | `(float) -> float` | host: `vdag:math.cos` |
| `tan(f)` | `(float) -> float` | host: `vdag:math.tan` |
| `log(f)` | `(float) -> float` | host: `vdag:math.log` |
| `exp(f)` | `(float) -> float` | host: `vdag:math.exp` |
| `pow(b, e)` | `(float, float) -> float` | host: `vdag:math.pow` |

The pure-int helpers never emit an `(import "vdag:math" ...)`
declaration -- they are intercepted at IR generation
(`src/ir_gen.c::AST_HOST_CALL`) and lowered to branch / phi IR
inline. The float-domain helpers pass f64 in registers (no buffer
protocol).

### Standard Library -- `string`

| Verb | Signature | Notes |
|---|---|---|
| `trim(s)` | `(string) -> int` | byte count of trimmed result in scratch |
| `contains(s, needle)` | `(string, string) -> bool` | 0/1 |
| `starts_with(s, prefix)` | `(string, string) -> bool` | 0/1 |
| `ends_with(s, suffix)` | `(string, string) -> bool` | 0/1 |
| `to_upper(s)` | `(string) -> int` | byte count of uppercased result |
| `to_lower(s)` | `(string) -> int` | byte count of lowercased result |
| `split(s, sep)` | `(string, string) -> list<string>` | **TODO L3+list** -- degraded path |
| `join(parts, sep)` | `(list<string>, string) -> string` | **TODO L3+list** -- degraded path |

`split` / `join` operate on `list<string>` which has no native repr
in stage0; the wasm emit lowers them to a single-element best-effort
path pending the L3+collections roadmap row. The compile-time
signature still validates because `vdag:string.split` is declared in
`include/vdag.wit`.

### Standard Library -- `time`

| Verb | Signature | Notes |
|---|---|---|
| `now()` | `() -> int` | Unix epoch seconds (s64) |
| `now_millis()` | `() -> int` | Unix epoch milliseconds (s64) |
| `format(ts, layout)` | `(int, string) -> int` | byte count of formatted string in scratch; RFC3339 default |
| `parse(s, layout)` | `(string, string) -> int` | Unix-epoch seconds, or negative `HostErr*` |

`time.format` and `time.parse` use the Go-style reference layout
(`"2006-01-02T15:04:05Z07:00"`) for custom shapes. `time.parse`
participates in the L5 `?` propagator -- a negative return short-
circuits the enclosing fn or `try` block.

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

## Modules (L8)

L8 (2026-05-13) ships multi-file modules with module-relative `use`,
module-scoped symbol mangling, and DFS cycle detection at the
loader. There is no separate package manifest — a "module" is just a
`.lceron` file in the same source tree as the entry point.

### `use` syntax

Module-level `use` declarations appear at the top of a file, BEFORE
any `fn` / `agent` / `struct` / `capability` block:

```limceron
use helpers              // resolves ./helpers.lceron
use util.math            // resolves ./util/math.lceron
use stdlib::math         // L7 stdlib stub still accepted, treated as ./stdlib/math.lceron
```

The dotted form (`use util.math`) walks subdirectories — `.` becomes
`/` for path resolution. The L7 stub `::` form is still accepted for
backward compatibility with samples written against the pre-L8
syntax; the colons normalise to dots before resolution.

L8 limitation: the path after `use` must start with an identifier
(no leading digit). A sibling file named `12_lib.lceron` cannot be
imported as `use 12_lib;` — rename it (e.g. `lib.lceron`) or wrap
the import in a directory (`use widgets.lib;` against
`widgets/12_lib.lceron`... still won't work for the same reason).

### File resolution

| `use` form           | Resolved path                                |
|----------------------|----------------------------------------------|
| `use helpers`        | `<entry_dir>/helpers.lceron`                 |
| `use util.math`      | `<entry_dir>/util/math.lceron`               |
| `use std.io`         | `<stdlib_dir>/io.lceron` (stdlib shortcut)   |
| `use stdlib::math`   | `<entry_dir>/stdlib/math.lceron` (L7 stub)   |

The loader tries `.lceron` first, then `.lceron.md`. A `use` that
matches neither prints a single-line warning and continues; an empty
loader path is never a hard error (matches the legacy L1b
behaviour). A `use` that resolves to a file currently on the
resolution stack is **always** a hard error
(`ERR_CIRCULAR_IMPORT`); see Cycle detection below.

### Visibility

| Form          | Default visibility | Explicit form |
|---------------|--------------------|---------------|
| `fn name() …` | **public**         | `pub fn name() …` |
| `struct T …`  | private            | `pub struct T …`  |
| `enum E …`    | private            | `pub enum E …`    |
| `const K …`   | private            | `pub const K …`   |

Top-level `fn`s are public by default — this matches the L8 spec
(*"All top-level `fn`s in a module are public by default"*) and
makes the common helper-module pattern (`fn greet(name)`) work with
zero ceremony. The `pub` keyword is still accepted and treated as
the explicit-public form; it has no semantic effect on `fn`s but is
required for every other kind to cross the module boundary.

### Module-scoped mangling

To keep the global IR / C symbol space collision-free when two
sibling modules each declare a `fn` with the same name, L8 extends
the pre-existing `lcn___<kebab>_<fn>` agent-method mangling to
top-level fns:

| Source location                         | Emitted symbol             |
|-----------------------------------------|----------------------------|
| Main translation unit, `fn greet(...)`  | `lcn_greet`                |
| `helpers.lceron`, `fn greet(...)`       | `lcn___helpers_greet`      |
| `agent Foo { fn greet(...) }` (in main) | `lcn___foo_greet`          |

The module stem is the file's basename minus `.lceron` (or
`.lceron.md`); hyphens collapse to underscores so the stem is
always a legal WASM / C symbol component. The main file's free fns
intentionally retain the bare `lcn_<fn>` form so existing
single-file callers and the WASM `main` export logic keep working
unchanged.

### Cross-module calls

Once `use helpers;` is in scope, any `fn` from `helpers.lceron` is
callable via the qualified-method shape:

```limceron
use helpers
fn build_label(name: string) -> string {
    helpers.greet(name)            // resolves to lcn___helpers_greet
}
```

Parsing produces an `AST_METHOD_CALL` node with the module stem on
the LHS and the fn name in `->name`; the IR-gen and C-codegen
passes intercept that shape, look up the (stem, name) pair in their
respective module-fn registries, and route the call to the mangled
symbol. No inter-module wasm imports are emitted — every merged fn
lives in the same `.wasm` (or the same C translation unit).

### Cycle detection

The loader runs a DFS over `use` graph. Each file is pushed onto an
ancestor stack when its imports are about to be processed, and
popped on return. A `use` that resolves to a file already on the
stack triggers `ERR_CIRCULAR_IMPORT`:

```
  import: ERR_CIRCULAR_IMPORT — cycle detected at '/tmp/lcn_l8_cycle/a.lceron'
    cycle path:
        /tmp/lcn_l8_cycle/a.lceron
        -> /tmp/lcn_l8_cycle/b.lceron
      -> /tmp/lcn_l8_cycle/a.lceron   (cycle)
  Imports: aborting build (1 circular import error(s))
```

A *diamond* import (file C reached via both B and the entry) is NOT
a cycle — the second visit silently skips the merge step because
the visited set has already absorbed C's decls. Only paths that
land back on an ancestor count.

### Not yet supported (deferred past L8)

- Wildcard imports (`use helpers::*;`)
- Re-exports (`pub use helpers::format_name;`)
- Selective imports (`use helpers::{a, b};`) — full module import only
- Module-level `const` declarations crossing module boundaries
- `./` / `../` relative path syntax in `use` — use the dotted form
  (`use util.math`) for subdirectory imports

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
- Closures / higher-order functions.
- `struct` / `enum` declarations beyond the implicit `Result`.
- Range patterns (`1..=10`) in `match` arms — parsed but no IR-gen.
- Struct / tuple destructuring patterns in `match` arms.
- `Option<T>` patterns — gated on generic ADTs (L9).

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
| `examples/wasm/poc/10_stdlib.lceron` | L7 stdlib: `math.clamp` + `time.now` + `string.contains`. |
| `examples/wasm/poc/12_multifile.lceron` | L8 multi-file: `use multifile_lib;` + cross-module `multifile_lib.format_prompt(...)`. |
| `examples/language/multifile/main.lceron` | L8 fixture for `make test-multifile`: imports `helpers.lceron`. |

## Tooling

L10 introduces a language server + a VSCode extension scaffold.

| Tool | Source | Notes |
|---|---|---|
| `limceron-stage0 lsp` | `src/lsp.c` | Embedded LSP subcommand. JSON-RPC 2.0 over stdio. |
| `build/limceron-lsp` | `make lsp` | Standalone LSP binary (alias around `cmd_lsp`). |
| VSCode extension | `editor/vscode/` | Syntax highlighting + language client. Not published to the marketplace. |

The LSP supports diagnostics-on-save, hover, go-to-definition, and
completion (keywords, builtins, scope identifiers, host modules,
enum variants). The wasm backend additionally emits `vdag.sourcemap`
+ minimal DWARF custom sections so a wazero trap maps back to a
Limceron source position. See [docs/lsp.md](lsp.md) for the full
capabilities matrix, editor setup (VSCode + Neovim + Emacs) and
troubleshooting.

## Cross-references

- [docs/lsp.md](lsp.md) — L10 LSP + source-map chain.
- [editor/vscode/README.md](../editor/vscode/README.md) — VSCode extension scaffold.
- [ADR-0001](adr/0001-wasm-target.md) — wasm target.
- [ADR-0002](adr/0002-result-as-negative-i64-union.md) — Result repr.
- [ADR-0003](adr/0003-entropy-budget-runtime-fence.md) — L11.
- [ADR-0004](adr/0004-budget-runtime-fence-chain-order.md) — L13.
- [ADR-0005](adr/0005-capability-network-compile-time-allowlist.md) — L12.
- [ADR-0006](adr/0006-vdag-json-host-module.md) — L3.
- Visual-DAG: [ADR-0016](../../../Visual-DAG/docs/adr/0016-limceron-execution-substrate.md)
  — execution substrate context for these features.
