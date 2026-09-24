# Limceron — Correcciones pendientes (auditoría 2026-06-11)

> **Documento de acción para un agente.** Hallazgos verificados ejecutando la suite (`make test`: 475+120+15 tests OK) y el bootstrap (`make bootstrap`: **FALLA**) sobre el HEAD actual (`b0d07ad`). Cada ítem tiene repro, causa raíz cuando se identificó, fix propuesto y criterio de aceptación. Orden = prioridad.

---

## C1 — CRÍTICO: el bootstrap está roto en HEAD — ✅ RESUELTO (2026-09-23)

**Nota de cierre:** los dos bugs de codegen (C1.a, C1.b) estaban tapando un tercero, no documentado acá:
`system()` en `src/main.c` arma los comandos de `cc` sin comillas, así que cualquier checkout en una ruta
con espacios (como esta, `.../Desarrollo IA/...`) rompe la compilación del runtime aunque C1.a/C1.b estén
arreglados. Se agregó como C1.d. También se encontró que la causa raíz real de C1.b no era el codegen sino
`parse_pattern` (L6): el stripping de `_<name>` se estaba aplicando también a las variables de loop
(`for _wi in ...`), que en stage1 se referencian con el underscore adentro del cuerpo — la solución fue
que `for` deje de pasar por el stripping (solo los arms de `match` lo necesitan), no mangling en codegen.
`make bootstrap` y `make test` (475+120+15) quedan en verde. Se agregó `test-stage1-compiles` (C1.c) a
`make test` para que esto no vuelva a colarse en silencio.

**Repro:**
```bash
make bootstrap
# === Stage 2: 0 built, 4 skipped ===
# [FAIL] stage1-parser failed to compile
# [FAIL] stage1-typecheck failed to compile
# [FAIL] stage1-codegen failed to compile
```

Stage 0 compila pero el C generado para `stage1/parser.lceron` (y typecheck/codegen) no es C válido. Dos bugs de codegen independientes, ambos probables regresiones de los commits recientes (L6 pattern matching, L9 inference, L8 modules, L2 loops):

### C1.a — Template del supervisor emite `%%` literal en el C generado

```bash
./build/limceron-stage0 emit stage1/parser.lceron > /tmp/p.c
sed -n '245p' /tmp/p.c
# { int idx = (sup->history.head + sup->history.count) %% LCN_SUPERVISOR_MAX_RESTARTS;
#                                                      ^^ error: expected expression
```

**Causa raíz:** `src/codegen.c:9168` (y líneas vecinas del template supervisor) tienen `%%` escrito como si la cadena pasara por un `printf`-family, pero ese bloque se emite por un camino sin formateo → el `%%` llega literal al `.c`. `stage1/parser.lceron` usa `supervisor` (9 referencias), por eso lo arrastra.
**Fix:** unificar el camino de emisión — si el template se escribe con `fputs`/append, usar `%` simple; si se escribe con `fprintf`, mantener `%%`. Revisar TODO el template supervisor (≈ src/codegen.c:9078-9200) por el mismo patrón.
**Aceptación:** el C generado contiene `% LCN_SUPERVISOR_MAX_RESTARTS` y compila.

### C1.b — Variables de loop generan el identificador C `if`

```bash
grep -n "int64_t if" /tmp/p.c
# 2628: { int64_t if; for (if = 0; if < 200; if++) {
```

El fuente correspondiente es `stage1/parser.lceron:520` etc.: `for _bp in 0..200 { ... }`. El codegen está tomando un nombre equivocado para la variable del loop (termina siendo `if`, keyword de C). Afecta a los loops con variables `_`-prefijadas (`_bp`, `_m`, `_t`, `_sv`, `_ms` — todas en parser.lceron).
**Causa raíz probable:** el lowering de loops (ADR-0007, loop-carried bindings) o el manejo de wildcard/`_` en pattern matching (L6) lee el slot/token incorrecto del AST para el nombre, o el mangling de `_x` colisiona. Bisect sugerido: `git bisect` entre el último bootstrap verde y HEAD usando `make bootstrap` como test.
**Fix:** corregir la fuente del nombre + sanitizar SIEMPRE identificadores generados contra keywords de C (lista C99) con un sufijo de mangling — defensa en profundidad para que esto sea imposible por construcción.
**Aceptación:** `make bootstrap` completo en verde, incluyendo el diff de punto fijo Stage 2 == Stage 3.

