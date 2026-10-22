//! `std.data.redis` against real `redis-server` processes.
//!
//! The test starts, on free ports, in a temporary directory:
//! a plain server; a TLS server with a password (certificate made by
//! `openssl`, skipped without it); a Sentinel setup (master, replica and
//! one sentinel with quorum 1 and `down-after-milliseconds 1000`), whose
//! master the program kills with SIGKILL to check that the client follows
//! the failover to the promoted replica; and a 3-master Cluster
//! (`redis-cli --cluster create`), where the program migrates a slot to see
//! ASK and MOVED redirects. It then runs
//! `tests/fixtures/redis/redis_real.zl` (built with `--leak-check`) and
//! compares its output with `redis_real.out`. Without `redis-server` and
//! `redis-cli` the test is skipped with a note; the protocol is still
//! covered by the mock-server spec tests `tests/spec/stdlib5/redis_*.zl`.

use std::net::TcpListener;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

fn repo() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap().to_path_buf()
}

fn have(tool: &str) -> bool {
    Command::new(tool).arg("--version").stdout(Stdio::null()).stderr(Stdio::null()).status().map(|s| s.success()).unwrap_or(false)
}

fn free_port() -> u16 {
    TcpListener::bind("127.0.0.1:0").unwrap().local_addr().unwrap().port()
}

/// Spawned servers, killed when dropped.
struct Servers(Vec<Child>);

impl Drop for Servers {
    fn drop(&mut self) {
        for c in &mut self.0 {
            let _ = c.kill();
            let _ = c.wait();
        }
    }
}

fn spawn(dir: &Path, args: &[&str], servers: &mut Servers) -> u32 {
    let child = Command::new("redis-server").current_dir(dir).args(args).stdout(Stdio::null()).stderr(Stdio::null()).spawn().expect("cannot start redis-server");
    let pid = child.id();
    servers.0.push(child);
    pid
}

fn cli(port: u16, args: &[&str]) -> String {
    let o = Command::new("redis-cli").arg("-p").arg(port.to_string()).args(args).output().unwrap();
    String::from_utf8_lossy(&o.stdout).trim().to_string()
}

