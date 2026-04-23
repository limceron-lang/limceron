# Limceron Roadmap — Compiler Stages & Feature Distribution

## Stage Assessment

### Stage 0 — C99 Bootstrap Compiler (CURRENT)

| Component | LOC | Status |
|---|---|---|
| Lexer | 978 | Complete — 70+ tokens, auto-semicolons, escape sequences, Rust-style error diagnostics |
| Parser | 2,654 | Complete — recursive descent + Pratt precedence, secret type modifier |
| Type Checker | 3,846 | Complete — 10 passes: capabilities, access control, taint, secret, budgets, pub/priv, generics |
| Codegen | 7,322 | Complete — C99 transpiler, 90+ builtins, closures, defer LIFO, generics monomorphization, capability fence |
| Main / Driver | 1,170 | build, run, emit, parse, lex, audit, init + multi-file imports |
| Markdown parser | 1,406 | Complete — .lceron.md → AST (memory, kb, entropy_budget, access control) |
| Tests | 4,739 | **290 tests, 1,155 assertions — all passing** |
| Runtime (25 .c + 25 .h) | ~12,000 | Budget, LLM, MCP client+server, channels, threads, select, entropy, drift, delegation, memory, KB, dashboard, MySQL, Postgres, ONNX, access control, capability fence, string utils, JSON, stdlib |
| **Total (excl. sqlite3)** | **~44,000** | |

### Stage 1 — Self-Hosting (Limceron compiled by Stage 0) — IN PROGRESS

| Component | LOC | Status |
|---|---|---|
| `lexer.lceron` | 499 | Complete — tokenizes its own source (19,835 bytes → 3,447 tokens) |
| `parser.lceron` | 1,757 | Complete — parses its own source (10,567 tokens → S-expression AST) |
| `typecheck.lceron` | 2,677 | Complete — 4-pass type checker, verified end-to-end |
| `codegen.lceron` | 2,243 | Complete — C99 code generator, verified end-to-end |

### Stage 2 — Full Self-Hosting — NOT STARTED

`stage2(source) == stage2(stage2(source))` — compiler compiles itself, output is identical.

---

## Implementation Status — What Works

### Fully Functional (parsed + typechecked + codegen + runtime)

| Feature | Details |
|---|---|
| Agents | Declaration, capabilities, model, endpoint, api_key, budget, entropy_budget, guards, prompt |
| Access control | `allow endpoint`, `allow binary`, `allow path`, `deny private_ranges`, `default: deny` |
| Runtime capability fence | Tool dispatch interceptor — validates ToolCall against agent's allowed tools at runtime |
| SSRF prevention | Compile-time: 10/8, 172.16/12, 192.168/16, 127/8, 169.254/16 blocked |
| Taint tracking | `taint X` → wrapper struct, compile-time flow analysis |
| Secret type | `secret string` → compile-time leakage prevention, `secret_redact()` / `secret_unwrap()` |
| Shannon entropy | `result.confidence`, `result.entropy` on every `ask()` via logprobs |
| Entropy budget | `max_avg_entropy`, `max_low_confidence`, `max_drift` — ring buffer + budget checking |
| Drift detection | KL-divergence, Jensen-Shannon divergence (runtime library) |
| Invariants | Real function generation, wired to entropy/drift runtime |
| Concurrency | Thread pool (pthreads), typed channels (ring buffer), select multiplexing |
| Capability delegation | `delegate()`, `revoke()`, `revoke_all()`, `has_capability()` — Hurd-inspired |
| Generics | `Result<T,E>`, `Option<T>`, `T?` — monomorphization with typed constructors and match |
| Closures | Function pointers + capture structs in C99 |
| defer LIFO | Scope-exit cleanup stack, reverse execution order |
| Native MySQL driver | `use driver("mysql")` → libmysqlclient binding, direct TCP |
| Native Postgres driver | `use driver("postgres")` → libpq binding, auto-detected |
| ONNX model binding | `use model("file.onnx")` → conditional libonnxruntime + WordPiece tokenizer |
| MCP client | `use mcp("command") as alias` → JSON-RPC over stdio |
| MCP server | `--serve` flag → agent exposes itself as MCP tool server |
| Agent memory | SQLite-backed, sessions, TTL, compaction, JSONL audit trail |
| Knowledge base / RAG | FTS5 full-text search, document ingestion, auto-context injection |
| Dashboard | 18 REST endpoints: agents, events, metrics, budget, guards, memory, KB |
| ADTs | `enum Token { Ident(string), Int(int), Eof }` with pattern matching |
| Structs + impl | Struct literals, method dispatch, field access |
| for/while/loop | With break/continue |
| match | Exhaustive pattern matching with field extraction, Result/Option typed match |
| FFI | `extern fn` for C library binding |
| Multi-file modules | `mod`/`use` with cycle detection, stdlib imports |
| pub/priv visibility | Pass 8 in typecheck — advisory warnings |
| Syntactic sugar | `keep where` (filter), `each` (map), `try/otherwise` (error fallback) |
| Error messages | Rust-style source snippets with line:col, carets, hints |
| Audit CLI | `limceron audit` — decision paths, LLM calls, entropy score |
| Init CLI | `limceron init` — project scaffold |
| 12 stdlib modules | io, string, json, math, time, env, log, retry, batch, budget, tokens, trace |

