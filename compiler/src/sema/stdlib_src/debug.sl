namespace std.debug

// std.debug: the in-process debug agent (docs/DEBUGGER.md stage 2). A
// program built with `--debug-agent` (the default for debug builds) and
// started with SLANG_DEBUG=[host]:port[,wait] runs AgentMain before main:
// it listens on a local TCP socket and serves the Debug Adapter Protocol
// from a dedicated OS thread (std.thread), outside the task scheduler, so
// it keeps answering while every task is paused. The C side
// (runtime/src/debug.c, ABI.md section 23) calls back into `hook` from the
// statement hooks when a breakpoint, step, pause, panic or error asks for
// it; the callback decides (conditions, logpoints, exception filters) and
// the runtime blocks the task while it is stopped.
//
// Files: debug.sl (start-up, stop decisions, DAP requests), debug_dap.sl
// (framing and JSON output), debug_vars.sl (tables, values, expressions).

// Reasons passed to the hook (ABI.md 23.3).
func rBp() int { return 1 }
func rStep() int { return 2 }
func rPause() int { return 4 }
func rPanic() int { return 8 }
func rError() int { return 16 }
func rEntry() int { return 32 }

// A source breakpoint at statement `stmt`.
struct breakpoint {
    id      int
    stmt    int
    line    int
    cond    string
    log     string
    hits    int
    hitCond int
}

// What a variablesReference stands for: kind 1 the locals of frame
// `frame`, kind 2 the children of the value at addr of type ti.
struct varRef {
    kind  int
    frame int
    addr  int
    ti    int
}

// The agent's state, shared by the agent thread and the hooks (guarded by
// rtLock(0); output by rtLock(1)).
struct Agent {
    t      tables
    listen int
    conn   int
    seq    int
    nextBp int
    // statement -> breakpoint
    bps map[int]*breakpoint
    // file index -> statements with a source breakpoint
    fileBps      map[int][]int
    funcBps      []int
    excPanic     bool
    excError     bool
    errTypes     []string
    pausePending bool
    stopped      bool
    // frame ids (index + 1) and variable references of the current stop
    frames    []int
    frameTask []int
    refs      []varRef
    exited    bool
    waiting   bool

    func reset() {
        this.frames = []
        this.frameTask = []
        this.refs = []
    }

    func frameId(f int, task int) int {
        for i in 0..len(this.frames) {
            if this.frames[i] == f {
                return i + 1
            }
        }
        this.frames = append(this.frames, f)
        this.frameTask = append(this.frameTask, task)
        return len(this.frames)
    }

    func newRef(r varRef) int {
        this.refs = append(this.refs, r)
        return len(this.refs)
    }

    func refFor(addr int, ti int) int {
        if !hasChildren(addr, ti) {
            return 0
        }
        return this.newRef(varRef{kind: 2, addr: addr, ti: ti})
    }

    // The innermost frame of a stopped task (0 if it is not stopped).
    func topFrame(task int) int {
        return rtTaskFrame(task)
    }

    func frameAt(id int) int {
        if id <= 0 || id > len(this.frames) {
            return 0
        }
        return this.frames[id - 1]
    }

    // ---- output ---------------------------------------------------------

    func send(body string) {
        rtLock(1)
        if this.conn >= 0 {
            if !rtSend(
                this.conn,
                "Content-Length: " + std.strconv.Itoa(len(body)) + "\r\n\r\n" + body,
            ) {
                rtClose(this.conn)
                this.conn = -1
            }
        }
        rtUnlock(1)
    }

    func nextSeq() int {
        rtLock(1)
        this.seq += 1
        var s = this.seq
        rtUnlock(1)
        return s
    }

    func event(name string, body string) {
        var b = body
        if b == "" {
            b = "{}"
        }
        this.send(
            "{\"seq\":" +
                std.strconv.Itoa(this.nextSeq()) +
                ",\"type\":\"event\",\"event\":" +
                q(name) +
                ",\"body\":" +
                b +
                "}",
        )
    }

