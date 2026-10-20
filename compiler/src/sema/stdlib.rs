//! The standard library surface known to the compiler.
//!
//! Every ordinary stdlib function is an *extern*: a C runtime function
//! named `slang_<ns>_<snake_name>` called with the Slang ABI types
//! (`string`, `[]T`, `error`, `T!`, `Task<T>` as defined by codegen/runtime).
//! A few functions need compiler support and are lowered specially
//! ([`Special`]).
//!
//! # Runtime symbols
//!
//! | Slang | C symbol | Signature |
//! |-------|----------|-----------|
//! | `std.io.ReadFile` | `slang_io_read_file` | `(string) string!` |
//! | `std.io.WriteFile` | `slang_io_write_file` | `(string, string) void!` |
//! | `std.io.ReadLine` | `slang_io_read_line` | `() string!` |
//! | `std.fmt.Format` | `slang_fmt_format` | `(string, []string) string` (args pre-formatted) |
//! | `std.strings.Split` | `slang_strings_split` | `(string, string) []string` |
//! | `std.strings.Join` | `slang_strings_join` | `([]string, string) string` |
//! | `std.strings.Trim` | `slang_strings_trim` | `(string) string` |
//! | `std.strings.Contains` | `slang_strings_contains` | `(string, string) bool` |
//! | `std.strings.Replace` | `slang_strings_replace` | `(string, string, string) string` |
//! | `std.strings.ToUpper` | `slang_strings_to_upper` | `(string) string` |
//! | `std.strings.ToLower` | `slang_strings_to_lower` | `(string) string` |
//! | `std.strings.HasPrefix` | `slang_strings_has_prefix` | `(string, string) bool` |
//! | `std.strings.HasSuffix` | `slang_strings_has_suffix` | `(string, string) bool` |
//! | `std.strings.Index` | `slang_strings_index` | `(string, string) int` |
//! | `std.strings.Repeat` | `slang_strings_repeat` | `(string, int) string` |
//! | `std.strings.ToInt` | `slang_strings_to_int` | `(string) int!` |
//! | `std.strings.ToFloat` | `slang_strings_to_float` | `(string) float!` |
//! | `std.math.Sqrt` | `slang_math_sqrt` | `(float) float` |
//! | `std.math.Abs` | `slang_math_abs` | `(float) float` |
//! | `std.math.Pow` | `slang_math_pow` | `(float, float) float` |
//! | `std.math.Floor` | `slang_math_floor` | `(float) float` |
//! | `std.math.Ceil` | `slang_math_ceil` | `(float) float` |
//! | `std.math.Min` | `slang_math_min` | `(float, float) float` |
//! | `std.math.Max` | `slang_math_max` | `(float, float) float` |
//! | `std.math.Sin` / `Cos` / `Tan` | `slang_math_sin` / `_cos` / `_tan` | `(float) float` |
//! | `std.math.Log` / `Exp` | `slang_math_log` / `_exp` | `(float) float` |
//! | `std.errors.Wrap` | `slang_errors_wrap` | `(error, string) error` |
//! | `std.errors.Unwrap` | `slang_errors_unwrap` | `(error) error` (nil if not a wrapper) |
//! | `std.errors.Is` | `slang_errors_is` | `(error, error) bool` |
//! | `std.errors.Trace` | `slang_errors_trace` | `(error) string` (the return trace, see DESIGN.md) |
//! | `std.errors.Frames` | `slang_errors_frames` | `(error) []errors.Frame` (special: struct result) |
//! | `std.task.Delay` | `slang_task_delay` | `(int) Task<void>` |
//!
//!
//! The second batch (`std.time`, `std.os`, `std.fs`, `std.random`,
//! `std.crypto.{hash,hmac,rand}`, `std.encoding.{base64,hex}`, `std.uuid`
//! and the additions to `std.strings`/`std.math`/`std.io`) follows the same
//! rule, with dots of nested namespaces turned into `_`
//! (`std.crypto.hash.Sha256Hex` -> `slang_crypto_hash_sha256_hex`); the
//! full list with C signatures is `runtime/ABI.md` section 14.3 and the
//! [`FUNCS`] table below. Int versions of the float `math.Min/Max/Abs` are
//! `IMin/IMax/IAbs` (no overloading).
//!
//! Constants ([`CONSTS`]): `std.math.Pi`, `std.math.E` (untyped float),
//! `std.math.MaxInt`/`MinInt`/`MaxInt32`/`MinInt32`/`MaxUint32`/`MaxUint`
//! (untyped int), all inlined, and the sentinel error `std.fs.ErrNotFound`
//! (runtime `slang_fs_err_not_found()`, wrapped by every "file not found"
//! error of `std.io`/`std.fs`), the `std.net` sentinels `ErrClosed`,
//! `ErrTimeout`, `ErrRefused`, `ErrReset`, `ErrUnreachable`, `ErrAddrInUse`,
//! `ErrNotFound`, `ErrEOF` (`slang_net_err_*()`, runtime/ABI.md 16.2); the
//! target constants `std.os.Name`,
//! `Arch`, `PathSeparator`, `LineSeparator` (`string`, folded at compile
//! time from [`crate::target::current`]).
//!
//! Specials (no extern; see `hir`):
//! `std.io.println` / `std.io.print` → [`crate::hir::ExprKind::Print`]
//! (runtime `slang_print_{int,uint,float,bool,rune,str,space,newline}`),
//! `std.fmt.Format` → extern `slang_fmt_format` with a `[]string` of
//! [`crate::hir::ExprKind::ToString`] arguments,
//! `std.task.All` → [`crate::hir::ExprKind::TaskAll`],
//! `std.task.Any` → [`crate::hir::Builtin::TaskAny`],
//! `std.errors.New` → [`crate::hir::Builtin::ErrorNew`],
//! `std.errors.As` → [`crate::hir::Builtin::ErrorsAs`].
//!
//! Slang-source stdlib (`std.task` types, `std.net`, `std.net.http`,
//! `std.crypto.x509`, `std.net.tls`; the C crypto primitives behind the
//! latter are the `std.crypto.*` rows below, `runtime/ABI.md` section 19):
//! [`super::stdsrc`]; their runtime primitives are the `rt*` externs below
//! (internal; `runtime/ABI.md` 10.10, 10.11 and 16).
//!
//! Builtins always in scope (no import): `error`, `len`, `append`,
//! `delete`, `panic`, conversions `int(x)`, `float(x)`, `string(c)` (and
//! the other numeric type names), `typeof(x)` (the `*reflect.TypeInfo` of
//! `x`, see [`super::reflect`]).
//!
//! `std.reflect`, `std.encoding.json` (apart from the externs `Quote` and
//! `scan` below) and `std.strings.Builder` are Slang sources loaded on first
//! use ([`super::reflect`]).

use crate::ast;
use crate::span::Span;
use crate::types::{TypeId, TypeTable};
use std::sync::OnceLock;

/// Synthesized declarations of [`STRUCTS`] (shared by every analysis).
fn std_struct_decls() -> &'static [ast::StructDecl] {
    static DECLS: OnceLock<Vec<ast::StructDecl>> = OnceLock::new();
    DECLS.get_or_init(|| {
        let ident = |n: &str| ast::Ident { name: n.to_string(), span: Span::DUMMY };
        STRUCTS
            .iter()
            .map(|(_, name, fields)| ast::StructDecl {
                attrs: Vec::new(),
                name: ident(name),
                generics: Vec::new(),
                fields: fields
                    .iter()
                    .map(|(f, t)| ast::Field {
                        attrs: Vec::new(),
                        name: ident(f),
                        ty: ast::TypeExpr {
                            kind: ast::TypeKind::Named { path: ast::Path { segments: vec![ident(t)], span: Span::DUMMY }, generics: Vec::new() },
                            span: Span::DUMMY,
                        },
                        default: None,
                        span: Span::DUMMY,
                    })
                    .collect(),
                embeds: Vec::new(),
                methods: Vec::new(),
                implements: Vec::new(),
                span: Span::DUMMY,
            })
            .collect()
    })
}

impl<'a> super::Sema<'a> {
    /// Registers the [`STRUCTS`] (after the user declarations, so user
    /// struct ids are unchanged). Needs at least one file.
    pub(crate) fn add_std_structs(&mut self) {
        if self.files.is_empty() {
            return;
        }
        for ((ns, name, _), decl) in STRUCTS.iter().zip(std_struct_decls()) {
            let id = self.structs.len() as u32;
            self.structs.push(crate::hir::Struct {
                name: name.to_string(),
                full_name: format!("{ns}.{name}"),
                namespace: crate::hir::NamespaceId(0),
                fields: Vec::new(),
                methods: Vec::new(),
                attrs: Vec::new(),
                is_attribute: false,
                exported: true,
                file: 0,
                span: Span::DUMMY,
            });
            self.struct_src.push(decl);
            let nsi = self.ns_index[*ns];
            self.nss[nsi].members.insert(name.to_string(), (super::Member::Struct(id), Span::DUMMY, 0));
        }
    }

    /// The id of builtin struct `ns.name` (see [`STRUCTS`]).
    pub(crate) fn std_struct(&self, ns: &str, name: &str) -> Option<u32> {
        match self.nss[*self.ns_index.get(ns)?].members.get(name)? {
            (super::Member::Struct(id), _, _) => Some(*id),
            _ => None,
        }
    }
}

