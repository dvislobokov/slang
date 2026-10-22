// test: std.encoding.xml pull Reader: StartElement/EndElement/CharData/Comment/ProcInst/Directive tokens, namespaces (default, prefixed, nested scopes, xml:), entities and character references, CDATA, CRLF and attribute white-space normalization, self-closing elements; well-formedness and safety errors with line numbers (mismatched/unclosed tags, duplicate attributes, undefined entities: no DTD expansion, so an entity bomb fails fast), Strict = false
// status: passing
namespace spec.stdlib3.xml_reader

import std
import std.io

func dump(text string) {
    var r = encoding.xml.NewReader(text)
    for {
        var t = try r.Next() catch err {
            println("  error:", err.Message())
            return
        }
        if t.Kind == encoding.xml.Kind.EOF {
            println("  EOF")
            return
        }
        var attrs []string = []
        for a in t.Attrs {
            attrs = append(
                attrs,
                "{" + a.Name.Space + "}" + a.Name.Local + "=" + encoding.json.Quote(a.Value),
            )
        }
        if t.Kind == encoding.xml.Kind.StartElement || t.Kind == encoding.xml.Kind.EndElement {
            println(
                " ",
                t.Line,
                t.Kind,
                "{" + t.Name.Space + "}" + t.Name.Local,
                t.Name.String(),
                attrs,
            )
        } else {
            println(" ", t.Line, t.Kind, t.Name.Local, encoding.json.Quote(t.Text))
        }
    }
}

func main() {
    println("namespaces:")
    dump(
        "<root xmlns=\"urn:default\" xmlns:a=\"urn:a\"><a:item a:id=\"1\" plain=\"x\"/><inner xmlns=\"urn:inner\"><leaf/></inner><b:u xml:lang=\"en\"/></root>",
    )
    println("text:")
    dump(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<!-- note -->\r\n<t a=\"x&#9;y\n z\" b='&lt;&amp;&gt;&quot;&apos;'>1 &lt; 2 &#x263A; &#65;<![CDATA[ <not> &markup; ]]>end\r\nline</t>",
    )
    println("doctype:")
    dump(
        "<!DOCTYPE bomb [<!ENTITY a \"aaaaaaaaaa\"><!ENTITY b \"&a;&a;&a;&a;&a;&a;&a;&a;&a;&a;\">]><bomb>&b;</bomb>",
    )
    println("errors:")
    for bad in [
        "<a><b></a>",
        "<a>",
        "</a>",
        "<a x=\"1\" x=\"2\"/>",
        "<a x=1/>",
        "<a x=\"<\"/>",
        "<a>&nbsp;</a>",
        "<a>x &amp y</a>",
        "<a><!-- open</a>",
        "<a>\n\n<b>\n</c></a>",
        "<1a/>",
        "<a>&#0;</a>",
    ] {
        dump(bad)
    }
    println("lenient:")
    var r = encoding.xml.NewReader("<p>&nbsp;&copy;</p>")
    r.Strict = false
    for {
        var t = try r.Next() else encoding.xml.Token{Kind: encoding.xml.Kind.EOF}
        if t.Kind == encoding.xml.Kind.EOF {
            break
        }
        if t.Kind == encoding.xml.Kind.CharData {
            println(" ", t.Text, r.Depth())
        }
    }
    println(encoding.xml.Escape("a<b>&c\"d"), encoding.xml.EscapeAttr("a<b>&c\"d\n"))
}
