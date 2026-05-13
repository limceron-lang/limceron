# ADR 0005 — capability.network compile-time host:port allowlist (L12)

## Status
Accepted (2026-05-13). Implemented in commit `23dce20`.

## Context

Pre-L12 the `capabilities:` field accepted only bare identifiers:

```limceron
capabilities: [http.fetch, llm.classify]
```

The bare `http.fetch` means "this agent may issue HTTP requests"; the
specific hosts it may reach are enforced by the Visual-DAG runtime
allowlist via `internal/nodes/http/AllowlistedClient`. That works for
trusted operators; it does not work for the platform claim "this
workflow cannot exfiltrate to evil.example.com".

ADR-0016 specifies that the platform should be able to certify "this
workflow cannot reach the network" or "this workflow cannot reach
hosts outside this list" by inspecting the compiled `.wasm` + its
WIT, without auditing the runtime config. That requires the allowlist
to be a compile-time property of the artifact, not a runtime policy
side-channel.

## Decision

**Extend the `capabilities:` field with a parameterised form
`http.fetch(["host:port", ...])` that pins a compile-time allowlist.
The compiler advertises the list in the WIT and records it as a wasm
custom section. The runtime (Visual-DAG `code.limceron` Activity)
reads the custom section at instantiate time and refuses the module
when its allowlist contradicts.**

### Syntax

```limceron
agent FetchOpenAI {
    capabilities: [
        http.fetch(["api.openai.com:443", "*.example.com:443"]),
        llm.classify
    ]
    budget: { max_tokens: 100, max_cost: 0.01 }
    // ...
}
```

Rules (validated by typecheck pass 2c):

- `host:port` required for every entry.
- Glob form `*.<rest>` permitted only as a LEADING wildcard.
- Port must be decimal, in (0, 65535].
- IPv4 dotted-quad rejected universally.
- `*` / `*:*` / `<empty>` rejected.
- Empty list rejected.

The bare form `capabilities: [http.fetch]` continues to parse
unchanged — back-compat preserved.

### WIT emission

```wit
package vdag:agent

interface fetch-openai {
    import http;
    import http.fetch { hosts: ["api.openai.com:443", "*.example.com:443"] }
    import llm;
}
```

The parameterised entry emits a per-host annotation block alongside
the existing prefix `import http;`. Bare `http.fetch` emits only the
prefix.

### Wasm custom section

```
section name: vdag.capability.network.allowlist
section body: <UTF-8 JSON>
```

JSON shape:

```json
[
  { "verb": "http.fetch", "hosts": ["api.openai.com:443", "*.example.com:443"] }
]
```

Omitted entirely on the bare form (asserted by IR test).

Wat2wasm gains `--enable-annotations` so the `(@custom ...)` block
propagates into the binary.

### Visual-DAG runtime contract (F36c)

At instantiate time, `internal/nodes/code/limceron.go`:

1. Opens the wasm binary and locates the `vdag.capability.network.allowlist`
   custom section.
2. If absent: bare form was used; apply only the runtime allowlist.
3. If present: parse the UTF-8 JSON payload; for every entry whose
   `verb == "http.fetch"`, check that every declared `host:port` is
   in the worker's configured allowlist.
4. If any declared host:port is not allowed, refuse instantiation
   with a non-retryable `PolicyDenied` error.
5. If accepted: the effective per-module allowlist is the
   intersection of declared list and worker allowlist.

## Trade-offs

**Eases**

- Compliance certifies "this agent cannot exfiltrate" by inspecting
  the custom section. The artifact carries its own contract.
- Multi-host agents (a marketplace agent that needs both OpenAI and
  a partner) declare exactly what they need; reviewer can read it
  in the WIT.
- Bare form keeps working for trusted internal agents that delegate
  to the runtime allowlist.

**Hardens**

- IPs rejected even when an operator would want one. We
  deliberately keep the schema host-only; a future `ip_addrs: [...]`
  field can land separately rather than relax the host validator.
- Glob form supports leading `*.<rest>` only. No middle / trailing
  wildcards. Operationally this is sufficient — `*.example.com:443`
  covers most subdomain-routing scenarios.
- The custom section is a Limceron-defined contract, not standard;
  third-party wasm tooling will not inspect it. Mitigated because the
  contract is enforced at the trust boundary (Visual-DAG worker).

## Consequences

- `wat2wasm --enable-annotations` becomes a required flag of the
  build pipeline.
- AST_CAPABILITY_ITEM remains a parameterised AST node (no new
  kind); typecheck owns shape validation; emit owns serialisation.
- Future verbs (`agent.call(["worker_a", ...])`, `data.read([...])`)
  use the same syntactic shape but only `http.fetch` is enforced in
  v1. The typecheck rule is keyed on the verb name; plugging in the
  next verb is a tiny diff.

## Alternatives considered

**A. Runtime allowlist only.** Status quo pre-L12. Loses the
artifact-level contract.

**B. WIT-only annotation (no custom section).** WIT-aware tools see
the contract; wazero does not. Custom section is the channel wazero
actually reads at instantiate.

**C. New AST kind for capability items.** Considered. Rejected
because the existing `AST_CAPABILITY_ITEM` already carries a `name`
+ a `params` chain; the leading bare form is identical syntactically
to the v1 identifier.

**D. SPKI / X.509 host certificate pinning.** Considered. Wrong
abstraction — DNS hostnames are what authors think about; cert pins
churn faster than the agent code.

## Open questions

- **`agent.call(["worker_a", ...])`** support. ADR-0017 contemplates
  this; same allowlist shape, different verb. Plugging in is a one-line
  typecheck addition.
- **Cross-verb intersection.** What if a marketplace publisher
  declares `http.fetch(["a.com:443"])` and the runtime allowlist
  is `["b.com:443"]`? Today: refuse. Reasonable; documented.
- **WIT-level forward compatibility.** Future stage1 may want a
  richer capability grammar (per-verb param schema); the v1 shape
  is forward-compatible because `name + params` is the AST general
  case.

## Implementation order

1. Parser: array elements may carry trailing param list.
2. Typecheck pass 2c: host:port shape validation.
3. WIT emit: `import http.fetch { hosts: [...] }` block.
4. Wasm emit: custom section JSON payload + `--enable-annotations`.
5. Four IR tests (parser shape, typecheck rejection, WIT emission,
   custom section presence/absence).
6. Three examples (open / restricted / glob).

## Cross-references

- [language reference: capabilities](../language-reference.md#capabilities)
- [ADR-0001](0001-wasm-target.md) — wasm32-wasip2 target.
- Visual-DAG: [ADR-0016](../../../../Visual-DAG/docs/adr/0016-limceron-execution-substrate.md) — substrate strategy.
- Examples: `examples/wasm/capabilities/`.
