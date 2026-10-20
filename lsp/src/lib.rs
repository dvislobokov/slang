//! Slang language server (LSP over stdio), built on the compiler library's
//! [`slang::session::Session`] API (docs/TOOLING.md, stages 1-2 plus
//! semantic tokens and stage-1 completion).
//!
//! The server is synchronous (`lsp-server`): one thread reads messages;
//! document changes mark the session dirty and analysis runs ~100 ms after
//! the last change (or immediately before answering a request), after which
//! diagnostics are published for every file.

mod features;

use crossbeam_channel::{RecvTimeoutError, Sender};
use lsp_server::{Connection, ExtractError, Message, Notification, Request, RequestId, Response};
use lsp_types::notification::Notification as _;
use lsp_types::*;
use slang::session::{Analysis, Session};
use slang::span::Span;
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

pub use features::{SEMANTIC_TOKEN_MODIFIERS, SEMANTIC_TOKEN_TYPES};

/// Delay between the last edit and re-analysis.
pub const DEBOUNCE: Duration = Duration::from_millis(100);

/// Upper bound on files loaded by workspace discovery.
const MAX_FILES: usize = 5000;

/// Manifest file names that mark a project root (docs/PACKAGES.md).
const MANIFESTS: &[&str] = &["slang.toml", "slang.json", "slang.yaml", "slang.yml"];

/// Runs the server on stdin/stdout until the client disconnects.
pub fn run_stdio() -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    let (connection, io_threads) = Connection::stdio();
    run(connection)?;
    io_threads.join()?;
    Ok(())
}

/// Runs the server on `connection` (stdio or an in-memory pair in tests).
pub fn run(connection: Connection) -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    let (id, params) = connection.initialize_start()?;
    let params: InitializeParams = serde_json::from_value(params)?;
    let result = InitializeResult {
        capabilities: capabilities(),
        server_info: Some(ServerInfo { name: "slang-lsp".into(), version: Some(env!("CARGO_PKG_VERSION").into()) }),
    };
    connection.initialize_finish(id, serde_json::to_value(result)?)?;
    let mut server = Server::new(connection.sender.clone());
    server.discover(&params);
    server.refresh();
    loop {
        let msg = match server.dirty {
            Some(t) => {
                let wait = (t + DEBOUNCE).saturating_duration_since(Instant::now());
                match connection.receiver.recv_timeout(wait) {
                    Ok(m) => m,
                    Err(RecvTimeoutError::Timeout) => {
                        server.refresh();
                        continue;
                    }
                    Err(RecvTimeoutError::Disconnected) => break,
                }
            }
            None => match connection.receiver.recv() {
                Ok(m) => m,
                Err(_) => break,
            },
        };
        match msg {
            Message::Request(req) => {
                if connection.handle_shutdown(&req)? {
                    return Ok(());
                }
                server.handle_request(req);
            }
            Message::Notification(n) => {
                if n.method == notification::Exit::METHOD {
                    return Ok(());
                }
                server.handle_notification(n);
            }
            Message::Response(_) => {}
        }
    }
    Ok(())
}

/// What the server supports.
pub fn capabilities() -> ServerCapabilities {
    ServerCapabilities {
        position_encoding: Some(PositionEncodingKind::UTF16),
        text_document_sync: Some(TextDocumentSyncCapability::Options(TextDocumentSyncOptions {
            open_close: Some(true),
            change: Some(TextDocumentSyncKind::FULL),
            save: Some(TextDocumentSyncSaveOptions::SaveOptions(SaveOptions { include_text: Some(true) })),
            ..Default::default()
        })),
        hover_provider: Some(HoverProviderCapability::Simple(true)),
        completion_provider: Some(CompletionOptions { trigger_characters: Some(vec![".".into()]), ..Default::default() }),
        definition_provider: Some(OneOf::Left(true)),
        references_provider: Some(OneOf::Left(true)),
        document_symbol_provider: Some(OneOf::Left(true)),
        rename_provider: Some(OneOf::Right(RenameOptions { prepare_provider: Some(true), work_done_progress_options: Default::default() })),
        folding_range_provider: Some(FoldingRangeProviderCapability::Simple(true)),
        semantic_tokens_provider: Some(SemanticTokensServerCapabilities::SemanticTokensOptions(SemanticTokensOptions {
            legend: features::legend(),
            full: Some(SemanticTokensFullOptions::Bool(true)),
            range: None,
            ..Default::default()
        })),
        ..Default::default()
    }
}

