namespace std.encoding.xml

// XML 1.0 for documents and configuration: a pull Reader (tokens with
// namespaces resolved, tags checked), a Writer with indentation and
// escaping, the dynamic Value tree and Parse<T>/Format<T> through
// compile-time reflection with @xml(name:, attr:, text:, omitEmpty:, ns:,
// skip:) and @serde. The tokenizer is C (runtime/src/std_xml.c). For
// safety a DOCTYPE is reported as a Directive token and never interpreted:
// no DTD is read and no entity other than the five predefined ones (and
// character references) is expanded, so entity bombs are impossible.

// Token kinds.
enum Kind {
    StartElement
    EndElement
    CharData
    Comment
    ProcInst
    Directive
    EOF
}

// A name: Space is the namespace URI (or the unresolved prefix), Local
// the local part, Prefix the prefix as written.
struct Name {
    Space string
    Local string
    Prefix string

    // prefix:local as written.
    func String() string {
        if this.Prefix == "" {
            return this.Local
        }
        return this.Prefix + ":" + this.Local
    }
}

struct Attr {
    Name Name
    Value string
}

// An attribute with a plain name (for the Writer).
func A(name string, value string) Attr {
    return Attr{Name: Name{Local: name}, Value: value}
}

// One token of a document. Name and Attrs are set for StartElement (Name
// for EndElement); Text holds character data, comment and directive text,
// and the instruction of a ProcInst (Name.Local is its target).
struct Token {
    Kind Kind
    Name Name
    Attrs []Attr
    Text string
    Line int

    // The value of the attribute with local name `name` ("" if none).
    func Attr(name string) string {
        for a in this.Attrs {
            if a.Name.Local == name {
                return a.Value
            }
        }
        return ""
    }
}

// A pull parser: Next() returns the tokens of the text in order and Kind.EOF
// at the end. Set Strict = false before the first Next to keep unknown
// entities as text instead of failing.
struct Reader {
    Strict bool = true
    text string
    toks []string
    scanned bool
    pos int
    stack []Name
    nsPrefix []string
    nsURI []string
    nsCount []int
    pendingEnd bool
    line int

    func Next() Token! {
        if !this.scanned {
            this.toks = rtScan(this.text, this.Strict)?
            this.scanned = true
        }
        if this.pendingEnd {
            this.pendingEnd = false
            return this.closeTop()
        }
        if this.pos >= len(this.toks) {
            if len(this.stack) > 0 {
                return error("xml: unexpected end of input: element <" + this.stack[len(this.stack) - 1].String() + "> not closed")
            }
            return Token{Kind: Kind.EOF, Line: this.line}
        }
        var tok = this.toks[this.pos]
        this.pos += 1
        var k = int(tok[0])
        // every token but attribute parts starts with its line: "K<line> <rest>"
        var sp = std.strings.Index(tok, " ")
        this.line = try std.strings.ToInt(tok[1..sp]) else 0
        var t = tok[sp + 1..]
        if k == 83 {
            var qname = t
            var raw []string = []
            for this.pos < len(this.toks) && int(this.toks[this.pos][0]) == 65 {
                raw = append(raw, this.toks[this.pos][1..])
                raw = append(raw, this.toks[this.pos + 1][1..])
                this.pos += 2
            }
            var empty = int(this.toks[this.pos][0]) == 47
            this.pos += 1
            // namespace declarations first
            var count = 0
            for i in 0..len(raw) / 2 {
                var an = raw[i * 2]
                if an == "xmlns" {
                    this.nsPrefix = append(this.nsPrefix, "")
                    this.nsURI = append(this.nsURI, raw[i * 2 + 1])
                    count += 1
                } else if std.strings.HasPrefix(an, "xmlns:") {
                    this.nsPrefix = append(this.nsPrefix, an[6..])
                    this.nsURI = append(this.nsURI, raw[i * 2 + 1])
                    count += 1
                }
            }
            this.nsCount = append(this.nsCount, count)
            var attrs []Attr = []
            for i in 0..len(raw) / 2 {
                var an = raw[i * 2]
                for a in attrs {
                    if a.Name.String() == an {
                        return error(std.fmt.Format("xml: duplicate attribute {0} on line {1}", an, this.line))
                    }
                }
                attrs = append(attrs, Attr{Name: this.resolve(an, false), Value: raw[i * 2 + 1]})
            }
            var name = this.resolve(qname, true)
            this.stack = append(this.stack, name)
            this.pendingEnd = empty
            return Token{Kind: Kind.StartElement, Name: name, Attrs: attrs, Line: this.line}
        }
        if k == 69 {
            var qname = t
            if len(this.stack) == 0 {
                return error(std.fmt.Format("xml: unexpected end element </{0}> on line {1}", qname, this.line))
            }
            var top = this.stack[len(this.stack) - 1]
            if top.String() != qname {
                return error(std.fmt.Format("xml: element <{0}> closed by </{1}> on line {2}", top.String(), qname, this.line))
            }
            return this.closeTop()
        }
        if k == 84 {
            return Token{Kind: Kind.CharData, Text: t, Line: this.line}
        }
        if k == 67 {
            return Token{Kind: Kind.Comment, Text: t, Line: this.line}
        }
        if k == 80 {
            var inst = this.toks[this.pos][1..]
            this.pos += 1
            return Token{Kind: Kind.ProcInst, Name: Name{Local: t}, Text: inst, Line: this.line}
        }
        return Token{Kind: Kind.Directive, Text: t, Line: this.line}
    }

