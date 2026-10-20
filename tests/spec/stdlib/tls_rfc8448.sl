// test: the TLS 1.3 key schedule and record protection reproduce RFC 8448 (simple 1-RTT and resumed handshakes) byte for byte
// status: passing
// run-flags: --leak-check
namespace spec.stdlib.tls_rfc8448

import std.io
import std
import std.net
import std.crypto
import std.encoding

func h(s string) []byte {
    return try hex.Decode(s) else []
}

func show(name string, b []byte) {
    println(name + ":", hex.Encode(b))
}

func main() void! {
    // RFC 8448 section 3: simple 1-RTT handshake (TLS_AES_128_GCM_SHA256, x25519)
    var clientPriv = h("49af42ba7f7994852d713ef2784bcbcaa7911de26adc5642cb634540e7ea5005")
    var serverPriv = h("b1580eeadf6dd589b8ef4f2d5652578cc810e9980191ec8d058308cea216a21e")
    show("client share", x25519.PublicKey(clientPriv)?)
    show("server share", x25519.PublicKey(serverPriv)?)
    var ecdhe = x25519.SharedSecret(clientPriv, x25519.PublicKey(serverPriv)?)?
    show("ecdhe", ecdhe)
    var zeros = h("0000000000000000000000000000000000000000000000000000000000000000")
    var early = hkdf.Extract("sha256", [], zeros)?
    show("early secret", early)
    var empty = crypto.hash.Sum("sha256", [])?
    var derived = tls.DeriveSecret("sha256", early, "derived", empty)?
    show("derived", derived)
    var hs = hkdf.Extract("sha256", derived, ecdhe)?
    show("handshake secret", hs)
    var thSH = h("860c06edc07858ee8e78f0e7428c58edd6b43f2ca3e6e95f02ed063cf0e1cad8")
    var cHS = tls.DeriveSecret("sha256", hs, "c hs traffic", thSH)?
    var sHS = tls.DeriveSecret("sha256", hs, "s hs traffic", thSH)?
    show("c hs traffic", cHS)
    show("s hs traffic", sHS)
    show("s hs key", tls.ExpandLabel("sha256", sHS, "key", [], 16)?)
    show("s hs iv", tls.ExpandLabel("sha256", sHS, "iv", [], 12)?)
    show("c hs key", tls.ExpandLabel("sha256", cHS, "key", [], 16)?)
    show("c hs iv", tls.ExpandLabel("sha256", cHS, "iv", [], 12)?)
    show("c finished key", tls.ExpandLabel("sha256", cHS, "finished", [], 32)?)
    show("s finished key", tls.ExpandLabel("sha256", sHS, "finished", [], 32)?)
    var derived2 = tls.DeriveSecret("sha256", hs, "derived", empty)?
    var master = hkdf.Extract("sha256", derived2, zeros)?
    show("master secret", master)
    var thSF = h("9608102a0f1ccc6db6250b7b7e417b1a000eaada3daae4777a7686c9ff83df13")
    var cAP = tls.DeriveSecret("sha256", master, "c ap traffic", thSF)?
    var sAP = tls.DeriveSecret("sha256", master, "s ap traffic", thSF)?
    show("c ap traffic", cAP)
    show("s ap traffic", sAP)
    show("exp master", tls.DeriveSecret("sha256", master, "exp master", thSF)?)
    var thCF = h("209145a96ee8e2a122ff810047cc952684658d6049e86429426db87c54ad143d")
    var res = tls.DeriveSecret("sha256", master, "res master", thCF)?
    show("res master", res)
    var sKey = tls.ExpandLabel("sha256", sAP, "key", [], 16)?
    var sIV = tls.ExpandLabel("sha256", sAP, "iv", [], 12)?
    var cKey = tls.ExpandLabel("sha256", cAP, "key", [], 16)?
    var cIV = tls.ExpandLabel("sha256", cAP, "iv", [], 12)?
    show("s ap key", sKey)
    show("s ap iv", sIV)
    show("c ap key", cKey)
    show("c ap iv", cIV)
    // the application data records of both sides (50 bytes 00..31); the
    // server's is its second record (after the NewSessionTicket)
    var payload []byte = []
    for i in 0..50 {
        payload = append(payload, byte(i))
    }
    var crec = tls.SealRecord(0x1301, cKey, cIV, 0, 23, payload)?
    var srec = tls.SealRecord(0x1301, sKey, sIV, 1, 23, payload)?
    show("client record", crec)
    show("server record", srec)
    var back = tls.OpenRecord(0x1301, sKey, sIV, 1, srec)?
    println("opened:", back[0], len(back) - 1, back[len(back) - 1])
    try tls.OpenRecord(0x1301, sKey, sIV, 0, srec) catch err {
        println("wrong sequence:", err)
    }

    // RFC 8448 section 4: resumed handshake from the ticket (nonce 00 00)
    var psk = tls.ExpandLabel("sha256", res, "resumption", h("0000"), 32)?
    show("psk", psk)
    var early2 = hkdf.Extract("sha256", [], psk)?
    show("resumed early secret", early2)
    var binderKey = tls.DeriveSecret("sha256", early2, "res binder", empty)?
    show("binder key", binderKey)
    var fk = tls.ExpandLabel("sha256", binderKey, "finished", [], 32)?
    show("binder", hmac.Sum("sha256", fk, h("63224b2e4573f2d3454ca84b9d009a04f6be9e05711a8396473aefa01e924a14"))?)
    var thCH = h("08ad0fa05d7c7233b1775ba2ff9f4c5b8b59276b7f227f13a976245f5d960913")
    show("c e traffic", tls.DeriveSecret("sha256", early2, "c e traffic", thCH)?)
    show("e exp master", tls.DeriveSecret("sha256", early2, "e exp master", thCH)?)
    var hs2 = h("005cb112fd8eb4ccc623bb88a07c64b3ede1605363fc7d0df8c7ce4ff0fb4ae6")
    var thSH2 = h("f736cb34fe25e701551bee6fd24c1cc7102a7daf9405cb15d97aafe16f757d03")
    show("resumed c hs traffic", tls.DeriveSecret("sha256", hs2, "c hs traffic", thSH2)?)
    show("resumed s hs traffic", tls.DeriveSecret("sha256", hs2, "s hs traffic", thSH2)?)
    var master2 = h("e2d32d4ed66dd37897a0e80c84107503ce58bf8aad4cb55a5002d77ecb890ece")
    var thSF2 = h("b0aeffc46a2cfe33114e6fd7d51f9f04b1ca3c497dab08934a774a9d9ad7dbf3")
    show("resumed c ap traffic", tls.DeriveSecret("sha256", master2, "c ap traffic", thSF2)?)
    show("resumed s ap traffic", tls.DeriveSecret("sha256", master2, "s ap traffic", thSF2)?)
    show("resumed exp master", tls.DeriveSecret("sha256", master2, "exp master", thSF2)?)
    show("resumed res master", tls.DeriveSecret("sha256", master2, "res master", h("c3c122e0bd907a4a3ff6112d8fd53dbf89c773d9552e8b6b9d56d361b3a97bf6"))?)
}