/// Server state.
struct Server {
    sender: Sender<Message>,
    session: Session,
    analysis: Option<Analysis>,
    /// Time of the last unanalysed change.
    dirty: Option<Instant>,
    roots: Vec<PathBuf>,
    /// Session path -> URI the client knows the file by.
    uris: HashMap<String, Url>,
    open: HashSet<String>,
    /// URIs that got non-empty diagnostics last time.
    published: HashSet<Url>,
}

/// Session key for a URI: the file path for `file:` URIs.
fn path_of(uri: &Url) -> String {
    match uri.to_file_path() {
        Ok(p) => p.display().to_string(),
        Err(()) => uri.to_string(),
    }
}

impl Server {
    fn new(sender: Sender<Message>) -> Server {
        Server {
            sender,
            session: Session::new(),
            analysis: None,
            dirty: None,
            roots: Vec::new(),
            uris: HashMap::new(),
            open: HashSet::new(),
            published: HashSet::new(),
        }
    }

    // -- workspace ---------------------------------------------------------

    #[allow(deprecated)]
    fn discover(&mut self, params: &InitializeParams) {
        let mut roots: Vec<PathBuf> = Vec::new();
        if let Some(folders) = &params.workspace_folders {
            roots.extend(folders.iter().filter_map(|f| f.uri.to_file_path().ok()));
        }
        if roots.is_empty() {
            if let Some(p) = params.root_uri.as_ref().and_then(|u| u.to_file_path().ok()) {
                roots.push(p);
            }
        }
        for r in roots {
            self.add_root(r);
        }
    }

    /// Loads every `.sl` file of the project rooted at `root`: `src/**` when
    /// a manifest is present (and `src/` exists), else the whole tree.
    fn add_root(&mut self, root: PathBuf) {
        if self.roots.iter().any(|r| root.starts_with(r)) {
            return;
        }
        let has_manifest = MANIFESTS.iter().any(|m| root.join(m).is_file());
        let src = root.join("src");
        let dir = if has_manifest && src.is_dir() { src } else { root.clone() };
        let mut files = Vec::new();
        collect_sl(&dir, &mut files);
        files.sort();
        for p in files {
            let key = p.display().to_string();
            if self.open.contains(&key) || self.session.contains(&key) {
                continue;
            }
            if let (Ok(text), Ok(uri)) = (std::fs::read_to_string(&p), Url::from_file_path(&p)) {
                self.uris.insert(key.clone(), uri);
                self.session.update_file(key, text);
            }
        }
        self.roots.push(root);
        self.dirty.get_or_insert_with(Instant::now);
    }

    /// For a file outside every known root, loads the project whose
    /// manifest is found walking up from it.
    fn discover_for(&mut self, path: &Path) {
        if self.roots.iter().any(|r| path.starts_with(r)) {
            return;
        }
        let mut dir = path.parent();
        while let Some(d) = dir {
            if MANIFESTS.iter().any(|m| d.join(m).is_file()) {
                self.add_root(d.to_path_buf());
                return;
            }
            dir = d.parent();
        }
    }

    fn in_roots(&self, path: &Path) -> bool {
        self.roots.iter().any(|r| path.starts_with(r))
    }

    // -- analysis ------------------------------------------------------------

    fn changed(&mut self) {
        self.dirty = Some(Instant::now());
    }

