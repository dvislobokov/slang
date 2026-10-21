// std.debug, part 2: the program's tables, frames and values seen through
// std.unsafe (runtime/ABI.md section 23), the value formatter (like
// tools/lldb/slang_formatters.py) and the v1 expression evaluator
// (variable paths and comparisons).
namespace std.debug

func rd(addr int) int {
    return std.unsafe.ReadInt(addr, 8)
}

func rd32(addr int) int {
    return std.unsafe.ReadInt(addr, 4)
}

// The static tables emitted by `--debug-agent` (ABI.md 23.1).
struct tables {
    addr   int
    nstmts int
    stmts  int
    nfuncs int
    funcs  int
    nfiles int
    files  []string

    func stmtFile(i int) int {
        return rd32(this.stmts + 24 * i)
    }

    func stmtLine(i int) int {
        return rd32(this.stmts + 24 * i + 4)
    }

    func stmtCol(i int) int {
        return rd32(this.stmts + 24 * i + 8)
    }

    func stmtFunc(i int) int {
        return rd32(this.stmts + 24 * i + 12)
    }

    func stmtKind(i int) int {
        return rd32(this.stmts + 24 * i + 20)
    }

    func funcAddr(i int) int {
        return this.funcs + 56 * i
    }

    func funcName(i int) string {
        if i < 0 || i >= this.nfuncs {
            return "?"
        }
        return rtString(this.funcAddr(i))
    }

    func funcShort(i int) string {
        if i < 0 || i >= this.nfuncs {
            return "?"
        }
        return rtString(this.funcAddr(i) + 8)
    }

    func funcEntry(i int) int {
        return rd32(this.funcAddr(i) + 24)
    }

    // Index of the function record at address p (from a frame).
    func funcIndex(p int) int {
        if p == 0 || p < this.funcs {
            return -1
        }
        return (p - this.funcs) / 56
    }

    func fileName(i int) string {
        if i < 0 || i >= len(this.files) {
            return "?"
        }
        return this.files[i]
    }
}

func loadTables() tables {
    var a = rtTables()
    if a == 0 {
        return tables{}
    }
    var t = tables{
        addr: a,
        nstmts: rd(a + 8),
        stmts: rd(a + 16),
        nfuncs: rd(a + 24),
        funcs: rd(a + 32),
        nfiles: rd(a + 40),
    }
    var files []string = []
    var fp = rd(a + 48)
    for i in 0..t.nfiles {
        files = append(files, rtString(fp + 8 * i))
    }
    t.files = files
    return t
}

// A local variable of a frame: where its value lives and its type.
struct local {
    name string
    addr int
    ti   int
}

// The visible locals of frame f (a slang_dbg_frame), innermost
// declaration of each name, in declaration order.
func frameLocals(t tables, f int) []local {
    var out []local = []
    var fi = t.funcIndex(rd(f + 8))
    var arr = rd(f + 16)
    if fi < 0 || arr == 0 {
        return out
    }
    var stmt = rd32(f + 24)
    var line = 0
    if stmt >= 0 && stmt < t.nstmts {
        line = t.stmtLine(stmt)
    }
    var fa = t.funcAddr(fi)
    var n = rd32(fa + 32)
    var recs = rd(fa + 40)
    var fline = rd32(fa + 20)
    for j in 0..n {
        var r = recs + 32 * j
        var decl = rd32(r + 24)
        var end = rd32(r + 28)
        var visible = line >= decl && line <= end
        if decl > fline && line == decl {
            // A local declared on this line is not assigned yet.
            visible = false
        }
        if !visible {
            continue
        }
        var name = rtString(r)
        var slot = rd(arr + 8 * rd32(r + 16))
        var addr = slot
        if rd32(r + 20) & 1 != 0 {
            var cell = rd(slot)
            if cell == 0 {
                continue
            }
            addr = cell + 16
        }
        var l = local{name: name, addr: addr, ti: rd(r + 8)}
        var dup = -1
        for k in 0..len(out) {
            if out[k].name == name {
                dup = k
            }
        }
        if dup >= 0 {
            out[dup] = l
        } else {
            out = append(out, l)
        }
    }
    return out
}

// std.reflect.Kind values (the order of the enum in reflect.sl).

