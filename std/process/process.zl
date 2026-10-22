// std.process: running child processes (Go's os/exec), asynchronously.
//
//   var r = await process.Run("git", ["status", "--short"])?
//   println(r.ExitCode, r.Stdout)
//
//   var p = process.Start("sort", [], process.Options{Stdin: process.Pipe, Stdout: process.Pipe})?
//   _ = await p.Stdin.Write(strings.Bytes("b\na\n"))?
//   p.Stdin.Close()
//   var out = await p.Stdout.ReadAll()?
//   var code = await p.Wait()?
//
// The pipes implement std.io.AsyncReader / AsyncWriter (waits go through
// the scheduler's poller; no worker thread blocks). Cancelling the task
// that waits (task.WithTimeout, Options.Timeout) kills the child.
// Runtime: runtime/src/std_process.c.
namespace std.process

// Stream modes for Options.Stdin/Stdout/Stderr.
// Inherit (0): the parent's stream; Pipe (1): a pipe to this process;
// Null (2): /dev/null. (The constants process.Inherit, Pipe, Null.)
struct Options {
    // Working directory ("" = the current one).
    Dir string
    // Extra environment entries "KEY=VALUE" (override inherited ones).
    Env []string
    // Start from an empty environment instead of inheriting this one.
    ClearEnv bool
    Stdin    int
    Stdout   int
    Stderr   int
    // Kill the child if it runs longer (zero: no limit).
    Timeout std.time.Duration
}

// The read end of a child's stdout/stderr.
struct ReadPipe {
    st *pipeFd

    // Up to n bytes as they arrive; empty at the end of the stream
    // (std.io.AsyncReader).
    func Read(n int) Task<[]byte!> {
        return readPipeRead(*this, n)
    }

    // Everything until the end of the stream.
    func ReadAll() Task<[]byte!> {
        return readPipeReadAll(*this)
    }

    func Close() {
        this.st.close()
    }
}

// The write end of a child's stdin.
struct WritePipe {
    st *pipeFd

    // Writes all of data (std.io.AsyncWriter).
    func Write(data []byte) Task<int!> {
        return writePipeWrite(*this, data)
    }

    func WriteString(s string) Task<int!> {
        return writePipeWriteString(*this, s)
    }

    // Closes stdin (the child sees end of input).
    func Close() {
        this.st.close()
    }
}

struct pipeFd {
    fd int

    func close() {
        if this.fd >= 0 {
            rtClose(this.fd)
            this.fd = -1
        }
    }

    func Drop() {
        this.close()
    }
}

func makeBuf(n int) []byte {
    var b []byte = []
    for _ in 0..n {
        b = append(b, 0)
    }
    return b
}

struct procState {
    pid   int
    pidfd int
    done  bool
    code  int

    func Drop() {
        if this.pidfd >= 0 {
            rtClose(this.pidfd)
        }
    }
}

// A started child process.
struct Process {
    st *procState
    // Pipes for the streams started with Pipe (otherwise closed/empty).
    Stdin   WritePipe
    Stdout  ReadPipe
    Stderr  ReadPipe
    timeout std.time.Duration

    func Pid() int {
        return this.st.pid
    }

    // Waits for the child to exit and returns its exit code (128 + n if
    // it was killed by signal n, like a shell). Idempotent.
    func Wait() Task<int!> {
        return processWait(*this)
    }

    // Sends SIGKILL.
    func Kill() void! {
        this.Signal(9)?
    }

    func killQuietly() {
        try {
            this.Signal(9)?
        } catch err {}
    }

    // Sends signal sig (std.os.signal.SIGTERM ...).
    func Signal(sig int) void! {
        if !this.st.done {
            rtKill(this.st.pid, sig)?
        }
    }
}

// Starts cmd (looked up in PATH unless it contains "/") with args.
func Start(cmd string, args []string, opts Options) Process! {
    var v = rtSpawn(
        cmd,
        args,
        opts.Env,
        !opts.ClearEnv,
        opts.Dir,
        opts.Stdin,
        opts.Stdout,
        opts.Stderr,
    )?
    return Process{
        st: new procState{pid: v[0], pidfd: v[1]},
        Stdin: WritePipe{st: new pipeFd{fd: v[2]}},
        Stdout: ReadPipe{st: new pipeFd{fd: v[3]}},
        Stderr: ReadPipe{st: new pipeFd{fd: v[4]}},
        timeout: opts.Timeout,
    }
}

// The outcome of Run.
struct Result {
    ExitCode int
    Stdout   string
    Stderr   string

