# Limceron — AI Agent Instructions

## What is this?

Limceron is a programming language for AI agents. The compiler is written in C (Stage 0) and self-hosts in Limceron (Stage 1). It compiles to C99 or native assembly (x86_64/aarch64).

## Post-task persistence (non-negotiable rule)

> After finishing any task, before closing the response to the user, always update — in this order:
> 
> 1. **Persistent memory** (`/Users/mikelcarozzi/.claude/projects/-Users-mikelcarozzi-Documents-Desarrollo-IA-Limceron-lang/memory/`) — add/update `user`, `feedback`, `project`, `reference` entries.
> 2. **`LEARNINGS.md`** — record mistakes made and how they were fixed (see the learnings loop below).
> 3. **`CLAUDE.md`** — if the task revealed a new operating rule, project convention, frequent command, or architectural decision, add it here.
> 4. **`ROADMAP.md`** — update whenever a bootstrap stage (Stage 0/1/2) or an L1-L13 feature is closed or replanned. This is the repo's living roadmap.
> 5. **`docs/adr/`** — if the task changed architecture, contracts (`include/vdag.wit`), or technical decisions, add a new ADR or update the relevant one.
> 6. **Repo artifacts** — `README.md`, `docs/language-reference.md`, WIT contracts (`include/vdag.wit`, `include/vdag.errors.wit`) if the task touched them.

**Why this matters:** We can't afford to lose valuable context to sessions that closed without persisting. Every task that skips this step becomes technical debt that resurfaces next session and forces re-explaining everything. The rule exists so the next session starts with ALL of the previous session's knowledge.

**How to apply it:**

- Don't rely solely on conversation context. Assume the session can end at any moment.
- If a decision took more than 5 minutes of discussion, capture it in the right file **before** implementing.
- When a task is done, tell the user in one line what was updated (e.g. "Updated memory + LEARNINGS + CLAUDE.md").

## Automatic improvement loop (Boris Cherny pattern)

> Every time you make a mistake — and every time the user corrects you — document it in `LEARNINGS.md` with:
> 
> 1. **Symptom** — what went wrong or what failed.
> 2. **Root cause** — why it happened (not surface-level: the real cause).
> 3. **Rule** — what to do differently next time, as an executable instruction.
> 4. **Trigger** — how to recognize the situation in the future so the rule gets applied.

**Rules for the loop:**

- **Document every mistake**, even minor ones. The value comes from accumulation.
- **If a rule in `LEARNINGS.md` gets applied 3+ times and holds up, promote it to `CLAUDE.md`** as a project convention. `LEARNINGS.md` is the lab; `CLAUDE.md` is the doctrine.
- **Before starting non-trivial tasks, read `LEARNINGS.md`**. This prevents repeating already-documented mistakes.
- **If a rule turns out wrong given new evidence, mark it `DEPRECATED`** and record why — don't delete it, the history has value.

**Entry format for `LEARNINGS.md`:** see the template at the top of that file.

## Code rules

- **Language:** comments and identifiers in **English** — that's the convention across the entire existing codebase (`src/`, `runtime/`, `stage1/`, `test/`); don't introduce Spanish comments.
- **Comments:** none by default. Only when the "why" isn't obvious (a specific workaround, a subtle invariant, the root cause of a past bug — don't restate what the code already says).
- **No speculative features.** Don't add abstractions for "future cases". Three similar lines beat a premature abstraction.
- **Strong typing everywhere:** explicit C99 in the compiler/runtime; in Limceron itself, `Result<T,E>`, generics monomorphization, exhaustive tagged enums in `match`.

## Architecture

