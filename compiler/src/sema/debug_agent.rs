//! Front-end side of the in-process debug agent (docs/DEBUGGER.md stage 2):
//! a build with `--debug-agent` instantiates `debug.AgentMain` (the
//! agent, written in Zell in `stdlib_src/debug*.zl`) so code generation
//! can start it from the C `main`; and the capability gate of the
//! low-level modules the agent is built on (`unsafe`, `plugin`).

use super::*;
use std::cell::Cell;

thread_local! {
    static AGENT: Cell<bool> = const { Cell::new(false) };
}

/// Runs `f` (a front-end run) with the debug agent wanted or not.
pub fn with<R>(on: bool, f: impl FnOnce() -> R) -> R {
    let saved = AGENT.with(|a| a.replace(on));
    let r = f();
    AGENT.with(|a| a.set(saved));
    r
}

/// Modules that need a capability: (namespace, capability name).
pub const GATED: &[(&str, &str)] = &[("unsafe", "unsafe"), ("plugin", "plugin")];

/// The capability needed to import `path`, if any.
pub fn gate_of(path: &str) -> Option<&'static str> {
    GATED.iter().find(|(ns, _)| path == *ns || path.strip_prefix(ns).is_some_and(|r| r.starts_with('.'))).map(|(_, c)| *c)
}

impl<'a> Sema<'a> {
    /// With the agent wanted and an entry point: instantiates the agent's
    /// entry function so that it is checked and compiled.
    pub(crate) fn instantiate_debug_agent(&mut self) {
        if !AGENT.with(|a| a.get()) || self.files.is_empty() || self.find_entry().is_none() {
            return;
        }
        let Some(&ns) = self.ns_index.get("debug") else { return };
        if let Some((Member::Generic(t), _, _)) = self.nss[ns].members.get("AgentMain").cloned() {
            let saved = (self.cur_file, self.cur_ns);
            self.cur_file = 0;
            self.cur_ns = self.file_scopes.first().map_or(0, |f| f.ns);
            self.instantiate_func(t, Vec::new(), Span::DUMMY);
            (self.cur_file, self.cur_ns) = saved;
        }
    }

    /// `Err(message)` if file `file` may not import the gated module
    /// `path`: only packages granting the capability in their manifest
    /// (`[capabilities] unsafe = true`) may; plain builds may not.
    pub(crate) fn check_capability(&self, file: usize, path: &str) -> Result<(), String> {
        let Some(cap) = gate_of(path) else { return Ok(()) };
        if let Some(p) = self.policy {
            if p.has_capability(file, cap) {
                return Ok(());
            }
            let name = p.file_package.get(file).and_then(|&i| p.packages.get(i)).map(|s| s.name.clone()).unwrap_or_default();
            return Err(format!("package `{name}` cannot import `{path}`: it needs the `{cap}` capability; add `[capabilities]` with `{cap} = true` to its manifest"));
        }
        Err(format!("`{path}` needs the `{cap}` capability, which only packages can grant: add `[capabilities]` with `{cap} = true` to the package manifest"))
    }
}
