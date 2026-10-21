//! Module content hashes (`h1:`), module archives (`.zip`) and the base64
//! they need.
//!
//! # The `h1:` hash
//!
//! The checksum of a module version, recorded in `slang.lock`, in the
//! store index and in the checksum log, is computed over the module's file
//! tree (every regular file at the tag, without `.git/`):
//!
//! ```text
//! summary = for each file, sorted by path (bytewise, `/`-separated, relative):
//!               lowercase-hex(sha256(contents)) + "  " + path + "\n"
//! h1      = "h1:" + base64-std-padded(sha256(summary))
//! ```
//!
//! This is Go's `dirhash.Hash1` without the `module@version/` prefix on
//! paths, so the same tree has the same hash under every mirror; the
//! identity is carried by the key (`path@version`) next to the hash. The
//! manifest alone gets its own hash (`<path> <version>/manifest h1:...`
//! in the log), the same function over the single file `[name, bytes]`,
//! so resolution can trust a `.manifest` fetched without the archive.
//!
//! # Archives
//!
//! `<version>.zip` holds the files under `<path>@<version>/` (Go's
//! layout), stored without compression (method 0) so the archive is
//! reproducible byte for byte from the tree; [`zip_read`] accepts only
//! that and checks every CRC.

use sha2::{Digest, Sha256};
use std::path::Path;

/// One file of a module: relative `/`-separated path and contents.
pub type FileEntry = (String, Vec<u8>);

pub fn sha256_hex(bytes: &[u8]) -> String {
    super::lock::hex(&Sha256::digest(bytes))
}

/// The `h1:` hash of a file list (see the module docs).
pub fn h1(files: &[FileEntry]) -> String {
    let mut sorted: Vec<&FileEntry> = files.iter().collect();
    sorted.sort_by(|a, b| a.0.as_bytes().cmp(b.0.as_bytes()));
    let mut summary = String::new();
    for (p, data) in sorted {
        summary.push_str(&sha256_hex(data));
        summary.push_str("  ");
        summary.push_str(p);
        summary.push('\n');
    }
    format!("h1:{}", base64(&Sha256::digest(summary.as_bytes())))
}

/// The `h1:` hash of a summary given as (path, sha256-hex) pairs (the
/// store index already has the per-file hashes).
pub fn h1_of_hashes(files: &[(String, String)]) -> String {
    let mut sorted: Vec<&(String, String)> = files.iter().collect();
    sorted.sort_by(|a, b| a.0.as_bytes().cmp(b.0.as_bytes()));
    let mut summary = String::new();
    for (p, h) in sorted {
        summary.push_str(h);
        summary.push_str("  ");
        summary.push_str(p);
        summary.push('\n');
    }
    format!("h1:{}", base64(&Sha256::digest(summary.as_bytes())))
}

/// Checks a module-relative path: non-empty `/`-separated segments, no
/// `.`/`..`, no backslashes or NULs, no `.git` segment.
pub fn valid_file_path(p: &str) -> bool {
    !p.is_empty() && !p.contains(['\\', '\0']) && p.split('/').all(|s| !s.is_empty() && s != "." && s != ".." && s != ".git")
}

/// Every regular file under `dir` except `.git/` (sorted). Symbolic links
/// are refused: a module is plain files.
pub fn read_tree(dir: &Path) -> Result<Vec<FileEntry>, String> {
    fn walk(root: &Path, dir: &Path, out: &mut Vec<FileEntry>) -> Result<(), String> {
        let rd = std::fs::read_dir(dir).map_err(|e| format!("cannot read {}: {e}", dir.display()))?;
        for e in rd {
            let e = e.map_err(|e| e.to_string())?;
            let p = e.path();
            let name = e.file_name().to_string_lossy().to_string();
            let ft = e.file_type().map_err(|e| e.to_string())?;
            if name == ".git" {
                continue;
            }
            if ft.is_symlink() {
                return Err(format!("{}: symbolic links are not allowed in a module", p.display()));
            }
            if ft.is_dir() {
                walk(root, &p, out)?;
            } else if ft.is_file() {
                let rel = p.strip_prefix(root).unwrap().components().map(|c| c.as_os_str().to_string_lossy().to_string()).collect::<Vec<_>>().join("/");
                let data = std::fs::read(&p).map_err(|e| format!("cannot read {}: {e}", p.display()))?;
                out.push((rel, data));
            }
        }
        Ok(())
    }
    let mut out = Vec::new();
    walk(dir, dir, &mut out)?;
    out.sort_by(|a, b| a.0.cmp(&b.0));
    Ok(out)
}

// -- base64 --------------------------------------------------------------------

