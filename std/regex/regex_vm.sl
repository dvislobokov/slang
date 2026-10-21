namespace std.regex

// Compilation of the syntax tree to a byte-level program, the Pike VM and
// the lazy DFA of std.regex (overview in regex.sl).
//
// Instructions (parallel slices of `prog`): 1 byte set (arg = set index,
// a 256-entry membership table in `sets`), 2 split (out preferred over
// out1), 3 save (arg = capture slot), 4 assertion (arg = one flag bit of
// `flagsAt`), 5 match, 6 no-op jump, 7 fail. Rune classes are compiled to
// alternations of UTF-8 byte-range sequences (a trie on the leading
// ranges), so both engines consume bytes.

struct prog {
    op      []int
    arg     []int
    out     []int
    out1    []int
    sets    []bool
    setKeys map[string]int
    start   int

    func emit(op int, arg int) int {
        this.op = append(this.op, op)
        this.arg = append(this.arg, arg)
        this.out = append(this.out, -1)
        this.out1 = append(this.out1, -1)
        return len(this.op) - 1
    }

    // The index of the byte set holding the bytes of the [lo, hi] pairs.
    func set(ranges []int) int {
        var mask = rtBytes(32)
        for i in 0..len(ranges) / 2 {
            for b in ranges[i * 2]..(ranges[i * 2 + 1] + 1) {
                mask[b >> 3] = mask[b >> 3] | byte(1 << (b & 7))
            }
        }
        var key = std.strings.FromBytes(mask)
        var id = this.setKeys[key]
        if id > 0 {
            return id - 1
        }
        var idx = len(this.sets) / 256
        for b in 0..256 {
            this.sets = append(this.sets, (int(mask[b >> 3]) & (1 << (b & 7))) != 0)
        }
        this.setKeys[key] = idx + 1
        return idx
    }

    func size() int {
        return len(this.op)
    }
}

// A program fragment: its entry and its dangling exits (pc*2 + 0 for out,
// pc*2 + 1 for out1).
struct frag {
    start int
    holes []int
}

struct compiler {
    p         *prog
    nodes     []node
    rev       bool
    lastStart int

    func patch(holes []int, target int) {
        for h in holes {
            if (h & 1) == 0 {
                this.p.out[h >> 1] = target
            } else {
                this.p.out1[h >> 1] = target
            }
        }
    }

    func one(op int, arg int) frag {
        var pc = this.p.emit(op, arg)
        return frag{start: pc, holes: [pc * 2]}
    }

    func comp(n int) frag {
        var nd = this.nodes[n]
        if nd.op == 0 {
            return this.one(6, 0)
        }
        if nd.op == 1 {
            return this.compClass(nd.r)
        }
        if nd.op == 2 {
            return this.one(4, nd.min)
        }
        if nd.op == 3 {
            if this.rev {
                return this.comp(nd.subs[0])
            }
            var a = this.one(3, nd.cap * 2)
            var x = this.comp(nd.subs[0])
            var b = this.one(3, nd.cap * 2 + 1)
            this.patch(a.holes, x.start)
            this.patch(x.holes, b.start)
            return frag{start: a.start, holes: b.holes}
        }
        if nd.op == 4 {
            var subs = nd.subs
            var f = frag{start: -1}
            for k in 0..len(subs) {
                var i = k
                if this.rev {
                    i = len(subs) - 1 - k
                }
                var x = this.comp(subs[i])
                if f.start < 0 {
                    f = x
                } else {
                    this.patch(f.holes, x.start)
                    f = frag{start: f.start, holes: x.holes}
                }
            }
            return f
        }
        if nd.op == 5 {
            return this.alt(nd.subs, 0)
        }
        return this.repeat(nd.subs[0], nd.min, nd.max, nd.greedy)
    }

    func alt(subs []int, i int) frag {
        if i == len(subs) - 1 {
            return this.comp(subs[i])
        }
        var sp = this.p.emit(2, 0)
        var x = this.comp(subs[i])
        var y = this.alt(subs, i + 1)
        this.p.out[sp] = x.start
        this.p.out1[sp] = y.start
        var holes = x.holes
        for h in y.holes {
            holes = append(holes, h)
        }
        return frag{start: sp, holes: holes}
    }

