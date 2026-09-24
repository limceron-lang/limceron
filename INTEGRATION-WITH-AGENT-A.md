# Integration: WIT emitter -> WASM build flow

The WIT (WebAssembly Interface Types) generator lives in
`src/wit_emit.{c,h}` and exposes one entry point:

```c
int lcn_emit_wit(AstNode *program, const char *wit_path);
```

It produces a `.wit` document describing every `agent` declaration found in
the parsed program (capability prefixes become `interface` blocks, agent
fns become `world` exports).

## What the integrator needs to do

`src/ir_emit_wasm.c` (owned by agent A) is the natural call site: it
already receives `program`, the resolved output path, the arena, and the
target.  After a successful `.wasm` emit we want to drop a sibling `.wit`
next to it.

### One-line `#include`

At the top of `src/ir_emit_wasm.c`, alongside the existing `#include`
directives, add:

```c
#include "wit_emit.h"
```

### Call site

Inside `lcn_emit_wasm`, **after** `wat2wasm` has produced the final
`.wasm` (i.e. just before the function returns 0), add:

```c
/* Sibling .wit emit.  Replace the .wasm extension; if there is none,
 * append .wit so we always produce an artifact. */
{
    char wit_path[1024];
    size_t n = strlen(output);
    const char *dot = strrchr(output, '.');
    if (dot && strcmp(dot, ".wasm") == 0) {
        size_t prefix = (size_t)(dot - output);
        if (prefix + 5 < sizeof(wit_path)) {
            memcpy(wit_path, output, prefix);
            memcpy(wit_path + prefix, ".wit", 5);
            (void)lcn_emit_wit(program, wit_path);
        }
    } else if (n + 5 < sizeof(wit_path)) {
        memcpy(wit_path, output, n);
        memcpy(wit_path + n, ".wit", 5);
        (void)lcn_emit_wit(program, wit_path);
    }
}
```

That's the entire integration.  The cast to `void` is intentional: a
missing-agents file shouldn't fail the wasm build (the WIT is auxiliary
metadata).  If you'd rather make it strict, propagate the return code:

```c
int wrc = lcn_emit_wit(program, wit_path);
if (wrc != 0) return wrc;
```

### Optional: gate behind `LCN_EMIT_WIT=1`

If you want `.wit` emission to be opt-in, wrap the block:

```c
const char *want = getenv("LCN_EMIT_WIT");
if (want && want[0] && strcmp(want, "0") != 0) {
    /* ...emit block above... */
}
```

The current expectation per the spec is to always emit, so leaving the
unconditional version above is recommended.

## Build system note

`src/wit_emit.c` is already wired into `S0_SRCS` in the `Makefile`; no
further build-system changes are required.  The object file is
`build/stage0/wit_emit.o` and links into both `limceron-stage0` and the
test binaries (it's part of `S0_LIB_OBJS`).

## Verification

After integration, build a wasm target file and confirm the sibling .wit
appears alongside the .wasm.  Validate it with `wasm-tools` if available:

```sh
wasm-tools component wit path/to/agent.wit
```

A standalone smoke test driver lives in `/tmp/test_wit_emit.c` during
development; the WIT generator is otherwise exercised only through the
wasm build path post-integration.
