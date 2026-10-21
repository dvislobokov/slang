namespace std.text.table

// Text tables: columns padded to the widest cell (display width: wide
// East Asian characters and emoji count as 2, combining marks as 0),
// per-column alignment, and several styles: Plain (space separated, like
// Go's tabwriter), ASCII and Unicode boxes, and Markdown. Cells may hold
// several lines.

struct Table {
    Headers []string
    // per column: Left (default), Right or Center
    Align []int
    Style int
    // spaces between columns in Plain style, inside borders otherwise
    Padding int
    rows    [][]string

    func AddRow(cells []string) {
        var r []string = []
        for c in cells {
            r = append(r, c)
        }
        this.rows = append(this.rows, r)
    }

    func SetAlign(col int, align int) {
        for len(this.Align) <= col {
            this.Align = append(this.Align, Left)
        }
        this.Align[col] = align
    }

    func NumRows() int {
        return len(this.rows)
    }

    func String() string {
        var ncol = len(this.Headers)
        for r in this.rows {
            if len(r) > ncol {
                ncol = len(r)
            }
        }
        if ncol == 0 {
            return ""
        }
        var widths []int = []
        for _ in 0..ncol {
            widths = append(widths, 0)
        }
        var hasHeader = len(this.Headers) > 0
        if hasHeader {
            measure(widths, this.Headers)
        }
        for r in this.rows {
            measure(widths, r)
        }
        if this.Style == Markdown {
            for i in 0..ncol {
                if widths[i] < 3 {
                    widths[i] = 3
                }
            }
        }
        var b = std.strings.Builder{}
        var pad = this.Padding
        if pad <= 0 {
            pad = 1
            if this.Style == Plain {
                pad = 2
            }
        }
        var st = this.Style
        if st == ASCII || st == Unicode {
            this.rule(&b, widths, pad, 0)
        }
        if hasHeader {
            this.line(&b, widths, this.Headers, pad, true)
            if st == Plain {
                var dashes []string = []
                for i in 0..ncol {
                    dashes = append(dashes, std.strings.Repeat("-", widths[i]))
                }
                this.line(&b, widths, dashes, pad, true)
            } else if st == Markdown {
                b.Write("|")
                for i in 0..ncol {
                    var a = this.align(i)
                    var w = widths[i] + 2 * pad
                    if a == Left {
                        b.Write(":" + std.strings.Repeat("-", w - 1))
                    } else if a == Right {
                        b.Write(std.strings.Repeat("-", w - 1) + ":")
                    } else {
                        b.Write(":" + std.strings.Repeat("-", w - 2) + ":")
                    }
                    b.Write("|")
                }
                b.Write("\n")
            } else {
                this.rule(&b, widths, pad, 1)
            }
        }
        for r in this.rows {
            this.line(&b, widths, r, pad, false)
        }
        if st == ASCII || st == Unicode {
            this.rule(&b, widths, pad, 2)
        }
        return b.String()
    }

    func align(col int) int {
        if col < len(this.Align) {
            return this.Align[col]
        }
        return Left
    }

    // a horizontal border: 0 top, 1 under the header, 2 bottom
    func rule(b *std.strings.Builder, widths []int, pad int, kind int) {
        var h = "-"
        var l = "+"
        var m = "+"
        var r = "+"
        if this.Style == Unicode {
            h = "─"
            if kind == 0 {
                l = "┌"
                m = "┬"
                r = "┐"
            } else if kind == 1 {
                l = "├"
                m = "┼"
                r = "┤"
            } else {
                l = "└"
                m = "┴"
                r = "┘"
            }
        }
        b.Write(l)
        for i, w in widths {
            if i > 0 {
                b.Write(m)
            }
            b.Write(std.strings.Repeat(h, w + 2 * pad))
        }
        b.Write(r)
        b.Write("\n")
    }

