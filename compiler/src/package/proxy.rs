//! `zell proxy serve --dir <cache> [--addr host:port] [--upstream direct|off|<url>] [--sumdb]`:
//! a module proxy that serves the static layout of [`super::fetch`] from
//! a directory and fills it on demand from the upstream (git for
//! `direct`, another proxy for a URL). With `--sumdb` it also keeps the
//! checksum log under `/sumdb/` (`<dir>/sumdb/{log,latest,lookup/...}`),
//! appending a record the first time it serves a module version.
//!
//! Everything it writes is a plain file, so the filled directory can be
//! served by any static HTTP server afterwards (`--upstream off` does
//! exactly that).

use super::git::{self, GitRef};
use super::semver::Version;
use super::store::{escape_path, unescape_path, valid_module_path};
use super::sum;
use sha2::{Digest, Sha256};
use std::io::{BufRead, BufReader, Write};
use std::net::{TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

/// Where cache misses are filled from.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Upstream {
    Direct,
    Off,
    Proxy(String),
}

/// Proxy configuration.
#[derive(Clone, Debug)]
pub struct Config {
    pub dir: PathBuf,
    pub upstream: Upstream,
    pub sumdb: bool,
}

/// The shared state of a running proxy.
pub struct Proxy {
    cfg: Config,
    /// Serialises cache fills and log appends.
    fill: Mutex<()>,
}

/// Parses `proxy serve` arguments: (config, listen address).
pub fn parse_args(args: &[String]) -> Result<(Config, String), String> {
    let mut dir = None;
    let mut addr = "127.0.0.1:0".to_string();
    let mut upstream = Upstream::Direct;
    let mut sumdb = false;
    let mut it = args.iter();
    while let Some(a) = it.next() {
        let mut val = |name: &str| it.next().cloned().ok_or_else(|| format!("{name} needs a value"));
        match a.as_str() {
            "--dir" => dir = Some(PathBuf::from(val("--dir")?)),
            "--addr" => addr = val("--addr")?,
            "--upstream" => {
                upstream = match val("--upstream")?.as_str() {
                    "direct" => Upstream::Direct,
                    "off" => Upstream::Off,
                    u => Upstream::Proxy(u.trim_end_matches('/').to_string()),
                }
            }
            "--sumdb" => sumdb = true,
            _ => return Err(format!("unknown option `{a}` (usage: zell proxy serve --dir <cache> [--addr host:port] [--upstream direct|off|<url>] [--sumdb])")),
        }
    }
    let dir = dir.ok_or("proxy serve needs --dir <cache>")?;
    Ok((Config { dir, upstream, sumdb }, addr))
}

/// Binds `addr`, prints `listening on http://<addr>` and serves forever.
pub fn serve(cfg: Config, addr: &str) -> Result<(), String> {
    std::fs::create_dir_all(&cfg.dir).map_err(|e| format!("cannot create {}: {e}", cfg.dir.display()))?;
    let listener = TcpListener::bind(addr).map_err(|e| format!("cannot listen on {addr}: {e}"))?;
    let local = listener.local_addr().map_err(|e| e.to_string())?;
    println!("listening on http://{local}");
    let _ = std::io::stdout().flush();
    let proxy = Arc::new(Proxy { cfg, fill: Mutex::new(()) });
    for conn in listener.incoming() {
        let Ok(conn) = conn else { continue };
        let p = proxy.clone();
        std::thread::spawn(move || {
            let _ = p.handle(conn);
        });
    }
    Ok(())
}

fn respond(mut s: &TcpStream, status: u16, ctype: &str, body: &[u8]) -> std::io::Result<()> {
    let reason = match status {
        200 => "OK",
        400 => "Bad Request",
        404 => "Not Found",
        405 => "Method Not Allowed",
        _ => "Internal Server Error",
    };
    write!(s, "HTTP/1.1 {status} {reason}\r\nContent-Type: {ctype}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", body.len())?;
    s.write_all(body)?;
    s.flush()
}

fn percent_decode(s: &str) -> Option<String> {
    let b = s.as_bytes();
    let mut out = Vec::new();
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'%' {
            let h = std::str::from_utf8(b.get(i + 1..i + 3)?).ok()?;
            out.push(u8::from_str_radix(h, 16).ok()?);
            i += 3;
        } else {
            out.push(b[i]);
            i += 1;
        }
    }
    String::from_utf8(out).ok()
}