    // The depth of open elements.
    func Depth() int {
        return len(this.stack)
    }

    func closeTop() Token {
        var top = this.stack[len(this.stack) - 1]
        this.stack = this.stack[..len(this.stack) - 1]
        var c = this.nsCount[len(this.nsCount) - 1]
        this.nsCount = this.nsCount[..len(this.nsCount) - 1]
        this.nsPrefix = this.nsPrefix[..len(this.nsPrefix) - c]
        this.nsURI = this.nsURI[..len(this.nsURI) - c]
        return Token{Kind: Kind.EndElement, Name: top, Line: this.line}
    }

    // prefix:local -> Name (elements use the default namespace).
    func resolve(qname string, element bool) Name {
        var colon = std.strings.Index(qname, ":")
        var prefix = ""
        var local = qname
        if colon > 0 {
            prefix = qname[..colon]
            local = qname[colon + 1..]
        }
        if prefix == "xmlns" {
            return Name{Space: "xmlns", Local: local, Prefix: prefix}
        }
        if prefix == "xml" {
            return Name{Space: "http://www.w3.org/XML/1998/namespace", Local: local, Prefix: prefix}
        }
        if prefix == "" && !element {
            return Name{Local: local}
        }
        if prefix == "" && local == "xmlns" {
            return Name{Space: "xmlns", Local: "", Prefix: ""}
        }
        var i = len(this.nsPrefix) - 1
        for i >= 0 {
            if this.nsPrefix[i] == prefix {
                return Name{Space: this.nsURI[i], Local: local, Prefix: prefix}
            }
            i -= 1
        }
        return Name{Space: prefix, Local: local, Prefix: prefix}
    }
}

func NewReader(text string) *Reader {
    return &Reader{Strict: true, text: text}
}

// Escapes &, <, > for character data.
func Escape(s string) string {
    return rtEscape(s, false)
}

// Escapes for a double-quoted attribute value.
func EscapeAttr(s string) string {
    return rtEscape(s, true)
}

// -- dynamic tree -----------------------------------------------------------------

// A node of a document tree: an element (Kind StartElement) with its
// attributes and children, or a CharData / Comment / ProcInst node.
struct Value {
    Kind Kind
    Name Name
    Attrs []Attr
    Children []Value
    Text string

    func IsElement() bool {
        return this.Kind == Kind.StartElement
    }

    // The attribute with local name `name` ("" if none).
    func Attr(name string) string {
        for a in this.Attrs {
            if a.Name.Local == name {
                return a.Value
            }
        }
        return ""
    }

    func HasAttr(name string) bool {
        for a in this.Attrs {
            if a.Name.Local == name {
                return true
            }
        }
        return false
    }