func kVoid() int { return 1 }
func kBool() int { return 2 }
func kInt() int { return 3 }
func kInt8() int { return 4 }
func kInt16() int { return 5 }
func kInt32() int { return 6 }
func kUint() int { return 7 }
func kUint8() int { return 8 }
func kUint16() int { return 9 }
func kUint32() int { return 10 }
func kFloat() int { return 11 }
func kFloat32() int { return 12 }
func kChar() int { return 13 }
func kString() int { return 14 }
func kError() int { return 15 }
func kAny() int { return 16 }
func kInterface() int { return 17 }
func kStruct() int { return 18 }
func kEnum() int { return 19 }
func kPointer() int { return 20 }
func kWeak() int { return 21 }
func kSlice() int { return 22 }
func kArray() int { return 23 }
func kMap() int { return 24 }
func kTuple() int { return 25 }
func kFunc() int { return 26 }
func kFallible() int { return 27 }
func kTask() int { return 28 }

// A std.reflect.TypeInfo read through memory (ABI.md 23.4 gives the
// offsets); ti is its address.
func tiKind(ti int) int {
    if ti == 0 {
        return 0
    }
    return rd32(ti + 24)
}

func tiName(ti int) string {
    if ti == 0 {
        return "?"
    }
    return rtString(ti + 32)
}

func tiSize(ti int) int {
    return rd(ti + 48)
}

func tiElem(ti int) int {
    return rd(ti + 160)
}

func tiKey(ti int) int {
    return rd(ti + 168)
}

func tiLen(ti int) int {
    return rd(ti + 176)
}

func tiNFields(ti int) int {
    return rd(ti + 72)
}

// FieldInfo i of struct type ti.
func tiField(ti int, i int) int {
    return rd(rd(ti + 64) + 8 * i)
}

func fieldName(fi int) string {
    return rtString(fi)
}

func fieldType(fi int) int {
    return rd(fi + 8)
}

func fieldOffset(fi int) int {
    return rd(fi + 16)
}

func tiNVariants(ti int) int {
    return rd(ti + 136)
}

func tiVariant(ti int, i int) int {
    return rd(rd(ti + 128) + 8 * i)
}

func variantNFields(v int) int {
    return rd(v + 24)
}

func variantField(v int, i int) int {
    return rd(rd(v + 16) + 8 * i)
}

func isDataEnum(ti int) bool {
    var n = tiNVariants(ti)
    for i in 0..n {
        if variantNFields(tiVariant(ti, i)) > 0 {
            return true
        }
    }
    return tiSize(ti) > 4
}

func roundUp8(n int) int {
    return (n + 7) / 8 * 8
}

// A child of a value: name, address, type.
struct child {
    name string
    addr int
    ti   int
}

// The dynamic value of an interface or `any` at addr: (address, type).
func dynamic(addr int, ti int) child {
    var k = tiKind(ti)
    if k == kAny() {
        var t = rd(addr + 8)
        var tk = tiKind(t)
        if t != 0 && tk >= kBool() && tk <= kChar() {
            // A scalar is held inline: the data word is the value.
            return child{name: "", addr: addr, ti: t}
        }
        return child{name: "", addr: rd(addr), ti: t}
    }
    var vt = rd(addr + 8)
    if vt == 0 {
        return child{}
    }
    var t = rd(vt)
    if rd(vt + 8) != 0 {
        return child{name: "", addr: rd(addr), ti: t}
    }
    return child{name: "", addr: addr, ti: t}
}

// Whether a value of type ti at addr has children to expand.
func hasChildren(addr int, ti int) bool {
    var k = tiKind(ti)
    if k == kStruct() || k == kTuple() {
        return tiNFields(ti) > 0
    }
    if k == kPointer() {
        return rd(addr) != 0 && hasChildren(rd(addr), tiElem(ti))
    }
    if k == kSlice() {
        return rd(addr + 8) > 0
    }
    if k == kArray() {
        return tiLen(ti) > 0
    }
    if k == kMap() {
        return rtMapLen(rd(addr)) > 0
    }
    if k == kAny() || k == kInterface() {
        var d = dynamic(addr, ti)
        return d.ti != 0 && hasChildren(d.addr, d.ti)
    }
    if k == kEnum() {
        if !isDataEnum(ti) {
            return false
        }
        var tag = rd32(addr)
        return tag >= 0 && tag < tiNVariants(ti) && variantNFields(tiVariant(ti, tag)) > 0
    }
    if k == kFallible() {
        var e = tiElem(ti)
        var err = fallibleErr(addr, ti)
        return err == 0 && tiKind(e) != kVoid() && hasChildren(addr, e)
    }
    if k == kWeak() {
        var p = weakTarget(addr)
        return p != 0 && hasChildren(p, tiElem(ti))
    }
    return false
}