    /// Re-analyses and publishes diagnostics for every file.
    fn refresh(&mut self) {
        self.dirty = None;
        let session = &self.session;
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| session.analyze()));
        match result {
            Ok(a) => {
                self.analysis = Some(a);
                self.publish();
            }
            Err(_) => self.log(MessageType::ERROR, "slang-lsp: internal error during analysis (kept the previous results)"),
        }
    }

    fn ensure_fresh(&mut self) {
        if self.dirty.is_some() || self.analysis.is_none() {
            self.refresh();
        }
    }

    fn publish(&mut self) {
        let Some(a) = &self.analysis else { return };
        let mut now = HashSet::new();
        let mut out = Vec::new();
        for (i, f) in a.files.iter().enumerate() {
            let Some(uri) = self.uris.get(&f.path).cloned() else { continue };
            let diags: Vec<Diagnostic> = a.diagnostics_for(i).map(|d| self.to_lsp_diag(a, d)).collect();
            if !diags.is_empty() {
                now.insert(uri.clone());
            }
            out.push(PublishDiagnosticsParams { uri, diagnostics: diags, version: None });
        }
        for uri in self.published.difference(&now) {
            if !out.iter().any(|p| &p.uri == uri) {
                out.push(PublishDiagnosticsParams { uri: uri.clone(), diagnostics: Vec::new(), version: None });
            }
        }
        self.published = now;
        for p in out {
            self.notify::<notification::PublishDiagnostics>(p);
        }
    }

    fn to_lsp_diag(&self, a: &Analysis, d: &slang::diagnostics::Diagnostic) -> Diagnostic {
        let file = &a.files[d.file];
        let mut message = d.message.clone();
        let mut related = Vec::new();
        for (note, span) in &d.notes {
            match (span, self.uris.get(&file.path)) {
                (Some(s), Some(uri)) => related.push(DiagnosticRelatedInformation { location: Location { uri: uri.clone(), range: range(file, *s) }, message: note.clone() }),
                _ => message.push_str(&format!("\nnote: {note}")),
            }
        }
        Diagnostic {
            range: range(file, d.span),
            severity: Some(match d.severity {
                slang::diagnostics::Severity::Error => DiagnosticSeverity::ERROR,
                slang::diagnostics::Severity::Warning => DiagnosticSeverity::WARNING,
            }),
            source: Some("slang".into()),
            message,
            related_information: (!related.is_empty()).then_some(related),
            ..Default::default()
        }
    }

    fn uri_of(&self, a: &Analysis, file: usize) -> Option<Url> {
        let path = &a.files.get(file)?.path;
        self.uris.get(path).cloned().or_else(|| Url::from_file_path(path).ok())
    }

    // -- messages ------------------------------------------------------------

    fn notify<N: notification::Notification>(&self, params: N::Params) {
        let _ = self.sender.send(Message::Notification(Notification::new(N::METHOD.to_string(), params)));
    }

    fn log(&self, typ: MessageType, message: &str) {
        self.notify::<notification::LogMessage>(LogMessageParams { typ, message: message.to_string() });
    }

    fn reply(&self, id: RequestId, result: impl serde::Serialize) {
        let _ = self.sender.send(Message::Response(Response::new_ok(id, result)));
    }

    fn reply_err(&self, id: RequestId, code: i32, msg: impl Into<String>) {
        let _ = self.sender.send(Message::Response(Response::new_err(id, code, msg.into())));
    }

    fn handle_notification(&mut self, n: Notification) {
        match n.method.as_str() {
            notification::DidOpenTextDocument::METHOD => {
                let Ok(p) = serde_json::from_value::<DidOpenTextDocumentParams>(n.params) else { return };
                let key = path_of(&p.text_document.uri);
                if let Ok(path) = p.text_document.uri.to_file_path() {
                    self.discover_for(&path);
                }
                self.open.insert(key.clone());
                self.uris.insert(key.clone(), p.text_document.uri);
                self.session.update_file(key, p.text_document.text);
                self.changed();
            }
            notification::DidChangeTextDocument::METHOD => {
                let Ok(p) = serde_json::from_value::<DidChangeTextDocumentParams>(n.params) else { return };
                let key = path_of(&p.text_document.uri);
                // Full sync: the last change carries the whole text.
                if let Some(c) = p.content_changes.into_iter().last() {
                    if c.range.is_none() {
                        self.session.update_file(key, c.text);
                        self.changed();
                    }
                }
            }
            notification::DidSaveTextDocument::METHOD => {
                let Ok(p) = serde_json::from_value::<DidSaveTextDocumentParams>(n.params) else { return };
                if let Some(text) = p.text {
                    self.session.update_file(path_of(&p.text_document.uri), text);
                    self.changed();
                }
            }
            notification::DidCloseTextDocument::METHOD => {
                let Ok(p) = serde_json::from_value::<DidCloseTextDocumentParams>(n.params) else { return };
                let key = path_of(&p.text_document.uri);
                self.open.remove(&key);
                let on_disk = p.text_document.uri.to_file_path().ok().filter(|p| self.in_roots(p)).and_then(|p| std::fs::read_to_string(p).ok());
                match on_disk {
                    Some(text) => self.session.update_file(key, text),
                    None => {
                        self.session.remove_file(&key);
                    }
                }
                self.changed();
            }
            _ => {}
        }
    }

    fn handle_request(&mut self, req: Request) {
        let req = match try_handle::<request::HoverRequest>(req, |id, p| self.hover(id, p)) {
            Ok(()) => return,
            Err(r) => r,
        };
        let req = match try_handle::<request::GotoDefinition>(req, |id, p| self.definition(id, p)) {
            Ok(()) => return,
            Err(r) => r,
        };
        let req = match try_handle::<request::References>(req, |id, p| self.references(id, p)) {
            Ok(()) => return,
            Err(r) => r,
        };
        let req = match try_handle::<request::DocumentSymbolRequest>(req, |id, p| self.document_symbols(id, p)) {
            Ok(()) => return,
            Err(r) => r,
        };
        let req = match try_handle::<request::PrepareRenameRequest>(req, |id, p| self.prepare_rename(id, p)) {
            Ok(()) => return,
            Err(r) => r,
        };
        let req = match try_handle::<request::Rename>(req, |id, p| self.rename(id, p)) {
            Ok(()) => return,
            Err(r) => r,
        };
        let req = match try_handle::<request::SemanticTokensFullRequest>(req, |id, p| self.semantic_tokens(id, p)) {
            Ok(()) => return,
            Err(r) => r,
        };
        let req = match try_handle::<request::FoldingRangeRequest>(req, |id, p| self.folding(id, p)) {
            Ok(()) => return,
            Err(r) => r,
        };
        let req = match try_handle::<request::Completion>(req, |id, p| self.completion(id, p)) {
            Ok(()) => return,
            Err(r) => r,
        };
        let _ = self.sender.send(Message::Response(Response::new_err(req.id, lsp_server::ErrorCode::MethodNotFound as i32, format!("unsupported request {}", req.method))));
    }

    /// Fresh analysis plus the file index and byte offset of a position.
    fn locate(&mut self, doc: &TextDocumentIdentifier, pos: Option<Position>) -> Option<(usize, u32)> {
        self.ensure_fresh();
        let a = self.analysis.as_ref()?;
        let file = a.file_index(&path_of(&doc.uri))?;
        let offset = pos.map(|p| a.files[file].utf16_offset(p.line, p.character)).unwrap_or(0);
        Some((file, offset))
    }

    // -- features --------------------------------------------------------------

    fn hover(&mut self, id: RequestId, p: HoverParams) {
        let tdp = p.text_document_position_params;
        let result = self.locate(&tdp.text_document, Some(tdp.position)).and_then(|(file, off)| {
            let a = self.analysis.as_ref()?;
            let sym = a.symbol_at(file, off)?;
            let value = features::hover_markdown(a, sym);
            let r = a.name_span_at(file, off).map(|s| range(&a.files[file], s));
            Some(Hover { contents: HoverContents::Markup(MarkupContent { kind: MarkupKind::Markdown, value }), range: r })
        });
        self.reply(id, result);
    }

    fn definition(&mut self, id: RequestId, p: GotoDefinitionParams) {
        let tdp = p.text_document_position_params;
        let result = self.locate(&tdp.text_document, Some(tdp.position)).and_then(|(file, off)| {
            let a = self.analysis.as_ref()?;
            let loc = a.definition_at(file, off)?;
            let uri = self.uri_of(a, loc.file)?;
            Some(GotoDefinitionResponse::Scalar(Location { uri, range: range(&a.files[loc.file], loc.name_span) }))
        });
        self.reply(id, result);
    }

    fn references(&mut self, id: RequestId, p: ReferenceParams) {
        let tdp = p.text_document_position;
        let include_decl = p.context.include_declaration;
        let result = self.locate(&tdp.text_document, Some(tdp.position)).and_then(|(file, off)| {
            let a = self.analysis.as_ref()?;
            let sym = a.symbol_at(file, off)?;
            let mut out = Vec::new();
            if include_decl {
                if let Some(l) = a.index.symbol(sym).loc {
                    out.push((l.file, l.name_span));
                }
            }
            out.extend(a.references_of(sym).into_iter().map(|r| (r.file, r.span)));
            Some(out.into_iter().filter_map(|(f, s)| Some(Location { uri: self.uri_of(a, f)?, range: range(&a.files[f], s) })).collect::<Vec<_>>())
        });
        self.reply(id, result);
    }

    fn document_symbols(&mut self, id: RequestId, p: DocumentSymbolParams) {
        let result = self.locate(&p.text_document, None).map(|(file, _)| {
            let a = self.analysis.as_ref().unwrap();
            let f = &a.files[file];
            DocumentSymbolResponse::Nested(a.document_symbols(file).iter().map(|s| features::doc_symbol(f, s)).collect())
        });
        self.reply(id, result);
    }

    /// The renamable symbol at a position, or a reason why there is none.
    fn rename_target(&self, file: usize, off: u32) -> Result<(slang::sema::index::SymbolId, Span), String> {
        let a = self.analysis.as_ref().ok_or("no analysis")?;
        let sym_id = a.symbol_at(file, off).ok_or("no symbol at this position")?;
        let sym = a.index.symbol(sym_id);
        if sym.is_std || sym.loc.is_none() {
            return Err(format!("{} is part of the standard library or a builtin and cannot be renamed", sym.name));
        }
        let span = a.name_span_at(file, off).ok_or("no name at this position")?;
        let mut files: Vec<usize> = a.references_of(sym_id).iter().map(|r| r.file).collect();
        files.extend(sym.loc.map(|l| l.file));
        if let Some(&f) = files.iter().find(|&&f| a.stale[f]) {
            return Err(format!("cannot rename while {} has syntax errors", a.files[f].path));
        }
        Ok((sym_id, span))
    }

    fn prepare_rename(&mut self, id: RequestId, p: TextDocumentPositionParams) {
        let Some((file, off)) = self.locate(&p.text_document, Some(p.position)) else {
            return self.reply(id, None::<PrepareRenameResponse>);
        };
        match self.rename_target(file, off) {
            Ok((sym, span)) => {
                let a = self.analysis.as_ref().unwrap();
                let placeholder = a.index.symbol(sym).name.clone();
                self.reply(id, Some(PrepareRenameResponse::RangeWithPlaceholder { range: range(&a.files[file], span), placeholder }));
            }
            Err(msg) => self.reply_err(id, -32803, msg),
        }
    }

    fn rename(&mut self, id: RequestId, p: RenameParams) {
        let tdp = p.text_document_position;
        let Some((file, off)) = self.locate(&tdp.text_document, Some(tdp.position)) else {
            return self.reply(id, None::<WorkspaceEdit>);
        };
        if !features::is_identifier(&p.new_name) {
            return self.reply_err(id, -32803, format!("`{}` is not a valid Slang identifier", p.new_name));
        }
        let (sym_id, _) = match self.rename_target(file, off) {
            Ok(t) => t,
            Err(msg) => return self.reply_err(id, -32803, msg),
        };
        let a = self.analysis.as_ref().unwrap();
        let sym = a.index.symbol(sym_id);
        let mut spans: Vec<(usize, Span)> = sym.loc.map(|l| (l.file, l.name_span)).into_iter().collect();
        spans.extend(a.references_of(sym_id).into_iter().map(|r| (r.file, r.span)));
        let mut changes: HashMap<Url, Vec<TextEdit>> = HashMap::new();
        let mut seen = HashSet::new();
        for (f, s) in spans {
            if !seen.insert((f, s.start)) || a.files[f].text.get(s.start as usize..s.end as usize) != Some(sym.name.as_str()) {
                continue;
            }
            let Some(uri) = self.uri_of(a, f) else { continue };
            changes.entry(uri).or_default().push(TextEdit { range: range(&a.files[f], s), new_text: p.new_name.clone() });
        }
        self.reply(id, Some(WorkspaceEdit { changes: Some(changes), ..Default::default() }));
    }

    fn semantic_tokens(&mut self, id: RequestId, p: SemanticTokensParams) {
        let result = self.locate(&p.text_document, None).map(|(file, _)| {
            let a = self.analysis.as_ref().unwrap();
            SemanticTokensResult::Tokens(SemanticTokens { result_id: None, data: features::semantic_tokens(a, file) })
        });
        self.reply(id, result);
    }

    fn folding(&mut self, id: RequestId, p: FoldingRangeParams) {
        let result = self.locate(&p.text_document, None).map(|(file, _)| features::folding_ranges(&self.analysis.as_ref().unwrap().files[file]));
        self.reply(id, result);
    }

    fn completion(&mut self, id: RequestId, p: CompletionParams) {
        let tdp = p.text_document_position;
        let result = self.locate(&tdp.text_document, Some(tdp.position)).map(|(file, off)| {
            let a = self.analysis.as_ref().unwrap();
            CompletionResponse::Array(features::completions(a, file, off))
        });
        self.reply(id, result);
    }
}