    // True if the exit code is 0.
    func Success() bool {
        return this.ExitCode == 0
    }
}

async func collect(p ReadPipe) []byte {
    return try await p.ReadAll() else []
}

// Runs cmd to completion, capturing stdout and stderr (stdin: /dev/null).
async func Run(cmd string, args []string) Result! {
    return await RunWith(cmd, args, Options{})?
}

// Run with options (Dir, Env, ClearEnv, Timeout); the streams are fixed:
// stdin /dev/null, stdout and stderr captured.
async func RunWith(cmd string, args []string, opts Options) Result! {
    var o = opts
    o.Stdin = 2
    o.Stdout = 1
    o.Stderr = 1
    var p = Start(cmd, args, o)?
    var out = collect(p.Stdout)
    var errs = collect(p.Stderr)
    var code = await p.Wait()?
    var ob = await out
    var eb = await errs
    return Result{
        ExitCode: code,
        Stdout: std.strings.FromBytes(ob),
        Stderr: std.strings.FromBytes(eb),
    }
}

// The stdout of cmd; an error (with stderr) if it exits non-zero.
async func Output(cmd string, args []string) string! {
    var r = await Run(cmd, args)?
    if r.ExitCode != 0 {
        var msg = std.strings.Trim(r.Stderr)
        if msg == "" {
            msg = "exit status " + std.strconv.Itoa(r.ExitCode)
        }
        return error(cmd + ": " + msg)
    }
    return r.Stdout
}

// The path of an executable named file: file itself if it contains a
// "/", else the first match in $PATH.
func LookPath(file string) string! {
    if std.strings.Contains(file, "/") {
        if rtExecutable(file) {
            return file
        }
        return error("exec: " + std.strconv.Quote(file) + ": not an executable file")
    }
    for dir in std.strings.Split(std.os.Getenv("PATH"), ":") {
        var d = dir
        if d == "" {
            d = "."
        }
        var p = d + "/" + file
        if rtExecutable(p) {
            return p
        }
    }
    return error("exec: " + std.strconv.Quote(file) + ": executable file not found in $PATH")
}

async func readPipeRead(self ReadPipe, n int) []byte! {
    var s = self.st
    if s.fd < 0 {
        return []
    }
    var buf = makeBuf(n)
    for {
        var k = rtRead(s.fd, buf)?
        if k >= 0 {
            return buf[..k]
        }
        var r = await rtWaitReadable(s.fd, -1)
        if r == 3 {
            return std.task.ErrCancelled
        }
        if r == 2 {
            return []
        }
    }
}

async func readPipeReadAll(self ReadPipe) []byte! {
    var out []byte = []
    for {
        var chunk = await self.Read(65536)?
        if len(chunk) == 0 {
            return out
        }
        for c in chunk {
            out = append(out, c)
        }
    }
}

async func writePipeWrite(self WritePipe, data []byte) int! {
    var s = self.st
    if s.fd < 0 {
        return error("process: write to closed stdin")
    }
    var off = 0
    for off < len(data) {
        var k = rtWrite(s.fd, data, off)?
        if k >= 0 {
            off += k
            continue
        }
        var r = await rtWaitWritable(s.fd, -1)
        if r == 3 {
            return std.task.ErrCancelled
        }
        if r == 2 {
            return error("process: write to closed stdin")
        }
    }
    return len(data)
}

async func writePipeWriteString(self WritePipe, s string) int! {
    return await self.Write(std.strings.Bytes(s))?
}

async func processWait(self Process) int! {
    var s = self.st
    if s.done {
        return s.code
    }
    var deadline = 0
    if self.timeout.Nanoseconds() > 0 {
        deadline = std.diag.rtMonoNanos() + self.timeout.Nanoseconds()
    }
    for {
        var c = rtWaitPid(s.pid)?
        if c >= 0 {
            s.done = true
            if c >= 1000 {
                s.code = 128 + (c - 1000)
            } else {
                s.code = c
            }
            return s.code
        }
        var ms = -1
        if deadline > 0 {
            var left = deadline - std.diag.rtMonoNanos()
            if left <= 0 {
                self.killQuietly()
                deadline = 0
                continue
            }
            ms = left / 1000000 + 1
        }
        if s.pidfd >= 0 {
            var r = await rtWaitReadable(s.pidfd, ms)
            if r == 3 {
                self.killQuietly()
                return std.task.ErrCancelled
            }
        } else {
            await std.task.Delay(5)
            if std.task.IsCancelled() {
                self.killQuietly()
                return std.task.ErrCancelled
            }
        }
    }
}
