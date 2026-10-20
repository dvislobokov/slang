//! Compile-time checks of string-literal arguments of stdlib functions
//! whose argument is a small language of its own: `regex.Pattern(lit)`
//! (RE2 syntax, std.regex) and `template.Parse<T>(lit)` /
//! `html.template.Parse<T>(lit)` (std.text.template; fields checked
//! against `T`, see [`super::tmplcheck`]).
//!
//! The checks only report diagnostics; the call is compiled as usual (the
//! Slang implementation parses the literal again at run time, where it
//! can no longer fail). The regex validator mirrors the parser of
//! `stdlib_src/regex_syntax.sl` rule for rule and produces the same
//! messages (byte offsets into the pattern); keep the two in sync.

use super::*;

/// Unicode classes known to `\p{...}` (std_regex_tables.h, plus `Any`).
const UNICODE_CLASSES: &[&str] = &[
    "Any", "Adlam", "Ahom", "Anatolian_Hieroglyphs", "Arabic", "Armenian", "Avestan", "Balinese", "Bamum",
    "Bassa_Vah", "Batak", "Bengali", "Bhaiksuki", "Bopomofo", "Brahmi", "Braille", "Buginese", "Buhid", "C",
    "Canadian_Aboriginal", "Carian", "Caucasian_Albanian", "Cc", "Cf", "Chakma", "Cham", "Cherokee",
    "Chorasmian", "Co", "Common", "Coptic", "Cs", "Cuneiform", "Cypriot", "Cypro_Minoan", "Cyrillic",
    "Deseret", "Devanagari", "Dives_Akuru", "Dogra", "Duployan", "Egyptian_Hieroglyphs", "Elbasan",
    "Elymaic", "Ethiopic", "Georgian", "Glagolitic", "Gothic", "Grantha", "Greek", "Gujarati",
    "Gunjala_Gondi", "Gurmukhi", "Han", "Hangul", "Hanifi_Rohingya", "Hanunoo", "Hatran", "Hebrew",
    "Hiragana", "Imperial_Aramaic", "Inherited", "Inscriptional_Pahlavi", "Inscriptional_Parthian",
    "Javanese", "Kaithi", "Kannada", "Katakana", "Kawi", "Kayah_Li", "Kharoshthi", "Khitan_Small_Script",
    "Khmer", "Khojki", "Khudawadi", "L", "Lao", "Latin", "Lepcha", "Limbu", "Linear_A", "Linear_B", "Lisu",
    "Ll", "Lm", "Lo", "Lt", "Lu", "Lycian", "Lydian", "M", "Mahajani", "Makasar", "Malayalam", "Mandaic",
    "Manichaean", "Marchen", "Masaram_Gondi", "Mc", "Me", "Medefaidrin", "Meetei_Mayek", "Mende_Kikakui",
    "Meroitic_Cursive", "Meroitic_Hieroglyphs", "Miao", "Mn", "Modi", "Mongolian", "Mro", "Multani",
    "Myanmar", "N", "Nabataean", "Nag_Mundari", "Nandinagari", "Nd", "New_Tai_Lue", "Newa", "Nko", "Nl",
    "No", "Nushu", "Nyiakeng_Puachue_Hmong", "Ogham", "Ol_Chiki", "Old_Hungarian", "Old_Italic",
    "Old_North_Arabian", "Old_Permic", "Old_Persian", "Old_Sogdian", "Old_South_Arabian", "Old_Turkic",
    "Old_Uyghur", "Oriya", "Osage", "Osmanya", "P", "Pahawh_Hmong", "Palmyrene", "Pau_Cin_Hau", "Pc", "Pd",
    "Pe", "Pf", "Phags_Pa", "Phoenician", "Pi", "Po", "Ps", "Psalter_Pahlavi", "Rejang", "Runic", "S",
    "Samaritan", "Saurashtra", "Sc", "Sharada", "Shavian", "Siddham", "SignWriting", "Sinhala", "Sk", "Sm",
    "So", "Sogdian", "Sora_Sompeng", "Soyombo", "Sundanese", "Syloti_Nagri", "Syriac", "Tagalog", "Tagbanwa",
    "Tai_Le", "Tai_Tham", "Tai_Viet", "Takri", "Tamil", "Tangsa", "Tangut", "Telugu", "Thaana", "Thai",
    "Tibetan", "Tifinagh", "Tirhuta", "Toto", "Ugaritic", "Vai", "Vithkuqi", "Wancho", "Warang_Citi",
    "Yezidi", "Yi", "Z", "Zanabazar_Square", "Zl", "Zp", "Zs",
];

const POSIX_CLASSES: &[&str] = &["alnum", "alpha", "ascii", "blank", "cntrl", "digit", "graph", "lower", "print", "punct", "space", "upper", "word", "xdigit"];

/// A syntax error of a regex: message and byte offset.
#[derive(Debug, PartialEq)]
pub struct RegexError {
    pub msg: String,
    pub offset: usize,
}

struct RegexParser<'p> {
    pat: &'p str,
    rs: Vec<char>,
    offs: Vec<usize>,
    pos: usize,
}

type R<T> = Result<T, RegexError>;

impl<'p> RegexParser<'p> {
    fn fail<T>(&self, msg: String, at: usize) -> R<T> {
        let offset = self.offs.get(at).copied().unwrap_or(self.pat.len());
        Err(RegexError { msg, offset })
    }
    fn more(&self) -> bool {
        self.pos < self.rs.len()
    }
    fn peek(&self) -> char {
        self.rs.get(self.pos).copied().unwrap_or('\0')
    }
    fn peek_at(&self, k: usize) -> char {
        self.rs.get(self.pos + k).copied().unwrap_or('\0')
    }
    fn looking_at(&self, t: &str) -> bool {
        t.chars().enumerate().all(|(i, c)| self.rs.get(self.pos + i) == Some(&c))
    }
    fn text(&self, a: usize, b: usize) -> &str {
        let hi = b.min(self.rs.len());
        &self.pat[self.offs[a]..self.offs[hi]]
    }

