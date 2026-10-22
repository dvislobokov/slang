// std.sync: synchronization beyond std.task's Mutex, WaitGroup and
// Channel: RwMutex (async), Semaphore (async), Barrier (async), Once,
// AtomicInt / AtomicBool (lock-free, runtime/src/std_sync.c), Atomic<T>
// (any comparable value) and Pool<T>. The async primitives are built like
// task.Mutex: a per-object spin lock held for a few statements and waker
// tasks fired in FIFO order; nothing blocks a worker thread.
namespace std.sync

// A reader/writer lock: many readers or one writer. Writers are
// preferred: once a writer waits, new readers queue behind it.
struct RwMutex {
    guard          int
    readers        int
    writer         bool
    waitingWriters int
    rq             []Task<void>
    wq             []Task<void>

    // Waits for shared (read) access.
    func RLock() Task<void> {
        std.task.rtLock(&this.guard)
        if !this.writer && this.waitingWriters == 0 {
            this.readers += 1
            std.task.rtUnlock(&this.guard)
            return std.task.Delay(0)
        }
        var s = std.task.rtWaker()
        this.rq = append(this.rq, s)
        std.task.rtUnlock(&this.guard)
        return s
    }

    // TryRLock takes read access without waiting; false if a writer holds or waits for the lock.
    func TryRLock() bool {
        std.task.rtLock(&this.guard)
        var ok = !this.writer && this.waitingWriters == 0
        if ok {
            this.readers += 1
        }
        std.task.rtUnlock(&this.guard)
        return ok
    }

    // RUnlock releases read access; it panics without a matching RLock.
    func RUnlock() {
        std.task.rtLock(&this.guard)
        if this.readers <= 0 {
            std.task.rtUnlock(&this.guard)
            panic("sync: RUnlock of unlocked RwMutex")
        }
        this.readers -= 1
        this.handoff()
    }

    // Waits for exclusive (write) access.
    func Lock() Task<void> {
        std.task.rtLock(&this.guard)
        if !this.writer && this.readers == 0 {
            this.writer = true
            std.task.rtUnlock(&this.guard)
            return std.task.Delay(0)
        }
        var s = std.task.rtWaker()
        this.wq = append(this.wq, s)
        this.waitingWriters += 1
        std.task.rtUnlock(&this.guard)
        return s
    }

    // TryLock takes write access without waiting; false if the lock is held.
    func TryLock() bool {
        std.task.rtLock(&this.guard)
        var ok = !this.writer && this.readers == 0
        if ok {
            this.writer = true
        }
        std.task.rtUnlock(&this.guard)
        return ok
    }

    // Unlock releases write access, waking the next writer or the waiting readers; it panics if not locked.
    func Unlock() {
        std.task.rtLock(&this.guard)
        if !this.writer {
            std.task.rtUnlock(&this.guard)
            panic("sync: Unlock of unlocked RwMutex")
        }
        this.writer = false
        this.handoff()
    }

    // Called with the guard held; releases it. Gives the lock to the next
    // writer, or else to every waiting reader.
    func handoff() {
        if this.writer || this.readers > 0 {
            std.task.rtUnlock(&this.guard)
            return
        }
        if len(this.wq) > 0 {
            var w = this.wq[0]
            var none Task<void>
            this.wq[0] = none
            this.wq = this.wq[1..]
            this.waitingWriters -= 1
            this.writer = true
            std.task.rtUnlock(&this.guard)
            std.task.rtFire(w)
            return
        }
        var rs = this.rq
        this.rq = nil
        this.readers += len(rs)
        std.task.rtUnlock(&this.guard)
        for r in rs {
            std.task.rtFire(r)
        }
    }
}

// A counting semaphore: at most n holders at a time.
struct Semaphore {
    guard   int
    avail   int
    waiters []Task<void>

    // A semaphore with n permits.
    static func New(n int) Semaphore {
        if n < 0 {
            panic("sync: negative Semaphore count")
        }
        return Semaphore{avail: n}
    }

    // Waits for a permit.
    func Acquire() Task<void> {
        std.task.rtLock(&this.guard)
        if this.avail > 0 {
            this.avail -= 1
            std.task.rtUnlock(&this.guard)
            return std.task.Delay(0)
        }
        var s = std.task.rtWaker()
        this.waiters = append(this.waiters, s)
        std.task.rtUnlock(&this.guard)
        return s
    }

    // TryAcquire takes a unit without waiting; false if none is free.
    func TryAcquire() bool {
        std.task.rtLock(&this.guard)
        var ok = this.avail > 0
        if ok {
            this.avail -= 1
        }
        std.task.rtUnlock(&this.guard)
        return ok
    }

    // Returns a permit (to the oldest waiter, if any).
    func Release() {
        std.task.rtLock(&this.guard)
        if len(this.waiters) > 0 {
            var w = this.waiters[0]
            var none Task<void>
            this.waiters[0] = none
            this.waiters = this.waiters[1..]
            std.task.rtUnlock(&this.guard)
            std.task.rtFire(w)
            return
        }
        this.avail += 1
        std.task.rtUnlock(&this.guard)
    }

    // Permits currently free.
    func Available() int {
        std.task.rtLock(&this.guard)
        var n = this.avail
        std.task.rtUnlock(&this.guard)
        return n
    }
}

// A reusable barrier for n tasks: `await b.Wait()` returns when n tasks
// are waiting; then the barrier resets for the next round. Wait yields
// true in exactly one task per round (the last to arrive).
struct Barrier {
    st *barrierState

