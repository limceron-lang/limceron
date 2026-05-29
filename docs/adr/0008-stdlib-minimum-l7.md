# ADR-0008: Stdlib minimum (L7) -- math + string + time

**Status:** Accepted (2026-05-13)
**Roadmap row:** L7
**Subject:** Pin a small, host-backed standard library so authors can
reach for `math.sqrt`, `string.contains`, `time.now` without
re-declaring them in every agent.

---

## Context

L7 is the "useful from day one" row of the wasm-poc ROADMAP. Without
a stdlib, every agent that needs RFC3339 timestamp formatting, simple
string predicates, or `sqrt` has to either:

- declare its own `extern fn` import (and miss the compile-time
  capability fence), or
- inline an ad-hoc helper in Limceron source per agent.

Both shapes leak into agent code and undermine the L1b contract that
`include/vdag.wit` is the single source of truth for the wasm guest
ABI surface.

## Decision

### Three new `vdag:*` interfaces

`include/vdag.wit` ships three additional interfaces:

| Interface | Surface |
|---|---|
| `vdag:math` | float-domain `sqrt`, `sin`, `cos`, `tan`, `log`, `exp`, `pow`; integer helpers `min`, `max`, `clamp`, `abs`, `sign` |
| `vdag:string` | `trim`, `contains`, `starts_with`, `ends_with`, `to_upper`, `to_lower`; placeholders `split` / `join` (TODO L3+list) |
| `vdag:time` | `now`, `now_millis`, `format`, `parse` |

The integer math helpers (`min` / `max` / `clamp` / `abs` / `sign`)
are pinned in the contract for completeness only -- they are
intercepted by the front-end at IR-gen
(`src/ir_gen.c::AST_HOST_CALL`) and lowered inline as compiler
builtins. No `(import "vdag:math" "clamp" ...)` declaration is ever
emitted. Authors writing `math.clamp(x, 0, 60)` pay zero host-call
cost.

The float-domain math verbs pass plain WASM f64s in registers; no
buffer-protocol scratch slot is required. The IR-gen pass tags
`IR_HOST_CALL` instructions for those verbs with `IR_TYPE_F64` so the
SSA slot allocator and the wasm backend's `wasm_type_name` consistently
agree on the local declaration.

The boolean string predicates (`string.contains` / `starts_with` /
`ends_with`) return 0/1 i32 directly. The IR-gen pass tags them
`IR_TYPE_BOOL` so a `fn foo() -> bool { string.contains(...) }` shape
lines up with the wasm `(result i32)` Limceron emits for `bool`
returns -- no `i64.extend_i32_s` post-call lift required.

### Front-end namespace reservation

`parser.c::parse_expr` extends the `is_host_ns` set with `math`,
`string`, `time`. Authors call the stdlib with the bare-prefix form
(`math.sqrt(x)`, `time.now()`) the way they already call `llm.*` /
`json.*`. The parser also accepts `use stdlib::math;` (and friends)
as a no-op stub so authors who follow the L8 cross-file import
convention do not trip a parse error today; the `::` separator is
normalised onto `.` for the stored path.

### Ownership-pass exceptions

`typecheck.c::own_check_stmt` registers AST_HOST_CALL results as
Copy. Without this the L7 pattern
`let secs = time.now(); ... math.clamp(secs, 0, 60)` trips the
ownership pass on the second use of `secs`. The same pass also marks
scalar-typed (`int` / `bool` / `float` / `string`) function
parameters as Copy so
`fn f(label: string) -> bool { string.contains(label, "x") }`
type-checks cleanly.

## Considered alternatives

1. **Inline pure-Limceron stubs in `stdlib/math.lceron`** -- rejected
   because stage0 does not yet have cross-file `use` resolution. The
   L8 module system would be a prerequisite. The bare-prefix
   shortcut is the smallest move that ships L7 today and keeps the
   `stdlib/*.lceron` files as forward-looking reference material.
2. **Always route math through the host** -- rejected because the
   pure-int helpers compose better with the rest of Limceron's
   integer arithmetic when inlined; routing them through the wasm
   import barrier would mean a trap-path on every call and a
   per-site scratch slot. The five integer helpers are closed-form
   expressions that fit in a handful of branch / phi IR
   instructions.
3. **Ship `split` / `join` with a real `list<string>` repr** --
   rejected because the L3+collections roadmap row is the precursor
   to a native list type. Shipping `split` / `join` today as
   degraded single-element placeholders (the host returns the first
   element / a single joined string into the scratch slot) keeps the
   `vdag:string.split` verb pinned in the contract so call sites can
   be written today and upgraded transparently when L3+collections
   lands.

## Consequences

- Authors get `math.clamp` / `time.now()` / `string.contains` today.
- `Visual-DAG/internal/nodes/code/imports.go` MUST export the new
  verbs before .wasm modules using them will instantiate at wazero
  load time (tracked as F36-stdlib). The compile-time check still
  passes because the names are declared in `include/vdag.wit`, but
  the missing host stubs will trip linker failures otherwise.
- `split` / `join` ship as best-effort placeholders pending the
  L3+collections roadmap row (`list<T>` primitives).
- The L8 cross-file module system can reach these helpers through
  `use stdlib::math;` once cross-file imports land; the bare-prefix
  shortcut becomes opt-in at that point.

## Attribution note

The L7 implementation work (vdag.wit surface, ir_gen / ir_emit_wasm
lowering, parser host-namespace extension, typecheck ownership
exceptions, language-reference docs, end-to-end sample
`examples/wasm/poc/10_stdlib.lceron`, and 14 new tests across
`test/test_runner.c` + `test/test_ir.c`) landed in the same
checkout cycle as L4 string interpolation. Because both rows
extend the same source files (`include/vdag.wit`,
`src/ir_emit_wasm.c`, `src/ir_gen.c`, `src/parser.c`,
`src/typecheck.c`, `test/test_ir.c`, `test/test_runner.c`),
the two row's hunks are co-located in the L4 commit
(54fa2242 / 2fbb3f6 after re-apply). This ADR is the L7 marker
commit that distinguishes the L7 deliverable from the L4
deliverable; see git blame on the L7 markers (search for
"L7 (2026-05-13)" comments) for the per-hunk attribution.

## References

- `examples/wasm/poc/10_stdlib.lceron` -- end-to-end sample
  exercising `math.clamp`, `time.now`, `string.contains` in a single
  agent.
- `docs/language-reference.md` §Standard Library -- author-facing
  surface documentation.
- `include/vdag.wit` -- canonical contract definitions.
- ADR-0006 (vdag:json) -- the precedent for shipping a host-backed
  module under a `vdag:*` namespace.
