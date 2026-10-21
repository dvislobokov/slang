namespace std.text.template

// Contextual escaping for std.html.template. After parsing, the static
// text of the template is run through an HTML tokenizer state machine;
// every action is given the escaper of the context it appears in:
//
//   text, <title>/<textarea>     HTML-escaped (template.HTML values pass)
//   quoted / unquoted attribute  attribute-escaped
//   URL attribute (href, src..)  at the start: unsafe schemes become
//                                "#ZgotmplZ", then URL-normalized; later
//                                in the value: query-escaped
//   <script>, on* attributes     a JS value (strings quoted and escaped),
//                                or JS-string-escaped inside a JS string
//   <style>, style attributes    only safe CSS tokens, else "ZgotmplZ"
//   tag or attribute names       only [A-Za-z0-9-], else "ZgotmplZ"
//   <!-- comments -->            dropped
//
// Branches of {{if}}/{{with}}/{{range}} must end in the same context.
// {{template}} calls are assumed to be made from text context.

// Context: state 0 text, 1 tag name, 2 in tag, 3 attribute name, 4 after
// attribute name, 5 before value, 6 attribute value, 7 script, 8 style,
// 9 rcdata, 10 comment. attr: 0 normal, 1 URL, 2 JS, 3 CSS.
struct hctx {
    state    int
    quote    int
    attr     int
    valLen   int
    jsq      int
    tagName  string
    attrName string
    endTag   bool

    func same(o hctx) bool {
        return this.state == o.state &&
            this.quote == o.quote &&
            this.attr == o.attr &&
            this.jsq == o.jsq &&
            this.tagName == o.tagName
    }
}

func escapeAll(t *tmpl) void! {
    _ = escList(t, t.nodes[t.root].list, hctx{})?
    for _, d in t.defs {
        _ = escList(t, t.nodes[d - 1].list, hctx{})?
    }
}

func escList(t *tmpl, list []int, c0 hctx) hctx! {
    var c = c0
    for idx in list {
        var n = t.nodes[idx]
        if n.kind == 1 {
            var r = advanceStrip(c, n.text)
            c = r.c
            t.nodes[idx].text = r.text
        } else if n.kind == 2 {
            var p = t.pipes[n.pipe]
            if len(p.vars) == 0 {
                var m = modeFor(c)
                if m == 0 {
                    return t.fail(n.line, "action in an unsupported HTML context")
                }
                t.nodes[idx].esc = m
                if c.state == 5 {
                    // the action starts an unquoted value
                    c.state = 6
                    c.quote = 0
                    c.attr = attrKind(c.attrName)
                    c.valLen = 0
                    c.jsq = 0
                }
                if c.state == 6 {
                    c.valLen += 1
                }
            }
        } else if n.kind == 3 || n.kind == 5 || n.kind == 4 {
            var a = escList(t, n.list, c)?
            var b = c
            if n.hasElse {
                b = escList(t, n.elseList, c)?
            }
            if !a.same(b) || (n.kind == 4 && !a.same(c)) {
                return t.fail(
                    n.line,
                    "{{if}}/{{with}}/{{range}} branches end in different HTML contexts",
                )
            }
            c = a
            c.valLen = std.math.IMax(a.valLen, b.valLen)
        } else if n.kind == 6 {
            if c.state != 0 {
                return t.fail(n.line, "{{template}} is only supported in HTML text context")
            }
        }
    }
    return c
}

func attrKind(name string) int {
    if std.strings.HasPrefix(name, "on") {
        return 2
    }
    if name == "style" {
        return 3
    }
    for u in [
        "href",
        "src",
        "action",
        "formaction",
        "cite",
        "data",
        "poster",
        "background",
        "codebase",
        "longdesc",
        "profile",
        "usemap",
        "manifest",
        "icon",
        "srcset",
        "xmlns",
        "ping",
    ] {
        if name == u {
            return 1
        }
    }
    if std.strings.Contains(name, "url") || std.strings.Contains(name, "uri") {
        return 1
    }
    return 0
}

func lowerByte(c int) int {
    if c >= 65 && c <= 90 {
        return c + 32
    }
    return c
}