### Partially Functional (parsed + partial codegen)

| Feature | What works | What's missing |
|---|---|---|
| Supervisors | Parsed, struct generated | Strategy execution, restart logic |
| Mesh pipelines | Parsed, sequential codegen | Parallel fan-out/fan-in, error handlers |
| Inference router | Parsed, model selection codegen | Health checking, cost tracking |
| A2A protocol | Parsed, URL constant generated | Real HTTP client calls |

### Parsed Only (no codegen)

| Feature | Spec Section | Estimated LOC to implement |
|---|---|---|
| Traits / interfaces | 2.6-2.7 | ~1,500 |
| Union types | 2.3 | ~600 |
| Ownership / borrowing | 3 | ~3,000 (borrow checker) |
| comptime | 8 | ~2,000 (compile-time evaluator) |
| Deterministic replay | 23 | ~1,500 |
| Structured concurrency (TaskGroup) | 5.4 | ~500 |
| Gradual safety mode | 10.5 | ~200 |

---

## Completion Percentages

### Stage 0 Compiler — COMPLETE

Stage 0's job: compile enough Limceron to build Stage 1. That job is done.

| Area | Status | Notes |
|---|---|---|
| Lexer | Done | 70+ tokens, auto-semicolons, escape sequences, Rust-style diagnostics |
| Parser | Done | Full Limceron syntax — all constructs parsed, secret type modifier |
| Type checker | Done | 10 passes: capabilities, access control, taint, secret, budgets, generics, pub/priv |
| Codegen | Done | C99 transpiler, 90+ builtins, closures, defer LIFO, generics, capability fence |
| Runtime | Done | LLM, MCP, dashboard, memory, KB, entropy, drift, threads, channels, MySQL, Postgres, ONNX, capability fence |
| Tests | Done | 290 tests, 1,155 assertions |
| CLI | Done | build, run, emit, parse, lex, audit, init |
| Docs | Done | README, getting-started, spec (30 sections), grammar EBNF, ROADMAP |

Features like traits, ownership, comptime are spec features for Stage 2 — not the bootstrap compiler.

### Stage 1 Self-Hosting — 70%

| Area | % | Notes |
|---|---|---|
| Lexer in Limceron | 100% | `lexer.lceron` — tokenizes itself |
| Parser in Limceron | 100% | `parser.lceron` — parses itself |
| Type checker in Limceron | 100% | `typecheck.lceron` — 4-pass, verified end-to-end |
| Codegen in Limceron | 100% | `codegen.lceron` — C99 emitter, verified end-to-end |
| Full pipeline verification | 80% | lex→parse→typecheck→codegen→gcc→run works, some false positives on self-referential code |

### Stage 1 Roadmap Phases (from plan)

| Phase | Objective | % Complete |
|---|---|---|
| **Phase 1**: Shannon Entropy + Quick Wins | `result.confidence`, `?` operator, drivers | **100%** — entropy, postgres, closures, defer all done |
| **Phase 2**: Concurrency | spawn, await, channels, select, thread pool | **90%** — all codegen+runtime done, typecheck shared-var checks pending |
| **Phase 3**: Entropy Budget + Drift | Ring buffer, KL-divergence, invariants, audit CLI | **95%** — runtime done, invariant wiring done, invariant run-loop integration pending |
| **Phase 4**: ONNX + Generics + Launch | ONNX real, generics, extern fn, docs | **85%** — ONNX done, generics done, extern fn done, docs pending update |

### Overall Project