/// Stdlib namespaces (all start with `std`).
/// Nested namespaces (`std.crypto.hash`) need no extra support: the
/// resolver creates every parent (`std.crypto`) as a namespace member of its
/// own parent, so `import std.crypto` gives `hash.Sha256Hex` and
/// `import std` gives `crypto.hash.Sha256Hex`.
pub const NAMESPACES: &[&str] = &[
    "std",
    "std.io",
    "std.fmt",
    "std.strings",
    "std.math",
    "std.errors",
    "std.task",
    "std.time",
    "std.os",
    "std.fs",
    "std.random",
    "std.crypto",
    "std.crypto.hash",
    "std.crypto.hmac",
    "std.crypto.rand",
    "std.encoding",
    "std.encoding.base64",
    "std.encoding.hex",
    "std.encoding.json",
    "std.reflect",
    "std.uuid",
    "std.net",
    "std.net.http",
    // TLS and the crypto primitives behind it (runtime/ABI.md section 19)
    "std.net.tls",
    "std.crypto.aes",
    "std.crypto.chacha",
    "std.crypto.x25519",
    "std.crypto.ecdh",
    "std.crypto.ecdsa",
    "std.crypto.ed25519",
    "std.crypto.rsa",
    "std.crypto.hkdf",
    "std.crypto.x509",
    "std.log",
    "std.diag",
    "std.arena",
    // >>> tier 2 stdlib (docs/STDLIB.md level 2)
    "std.regex",
    "std.compress",
    "std.compress.deflate",
    "std.compress.zlib",
    "std.compress.gzip",
    "std.compress.zstd",
    "std.compress.brotli",
    "std.archive",
    "std.archive.zip",
    "std.archive.tar",
    "std.encoding.csv",
    "std.encoding.xml",
    "std.encoding.yaml",
    "std.encoding.toml",
    "std.html",
    "std.html.template",
    "std.text",
    "std.text.template",
    "std.mime",
    "std.mime.multipart",
    "std.mime.quotedprintable",
    "std.net.textproto",
    "std.net.url",
    "std.text.scanner",
    "std.text.table",
    // <<< tier 2 stdlib
    // tier 0/1 stdlib (docs/STDLIB.md)
    "std.slices",
    "std.maps",
    "std.cmp",
    "std.bytes",
    "std.strconv",
    "std.unicode",
    "std.unicode.utf8",
    "std.unicode.utf16",
    "std.math.bits",
    "std.math.complex",
    "std.math.big",
    "std.hash",
    "std.hash.fnv",
    "std.hash.crc32",
    "std.hash.crc64",
    "std.hash.adler32",
    "std.hash.maphash",
    "std.encoding.binary",
    "std.encoding.base32",
    "std.encoding.pem",
    "std.encoding.asn1",
    "std.sync",
    "std.os.signal",
    "std.process",
    "std.os.user",
    "std.cli",
    "std.testing",
    "std.embed",
];

/// An ordinary stdlib function implemented by the runtime.
#[derive(Debug, Clone, Copy)]
pub struct StdFunc {
    /// Namespace path, e.g. `std.strings`.
    pub ns: &'static str,
    /// Slang name, e.g. `Split`.
    pub name: &'static str,
    /// Comma-separated parameter types in Slang syntax.
    pub params: &'static str,
    /// Result type in Slang syntax.
    pub ret: &'static str,
    /// C runtime symbol.
    pub symbol: &'static str,
}

/// Stdlib functions that the compiler lowers itself.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Special {
    /// `std.io.println(args ...any)`.
    Println,
    /// `std.io.print(args ...any)`: like `println` without the newline.
    Print,
    /// `std.fmt.Format(template string, args ...any) string`.
    Format,
    /// `std.fmt.Sprintf(format string, args ...any) string` (Go verbs):
    /// extern [`SPRINTF_SYMBOL`] with per-argument kinds and values.
    Sprintf,
    /// `std.task.All(tasks ...)`.
    TaskAll,
    /// `std.task.Any(tasks ...)` → [`crate::hir::Builtin::TaskAny`].
    TaskAny,
    /// `std.errors.New(msg string) error`.
    ErrorsNew,
    /// `std.errors.As(err error, target *T) bool`.
    ErrorsAs,
    /// `std.errors.Frames(err error) []errors.Frame` (extern
    /// `slang_errors_frames`; the result type names a [`STRUCTS`] entry).
    ErrorsFrames,
    /// `std.arena.rtArenaNew<T>(h int) *T` (internal, used by
    /// `Arena.New<T>`) → [`crate::hir::Builtin::ArenaNew`].
    ArenaNew,
    /// `std.arena.rtArenaAlloc<T>(h int, n int) []T` (internal, used by
    /// `Arena.Alloc<T>`) → [`crate::hir::Builtin::ArenaSlice`].
    ArenaAlloc,
}

/// Special functions by namespace and name.
pub const SPECIALS: &[(&str, &str, Special)] = &[
    ("std.io", "println", Special::Println),
    ("std.io", "print", Special::Print),
    ("std.fmt", "Format", Special::Format),
    ("std.fmt", "Sprintf", Special::Sprintf),
    ("std.task", "All", Special::TaskAll),
    ("std.task", "Any", Special::TaskAny),
    ("std.errors", "New", Special::ErrorsNew),
    ("std.errors", "As", Special::ErrorsAs),
    ("std.errors", "Frames", Special::ErrorsFrames),
    ("std.arena", "rtArenaNew", Special::ArenaNew),
    ("std.arena", "rtArenaAlloc", Special::ArenaAlloc),
];

/// Builtin stdlib struct types: (namespace, name, fields as (name, type)).
/// They become ordinary [`crate::hir::Struct`]s (after the user structs)
/// whose C layout the runtime shares.
pub const STRUCTS: &[(&str, &str, &[(&str, &str)])] = &[
    // One error-trace record (runtime `frame_rec` in error.c, 32 bytes).
    ("std.errors", "Frame", &[("File", "string"), ("Line", "int"), ("Func", "string"), ("Context", "string")]),
];

/// Symbol used for the lowered `fmt.Format`.
pub const FORMAT_SYMBOL: &str = "slang_fmt_format";

/// Symbol of the lowered `fmt.Sprintf`: `(format string, kinds string,
/// ints []int, floats []float, texts []string) string`, see
/// `runtime/src/std_fmt_sprintf.c`.
pub const SPRINTF_SYMBOL: &str = "slang_fmt_sprintf";

/// Value of a stdlib constant ([`CONSTS`]).
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum StdConst {
    /// Untyped float constant (inlined).
    Float(f64),
    /// Untyped integer constant (inlined).
    Int(i128),
    /// Sentinel error: a zero-argument runtime function returning the
    /// immortal error object (so `errors.Is` compares identity).
    Error(&'static str),
    /// `string` constant describing the compilation target
    /// ([`crate::target::current`]): `os.Name`, `os.Arch`,
    /// `os.PathSeparator`, `os.LineSeparator`.
    Target(TargetConst),
}

/// Target-dependent `std.os` string constants.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum TargetConst {
    Name,
    Arch,
    PathSeparator,
    LineSeparator,
    /// A fixed string constant (not target dependent): `time.RFC3339`.
    Str(&'static str),
}

impl TargetConst {
    /// The value for the current target.
    pub fn value(self) -> String {
        let t = crate::target::current();
        match self {
            TargetConst::Name => t.os.clone(),
            TargetConst::Arch => t.arch.clone(),
            TargetConst::PathSeparator => t.path_separator().to_string(),
            TargetConst::LineSeparator => t.line_separator().to_string(),
            TargetConst::Str(v) => v.to_string(),
        }
    }
}

impl std::fmt::Display for StdConst {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            StdConst::Float(v) => write!(f, "{v} (untyped float)"),
            StdConst::Int(v) => write!(f, "{v} (untyped int)"),
            StdConst::Error(_) => write!(f, "error (sentinel)"),
            StdConst::Target(t) => write!(f, "{:?} (string)", t.value()),
        }
    }
}

