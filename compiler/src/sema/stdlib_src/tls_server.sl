namespace std.net.tls

// TLS server handshake (part of std.net.tls, see tls.sl): TLS 1.3 with
// HelloRetryRequest, PSK resumption from our own tickets (psk_dhe_ke only),
// client certificates; TLS 1.2 ECDHE + AEAD with extended_master_secret
// (required), RFC 5077 tickets and the downgrade sentinel when 1.3 is
// also enabled. 0-RTT is never accepted.

// Parsed ClientHello.
struct clientHello {
    raw []byte
    version int
    random []byte
    sessionID []byte
    suites []int
    serverName string
    groups []int
    schemes []int
    alpn []string
    versions []int
    shareGroups []int
    shareKeys [][]byte
    pskModes []int
    pskIdentities [][]byte
    pskAges []int
    pskBinders [][]byte
    // bytes of the binders list (with its length) at the end of raw
    bindersLen int
    ems bool
    reneg bool
    renegOK bool
    ticketExt bool
    ticket []byte
    cookie []byte
    pointFormats bool
}

func parseClientHello(c *conn, m hsMsg) clientHello! {
    var p = newParser(m.body)
    var ch = clientHello{raw: m.raw, version: p.u16(), random: p.take(32), sessionID: p.vec8()}
    var sp = newParser(p.vec16())
    for !sp.empty() {
        ch.suites = append(ch.suites, sp.u16())
    }
    var comp = p.vec8()
    if !p.ok() || !sp.ok() || len(ch.sessionID) > 32 {
        return c.fail(50, "malformed ClientHello")
    }
    var hasNull = false
    for x in comp {
        if int(x) == 0 {
            hasNull = true
        }
    }
    if !hasNull {
        return c.fail(47, "ClientHello without the null compression method")
    }
    if containsInt(ch.suites, 255) {
        ch.reneg = true
        ch.renegOK = true
    }
    if p.empty() {
        return ch
    }
    var exts = newParser(p.vec16())
    if !p.ok() || !p.empty() {
        return c.fail(50, "malformed ClientHello extensions")
    }
    var seen []int = []
    for !exts.empty() {
        var typ = exts.u16()
        var data = exts.vec16()
        if !exts.ok() {
            return c.fail(50, "malformed ClientHello extension")
        }
        if containsInt(seen, typ) {
            return c.fail(47, "duplicate extension in ClientHello")
        }
        if containsInt(seen, 41) {
            return c.fail(47, "pre_shared_key is not the last extension")
        }
        seen = append(seen, typ)
        var e = newParser(data)
        switch typ {
            case 0:
                var list = newParser(e.vec16())
                for !list.empty() {
                    var nt = list.u8()
                    var name = list.vec16()
                    if nt == 0 {
                        ch.serverName = std.strings.ToLower(std.strings.FromBytes(name))
                    }
                }
                if !list.ok() {
                    return c.fail(50, "malformed server_name")
                }
            case 10:
                var gl = newParser(e.vec16())
                for !gl.empty() {
                    ch.groups = append(ch.groups, gl.u16())
                }
            case 13:
                var sl = newParser(e.vec16())
                for !sl.empty() {
                    ch.schemes = append(ch.schemes, sl.u16())
                }
            case 16:
                var al = newParser(e.vec16())
                for !al.empty() {
                    ch.alpn = append(ch.alpn, std.strings.FromBytes(al.vec8()))
                }
                if !al.ok() {
                    return c.fail(50, "malformed ALPN extension")
                }
            case 43:
                var vl = newParser(e.vec8())
                for !vl.empty() {
                    ch.versions = append(ch.versions, vl.u16())
                }
            case 51:
                var kl = newParser(e.vec16())
                for !kl.empty() {
                    ch.shareGroups = append(ch.shareGroups, kl.u16())
                    ch.shareKeys = append(ch.shareKeys, kl.vec16())
                }
                if !kl.ok() {
                    return c.fail(50, "malformed key_share")
                }
            case 45:
                var ml = e.vec8()
                for x in ml {
                    ch.pskModes = append(ch.pskModes, int(x))
                }
            case 41:
                var ids = newParser(e.vec16())
                for !ids.empty() {
                    ch.pskIdentities = append(ch.pskIdentities, ids.vec16())
                    ch.pskAges = append(ch.pskAges, ids.u32())
                }
                var start = e.off
                var bl = newParser(e.vec16())
                ch.bindersLen = e.off - start
                for !bl.empty() {
                    ch.pskBinders = append(ch.pskBinders, bl.vec8())
                }
                if !ids.ok() || !bl.ok() || len(ch.pskBinders) != len(ch.pskIdentities) {
                    return c.fail(50, "malformed pre_shared_key")
                }
            case 23:
                ch.ems = true
            case 65281:
                ch.reneg = true
                ch.renegOK = len(data) == 1 && int(data[0]) == 0
                e.off = len(data)
            case 35:
                ch.ticketExt = true
                ch.ticket = data
                e.off = len(data)
            case 44:
                ch.cookie = e.vec16()
            case 11:
                ch.pointFormats = true
                e.off = len(data)
            default:
                e.off = len(data)
        }
        if !e.ok() || !e.empty() {
            return c.fail(50, std.fmt.Format("malformed extension {0} in ClientHello", typ))
        }
    }
    return ch
}

