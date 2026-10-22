// test: P-384 ECDSA (RFC 6979 A.2.6 key and deterministic signature, DER sign/verify) and ECDH (OpenSSL-computed vector, invalid point); AES-CBC (SP 800-38A F.2.1/F.2.5), PKCS#7 padding, DecryptBlock; RSA OAEP (decrypts a ciphertext made by Python cryptography, round trip, label mismatch), GenerateKey 2048, PublicKeyOf, IsProbablePrime
// status: passing
// run-flags: --leak-check
namespace spec.stdlib4.crypto_pk

import std
import std.encoding
import std.io

const KeyDER = "308204a30201000282010100a17859bbaaed529ee280d7dc67b4b69c1e8c6fab69eec490c36f6741bf38fefde47caed74a7c4c37d5c918716aaa8434aa55a9f471cacbba981f00405eaa6573d936c916f1eefe77dd630052d890b7b18efabaaceb61866eee8c9464ac3e10ceb846e4bb7f556b45eab3c7429a7e9b2868d7f184ef491615c9b12ab91628cfc7107e40c25e19633b49e90621c6b711e04b6a4e43df1892f678d3417ac7cce07263703a4251baf81b5df0e6aeb0caf92a69106bc4d62c933585ca4a44225aa9d91c7ba13106f25c7595c8ed8709dcec8a008d1251c7e86e8de98698f5da841a71e4a6c21ae8ca35bbb0d97e3308735a351c5d2023c5446fa8443503c070b4b3b7020301000102820101009663f1afc953a1503ac6a646b290ae445a6f92d2a4a7ae8ce67a2c42204885b713c8cd4fd8204d1cf41188a29a26975c3609f371a94aa8d5f2396d9286994e44c85b2e902065e8bcdb860a171f7e2a994602613c0d3f986659e8b58dce9f4028499a3e102458fac0f408ddde3ae710bc626a3e990205a07c4a8ea8ac1ae7881cf593616d968c0ed81b419d9e644513463a05c2f17d2ae58a6bdebc2e1c5f251d26dff5f92793796cc16ed0c4d508408763917af1285b32f57123c28717288a4c42e95dda87ac0413b39f4857a5a9aea3124693dc2081d9560df8f117cc509367838aa6b4b43898d35dac962d112b7574af11b6d49844ed21a92ecaa68af44e0102818100cc4f7ab2433908cd00847e12baa2d6de446d4ad1ac7c5d1709944bac74b74bf526b199034abd247828d1dadb61548b2278282953fc639d1645ee59cacc7b00092e1dfa66a0d6760db4c5fae665c7b6e994d337e43c9824ae8717dcd0ccd08f8bfc8f8aef0976d6dd3e19714e688353faa0815b363492da73a00d8381f1223fb102818100ca523df90e1614a02b5b4d1321f0b37a7d4f06d6845b82e2a64caec1ac57f06fc42bf46a9679177a6e1c5a523ef7a5dcd7e85538000c9f160a00e5e4a915683b60265ae15339b63a03d3636644eca9314b23eb0ee8ee7a3899ff9d8adc472be9a2a1232d5eb31296d24e4b31178c257f83aec8feae2de72db3fd247dc0e2abe7028180588229ee25cf047002c462f56c22f5206ed645ee5561bed6271136432c80e787f94e40b23b637af40f02f263cd6359af020cabf2b548d6197b0c099c203f3cc1bd57c14d4a69fdd46c36c43da164066a644da84dd8ae345de32a443fb42ab9b4f4219f1774ab25e3cc6e2b36c371f9247ef9eaf0ad8fdcd0f04a4a69357417d1028180290fd9fe132ace12d7d9d6e1bc30b2ec784432ba14f0d1b69853d5dffecf763fe8cb96f68f05928b498fbd9115b0835c3801b25204488b7d1898a80fe9752bcb692f3fa09d9887f06c5859c326d7f55ac8ca4869a542c9517587d87c974a66922b6c92cc217d2c97a9a534438968eb2f5f15793b9f3437c821d08db86c0e551f0281807efca688bf7a36e698ce7096b8a8faeb6bf2a4a0b2bd059eaf53990f9bb7a4817e0e7809d9bdad1a82af98b262c0bed0ef93d939c7eb9ba51a3f75f37a7e6e3ee9d88998b3fdd08eb824cd1eae5da4ff7971298094f3a31a27559ef97eb22d421d871e2d56023d4a088da7abbb882e6461b49d491fbe1f38409d936beb249b45"
const PyCiphertext = "3c17076c0c69287d50724076bcb3105018d41ebf69e5463cc5b3e3a1c70cbc4118d968a07c54a3c16a864d5c8d7469c14ba7baa3393e70cf9f6c5495f9b099978a4d7a5ddede610cccbd63d2c5e597f0f15e5c5e54d4034a100be725c7c5d48d78add68cf7d66b13fc40c600be09cc73f7003d228116f11bc9bd89898329aeda025c00cc3095c5998de9e8b06de4762b9783703c16253bbcdbd86074a752e83e8f1b5089a34e557795120366ec05dfd26f9a3e85904b11a4662707b0f537c0beaa16eb578c07a09aa118821e758788580961b1ab09f5d97a5f697d30a807c8b95f6e14d898e3d3873aa0102480fe251ffd3234f37452bb468b49511ed605a8d4"