/// Stdlib constants: (namespace, name, value).
pub const CONSTS: &[(&str, &str, StdConst)] = &[
    ("std.math", "Pi", StdConst::Float(std::f64::consts::PI)),
    ("std.math", "E", StdConst::Float(std::f64::consts::E)),
    ("std.math", "MaxInt", StdConst::Int(i64::MAX as i128)),
    ("std.math", "MinInt", StdConst::Int(i64::MIN as i128)),
    ("std.math", "MaxInt32", StdConst::Int(i32::MAX as i128)),
    ("std.math", "MinInt32", StdConst::Int(i32::MIN as i128)),
    ("std.math", "MaxUint32", StdConst::Int(u32::MAX as i128)),
    ("std.math", "MaxUint", StdConst::Int(u64::MAX as i128)),
    ("std.fs", "ErrNotFound", StdConst::Error("slang_fs_err_not_found")),
    ("std.task", "ErrCancelled", StdConst::Error("slang_task_err_cancelled")),
    // std.net sentinels (runtime/src/std_net.c); every net error wraps one
    // of them when its cause is of that kind
    ("std.net", "ErrClosed", StdConst::Error("slang_net_err_closed")),
    ("std.net", "ErrTimeout", StdConst::Error("slang_net_err_timeout")),
    ("std.net", "ErrRefused", StdConst::Error("slang_net_err_refused")),
    ("std.net", "ErrReset", StdConst::Error("slang_net_err_reset")),
    ("std.net", "ErrUnreachable", StdConst::Error("slang_net_err_unreachable")),
    ("std.net", "ErrAddrInUse", StdConst::Error("slang_net_err_addr_in_use")),
    ("std.net", "ErrNotFound", StdConst::Error("slang_net_err_not_found")),
    ("std.net", "ErrEOF", StdConst::Error("slang_net_err_eof")),
    // std.net.tls: protocol versions and ServerConfig.ClientAuth modes
    ("std.net.tls", "V12", StdConst::Int(0x0303)),
    ("std.net.tls", "V13", StdConst::Int(0x0304)),
    ("std.net.tls", "NoClientCert", StdConst::Int(0)),
    ("std.net.tls", "RequestClientCert", StdConst::Int(1)),
    ("std.net.tls", "RequireAnyClientCert", StdConst::Int(2)),
    ("std.net.tls", "VerifyClientCertIfGiven", StdConst::Int(3)),
    ("std.net.tls", "RequireAndVerifyClientCert", StdConst::Int(4)),
    // tier 0/1 stdlib (docs/STDLIB.md)
    ("std.strconv", "ErrSyntax", StdConst::Error("slang_strconv_err_syntax")),
    ("std.strconv", "ErrRange", StdConst::Error("slang_strconv_err_range")),
    ("std.unicode", "MaxRune", StdConst::Int(0x10FFFF)),
    ("std.unicode", "ReplacementChar", StdConst::Int(0xFFFD)),
    ("std.unicode.utf8", "RuneError", StdConst::Int(0xFFFD)),
    ("std.unicode.utf8", "UTFMax", StdConst::Int(4)),
    ("std.unicode.utf8", "RuneSelf", StdConst::Int(0x80)),
    ("std.math.bits", "UintSize", StdConst::Int(64)),
    ("std.hash.crc32", "IEEE", StdConst::Int(0xEDB88320)),
    ("std.hash.crc32", "Castagnoli", StdConst::Int(0x82F63B78)),
    ("std.hash.crc32", "Koopman", StdConst::Int(0xEB31D82E)),
    ("std.hash.crc64", "ISO", StdConst::Int(0xD800000000000000)),
    ("std.hash.crc64", "ECMA", StdConst::Int(0xC96C5795D7870F42)),
    ("std.encoding.binary", "MaxVarintLen16", StdConst::Int(3)),
    ("std.encoding.binary", "MaxVarintLen32", StdConst::Int(5)),
    ("std.encoding.binary", "MaxVarintLen64", StdConst::Int(10)),
    // std.time layouts (Go reference layouts)
    ("std.time", "Layout", StdConst::Target(TargetConst::Str("01/02 03:04:05PM '06 -0700"))),
    ("std.time", "ANSIC", StdConst::Target(TargetConst::Str("Mon Jan _2 15:04:05 2006"))),
    ("std.time", "UnixDate", StdConst::Target(TargetConst::Str("Mon Jan _2 15:04:05 MST 2006"))),
    ("std.time", "RubyDate", StdConst::Target(TargetConst::Str("Mon Jan 02 15:04:05 -0700 2006"))),
    ("std.time", "RFC822", StdConst::Target(TargetConst::Str("02 Jan 06 15:04 MST"))),
    ("std.time", "RFC822Z", StdConst::Target(TargetConst::Str("02 Jan 06 15:04 -0700"))),
    ("std.time", "RFC850", StdConst::Target(TargetConst::Str("Monday, 02-Jan-06 15:04:05 MST"))),
    ("std.time", "RFC1123", StdConst::Target(TargetConst::Str("Mon, 02 Jan 2006 15:04:05 MST"))),
    ("std.time", "RFC1123Z", StdConst::Target(TargetConst::Str("Mon, 02 Jan 2006 15:04:05 -0700"))),
    ("std.time", "RFC3339", StdConst::Target(TargetConst::Str("2006-01-02T15:04:05Z07:00"))),
    ("std.time", "RFC3339Nano", StdConst::Target(TargetConst::Str("2006-01-02T15:04:05.999999999Z07:00"))),
    ("std.time", "Kitchen", StdConst::Target(TargetConst::Str("3:04PM"))),
    ("std.time", "Stamp", StdConst::Target(TargetConst::Str("Jan _2 15:04:05"))),
    ("std.time", "StampMilli", StdConst::Target(TargetConst::Str("Jan _2 15:04:05.000"))),
    ("std.time", "StampMicro", StdConst::Target(TargetConst::Str("Jan _2 15:04:05.000000"))),
    ("std.time", "StampNano", StdConst::Target(TargetConst::Str("Jan _2 15:04:05.000000000"))),
    ("std.time", "DateTime", StdConst::Target(TargetConst::Str("2006-01-02 15:04:05"))),
    ("std.time", "DateOnly", StdConst::Target(TargetConst::Str("2006-01-02"))),
    ("std.time", "TimeOnly", StdConst::Target(TargetConst::Str("15:04:05"))),
    // >>> tier 2 stdlib constants
    ("std.compress", "NoCompression", StdConst::Int(0)),
    ("std.compress", "BestSpeed", StdConst::Int(1)),
    ("std.compress", "BestCompression", StdConst::Int(9)),
    ("std.compress", "DefaultCompression", StdConst::Int(-1)),
    ("std.archive.zip", "Store", StdConst::Int(0)),
    ("std.archive.zip", "Deflate", StdConst::Int(8)),
    ("std.archive.tar", "TypeReg", StdConst::Int(48)),
    ("std.archive.tar", "TypeLink", StdConst::Int(49)),
    ("std.archive.tar", "TypeSymlink", StdConst::Int(50)),
    ("std.archive.tar", "TypeDir", StdConst::Int(53)),
    ("std.text.scanner", "EOF", StdConst::Int(-1)),
    ("std.text.scanner", "Ident", StdConst::Int(-2)),
    ("std.text.scanner", "Int", StdConst::Int(-3)),
    ("std.text.scanner", "Float", StdConst::Int(-4)),
    ("std.text.scanner", "Char", StdConst::Int(-5)),
    ("std.text.scanner", "String", StdConst::Int(-6)),
    ("std.text.scanner", "RawString", StdConst::Int(-7)),
    ("std.text.scanner", "Comment", StdConst::Int(-8)),
    ("std.text.scanner", "ScanIdents", StdConst::Int(4)),
    ("std.text.scanner", "ScanInts", StdConst::Int(8)),
    ("std.text.scanner", "ScanFloats", StdConst::Int(16)),
    ("std.text.scanner", "ScanChars", StdConst::Int(32)),
    ("std.text.scanner", "ScanStrings", StdConst::Int(64)),
    ("std.text.scanner", "ScanRawStrings", StdConst::Int(128)),
    ("std.text.scanner", "ScanComments", StdConst::Int(256)),
    ("std.text.scanner", "SkipComments", StdConst::Int(512)),
    ("std.text.scanner", "GoTokens", StdConst::Int(1012)),
    ("std.text.scanner", "GoWhitespace", StdConst::Int(4294977024)),
    ("std.text.table", "Plain", StdConst::Int(0)),
    ("std.text.table", "ASCII", StdConst::Int(1)),
    ("std.text.table", "Unicode", StdConst::Int(2)),
    ("std.text.table", "Markdown", StdConst::Int(3)),
    ("std.text.table", "Left", StdConst::Int(0)),
    ("std.text.table", "Right", StdConst::Int(1)),
    ("std.text.table", "Center", StdConst::Int(2)),
    // <<< tier 2 stdlib constants
    ("std.io", "EOF", StdConst::Error("slang_io_err_eof")),
    ("std.io", "SeekStart", StdConst::Int(0)),
    ("std.io", "SeekCurrent", StdConst::Int(1)),
    ("std.io", "SeekEnd", StdConst::Int(2)),
    ("std.os", "O_RDONLY", StdConst::Int(0)),
    ("std.os", "O_WRONLY", StdConst::Int(1)),
    ("std.os", "O_RDWR", StdConst::Int(2)),
    ("std.os", "O_APPEND", StdConst::Int(8)),
    ("std.os", "O_CREATE", StdConst::Int(64)),
    ("std.os", "O_EXCL", StdConst::Int(128)),
    ("std.os", "O_SYNC", StdConst::Int(256)),
    ("std.os", "O_TRUNC", StdConst::Int(512)),
    ("std.encoding.asn1", "TagBoolean", StdConst::Int(1)),
    ("std.encoding.asn1", "TagInteger", StdConst::Int(2)),
    ("std.encoding.asn1", "TagBitString", StdConst::Int(3)),
    ("std.encoding.asn1", "TagOctetString", StdConst::Int(4)),
    ("std.encoding.asn1", "TagNull", StdConst::Int(5)),
    ("std.encoding.asn1", "TagOID", StdConst::Int(6)),
    ("std.encoding.asn1", "TagEnum", StdConst::Int(10)),
    ("std.encoding.asn1", "TagUTF8String", StdConst::Int(12)),
    ("std.encoding.asn1", "TagSequence", StdConst::Int(16)),
    ("std.encoding.asn1", "TagSet", StdConst::Int(17)),
    ("std.encoding.asn1", "TagNumericString", StdConst::Int(18)),
    ("std.encoding.asn1", "TagPrintableString", StdConst::Int(19)),
    ("std.encoding.asn1", "TagT61String", StdConst::Int(20)),
    ("std.encoding.asn1", "TagIA5String", StdConst::Int(22)),
    ("std.encoding.asn1", "TagUTCTime", StdConst::Int(23)),
    ("std.encoding.asn1", "TagGeneralizedTime", StdConst::Int(24)),
    ("std.encoding.asn1", "TagBMPString", StdConst::Int(30)),
    ("std.encoding.asn1", "ClassUniversal", StdConst::Int(0)),
    ("std.encoding.asn1", "ClassApplication", StdConst::Int(1)),
    ("std.encoding.asn1", "ClassContextSpecific", StdConst::Int(2)),
    ("std.encoding.asn1", "ClassPrivate", StdConst::Int(3)),
    // std.os.signal numbers (Linux values)
    ("std.os.signal", "SIGHUP", StdConst::Int(1)),
    ("std.os.signal", "SIGINT", StdConst::Int(2)),
    ("std.os.signal", "SIGQUIT", StdConst::Int(3)),
    ("std.os.signal", "SIGABRT", StdConst::Int(6)),
    ("std.os.signal", "SIGKILL", StdConst::Int(9)),
    ("std.os.signal", "SIGUSR1", StdConst::Int(10)),
    ("std.os.signal", "SIGUSR2", StdConst::Int(12)),
    ("std.os.signal", "SIGPIPE", StdConst::Int(13)),
    ("std.os.signal", "SIGALRM", StdConst::Int(14)),
    ("std.os.signal", "SIGTERM", StdConst::Int(15)),
    ("std.os.signal", "SIGCHLD", StdConst::Int(17)),
    ("std.os.signal", "SIGWINCH", StdConst::Int(28)),
    ("std.process", "Inherit", StdConst::Int(0)),
    ("std.process", "Pipe", StdConst::Int(1)),
    ("std.process", "Null", StdConst::Int(2)),
    ("std.os", "Name", StdConst::Target(TargetConst::Name)),
    ("std.os", "Arch", StdConst::Target(TargetConst::Arch)),
    ("std.os", "PathSeparator", StdConst::Target(TargetConst::PathSeparator)),
    ("std.os", "LineSeparator", StdConst::Target(TargetConst::LineSeparator)),
];