    // x? (greedy: try x first)
    func quest(x frag, greedy bool) frag {
        var sp = this.p.emit(2, 0)
        var holes = x.holes
        if greedy {
            this.p.out[sp] = x.start
            holes = append(holes, sp * 2 + 1)
        } else {
            this.p.out1[sp] = x.start
            holes = append(holes, sp * 2)
        }
        return frag{start: sp, holes: holes}
    }

    // x* (as (x+)? when x can match empty, like Go, for the priority order)
    func star(sub int, greedy bool) frag {
        if nullable(this.nodes, sub) {
            var x = this.comp(sub)
            var sp0 = this.p.emit(2, 0)
            this.patch(x.holes, sp0)
            var plus = frag{start: x.start, holes: [sp0 * 2 + 1]}
            if greedy {
                this.p.out[sp0] = x.start
            } else {
                this.p.out1[sp0] = x.start
                plus = frag{start: x.start, holes: [sp0 * 2]}
            }
            return this.quest(plus, greedy)
        }
        var sp = this.p.emit(2, 0)
        var x = this.comp(sub)
        this.patch(x.holes, sp)
        if greedy {
            this.p.out[sp] = x.start
            return frag{start: sp, holes: [sp * 2 + 1]}
        }
        this.p.out1[sp] = x.start
        return frag{start: sp, holes: [sp * 2]}
    }

    func repeat(sub int, min int, max int, greedy bool) frag {
        if min == 0 && max == 0 {
            return this.one(6, 0)
        }
        var f = frag{start: -1}
        for _ in 0..min {
            var x = this.comp(sub)
            f = this.join(f, x)
        }
        if max < 0 {
            if min > 0 {
                // x+ : loop back over the last copy
                var sp = this.p.emit(2, 0)
                this.patch(f.holes, sp)
                var last = this.lastStart
                if greedy {
                    this.p.out[sp] = last
                    return frag{start: f.start, holes: [sp * 2 + 1]}
                }
                this.p.out1[sp] = last
                return frag{start: f.start, holes: [sp * 2]}
            }
            return this.star(sub, greedy)
        }
        // max - min nested optional copies: (x(x(x)?)?)?
        if max > min {
            var opt = this.optional(sub, max - min, greedy)
            f = this.join(f, opt)
        }
        return f
    }

    func optional(sub int, k int, greedy bool) frag {
        var x = this.comp(sub)
        if k > 1 {
            var rest = this.optional(sub, k - 1, greedy)
            this.patch(x.holes, rest.start)
            x = frag{start: x.start, holes: rest.holes}
        }
        return this.quest(x, greedy)
    }

    func join(f frag, x frag) frag {
        this.lastStart = x.start
        if f.start < 0 {
            return x
        }
        this.patch(f.holes, x.start)
        return frag{start: f.start, holes: x.holes}
    }

    // A rune class as UTF-8 byte sequences.
    func compClass(r []int) frag {
        if len(r) == 0 {
            var pc = this.p.emit(7, 0)
            var none []int = []
            return frag{start: pc, holes: none}
        }
        var seqs = utf8Sequences(r)
        var n = len(seqs) / 9
        if this.rev {
            for i in 0..n {
                var k = seqs[i * 9]
                for j in 0..k / 2 {
                    var a = i * 9 + 1 + j * 2
                    var b = i * 9 + 1 + (k - 1 - j) * 2
                    var lo = seqs[a]
                    var hi = seqs[a + 1]
                    seqs[a] = seqs[b]
                    seqs[a + 1] = seqs[b + 1]
                    seqs[b] = lo
                    seqs[b + 1] = hi
                }
            }
        }
        var idx []int = []
        for i in 0..n {
            idx = append(idx, i)
        }
        return this.trie(seqs, idx, 0)
    }

