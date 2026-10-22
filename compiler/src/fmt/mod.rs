//! `zell fmt`: the canonical formatter (docs/TOOLING.md, "Форматтер").
//!
//! One style, no options, like `gofmt`: 4-space indentation, braces on the
//! same line, one space around binary operators, none inside parentheses,
//! struct fields aligned in columns, imports sorted (standard library first, then the
//! rest, one blank line between the groups), at most one blank line kept
//! between statements and members, comments kept in place, lines of at
//! most [`WIDTH`] columns with Wadler/Prettier-style breaking of long
//! calls, literals and binary expressions (trailing commas when broken).
//!
//! Design: the AST carries no trivia. The formatter re-lexes the source
//! with [`lexer::lex_with_trivia`] and walks the AST by spans: before each
//! statement, member or list element it emits the comments that precede
//! it (keeping one blank line where the source had any), and a comment on
//! the same line after a node becomes a trailing comment. Literals keep
//! their source spelling (`0xff`, escapes). Line breaks are only placed
//! where the lexer inserts no semicolon (after `(`, `[`, `{`, `,` and
//! binary operators).
//!
//! Safety net: [`format_source`] re-parses its output and fails instead of
//! returning text whose AST differs from the input's (modulo spans and
//! import order) or that lost a comment.

pub mod cli;
mod diff;
mod doc;
mod print;

pub use diff::{line_hunks, unified_diff, Hunk};

use crate::ast;
use crate::diagnostics::Diagnostics;
use crate::lexer::{self, TriviaKind};
use crate::parser;

/// Maximum line width.
pub const WIDTH: usize = 100;

/// Why a file could not be formatted.
#[derive(Debug)]
pub enum FmtError {
    /// The source has lexical or syntax errors.
    Syntax(Diagnostics),
    /// Internal check failed (a formatter bug); the input is left alone.
    Internal(String),
}

impl std::fmt::Display for FmtError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            FmtError::Syntax(d) => write!(f, "{} syntax error(s)", d.items.len()),
            FmtError::Internal(m) => write!(f, "internal formatter error: {m}"),
        }
    }
}

/// A comment of the source, in order.
#[derive(Clone, Debug)]
pub(crate) struct Comment {
    pub start: u32,
    pub text: String,
    /// A blank line separates it from what precedes it.
    pub blank_before: bool,
}

/// Formats a whole file.
pub fn format_source(src: &str) -> Result<String, FmtError> {
    let (file, diags) = parse_clean(src)?;
    let _ = diags;
    let out = format_ast(src, &file);
    verify(src, &file, &out)?;
    Ok(out)
}

/// Lexes and parses `src`, failing on any lexical or syntax error.
fn parse_clean(src: &str) -> Result<(ast::File, Diagnostics), FmtError> {
    let mut diags = Diagnostics::new();
    let tokens = lexer::lex(src, &mut diags);
    let file = parser::parse_file(tokens, &mut diags);
    if diags.has_errors() {
        return Err(FmtError::Syntax(diags));
    }
    Ok((file, diags))
}

/// Formats a parsed file (`src` is its text).
fn format_ast(src: &str, file: &ast::File) -> String {
    let mut d = Diagnostics::new();
    let lexed = lexer::lex_with_trivia(src, &mut d);
    let mut comments = Vec::new();
    for (ti, range) in lexed.leading.iter().enumerate() {
        let _ = ti;
        let items = &lexed.trivia[range.clone()];
        for (i, t) in items.iter().enumerate() {
            if !t.is_comment() {
                continue;
            }
            let blank_before = i > 0 && matches!(items[i - 1].kind, TriviaKind::Newlines(n) if n >= 2);
            let text = src[t.span.start as usize..t.span.end as usize].trim_end().to_string();
            comments.push(Comment { start: t.span.start, text, blank_before });
        }
    }
    let mut p = print::Fmt::new(src, &lexed, comments);
    let doc = p.file(file);
    let mut out = doc::print(&doc, WIDTH);
    while out.ends_with('\n') {
        out.pop();
    }
    out.push('\n');
    out
}

/// Checks that `out` parses to the same AST as `file` and keeps every comment.
fn verify(src: &str, file: &ast::File, out: &str) -> Result<(), FmtError> {
    let (again, _) = parse_clean(out).map_err(|e| match e {
        FmtError::Syntax(d) => FmtError::Internal(format!("output does not parse: {}", d.items.first().map(|d| d.message.clone()).unwrap_or_default())),
        e => e,
    })?;
    if normalized(file) != normalized(&again) {
        return Err(FmtError::Internal("output parses to a different syntax tree".into()));
    }
    let mut d = Diagnostics::new();
    let count = |s: &str, d: &mut Diagnostics| lexer::lex_with_comments(s, d).1.len();
    if count(src, &mut d) != count(out, &mut d) {
        return Err(FmtError::Internal("a comment was lost".into()));
    }
    Ok(())
}

/// Debug rendering of `file` without spans and with sorted imports: two
/// files are "the same program" for the formatter iff these are equal.
pub fn normalized(file: &ast::File) -> String {
    let mut f = file.clone();
    f.imports.sort_by_key(|i| (i.path.to_string(), i.alias.as_ref().map(|a| a.name.clone())));
    strip_spans(&format!("{f:?}"))
}

/// Removes every `Span { start: N, end: M }` from a Debug rendering.
fn strip_spans(s: &str) -> String {
    const PAT: &str = "Span { start: ";
    let mut out = String::with_capacity(s.len());
    let mut rest = s;
    while let Some(i) = rest.find(PAT) {
        out.push_str(&rest[..i]);
        let after = &rest[i..];
        match after.find('}') {
            Some(j) => {
                out.push('_');
                rest = &after[j + 1..];
            }
            None => {
                rest = after;
                break;
            }
        }
    }
    out.push_str(rest);
    out
}

/// Whether `src` is already formatted (`Err` on syntax errors).
pub fn is_formatted(src: &str) -> Result<bool, FmtError> {
    Ok(format_source(src)? == src)
}
