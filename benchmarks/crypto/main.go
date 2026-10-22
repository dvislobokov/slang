package main

import (
	"crypto"
	"crypto/aes"
	"crypto/cipher"
	"crypto/ecdh"
	"crypto/ecdsa"
	"crypto/ed25519"
	"crypto/elliptic"
	"crypto/hkdf"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/sha512"
	"crypto/tls"
	"crypto/x509"
	"encoding/hex"
	"fmt"
	"hash"
	"math/big"
	"net"
	"os"
	"strings"
	"time"

	"golang.org/x/crypto/argon2"
	"golang.org/x/crypto/chacha20poly1305"
)

// crypto: SHA-256 / SHA-512 over 1 GB (1 MB chunks), AES-256-GCM and
// ChaCha20-Poly1305 sealing 1 GB (1 MB messages), X25519 shared secrets,
// Ed25519 sign/verify, RSA-2048 PKCS#1 v1.5 sign/verify, P-256 ECDSA
// sign/verify, HKDF-SHA256, Argon2id (t=1, 64 MiB, 4 lanes) and TLS 1.3
// handshakes on loopback. Same inputs and checksums as main.zl.
// SCALE=small runs 16 MB streams and 1/20 of the operations.
// Timings and rates go to stderr.

const rsaKeyHex = `308204a30201000282010100c43a37f68042b05827ca872f5b9e9d0ee255dc6d8b928e1471695383e9e9ad0172c67b4c
ed358c5caa1f2bb64a4d5750606e178b56df76bc2a7efd16325efbd20ff34dc573fa4cd5b181487e00e84f53c869db1c
edb865df92357564c6f740e57770ba58bae73909ab712a4f5e6c362db26411be398e2597bcd5568ebc561ff4f22b2394
be49a458e1c1a0c5ada8ec87b145e91e60e1f3fb58657eb666baee940224af045c3ffa2087da6a645a27d6a8c378d99f
970e93f599edb118960acd22655fa4d6c47e194e765f8b72936a8c57a830839ec9076dbc8f9354682a9b6229b20a7d66
a03c080c05e1e372df2d4ba562d1ab494b0ca0a8b5bcb1fefa62b941020301000102820100120d9f27751e6d95f6b739
dfc008a43f81d04f72dea3f1a16cdbb95cca10ff234279834f8d055fb008d0cc573d97c7003496fc608bf20c3cfc95e9
57f8f3672ca8de041a35f4e1cbc94471347285ed0272ce5663df75171e4e17d000a1138f6b6d253867c06e547f90bfdd
1a2740426dcadb4d0c98c3fcae6ca79e0a8cc649b338876d7263daa26abe54cf7de229a5b86f95b29885b2022f50e0e1
12d5e79d4f90bb7a1492a52383812068d41b336aa3452157c16e8795c5239026698312f0d2f149ff3a5c4f46c9477184
deeb89922b2cb61db7b93874593598dcbfb012d7a89682beb119e7ef1ce64a22182481f1ce91d368f50c846fad4fa9f7
7a2423a60902818100cc4c8e337f9295c9eaabdc2b1cbbe9fc6346f4c2fd2c0b56c414d9252b3d5fd99b28a6b642bd15
c0acd505a3283da6292aaba29bbef8d949653cb2912575d6a17e4ae0593b88687c3499d7e3615f5f93e635c14cb76715
613177db8f8bfc2aeff07d9027270bb418767682ca3cda00f734208f9aba9075398bb62432d88a82ed02818100f5e2be
d397b4b05ee9db6ac69c11d8de41258ff4b6c1c7e4e1121648b1a7283a22953ff1ae309f85d9742b0dbecf78131594a5
0306782d26a3eb39f1291b225e15163b4b2da5edefe76e2eab2ddca749acd2b96946548e09b92d2efd0e5941a6e87dd3
09662a8818c75b3a2e1e10b450a9c73778eb7e84906a5bd88a3b8a612502818028c241408efc1cbc3b006c9d814f1040
9541ebc517237963a71d0db177adf7f87f171e2390af0eaef10c52e2993dab6fdd1e7f039997262a997301a1299b17e6
023f4801257a72760d3c8030d215680926cda4b1fc89295fc9b1c2f121997eab7e0c5efd20b21a0d235d437cbba536ba
1b0140053ba16471c5a8e211eed7bb8502818100ae7ce0e430b4e5b0050e53006c02723e8e468498a9146cb0ec533483
13a6c67232d66d11298691bd9f69c39fb6776e0344fe3df71ea12b914f237ef3b732ed398c87413e813f4dd1400f3d88
f63bae6f26f846e1012ac1bcfeaea4b8a9750794d5496cf82f8b21174c695da91a66ca3eafac929df9a7c794e0151b5e
4510570d02818006527f4ea6c52bdf286b0d0cd275470cb984c8feca3f01c153a1b2be8001b31ce9a6023cb943ff3fdd
71fca0dedfe8f4060d5ad3c9549bfea465c2b6fb71aa1ec8111a8b163dde9cda0d346e266d8b8ce9f33040453e3b0d3b
277713835af46f33c868a23fd9e453cb7dae7129e6f3c3e799cd67af4000550d71bdc09ea62509`

