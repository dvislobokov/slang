//! `database.postgres` against a real PostgreSQL server.
//!
//! The test starts a temporary cluster: `initdb` into a temporary
//! directory (as the `postgres` user when running as root), TLS with a
//! certificate made by `openssl`, `pg_hba.conf` rules for trust, cleartext
//! password, MD5, SCRAM-SHA-256 and TLS-only users, a free TCP port and a
//! Unix socket in the directory. It then runs
//! `tests/fixtures/database/postgres_real.zl` (built with `--leak-check`)
//! and compares its output with `postgres_real.out`, and stops the server.
//! Without PostgreSQL binaries (`initdb`, `pg_ctl`, `postgres`) or
//! `openssl` the test is skipped with a note; the protocol is still covered
//! by the mock-server spec test `tests/spec/stdlib4/postgres_mock.zl`.

use std::process::Command;

#[path = "support/pgcluster.rs"]
mod pgcluster;
use pgcluster::{pg, repo};

#[test]
fn postgres_real_server() {
    let Some(bin) = pgcluster::available() else { return };
    if Command::new("openssl").arg("version").output().map(|o| !o.status.success()).unwrap_or(true) {
        eprintln!("skipping: openssl not found");
        return;
    }
    let root = pgcluster::is_root();
    let dir = std::env::temp_dir().join(format!("zell_pgtest_{}", std::process::id()));
    let cluster = pgcluster::init(&dir);
    let ds = dir.to_string_lossy().to_string();
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
    let hba = "local all all trust\nhost all md5u 127.0.0.1/32 md5\nhost all scramu 127.0.0.1/32 scram-sha-256\nhost all pwu 127.0.0.1/32 password\nhostssl all sslu 127.0.0.1/32 trust\nhostnossl all sslu 127.0.0.1/32 reject\nhost all all 127.0.0.1/32 trust\n";
    let _cluster = cluster.launch(Some(hba), &format!("-c ssl=on -c ssl_cert_file={ds}/server.crt -c ssl_key_file={ds}/server.key"));
    let port = _cluster.port.clone();
    let setup = pg(
        &bin,
        "psql",
        &["-h", &ds, "-p", &port, "-U", "postgres", "-q", "-v", "ON_ERROR_STOP=1", "-c", "set password_encryption='md5'; create user md5u password 'md5pw';", "-c", "set password_encryption='scram-sha-256'; create user scramu password 'scr@m pw'; create user pwu password 'plainpw'; create user sslu;"],
        root,
    );
    assert!(setup.status.success(), "psql setup failed:\n{}", String::from_utf8_lossy(&setup.stderr));

    if !repo().join("runtime/libzell_rt.a").is_file() {
        assert!(Command::new("make").arg("-C").arg(repo().join("runtime")).status().unwrap().success());
    }
    let src = repo().join("compiler/tests/fixtures/database/postgres_real.zl");
    let exe = dir.join("postgres_real");
    let b = Command::new(env!("CARGO_BIN_EXE_zell")).arg("build").arg(&src).arg("-o").arg(&exe).arg("--leak-check").output().unwrap();
    assert!(b.status.success(), "build failed:\n{}", String::from_utf8_lossy(&b.stderr));
    let run = Command::new(&exe).env("PGTEST_PORT", &port).env("PGTEST_DIR", &ds).env("ZELL_NO_BACKTRACE", "1").output().unwrap();
    let stdout = String::from_utf8_lossy(&run.stdout).into_owned();
    let stderr = String::from_utf8_lossy(&run.stderr).into_owned();
    assert!(run.status.success(), "program failed ({:?}):\n{stdout}\n{stderr}", run.status.code());
    let want = std::fs::read_to_string(repo().join("compiler/tests/fixtures/database/postgres_real.out")).unwrap();
    assert_eq!(stdout, want, "output differs; stderr:\n{stderr}");
    drop(_cluster);
    let _ = std::fs::remove_dir_all(&dir);
}