// Number of indexed children (slices, arrays, maps) for paging.
func indexedCount(addr int, ti int) int {
    var k = tiKind(ti)
    if k == kSlice() {
        return rd(addr + 8)
    }
    if k == kArray() {
        return tiLen(ti)
    }
    if k == kMap() {
        return rtMapLen(rd(addr))
    }
    if k == kPointer() && rd(addr) != 0 {
        return indexedCount(rd(addr), tiElem(ti))
    }
    return 0
}

func fallibleErr(addr int, ti int) int {
    var e = tiElem(ti)
    if tiKind(e) == kVoid() {
        return rd(addr)
    }
    return rd(addr + roundUp8(tiSize(e)))
}

// Payload address of the target of a weak pointer, 0 if gone.
func weakTarget(addr int) int {
    var w = rd(addr)
    if w == 0 {
        return 0
    }
    var obj = rd(w + 16)
    if obj == 0 {
        return 0
    }
    return obj + 16
}

// Children start..start+count of a value (count < 0: all).
func children(addr int, ti int, start int, count int) []child {
    var out []child = []
    var k = tiKind(ti)
    if k == kStruct() || k == kTuple() {
        for i in 0..tiNFields(ti) {
            var f = tiField(ti, i)
            out = append(out, child{
                name: fieldName(f),
                addr: addr + fieldOffset(f),
                ti: fieldType(f),
            })
        }
        return out
    }
    if k == kPointer() {
        if rd(addr) == 0 {
            return out
        }
        return children(rd(addr), tiElem(ti), start, count)
    }
    if k == kAny() || k == kInterface() {
        var d = dynamic(addr, ti)
        if d.ti == 0 {
            return out
        }
        return children(d.addr, d.ti, start, count)
    }
    if k == kWeak() {
        var p = weakTarget(addr)
        if p == 0 {
            return out
        }
        return children(p, tiElem(ti), start, count)
    }
    if k == kFallible() {
        if fallibleErr(addr, ti) != 0 {
            return out
        }
        return children(addr, tiElem(ti), start, count)
    }
    if k == kEnum() {
        var tag = rd32(addr)
        if tag < 0 || tag >= tiNVariants(ti) {
            return out
        }
        var v = tiVariant(ti, tag)
        for i in 0..variantNFields(v) {
            var f = variantField(v, i)
            out = append(out, child{
                name: fieldName(f),
                addr: addr + fieldOffset(f),
                ti: fieldType(f),
            })
        }
        return out
    }
    if k == kSlice() || k == kArray() {
        var n = tiLen(ti)
        var data = addr
        if k == kSlice() {
            n = rd(addr + 8)
            data = rd(addr)
        }
        var e = tiElem(ti)
        var sz = tiSize(e)
        var end = n
        if count >= 0 && start + count < n {
            end = start + count
        }
        var i = start
        for i < end {
            out = append(out, child{
                name: "[" + std.strconv.Itoa(i) + "]",
                addr: data + i * sz,
                ti: e,
            })
            i += 1
        }
        return out
    }
    if k == kMap() {
        var m = rd(addr)
        var n = rtMapLen(m)
        var c = n
        if count >= 0 {
            c = count
        }
        var ents = rtMapEntries(m, start, c)
        var kt = tiKey(ti)
        var vt = tiElem(ti)
        var i = 0
        for i + 1 < len(ents) {
            out = append(out, child{
                name: "[" + format(ents[i], kt, 1) + "]",
                addr: ents[i + 1],
                ti: vt,
            })
            i += 2
        }
        return out
    }
    return out
}

func quoteLimited(s string) string {
    if len(s) > 512 {
        return std.strconv.Quote(s[..512]) + "..."
    }
    return std.strconv.Quote(s)
}

func errorText(e int) string {
    var ty = rtErrorType(e)
    var msg = rtErrorText(e)
    if ty != "" {
        return ty + "(" + quoteLimited(msg) + ")"
    }
    return quoteLimited(msg)
}