    func respond(req request, body string) {
        var b = body
        if b == "" {
            b = "{}"
        }
        this.send(
            "{\"seq\":" +
                std.strconv.Itoa(this.nextSeq()) +
                ",\"type\":\"response\",\"request_seq\":" +
                std.strconv.Itoa(req.seq) +
                ",\"success\":true,\"command\":" +
                q(req.command) +
                ",\"body\":" +
                b +
                "}",
        )
    }

    func fail(req request, msg string) {
        this.send(
            "{\"seq\":" +
                std.strconv.Itoa(this.nextSeq()) +
                ",\"type\":\"response\",\"request_seq\":" +
                std.strconv.Itoa(req.seq) +
                ",\"success\":false,\"command\":" +
                q(req.command) +
                ",\"message\":" +
                q(msg) +
                ",\"body\":{\"error\":{\"id\":1,\"format\":" +
                q(msg) +
                "}}}",
        )
    }

    func output(text string, category string) {
        this.event("output", obj(["category", q(category), "output", q(text)]))
    }

    // ---- stops (called from the hooks, on the program's threads) --------

    func decide(stmt int, frame int, reason int, task int) int {
        rtLock(0)
        var why = ""
        var text = ""
        var hit = 0
        if reason & rBp() != 0 {
            var bp = this.bps[stmt]
            if bp != nil {
                var ok = true
                if bp.cond != "" {
                    ok = this.condTrue(bp.cond, frame)
                }
                if ok {
                    bp.hits += 1
                    if bp.hitCond > 0 && bp.hits < bp.hitCond {
                        ok = false
                    }
                }
                if ok {
                    if bp.log != "" {
                        this.output(this.interpolate(bp.log, frame) + "\n", "console")
                    } else {
                        why = "breakpoint"
                        hit = bp.id
                    }
                }
            } else if this.isFuncBp(stmt) {
                why = "function breakpoint"
            }
        }
        if why == "" && reason & rStep() != 0 {
            why = "step"
        }
        if why == "" && reason & rEntry() != 0 {
            why = "entry"
        }
        if reason & rPanic() != 0 && this.excPanic {
            why = "exception"
            text = "panic: " + rtPanicText()
        }
        if reason & rError() != 0 && this.excError {
            var e = rtCurError()
            var ty = rtErrorType(e)
            var want = len(this.errTypes) == 0
            for t in this.errTypes {
                if t == ty || std.strings.HasSuffix(ty, "." + t) {
                    want = true
                }
            }
            if want {
                why = "exception"
                text = "error created: " + errorText(e)
            }
        }
        var paused = false
        if why == "" && reason & rPause() != 0 {
            if this.pausePending {
                this.pausePending = false
                why = "pause"
            } else {
                // Frozen while another task is stopped: wait silently.
                paused = true
            }
        }
        if why == "" {
            rtUnlock(0)
            if paused {
                return 2
            }
            return 0
        }
        if this.conn < 0 {
            // Nobody is attached: never stop (a logpoint was printed above).
            rtUnlock(0)
            return 0
        }
        rtFreeze(true)
        this.stopped = true
        var fields = [
            "reason",
            q(why),
            "threadId",
            std.strconv.Itoa(task),
            "allThreadsStopped",
            "true",
        ]
        if text != "" {
            fields = append(fields, "text", q(text), "description", q(text))
        }
        if hit > 0 {
            fields = append(fields, "hitBreakpointIds", "[" + std.strconv.Itoa(hit) + "]")
        }
        this.event("stopped", obj(fields))
        rtUnlock(0)
        return 1
    }

    func isFuncBp(stmt int) bool {
        for s in this.funcBps {
            if s == stmt {
                return true
            }
        }
        return false
    }