    fn alt(&mut self, depth: usize) -> R<()> {
        if depth > 1000 {
            return self.fail("expression nests too deeply".into(), self.pos);
        }
        self.concat(depth)?;
        while self.more() && self.peek() == '|' {
            self.pos += 1;
            self.concat(depth)?;
        }
        Ok(())
    }

    fn concat(&mut self, depth: usize) -> R<()> {
        while self.more() {
            let c = self.peek();
            if c == '|' || c == ')' {
                break;
            }
            if c == '*' || c == '+' || c == '?' {
                return self.fail(format!("missing argument to repetition operator {c}"), self.pos);
            }
            if c == '{' {
                if let Some(end) = self.repeat_end() {
                    return self.fail(format!("missing argument to repetition operator {}", self.text(self.pos, end)), self.pos);
                }
            }
            if c == '\\' && self.peek_at(1) == 'Q' {
                self.pos += 2;
                let mut n = 0;
                while self.more() {
                    if self.looking_at("\\E") {
                        self.pos += 2;
                        break;
                    }
                    self.pos += 1;
                    n += 1;
                }
                if n > 0 {
                    self.quantifiers(self.pos - 1)?;
                }
                continue;
            }
            let start = self.pos;
            if !self.atom(depth)? {
                continue;
            }
            self.quantifiers(start)?;
        }
        Ok(())
    }

    fn quantifiers(&mut self, start: usize) -> R<()> {
        let mut seen = false;
        while self.more() {
            let c = self.peek();
            let op_start = self.pos;
            if c == '*' || c == '+' || c == '?' {
                self.pos += 1;
            } else if c == '{' && self.repeat_end().is_some() {
                let end = self.repeat_end().unwrap();
                let (min, max) = self.parse_repeat();
                if min > 1000 || max > 1000 || (max >= 0 && min > max) {
                    return self.fail(format!("invalid repeat count {}", self.text(op_start, end)), op_start);
                }
            } else {
                break;
            }
            if seen {
                return self.fail(format!("invalid nested repetition operator {}", self.text(start, self.pos)), op_start);
            }
            seen = true;
            if self.more() && self.peek() == '?' {
                self.pos += 1;
            }
        }
        Ok(())
    }

    fn repeat_end(&self) -> Option<usize> {
        let mut i = self.pos;
        if self.rs.get(i) != Some(&'{') {
            return None;
        }
        i += 1;
        let d0 = i;
        while self.rs.get(i).is_some_and(|c| c.is_ascii_digit()) {
            i += 1;
        }
        if i == d0 {
            return None;
        }
        if self.rs.get(i) == Some(&',') {
            i += 1;
            while self.rs.get(i).is_some_and(|c| c.is_ascii_digit()) {
                i += 1;
            }
        }
        (self.rs.get(i) == Some(&'}')).then_some(i + 1)
    }

    fn parse_repeat(&mut self) -> (i64, i64) {
        self.pos += 1;
        let min = self.number();
        let mut max = min;
        if self.peek() == ',' {
            self.pos += 1;
            max = if self.peek() == '}' { -1 } else { self.number() };
        }
        self.pos += 1;
        (min, max)
    }

    fn number(&mut self) -> i64 {
        let mut n: i64 = 0;
        while self.more() && self.peek().is_ascii_digit() {
            if n < 100000 {
                n = n * 10 + (self.peek() as i64 - 48);
            }
            self.pos += 1;
        }
        n
    }

    /// One atom; false for a flag group like `(?i)`.
    fn atom(&mut self, depth: usize) -> R<bool> {
        let c = self.peek();
        let start = self.pos;
        match c {
            '(' => return self.group(depth),
            '[' => {
                self.class()?;
                return Ok(true);
            }
            '.' | '^' | '$' => {
                self.pos += 1;
                return Ok(true);
            }
            '\\' => {
                if self.pos + 1 >= self.rs.len() {
                    return self.fail("trailing backslash at end of expression".into(), start);
                }
                let e = self.rs[self.pos + 1];
                if matches!(e, 'A' | 'z' | 'b' | 'B') {
                    self.pos += 2;
                    return Ok(true);
                }
                if matches!(e, 'd' | 'D' | 'w' | 'W' | 's' | 'S' | 'p' | 'P') {
                    self.perl_or_unicode()?;
                    return Ok(true);
                }
                self.escape()?;
                return Ok(true);
            }
            _ => {}
        }
        self.pos += 1;
        Ok(true)
    }

