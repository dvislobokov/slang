// VS Code client for the Slang language server (slang-lsp).
//
// Launches `slang.serverPath` (default `slang-lsp` on PATH; falls back to
// `slang lsp` when slang-lsp is not installed) over stdio. The `slang.*`
// settings are sent to the server (initialization options and
// workspace/didChangeConfiguration), which reads `slang.inlayHints.*`.
//
// Code lens commands sent by the server are executed here:
// `slang.runTest` / `slang.debugTest` (file URI, test name: `slang test
// <package or file> --filter ^Name$ -v` as a task), `slang.run` (file URI:
// `slang run`) and `slang.showReferences` (URI, position, locations: the
// references peek view).

import * as fs from "fs";
import * as path from "path";
import * as vscode from "vscode";
import { Executable, LanguageClient, LanguageClientOptions, ServerOptions } from "vscode-languageclient/node";
import { registerDebugging } from "./debug";

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

/** The settings the server reads (`slang.inlayHints.*`). */
function serverSettings(): object {
  const config = vscode.workspace.getConfiguration("slang");
  return {
    inlayHints: {
      types: { enabled: config.get<boolean>("inlayHints.types.enabled", true) },
      parameterNames: { enabled: config.get<boolean>("inlayHints.parameterNames.enabled", true) },
    },
  };
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
    initializationOptions: serverSettings(),
    synchronize: {
      // Created/changed/deleted .sl files -> workspace/didChangeWatchedFiles.
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

/** The `slang` CLI (setting `slang.path`). */
function slangCli(): string {
  return vscode.workspace.getConfiguration("slang").get<string>("path", "slang").trim() || "slang";
}

/** The package directory of `file` (nearest manifest upwards), if any. */
function packageDir(file: string): string | undefined {
  let dir = path.dirname(file);
  for (;;) {
    if (["slang.toml", "slang.json", "slang.yaml", "slang.yml"].some((m) => fs.existsSync(path.join(dir, m)))) {
      return dir;
    }
    const parent = path.dirname(dir);
    if (parent === dir) {
      return undefined;
    }
    dir = parent;
  }
}

/** Runs `slang <args>` as a task named `name` in the folder of `file`. */
async function runSlangTask(file: string, name: string, args: string[]): Promise<void> {
  const folder = vscode.workspace.getWorkspaceFolder(vscode.Uri.file(file));
  const cwd = folder?.uri.fsPath ?? path.dirname(file);
  const task = new vscode.Task(
    { type: "slang", command: args[0] },
    folder ?? vscode.TaskScope.Workspace,
    name,
    "slang",
    new vscode.ProcessExecution(slangCli(), args, { cwd }),
  );
  task.presentationOptions = { reveal: vscode.TaskRevealKind.Always, clear: true };
  await vscode.tasks.executeTask(task);
}

/** Code lens `slang.runTest`: runs one `@test` function. */
async function runTest(uri: string, name: string): Promise<void> {
  const file = vscode.Uri.parse(uri).fsPath;
  await runSlangTask(file, `test ${name}`, ["test", packageDir(file) ?? file, "--filter", `^${name}$`, "-v"]);
}

/** Code lens `slang.debugTest`: `slang test` has no debug mode yet, so the test runs verbosely. */
async function debugTest(uri: string, name: string): Promise<void> {
  void vscode.window.showInformationMessage(
    `Slang: debugging a single test is not supported by \`slang test\` yet; running ${name} with -v instead.`,
  );
  await runTest(uri, name);
}

/** Code lens `slang.run`: `slang run` of the package (or the file). */
async function runMain(uri: string): Promise<void> {
  const file = vscode.Uri.parse(uri).fsPath;
  await runSlangTask(file, "run", ["run", packageDir(file) ?? file]);
}

interface LspPosition {
  line: number;
  character: number;
}

interface LspLocation {
  uri: string;
  range: { start: LspPosition; end: LspPosition };
}

/** Code lens `slang.showReferences`: the references peek view. */
async function showReferences(uri: string, position: LspPosition, locations: LspLocation[]): Promise<void> {
  const toPos = (p: LspPosition) => new vscode.Position(p.line, p.character);
  const locs = locations.map((l) => new vscode.Location(vscode.Uri.parse(l.uri), new vscode.Range(toPos(l.range.start), toPos(l.range.end))));
  await vscode.commands.executeCommand("editor.action.showReferences", vscode.Uri.parse(uri), toPos(position), locs);
}

/** `Slang: Format document`: formats the active .sl file through the server (`slang fmt` rules). */
async function formatDocument(): Promise<void> {
  const editor = vscode.window.activeTextEditor;
  if (!editor || editor.document.languageId !== "slang") {
    void vscode.window.showInformationMessage("Slang: open a .sl file to format it.");
    return;
  }
  await vscode.commands.executeCommand("editor.action.formatDocument");
}

export async function activate(context: vscode.ExtensionContext): Promise<void> {
  output = vscode.window.createOutputChannel("Slang");
  context.subscriptions.push(output);
  context.subscriptions.push(vscode.commands.registerCommand("slang.restartServer", restartClient));
  context.subscriptions.push(vscode.commands.registerCommand("slang.fmt", formatDocument));
  context.subscriptions.push(vscode.commands.registerCommand("slang.runTest", runTest));
  context.subscriptions.push(vscode.commands.registerCommand("slang.debugTest", debugTest));
  context.subscriptions.push(vscode.commands.registerCommand("slang.run", runMain));
  context.subscriptions.push(vscode.commands.registerCommand("slang.showReferences", showReferences));
  registerDebugging(context);
  context.subscriptions.push(
    vscode.workspace.onDidChangeConfiguration((e) => {
      if (e.affectsConfiguration("slang.serverPath") || e.affectsConfiguration("slang.serverArgs")) {
        void restartClient();
      } else if (e.affectsConfiguration("slang.inlayHints") && client) {
        void client.sendNotification("workspace/didChangeConfiguration", { settings: { slang: serverSettings() } });
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
