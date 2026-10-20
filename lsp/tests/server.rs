//! Drives the language server in-process over an in-memory JSON-RPC
//! connection, like an editor would.

use lsp_server::{Connection, Message, Notification, Request, RequestId};
use lsp_types::notification::{self, Notification as _};
use lsp_types::request::{self, Request as _};
use lsp_types::*;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::thread::JoinHandle;
use std::time::Duration;

struct Client {
    conn: Connection,
    server: Option<JoinHandle<()>>,
    next_id: i32,
    /// Notifications received while waiting for responses.
    notes: Vec<Notification>,
}

impl Client {
    fn start(root: &Path) -> Client {
        let (server_conn, client_conn) = Connection::memory();
        let server = std::thread::spawn(move || slang_lsp::run(server_conn).expect("server failed"));
        let mut c = Client { conn: client_conn, server: Some(server), next_id: 0, notes: Vec::new() };
        let root_uri = Url::from_file_path(root).unwrap();
        #[allow(deprecated)]
        let params = InitializeParams {
            root_uri: Some(root_uri.clone()),
            workspace_folders: Some(vec![WorkspaceFolder { uri: root_uri, name: "test".into() }]),
            ..Default::default()
        };
        let init: InitializeResult = serde_json::from_value(c.request_raw(request::Initialize::METHOD, serde_json::to_value(params).unwrap())).unwrap();
        assert!(init.capabilities.semantic_tokens_provider.is_some());
        c.notify::<notification::Initialized>(InitializedParams {});
        c
    }

    fn request_raw(&mut self, method: &str, params: serde_json::Value) -> serde_json::Value {
        self.next_id += 1;
        let id = RequestId::from(self.next_id);
        self.conn.sender.send(Message::Request(Request { id: id.clone(), method: method.into(), params })).unwrap();
        loop {
            match self.conn.receiver.recv_timeout(Duration::from_secs(30)).expect("no response from server") {
                Message::Response(r) if r.id == id => {
                    if let Some(e) = r.error {
                        panic!("{method} failed: {} {}", e.code, e.message);
                    }
                    return r.result.unwrap_or(serde_json::Value::Null);
                }
                Message::Notification(n) => self.notes.push(n),
                _ => {}
            }
        }
    }

    fn request<R: request::Request>(&mut self, params: R::Params) -> R::Result {
        let v = self.request_raw(R::METHOD, serde_json::to_value(params).unwrap());
        serde_json::from_value(v).unwrap()
    }

    /// Sends a request expecting an error response; returns its message.
    fn request_err<R: request::Request>(&mut self, params: R::Params) -> String {
        self.next_id += 1;
        let id = RequestId::from(self.next_id);
        self.conn.sender.send(Message::Request(Request { id: id.clone(), method: R::METHOD.into(), params: serde_json::to_value(params).unwrap() })).unwrap();
        loop {
            match self.conn.receiver.recv_timeout(Duration::from_secs(30)).unwrap() {
                Message::Response(r) if r.id == id => return r.error.expect("expected an error").message,
                Message::Notification(n) => self.notes.push(n),
                _ => {}
            }
        }
    }

    fn notify<N: notification::Notification>(&self, params: N::Params) {
        self.conn.sender.send(Message::Notification(Notification::new(N::METHOD.into(), params))).unwrap();
    }

    fn open(&self, uri: &Url, text: &str) {
        self.notify::<notification::DidOpenTextDocument>(DidOpenTextDocumentParams {
            text_document: TextDocumentItem { uri: uri.clone(), language_id: "slang".into(), version: 1, text: text.into() },
        });
    }

    fn change(&self, uri: &Url, version: i32, text: &str) {
        self.notify::<notification::DidChangeTextDocument>(DidChangeTextDocumentParams {
            text_document: VersionedTextDocumentIdentifier { uri: uri.clone(), version },
            content_changes: vec![TextDocumentContentChangeEvent { range: None, range_length: None, text: text.into() }],
        });
    }

