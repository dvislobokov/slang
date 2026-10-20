// test: std.net.url agrees with Go net/url: Parse of 61 URLs (schemes, opaque, userinfo with escapes, IPv4/IPv6 hosts and zones, ports, escaped paths with RawPath, queries, fragments, errors) and String round trips, ResolveReference on the RFC 3986 examples, ParseQuery/Encode, JoinPath, QueryEscape/PathEscape
// status: passing
namespace spec.stdlib3.url_corpus

import std
import std.io

func q(s string) string {
    return encoding.json.Quote(s)
}

func show(u net.url.URL) string {
    var user = "-"
    if u.User != nil {
        user = u.User.Username + "/" + u.User.Password + "/" + fmt.Format("{0}", u.User.HasPassword)
    }
    return "s=" + q(u.Scheme) + " o=" + q(u.Opaque) + " u=" + user + " h=" + q(u.Host) + " p=" + q(u.Port) + " path=" + q(u.Path) + " raw=" + q(u.RawPath) + " q=" + q(u.RawQuery) + " f=" + q(u.Fragment) + " str=" + q(u.String())
}

func queryTest(s string) {
    var failed = false
    try net.url.ParseQuery(s) catch err {
        failed = true
    }
    var u = net.url.URL{RawQuery: s}
    println(u.Query().Encode(), failed)
}

func main() {
    var us = ["http://www.google.com", "http://www.google.com/", "http://www.google.com/file%20one%26two", "http://www.google.com/#file%20one%26two", "ftp://webmaster@www.google.com/", "ftp://john%20doe@www.google.com/", "http://www.google.com/?", "http://www.google.com/?foo=bar?", "http://www.google.com/?q=go+language", "http://www.google.com/?q=go%20language", "http://www.google.com/a%20b?q=c+d", "http:www.google.com/?q=go+language", "http:%2f%2fwww.google.com/?q=go+language", "mailto:/webmaster@golang.org", "mailto:webmaster@golang.org", "/foo?query=http://bad", "//foo", "//user@foo/path?a=b", "///threeslashes", "http://user:password@google.com", "http://j@ne:password@google.com", "http://jane:p@ssword@google.com", "http://j@ne:password@google.com/p@th?q=@go", "http://www.google.com/?q=go+language#foo", "http://www.google.com/?q=go+language#foo&bar", "http://www.google.com/?q=go+language#foo%26bar", "file:///home/adg/rabbits", "file:///C:/FooBar/Baz.txt", "MaIlTo:webmaster@golang.org", "a/b/c", "http://%3Fam:pa%3Fsword@google.com", "http://192.168.0.1/", "http://192.168.0.1:8080/", "http://[fe80::1]/", "http://[fe80::1]:8080/", "http://[fe80::1%25en0]/", "http://rest.rsc.io/foo%2fbar/baz%2Fquux?alt=media", "mysql://a,b,c/bar", "scheme://!$&'()*+,;=hello!:1/path", "http://host/!$&'()*+,;=:@[hello]", "http://example.com/oid/[order_id]", "http://192.168.0.2:8080/foo", "http://[2b01:e34:ef40:7730:8e70:5aff:fefe:edac]:8080/foo", "http://hello.世界.com/foo", "http://example.com//foo", "myscheme://authority<\"hi\">/foo", "tcp://[2020::2020:20:2020:2020%25Windows%20Loves%20Spaces]:2020", "magnet:?xt=urn:btih:c12fe1c06bba254a9dc9f519b335aa7c1367a88a&dn", "mailto:?subject=hi", "http://[::1", "http://a b.com/", "http://host:port/", "%gh&%ij", "http://example.com/%zz", "1http://foo", ":foo", "cache_object:foo/bar", "http://x.com/a\u{7F}b", "", "*", "https://user@host:8443/p/a/t/h?query=1&other=%20#frag ment"]
    for i, s in us {
        var u = try net.url.Parse(s) catch err {
            println(i, "error", err.Message())
            continue
        }
        println(i, show(u))
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("g:h") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("g") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("./g") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("g/") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("/g") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("//g") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("?y") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("g?y") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("#s") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("g#s") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("g?y#s") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse(";x") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("g;x") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("g;x?y#s") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse(".") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("./") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("..") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("../") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("../g") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("../..") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("../../") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("../../g") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("../../../g") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("../../../../g") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("/./g") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("/../g") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("g.") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse(".g") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("g..") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("..g") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("./../g") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("./g/.") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("g/./h") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("g/../h") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("g;x=1/./y") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("g;x=1/../y") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("g?y/./x") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("g?y/../x") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("g#s/./x") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("g#s/../x") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://a/b/c/d;p?q") else net.url.URL{}
        var r = try net.url.Parse("http:g") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://foo.com/bar") else net.url.URL{}
        var r = try net.url.Parse("mailto:x") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("mailto:a@b") else net.url.URL{}
        var r = try net.url.Parse("#f") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://foo.com?q=1") else net.url.URL{}
        var r = try net.url.Parse("") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    {
        var b = try net.url.Parse("http://foo.com/a/b/") else net.url.URL{}
        var r = try net.url.Parse("c/../../d") catch err {
            println("error")
            return
        }
        println(b.ResolveReference(r).String())
    }
    queryTest("a=1&b=2")
    queryTest("a=1&a=2&b")
    queryTest("x=%zz&y=1")
    queryTest("a=1;b=2")
    queryTest("q=go+lang&q2=go%20lang")
    queryTest("=empty&k=")
    queryTest("")
    queryTest("%E2%82%AC=euro")
    {
        var u = try net.url.Parse("https://go.dev") else net.url.URL{}
        var es []string = ["x", "y"]
        println(u.JoinPath(es).String())
    }
    {
        var u = try net.url.Parse("https://go.dev/a/") else net.url.URL{}
        var es []string = ["../b", "c/"]
        println(u.JoinPath(es).String())
    }
    {
        var u = try net.url.Parse("https://go.dev/a") else net.url.URL{}
        var es []string = ["/b//c", "d"]
        println(u.JoinPath(es).String())
    }
    {
        var u = try net.url.Parse("rel/path") else net.url.URL{}
        var es []string = ["../../up"]
        println(u.JoinPath(es).String())
    }
    {
        var u = try net.url.Parse("https://go.dev") else net.url.URL{}
        var es []string = []
        println(u.JoinPath(es).String())
    }
    {
        var u = try net.url.Parse("https://go.dev/a%2fb") else net.url.URL{}
        var es []string = ["c"]
        println(u.JoinPath(es).String())
    }
    println(net.url.QueryEscape("a b/c?d&e=f+g"), net.url.PathEscape("a b/c?d&e=f+g"))
    println(net.url.QueryEscape("100%"), net.url.PathEscape("100%"))
    println(net.url.QueryEscape("/path"), net.url.PathEscape("/path"))
    println(net.url.QueryEscape("ü"), net.url.PathEscape("ü"))
    println(net.url.QueryEscape("a:b@c"), net.url.PathEscape("a:b@c"))
}
