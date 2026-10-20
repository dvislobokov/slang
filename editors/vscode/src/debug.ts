// Debugging Slang programs (docs/DEBUGGER.md, stage 1).
//
// A debug configuration of `type: "slang"` is resolved here and handed to
// `lldb-dap` (LLVM's Debug Adapter Protocol server, `lldb-vscode` before
// LLVM 18) with the Slang formatters loaded through `initCommands`
// (`command script import tools/lldb/slang_formatters.py`, path printed by
// `slang debug --formatters lldb`). The `slang` task type builds the
// program with debug info (`slang build`), for `preLaunchTask`.

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
  return vscode.workspace.getConfiguration("slang");
}

/** The `slang` CLI (setting `slang.path`, default `slang` on PATH). */
function slangCli(): string {
  return config().get<string>("path", "slang").trim() || "slang";
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

/** Path of the lldb formatter script: setting, else `slang debug --formatters lldb`. */
function formattersPath(): string | undefined {
  const configured = config().get<string>("debug.formatters", "").trim();
  if (configured) {
    return configured;
  }
  try {
    const out = cp.execFileSync(slangCli(), ["debug", "--formatters", "lldb"], { encoding: "utf8", timeout: 10000 });
    const p = out.trim();
    return p && fs.existsSync(p) ? p : undefined;
  } catch {
    return undefined;
  }
}

/** Default executable for a source file or project directory. */
function defaultProgram(source: string): string {
  const stem = path.basename(source).replace(/\.sl$/, "") || "prog";
  const dir = path.join(vscode.workspace.workspaceFolders?.[0]?.uri.fsPath ?? os.tmpdir(), "target", "slang-debug");
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

class SlangConfigurationProvider implements vscode.DebugConfigurationProvider {
  provideDebugConfigurations(): vscode.DebugConfiguration[] {
    return [
      {
        type: "slang",
        request: "launch",
        name: "Debug Slang file",
        source: "${file}",
        program: "${workspaceFolder}/target/slang-debug/${fileBasenameNoExtension}",
        args: [],
        cwd: "${workspaceFolder}",
        preLaunchTask: "slang: build",
      },
    ];
  }

  resolveDebugConfiguration(_folder: vscode.WorkspaceFolder | undefined, cfg: vscode.DebugConfiguration): vscode.DebugConfiguration | undefined {
    // F5 without a launch.json: debug the active .sl file.
    if (!cfg.type && !cfg.request && !cfg.name) {
      const editor = vscode.window.activeTextEditor;
      if (!editor || editor.document.languageId !== "slang") {
        void vscode.window.showInformationMessage("Slang: open a .sl file or add a `slang` launch configuration.");
        return undefined;
      }
      cfg.type = "slang";
      cfg.request = "launch";
      cfg.name = "Debug Slang file";
      cfg.source = editor.document.uri.fsPath;
    }
    return cfg;
  }

  async resolveDebugConfigurationWithSubstitutedVariables(
    _folder: vscode.WorkspaceFolder | undefined,
    cfg: vscode.DebugConfiguration,
  ): Promise<vscode.DebugConfiguration | undefined> {
    const init: string[] = [];
    const formatters = formattersPath();
    if (formatters) {
      init.push(`command script import "${formatters}"`);
    } else {
      void vscode.window.showWarningMessage("Slang: formatters not found (`slang debug --formatters lldb` failed); values are shown raw.");
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
        void vscode.window.showErrorMessage("Slang: the launch configuration needs `program` or `source`.");
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

/** `slang build <source> -o <program>`; shows the diagnostics on failure. */
function build(source: string, program: string, extra: string[]): Promise<boolean> {
  buildOutput ??= vscode.window.createOutputChannel("Slang Build");
  fs.mkdirSync(path.dirname(program), { recursive: true });
  const args = ["build", source, "-o", program, ...extra];
  buildOutput.appendLine(`$ ${slangCli()} ${args.join(" ")}`);
  return new Promise((resolve) => {
    cp.execFile(slangCli(), args, { cwd: vscode.workspace.workspaceFolders?.[0]?.uri.fsPath }, (err, stdout, stderr) => {
      buildOutput?.append(stdout + stderr);
      if (err) {
        buildOutput?.show(true);
        void vscode.window.showErrorMessage(`Slang: build failed (see "Slang Build" output).`);
        resolve(false);
      } else {
        resolve(true);
      }
    });
  });
}

class LldbDapFactory implements vscode.DebugAdapterDescriptorFactory {
  createDebugAdapterDescriptor(): vscode.ProviderResult<vscode.DebugAdapterDescriptor> {
    const dap = findLldbDap();
    if (!dap) {
      void vscode.window.showErrorMessage(
        'Slang: lldb-dap not found. Install LLDB 18+ (e.g. `apt install lldb-18`) or set "slang.debug.lldbDapPath".',
      );
      return undefined;
    }
    return new vscode.DebugAdapterExecutable(dap, []);
  }
}

interface SlangTaskDefinition extends vscode.TaskDefinition {
  command: "build";
  source?: string;
  program?: string;
  args?: string[];
}

/** The `slang` task type: `slang build` with debug info for preLaunchTask. */
class SlangTaskProvider implements vscode.TaskProvider {
  provideTasks(): vscode.Task[] {
    const def: SlangTaskDefinition = { type: "slang", command: "build", source: "${file}" };
    return [this.make(def, "build")];
  }

  resolveTask(task: vscode.Task): vscode.Task | undefined {
    const def = task.definition as SlangTaskDefinition;
    if (def.command !== "build") {
      return undefined;
    }
    return this.make(def, task.name);
  }

  private make(def: SlangTaskDefinition, name: string): vscode.Task {
    const source = def.source ?? "${file}";
    const program = def.program ?? "${workspaceFolder}/target/slang-debug/${fileBasenameNoExtension}";
    const args = ["build", source, "-o", program, ...(def.args ?? [])];
    const exec = new vscode.ShellExecution(slangCli(), args);
    const task = new vscode.Task(def, vscode.TaskScope.Workspace, name, "slang", exec, []);
    task.group = vscode.TaskGroup.Build;
    return task;
  }
}

/** Registers the `slang` debugger and task types. */
export function registerDebugging(context: vscode.ExtensionContext): void {
  context.subscriptions.push(
    vscode.debug.registerDebugConfigurationProvider("slang", new SlangConfigurationProvider()),
    vscode.debug.registerDebugAdapterDescriptorFactory("slang", new LldbDapFactory()),
    vscode.tasks.registerTaskProvider("slang", new SlangTaskProvider()),
  );
}