    func condTrue(cond string, frame int) bool {
        var v = try evaluate(cond, frameLocals(this.t, frame)) catch err {
            this.output("breakpoint condition " + q(cond) + ": " + err.Message() + "\n", "stderr")
            return true
        }
        var b = try truth(v) catch err {
            this.output("breakpoint condition " + q(cond) + ": " + err.Message() + "\n", "stderr")
            return true
        }
        return b
    }

    // A logpoint message: {expr} parts are evaluated.
    func interpolate(msg string, frame int) string {
        var out = ""
        var rest = msg
        for {
            var open = std.strings.Index(rest, "{")
            if open < 0 {
                break
            }
            var close = std.strings.Index(rest[open..], "}")
            if close < 0 {
                break
            }
            out = out + rest[..open]
            var expr = rest[open + 1..open + close]
            var v = try evaluate(expr, frameLocals(this.t, frame)) catch err {
                out = out + "<" + err.Message() + ">"
                rest = rest[open + close + 1..]
                continue
            }
            var s = scalar(v)
            if s.lit == 3 {
                out = out + s.s
            } else {
                out = out + show(v)
            }
            rest = rest[open + close + 1..]
        }
        return out + rest
    }

    func exitHook(code int) {
        rtLock(0)
        this.exited = true
        this.event("exited", obj(["exitCode", std.strconv.Itoa(code)]))
        this.event("terminated", "")
        rtUnlock(0)
    }

    // ---- requests (on the agent thread) ---------------------------------

    func handle(req request) {
        rtLock(0)
        var c = req.command
        if c == "initialize" {
            this.respond(req, capabilities())
            this.event("initialized", "")
        } else if c == "launch" || c == "attach" {
            if req.args.Get("stopOnEntry").IsBool() &&
                (try req.args.Get("stopOnEntry").Bool() else false) {
                rtStopNext()
            }
            this.respond(req, "")
        } else if c == "configurationDone" {
            this.respond(req, "")
            this.release()
        } else if c == "setBreakpoints" {
            this.setBreakpoints(req)
        } else if c == "setFunctionBreakpoints" {
            this.setFunctionBreakpoints(req)
        } else if c == "setExceptionBreakpoints" {
            this.setExceptionBreakpoints(req)
        } else if c == "threads" {
            this.threads(req)
        } else if c == "stackTrace" {
            this.stackTrace(req)
        } else if c == "scopes" {
            var f = this.frameAt(argInt(req, "frameId"))
            if f == 0 {
                this.fail(req, "unknown frame")
            } else {
                var r = this.newRef(varRef{kind: 1, frame: f})
                this.respond(
                    req,
                    obj([
                        "scopes",
                        "[" +
                            obj([
                                "name",
                                q("Locals"),
                                "presentationHint",
                                q("locals"),
                                "variablesReference",
                                std.strconv.Itoa(r),
                                "expensive",
                                "false",
                            ]) +
                            "]",
                    ]),
                )
            }
        } else if c == "variables" {
            this.variables(req)
        } else if c == "evaluate" {
            this.evaluateReq(req)
        } else if c == "continue" {
            this.resume(req, 0)
        } else if c == "next" {
            this.resume(req, 1)
        } else if c == "stepIn" {
            this.resume(req, 2)
        } else if c == "stepOut" {
            this.resume(req, 3)
        } else if c == "pause" {
            this.pausePending = true
            rtFreeze(true)
            this.respond(req, "")
        } else if c == "disconnect" || c == "terminate" {
            var kill = c == "terminate" || (try req.args.Get("terminateDebuggee").Bool() else false)
            this.respond(req, "")
            this.detach()
            if kill {
                rtUnlock(0)
                rtExit(0)
            }
        } else {
            this.fail(req, "unsupported request " + c)
        }
        rtUnlock(0)
    }

    // The program may start (or go on): `--wait` ends here.
    func release() {
        if this.waiting {
            this.waiting = false
            rtSignalReady()
        }
    }

