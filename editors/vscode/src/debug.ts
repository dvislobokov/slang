// Debugging Zell programs (docs/DEBUGGER.md).
//
// A debug configuration of `type: "zell"` is resolved here and served by
// one of two adapters (`"adapter"`):
//
// * `"agent"` (default, stage 2): the in-process debug agent (`std.debug`,
//   linked into every debug build). `launch` starts the program with
//   `ZELL_DEBUG=127.0.0.1:0,wait`, reads the announced port from its
//   stderr (`zell-debug: listening on HOST:PORT`) and connects VS Code to
//   it with a `DebugAdapterServer`; the program's output goes to the debug
//   console. `attach` connects to `address` / `host` + `port` of a program
//   started with `ZELL_DEBUG=:2345` (or `zell debug run --headless`).
// * `"lldb"` (stage 1): `lldb-dap` (`lldb-vscode` before LLVM 18) with the
//   Zell formatters loaded through `initCommands` (`command script import
//   tools/lldb/zell_formatters.py`, path printed by `zell debug
//   --formatters lldb`).
//
// The `zell` task type builds the program with debug info (`zell
// build`), for `preLaunchTask`.

import * as cp from "child_process";
import * as fs from "fs";
import * as os from "os";
import * as path from "path";
import * as vscode from "vscode";

