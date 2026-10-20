//! Compilation session for tools (LSP, `slang fmt`, ...): a set of source
//! files kept in memory and re-analysed on demand (docs/TOOLING.md,
//! "Session API").
//!
//! The first version re-runs semantic analysis over the whole project on
//! every [`Session::analyze`], but parsing is cached per file and clients
//! only use [`Session::update_file`] / [`Session::analyze`], so incremental
//! analysis can be added later without changing them.
//!
//! Error resilience: when a file stops parsing, the session keeps its last
//! successfully parsed AST, so the other files (and most of the symbol
//! information of the broken file) stay available. The parse diagnostics
//! are still reported; semantic diagnostics of such a *stale* file are
//! dropped because their positions refer to the old text.

use crate::ast;
use crate::diagnostics::{Diagnostic, Diagnostics, Severity};
use crate::hir::Program;
use crate::sema::index::{self, Location, Reference, SymKey, SymbolId, SymbolIndex, SymbolKind};
use crate::span::{SourceFile, Span};
use crate::{lexer, parser};
use std::collections::BTreeMap;

struct FileState {
    source: SourceFile,
    parse_diags: Vec<Diagnostic>,
    /// Whether the current text parsed.
    parsed: bool,
    /// The last successfully parsed AST and the text it came from.
    last_good: Option<(ast::File, String)>,
}

/// In-memory project: path -> source text.
#[derive(Default)]
pub struct Session {
    files: BTreeMap<String, FileState>,
}

impl Session {
    pub fn new() -> Session {
        Session::default()
    }

    /// Adds or replaces a file and parses it.
    pub fn update_file(&mut self, path: impl Into<String>, text: impl Into<String>) {
        let path = path.into();
        let text = text.into();
        if let Some(f) = self.files.get(&path) {
            if f.source.text == text {
                return;
            }
        }
        let mut diags = Diagnostics::new();
        let tokens = lexer::lex(&text, &mut diags);
        let ast = if diags.has_errors() { None } else { parser::parse(tokens, &mut diags) };
        let last_good = match ast {
            Some(a) => Some((a, text.clone())),
            None => self.files.remove(&path).and_then(|f| f.last_good),
        };
        let parsed = last_good.as_ref().is_some_and(|(_, t)| *t == text);
        let state = FileState { source: SourceFile::new(path.clone(), text), parse_diags: diags.items, parsed, last_good };
        self.files.insert(path, state);
    }

    pub fn remove_file(&mut self, path: &str) -> bool {
        self.files.remove(path).is_some()
    }

    pub fn contains(&self, path: &str) -> bool {
        self.files.contains_key(path)
    }

    /// Paths in analysis order (sorted).
    pub fn paths(&self) -> impl Iterator<Item = &str> {
        self.files.keys().map(|s| s.as_str())
    }

    pub fn text(&self, path: &str) -> Option<&str> {
        self.files.get(path).map(|f| f.source.text.as_str())
    }

    /// Parses (cached) and analyses all files.
    pub fn analyze(&self) -> Analysis {
        let files: Vec<SourceFile> = self.files.values().map(|f| f.source.clone()).collect();
        let n = files.len();
        let mut diagnostics = Vec::new();
        let mut stale = vec![false; n];
        let mut asts = Vec::new();
        let mut texts = Vec::new();
        let mut sema_files = Vec::new();
        for (i, f) in self.files.values().enumerate() {
            diagnostics.extend(f.parse_diags.iter().cloned().map(|d| d.in_file(i)));
            if let Some((ast, text)) = &f.last_good {
                stale[i] = !f.parsed;
                asts.push(ast.clone());
                texts.push(text.as_str());
                sema_files.push(i);
            }
        }
        let mut diags = Diagnostics::new();
        let (hir, mut index) = index::analyze_indexed(&asts, &texts, &mut diags);
        let ok = !diags.has_errors() && !diagnostics.iter().any(|d: &Diagnostic| d.severity == Severity::Error);
        for d in diags.items {
            let file = sema_files.get(d.file).copied().unwrap_or(0);
            if !stale[file] {
                diagnostics.push(d.in_file(file));
            }
        }
        index.remap_files(&sema_files, n);
        let mut analyzed = vec![false; n];
        for &i in &sema_files {
            analyzed[i] = true;
        }
        Analysis { files, diagnostics, stale, analyzed, index, hir, sema_files, ok }
    }
}

