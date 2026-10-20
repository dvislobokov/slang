//! Parser: tokens -> AST. TODO: implemented by the lexer/parser task.

use crate::ast::File;
use crate::diagnostics::Diagnostics;
use crate::token::Token;

pub fn parse(_tokens: Vec<Token>, _diags: &mut Diagnostics) -> Option<File> {
    unimplemented!("parser")
}
