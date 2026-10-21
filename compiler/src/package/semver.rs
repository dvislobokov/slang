//! Semantic versions of modules (git tags `v1.2.3`) and the constraint
//! syntax of manifests (docs/PACKAGES.md): `^1.2`, `~1.2.3`, `>=1, <2`,
//! `=1.4.0`, `*`; a bare `1.2` means `^1.2`.

use std::cmp::Ordering;
use std::fmt;

/// `MAJOR.MINOR.PATCH[-pre][+build]`. Ordering follows semver 2.0
/// (pre-releases sort before the release; build metadata is ignored).
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct Version {
    pub major: u64,
    pub minor: u64,
    pub patch: u64,
    /// Pre-release identifiers (`rc.1` -> ["rc", "1"]).
    pub pre: Vec<String>,
    pub build: String,
}

impl Version {
    pub fn new(major: u64, minor: u64, patch: u64) -> Version {
        Version { major, minor, patch, pre: Vec::new(), build: String::new() }
    }

    /// Parses `v1.2.3` or `1.2.3` (with optional `-pre` and `+build`).
    pub fn parse(s: &str) -> Option<Version> {
        let s = s.strip_prefix('v').unwrap_or(s);
        let (s, build) = match s.split_once('+') {
            Some((a, b)) => (a, b.to_string()),
            None => (s, String::new()),
        };
        let (core, pre) = match s.split_once('-') {
            Some((a, b)) => (a, b.split('.').map(str::to_string).collect::<Vec<_>>()),
            None => (s, Vec::new()),
        };
        if pre.iter().any(|p| p.is_empty() || !p.chars().all(|c| c.is_ascii_alphanumeric() || c == '-')) {
            return None;
        }
        let nums: Vec<&str> = core.split('.').collect();
        if nums.len() != 3 {
            return None;
        }
        let num = |p: &str| -> Option<u64> {
            if p.is_empty() || !p.chars().all(|c| c.is_ascii_digit()) || (p.len() > 1 && p.starts_with('0')) {
                return None;
            }
            p.parse().ok()
        };
        Some(Version { major: num(nums[0])?, minor: num(nums[1])?, patch: num(nums[2])?, pre, build })
    }

    /// Canonical tag spelling: `v1.2.3[-pre]`.
    pub fn tag(&self) -> String {
        format!("v{self}")
    }

    pub fn is_prerelease(&self) -> bool {
        !self.pre.is_empty()
    }
}

impl fmt::Display for Version {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}.{}.{}", self.major, self.minor, self.patch)?;
        if !self.pre.is_empty() {
            write!(f, "-{}", self.pre.join("."))?;
        }
        Ok(())
    }
}

impl Ord for Version {
    fn cmp(&self, o: &Version) -> Ordering {
        (self.major, self.minor, self.patch).cmp(&(o.major, o.minor, o.patch)).then_with(|| match (self.pre.is_empty(), o.pre.is_empty()) {
            (true, true) => Ordering::Equal,
            (true, false) => Ordering::Greater,
            (false, true) => Ordering::Less,
            (false, false) => {
                for (a, b) in self.pre.iter().zip(&o.pre) {
                    let c = match (a.parse::<u64>(), b.parse::<u64>()) {
                        (Ok(x), Ok(y)) => x.cmp(&y),
                        (Ok(_), Err(_)) => Ordering::Less,
                        (Err(_), Ok(_)) => Ordering::Greater,
                        (Err(_), Err(_)) => a.cmp(b),
                    };
                    if c != Ordering::Equal {
                        return c;
                    }
                }
                self.pre.len().cmp(&o.pre.len())
            }
        })
    }
}