/// A node of the document outline.
#[derive(Clone, Debug)]
pub struct DocumentSymbol {
    pub name: String,
    pub kind: SymbolKind,
    pub detail: String,
    pub full_span: Span,
    pub name_span: Span,
    pub symbol: Option<SymbolId>,
    pub children: Vec<DocumentSymbol>,
}

/// Result of [`Session::analyze`].
pub struct Analysis {
    /// Current texts, in session order; indices are the `file` of
    /// diagnostics, locations and references.
    pub files: Vec<SourceFile>,
    /// Lexer, parser and semantic diagnostics, with their file index.
    pub diagnostics: Vec<Diagnostic>,
    /// The file does not parse; its symbols come from its last good version.
    pub stale: Vec<bool>,
    /// The file took part in semantic analysis (it parsed now or earlier).
    pub analyzed: Vec<bool>,
    pub index: SymbolIndex,
    /// The HIR program, possibly partial (with `Invalid` types) when there
    /// were errors. Its `file` fields index [`Analysis::sema_files`].
    pub hir: Program,
    /// Session file index of each file given to sema.
    pub sema_files: Vec<usize>,
    ok: bool,
}

impl Analysis {
    /// The analysed program when every file parsed and checked cleanly.
    pub fn program(&self) -> Option<&Program> {
        self.ok.then_some(&self.hir)
    }

    pub fn file_index(&self, path: &str) -> Option<usize> {
        self.files.iter().position(|f| f.path == path)
    }

    pub fn diagnostics_for(&self, file: usize) -> impl Iterator<Item = &Diagnostic> {
        self.diagnostics.iter().filter(move |d| d.file == file)
    }

    /// The symbol named at `offset`: a reference, else a declaration name.
    pub fn symbol_at(&self, file: usize, offset: u32) -> Option<SymbolId> {
        self.index.reference_at(file, offset).map(|r| r.symbol).or_else(|| self.index.declaration_at(file, offset))
    }

    /// The span of the name at `offset` (reference or declaration).
    pub fn name_span_at(&self, file: usize, offset: u32) -> Option<Span> {
        if let Some(r) = self.index.reference_at(file, offset) {
            return Some(r.span);
        }
        let id = self.index.declaration_at(file, offset)?;
        self.index.symbol(id).loc.map(|l| l.name_span)
    }

    /// Where the symbol at `offset` is declared (`None` for std/builtins).
    pub fn definition_at(&self, file: usize, offset: u32) -> Option<Location> {
        let id = self.symbol_at(file, offset)?;
        self.index.symbol(id).loc
    }

    /// Uses of `id` (the declaration itself is not included).
    pub fn references_of(&self, id: SymbolId) -> Vec<Reference> {
        self.index.references_of(id)
    }

    /// Outline of a file: namespace > types/functions > members.
    pub fn document_symbols(&self, file: usize) -> Vec<DocumentSymbol> {
        let Some(Some(info)) = self.index.files.get(file) else { return Vec::new() };
        let ns_id = self.index.lookup(&SymKey::Ns(info.namespace.clone()));
        let mut top: Vec<DocumentSymbol> = self
            .index
            .symbols
            .iter()
            .filter(|s| s.loc.is_some_and(|l| l.file == file) && s.kind != SymbolKind::Namespace)
            .filter(|s| s.container.is_none() || s.container.map(|c| self.index.symbol(c).kind) == Some(SymbolKind::Namespace))
            .filter(|s| outline_kind(s.kind) && !matches!(s.key, SymKey::LocalConst(..)))
            .map(|s| self.outline_node(s.id, file))
            .collect();
        top.sort_by_key(|n| n.full_span.start);
        vec![DocumentSymbol {
            name: info.namespace.clone(),
            kind: SymbolKind::Namespace,
            detail: String::new(),
            full_span: info.span,
            name_span: info.namespace_span,
            symbol: ns_id,
            children: top,
        }]
    }