    /// Diagnostics most recently published for `uri`, after forcing the
    /// server to process everything sent so far.
    fn diagnostics(&mut self, uri: &Url) -> Vec<Diagnostic> {
        self.request::<request::DocumentSymbolRequest>(DocumentSymbolParams {
            text_document: TextDocumentIdentifier { uri: uri.clone() },
            work_done_progress_params: Default::default(),
            partial_result_params: Default::default(),
        });
        self.notes
            .iter()
            .rev()
            .filter(|n| n.method == notification::PublishDiagnostics::METHOD)
            .map(|n| serde_json::from_value::<PublishDiagnosticsParams>(n.params.clone()).unwrap())
            .find(|p| &p.uri == uri)
            .map(|p| p.diagnostics)
            .expect("no diagnostics published for the file")
    }
}

impl Drop for Client {
    fn drop(&mut self) {
        let _ = self.conn.sender.send(Message::Request(Request { id: RequestId::from(9999), method: request::Shutdown::METHOD.into(), params: serde_json::Value::Null }));
        let _ = self.conn.sender.send(Message::Notification(Notification::new(notification::Exit::METHOD.into(), ())));
        if let Some(h) = self.server.take() {
            let _ = h.join();
        }
    }
}

fn repo() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap().to_path_buf()
}

/// A fresh temporary project with copies of the given example files.
fn project(files: &[&str]) -> (PathBuf, Vec<(Url, String)>) {
    static N: AtomicUsize = AtomicUsize::new(0);
    let dir = std::env::temp_dir().join(format!("slang-lsp-test-{}-{}", std::process::id(), N.fetch_add(1, Ordering::SeqCst)));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    let mut out = Vec::new();
    for f in files {
        let text = std::fs::read_to_string(repo().join("examples").join(f)).unwrap();
        let name = Path::new(f).file_name().unwrap();
        let p = dir.join(name);
        std::fs::write(&p, &text).unwrap();
        out.push((Url::from_file_path(&p).unwrap(), text));
    }
    (dir, out)
}

fn pos_of(text: &str, needle: &str, nth: usize, delta: usize) -> Position {
    let off = text.match_indices(needle).nth(nth).unwrap_or_else(|| panic!("{needle} not found")).0 + delta;
    let line = text[..off].matches('\n').count() as u32;
    let col = (off - text[..off].rfind('\n').map_or(0, |i| i + 1)) as u32;
    Position { line, character: col }
}

fn tdp(uri: &Url, pos: Position) -> TextDocumentPositionParams {
    TextDocumentPositionParams { text_document: TextDocumentIdentifier { uri: uri.clone() }, position: pos }
}

fn line_text(text: &str, line: u32) -> &str {
    text.lines().nth(line as usize).unwrap()
}

fn range_text<'t>(text: &'t str, r: &Range) -> &'t str {
    assert_eq!(r.start.line, r.end.line);
    let l = line_text(text, r.start.line);
    &l[r.start.character as usize..r.end.character as usize]
}

const MULTI: &[&str] = &["multi/models.sl", "multi/namespaces.sl", "multi/report.sl"];

#[test]
fn examples_have_no_diagnostics() {
    let (dir, files) = project(&["bank.sl"]);
    let mut c = Client::start(&dir);
    c.open(&files[0].0, &files[0].1);
    assert_eq!(c.diagnostics(&files[0].0), vec![]);
    drop(c);

    let (dir, files) = project(MULTI);
    let mut c = Client::start(&dir);
    for (uri, text) in &files {
        c.open(uri, text);
    }
    for (uri, _) in &files {
        assert_eq!(c.diagnostics(uri), vec![], "{uri}");
    }
}

#[test]
fn workspace_files_are_discovered_without_opening() {
    let (dir, files) = project(MULTI);
    let mut c = Client::start(&dir);
    // Only open one file: names from the other two must still resolve.
    let (uri, text) = &files[1];
    c.open(uri, text);
    assert_eq!(c.diagnostics(uri), vec![]);
    let def: Option<GotoDefinitionResponse> = c.request::<request::GotoDefinition>(GotoDefinitionParams {
        text_document_position_params: tdp(uri, pos_of(text, "Rename", 0, 1)),
        work_done_progress_params: Default::default(),
        partial_result_params: Default::default(),
    });
    let Some(GotoDefinitionResponse::Scalar(loc)) = def else { panic!("no definition") };
    assert_eq!(loc.uri, files[0].0);
}

