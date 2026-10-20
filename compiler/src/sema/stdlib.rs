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
//! error of `std.io`/`std.fs`).
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
//! Builtins always in scope (no import): `error`, `len`, `append`,
//! `delete`, `panic`, conversions `int(x)`, `float(x)`, `string(c)` (and
//! the other numeric type names), `typeof` (rejected for now: reflection is
//! not implemented).

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
    "std.uuid",
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
}

impl std::fmt::Display for StdConst {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            StdConst::Float(v) => write!(f, "{v} (untyped float)"),
            StdConst::Int(v) => write!(f, "{v} (untyped int)"),
            StdConst::Error(_) => write!(f, "error (sentinel)"),
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
    f!("std.task", "Delay", "int", "Task<void>", "slang_task_delay"),
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
    // std.uuid
    f!("std.uuid", "New", "", "string", "slang_uuid_new"),
    f!("std.uuid", "IsValid", "string", "bool", "slang_uuid_is_valid"),
];

/// Parses a type in the table's mini syntax: builtin names, `[]T`, `T!`,
/// `Task<T>`.
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