    // The first child element with local name `name`, or nil.
    func Child(name string) *Value {
        for c in this.Children {
            if c.Kind == Kind.StartElement && c.Name.Local == name {
                var copy = c
                return &copy
            }
        }
        return nil
    }

    // The child elements with local name `name` ("": all child elements).
    func Elements(name string) []Value {
        var out []Value = []
        for c in this.Children {
            if c.Kind == Kind.StartElement && (name == "" || c.Name.Local == name) {
                out = append(out, c)
            }
        }
        return out
    }

    // The text of the node and all its descendants.
    func InnerText() string {
        if this.Kind == Kind.CharData {
            return this.Text
        }
        var b = std.strings.Builder{}
        this.collect(&b)
        return b.String()
    }

    func collect(b *std.strings.Builder) {
        for c in this.Children {
            if c.Kind == Kind.CharData {
                b.Write(c.Text)
            } else if c.Kind == Kind.StartElement {
                c.collect(b)
            }
        }
    }

    // The document text of the node (no added white space).
    func Format() string {
        var w = NewWriter()
        writeValue(w, *this)
        return w.String()
    }

    // Indented document text of the node.
    func FormatIndent(indent string) string {
        var w = NewWriter()
        w.Indent = indent
        writeValue(w, *this)
        return w.String()
    }
}

// An element value with attributes and children.
func NewElement(name string, attrs []Attr, children []Value) Value {
    return Value{Kind: Kind.StartElement, Name: Name{Local: name}, Attrs: attrs, Children: children}
}

func NewText(text string) Value {
    return Value{Kind: Kind.CharData, Text: text}
}

func writeValue(w *Writer, v Value) {
    if v.Kind == Kind.StartElement {
        w.Start(v.Name.String(), v.Attrs)
        for c in v.Children {
            writeValue(w, c)
        }
        w.End()
    } else if v.Kind == Kind.CharData {
        w.Text(v.Text)
    } else if v.Kind == Kind.Comment {
        w.Comment(v.Text)
    } else if v.Kind == Kind.ProcInst {
        w.ProcInst(v.Name.Local, v.Text)
    }
}

// Parses a document into the tree of its root element. Comments and
// processing instructions outside the root are dropped; with
// keepSpace = false, character data that is only white space between
// elements is dropped too.
func ParseValue(text string) Value! {
    return parseTree(text, false)?
}

func parseTree(text string, keepSpace bool) Value! {
    var r = NewReader(text)
    var stack []Value = []
    var root = Value{}
    var haveRoot = false
    for {
        var t = r.Next()?
        if t.Kind == Kind.EOF {
            break
        }
        if t.Kind == Kind.StartElement {
            if len(stack) == 0 && haveRoot {
                return error(std.fmt.Format("xml: a second root element <{0}> on line {1}", t.Name.String(), t.Line))
            }
            stack = append(stack, Value{Kind: Kind.StartElement, Name: t.Name, Attrs: t.Attrs})
        } else if t.Kind == Kind.EndElement {
            var done = stack[len(stack) - 1]
            stack = stack[..len(stack) - 1]
            if len(stack) == 0 {
                root = done
                haveRoot = true
            } else {
                stack[len(stack) - 1].Children = append(stack[len(stack) - 1].Children, done)
            }
        } else if len(stack) > 0 {
            if t.Kind == Kind.CharData && !keepSpace && std.strings.Trim(t.Text) == "" {
                continue
            }
            if t.Kind == Kind.Directive {
                continue
            }
            stack[len(stack) - 1].Children = append(stack[len(stack) - 1].Children, Value{Kind: t.Kind, Name: t.Name, Text: t.Text})
        } else if t.Kind == Kind.CharData && std.strings.Trim(t.Text) != "" {
            return error(std.fmt.Format("xml: character data outside the root element on line {0}", t.Line))
        }
    }
    if !haveRoot {
        return error("xml: no root element")
    }
    return root
}

// -- writer -----------------------------------------------------------------------

// Writes a document: Start/End elements, Text, Comment, ProcInst. With
// Indent set, child elements go on their own lines; an element holding
// only text stays on one line.
struct Writer {
    Indent string
    Prefix string
    b std.strings.Builder
    stack []string
    open bool
    // per open element: 0 empty, 1 has text, 2 has child elements
    content []int

