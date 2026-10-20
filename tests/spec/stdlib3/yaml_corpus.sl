// test: std.encoding.yaml parses 30 documents the same way as PyYAML (on YAML 1.2-compatible input): scalars and core-schema resolution, quoting and escapes, plain/quoted multi-line folding, literal/folded block scalars with chomping and indentation indicators, block/flow/compact collections (keys compared sorted), anchors, aliases and merge keys, tags, comments, multi-document streams
// status: passing
namespace spec.stdlib3.yaml_corpus

import std
import std.io

func canon(v encoding.yaml.Value) string {
    if v.IsNull() {
        return "null"
    }
    if v.IsBool() || v.IsInt() {
        return v.Text()
    }
    if v.IsFloat() {
        var f = try v.Float() else 0.0
        if math.IsNaN(f) {
            return ".nan"
        }
        if math.IsInf(f, 1) {
            return ".inf"
        }
        if math.IsInf(f, -1) {
            return "-.inf"
        }
        var s = fmt.Format("{0}", f)
        if !strings.Contains(s, ".") && !strings.Contains(s, "e") {
            s += ".0"
        }
        return s
    }
    if v.IsString() {
        return encoding.json.Quote(v.Text())
    }
    var parts []string = []
    if v.IsSeq() {
        for x in v.Items() {
            parts = append(parts, canon(x))
        }
        return "[" + strings.Join(parts, " ") + "]"
    }
    var keys = v.Keys()
    sortStrings(keys)
    for k in keys {
        parts = append(parts, encoding.json.Quote(k) + ":" + canon(v.Get(k)))
    }
    return "{" + strings.Join(parts, " ") + "}"
}

func main() {
    var cases = [
        "a: 1\nb: -2\nc: 3.5\nd: 1.5e+3\ne: .inf\nf: -.inf\ng: .nan\nh: ~\ni: null\nj:\nk: true\nl: false\nm: hello world\n",
        "s1: '123'\ns2: \"true\"\ns3: 0x1F\ns4: +12\ns5: -0.5\ns6: .5\ns7: 1.\ns8: 007\n",
        "url: http://example.com:8080/path?q=1\ncolon: a:b\nhash: a#b\ncomment: value # trailing comment\nquoted: \"a # not comment\"\n",
        "single: 'it''s a ''test'''\ndouble: \"tab\\there \\\"q\\\" \\u00e9 \\x41 \\\\ end\"\nempty1: ''\nempty2: \"\"\n",
        "plain: this is\n  a folded\n  plain scalar\nnext: x\n",
        "dq: \"first line\n  second line\n\n  after blank\"\nsq: 'one\n  two'\n",
        "lit: |\n  line 1\n    indented\n  line 3\nkeep: |+\n  text\n\nstrip: |-\n  text\n\nafter: x\n",
        "fold: >\n  folded\n  text\n\n  new para\n    more indented\n  back\nfold_strip: >-\n  a\n  b\n",
        "ind: |2\n    two extra\n  base\nlast: |\n  end",
        "- a\n- b\n- - c\n  - d\n- e: 1\n  f: 2\n-\n  g: 3\n- \n- [x, y]\n",
        "key:\n- one\n- two\nother:\n  - three\nmap:\n  inner:\n    deep: 1\n",
        "flow: {a: 1, b: [2, 3], \"c d\": {e: f}, g: }\nseq: [ 1 , 'two', \"three\", [], {} ]\nmulti: [a,\n  b,\n  c]\n",
        "base: &base\n  x: 1\n  y: 2\nderived:\n  <<: *base\n  y: 3\n  z: 4\nlist: &l [1, 2]\ncopy: *l\nscalar: &s hello\nref: *s\n",
        "b1: &b1 {a: 1, c: 1}\nb2: &b2 {b: 2, c: 2}\nmulti:\n  <<: [*b1, *b2]\n  own: 0\n",
        "--- \nfirst: 1\n...\n---\nsecond: 2\n--- 3\n",
        "# only a comment\n\n# another\nk: v # c\n# trailing\n",
        "\"quoted key\": 1\n'single key': 2\nkey with spaces: 3\n123: four\n",
        "nested:\n  - name: a\n    items:\n      - 1\n      - 2\n  - name: b\n    items: []\n",
        "str: !!str 123\nint: !!int \"42\"\nflt: !!float 3\n",
        "empty_map: {}\nempty_seq: []\nnull_seq:\n  -\n  - ~\n",
        "text: \"line\\nbreak\"\nesc: \"\\/slash \\0 \\a\"\nlong: >\n  a very long folded line that keeps going\n",
        "  indented: root\n  other: 2\n",
        "a:\n  b:\n    c:\n      d: deep\n  e: back\nf: top\n",
        "seqmap:\n- a: 1\n  b: 2\n- c: 3\n",
        "unicode: héllo wörld ☺\n日本: 語\n",
        "- |\n  literal in seq\n- >\n  folded in seq\n- plain\n",
        "x: '- not a seq'\ny: '[not, flow]'\n",
        "k: v\n...\n",
        "---\n- 1\n---\n- 2\n",
        "dash-key: 1\n-neg: 2\nunder_score: 3\n"
    ]
    for i, c in cases {
        var docs = try encoding.yaml.ParseAll(c) catch err {
            println(i, "error", err.Message())
            continue
        }
        for d in docs {
            println(i, canon(d))
        }
    }
}

func sortStrings(a []string) {
    for i in 1..len(a) {
        var j = i
        for j > 0 && a[j - 1] > a[j] {
            var t = a[j]
            a[j] = a[j - 1]
            a[j - 1] = t
            j -= 1
        }
    }
}