#[test]
fn error_is_reported_with_range_and_cleared() {
    let (dir, files) = project(&["bank.sl"]);
    let (uri, text) = &files[0];
    let mut c = Client::start(&dir);
    c.open(uri, text);
    assert!(c.diagnostics(uri).is_empty());
    // Semantic error: unknown identifier.
    let broken = text.replacen("var frac = cents % 100", "var frac = centz % 100", 1);
    c.change(uri, 2, &broken);
    let d = c.diagnostics(uri);
    assert_eq!(d.len(), 1, "{d:?}");
    assert!(d[0].message.contains("unknown identifier centz"), "{}", d[0].message);
    assert_eq!(d[0].range.start, pos_of(&broken, "centz", 0, 0));
    assert_eq!(d[0].range.end, pos_of(&broken, "centz", 0, 5));
    assert_eq!(d[0].severity, Some(DiagnosticSeverity::ERROR));
    // Syntax error.
    let broken = text.replacen("func Money(cents int) string {", "func Money(cents int string {", 1);
    c.change(uri, 3, &broken);
    let d = c.diagnostics(uri);
    assert!(!d.is_empty());
    assert_eq!(d[0].range.start.line, pos_of(&broken, "func Money", 0, 0).line);
    // Fixed again.
    c.change(uri, 4, text);
    assert!(c.diagnostics(uri).is_empty());
}

#[test]
fn definition_of_method_call() {
    let (dir, files) = project(&["bank.sl"]);
    let (uri, text) = &files[0];
    let mut c = Client::start(&dir);
    c.open(uri, text);
    let def: Option<GotoDefinitionResponse> = c.request::<request::GotoDefinition>(GotoDefinitionParams {
        text_document_position_params: tdp(uri, pos_of(text, "].Deposit(amount)", 0, 3)),
        work_done_progress_params: Default::default(),
        partial_result_params: Default::default(),
    });
    let Some(GotoDefinitionResponse::Scalar(loc)) = def else { panic!("no definition") };
    assert_eq!(&loc.uri, uri);
    assert_eq!(loc.range.start, pos_of(text, "func Deposit(", 0, 5));
    assert_eq!(range_text(text, &loc.range), "Deposit");
}

#[test]
fn references_and_rename_across_files() {
    let (dir, files) = project(MULTI);
    let mut c = Client::start(&dir);
    for (uri, text) in &files {
        c.open(uri, text);
    }
    let (models_uri, models) = &files[0];
    let at = tdp(models_uri, pos_of(models, "struct User", 0, 7));
    let refs: Option<Vec<Location>> = c.request::<request::References>(ReferenceParams {
        text_document_position: at.clone(),
        context: ReferenceContext { include_declaration: false },
        work_done_progress_params: Default::default(),
        partial_result_params: Default::default(),
    });
    let refs = refs.unwrap();
    // models.sl: User{...} in New and the return type; namespaces.sl:
    // User.New x2, app.models.User.New, []User; report.sl: app.models.User.
    let count = |u: &Url| refs.iter().filter(|l| &l.uri == u).count();
    assert_eq!(count(&files[0].0), 2, "{refs:?}");
    assert_eq!(count(&files[1].0), 4, "{refs:?}");
    assert_eq!(count(&files[2].0), 1, "{refs:?}");
    for l in &refs {
        let text = &files.iter().find(|f| f.0 == l.uri).unwrap().1;
        assert_eq!(range_text(text, &l.range), "User");
    }

    let prep: Option<PrepareRenameResponse> = c.request::<request::PrepareRenameRequest>(at.clone());
    assert!(matches!(prep, Some(PrepareRenameResponse::RangeWithPlaceholder { ref placeholder, .. }) if placeholder == "User"));
    let edit: Option<WorkspaceEdit> = c.request::<request::Rename>(RenameParams { text_document_position: at, new_name: "Member".into(), work_done_progress_params: Default::default() });
    let changes = edit.unwrap().changes.unwrap();
    assert_eq!(changes.len(), 3, "{changes:?}");
    assert_eq!(changes[&files[0].0].len(), 3); // declaration + 2 uses
    assert_eq!(changes[&files[1].0].len(), 4);
    // Apply the edits and check that the project still type-checks.
    for (uri, text) in &files {
        let mut edits = changes[uri].clone();
        edits.sort_by_key(|e| std::cmp::Reverse((e.range.start.line, e.range.start.character)));
        let mut lines: Vec<String> = text.lines().map(String::from).collect();
        for e in edits {
            let l = &mut lines[e.range.start.line as usize];
            l.replace_range(e.range.start.character as usize..e.range.end.character as usize, &e.new_text);
        }
        c.change(uri, 2, &(lines.join("\n") + "\n"));
    }
    for (uri, _) in &files {
        assert_eq!(c.diagnostics(uri), vec![], "{uri}");
    }
}