```
src/            Compiler source (C99)
  lexer.c       Tokenizer (70+ tokens, auto-semicolons)
  parser.c      Recursive descent + Pratt precedence
  typecheck.c   10 passes: caps, access control, taint, ownership, etc.
  codegen.c     C99 transpiler (~10K LOC, 90+ builtins)
  ir_gen.c      SSA IR generator + optimizer
  ir_emit_arm64.c  ARM64 assembly emitter
  ir_emit_x86.c    x86_64 assembly emitter
  lsp.c         Language Server Protocol (JSON-RPC)
  package.c     Package manager (TOML, semver)
  target.c      Cross-compilation targets
  markdown.c    .lceron.md parser (Markdown-as-source)
  main.c        CLI: build, run, emit, fmt, doctor, lsp, compile, ir, init, add, install, publish, targets, audit
include/
  lcn.h         AST node types, public API
runtime/        C runtime library (30+ files)
  llm.c         LLM HTTP calls + local ONNX intercept (endpoint:"local")
  entropy.c     Shannon entropy tracker + budget checker
  supervisor.c  Supervisor strategies (one_for_one, all_for_one, rest_for_all)
  mesh.c        Fan-out/fan-in parallel pipelines
  onnx_model.c  ONNX Runtime inference + WordPiece tokenizer
  ...
stage1/         Self-hosted compiler (.lceron files)
stdlib/         Standard library (12 modules)
test/           Test suites (424+ tests, 2000+ assertions)
```

## Building

```bash
make stage0         # Build the compiler
make test           # Run 424+ tests
make test-ir        # Run 65 IR tests
```

The binary is `build/limceron-stage0`. Users call it `limceron`.

## Key design decisions

- **C prefix**: `lcn_` for functions, `Lcn` for types, `LCN_` for macros
- **File extension**: `.lceron` (source), `.lceron.md` (Markdown-as-source)
- **Naming**: Binary is `limceron`, internal bootstrap stages use `limceron-stage0`
- **Codegen**: Transpiles to C99, links against runtime .o files
- **Self-hosting**: Stage 0 (C) → Stage 1 (Limceron) → Stage 2 → Stage 3 (fixed point verified)
- **Ownership checker**: Advisory mode (warnings), Copy types: int, bool, float, handles, method results, loop vars
- **Entropy budget**: Runtime tracks confidence/entropy per ask(), circuit breaker on threshold violation
- **Local models**: `endpoint: "local"` in agent → routes ask() to ONNX model via lcn_model_predict()

## Commit conventions

- Gitmoji in commit messages
- Commit message in Spanish, technical terms in English
- NO co-authored-by tags
- Commit message goes in `cmsg.txt`, user commits manually: `git add -A && git commit -F cmsg.txt`

## Testing

```bash
make test           # Stage 0 (424 tests)
make test-ir        # SSA IR (65 tests)
bash stage1/test_stage1.sh      # Stage 1 (25 tests)
bash stage1/test_parity.sh      # Output parity Stage 0 vs 1 (10 tests)
bash stage1/test_bootstrap.sh   # Bootstrap chain (11 tests)
```

## Language principles

1. **Near-zero learning curve**: Markdown IS source code. Syntax reads like pseudocode.
2. **Empathy-driven**: All docs lead with fear/pain, then solution. Never list features in isolation.
3. **Agents cannot exist without guardrails**: capability, budget, entropy_budget are enforced by the compiler.

## What NOT to do

- Don't add features without tests
- Don't break self-hosting (run bootstrap tests after parser/codegen changes)
- Don't commit .env, credentials, client-specific data
- Don't use `lcn` as binary name — it's `limceron`
- Don't add emojis to code unless explicitly asked
- Don't refactor beyond what was asked

## Common tasks

### Add a new builtin function

1. Add to `is_codegen_builtin()` skip list in codegen.c (~line 42)
2. Add type inference in `cg_infer_type_emit()` if needed
3. Add emission in the builtin call section (~line 2200)
4. Add runtime implementation if needed (runtime/*.c)
5. Add test in test/test_runner.c
6. Add to Stage 1 codegen.lceron (is_builtin + emit handler)

### Add a new AST node type

1. Add to `AstKind` enum in include/lcn.h
2. Add to `ast_kind_name()` in parser.c
3. Add parsing in parser.c
4. Add codegen in codegen.c
5. Add to Stage 1 parser.lceron + codegen.lceron
6. Add to markdown.c if it should be expressible in .lceron.md
7. Add tests

### Add a new markdown section

1. Add `MD_SECTION_*` enum in markdown.c
2. Add classification in `md_classify_section()`
3. Implement `md_parse_*()` function
4. Add switch case in main parse loop
5. Add test in test_runner.c

## Current state

Point-in-time project status (test counts, stage completion, subsystem status) lives in
`ROADMAP.md`, not here — see the "Overall Project" milestone table and the per-stage sections
above it. Keep it updated there per the post-task persistence rule. 