    fn outline_node(&self, id: SymbolId, file: usize) -> DocumentSymbol {
        let s = self.index.symbol(id);
        let loc = s.loc.unwrap();
        let mut children: Vec<DocumentSymbol> = self
            .index
            .children(id)
            .into_iter()
            .filter(|c| {
                let c = self.index.symbol(*c);
                outline_kind(c.kind) && c.loc.is_some_and(|l| l.file == file) && !matches!(c.key, SymKey::LocalConst(..))
            })
            .map(|c| self.outline_node(c, file))
            .collect();
        children.sort_by_key(|n| n.full_span.start);
        let detail = match s.kind {
            SymbolKind::Struct | SymbolKind::Interface | SymbolKind::Enum => String::new(),
            _ => s.signature.lines().next().unwrap_or("").to_string(),
        };
        let full_span = if loc.full_span.start <= loc.name_span.start && loc.full_span.end >= loc.name_span.end { loc.full_span } else { loc.name_span };
        DocumentSymbol { name: s.name.clone(), kind: s.kind, detail, full_span, name_span: loc.name_span, symbol: Some(id), children }
    }
}

fn outline_kind(k: SymbolKind) -> bool {
    !matches!(k, SymbolKind::Local | SymbolKind::Param | SymbolKind::BuiltinFunc | SymbolKind::BuiltinType)
}

#[cfg(test)]
mod tests {
    use super::*;

    const MODELS: &str = "namespace app.models\n\n// A user.\nstruct User {\n    Name string\n\n    func Hello() string {\n        return \"hi \" + this.Name\n    }\n}\n";
    const MAIN: &str = "namespace app\n\nimport std.io\nimport app.models\n\nfunc main() {\n    var u = User{Name: \"a\"}\n    println(u.Hello())\n}\n";

    fn session() -> Session {
        let mut s = Session::new();
        s.update_file("models.sl", MODELS);
        s.update_file("main.sl", MAIN);
        s
    }

    #[test]
    fn analyzes_and_resolves_across_files() {
        let a = session().analyze();
        assert!(a.diagnostics.is_empty(), "{:?}", a.diagnostics);
        assert!(a.program().is_some());
        let main = a.file_index("main.sl").unwrap();
        let models = a.file_index("models.sl").unwrap();
        let off = MAIN.find("Hello").unwrap() as u32 + 1;
        let loc = a.definition_at(main, off).unwrap();
        assert_eq!(loc.file, models);
        assert_eq!(a.files[models].slice(loc.name_span), "Hello");
        let user = a.symbol_at(main, MAIN.find("User{").unwrap() as u32).unwrap();
        let sym = a.index.symbol(user);
        assert_eq!(sym.kind, SymbolKind::Struct);
        assert_eq!(sym.doc.as_deref(), Some("A user."));
        // `import app.models` does not name User; the struct literal does.
        assert_eq!(a.references_of(user).len(), 1);
        let local = a.symbol_at(main, MAIN.find("u.Hello").unwrap() as u32).unwrap();
        assert_eq!(a.index.symbol(local).signature, "var u User");
    }

    #[test]
    fn keeps_last_good_ast_on_parse_error() {
        let mut s = session();
        s.update_file("models.sl", MODELS.replace("Name string\n", "Name string\n    func (\n"));
        let a = s.analyze();
        let models = a.file_index("models.sl").unwrap();
        assert!(a.stale[models]);
        assert!(a.diagnostics.iter().all(|d| d.file == models), "{:?}", a.diagnostics);
        assert!(!a.diagnostics.is_empty());
        // main.sl still resolves against the last good models.sl.
        let main = a.file_index("main.sl").unwrap();
        assert!(a.definition_at(main, MAIN.find("Hello").unwrap() as u32).is_some());
    }

    #[test]
    fn document_symbols_are_hierarchical() {
        let a = session().analyze();
        let models = a.file_index("models.sl").unwrap();
        let syms = a.document_symbols(models);
        assert_eq!(syms[0].name, "app.models");
        let user = &syms[0].children[0];
        assert_eq!(user.name, "User");
        let names: Vec<&str> = user.children.iter().map(|c| c.name.as_str()).collect();
        assert_eq!(names, ["Name", "Hello"]);
    }

    #[test]
    fn utf16_positions_round_trip() {
        let f = SourceFile::new("x", "a\nпр𝄞x\n");
        let off = "a\nпр𝄞".len() as u32;
        assert_eq!(f.utf16_position(off), (1, 4));
        assert_eq!(f.utf16_offset(1, 4), off);
        assert_eq!(f.utf16_offset(1, 99), "a\nпр𝄞x".len() as u32);
        assert_eq!(f.utf16_offset(9, 0), f.text.len() as u32);
    }
}