    fn group(&mut self, depth: usize) -> R<bool> {
        let start = self.pos;
        self.pos += 1;
        if self.peek() == '?' {
            if self.looking_at("?=") || self.looking_at("?!") || self.looking_at("?<=") || self.looking_at("?<!") {
                let n = if self.peek_at(1) == '<' { 3 } else { 2 };
                return self.fail(format!("lookaround is not supported (RE2 syntax): {}", self.text(start, self.pos + n)), start);
            }
            if self.looking_at("?P=") || self.looking_at("?P>") {
                return self.fail(format!("backreferences are not supported (RE2 syntax): {}", self.text(start, self.pos + 3)), start);
            }
            if self.looking_at("?P<") || self.looking_at("?<") {
                self.pos += if self.looking_at("?P<") { 3 } else { 2 };
                let name_start = self.pos;
                while self.more() && self.peek() != '>' {
                    self.pos += 1;
                }
                if !self.more() {
                    return self.fail(format!("invalid named capture {}", self.text(start, self.pos)), start);
                }
                let name = self.text(name_start, self.pos);
                if name.is_empty() || !name.chars().all(|c| c == '_' || c.is_ascii_alphanumeric()) {
                    return self.fail(format!("invalid named capture {}", self.text(start, self.pos + 1)), start);
                }
                self.pos += 1;
            } else {
                self.pos += 1;
                let (mut neg, mut any) = (false, false);
                loop {
                    if !self.more() {
                        return self.fail("missing closing )".into(), start);
                    }
                    let c = self.peek();
                    if matches!(c, 'i' | 'm' | 's' | 'U') {
                        any = true;
                        self.pos += 1;
                    } else if c == '-' && !neg {
                        neg = true;
                        any = false;
                        self.pos += 1;
                    } else if (c == ':' || c == ')') && (any || !neg) {
                        break;
                    } else {
                        return self.fail(format!("invalid or unsupported Perl syntax {}", self.text(start, self.pos + 1)), start);
                    }
                }
                if self.peek() == ')' {
                    if !any && !neg {
                        return self.fail("missing argument to repetition operator".into(), start);
                    }
                    self.pos += 1;
                    return Ok(false);
                }
                self.pos += 1;
            }
        }
        self.alt(depth + 1)?;
        if !self.more() || self.peek() != ')' {
            return self.fail("missing closing )".into(), start);
        }
        self.pos += 1;
        Ok(true)
    }

    fn escape(&mut self) -> R<()> {
        let start = self.pos;
        self.pos += 1;
        if !self.more() {
            return self.fail("trailing backslash at end of expression".into(), start);
        }
        let c = self.peek();
        self.pos += 1;
        let octal = |p: &mut Self| {
            for _ in 0..2 {
                if p.more() && ('0'..='7').contains(&p.peek()) {
                    p.pos += 1;
                }
            }
        };
        if ('1'..='7').contains(&c) && self.more() && ('0'..='7').contains(&self.peek()) {
            octal(self);
            return Ok(());
        }
        if ('1'..='9').contains(&c) {
            return self.fail(format!("backreferences are not supported (RE2 syntax): {}", self.text(start, self.pos)), start);
        }
        if c == '0' {
            octal(self);
            return Ok(());
        }
        if c == 'x' {
            if self.peek() == '{' {
                self.pos += 1;
                let (mut v, mut digits) = (0u32, 0);
                while self.more() && self.peek() != '}' {
                    let h = self.peek().to_digit(16);
                    match h {
                        Some(h) if v <= 0x10FFFF => v = v * 16 + h,
                        _ => return self.fail(format!("invalid escape sequence {}", self.text(start, self.pos + 1)), start),
                    }
                    digits += 1;
                    self.pos += 1;
                }
                if !self.more() || digits == 0 || v > 0x10FFFF {
                    return self.fail(format!("invalid escape sequence {}", self.text(start, self.pos)), start);
                }
                self.pos += 1;
                return Ok(());
            }
            if !self.peek().is_ascii_hexdigit() || !self.peek_at(1).is_ascii_hexdigit() {
                return self.fail(format!("invalid escape sequence {}", self.text(start, self.pos + 2)), start);
            }
            self.pos += 2;
            return Ok(());
        }
        if matches!(c, 'a' | 'f' | 't' | 'n' | 'r' | 'v') || (c.is_ascii() && !c.is_ascii_alphanumeric()) {
            return Ok(());
        }
        self.fail(format!("invalid escape sequence {}", self.text(start, self.pos)), start)
    }

    fn perl_or_unicode(&mut self) -> R<()> {
        let start = self.pos;
        let e = self.rs[self.pos + 1];
        self.pos += 2;
        if !matches!(e, 'p' | 'P') {
            return Ok(());
        }
        if !self.more() {
            return self.fail(format!("invalid character class range {}", self.text(start, self.pos)), start);
        }
        let name = if self.peek() == '{' {
            let name_start = self.pos + 1;
            while self.more() && self.peek() != '}' {
                self.pos += 1;
            }
            if !self.more() {
                return self.fail(format!("invalid character class range {}", self.text(start, self.pos)), start);
            }
            let n = self.text(name_start, self.pos).to_string();
            self.pos += 1;
            n
        } else {
            self.pos += 1;
            self.rs[self.pos - 1].to_string()
        };
        let name = name.strip_prefix('^').unwrap_or(&name);
        if !UNICODE_CLASSES.contains(&name) {
            return self.fail(format!("unknown Unicode class {}", self.text(start, self.pos)), start);
        }
        Ok(())
    }

    fn class(&mut self) -> R<()> {
        let start = self.pos;
        self.pos += 1;
        if self.peek() == '^' {
            self.pos += 1;
        }
        let mut first = true;
        loop {
            if !self.more() {
                return self.fail("missing closing ]".into(), start);
            }
            let c = self.peek();
            if c == ']' && !first {
                self.pos += 1;
                return Ok(());
            }
            first = false;
            if c == '[' && self.peek_at(1) == ':' {
                if let Some(end) = self.posix_end() {
                    let name = self.text(self.pos + 2, end - 2);
                    let name = name.strip_prefix('^').unwrap_or(name);
                    if !POSIX_CLASSES.contains(&name) {
                        return self.fail(format!("invalid character class range {}", self.text(self.pos, end)), self.pos);
                    }
                    self.pos = end;
                    continue;
                }
            }
            if c == '\\' && self.pos + 1 < self.rs.len() && matches!(self.rs[self.pos + 1], 'd' | 'D' | 'w' | 'W' | 's' | 'S' | 'p' | 'P') {
                self.perl_or_unicode()?;
                continue;
            }
            let lo_start = self.pos;
            let lo = self.class_char()?;
            if self.peek() == '-' && self.peek_at(1) != ']' && self.pos + 1 < self.rs.len() {
                self.pos += 1;
                let hi = self.class_char()?;
                if hi < lo {
                    return self.fail(format!("invalid character class range {}", self.text(lo_start, self.pos)), lo_start);
                }
            }
        }
    }

