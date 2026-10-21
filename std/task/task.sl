// The Slang-source part of std.task (see sema/stdsrc.rs for the rules).
// Delay, Yield, All and Any are runtime functions (stdlib.rs); the types
// here are built from three runtime primitives: signals (rtSignal: a
// pending Task<void> that rtFire completes once), per-object spin locks
// (rtLock/rtUnlock on an int field, held only for a few statements, never
// across an await) and the task context slot (rtGetCtx/rtSetCtx).
namespace std.task

// Mutex is an async mutual-exclusion lock. The zero value is unlocked;
// `await m.Lock()` waits until the caller owns it, `m.Unlock()` hands it
// to the oldest waiter (FIFO). Not re-entrant.
struct Mutex {
    guard   int
    locked  bool
    waiters []Task<void>

    // Lock waits until the mutex is free and takes it (FIFO among waiters).
    func Lock() Task<void> {
        rtLock(&this.guard)
        if !this.locked {
            this.locked = true
            rtUnlock(&this.guard)
            return Delay(0)
        }
        var s = rtWaker()
        this.waiters = append(this.waiters, s)
        rtUnlock(&this.guard)
        return s
    }

    // TryLock takes the mutex if it is free; reports whether it did.
    func TryLock() bool {
        rtLock(&this.guard)
        var ok = !this.locked
        this.locked = true
        rtUnlock(&this.guard)
        return ok
    }

    // Unlock releases the mutex, handing it to the oldest waiter; it panics if not locked.
    func Unlock() {
        rtLock(&this.guard)
        if !this.locked {
            rtUnlock(&this.guard)
            panic("unlock of unlocked mutex")
        }
        if len(this.waiters) == 0 {
            this.locked = false
            rtUnlock(&this.guard)
            return
        }
        // Ownership passes directly to the oldest waiter.
        var s = this.waiters[0]
        var none Task<void>
        this.waiters[0] = none
        this.waiters = this.waiters[1..]
        rtUnlock(&this.guard)
        rtFire(s)
    }
}

// WaitGroup waits for a number of tasks: Add(n) before starting them,
// Done() when each finishes, `await wg.Wait()` until the count is zero.
struct WaitGroup {
    guard   int
    n       int
    waiters []Task<void>

    // Add adds n (may be negative) to the counter; it panics if the counter goes negative.
    func Add(n int) {
        rtLock(&this.guard)
        this.n += n
        if this.n < 0 {
            rtUnlock(&this.guard)
            panic("negative WaitGroup counter")
        }
        if this.n > 0 || len(this.waiters) == 0 {
            rtUnlock(&this.guard)
            return
        }
        var ws = this.waiters
        this.waiters = nil
        rtUnlock(&this.guard)
        for w in ws {
            rtFire(w)
        }
    }

    // Done decrements the counter by one.
    func Done() {
        this.Add(-1)
    }

    // Count returns the current counter.
    func Count() int {
        rtLock(&this.guard)
        var n = this.n
        rtUnlock(&this.guard)
        return n
    }

    // Wait waits until the counter is zero.
    func Wait() Task<void> {
        rtLock(&this.guard)
        if this.n == 0 {
            rtUnlock(&this.guard)
            return Delay(0)
        }
        var s = rtWaker()
        this.waiters = append(this.waiters, s)
        rtUnlock(&this.guard)
        return s
    }
}

// Channel<T> is a FIFO channel between tasks. `task.Channel<int>(n)`
// makes one with a buffer of n values (0: unbuffered, every Send waits for
// its Recv). Copies of a Channel share it. `await ch.Send(v)` waits for
// room, `await ch.Recv()` for a value and fails once the channel is closed
// and drained; Send on a closed channel panics. `for v in ch` receives
// until then (Next, the iteration protocol).
struct Channel<T> {
    st *chanState<T>

    // New makes a channel with a buffer of capacity values (0: unbuffered); the same as `task.Channel<T>(capacity)`.
    static func New(capacity int) Channel<T> {
        if capacity < 0 {
            panic("negative channel capacity")
        }
        return Channel<T>{st: new chanState<T>{capacity: capacity}}
    }

    // Send waits for room (or a receiver) and sends v; it panics on a closed channel.
    func Send(v T) Task<void> {
        return this.st.send(v)
    }