    // Compiles the sequences idx (all at least depth+1 long, sharing the
    // ranges before depth) from position depth on.
    func trie(seqs []int, idx []int, depth int) frag {
        var leaves []int = []
        var groups []frag = []
        var i = 0
        for i < len(idx) {
            var s = idx[i]
            if seqs[s * 9] == depth + 1 {
                leaves = append(leaves, seqs[s * 9 + 1 + depth * 2])
                leaves = append(leaves, seqs[s * 9 + 2 + depth * 2])
                i += 1
                continue
            }
            var lo = seqs[s * 9 + 1 + depth * 2]
            var hi = seqs[s * 9 + 2 + depth * 2]
            var group []int = []
            for i < len(idx) &&
                seqs[idx[i] * 9] > depth + 1 &&
                seqs[idx[i] * 9 + 1 + depth * 2] == lo &&
                seqs[idx[i] * 9 + 2 + depth * 2] == hi {
                group = append(group, idx[i])
                i += 1
            }
            var pc = this.p.emit(1, this.p.set([lo, hi]))
            var child = this.trie(seqs, group, depth + 1)
            this.p.out[pc] = child.start
            groups = append(groups, frag{start: pc, holes: child.holes})
        }
        if len(leaves) > 0 {
            var pc = this.p.emit(1, this.p.set(leaves))
            groups = append(groups, frag{start: pc, holes: [pc * 2]})
        }
        if len(groups) == 1 {
            return groups[0]
        }
        // split chain, in order
        var f = groups[len(groups) - 1]
        var k = len(groups) - 2
        for k >= 0 {
            var sp = this.p.emit(2, 0)
            this.p.out[sp] = groups[k].start
            this.p.out1[sp] = f.start
            var holes = groups[k].holes
            for h in f.holes {
                holes = append(holes, h)
            }
            f = frag{start: sp, holes: holes}
            k -= 1
        }
        return f
    }
}

// True if node n can match the empty string.
func nullable(nodes []node, n int) bool {
    var nd = nodes[n]
    if nd.op == 0 || nd.op == 2 {
        return true
    }
    if nd.op == 1 {
        return false
    }
    if nd.op == 3 {
        return nullable(nodes, nd.subs[0])
    }
    if nd.op == 4 {
        for x in nd.subs {
            if !nullable(nodes, x) {
                return false
            }
        }
        return true
    }
    if nd.op == 5 {
        for x in nd.subs {
            if nullable(nodes, x) {
                return true
            }
        }
        return false
    }
    return nd.min == 0 || nullable(nodes, nd.subs[0])
}

// The UTF-8 byte-range sequences of a normalized rune class: records of 9
// ints [n, lo0, hi0, lo1, hi1, ...] (n = number of bytes; the rune range
// of each record encodes to exactly the byte ranges listed).
func utf8Sequences(r []int) []int {
    var out []int = []
    for i in 0..len(r) / 2 {
        var lo = r[i * 2]
        var hi = r[i * 2 + 1]
        // surrogates never occur in UTF-8 text
        if lo <= 57343 && hi >= 55296 {
            if lo < 55296 {
                out = splitRange(out, lo, 55295)
            }
            if hi > 57343 {
                out = splitRange(out, 57344, hi)
            }
        } else {
            out = splitRange(out, lo, hi)
        }
    }
    return out
}

func splitRange(out0 []int, lo int, hi int) []int {
    var out = out0
    if lo <= 127 && hi > 127 {
        out = splitRange(out, lo, 127)
        return splitRange(out, 128, hi)
    }
    if lo <= 2047 && hi > 2047 {
        out = splitRange(out, lo, 2047)
        return splitRange(out, 2048, hi)
    }
    if lo <= 65535 && hi > 65535 {
        out = splitRange(out, lo, 65535)
        return splitRange(out, 65536, hi)
    }
    if hi <= 127 {
        return appendSeq(out, lo, hi, 1)
    }
    var n = utf8Len(lo)
    for i in 1..n {
        var m = (1 << (6 * i)) - 1
        if (lo - (lo & m)) != (hi - (hi & m)) {
            if (lo & m) != 0 {
                out = splitRange(out, lo, lo | m)
                return splitRange(out, (lo | m) + 1, hi)
            }
            if (hi & m) != m {
                out = splitRange(out, lo, hi - (hi & m) - 1)
                return splitRange(out, hi - (hi & m), hi)
            }
        }
    }
    return appendSeq(out, lo, hi, n)
}