// The display text of the value of type ti at addr; depth limits how far
// aggregates are summarised inline.
func format(addr int, ti int, depth int) string {
    var k = tiKind(ti)
    if ti == 0 || k == 0 {
        return "?"
    }
    if k == kVoid() {
        return ""
    }
    if k == kBool() {
        if std.unsafe.ReadUint(addr, 1) != 0 {
            return "true"
        }
        return "false"
    }
    if k == kInt() {
        return std.strconv.Itoa(rd(addr))
    }
    if k == kInt8() {
        return std.strconv.Itoa(std.unsafe.ReadInt(addr, 1))
    }
    if k == kInt16() {
        return std.strconv.Itoa(std.unsafe.ReadInt(addr, 2))
    }
    if k == kInt32() {
        return std.strconv.Itoa(std.unsafe.ReadInt(addr, 4))
    }
    if k == kUint() {
        return std.strconv.FormatUint(uint(rd(addr)), 10)
    }
    if k == kUint8() {
        return std.strconv.Itoa(std.unsafe.ReadUint(addr, 1))
    }
    if k == kUint16() {
        return std.strconv.Itoa(std.unsafe.ReadUint(addr, 2))
    }
    if k == kUint32() {
        return std.strconv.Itoa(std.unsafe.ReadUint(addr, 4))
    }
    if k == kFloat() {
        return std.strconv.FormatFloat(std.unsafe.ReadFloat(addr, 8), 'g', -1)
    }
    if k == kFloat32() {
        return std.strconv.FormatFloat(std.unsafe.ReadFloat(addr, 4), 'g', -1)
    }
    if k == kChar() {
        return std.strconv.QuoteRune(char(std.unsafe.ReadInt(addr, 4)))
    }
    if k == kString() {
        return quoteLimited(rtString(addr))
    }
    if k == kError() {
        var e = rd(addr)
        if e == 0 {
            return "nil"
        }
        return errorText(e)
    }
    if k == kPointer() {
        var p = rd(addr)
        if p == 0 {
            return "nil"
        }
        if depth <= 0 {
            return "*" + tiName(tiElem(ti))
        }
        return "*" + format(p, tiElem(ti), depth)
    }
    if k == kWeak() {
        if rd(addr) == 0 {
            return "nil"
        }
        var p = weakTarget(addr)
        if p == 0 {
            return "weak (dropped)"
        }
        return "weak " + format(p, tiElem(ti), depth - 1)
    }
    if k == kAny() || k == kInterface() {
        var d = dynamic(addr, ti)
        if d.ti == 0 {
            return "nil"
        }
        return tiName(d.ti) + "(" + format(d.addr, d.ti, depth - 1) + ")"
    }
    if k == kFallible() {
        var err = fallibleErr(addr, ti)
        if err != 0 {
            return "error: " + errorText(err)
        }
        if tiKind(tiElem(ti)) == kVoid() {
            return "ok"
        }
        return format(addr, tiElem(ti), depth)
    }
    if k == kStruct() {
        if depth <= 0 {
            return tiName(ti) + "{...}"
        }
        var parts []string = []
        var n = tiNFields(ti)
        for i in 0..n {
            if i >= 6 {
                parts = append(parts, "...")
                break
            }
            var f = tiField(ti, i)
            parts = append(
                parts,
                fieldName(f) + ": " + format(addr + fieldOffset(f), fieldType(f), depth - 1),
            )
        }
        return tiName(ti) + "{" + std.strings.Join(parts, ", ") + "}"
    }
    if k == kTuple() {
        if depth <= 0 {
            return "(...)"
        }
        var parts []string = []
        for i in 0..tiNFields(ti) {
            var f = tiField(ti, i)
            parts = append(parts, format(addr + fieldOffset(f), fieldType(f), depth - 1))
        }
        return "(" + std.strings.Join(parts, ", ") + ")"
    }
    if k == kEnum() {
        var tag = rd32(addr)
        if tag < 0 || tag >= tiNVariants(ti) {
            return tiName(ti) + "(" + std.strconv.Itoa(tag) + ")"
        }
        var v = tiVariant(ti, tag)
        var name = rtString(v)
        var nf = variantNFields(v)
        if nf == 0 {
            return tiName(ti) + "." + name
        }
        if depth <= 0 {
            return name + "(...)"
        }
        var parts []string = []
        for i in 0..nf {
            var f = variantField(v, i)
            parts = append(
                parts,
                fieldName(f) + ": " + format(addr + fieldOffset(f), fieldType(f), depth - 1),
            )
        }
        return name + "(" + std.strings.Join(parts, ", ") + ")"
    }
    if k == kSlice() || k == kArray() {
        var n = tiLen(ti)
        var prefix = ""
        if k == kSlice() {
            n = rd(addr + 8)
            prefix = "len " +
                std.strconv.Itoa(n) +
                ", cap " +
                std.strconv.Itoa(rd(addr + 16)) +
                ": "
        }
        if depth <= 0 {
            return prefix + "[...]"
        }
        var parts []string = []
        for c in children(addr, ti, 0, 8) {
            parts = append(parts, format(c.addr, c.ti, depth - 1))
        }
        if n > 8 {
            parts = append(parts, "...")
        }
        return prefix + "[" + std.strings.Join(parts, " ") + "]"
    }
    if k == kMap() {
        var m = rd(addr)
        var n = rtMapLen(m)
        if m == 0 {
            return "map[] (nil)"
        }
        if depth <= 0 {
            return "map[...] len " + std.strconv.Itoa(n)
        }
        var parts []string = []
        var ents = rtMapEntries(m, 0, 8)
        var i = 0
        for i + 1 < len(ents) {
            parts = append(
                parts,
                format(ents[i], tiKey(ti), 1) + ":" + format(ents[i + 1], tiElem(ti), depth - 1),
            )
            i += 2
        }
        if n > 8 {
            parts = append(parts, "...")
        }
        return "map[" + std.strings.Join(parts, " ") + "]"
    }
    if k == kTask() {
        var t = rd(addr)
        if t == 0 {
            return "nil"
        }
        if !rtTaskDone(t) {
            return "Task (running)"
        }
        var e = tiElem(ti)
        if tiKind(e) == kVoid() {
            return "Task (done)"
        }
        return "Task (done: " + format(rtTaskSlot(t), e, depth - 1) + ")"
    }
    if k == kFunc() {
        var fp = rd(addr)
        if fp == 0 {
            return "nil"
        }
        var s = rtSymName(fp)
        if s == "" {
            return "func"
        }
        return "func " + s
    }
    return tiName(ti)
}

