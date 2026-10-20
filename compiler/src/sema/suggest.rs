//! "Did you mean" suggestions for unknown names (identifiers, types,
//! fields, methods, attributes): the closest candidate by edit distance.

use super::*;

/// Levenshtein distance, case-insensitive for the comparison of letters
/// (a pure case difference costs 1 in total, not per letter).
fn distance(a: &str, b: &str) -> usize {
    let a: Vec<char> = a.chars().collect();
    let b: Vec<char> = b.chars().collect();
    let mut prev: Vec<usize> = (0..=b.len()).collect();
    for (i, ca) in a.iter().enumerate() {
        let mut cur = vec![i + 1];
        for (j, cb) in b.iter().enumerate() {
            let cost = usize::from(ca.to_lowercase().ne(cb.to_lowercase()));
            cur.push((prev[j] + cost).min(prev[j + 1] + 1).min(cur[j] + 1));
        }
        prev = cur;
    }
    let d = prev[b.len()];
    if d == 0 && a != b {
        1
    } else {
        d
    }
}

/// The candidate closest to `name` (at most a third of its length away,
/// at least 1), if any.
pub(crate) fn closest<'c>(name: &str, candidates: impl IntoIterator<Item = &'c str>) -> Option<&'c str> {
    let limit = (name.chars().count() / 3).max(1);
    candidates
        .into_iter()
        .filter(|c| *c != name && !c.is_empty() && !c.starts_with('$'))
        .map(|c| (distance(name, c), c))
        .filter(|(d, _)| *d <= limit)
        .min_by_key(|(d, c)| (*d, c.len()))
        .map(|(_, c)| c)
}

impl<'a> Sema<'a> {
    /// Names visible at the current point: locals, members of the current
    /// namespace and of glob-imported namespaces, builtin functions/types.
    pub(crate) fn visible_names(&self) -> Vec<String> {
        let mut out: Vec<String> = Vec::new();
        if let Some(f) = self.fns.last() {
            for s in &f.scopes {
                out.extend(s.iter().map(|(n, _)| n.clone()));
            }
        }
        out.extend(self.nss[self.cur_ns].members.keys().cloned());
        if let Some(fs) = self.file_scopes.get(self.cur_file) {
            for e in &fs.imports {
                if let ImportEntry::Glob(ns) = e {
                    out.extend(self.nss[*ns].members.keys().filter(|n| self.nss[*ns].is_std || is_exported(n)).cloned());
                }
            }
        }
        out.extend(["println", "print", "len", "append", "delete", "panic", "error", "copy"].iter().map(|s| s.to_string()));
        out.extend(["int", "uint", "float", "string", "bool", "char", "byte", "any", "error"].iter().map(|s| s.to_string()));
        out
    }

    /// `did you mean `X`?` for an unknown name among `candidates`.
    pub(crate) fn did_you_mean(&self, name: &str, candidates: &[String]) -> Option<String> {
        closest(name, candidates.iter().map(|s| s.as_str())).map(|c| format!("did you mean `{c}`?"))
    }

    /// Fields and non-static methods of struct `sid` (with promoted ones).
    pub(crate) fn struct_member_names(&self, sid: u32) -> Vec<String> {
        let mut out = Vec::new();
        let mut stack = vec![(sid, 0)];
        while let Some((s, depth)) = stack.pop() {
            let st = &self.structs[s as usize];
            out.extend(st.fields.iter().map(|f| f.name.clone()));
            out.extend(st.methods.iter().map(|(n, _)| n.clone()));
            if depth < 8 {
                for f in st.fields.iter().filter(|f| f.embedded) {
                    if let Type::Struct(d) = self.tt.get(f.ty) {
                        stack.push((d.0, depth + 1));
                    }
                }
            }
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::closest;

    #[test]
    fn closest_names() {
        assert_eq!(closest("Strng", ["string", "int"]), Some("string"));
        assert_eq!(closest("Lenght", ["Length", "Len"]), Some("Length"));
        assert_eq!(closest("x", ["y", "xx"]), Some("y"));
        assert_eq!(closest("Height", ["Width", "Area"]), None);
        assert_eq!(closest("count", ["Count"]), Some("Count"));
    }
}
