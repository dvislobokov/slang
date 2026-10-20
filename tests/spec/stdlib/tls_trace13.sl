// test: a complete TLS 1.3 handshake trace (TLS_AES_256_GCM_SHA384, X25519, RSA-PSS; "The Illustrated TLS 1.3 Connection") is reproduced byte for byte: secrets, record decryption, CertificateVerify, both Finished messages, application data
// status: passing
// run-flags: --leak-check
namespace spec.stdlib.tls_trace13

import std.io
import std
import std.net
import std.crypto
import std.encoding

// The records of the connection documented at https://tls13.xargs.org
// (github.com/syncsynchalt/illustrated-tls13, captures/caps): every
// random value of that connection is fixed (client key 20..3f, server key
// 90..af), so the trace is reproducible.
const clientHello = "16030100f8010000f40303000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20e0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff000813021303130100ff010000a30000001800160000136578616d706c652e756c666865696d2e6e6574000b000403000102000a00160014001d0017001e0019001801000101010201030104002300000016000000170000000d001e001c040305030603080708080809080a080b080408050806040105010601002b0003020304002d00020101003300260024001d0020358072d6365880d1aeea329adf9121383851ed21a28e3b75e965d0d2cd166254"
const serverHello = "160303007a020000760303707172737475767778797a7b7c7d7e7f808182838485868788898a8b8c8d8e8f20e0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff130200002e002b0002030400330024001d00209fd7ad6dcff4298dd3f96d5b1b2af910a0535b1488d7f8fabb349a982880b615"
const encExtensions = "17030300176be02f9da7c2dc9ddef56f2468b90adfa25101ab0344ae"
const encCertificate = "1703030343baf00a9be50f3f2307e726edcbdacbe4b18616449d46c6207af6e9953ee5d2411ba65d31feaf4f78764f2d693987186cc01329c187a5e4608e8d27b318e98dd94769f7739ce6768392caca8dcc597d77ec0d1272233785f6e69d6f43effa8e7905edfdc4037eee5933e990a7972f206913a31e8d04931366d3d8bcd6a4a4d647dd4bd80b0ff863ce3554833d744cf0e0b9c07cae726dd23f9953df1f1ce3aceb3b7230871e92310cfb2b098486f43538f8e82d8404e5c6c25f66a62ebe3c5f26232640e20a769175ef83483cd81e6cb16e78dfad4c1b714b04b45f6ac8d1065ad18c13451c9055c47da300f93536ea56f531986d6492775393c4ccb095467092a0ec0b43ed7a0687cb470ce350917b0ac30c6e5c24725a78c45f9f5f29b6626867f6f79ce054273547b36df030bd24af10d632dba54fc4e890bd0586928c0206ca2e28e44e227a2d5063195935df38da8936092eef01e84cad2e49d62e470a6c7745f625ec39e4fc23329c79d1172876807c36d736ba42bb69b004ff55f93850dc33c1f98abb92858324c76ff1eb085db3c1fc50f74ec04442e622973ea70743418794c388140bb492d6294a0540e5a59cfae60ba0f14899fca71333315ea083a68e1d7c1e4cdc2f56bcd6119681a4adbc1bbf42afd806c3cbd42a076f545dee4e118d0b396754be2b042a685dd4727e89c0386a94d3cd6ecb9820e9d49afeed66c47e6fc243eabebbcb0b02453877f5ac5dbfbdf8db1052a3c994b224cd9aaaf56b026bb9efa2e01302b36401ab6494e7018d6e5b573bd38bcef023b1fc92946bbca0209ca5fa926b4970b1009103645cb1fcfe552311ff730558984370038fd2cce2a91fc74d6f3e3ea9f843eed356f6f82d35d03bc24b81b58ceb1a43ec9437e6f1e50eb6f555e321fd67c8332eb1b832aa8d795a27d479c6e27d5a61034683891903f66421d094e1b00a9a138d861e6f78a20ad3e1580054d2e305253c713a02fe1e28deee7336246f6ae34331806b46b47b833c39b9d31cd300c2a6ed831399776d07f570eaf0059a2c68a5f3ae16b617404af7b7231a4d942758fc020b3f23ee8c15e36044cfd67cd640993b16207597fbf385ea7a4d99e8d456ff83d41f7b8b4f069b028a2a63a919a70e3a10e3084158faa5bafa30186c6b2f238eb530c73e"
const encCertVerify = "170303011973719fce07ec2f6d3bba0292a0d40b2770c06a271799a53314f6f77fc95c5fe7b9a4329fd9548c670ebeea2f2d5c351dd9356ef2dcd52eb137bd3a676522f8cd0fb7560789ad7b0e3caba2e37e6b4199c6793b3346ed46cf740a9fa1fec414dc715c415c60e575703ce6a34b70b5191aa6a61a18faff216c687ad8d17e12a7e99915a611bfc1a2befc15e6e94d784642e682fd17382a348c301056b940c9847200408bec56c81ea3d7217ab8e85a88715395899c90587f72e8ddd74b26d8edc1c7c837d9f2ebbc260962219038b05654a63a0b12999b4a8306a3ddcc0e17c53ba8f9c80363f7841354d291b4ace0c0f330c0fcd5aa9deef969ae8ab2d98da88ebb6ea80a3a11f00ea296a3232367ff075e1c66dd9cbedc4713"
const encServerFinished = "17030300451061de27e51c2c9f342911806f282b710c10632ca5006755880dbf7006002d0e84fed9adf27a43b5192303e4df5c285d58e3c76224078440c0742374744aecf28cf3182fd0"
const encClientFinished = "17030300459ff9b063175177322a46dd9896f3c3bb820ab51743ebc25fdadd53454b73deb54cc7248d411a18bccf657a960824e9a19364837c350a69a88d4bf635c85eb874aebc9dfde8"
const encClientData = "1703030015828139cb7b73aaabf5b82fbf9a2961bcde10038a32"
const encServerData = "17030300150cda85f1447ae23fa66d56f4c5408482b1b1d4c998"