// ---- expressions (evaluate v1) -------------------------------------------

// A token of an expression: kind 1 identifier, 2 number, 3 string,
// 4 operator / punctuation.
struct token {
    kind int
    text string
}

func isIdentByte(c byte) bool {
    return c == 95 || (c >= 97 && c <= 122) || (c >= 65 && c <= 90) || (c >= 48 && c <= 57)
}

func tokenize(s string) []token! {
    var out []token = []
    var b = std.strings.Bytes(s)
    var i = 0
    for i < len(b) {
        var c = b[i]
        if c == 32 || c == 9 {
            i += 1
            continue
        }
        if (c >= 48 && c <= 57) ||
            (c == 45 &&
                i + 1 < len(b) &&
                b[i + 1] >= 48 &&
                b[i + 1] <= 57 &&
                (len(out) == 0 || out[len(out) - 1].kind == 4)) {
            var j = i + 1
            for j < len(b) && (isIdentByte(b[j]) || b[j] == 46) {
                j += 1
            }
            out = append(out, token{kind: 2, text: s[i..j]})
            i = j
            continue
        }
        if isIdentByte(c) {
            var j = i
            for j < len(b) && isIdentByte(b[j]) {
                j += 1
            }
            out = append(out, token{kind: 1, text: s[i..j]})
            i = j
            continue
        }
        if c == 34 {
            var j = i + 1
            for j < len(b) && b[j] != 34 {
                if b[j] == 92 {
                    j += 1
                }
                j += 1
            }
            if j >= len(b) {
                return error("unterminated string")
            }
            var lit = std.strconv.Unquote(s[i..j + 1])?
            out = append(out, token{kind: 3, text: lit})
            i = j + 1
            continue
        }
        if i + 1 < len(b) {
            var two = s[i..i + 2]
            if two == "==" ||
                two == "!=" ||
                two == "<=" ||
                two == ">=" ||
                two == "&&" ||
                two == "||" {
                out = append(out, token{kind: 4, text: two})
                i += 2
                continue
            }
        }
        out = append(out, token{kind: 4, text: s[i..i + 1]})
        i += 1
    }
    return out
}