func startsWithFold(s string, i int, lit string) bool {
    if i + len(lit) > len(s) {
        return false
    }
    for k in 0..len(lit) {
        if lowerByte(int(s[i + k])) != int(lit[k]) {
            return false
        }
    }
    return true
}

func isLetter(c int) bool {
    return (c >= 65 && c <= 90) || (c >= 97 && c <= 122)
}

func closeTag(c0 hctx) hctx {
    var c = c0
    c.attr = 0
    c.quote = 0
    if c.endTag {
        c.state = 0
        c.tagName = ""
    } else if c.tagName == "script" {
        c.state = 7
    } else if c.tagName == "style" {
        c.state = 8
    } else if c.tagName == "textarea" || c.tagName == "title" {
        c.state = 9
    } else {
        c.state = 0
        c.tagName = ""
    }
    return c
}

struct advanced {
    c    hctx
    text string
}

// The context after s, and s without its HTML comments (as Go's
// html/template, comments are removed from the output).
func advanceStrip(c0 hctx, s string) advanced {
    var b = std.strings.Builder{}
    var c = advanceOut(c0, s, &b)
    return advanced{c: c, text: b.String()}
}

func advance(c0 hctx, s string) hctx {
    var b = std.strings.Builder{}
    return advanceOut(c0, s, &b)
}

// The context after the static text s; the text outside comments is
// copied to out.
func advanceOut(c0 hctx, s string, out *std.strings.Builder) hctx {
    var c = c0
    var i = 0
    var n = len(s)
    var copied = 0
    if c.state == 10 {
        copied = -1
    }
    for i < n {
        var ch = int(s[i])
        var st = c.state
        if st == 0 {
            if ch == 60 {
                if startsWithFold(s, i, "<!--") {
                    if copied >= 0 {
                        out.Write(s[copied..i])
                    }
                    copied = -1
                    c.state = 10
                    i += 4
                    continue
                }
                if i + 1 < n && isLetter(int(s[i + 1])) {
                    c.state = 1
                    c.tagName = ""
                    c.endTag = false
                } else if i + 2 < n && int(s[i + 1]) == 47 && isLetter(int(s[i + 2])) {
                    c.state = 1
                    c.tagName = ""
                    c.endTag = true
                    i += 1
                }
            }
        } else if st == 1 {
            if isLetter(ch) || (ch >= 48 && ch <= 57) || ch == 45 || ch == 58 {
                c.tagName = c.tagName + string(char(lowerByte(ch)))
            } else if ch == 62 {
                c = closeTag(c)
            } else {
                c.state = 2
            }
        } else if st == 2 {
            if ch == 62 {
                c = closeTag(c)
            } else if ch != 32 && ch != 9 && ch != 10 && ch != 13 && ch != 47 {
                c.state = 3
                c.attrName = string(char(lowerByte(ch)))
            }
        } else if st == 3 {
            if ch == 61 {
                c.state = 5
            } else if ch == 62 {
                c = closeTag(c)
            } else if ch == 32 || ch == 9 || ch == 10 || ch == 13 {
                c.state = 4
            } else if ch == 47 {
                c.state = 2
            } else {
                c.attrName = c.attrName + string(char(lowerByte(ch)))
            }
        } else if st == 4 {
            if ch == 61 {
                c.state = 5
            } else if ch == 62 {
                c = closeTag(c)
            } else if ch != 32 && ch != 9 && ch != 10 && ch != 13 {
                c.state = 3
                c.attrName = string(char(lowerByte(ch)))
            }
        } else if st == 5 {
            if ch == 34 || ch == 39 {
                c.state = 6
                c.quote = ch
                c.valLen = 0
                c.attr = attrKind(c.attrName)
                c.jsq = 0
            } else if ch == 62 {
                c = closeTag(c)
            } else if ch != 32 && ch != 9 && ch != 10 && ch != 13 {
                c.state = 6
                c.quote = 0
                c.valLen = 1
                c.attr = attrKind(c.attrName)
                c.jsq = 0
            }
        } else if st == 6 {
            if c.quote != 0 && ch == c.quote {
                c.state = 2
                c.quote = 0
                c.attr = 0
            } else if c.quote == 0 && (ch == 32 || ch == 9 || ch == 10 || ch == 13) {
                c.state = 2
                c.attr = 0
            } else if c.quote == 0 && ch == 62 {
                c = closeTag(c)
            } else {
                c.valLen += 1
                if c.attr == 2 {
                    c.jsq = jsQuote(c.jsq, ch)
                }
            }
        } else if st == 7 {
            if c.jsq != 0 {
                if ch == 92 {
                    i += 1
                } else if ch == c.jsq {
                    c.jsq = 0
                }
            } else if startsWithFold(s, i, "</script") {
                c.state = 1
                c.tagName = ""
                c.endTag = true
                i += 1
            } else if ch == 47 && i + 1 < n && int(s[i + 1]) == 47 {
                // line comment
                var j = std.regex.rtIndex(s, "\n", i)
                if j < 0 {
                    i = n
                    continue
                }
                i = j
            } else if ch == 34 || ch == 39 || ch == 96 {
                c.jsq = ch
            }
        } else if st == 8 {
            if startsWithFold(s, i, "</style") {
                c.state = 1
                c.tagName = ""
                c.endTag = true
                i += 1
            }
        } else if st == 9 {
            if startsWithFold(s, i, "</" + c.tagName) {
                c.state = 1
                c.tagName = ""
                c.endTag = true
                i += 1
            }
        } else if st == 10 {
            if startsWithFold(s, i, "-->") {
                c.state = 0
                i += 3
                copied = i
                continue
            }
        }
        i += 1
    }
    if copied >= 0 {
        out.Write(s[copied..])
    }
    return c
}

