// Limceron VSCode extension entrypoint.
//
// Activates on `onLanguage:limceron`, spawns the `limceron-lsp` binary
// (path resolved from settings, falling back to $PATH), and bridges it
// to VSCode via the `vscode-languageclient` library.
//
// The extension is intentionally thin -- all language intelligence
// lives on the server side (compiled from src/lsp.c).

import * as path from "path";
import {
    ExtensionContext,
    workspace,
    window,
    OutputChannel,
} from "vscode";

import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind,
} from "vscode-languageclient/node";

let client: LanguageClient | undefined;

export function activate(context: ExtensionContext): void {
    const cfg = workspace.getConfiguration("limceron");
    const serverPath: string = cfg.get<string>("serverPath", "limceron-lsp");

    const traceLevel: string = cfg.get<string>("trace.server", "off");

    const channel: OutputChannel = window.createOutputChannel("Limceron LSP");

    const serverOptions: ServerOptions = {
        run:   { command: serverPath, args: ["lsp"], transport: TransportKind.stdio },
        debug: { command: serverPath, args: ["lsp"], transport: TransportKind.stdio },
    };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [
            { scheme: "file", language: "limceron" },
        ],
        synchronize: {
            fileEvents: workspace.createFileSystemWatcher("**/*.lceron"),
        },
        outputChannel: channel,
        traceOutputChannel: channel,
    };

    client = new LanguageClient(
        "limceronLanguageServer",
        "Limceron Language Server",
        serverOptions,
        clientOptions,
    );

    // Honour the user trace setting. The vscode-languageclient API
    // expects one of `off | messages | verbose`.
    client.start().catch((err) => {
        window.showErrorMessage(
            `limceron-lsp failed to start (${serverPath}): ${err}. Set ` +
            `"limceron.serverPath" in settings or add the binary to $PATH.`,
        );
    });

    if (traceLevel !== "off") {
        channel.appendLine(`limceron-lsp tracing: ${traceLevel}`);
    }

    // For completeness in case future entry-point logic needs to dispose.
    context.subscriptions.push({
        dispose: () => {
            client?.stop();
        },
    });

    // Silence unused-import lint on `path` (we don't currently resolve
    // a workspace-relative server path -- left for future use).
    void path;
}

export function deactivate(): Thenable<void> | undefined {
    return client?.stop();
}