    fn posix_end(&self) -> Option<usize> {
        let mut i = self.pos + 2;
        while i + 1 < self.rs.len() {
            if self.rs[i] == ':' && self.rs[i + 1] == ']' {
                return Some(i + 2);
            }
            if self.rs[i] == ']' {
                return None;
            }
            i += 1;
        }
        None
    }

    /// One class member; its code point (escapes decoded).
    fn class_char(&mut self) -> R<u32> {
        let c = self.peek();
        if c != '\\' {
            self.pos += 1;
            return Ok(c as u32);
        }
        if self.peek_at(1) == 'b' {
            return self.fail(format!("invalid escape sequence {}", self.text(self.pos, self.pos + 2)), self.pos);
        }
        let start = self.pos;
        self.escape()?;
        Ok(decode_escape(self.text(start, self.pos)))
    }
}

/// The code point of a valid single-character escape (`\n`, `\x41`, `\101`, `\.`).
fn decode_escape(t: &str) -> u32 {
    let body = &t[1..];
    let first = body.chars().next().unwrap_or('\0');
    match first {
        'a' => 7,
        'f' => 12,
        't' => 9,
        'n' => 10,
        'r' => 13,
        'v' => 11,
        'x' => {
            let h = body[1..].trim_start_matches('{').trim_end_matches('}');
            u32::from_str_radix(h, 16).unwrap_or(0)
        }
        '0'..='7' => u32::from_str_radix(body, 8).unwrap_or(0),
        c => c as u32,
    }
}

/// Checks a regex pattern (RE2 syntax as accepted by std.regex).
pub fn check_regex(pattern: &str) -> Result<(), RegexError> {
    let mut offs: Vec<usize> = pattern.char_indices().map(|(i, _)| i).collect();
    offs.push(pattern.len());
    let mut p = RegexParser { pat: pattern, rs: pattern.chars().collect(), offs, pos: 0 };
    p.alt(0)?;
    if p.more() {
        return p.fail("unexpected )".into(), p.pos);
    }
    Ok(())
}

impl<'a> Sema<'a> {
    /// Hook of [`generic_call`](Self::generic_call), after the callee
    /// template `t` was instantiated with `types`: literal checks of
    /// `std.regex.Pattern` and `std.text.template.Parse<T>` /
    /// `std.html.template.Parse<T>`.
    pub(crate) fn std_literal_check(&mut self, t: u32, types: &[TypeId], args: &[ast::Expr]) {
        let tm = &self.gen.templates[t as usize];
        if !tm.is_std {
            return;
        }
        let ns = self.nss[tm.ns].path.clone();
        let name = tm.name.clone();
        // the literal: the first argument (ParseNamed: the second)
        let idx = usize::from(name == "ParseNamed");
        let Some(arg) = args.get(idx) else { return };
        let ast::ExprKind::Str(lit) = &expr::strip_parens(arg).kind else { return };
        match (ns.as_str(), name.as_str()) {
            ("std.regex", "Pattern") => {
                if let Err(e) = check_regex(lit) {
                    self.err(format!("invalid regex pattern: {} at offset {}", e.msg, e.offset), arg.span);
                }
            }
            ("std.text.template" | "std.html.template", "Parse" | "ParseWith" | "ParseNamed") => {
                if let Some(&ty) = types.first() {
                    let lit = lit.clone();
                    self.check_template_literal(&lit, ty, name == "ParseWith", arg.span);
                }
            }
            _ => {}
        }
    }

    /// Checks a template literal against its data type `ty` (the rules of
    /// `stdlib_src/template_check.sl`); `custom` allows unknown functions
    /// (`ParseWith`, whose function map is only known at run time).
    fn check_template_literal(&mut self, src: &str, ty: TypeId, custom: bool, span: Span) {
        let tree = match TmplParser::new(src, custom).parse() {
            Ok(t) => t,
            Err(e) => {
                self.err(format!("invalid template: {e}"), span);
                return;
            }
        };
        let mut ck = TmplChecker { sema: self, vars: vec![("$".to_string(), Some(ty))] };
        if let Err(e) = ck.list(&tree, Some(ty)) {
            self.err(format!("invalid template: {e}"), span);
        }
    }
}

// -- templates (std.text.template) ----------------------------------------------

/// A template node, as far as the type check needs it.
enum TNode {
    Action(TPipe),
    /// if / with (with: the body's dot is the pipeline value).
    Cond { with: bool, pipe: TPipe, list: Vec<TNode>, else_list: Vec<TNode> },
    Range { pipe: TPipe, list: Vec<TNode>, else_list: Vec<TNode> },
    Call(Option<TPipe>),
}

struct TPipe {
    vars: Vec<String>,
    assign: bool,
    cmds: Vec<Vec<TArg>>,
    line: usize,
}

enum TArg {
    Field(Vec<String>),
    Var(String, Vec<String>),
    Dot,
    Func(String),
    Lit,
    Pipe(Box<TPipe>, Vec<String>),
}

/// Built-in template functions (template_exec.sl `builtin`).
const TMPL_FUNCS: &[&str] = &[
    "and", "or", "not", "len", "index", "slice", "print", "printf", "println", "eq", "ne", "lt", "le", "gt", "ge", "html", "js", "urlquery", "upper",
    "lower", "title", "trim", "join", "split", "contains", "hasPrefix", "hasSuffix", "replace", "repeat", "default",
];