func appendSeq(out0 []int, lo int, hi int, n int) []int {
    var out = out0
    var a = utf8Bytes(lo)
    var b = utf8Bytes(hi)
    out = append(out, n)
    for k in 0..4 {
        if k < n {
            out = append(out, a[k])
            out = append(out, b[k])
        } else {
            out = append(out, 0)
            out = append(out, 0)
        }
    }
    return out
}

func utf8Len(c int) int {
    if c < 128 {
        return 1
    }
    if c < 2048 {
        return 2
    }
    if c < 65536 {
        return 3
    }
    return 4
}

func utf8Bytes(c int) []int {
    if c < 128 {
        return [c]
    }
    if c < 2048 {
        return [192 | (c >> 6), 128 | (c & 63)]
    }
    if c < 65536 {
        return [224 | (c >> 12), 128 | ((c >> 6) & 63), 128 | (c & 63)]
    }
    return [240 | (c >> 18), 128 | ((c >> 12) & 63), 128 | ((c >> 6) & 63), 128 | (c & 63)]
}

// -- empty-width context ------------------------------------------------------

// Context of a byte for assertions: 2 newline, 4 word byte, 0 other; 1 is
// the edge of the text.
func info(b int) int {
    if b == 10 {
        return 2
    }
    if (b >= 48 && b <= 57) || (b >= 65 && b <= 90) || (b >= 97 && b <= 122) || b == 95 {
        return 4
    }
    return 0
}

// The assertions that hold between a byte of context l and one of r.
func flagsAt(l int, r int) int {
    var f = 0
    if (l & 3) != 0 {
        f = f | 1
    }
    if (r & 3) != 0 {
        f = f | 2
    }
    if (l & 1) != 0 {
        f = f | 4
    }
    if (r & 1) != 0 {
        f = f | 8
    }
    if ((l & 4) != 0) != ((r & 4) != 0) {
        f = f | 16
    } else {
        f = f | 32
    }
    return f
}

func flagsOf(s string, pos int) int {
    var l = 1
    var r = 1
    if pos > 0 {
        l = info(int(s[pos - 1]))
    }
    if pos < len(s) {
        r = info(int(s[pos]))
    }
    return flagsAt(l, r)
}

// -- sparse sets ----------------------------------------------------------------

struct sparse {
    dense []int
    index []int
    n     int

    func init(size int) {
        this.dense = rtInts(size)
        this.index = rtInts(size)
        this.n = 0
    }

    func has(pc int) bool {
        var i = this.index[pc]
        return i < this.n && this.dense[i] == pc
    }

    func add(pc int) int {
        var i = this.n
        this.dense[i] = pc
        this.index[pc] = i
        this.n = i + 1
        return i
    }
}

// -- Pike VM ----------------------------------------------------------------------

struct pike {
    p     *prog
    ncap  int
    q0    *sparse
    q1    *sparse
    caps0 []int
    caps1 []int
    stack []int
    vals  []int
    tmp   []int
    best  []int

    func init(p *prog, ncap int) {
        var n = p.size()
        this.p = p
        this.ncap = ncap
        this.q0 = &sparse{}
        this.q1 = &sparse{}
        this.q0.init(n)
        this.q1.init(n)
        this.caps0 = rtInts(n * ncap)
        this.caps1 = rtInts(n * ncap)
        this.tmp = rtInts(ncap)
        this.best = rtInts(ncap)
    }

