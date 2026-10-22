//! `data.kafka` against a real Kafka broker.
//!
//! Ignored by default: there is no broker in a normal test run. Start one
//! (see `tests/fixtures/kafka/README.md`) and run
//!
//! ```sh
//! KAFKA_BROKERS=localhost:9092 cargo test --test kafka -- --ignored
//! KAFKA_BROKERS=127.0.0.1:19092 KAFKA_SASL_BROKERS=127.0.0.1:19094 cargo test --test kafka -- --ignored
//! ```
//!
//! The test builds `tests/fixtures/kafka/kafka_real.zl` with `--leak-check`,
//! runs it against the broker(s) and compares its output with
//! `kafka_real.out` (without `KAFKA_SASL_BROKERS` the six SASL lines are
//! replaced by `sasl skipped`). The protocol is also covered without a
//! broker by `tests/spec/stdlib5/kafka_*.zl` (byte vectors from the Java
//! client and an in-process mock broker).

use std::path::{Path, PathBuf};
use std::process::Command;

fn repo() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap().to_path_buf()
}

#[test]
#[ignore = "needs a Kafka broker: KAFKA_BROKERS=host:port cargo test --test kafka -- --ignored"]
fn kafka_real_broker() {
    let Ok(brokers) = std::env::var("KAFKA_BROKERS") else {
        eprintln!("skipping: KAFKA_BROKERS is not set");
        return;
    };
    let sasl = std::env::var("KAFKA_SASL_BROKERS").unwrap_or_default();
    if !repo().join("runtime/libzell_rt.a").is_file() {
        assert!(Command::new("make").arg("-C").arg(repo().join("runtime")).status().unwrap().success());
    }
    let dir = std::env::temp_dir().join(format!("zell_kafkatest_{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let src = repo().join("compiler/tests/fixtures/kafka/kafka_real.zl");
    let exe = dir.join("kafka_real");
    let b = Command::new(env!("CARGO_BIN_EXE_zell")).arg("build").arg(&src).arg("-o").arg(&exe).arg("--leak-check").output().unwrap();
    assert!(b.status.success(), "build failed:\n{}", String::from_utf8_lossy(&b.stderr));
    let run = Command::new(&exe).env("KAFKA_BROKERS", &brokers).env("KAFKA_SASL_BROKERS", &sasl).env("ZELL_NO_BACKTRACE", "1").output().unwrap();
    let stdout = String::from_utf8_lossy(&run.stdout).into_owned();
    let stderr = String::from_utf8_lossy(&run.stderr).into_owned();
    assert!(run.status.success(), "program failed ({:?}):\n{stdout}\n{stderr}", run.status.code());
    let mut want = std::fs::read_to_string(repo().join("compiler/tests/fixtures/kafka/kafka_real.out")).unwrap();
    if sasl.is_empty() {
        let lines: Vec<&str> = want.lines().filter(|l| !l.starts_with("sasl ")).collect();
        let mut out = String::new();
        for l in lines {
            if l == "deleted" {
                out.push_str("sasl skipped\n");
            }
            out.push_str(l);
            out.push('\n');
        }
        want = out;
    }
    assert_eq!(stdout, want, "output differs; stderr:\n{stderr}");
    let _ = std::fs::remove_dir_all(&dir);
}
