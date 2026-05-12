# ADR 0001 — wasm32-wasip2 as primary multi-tenant compilation target

## Status
Accepted (2026-05-09)

## Context

Limceron's three pillars — capabilities (what an agent may touch),
budgets (what an agent may spend), entropy (how unsure an agent may
become) — have until now been enforced for a single-tenant native
deployment model. The compiler emits C99, gcc/clang produces a ~1.2 MB
binary, and the operator runs one process per agent. That model is
correct for embedded and edge use cases and does not change.

The Visual DAG project (multi-tenant SaaS workflow engine) consumes
Limceron as an embedded compute layer: hundreds to thousands of
tenants, each running arbitrary Limceron code inside a single Go host
process. C99 + OS-level sandboxing (gVisor, Firecracker, per-tenant
pods) does not compose at this scale — both the infrastructure cost
and the operational surface grow linearly in tenants. A PoC was run
on `feature/wasm-poc` (commit `b554fcf`) to determine whether
wasm32-wasi-preview2 + wazero can replace the OS-level sandbox while
preserving Limceron's compile-time guarantees. This ADR records the
result.

## Decision

WASM (`wasm32-wasi-preview2`) becomes the **primary target for SaaS
and multi-tenant deployments**. C99 native (via `cc`), `ir_emit_arm64`
and `ir_emit_x86` direct backends remain as **secondary targets** for
embedded systems, single-tenant edge, and benchmark/measurement work.

## Rationale

Each item below is grounded in a measurement from
`poc-handoff/findings/findings.md` (darwin/arm64, M4 Max, wazero
v1.11.0, 2026-05-08):

- **Multi-tenant isolation holds under stress.** 4 goroutines × 1000
  iterations = 4000 instantiations writing per-tenant sentinels into
  per-instance linear memory. Zero cross-reads observed. wazero's
  per-instance memory model is honored.
- **WIT contract verification at load time.** A module importing
  `evil.exec` is rejected by wazero at instantiation against a host
  exporting only `data.read`. This augments compile-time capability
  enforcement with a load-time check the host operator controls.
- **Cold start is ~24× under budget.** 70–205 µs measured (target
  was 5 ms). Includes `NewRuntime` → `InstantiateWithConfig` →
  first `Call`.
- **Warm start is ~2–4× under budget.** 11–28 µs average,
  22–43 µs p95 across three placeholder modules (target was 100 µs).
- **Module size budget headroom is large.** Placeholders are
  54–91 B. Real emitter output for `fn main → 42` is 194 B (well
  under the 200 KB target). Realistic activity-shaped programs will
  grow but remain comfortably inside budget.
- **The pipeline shape is already proven.** `ir_emit_arm64.c` (1002
  LOC) and `ir_emit_x86.c` (833 LOC) demonstrate the SSA → backend
  emitter pattern. `ir_emit_wasm.c` is 1172 LOC and follows the
  same shape (dispatch-loop CFG, SSA walker). `wit_emit.c` is
  570 LOC and lowers the agent capability set to a WIT contract.

## Consequences

**Positive:**

- N tenants × M workflows in a single Go host process via wazero,
  replacing N pods / N firecracker VMs.
- Browser preview becomes feasible — the same `.wasm` artifact runs
  in a JS host shim with no separate compilation path.
- Multi-arch is solved by construction. One `wasm32-wasi-preview2`
  binary runs on macOS arm64, Linux x86_64, Linux aarch64, Windows.
- WIT-as-capability-contract gives operators and auditors a ~30-line
  declarative file in place of 12,000 LOC of C runtime to read.

**Negative / costs:**

- Runtime port is staged. The current `runtime/` (~12K LOC of C)
  splits into three tiers:
  - **Tier 1 (compute, ports directly):** entropy, drift, budget,
    json, kb, capability_fence, access_control, memory, stdlib_rt.
  - **Tier 2 (replaced by WASI APIs):** http → wasi-http; threads →
    wasi-threads or Component Model async.
  - **Tier 3 (becomes host imports via Component Model):** postgres,
    mysql, sqlite, onnx, mcp.
- Native database drivers (`libpq`, `libmysqlclient`) are not
  available inside the WASM target. Agents access databases through
  capability-typed host imports. We treat this as a feature: the
  host, not the guest, owns the connection pool and the credential.
- WIT is structural and cannot express `entropy_budget` statically.
  That pillar remains a runtime check inside the guest. We note this
  honestly rather than overclaim the contract surface.

## Trade-offs explicit

- **Performance.** WASM is 10–30 % slower than native in tight
  arithmetic and pointer-chasing loops. For agent workloads, where
  >99.9 % of wall time sits in LLM HTTP round-trips, the end-to-end
  impact is on the order of 0.0006 %. Acceptable.
- **Self-hosting.** Stage 1 (Limceron compiling itself to C99) is
  unaffected. The bootstrap chain Stage 0 → 1 → 2 → 3 still runs
  through C99. Adding the wasm backend adds an emitter; it does not
  perturb the fixed-point proof.

## Multi-target strategy going forward

| Target               | Use case                                    | Status                  |
|----------------------|---------------------------------------------|-------------------------|
| wasm32-wasi-preview2 | SaaS / multi-tenant / browser preview       | **primary** (this ADR)  |
| native (cc)          | Embedded / single-tenant edge / benchmarks  | retained                |
| ir_emit_arm64.c      | Direct native, no C compiler                | retained, niche         |
| ir_emit_x86.c        | Direct native, no C compiler                | retained, niche         |

## Implementation reference

- `src/target.c` — triple parsing (`wasm32-wasi-preview2`),
  cross-cc detection, `wat2wasm` discovery.
- `src/ir_emit_wasm.c` — SSA → WAT walker, 1172 LOC,
  dispatch-loop CFG.
- `src/wit_emit.c` — capability set → WIT contract, 570 LOC.
- `Makefile` — `make poc-wasm` target.

## Open items (not blocking the decision)

- Agent-scoped functions (`agent { fn ... }`) currently lower to
  top-level — tracked under F1-A2.
- Component Model imports for db / llm / http — Phase 2.
- Source-map chain DAG ↔ Limceron ↔ WAT ↔ DWARF — Phase 2.
- SHA256 binary verification chain across stages 6 → 7 — tracked
  F1-A4 in Visual DAG.

## Alternatives considered

- **Stay on C99 + gVisor / Firecracker for sandboxing.** Estimated
  $5K–15K/month infra at 1K tenants vs ~$200/month with WASM-in-Go.
  Operational complexity is roughly 10× higher (per-tenant pod
  lifecycle, image registry, per-tenant network policy). Rejected
  as primary; acceptable as defense-in-depth on top of WASM.
- **Fork Dify FE and reuse its runtime.** Dify's licensing carries
  commercial restrictions incompatible with Limceron's
  Apache-2.0 / MIT dual license. Rejected.
- **Native binary signing + OS-level sandbox only.** Per-tenant pod
  overhead does not compose at thousands of tenants. Rejected as
  primary; useful as a second layer.

## Authors