func jsQuote(q int, ch int) int {
    if q != 0 {
        if ch == q {
            return 0
        }
        return q
    }
    if ch == 34 || ch == 39 || ch == 96 {
        return ch
    }
    return 0
}

// Escaping modes: 1 text, 2 quoted attribute, 3 unquoted attribute,
// 4/5 URL start/part (quoted), 6/7 URL start/part (unquoted), 8 JS value
// (script), 9 JS string (script), 10/11 JS value/string in an attribute,
// 12 CSS (style), 13 CSS in an attribute, 14 comment, 15 rcdata, 16 name;
// 0: not allowed.
func modeFor(c hctx) int {
    var st = c.state
    if st == 0 {
        return 1
    }
    if st == 10 {
        return 14
    }
    if st == 9 {
        return 15
    }
    if st == 7 {
        if c.jsq != 0 {
            return 9
        }
        return 8
    }
    if st == 8 {
        return 12
    }
    if st == 1 || st == 2 || st == 3 || st == 4 {
        return 16
    }
    if st == 5 {
        // an unquoted value starting with the action
        var k = attrKind(c.attrName)
        if k == 1 {
            return 6
        }
        if k == 2 {
            return 10
        }
        if k == 3 {
            return 13
        }
        return 3
    }
    if st == 6 {
        if c.attr == 1 {
            if c.quote != 0 {
                if c.valLen == 0 {
                    return 4
                }
                return 5
            }
            if c.valLen == 0 {
                return 6
            }
            return 7
        }
        if c.attr == 2 {
            if c.jsq != 0 {
                return 11
            }
            return 10
        }
        if c.attr == 3 {
            return 13
        }
        if c.quote != 0 {
            return 2
        }
        return 3
    }
    return 0
}