fn write_file(path: &Path, data: &[u8]) -> Result<(), String> {
    let dir = path.parent().unwrap();
    std::fs::create_dir_all(dir).map_err(|e| format!("cannot create {}: {e}", dir.display()))?;
    let tmp = dir.join(format!(".tmp-{}", path.file_name().unwrap().to_string_lossy()));
    std::fs::write(&tmp, data).map_err(|e| e.to_string())?;
    std::fs::rename(&tmp, path).map_err(|e| e.to_string())
}

/// A request for the module layout: (module path, file under `@v/`).
fn parse_module_request(path: &str) -> Option<(String, String)> {
    let (module, file) = path.trim_start_matches('/').split_once("/@v/")?;
    let module = unescape_path(module)?;
    (valid_module_path(&module) && !file.contains('/') && !file.is_empty()).then(|| (module, file.to_string()))
}

impl Proxy {
    fn handle(&self, conn: TcpStream) -> std::io::Result<()> {
        let mut r = BufReader::new(&conn);
        let mut line = String::new();
        r.read_line(&mut line)?;
        loop {
            let mut h = String::new();
            if r.read_line(&mut h)? == 0 || h == "\r\n" || h == "\n" {
                break;
            }
        }
        let mut parts = line.split_whitespace();
        let (method, target) = (parts.next().unwrap_or(""), parts.next().unwrap_or("/"));
        if method != "GET" && method != "HEAD" {
            return respond(&conn, 405, "text/plain", b"only GET\n");
        }
        let Some(path) = percent_decode(target.split('?').next().unwrap_or("/")) else { return respond(&conn, 400, "text/plain", b"bad path\n") };
        if path.split('/').any(|s| s == ".." || s == ".") {
            return respond(&conn, 400, "text/plain", b"bad path\n");
        }
        match self.get(&path) {
            Ok(Some((ctype, body))) => respond(&conn, 200, ctype, &body),
            Ok(None) => respond(&conn, 404, "text/plain", b"not found\n"),
            Err(e) => respond(&conn, 404, "text/plain", format!("not found: {e}\n").as_bytes()),
        }
    }

    /// The response for a path: `None` = 404.
    fn get(&self, path: &str) -> Result<Option<(&'static str, Vec<u8>)>, String> {
        let dir = &self.cfg.dir;
        if let Some(rest) = path.strip_prefix("/sumdb/") {
            if !self.cfg.sumdb {
                return Ok(None);
            }
            let file = dir.join("sumdb").join(rest);
            if let Some(key) = rest.strip_prefix("lookup/") {
                if !file.is_file() {
                    let (m, v) = key.rsplit_once('@').ok_or("bad lookup")?;
                    let m = unescape_path(m).ok_or("bad module path")?;
                    self.ensure_module(&m, v)?;
                }
            }
            return Ok(std::fs::read(&file).ok().map(|b| ("text/plain; charset=utf-8", b)));
        }
        let Some((module, file)) = parse_module_request(path) else { return Ok(None) };
        let base = dir.join(escape_path(&module)).join("@v");
        if file == "list" {
            let fresh = self.list(&module);
            if let Ok(Some(text)) = &fresh {
                write_file(&base.join("list"), text.as_bytes())?;
            }
            return Ok(std::fs::read(base.join("list")).ok().map(|b| ("text/plain; charset=utf-8", b)));
        }
        let (ver, ext) = file.rsplit_once('.').ok_or("bad file")?;
        if !matches!(ext, "info" | "manifest" | "zip") || Version::parse(ver).is_none() {
            return Ok(None);
        }
        if !base.join(&file).is_file() {
            self.ensure_module(&module, ver)?;
        }
        let ctype = match ext {
            "info" => "application/json",
            "zip" => "application/zip",
            _ => "text/plain; charset=utf-8",
        };
        Ok(std::fs::read(base.join(&file)).ok().map(|b| (ctype, b)))
    }