#[test]
fn rename_refuses_std_symbols() {
    let (dir, files) = project(MULTI);
    let (uri, text) = &files[2];
    let mut c = Client::start(&dir);
    c.open(uri, text);
    let msg = c.request_err::<request::PrepareRenameRequest>(tdp(uri, pos_of(text, "println", 0, 2)));
    assert!(msg.contains("standard library"), "{msg}");
}

#[test]
fn hover_shows_signature_and_doc() {
    let (dir, files) = project(&["bank.sl"]);
    let (uri, text) = &files[0];
    let mut c = Client::start(&dir);
    c.open(uri, text);
    let hover = |c: &mut Client, pos| -> String {
        let h: Option<Hover> = c.request::<request::HoverRequest>(HoverParams { text_document_position_params: tdp(uri, pos), work_done_progress_params: Default::default() });
        match h.expect("no hover").contents {
            HoverContents::Markup(m) => m.value,
            other => panic!("{other:?}"),
        }
    };
    // A call of a documented function.
    let h = hover(&mut c, pos_of(text, "Money(this.Balance)", 0, 1));
    assert!(h.contains("func Money(cents int) string"), "{h}");
    assert!(h.contains("Amounts are integer cents."), "{h}");
    // A method.
    let h = hover(&mut c, pos_of(text, "].Deposit(amount)", 0, 3));
    assert!(h.contains("func Deposit(amount int) void!"), "{h}");
    assert!(h.contains("*method* of `examples.bank.Account`"), "{h}");
    // A stdlib function.
    let h = hover(&mut c, pos_of(text, "fmt.Format", 0, 5));
    assert!(h.contains("func Format(template string, args ...any) string"), "{h}");
}

#[test]
fn document_symbols_are_hierarchical() {
    let (dir, files) = project(MULTI);
    let (uri, text) = &files[0];
    let mut c = Client::start(&dir);
    c.open(uri, text);
    let resp: Option<DocumentSymbolResponse> = c.request::<request::DocumentSymbolRequest>(DocumentSymbolParams {
        text_document: TextDocumentIdentifier { uri: uri.clone() },
        work_done_progress_params: Default::default(),
        partial_result_params: Default::default(),
    });
    let Some(DocumentSymbolResponse::Nested(syms)) = resp else { panic!("expected nested symbols") };
    assert_eq!(syms.len(), 1);
    assert_eq!(syms[0].name, "app.models");
    assert_eq!(syms[0].kind, SymbolKind::NAMESPACE);
    let kids = syms[0].children.as_ref().unwrap();
    let names: Vec<&str> = kids.iter().map(|k| k.name.as_str()).collect();
    assert_eq!(names, ["User", "MaxUsers"]);
    assert_eq!(kids[0].kind, SymbolKind::STRUCT);
    let members: Vec<(&str, SymbolKind)> = kids[0].children.as_ref().unwrap().iter().map(|k| (k.name.as_str(), k.kind)).collect();
    assert_eq!(
        members,
        [
            ("Name", SymbolKind::FIELD),
            ("Email", SymbolKind::FIELD),
            ("id", SymbolKind::FIELD),
            ("ID", SymbolKind::METHOD),
            ("Display", SymbolKind::METHOD),
            ("Rename", SymbolKind::METHOD),
            ("New", SymbolKind::METHOD)
        ]
    );
    assert_eq!(range_text(text, &kids[0].selection_range), "User");
}

