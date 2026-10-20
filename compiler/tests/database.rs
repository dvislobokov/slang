//! `std.database.postgres` against a real PostgreSQL server.
//!
//! The test starts a temporary cluster: `initdb` into a temporary
//! directory (as the `postgres` user when running as root), TLS with a
//! certificate made by `openssl`, `pg_hba.conf` rules for trust, cleartext
//! password, MD5, SCRAM-SHA-256 and TLS-only users, a free TCP port and a
//! Unix socket in the directory. It then runs
//! `tests/fixtures/database/postgres_real.sl` (built with `--leak-check`)
//! and compares its output with `postgres_real.out`, and stops the server.
//! Without PostgreSQL binaries (`initdb`, `pg_ctl`, `postgres`) or
//! `openssl` the test is skipped with a note; the protocol is still covered
//! by the mock-server spec test `tests/spec/stdlib4/postgres_mock.sl`.

use std::net::TcpListener;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

fn repo() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap().to_path_buf()
}

fn pg_bindir() -> Option<PathBuf> {
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

fn is_root() -> bool {
    Command::new("id").arg("-u").output().map(|o| String::from_utf8_lossy(&o.stdout).trim() == "0").unwrap_or(false)
}

/// Runs a PostgreSQL tool (as `postgres` when root).
fn pg(bin: &Path, tool: &str, args: &[&str], root: bool) -> std::process::Output {
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

struct Cluster {
    bin: PathBuf,
    data: PathBuf,
    root: bool,
}

impl Drop for Cluster {
    fn drop(&mut self) {
        let d = self.data.to_string_lossy().to_string();
        let _ = pg(&self.bin, "pg_ctl", &["-D", &d, "-m", "immediate", "-w", "stop"], self.root);
    }
}

#[test]
fn postgres_real_server() {
    let Some(bin) = pg_bindir() else {
        eprintln!("skipping: PostgreSQL binaries (initdb, pg_ctl) not found");
        return;
    };
    if Command::new("openssl").arg("version").output().map(|o| !o.status.success()).unwrap_or(true) {
        eprintln!("skipping: openssl not found");
        return;
    }
    let root = is_root();
    if root && Command::new("id").arg("postgres").output().map(|o| !o.status.success()).unwrap_or(true) {
        eprintln!("skipping: running as root without a postgres user");
        return;
    }
    let dir = std::env::temp_dir().join(format!("slang_pgtest_{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    if root {
        assert!(Command::new("chown").arg("postgres").arg(&dir).status().unwrap().success());
    }
    let ds = dir.to_string_lossy().to_string();
    let data = dir.join("data");
    let dd = data.to_string_lossy().to_string();
    let out = pg(&bin, "initdb", &["-D", &dd, "-A", "trust", "-U", "postgres", "--no-sync"], root);
    assert!(out.status.success(), "initdb failed:\n{}", String::from_utf8_lossy(&out.stderr));
    let ok = Command::new("openssl")
        .current_dir(&dir)
        .args(["req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes", "-keyout", "server.key", "-out", "server.crt", "-days", "3650", "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1"])
        .output()
        .unwrap();
    assert!(ok.status.success(), "openssl failed");
    if root {
        Command::new("chown").arg("postgres").arg(dir.join("server.key")).arg(dir.join("server.crt")).status().unwrap();
    }
    Command::new("chmod").arg("600").arg(dir.join("server.key")).status().unwrap();
    std::fs::write(
        data.join("pg_hba.conf"),
        "local all all trust\nhost all md5u 127.0.0.1/32 md5\nhost all scramu 127.0.0.1/32 scram-sha-256\nhost all pwu 127.0.0.1/32 password\nhostssl all sslu 127.0.0.1/32 trust\nhostnossl all sslu 127.0.0.1/32 reject\nhost all all 127.0.0.1/32 trust\n",
    )
    .unwrap();
    let port = TcpListener::bind("127.0.0.1:0").unwrap().local_addr().unwrap().port().to_string();
    let opts = format!("-p {port} -k {ds} -c listen_addresses=127.0.0.1 -c ssl=on -c ssl_cert_file={ds}/server.crt -c ssl_key_file={ds}/server.key -c fsync=off");
    let log = format!("{ds}/server.log");
    let start = pg(&bin, "pg_ctl", &["-D", &dd, "-o", &opts, "-l", &log, "-w", "-t", "30", "start"], root);
    let _cluster = Cluster { bin: bin.clone(), data: data.clone(), root };
    assert!(start.status.success(), "pg_ctl start failed:\n{}\n{}", String::from_utf8_lossy(&start.stderr), std::fs::read_to_string(&log).unwrap_or_default());
    let setup = pg(
        &bin,
        "psql",
        &["-h", &ds, "-p", &port, "-U", "postgres", "-q", "-v", "ON_ERROR_STOP=1", "-c", "set password_encryption='md5'; create user md5u password 'md5pw';", "-c", "set password_encryption='scram-sha-256'; create user scramu password 'scr@m pw'; create user pwu password 'plainpw'; create user sslu;"],
        root,
    );
    assert!(setup.status.success(), "psql setup failed:\n{}", String::from_utf8_lossy(&setup.stderr));

    if !repo().join("runtime/libslang_rt.a").is_file() {
        assert!(Command::new("make").arg("-C").arg(repo().join("runtime")).status().unwrap().success());
    }
    let src = repo().join("compiler/tests/fixtures/database/postgres_real.sl");
    let exe = dir.join("postgres_real");
    let b = Command::new(env!("CARGO_BIN_EXE_slang")).arg("build").arg(&src).arg("-o").arg(&exe).arg("--leak-check").output().unwrap();
    assert!(b.status.success(), "build failed:\n{}", String::from_utf8_lossy(&b.stderr));
    let run = Command::new(&exe).env("PGTEST_PORT", &port).env("PGTEST_DIR", &ds).env("SLANG_NO_BACKTRACE", "1").output().unwrap();
    let stdout = String::from_utf8_lossy(&run.stdout).into_owned();
    let stderr = String::from_utf8_lossy(&run.stderr).into_owned();
    assert!(run.status.success(), "program failed ({:?}):\n{stdout}\n{stderr}", run.status.code());
    let want = std::fs::read_to_string(repo().join("compiler/tests/fixtures/database/postgres_real.out")).unwrap();
    assert_eq!(stdout, want, "output differs; stderr:\n{stderr}");
    drop(_cluster);
    let _ = std::fs::remove_dir_all(&dir);
}