    /// The version list from the upstream (`None` when there is none).
    fn list(&self, module: &str) -> Result<Option<String>, String> {
        match &self.cfg.upstream {
            Upstream::Off => Ok(None),
            Upstream::Direct => {
                let tags = git::list_tags(&git::url_of(module))?;
                Ok(Some(tags.iter().map(|t| format!("{}\n", t.version.tag())).collect()))
            }
            Upstream::Proxy(u) => Ok(super::http::get(&format!("{u}/{}/@v/list", escape_path(module)))?.map(|b| String::from_utf8_lossy(&b).to_string())),
        }
    }

    /// Fills `<module>/@v/<version>.{info,manifest,zip}` (and the log
    /// record) unless present.
    fn ensure_module(&self, module: &str, version: &str) -> Result<(), String> {
        let _guard = self.fill.lock().unwrap();
        let base = self.cfg.dir.join(escape_path(module)).join("@v");
        let zip = base.join(format!("{version}.zip"));
        if !zip.is_file() {
            match &self.cfg.upstream {
                Upstream::Off => return Err(format!("{module}@{version} is not cached")),
                Upstream::Direct => {
                    let want = Version::parse(version).ok_or("bad version")?;
                    let url = git::url_of(module);
                    let tags = git::list_tags(&url)?;
                    let tag = tags.iter().find(|t| t.version == want).ok_or_else(|| format!("{module}: no tag for {version}"))?;
                    let (files, (commit, time)) = super::fetch::checkout_tree(&url, &GitRef::Tag(tag.name.clone()))?;
                    let manifest = super::fetch::manifest_entry(&files);
                    let t = if time.len() == 14 { format!("{}-{}-{}T{}:{}:{}Z", &time[0..4], &time[4..6], &time[6..8], &time[8..10], &time[10..12], &time[12..14]) } else { time.clone() };
                    let info = serde_json::json!({
                        "Version": version,
                        "Time": t,
                        "Manifest": manifest.as_ref().map(|m| m.0.clone()),
                        "Origin": { "VCS": "git", "URL": url, "Ref": format!("refs/tags/{}", tag.name), "Hash": commit },
                    });
                    write_file(&base.join(format!("{version}.info")), format!("{}\n", serde_json::to_string_pretty(&info).unwrap()).as_bytes())?;
                    if let Some((_, bytes)) = &manifest {
                        write_file(&base.join(format!("{version}.manifest")), bytes)?;
                    }
                    write_file(&zip, &sum::zip_write(&format!("{module}@{version}"), &files))?;
                }
                Upstream::Proxy(u) => {
                    let pre = format!("{u}/{}/@v/{version}", escape_path(module));
                    for ext in ["info", "manifest", "zip"] {
                        if let Some(b) = super::http::get(&format!("{pre}.{ext}"))? {
                            write_file(&base.join(format!("{version}.{ext}")), &b)?;
                        } else if ext != "manifest" {
                            return Err(format!("{module}@{version}: upstream has no .{ext}"));
                        }
                    }
                }
            }
        }
        if self.cfg.sumdb {
            self.log_append(module, version, &zip)?;
        }
        Ok(())
    }

    /// Appends the record of `module@version` to the checksum log (once).
    fn log_append(&self, module: &str, version: &str, zip: &Path) -> Result<(), String> {
        let sumdir = self.cfg.dir.join("sumdb");
        let lookup = sumdir.join("lookup").join(format!("{}@{version}", escape_path(module)));
        if lookup.is_file() {
            return Ok(());
        }
        let data = std::fs::read(zip).map_err(|e| e.to_string())?;
        let files = sum::zip_read(&data, &format!("{module}@{version}"))?;
        let mut record = format!("{module} {version} {}\n", sum::h1(&files));
        if let Some(m) = super::fetch::manifest_entry(&files) {
            record.push_str(&format!("{module} {version}/manifest {}\n", sum::h1(&[m])));
        }
        let log_path = sumdir.join("log");
        let mut log = std::fs::read(&log_path).unwrap_or_default();
        let n = String::from_utf8_lossy(&log).lines().filter(|l| !l.contains("/manifest ")).count();
        log.extend_from_slice(record.as_bytes());
        write_file(&log_path, &log)?;
        write_file(&lookup, format!("{n}\n{record}").as_bytes())?;
        let head = format!("zell-sumdb v1\n{}\n{}\n", n + 1, sum::base64(&Sha256::digest(&log)));
        write_file(&sumdir.join("latest"), head.as_bytes())
    }
}
