//! A small Wadler/Prettier-style document printer.
//!
//! A [`Doc`] describes text with optional line breaks; [`print`] lays it
//! out within a line width. Semantics (differences from Prettier noted):
//!
//! - [`Doc::Line`] is a space and [`Doc::Soft`] nothing when their group
//!   is flat; both are a newline when it is broken. [`Doc::Hard`] is always
//!   a newline (statement bodies), but does *not* force enclosing groups to
//!   break: "fits" measures only up to the first newline, so a call whose
//!   last argument is a lambda stays `f(func() {` ... `})`.
//! - [`Doc::Nest`] indents only when its group is broken; [`Doc::Indent`]
//!   always does (bodies of blocks).
//! - A group built with `force` (a literal written multi-line, a list with
//!   comments) is always broken, and so is every group containing one,
//!   up to an `Indent` (a block body) or a [`Doc::Cond`].
//! - [`Doc::Cond`] picks the first alternative whose first line fits
//!   (Prettier's `conditionalGroup`, used to hug a last-argument literal).
//! - [`Doc::Suffix`] (a trailing comment) has no width and is emitted at
//!   the end of the current line.

/// Indentation step.
pub const INDENT: usize = 4;

#[derive(Clone, Debug)]
pub enum Doc {
    Nil,
    /// Text without newlines.
    Text(String),
    Line,
    Soft,
    Hard,
    Concat(Vec<Doc>),
    Nest(Box<Doc>),
    Indent(Box<Doc>),
    Group { doc: Box<Doc>, force: bool },
    /// (when broken, when flat)
    IfBreak(Box<Doc>, Box<Doc>),
    Suffix(String),
    Cond(Vec<Doc>),
}

pub fn text(s: impl Into<String>) -> Doc {
    Doc::Text(s.into())
}

pub fn concat(v: Vec<Doc>) -> Doc {
    Doc::Concat(v)
}

pub fn nest(d: Doc) -> Doc {
    Doc::Nest(Box::new(d))
}

pub fn indent(d: Doc) -> Doc {
    Doc::Indent(Box::new(d))
}

/// A group; forced broken if `force` or if it contains a forced group.
pub fn group_with(d: Doc, force: bool) -> Doc {
    let force = force || has_forced(&d);
    Doc::Group { doc: Box::new(d), force }
}

pub fn group(d: Doc) -> Doc {
    group_with(d, false)
}

pub fn if_break(broken: Doc, flat: Doc) -> Doc {
    Doc::IfBreak(Box::new(broken), Box::new(flat))
}

/// Does `d` contain a forced group (not looking into `Indent`/`Cond`)?
pub fn has_forced(d: &Doc) -> bool {
    match d {
        Doc::Group { force, .. } => *force,
        Doc::Concat(v) => v.iter().any(has_forced),
        Doc::Nest(d) => has_forced(d),
        Doc::IfBreak(a, b) => has_forced(a) || has_forced(b),
        _ => false,
    }
}