/// The template parser of `template.sl`, rule for rule (same messages).
struct TmplParser<'s> {
    src: &'s str,
    b: &'s [u8],
    pos: usize,
    line: usize,
    custom: bool,
    range_depth: usize,
    vars: Vec<String>,
    defs: Vec<String>,
}

type TR<T> = Result<T, String>;

struct TList {
    list: Vec<TNode>,
    end: String,
    else_kw: String,
}

fn tmpl_space(c: u8) -> bool {
    c == b' ' || c == b'\t' || c == b'\n' || c == b'\r'
}

fn tmpl_ident(c: u8) -> bool {
    c == b'_' || c.is_ascii_alphanumeric() || c >= 128
}

impl<'s> TmplParser<'s> {
    fn new(src: &'s str, custom: bool) -> Self {
        TmplParser { src, b: src.as_bytes(), pos: 0, line: 1, custom, range_depth: 0, vars: Vec::new(), defs: Vec::new() }
    }

    fn fail<T>(&self, msg: impl std::fmt::Display) -> TR<T> {
        Err(format!("template: template:{}: {msg}", self.line))
    }

    fn rest(&self) -> &'s str {
        &self.src[self.pos.min(self.src.len())..]
    }

    fn parse(mut self) -> TR<Vec<TNode>> {
        let r = self.list()?;
        if !r.end.is_empty() {
            return self.fail(format!("unexpected {{{{{}}}}}", r.end));
        }
        Ok(r.list)
    }

    fn list(&mut self) -> TR<TList> {
        let mut nodes = Vec::new();
        loop {
            let open = self.src[self.pos..].find("{{").map(|i| i + self.pos);
            let text_end = open.unwrap_or(self.src.len());
            self.line += self.src[self.pos..text_end].matches('\n').count();
            let Some(open) = open else {
                self.pos = self.src.len();
                return Ok(TList { list: nodes, end: String::new(), else_kw: String::new() });
            };
            let trim_left = self.b.get(open + 2) == Some(&b'-') && self.b.get(open + 3).is_some_and(|c| tmpl_space(*c));
            self.pos = open + 2 + if trim_left { 2 } else { 0 };
            self.skip_space();
            if self.rest().starts_with("/*") {
                let Some(close) = self.src[self.pos + 2..].find("*/").map(|i| i + self.pos + 2) else {
                    return self.fail("unclosed comment");
                };
                self.line += self.src[self.pos..close].matches('\n').count();
                self.pos = close + 2;
                self.close_action()?;
                continue;
            }
            let kw = self.peek_word().to_string();
            match kw.as_str() {
                "end" => {
                    self.pos += 3;
                    self.close_action()?;
                    return Ok(TList { list: nodes, end: "end".into(), else_kw: String::new() });
                }
                "else" => {
                    self.pos += 4;
                    self.skip_space();
                    let next = self.peek_word().to_string();
                    if next == "if" || next == "with" {
                        return Ok(TList { list: nodes, end: "else".into(), else_kw: next });
                    }
                    self.close_action()?;
                    return Ok(TList { list: nodes, end: "else".into(), else_kw: String::new() });
                }
                "if" | "with" | "range" => {
                    self.pos += kw.len();
                    nodes.push(self.control(&kw)?);
                }
                "define" | "block" => {
                    self.pos += kw.len();
                    if let Some(n) = self.define(kw == "block")? {
                        nodes.push(n);
                    }
                }
                "template" => {
                    self.pos += 8;
                    self.skip_space();
                    self.string_lit()?;
                    self.skip_space();
                    let pipe = if self.at_action_end() { None } else { Some(self.pipeline(false)?) };
                    self.close_action()?;
                    nodes.push(TNode::Call(pipe));
                }
                "break" | "continue" => {
                    if self.range_depth == 0 {
                        return self.fail(format!("{{{{{kw}}}}} outside {{{{range}}}}"));
                    }
                    self.pos += kw.len();
                    self.close_action()?;
                }
                _ => {
                    let pipe = self.pipeline(true)?;
                    self.close_action()?;
                    nodes.push(TNode::Action(pipe));
                }
            }
        }
    }

    fn control(&mut self, kw: &str) -> TR<TNode> {
        let mark = self.vars.len();
        let pipe = self.pipeline(true)?;
        self.close_action()?;
        if kw == "range" {
            self.range_depth += 1;
        }
        let body = self.list()?;
        if kw == "range" {
            self.range_depth -= 1;
        }
        if body.end.is_empty() {
            return self.fail(format!("unexpected EOF: missing {{{{end}}}} for {{{{{kw}}}}}"));
        }
        let mut else_list = Vec::new();
        if body.end == "else" {
            if !body.else_kw.is_empty() {
                if kw == "range" {
                    return self.fail(format!("{{{{else {}}}}} after {{{{range}}}}", body.else_kw));
                }
                self.pos += body.else_kw.len();
                let kw2 = body.else_kw.clone();
                else_list.push(self.control(&kw2)?);
            } else {
                let els = self.list()?;
                if els.end != "end" {
                    if els.end.is_empty() {
                        return self.fail(format!("unexpected EOF: missing {{{{end}}}} for {{{{{kw}}}}}"));
                    }
                    return self.fail(format!("expected {{{{end}}}}, found {{{{{}}}}}", els.end));
                }
                else_list = els.list;
            }
        }
        self.vars.truncate(mark);
        Ok(match kw {
            "range" => TNode::Range { pipe, list: body.list, else_list },
            _ => TNode::Cond { with: kw == "with", pipe, list: body.list, else_list },
        })
    }

    fn define(&mut self, block: bool) -> TR<Option<TNode>> {
        self.skip_space();
        let name = self.string_lit()?;
        self.skip_space();
        let pipe = if block { Some(self.pipeline(false)?) } else { None };
        self.close_action()?;
        let saved = std::mem::take(&mut self.vars);
        let body = self.list()?;
        self.vars = saved;
        if body.end != "end" {
            return self.fail("unexpected EOF: missing {{end}} for {{define}}");
        }
        if self.defs.contains(&name) {
            return self.fail(format!("template {name:?} redefined"));
        }
        self.defs.push(name);
        Ok(if block { Some(TNode::Call(pipe)) } else { None })
    }

    fn close_action(&mut self) -> TR<()> {
        self.skip_space();
        let r = self.rest();
        if r.starts_with("-}}") {
            self.pos += 3;
            return Ok(());
        }
        if r.starts_with("}}") {
            self.pos += 2;
            return Ok(());
        }
        if r.is_empty() {
            return self.fail("unclosed action");
        }
        let end = r.char_indices().nth(10).map_or(r.len(), |(i, _)| i);
        self.fail(format!("unexpected {} in action", json_quote(&r[..end])))
    }

    fn at_action_end(&self) -> bool {
        let r = self.rest();
        r.starts_with("}}") || r.starts_with("-}}") || r.is_empty()
    }

    fn skip_space(&mut self) {
        while self.pos < self.b.len() && tmpl_space(self.b[self.pos]) {
            if self.b[self.pos] == b'\n' {
                self.line += 1;
            }
            self.pos += 1;
        }
    }

    fn peek_word(&self) -> &'s str {
        let mut i = self.pos;
        while i < self.b.len() && tmpl_ident(self.b[i]) {
            i += 1;
        }
        &self.src[self.pos..i]
    }

    fn string_lit(&mut self) -> TR<String> {
        match self.b.get(self.pos) {
            None => self.fail("expected a template name"),
            Some(b'"') => self.quoted(),
            Some(b'`') => {
                let Some(close) = self.src[self.pos + 1..].find('`').map(|i| i + self.pos + 1) else {
                    return self.fail("unterminated raw string");
                };
                let s = self.src[self.pos + 1..close].to_string();
                self.pos = close + 1;
                Ok(s)
            }
            _ => self.fail("expected a quoted template name"),
        }
    }

    fn quoted(&mut self) -> TR<String> {
        self.pos += 1;
        let mut out = String::new();
        loop {
            let Some(&c) = self.b.get(self.pos) else { return self.fail("unterminated quoted string") };
            if c == b'\n' {
                return self.fail("unterminated quoted string");
            }
            if c == b'"' {
                self.pos += 1;
                return Ok(out);
            }
            if c == b'\\' && self.pos + 1 < self.b.len() {
                let e = self.b[self.pos + 1];
                self.pos += 2;
                match e {
                    b'n' => out.push('\n'),
                    b't' => out.push('\t'),
                    b'r' => out.push('\r'),
                    b'"' | b'\\' | b'\'' => out.push(e as char),
                    _ => return self.fail("invalid escape in a string"),
                }
                continue;
            }
            let ch = self.rest().chars().next().unwrap();
            out.push(ch);
            self.pos += ch.len_utf8();
        }
    }

    fn has_var(&self, n: &str) -> bool {
        n == "$" || self.vars.iter().any(|v| v == n)
    }

    fn pipeline(&mut self, allow_decl: bool) -> TR<TPipe> {
        let mut p = TPipe { vars: Vec::new(), assign: false, cmds: Vec::new(), line: self.line };
        self.skip_space();
        let save = self.pos;
        if allow_decl && self.b.get(self.pos) == Some(&b'$') {
            let mut names = Vec::new();
            let mut ok = false;
            loop {
                let start = self.pos;
                self.pos += 1;
                while self.pos < self.b.len() && tmpl_ident(self.b[self.pos]) {
                    self.pos += 1;
                }
                names.push(self.src[start..self.pos].to_string());
                self.skip_space();
                if self.rest().starts_with(":=") {
                    self.pos += 2;
                    ok = true;
                    break;
                }
                if self.rest().starts_with('=') && !self.rest().starts_with("==") {
                    self.pos += 1;
                    p.assign = true;
                    ok = true;
                    break;
                }
                if self.b.get(self.pos) == Some(&b',') && names.len() == 1 {
                    self.pos += 1;
                    self.skip_space();
                    if self.b.get(self.pos) == Some(&b'$') {
                        continue;
                    }
                }
                break;
            }
            if ok {
                if p.assign {
                    for n in &names {
                        if !self.has_var(n) {
                            return self.fail(format!("undefined variable {n}"));
                        }
                    }
                }
                p.vars = names;
            } else {
                self.pos = save;
            }
        }
        loop {
            self.skip_space();
            let mut cmd = Vec::new();
            loop {
                self.skip_space();
                if self.at_action_end() || self.pos >= self.b.len() {
                    break;
                }
                let c = self.b[self.pos];
                if c == b'|' || c == b')' {
                    break;
                }
                cmd.push(self.operand()?);
            }
            if cmd.is_empty() {
                return self.fail("missing value for command");
            }
            p.cmds.push(cmd);
            self.skip_space();
            if self.b.get(self.pos) == Some(&b'|') {
                self.pos += 1;
                continue;
            }
            break;
        }
        if !p.assign {
            self.vars.extend(p.vars.iter().cloned());
        }
        Ok(p)
    }

    fn chain(&mut self) -> Vec<String> {
        let mut fields = Vec::new();
        while self.b.get(self.pos) == Some(&b'.') && self.b.get(self.pos + 1).is_some_and(|c| tmpl_ident(*c)) {
            self.pos += 1;
            let start = self.pos;
            while self.pos < self.b.len() && tmpl_ident(self.b[self.pos]) {
                self.pos += 1;
            }
            fields.push(self.src[start..self.pos].to_string());
        }
        fields
    }

    fn operand(&mut self) -> TR<TArg> {
        let c = self.b[self.pos];
        if c == b'.' {
            if self.b.get(self.pos + 1).is_some_and(|c| tmpl_ident(*c)) {
                return Ok(TArg::Field(self.chain()));
            }
            self.pos += 1;
            return Ok(TArg::Dot);
        }
        if c == b'$' {
            let start = self.pos;
            self.pos += 1;
            while self.pos < self.b.len() && tmpl_ident(self.b[self.pos]) {
                self.pos += 1;
            }
            let name = self.src[start..self.pos].to_string();
            if !self.has_var(&name) {
                return self.fail(format!("undefined variable {name}"));
            }
            let fields = self.chain();
            return Ok(TArg::Var(name, fields));
        }
        if c == b'"' {
            self.quoted()?;
            return Ok(TArg::Lit);
        }
        if c == b'`' {
            self.string_lit()?;
            return Ok(TArg::Lit);
        }
        if c == b'\'' {
            let Some(close) = self.src[self.pos + 1..].find('\'').map(|i| i + self.pos + 1) else {
                return self.fail("unterminated character constant");
            };
            let body = &self.src[self.pos + 1..close];
            self.pos = close + 1;
            if body.chars().count() != 1 {
                return self.fail("invalid character constant");
            }
            return Ok(TArg::Lit);
        }
        if c == b'(' {
            self.pos += 1;
            let p = self.pipeline(false)?;
            self.skip_space();
            if self.b.get(self.pos) != Some(&b')') {
                return self.fail("unclosed left paren");
            }
            self.pos += 1;
            let fields = self.chain();
            return Ok(TArg::Pipe(Box::new(p), fields));
        }
        if c.is_ascii_digit() || c == b'-' || c == b'+' {
            let start = self.pos;
            self.pos += 1;
            while self.pos < self.b.len() {
                let d = self.b[self.pos];
                let exp_sign = (d == b'-' || d == b'+') && matches!(self.b[self.pos - 1], b'e' | b'E');
                if tmpl_ident(d) || d == b'.' || exp_sign {
                    self.pos += 1;
                } else {
                    break;
                }
            }
            let text = &self.src[start..self.pos];
            let body = text.trim_start_matches(['+', '-']);
            let valid = if text.contains('.') || ((text.contains('e') || text.contains('E')) && !body.starts_with("0x")) {
                text.parse::<f64>().is_ok()
            } else if let Some(h) = body.strip_prefix("0x") {
                h.bytes().all(|c| c.is_ascii_hexdigit())
            } else {
                text.trim_start_matches('+').parse::<i64>().is_ok()
            };
            if !valid {
                return self.fail(format!("bad number syntax: {}", json_quote(text)));
            }
            return Ok(TArg::Lit);
        }
        if tmpl_ident(c) {
            let w = self.peek_word().to_string();
            self.pos += w.len();
            if w == "true" || w == "false" || w == "nil" {
                return Ok(TArg::Lit);
            }
            if !TMPL_FUNCS.contains(&w.as_str()) && !self.custom {
                return self.fail(format!("function {} not defined", json_quote(&w)));
            }
            return Ok(TArg::Func(w));
        }
        let ch = self.rest().chars().next().unwrap();
        self.fail(format!("unexpected {} in operand", json_quote(&ch.to_string())))
    }
}

