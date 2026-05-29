# Limceron LSP (L10)

Status: **stable for stage0**. Ships with `limceron-stage0` (as the
`lsp` subcommand) and is also available as a standalone binary at
`build/limceron-lsp` (via `make lsp`).

## Capabilities matrix

| LSP capability | Status | Notes |
|---|---|---|
| `initialize` / `initialized` | done | Returns `serverCapabilities` describing the rest of this table. |
| `textDocument/didOpen` | done | Full text held in-memory; parses + typechecks immediately. |
| `textDocument/didChange` | done | Full sync (`textDocumentSync.change = 1`). Re-parses on every change. |
| `textDocument/didSave` | done | Re-runs typecheck and republishes diagnostics. |
| `textDocument/didClose` | done | Drops the per-document arena. |
| `textDocument/publishDiagnostics` | done | One `Diagnostic` per `LcnError`. Carries `range`, `severity`, `code = "LCN-NNN"`, `source = "limceron"`. |
| `textDocument/hover` | done | Inferred type + WIT signature for fns. Unannotated `let` falls back to a stub when the L9 inference pass has not landed. |
| `textDocument/definition` | done | Within-file go-to-definition. Cross-file lookup waits on L8 module resolution. |
| `textDocument/completion` | done | Keywords, builtins, scope identifiers, host modules (`llm.`, `json.`, ...), enum variants after `::`. Trigger characters: `.` and `:`. |
| `textDocument/rename` | skipped | Out of scope for stage0. |
| Code actions / quick-fix | skipped | Out of scope for stage0. |
| Semantic tokens | skipped | Use the bundled `tmLanguage.json` for syntax highlighting. |
| Inlay hints | skipped | Tracked for L10b. |

## Source-map + DWARF chain

The wasm backend (`src/ir_emit_wasm.c`) emits two custom sections on
every `wasm32-wasi-preview2` build so a wazero trap can be translated
back to a Limceron source position:

- `vdag.sourcemap` -- JSON array of `{name, line, col}` tuples (one
  per function). The runtime reads this section at instantiate time.
- `.debug_info` + `.debug_line` -- minimal DWARF-shaped payloads
  embedding the source filename and a function-level line table.
  Sufficient for `wasmtime --debug` to surface a frame name; full
  scope/inline information is L10b.

Stage0 only carries function-level granularity. Intra-function byte
offset to source line mapping requires threading `SourceLoc` through
the IR layer and is tracked for L10b.

## Editor setup

### VSCode

The bundled extension lives under `editor/vscode/` (see its README).
Quickest path:

```bash
make lsp
cp build/limceron-lsp /usr/local/bin/limceron-lsp
cd editor/vscode && npm install && npm run compile
# Launch the "Run Extension" debug target.
```

### Neovim (nvim-lspconfig)

```lua
local lspconfig = require("lspconfig")
local configs   = require("lspconfig.configs")

if not configs.limceron then
  configs.limceron = {
    default_config = {
      cmd = { "limceron-lsp", "lsp" },
      filetypes = { "limceron" },
      root_dir = lspconfig.util.find_git_ancestor,
      settings = {},
    },
  }
end

lspconfig.limceron.setup({})
vim.filetype.add({ extension = { lceron = "limceron" } })
```

### Emacs (eglot)

```elisp
(define-derived-mode limceron-mode prog-mode "Limceron")
(add-to-list 'auto-mode-alist '("\\.lceron\\'" . limceron-mode))
(with-eval-after-load 'eglot
  (add-to-list 'eglot-server-programs
               '(limceron-mode . ("limceron-lsp" "lsp"))))
```

## Wire format

LSP messages are JSON-RPC 2.0 over stdio with the standard
Content-Length header framing:

```
Content-Length: 58\r\n
\r\n
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}
```

The server logs to stderr (initialization banner, shutdown notice).
Editors typically route stderr into a dedicated channel; in VSCode
it lands in **Output > Limceron LSP**.

## Diagnostic codes

Stage0 doesn't yet carry structured error codes, so each diagnostic's
`code` field is `"LCN-NNN"` where `NNN` is the reporter slot index
(1-based, three-digit zero-padded). Editors render these as a
clickable code lens; future stage1 work will introduce stable
per-error sentinels.

## Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| Editor never connects | `limceron-lsp` not on PATH | Set `limceron.serverPath` to absolute path, or `cp build/limceron-lsp /usr/local/bin`. |
| Empty diagnostics | Source has zero issues | Editors render the empty array silently. |
| Hover shows "type info pending L9 close" | L9 inference results not yet exposed on AstNode | Expected -- updates once L9 lands. |
| Completion only shows keywords | Cursor not after a trigger character | Type `.` (for host members) or `::` (for enum variants). |
| Stderr complains about EOF | Editor closed the channel | Normal during reload; not an error. |

## Testing

The LSP test suite lives at `test/test_lsp.c` and is run via:

```bash
make test-lsp
```

It exercises:

1. JSON-RPC framing helpers (`lsp_json_get_string`, `lsp_json_get_int`).
2. Diagnostic transformation -- known-bad source -> expected payload shape.
3. Hover -- keyword / user-fn / unannotated let.
4. Completion -- in-scope keywords, host-module dot, enum variants
   after `::`, user-declared identifiers in scope.

## Related

- [docs/language-reference.md](language-reference.md) -- §Tooling.
- [editor/vscode/README.md](../editor/vscode/README.md) -- VSCode setup.
- `src/lsp.c` -- the LSP implementation.
- `src/ir_emit_wasm.c` -- the source-map + DWARF emission.