#[test]
fn semantic_tokens_are_classified_and_stable() {
    let (dir, files) = project(&["bank.sl"]);
    let (uri, text) = &files[0];
    let mut c = Client::start(&dir);
    c.open(uri, text);
    let get = |c: &mut Client| -> Vec<SemanticToken> {
        let r: Option<SemanticTokensResult> = c.request::<request::SemanticTokensFullRequest>(SemanticTokensParams {
            text_document: TextDocumentIdentifier { uri: uri.clone() },
            work_done_progress_params: Default::default(),
            partial_result_params: Default::default(),
        });
        match r.unwrap() {
            SemanticTokensResult::Tokens(t) => t.data,
            other => panic!("{other:?}"),
        }
    };
    let a = get(&mut c);
    assert!(a.len() > 100, "{}", a.len());
    assert_eq!(a, get(&mut c));
    // Decode and look at a few tokens.
    let types = slang_lsp::SEMANTIC_TOKEN_TYPES;
    let (mut line, mut col) = (0u32, 0u32);
    let mut decoded = Vec::new();
    for t in &a {
        if t.delta_line > 0 {
            line += t.delta_line;
            col = t.delta_start;
        } else {
            col += t.delta_start;
        }
        let l = line_text(text, line);
        decoded.push((l[col as usize..(col + t.length) as usize].to_string(), types[t.token_type as usize], t.token_modifiers_bitset));
    }
    let find = |s: &str| decoded.iter().find(|d| d.0 == s).unwrap_or_else(|| panic!("{s} not highlighted"));
    assert_eq!(find("struct").1, "keyword");
    // The first `Account` is a field of InsufficientFunds; the struct is declared later.
    assert_eq!(find("Account").1, "property");
    assert!(decoded.iter().any(|d| d.0 == "Account" && d.1 == "struct" && d.2 & 1 == 1), "struct declaration");
    assert!(decoded.iter().any(|d| d.0 == "Account" && d.1 == "struct" && d.2 & 1 == 0), "struct use");
    assert_eq!(find("Money").1, "function");
    assert_eq!(find("Deposit").1, "enumMember");
    assert_eq!(find("// A transaction request.").1, "comment");
    assert_eq!(find("ErrUnknownAccount").1, "variable");
    assert_eq!(find("ErrUnknownAccount").2 & 8, 8, "readonly modifier on const");
    assert!(decoded.iter().any(|d| d.0 == "Balance" && d.1 == "method"));
    assert!(decoded.iter().any(|d| d.0 == "amount" && d.1 == "parameter"));
    assert!(decoded.iter().any(|d| d.0 == "balance" && d.1 == "property"));
}

#[test]
fn folding_ranges_cover_blocks_and_comments() {
    let (dir, files) = project(&["bank.sl"]);
    let (uri, text) = &files[0];
    let mut c = Client::start(&dir);
    c.open(uri, text);
    let r: Option<Vec<FoldingRange>> = c.request::<request::FoldingRangeRequest>(FoldingRangeParams {
        text_document: TextDocumentIdentifier { uri: uri.clone() },
        work_done_progress_params: Default::default(),
        partial_result_params: Default::default(),
    });
    let r = r.unwrap();
    let account = pos_of(text, "struct Account {", 0, 0).line;
    assert!(r.iter().any(|f| f.start_line == account && f.end_line > account + 5), "{r:?}");
    assert!(r.iter().any(|f| f.kind == Some(FoldingRangeKind::Imports)), "{r:?}");
}