func h(s string) []byte {
    return try hex.Decode(s) else []
}

func show(name string, b []byte) {
    println(name + ":", hex.Encode(b))
}

func join(a []byte, b []byte) []byte {
    var out []byte = []
    for x in a {
        out = append(out, x)
    }
    for x in b {
        out = append(out, x)
    }
    return out
}

// The handshake message inside a plaintext record.
func body(rec string) []byte {
    return h(rec)[5..]
}

func main() void! {
    var hh = "sha384"
    var clientPriv = h("202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f")
    var serverPriv = h("909192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeaf")
    var ch = body(clientHello)
    var sh = body(serverHello)
    // the server key share is the last 32 bytes of the ServerHello
    var serverShare = sh[len(sh) - 32..]
    println("server share matches:", hex.Encode(serverShare) == hex.Encode(x25519.PublicKey(serverPriv)?))
    var shared = x25519.SharedSecret(clientPriv, serverShare)?
    println("both sides agree:", hex.Encode(shared) == hex.Encode(x25519.SharedSecret(serverPriv, x25519.PublicKey(clientPriv)?)?))
    var empty = crypto.hash.Sum(hh, [])?
    var e0 = hkdf.Extract(hh, [], h("000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"))?
    var hs = hkdf.Extract(hh, tls.DeriveSecret(hh, e0, "derived", empty)?, shared)?
    var transcript = join(ch, sh)
    var thSH = crypto.hash.Sum(hh, transcript)?
    var sHS = tls.DeriveSecret(hh, hs, "s hs traffic", thSH)?
    var cHS = tls.DeriveSecret(hh, hs, "c hs traffic", thSH)?
    show("SERVER_HANDSHAKE_TRAFFIC_SECRET", sHS)
    show("CLIENT_HANDSHAKE_TRAFFIC_SECRET", cHS)
    var sKey = tls.ExpandLabel(hh, sHS, "key", [], 32)?
    var sIV = tls.ExpandLabel(hh, sHS, "iv", [], 12)?
    var cKey = tls.ExpandLabel(hh, cHS, "key", [], 32)?
    var cIV = tls.ExpandLabel(hh, cHS, "iv", [], 12)?
    show("server handshake key", sKey)
    show("server handshake iv", sIV)
    show("client handshake key", cKey)
    show("client handshake iv", cIV)

    // the server's encrypted flight, records 0..3
    var ee = tls.OpenRecord(0x1302, sKey, sIV, 0, h(encExtensions))?
    var cert = tls.OpenRecord(0x1302, sKey, sIV, 1, h(encCertificate))?
    var cv = tls.OpenRecord(0x1302, sKey, sIV, 2, h(encCertVerify))?
    var fin = tls.OpenRecord(0x1302, sKey, sIV, 3, h(encServerFinished))?
    println("inner types:", ee[0], cert[0], cv[0], fin[0], "messages:", ee[1], cert[1], cv[1], fin[1])
    show("EncryptedExtensions", ee[1..])
    transcript = join(transcript, ee[1..])
    // Certificate: type(1) len(3) context(1) list(3) cert_len(3) cert ...
    var certMsg = cert[1..]
    var certLen = int(certMsg[8]) * 65536 + int(certMsg[9]) * 256 + int(certMsg[10])
    var leaf = x509.ParseDER(certMsg[11..11 + certLen])?
    println("server certificate:", leaf.Subject.CommonName, leaf.PublicKeyAlgorithm, len(leaf.PublicKey) * 8, "bits")
    transcript = join(transcript, certMsg)
    // CertificateVerify over the transcript through Certificate
    var cvMsg = cv[1..]
    var scheme = int(cvMsg[4]) * 256 + int(cvMsg[5])
    var sig = cvMsg[8..]
    var content = strings.Bytes(strings.Repeat(" ", 64) + "TLS 1.3, server CertificateVerify")
    content = join(content, h("00"))
    content = join(content, crypto.hash.Sum(hh, transcript)?)
    println("CertificateVerify scheme:", fmt.Sprintf("0x%04x", scheme), "signature bytes:", len(sig))
    leaf.CheckSignature("SHA256-RSAPSS", content, sig)?
    println("CertificateVerify signature: ok")
    transcript = join(transcript, cvMsg)
    var sfk = tls.ExpandLabel(hh, sHS, "finished", [], 48)?
    var want = hmac.Sum(hh, sfk, crypto.hash.Sum(hh, transcript)?)?
    println("server Finished verifies:", hex.Encode(want) == hex.Encode(fin[5..]))
    transcript = join(transcript, fin[1..])

    // application secrets
    var hs2 = tls.DeriveSecret(hh, hs, "derived", empty)?
    var master = hkdf.Extract(hh, hs2, h("000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"))?
    var thSF = crypto.hash.Sum(hh, transcript)?
    var sAP = tls.DeriveSecret(hh, master, "s ap traffic", thSF)?
    var cAP = tls.DeriveSecret(hh, master, "c ap traffic", thSF)?
    show("SERVER_TRAFFIC_SECRET_0", sAP)
    show("CLIENT_TRAFFIC_SECRET_0", cAP)
    show("EXPORTER_SECRET", tls.DeriveSecret(hh, master, "exp master", thSF)?)

    // the client Finished, sealed again byte for byte
    var cfk = tls.ExpandLabel(hh, cHS, "finished", [], 48)?
    var cfin = join(h("14000030"), hmac.Sum(hh, cfk, thSF)?)
    var sealed = tls.SealRecord(0x1302, cKey, cIV, 0, 22, cfin)?
    println("client Finished record identical:", hex.Encode(sealed) == encClientFinished)

    // application data both ways
    var caKey = tls.ExpandLabel(hh, cAP, "key", [], 32)?
    var caIV = tls.ExpandLabel(hh, cAP, "iv", [], 12)?
    var saKey = tls.ExpandLabel(hh, sAP, "key", [], 32)?
    var saIV = tls.ExpandLabel(hh, sAP, "iv", [], 12)?
    var ping = tls.OpenRecord(0x1302, caKey, caIV, 0, h(encClientData))?
    // the server's data record follows its two NewSessionTicket records
    var pong = tls.OpenRecord(0x1302, saKey, saIV, 2, h(encServerData))?
    println("client sent:", strings.FromBytes(ping[1..]), "server sent:", strings.FromBytes(pong[1..]))
    println("client data record identical:", hex.Encode(tls.SealRecord(0x1302, caKey, caIV, 0, 23, ping[1..])?) == encClientData)
}