/// Runs `f` if `req` is an `R` request; otherwise gives the request back.
fn try_handle<R: request::Request>(req: Request, f: impl FnOnce(RequestId, R::Params)) -> Result<(), Request> {
    match req.extract::<R::Params>(R::METHOD) {
        Ok((id, params)) => {
            f(id, params);
            Ok(())
        }
        Err(ExtractError::MethodMismatch(r)) => Err(r),
        Err(ExtractError::JsonError { method, error }) => {
            // Malformed params: report and drop.
            eprintln!("slang-lsp: bad params for {method}: {error}");
            Ok(())
        }
    }
}

/// LSP range of a byte span.
pub(crate) fn range(f: &slang::span::SourceFile, s: Span) -> Range {
    let (l1, c1) = f.utf16_position(s.start);
    let (l2, c2) = f.utf16_position(s.end.max(s.start));
    Range { start: Position { line: l1, character: c1 }, end: Position { line: l2, character: c2 } }
}

/// Recursively collects `.sl` files, skipping hidden and build directories.
fn collect_sl(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(entries) = std::fs::read_dir(dir) else { return };
    for e in entries.flatten() {
        if out.len() >= MAX_FILES {
            return;
        }
        let p = e.path();
        let name = e.file_name().to_string_lossy().to_string();
        if p.is_dir() {
            if !name.starts_with('.') && !matches!(name.as_str(), "target" | "node_modules" | "out") {
                collect_sl(&p, out);
            }
        } else if p.extension().is_some_and(|x| x == "sl") {
            out.push(p);
        }
    }
}