    func detach() {
        this.bps = map[int]*breakpoint{}
        this.fileBps = map[int][]int{}
        this.funcBps = []
        this.excPanic = false
        this.excError = false
        this.pausePending = false
        this.stopped = false
        this.reset()
        rtReleaseAll()
        this.release()
    }

    func resume(req request, mode int) {
        var task = argInt(req, "threadId")
        var single = try req.args.Get("singleThread").Bool() else false
        this.reset()
        this.stopped = false
        // The response goes first: the program may end right away.
        if mode == 0 {
            this.respond(req, obj(["allThreadsContinued", std.strconv.FormatBool(!single)]))
        } else {
            this.respond(req, "")
        }
        if !rtResume(task, mode, single) && mode == 0 {
            // Not stopped (or gone): still let everything run.
            rtResume(0, 0, false)
        }
    }

    // The file index of a DAP source path (exact path, else a path suffix).
    func fileIndex(path string) int {
        for i in 0..len(this.t.files) {
            if this.t.files[i] == path {
                return i
            }
        }
        var best = -1
        for i in 0..len(this.t.files) {
            var f = this.t.files[i]
            if std.strings.HasSuffix(f, "/" + path) ||
                std.strings.HasSuffix(path, "/" + baseName(f)) && baseName(f) == baseName(path) {
                best = i
            }
        }
        return best
    }

    // The statements a breakpoint at line binds to: on the first line at
    // or after it (within 50 lines) that has code, the first statement of
    // each function there (generic instances share their lines).
    func stmtsAt(file int, line int) []int {
        var bestLine = 0
        for i in 0..this.t.nstmts {
            if this.t.stmtFile(i) != file || this.t.stmtKind(i) != 0 {
                continue
            }
            var l = this.t.stmtLine(i)
            if l >= line && l <= line + 50 && (bestLine == 0 || l < bestLine) {
                bestLine = l
            }
        }
        var out []int = []
        if bestLine == 0 {
            return out
        }
        var funcs []int = []
        for i in 0..this.t.nstmts {
            if this.t.stmtFile(i) != file ||
                this.t.stmtKind(i) != 0 ||
                this.t.stmtLine(i) != bestLine {
                continue
            }
            var fn = this.t.stmtFunc(i)
            var seen = -1
            for k in 0..len(funcs) {
                if funcs[k] == fn {
                    seen = k
                }
            }
            if seen < 0 {
                funcs = append(funcs, fn)
                out = append(out, i)
            } else if this.t.stmtCol(i) < this.t.stmtCol(out[seen]) {
                out[seen] = i
            }
        }
        return out
    }

    func setBreakpoints(req request) {
        var path = try req.args.Get("source").Get("path").String() else ""
        if path == "" {
            path = try req.args.Get("source").Get("name").String() else ""
        }
        var file = this.fileIndex(path)
        if file >= 0 {
            for s in this.fileBps[file] {
                delete(this.bps, s)
                if !this.isFuncBp(s) {
                    rtSetBp(s, false)
                }
            }
        }
        var stmts []int = []
        var results []string = []
        var list = req.args.Get("breakpoints")
        for i in 0..list.Len() {
            var b = list.Index(i)
            var line = try b.Get("line").Int() else 0
            this.nextBp += 1
            var id = this.nextBp
            var at []int = []
            if file >= 0 {
                at = this.stmtsAt(file, line)
            }
            if len(at) == 0 {
                results = append(
                    results,
                    obj([
                        "id",
                        std.strconv.Itoa(id),
                        "verified",
                        "false",
                        "line",
                        std.strconv.Itoa(line),
                        "message",
                        q("no code at this line"),
                    ]),
                )
                continue
            }
            var bp = new breakpoint{id: id, stmt: at[0], line: this.t.stmtLine(at[0])}
            bp.cond = try b.Get("condition").String() else ""
            bp.log = try b.Get("logMessage").String() else ""
            var hc = try b.Get("hitCondition").String() else ""
            if hc != "" {
                bp.hitCond = try std.strconv.Atoi(
                    std.strings.TrimPrefix(std.strings.Trim(hc), ">="),
                ) else 0
            }
            for st in at {
                this.bps[st] = bp
                stmts = append(stmts, st)
                rtSetBp(st, true)
            }
            results = append(
                results,
                obj([
                    "id",
                    std.strconv.Itoa(id),
                    "verified",
                    "true",
                    "line",
                    std.strconv.Itoa(bp.line),
                    "source",
                    obj(["path", q(this.t.fileName(file))]),
                ]),
            )
        }
        if file >= 0 {
            this.fileBps[file] = stmts
        }
        this.respond(req, obj(["breakpoints", "[" + std.strings.Join(results, ",") + "]"]))
    }