### C1.c — Gap de cobertura que dejó pasar esto

La suite (475 tests) pasa con el bootstrap roto: ningún test compila `stage1/*.lceron` ni ejercita supervisor+loops juntos hasta C válido.
**Fix:** agregar al `make test` default (o a CI) un target barato que al menos haga `limceron-stage0 build stage1/parser.lceron` (compile-only). El bootstrap completo puede quedar en un target nightly, pero "stage1 compila" debe ser bloqueante.
**Aceptación:** romper codegen de nuevo hace fallar `make test`.

---

## C2 — README desincronizado con la realidad (riesgo de credibilidad)

El README es excelente marketing, pero hoy contiene claims falsificables en 5 minutos por cualquier evaluador — y la audiencia de un lenguaje de *trust* es exactamente la que va a verificar:

1. **"The bootstrap chain is verified to a fixed point... Stages 2 and 3 produce identical output"** — falso en HEAD (ver C1). Tras arreglar C1: hacer que un target CI corra la verificación y mantenga el claim honesto. Mientras tanto, suavizar a "bootstrap en progreso".
2. **"477 tests. 2,123 assertions."** — medido: 475 tests / 2.132 assertions (+ 120 IR + 15 LSP en suites separadas = 610 + 37 assertions más). Los números reales son MEJORES que los publicados. **Fix:** target `make readme-stats` que extraiga los contadores y falle si README divergió, o citar "600+ tests, 2.700+ assertions" redondeado hacia abajo.
3. **"Stage 1 ... 8,755+ lines of Limceron"** — hoy son 11.502 (`wc -l stage1/*.lceron`). Understatement, actualizar.
4. **Pipeline contradictorio:** README dice transpilación C99 → binario nativo como LA arquitectura; ADR-0001 declara `wasm32-wasi-preview2` como "primary target". Decidir la narrativa (¿dual-track? ¿wasm primario y C99 legacy?) y alinear README + ADRs + sección Documentation (que ya mezcla ejemplos `examples/language/` y `examples/wasm/`).
5. **Tabla competitiva con "No" absolutos** — varios son contestables (OpenAI Agents SDK tiene guardrails y handoffs; LangGraph tiene interrupts/checkpoints/budgets vía middleware; ADK tiene TS). El punto fuerte real es "**as a language primitive, enforced at compile time**" — reformular los encabezados con ese criterio y los "No" se vuelven defendibles.

**Aceptación:** cada claim numérico del README es reproducible con un comando del repo.

## C3 — ROADMAP.md interno stale

- Tabla Stage 1 con LOC viejas (suma ≈7.176 vs 11.502 reales).
- "Stage 2 — Full Self-Hosting — NOT STARTED" pero el Makefile ya tiene `stage2-build` + `test-bootstrap` implementados.
**Fix:** actualizar tras cerrar C1; idealmente generar las LOC con un script para que no vuelva a divergir.

## C4 — Test de seguridad imprime FAILED dentro de una suite verde

`make test` muestra `security: .lceron signature verification FAILED` (origen: `src/security.c:327`) y aun así "ALL TESTS PASSED".
**Fix:** si es un caso negativo esperado (verificar que una firma inválida se rechaza), el mensaje debe decirlo (`expected-fail OK`); si no lo es, el test no está assertando el resultado. Cualquier línea con "FAILED" en una suite verde erosiona confianza en la suite.
**Aceptación:** salida de `make test` sin la palabra FAILED salvo en fallos reales.

## C5 — `confidence = 1.0` silencioso cuando el proveedor no da logprobs — ✅ RESUELTO (2026-09-24)