func escapeFor(mode int, v Value) string {
    // a missing value prints nothing; a nil pointer prints <nil>
    var s = v.s
    if v.kind != 0 {
        s = strOf(v)
    }
    if mode == 1 || mode == 15 {
        if v.kind == 8 && mode == 1 {
            return v.s
        }
        return std.html.Escape(s)
    }
    if mode == 2 {
        return std.html.Escape(s)
    }
    if mode == 3 {
        return unquotedEscape(s)
    }
    if mode == 4 || mode == 6 {
        var u = s
        if v.kind != 9 && unsafeURL(s) {
            u = "#ZgotmplZ"
        }
        u = normalizeURL(u)
        if mode == 6 {
            return unquotedEscape(u)
        }
        return std.html.Escape(u)
    }
    if mode == 5 || mode == 7 {
        var u = urlPartEscape(s)
        if v.kind == 9 {
            u = normalizeURL(s)
        }
        if mode == 7 {
            return unquotedEscape(u)
        }
        return std.html.Escape(u)
    }
    if mode == 8 || mode == 10 {
        var j = jsValue(v)
        if mode == 10 {
            return std.html.Escape(j)
        }
        return j
    }
    if mode == 9 || mode == 11 {
        var j = jsStrEscape(s)
        if mode == 11 {
            return std.html.Escape(j)
        }
        return j
    }
    if mode == 12 || mode == 13 {
        if !safeCSS(s) {
            return "ZgotmplZ"
        }
        if mode == 13 {
            return std.html.Escape(s)
        }
        return s
    }
    if mode == 14 {
        return ""
    }
    if mode == 16 {
        for i in 0..len(s) {
            var c = int(s[i])
            if !(isLetter(c) || (c >= 48 && c <= 57) || c == 45) {
                return "ZgotmplZ"
            }
        }
        if s == "" {
            return "ZgotmplZ"
        }
        return s
    }
    return std.html.Escape(s)
}

func unquotedEscape(s string) string {
    var b = std.strings.Builder{}
    var start = 0
    for i in 0..len(s) {
        var c = int(s[i])
        var e = ""
        if c == 38 {
            e = "&amp;"
        } else if c == 60 {
            e = "&lt;"
        } else if c == 62 {
            e = "&gt;"
        } else if c == 34 {
            e = "&#34;"
        } else if c == 39 {
            e = "&#39;"
        } else if c == 32 || c == 9 || c == 10 || c == 12 || c == 13 || c == 61 || c == 96 {
            e = "&#" + std.fmt.Format("{0}", c) + ";"
        } else {
            continue
        }
        b.Write(s[start..i])
        b.Write(e)
        start = i + 1
    }
    b.Write(s[start..])
    return b.String()
}

// A scheme other than http, https or mailto (before any / ? #).
func unsafeURL(s string) bool {
    var colon = std.strings.Index(s, ":")
    if colon < 0 {
        return false
    }
    var slash = std.strings.Index(s, "/")
    var q = std.strings.Index(s, "?")
    var h = std.strings.Index(s, "#")
    if (slash >= 0 && slash < colon) || (q >= 0 && q < colon) || (h >= 0 && h < colon) {
        return false
    }
    var scheme = std.strings.ToLower(std.strings.Trim(s[..colon]))
    return scheme != "http" && scheme != "https" && scheme != "mailto"
}

// Percent-encodes bytes that cannot appear in a URL (keeps reserved
// characters and existing %XX escapes).
func normalizeURL(s string) string {
    var b = std.strings.Builder{}
    for i in 0..len(s) {
        var c = int(s[i])
        var ok = (c >= 48 && c <= 57) ||
            (c >= 65 && c <= 90) ||
            (c >= 97 && c <= 122) ||
            c == 45 ||
            c == 95 ||
            c == 46 ||
            c == 126 ||
            c == 33 ||
            c == 35 ||
            c == 36 ||
            c == 38 ||
            c == 39 ||
            c == 40 ||
            c == 41 ||
            c == 42 ||
            c == 43 ||
            c == 44 ||
            c == 47 ||
            c == 58 ||
            c == 59 ||
            c == 61 ||
            c == 63 ||
            c == 64 ||
            c == 91 ||
            c == 93 ||
            c == 37
        if ok {
            b.Write(s[i..i + 1])
        } else {
            b.Write("%" + hex2(c))
        }
    }
    return b.String()
}

// A JS literal for the value (JSON-like; strings escaped for HTML safety).
func jsValue(v Value) string {
    if v.kind == 0 {
        return "null"
    }
    if v.kind == 10 {
        return v.s
    }
    if v.kind == 1 || v.kind == 2 || v.kind == 3 {
        // padded so that it does not join neighbouring tokens (as Go)
        return " " + show(v) + " "
    }
    if v.kind == 4 || v.kind == 8 || v.kind == 9 {
        return jsonString(v.s)
    }
    var parts []string = []
    if v.kind == 5 {
        for x in v.items {
            parts = append(parts, std.strings.Trim(jsValue(x)))
        }
        return "[" + std.strings.Join(parts, ",") + "]"
    }
    for i, k in v.keys {
        parts = append(parts, jsonString(k) + ":" + std.strings.Trim(jsValue(v.items[i])))
    }
    return "{" + std.strings.Join(parts, ",") + "}"
}