    // A barrier for n (> 0) parties.
    static func New(n int) Barrier {
        if n <= 0 {
            panic("sync: Barrier needs at least one party")
        }
        return Barrier{st: new barrierState{n: n}}
    }

    // Wait waits until n tasks are waiting; true in the last task to arrive.
    func Wait() Task<bool> {
        return barrierWait(*this)
    }

    // Completed rounds.
    func Rounds() int {
        return this.st.round
    }
}

struct barrierState {
    guard   int
    n       int
    count   int
    round   int
    waiters []Task<void>
}

// Runs a function once (Go's sync.Once); concurrent callers wait (spin
// with thread yields) until the first call finished.
struct Once {
    state int

    // Do calls f if no call of Do has run yet on this Once.
    func Do(f func()) {
        if rtLoad(&this.state) == 2 {
            return
        }
        if rtCas(&this.state, 0, 1) {
            f()
            rtStore(&this.state, 2)
            return
        }
        for rtLoad(&this.state) != 2 {
            rtYield()
        }
    }

    // Done reports whether f has finished.
    func Done() bool {
        return rtLoad(&this.state) == 2
    }
}

// A lock-free int.
struct AtomicInt {
    v int

    // Load returns the value.
    func Load() int {
        return rtLoad(&this.v)
    }

    // Store sets the value.
    func Store(x int) {
        rtStore(&this.v, x)
    }

    // Adds delta; returns the new value.
    func Add(delta int) int {
        return rtAdd(&this.v, delta)
    }

    // Stores x; returns the old value.
    func Swap(x int) int {
        return rtSwap(&this.v, x)
    }

    // CompareAndSwap sets the value to next if it equals old; reports whether it did.
    func CompareAndSwap(old int, next int) bool {
        return rtCas(&this.v, old, next)
    }
}

// A lock-free bool.
struct AtomicBool {
    v int

    // Load returns the value.
    func Load() bool {
        return rtLoad(&this.v) != 0
    }

    // Store sets the value.
    func Store(x bool) {
        rtStore(&this.v, b2i(x))
    }

    // Swap sets the value and returns the old one.
    func Swap(x bool) bool {
        return rtSwap(&this.v, b2i(x)) != 0
    }

    // CompareAndSwap sets the value to next if it equals old; reports whether it did.
    func CompareAndSwap(old bool, next bool) bool {
        return rtCas(&this.v, b2i(old), b2i(next))
    }
}

func b2i(b bool) int {
    if b {
        return 1
    }
    return 0
}

// An atomic cell for any comparable value (pointers, strings, ...),
// guarded by a spin lock; use AtomicInt/AtomicBool for lock-free ints.
struct Atomic<T: Comparable> {
    guard int
    v     T

    // Load returns the value.
    func Load() T {
        std.task.rtLock(&this.guard)
        var x = this.v
        std.task.rtUnlock(&this.guard)
        return x
    }

    // Store sets the value.
    func Store(x T) {
        std.task.rtLock(&this.guard)
        this.v = x
        std.task.rtUnlock(&this.guard)
    }

    // Swap sets the value and returns the old one.
    func Swap(x T) T {
        std.task.rtLock(&this.guard)
        var old = this.v
        this.v = x
        std.task.rtUnlock(&this.guard)
        return old
    }

    // CompareAndSwap sets the value to next if it equals old; reports whether it did.
    func CompareAndSwap(old T, next T) bool {
        std.task.rtLock(&this.guard)
        var ok = this.v == old
        if ok {
            this.v = next
        }
        std.task.rtUnlock(&this.guard)
        return ok
    }
}

// A free list of reusable values (Go's sync.Pool, without GC-driven
// clearing): Get returns a pooled value or a new one from New.
struct Pool<T> {
    guard int
    items []T
    newFn func() T
    max   int

    // A pool creating values with newFn, keeping at most 64 idle values.
    static func New(newFn func() T) Pool<T> {
        return Pool<T>{newFn: newFn, max: 64}
    }

    // Get returns a pooled value, or a new one from newFn (the zero value without one).
    func Get() T {
        std.task.rtLock(&this.guard)
        var n = len(this.items)
        if n > 0 {
            var x = this.items[n - 1]
            var zero T
            this.items[n - 1] = zero
            this.items = this.items[..n - 1]
            std.task.rtUnlock(&this.guard)
            return x
        }
        std.task.rtUnlock(&this.guard)
        if this.newFn == nil {
            var zero T
            return zero
        }
        return this.newFn()
    }

    // Put returns x to the pool (dropped when 64 are already idle).
    func Put(x T) {
        std.task.rtLock(&this.guard)
        if this.max == 0 || len(this.items) < this.max {
            this.items = append(this.items, x)
        }
        std.task.rtUnlock(&this.guard)
    }

    // Idle values held.
    func Len() int {
        std.task.rtLock(&this.guard)
        var n = len(this.items)
        std.task.rtUnlock(&this.guard)
        return n
    }
}

async func barrierWait(self Barrier) bool {
    var s = self.st
    std.task.rtLock(&s.guard)
    s.count += 1
    if s.count == s.n {
        var ws = s.waiters
        s.waiters = nil
        s.count = 0
        s.round += 1
        std.task.rtUnlock(&s.guard)
        for w in ws {
            std.task.rtFire(w)
        }
        return true
    }
    var w = std.task.rtWaker()
    s.waiters = append(s.waiters, w)
    std.task.rtUnlock(&s.guard)
    await w
    return false
}