/// Does `d` contain a hard newline anywhere (a block body)?
pub fn has_hard(d: &Doc) -> bool {
    match d {
        Doc::Hard => true,
        Doc::Concat(v) | Doc::Cond(v) => v.iter().any(has_hard),
        Doc::Nest(d) | Doc::Indent(d) | Doc::Group { doc: d, .. } => has_hard(d),
        Doc::IfBreak(a, b) => has_hard(a) || has_hard(b),
        _ => false,
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Mode {
    Flat,
    Break,
}

struct Printer {
    out: String,
    col: usize,
    width: usize,
    suffixes: Vec<String>,
}

impl Printer {
    fn newline(&mut self, ind: usize) {
        self.end_line();
        self.out.push('\n');
        self.out.extend(std::iter::repeat_n(' ', ind));
        self.col = ind;
    }

    fn end_line(&mut self) {
        for s in std::mem::take(&mut self.suffixes) {
            let trimmed = self.out.trim_end_matches(' ').len();
            self.out.truncate(trimmed);
            self.out.push(' ');
            self.out.push_str(&s);
        }
        let trimmed = self.out.trim_end_matches(' ').len();
        self.out.truncate(trimmed);
    }

    fn text(&mut self, s: &str) {
        self.out.push_str(s);
        self.col += s.chars().count();
    }
}

/// Whether `next` (in `mode`) followed by the pending commands `rest`
/// fits in `width` columns up to the next newline.
fn fits<'d>(next: (Mode, &'d Doc), rest: &[(usize, Mode, &'d Doc)], width: isize) -> bool {
    fits_from(vec![next], rest, rest.len(), width)
}

/// [`fits`] with an explicit work stack and the number of `rest` commands
/// not consumed yet. A flat [`Doc::Cond`] is measured the way [`print`]
/// will lay it out: the first alternative that fits flat, else the last
/// one broken. (Measuring only the first alternative made an enclosing
/// group break around a call that then hugged its last argument, and the
/// hugged literal, written multi-line, flipped the choice on the next run:
/// `fmt` was not idempotent on nested calls.)
fn fits_from<'d>(mut stack: Vec<(Mode, &'d Doc)>, rest: &[(usize, Mode, &'d Doc)], mut rest_idx: usize, mut width: isize) -> bool {
    loop {
        let (mode, d) = match stack.pop() {
            Some(x) => x,
            None => {
                if rest_idx == 0 {
                    return true;
                }
                rest_idx -= 1;
                (rest[rest_idx].1, rest[rest_idx].2)
            }
        };
        match d {
            Doc::Nil | Doc::Suffix(_) => {}
            Doc::Text(s) => {
                width -= s.chars().count() as isize;
                if width < 0 {
                    return false;
                }
            }
            Doc::Line => {
                if mode == Mode::Break {
                    return true;
                }
                width -= 1;
                if width < 0 {
                    return false;
                }
            }
            Doc::Soft => {
                if mode == Mode::Break {
                    return true;
                }
            }
            Doc::Hard => return true,
            Doc::Concat(v) => stack.extend(v.iter().rev().map(|d| (mode, d))),
            Doc::Nest(d) | Doc::Indent(d) => stack.push((mode, d)),
            Doc::Group { doc, force } => stack.push((if *force { Mode::Break } else { mode }, doc)),
            Doc::IfBreak(b, f) => stack.push((mode, if mode == Mode::Break { b } else { f })),
            Doc::Cond(vs) => {
                let Some(last) = vs.last() else { continue };
                if mode == Mode::Flat {
                    for v in &vs[..vs.len() - 1] {
                        let mut s = stack.clone();
                        s.push((Mode::Flat, v));
                        if fits_from(s, rest, rest_idx, width) {
                            return true;
                        }
                    }
                }
                stack.push((Mode::Break, last));
            }
        }
    }
}

/// Lays out `doc` within `width` columns. Lines never end in spaces.
pub fn print(doc: &Doc, width: usize) -> String {
    let mut p = Printer { out: String::new(), col: 0, width, suffixes: Vec::new() };
    let mut stack: Vec<(usize, Mode, &Doc)> = vec![(0, Mode::Break, doc)];
    while let Some((ind, mode, d)) = stack.pop() {
        match d {
            Doc::Nil => {}
            Doc::Text(s) => p.text(s),
            Doc::Line => {
                if mode == Mode::Flat {
                    p.text(" ");
                } else {
                    p.newline(ind);
                }
            }
            Doc::Soft => {
                if mode == Mode::Break {
                    p.newline(ind);
                }
            }
            Doc::Hard => p.newline(ind),
            Doc::Concat(v) => stack.extend(v.iter().rev().map(|d| (ind, mode, d))),
            Doc::Nest(d) => stack.push((if mode == Mode::Break { ind + INDENT } else { ind }, mode, d)),
            Doc::Indent(d) => stack.push((ind + INDENT, mode, d)),
            Doc::Group { doc, force } => {
                let m = if *force {
                    Mode::Break
                } else if fits((Mode::Flat, doc), &stack, p.width as isize - p.col as isize) {
                    Mode::Flat
                } else {
                    Mode::Break
                };
                stack.push((ind, m, doc));
            }
            Doc::IfBreak(b, f) => stack.push((ind, mode, if mode == Mode::Break { b } else { f })),
            Doc::Suffix(s) => p.suffixes.push(s.clone()),
            Doc::Cond(vs) => {
                let n = vs.len();
                let mut chosen = None;
                for (i, v) in vs.iter().enumerate() {
                    if i + 1 == n {
                        chosen = Some((Mode::Break, v));
                    } else if fits((Mode::Flat, v), &stack, p.width as isize - p.col as isize) {
                        chosen = Some((Mode::Flat, v));
                        break;
                    }
                }
                if let Some((m, v)) = chosen {
                    stack.push((ind, m, v));
                }
            }
        }
    }
    p.end_line();
    p.out
}

/// Renders `doc` on one line (groups flat), for column alignment.
pub fn flat(doc: &Doc) -> String {
    print(doc, usize::MAX / 4)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn call(name: &str, args: Vec<Doc>) -> Doc {
        let mut inner = vec![Doc::Soft];
        let n = args.len();
        for (i, a) in args.into_iter().enumerate() {
            inner.push(a);
            if i + 1 < n {
                inner.push(text(","));
                inner.push(Doc::Line);
            }
        }
        inner.push(if_break(text(","), Doc::Nil));
        group(concat(vec![text(name), text("("), nest(concat(inner)), Doc::Soft, text(")")]))
    }

    #[test]
    fn groups_break_only_when_too_wide() {
        let d = call("f", vec![text("aaa"), text("bbb")]);
        assert_eq!(print(&d, 20), "f(aaa, bbb)");
        assert_eq!(print(&d, 8), "f(\n    aaa,\n    bbb,\n)");
    }

    #[test]
    fn hard_lines_do_not_break_the_enclosing_group() {
        let body = concat(vec![text("func() {"), indent(concat(vec![Doc::Hard, text("x()")])), Doc::Hard, text("}")]);
        let d = call("go", vec![body]);
        assert_eq!(print(&d, 40), "go(func() {\n    x()\n})");
    }

    #[test]
    fn forced_groups_propagate_and_suffixes_end_lines() {
        let inner = group_with(concat(vec![text("{"), nest(concat(vec![Doc::Line, text("a")])), Doc::Line, text("}")]), true);
        let d = call("f", vec![text("x"), inner]);
        assert_eq!(print(&d, 80), "f(\n    x,\n    {\n        a\n    },\n)");
        let d = concat(vec![text("a"), Doc::Suffix("// c".into()), text(" "), Doc::Hard, text("b")]);
        assert_eq!(print(&d, 80), "a // c\nb");
    }
}