/// A string as std.encoding.json.Quote writes it (for identical messages).
fn json_quote(s: &str) -> String {
    let mut out = String::from("\"");
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\t' => out.push_str("\\t"),
            '\r' => out.push_str("\\r"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

/// Field-path check of a template tree against the data type.
struct TmplChecker<'x, 'a> {
    sema: &'x mut Sema<'a>,
    vars: Vec<(String, Option<TypeId>)>,
}

impl TmplChecker<'_, '_> {
    fn fail<T>(&self, line: usize, msg: String) -> TR<T> {
        Err(format!("template: template:{line}: {msg}"))
    }

    fn list(&mut self, nodes: &[TNode], dot: Option<TypeId>) -> TR<()> {
        for n in nodes {
            match n {
                TNode::Action(p) => {
                    self.pipe(p, dot, true)?;
                }
                TNode::Cond { with, pipe, list, else_list } => {
                    let mark = self.vars.len();
                    let t = self.pipe(pipe, dot, true)?;
                    self.list(list, if *with { t } else { dot })?;
                    self.list(else_list, dot)?;
                    self.vars.truncate(mark);
                }
                TNode::Range { pipe, list, else_list } => {
                    let mark = self.vars.len();
                    let t = self.pipe(pipe, dot, false)?;
                    let elem = self.elem(t, pipe.line)?;
                    let key = self.key(t);
                    match pipe.vars.len() {
                        1 => self.vars.push((pipe.vars[0].clone(), elem)),
                        2 => {
                            self.vars.push((pipe.vars[0].clone(), key));
                            self.vars.push((pipe.vars[1].clone(), elem));
                        }
                        _ => {}
                    }
                    self.list(list, elem)?;
                    self.vars.truncate(mark);
                    self.list(else_list, dot)?;
                }
                TNode::Call(p) => {
                    if let Some(p) = p {
                        self.pipe(p, dot, true)?;
                    }
                }
            }
        }
        Ok(())
    }

    fn pipe(&mut self, p: &TPipe, dot: Option<TypeId>, declare: bool) -> TR<Option<TypeId>> {
        let mut t = None;
        for cmd in &p.cmds {
            t = self.command(cmd, dot, p.line)?;
        }
        if declare && !p.vars.is_empty() && !p.assign {
            self.vars.push((p.vars[0].clone(), t));
        }
        Ok(t)
    }

    fn command(&mut self, cmd: &[TArg], dot: Option<TypeId>, line: usize) -> TR<Option<TypeId>> {
        if let TArg::Func(name) = &cmd[0] {
            let mut tys = Vec::new();
            for a in &cmd[1..] {
                tys.push(self.arg(a, dot, line)?);
            }
            if name == "index" && !tys.is_empty() {
                let mut t = tys[0];
                for _ in 1..tys.len() {
                    t = t.and_then(|t| self.elem_of(t));
                }
                return Ok(t);
            }
            return Ok(None);
        }
        self.arg(&cmd[0], dot, line)
    }

    fn arg(&mut self, a: &TArg, dot: Option<TypeId>, line: usize) -> TR<Option<TypeId>> {
        let (mut t, fields) = match a {
            TArg::Field(f) => (dot, f.as_slice()),
            TArg::Var(name, f) => (self.vars.iter().rev().find(|(n, _)| n == name).and_then(|(_, t)| *t), f.as_slice()),
            TArg::Dot => return Ok(dot),
            TArg::Pipe(p, f) => (self.pipe(p, dot, false)?, f.as_slice()),
            TArg::Func(_) | TArg::Lit => return Ok(None),
        };
        for f in fields {
            t = self.field(t, f, line)?;
        }
        Ok(t)
    }

    fn deref(&self, mut t: TypeId) -> TypeId {
        while let Type::Pointer(p) = self.sema.tt.get(t) {
            t = *p;
        }
        t
    }

    /// The type name as the reflection table spells it (for messages).
    fn name(&self, t: TypeId) -> String {
        self.sema.tn(t)
    }

    fn is_template_value(&self, t: TypeId) -> bool {
        matches!(self.sema.tt.get(t), Type::Struct(d) if self.sema.structs[d.0 as usize].full_name == "std.text.template.Value")
    }

    fn field(&mut self, t: Option<TypeId>, name: &str, line: usize) -> TR<Option<TypeId>> {
        let Some(t0) = t else { return Ok(None) };
        let t = self.deref(t0);
        if self.is_template_value(t) {
            return Ok(None);
        }
        match self.sema.tt.get(t).clone() {
            Type::Any | Type::Interface(_) | Type::Error => Ok(None),
            Type::Struct(d) => match self.sema.structs[d.0 as usize].fields.iter().find(|f| f.name == name) {
                Some(f) => Ok(Some(f.ty)),
                None => self.fail(line, format!("can't evaluate field {name} in type {}", self.name(t))),
            },
            Type::Map(k, v) => {
                if matches!(self.sema.tt.get(k), Type::String) {
                    Ok(Some(v))
                } else {
                    self.fail(line, format!("can't evaluate field {name} in type {} (map keys are not strings)", self.name(t)))
                }
            }
            _ => self.fail(line, format!("can't evaluate field {name} in type {}", self.name(t))),
        }
    }

    fn elem_of(&self, t: TypeId) -> Option<TypeId> {
        match self.sema.tt.get(self.deref(t)) {
            Type::Slice(e) | Type::Array(e, _) => Some(*e),
            Type::Map(_, v) => Some(*v),
            _ => None,
        }
    }

    fn key(&self, t: Option<TypeId>) -> Option<TypeId> {
        match self.sema.tt.get(self.deref(t?)) {
            Type::Map(k, _) => Some(*k),
            _ => None,
        }
    }

    fn elem(&mut self, t: Option<TypeId>, line: usize) -> TR<Option<TypeId>> {
        let Some(t0) = t else { return Ok(None) };
        let t = self.deref(t0);
        if self.is_template_value(t) {
            return Ok(None);
        }
        match self.sema.tt.get(t).clone() {
            Type::Any | Type::Interface(_) => Ok(None),
            Type::Slice(e) | Type::Array(e, _) => Ok(Some(e)),
            Type::Map(_, v) => Ok(Some(v)),
            Type::Int(_) => Ok(Some(t)),
            _ => self.fail(line, format!("range can't iterate over {}", self.name(t))),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::check_regex;

    fn err(p: &str) -> String {
        let e = check_regex(p).unwrap_err();
        format!("{} at offset {}", e.msg, e.offset)
    }

    #[test]
    fn valid_patterns() {
        for p in [r"a(b+)c", r"(?P<y>\d{4})-(?<m>\d\d)", r"[[:alpha:]\p{Greek}\x{263a}-\x{263b}]", r"(?i)k(?-i:x)", r"\Qa.b\E+", r"a{,2}", r"x{}", r"[]a]", r"\pL\PN\p{^Lu}", r"(?s:.)", r"\012", ""] {
            assert!(check_regex(p).is_ok(), "{p}");
        }
    }

    #[test]
    fn errors_match_the_runtime_parser() {
        assert_eq!(err("(abc"), "missing closing ) at offset 0");
        assert_eq!(err("ab)"), "unexpected ) at offset 2");
        assert_eq!(err("[a"), "missing closing ] at offset 0");
        assert_eq!(err("a**"), "invalid nested repetition operator a** at offset 2");
        assert_eq!(err("*a"), "missing argument to repetition operator * at offset 0");
        assert_eq!(err("x{1001}"), "invalid repeat count {1001} at offset 1");
        assert_eq!(err(r"(a)\1"), r"backreferences are not supported (RE2 syntax): \1 at offset 3");
        assert_eq!(err("(?=a)"), "lookaround is not supported (RE2 syntax): (?= at offset 0");
        assert_eq!(err(r"\p{Foo}"), r"unknown Unicode class \p{Foo} at offset 0");
        assert_eq!(err("[z-a]"), "invalid character class range z-a at offset 1");
        assert_eq!(err(r"\q"), r"invalid escape sequence \q at offset 0");
        assert_eq!(err("(?x)"), "invalid or unsupported Perl syntax (?x at offset 0");
    }
}
