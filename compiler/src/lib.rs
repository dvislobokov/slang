//! Slang compiler library. Phases:
//! lexer -> parser (AST) -> sema (names, types) -> codegen (LLVM IR).

pub mod ast;
pub mod diagnostics;
pub mod lexer;
pub mod parser;
pub mod span;
pub mod token;
