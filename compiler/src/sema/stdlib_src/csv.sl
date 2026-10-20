namespace std.encoding.csv

// CSV (RFC 4180) with Go's encoding/csv semantics: quoted fields with ""
// escapes and embedded newlines, a custom delimiter, comment lines, lazy
// quotes, leading-space trimming and a fixed or free field count; the
// Writer quotes only what needs it. Read<T> / Write<T> map rows to structs
// with compile-time reflection: a column per field, named by @csv(name:)
// or the field name, or placed by @csv(index:); @csv(skip: true) and
// @serde(name:, skip:) also apply.

// A CSV reader over a text: Read() returns one record at a time. Lines
// end with \n or \r\n (a quoted \r\n becomes \n).
struct Reader {
    // Field delimiter (default ',').
    Comma char = ','
    // Lines starting with Comment are skipped (0: none).
    Comment char
    // Allow a quote in an unquoted field and a non-doubled quote in a quoted one.
    LazyQuotes bool
    // Ignore leading white space in a field.
    TrimLeadingSpace bool
    // Fields per record: > 0 required count, 0 set by the first record, < 0 any.
    FieldsPerRecord int
    text string
    pos int
    numLine int
    recordLine int
    eof bool

    // The next record, or nil at the end of the text.
    func Read() []string! {
        var rec = this.readRecord()?
        if rec == nil {
            return nil
        }
        if this.FieldsPerRecord > 0 {
            if len(rec) != this.FieldsPerRecord {
                return error(std.fmt.Format("record on line {0}: wrong number of fields", this.recordLine))
            }
        } else if this.FieldsPerRecord == 0 {
            this.FieldsPerRecord = len(rec)
        }
        return rec
    }

    // All remaining records.
    func ReadAll() [][]string! {
        var out [][]string = []
        for {
            var r = this.Read()?
            if r == nil {
                break
            }
            out = append(out, r)
        }
        return out
    }

    // The line of the last record returned by Read (1-based).
    func Line() int {
        return this.recordLine
    }

    // The next line with its "\n" ("\r\n" normalized), "" at the end.
    func readLine() string {
        var t = this.text
        if this.pos >= len(t) {
            this.eof = true
            return ""
        }
        var j = std.regex.rtIndex(t, "\n", this.pos)
        var line = ""
        if j < 0 {
            line = t[this.pos..]
            this.pos = len(t)
            if std.strings.HasSuffix(line, "\r") {
                line = line[..len(line) - 1]
            }
        } else {
            line = t[this.pos..j + 1]
            this.pos = j + 1
            if std.strings.HasSuffix(line, "\r\n") {
                line = line[..len(line) - 2] + "\n"
            }
        }
        this.numLine += 1
        return line
    }

    func readRecord() []string! {
        var comma = string(this.Comma)
        var line = ""
        for {
            line = this.readLine()
            if line == "" {
                return nil
            }
            if this.Comment != char(0) && std.strings.HasPrefix(line, string(this.Comment)) {
                continue
            }
            if line == "\n" {
                continue
            }
            break
        }
        var recLine = this.numLine
        this.recordLine = recLine
        var fields []string = []
        var col = 1
        var lineNo = this.numLine
        for {
            if this.TrimLeadingSpace {
                var i = 0
                for i < len(line) && isSpace(int(line[i])) {
                    i += 1
                }
                if i == len(line) && std.strings.HasSuffix(line, "\n") {
                    col -= 1
                }
                line = line[i..]
                col += i
            }
            if len(line) == 0 || int(line[0]) != 34 {
                // unquoted field
                var i = std.strings.Index(line, comma)
                var field = line
                if i >= 0 {
                    field = field[..i]
                } else if std.strings.HasSuffix(field, "\n") {
                    field = field[..len(field) - 1]
                }
                if !this.LazyQuotes {
                    var q = std.strings.Index(field, "\"")
                    if q >= 0 {
                        return error(parseError(recLine, this.numLine, col + q, "bare \" in non-quoted-field"))
                    }
                }
                fields = append(fields, field)
                if i >= 0 {
                    line = line[i + len(comma)..]
                    col += i + len(comma)
                    continue
                }
                break
            }
            // quoted field
            var b = std.strings.Builder{}
            line = line[1..]
            col += 1
            var ended = false
            for !ended {
                var i = std.strings.Index(line, "\"")
                if i >= 0 {
                    b.Write(line[..i])
                    line = line[i + 1..]
                    col += i + 1
                    if std.strings.HasPrefix(line, "\"") {
                        b.Write("\"")
                        line = line[1..]
                        col += 1
                    } else if std.strings.HasPrefix(line, comma) {
                        line = line[len(comma)..]
                        col += len(comma)
                        fields = append(fields, b.String())
                        ended = true
                    } else if line == "" || line == "\n" {
                        fields = append(fields, b.String())
                        return fields
                    } else if this.LazyQuotes {
                        b.Write("\"")
                    } else {
                        return error(parseError(recLine, this.numLine, col - 1, "extraneous or missing \" in quoted-field"))
                    }
                } else if len(line) > 0 {
                    b.Write(line)
                    col += len(line)
                    line = this.readLine()
                    if len(line) > 0 {
                        lineNo = this.numLine
                        col = 1
                    }
                } else {
                    if !this.LazyQuotes {
                        return error(parseError(recLine, lineNo, col, "extraneous or missing \" in quoted-field"))
                    }
                    fields = append(fields, b.String())
                    return fields
                }
            }
        }
        return fields
    }
}