// The self-signed P-256 test certificate of examples/tls_echo.zl (localhost).
const serverCert = `-----BEGIN CERTIFICATE-----
MIIBtjCCAV2gAwIBAgIBATAKBggqhkjOPQQDAjArMRIwEAYDVQQDDAlsb2NhbGhv
c3QxFTATBgNVBAoMDFplbGwgZXhhbXBsZTAgFw0wMDAxMDEwMDAwMDBaGA8yMTAw
MDEwMTAwMDAwMFowKzESMBAGA1UEAwwJbG9jYWxob3N0MRUwEwYDVQQKDAxaZWxs
IGV4YW1wbGUwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAASc1zlpmSuVZO3wBCod
f/2ckVINRHTUmOSXBzpVELQe6p45BK0zK1pyWibOTfHSGzqN94wNd2hMhWaKQIs0
wMaQo3AwbjAaBgNVHREEEzARgglsb2NhbGhvc3SHBH8AAAEwEwYDVR0lBAwwCgYI
KwYBBQUHAwEwDAYDVR0TAQH/BAIwADAOBgNVHQ8BAf8EBAMCB4AwHQYDVR0OBBYE
FO3Pu7WkrsFL3x1cVZbVkCe31m0RMAoGCCqGSM49BAMCA0cAMEQCIGpTh2h8NI9Z
GuHmqgmIvc/Q23bpqOkAIp/C00RTYPGZAiA1yB4ABbqgvV4v17SkrLUxptZkTCTc
Qt9q+e7qPOGNFQ==
-----END CERTIFICATE-----`

const serverKey = `-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgjA2gD248y45QVG96
fXrFkd1iVNl1OOz2VN/OkZMN5jehRANCAASc1zlpmSuVZO3wBCodf/2ckVINRHTU
mOSXBzpVELQe6p45BK0zK1pyWibOTfHSGzqN94wNd2hMhWaKQIs0wMaQ
-----END PRIVATE KEY-----`

func seq(n, base int) []byte {
	out := make([]byte, n)
	for i := range out {
		out[i] = byte((base + i) & 255)
	}
	return out
}

func chunk() []byte {
	out := make([]byte, 1048576)
	s := 12345
	for i := range out {
		s = (s*1103515245 + 12345) % 2147483648
		out[i] = byte((s >> 16) & 255)
	}
	return out
}

func xorInto(acc, x []byte) {
	for i := range acc {
		acc[i] ^= x[i]
	}
}

func sum256(b []byte) string {
	d := sha256.Sum256(b)
	return hex.EncodeToString(d[:])
}

func report(name string, d time.Duration, n int, unit string) {
	ns := d.Nanoseconds()
	if ns <= 0 {
		ns = 1
	}
	if unit == "MB" {
		fmt.Fprintf(os.Stderr, "%s: %d ms, %d MB/s\n", name, ns/1000000, int64(n)*1000/ns)
	} else {
		fmt.Fprintf(os.Stderr, "%s: %d ms, %d ops/s\n", name, ns/1000000, int64(n)*1000000000/ns)
	}
}

func must(err error) {
	if err != nil {
		panic(err)
	}
}