// A value computed by an expression: a place in memory (ti != 0), or a
// literal (kind: 1 int, 2 float, 3 string, 4 bool, 5 nil).
struct evalue {
    // the identifier of an unresolved name (lit 8)
    name string
    addr int
    ti   int
    lit  int
    i    int
    f    float
    s    string
    b    bool
}

struct evaluator {
    toks   []token
    pos    int
    locals []local

    func peek() string {
        if this.pos < len(this.toks) {
            return this.toks[this.pos].text
        }
        return ""
    }

    func expr() evalue! {
        var l = this.and()?
        for this.peek() == "||" {
            this.pos += 1
            var r = this.and()?
            l = evalue{lit: 4, b: truth(l)? || truth(r)?}
        }
        return l
    }

    func and() evalue! {
        var l = this.cmp()?
        for this.peek() == "&&" {
            this.pos += 1
            var r = this.cmp()?
            l = evalue{lit: 4, b: truth(l)? && truth(r)?}
        }
        return l
    }

    func cmp() evalue! {
        var l = this.unary()?
        var op = this.peek()
        if op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=" {
            this.pos += 1
            var r = this.unary()?
            return evalue{lit: 4, b: compare(l, op, r)?}
        }
        return l
    }

    func unary() evalue! {
        if this.peek() == "!" {
            this.pos += 1
            var v = this.unary()?
            return evalue{lit: 4, b: !truth(v)?}
        }
        if this.peek() == "(" {
            this.pos += 1
            var v = this.expr()?
            if this.peek() != ")" {
                return error("expected )")
            }
            this.pos += 1
            return v
        }
        return this.path()?
    }

    func path() evalue! {
        if this.pos >= len(this.toks) {
            return error("expression expected")
        }
        var t = this.toks[this.pos]
        this.pos += 1
        if t.kind == 2 {
            if std.strings.Contains(t.text, ".") {
                return evalue{lit: 2, f: std.strconv.ParseFloat(t.text)?}
            }
            return evalue{lit: 1, i: std.strconv.ParseInt(t.text, 0)?}
        }
        if t.kind == 3 {
            return evalue{lit: 3, s: t.text}
        }
        if t.kind != 1 {
            return error("unexpected " + std.strconv.Quote(t.text))
        }
        if t.text == "true" || t.text == "false" {
            return evalue{lit: 4, b: t.text == "true"}
        }
        if t.text == "nil" {
            return evalue{lit: 5}
        }
        if t.text == "len" && this.peek() == "(" {
            this.pos += 1
            var v = this.expr()?
            if this.peek() != ")" {
                return error("expected )")
            }
            this.pos += 1
            return evalue{lit: 1, i: lengthOf(v)?}
        }
        var found = -1
        for i in 0..len(this.locals) {
            if this.locals[i].name == t.text {
                found = i
            }
        }
        if found < 0 {
            // An enum variant name (`Green`, `Color.Green`), only valid in
            // a comparison with an enum value.
            if this.peek() == "[" || this.peek() == "(" {
                return error("unknown variable " + t.text)
            }
            var vname = t.text
            if this.peek() == "." &&
                this.pos + 1 < len(this.toks) &&
                this.toks[this.pos + 1].kind == 1 {
                vname = this.toks[this.pos + 1].text
                this.pos += 2
            }
            return evalue{lit: 8, s: vname, name: t.text}
        }
        var v = evalue{addr: this.locals[found].addr, ti: this.locals[found].ti}
        for {
            var p = this.peek()
            if p == "." {
                this.pos += 1
                if this.pos >= len(this.toks) || this.toks[this.pos].kind != 1 {
                    return error("field name expected after .")
                }
                v = selectField(v, this.toks[this.pos].text)?
                this.pos += 1
            } else if p == "[" {
                this.pos += 1
                var idx = this.expr()?
                if this.peek() != "]" {
                    return error("expected ]")
                }
                this.pos += 1
                v = index(v, idx)?
            } else {
                break
            }
        }
        return v
    }
}

