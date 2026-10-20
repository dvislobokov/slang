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
//! | `std.task.Delay` | `slang_task_delay` | `(int) Task<void>` |
//!
//! Constants: `std.math.Pi`, `std.math.E` (untyped float constants, inlined).
//!
//! Specials (no extern; see `hir`):
//! `std.io.println` / `std.io.print` → [`crate::hir::ExprKind::Print`]
//! (runtime `slang_print_{int,uint,float,bool,rune,str,space,newline}`),
//! `std.fmt.Format` → extern `slang_fmt_format` with a `[]string` of
//! [`crate::hir::ExprKind::ToString`] arguments,
//! `std.task.All` → [`crate::hir::ExprKind::TaskAll`],
//! `std.errors.New` → [`crate::hir::Builtin::ErrorNew`],
//! `std.errors.As` → [`crate::hir::Builtin::ErrorsAs`].
//!
//! Builtins always in scope (no import): `error`, `len`, `append`,
//! `delete`, `panic`, conversions `int(x)`, `float(x)`, `string(c)` (and
//! the other numeric type names), `typeof` (rejected for now: reflection is
//! not implemented).

use crate::types::{TypeId, TypeTable};

/// Stdlib namespaces (all start with `std`).
pub const NAMESPACES: &[&str] = &["std", "std.io", "std.fmt", "std.strings", "std.math", "std.errors", "std.task"];

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
    /// `std.task.All(tasks ...)`.
    TaskAll,
    /// `std.errors.New(msg string) error`.
    ErrorsNew,
    /// `std.errors.As(err error, target *T) bool`.
    ErrorsAs,
}

/// Special functions by namespace and name.
pub const SPECIALS: &[(&str, &str, Special)] = &[
    ("std.io", "println", Special::Println),
    ("std.io", "print", Special::Print),
    ("std.fmt", "Format", Special::Format),
    ("std.task", "All", Special::TaskAll),
    ("std.errors", "New", Special::ErrorsNew),
    ("std.errors", "As", Special::ErrorsAs),
];

/// Symbol used for the lowered `fmt.Format`.
pub const FORMAT_SYMBOL: &str = "slang_fmt_format";

/// Untyped float constants: (namespace, name, value).
pub const CONSTS: &[(&str, &str, f64)] = &[("std.math", "Pi", std::f64::consts::PI), ("std.math", "E", std::f64::consts::E)];

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
    f!("std.task", "Delay", "int", "Task<void>", "slang_task_delay"),
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
            let ns = f.ns.strip_prefix("std.").unwrap();
            assert!(f.symbol.starts_with(&format!("slang_{ns}_")), "{}", f.symbol);
        }
    }
}
