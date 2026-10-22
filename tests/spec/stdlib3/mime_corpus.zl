// test: std.mime agrees with Go mime / mime/quotedprintable / mime/multipart: ParseMediaType (case folding, quoting, RFC 2231 charsets and continuations, errors), FormatMediaType (quoting, RFC 2231 for non-ASCII, sorted keys, invalid types), TypeByExtension, quoted-printable encoding and decoding, a multipart Writer body byte for byte and reading it back
// status: passing
namespace spec.stdlib3.mime_corpus

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

func showParams(p map[string]string) string {
    var ks []string = []
    for k, _ in p {
        ks = append(ks, k)
    }
    sortStrings(ks)
    var parts []string = []
    for k in ks {
        parts = append(parts, k + "=" + encoding.json.Quote(p[k]))
    }
    return "{" + strings.Join(parts, " ") + "}"
}

func q(s string) string {
    return encoding.json.Quote(s)
}

func main() void! {
    var ps = [
        "text/html",
        "text/html; charset=UTF-8",
        "Text/HTML ; Charset=\"utf-8\"",
        "multipart/form-data; boundary=\"--abc--\"",
        "attachment; filename=\"a b.txt\"",
        "form-data; name=\"f\"; filename=\"C:\\\\x\\\\y.txt\"",
        "application/x-stuff; title*=us-ascii'en-us'This%20is%20%2A%2A%2Afun%2A%2A%2A",
        "message/external-body; access-type=URL; URL*0=\"ftp://\"; URL*1=\"cs.utk.edu/pub/moore/bulk-mailer/bulk-mailer.tar\"",
        "application/x-stuff; title*0*=us-ascii'en'This%20is%20even%20more%20; title*1*=%2A%2A%2Afun%2A%2A%2A%20; title*2=\"isn't it!\"",
        "text/plain; name*=utf-8''%E2%82%AC%20rates",
        "text/plain;",
        "text/",
        "/plain",
        "text/plain; =x",
        "text/plain; a=1; a=2",
        "text/plain; a",
        "",
        "foo bar",
        "text/plain; a=\"unterminated",
        "inline",
        "text/plain;charset=utf-8;format=flowed",
    ]
    for i, p in ps {
        var m = try mime.ParseMediaType(p) catch err {
            println(i, "error")
            continue
        }
        println(i, m.Type, showParams(m.Params))
    }
    println(q(mime.FormatMediaType("text/html", map[string]string{})))
    println(q(mime.FormatMediaType("TEXT/html", map[string]string{"charset": "utf-8"})))
    println(q(mime.FormatMediaType("attachment", map[string]string{"filename": "a b.txt"})))
    println(q(mime.FormatMediaType("text/plain", map[string]string{
        "name": "€ rates",
        "a": "x\"y",
    })))
    println(q(mime.FormatMediaType("noslash", map[string]string{})))
    println(q(mime.FormatMediaType("bad type", map[string]string{})))
    println(q(mime.FormatMediaType("text/plain", map[string]string{
        "z": "1",
        "a": "2",
        "m": "tok",
    })))
    println(q(mime.TypeByExtension(".html")))
    println(q(mime.TypeByExtension(".HTML")))
    println(q(mime.TypeByExtension(".png")))
    println(q(mime.TypeByExtension(".json")))
    println(q(mime.TypeByExtension(".css")))
    println(q(mime.TypeByExtension(".svg")))
    println(q(mime.TypeByExtension(".nope")))
    println(q(mime.TypeByExtension("")))
    println(q(mime.TypeByExtension(".webp")))
    println(q(mime.TypeByExtension(".js")))
    println(q(mime.quotedprintable.Encode("plain text")))
    println(q(mime.quotedprintable.Encode("café = été")))
    println(q(mime.quotedprintable.Encode("trailing space \nnext")))
    println(q(mime.quotedprintable.Encode("tab\tend\t")))
    println(
        q(
            mime.quotedprintable.Encode(
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            ),
        ),
    )
    println(
        q(
            mime.quotedprintable.Encode(
                "x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=x=",
            ),
        ),
    )
    println(q(mime.quotedprintable.Encode("line1\r\nline2\n")))
    println(q(mime.quotedprintable.Encode("ÿÿÿÿÿÿÿÿÿÿÿÿÿÿÿÿÿÿÿÿÿÿÿÿÿÿÿÿÿÿ")))
    println(q(try mime.quotedprintable.Decode("foo=\r\nbar") else "error"))
    println(q(try mime.quotedprintable.Decode("caf=C3=A9") else "error"))
    println(q(try mime.quotedprintable.Decode("=3D=3d") else "error"))
    println(q(try mime.quotedprintable.Decode("trailing   \r\nnext") else "error"))
    println(q(try mime.quotedprintable.Decode("bad=ZZ") else "error"))
    println(q(try mime.quotedprintable.Decode("soft=\nbreak") else "error"))
    println(q(try mime.quotedprintable.Decode("end=") else "error"))
    var w = mime.multipart.NewWriter()
    w.SetBoundary("XyZ-boundary")?
    w.WriteField("name", "Ann \"A\"")?
    w.CreateFormFile("upload", "dir/report.txt", strings.Bytes("file\r\ncontent\n"))?
    var h = net.textproto.NewHeader()
    h.Set("Content-Type", "text/plain")
    h.Set("x-custom", "1")
    w.CreatePart(h, strings.Bytes("raw part"))?
    w.Close()
    println(q(w.String()))
    println(w.FormDataContentType())
    var r = mime.multipart.NewReader(w.String(), "XyZ-boundary")
    for {
        var p = r.NextPart()?
        if p == nil {
            break
        }
        println(q(p.FormName()), q(p.FileName()), q(p.Header.Get("Content-Type")), q(p.Body))
    }
}