    // The XML declaration <?xml version="1.0" encoding="UTF-8"?>.
    func Header() {
        this.b.Write("<?xml version=\"1.0\" encoding=\"UTF-8\"?>")
    }

    func Start(name string, attrs []Attr) {
        this.closeStart()
        if len(this.content) > 0 {
            if this.content[len(this.content) - 1] != 1 {
                this.content[len(this.content) - 1] = 2
            }
        }
        this.newline(len(this.stack))
        this.b.Write("<")
        this.b.Write(name)
        for a in attrs {
            this.b.Write(" ")
            this.b.Write(a.Name.String())
            this.b.Write("=\"")
            this.b.Write(rtEscape(a.Value, true))
            this.b.Write("\"")
        }
        this.open = true
        this.stack = append(this.stack, name)
        this.content = append(this.content, 0)
    }

    // Ends the innermost open element (an empty one as <a></a>).
    func End() {
        if len(this.stack) == 0 {
            panic("xml: End without an open element")
        }
        this.closeStart()
        var name = this.stack[len(this.stack) - 1]
        var c = this.content[len(this.content) - 1]
        this.stack = this.stack[..len(this.stack) - 1]
        this.content = this.content[..len(this.content) - 1]
        if c == 2 {
            this.newline(len(this.stack))
        }
        this.b.Write("</")
        this.b.Write(name)
        this.b.Write(">")
    }

    // <name>text</name>
    func Element(name string, text string) {
        var none []Attr = []
        this.Start(name, none)
        this.Text(text)
        this.End()
    }

    func Text(s string) {
        this.closeStart()
        if len(this.content) > 0 {
            this.content[len(this.content) - 1] = 1
        }
        this.b.Write(rtEscape(s, false))
    }

    // Character data as a CDATA section (split if it contains "]]>").
    func CData(s string) {
        this.closeStart()
        if len(this.content) > 0 {
            this.content[len(this.content) - 1] = 1
        }
        this.b.Write("<![CDATA[")
        this.b.Write(std.strings.Replace(s, "]]>", "]]]]><![CDATA[>"))
        this.b.Write("]]>")
    }

    func Comment(s string) {
        this.closeStart()
        this.mark()
        this.newline(len(this.stack))
        this.b.Write("<!--")
        this.b.Write(std.strings.Replace(s, "--", "- -"))
        this.b.Write("-->")
    }

    func ProcInst(target string, inst string) {
        this.closeStart()
        this.mark()
        this.newline(len(this.stack))
        this.b.Write("<?")
        this.b.Write(target)
        if inst != "" {
            this.b.Write(" ")
            this.b.Write(inst)
        }
        this.b.Write("?>")
    }

    // Writes s verbatim.
    func Raw(s string) {
        this.closeStart()
        this.b.Write(s)
    }

    // The text written so far.
    func String() string {
        this.closeStart()
        return this.b.String()
    }

    // Fails if elements are still open.
    func Close() void! {
        if len(this.stack) > 0 {
            return error("xml: unclosed element <" + this.stack[len(this.stack) - 1] + ">")
        }
    }

    func mark() {
        if len(this.content) > 0 && this.content[len(this.content) - 1] != 1 {
            this.content[len(this.content) - 1] = 2
        }
    }

    func closeStart() {
        if this.open {
            this.b.Write(">")
            this.open = false
        }
    }

    func newline(depth int) {
        if this.Indent == "" && this.Prefix == "" {
            return
        }
        if this.b.Len() > 0 {
            this.b.Write("\n")
        }
        this.b.Write(this.Prefix)
        for _ in 0..depth {
            this.b.Write(this.Indent)
        }
    }
}

func NewWriter() *Writer {
    return &Writer{}
}

// -- typed decoding ---------------------------------------------------------------

// The element name of type T: @xml(name:) on the type, else its name.
func rootName<T>() string {
    var t = std.reflect.TypeOf<T>()
    var tag = t.Attribute<std.reflect.xmlAttr>()
    if tag != nil && tag.name != "" {
        return tag.name
    }
    var sd = t.Attribute<std.reflect.serde>()
    if sd != nil && sd.name != "" {
        return sd.name
    }
    return t.Name
}

