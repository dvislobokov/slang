// VS Code client for the Slang language server (slang-lsp).
//
// Launches `slang.serverPath` (default `slang-lsp` on PATH; falls back to
// `slang lsp` when slang-lsp is not installed) over stdio.

import * as fs from "fs";
import * as path from "path";
import * as vscode from "vscode";
import { Executable, LanguageClient, LanguageClientOptions, ServerOptions } from "vscode-languageclient/node";

let client: LanguageClient | undefined;
let output: vscode.OutputChannel | undefined;

/** Finds `name` on PATH (with Windows extensions); undefined if missing. */
function which(name: string): string | undefined {
  if (name.includes("/") || name.includes("\\")) {
    return fs.existsSync(name) ? name : undefined;
  }
  const exts = process.platform === "win32" ? (process.env.PATHEXT ?? ".EXE;.CMD;.BAT").split(";") : [""];
  for (const dir of (process.env.PATH ?? "").split(path.delimiter)) {
    for (const ext of exts) {
      const candidate = path.join(dir, name + ext);
      try {
        if (fs.statSync(candidate).isFile()) {
          return candidate;
        }
      } catch {
        // not here
      }
    }
  }
  return undefined;
}

/** Resolves the server command from the settings. */
function serverExecutable(): Executable {
  const config = vscode.workspace.getConfiguration("slang");
  const configured = config.get<string>("serverPath", "slang-lsp").trim() || "slang-lsp";
  const extraArgs = config.get<string[]>("serverArgs", []);
  const base = path.basename(configured).replace(/\.exe$/i, "");
  // A path to the `slang` CLI: run its `lsp` subcommand.
  if (base === "slang") {
    return { command: configured, args: ["lsp", ...extraArgs] };
  }
  if (configured === "slang-lsp" && !which("slang-lsp") && which("slang")) {
    output?.appendLine("slang-lsp not found on PATH; using `slang lsp`");
    return { command: "slang", args: ["lsp", ...extraArgs] };
  }
  return { command: configured, args: extraArgs };
}

function createClient(): LanguageClient {
  const exe = serverExecutable();
  output?.appendLine(`starting ${exe.command} ${(exe.args ?? []).join(" ")}`);
  const serverOptions: ServerOptions = { run: exe, debug: exe };
  const clientOptions: LanguageClientOptions = {
    documentSelector: [
      { scheme: "file", language: "slang" },
      { scheme: "untitled", language: "slang" },
    ],
    outputChannel: output,
    synchronize: {
      fileEvents: vscode.workspace.createFileSystemWatcher("**/*.sl"),
    },
  };
  return new LanguageClient("slang", "Slang Language Server", serverOptions, clientOptions);
}

async function startClient(): Promise<void> {
  client = createClient();
  try {
    await client.start();
  } catch (e) {
    const msg = e instanceof Error ? e.message : String(e);
    void vscode.window.showErrorMessage(
      `Slang: could not start the language server (${msg}). Install slang-lsp or set "slang.serverPath".`,
    );
  }
}

async function restartClient(): Promise<void> {
  if (client) {
    const old = client;
    client = undefined;
    try {
      await old.stop();
    } catch {
      // already stopped
    }
  }
  await startClient();
}

export async function activate(context: vscode.ExtensionContext): Promise<void> {
  output = vscode.window.createOutputChannel("Slang");
  context.subscriptions.push(output);
  context.subscriptions.push(vscode.commands.registerCommand("slang.restartServer", restartClient));
  context.subscriptions.push(
    vscode.workspace.onDidChangeConfiguration((e) => {
      if (e.affectsConfiguration("slang.serverPath") || e.affectsConfiguration("slang.serverArgs")) {
        void restartClient();
      }
    }),
  );
  await startClient();
}

export async function deactivate(): Promise<void> {
  if (client) {
    await client.stop();
    client = undefined;
  }
}