func isSpace(c int) bool {
    return c == 32 || c == 9 || c == 10 || c == 11 || c == 12 || c == 13
}

func parseError(start int, line int, col int, msg string) string {
    if start != line {
        return std.fmt.Format("record on line {0}; parse error on line {1}, column {2}: {3}", start, line, col, msg)
    }
    return std.fmt.Format("parse error on line {0}, column {1}: {2}", line, col, msg)
}

func NewReader(text string) *Reader {
    return &Reader{Comma: ',', text: text}
}

// Parses all records of text with the default options.
func Parse(text string) [][]string! {
    return NewReader(text).ReadAll()?
}

// A CSV writer: Write records, take the text with String().
struct Writer {
    Comma char = ','
    // End lines with \r\n instead of \n.
    UseCRLF bool
    b std.strings.Builder

    func Write(record []string) {
        var comma = string(this.Comma)
        for i, f in record {
            if i > 0 {
                this.b.Write(comma)
            }
            if !needsQuotes(f, comma) {
                this.b.Write(f)
                continue
            }
            this.b.Write("\"")
            var start = 0
            for k in 0..len(f) {
                var c = int(f[k])
                if c == 34 {
                    this.b.Write(f[start..k + 1])
                    this.b.Write("\"")
                    start = k + 1
                } else if c == 13 && this.UseCRLF {
                    // as Go: a bare \r is dropped in CRLF mode
                    this.b.Write(f[start..k])
                    start = k + 1
                } else if c == 10 && this.UseCRLF {
                    this.b.Write(f[start..k])
                    this.b.Write("\r\n")
                    start = k + 1
                }
            }
            this.b.Write(f[start..])
            this.b.Write("\"")
        }
        if this.UseCRLF {
            this.b.Write("\r\n")
        } else {
            this.b.Write("\n")
        }
    }

    func WriteAll(records [][]string) {
        for r in records {
            this.Write(r)
        }
    }

    // The text written so far.
    func String() string {
        return this.b.String()
    }
}

func NewWriter() *Writer {
    return &Writer{Comma: ','}
}

// Formats records with the default options.
func Format(records [][]string) string {
    var w = NewWriter()
    w.WriteAll(records)
    return w.String()
}

func needsQuotes(f string, comma string) bool {
    if f == "" {
        return false
    }
    if f == "\\." {
        return true
    }
    if std.strings.Contains(f, comma) || std.strings.Contains(f, "\"") || std.strings.Contains(f, "\n") || std.strings.Contains(f, "\r") {
        return true
    }
    var c = int(f[0])
    return c == 32 || c == 9 || c == 11 || c == 12
}

// -- typed rows ---------------------------------------------------------------

// Reads a header row and then one T per record (see the file comment).
func Read<T>(text string) []T! {
    var r = NewReader(text)
    var header = r.Read()?
    if header == nil {
        return error("csv: missing header row")
    }
    var cols = map[string]int{}
    for i, h in header {
        if cols[h] == 0 {
            cols[h] = i + 1
        }
    }
    var out []T = []
    for {
        var rec = r.Read()?
        if rec == nil {
            break
        }
        var row T
        fillRow(&row, rec, cols, true, r.Line())?
        out = append(out, row)
    }
    return out
}

// Reads records without a header: field i (or @csv(index:)) is column i.
func ReadNoHeader<T>(text string) []T! {
    var r = NewReader(text)
    var out []T = []
    var cols = map[string]int{}
    for {
        var rec = r.Read()?
        if rec == nil {
            break
        }
        var row T
        fillRow(&row, rec, cols, false, r.Line())?
        out = append(out, row)
    }
    return out
}

func fillRow<T>(row *T, rec []string, cols map[string]int, byName bool, line int) void! {
    var i = -1
    for f in std.reflect.Fields<T>() {
        var tag = f.Attribute<std.reflect.csvAttr>()
        var sd = f.Attribute<std.reflect.serde>()
        if (tag != nil && tag.skip) || (sd != nil && sd.skip) {
            continue
        }
        i += 1
        var name = f.Name
        if sd != nil && sd.name != "" {
            name = sd.name
        }
        if tag != nil && tag.name != "" {
            name = tag.name
        }
        var col = i
        if tag != nil && tag.index >= 0 {
            col = tag.index
        } else if byName {
            col = cols[name] - 1
            if col < 0 {
                return error(std.fmt.Format("csv: line {0}: no column {1}", line, std.encoding.json.Quote(name)))
            }
        }
        if col >= len(rec) {
            return error(std.fmt.Format("csv: line {0}: no column {1} (the record has {2} fields)", line, col, len(rec)))
        }
        setCell(&f.Get(*row), rec[col], line, name)?
    }
}

