//! Interface rules beyond structural conversion (DESIGN.md "Интерфейсы"):
//! `implements` checks at the declaration, embedding cycles, and the
//! exhaustiveness of switches over sealed interfaces and enums.

use super::*;
use crate::hir::{ExprKind, Pattern, StmtKind};

impl<'a> Sema<'a> {
    /// Checks the `implements` lists of every struct and enum (after all
    /// signatures are resolved).
    pub(crate) fn check_implements(&mut self) {
        for sid in 0..self.structs.len() {
            if self.gen.inst_structs.contains_key(&(sid as u32)) || self.struct_src[sid].implements.is_empty() {
                continue;
            }
            self.cur_file = self.structs[sid].file;
            self.cur_ns = self.file_scopes[self.cur_file].ns;
            self.check_struct_implements(sid as u32);
        }
        for eid in 0..self.enums.len() {
            if self.gen.inst_enums.contains_key(&(eid as u32)) || self.enum_src[eid].implements.is_empty() {
                continue;
            }
            self.cur_file = self.enums[eid].file;
            self.cur_ns = self.file_scopes[self.cur_file].ns;
            let src = self.enum_src[eid];
            let ty = self.enum_ty(eid as u32);
            for t in &src.implements {
                if let Some(iface) = self.implements_target(t) {
                    let problems = self.missing_methods(ty, iface);
                    self.report_implements(ty, iface, problems, t.span);
                }
            }
        }
    }

    /// Checks the `implements` list of struct `sid` (also used for
    /// instances of generic structs) in the current scope.
    pub(crate) fn check_struct_implements(&mut self, sid: u32) {
        let src = self.struct_src[sid as usize];
        let ty = self.struct_ty(sid);
        for t in &src.implements {
            if let Some(iface) = self.implements_target(t) {
                let problems = self.missing_methods(ty, iface);
                self.report_implements(ty, iface, problems, t.span);
            }
        }
    }

    /// Resolves one entry of an `implements` list; reports a non-interface.
    fn implements_target(&mut self, t: &ast::TypeExpr) -> Option<TypeId> {
        let iface = self.resolve_type(t);
        if self.tt.is_invalid(iface) {
            return None;
        }
        if !matches!(self.tt.get(iface), Type::Interface(_) | Type::Error) {
            self.err(format!("{} is not an interface (only interfaces can follow `implements`)", self.tn(iface)), t.span);
            return None;
        }
        Some(iface)
    }

    fn report_implements(&mut self, ty: TypeId, iface: TypeId, problems: Vec<String>, span: Span) {
        if !problems.is_empty() {
            let (s, i) = (self.tn(ty), self.tn(iface));
            self.err(format!("{s} does not implement {i}: {}", problems.join(", ")), span);
        }
    }

    /// Reports interfaces that embed themselves through other interfaces
    /// (`A { B }`, `B { A }`); `embeds[i]` are the interfaces `i` embeds.
    /// Direct self-embedding is reported by the caller. Returns the edges
    /// to drop (they close a cycle).
    pub(crate) fn embedding_cycles(&mut self, embeds: &[Vec<(u32, Span)>]) -> Vec<(usize, u32)> {
        fn reaches(embeds: &[Vec<(u32, Span)>], from: u32, to: u32, seen: &mut Vec<u32>, path: &mut Vec<u32>) -> bool {
            if from == to {
                return true;
            }
            if seen.contains(&from) {
                return false;
            }
            seen.push(from);
            for &(n, _) in &embeds[from as usize] {
                path.push(n);
                if reaches(embeds, n, to, seen, path) {
                    return true;
                }
                path.pop();
            }
            false
        }
        let mut drop = Vec::new();
        for i in 0..embeds.len() {
            for &(first, span) in &embeds[i] {
                if first as usize == i {
                    continue;
                }
                let mut path = vec![first];
                if reaches(embeds, first, i as u32, &mut Vec::new(), &mut path) {
                    drop.push((i, first));
                    // Report each cycle once, at its smallest interface.
                    if path.iter().all(|&p| p as usize >= i) {
                        self.cur_file = self.interfaces[i].file;
                        let names: Vec<String> = path[..path.len() - 1].iter().map(|&p| self.interfaces[p as usize].name.clone()).collect();
                        let name = self.interfaces[i].name.clone();
                        self.err(format!("interface {name} embeds itself through {}", names.join(", ")), span);
                    }
                }
            }
        }
        drop
    }

    /// The types implementing sealed interface `iface`: the structs and
    /// enums of its namespace with all its methods (in declaration order).
    fn sealed_impls(&mut self, iface: TypeId) -> Vec<TypeId> {
        let Type::Interface(d) = *self.tt.get(iface) else { return Vec::new() };
        let ns = self.interfaces[d.0 as usize].namespace;
        let mut out = Vec::new();
        for s in 0..self.structs.len() as u32 {
            if self.structs[s as usize].namespace == ns && !self.gen.inst_structs.contains_key(&s) {
                let t = self.struct_ty(s);
                if self.missing_methods(t, iface).is_empty() {
                    out.push(t);
                }
            }
        }
        for e in 0..self.enums.len() as u32 {
            if self.enums[e as usize].namespace == ns && !self.gen.inst_enums.contains_key(&e) {
                let t = self.enum_ty(e);
                if self.missing_methods(t, iface).is_empty() {
                    out.push(t);
                }
            }
        }
        out
    }

    /// For a switch without `default` over subject type `subj`: when it
    /// names every implementation of a sealed interface (or every variant
    /// of an enum), a `default` that panics is returned, so the switch is a
    /// terminating statement (the default is unreachable). A switch over a
    /// sealed interface that misses implementations is an error.
    pub(crate) fn exhaustive_default(&mut self, subj: TypeId, cases: &[hir::Case], span: Span) -> Option<hir::Block> {
        let covered = |want: &dyn Fn(&Pattern) -> bool| cases.iter().any(|c| c.patterns.iter().any(want));
        let exhaustive = match *self.tt.get(subj) {
            Type::Interface(d) if self.interfaces[d.0 as usize].sealed => {
                let mut missing = Vec::new();
                for st in self.sealed_impls(subj) {
                    let pt = self.tt.pointer(st);
                    if !covered(&|p| matches!(p, Pattern::Type { ty, .. } if *ty == st || *ty == pt)) {
                        missing.push(self.tn(st));
                    }
                }
                if !missing.is_empty() {
                    let msg = format!("switch on sealed interface {} is not exhaustive: missing {}", self.tn(subj), missing.join(", "));
                    self.err_note(msg, span, "add the missing cases or a `default`", None);
                    return None;
                }
                true
            }
            Type::Enum(d) => {
                let n = self.enums[d.0 as usize].variants.len() as u32;
                n > 0 && (0..n).all(|v| covered(&|p| matches!(p, Pattern::Variant { variant, .. } if *variant == v)))
            }
            _ => false,
        };
        if !exhaustive {
            return None;
        }
        let msg = mk(ExprKind::Const(hir::ConstValue::Str("unreachable: no case of an exhaustive switch matched".into())), self.tt.string, span);
        let panic = mk(ExprKind::Builtin { op: hir::Builtin::Panic, args: vec![msg] }, self.tt.void, span);
        Some(hir::Block { stmts: vec![hir::Stmt { kind: StmtKind::Expr(panic), span }], span })
    }
}