**Nota de cierre:** implementado el fix #1+#2 del doc (sentinel + fail-fast). `resp->confidence` es
`-1.0` (no `1.0`) cuando no hay logprobs — documentado en `runtime/llm.h`, `runtime/lcn_runtime.h`
(`LcnLlmResult` y `LcnLlmOutput`). El `run()` auto-generado para agentes con `entropy_budget` ahora
chequea `_llm.confidence < 0.0` ANTES de `lcn_entropy_record`/`lcn_entropy_check_budget` y devuelve
error con el endpoint/modelo en el mensaje, en vez de dejar que una confianza sintética 1.0 nunca
dispare el fence. README (línea de `ask()` y tabla competitiva) matizado con la salvedad de logprobs —
fix #3. Fix #4 (self-consistency sampling como proxy) queda en roadmap, no bloqueante, tal como decía
el doc. De paso, se encontró y corrigió un bug UB no relacionado en el mismo bloque de codegen: un
`cg_line(g, "...%s...")` sin el argumento correspondiente (mismo patrón que C1.a pero al revés —
faltaba `%%` en vez de sobrar). Test nuevo: `codegen_entropy_budget_fails_fast_without_logprobs`.
`make test` (476+120+15) y `make bootstrap` en verde.



`runtime/llm.c:383`: si la respuesta no trae logprobs, `resp->confidence = 1.0`. Anthropic (API directa y Bedrock) **no expone logprobs** → contra los modelos Claude, todo `ask()` reporta confianza máxima y el `entropy_budget` no dispara jamás. Es el peor default posible para un lenguaje cuyo pitch es "el agente sabe cuándo no sabe": **ausencia de señal se reporta como certeza absoluta.**

**Fix propuesto:**
1. Sentinel explícito: `confidence = -1` (o `Option`/flag `confidence_available: false`) cuando no hay logprobs.
2. `entropy_budget` declarado + proveedor sin logprobs = **error en runtime al primer `ask()`** ("entropy_budget requiere logprobs u ONNX local; el proveedor X no los expone") — coherente con la filosofía fail-fast del lenguaje.
3. README: matizar "Entropy/confidence on every call: Yes" → "Yes (providers exposing logprobs, or local ONNX)". Documentar la matriz de proveedores.
4. (Roadmap, no bloqueante) fallback honesto para proveedores sin logprobs: self-consistency sampling (k llamadas, acuerdo como proxy de confianza), claramente etiquetado como proxy.

**Aceptación:** un agente con `entropy_budget` contra un endpoint sin logprobs falla ruidosamente con mensaje claro, nunca corre con confianza 1.0 sintética.

## C6 — Entropía calculada solo sobre el primer token

`runtime/llm.c:378-379`: la entropía usa los top-logprobs del **primer token** de la completion. Es un proxy razonable para clasificación de pocas clases (el caso radiología) pero débil/engañoso para outputs largos (razonamiento, JSON rico).
**Fix:** documentar la limitación donde se describe `result.confidence`; opcionalmente promediar sobre los primeros N tokens (configurable, default 1 para no romper compatibilidad).

## C7 — Higiene de warnings

- `runtime/onnx_model.c`: 13 warnings al compilar (p.ej. `output_tensor` declarado/posiblemente sin inicializar, línea 469).
- El ownership checker emite `use of moved value 'sb'` para `stage1/parser.lceron:192-194` (patrón `sb_append(sb,...)` repetido) — determinar si es falso positivo del checker (las funciones sb_* probablemente toman préstamo, no ownership) o código stage1 a corregir. Si es falso positivo, es un bug del checker que va a moler a cualquier usuario real.
**Fix:** llegar a cero warnings en `make` y considerar `-Werror` en CI.

## C8 — Exit codes y UX del compilador (menor)

`limceron-stage0 build` con C inválido reporta `error: C compilation failed` y exit 1 ✓ (verificado) — pero el mensaje no muestra los errores del cc subyacente; hay que re-correr `emit` + `cc` a mano para verlos.
**Fix:** capturar y mostrar stderr del compilador C (primeros N errores) en el mensaje de fallo. Acorta el loop de debugging de cualquier usuario — y del propio bootstrap.

---

## Orden de ejecución sugerido

| # | Ítem | Por qué primero |
|---|---|---|
| 1 | C1.a + C1.b (bootstrap) | El claim central del proyecto está roto; todo lo demás es cosmético al lado |
| 2 | C1.c (stage1 compile en CI) | Evita la recaída inmediata |
| 3 | C5 (confidence sentinel) | Bug de semántica que contradice la tesis del lenguaje |
| 4 | C2 + C3 (README/ROADMAP) | 30 minutos, elimina el riesgo de credibilidad |
| 5 | C4, C6, C7, C8 | Higiene |

**Verificación final:** `make test && make bootstrap` ambos en verde, README reproducible comando a comando.