const B64: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// Standard base64 with padding.
pub fn base64(data: &[u8]) -> String {
    let mut out = String::with_capacity(data.len().div_ceil(3) * 4);
    for chunk in data.chunks(3) {
        let b = [chunk[0], *chunk.get(1).unwrap_or(&0), *chunk.get(2).unwrap_or(&0)];
        let n = (b[0] as u32) << 16 | (b[1] as u32) << 8 | b[2] as u32;
        for i in 0..4 {
            if i <= chunk.len() {
                out.push(B64[(n >> (18 - 6 * i) & 63) as usize] as char);
            } else {
                out.push('=');
            }
        }
    }
    out
}

/// Decodes standard padded base64.
pub fn base64_decode(s: &str) -> Option<Vec<u8>> {
    let s = s.trim();
    if s.len() % 4 != 0 {
        return None;
    }
    let mut out = Vec::new();
    for chunk in s.as_bytes().chunks(4) {
        let mut n = 0u32;
        let mut pad = 0;
        for &c in chunk {
            let v = match c {
                b'=' => {
                    pad += 1;
                    0
                }
                _ if pad > 0 => return None,
                _ => B64.iter().position(|&x| x == c)? as u32,
            };
            n = n << 6 | v;
        }
        let bytes = [(n >> 16) as u8, (n >> 8) as u8, n as u8];
        out.extend_from_slice(&bytes[..3 - pad.min(2)]);
    }
    Some(out)
}

// -- zip (stored) --------------------------------------------------------------

fn crc32(data: &[u8]) -> u32 {
    static TABLE: std::sync::OnceLock<[u32; 256]> = std::sync::OnceLock::new();
    let t = TABLE.get_or_init(|| {
        let mut t = [0u32; 256];
        for (i, e) in t.iter_mut().enumerate() {
            let mut c = i as u32;
            for _ in 0..8 {
                c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 };
            }
            *e = c;
        }
        t
    });
    let mut c = !0u32;
    for &b in data {
        c = t[((c ^ b as u32) & 0xff) as usize] ^ (c >> 8);
    }
    !c
}

/// A reproducible store-only zip of `files` under `prefix/`.
pub fn zip_write(prefix: &str, files: &[FileEntry]) -> Vec<u8> {
    let mut sorted: Vec<&FileEntry> = files.iter().collect();
    sorted.sort_by(|a, b| a.0.cmp(&b.0));
    let mut out = Vec::new();
    let mut central = Vec::new();
    let le16 = |v: &mut Vec<u8>, x: u16| v.extend_from_slice(&x.to_le_bytes());
    let le32 = |v: &mut Vec<u8>, x: u32| v.extend_from_slice(&x.to_le_bytes());
    for (p, data) in &sorted {
        let name = format!("{prefix}/{p}");
        let crc = crc32(data);
        let off = out.len() as u32;
        le32(&mut out, 0x0403_4b50);
        le16(&mut out, 20); // version needed
        le16(&mut out, 0x0800); // UTF-8 names
        le16(&mut out, 0); // stored
        le16(&mut out, 0); // time
        le16(&mut out, 0x21); // date 1980-01-01
        le32(&mut out, crc);
        le32(&mut out, data.len() as u32);
        le32(&mut out, data.len() as u32);
        le16(&mut out, name.len() as u16);
        le16(&mut out, 0);
        out.extend_from_slice(name.as_bytes());
        out.extend_from_slice(data);
        le32(&mut central, 0x0201_4b50);
        le16(&mut central, 20);
        le16(&mut central, 20);
        le16(&mut central, 0x0800);
        le16(&mut central, 0);
        le16(&mut central, 0);
        le16(&mut central, 0x21);
        le32(&mut central, crc);
        le32(&mut central, data.len() as u32);
        le32(&mut central, data.len() as u32);
        le16(&mut central, name.len() as u16);
        le16(&mut central, 0); // extra
        le16(&mut central, 0); // comment
        le16(&mut central, 0); // disk
        le16(&mut central, 0); // internal attrs
        le32(&mut central, 0o100644 << 16); // external attrs (unix mode)
        le32(&mut central, off);
        central.extend_from_slice(name.as_bytes());
    }
    let cd_off = out.len() as u32;
    let cd_len = central.len() as u32;
    out.extend_from_slice(&central);
    le32(&mut out, 0x0605_4b50);
    le16(&mut out, 0);
    le16(&mut out, 0);
    le16(&mut out, sorted.len() as u16);
    le16(&mut out, sorted.len() as u16);
    le32(&mut out, cd_len);
    le32(&mut out, cd_off);
    le16(&mut out, 0);
    out
}