impl PartialOrd for Version {
    fn partial_cmp(&self, o: &Version) -> Option<Ordering> {
        Some(self.cmp(o))
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Op {
    Eq,
    Gt,
    Ge,
    Lt,
    Le,
}

/// A version constraint: a conjunction of comparisons.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Constraint {
    text: String,
    preds: Vec<(Op, Version)>,
}

/// `1`, `1.2`, `1.2.3` (missing parts are `None`).
fn partial(s: &str) -> Option<(u64, Option<u64>, Option<u64>, Vec<String>)> {
    let s = s.trim().strip_prefix('v').unwrap_or(s.trim());
    if let Some(v) = Version::parse(s) {
        return Some((v.major, Some(v.minor), Some(v.patch), v.pre));
    }
    let parts: Vec<&str> = s.split('.').collect();
    let n = |p: &str| p.parse::<u64>().ok().filter(|_| p.chars().all(|c| c.is_ascii_digit()));
    match parts.as_slice() {
        [a] => Some((n(a)?, None, None, Vec::new())),
        [a, b] => Some((n(a)?, Some(n(b)?), None, Vec::new())),
        _ => None,
    }
}

impl Constraint {
    /// Parses a manifest constraint.
    pub fn parse(text: &str) -> Result<Constraint, String> {
        let bad = || format!("invalid version constraint `{text}` (expected e.g. `^1.2`, `~1.2.3`, `>=1, <2`, `=1.4.0`)");
        let mut preds = Vec::new();
        for part in text.split(',') {
            let p = part.trim();
            if p.is_empty() {
                return Err(bad());
            }
            if p == "*" {
                continue;
            }
            let (op, rest) = if let Some(r) = p.strip_prefix(">=") {
                ("ge", r)
            } else if let Some(r) = p.strip_prefix("<=") {
                ("le", r)
            } else if let Some(r) = p.strip_prefix('>') {
                ("gt", r)
            } else if let Some(r) = p.strip_prefix('<') {
                ("lt", r)
            } else if let Some(r) = p.strip_prefix('=') {
                ("eq", r)
            } else if let Some(r) = p.strip_prefix('~') {
                ("tilde", r)
            } else if let Some(r) = p.strip_prefix('^') {
                ("caret", r)
            } else {
                ("caret", p)
            };
            let (ma, mi, pa, pre) = partial(rest).ok_or_else(bad)?;
            let low = Version { major: ma, minor: mi.unwrap_or(0), patch: pa.unwrap_or(0), pre, build: String::new() };
            match op {
                "ge" => preds.push((Op::Ge, low)),
                "gt" => preds.push((Op::Gt, low)),
                "lt" => preds.push((Op::Lt, low)),
                "le" => preds.push((Op::Le, low)),
                "eq" => {
                    if mi.is_none() || pa.is_none() {
                        // `=1.2` means `>=1.2.0, <1.3.0`.
                        preds.push((Op::Ge, low));
                        let up = if mi.is_none() { Version::new(ma + 1, 0, 0) } else { Version::new(ma, mi.unwrap() + 1, 0) };
                        preds.push((Op::Lt, up));
                    } else {
                        preds.push((Op::Eq, low));
                    }
                }
                "tilde" => {
                    let up = if mi.is_none() { Version::new(ma + 1, 0, 0) } else { Version::new(ma, mi.unwrap() + 1, 0) };
                    preds.push((Op::Ge, low));
                    preds.push((Op::Lt, up));
                }
                _ => {
                    // Caret: the left-most non-zero part stays fixed.
                    let up = if ma > 0 || mi.is_none() {
                        Version::new(ma + 1, 0, 0)
                    } else if mi.unwrap() > 0 || pa.is_none() {
                        Version::new(0, mi.unwrap() + 1, 0)
                    } else {
                        Version::new(0, 0, pa.unwrap() + 1)
                    };
                    preds.push((Op::Ge, low));
                    preds.push((Op::Lt, up));
                }
            }
        }
        Ok(Constraint { text: text.trim().to_string(), preds })
    }

    /// The exact constraint `=v`.
    pub fn exact(v: &Version) -> Constraint {
        Constraint { text: format!("={v}"), preds: vec![(Op::Eq, v.clone())] }
    }