// Parses a document whose root element is a T (named as rootName says)
// into a T: attribute fields from @xml(attr: true), the text from
// @xml(text: true), other fields from child elements (slices from every
// matching child). Unknown attributes and elements are ignored.
func Parse<T>(text string) T! {
    var v = ParseValue(text)?
    var want = rootName<T>()
    if v.Name.Local != want {
        return error("xml: expected element <" + want + ">, found <" + v.Name.String() + ">")
    }
    var out T
    decodeElem(v, want, &out)?
    return out
}

// Decodes an element tree into a T.
func Decode<T>(v Value) T! {
    var out T
    decodeElem(v, v.Name.Local, &out)?
    return out
}

func decodeElem<T>(v Value, path string, out *T) void! {
    if std.reflect.TypeOf<T>() == std.reflect.TypeOf<Value>() {
        *out = v
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Struct {
        for f in std.reflect.Fields<T>() {
            var tag = f.Attribute<std.reflect.xmlAttr>()
            var sd = f.Attribute<std.reflect.serde>()
            if (tag != nil && tag.skip) || (sd != nil && sd.skip) {
                continue
            }
            var name = f.Name
            if sd != nil && sd.name != "" {
                name = sd.name
            }
            if tag != nil && tag.name != "" {
                name = tag.name
            }
            if tag != nil && tag.attr {
                if v.HasAttr(name) {
                    decodeText(v.Attr(name), path + "@" + name, &f.Get(*out))?
                }
            } else if tag != nil && tag.text {
                decodeText(v.InnerText(), path, &f.Get(*out))?
            } else {
                decodeChildren(v.Elements(name), path + "." + name, &f.Get(*out))?
            }
        }
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Pointer {
        decodePtr(v, path, out)?
    } else {
        decodeText(v.InnerText(), path, out)?
    }
}

// A field from its matching child elements.
func decodeChildren<F>(cs []Value, path string, out *F) void! {
    if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Slice {
        decodeSlice(cs, path, out)?
    } else {
        if len(cs) > 0 {
            decodeElem(cs[0], path, out)?
        }
    }
}

func decodeSlice<E>(cs []Value, path string, out *[]E) void! {
    var res []E = []
    for i, c in cs {
        var e E
        decodeElem(c, path + "[" + std.fmt.Format("{0}", i) + "]", &e)?
        res = append(res, e)
    }
    *out = res
}

func decodePtr<E>(v Value, path string, out **E) void! {
    var e E
    decodeElem(v, path, &e)?
    *out = &e
}

func textError(path string, msg string) error {
    return error("xml: " + path + ": " + msg)
}

func decodeText<F>(s string, path string, out *F) void! {
    if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.String {
        *out = s
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Bool {
        var t = std.strings.Trim(s)
        if t == "true" || t == "1" {
            *out = true
        } else if t == "false" || t == "0" {
            *out = false
        } else {
            return textError(path, "invalid bool " + std.encoding.json.Quote(s))
        }
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int8 || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int16 || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int32 || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint8 || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint16 || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint32 {
        var v = try std.strings.ToInt(std.strings.Trim(s)) catch err {
            return textError(path, "invalid integer " + std.encoding.json.Quote(s))
        }
        *out = try std.encoding.intAs<F>(v) catch err {
            return textError(path, err.Message())
        }
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Float || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Float32 {
        var v = try std.strings.ToFloat(std.strings.Trim(s)) catch err {
            return textError(path, "invalid number " + std.encoding.json.Quote(s))
        }
        *out = std.encoding.floatAs<F>(v)
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Enum {
        var ty = std.reflect.TypeOf<F>()
        var variant = ty.Variant(std.strings.Trim(s))
        if variant == nil || len(variant.Fields) > 0 {
            return textError(path, "unknown " + ty.Name + " value " + std.encoding.json.Quote(s))
        }
        std.reflect.storeTag(out, variant.Index)
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Pointer {
        decodeTextPtr(s, path, out)?
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Slice {
        // a list attribute or text: white-space separated items
        decodeTextList(s, path, out)?
    } else {
        return textError(path, "cannot decode text into " + std.reflect.TypeOf<F>().Name)
    }
}

func decodeTextPtr<E>(s string, path string, out **E) void! {
    var e E
    decodeText(s, path, &e)?
    *out = &e
}

func decodeTextList<E>(s string, path string, out *[]E) void! {
    var res []E = []
    for part in std.strings.Fields(s) {
        var e E
        decodeText(part, path, &e)?
        res = append(res, e)
    }
    *out = res
}

// -- typed encoding ---------------------------------------------------------------

// The document of v (root element named as rootName says), compact.
func Format<T>(v T) string {
    var w = NewWriter()
    encodeElem(w, rootName<T>(), typeNs<T>(), v)
    return w.String()
}

// The indented document of v, with an XML declaration.
func FormatIndent<T>(v T, indent string) string {
    var w = NewWriter()
    w.Indent = indent
    w.Header()
    encodeElem(w, rootName<T>(), typeNs<T>(), v)
    return w.String() + "\n"
}

func typeNs<T>() string {
    var tag = std.reflect.TypeOf<T>().Attribute<std.reflect.xmlAttr>()
    if tag != nil {
        return tag.ns
    }
    return ""
}

func encodeElem<T>(w *Writer, name string, ns string, v T) {
    var attrs []Attr = []
    if ns != "" {
        attrs = append(attrs, A("xmlns", ns))
    }
    if std.reflect.TypeOf<T>() == std.reflect.TypeOf<Value>() {
        writeValue(w, v)
    } else if std.reflect.TypeOf<T>().Kind == std.reflect.Kind.Struct {
        for f in std.reflect.Fields<T>() {
            var tag = f.Attribute<std.reflect.xmlAttr>()
            if tag != nil && tag.attr && !tag.skip {
                var an = f.Name
                if tag.name != "" {
                    an = tag.name
                }
                if !(tag.omitEmpty && std.encoding.isEmpty(f.Get(v))) {
                    attrs = append(attrs, A(an, textOf(f.Get(v))))
                }
            }
        }
        w.Start(name, attrs)
        for f in std.reflect.Fields<T>() {
            var tag = f.Attribute<std.reflect.xmlAttr>()
            var sd = f.Attribute<std.reflect.serde>()
            if (tag != nil && (tag.skip || tag.attr)) || (sd != nil && sd.skip) {
                continue
            }
            var omit = (tag != nil && tag.omitEmpty) || (sd != nil && sd.omitEmpty)
            if omit && std.encoding.isEmpty(f.Get(v)) {
                continue
            }
            if tag != nil && tag.text {
                w.Text(textOf(f.Get(v)))
                continue
            }
            var cn = f.Name
            if sd != nil && sd.name != "" {
                cn = sd.name
            }
            if tag != nil && tag.name != "" {
                cn = tag.name
            }
            var cns = ""
            if tag != nil {
                cns = tag.ns
            }
            encodeField(w, cn, cns, f.Get(v))
        }
        w.End()
    } else {
        w.Start(name, attrs)
        w.Text(textOf(v))
        w.End()
    }
}

func encodeField<F>(w *Writer, name string, ns string, x F) {
    if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Slice || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Array {
        for e in x {
            encodeField(w, name, ns, e)
        }
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Pointer {
        if x != nil {
            encodeElem(w, name, ns, *x)
        }
    } else {
        encodeElem(w, name, ns, x)
    }
}

func textOf<F>(x F) string {
    if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.String {
        return x
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Pointer {
        if x == nil {
            return ""
        }
        return textOf(*x)
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Slice {
        var parts []string = []
        for e in x {
            parts = append(parts, textOf(e))
        }
        return std.strings.Join(parts, " ")
    } else if std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Bool || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Enum || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Char || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Float || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Float32 || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int8 || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int16 || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Int32 || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint8 || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint16 || std.reflect.TypeOf<F>().Kind == std.reflect.Kind.Uint32 {
        return std.fmt.Format("{0}", x)
    } else {
        return ""
    }
}
