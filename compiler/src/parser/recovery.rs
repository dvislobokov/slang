//! Error reporting and recovery for the parser (see the module docs of
//! [`crate::parser`]): "expected X, found Y" messages, error nodes,
//! resynchronisation at statement and declaration boundaries, lenient
//! closing of lists and bodies.

use super::*;

impl Parser<'_> {

    pub(super) fn error_at(&mut self, span: Span, msg: impl Into<String>) -> ParseError {
        if !self.silent && self.last_error_at != Some(span.start) {
            self.diags.error(msg, span);
            self.error_count += 1;
            self.last_error_at = Some(span.start);
        }
        if !self.silent {
            // Even a de-duplicated error makes the parse fail.
            self.error_count = self.error_count.max(1);
        }
        ParseError
    }

    /// "expected {what}, found {current token}".
    pub(super) fn unexpected(&mut self, what: &str) -> ParseError {
        let msg = format!("expected {what}, found {}", self.peek().describe());
        self.error_at(self.span(), msg)
    }

    /// Reports "expected {what}" and returns the error node `node`, or
    /// fails in speculative (silent) mode so the caller backtracks.
    pub(super) fn recover<T>(&mut self, what: &str, node: T) -> PResult<T> {
        let e = self.unexpected(what);
        if self.silent {
            Err(e)
        } else {
            Ok(node)
        }
    }

    /// Zero-width span at the start of the current token.
    pub(super) fn here(&self) -> Span {
        let s = self.span().start;
        Span::new(s, s)
    }

    /// Consumes the closing delimiter `close` of a list. When it is
    /// missing but the current token ends the enclosing construct (`}`,
    /// end of line, end of file), the error is reported and the list is
    /// treated as closed (`Ok(false)`); otherwise the error propagates.
    pub(super) fn close_list(&mut self, close: &TokenKind, report: bool) -> PResult<bool> {
        if self.eat(close) {
            return Ok(true);
        }
        if !self.silent && self.at_list_boundary() {
            if report {
                self.unexpected(&format!("`{}`", close.text()));
            }
            return Ok(false);
        }
        Err(self.unexpected(&format!("`{}`", close.text())))
    }

    /// A token that cannot continue an expression list: `}`, end of line,
    /// end of file, or a statement keyword (`var`, `return`, `case`, ...;
    /// not after `.`, where keywords are names).
    pub(super) fn at_list_boundary(&self) -> bool {
        use TokenKind::*;
        match self.peek() {
            RBrace | Semi | Eof => true,
            Var | Const | Return | If | For | Switch | Defer | Break | Continue | Case | Default => {
                self.pos == 0 || self.tokens[self.pos - 1].kind != Dot
            }
            _ => false,
        }
    }

    /// Comma-separated `elem`s up to `close` (consumed; trailing comma
    /// allowed). A malformed element is skipped up to the next `,` or
    /// `close` at this nesting level and replaced by `error(span)`; a
    /// missing `close` before `}`/end of line/end of file closes the list
    /// there and appends `error(here)`, so the node is marked recovered.
    pub(super) fn recovering_list<T>(&mut self, close: &TokenKind, mut elem: impl FnMut(&mut Self) -> PResult<T>, error: impl Fn(Span) -> T) -> PResult<Vec<T>> {
        let mut items = Vec::new();
        let errors_before = self.error_count;
        loop {
            if self.at(close) || self.at_eof() {
                break;
            }
            if !self.silent && self.at_list_boundary() {
                // `f(a,` and the line ends: the next element is missing.
                self.unexpected("expression");
                break;
            }
            let start = self.span();
            let before = self.pos;
            match elem(self) {
                Ok(x) => items.push(x),
                Err(e) if self.silent => return Err(e),
                Err(_) => {
                    self.skip_list_elem(close);
                    items.push(error(self.skipped_from(start)));
                }
            }
            if self.eat(&TokenKind::Comma) {
                continue;
            }
            if self.at(close) || self.at_list_boundary() {
                break;
            }
            // Something unexpected after an element: skip to `,` or `close`.
            let e = self.unexpected(&format!("`,` or `{}`", close.text()));
            if self.silent {
                return Err(e);
            }
            self.skip_list_elem(close);
            if self.pos == before {
                break;
            }
            if !self.eat(&TokenKind::Comma) {
                break;
            }
        }
        // One error per list: a missing `)` after a bad element is implied.
        if !self.close_list(close, self.error_count == errors_before)? {
            items.push(error(self.here()));
        }
        Ok(items)
    }

    /// Skips tokens up to (not past) a `,` or `close` at this nesting level,
    /// or an unmatched closing bracket, `;` or end of file.
    pub(super) fn skip_list_elem(&mut self, close: &TokenKind) {
        let mut depth = 0usize;
        loop {
            let k = self.peek();
            if depth == 0 && (k == close || matches!(k, TokenKind::Comma) || self.at_list_boundary()) {
                return;
            }
            match k {
                TokenKind::Eof => return,
                TokenKind::LParen | TokenKind::LBracket | TokenKind::LBrace => depth += 1,
                TokenKind::RParen | TokenKind::RBracket | TokenKind::RBrace => {
                    if depth == 0 {
                        return;
                    }
                    depth -= 1;
                }
                _ => {}
            }
            self.bump();
        }
    }

    /// At statement start: a declaration that cannot be a statement
    /// (`func Name`, `struct`, `interface`, `enum`, `import`), i.e. the
    /// enclosing block lacks its `}`.
    pub(super) fn at_decl_start(&self) -> bool {
        match self.peek() {
            TokenKind::Struct | TokenKind::Interface | TokenKind::Enum | TokenKind::Import | TokenKind::Namespace => true,
            TokenKind::Func => matches!(self.peek_n(1), TokenKind::Ident(_)),
            TokenKind::Static => true,
            TokenKind::Async => matches!(self.peek_n(1), TokenKind::Func) && matches!(self.peek_n(2), TokenKind::Ident(_)),
            _ => false,
        }
    }

    /// After a statement or member: `;`/newline, or the closing `}`.
    pub(super) fn expect_terminator(&mut self) -> PResult<()> {
        match self.peek() {
            TokenKind::Semi => {
                self.bump();
                Ok(())
            }
            TokenKind::RBrace | TokenKind::Eof => Ok(()),
            _ => Err(self.unexpected("end of line")),
        }
    }

    /// Skips to the end of the current statement: past the next `;` at this
    /// nesting level, or up to (not past) a `}` closing the enclosing block,
    /// `case`/`default`, or end of file. Always makes progress unless it stops
    /// at `}` or end of file.
    pub(super) fn sync_stmt(&mut self) {
        let start = self.pos;
        let mut depth = 0usize;
        loop {
            match self.peek() {
                TokenKind::Eof => return,
                TokenKind::Semi if depth == 0 => {
                    self.bump();
                    return;
                }
                TokenKind::RBrace if depth == 0 => return,
                // `func Name`, `struct`, ...: never inside a statement.
                _ if self.pos != start && self.at_decl_start() => return,
                TokenKind::Case | TokenKind::Default if depth == 0 => {
                    if self.pos == start {
                        self.bump();
                    }
                    return;
                }
                TokenKind::LBrace => depth += 1,
                TokenKind::RBrace => depth -= 1,
                _ => {}
            }
            self.bump();
        }
    }

    /// Top-level recovery: skip to the next `;` at depth 0 or the next
    /// declaration keyword.
    pub(super) fn sync_decl(&mut self) {
        let start = self.pos;
        let mut depth = 0usize;
        loop {
            match self.peek() {
                TokenKind::Eof => return,
                TokenKind::Semi if depth == 0 => {
                    self.bump();
                    return;
                }
                TokenKind::Struct
                | TokenKind::Interface
                | TokenKind::Enum
                | TokenKind::Func
                | TokenKind::Async
                | TokenKind::Var
                | TokenKind::Const
                | TokenKind::Import
                    if depth == 0 && self.pos != start =>
                {
                    return
                }
                TokenKind::LBrace => depth += 1,
                TokenKind::RBrace => depth = depth.saturating_sub(1),
                _ => {}
            }
            self.bump();
        }
    }

    /// Runs `f` with one more level of nesting, failing if too deep.
    pub(super) fn nested<T>(&mut self, f: impl FnOnce(&mut Self) -> PResult<T>) -> PResult<T> {
        if self.depth >= MAX_DEPTH {
            return Err(self.error_at(self.span(), "nesting is too deep"));
        }
        self.depth += 1;
        let r = f(self);
        self.depth -= 1;
        r
    }

    /// Runs `f` with struct literals allowed (`true`) or not (`false`).
    pub(super) fn with_struct_lit<T>(&mut self, allowed: bool, f: impl FnOnce(&mut Self) -> T) -> T {
        let saved = self.no_struct_lit;
        self.no_struct_lit = !allowed;
        let r = f(self);
        self.no_struct_lit = saved;
        r
    }

    /// Span of the tokens consumed since `start` (zero-width at `start` if
    /// none were), for error nodes covering skipped input.
    pub(super) fn skipped_from(&self, start: Span) -> Span {
        if self.prev_span.end <= start.start {
            Span::new(start.start, start.start)
        } else {
            start.to(self.prev_span)
        }
    }

    /// The `}` closing a body. When it is missing at end of file or before
    /// a declaration, the error is reported and the body closed there.
    pub(super) fn close_brace(&mut self) -> PResult<()> {
        if self.eat(&TokenKind::RBrace) {
            return Ok(());
        }
        let e = self.unexpected("`}`");
        if !self.silent && (self.at_eof() || self.at_decl_start()) {
            self.closed_early = true;
            return Ok(());
        }
        Err(e)
    }
}
