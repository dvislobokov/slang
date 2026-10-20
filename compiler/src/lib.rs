//! Slang compiler library. Phases:
//! lexer -> parser (AST) -> sema (names, types) -> codegen (LLVM IR).

pub mod ast;
pub mod build;
pub mod codegen;
pub mod diagnostics;
pub mod driver;
pub mod fmt;
pub mod hir;
pub mod lexer;
pub mod package;
pub mod parser;
pub mod sema;
pub mod session;
pub mod span;
pub mod token;
pub mod types;
