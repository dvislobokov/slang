// test: std.net.mail against an in-process SMTP server: EHLO, STARTTLS (std.net.tls server), AUTH PLAIN/LOGIN/CRAM-MD5, MAIL/RCPT/DATA with dot-stuffing, MIME build (alternative, attachment, encoded subject, folding), SMTP errors, plain-text AUTH refused
// status: passing
// run-flags: --leak-check
namespace spec.stdlib4.mail_smtp

import std.io
import std

const CertPEM = "-----BEGIN CERTIFICATE-----\nMIIBlTCCATugAwIBAgIUdwzHKFFR9jSgb1hmp35Ag4eQVnMwCgYIKoZIzj0EAwIw\nFDESMBAGA1UEAwwJbG9jYWxob3N0MCAXDTI2MTAyMDIyMTI1NloYDzIxMjYwOTI2\nMjIxMjU2WjAUMRIwEAYDVQQDDAlsb2NhbGhvc3QwWTATBgcqhkjOPQIBBggqhkjO\nPQMBBwNCAARYFUuhrEhEqCXaURmgVzW8K02wCVyhH2HMM7MbEJGxGN10xQtbl9xT\nEC49C9A6sALZQIllZNstBF9nwBf9kdo8o2kwZzAdBgNVHQ4EFgQUiIXwxg3vSv4v\nZMl5fm9gF6BNTbkwHwYDVR0jBBgwFoAUiIXwxg3vSv4vZMl5fm9gF6BNTbkwDwYD\nVR0TAQH/BAUwAwEB/zAUBgNVHREEDTALgglsb2NhbGhvc3QwCgYIKoZIzj0EAwID\nSAAwRQIgKIPUG22D9Uer8pgh/72P7ASAZfozUU+95MRNqkZXj4YCIQDp+mPI+8Mw\nw9FFCGseOrUYuaNwR6h3rQL+o9c+FcMnBA==\n-----END CERTIFICATE-----\n"
const KeyPEM = "-----BEGIN PRIVATE KEY-----\nMIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgiJNillhGcKwgcCOx\n1SSTFhRJeF/AFLwXyHhucI/a1imhRANCAARYFUuhrEhEqCXaURmgVzW8K02wCVyh\nH2HMM7MbEJGxGN10xQtbl9xTEC49C9A6sALZQIllZNstBF9nwBf9kdo8\n-----END PRIVATE KEY-----\n"

interface stream {
    func Read(n int) Task<[]byte!>
    func Write(data []byte) Task<int!>
    func Close()
}

// A tiny SMTP server: one session, a transcript of what it received.
struct server {
    s stream
    raw net.Conn
    buf string
    tlsOn bool
    starttls bool
    log []string
    data string

    async func line() string! {
        for {
            var i = strings.Index(this.buf, "\r\n")
            if i >= 0 {
                var l = this.buf[..i]
                this.buf = this.buf[i + 2..]
                return l
            }
            var b = await this.s.Read(4096)?
            if len(b) == 0 {
                return error("eof")
            }
            this.buf = this.buf + strings.FromBytes(b)
        }
    }

    async func say(text string) void! {
        _ = await this.s.Write(strings.Bytes(text + "\r\n"))?
    }

    async func run() void! {
        await this.say("220 test.local ESMTP ready")?
        for {
            var l = try await this.line() catch err {
                return
            }
            var up = strings.ToUpper(l)
            if strings.HasPrefix(up, "EHLO") {
                this.log = append(this.log, l + " (tls=" + fmt.Format("{0}", this.tlsOn) + ")")
                var ext = "250-test.local\r\n250-8BITMIME\r\n"
                if this.starttls && !this.tlsOn {
                    ext = ext + "250-STARTTLS\r\n"
                }
                await this.say(ext + "250 AUTH PLAIN LOGIN CRAM-MD5")?
            } else if up == "STARTTLS" {
                this.log = append(this.log, l)
                await this.say("220 go ahead")?
                var cert = net.tls.LoadKeyPair(CertPEM, KeyPEM)?
                this.s = net.tls.ServerConn(this.raw, net.tls.ServerConfig{Certificates: [cert]})?
                this.buf = ""
                this.tlsOn = true
            } else if strings.HasPrefix(up, "AUTH PLAIN") {
                var dec = encoding.base64.DecodeString(l[11..])?
                this.log = append(this.log, "AUTH PLAIN " + strings.Replace(dec, "\u{0}", "|"))
                await this.say("235 ok")?
            } else if up == "AUTH LOGIN" {
                await this.say("334 VXNlcm5hbWU6")?
                var u = encoding.base64.DecodeString(await this.line()?)?
                await this.say("334 UGFzc3dvcmQ6")?
                var p = encoding.base64.DecodeString(await this.line()?)?
                this.log = append(this.log, "AUTH LOGIN " + u + "|" + p)
                await this.say("235 ok")?
            } else if up == "AUTH CRAM-MD5" {
                var ch = "<1896.697170952@postoffice.reston.mci.net>"
                await this.say("334 " + encoding.base64.EncodeString(ch))?
                var resp = encoding.base64.DecodeString(await this.line()?)?
                var want = "tim " + encoding.hex.Encode(net.mail.HmacMD5(strings.Bytes("tanstaaftanstaaf"), strings.Bytes(ch)))
                this.log = append(this.log, "AUTH CRAM-MD5 " + resp + " ok=" + fmt.Format("{0}", resp == want))
                await this.say("235 ok")?
            } else if strings.HasPrefix(up, "MAIL FROM:") || strings.HasPrefix(up, "RCPT TO:") {
                this.log = append(this.log, l)
                if strings.Contains(l, "<bounce@") {
                    await this.say("550 5.1.1 no such user")?
                } else {
                    await this.say("250 ok")?
                }
            } else if up == "DATA" {
                await this.say("354 end with .")?
                var b = strings.Builder{}
                for {
                    var d = await this.line()?
                    if d == "." {
                        break
                    }
                    b.Write(d + "\n")
                }
                this.data = b.String()
                this.log = append(this.log, "DATA " + fmt.Format("{0}", len(this.data)) + " bytes")
                await this.say("250 queued")?
            } else if up == "QUIT" {
                this.log = append(this.log, l)
                await this.say("221 bye")?
                this.s.Close()
                return
            } else {
                await this.say("502 unknown")?
            }
        }
    }
}