    // Adds pc and its closure at pos to q (captures from this.tmp).
    func addThread(q *sparse, caps []int, pc0 int, pos int, flags int) {
        var p = this.p
        var ncap = this.ncap
        this.stack = append(this.stack, pc0)
        var base = len(this.stack) - 1
        for len(this.stack) > base {
            var top = len(this.stack) - 1
            var e = this.stack[top]
            this.stack = this.stack[..top]
            if e < 0 {
                var v = this.vals[len(this.vals) - 1]
                this.vals = this.vals[..len(this.vals) - 1]
                this.tmp[-e - 1] = v
                continue
            }
            if q.has(e) {
                continue
            }
            var i = q.add(e)
            var op = p.op[e]
            if op == 2 {
                this.stack = append(this.stack, p.out1[e])
                this.stack = append(this.stack, p.out[e])
            } else if op == 3 {
                var slot = p.arg[e]
                if slot < ncap {
                    this.vals = append(this.vals, this.tmp[slot])
                    this.stack = append(this.stack, -slot - 1)
                    this.tmp[slot] = pos
                }
                this.stack = append(this.stack, p.out[e])
            } else if op == 4 {
                if (flags & p.arg[e]) != 0 {
                    this.stack = append(this.stack, p.out[e])
                }
            } else if op == 6 {
                this.stack = append(this.stack, p.out[e])
            } else if op == 1 || op == 5 {
                for k in 0..ncap {
                    caps[i * ncap + k] = this.tmp[k]
                }
            }
        }
    }

    // Leftmost-first match at or after pos (only at pos if anchored); the
    // capture slots (empty if none).
    func run(s string, pos int, anchored bool) []int {
        var p = this.p
        var ncap = this.ncap
        var n = len(s)
        var matched = false
        this.q0.n = 0
        this.q1.n = 0
        var cur = this.q0
        var nxt = this.q1
        var ccaps = this.caps0
        var ncaps = this.caps1
        var i = pos
        for {
            if !matched && (i == pos || !anchored) {
                for k in 0..ncap {
                    this.tmp[k] = -1
                }
                this.addThread(cur, ccaps, p.start, i, flagsOf(s, i))
            }
            if cur.n == 0 {
                break
            }
            var c = -1
            if i < n {
                c = int(s[i])
            }
            var nflags = 0
            if i < n {
                nflags = flagsOf(s, i + 1)
            }
            nxt.n = 0
            for t in 0..cur.n {
                var pc = cur.dense[t]
                var op = p.op[pc]
                if op == 5 {
                    for k in 0..ncap {
                        this.best[k] = ccaps[t * ncap + k]
                    }
                    matched = true
                    break
                }
                if op == 1 && c >= 0 && p.sets[p.arg[pc] * 256 + c] {
                    for k in 0..ncap {
                        this.tmp[k] = ccaps[t * ncap + k]
                    }
                    this.addThread(nxt, ncaps, p.out[pc], i + 1, nflags)
                }
            }
            if i >= n {
                break
            }
            var tq = cur
            cur = nxt
            nxt = tq
            var tc = ccaps
            ccaps = ncaps
            ncaps = tc
            i += 1
        }
        if !matched {
            var none []int = []
            return none
        }
        var out = rtInts(ncap)
        for k in 0..ncap {
            out[k] = this.best[k]
        }
        return out
    }
}

// -- lazy DFA -----------------------------------------------------------------------

// A DFA state is an ordered list of instructions (byte sets, matches and
// unresolved assertions) plus the context of the byte before it (only
// when it has assertions). Transitions are computed on first use and
// cached in `trans` (next*2 + 1 if a match ends before the byte, -1 if not
// computed yet); class `ncls` is the end of the text. State 0 is dead.
struct dfa {
    p          *prog
    rev        bool
    longest    bool
    unanchored bool
    cls        []int
    rep        []int
    ncls       int
    stride     int
    soff       []int
    slen       []int
    sctx       []int
    sstart     []bool
    pool       []int
    keys       map[string]int
    trans      []int
    starts     []int
    v1         *sparse
    v2         *sparse
    list       []int
    stack      []int
    key        []byte

    func init(p *prog, rev bool, longest bool, unanchored bool, cls []int, ncls int) {
        this.p = p
        this.rev = rev
        this.longest = longest
        this.unanchored = unanchored
        this.cls = cls
        this.ncls = ncls
        this.stride = ncls + 1
        this.rep = rtInts(ncls)
        for b in 0..256 {
            this.rep[cls[b]] = b
        }
        this.v1 = &sparse{}
        this.v2 = &sparse{}
        this.v1.init(p.size())
        this.v2.init(p.size())
        this.reset()
    }

