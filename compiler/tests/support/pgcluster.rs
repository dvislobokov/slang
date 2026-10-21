//! A temporary PostgreSQL cluster for integration tests (`database.rs`,
//! `data.rs`): `initdb` into a temporary directory (as the `postgres`
//! user when running as root), a free TCP port and a Unix socket in the
//! directory; dropping the [`Cluster`] stops the server.
#![allow(dead_code)]

use std::net::TcpListener;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

pub fn repo() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap().to_path_buf()
}

/// The directory of `initdb`, `pg_ctl`, `postgres`, `psql`, if installed.
pub fn pg_bindir() -> Option<PathBuf> {
    if let Ok(o) = Command::new("pg_config").arg("--bindir").output() {
        if o.status.success() {
            let d = PathBuf::from(String::from_utf8_lossy(&o.stdout).trim());
            if d.join("initdb").is_file() {
                return Some(d);
            }
        }
    }
    let mut dirs: Vec<PathBuf> = std::fs::read_dir("/usr/lib/postgresql").ok()?.filter_map(|e| e.ok()).map(|e| e.path().join("bin")).filter(|p| p.join("initdb").is_file()).collect();
    dirs.sort();
    dirs.pop()
}

pub fn is_root() -> bool {
    Command::new("id").arg("-u").output().map(|o| String::from_utf8_lossy(&o.stdout).trim() == "0").unwrap_or(false)
}

/// Runs a PostgreSQL tool (as `postgres` when root).
pub fn pg(bin: &Path, tool: &str, args: &[&str], root: bool) -> std::process::Output {
    let exe = bin.join(tool);
    let mut cmd = if root {
        let mut c = Command::new("runuser");
        c.arg("-u").arg("postgres").arg("--").arg(&exe);
        c
    } else {
        Command::new(&exe)
    };
    cmd.args(args).stdin(Stdio::null()).output().expect("cannot run a PostgreSQL tool")
}

/// A running server; stopped on drop.
pub struct Cluster {
    pub bin: PathBuf,
    pub data: PathBuf,
    pub dir: PathBuf,
    pub root: bool,
    pub port: String,
}

impl Drop for Cluster {
    fn drop(&mut self) {
        if self.port.is_empty() {
            return;
        }
        let d = self.data.to_string_lossy().to_string();
        let _ = pg(&self.bin, "pg_ctl", &["-D", &d, "-m", "immediate", "-w", "stop"], self.root);
    }
}

impl Cluster {
    /// `postgres://postgres@127.0.0.1:<port>/postgres?sslmode=disable`.
    pub fn dsn(&self) -> String {
        format!("postgres://postgres@127.0.0.1:{}/postgres?sslmode=disable", self.port)
    }

    /// Runs SQL with psql as the superuser.
    pub fn psql(&self, sql: &str) -> std::process::Output {
        let d = self.dir.to_string_lossy().to_string();
        pg(&self.bin, "psql", &["-h", &d, "-p", &self.port, "-U", "postgres", "-q", "-v", "ON_ERROR_STOP=1", "-c", sql], self.root)
    }
}

/// Whether the cluster prerequisites exist (a note is printed otherwise).
pub fn available() -> Option<PathBuf> {
    let Some(bin) = pg_bindir() else {
        eprintln!("skipping: PostgreSQL binaries (initdb, pg_ctl) not found");
        return None;
    };
    if is_root() && Command::new("id").arg("postgres").output().map(|o| !o.status.success()).unwrap_or(true) {
        eprintln!("skipping: running as root without a postgres user");
        return None;
    }
    Some(bin)
}

/// A free TCP port.
pub fn free_port() -> String {
    TcpListener::bind("127.0.0.1:0").unwrap().local_addr().unwrap().port().to_string()
}

/// Initialises a cluster in `dir/data` (trust authentication); files can
/// be added to `dir` before [`Cluster::launch`] starts it.
pub fn init(dir: &Path) -> Cluster {
    let bin = pg_bindir().expect("PostgreSQL binaries");
    let root = is_root();
    let _ = std::fs::remove_dir_all(dir);
    std::fs::create_dir_all(dir).unwrap();
    if root {
        assert!(Command::new("chown").arg("postgres").arg(dir).status().unwrap().success());
    }
    let data = dir.join("data");
    let dd = data.to_string_lossy().to_string();
    let out = pg(&bin, "initdb", &["-D", &dd, "-A", "trust", "-U", "postgres", "--no-sync"], root);
    assert!(out.status.success(), "initdb failed:\n{}", String::from_utf8_lossy(&out.stderr));
    Cluster { bin, data, dir: dir.to_path_buf(), root, port: String::new() }
}

impl Cluster {
    /// Starts the server on a free port (`hba` replaces pg_hba.conf if
    /// given; `extra`: more server options). The server is only stopped
    /// on drop once it is launched.
    pub fn launch(mut self, hba: Option<&str>, extra: &str) -> Cluster {
        if let Some(h) = hba {
            std::fs::write(self.data.join("pg_hba.conf"), h).unwrap();
        }
        self.port = free_port();
        let ds = self.dir.to_string_lossy().to_string();
        let dd = self.data.to_string_lossy().to_string();
        let opts = format!("-p {} -k {ds} -c listen_addresses=127.0.0.1 -c fsync=off {extra}", self.port);
        let log = format!("{ds}/server.log");
        let start = pg(&self.bin, "pg_ctl", &["-D", &dd, "-o", &opts, "-l", &log, "-w", "-t", "30", "start"], self.root);
        assert!(start.status.success(), "pg_ctl start failed:\n{}\n{}", String::from_utf8_lossy(&start.stderr), std::fs::read_to_string(&log).unwrap_or_default());
        self
    }
}

/// [`init`] and [`Cluster::launch`] with default settings.
pub fn start(dir: &Path) -> Cluster {
    init(dir).launch(None, "")
}