    /// Whether `v` satisfies the constraint. Pre-releases match only a
    /// constraint that names a pre-release of the same `MAJOR.MINOR.PATCH`.
    pub fn matches(&self, v: &Version) -> bool {
        if v.is_prerelease() && !self.preds.iter().any(|(_, b)| b.is_prerelease() && (b.major, b.minor, b.patch) == (v.major, v.minor, v.patch)) {
            return false;
        }
        self.preds.iter().all(|(op, b)| match op {
            Op::Eq => v == b,
            Op::Gt => v > b,
            Op::Ge => v >= b,
            Op::Lt => v < b,
            Op::Le => v <= b,
        })
    }

    /// The major versions the constraint can match, when it is bounded to
    /// one (`^1.2` -> 1); used for the one-major-per-graph rule.
    pub fn major(&self) -> Option<u64> {
        let lo = self.preds.iter().filter(|(o, _)| matches!(o, Op::Ge | Op::Gt | Op::Eq)).map(|(_, v)| v.major).max()?;
        let hi = self
            .preds
            .iter()
            .filter_map(|(o, v)| match o {
                Op::Eq | Op::Le => Some(v.major),
                Op::Lt if v.minor == 0 && v.patch == 0 && v.pre.is_empty() => Some(v.major.saturating_sub(1)),
                Op::Lt => Some(v.major),
                _ => None,
            })
            .min()?;
        (lo == hi).then_some(lo)
    }

    /// The smallest of `versions` matching the constraint (MVS minimum).
    pub fn lowest<'a>(&self, versions: &'a [Version]) -> Option<&'a Version> {
        versions.iter().filter(|v| self.matches(v)).min()
    }

    /// The newest of `versions` matching the constraint (`slang update`).
    pub fn highest<'a>(&self, versions: &'a [Version]) -> Option<&'a Version> {
        versions.iter().filter(|v| self.matches(v)).max()
    }
}

impl fmt::Display for Constraint {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.text)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn v(s: &str) -> Version {
        Version::parse(s).unwrap()
    }

    #[test]
    fn parses_and_orders_versions() {
        assert_eq!(v("v1.2.3"), Version::new(1, 2, 3));
        assert!(Version::parse("1.2").is_none());
        assert!(Version::parse("01.2.3").is_none());
        assert!(v("1.0.0-rc.1") < v("1.0.0"));
        assert!(v("1.0.0-alpha") < v("1.0.0-alpha.1"));
        assert!(v("1.0.0-alpha.2") < v("1.0.0-alpha.10"));
        assert!(v("1.10.0") > v("1.9.9"));
        assert_eq!(v("1.2.3-rc.1").tag(), "v1.2.3-rc.1");
    }

    #[test]
    fn constraints_match_like_cargo() {
        let c = |s: &str| Constraint::parse(s).unwrap();
        assert!(c("^1.2").matches(&v("1.9.0")) && !c("^1.2").matches(&v("2.0.0")) && !c("^1.2").matches(&v("1.1.9")));
        assert!(c("1.2").matches(&v("1.2.0")));
        assert!(c("^0.2.3").matches(&v("0.2.9")) && !c("^0.2.3").matches(&v("0.3.0")));
        assert!(c("~1.2.3").matches(&v("1.2.9")) && !c("~1.2.3").matches(&v("1.3.0")));
        assert!(c(">=0.5, <1").matches(&v("0.9.1")) && !c(">=0.5, <1").matches(&v("1.0.0")));
        assert!(c("=1.4.0").matches(&v("1.4.0")) && !c("=1.4.0").matches(&v("1.4.1")));
        assert!(!c("^1").matches(&v("1.5.0-rc.1")));
        assert!(c("*").matches(&v("7.0.0")));
        assert_eq!(c("^1.2").major(), Some(1));
        assert_eq!(c(">=1").major(), None);
        assert!(Constraint::parse("^x").is_err());
        let vs = [v("1.0.0"), v("1.2.0"), v("1.3.1"), v("2.0.0")];
        assert_eq!(c("^1.1").lowest(&vs), Some(&v("1.2.0")));
        assert_eq!(c("^1.1").highest(&vs), Some(&v("1.3.1")));
    }
}
