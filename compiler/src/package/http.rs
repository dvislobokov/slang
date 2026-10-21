//! The small HTTP client of the proxy and checksum-log protocols:
//! `http://` over a plain TCP connection (HTTP/1.1, `Connection: close`),
//! `https://` through the system `curl` (certificates, proxies and
//! credentials come from the system, as with git), and `file://` URLs for
//! mirrors on disk.

use std::io::{Read, Write};
use std::net::TcpStream;
use std::time::Duration;

/// Fetches `url`. `Ok(None)` means "not found" (404 or 410, or a missing
/// file): the protocol's signal to try the next proxy.
pub fn get(url: &str) -> Result<Option<Vec<u8>>, String> {
    if let Some(path) = url.strip_prefix("file://") {
        return match std::fs::read(path) {
            Ok(b) => Ok(Some(b)),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(None),
            Err(e) => Err(format!("cannot read {path}: {e}")),
        };
    }
    if let Some(rest) = url.strip_prefix("http://") {
        return get_plain(url, rest, 5);
    }
    if url.starts_with("https://") {
        return get_curl(url);
    }
    Err(format!("unsupported URL `{url}` (expected http://, https:// or file://)"))
}

fn get_plain(url: &str, rest: &str, redirects: u32) -> Result<Option<Vec<u8>>, String> {
    let (hostport, path) = match rest.find('/') {
        Some(i) => (&rest[..i], &rest[i..]),
        None => (rest, "/"),
    };
    let addr = if hostport.contains(':') { hostport.to_string() } else { format!("{hostport}:80") };
    let err = |e: std::io::Error| format!("GET {url}: {e}");
    let mut s = TcpStream::connect(&addr).map_err(err)?;
    s.set_read_timeout(Some(Duration::from_secs(120))).map_err(err)?;
    write!(s, "GET {path} HTTP/1.1\r\nHost: {hostport}\r\nUser-Agent: slang/{}\r\nAccept: */*\r\nConnection: close\r\n\r\n", env!("CARGO_PKG_VERSION")).map_err(err)?;
    let mut raw = Vec::new();
    s.read_to_end(&mut raw).map_err(err)?;
    let head_end = raw.windows(4).position(|w| w == b"\r\n\r\n").ok_or_else(|| format!("GET {url}: malformed response"))?;
    let head = String::from_utf8_lossy(&raw[..head_end]).to_string();
    let mut body = raw[head_end + 4..].to_vec();
    let mut lines = head.lines();
    let status: u16 = lines.next().and_then(|l| l.split_whitespace().nth(1)).and_then(|c| c.parse().ok()).ok_or_else(|| format!("GET {url}: malformed status line"))?;
    let headers: Vec<(String, String)> = lines.filter_map(|l| l.split_once(':')).map(|(k, v)| (k.trim().to_ascii_lowercase(), v.trim().to_string())).collect();
    let header = |k: &str| headers.iter().find(|(h, _)| h == k).map(|(_, v)| v.clone());
    if header("transfer-encoding").is_some_and(|t| t.eq_ignore_ascii_case("chunked")) {
        body = dechunk(&body).ok_or_else(|| format!("GET {url}: malformed chunked body"))?;
    } else if let Some(n) = header("content-length").and_then(|n| n.parse::<usize>().ok()) {
        body.truncate(n);
    }
    match status {
        200 => Ok(Some(body)),
        404 | 410 => Ok(None),
        301 | 302 | 303 | 307 | 308 if redirects > 0 => {
            let loc = header("location").ok_or_else(|| format!("GET {url}: redirect without Location"))?;
            if let Some(r) = loc.strip_prefix("http://") {
                get_plain(&loc, r, redirects - 1)
            } else if loc.starts_with('/') {
                get_plain(url, &format!("{hostport}{loc}"), redirects - 1)
            } else {
                get(&loc)
            }
        }
        _ => Err(format!("GET {url}: HTTP {status}: {}", String::from_utf8_lossy(&body[..body.len().min(200)]).trim())),
    }
}

fn dechunk(mut b: &[u8]) -> Option<Vec<u8>> {
    let mut out = Vec::new();
    loop {
        let i = b.windows(2).position(|w| w == b"\r\n")?;
        let size = usize::from_str_radix(std::str::from_utf8(&b[..i]).ok()?.split(';').next()?.trim(), 16).ok()?;
        b = &b[i + 2..];
        if size == 0 {
            return Some(out);
        }
        out.extend_from_slice(b.get(..size)?);
        b = b.get(size + 2..)?;
    }
}

fn get_curl(url: &str) -> Result<Option<Vec<u8>>, String> {
    let tmp = std::env::temp_dir().join(format!("slang-curl-{}-{}", std::process::id(), unique()));
    let out = std::process::Command::new("curl")
        .args(["-sS", "-L", "--max-time", "300", "-o"])
        .arg(&tmp)
        .args(["-w", "%{http_code}", url])
        .output()
        .map_err(|e| format!("cannot run curl for {url}: {e} (https proxies need curl)"))?;
    let body = std::fs::read(&tmp).unwrap_or_default();
    let _ = std::fs::remove_file(&tmp);
    if !out.status.success() {
        return Err(format!("GET {url}: {}", String::from_utf8_lossy(&out.stderr).trim()));
    }
    match String::from_utf8_lossy(&out.stdout).trim() {
        "200" => Ok(Some(body)),
        "404" | "410" => Ok(None),
        code => Err(format!("GET {url}: HTTP {code}")),
    }
}

fn unique() -> u64 {
    use std::sync::atomic::{AtomicU64, Ordering};
    static N: AtomicU64 = AtomicU64::new(0);
    N.fetch_add(1, Ordering::Relaxed)
}