    func setFunctionBreakpoints(req request) {
        for s in this.funcBps {
            if this.bps[s] == nil {
                rtSetBp(s, false)
            }
        }
        this.funcBps = []
        var results []string = []
        var list = req.args.Get("breakpoints")
        for i in 0..list.Len() {
            var name = try list.Index(i).Get("name").String() else ""
            this.nextBp += 1
            var found = -1
            for f in 0..this.t.nfuncs {
                var full = this.t.funcName(f)
                if full == name ||
                    this.t.funcShort(f) == name ||
                    std.strings.HasSuffix(full, "." + name) {
                    found = f
                    break
                }
            }
            if found < 0 {
                results = append(
                    results,
                    obj([
                        "id",
                        std.strconv.Itoa(this.nextBp),
                        "verified",
                        "false",
                        "message",
                        q("no function " + name),
                    ]),
                )
                continue
            }
            var s = this.t.funcEntry(found)
            this.funcBps = append(this.funcBps, s)
            rtSetBp(s, true)
            results = append(
                results,
                obj([
                    "id",
                    std.strconv.Itoa(this.nextBp),
                    "verified",
                    "true",
                    "line",
                    std.strconv.Itoa(this.t.stmtLine(s)),
                    "source",
                    obj(["path", q(this.t.fileName(this.t.stmtFile(s)))]),
                ]),
            )
        }
        this.respond(req, obj(["breakpoints", "[" + std.strings.Join(results, ",") + "]"]))
    }

    func setExceptionBreakpoints(req request) {
        this.excPanic = false
        this.excError = false
        this.errTypes = []
        var fs = req.args.Get("filters")
        for i in 0..fs.Len() {
            var f = try fs.Index(i).String() else ""
            if f == "panic" {
                this.excPanic = true
            } else if f == "error" {
                this.excError = true
            }
        }
        var opts = req.args.Get("filterOptions")
        for i in 0..opts.Len() {
            var o = opts.Index(i)
            var id = try o.Get("filterId").String() else ""
            var cond = try o.Get("condition").String() else ""
            if id == "panic" {
                this.excPanic = true
            } else if id == "error" {
                this.excError = true
                for part in std.strings.Split(cond, ",") {
                    var p = std.strings.Trim(part)
                    if p != "" {
                        this.errTypes = append(this.errTypes, p)
                    }
                }
            }
        }
        rtOnError(this.excError)
        this.respond(req, "")
    }

    func taskName(task int) string {
        var fn = this.t.funcShort(rtTaskFunc(task))
        if task == 1 || fn == "main" {
            return "main"
        }
        return "task " + std.strconv.Itoa(task) + " " + fn
    }

    func threads(req request) {
        var items []string = []
        for task in rtTasks() {
            items = append(
                items,
                obj(["id", std.strconv.Itoa(task), "name", q(this.taskName(task))]),
            )
        }
        if len(items) == 0 {
            items = append(items, obj(["id", "1", "name", q("main")]))
        }
        this.respond(req, obj(["threads", "[" + std.strings.Join(items, ",") + "]"]))
    }