// The certificate for the requested host (first matching SAN, else the first).
func pickCertificate(cfg ServerConfig, host string) Certificate {
    if host != "" {
        for cert in cfg.Certificates {
            var matched = true
            try cert.Chain[0].VerifyHostname(host) catch err {
                matched = false
            }
            if matched {
                return cert
            }
        }
    }
    return cfg.Certificates[0]
}

// The application protocol to use (server preference); "" if none.
func pickALPN(c *conn, ch clientHello) string! {
    var srv = c.srv.cfg.ALPN
    if len(ch.alpn) == 0 || len(srv) == 0 {
        return ""
    }
    for p in srv {
        for q in ch.alpn {
            if p == q {
                return p
            }
        }
    }
    return c.fail(120, "no application protocol in common")
}

// ---- tickets ----

func sealTicket(st *serverState, plain []byte) []byte! {
    var nonce = randomBytes(12)?
    return cat(nonce, std.crypto.aes.GcmSeal(st.ticketKey, nonce, plain, bytesOf("slang tls ticket"))?)
}

// The ticket contents, or nil if it is not ours or is damaged.
struct ticketData {
    version int
    suite int
    created int
    ageAdd int
    secret []byte
    ems bool
    alpn string
    // the client certificate chain of the original handshake (DER)
    certs []*std.crypto.x509.Certificate
}

func encodeTicket(t ticketData) []byte {
    var b = newBuilder()
    b.u16(t.version)
    b.u16(t.suite)
    b.raw(beBytes(t.created, 8))
    b.u32(t.ageAdd)
    b.vec8(t.secret)
    if t.ems {
        b.u8(1)
    } else {
        b.u8(0)
    }
    b.vec8(bytesOf(t.alpn))
    var l = b.open(3)
    for cert in t.certs {
        b.vec24(cert.Raw)
    }
    b.close(l, 3)
    return b.bytes()
}

func openTicket(st *serverState, ticket []byte) *ticketData {
    if len(ticket) < 12 + 16 {
        return nil
    }
    var plain = try std.crypto.aes.GcmOpen(st.ticketKey, ticket[..12], ticket[12..], bytesOf("slang tls ticket")) else []
    if len(plain) == 0 {
        return nil
    }
    var p = newParser(plain)
    var t = new ticketData{version: p.u16(), suite: p.u16()}
    var hi = p.u32()
    t.created = hi * 4294967296 + p.u32()
    t.ageAdd = p.u32()
    t.secret = p.vec8()
    t.ems = p.u8() == 1
    t.alpn = std.strings.FromBytes(p.vec8())
    var cl = newParser(p.vec24())
    for !cl.empty() {
        var cert = try std.crypto.x509.ParseDER(cl.vec24()) else nil
        if cert == nil {
            return nil
        }
        t.certs = append(t.certs, cert)
    }
    if !p.ok() || !p.empty() || !cl.ok() {
        return nil
    }
    // tickets live one day
    if std.time.Now() > t.created + 86400000 {
        return nil
    }
    return t
}

// Verifies client certificates per ServerConfig.ClientAuth.
func checkClientCerts(c *conn, certs []*std.crypto.x509.Certificate) void! {
    var mode = c.srv.cfg.ClientAuth
    if len(certs) == 0 {
        if mode == 2 || mode == 4 {
            return c.fail(116, "client certificate required")
        }
        return
    }
    c.peerCerts = certs
    if mode == 3 || mode == 4 {
        var inter []*std.crypto.x509.Certificate = []
        for i in 1..len(certs) {
            inter = append(inter, certs[i])
        }
        var chain = try std.crypto.x509.Verify(certs[0], std.crypto.x509.VerifyOptions{Roots: c.srv.cfg.ClientCAs,
            Intermediates: inter, KeyUsage: "clientAuth"}) catch err {
            return c.fail(certAlert(err.Message()), "client certificate: " + std.strings.TrimPrefix(err.Message(), "x509: "))
        }
        c.verified = chain
    }
}