    func reset() {
        this.soff = []
        this.slen = []
        this.sctx = []
        this.sstart = []
        this.pool = []
        this.keys = map[string]int{}
        this.trans = []
        this.starts = rtInts(8)
        var empty []int = []
        this.addState(empty, 0, false)
    }

    func addState(list []int, ctx int, start bool) int {
        var id = len(this.soff)
        this.soff = append(this.soff, len(this.pool))
        this.slen = append(this.slen, len(list))
        this.sctx = append(this.sctx, ctx)
        this.sstart = append(this.sstart, start)
        for x in list {
            this.pool = append(this.pool, x)
        }
        for _ in 0..this.stride {
            this.trans = append(this.trans, -1)
        }
        return id
    }

    // The state for list/ctx, created if new.
    func state(list []int, ctx0 int, start bool) int {
        if len(list) == 0 {
            return 0
        }
        var ctx = ctx0
        var asserts = false
        for pc in list {
            if this.p.op[pc] == 4 {
                asserts = true
            }
        }
        if !asserts {
            ctx = 0
        }
        this.key = this.key[..0]
        this.key = append(this.key, byte(ctx))
        if start {
            this.key = append(this.key, byte(1))
        } else {
            this.key = append(this.key, byte(0))
        }
        for pc in list {
            this.key = append(this.key, byte(pc & 255))
            this.key = append(this.key, byte((pc >> 8) & 255))
            this.key = append(this.key, byte(pc >> 16))
        }
        var k = std.strings.FromBytes(this.key)
        var id = this.keys[k]
        if id > 0 {
            return id - 1
        }
        if len(this.soff) >= 4000 || len(this.pool) >= 400000 {
            // cache full: start over (the list is copied first)
            var copy []int = []
            for pc in list {
                copy = append(copy, pc)
            }
            this.reset()
            var nid = this.addState(copy, ctx, start)
            this.keys[k] = nid + 1
            return nid
        }
        var nid = this.addState(list, ctx, start)
        this.keys[k] = nid + 1
        return nid
    }

    // Appends the closure of pc (through splits, saves and no-ops) to
    // this.list, using v as the visited set. With flags >= 0 assertions are
    // resolved, otherwise kept.
    func closure(v *sparse, pc0 int, flags int) {
        var p = this.p
        this.stack = append(this.stack, pc0)
        for len(this.stack) > 0 {
            var top = len(this.stack) - 1
            var pc = this.stack[top]
            this.stack = this.stack[..top]
            if pc < 0 || v.has(pc) {
                continue
            }
            v.add(pc)
            var op = p.op[pc]
            if op == 2 {
                this.stack = append(this.stack, p.out1[pc])
                this.stack = append(this.stack, p.out[pc])
            } else if op == 3 || op == 6 {
                this.stack = append(this.stack, p.out[pc])
            } else if op == 4 {
                if flags < 0 {
                    this.list = append(this.list, pc)
                } else if (flags & p.arg[pc]) != 0 {
                    this.stack = append(this.stack, p.out[pc])
                }
            } else if op == 1 || op == 5 {
                this.list = append(this.list, pc)
            }
        }
    }

    // The start state for the context of the byte before the start.
    func startState(ctx int) int {
        var s = this.starts[ctx]
        if s > 0 {
            return s - 1
        }
        this.list = this.list[..0]
        this.v1.n = 0
        this.closure(this.v1, this.p.start, -1)
        var id = this.state(this.list, ctx, true)
        this.starts[ctx] = id + 1
        return id
    }

