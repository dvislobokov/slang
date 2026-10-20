//! Compiler diagnostics: errors and warnings with source positions.

use crate::span::{SourceFile, Span};
use std::fmt;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Severity {
    Error,
    Warning,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Diagnostic {
    pub severity: Severity,
    pub message: String,
    pub span: Span,
    /// Optional extra lines, e.g. "note: declared here".
    pub notes: Vec<(String, Option<Span>)>,
    /// Index of the source file the span refers to, for multi-file
    /// compilations (the order of files passed to `sema::analyze`).
    /// Single-file phases leave it at 0.
    pub file: usize,
}

impl Diagnostic {
    pub fn error(message: impl Into<String>, span: Span) -> Diagnostic {
        Diagnostic { severity: Severity::Error, message: message.into(), span, notes: vec![], file: 0 }
    }

    pub fn warning(message: impl Into<String>, span: Span) -> Diagnostic {
        Diagnostic { severity: Severity::Warning, message: message.into(), span, notes: vec![], file: 0 }
    }

    /// Sets the file index (see [`Diagnostic::file`]).
    pub fn in_file(mut self, file: usize) -> Diagnostic {
        self.file = file;
        self
    }

    pub fn with_note(mut self, note: impl Into<String>, span: Option<Span>) -> Diagnostic {
        self.notes.push((note.into(), span));
        self
    }

    /// Human-readable rendering:
    /// ```text
    /// error: unexpected token
    ///   --> main.sl:3:5
    ///    |
    ///  3 |     var = 1
    ///    |     ^
    /// ```
    pub fn render(&self, file: &SourceFile) -> String {
        let (line, col) = file.line_col(self.span.start);
        let sev = match self.severity {
            Severity::Error => "error",
            Severity::Warning => "warning",
        };
        let mut out = format!("{sev}: {}\n  --> {}:{line}:{col}\n", self.message, file.path);
        let src = file.line_text(line);
        let width = line.to_string().len();
        let len = (self.span.end.saturating_sub(self.span.start)).max(1) as usize;
        let (eline, _) = file.line_col(self.span.end.max(self.span.start));
        let caret_len = if eline == line { len.min(src.len().saturating_sub(col - 1).max(1)) } else { src.len().saturating_sub(col - 1).max(1) };
        out.push_str(&format!("{:w$} |\n{line:w$} | {src}\n{:w$} | {}{}\n", "", "", " ".repeat(col - 1), "^".repeat(caret_len), w = width));
        for (note, span) in &self.notes {
            match span {
                Some(s) => {
                    let (l, c) = file.line_col(s.start);
                    out.push_str(&format!("{:w$} = note: {note} ({}:{l}:{c})\n", "", file.path, w = width));
                }
                None => out.push_str(&format!("{:w$} = note: {note}\n", "", w = width)),
            }
        }
        out
    }
}

impl fmt::Display for Diagnostic {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}: {} at {}", match self.severity { Severity::Error => "error", Severity::Warning => "warning" }, self.message, self.span)
    }
}

/// Accumulates diagnostics for one compilation.
#[derive(Default, Debug, Clone)]
pub struct Diagnostics {
    pub items: Vec<Diagnostic>,
}

impl Diagnostics {
    pub fn new() -> Diagnostics {
        Diagnostics::default()
    }

    pub fn error(&mut self, message: impl Into<String>, span: Span) {
        self.items.push(Diagnostic::error(message, span));
    }

    pub fn warning(&mut self, message: impl Into<String>, span: Span) {
        self.items.push(Diagnostic::warning(message, span));
    }

    pub fn push(&mut self, d: Diagnostic) {
        self.items.push(d);
    }

    /// True if a diagnostic was already reported at exactly this span and
    /// file (lets later phases avoid duplicate messages).
    pub fn has_errors_at(&self, span: Span, file: usize) -> bool {
        self.items.iter().any(|d| d.span == span && d.file == file)
    }

    pub fn has_errors(&self) -> bool {
        self.items.iter().any(|d| d.severity == Severity::Error)
    }

    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }

    pub fn render_all(&self, file: &SourceFile) -> String {
        self.items.iter().map(|d| d.render(file)).collect::<Vec<_>>().join("\n")
    }

    /// Renders every diagnostic against `files[d.file]` (multi-file builds).
    pub fn render_all_files(&self, files: &[SourceFile]) -> String {
        self.items
            .iter()
            .map(|d| match files.get(d.file) {
                Some(f) => d.render(f),
                None => format!("{d}\n"),
            })
            .collect::<Vec<_>>()
            .join("\n")
    }
}