macro_rules! f {
    ($ns:literal, $name:literal, $params:literal, $ret:literal, $sym:literal) => {
        StdFunc { ns: $ns, name: $name, params: $params, ret: $ret, symbol: $sym }
    };
}

/// All extern stdlib functions.
pub const FUNCS: &[StdFunc] = &[
    f!("std.io", "ReadFile", "string", "string!", "slang_io_read_file"),
    f!("std.io", "WriteFile", "string, string", "void!", "slang_io_write_file"),
    f!("std.io", "ReadLine", "", "string!", "slang_io_read_line"),
    f!("std.strings", "Split", "string, string", "[]string", "slang_strings_split"),
    f!("std.strings", "Join", "[]string, string", "string", "slang_strings_join"),
    f!("std.strings", "Trim", "string", "string", "slang_strings_trim"),
    f!("std.strings", "Contains", "string, string", "bool", "slang_strings_contains"),
    f!("std.strings", "Replace", "string, string, string", "string", "slang_strings_replace"),
    f!("std.strings", "ToUpper", "string", "string", "slang_strings_to_upper"),
    f!("std.strings", "ToLower", "string", "string", "slang_strings_to_lower"),
    f!("std.strings", "HasPrefix", "string, string", "bool", "slang_strings_has_prefix"),
    f!("std.strings", "HasSuffix", "string, string", "bool", "slang_strings_has_suffix"),
    f!("std.strings", "Index", "string, string", "int", "slang_strings_index"),
    f!("std.strings", "Repeat", "string, int", "string", "slang_strings_repeat"),
    f!("std.strings", "ToInt", "string", "int!", "slang_strings_to_int"),
    f!("std.strings", "ToFloat", "string", "float!", "slang_strings_to_float"),
    f!("std.math", "Sqrt", "float", "float", "slang_math_sqrt"),
    f!("std.math", "Abs", "float", "float", "slang_math_abs"),
    f!("std.math", "Pow", "float, float", "float", "slang_math_pow"),
    f!("std.math", "Floor", "float", "float", "slang_math_floor"),
    f!("std.math", "Ceil", "float", "float", "slang_math_ceil"),
    f!("std.math", "Min", "float, float", "float", "slang_math_min"),
    f!("std.math", "Max", "float, float", "float", "slang_math_max"),
    f!("std.math", "Sin", "float", "float", "slang_math_sin"),
    f!("std.math", "Cos", "float", "float", "slang_math_cos"),
    f!("std.math", "Tan", "float", "float", "slang_math_tan"),
    f!("std.math", "Log", "float", "float", "slang_math_log"),
    f!("std.math", "Exp", "float", "float", "slang_math_exp"),
    f!("std.errors", "Wrap", "error, string", "error", "slang_errors_wrap"),
    f!("std.errors", "Unwrap", "error", "error", "slang_errors_unwrap"),
    f!("std.errors", "Is", "error, error", "bool", "slang_errors_is"),
    f!("std.errors", "Trace", "error", "string", "slang_errors_trace"),
    f!("std.arena", "rtArenaCreate", "", "int", "slang_arena_create"),
    f!("std.arena", "rtArenaDestroy", "int", "void", "slang_arena_destroy"),
    f!("std.arena", "rtArenaFree", "int", "void", "slang_arena_free"),
    f!("std.arena", "rtArenaString", "int, string", "string", "slang_arena_string"),
    f!("std.arena", "rtArenaLen", "int", "int", "slang_arena_objects"),
    f!("std.task", "Delay", "int", "Task<void>", "slang_task_delay"),
    f!("std.task", "Yield", "", "Task<void>", "slang_task_yield"),
    // internal primitives of the Slang-source std.task (sema/stdlib_src/task.sl)
    f!("std.task", "rtSignal", "", "Task<void>", "slang_task_rt_signal"),
    f!("std.task", "rtWaker", "", "Task<void>", "slang_task_rt_waker"),
    f!("std.task", "IsCancelled", "", "bool", "slang_task_cur_cancelled"),
    f!("std.task", "rtCancel", "Task<void>", "void", "slang_task_cancel"),
    f!("std.task", "rtScopeCancelled", "Task<void>", "bool", "slang_task_is_cancelled"),
    f!("std.task", "rtSetDeadline", "Task<void>, int", "void", "slang_task_set_deadline"),
    f!("std.task", "rtCurrent", "", "Task<void>", "slang_task_current"),
    f!("std.task", "rtNewScope", "", "Task<void>", "slang_task_new_scope"),
    f!("std.task", "rtEnterScope", "Task<void>", "Task<void>", "slang_task_enter_scope"),
    f!("std.task", "rtLeaveScope", "Task<void>", "void", "slang_task_leave_scope"),
    f!("std.task", "rtNextKey", "", "int", "slang_task_next_key"),
    f!("std.task", "rtFire", "Task<void>", "void", "slang_task_rt_fire"),
    f!("std.task", "rtLock", "*int", "void", "slang_task_spin_lock"),
    f!("std.task", "rtUnlock", "*int", "void", "slang_task_spin_unlock"),
    f!("std.task", "rtGetCtx", "", "map[string]any", "slang_task_get_ctx"),
    f!("std.task", "rtSetCtx", "map[string]any", "void", "slang_task_set_ctx"),
    // std.strings (more)
    f!("std.strings", "Fields", "string", "[]string", "slang_strings_fields"),
    f!("std.strings", "Count", "string, string", "int", "slang_strings_count"),
    f!("std.strings", "LastIndex", "string, string", "int", "slang_strings_last_index"),
    f!("std.strings", "TrimPrefix", "string, string", "string", "slang_strings_trim_prefix"),
    f!("std.strings", "TrimSuffix", "string, string", "string", "slang_strings_trim_suffix"),
    f!("std.strings", "TrimLeft", "string, string", "string", "slang_strings_trim_left"),
    f!("std.strings", "TrimRight", "string, string", "string", "slang_strings_trim_right"),
    f!("std.strings", "SplitN", "string, string, int", "[]string", "slang_strings_split_n"),
    f!("std.strings", "EqualFold", "string, string", "bool", "slang_strings_equal_fold"),
    f!("std.strings", "Reverse", "string", "string", "slang_strings_reverse"),
    f!("std.strings", "PadLeft", "string, int, string", "string", "slang_strings_pad_left"),
    f!("std.strings", "PadRight", "string, int, string", "string", "slang_strings_pad_right"),
    f!("std.strings", "Runes", "string", "[]char", "slang_strings_runes"),
    f!("std.strings", "FromRunes", "[]char", "string", "slang_strings_from_runes"),
    f!("std.strings", "Bytes", "string", "[]byte", "slang_strings_bytes"),
    f!("std.strings", "FromBytes", "[]byte", "string", "slang_strings_from_bytes"),
    f!("std.strings", "Compare", "string, string", "int", "slang_strings_compare"),
    f!("std.strings", "Title", "string", "string", "slang_strings_title"),
    // std.math (more); int helpers are I-prefixed (no overloading)
    f!("std.math", "Round", "float", "float", "slang_math_round"),
    f!("std.math", "Trunc", "float", "float", "slang_math_trunc"),
    f!("std.math", "Mod", "float, float", "float", "slang_math_mod"),
    f!("std.math", "Hypot", "float, float", "float", "slang_math_hypot"),
    f!("std.math", "Atan2", "float, float", "float", "slang_math_atan2"),
    f!("std.math", "Log2", "float", "float", "slang_math_log2"),
    f!("std.math", "Log10", "float", "float", "slang_math_log10"),
    f!("std.math", "IsNaN", "float", "bool", "slang_math_is_nan"),
    f!("std.math", "IsInf", "float, int", "bool", "slang_math_is_inf"),
    f!("std.math", "Inf", "int", "float", "slang_math_inf"),
    f!("std.math", "NaN", "", "float", "slang_math_nan"),
    f!("std.math", "IMin", "int, int", "int", "slang_math_imin"),
    f!("std.math", "IMax", "int, int", "int", "slang_math_imax"),
    f!("std.math", "IAbs", "int", "int", "slang_math_iabs"),
    // std.io (more)
    f!("std.io", "ReadAll", "", "string!", "slang_io_read_all"),
    f!("std.io", "ReadLines", "string", "[]string!", "slang_io_read_lines"),
    f!("std.io", "WriteLines", "string, []string", "void!", "slang_io_write_lines"),
    f!("std.io", "Eprint", "string", "void", "slang_io_eprint"),
    f!("std.io", "Eprintln", "string", "void", "slang_io_eprintln"),
    // std.time: int Unix milliseconds, UTC calendar
    f!("std.time", "Now", "", "int", "slang_time_now"),
    f!("std.time", "NowNanos", "", "int", "slang_time_now_nanos"),
    f!("std.time", "Sleep", "int", "void", "slang_time_sleep"),
    f!("std.time", "Since", "int", "int", "slang_time_since"),
    f!("std.time", "Format", "int, string", "string", "slang_time_format"),
    f!("std.time", "Parse", "string, string", "int!", "slang_time_parse"),
    f!("std.time", "Year", "int", "int", "slang_time_year"),
    f!("std.time", "Month", "int", "int", "slang_time_month"),
    f!("std.time", "Day", "int", "int", "slang_time_day"),
    f!("std.time", "Hour", "int", "int", "slang_time_hour"),
    f!("std.time", "Minute", "int", "int", "slang_time_minute"),
    f!("std.time", "Second", "int", "int", "slang_time_second"),
    f!("std.time", "Weekday", "int", "int", "slang_time_weekday"),
    // std.os
    f!("std.os", "Args", "", "[]string", "slang_os_args"),
    f!("std.os", "Getenv", "string", "string", "slang_os_getenv"),
    f!("std.os", "Setenv", "string, string", "void!", "slang_os_setenv"),
    f!("std.os", "Exit", "int", "void", "slang_os_exit"),
    f!("std.os", "Hostname", "", "string!", "slang_os_hostname"),
    f!("std.os", "Pid", "", "int", "slang_os_pid"),
    f!("std.os", "Cwd", "", "string!", "slang_os_cwd"),
    f!("std.os", "Chdir", "string", "void!", "slang_os_chdir"),
    // std.fs
    f!("std.fs", "Exists", "string", "bool", "slang_fs_exists"),
    f!("std.fs", "IsDir", "string", "bool", "slang_fs_is_dir"),
    f!("std.fs", "ReadDir", "string", "[]string!", "slang_fs_read_dir"),
    f!("std.fs", "Mkdir", "string", "void!", "slang_fs_mkdir"),
    f!("std.fs", "MkdirAll", "string", "void!", "slang_fs_mkdir_all"),
    f!("std.fs", "Remove", "string", "void!", "slang_fs_remove"),
    f!("std.fs", "RemoveAll", "string", "void!", "slang_fs_remove_all"),
    f!("std.fs", "Rename", "string, string", "void!", "slang_fs_rename"),
    f!("std.fs", "Size", "string", "int!", "slang_fs_size"),
    f!("std.fs", "ModTime", "string", "int!", "slang_fs_mod_time"),
    f!("std.fs", "Copy", "string, string", "void!", "slang_fs_copy"),
    f!("std.fs", "Join", "string, string", "string", "slang_fs_join"),
    f!("std.fs", "Base", "string", "string", "slang_fs_base"),
    f!("std.fs", "Dir", "string", "string", "slang_fs_dir"),
    f!("std.fs", "Ext", "string", "string", "slang_fs_ext"),
    f!("std.fs", "Abs", "string", "string!", "slang_fs_abs"),
    f!("std.fs", "TempDir", "", "string", "slang_fs_temp_dir"),
    f!("std.fs", "AppendFile", "string, string", "void!", "slang_fs_append_file"),
    // std.random (non-cryptographic)
    f!("std.random", "Seed", "int", "void", "slang_random_seed"),
    f!("std.random", "Int", "", "int", "slang_random_int"),
    f!("std.random", "IntN", "int", "int", "slang_random_int_n"),
    f!("std.random", "Float", "", "float", "slang_random_float"),
    f!("std.random", "Bytes", "int", "[]byte", "slang_random_bytes"),
    f!("std.random", "Shuffle", "[]int", "void", "slang_random_shuffle"),
    // std.crypto.*
    f!("std.crypto.hash", "Sha256", "string", "[]byte", "slang_crypto_hash_sha256"),
    f!("std.crypto.hash", "Sha256Hex", "string", "string", "slang_crypto_hash_sha256_hex"),
    f!("std.crypto.hash", "Sha512", "string", "[]byte", "slang_crypto_hash_sha512"),
    f!("std.crypto.hash", "Sha512Hex", "string", "string", "slang_crypto_hash_sha512_hex"),
    f!("std.crypto.hash", "Sha1", "string", "[]byte", "slang_crypto_hash_sha1"),
    f!("std.crypto.hash", "Sha1Hex", "string", "string", "slang_crypto_hash_sha1_hex"),
    f!("std.crypto.hash", "Md5", "string", "[]byte", "slang_crypto_hash_md5"),
    f!("std.crypto.hash", "Md5Hex", "string", "string", "slang_crypto_hash_md5_hex"),
    f!("std.crypto.hash", "Crc32", "string", "int", "slang_crypto_hash_crc32"),
    f!("std.crypto.hmac", "Sha256", "string, string", "[]byte", "slang_crypto_hmac_sha256"),
    f!("std.crypto.hmac", "Sha256Hex", "string, string", "string", "slang_crypto_hmac_sha256_hex"),
    f!("std.crypto.hmac", "Equal", "[]byte, []byte", "bool", "slang_crypto_hmac_equal"),
    f!("std.crypto.rand", "Bytes", "int", "[]byte!", "slang_crypto_rand_bytes"),
    f!("std.crypto.rand", "Int", "", "int!", "slang_crypto_rand_int"),
    f!("std.crypto.rand", "Hex", "int", "string!", "slang_crypto_rand_hex"),
    // std.encoding.*
    f!("std.encoding.base64", "Encode", "[]byte", "string", "slang_encoding_base64_encode"),
    f!("std.encoding.base64", "EncodeString", "string", "string", "slang_encoding_base64_encode_string"),
    f!("std.encoding.base64", "Decode", "string", "[]byte!", "slang_encoding_base64_decode"),
    f!("std.encoding.base64", "DecodeString", "string", "string!", "slang_encoding_base64_decode_string"),
    f!("std.encoding.base64", "EncodeURL", "[]byte", "string", "slang_encoding_base64_encode_url"),
    f!("std.encoding.base64", "DecodeURL", "string", "[]byte!", "slang_encoding_base64_decode_url"),
    f!("std.encoding.hex", "Encode", "[]byte", "string", "slang_encoding_hex_encode"),
    f!("std.encoding.hex", "EncodeString", "string", "string", "slang_encoding_hex_encode_string"),
    f!("std.encoding.hex", "Decode", "string", "[]byte!", "slang_encoding_hex_decode"),
    f!("std.encoding.hex", "DecodeString", "string", "string!", "slang_encoding_hex_decode_string"),
    // std.encoding.json: the C half (std_json.c); the rest is Slang (sema/reflect_src/json.sl)
    f!("std.encoding.json", "Quote", "string", "string", "slang_encoding_json_quote"),
    f!("std.encoding.json", "scan", "string", "[]string!", "slang_encoding_json_scan"),
    // internal primitives of the Slang-source std.net (sema/stdlib_src/net.sl, net_udp.sl);
    // sockets are runtime handles (ABI.md 16)
    f!("std.net", "rtOpError", "error, string, error", "error", "slang_net_rt_op_error"),
    f!("std.net", "rtDial", "string, int, string, int", "int!", "slang_net_rt_dial"),
    f!("std.net", "rtDialUnix", "string", "int!", "slang_net_rt_dial_unix"),
    f!("std.net", "rtConnectResult", "int", "void!", "slang_net_rt_connect_result"),
    f!("std.net", "rtConnectCheck", "int", "int!", "slang_net_rt_connect_check"),
    f!("std.net", "rtWaitAnyWritable", "[]int, int", "Task<int>", "slang_net_rt_wait_any_writable"),
    f!("std.net", "rtListen", "string, int, int, int", "int!", "slang_net_rt_listen"),
    f!("std.net", "rtListenUnix", "string, int", "int!", "slang_net_rt_listen_unix"),
    f!("std.net", "rtAccept", "int", "int!", "slang_net_rt_accept"),
    f!("std.net", "rtRead", "int, int", "[]byte!", "slang_net_rt_read"),
    f!("std.net", "rtReadInto", "int, []byte, int", "int!", "slang_net_rt_read_into"),
    f!("std.net", "rtAgain", "", "bool", "slang_net_rt_again"),
    f!("std.net", "rtWrite", "int, []byte, int", "int!", "slang_net_rt_write"),
    f!("std.net", "rtWriteStr", "int, string, int", "int!", "slang_net_rt_write_str"),
    f!("std.net", "rtWriteV", "int, [][]byte, int", "int!", "slang_net_rt_write_v"),
    f!("std.net", "rtShutdown", "int, int", "void!", "slang_net_rt_shutdown"),
    f!("std.net", "rtClose", "int", "void", "slang_net_rt_close"),
    f!("std.net", "rtFree", "int", "void", "slang_net_rt_free"),
    f!("std.net", "rtIsClosed", "int", "bool", "slang_net_rt_is_closed"),
    f!("std.net", "rtFd", "int", "int", "slang_net_rt_fd"),
    f!("std.net", "rtWaitReadable", "int, int", "Task<int>", "slang_net_rt_wait_readable"),
    f!("std.net", "rtWaitWritable", "int, int", "Task<int>", "slang_net_rt_wait_writable"),
    f!("std.net", "rtLocalAddr", "int", "string", "slang_net_rt_local_addr"),
    f!("std.net", "rtRemoteAddr", "int", "string", "slang_net_rt_remote_addr"),
    f!("std.net", "rtPort", "int", "int", "slang_net_rt_port"),
    f!("std.net", "rtSetOpt", "int, int, int", "void!", "slang_net_rt_set_opt"),
    f!("std.net", "rtGetOpt", "int, int", "int!", "slang_net_rt_get_opt"),
    f!("std.net", "rtMcast", "int, bool, string, string", "void!", "slang_net_rt_mcast"),
    f!("std.net", "rtOpenFile", "string", "int!", "slang_net_rt_open_file"),
    f!("std.net", "rtFileSize", "int", "int", "slang_net_rt_file_size"),
    f!("std.net", "rtCloseFile", "int", "void", "slang_net_rt_close_file"),
    f!("std.net", "rtSendFile", "int, int, int, int", "int!", "slang_net_rt_send_file"),
    f!("std.net", "rtParseIP", "string", "[]byte", "slang_net_rt_parse_ip"),
    f!("std.net", "rtFormatIP", "[]byte", "string", "slang_net_rt_format_ip"),
    f!("std.net", "rtIsIP", "string", "bool", "slang_net_rt_is_ip"),
    f!("std.net", "rtIfIndex", "string", "int", "slang_net_rt_if_index"),
    f!("std.net", "rtIfName", "int", "string", "slang_net_rt_if_name"),
    f!("std.net", "rtInterfaces", "", "[]string!", "slang_net_rt_interfaces"),
    f!("std.net", "rtLookupPort", "string, string", "int", "slang_net_rt_lookup_port"),
    f!("std.net", "rtResolve", "string, int", "Task<int>", "slang_net_rt_resolve"),
    f!("std.net", "rtReverse", "string", "Task<int>", "slang_net_rt_reverse"),
    f!("std.net", "rtResolveTake", "int", "[]string!", "slang_net_rt_resolve_take"),
    f!("std.net", "rtResolveSync", "string, int", "[]string!", "slang_net_rt_resolve_sync"),
    f!("std.net", "rtUdp", "string, int, int", "int!", "slang_net_rt_udp"),
    f!("std.net", "rtUdpDial", "string, int, string, int", "int!", "slang_net_rt_udp_dial"),
    f!("std.net", "rtRecvFrom", "int, int", "[]byte!", "slang_net_rt_recv_from"),
    f!("std.net", "rtReadFrom", "int, []byte", "int!", "slang_net_rt_read_from"),
    f!("std.net", "rtLastFrom", "", "string", "slang_net_rt_last_from"),
    f!("std.net", "rtLastFromPort", "", "int", "slang_net_rt_last_from_port"),
    f!("std.net", "rtLastTrunc", "", "bool", "slang_net_rt_last_trunc"),
    f!("std.net", "rtSendTo", "int, []byte, string, int", "int!", "slang_net_rt_send_to"),
    f!("std.net", "rtBytes", "int", "[]byte", "slang_net_rt_bytes"),
    f!("std.net", "rtCopyBytes", "[]byte, int, []byte, int, int", "int", "slang_net_rt_copy_bytes"),
    f!("std.net", "rtIndexByte", "[]byte, int, int", "int", "slang_net_rt_index_byte"),
    f!("std.net", "rtJoin", "[]byte, []byte, int", "[]byte", "slang_net_rt_join"),
    // internal primitives of the Slang-source std.diag / std.log
    // (sema/stdlib_src/{diag,log}.sl; runtime diag.c, profile.c)
    f!("std.diag", "rtSlots", "", "map[string]any", "slang_diag_rt_slots"),
    f!("std.diag", "rtSetSlots", "map[string]any", "void", "slang_diag_rt_set_slots"),
    f!("std.diag", "rtLock", "", "void", "slang_diag_rt_lock"),
    f!("std.diag", "rtUnlock", "", "void", "slang_diag_rt_unlock"),
    f!("std.diag", "rtLiveObjects", "", "int", "slang_diag_rt_live_objects"),
    f!("std.diag", "rtAllocations", "", "int", "slang_diag_rt_allocations"),
    f!("std.diag", "rtLiveTasks", "", "int", "slang_diag_rt_live_tasks"),
    f!("std.diag", "rtQueueLen", "", "int", "slang_diag_rt_queue_len"),
    f!("std.diag", "rtWorkers", "", "int", "slang_diag_rt_workers"),
    f!("std.diag", "rtMonoNanos", "", "int", "slang_diag_rt_mono_nanos"),
    f!("std.diag", "rtStartCPUProfile", "string, int", "void!", "slang_diag_rt_start_cpu_profile"),
    f!("std.diag", "rtStopCPUProfile", "", "int!", "slang_diag_rt_stop_cpu_profile"),
    f!("std.diag", "rtHeapText", "", "string", "slang_diag_rt_heap_text"),
    f!("std.diag", "rtWriteHeapProfile", "string", "void!", "slang_diag_rt_write_heap_profile"),
    f!("std.log", "rtOpen", "string", "int!", "slang_log_rt_open"),
    f!("std.log", "rtWrite", "int, string", "void!", "slang_log_rt_write"),
    f!("std.log", "rtClose", "int", "void", "slang_log_rt_close"),
    f!("std.log", "rtIsatty", "int", "bool", "slang_log_rt_isatty"),
    // std.crypto primitives (runtime/src/crypto/, ABI.md section 19)
    f!("std.crypto.hash", "Sha384", "string", "[]byte", "slang_crypto_hash_sha384"),
    f!("std.crypto.hash", "Sha384Hex", "string", "string", "slang_crypto_hash_sha384_hex"),
    f!("std.crypto.hash", "Sum", "string, []byte", "[]byte!", "slang_crypto_hash_sum"),
    f!("std.crypto.hash", "rtNew", "string", "[]byte!", "slang_crypto_hash_rt_new"),
    f!("std.crypto.hash", "rtWrite", "[]byte, []byte", "void", "slang_crypto_hash_rt_write"),
    f!("std.crypto.hash", "rtSum", "[]byte", "[]byte", "slang_crypto_hash_rt_sum"),
    f!("std.crypto.hmac", "Sum", "string, []byte, []byte", "[]byte!", "slang_crypto_hmac_sum"),
    f!("std.crypto.hkdf", "Extract", "string, []byte, []byte", "[]byte!", "slang_crypto_hkdf_extract"),
    f!("std.crypto.hkdf", "Expand", "string, []byte, []byte, int", "[]byte!", "slang_crypto_hkdf_expand"),
    f!("std.crypto.aes", "GcmSeal", "[]byte, []byte, []byte, []byte", "[]byte!", "slang_crypto_aes_gcm_seal"),
    f!("std.crypto.aes", "GcmOpen", "[]byte, []byte, []byte, []byte", "[]byte!", "slang_crypto_aes_gcm_open"),
    f!("std.crypto.aes", "EncryptBlock", "[]byte, []byte", "[]byte!", "slang_crypto_aes_encrypt_block"),
    f!("std.crypto.aes", "HasHardware", "", "bool", "slang_crypto_aes_has_hardware"),
    f!("std.crypto.chacha", "Seal", "[]byte, []byte, []byte, []byte", "[]byte!", "slang_crypto_chacha_seal"),
    f!("std.crypto.chacha", "Open", "[]byte, []byte, []byte, []byte", "[]byte!", "slang_crypto_chacha_open"),
    f!("std.crypto.x25519", "GenerateKey", "", "[]byte!", "slang_crypto_x25519_generate_key"),
    f!("std.crypto.x25519", "PublicKey", "[]byte", "[]byte!", "slang_crypto_x25519_public_key"),
    f!("std.crypto.x25519", "SharedSecret", "[]byte, []byte", "[]byte!", "slang_crypto_x25519_shared_secret"),
    f!("std.crypto.ecdh", "GenerateKey", "", "[]byte!", "slang_crypto_ecdh_generate_key"),
    f!("std.crypto.ecdh", "PublicKey", "[]byte", "[]byte!", "slang_crypto_ecdh_public_key"),
    f!("std.crypto.ecdh", "SharedSecret", "[]byte, []byte", "[]byte!", "slang_crypto_ecdh_shared_secret"),
    f!("std.crypto.ecdsa", "GenerateKey", "", "[]byte!", "slang_crypto_ecdsa_generate_key"),
    f!("std.crypto.ecdsa", "PublicKey", "[]byte", "[]byte!", "slang_crypto_ecdsa_public_key"),
    f!("std.crypto.ecdsa", "Sign", "[]byte, []byte", "[]byte!", "slang_crypto_ecdsa_sign"),
    f!("std.crypto.ecdsa", "Verify", "[]byte, []byte, []byte", "bool", "slang_crypto_ecdsa_verify"),
    f!("std.crypto.ed25519", "GenerateKey", "", "[]byte!", "slang_crypto_ed25519_generate_key"),
    f!("std.crypto.ed25519", "PublicKey", "[]byte", "[]byte!", "slang_crypto_ed25519_public_key"),
    f!("std.crypto.ed25519", "Sign", "[]byte, []byte", "[]byte!", "slang_crypto_ed25519_sign"),
    f!("std.crypto.ed25519", "Verify", "[]byte, []byte, []byte", "bool", "slang_crypto_ed25519_verify"),
    f!("std.crypto.rsa", "VerifyPKCS1v15", "[]byte, []byte, string, []byte, []byte", "bool", "slang_crypto_rsa_verify_pkcs1v15"),
    f!("std.crypto.rsa", "VerifyPSS", "[]byte, []byte, string, []byte, []byte", "bool", "slang_crypto_rsa_verify_pss"),
    f!("std.crypto.rsa", "SignPKCS1v15", "[]byte, string, []byte", "[]byte!", "slang_crypto_rsa_sign_pkcs1v15"),
    f!("std.crypto.rsa", "SignPSS", "[]byte, string, []byte", "[]byte!", "slang_crypto_rsa_sign_pss"),
    // internal primitives of the Slang-source std.crypto.x509 / std.net.tls
    // (sema/stdlib_src/{x509,tls}.sl)
    f!("std.crypto.x509", "rtSystemRootsPath", "", "string", "slang_crypto_x509_rt_system_roots_path"),
    f!("std.net.tls", "rtConcat", "[]byte, []byte", "[]byte", "slang_net_tls_rt_concat"),
    f!("std.net.tls", "rtZeros", "int", "[]byte", "slang_net_tls_rt_zeros"),
    f!("std.net.tls", "rtCopy", "[]byte, int, []byte", "void", "slang_net_tls_rt_copy"),
    f!("std.net.tls", "rtAeadNew", "int, []byte", "[]byte!", "slang_net_tls_rt_aead_new"),
    f!("std.net.tls", "rtAeadSeal", "[]byte, []byte, []byte, []byte", "[]byte", "slang_net_tls_rt_aead_seal"),
    f!("std.net.tls", "rtAeadOpen", "[]byte, []byte, []byte, []byte", "[]byte!", "slang_net_tls_rt_aead_open"),
    // tier 0/1 stdlib (docs/STDLIB.md): std.strconv (std_strconv.c), std.unicode (std_unicode.c)
    f!("std.strconv", "rtFormatFloat", "float, int, int", "string", "slang_strconv_rt_format_float"),
    f!("std.strconv", "rtParseFloat", "string", "float!", "slang_strconv_rt_parse_float"),
    f!("std.unicode", "IsLetter", "char", "bool", "slang_unicode_is_letter"),
    f!("std.unicode", "IsDigit", "char", "bool", "slang_unicode_is_digit"),
    f!("std.unicode", "IsNumber", "char", "bool", "slang_unicode_is_number"),
    f!("std.unicode", "IsSpace", "char", "bool", "slang_unicode_is_space"),
    f!("std.unicode", "IsUpper", "char", "bool", "slang_unicode_is_upper"),
    f!("std.unicode", "IsLower", "char", "bool", "slang_unicode_is_lower"),
    f!("std.unicode", "IsTitle", "char", "bool", "slang_unicode_is_title"),
    f!("std.unicode", "IsPunct", "char", "bool", "slang_unicode_is_punct"),
    f!("std.unicode", "IsSymbol", "char", "bool", "slang_unicode_is_symbol"),
    f!("std.unicode", "IsMark", "char", "bool", "slang_unicode_is_mark"),
    f!("std.unicode", "IsControl", "char", "bool", "slang_unicode_is_control"),
    f!("std.unicode", "IsPrint", "char", "bool", "slang_unicode_is_print"),
    f!("std.unicode", "IsGraphic", "char", "bool", "slang_unicode_is_graphic"),
    f!("std.unicode", "ToUpper", "char", "char", "slang_unicode_to_upper"),
    f!("std.unicode", "ToLower", "char", "char", "slang_unicode_to_lower"),
    f!("std.unicode", "ToTitle", "char", "char", "slang_unicode_to_title"),
    f!("std.unicode", "SimpleFold", "char", "char", "slang_unicode_simple_fold"),
    // std.math.bits (std_math_bits.c)
    f!("std.math", "Float64bits", "float", "uint", "slang_math_float64bits"),
    f!("std.math", "Float64frombits", "uint", "float", "slang_math_float64frombits"),
    f!("std.math", "Float32bits", "float32", "uint32", "slang_math_float32bits"),
    f!("std.math", "Float32frombits", "uint32", "float32", "slang_math_float32frombits"),
    f!("std.math.bits", "LeadingZeros", "uint", "int", "slang_math_bits_leading_zeros"),
    f!("std.math.bits", "LeadingZeros32", "uint32", "int", "slang_math_bits_leading_zeros32"),
    f!("std.math.bits", "TrailingZeros", "uint", "int", "slang_math_bits_trailing_zeros"),
    f!("std.math.bits", "TrailingZeros32", "uint32", "int", "slang_math_bits_trailing_zeros32"),
    f!("std.math.bits", "OnesCount", "uint", "int", "slang_math_bits_ones_count"),
    f!("std.math.bits", "OnesCount32", "uint32", "int", "slang_math_bits_ones_count32"),
    f!("std.math.bits", "Len", "uint", "int", "slang_math_bits_len"),
    f!("std.math.bits", "Len32", "uint32", "int", "slang_math_bits_len32"),
    f!("std.math.bits", "RotateLeft", "uint, int", "uint", "slang_math_bits_rotate_left"),
    f!("std.math.bits", "RotateLeft32", "uint32, int", "uint32", "slang_math_bits_rotate_left32"),
    f!("std.math.bits", "Reverse", "uint", "uint", "slang_math_bits_reverse"),
    f!("std.math.bits", "Reverse32", "uint32", "uint32", "slang_math_bits_reverse32"),
    f!("std.math.bits", "ReverseBytes", "uint", "uint", "slang_math_bits_reverse_bytes"),
    f!("std.math.bits", "ReverseBytes32", "uint32", "uint32", "slang_math_bits_reverse_bytes32"),
    f!("std.math.bits", "ReverseBytes16", "uint16", "uint16", "slang_math_bits_reverse_bytes16"),
    f!("std.math.bits", "rtMulHi", "uint, uint", "uint", "slang_math_bits_rt_mul_hi"),
    f!("std.math.bits", "rtDivQuo", "uint, uint, uint", "uint", "slang_math_bits_rt_div_quo"),
    f!("std.math.bits", "rtDivRem", "uint, uint, uint", "uint", "slang_math_bits_rt_div_rem"),
    // >>> tier 2 stdlib externs
    // std.regex: the C half (std_regex.c); the engine is Slang (sema/stdlib_src/regex*.sl)
    f!("std.regex", "rtUnicode", "string", "[]int", "slang_regex_rt_unicode"),
    f!("std.regex", "rtFold", "int", "int", "slang_regex_rt_fold"),
    f!("std.regex", "rtInts", "int", "[]int", "slang_regex_rt_ints"),
    f!("std.regex", "rtBytes", "int", "[]byte", "slang_regex_rt_bytes"),
    f!("std.regex", "rtIndex", "string, string, int", "int", "slang_regex_rt_index"),
    f!("std.regex", "rtTryLock", "*int", "bool", "slang_regex_rt_try_lock"),
    f!("std.regex", "rtUnlock", "*int", "void", "slang_regex_rt_unlock"),
    // std.compress: DEFLATE coder and checksums (std_compress.c), zstd decoder (std_zstd.c)
    f!("std.compress", "rtCrc32", "int, []byte", "int", "slang_compress_rt_crc32"),
    f!("std.compress", "rtAdler32", "int, []byte", "int", "slang_compress_rt_adler32"),
    f!("std.compress", "rtInflateNew", "", "[]byte", "slang_compress_rt_inflate_new"),
    f!("std.compress", "rtInflate", "[]byte, []byte", "[]byte!", "slang_compress_rt_inflate"),
    f!("std.compress", "rtInflateDone", "[]byte", "bool", "slang_compress_rt_inflate_done"),
    f!("std.compress", "rtInflateTail", "[]byte", "[]byte", "slang_compress_rt_inflate_tail"),
    f!("std.compress", "rtInflateUnused", "[]byte", "int", "slang_compress_rt_inflate_unused"),
    f!("std.compress", "rtDeflate", "[]byte, []byte, int, bool", "[]byte", "slang_compress_rt_deflate"),
    f!("std.compress", "rtConcat", "[]byte, []byte", "[]byte", "slang_compress_rt_concat"),
    f!("std.compress", "rtCopy", "[]byte, int, int", "[]byte", "slang_compress_rt_copy"),
    f!("std.compress", "rtZstdDecode", "[]byte", "[]byte!", "slang_compress_rt_zstd_decode"),
    f!("std.compress", "rtZstdFrameLen", "[]byte", "int", "slang_compress_rt_zstd_frame_len"),
    f!("std.compress", "rtXxh64", "[]byte", "int", "slang_compress_rt_xxh64"),
    f!("std.compress", "rtZeros", "int", "[]byte", "slang_compress_rt_zeros"),
    // std.encoding.xml: the tokenizer (std_xml.c); the rest is Slang (sema/stdlib_src/xml.sl)
    f!("std.encoding.xml", "rtScan", "string, bool", "[]string!", "slang_encoding_xml_rt_scan"),
    f!("std.encoding.xml", "rtEscape", "string, bool", "string", "slang_encoding_xml_rt_escape"),
    // std.html (std_html.c)
    f!("std.html", "Escape", "string", "string", "slang_html_escape"),
    f!("std.html", "Unescape", "string", "string", "slang_html_unescape"),
    // std.mime extension table (std_mime.c)
    f!("std.mime", "rtType", "string", "string", "slang_mime_rt_type"),
    f!("std.mime", "rtAdd", "string, string", "void", "slang_mime_rt_add"),
    f!("std.mime", "rtExts", "string", "[]string", "slang_mime_rt_exts"),
    // <<< tier 2 stdlib externs
    // std.math.big limb arithmetic (std_math_big.c)
    f!("std.math.big", "rtAdd", "[]uint32, []uint32", "[]uint32", "slang_math_big_rt_add"),
    f!("std.math.big", "rtSub", "[]uint32, []uint32", "[]uint32", "slang_math_big_rt_sub"),
    f!("std.math.big", "rtCmp", "[]uint32, []uint32", "int", "slang_math_big_rt_cmp"),
    f!("std.math.big", "rtMul", "[]uint32, []uint32", "[]uint32", "slang_math_big_rt_mul"),
    f!("std.math.big", "rtDivMod", "[]uint32, []uint32", "[]uint32", "slang_math_big_rt_divmod"),
    f!("std.math.big", "rtShl", "[]uint32, int", "[]uint32", "slang_math_big_rt_shl"),
    f!("std.math.big", "rtShr", "[]uint32, int", "[]uint32", "slang_math_big_rt_shr"),
    f!("std.math.big", "rtBitLen", "[]uint32", "int", "slang_math_big_rt_bit_len"),
    f!("std.math.big", "rtFormat", "[]uint32, int", "string", "slang_math_big_rt_format"),
    f!("std.math.big", "rtParse", "string, int", "[]uint32!", "slang_math_big_rt_parse"),
    f!("std.math.big", "rtFromBytes", "[]byte", "[]uint32", "slang_math_big_rt_from_bytes"),
    f!("std.math.big", "rtToBytes", "[]uint32", "[]byte", "slang_math_big_rt_to_bytes"),
    f!("std.math.big", "rtFromUint", "uint", "[]uint32", "slang_math_big_rt_from_uint"),
    // std.hash.maphash (std_hash.c)
    f!("std.hash.maphash", "rtSip13", "uint, uint, []byte", "uint", "slang_hash_maphash_rt_sip13"),
    f!("std.hash.maphash", "rtSip13Str", "uint, uint, string", "uint", "slang_hash_maphash_rt_sip13_str"),
    // std.time types (time.sl): the local zone's TZif bytes (std_time_zone.c)
    f!("std.time", "rtLocalTZif", "", "string!", "slang_time_rt_local_tzif"),
    // std.os files (os.sl over std_os_file.c)
    f!("std.os", "rtOpen", "string, int, int", "int!", "slang_os_rt_open"),
    f!("std.os", "rtRead", "int, []byte", "int!", "slang_os_rt_read"),
    f!("std.os", "rtPread", "int, []byte, int", "int!", "slang_os_rt_pread"),
    f!("std.os", "rtWrite", "int, []byte", "int!", "slang_os_rt_write"),
    f!("std.os", "rtWriteStr", "int, string", "int!", "slang_os_rt_write_str"),
    f!("std.os", "rtSeek", "int, int, int", "int!", "slang_os_rt_seek"),
    f!("std.os", "rtClose", "int", "void!", "slang_os_rt_close"),
    f!("std.os", "rtSync", "int", "void!", "slang_os_rt_sync"),
    f!("std.os", "rtTruncate", "int, int", "void!", "slang_os_rt_truncate"),
    f!("std.os", "rtFstat", "int", "[]int!", "slang_os_rt_fstat"),
    f!("std.os", "rtStat", "string, bool", "[]int!", "slang_os_rt_stat"),
    f!("std.os", "rtReadDir", "string", "[]string!", "slang_os_rt_read_dir"),
    f!("std.os", "rtChmod", "string, int", "void!", "slang_os_rt_chmod"),
    // std.sync atomics (std_sync.c)
    f!("std.sync", "rtLoad", "*int", "int", "slang_sync_rt_load"),
    f!("std.sync", "rtStore", "*int, int", "void", "slang_sync_rt_store"),
    f!("std.sync", "rtAdd", "*int, int", "int", "slang_sync_rt_add"),
    f!("std.sync", "rtSwap", "*int, int", "int", "slang_sync_rt_swap"),
    f!("std.sync", "rtCas", "*int, int, int", "bool", "slang_sync_rt_cas"),
    f!("std.sync", "rtYield", "", "void", "slang_sync_rt_yield"),
    // std.os.signal (std_os_signal.c)
    f!("std.os.signal", "rtWait", "int", "Task<void>", "slang_os_signal_rt_wait"),
    f!("std.os.signal", "rtIgnore", "int", "void", "slang_os_signal_rt_ignore"),
    f!("std.os.signal", "rtReset", "int", "void", "slang_os_signal_rt_reset"),
    f!("std.os.signal", "rtRaise", "int", "void", "slang_os_signal_rt_raise"),
    f!("std.os.signal", "rtPending", "int", "int", "slang_os_signal_rt_pending"),
    f!("std.os.signal", "rtCatch", "int", "void", "slang_os_signal_rt_catch"),
    // std.process (std_process.c)
    f!("std.process", "rtSpawn", "string, []string, []string, bool, string, int, int, int", "[]int!", "slang_process_rt_spawn"),
    f!("std.process", "rtRead", "int, []byte", "int!", "slang_process_rt_read"),
    f!("std.process", "rtWrite", "int, []byte, int", "int!", "slang_process_rt_write"),
    f!("std.process", "rtClose", "int", "void", "slang_process_rt_close"),
    f!("std.process", "rtWaitPid", "int", "int!", "slang_process_rt_wait_pid"),
    f!("std.process", "rtKill", "int, int", "void!", "slang_process_rt_kill"),
    f!("std.process", "rtExecutable", "string", "bool", "slang_process_rt_executable"),
    f!("std.process", "rtWaitReadable", "int, int", "Task<int>", "slang_process_rt_wait_readable"),
    f!("std.process", "rtWaitWritable", "int, int", "Task<int>", "slang_process_rt_wait_writable"),
    // std.os.user (std_os_user.c)
    f!("std.os.user", "rtLookup", "string, int, bool", "[]string!", "slang_os_user_rt_lookup"),
    f!("std.os.user", "rtGroup", "string, int, bool", "[]string!", "slang_os_user_rt_group"),
    f!("std.os.user", "rtUid", "", "int", "slang_os_user_rt_uid"),
    // std.uuid
    f!("std.uuid", "New", "", "string", "slang_uuid_new"),
    f!("std.uuid", "IsValid", "string", "bool", "slang_uuid_is_valid"),
];