    // Computes the transition of state st on byte class k.
    func step(st int, k int) int {
        var p = this.p
        var end = k == this.ncls
        var c = -1
        var ci = 1
        if !end {
            c = this.rep[k]
            ci = info(c)
        }
        var flags = 0
        if this.rev {
            flags = flagsAt(ci, this.sctx[st])
        } else {
            flags = flagsAt(this.sctx[st], ci)
        }
        // resolve assertions, in priority order
        this.list = this.list[..0]
        this.v1.n = 0
        var off = this.soff[st]
        for j in 0..this.slen[st] {
            this.closure(this.v1, this.pool[off + j], flags)
        }
        var expanded []int = []
        for pc in this.list {
            expanded = append(expanded, pc)
        }
        // step on the byte
        this.list = this.list[..0]
        this.v2.n = 0
        var matched = false
        for pc in expanded {
            var op = p.op[pc]
            if op == 5 {
                matched = true
                if !this.longest {
                    break
                }
            } else if op == 1 && !end && p.sets[p.arg[pc] * 256 + c] {
                this.closure(this.v2, p.out[pc], -1)
            }
        }
        if this.unanchored && !end && !(matched && !this.longest) {
            this.closure(this.v2, p.start, -1)
        }
        var next = 0
        if !end {
            next = this.state(this.list, ci, false)
        }
        var t = next * 2
        if matched {
            t += 1
        }
        // `state` may have reset the cache: st is then gone, only the
        // result matters to the caller.
        if st < len(this.slen) {
            this.trans[st * this.stride + k] = t
        }
        return t
    }

    // Forward search from pos: the end of the leftmost-first match, or -1.
    // With earliest it returns as soon as any match is known.
    func forward(s string, pos int, earliest bool, prefix string) int {
        var n = len(s)
        var ctx = 1
        if pos > 0 {
            ctx = info(int(s[pos - 1]))
        }
        var st = this.startState(ctx)
        var last = -1
        var i = pos
        var cls = this.cls
        var trans = this.trans
        var stride = this.stride
        var accel = prefix != "" && this.unanchored
        for i < n {
            if accel && last < 0 && this.sstart[st] {
                var j = rtIndex(s, prefix, i)
                if j < 0 {
                    return -1
                }
                if j > i {
                    i = j
                    st = this.startState(info(int(s[i - 1])))
                    trans = this.trans
                }
            }
            var k = cls[int(s[i])]
            var t = trans[st * stride + k]
            if t < 0 {
                t = this.step(st, k)
                trans = this.trans
            }
            if (t & 1) != 0 {
                last = i
                if earliest {
                    return last
                }
            }
            st = t >> 1
            if st == 0 {
                return last
            }
            i += 1
        }
        var t = trans[st * stride + this.ncls]
        if t < 0 {
            t = this.step(st, this.ncls)
        }
        if (t & 1) != 0 {
            last = n
        }
        return last
    }

    // Reverse longest search from end down to lo: the smallest start of a
    // match ending at end, or -1.
    func backward(s string, end int, lo int) int {
        var ctx = 1
        if end < len(s) {
            ctx = info(int(s[end]))
        }
        var st = this.startState(ctx)
        var last = -1
        var i = end
        var cls = this.cls
        var trans = this.trans
        var stride = this.stride
        for i > lo {
            var k = cls[int(s[i - 1])]
            var t = trans[st * stride + k]
            if t < 0 {
                t = this.step(st, k)
                trans = this.trans
            }
            if (t & 1) != 0 {
                last = i
            }
            st = t >> 1
            if st == 0 {
                return last
            }
            i -= 1
        }
        var k = this.ncls
        if lo > 0 {
            k = cls[int(s[lo - 1])]
        }
        var t = trans[st * stride + k]
        if t < 0 {
            t = this.step(st, k)
        }
        if (t & 1) != 0 {
            last = lo
        }
        return last
    }
}

// Byte classes: bytes that no byte set (nor the assertion context)
// distinguishes share a class. Returns the 256 class ids followed by the
// number of classes.
func byteClasses(p *prog) []int {
    var cls = rtInts(257)
    var n = 1
    var nsets = len(p.sets) / 256
    for k in 0..(nsets + 2) {
        var remap = map[int]int{}
        var m = 0
        for b in 0..256 {
            var member = false
            if k < nsets {
                member = p.sets[k * 256 + b]
            } else if k == nsets {
                member = b == 10
            } else {
                member = info(b) == 4
            }
            var key = cls[b] * 2
            if member {
                key += 1
            }
            var id = remap[key]
            if id == 0 {
                m += 1
                id = m
                remap[key] = id
            }
            cls[b] = id - 1
        }
        n = m
    }
    cls[256] = n
    return cls
}
