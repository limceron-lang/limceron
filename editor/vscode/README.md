# Limceron VSCode extension

Language client + syntax highlighting for the Limceron language.

This is the scaffold extension that ships with the Limceron compiler
source tree (`editor/vscode/`). It is **not** published to the VSCode
marketplace.

## What it provides

- Syntax highlighting (via `syntaxes/limceron.tmLanguage.json`):
  keywords, agent keywords, host calls (`llm.classify`, `json.parse`,
  ...), enum variants (`Result::Ok`, `host-error::BudgetExceeded`),
  comments, strings with `${...}` interpolation, and numbers.
- Language client wired to `limceron-lsp` over stdio, surfacing:
  - Diagnostics on save (via the existing typecheck pass).
  - Hover with inferred types + WIT signatures for fns.
  - Go-to-definition for identifiers declared in the current file.
  - Completions: keywords, builtins, scope identifiers,
    host modules (`llm.`, `json.`, `http.`, `math.`, `string.`, ...),
    enum variants after `::`.

## Install (from source)

1. Build the LSP binary:

   ```bash
   cd /path/to/limceron/lang
   make lsp
   # produces ./build/limceron-lsp
   ```

2. Make `limceron-lsp` discoverable:

   ```bash
   cp build/limceron-lsp /usr/local/bin/limceron-lsp
   ```

   or set `limceron.serverPath` in VSCode settings to the absolute
   path of `build/limceron-lsp`.

3. From this directory:

   ```bash
   cd editor/vscode
   npm install
   npm run compile
   ```

4. Launch the **Run Extension** target from VSCode's debug pane, or
   package via `vsce package` and `code --install-extension *.vsix`.

## Configuration

| Setting | Default | Purpose |
|---|---|---|
| `limceron.serverPath` | `limceron-lsp` | Path to the language server binary. Set to absolute path when not on `$PATH`. |
| `limceron.trace.server` | `off` | One of `off`, `messages`, `verbose`. Mirrors `vscode-languageclient` trace settings. |

## Other editors

- **Neovim** -- use any `vim-lsp` / `nvim-lspconfig` client and point
  it at `limceron-lsp` with `cmd = { "limceron-lsp", "lsp" }` and
  `filetypes = { "limceron" }`. See [docs/lsp.md](../../docs/lsp.md)
  for a full example.
- **Emacs** -- `eglot` accepts the same command. Set the
  `eglot-server-programs` entry to `(limceron-mode . ("limceron-lsp" "lsp"))`.

## Troubleshooting

- *"limceron-lsp failed to start"* -- verify the binary is executable
  and on `$PATH`, or set `limceron.serverPath` explicitly.
- *"No completions / no diagnostics"* -- check the **Output > Limceron
  LSP** channel. Enable `limceron.trace.server: verbose` for the full
  JSON-RPC transcript.
- *"Editor highlights but no language features"* -- the extension only
  activates on `.lceron` files; check the file extension and the
  bottom-right language indicator in VSCode.
