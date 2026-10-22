// test: std.web.auth.jwt (HS256, RS256, ES256 sign/verify, claims, expiry and not-before with leeway, issuer/audience, wrong key or algorithm, tampering) and UseAuth with @authorize(roles), @allowAnonymous and RequireAuthorization on minimal endpoints (401 vs 403)
// status: passing
// run-flags: --leak-check
namespace spec.web.jwt_auth

import std
import std.io
import std.web
import std.web.auth

@route("/reports")
@authorize
struct ReportsController {
    @get("")
    func List(c *Context) string {
        return "reports for " + c.User.Name + " (" + c.User.Id + ")"
    }

    @delete("{id}")
    @authorize(roles: ["admin", "auditor"])
    func Delete(id int) string {
        return "deleted " + fmt.Format("{0}", id)
    }

    @get("public")
    @allowAnonymous
    func Public() string {
        return "public"
    }
}

async func show(who string, cl *TestClient, m string, path string) {
    var r = await cl.Send(m, path, "", nil)
    println(who, m, path, "->", r.Status, r.Header("WWW-Authenticate"), r.Body)
}

func now() int {
    return 1700000000
}

async func main() void! {
    var key = jwt.HS256("a-shared-secret-of-some-length")
    var claims = jwt.Claims{
        Subject: "u1",
        Name: "Ann",
        Email: "ann@example.com",
        Roles: ["user", "auditor"],
        Issuer: "spec",
        Audience: "api",
        IssuedAt: 1699990000,
        ExpiresAt: 1700003600,
    }
    claims.Set("tenant", encoding.json.NewString("acme"))
    var t = jwt.Sign(claims, key)?
    var parts = strings.Split(t, ".")
    println("parts:", len(parts), parts[0])
    var (header, decoded) = jwt.Decode(t)?
    println("header:", header.Format())
    println("payload:", decoded.Json())
    var opts = jwt.VerifyOptions{Issuer: "spec", Audience: "api", Clock: now}
    var v = jwt.Verify(t, key, opts)?
    println("verified:", v.Subject, v.Name, v.Roles, v.HasRole("auditor"), v.String("tenant"))
    try jwt.Verify(t, jwt.HS256("another-secret"), opts) catch err { println(err) }
    try jwt.Verify(t, key, jwt.VerifyOptions{Issuer: "other", Clock: now}) catch err {
        println(err)
    }
    try jwt.Verify(t, key, jwt.VerifyOptions{Audience: "web", Clock: now}) catch err {
        println(err)
    }
    try jwt.Verify(t, key, jwt.VerifyOptions{Clock: () => 1700003601}) catch err { println(err) }
    println("leeway ok:", jwt.Verify(t, key, jwt.VerifyOptions{
        Clock: () => 1700003601,
        Leeway: 5,
    })?.Subject)
    var early = jwt.Sign(jwt.Claims{Subject: "x", NotBefore: 1700000100}, key)?
    try jwt.Verify(early, key, opts) catch err { println(err) }
    var tampered = parts[0] +
        "." +
        encoding.base64.EncodeURL(strings.Bytes("{\"sub\":\"admin\",\"roles\":[\"admin\"]}")) +
        "." +
        parts[2]
    try jwt.Verify(tampered, key, opts) catch err { println(err) }
    var none = encoding.base64.EncodeURL(strings.Bytes("{\"alg\":\"none\"}")) + "." + parts[1] + "."
    try jwt.Verify(none, key, opts) catch err { println(err) }
    try jwt.Verify("abc", key, opts) catch err { println(err) }

    var der = crypto.rsa.GenerateKey(1024)?
    var rsa = jwt.RS256(der)?
    var rt = jwt.Sign(jwt.Claims{Subject: "r"}, rsa)?
    var rpub = crypto.rsa.PublicKeyOf(der)?
    var pub = jwt.RS256Public(rpub.N, rpub.E)
    println("RS256:", jwt.Verify(rt, pub, jwt.VerifyOptions{

    })?.Subject, len(strings.Split(rt, ".")[2]))
    try jwt.Sign(jwt.Claims{}, pub) catch err { println(err) }
    try jwt.Verify(rt, key, jwt.VerifyOptions{}) catch err { println(err) }
    var ecPriv = crypto.ecdsa.GenerateKey()?
    var es = jwt.ES256(ecPriv)?
    var et = jwt.Sign(jwt.Claims{Subject: "e"}, es.WithKid("k1"))?
    var (eh, _) = jwt.Decode(et)?
    println("ES256:", jwt.Verify(
        et,
        jwt.ES256Public(crypto.ecdsa.PublicKey(ecPriv)?),
        jwt.VerifyOptions{},
    )?.Subject, eh.Get("kid").String()?, len(encoding.base64.DecodeURL(strings.Split(et, ".")[2])?))

    var app = NewApp()
    app.UseAuth(AuthOptions{Key: key, Issuer: "spec", Clock: now})
    app.MapController<ReportsController>()
    app.MapGet(
        "/me",
        (u *Principal) => u.Name + " " + u.Claims.String("tenant"),
    ).RequireAuthorization([])
    app.MapGet("/admin", () => "admin area").RequireAuthorization(["admin"])
    var c = TestServer(app)?
    var user = c.WithBearer(t)
    var adminToken = jwt.Sign(
        jwt.Claims{Subject: "root", Name: "Root", Roles: ["admin"], Issuer: "spec"},
        key,
    )?
    var admin = c.WithBearer(adminToken)
    await show("anonymous", c, "GET", "/reports")
    await show("user", user, "GET", "/reports")
    await show("anonymous", c, "GET", "/reports/public")
    await show("user", user, "DELETE", "/reports/3")
    await show("admin", admin, "DELETE", "/reports/4")
    await show("user", user, "GET", "/me")
    await show("user", user, "GET", "/admin")
    await show("admin", admin, "GET", "/admin")
    await show("forged", c.WithBearer(tampered), "GET", "/reports/public")
    await show("basic", c.WithHeader("Authorization", "Basic YTpi"), "GET", "/reports/public")
}