/// Reads a module zip written by [`zip_write`] (or any store-only zip):
/// every entry must lie under `prefix/` and have a valid path.
pub fn zip_read(data: &[u8], prefix: &str) -> Result<Vec<FileEntry>, String> {
    let u16at = |o: usize| -> Result<u16, String> { data.get(o..o + 2).map(|b| u16::from_le_bytes([b[0], b[1]])).ok_or_else(|| "truncated zip".to_string()) };
    let u32at = |o: usize| -> Result<u32, String> { data.get(o..o + 4).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]])).ok_or_else(|| "truncated zip".to_string()) };
    if data.len() < 22 {
        return Err("not a zip archive".into());
    }
    let mut eocd = None;
    let lo = data.len().saturating_sub(22 + 65535);
    for i in (lo..=data.len() - 22).rev() {
        if data[i..i + 4] == [0x50, 0x4b, 0x05, 0x06] {
            eocd = Some(i);
            break;
        }
    }
    let eocd = eocd.ok_or("not a zip archive (no end of central directory)")?;
    let count = u16at(eocd + 10)? as usize;
    let mut off = u32at(eocd + 16)? as usize;
    let mut out = Vec::new();
    let want = format!("{prefix}/");
    for _ in 0..count {
        if u32at(off)? != 0x0201_4b50 {
            return Err("corrupt zip central directory".into());
        }
        let method = u16at(off + 10)?;
        let crc = u32at(off + 16)?;
        let csize = u32at(off + 20)? as usize;
        let usize_ = u32at(off + 24)? as usize;
        let nlen = u16at(off + 28)? as usize;
        let elen = u16at(off + 30)? as usize;
        let clen = u16at(off + 32)? as usize;
        let local = u32at(off + 42)? as usize;
        let name = std::str::from_utf8(data.get(off + 46..off + 46 + nlen).ok_or("truncated zip")?).map_err(|_| "zip entry name is not UTF-8")?.to_string();
        off += 46 + nlen + elen + clen;
        if name.ends_with('/') {
            continue;
        }
        let rel = name.strip_prefix(&want).ok_or_else(|| format!("zip entry `{name}` is outside `{want}`"))?;
        if !valid_file_path(rel) {
            return Err(format!("zip entry `{name}` has an invalid path"));
        }
        if method != 0 || csize != usize_ {
            return Err(format!("zip entry `{name}` is compressed (method {method}); module archives are stored"));
        }
        if u32at(local)? != 0x0403_4b50 {
            return Err("corrupt zip local header".into());
        }
        let start = local + 30 + u16at(local + 26)? as usize + u16at(local + 28)? as usize;
        let body = data.get(start..start + csize).ok_or("truncated zip entry")?;
        if crc32(body) != crc {
            return Err(format!("zip entry `{name}`: CRC mismatch"));
        }
        out.push((rel.to_string(), body.to_vec()));
    }
    out.sort_by(|a, b| a.0.cmp(&b.0));
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn base64_round_trips() {
        for s in [&b""[..], b"f", b"fo", b"foo", b"foob", b"fooba", b"foobar"] {
            assert_eq!(base64_decode(&base64(s)).unwrap(), s);
        }
        assert_eq!(base64(b"foobar"), "Zm9vYmFy");
        assert_eq!(base64(b"fo"), "Zm8=");
    }

    #[test]
    fn h1_is_order_independent_and_content_sensitive() {
        let a = vec![("b.sl".to_string(), b"x".to_vec()), ("a/c.sl".to_string(), b"y".to_vec())];
        let mut b = a.clone();
        b.reverse();
        assert_eq!(h1(&a), h1(&b));
        assert!(h1(&a).starts_with("h1:") && h1(&a).len() == 3 + 44);
        let hashes: Vec<(String, String)> = a.iter().map(|(p, d)| (p.clone(), sha256_hex(d))).collect();
        assert_eq!(h1_of_hashes(&hashes), h1(&a));
        b[0].1 = b"z".to_vec();
        assert_ne!(h1(&a), h1(&b));
        // Known answer: one empty file `a`.
        // sha256("") = e3b0c442...; summary = "<hex>  a\n".
        let summary = format!("{}  a\n", sha256_hex(b""));
        assert_eq!(h1(&[("a".into(), Vec::new())]), format!("h1:{}", base64(&Sha256::digest(summary.as_bytes()))));
    }

    #[test]
    fn zip_round_trips_and_is_reproducible() {
        let files = vec![("slang.toml".to_string(), b"[package]\n".to_vec()), ("src/lib.sl".to_string(), b"namespace a\n".to_vec())];
        let z = zip_write("example.com/a@v1.0.0", &files);
        assert_eq!(z, zip_write("example.com/a@v1.0.0", &files));
        assert_eq!(zip_read(&z, "example.com/a@v1.0.0").unwrap(), files);
        assert!(zip_read(&z, "example.com/b@v1.0.0").is_err());
        let mut bad = z.clone();
        let i = bad.windows(9).position(|w| w == b"namespace").unwrap();
        bad[i] = b'N';
        assert!(zip_read(&bad, "example.com/a@v1.0.0").unwrap_err().contains("CRC"));
    }
}
