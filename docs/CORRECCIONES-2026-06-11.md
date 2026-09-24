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

## C2 — README desincronizado con la realidad (riesgo de credibilidad) — ✅ RESUELTO (2026-09-24)

**Nota de cierre:** los 5 puntos atacados. (1) Reescrito el claim de fixed-point para decir exactamente
lo que `make bootstrap` verifica hoy (Stage1==Stage2 en un lexer de prueba + self-hosting lexeando el
propio Stage 1) — sin mencionar un "Stage 3" que no existe en el Makefile. (2) "477 tests / 2.123
assertions" → "600+ tests / 2.700+ assertions" (real: 476+120+15=611 tests, 2139+609+37=2785
assertions, redondeado hacia abajo como sugería el doc — no se armó `make readme-stats`, queda en
roadmap). (3) LOC de Stage 1: 8.755 → 11.500+. (4) Narrativa dual-track: la sección "Native Compilation"
ahora aclara que es el target primario para single-tenant/edge y linkea el ADR-0001 para el caso SaaS
multi-tenant; la sección Documentation separa `examples/language/` de `examples/wasm/` en vez de listar
solo wasm. (5) Tabla competitiva: agregada una oración que fija el criterio de comparación ("language
primitive enforced at compile time") en vez de tocar cada celda — la mayoría de las filas ya estaban
bien acotadas (p.ej. "Handoffs only" para OpenAI SDK), el problema era la falta de ese marco explícito.

## C3 — ROADMAP.md interno stale — ✅ RESUELTO (2026-09-24, alcance acotado)

**Nota de cierre:** arreglados los dos puntos que nombraba el doc — tabla de LOC de Stage 1 (7.176 →
11.502, por archivo) y el estado "Stage 2 — NOT STARTED". Sobre esto último se encontró algo más grave
que una cifra vieja: el ROADMAP usa "Stage 2" para dos cosas distintas — el Makefile (`stage2-build`/
`test-bootstrap`, ya implementado y ahora verde) construye "Stage 1 compilándose a sí mismo una vez",
mientras que la sección "Stage 2" del ROADMAP se refiere a un fixed-point real de doble compilación
(`stage2(source)==stage2(stage2(source))`, un Stage 3 que no existe) MÁS ownership/traits/comptime — eso
sigue en 0%, genuinamente no arrancado. Se documentó la colisión de nombres explícitamente para que no
se vuelva a confundir "bootstrap pasa" con "Stage 2 del roadmap está listo". El resto del archivo (LOC
de Stage 0, conteos de tests viejos, % de fases) sigue stale — no verificado en esta pasada, fuera del
alcance que pedía este ítem; si se quiere ese archivo 100% honesto hace falta una pasada dedicada.


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

## C4 — Test de seguridad imprime FAILED dentro de una suite verde — ✅ RESUELTO (2026-09-24)

**Nota de cierre:** era caso negativo esperado (`security_lceron_wrong_key`/`tamper_detection`/
`bad_magic` deliberadamente pasan una key/dato/magic incorrectos y assertan `ASSERT_FALSE`), tal como
preveía el doc. El fix fue en el test, no en `security.c`: `security_verify_lceron()` sigue imprimiendo
su diagnóstico completo a stderr cuando falla de verdad en runtime (correcto — es una alerta de
seguridad real), pero los 3 tests que la ejercitan a propósito ahora silencian stderr alrededor de la
llamada (`dup`/`freopen("/dev/null")`/`dup2` para restaurar) e imprimen su propia línea
`[expected-fail OK] ...` explicando qué se esperaba que fallara y por qué. `make test` ya no tiene
ninguna línea con "FAILED" en una corrida verde.



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

## C6 — Entropía calculada solo sobre el primer token — ✅ RESUELTO (2026-09-24, doc; N-token queda roadmap)

`runtime/llm.c:378-379`: la entropía usa los top-logprobs del **primer token** de la completion. Es un proxy razonable para clasificación de pocas clases (el caso radiología) pero débil/engañoso para outputs largos (razonamiento, JSON rico).
**Fix:** documentar la limitación donde se describe `result.confidence`; opcionalmente promediar sobre los primeros N tokens (configurable, default 1 para no romper compatibilidad).

**Nota de cierre:** documentada la limitación en los 3 lugares donde importa — el comentario junto al
cálculo en `runtime/llm.c`, el campo `entropy` en `runtime/llm.h`, y el párrafo de README que describe
`result.confidence` al usuario (explica por qué un `{` inicial de JSON puede leer como "confiado" aunque
el contenido sea incierto). El promedio sobre N tokens configurable queda sin implementar — es explícitamente
opcional en este ítem y no bloqueante; requeriría enhebrar un parámetro nuevo por request → codegen →
runtime, que es más alcance del que este ítem pedía.

## C7 — Higiene de warnings — ⚠️ PARCIAL (2026-09-24) — ver nota, encontró algo más grande

- `runtime/onnx_model.c`: 13 warnings al compilar (p.ej. `output_tensor` declarado/posiblemente sin inicializar, línea 469).
- El ownership checker emite `use of moved value 'sb'` para `stage1/parser.lceron:192-194` (patrón `sb_append(sb,...)` repetido) — determinar si es falso positivo del checker (las funciones sb_* probablemente toman préstamo, no ownership) o código stage1 a corregir. Si es falso positivo, es un bug del checker que va a moler a cualquier usuario real.
**Fix:** llegar a cero warnings en `make` y considerar `-Werror` en CI.

**Nota de cierre:**
- `runtime/onnx_model.c`: 13 → 0 warnings. Los 9 `-Wunused-result` (ORT API marca todo con
  `warn_unused_result`) se resolvieron con un helper `ort_ignore_status()` que además libera el
  `OrtStatus*` si viene no-nulo — antes se descartaba sin liberar, un leak real, no solo un warning.
  Los otros 4 (`output_tensor` "sometimes uninitialized") eran un bug real, no ruido del compilador:
  `OrtValue *output_tensor = NULL;` estaba declarado en la línea 469, pero 3 rutas de error anteriores
  hacían `goto cleanup` ANTES de esa declaración — el cleanup podía llamar `ReleaseValue()` sobre un
  puntero de stack sin inicializar. Se movió la declaración al principio de la función.
- **`sb_append`: confirmado falso positivo**, y arreglado en el checker (`own_check_call` en
  `typecheck.c`), no en stage1. `lcn_sb_append(void *handle, ...)` muta a través del handle y nunca lo
  libera (a diferencia de `lcn_sb_to_string`, que sí hace `free(sb)`) — es exactamente un builder
  reutilizable, no un recurso lineal. El checker trataba CUALQUIER identificador pasado por valor a
  CUALQUIER call como "moved" sin mirar la función ni el tipo. Fix acotado: el primer argumento de
  `sb_append` específicamente ahora se trata como uso, no como move (se sigue detectando el use-after-
  move real si se llama `sb_to_string` dos veces sobre el mismo builder — test de regresión agregado
  para ambos casos).
- **Hallazgo nuevo, más grande, fuera de alcance de este ítem:** arreglar `sb_append` bajó los
  warnings de ownership de `stage1/parser.lceron` de "incluye sb" a **100 warnings restantes**, la
  mayoría (`cur`, `v`, `p2`, `p3`, `p4`, `ty`...) del mismo patrón general: `own_check_call` no
  distingue tipos Copy (int, bool) de recursos que realmente hay que mover, así que reasignar un
  escalar tras pasarlo por valor (`cur = tok_next(toks, cur)`, común en un parser recursivo-descendente
  escrito a mano) dispara el mismo falso positivo en escala mucho mayor. Arreglarlo de raíz necesita
  que el checker sea consciente de tipos (Copy vs. owned) — trabajo de diseño propio, no cabe en
  "higiene de warnings". Por eso **`-Werror` en CI queda explícitamente NO recomendado todavía**: con
  100 warnings de ownership más los `-Wparentheses-equality` cosméticos del C generado, forzar
  `-Werror` hoy rompería el build sin arreglar nada real.

## C8 — Exit codes y UX del compilador (menor) — ✅ RESUELTO (2026-09-24, causa real era el Makefile)

`limceron-stage0 build` con C inválido reporta `error: C compilation failed` y exit 1 ✓ (verificado) — pero el mensaje no muestra los errores del cc subyacente; hay que re-correr `emit` + `cc` a mano para verlos.
**Fix:** capturar y mostrar stderr del compilador C (primeros N errores) en el mensaje de fallo. Acorta el loop de debugging de cualquier usuario — y del propio bootstrap.

**Nota de cierre:** no reproducido en HEAD actual. El comando de compilación en `src/main.c` (compile+link
y cada uno de los 24 `.o` de runtime) usa `system(cmd)` con `2>&1` — `system()` no captura stdout/stderr,
los hereda del proceso padre, así que el error real de `cc`/`ld` YA se imprime en vivo, antes de nuestro
`error: C compilation failed`. Repro con link roto:

```
$ limceron-stage0 build badlink.lceron -o out
...
ld: library 'ThisLibraryDoesNotExist12345' not found
clang: error: linker command failed with exit code 1 (use -v to see invocation)
error: C compilation failed
  command: cc -std=c99 -O2 -Wall -I... ...
```

El error de `ld` aparece solo, sin re-correr nada a mano. Mismo comportamiento verificado durante C1 con
errores reales de sintaxis C (el bug de paths con espacio de C1.d mostraba el `clang: error: no such
file...` completo). Posible explicación: el auditor original testeó a través de `make bootstrap`, cuyo
target `stage1-build` SÍ pipeaba por `| tail -1` — eso trunca cualquier error a la última línea, pero es
el Makefile, no `limceron-stage0`. Se corrigió igual, ya que es la misma clase de problema aplicada al
lugar donde de verdad ocurre: ahora cada paso de `stage1-build` corre a un log temporal y solo lo
trunca a la última línea si salió bien — si falla, vuelca el log completo antes de propagar el error.
Verificado con una falla real de parseo inyectada a propósito: antes de este cambio solo se veía
`error: expected expression` (última línea); ahora se ven los 14 errores completos con snippets de
fuente. No se tocó `src/main.c` (el `system()` de compile+link ya no necesitaba nada).

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

---

## Estado final (2026-09-24)

Los 8 ítems (C1-C8) fueron atacados en el orden sugerido arriba. `make test` (478+120+15 = 613 tests,
2141+609+37 = 2787 assertions) y `make bootstrap` en verde. Pendiente real para quien retome esto:

- **El hallazgo más grande de esta pasada** no estaba en la lista original: `own_check_call` en
  `typecheck.c` trata cualquier identificador pasado por valor a cualquier función como "moved", sin
  distinguir tipos Copy (int, bool) de recursos que de verdad hay que mover. Se arregló el caso puntual
  de `sb_append` (C7), pero quedan ~100 warnings de ownership en `stage1/parser.lceron` solo, la mayoría
  del mismo patrón (`cur = tok_next(toks, cur)`). Arreglarlo de raíz es un ítem propio, no cabe en
  "higiene de warnings" — necesita que el checker sea consciente de tipos.
- El resto de `ROADMAP.md` (LOC de Stage 0, conteos de tests viejos en varias tablas, % de fases) sigue
  stale más allá de lo que tocó C3 — ver nota de cierre de C3.
- `-Werror` en CI sigue sin recomendarse hasta que se resuelva el punto de arriba.