/// Parses a type in the table's mini syntax: builtin names, `[]T`, `T!`,
/// `Task<T>`, `*T` (passed as a plain pointer: only `&place` arguments of
/// the internal `rt*` externs use it), `map[K]V`.
pub fn parse_type(tt: &mut TypeTable, s: &str) -> TypeId {
    let s = s.trim();
    if let Some(inner) = s.strip_suffix('!') {
        let t = parse_type(tt, inner);
        return tt.fallible(t);
    }
    if let Some(inner) = s.strip_prefix("[]") {
        let t = parse_type(tt, inner);
        return tt.slice(t);
    }
    if let Some(inner) = s.strip_prefix("Task<").and_then(|r| r.strip_suffix('>')) {
        let t = parse_type(tt, inner);
        return tt.task(t);
    }
    if let Some(inner) = s.strip_prefix('*') {
        let t = parse_type(tt, inner);
        return tt.pointer(t);
    }
    if let Some((k, v)) = s.strip_prefix("map[").and_then(|r| r.split_once(']')) {
        let (k, v) = (parse_type(tt, k), parse_type(tt, v));
        return tt.map(k, v);
    }
    tt.builtin(s).unwrap_or_else(|| panic!("stdlib table: unknown type `{s}`"))
}

impl StdFunc {
    /// Parameter and result types.
    pub fn signature(&self, tt: &mut TypeTable) -> (Vec<TypeId>, TypeId) {
        let params = if self.params.trim().is_empty() {
            Vec::new()
        } else {
            self.params.split(", ").map(|p| parse_type(tt, p)).collect()
        };
        (params, parse_type(tt, self.ret))
    }