// A JSON string literal with <, >, & escaped (as Go's json.Marshal).
func jsonString(s string) string {
    var b = std.strings.Builder{}
    b.Write("\"")
    var start = 0
    for i in 0..len(s) {
        var c = int(s[i])
        var e = ""
        if c == 34 {
            e = "\\\""
        } else if c == 92 {
            e = "\\\\"
        } else if c == 60 {
            e = "\\u003c"
        } else if c == 62 {
            e = "\\u003e"
        } else if c == 38 {
            e = "\\u0026"
        } else if c == 10 {
            e = "\\n"
        } else if c == 13 {
            e = "\\r"
        } else if c == 9 {
            e = "\\t"
        } else if c < 32 {
            e = "\\u00" + std.strings.ToLower(hex2(c))
        } else if c == 226 &&
            i + 2 < len(s) &&
            int(s[i + 1]) == 128 &&
            (int(s[i + 2]) == 168 || int(s[i + 2]) == 169) {
            // U+2028 / U+2029 end JS lines
            b.Write(s[start..i])
            if int(s[i + 2]) == 168 {
                b.Write("\\u2028")
            } else {
                b.Write("\\u2029")
            }
            start = i + 3
            continue
        } else {
            continue
        }
        b.Write(s[start..i])
        b.Write(e)
        start = i + 1
    }
    b.Write(s[start..])
    b.Write("\"")
    return b.String()
}

// Escaping inside a JS string literal (Go's jsStrEscaper).
func jsStrEscape(s string) string {
    var b = std.strings.Builder{}
    var start = 0
    for i in 0..len(s) {
        var c = int(s[i])
        var e = ""
        if c == 0 {
            e = "\\u0000"
        } else if c == 9 {
            e = "\\t"
        } else if c == 10 {
            e = "\\n"
        } else if c == 11 {
            e = "\\u000b"
        } else if c == 12 {
            e = "\\f"
        } else if c == 13 {
            e = "\\r"
        } else if c == 34 {
            e = "\\u0022"
        } else if c == 96 {
            e = "\\u0060"
        } else if c == 38 {
            e = "\\u0026"
        } else if c == 39 {
            e = "\\u0027"
        } else if c == 43 {
            e = "\\u002b"
        } else if c == 47 {
            e = "\\/"
        } else if c == 60 {
            e = "\\u003c"
        } else if c == 62 {
            e = "\\u003e"
        } else if c == 92 {
            e = "\\\\"
        } else if c < 32 {
            e = "\\u00" + std.strings.ToLower(hex2(c))
        } else {
            continue
        }
        b.Write(s[start..i])
        b.Write(e)
        start = i + 1
    }
    b.Write(s[start..])
    return b.String()
}

// Percent-encoding of everything but unreserved characters (a URL part).
func urlPartEscape(s string) string {
    var b = std.strings.Builder{}
    for i in 0..len(s) {
        var c = int(s[i])
        if (c >= 48 && c <= 57) ||
            (c >= 65 && c <= 90) ||
            (c >= 97 && c <= 122) ||
            c == 45 ||
            c == 95 ||
            c == 46 ||
            c == 126 {
            b.Write(s[i..i + 1])
        } else {
            b.Write("%" + std.strings.ToLower(hex2(c)))
        }
    }
    return b.String()
}

func safeCSS(s string) bool {
    for i in 0..len(s) {
        var c = int(s[i])
        if !(isLetter(c) ||
            (c >= 48 && c <= 57) ||
            c == 32 ||
            c == 35 ||
            c == 37 ||
            c == 46 ||
            c == 44 ||
            c == 45 ||
            c == 95) {
            return false
        }
    }
    return true
}