async func runServer(c *conn) void! {
    var cfg = c.srv.cfg
    var minV = minVersion(cfg.MinVersion)
    var maxV = maxVersion(cfg.MaxVersion)
    if len(cfg.Certificates) == 0 {
        return c.fail(80, "server has no certificate")
    }
    var m = await c.expect(1, "ClientHello")?
    var ch = parseClientHello(c, m)?
    c.serverName = ch.serverName
    var vers = 0
    if len(ch.versions) > 0 {
        if maxV >= 772 && minV <= 772 && containsInt(ch.versions, 772) {
            vers = 772
        } else if minV <= 771 && containsInt(ch.versions, 771) {
            vers = 771
        }
    } else if ch.version >= 771 && minV <= 771 {
        vers = 771
    }
    if vers == 0 {
        return c.fail(70, "no protocol version in common with the client")
    }
    c.alpn = pickALPN(c, ch)?
    c.addTranscript(m)
    if vers == 772 {
        await server13(c, ch, minV, maxV)?
    } else {
        await server12(c, ch, maxV)?
    }
}

func pickSuite(ours []int, theirs []int) suiteInfo {
    for s in ours {
        if containsInt(theirs, s) {
            return suiteByID(s)
        }
    }
    return suiteInfo{id: 0}
}

async func server13(c *conn, chIn clientHello, minV int, maxV int) void! {
    var cfg = c.srv.cfg
    var ch = chIn
    c.vers = 772
    var s = pickSuite(suites13(), ch.suites)
    if s.id == 0 {
        return c.fail(40, "no TLS 1.3 cipher suite in common")
    }
    c.suite = s
    var hh = s.hash
    var group = 0
    var peerKey []byte = []
    for i in 0..len(ch.shareGroups) {
        if group == 0 && (ch.shareGroups[i] == 29 || ch.shareGroups[i] == 23 || ch.shareGroups[i] == 24) {
            group = ch.shareGroups[i]
            peerKey = ch.shareKeys[i]
        }
    }
    if group == 0 {
        // HelloRetryRequest for a group we support (RFC 8446 4.1.4)
        var want = 0
        if containsInt(ch.groups, 29) {
            want = 29
        } else if containsInt(ch.groups, 23) {
            want = 23
        } else if containsInt(ch.groups, 24) {
            want = 24
        }
        if want == 0 {
            return c.fail(40, "no key exchange group in common")
        }
        var ch1 = c.transcriptHash()?
        var mh = newBuilder()
        mh.u8(254)
        mh.u24(len(ch1))
        mh.raw(ch1)
        c.transcript = mh.bytes()
        var hb = newBuilder()
        hb.u16(771)
        hb.raw(hrrRandom())
        hb.vec8(ch.sessionID)
        hb.u16(s.id)
        hb.u8(0)
        var ext = hb.open(2)
        hb.u16(43)
        hb.u16(2)
        hb.u16(772)
        hb.u16(51)
        hb.u16(2)
        hb.u16(want)
        hb.close(ext, 2)
        c.queueHandshake(2, hb.bytes())
        await c.flush()?
        var m2 = await c.expect(1, "second ClientHello")?
        ch = parseClientHello(c, m2)?
        c.addTranscript(m2)
        if !containsInt(ch.versions, 772) || !containsInt(ch.suites, s.id) {
            return c.fail(47, "second ClientHello is inconsistent with the HelloRetryRequest")
        }
        for i in 0..len(ch.shareGroups) {
            if ch.shareGroups[i] == want {
                group = want
                peerKey = ch.shareKeys[i]
            }
        }
        if group == 0 || len(ch.shareGroups) != 1 {
            return c.fail(47, "second ClientHello has no key share for the requested group")
        }
    }
    // PSK resumption (first identity only, psk_dhe_ke)
    var psk []byte = []
    if len(ch.pskIdentities) > 0 && containsInt(ch.pskModes, 1) && !cfg.DisableSessionTickets {
        var t = openTicket(c.srv, ch.pskIdentities[0])
        if t != nil && t.version == 772 && suiteByID(t.suite).hash == hh {
            var truncated = ch.raw[..len(ch.raw) - ch.bindersLen]
            var early0 = earlySecret(hh, t.secret)?
            var empty = hashOf(hh, [])?
            var binderKey = DeriveSecret(hh, early0, "res binder", empty)?
            var prefix = c.transcript[..len(c.transcript) - len(ch.raw)]
            var th0 = hashOf(hh, cat(prefix, truncated))?
            var want = finishedMAC(hh, binderKey, th0)?
            if !std.crypto.hmac.Equal(want, ch.pskBinders[0]) {
                return c.fail(51, "invalid PSK binder")
            }
            psk = t.secret
            c.resumed = true
            c.peerCerts = t.certs
        }
    }
    var share = newKeyShare(group)?
    var shared = try sharedSecret(share, peerKey) catch err {
        return c.fail(47, err.Message())
    }
    var random = randomBytes(32)?
    var sb = newBuilder()
    sb.u16(771)
    sb.raw(random)
    sb.vec8(ch.sessionID)
    sb.u16(s.id)
    sb.u8(0)
    var ext = sb.open(2)
    sb.u16(43)
    sb.u16(2)
    sb.u16(772)
    sb.u16(51)
    var ks = sb.open(2)
    sb.u16(group)
    sb.vec16(share.pub)
    sb.close(ks, 2)
    if c.resumed {
        sb.u16(41)
        sb.u16(2)
        sb.u16(0)
    }
    sb.close(ext, 2)
    c.queueHandshake(2, sb.bytes())
    var early = earlySecret(hh, psk)?
    var hs = nextSecret(hh, early, shared)?
    var th = c.transcriptHash()?
    var cHS = DeriveSecret(hh, hs, "c hs traffic", th)?
    var sHS = DeriveSecret(hh, hs, "s hs traffic", th)?
    c.wr = cipher13(s, sHS)?
    c.rd = cipher13(s, cHS)?
    // EncryptedExtensions
    var eb = newBuilder()
    var ee = eb.open(2)
    if c.alpn != "" {
        eb.u16(16)
        var a = eb.open(2)
        var al = eb.open(2)
        eb.vec8(bytesOf(c.alpn))
        eb.close(al, 2)
        eb.close(a, 2)
    }
    eb.close(ee, 2)
    c.queueHandshake(8, eb.bytes())
    var askCert = !c.resumed && cfg.ClientAuth > 0
    if !c.resumed {
        if askCert {
            var rb = newBuilder()
            rb.u8(0)
            var re = rb.open(2)
            rb.u16(13)
            var sa = rb.open(2)
            var sal = rb.open(2)
            for sch in ourSchemes() {
                if sch % 256 != 1 {
                    rb.u16(sch)
                }
            }
            rb.close(sal, 2)
            rb.close(sa, 2)
            rb.close(re, 2)
            c.queueHandshake(13, rb.bytes())
        }
        var cert = pickCertificate(cfg, ch.serverName)
        c.queueHandshake(11, certificateBody(cert.Chain, true, []))
        var scheme = pickScheme(cert.Key, ch.schemes, true)
        if scheme == 0 {
            return c.fail(40, "no signature scheme in common with the client")
        }
        var sig = signWith(cert.Key, scheme, signedContent13(true, c.transcriptHash()?))?
        var cvb = newBuilder()
        cvb.u16(scheme)
        cvb.vec16(sig)
        c.queueHandshake(15, cvb.bytes())
    }
    c.queueHandshake(20, finishedMAC(hh, sHS, c.transcriptHash()?)?)
    await c.flush()?
    var master = nextSecret(hh, hs, [])?
    var th2 = c.transcriptHash()?
    var cAP = DeriveSecret(hh, master, "c ap traffic", th2)?
    var sAP = DeriveSecret(hh, master, "s ap traffic", th2)?
    c.wr = cipher13(s, sAP)?
    if askCert {
        var cm = await c.expect(11, "client Certificate")?
        var certs = parseCertificates(c, cm.body, true)?
        c.addTranscript(cm)
        checkClientCerts(c, certs)?
        if len(certs) > 0 {
            var cv = await c.expect(15, "client CertificateVerify")?
            var vp = newParser(cv.body)
            var scheme = vp.u16()
            var sig = vp.vec16()
            if !vp.ok() || !vp.empty() {
                return c.fail(50, "malformed CertificateVerify")
            }
            var content = signedContent13(false, c.transcriptHash()?)
            try verifyWith(certs[0], scheme, content, sig, true) catch err {
                return c.fail(51, std.strings.TrimPrefix(err.Message(), "tls: "))
            }
            c.addTranscript(cv)
        }
    }
    var fin = await c.expect(20, "client Finished")?
    var want = finishedMAC(hh, cHS, c.transcriptHash()?)?
    if !std.crypto.hmac.Equal(want, fin.body) {
        return c.fail(51, "client Finished verification failed")
    }
    c.addTranscript(fin)
    c.rd = cipher13(s, cAP)?
    c.resMaster = DeriveSecret(hh, master, "res master", c.transcriptHash()?)?
    if !cfg.DisableSessionTickets && containsInt(ch.pskModes, 1) {
        var ageBytes = randomBytes(4)?
        var ageAdd = u16(ageBytes, 0) * 65536 + u16(ageBytes, 2)
        var nonce = [byte(0)]
        var tpsk = ExpandLabel(hh, c.resMaster, "resumption", nonce, hashLen(hh))?
        var ticket = sealTicket(c.srv, encodeTicket(ticketData{version: 772, suite: s.id, created: std.time.Now(),
            ageAdd: ageAdd, secret: tpsk, alpn: c.alpn, certs: c.peerCerts}))?
        var nb = newBuilder()
        nb.u32(86400)
        nb.u32(ageAdd)
        nb.vec8(nonce)
        nb.vec16(ticket)
        nb.u16(0)
        var tb = newBuilder()
        tb.u8(4)
        tb.vec24(nb.bytes())
        c.queue(22, tb.bytes())
        await c.flush()?
    }
}

