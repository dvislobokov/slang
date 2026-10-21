// test: std.html.Escape / Unescape agree with Go html.EscapeString / UnescapeString (full WHATWG entity table, legacy entities without ";", numeric references incl. C1 remapping and invalid code points)
// status: passing
namespace spec.stdlib3.html_escape

import std
import std.io

func main() {
    var us = [
        "&amp;",
        "&lt;&gt;&quot;&#39;&apos;",
        "&amp",
        "&ampx",
        "&notit;",
        "&notin;",
        "&not",
        "&NotEqualTilde;",
        "&#65;&#x42;&#X43;",
        "&#0;",
        "&#x110000;",
        "&#128;&#150;",
        "&#xD800;",
        "&;",
        "& ",
        "&#;",
        "&#x;",
        "&unknown;",
        "&copy2024",
        "&AElig &aelig;",
        "plain text",
        "",
        "&&&amp;&",
        "&lt",
        "&LT;",
        "&ThinSpace;&hairsp;",
        "&#9999999999;",
        "a&#x41b",
        "&fjlig;&bne;",
        "caf&eacute; &mdash; &hellip;",
    ]
    for u in us {
        println(encoding.hex.EncodeString(html.Unescape(u)))
    }
    var ss = ["<a href='x'>&\"</a>", "", "plain", "é<é>", "&amp;"]
    for s in ss {
        println(encoding.hex.EncodeString(html.Escape(s)), html.Unescape(html.Escape(s)) == s)
    }
}
