// test: std.encoding.toml agrees with Python tomllib (TOML 1.0) on 20 valid documents (integers in all bases, floats incl. inf/nan, all string forms, tables, dotted and quoted keys, arrays of tables, nested arrays, inline tables, comments) and rejects the same 30 invalid ones (redefinitions, bad numbers, escapes, dates, inline-table rules)
// status: passing
namespace spec.stdlib3.toml_corpus

import std
import std.io

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

func canon(v encoding.toml.Value) string {
    if v.IsBool() || v.IsInt() {
        return v.Format()
    }
    if v.IsFloat() {
        var f = try v.Float() else 0.0
        if math.IsNaN(f) {
            return "nan"
        }
        if math.IsInf(f, 1) {
            return "inf"
        }
        if math.IsInf(f, -1) {
            return "-inf"
        }
        var s = fmt.Format("{0}", f)
        if !strings.Contains(s, ".") && !strings.Contains(s, "e") {
            s += ".0"
        }
        return s
    }
    if v.IsString() {
        return encoding.json.Quote(try v.String() else "")
    }
    if v.IsArray() {
        var parts []string = []
        for x in v.Items() {
            parts = append(parts, canon(x))
        }
        return "[" + strings.Join(parts, " ") + "]"
    }
    var keys = v.Keys()
    sortStrings(keys)
    var parts []string = []
    for k in keys {
        parts = append(parts, encoding.json.Quote(k) + ":" + canon(v.Get(k)))
    }
    return "{" + strings.Join(parts, " ") + "}"
}

func main() {
    var valid = [
        "a = 1\nb = -2\nc = +3\nd = 1_000\ne = 0xDEAD_beef\nf = 0o755\ng = 0b1101\n",
        "f1 = 1.0\nf2 = -0.5\nf3 = 5e+22\nf4 = 1e05\nf5 = -2E-2\nf6 = 6.626e-34\nf7 = 224_617.445_991\nf8 = inf\nf9 = -inf\nf10 = nan\nf11 = +1.5\n",
        "b1 = true\nb2 = false\n",
        "s1 = \"basic \\\"q\\\" \\t \\u00E9 \\U0001F600 \\\\\"\ns2 = 'literal \\\\no escape'\ns3 = \"\"\ns4 = ''\n",
        "s = \"\"\"\nRoses are red\nViolets are blue\"\"\"\nt = \"\"\"\\\n  The quick \\\n  brown fox.\"\"\"\nu = \"\"\"a\"\"b\"\"\"\nv = \"\"\"\"\"quotes\"\"\"\"\"\n",
        "lit = '''\nfirst\n  second\n'''\nraw = '''I [dw]on't need \\d{2}'''\nq = ''''one quote''''\n",
        "[table]\nkey = \"value\"\n[table.sub]\nx = 1\n[\"quoted key\"]\ny = 2\n[a . b . c]\nz = 3\n",
        "name = \"x\"\nphysical.color = \"orange\"\nphysical.shape = \"round\"\nsite.\"google.com\" = true\n",
        "[fruit]\napple.color = \"red\"\napple.taste.sweet = true\n[fruit.apple.texture]\nsmooth = true\n",
        "[[products]]\nname = \"Hammer\"\nsku = 738594937\n[[products]]\n[[products]]\nname = \"Nail\"\ncolor = \"gray\"\n",
        "[[fruits]]\nname = \"apple\"\n[fruits.physical]\ncolor = \"red\"\n[[fruits.varieties]]\nname = \"red delicious\"\n[[fruits.varieties]]\nname = \"granny smith\"\n[[fruits]]\nname = \"banana\"\n[[fruits.varieties]]\nname = \"plantain\"\n",
        "arr1 = [ 1, 2, 3 ]\narr2 = [ \"red\", \"yellow\", \"green\" ]\narr3 = [ [ 1, 2 ], [3, 4, 5] ]\narr4 = [ \"all\", 'strings', \"\"\"are the same\"\"\", '''type''' ]\narr5 = [ [ 1, 2 ], [\"a\", \"b\", \"c\"] ]\narr6 = [\n  1, # one\n  2,\n]\narr7 = []\nmixed = [ 0.1, 0.2, 0.5, 1, 2, 5 ]\n",
        "name = { first = \"Tom\", last = \"Preston-Werner\" }\npoint = { x = 1, y = 2 }\nanimal = { type.name = \"pug\" }\nempty = {}\nnested = { a = { b = [1, {c = 2}] } }\n",
        "# comment\nkey = \"value\" # trailing\n\n\n  indented = 1\n\t[ spaced ]\n\tz = 0\n",
        "1234 = \"numeric key\"\n- = \"dash\"\n_ = \"under\"\n\"\" = \"empty\"\n",
        "[x.y.z.w]\n[x]\na = 1\n",
        "str1 = \"The quick brown fox jumps over the lazy dog.\"\nstr2 = \"\"\"\nThe quick brown \\\n\n\n  fox jumps over \\\n    the lazy dog.\"\"\"\n",
        "utf = \"日本語 ☺\"\n\"key_ü\" = 1\n",
        "a.b = 1\na.c = 2\n[d]\ne = 3\n",
        "x = 0\ny = -0\nz = +0\nw = 0.0\nv = -0.0\n"
    ]
    var invalid = [
        "a = 1\na = 2\n",
        "[t]\n[t]\n",
        "a = 01\n",
        "a = 1__0\n",
        "a = _1\n",
        "a = 0x\n",
        "a = \"unterminated\n",
        "a = { b = 1, }\n",
        "a = {\nb = 1 }\n",
        "[a]\nb = 1\n[a.b]\n",
        "[[a]]\n[a]\n",
        "a = [1]\n[[a]]\n",
        "a = 1 b = 2\n",
        "= 1\n",
        "a = .5\n",
        "a = 5.\n",
        "a = 1e\n",
        "a = \"\\x\"\n",
        "a = \"\\uD800\"\n",
        "a = 1979-13-01\n",
        "a = 1979-02-30\n",
        "a = 25:00:00\n",
        "a = tru\n",
        "[a\n",
        "[[a]\n",
        "a.b = 1\na.b.c = 2\n",
        "a = \"line\nbreak\"\n",
        "a = [1 2]\n",
        "x = { a = 1 }\nx.b = 2\n",
        "[fruit]\napple.color = \"red\"\n[fruit.apple]\n"
    ]
    for i, c in valid {
        var v = try encoding.toml.ParseValue(c) catch err {
            println(i, "error", err.Message())
            continue
        }
        println(i, canon(v))
    }
    for i, c in invalid {
        try encoding.toml.ParseValue(c) catch err {
            println(fmt.Format("e{0}", i), "error")
            continue
        }
        println(fmt.Format("e{0}", i), "ACCEPTED")
    }
}