func main() {
	mb, div := 1024, 1
	if os.Getenv("SCALE") == "small" {
		mb, div = 16, 20
	}
	buf := chunk()

	// SHA-256 / SHA-512: digest of each 1 MB chunk (chunk i starts with i).
	for _, alg := range []string{"sha256", "sha512"} {
		var h hash.Hash
		if alg == "sha256" {
			h = sha256.New()
		} else {
			h = sha512.New()
		}
		var digests []byte
		t0 := time.Now()
		for i := 0; i < mb; i++ {
			buf[0] = byte(i & 255)
			buf[1] = byte(i >> 8)
			h.Reset()
			h.Write(buf)
			digests = h.Sum(digests)
		}
		d := time.Since(t0)
		fmt.Println(alg, sum256(digests))
		report(alg, d, mb*1048576, "MB")
	}

	// AEAD sealing: 1 MB messages, nonce = counter.
	key := seq(32, 7)
	for _, alg := range []string{"aes-256-gcm", "chacha20-poly1305"} {
		var aead cipher.AEAD
		if alg == "aes-256-gcm" {
			blk, err := aes.NewCipher(key)
			must(err)
			aead, err = cipher.NewGCM(blk)
			must(err)
		} else {
			var err error
			aead, err = chacha20poly1305.New(key)
			must(err)
		}
		var tags []byte
		nonce := seq(12, 0)
		dst := make([]byte, 0, len(buf)+16)
		t0 := time.Now()
		for i := 0; i < mb; i++ {
			nonce[8] = byte(i & 255)
			nonce[9] = byte(i >> 8)
			ct := aead.Seal(dst[:0], nonce, buf, nil)
			tags = append(tags, ct[len(ct)-16:]...)
		}
		d := time.Since(t0)
		fmt.Println(alg, sum256(tags))
		report(alg, d, mb*1048576, "MB")
	}

	// X25519 shared secrets.
	nx := 20000 / div
	peerPriv, err := ecdh.X25519().NewPrivateKey(seq(32, 99))
	must(err)
	peer := peerPriv.PublicKey()
	priv := seq(32, 1)
	acc := seq(32, 0)
	t0 := time.Now()
	for i := 0; i < nx; i++ {
		priv[2] = byte(i & 255)
		priv[1] = byte(i >> 8)
		k, err := ecdh.X25519().NewPrivateKey(priv)
		must(err)
		s, err := k.ECDH(peer)
		must(err)
		xorInto(acc, s)
	}
	report("x25519", time.Since(t0), nx, "ops")
	fmt.Println("x25519", nx, hex.EncodeToString(acc))

	// Ed25519.
	ne := 20000 / div
	edKey := ed25519.NewKeyFromSeed(seq(32, 42))
	pub := edKey.Public().(ed25519.PublicKey)
	msg := seq(64, 3)
	sigs := make([][]byte, 0, ne)
	t0 = time.Now()
	for i := 0; i < ne; i++ {
		msg[0] = byte(i & 255)
		msg[1] = byte(i >> 8)
		sigs = append(sigs, ed25519.Sign(edKey, msg))
	}
	dSign := time.Since(t0)
	t1 := time.Now()
	ok := 0
	for i := 0; i < ne; i++ {
		msg[0] = byte(i & 255)
		msg[1] = byte(i >> 8)
		if ed25519.Verify(pub, msg, sigs[i]) {
			ok++
		}
	}
	dVerify := time.Since(t1)
	var all []byte
	for _, s := range sigs {
		all = append(all, s...)
	}
	fmt.Println("ed25519", ne, ok, sum256(all))
	report("ed25519 sign", dSign, ne, "ops")
	report("ed25519 verify", dVerify, ne, "ops")

	// RSA-2048 PKCS#1 v1.5 with SHA-256.
	der, err := hex.DecodeString(strings.ReplaceAll(rsaKeyHex, "\n", ""))
	must(err)
	rkey, err := x509.ParsePKCS1PrivateKey(der)
	must(err)
	nrs, nrv := 500/div, 5000/div
	dg := sha256.Sum256(msg)
	digest := dg[:]
	var rsig, rall []byte
	t0 = time.Now()
	for i := 0; i < nrs; i++ {
		digest[0] = byte(i & 255)
		rsig, err = rsa.SignPKCS1v15(nil, rkey, crypto.SHA256, digest)
		must(err)
		rall = append(rall, rsig[0:8]...)
	}
	dSign = time.Since(t0)
	t1 = time.Now()
	rok := 0
	for i := 0; i < nrv; i++ {
		if rsa.VerifyPKCS1v15(&rkey.PublicKey, crypto.SHA256, digest, rsig) == nil {
			rok++
		}
	}
	dVerify = time.Since(t1)
	fmt.Println("rsa2048", nrs, rok, sum256(rall))
	report("rsa2048 sign", dSign, nrs, "ops")
	report("rsa2048 verify", dVerify, nrv, "ops")

	// P-256 ECDSA (randomized signatures: count the verified ones).
	np := 10000 / div
	d := seq(32, 1)
	ek, err := ecdh.P256().NewPrivateKey(d)
	must(err)
	epub := ek.PublicKey().Bytes()
	x, y := elliptic.Unmarshal(elliptic.P256(), epub)
	epriv := &ecdsa.PrivateKey{PublicKey: ecdsa.PublicKey{Curve: elliptic.P256(), X: x, Y: y}, D: new(big.Int).SetBytes(d)}
	esigs := make([][]byte, 0, np)
	t0 = time.Now()
	for i := 0; i < np; i++ {
		digest[0] = byte(i & 255)
		digest[1] = byte(i >> 8)
		s, err := ecdsa.SignASN1(rand.Reader, epriv, digest)
		must(err)
		esigs = append(esigs, s)
	}
	dSign = time.Since(t0)
	t1 = time.Now()
	eok := 0
	for i := 0; i < np; i++ {
		digest[0] = byte(i & 255)
		digest[1] = byte(i >> 8)
		if ecdsa.VerifyASN1(&epriv.PublicKey, digest, esigs[i]) {
			eok++
		}
	}
	dVerify = time.Since(t1)
	fmt.Println("p256", np, eok, hex.EncodeToString(epub[0:8]))
	report("p256 sign", dSign, np, "ops")
	report("p256 verify", dVerify, np, "ops")

	// HKDF-SHA256, 32-byte keys.
	nh := 200000 / div
	secret, salt, info := seq(32, 5), seq(16, 9), seq(16, 11)
	hacc := seq(32, 0)
	t0 = time.Now()
	for i := 0; i < nh; i++ {
		info[0] = byte(i & 255)
		info[1] = byte(i >> 8)
		info[2] = byte(i >> 16)
		k, err := hkdf.Key(sha256.New, secret, salt, string(info), 32)
		must(err)
		xorInto(hacc, k)
	}
	report("hkdf", time.Since(t0), nh, "ops")
	fmt.Println("hkdf", nh, hex.EncodeToString(hacc))

	// Argon2id, t=1, 64 MiB, 4 lanes.
	na := 8
	if div > 1 {
		na = 1
	}
	aacc := seq(32, 0)
	pw := seq(16, 33)
	t0 = time.Now()
	for i := 0; i < na; i++ {
		pw[0] = byte(i)
		xorInto(aacc, argon2.IDKey(pw, salt, 1, 65536, 4, 32))
	}
	report("argon2id", time.Since(t0), na, "ops")
	fmt.Println("argon2id", na, hex.EncodeToString(aacc))

	// TLS 1.3 full handshakes on loopback (no session resumption), one
	// byte echoed per connection. X25519 only: Go 1.24 would otherwise
	// offer the hybrid X25519MLKEM768, which Zell does not implement.
	nt := 1000 / div
	cert, err := tls.X509KeyPair([]byte(serverCert), []byte(serverKey))
	must(err)
	l, err := tls.Listen("tcp", "127.0.0.1:0", &tls.Config{
		Certificates: []tls.Certificate{cert}, MinVersion: tls.VersionTLS13,
		SessionTicketsDisabled: true,
	})
	must(err)
	go func() {
		for {
			c, err := l.Accept()
			if err != nil {
				return
			}
			go func(c net.Conn) {
				b := make([]byte, 65536)
				n, _ := c.Read(b)
				if n > 0 {
					c.Write(b[:n])
				}
				c.Close()
			}(c)
		}
	}()
	ccfg := &tls.Config{InsecureSkipVerify: true, MinVersion: tls.VersionTLS13,
		CurvePreferences: []tls.CurveID{tls.X25519}}
	tok := 0
	t0 = time.Now()
	for i := 0; i < nt; i++ {
		c, err := tls.Dial("tcp", l.Addr().String(), ccfg)
		must(err)
		_, err = c.Write([]byte("x"))
		must(err)
		b := make([]byte, 16)
		n, _ := c.Read(b)
		if n == 1 && c.ConnectionState().Version == tls.VersionTLS13 {
			tok++
		}
		c.Close()
	}
	dTLS := time.Since(t0)
	l.Close()
	fmt.Println("tls13", nt, tok)
	report("tls13 handshake", dTLS, nt, "ops")
}