| Milestone | Status |
|---|---|
| Stage 0 bootstrap compiler | **COMPLETE** — compiles Stage 1, 24 examples, production agents |
| Stage 1 self-hosting | **70%** — 4/4 pipeline components written and verified |
| Stage 2 full self-hosting | **0%** — ownership, traits, comptime, native backend |
| Production validation | **COMPLETE** — medical categorizer: LLM 82.6%, BERT 93.1% |
| Documentation | **85%** — README needs update for new features |

---

## Security Audit: Capability Enforcement (2026-04-10)

### What's Enforced

| Feature | Compile-Time | Runtime | Verdict |
|---------|---|---|---|
| Endpoint policies (static URLs) | ✅ Compiler error | — | **ENFORCED** |
| Binary policies (static paths) | ✅ Compiler error | — | **ENFORCED** |
| Path policies (static paths) | ✅ Compiler error | — | **ENFORCED** |
| SSRF private range detection | ✅ Compiler error | — | **ENFORCED** |
| Capability delegation | ✅ Type-safe | Bitwise AND | **ENFORCED** |
| Tool requires vs agent caps | ✅ Compiler error | Bitwise assert | **ENFORCED** |
| **Tool calls via ask()** | — | ✅ `lcn_capability_check_tool()` | **ENFORCED** (P0 done) |
| Secret type leakage | ✅ Compiler error | — | **ENFORCED** |

### What's Partially Enforced

| Feature | Compile-Time | Runtime | Gap |
|---------|---|---|---|
| Endpoint policies (dynamic URLs) | Flagged unsafe | `lcn_fetch_checked()` | Works, but static calls skip runtime check |
| Binary policies (dynamic paths) | Flagged unsafe | `lcn_exec_checked()` | Same gap |
| Path policies (dynamic paths) | Flagged unsafe | `lcn_*file_checked()` | Same gap |
| MCP tool dispatch | — | Fence globals available | Needs per-dispatch wiring |

### Remaining Gaps

| Feature | Risk | Fix LOC |
|---------|---|---|
| Static call runtime bypass | **MEDIUM** — no defense-in-depth | ~100 |
| Dashboard capability view | **MEDIUM** — operators blind to permissions | ~300 |
| ask() policy parameter unused | **LOW** — fence covers tool calls, but ask() stub still ignores policy | ~100 |

---

## Immediate Priorities (next sprint)

1. **P1: Static call defense-in-depth** (~100 LOC) — all calls go through `_checked` variants
2. **FFI link directive** (~100 LOC) — `link "-lssl -lcrypto"` for consuming C libraries
3. **limceron fmt** (~500 LOC) — code formatter
4. **README update** — reflect new features (generics, secret, postgres, fence, error messages)

## Medium-Term

5. Dashboard capability view (~300 LOC)
6. Supervisor strategy execution (~500 LOC)
7. `limceron doctor` (~300 LOC) — verify runtime dependencies
8. Structured concurrency / TaskGroup (~500 LOC)

## Long-Term (Stage 2)

9. Ownership / borrow checker (~3,000 LOC)
10. Traits / interfaces codegen (~1,500 LOC)
11. Custom SSA IR + native code emission
12. Package manager / skill registry
13. Green threads / coroutines

---

## Agent OS Kernel Gaps (informed by Linux kernel architecture)

| Subsystem | Unix Equivalent | Limceron Status |
|---|---|---|
| Scheduler | CFS | **Done** — thread pool + spawn/await/select |
| IPC | pipes, signals | **Done** — typed channels, tell() |
| Access control | SELinux, seccomp | **Done** — compile-time + runtime capability fence |
| VFS | mount, /dev | **Partial** — `use driver()` + `use mcp()`, unified `use data()` planned |
| FFI | syscalls, ioctl | **Done** — `extern fn` (needs `link` directive for external libs) |
| Supervisor | systemd, init | **Partial** — parsed, struct generated, strategy execution pending |
| Memory management | mmap, slab | **Partial** — arena allocators, no ownership/borrow checker |
| Entropy runtime | (no equivalent) | **Done** — Shannon entropy, drift detection, entropy budget |
| Capability delegation | Hurd capabilities | **Done** — delegate/revoke/revoke_all/has_capability |

---

## Licensing

| Component | License | Directory |
|---|---|---|
| Compiler + Runtime | Apache 2.0 | `compiler/`, `LICENSE` |
| Specification + Grammar | CC-BY 4.0 | `spec/` |
| Standard Library | MIT | `stdlib/` |
| Examples | MIT | `examples/` |