async func server12(c *conn, ch clientHello, maxV int) void! {
    var cfg = c.srv.cfg
    c.vers = 771
    if !ch.ems {
        return c.fail(40, "client does not support extended_master_secret (RFC 7627), which is required")
    }
    if ch.reneg && !ch.renegOK {
        return c.fail(40, "renegotiation is not supported")
    }
    var cert = pickCertificate(cfg, ch.serverName)
    var auth = cert.Key.Algorithm
    if auth == "Ed25519" {
        auth = "ECDSA"
    }
    var s = suiteInfo{id: 0}
    for id in suites12() {
        var cand = suiteByID(id)
        if s.id == 0 && cand.auth == auth && containsInt(ch.suites, id) {
            s = cand
        }
    }
    if s.id == 0 {
        return c.fail(40, "no TLS 1.2 cipher suite in common")
    }
    c.suite = s
    var random = randomBytes(32)?
    if maxV >= 772 {
        rtCopy(random, 24, downgradeSentinel())
    }
    c.clientRandom = ch.random
    c.serverRandom = random
    var tickets = !cfg.DisableSessionTickets && ch.ticketExt
    // resumption from a ticket (RFC 5077 3.1)
    if tickets && len(ch.ticket) > 0 && len(ch.sessionID) > 0 {
        var t = openTicket(c.srv, ch.ticket)
        if t != nil && t.version == 771 && t.suite == s.id && t.ems {
            c.resumed = true
            c.master = t.secret
            c.peerCerts = t.certs
            c.queueHandshake(2, serverHello12(c, ch, random, ch.sessionID, false))
            var ks = deriveKeys12(s, c.master, c.clientRandom, c.serverRandom)?
            c.queue(20, [byte(1)])
            c.wr = ks.server
            c.queueHandshake(20, finished12(c, "server finished")?)
            await c.flush()?
            c.pendRd = ks.client
            var fm = await c.expect(20, "client Finished")?
            if !c.rd.active() {
                return c.fail(10, "Finished before change_cipher_spec")
            }
            var want = finished12(c, "client finished")?
            if !std.crypto.hmac.Equal(want, fm.body) {
                return c.fail(51, "client Finished verification failed")
            }
            c.addTranscript(fm)
            return
        }
    }
    var group = 0
    if containsInt(ch.groups, 29) {
        group = 29
    } else if containsInt(ch.groups, 23) || len(ch.groups) == 0 {
        group = 23
    } else if containsInt(ch.groups, 24) {
        group = 24
    }
    if group == 0 {
        return c.fail(40, "no key exchange group in common")
    }
    var none []byte = []
    c.queueHandshake(2, serverHello12(c, ch, random, none, tickets))
    c.queueHandshake(11, certificateBody(cert.Chain, false, []))
    var share = newKeyShare(group)?
    var pb = newBuilder()
    pb.u8(3)
    pb.u16(group)
    pb.vec8(share.pub)
    var params = pb.bytes()
    var schemes = ch.schemes
    if len(schemes) == 0 {
        schemes = [1025, 1027]
    }
    var scheme = pickScheme(cert.Key, schemes, false)
    if scheme == 0 {
        return c.fail(40, "no signature scheme in common with the client")
    }
    var sig = signWith(cert.Key, scheme, cat3(c.clientRandom, c.serverRandom, params))?
    var kb = newBuilder()
    kb.raw(params)
    kb.u16(scheme)
    kb.vec16(sig)
    c.queueHandshake(12, kb.bytes())
    var askCert = cfg.ClientAuth > 0
    if askCert {
        var rb = newBuilder()
        rb.vec8([byte(1), byte(64)])
        var sa = rb.open(2)
        for sch in ourSchemes() {
            rb.u16(sch)
        }
        rb.close(sa, 2)
        rb.u16(0)
        c.queueHandshake(13, rb.bytes())
    }
    c.queueHandshake(14, none)
    await c.flush()?
    var clientCerts []*std.crypto.x509.Certificate = []
    if askCert {
        var cm = await c.expect(11, "client Certificate")?
        clientCerts = parseCertificates(c, cm.body, false)?
        c.addTranscript(cm)
        checkClientCerts(c, clientCerts)?
    }
    var ckx = await c.expect(16, "ClientKeyExchange")?
    var kp = newParser(ckx.body)
    var point = kp.vec8()
    if !kp.ok() || !kp.empty() {
        return c.fail(50, "malformed ClientKeyExchange")
    }
    var pre = try sharedSecret(share, point) catch err {
        return c.fail(47, err.Message())
    }
    c.addTranscript(ckx)
    c.master = prf12(s.hash, pre, "extended master secret", c.transcriptHash()?, 48)?
    if len(clientCerts) > 0 {
        var cv = await c.expect(15, "client CertificateVerify")?
        var vp = newParser(cv.body)
        var sch = vp.u16()
        var csig = vp.vec16()
        if !vp.ok() || !vp.empty() {
            return c.fail(50, "malformed CertificateVerify")
        }
        try verifyWith(clientCerts[0], sch, c.transcript, csig, false) catch err {
            return c.fail(51, std.strings.TrimPrefix(err.Message(), "tls: "))
        }
        c.addTranscript(cv)
    }
    var ks = deriveKeys12(s, c.master, c.clientRandom, c.serverRandom)?
    c.pendRd = ks.client
    var fin = await c.expect(20, "client Finished")?
    if !c.rd.active() {
        return c.fail(10, "Finished before change_cipher_spec")
    }
    var want = finished12(c, "client finished")?
    if !std.crypto.hmac.Equal(want, fin.body) {
        return c.fail(51, "client Finished verification failed")
    }
    c.addTranscript(fin)
    if tickets {
        var ticket = sealTicket(c.srv, encodeTicket(ticketData{version: 771, suite: s.id, created: std.time.Now(),
            secret: c.master, ems: true, alpn: c.alpn, certs: c.peerCerts}))?
        var nb = newBuilder()
        nb.u32(86400)
        nb.vec16(ticket)
        c.queueHandshake(4, nb.bytes())
    }
    c.queue(20, [byte(1)])
    c.wr = ks.server
    c.queueHandshake(20, finished12(c, "server finished")?)
    await c.flush()?
}

// A TLS 1.2 ServerHello body.
func serverHello12(c *conn, ch clientHello, random []byte, sessionID []byte, ticketExt bool) []byte {
    var b = newBuilder()
    b.u16(771)
    b.raw(random)
    b.vec8(sessionID)
    b.u16(c.suite.id)
    b.u8(0)
    var ext = b.open(2)
    b.u16(23)
    b.u16(0)
    if ch.reneg {
        b.u16(65281)
        b.u16(1)
        b.u8(0)
    }
    if ch.pointFormats {
        b.u16(11)
        b.u16(2)
        b.u8(1)
        b.u8(0)
    }
    if ticketExt {
        b.u16(35)
        b.u16(0)
    }
    if c.alpn != "" {
        b.u16(16)
        var a = b.open(2)
        var al = b.open(2)
        b.vec8(bytesOf(c.alpn))
        b.close(al, 2)
        b.close(a, 2)
    }
    b.close(ext, 2)
    return b.bytes()
}