    /// Full Slang path, e.g. `std.strings.Split`.
    pub fn path(&self) -> String {
        format!("{}.{}", self.ns, self.name)
    }
}

// -- @embed source paths (sema/attrs.rs embed_global) ----------------------------

thread_local! {
    /// Source paths of the files being analysed (by file index), for
    /// `@embed` paths; set by the driver around analysis.
    static SOURCE_PATHS: std::cell::RefCell<Vec<String>> = const { std::cell::RefCell::new(Vec::new()) };
}

/// Runs `f` with the source paths of the files being analysed known to
/// `@embed` (relative embed paths resolve against their directories;
/// without them, against the current directory).
pub fn with_source_paths<R>(paths: Vec<String>, f: impl FnOnce() -> R) -> R {
    let saved = SOURCE_PATHS.with(|p| std::mem::replace(&mut *p.borrow_mut(), paths));
    let r = f();
    SOURCE_PATHS.with(|p| *p.borrow_mut() = saved);
    r
}

/// The directory of source file `file` (for `@embed`; empty if unknown).
pub(crate) fn embed_source_dir(file: usize) -> std::path::PathBuf {
    SOURCE_PATHS.with(|p| {
        p.borrow()
            .get(file)
            .and_then(|f| std::path::Path::new(f).parent().map(|d| d.to_path_buf()))
            .unwrap_or_default()
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn table_types_parse_and_symbols_follow_convention() {
        let mut tt = TypeTable::new();
        for f in FUNCS {
            f.signature(&mut tt);
            let ns = f.ns.strip_prefix("std.").unwrap().replace('.', "_");
            assert!(f.symbol.starts_with(&format!("slang_{ns}_")), "{}", f.symbol);
            assert!(NAMESPACES.contains(&f.ns), "{} not in NAMESPACES", f.ns);
        }
    }

    #[test]
    fn no_duplicate_names_or_symbols() {
        let mut seen = std::collections::HashSet::new();
        let mut syms = std::collections::HashSet::new();
        for f in FUNCS {
            assert!(seen.insert(f.path()), "duplicate {}", f.path());
            assert!(syms.insert(f.symbol), "duplicate symbol {}", f.symbol);
        }
    }
}
