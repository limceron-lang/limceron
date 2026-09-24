# Limceron — AI Agent Instructions

## What is this?

Limceron is a programming language for AI agents. The compiler is written in C (Stage 0) and self-hosts in Limceron (Stage 1). It compiles to C99 or native assembly (x86_64/aarch64).

## Persistencia post-tarea (regla no negociable)

> Después de terminar cualquier tarea, antes de cerrar la respuesta al usuario, actualizar siempre — en este orden:
>
> 1. **Memoria persistente** (`/Users/mikelcarozzi/.claude/projects/-Users-mikelcarozzi-Documents-Desarrollo-IA-Limceron-lang/memory/`) — agregar/actualizar entradas tipo `user`, `feedback`, `project`, `reference`.
> 2. **`LEARNINGS.md`** — registrar errores cometidos y resoluciones (bucle de mejora automática, ver más abajo).
> 3. **`CLAUDE.md`** — si la tarea reveló una nueva regla operativa, convención del proyecto, comando frecuente o decisión arquitectónica, agregarla acá.
> 4. **`ROADMAP.md`** — actualizar siempre que se cierre o replanifique una etapa del bootstrap (Stage 0/1/2) o una feature de la lista L1-L13. Es el roadmap vivo del repo.
> 5. **`docs/adr/`** — si la tarea cambió arquitectura, contratos (`include/vdag.wit`) o decisiones técnicas, agregar un ADR nuevo o actualizar el correspondiente.
> 6. **Artefactos del repo** — `README.md`, `docs/language-reference.md`, contratos WIT (`include/vdag.wit`, `include/vdag.errors.wit`) si la tarea los tocó.

**Motivo (no olvidar):** Mikel ya perdió contexto valioso por sesiones cerradas sin persistir. Cada tarea sin actualización es deuda técnica que vuelve a aparecer la próxima sesión y obliga a re-explicar todo. La regla existe para que la próxima sesión arranque con TODO el conocimiento de la anterior.

**Cómo aplicar:**
- No depender solo del contexto de la conversación. Asumir que la sesión puede cortarse en cualquier momento.
- Si una decisión cuesta más de 5 minutos de discusión, capturarla en el archivo que corresponda **antes** de implementar.
- Al terminar una tarea, decir explícitamente al usuario qué se actualizó (1 línea: "Actualicé memoria + LEARNINGS + CLAUDE.md").

## Bucle de mejora automática (patrón Boris Cherny)

> Cada vez que cometas un error — y cada vez que el usuario te corrija — documentarlo en `LEARNINGS.md` con:
>
> 1. **Síntoma** — qué hice mal o qué falló.
> 2. **Causa raíz** — por qué pasó (no superficial: la causa real).
> 3. **Regla** — qué haré distinto la próxima vez, en forma de instrucción ejecutable.
> 4. **Trigger** — cómo reconocer la situación en el futuro para aplicar la regla.

**Reglas del bucle:**

- **Documentar todo error**, incluso si parece menor. La acumulación es la que da el valor.
- **Si una regla en `LEARNINGS.md` se aplica más de 3 veces y es estable, promoverla a `CLAUDE.md`** como convención del proyecto. `LEARNINGS.md` es el laboratorio; `CLAUDE.md` es la doctrina.
- **Antes de empezar tareas no triviales, leer `LEARNINGS.md`**. Esto previene repetir errores ya documentados.
- **Si una regla resulta equivocada con nueva evidencia, marcarla `DEPRECATED`** y registrar por qué — no borrar, el histórico tiene valor.

**Formato de entrada en `LEARNINGS.md`:** ver template en cabecera de ese archivo.

## Reglas de código

- **Idioma:** comentarios e identificadores en **inglés** — así está todo el codebase existente (`src/`, `runtime/`, `stage1/`, `test/`); no introducir comentarios en español.
- **Comentarios:** por default, ninguno. Solo cuando el "porqué" no es obvio (workaround puntual, invariante sutil, causa raíz de un bug pasado — no repetir lo que ya dice el código).
- **Sin features especulativos.** No agregar abstracciones para "futuros casos". Tres líneas similares > abstracción prematura.
- **Tipos fuertes en todos lados:** C99 explícito en compilador/runtime; en Limceron mismo, `Result<T,E>`, generics monomorphization, enums tageados exhaustivos en `match`.

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

## Current state (April 2026)

- Stage 0: 100% complete
- Stage 1: 100% complete (12 features ported, all tests passing)
- Bootstrap: Stage 0→1→2→3 verified (fixed point)
- SSA IR: x86_64 + aarch64 emitters with register allocator
- K8s: health, metrics, signal, retry, progress primitives
- Markdown: Full parity with .lceron (capability, taint, access_control, supervisor, etc.)
- Tests: 489+ total across all suites
- Exit case: BERT Patana categorizer ran against production data