    // one row (several text lines when a cell has \n)
    func line(b *std.strings.Builder, widths []int, cells []string, pad int, header bool) {
        var split [][]string = []
        var height = 1
        for i in 0..len(widths) {
            var c = ""
            if i < len(cells) {
                c = cells[i]
            }
            var ls = std.strings.Split(c, "\n")
            if len(ls) > height {
                height = len(ls)
            }
            split = append(split, ls)
        }
        var sep = "|"
        if this.Style == Unicode {
            sep = "│"
        }
        var boxed = this.Style != Plain
        for k in 0..height {
            var lb = std.strings.Builder{}
            if boxed {
                lb.Write(sep)
            }
            for i, w in widths {
                var text = ""
                if k < len(split[i]) {
                    text = split[i][k]
                }
                var a = this.align(i)
                if header && this.Style != Markdown {
                    a = Left
                }
                if boxed {
                    lb.Write(std.strings.Repeat(" ", pad))
                } else if i > 0 {
                    lb.Write(std.strings.Repeat(" ", pad))
                }
                lb.Write(fit(text, w, a))
                if boxed {
                    lb.Write(std.strings.Repeat(" ", pad))
                    lb.Write(sep)
                }
            }
            var s = lb.String()
            if !boxed {
                s = std.strings.TrimRight(s, " ")
            }
            b.Write(s)
            b.Write("\n")
        }
    }
}

func New(headers []string) *Table {
    var t = &Table{}
    for h in headers {
        t.Headers = append(t.Headers, h)
    }
    return t
}

// Formats rows (the first one is the header) in a style.
func Format(rows [][]string, style int) string {
    if len(rows) == 0 {
        return ""
    }
    var t = New(rows[0])
    t.Style = style
    for i in 1..len(rows) {
        t.AddRow(rows[i])
    }
    return t.String()
}

func measure(widths []int, cells []string) {
    for i, c in cells {
        for l in std.strings.Split(c, "\n") {
            var w = Width(l)
            if w > widths[i] {
                widths[i] = w
            }
        }
    }
}

// pads text to width w with alignment a
func fit(text string, w int, a int) string {
    var gap = w - Width(text)
    if gap <= 0 {
        return text
    }
    if a == Right {
        return std.strings.Repeat(" ", gap) + text
    }
    if a == Center {
        var l = gap / 2
        return std.strings.Repeat(" ", l) + text + std.strings.Repeat(" ", gap - l)
    }
    return text + std.strings.Repeat(" ", gap)
}

// The display width of s in a terminal: 2 for wide East Asian characters
// and emoji, 0 for combining marks and zero-width characters, 1 otherwise.
func Width(s string) int {
    var w = 0
    var i = 0
    for i < len(s) {
        var c = int(s[i])
        if c < 128 {
            i += 1
            if c >= 32 && c != 127 {
                w += 1
            }
            continue
        }
        var (r, n) = std.unicode.utf8.DecodeRuneAt(s, i)
        i += n
        w += RuneWidth(int(r))
    }
    return w
}

func RuneWidth(r int) int {
    if r < 32 || (r >= 127 && r < 160) {
        return 0
    }
    // combining marks, zero-width space/joiners, variation selectors
    if (r >= 0x300 && r <= 0x36F) ||
        (r >= 0x200B && r <= 0x200F) ||
        (r >= 0xFE00 && r <= 0xFE0F) ||
        (r >= 0x1AB0 && r <= 0x1AFF) ||
        (r >= 0x1DC0 && r <= 0x1DFF) ||
        (r >= 0x20D0 && r <= 0x20FF) ||
        (r >= 0xFE20 && r <= 0xFE2F) {
        return 0
    }
    if (r >= 0x1100 && r <= 0x115F) ||
        r == 0x2329 ||
        r == 0x232A ||
        (r >= 0x2E80 && r <= 0x303E) ||
        (r >= 0x3041 && r <= 0x33FF) ||
        (r >= 0x3400 && r <= 0x4DBF) ||
        (r >= 0x4E00 && r <= 0x9FFF) ||
        (r >= 0xA000 && r <= 0xA4CF) ||
        (r >= 0xAC00 && r <= 0xD7A3) ||
        (r >= 0xF900 && r <= 0xFAFF) ||
        (r >= 0xFE30 && r <= 0xFE4F) ||
        (r >= 0xFF00 && r <= 0xFF60) ||
        (r >= 0xFFE0 && r <= 0xFFE6) ||
        (r >= 0x1F300 && r <= 0x1F64F) ||
        (r >= 0x1F900 && r <= 0x1F9FF) ||
        (r >= 0x1F680 && r <= 0x1F6FF) ||
        (r >= 0x20000 && r <= 0x3FFFD) {
        return 2
    }
    return 1
}