fn wait_ping(port: u16) {
    let start = Instant::now();
    while start.elapsed() < Duration::from_secs(10) {
        if cli(port, &["ping"]) == "PONG" {
            return;
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    panic!("redis-server on port {port} did not start");
}

#[test]
fn redis_real_servers() {
    if !have("redis-server") || !have("redis-cli") {
        eprintln!("skipping: redis-server / redis-cli not found");
        return;
    }
    let dir = std::env::temp_dir().join(format!("zell_redistest_{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    let mut servers = Servers(Vec::new());
    let mut envs: Vec<(String, String)> = Vec::new();

    // plain server
    let plain = free_port();
    spawn(&dir, &["--port", &plain.to_string(), "--save", "", "--appendonly", "no"], &mut servers);
    envs.push(("REDIS_PORT".into(), plain.to_string()));

    // TLS server with a password
    let tls_ok = Command::new("openssl")
        .current_dir(&dir)
        .args(["req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes", "-keyout", "server.key", "-out", "server.crt", "-days", "3650", "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1"])
        .output()
        .map(|o| o.status.success())
        .unwrap_or(false);
    let mut tls_port = 0;
    if tls_ok {
        tls_port = free_port();
        spawn(
            &dir,
            &["--port", "0", "--tls-port", &tls_port.to_string(), "--tls-cert-file", "server.crt", "--tls-key-file", "server.key", "--tls-ca-cert-file", "server.crt", "--tls-auth-clients", "no", "--requirepass", "sekret", "--save", ""],
            &mut servers,
        );
        envs.push(("REDIS_TLS_PORT".into(), tls_port.to_string()));
        envs.push(("REDIS_TLS_CA".into(), dir.join("server.crt").to_string_lossy().to_string()));
    }

    // Sentinel: master, replica, sentinel
    let (mport, rport, sport) = (free_port(), free_port(), free_port());
    let mpid = spawn(&dir, &["--port", &mport.to_string(), "--save", ""], &mut servers);
    spawn(&dir, &["--port", &rport.to_string(), "--save", "", "--replicaof", "127.0.0.1", &mport.to_string()], &mut servers);
    let sconf = dir.join("sentinel.conf");
    std::fs::write(&sconf, format!("port {sport}\nsentinel monitor mymaster 127.0.0.1 {mport} 1\nsentinel down-after-milliseconds mymaster 1000\nsentinel failover-timeout mymaster 5000\n")).unwrap();
    spawn(&dir, &[sconf.to_str().unwrap(), "--sentinel"], &mut servers);
    envs.push(("SENTINEL_PORT".into(), sport.to_string()));
    envs.push(("MASTER_PORT".into(), mport.to_string()));
    envs.push(("REPLICA_PORT".into(), rport.to_string()));
    envs.push(("MASTER_PID".into(), mpid.to_string()));

    // Cluster: 3 masters
    let cports: Vec<u16> = (0..3).map(|_| free_port()).collect();
    for p in &cports {
        let d = dir.join(format!("cluster{p}"));
        std::fs::create_dir_all(&d).unwrap();
        spawn(&d, &["--port", &p.to_string(), "--cluster-enabled", "yes", "--cluster-config-file", "nodes.conf", "--save", "", "--appendonly", "no"], &mut servers);
    }
    for p in [plain, mport, rport, sport].iter().chain(cports.iter()) {
        wait_ping(*p);
    }
    let addrs: Vec<String> = cports.iter().map(|p| format!("127.0.0.1:{p}")).collect();
    let mut args: Vec<&str> = vec!["--cluster", "create"];
    args.extend(addrs.iter().map(|s| s.as_str()));
    args.push("--cluster-yes");
    let created = Command::new("redis-cli").args(&args).output().unwrap();
    assert!(created.status.success(), "cluster create failed:\n{}", String::from_utf8_lossy(&created.stdout));
    let start = Instant::now();
    while !cli(cports[0], &["cluster", "info"]).contains("cluster_state:ok") {
        assert!(start.elapsed() < Duration::from_secs(20), "cluster did not become ok");
        std::thread::sleep(Duration::from_millis(100));
    }
    envs.push(("CLUSTER_PORTS".into(), cports.iter().map(|p| p.to_string()).collect::<Vec<_>>().join(",")));
    // the sentinel must know the replica before the failover
    let start = Instant::now();
    while !cli(sport, &["sentinel", "replicas", "mymaster"]).contains(&rport.to_string()) {
        assert!(start.elapsed() < Duration::from_secs(20), "sentinel did not discover the replica");
        std::thread::sleep(Duration::from_millis(100));
    }

    if !repo().join("runtime/libzell_rt.a").is_file() {
        assert!(Command::new("make").arg("-C").arg(repo().join("runtime")).status().unwrap().success());
    }
    let src = repo().join("compiler/tests/fixtures/redis/redis_real.zl");
    let exe = dir.join("redis_real");
    let b = Command::new(env!("CARGO_BIN_EXE_zell")).arg("build").arg(&src).arg("-o").arg(&exe).arg("--leak-check").output().unwrap();
    assert!(b.status.success(), "build failed:\n{}", String::from_utf8_lossy(&b.stderr));
    let mut cmd = Command::new(&exe);
    cmd.env("ZELL_NO_BACKTRACE", "1");
    for (k, v) in &envs {
        cmd.env(k, v);
    }
    let run = cmd.output().unwrap();
    let stdout = String::from_utf8_lossy(&run.stdout).into_owned();
    let stderr = String::from_utf8_lossy(&run.stderr).into_owned();
    assert!(run.status.success(), "program failed ({:?}):\n{stdout}\n{stderr}", run.status.code());
    let mut want = std::fs::read_to_string(repo().join("compiler/tests/fixtures/redis/redis_real.out")).unwrap();
    if tls_port == 0 {
        want = want.replace("tls over tls\ntls auth WRONGPASS\ntls untrusted certificate rejected\n", "tls skipped\n");
    }
    assert_eq!(stdout, want, "output differs; stderr:\n{stderr}");
    drop(servers);
    let _ = std::fs::remove_dir_all(&dir);
}