// Follows pointers, interfaces and weak pointers to the value itself.
func deref(v evalue) evalue {
    var cur = v
    var steps = 0
    for steps < 8 {
        steps += 1
        var kind = tiKind(cur.ti)
        if kind == kPointer() {
            var p = rd(cur.addr)
            if p == 0 {
                return cur
            }
            cur = evalue{addr: p, ti: tiElem(cur.ti)}
        } else if kind == kAny() || kind == kInterface() {
            var d = dynamic(cur.addr, cur.ti)
            if d.ti == 0 {
                return cur
            }
            cur = evalue{addr: d.addr, ti: d.ti}
        } else if kind == kWeak() {
            var p = weakTarget(cur.addr)
            if p == 0 {
                return cur
            }
            cur = evalue{addr: p, ti: tiElem(cur.ti)}
        } else {
            return cur
        }
    }
    return cur
}

func selectField(v evalue, name string) evalue! {
    if v.ti == 0 {
        return error("." + name + " of a literal")
    }
    var d = deref(v)
    var k = tiKind(d.ti)
    if k == kPointer() || k == kInterface() || k == kAny() {
        return error("nil dereference at ." + name)
    }
    for c in children(d.addr, d.ti, 0, -1) {
        if c.name == name {
            return evalue{addr: c.addr, ti: c.ti}
        }
    }
    return error(tiName(d.ti) + " has no field " + name)
}

func index(v evalue, idx evalue) evalue! {
    if v.ti == 0 {
        return error("index of a literal")
    }
    var d = deref(v)
    var k = tiKind(d.ti)
    if k == kSlice() || k == kArray() {
        var i = asInt(idx)?
        var n = indexedCount(d.addr, d.ti)
        if i < 0 || i >= n {
            return error(
                "index " + std.strconv.Itoa(i) + " out of range [0:" + std.strconv.Itoa(n) + "]",
            )
        }
        var cs = children(d.addr, d.ti, i, 1)
        return evalue{addr: cs[0].addr, ti: cs[0].ti}
    }
    if k == kMap() {
        var m = rd(d.addr)
        var ents = rtMapEntries(m, 0, rtMapLen(m))
        var kt = tiKey(d.ti)
        var i = 0
        for i + 1 < len(ents) {
            if compare(evalue{addr: ents[i], ti: kt}, "==", idx)? {
                return evalue{addr: ents[i + 1], ti: tiElem(d.ti)}
            }
            i += 2
        }
        return error("key not in map")
    }
    return error("cannot index " + tiName(d.ti))
}

func lengthOf(v evalue) int! {
    if v.lit == 3 {
        return len(v.s)
    }
    if v.ti == 0 {
        return error("len of a non-collection")
    }
    var d = deref(v)
    var k = tiKind(d.ti)
    if k == kString() {
        return len(rtString(d.addr))
    }
    if k == kSlice() || k == kArray() || k == kMap() {
        return indexedCount(d.addr, d.ti)
    }
    return error("len of " + tiName(d.ti))
}

// The scalar of v as a literal (lit 0 if it is no scalar).
func scalar(v evalue) evalue {
    if v.ti == 0 {
        return v
    }
    var k = tiKind(v.ti)
    if k >= kInt() && k <= kInt32() {
        return evalue{lit: 1, i: std.unsafe.ReadInt(v.addr, tiSize(v.ti))}
    }
    if k >= kUint() && k <= kUint32() {
        return evalue{lit: 1, i: std.unsafe.ReadUint(v.addr, tiSize(v.ti))}
    }
    if k == kChar() {
        return evalue{lit: 1, i: rd32(v.addr)}
    }
    if k == kFloat() || k == kFloat32() {
        return evalue{lit: 2, f: std.unsafe.ReadFloat(v.addr, tiSize(v.ti))}
    }
    if k == kString() {
        return evalue{lit: 3, s: rtString(v.addr)}
    }
    if k == kBool() {
        return evalue{lit: 4, b: std.unsafe.ReadUint(v.addr, 1) != 0}
    }
    if k == kPointer() ||
        k == kError() ||
        k == kMap() ||
        k == kTask() ||
        k == kWeak() ||
        k == kFunc() {
        if rd(v.addr) == 0 {
            return evalue{lit: 5}
        }
        return evalue{lit: 6, i: rd(v.addr)}
    }
    if k == kInterface() || k == kAny() {
        if dynamic(v.addr, v.ti).ti == 0 {
            return evalue{lit: 5}
        }
        return evalue{lit: 6, i: rd(v.addr)}
    }
    if k == kEnum() {
        var tag = rd32(v.addr)
        if tag >= 0 && tag < tiNVariants(v.ti) {
            return evalue{lit: 7, i: tag, s: rtString(tiVariant(v.ti, tag))}
        }
    }
    return evalue{}
}