    func stackTrace(req request) {
        var task = argInt(req, "threadId")
        var start = argInt(req, "startFrame")
        var levels = argInt(req, "levels")
        var f = this.topFrame(task)
        var items []string = []
        var n = 0
        for f != 0 && n < 256 {
            if n >= start && (levels <= 0 || len(items) < levels) {
                var fi = this.t.funcIndex(rd(f + 8))
                var stmt = rd32(f + 24)
                var line = 0
                var col = 0
                var file = -1
                if stmt >= 0 && stmt < this.t.nstmts {
                    line = this.t.stmtLine(stmt)
                    col = this.t.stmtCol(stmt)
                    file = this.t.stmtFile(stmt)
                }
                var path = this.t.fileName(file)
                var fid = this.frameId(f, task)
                items = append(
                    items,
                    obj([
                        "id",
                        std.strconv.Itoa(fid),
                        "name",
                        q(this.t.funcName(fi)),
                        "line",
                        std.strconv.Itoa(line),
                        "column",
                        std.strconv.Itoa(col),
                        "source",
                        obj(["name", q(baseName(path)), "path", q(path)]),
                    ]),
                )
            }
            n += 1
            f = rd(f)
        }
        this.respond(
            req,
            obj([
                "stackFrames",
                "[" + std.strings.Join(items, ",") + "]",
                "totalFrames",
                std.strconv.Itoa(n),
            ]),
        )
    }

    func variable(name string, addr int, ti int) string {
        var r = this.refFor(addr, ti)
        var fields = [
            "name",
            q(name),
            "value",
            q(format(addr, ti, 2)),
            "type",
            q(tiName(ti)),
            "variablesReference",
            std.strconv.Itoa(r),
        ]
        var n = indexedCount(addr, ti)
        if r > 0 && n > 0 {
            fields = append(fields, "indexedVariables", std.strconv.Itoa(n))
        }
        return obj(fields)
    }

    func variables(req request) {
        var id = argInt(req, "variablesReference")
        if id <= 0 || id > len(this.refs) {
            this.fail(req, "unknown variablesReference")
            return
        }
        var r = this.refs[id - 1]
        var items []string = []
        if r.kind == 1 {
            for l in frameLocals(this.t, r.frame) {
                items = append(items, this.variable(l.name, l.addr, l.ti))
            }
        } else {
            var start = argInt(req, "start")
            var count = -1
            if req.args.Has("count") && argInt(req, "count") > 0 {
                count = argInt(req, "count")
            }
            for c in children(r.addr, r.ti, start, count) {
                items = append(items, this.variable(c.name, c.addr, c.ti))
            }
        }
        this.respond(req, obj(["variables", "[" + std.strings.Join(items, ",") + "]"]))
    }

    func evaluateReq(req request) {
        var text = try req.args.Get("expression").String() else ""
        var f = this.frameAt(argInt(req, "frameId"))
        if f == 0 {
            // No frame: the first stopped task's top frame.
            for task in rtTasks() {
                if rtTaskState(task) == 1 {
                    f = this.topFrame(task)
                    break
                }
            }
        }
        if f == 0 {
            this.fail(req, "no stopped frame to evaluate in")
            return
        }
        var v = try evaluate(text, frameLocals(this.t, f)) catch err {
            this.fail(req, err.Message())
            return
        }
        var r = 0
        if v.ti != 0 {
            r = this.refFor(v.addr, v.ti)
        }
        this.respond(
            req,
            obj([
                "result",
                q(show(v)),
                "type",
                q(showType(v)),
                "variablesReference",
                std.strconv.Itoa(r),
            ]),
        )
    }

    // ---- the connection loop ---------------------------------------------

    func serve() {
        rtIgnoreHooks()
        for {
            var c = rtAccept(this.listen)
            if c < 0 {
                return
            }
            rtLock(0)
            rtLock(1)
            this.conn = c
            this.seq = 0
            rtUnlock(1)
            rtUnlock(0)
            this.readLoop(c)
            rtLock(0)
            this.detach()
            rtLock(1)
            if this.conn >= 0 {
                rtClose(this.conn)
            }
            this.conn = -1
            rtUnlock(1)
            rtUnlock(0)
        }
    }

