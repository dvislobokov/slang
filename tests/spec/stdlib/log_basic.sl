// test: std.log writes leveled lines to stderr, stdout stays clean
// status: pending
// stderr: INFO service started
namespace spec.stdlib.log_basic

import std.io
import std

func main() {
    log.Info("service started")
    log.Warn("disk almost full")
    println("stdout only")
}