/** Finds `name` on PATH; undefined if missing. */
function onPath(name: string): string | undefined {
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

function config(): vscode.WorkspaceConfiguration {
  return vscode.workspace.getConfiguration("zell");
}

/** The `zell` CLI (setting `zell.path`, default `zell` on PATH). */
function zellCli(): string {
  return config().get<string>("path", "zell").trim() || "zell";
}

/** The lldb-dap executable: setting, then the usual names on PATH. */
export function findLldbDap(): string | undefined {
  const configured = config().get<string>("debug.lldbDapPath", "").trim();
  if (configured) {
    return configured;
  }
  for (const name of ["lldb-dap", "lldb-dap-19", "lldb-dap-18", "lldb-vscode", "lldb-vscode-17", "lldb-vscode-16"]) {
    const p = onPath(name);
    if (p) {
      return p;
    }
  }
  return undefined;
}

/** Path of the lldb formatter script: setting, else `zell debug --formatters lldb`. */
function formattersPath(): string | undefined {
  const configured = config().get<string>("debug.formatters", "").trim();
  if (configured) {
    return configured;
  }
  try {
    const out = cp.execFileSync(zellCli(), ["debug", "--formatters", "lldb"], { encoding: "utf8", timeout: 10000 });
    const p = out.trim();
    return p && fs.existsSync(p) ? p : undefined;
  } catch {
    return undefined;
  }
}

/** Default executable for a source file or project directory. */
function defaultProgram(source: string): string {
  const stem = path.basename(source).replace(/\.zl$/, "") || "prog";
  const dir = path.join(vscode.workspace.workspaceFolders?.[0]?.uri.fsPath ?? os.tmpdir(), "target", "zell-debug");
  return path.join(dir, process.platform === "win32" ? `${stem}.exe` : stem);
}

/** Converts `env` given as an object to lldb-dap's `["K=V", ...]`. */
function envList(env: unknown): string[] | undefined {
  if (Array.isArray(env)) {
    return env.map(String);
  }
  if (env && typeof env === "object") {
    return Object.entries(env as Record<string, unknown>).map(([k, v]) => `${k}=${String(v)}`);
  }
  return undefined;
}

class ZellConfigurationProvider implements vscode.DebugConfigurationProvider {
  provideDebugConfigurations(): vscode.DebugConfiguration[] {
    return [
      {
        type: "zell",
        request: "launch",
        name: "Debug Zell file",
        source: "${file}",
        program: "${workspaceFolder}/target/zell-debug/${fileBasenameNoExtension}",
        args: [],
        cwd: "${workspaceFolder}",
        preLaunchTask: "zell: build",
      },
    ];
  }

  resolveDebugConfiguration(_folder: vscode.WorkspaceFolder | undefined, cfg: vscode.DebugConfiguration): vscode.DebugConfiguration | undefined {
    // F5 without a launch.json: debug the active .zl file.
    if (!cfg.type && !cfg.request && !cfg.name) {
      const editor = vscode.window.activeTextEditor;
      if (!editor || editor.document.languageId !== "zell") {
        void vscode.window.showInformationMessage("Zell: open a .zl file or add a `zell` launch configuration.");
        return undefined;
      }
      cfg.type = "zell";
      cfg.request = "launch";
      cfg.name = "Debug Zell file";
      cfg.source = editor.document.uri.fsPath;
    }
    return cfg;
  }

  async resolveDebugConfigurationWithSubstitutedVariables(
    _folder: vscode.WorkspaceFolder | undefined,
    cfg: vscode.DebugConfiguration,
  ): Promise<vscode.DebugConfiguration | undefined> {
    if (usesAgent(cfg)) {
      return resolveAgent(cfg);
    }
    const init: string[] = [];
    const formatters = formattersPath();
    if (formatters) {
      init.push(`command script import "${formatters}"`);
    } else {
      void vscode.window.showWarningMessage("Zell: formatters not found (`zell debug --formatters lldb` failed); values are shown raw.");
    }
    cfg.initCommands = [...init, ...((cfg.initCommands as string[] | undefined) ?? [])];
    if (cfg.request === "launch") {
      // `source` without `program` (and without a preLaunchTask): build here.
      if (cfg.source && !cfg.program) {
        cfg.program = defaultProgram(cfg.source);
      }
      if (cfg.source && cfg.build !== false && !cfg.preLaunchTask) {
        const ok = await build(cfg.source, cfg.program, (cfg.buildArgs as string[] | undefined) ?? []);
        if (!ok) {
          return undefined;
        }
      }
      if (!cfg.program) {
        void vscode.window.showErrorMessage("Zell: the launch configuration needs `program` or `source`.");
        return undefined;
      }
      const env = envList(cfg.env);
      if (env) {
        cfg.env = env;
      }
    } else if (cfg.request === "attach") {
      if (cfg.pid === undefined && !cfg.program) {
        const pid = await vscode.window.showInputBox({ prompt: "Process id to attach to" });
        if (!pid) {
          return undefined;
        }
        cfg.pid = Number(pid);
      }
      if (typeof cfg.pid === "string") {
        cfg.pid = Number(cfg.pid);
      }
    }
    return cfg;
  }
}

let buildOutput: vscode.OutputChannel | undefined;

/** `zell build <source> -o <program>`; shows the diagnostics on failure. */
function build(source: string, program: string, extra: string[]): Promise<boolean> {
  buildOutput ??= vscode.window.createOutputChannel("Zell Build");
  fs.mkdirSync(path.dirname(program), { recursive: true });
  const args = ["build", source, "-o", program, ...extra];
  buildOutput.appendLine(`$ ${zellCli()} ${args.join(" ")}`);
  return new Promise((resolve) => {
    cp.execFile(zellCli(), args, { cwd: vscode.workspace.workspaceFolders?.[0]?.uri.fsPath }, (err, stdout, stderr) => {
      buildOutput?.append(stdout + stderr);
      if (err) {
        buildOutput?.show(true);
        void vscode.window.showErrorMessage(`Zell: build failed (see "Zell Build" output).`);
        resolve(false);
      } else {
        resolve(true);
      }
    });
  });
}

/** Whether a configuration uses the in-process agent (the default). */
function usesAgent(cfg: vscode.DebugConfiguration): boolean {
  return (cfg.adapter ?? "agent") !== "lldb";
}

/** Resolution of an `adapter: "agent"` configuration. */
async function resolveAgent(cfg: vscode.DebugConfiguration): Promise<vscode.DebugConfiguration | undefined> {
  if (cfg.request === "launch") {
    if (cfg.source && !cfg.program) {
      cfg.program = defaultProgram(cfg.source);
    }
    if (cfg.source && cfg.build !== false && !cfg.preLaunchTask) {
      // Debug builds include the agent; `--debug-agent` also covers `--release`.
      const extra = (cfg.buildArgs as string[] | undefined) ?? [];
      const ok = await build(cfg.source, cfg.program, extra.includes("--release") ? [...extra, "--debug-agent"] : extra);
      if (!ok) {
        return undefined;
      }
    }
    if (!cfg.program) {
      void vscode.window.showErrorMessage("Zell: the launch configuration needs `program` or `source`.");
      return undefined;
    }
  } else if (cfg.request === "attach") {
    if (!cfg.address && cfg.port === undefined) {
      const a = await vscode.window.showInputBox({ prompt: "Debug agent address ([host:]port of a program started with ZELL_DEBUG)", value: ":2345" });
      if (!a) {
        return undefined;
      }
      cfg.address = a;
    }
  }
  return cfg;
}

/** `[host:]port` -> host and port (default host 127.0.0.1). */
export function parseAddress(address: string): { host: string; port: number } {
  const i = address.lastIndexOf(":");
  const host = i > 0 ? address.slice(0, i) : "127.0.0.1";
  const port = Number(i >= 0 ? address.slice(i + 1) : address);
  return { host, port };
}

/** The announcement the agent writes to stderr. */
const LISTENING = /zell-debug: listening on (\S+):(\d+)/;

/** Programs started for agent `launch` sessions, by session id. */
const launched = new Map<string, cp.ChildProcess>();

/** Starts `cfg.program` under the agent and resolves with its address. */
function launchUnderAgent(session: vscode.DebugSession): Promise<{ host: string; port: number }> {
  const cfg = session.configuration;
  const env: Record<string, string> = { ...(process.env as Record<string, string>) };
  if (Array.isArray(cfg.env)) {
    for (const kv of cfg.env as string[]) {
      const eq = String(kv).indexOf("=");
      if (eq > 0) {
        env[String(kv).slice(0, eq)] = String(kv).slice(eq + 1);
      }
    }
  } else if (cfg.env && typeof cfg.env === "object") {
    for (const [k, v] of Object.entries(cfg.env as Record<string, unknown>)) {
      env[k] = String(v);
    }
  }
  env.ZELL_DEBUG = "127.0.0.1:0,wait";
  const child = cp.spawn(cfg.program as string, (cfg.args as string[] | undefined) ?? [], { cwd: cfg.cwd as string | undefined, env });
  launched.set(session.id, child);
  const console = (text: string) => vscode.debug.activeDebugConsole.append(text);
  return new Promise((resolve, reject) => {
    let pending = "";
    let found = false;
    const timer = setTimeout(() => {
      if (!found) {
        child.kill();
        reject(new Error("the program did not start the debug agent (built without it? `--release` needs `--debug-agent`)"));
      }
    }, 20000);
    child.stdout?.on("data", (d: Buffer) => console(d.toString()));
    child.stderr?.on("data", (d: Buffer) => {
      if (found) {
        console(d.toString());
        return;
      }
      pending += d.toString();
      const m = LISTENING.exec(pending);
      if (m) {
        found = true;
        clearTimeout(timer);
        const rest = pending.slice(pending.indexOf("\n", m.index) + 1);
        if (rest) {
          console(rest);
        }
        resolve({ host: m[1], port: Number(m[2]) });
      }
    });
    child.on("error", (e) => {
      clearTimeout(timer);
      reject(e);
    });
    child.on("exit", (code) => {
      launched.delete(session.id);
      if (!found) {
        clearTimeout(timer);
        reject(new Error(`the program exited (code ${code}) before the debug agent started`));
      }
    });
  });
}

class ZellAdapterFactory implements vscode.DebugAdapterDescriptorFactory {
  async createDebugAdapterDescriptor(session: vscode.DebugSession): Promise<vscode.DebugAdapterDescriptor | undefined> {
    const cfg = session.configuration;
    if (!usesAgent(cfg)) {
      return lldbDescriptor();
    }
    try {
      if (cfg.request === "attach") {
        const a = cfg.address ? parseAddress(String(cfg.address)) : { host: String(cfg.host ?? "127.0.0.1"), port: Number(cfg.port) };
        return new vscode.DebugAdapterServer(a.port, a.host);
      }
      const a = await launchUnderAgent(session);
      return new vscode.DebugAdapterServer(a.port, a.host);
    } catch (e) {
      void vscode.window.showErrorMessage(`Zell: ${(e as Error).message}`);
      return undefined;
    }
  }
}

/** `adapter: "lldb"`: lldb-dap as an executable adapter. */
function lldbDescriptor(): vscode.DebugAdapterDescriptor | undefined {
  const dap = findLldbDap();
  if (!dap) {
    void vscode.window.showErrorMessage(
      'Zell: lldb-dap not found. Install LLDB 18+ (e.g. `apt install lldb-18`) or set "zell.debug.lldbDapPath".',
    );
    return undefined;
  }
  return new vscode.DebugAdapterExecutable(dap, []);
}

interface ZellTaskDefinition extends vscode.TaskDefinition {
  command: "build";
  source?: string;
  program?: string;
  args?: string[];
}

/** The `zell` task type: `zell build` with debug info for preLaunchTask. */
class ZellTaskProvider implements vscode.TaskProvider {
  provideTasks(): vscode.Task[] {
    const def: ZellTaskDefinition = { type: "zell", command: "build", source: "${file}" };
    return [this.make(def, "build")];
  }

  resolveTask(task: vscode.Task): vscode.Task | undefined {
    const def = task.definition as ZellTaskDefinition;
    if (def.command !== "build") {
      return undefined;
    }
    return this.make(def, task.name);
  }

  private make(def: ZellTaskDefinition, name: string): vscode.Task {
    const source = def.source ?? "${file}";
    const program = def.program ?? "${workspaceFolder}/target/zell-debug/${fileBasenameNoExtension}";
    const args = ["build", source, "-o", program, ...(def.args ?? [])];
    const exec = new vscode.ShellExecution(zellCli(), args);
    const task = new vscode.Task(def, vscode.TaskScope.Workspace, name, "zell", exec, []);
    task.group = vscode.TaskGroup.Build;
    return task;
  }
}

/** Registers the `zell` debugger and task types. */
export function registerDebugging(context: vscode.ExtensionContext): void {
  context.subscriptions.push(
    vscode.debug.registerDebugConfigurationProvider("zell", new ZellConfigurationProvider()),
    vscode.debug.registerDebugAdapterDescriptorFactory("zell", new ZellAdapterFactory()),
    vscode.debug.onDidTerminateDebugSession((s) => {
      // A launched program dies with its session (the agent also exits it
      // on `disconnect` with terminateDebuggee).
      launched.get(s.id)?.kill();
      launched.delete(s.id);
    }),
    vscode.tasks.registerTaskProvider("zell", new ZellTaskProvider()),
  );
}