    func readLoop(c int) {
        var r = reader{fd: c}
        for {
            var body = try r.next() catch err {
                if std.os.Getenv("SLANG_DEBUG_TRACE") != "" {
                    rtEprint("slang-debug: " + err.Message() + "\n")
                }
                return
            }
            var req = try parseRequest(body) catch err {
                continue
            }
            this.handle(req)
            if req.command == "disconnect" {
                return
            }
        }
    }
}

func capabilities() string {
    var filters = "[" +
        obj(["filter", q("panic"), "label", q("Panics"), "default", "true"]) +
        "," +
        obj([
            "filter",
            q("error"),
            "label",
            q("Errors (on creation)"),
            "default",
            "false",
            "supportsCondition",
            "true",
            "conditionDescription",
            q("comma-separated error type names, e.g. NotFound"),
        ]) +
        "]"
    return obj([
        "supportsConfigurationDoneRequest",
        "true",
        "supportsConditionalBreakpoints",
        "true",
        "supportsHitConditionalBreakpoints",
        "true",
        "supportsLogPoints",
        "true",
        "supportsFunctionBreakpoints",
        "true",
        "supportsEvaluateForHovers",
        "true",
        "supportsTerminateRequest",
        "true",
        "supportsSingleThreadExecutionRequests",
        "true",
        "supportsExceptionFilterOptions",
        "true",
        "exceptionBreakpointFilters",
        filters,
    ])
}

func baseName(p string) string {
    var i = std.strings.LastIndex(p, "/")
    if i < 0 {
        return p
    }
    return p[i + 1..]
}

// Parses SLANG_DEBUG: "[host]:port" with optional ",wait".
struct config {
    host string
    port int
    wait bool
}

func parseConfig(s string) config! {
    var c = config{host: "127.0.0.1", wait: std.os.Getenv("SLANG_DEBUG_WAIT") == "1"}
    var spec = s
    for part in std.strings.Split(s, ",") {
        if std.strings.Trim(part) == "wait" {
            c.wait = true
        }
    }
    var comma = std.strings.Index(spec, ",")
    if comma >= 0 {
        spec = spec[..comma]
    }
    if spec == "1" || spec == "on" || spec == "" {
        spec = ":0"
    }
    var colon = std.strings.LastIndex(spec, ":")
    if colon < 0 {
        c.port = std.strconv.Atoi(spec)?
        return c
    }
    if colon > 0 {
        c.host = spec[..colon]
    }
    c.port = std.strconv.Atoi(spec[colon + 1..])?
    return c
}

// The agent's entry point, called by the runtime before main when
// SLANG_DEBUG is set (slang_dbg_boot, ABI.md 23.2).
func AgentMain() {
    var env = std.os.Getenv("SLANG_DEBUG")
    var cfg = try parseConfig(env) catch err {
        rtEprint(
            "slang-debug: bad SLANG_DEBUG " + std.strconv.Quote(env) + ": " + err.Message() + "\n",
        )
        return
    }
    var fd = try rtListen(cfg.host, cfg.port) catch err {
        rtEprint(err.Message() + "\n")
        return
    }
    var a = new Agent{
        t: loadTables(),
        listen: fd,
        conn: -1,
        bps: map[int]*breakpoint{},
        fileBps: map[int][]int{},
        waiting: cfg.wait,
    }
    rtSetHook(func(stmt int, frame int, reason int, task int) int {
        return a.decide(stmt, frame, reason, task)
    })
    rtSetExitHook(func(code int) {
        a.exitHook(code)
    })
    rtEprint(
        "slang-debug: listening on " + cfg.host + ":" + std.strconv.Itoa(rtSockPort(fd)) + "\n",
    )
    std.thread.Spawn(func() {
        a.serve()
    })
    if cfg.wait {
        rtWaitReady()
    }
}
