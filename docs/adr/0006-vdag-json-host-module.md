# ADR 0006 — vdag:json host module (L3)

## Status
Accepted (2026-05-12). Implemented in commit `9dc3c7c` (Limceron)
and Visual-DAG commit `ba8b8d2` (F36b — host module).

## Context

The Limceron compiler emits host imports under named modules
(`vdag:llm`, `vdag:http`, `vdag:kb`, `vdag:data`, `vdag:mcp`). The
guest sees stable named imports; the host implements them. JSON
parsing was missing — agents that consume structured LLM responses or
MCP tool schemas had no way to traverse a JSON tree from inside the
wasm guest.

The standard alternative (ship a wasm-side JSON parser like `json-c`)
would balloon the artifact size and add an attack surface inside the
sandboxed code. The cleaner shape is host-provided primitives.

## Decision

**Ship `vdag:json` as a new named host module exposing 9 primitive
functions. The host holds the parsed tree in a per-call arena;
guests address nodes via monotonic i32 handles. All errors propagate
through the existing `HostErr*` negative-i32 sentinel space.**

### The nine functions

| Function | Wasm signature | Args |
|---|---|---|
| `parse` | `(i32 i32 i32 i32) -> i32` | bytes_ptr, bytes_len, out_ptr, out_cap |
| `field` | `(i32 i32 i32 i32 i32) -> i32` | handle, key_ptr, key_len, out_ptr, out_cap |
| `array_index` | `(i32 i32 i32 i32) -> i32` | handle, idx, out_ptr, out_cap |
| `length` | `(i32) -> i32` | handle |
| `string_value` | `(i32 i32 i32) -> i32` | handle, out_ptr, out_cap |
| `int_value` | `(i32) -> i64` | handle (returns i64 directly) |
| `bool_value` | `(i32) -> i32` | handle |
| `is_null` | `(i32) -> i32` | handle |
| `stringify` | `(i32 i32 i32) -> i32` | handle, out_ptr, out_cap |

### Handle lifetime

- The host maintains a per-`hostInvocation` `jsonValues map[int32]any`
  and a monotonic `jsonHandles int32` counter, guarded by `jsonMu`.
- A handle is valid only within the activity call that issued it.
- Activity-call end (the `hostInvocation` going out of scope) frees
  the arena.

### Return-code ABI

- Positive return: bytes written to out_ptr (for `parse`, `field`,
  `array_index`, `string_value`, `stringify`); the value (for
  `length`, `bool_value`, `is_null`); the i64 for `int_value`.
- Zero: empty/no-op.
- Negative: `HostErr*` sentinel.

### Missing-field semantics

`field()` on an absent key allocates a JSON-null handle (per the
L3 contract: missing == null for guest code). `is_null(handle)`
returns 1 on that handle.

### Sample WAT (parse + field + int_value)

```wat
(import "vdag:json" "parse"
  (func $hi_json_parse (param i32 i32 i32 i32) (result i32)))
(import "vdag:json" "field"
  (func $hi_json_field (param i32 i32 i32 i32 i32) (result i32)))
(import "vdag:json" "int_value"
  (func $hi_json_int_value (param i32) (result i64)))

;; let handle = json.parse("{\"intent\":\"refund\"}")
i32.const 1028                ;; ptr
i32.const 1028  i32.const 4  i32.sub  i32.load   ;; len at off-4
i32.const 8192  i32.const 1024                   ;; out_buf, out_cap
call $hi_json_parse
i64.extend_i32_s              ;; i32 -> i64 (handle / HostErr)

;; let intent_h = json.field(handle, "intent")
local.get $r4  i32.wrap_i64
i32.const 1052
i32.const 1052  i32.const 4  i32.sub  i32.load
i32.const 9280  i32.const 1024
call $hi_json_field
i64.extend_i32_s
```

`json.int_value` returns i64 directly and bypasses the `i64.extend_i32_s`
tail — the one path that breaks the uniform shape. Missing this would
silently corrupt every JSON-extracted int (top 32 bits clobbered).

## Trade-offs

**Eases**

- Guest-side artifact stays tiny — no wasm JSON parser.
- The host arena is per-call so leaks are bounded by the activity
  lifetime.
- Handles are opaque to the guest, so the host can change its
  internal representation without breaking guests.
- Composes with L5: `json.parse(...)?` short-circuits on parse error.

**Hardens**

- One handle map per `hostInvocation` adds memory pressure under
  pathological "parse 10000 small JSON values" runs. Bounded by the
  out-buffer capacity (1024 B per call site by default).
- Concurrent guests sharing a host invocation must serialise around
  `jsonMu`. v1 doesn't have shared invocations; documented forward.
- The 9-function bundle is all-or-nothing per `CapJSONParse`
  capability. A guest that wants only `parse + field` still pays the
  cost of binding all 9 — acceptable because the host binding is
  cheap.

## Consequences

- Visual-DAG `internal/nodes/code/imports.go` adds the `vdag:json`
  module + `CapJSONParse` constant + `buildJSONNamespace` builder
  (shipped F36b).
- WIT emission for `vdag:json` falls back to placeholder types in v1;
  the WIT_CAP_TABLE extension lands with the host module (out of L3
  scope).
- Limceron's L5 `?` propagator composes naturally — a parse failure
  returns `Err(-8)` (InvalidArg) which `?` propagates.

## Alternatives considered

**A. Guest-side JSON.** Ship a wasm port of `cJSON` or `json-c`.
Rejected — increases artifact size, attack surface, and removes the
clean separation between data layout (host) and computation (guest).

**B. Single combined function `json.eval(path)`.** Simpler API,
loses the ability to compose `field` + `array_index` programmatically.

**C. Tag-encoded i64 (no handles).** Pack a small JSON node into an
i64 directly. Works for booleans and integers; fails for strings and
nested structures.

**D. Bring-your-own arena.** Guest passes a memory region; host
writes nodes into it. Forces the guest to implement allocator logic.

## Open questions

- **Concurrent host invocations** — v1 has one invocation per
  activity call; future fan-out may share. Mitigation: shard handle
  maps per goroutine if needed.
- **Out-buffer overrun** — `parse` of a large JSON exceeding the
  guest's `out_cap` returns `HostErrTooLarge = -6`. Today the
  default cap is 1024 B; some flows want 64 KiB. Future work.
- **Streaming parse** — for very large JSON the round-trip through
  the out-buffer is wasteful; a streaming variant could ship in
  stage1.

## Implementation order

1. Parser: `json` to host-namespace gate.
2. IR-emit: 9 entries in host-import signature table.
3. Per-call-site marshalling (uses existing `HostCallSite` 1024-byte
   out-buf at `WASM_HOST_SCRATCH_BASE + idx * WASM_HOST_SCRATCH_STRIDE`).
4. IR tests (10): one per primitive + a chained parse → field →
   string_value pipeline.
5. Two wasm examples.
6. Visual-DAG host module (F36b).

## Cross-references

- [language reference: host calls](../language-reference.md#host-calls)
- [ADR-0002](0002-result-as-negative-i64-union.md) — `?` composes
  with json.* return codes.
- Visual-DAG: [`docs/api/knowledge.md`](../../../../Visual-DAG/docs/api/knowledge.md)
  — F36b vdag:json host implementation.
- Examples: `examples/wasm/json/`.