func asInt(v evalue) int! {
    var s = scalar(v)
    if s.lit == 1 {
        return s.i
    }
    return error("integer expected")
}

func truth(v evalue) bool! {
    if v.lit == 8 {
        return error("unknown variable " + v.name)
    }
    var s = scalar(v)
    if s.lit == 4 {
        return s.b
    }
    return error("bool expected")
}

func compare(a evalue, op string, b evalue) bool! {
    var x = scalar(a)
    var y = scalar(b)
    var c = 0
    if x.lit == 5 || y.lit == 5 || x.lit == 6 || y.lit == 6 {
        // nil comparisons: only == and !=
        var same = (x.lit == 5) == (y.lit == 5)
        if x.lit == 6 && y.lit == 6 {
            same = x.i == y.i
        }
        if op == "==" {
            return same
        }
        if op == "!=" {
            return !same
        }
        return error("only == and != apply to nil")
    }
    if x.lit == 8 && y.lit != 7 {
        return error("unknown variable " + x.name)
    }
    if y.lit == 8 && x.lit != 7 {
        return error("unknown variable " + y.name)
    }
    if x.lit == 7 || y.lit == 7 {
        // enums against a variant name: x == Red, x == Color.Red or x == "Red"
        var xs = x.s
        var ys = y.s
        if op == "==" {
            return xs == ys
        }
        if op == "!=" {
            return xs != ys
        }
        return error("only == and != apply to enums")
    }
    if x.lit == 1 && y.lit == 1 {
        if x.i < y.i {
            c = -1
        } else if x.i > y.i {
            c = 1
        }
    } else if (x.lit == 1 || x.lit == 2) && (y.lit == 1 || y.lit == 2) {
        var fx = x.f
        var fy = y.f
        if x.lit == 1 {
            fx = float(x.i)
        }
        if y.lit == 1 {
            fy = float(y.i)
        }
        if fx < fy {
            c = -1
        } else if fx > fy {
            c = 1
        }
    } else if x.lit == 3 && y.lit == 3 {
        c = std.strings.Compare(x.s, y.s)
    } else if x.lit == 4 && y.lit == 4 {
        if x.b != y.b {
            c = 1
        }
        if op != "==" && op != "!=" {
            return error("only == and != apply to bools")
        }
    } else {
        return error("cannot compare these values")
    }
    if op == "==" {
        return c == 0
    }
    if op == "!=" {
        return c != 0
    }
    if op == "<" {
        return c < 0
    }
    if op == "<=" {
        return c <= 0
    }
    if op == ">" {
        return c > 0
    }
    return c >= 0
}

// Evaluates expression text against the locals of a frame.
func evaluate(text string, ls []local) evalue! {
    var toks = tokenize(text)?
    var e = evaluator{toks: toks, locals: ls}
    var v = e.expr()?
    if e.pos < len(toks) {
        return error("unexpected " + std.strconv.Quote(toks[e.pos].text))
    }
    if v.lit == 8 {
        return error("unknown variable " + v.name)
    }
    return v
}

// The display text of an evaluated value.
func show(v evalue) string {
    if v.ti != 0 {
        return format(v.addr, v.ti, 2)
    }
    if v.lit == 1 {
        return std.strconv.Itoa(v.i)
    }
    if v.lit == 2 {
        return std.strconv.FormatFloat(v.f, 'g', -1)
    }
    if v.lit == 3 {
        return std.strconv.Quote(v.s)
    }
    if v.lit == 4 {
        if v.b {
            return "true"
        }
        return "false"
    }
    return "nil"
}

func showType(v evalue) string {
    if v.ti != 0 {
        return tiName(v.ti)
    }
    if v.lit == 1 {
        return "int"
    }
    if v.lit == 2 {
        return "float"
    }
    if v.lit == 3 {
        return "string"
    }
    if v.lit == 4 {
        return "bool"
    }
    return ""
}