#[test]
fn completion_members_and_scope() {
    let (dir, files) = project(&["structs.sl"]);
    let (uri, text) = &files[0];
    let mut c = Client::start(&dir);
    c.open(uri, text);
    assert!(c.diagnostics(uri).is_empty());
    // The user types `c.` on a new line: the file no longer parses.
    let anchor = "    var c = Counter{}\n";
    let edited = text.replacen(anchor, &format!("{anchor}    c.\n"), 1);
    c.change(uri, 2, &edited);
    assert!(!c.diagnostics(uri).is_empty());
    let complete = |c: &mut Client, pos| -> Vec<CompletionItem> {
        let r: Option<CompletionResponse> = c.request::<request::Completion>(CompletionParams {
            text_document_position: tdp(uri, pos),
            work_done_progress_params: Default::default(),
            partial_result_params: Default::default(),
            context: None,
        });
        match r {
            Some(CompletionResponse::Array(items)) => items,
            Some(CompletionResponse::List(l)) => l.items,
            None => Vec::new(),
        }
    };
    let items = complete(&mut c, pos_of(&edited, "    c.\n", 0, 6));
    let mut labels: Vec<&str> = items.iter().map(|i| i.label.as_str()).collect();
    labels.sort();
    assert_eq!(labels, ["Inc", "Value", "count"], "{labels:?}");
    assert_eq!(items.iter().find(|i| i.label == "Inc").unwrap().kind, Some(CompletionItemKind::METHOD));

    // Static members after a type name, namespace members after a namespace.
    let edited2 = text.replacen(anchor, &format!("{anchor}    Point.\n"), 1);
    c.change(uri, 3, &edited2);
    let items = complete(&mut c, pos_of(&edited2, "    Point.\n", 0, 10));
    assert!(items.iter().any(|i| i.label == "Origin"), "{items:?}");
    let edited3 = text.replacen(anchor, &format!("{anchor}    io.\n"), 1);
    c.change(uri, 4, &edited3);
    let items = complete(&mut c, pos_of(&edited3, "    io.\n", 0, 7));
    assert!(items.iter().any(|i| i.label == "println"), "{items:?}");

    // Names in scope: locals declared before the cursor, declarations, keywords.
    c.change(uri, 5, text);
    let items = complete(&mut c, pos_of(text, "    c.Inc()", 0, 4));
    let has = |l: &str| items.iter().any(|i| i.label == l);
    assert!(has("c") && has("Counter") && has("var") && has("len") && has("println"));
    assert!(!has("heap"), "locals declared later are not offered");
    // Unknown left-hand side: nothing rather than noise.
    let edited4 = text.replacen(anchor, &format!("{anchor}    nosuch.\n"), 1);
    c.change(uri, 6, &edited4);
    assert!(complete(&mut c, pos_of(&edited4, "    nosuch.\n", 0, 11)).is_empty());
}

#[test]
fn every_example_survives_all_queries() {
    // The whole examples tree as one workspace, including files with
    // deliberate compile errors: no request may fail or crash the server.
    let root = repo().join("examples");
    let mut c = Client::start(&root);
    let mut paths = Vec::new();
    let mut stack = vec![root.clone()];
    while let Some(d) = stack.pop() {
        for e in std::fs::read_dir(&d).unwrap().flatten() {
            let p = e.path();
            if p.is_dir() {
                stack.push(p);
            } else if p.extension().is_some_and(|x| x == "sl") {
                paths.push(p);
            }
        }
    }
    assert!(paths.len() > 10);
    for p in paths {
        let uri = Url::from_file_path(&p).unwrap();
        let text = std::fs::read_to_string(&p).unwrap();
        c.open(&uri, &text);
        let doc = TextDocumentIdentifier { uri: uri.clone() };
        let _: Option<SemanticTokensResult> = c.request::<request::SemanticTokensFullRequest>(SemanticTokensParams {
            text_document: doc.clone(),
            work_done_progress_params: Default::default(),
            partial_result_params: Default::default(),
        });
        let _: Option<Vec<FoldingRange>> = c.request::<request::FoldingRangeRequest>(FoldingRangeParams {
            text_document: doc.clone(),
            work_done_progress_params: Default::default(),
            partial_result_params: Default::default(),
        });
        for (line, l) in text.lines().enumerate() {
            let mut col = 0;
            for word in l.split(|c: char| !c.is_ascii_alphanumeric() && c != '_' && c != '.') {
                let at = Position { line: line as u32, character: (col + word.len()) as u32 };
                col += word.len() + 1;
                if word.is_empty() || !l.is_char_boundary(at.character as usize) {
                    continue;
                }
                let _: Option<Hover> = c.request::<request::HoverRequest>(HoverParams { text_document_position_params: tdp(&uri, at), work_done_progress_params: Default::default() });
                let _: Option<GotoDefinitionResponse> = c.request::<request::GotoDefinition>(GotoDefinitionParams {
                    text_document_position_params: tdp(&uri, at),
                    work_done_progress_params: Default::default(),
                    partial_result_params: Default::default(),
                });
                let _: Option<CompletionResponse> = c.request::<request::Completion>(CompletionParams {
                    text_document_position: tdp(&uri, at),
                    work_done_progress_params: Default::default(),
                    partial_result_params: Default::default(),
                    context: None,
                });
            }
        }
    }
}