func cellError(line int, name string, msg string) error {
    return error(std.fmt.Format("csv: line {0}, column {1}: {2}", line, std.encoding.json.Quote(name), msg))
}

func setCell<F>(p *F, s string, line int, name string) void! {
    if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.String {
        *p = s
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Bool {
        var t = std.strings.ToLower(std.strings.Trim(s))
        if t == "true" || t == "1" || t == "yes" {
            *p = true
        } else if t == "false" || t == "0" || t == "no" || t == "" {
            *p = false
        } else {
            return cellError(line, name, "invalid bool " + std.encoding.json.Quote(s))
        }
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int8 || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int16 || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int32 || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint8 || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint16 || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint32 {
        var t = std.strings.Trim(s)
        if t == "" {
            *p = 0
            return
        }
        var v = try std.strings.ToInt(t) catch err {
            return cellError(line, name, "invalid integer " + std.encoding.json.Quote(s))
        }
        *p = try std.encoding.intAs<F>(v) catch err {
            return cellError(line, name, err.Message())
        }
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Float || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Float32 {
        var t = std.strings.Trim(s)
        if t == "" {
            *p = 0
            return
        }
        var v = try std.strings.ToFloat(t) catch err {
            return cellError(line, name, "invalid number " + std.encoding.json.Quote(s))
        }
        *p = std.encoding.floatAs<F>(v)
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Enum {
        var ty = std.reflect.TypeOf<F>()
        var variant = ty.Variant(std.strings.Trim(s))
        if variant == nil || len(variant.Fields) > 0 {
            return cellError(line, name, "unknown " + ty.Name + " value " + std.encoding.json.Quote(s))
        }
        std.reflect.storeTag(p, variant.Index)
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Pointer {
        setPtr(p, s, line, name)?
    } else {
        return cellError(line, name, "cannot store a cell in a " + std.reflect.TypeOf<F>().Name)
    }
}

func setPtr<E>(p **E, s string, line int, name string) void! {
    if s == "" {
        *p = nil
        return
    }
    var e E
    setCell(&e, s, line, name)?
    *p = &e
}

// A header row and one record per row (see Read<T>); fields with
// @csv(index:) go to that column, the others fill the free columns in
// declaration order.
func Write<T>(rows []T) string {
    var w = NewWriter()
    var header []string = []
    var idx []int = []
    var probe T
    headerOf(probe, &header, &idx)
    w.Write(arrange(header, idx))
    for r in rows {
        var rec []string = []
        rowOf(r, &rec)
        w.Write(arrange(rec, idx))
    }
    return w.String()
}

func arrange(vals []string, idx []int) []string {
    var n = len(vals)
    for i in idx {
        if i + 1 > n {
            n = i + 1
        }
    }
    var out []string = []
    var used []bool = []
    for _ in 0..n {
        out = append(out, "")
        used = append(used, false)
    }
    for k, v in vals {
        if idx[k] >= 0 {
            out[idx[k]] = v
            used[idx[k]] = true
        }
    }
    var next = 0
    for k, v in vals {
        if idx[k] < 0 {
            for next < n && used[next] {
                next += 1
            }
            if next < n {
                out[next] = v
                used[next] = true
            }
        }
    }
    return out
}

func headerOf<T>(probe T, out *[]string, idx *[]int) {
    for f in std.reflect.Fields<T>() {
        var tag = f.Attribute<std.reflect.csvAttr>()
        var sd = f.Attribute<std.reflect.serde>()
        if (tag != nil && tag.skip) || (sd != nil && sd.skip) {
            continue
        }
        var name = f.Name
        if sd != nil && sd.name != "" {
            name = sd.name
        }
        var at = -1
        if tag != nil && tag.name != "" {
            name = tag.name
        }
        if tag != nil {
            at = tag.index
        }
        *out = append(*out, name)
        *idx = append(*idx, at)
    }
    _ = probe
}

func rowOf<T>(row T, out *[]string) {
    for f in std.reflect.Fields<T>() {
        var tag = f.Attribute<std.reflect.csvAttr>()
        var sd = f.Attribute<std.reflect.serde>()
        if (tag != nil && tag.skip) || (sd != nil && sd.skip) {
            continue
        }
        *out = append(*out, cell(f.Get(row)))
    }
}

func cell<F>(x F) string {
    if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.String {
        return x
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Pointer {
        if x == nil {
            return ""
        }
        return cell(*x)
    } else {
        return std.fmt.Format("{0}", x)
    }
}
