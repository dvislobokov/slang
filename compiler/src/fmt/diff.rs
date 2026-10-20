//! Line diffs between a file and its formatted text: hunks for LSP text
//! edits (whole and range formatting) and unified diffs for `--diff`.

/// Lines `old_start..old_start + old_len` (0-based) of the old text are
/// replaced by `new_lines` (each with its `\n`, except possibly the last
/// line of the file), which start at line `new_start` of the new text.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Hunk {
    pub old_start: usize,
    pub old_len: usize,
    pub new_start: usize,
    pub new_lines: Vec<String>,
}

/// Largest middle section (old lines x new lines) diffed precisely; above
/// it the middle becomes one hunk.
const MAX_CELLS: usize = 4_000_000;

fn lines(s: &str) -> Vec<&str> {
    s.split_inclusive('\n').collect()
}

/// Minimal line hunks turning `old` into `new` (LCS-based).
pub fn line_hunks(old: &str, new: &str) -> Vec<Hunk> {
    let a = lines(old);
    let b = lines(new);
    let mut pre = 0;
    while pre < a.len() && pre < b.len() && a[pre] == b[pre] {
        pre += 1;
    }
    let mut suf = 0;
    while suf < a.len() - pre && suf < b.len() - pre && a[a.len() - 1 - suf] == b[b.len() - 1 - suf] {
        suf += 1;
    }
    let (am, bm) = (&a[pre..a.len() - suf], &b[pre..b.len() - suf]);
    if am.is_empty() && bm.is_empty() {
        return Vec::new();
    }
    if am.len().saturating_mul(bm.len()) > MAX_CELLS || am.is_empty() || bm.is_empty() {
        return vec![Hunk { old_start: pre, old_len: am.len(), new_start: pre, new_lines: bm.iter().map(|s| s.to_string()).collect() }];
    }
    // lcs[i][j] = LCS length of am[i..] and bm[j..].
    let (n, m) = (am.len(), bm.len());
    let mut lcs = vec![0u32; (n + 1) * (m + 1)];
    let at = |i: usize, j: usize| i * (m + 1) + j;
    for i in (0..n).rev() {
        for j in (0..m).rev() {
            lcs[at(i, j)] = if am[i] == bm[j] { lcs[at(i + 1, j + 1)] + 1 } else { lcs[at(i + 1, j)].max(lcs[at(i, j + 1)]) };
        }
    }
    let mut hunks = Vec::new();
    let (mut i, mut j) = (0, 0);
    let mut cur: Option<Hunk> = None;
    while i < n || j < m {
        if i < n && j < m && am[i] == bm[j] {
            if let Some(h) = cur.take() {
                hunks.push(h);
            }
            i += 1;
            j += 1;
            continue;
        }
        let h = cur.get_or_insert_with(|| Hunk { old_start: pre + i, old_len: 0, new_start: pre + j, new_lines: Vec::new() });
        if j < m && (i == n || lcs[at(i, j + 1)] >= lcs[at(i + 1, j)]) {
            h.new_lines.push(bm[j].to_string());
            j += 1;
        } else {
            h.old_len += 1;
            i += 1;
        }
    }
    if let Some(h) = cur {
        hunks.push(h);
    }
    hunks
}

/// A unified diff (`--- path`, `+++ path`, 3 lines of context).
pub fn unified_diff(path: &str, old: &str, new: &str) -> String {
    let hunks = line_hunks(old, new);
    if hunks.is_empty() {
        return String::new();
    }
    let a = lines(old);
    let mut out = format!("--- {path}\n+++ {path} (formatted)\n");
    let ctx = 3;
    let mut k = 0;
    while k < hunks.len() {
        // Merge hunks whose context overlaps.
        let mut end = k;
        while end + 1 < hunks.len() && hunks[end + 1].old_start <= hunks[end].old_start + hunks[end].old_len + 2 * ctx {
            end += 1;
        }
        let first = &hunks[k];
        let last = &hunks[end];
        let old_from = first.old_start.saturating_sub(ctx);
        let old_to = (last.old_start + last.old_len + ctx).min(a.len());
        let new_from = first.new_start - (first.old_start - old_from);
        let mut body = String::new();
        let mut new_count = 0;
        let mut pos = old_from;
        for h in &hunks[k..=end] {
            for l in &a[pos..h.old_start] {
                body.push(' ');
                body.push_str(l);
                new_count += 1;
            }
            for l in &a[h.old_start..h.old_start + h.old_len] {
                body.push('-');
                body.push_str(l);
            }
            for l in &h.new_lines {
                body.push('+');
                body.push_str(l);
                new_count += 1;
            }
            pos = h.old_start + h.old_len;
        }
        for l in &a[pos..old_to] {
            body.push(' ');
            body.push_str(l);
            new_count += 1;
        }
        if !body.ends_with('\n') {
            body.push_str("\n\\ No newline at end of file\n");
        }
        out.push_str(&format!("@@ -{},{} +{},{} @@\n", old_from + 1, old_to - old_from, new_from + 1, new_count));
        out.push_str(&body);
        k = end + 1;
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn apply(old: &str, hunks: &[Hunk]) -> String {
        let a = lines(old);
        let mut out = String::new();
        let mut pos = 0;
        for h in hunks {
            out.extend(a[pos..h.old_start].iter().copied());
            out.extend(h.new_lines.iter().map(|s| s.as_str()));
            pos = h.old_start + h.old_len;
        }
        out.extend(a[pos..].iter().copied());
        out
    }

    #[test]
    fn hunks_reconstruct_the_new_text() {
        let cases = [("a\nb\nc\n", "a\nB\nc\n"), ("a\nb\n", "a\nb\nc\n"), ("x\ny\nz\n", "y\n"), ("", "a\n"), ("a\n", ""), ("1\n2\n3\n4\n", "0\n1\n3\n4\n5\n")];
        for (old, new) in cases {
            let h = line_hunks(old, new);
            assert_eq!(apply(old, &h), new, "{old:?} -> {new:?}: {h:?}");
        }
        assert!(line_hunks("same\n", "same\n").is_empty());
        let h = line_hunks("a\nb\nc\n", "a\nB\nc\n");
        assert_eq!(h, vec![Hunk { old_start: 1, old_len: 1, new_start: 1, new_lines: vec!["B\n".into()] }]);
    }

    #[test]
    fn unified_diff_format() {
        let d = unified_diff("f.sl", "a\nb\nc\n", "a\nB\nc\n");
        assert_eq!(d, "--- f.sl\n+++ f.sl (formatted)\n@@ -1,3 +1,3 @@\n a\n-b\n+B\n c\n");
    }
}
