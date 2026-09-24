# L12 -- capability.network compile-time allowlist

The Limceron compiler extends the agent `capabilities:` field with a
**parameterised form** for network-shaped capabilities. Today the
bare form

```limceron
capabilities: [http.fetch]
```

declares the verb without constraining the destination -- the runtime
(wazero) is the sole arbiter of which host:port pairs the wasm module
is allowed to reach. After L12 the same field accepts an inline
allowlist:

```limceron
capabilities: [http.fetch(["api.openai.com:443", "*.example.com:443"])]
```

which the compiler:

1. Parses into the AST as `AST_CAPABILITY_ITEM` with the qualified
   verb in `name` and a chain of `AST_STRING_LIT` host specs in
   `params`.
2. Validates at typecheck time (`check_capability_allowlists` in
   `src/typecheck.c`) against the host pattern rules below.
3. Advertises in the emitted `.wit`:

   ```wit
   import http.fetch {
       hosts: ["api.openai.com:443", "*.example.com:443"]
   }
   ```
4. Records as a wasm custom section
   `vdag.capability.network.allowlist` carrying the UTF-8 JSON
   payload `[{"verb":"http.fetch","hosts":["...","..."]}]`.

The runtime (Visual-DAG's `internal/nodes/code/limceron.go` activity)
reads the custom section at instantiate time and **refuses
instantiation** if any declared host:port is not in the worker's
runtime allowlist. That gives the "verifiable at link time" property
ADR-0016 mandates for `agent.compiled@1.0` and unblocks
`policy.guard@1.0`.

## Samples

| File | Form | Compile-time fence | Custom section? |
|------|------|-------------------|----------------|
| `01_open_fetch.lceron`        | `http.fetch`                              | none (runtime only) | absent  |
| `02_restricted_fetch.lceron`  | `http.fetch(["api.openai.com:443"])`      | single host         | present |
| `03_glob.lceron`              | `http.fetch(["*.example.com:443"])`       | subdomain glob      | present |

Build:

```sh
./build/limceron-stage0 build examples/wasm/capabilities/02_restricted_fetch.lceron \
    --target wasm32-wasi-preview2 -o /tmp/restricted.wasm
```

Inspect the custom section (requires `wasm-tools`):

```sh
wasm-tools dump /tmp/restricted.wasm | grep -A1 'vdag.capability.network'
```

Inspect the WIT advertisement:

```sh
cat /tmp/restricted.wit
```

## Host pattern rules (v1)

| Rule                                          | Example accepted             | Example rejected           |
|-----------------------------------------------|------------------------------|----------------------------|
| Must be `host:port`                           | `api.openai.com:443`         | `api.openai.com`           |
| Host may begin with `*.` (subdomain glob)     | `*.example.com:443`          | `api.*.com:443`            |
| Bare `*` / `*:*` rejected (use the bare form) | --                           | `*`, `*:*`                 |
| Port in `(0, 65535]`                          | `443`, `65535`               | `0`, `99999`               |
| Port must be decimal                          | `443`                        | `0x1bb`                    |
| IP literals rejected in v1                    | `api.openai.com:443`         | `127.0.0.1:443`            |
| Empty string rejected                         | --                           | `""`                       |
| Empty list rejected                           | --                           | `http.fetch([])`           |

Future expansion (out of scope for v1): `agent.call(["worker_a",
"worker_b"])` will use the same shape with a different validator
plug-in.

## Custom section format

WAT (text):

```wat
(@custom "vdag.capability.network.allowlist" "[{\"verb\":\"http.fetch\",\"hosts\":[\"api.openai.com:443\",\"*.example.com:443\"]}]")
```

Wasm (binary, after `wat2wasm --enable-annotations`): a single
custom section whose name is `vdag.capability.network.allowlist`
and whose payload is the UTF-8 JSON above.

The format is intentionally JSON (not protobuf or custom binary) so
that any introspection tool -- `wasm-tools dump`, `wasm-objdump`,
or a one-liner Go program -- can read it without sharing schema
code with the compiler.

## Visual-DAG runtime contract

The Visual-DAG worker activity that instantiates a compiled Limceron
module (`internal/nodes/code/limceron.go`) must:

1. Open the wasm binary and locate the custom section whose name is
   `vdag.capability.network.allowlist`.
2. If absent: the bare form was used; apply only the runtime allowlist
   (current behaviour).
3. If present: parse the JSON payload, and for every entry whose
   `verb` is `http.fetch`, check that **every** declared `host:port`
   is a member of the worker's configured allowlist. If any entry is
   not allowed by the worker, refuse instantiation with a
   non-retryable `PolicyDenied` error.
4. If accepted: the per-module allowlist becomes the **intersection**
   of the declared list and the worker's runtime allowlist, which is
   then used to gate `http.fetch` host imports.

This is the L12 -> F36c follow-up tracked in the commit message.
The bare form remains a valid expression of "I do not pin the
allowlist at compile time" -- it is not an error, just a missing
fence.