    // Recv waits for a value; it fails once the channel is closed and drained.
    func Recv() Task<T!> {
        return this.st.recv()
    }

    // The iteration protocol: `for v in ch` (inside an async function)
    // receives values until the channel is closed and drained.
    async func Next() (T, bool) {
        var v = try await this.Recv() catch _ {
            var zero T
            return (zero, false)
        }
        return (v, true)
    }

    // Close closes the channel: pending and later receives drain the buffer, then fail.
    func Close() {
        this.st.close()
    }

    // Number of buffered values.
    func Len() int {
        rtLock(&this.st.guard)
        var n = len(this.st.buf)
        rtUnlock(&this.st.guard)
        return n
    }

    // Cap returns the buffer capacity.
    func Cap() int {
        return this.st.capacity
    }

    // IsClosed reports whether Close was called.
    func IsClosed() bool {
        rtLock(&this.st.guard)
        var c = this.st.closed
        rtUnlock(&this.st.guard)
        return c
    }
}

struct chanState<T> {
    guard    int
    capacity int
    buf      []T
    closed   bool
    sent     int
    taken    int
    recvq    []Task<void>
    sendq    []Task<void>

    func room() int {
        if this.capacity == 0 {
            return 1
        }
        return this.capacity
    }

    // Wakes the oldest waiting receiver (guard held).
    func wakeRecv() {
        if len(this.recvq) == 0 {
            return
        }
        var s = this.recvq[0]
        var none Task<void>
        this.recvq[0] = none
        this.recvq = this.recvq[1..]
        rtFire(s)
    }

    // Wakes every waiting sender (guard held); they re-check.
    func wakeSenders() {
        for s in this.sendq {
            rtFire(s)
        }
        this.sendq = nil
    }

    // In a cancelled task Send returns without sending.
    async func send(v T) {
        rtLock(&this.guard)
        for !this.closed && len(this.buf) >= this.room() {
            if IsCancelled() {
                rtUnlock(&this.guard)
                return
            }
            var s = rtSignal()
            this.sendq = append(this.sendq, s)
            rtUnlock(&this.guard)
            await s
            rtLock(&this.guard)
        }
        if this.closed {
            rtUnlock(&this.guard)
            panic("send on closed channel")
        }
        this.buf = append(this.buf, v)
        this.sent += 1
        var seq = this.sent
        this.wakeRecv()
        if this.capacity == 0 {
            // Unbuffered: wait until a receiver took the value.
            for this.taken < seq && !this.closed && !IsCancelled() {
                var s = rtSignal()
                this.sendq = append(this.sendq, s)
                rtUnlock(&this.guard)
                await s
                rtLock(&this.guard)
            }
        }
        rtUnlock(&this.guard)
    }

    async func recv() T! {
        rtLock(&this.guard)
        for len(this.buf) == 0 && !this.closed {
            if IsCancelled() {
                rtUnlock(&this.guard)
                return ErrCancelled
            }
            var s = rtSignal()
            this.recvq = append(this.recvq, s)
            rtUnlock(&this.guard)
            await s
            rtLock(&this.guard)
        }
        if len(this.buf) == 0 {
            rtUnlock(&this.guard)
            return error("channel closed")
        }
        var v = this.buf[0]
        var zero T
        this.buf[0] = zero
        this.buf = this.buf[1..]
        this.taken += 1
        this.wakeSenders()
        if len(this.buf) > 0 {
            this.wakeRecv()
        }
        rtUnlock(&this.guard)
        return v
    }

    func close() {
        rtLock(&this.guard)
        if this.closed {
            rtUnlock(&this.guard)
            panic("close of closed channel")
        }
        this.closed = true
        var rs = this.recvq
        this.recvq = nil
        this.wakeSenders()
        rtUnlock(&this.guard)
        for s in rs {
            rtFire(s)
        }
    }
}

// Spawn runs f on a worker thread (not on the caller's stack, unlike a
// direct call of an async function) and returns its task. f must have a
// result; use Go for `func() Task<void>`.
async func Spawn<T>(f func() Task<T>) T {
    await Yield()
    var t = f()
    return await t
}

// Go is Spawn for functions without a result (`Spawn<void>` cannot
// return the result of its task).
async func Go(f func() Task<void>) {
    await Yield()
    await f()
}