func main() void! {
    var priv = hex.Decode(
        "6b9d3dad2e1b8c1c05b19875b6659f4de23c3b667bf297ba9aa47740787137d896d5724e4c70a825f872c9ea60d2edf5",
    )?
    var pub = crypto.ecdsa.PublicKeyP384(priv)?
    println(hex.Encode(pub))
    var digest = crypto.hash.Sha384("sample")
    println(hex.Encode(crypto.ecdsa.SignP384Raw(priv, digest)?))
    var sig = crypto.ecdsa.SignP384(priv, digest)?
    println(
        crypto.ecdsa.VerifyP384(pub, digest, sig),
        crypto.ecdsa.VerifyP384(pub, crypto.hash.Sha384("test"), sig),
    )
    var k1 = crypto.ecdsa.GenerateKeyP384()?
    var p1 = crypto.ecdsa.PublicKeyP384(k1)?
    println(
        len(k1),
        len(p1),
        crypto.ecdsa.VerifyP384(p1, digest, crypto.ecdsa.SignP384(k1, digest)?),
    )
    var a = hex.Decode(
        "543f88e5b664c8cfae8f912341a7fbceefc525f32d4bc5cad29f68484096c21e8323c517094ddf365996b97a73eca1b1",
    )?
    var bpub = hex.Decode(
        "04cea60335e06f588eb2d17f995a05cc9207f7b6aa42649900dc0edacb0f4806297ea6a4c64d414f9fda13fa8e70c2e71d8ccd244e4e093028db0a5eea7dad6164f3d13dd6409a16fd8393365f18d92689103b8d0f88d298ac9224436c3c3ff139",
    )?
    println(hex.Encode(crypto.ecdh.SharedSecretP384(a, bpub)?))
    var x = crypto.ecdh.GenerateKeyP384()?
    var y = crypto.ecdh.GenerateKeyP384()?
    println(
        crypto.hmac.Equal(
            crypto.ecdh.SharedSecretP384(x, crypto.ecdh.PublicKeyP384(y)?)?,
            crypto.ecdh.SharedSecretP384(y, crypto.ecdh.PublicKeyP384(x)?)?,
        ),
    )
    bpub[50] = bpub[50] ^ 1
    try crypto.ecdh.SharedSecretP384(a, bpub) catch err {
        println(err)
    }

    // AES-CBC
    var key = hex.Decode("2b7e151628aed2a6abf7158809cf4f3c")?
    var iv = hex.Decode("000102030405060708090a0b0c0d0e0f")?
    var pt = hex.Decode("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51")?
    var ct = crypto.aes.CbcEncryptRaw(key, iv, pt)?
    println(hex.Encode(ct))
    println(hex.Encode(crypto.aes.CbcDecryptRaw(key, iv, ct)?) == hex.Encode(pt))
    var key256 = hex.Decode("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4")?
    println(hex.Encode(crypto.aes.CbcEncryptRaw(key256, iv, pt)?))
    var blk = crypto.aes.EncryptBlock(key256, pt[..16])?
    println(hex.Encode(crypto.aes.DecryptBlock(key256, blk)?) == hex.Encode(pt[..16]))
    var sealed = crypto.aes.CbcEncrypt(key, iv, strings.Bytes("legacy secret"))?
    println(len(sealed), strings.FromBytes(crypto.aes.CbcDecrypt(key, iv, sealed)?))
    sealed[15] = sealed[15] ^ 7
    try crypto.aes.CbcDecrypt(key, iv, sealed) catch err {
        println(err)
    }
    try crypto.aes.CbcEncryptRaw(key, iv, strings.Bytes("odd")) catch err {
        println(err)
    }
    println(
        hex.Encode(crypto.aes.Pkcs7Pad(strings.Bytes("abc"), 8)?),
        strings.FromBytes(crypto.aes.Pkcs7Unpad(crypto.aes.Pkcs7Pad(strings.Bytes("abc"), 8)?, 8)?),
    )
    try crypto.aes.Pkcs7Unpad(hex.Decode("6162630505050504")?, 8) catch err {
        println(err)
    }

    // RSA
    var der = hex.Decode(KeyDER)?
    var pk = crypto.rsa.PublicKeyOf(der)?
    println("modulus bytes", pk.Size(), hex.Encode(pk.E))
    println(strings.FromBytes(crypto.rsa.DecryptOAEP(der, "sha256", hex.Decode(PyCiphertext)?, [

    ])?))
    var enc = crypto.rsa.EncryptOAEP(
        pk.N,
        pk.E,
        "sha384",
        strings.Bytes("round trip"),
        strings.Bytes("label"),
    )?
    println(
        len(enc),
        strings.FromBytes(crypto.rsa.DecryptOAEP(der, "sha384", enc, strings.Bytes("label"))?),
    )
    try crypto.rsa.DecryptOAEP(der, "sha384", enc, strings.Bytes("other")) catch err {
        println(err)
    }
    var big []byte = []
    for _ in 0..300 {
        big = append(big, 1)
    }
    try crypto.rsa.EncryptOAEP(pk.N, pk.E, "sha256", big, []) catch err {
        println(err)
    }
    var gen = crypto.rsa.GenerateKey(2048)?
    var gp = crypto.rsa.PublicKeyOf(gen)?
    var m = crypto.hash.Sha256("message")
    var s = crypto.rsa.SignPSS(gen, "sha256", m)?
    println("generated", len(gp.N), crypto.rsa.VerifyPSS(gp.N, gp.E, "sha256", m, s))
    var parsed = crypto.x509.ParsePrivateKey(gen)?
    println(parsed.Algorithm)
    println(
        crypto.rsa.IsProbablePrime(hex.Decode("7fffffffffffffffffffffffffffffff")?, 20),
        crypto.rsa.IsProbablePrime(hex.Decode("0231")?, 20),
    )
    try crypto.rsa.GenerateKey(512) catch err {
        println(err)
    }
}