async func session(l net.Listener, starttls bool) *server! {
    var c = await l.Accept()?
    var s = new server{s: c, raw: c, starttls: starttls}
    await s.run()?
    return s
}

func message() net.mail.Message! {
    var from = net.mail.ParseAddress("Ann Example <ann@example.com>")?
    return net.mail.Message{
        From: from,
        To: net.mail.ParseAddressList("bob@example.com, \"Smith, Carol\" <carol@example.org>")?,
        Bcc: [net.mail.Address{Address: "dave@example.net"}],
        Subject: "Привет, a subject that is long enough to need several encoded words in the header",
        Text: "Hello Bob,\n.leading dot line\nbye\n",
        HTML: "<p>Hello <b>Bob</b></p>",
        Attachments: [net.mail.Attachment{Filename: "notes.txt", Data: strings.Bytes("attached text")}],
        Date: time.Date(2024, 5, 6, 7, 8, 9, 0, time.UTC()),
        MessageID: "<fixed@example.com>",
        Boundary: "BOUND",
        Headers: map[string]string{"x-mailer": "slang"},
    }
}

async func main() void! {
    // address parsing
    for s in ["ann@example.com", "<b@x.org>", "Bob Smith <bob@x.org>", "\"Smith, Bob\" <bob@x.org>", "=?utf-8?q?J=C3=BCrgen?= <j@x.de>", "carl@x.org (Carl)", "bad", "a@b, c@d"] {
        var a = try net.mail.ParseAddress(s) catch err {
            println("parse", s, "->", err)
            continue
        }
        println("parse", s, "->", a.Name, "|", a.Address, "|", a.String())
    }
    var list = net.mail.ParseAddressList("Team: a@x.org, \"B, b\" <b@x.org>;")?
    println(len(list), list[0].Address, list[1].Name)
    println(net.mail.FoldHeader("Subject", "word word word word word word word word word word word word word word word word word"))

    // the built message
    var text = message()?.Build()?
    print(strings.Replace(text, "\r\n", "\n"))

    // plain session with AUTH PLAIN allowed explicitly
    var l = net.Listen("127.0.0.1:0")?
    var addr = fmt.Format("127.0.0.1:{0}", l.Port())
    var srvTask = session(l, false)
    var cfg = net.mail.Config{AllowInsecureAuth: true}
    await net.mail.SendWith(addr, cfg, net.mail.PlainAuth("ann", "secret"), message()?)?
    var srv = await srvTask?
    for x in srv.log {
        println("plain:", x)
    }
    println("dot-stuffed line kept:", strings.Contains(srv.data, "\n..leading dot line\n"))

    // STARTTLS, then AUTH LOGIN
    var tlsTask = session(l, true)
    var tcfg = net.mail.Config{TLS: "starttls", TLSConfig: net.tls.Config{InsecureSkipVerify: true}}
    await net.mail.SendWith(addr, tcfg, net.mail.LoginAuth("ann", "pw"), message()?)?
    for x in (await tlsTask?).log {
        println("tls:", x)
    }

    // CRAM-MD5 (RFC 2195 example credentials) and a rejected recipient
    var cramTask = session(l, false)
    var m = message()?
    m.To = [net.mail.Address{Address: "bounce@example.com"}]
    m.Bcc = []
    try await net.mail.SendWith(addr, net.mail.Config{TLS: "none"}, net.mail.CramMD5Auth("tim", "tanstaaftanstaaf"), m) catch err {
        println("send error:", err)
        var se net.mail.SMTPError
        println(errors.As(err, &se), se.Code, se.Temporary())
    }
    for x in (await cramTask?).log {
        println("cram:", x)
    }

    // PLAIN over an unencrypted connection is refused by default
    var refTask = session(l, false)
    try await net.mail.SendWith(addr, net.mail.Config{TLS: "none"}, net.mail.PlainAuth("ann", "secret"), message()?) catch err {
        println("refused:", err)
    }
    _ = await refTask?
    l.Close()
}