// SpawnWith is Spawn with an explicit context (from CurrentContext or
// NewCancel): its values and its cancellation apply to f.
func SpawnWith<T>(c Context, f func() Task<T>) Task<T> {
    return RunWith(c, func() Task<T> {
        return Spawn(f)
    })
}

struct timeoutFlag {
    done bool
}

async func watchTask<T>(t Task<T>, s Task<void>, f *timeoutFlag) {
    _ = await t
    f.done = true
    rtFire(s)
}

// Timeout waits at most ms milliseconds for t. It yields true if t has
// finished (its result is then available with `await t` at once), false
// on timeout; t keeps running either way.
async func Timeout<T>(t Task<T>, ms int) bool {
    var f = new timeoutFlag{}
    var s = Delay(ms)
    _ = watchTask<T>(t, s, f)
    await s
    return f.done
}

// ---- task context: values and cancellation (ABI.md 10.8, 10.11) ----

// Key<T> is a unique, typed key for a context value: `task.Key<Span>()`.
struct Key<T> {
    id int

    // New returns a fresh key (the same as `task.Key<T>()`).
    static func New() Key<T> {
        return Key<T>{id: rtNextKey()}
    }
}

// One frame of the immutable context list.
struct ctxFrame {
    key  int
    val  any
    next *ctxFrame
}

// Context is a snapshot of the context of a task: its values (an immutable
// list of frames) and its cancellation scope. Pass it to RunWith/SpawnWith
// to run code in it.
struct Context {
    // The runtime slot: {"": *ctxFrame} (nil when empty).
    slot  map[string]any
    scope Task<void>

    func top() *ctxFrame {
        switch this.slot[""] {
            case *ctxFrame f:
                return f
        }
        return nil
    }

    // Whether the context's scope has been cancelled.
    func IsCancelled() bool {
        return rtScopeCancelled(this.scope)
    }
}

// The context of the running task.
func CurrentContext() Context {
    return Context{slot: rtGetCtx(), scope: rtCurrent()}
}

func lookup<T>(c Context, k Key<T>) *T {
    var f = c.top()
    for f != nil {
        if f.key == k.id {
            switch f.val {
                case *T p:
                    return p
            }
            return nil
        }
        f = f.next
    }
    return nil
}

// Get is the value of k in the current context, nil if it is not set.
func Get<T>(k Key<T>) *T {
    var c = CurrentContext()
    return lookup<T>(c, k)
}

// With runs f with k set to value; the value is visible in f, in what f
// calls and in tasks started inside, and not after With returns.
func With<T, R>(k Key<T>, value T, f func() Task<R>) Task<R> {
    var c = CurrentContext()
    var v = value
    var fr = new ctxFrame{key: k.id, val: &v, next: c.top()}
    var m = map[string]any{}
    m[""] = fr
    c.slot = m
    return RunWith(c, f)
}

// RunWith calls f in context c and returns its task; the caller's own
// context is unchanged afterwards.
func RunWith<T>(c Context, f func() Task<T>) Task<T> {
    var old = rtGetCtx()
    rtSetCtx(c.slot)
    var prev = rtEnterScope(c.scope)
    var t = f()
    rtLeaveScope(prev)
    rtSetCtx(old)
    return t
}

// ---- cancellation ---------------------------------------------------------

// CancelHandle cancels the code run in its Context (with RunWith or
// SpawnWith) and every task started from it.
struct CancelHandle {
    Context Context

    // Cancel cancels the context and every task started in it.
    func Cancel() {
        rtCancel(this.Context.scope)
    }

    // IsCancelled reports whether Cancel was called.
    func IsCancelled() bool {
        return rtScopeCancelled(this.Context.scope)
    }
}

// NewCancel makes a cancellation scope under the current task.
func NewCancel() CancelHandle {
    var c = CurrentContext()
    c.scope = rtNewScope()
    return CancelHandle{Context: c}
}

// WithTimeout runs f in a scope that is cancelled after ms milliseconds.
func WithTimeout<T>(ms int, f func() Task<T>) Task<T> {
    var h = NewCancel()
    rtSetDeadline(h.Context.scope, ms)
    return RunWith(h.Context, f)
}

// CheckCancelled fails with ErrCancelled if the current task is cancelled.
func CheckCancelled() void! {
    if IsCancelled() {
        return ErrCancelled
    }
}
