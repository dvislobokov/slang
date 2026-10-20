// Go side of the HTTP/2 interop tests (compiler/tests/net_http.rs), stdlib only:
//
//	go run main.go server            serves h2 over TLS (self-signed, ALPN) and h2c
//	                                 (prior knowledge); prints "tls <port>" and
//	                                 "h2c <port>" on stdout; stops on GET /quit
//	go run main.go client <h2c> <tls> requests a Slang server over HTTP/2 (h2c prior
//	                                 knowledge and TLS) and prints what it got
//	go run main.go wsclient <url>    WebSocket client (handshake and framing by hand,
//	                                 RFC 6455) against a Slang echo server
//
// The server also has /ws: a hand-written WebSocket echo endpoint (net/http
// Hijacker), for the Slang client.
package main

import (
	"bufio"
	"bytes"
	"compress/gzip"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha1"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/base64"
	"encoding/binary"
	"fmt"
	"io"
	"math/big"
	"net"
	"net/http"
	"os"
	"strings"
	"time"
)

func selfSigned() tls.Certificate {
	key, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	tmpl := &x509.Certificate{
		SerialNumber: big.NewInt(1),
		Subject:      pkix.Name{CommonName: "localhost"},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(24 * time.Hour),
		DNSNames:     []string{"localhost"},
		IPAddresses:  []net.IP{net.ParseIP("127.0.0.1")},
		KeyUsage:     x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, tmpl, &key.PublicKey, key)
	if err != nil {
		panic(err)
	}
	return tls.Certificate{Certificate: [][]byte{der}, PrivateKey: key}
}

func server() {
	quit := make(chan bool)
	mux := http.NewServeMux()
	mux.HandleFunc("/hello", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Proto", r.Proto)
		fmt.Fprintf(w, "hello from go via %s\n", r.Proto)
	})
	mux.HandleFunc("/big", func(w http.ResponseWriter, r *http.Request) {
		io.WriteString(w, strings.Repeat("0123456789", 50000))
	})
	mux.HandleFunc("/echo", func(w http.ResponseWriter, r *http.Request) {
		b, _ := io.ReadAll(r.Body)
		fmt.Fprintf(w, "%s %d bytes", r.Method, len(b))
	})
	mux.HandleFunc("/stream", func(w http.ResponseWriter, r *http.Request) {
		for i := 0; i < 3; i++ {
			fmt.Fprintf(w, "chunk %d\n", i)
			w.(http.Flusher).Flush()
			time.Sleep(10 * time.Millisecond)
		}
	})
	mux.HandleFunc("/gzip", func(w http.ResponseWriter, r *http.Request) {
		if !strings.Contains(r.Header.Get("Accept-Encoding"), "gzip") {
			io.WriteString(w, "no gzip asked")
			return
		}
		w.Header().Set("Content-Encoding", "gzip")
		var buf bytes.Buffer
		z := gzip.NewWriter(&buf)
		io.WriteString(z, strings.Repeat("gzip body ", 1000))
		z.Close()
		w.Write(buf.Bytes())
	})
	mux.HandleFunc("/ws", wsEcho)
	mux.HandleFunc("/quit", func(w http.ResponseWriter, r *http.Request) {
		io.WriteString(w, "bye")
		go func() { time.Sleep(50 * time.Millisecond); quit <- true }()
	})
	tl, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}
	hl, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}
	ts := &http.Server{Handler: mux, TLSConfig: &tls.Config{Certificates: []tls.Certificate{selfSigned()}}}
	var p http.Protocols
	p.SetHTTP1(true)
	p.SetUnencryptedHTTP2(true)
	hs := &http.Server{Handler: mux, Protocols: &p}
	go ts.ServeTLS(tl, "", "")
	go hs.Serve(hl)
	fmt.Printf("tls %d\nh2c %d\n", tl.Addr().(*net.TCPAddr).Port, hl.Addr().(*net.TCPAddr).Port)
	os.Stdout.Sync()
	<-quit
}

func get(c *http.Client, method, url, body string) {
	var rd io.Reader
	if body != "" {
		rd = strings.NewReader(body)
	}
	req, _ := http.NewRequest(method, url, rd)
	res, err := c.Do(req)
	if err != nil {
		fmt.Println("error:", err)
		return
	}
	b, _ := io.ReadAll(res.Body)
	res.Body.Close()
	s := string(b)
	if len(s) > 40 {
		s = fmt.Sprintf("%d bytes", len(b))
	}
	fmt.Printf("%s %s %d %s %q\n", method, res.Proto, res.StatusCode, res.Header.Get("Content-Type"), s)
}

func client(h2c, tlsURL string) {
	var p http.Protocols
	p.SetUnencryptedHTTP2(true)
	plain := &http.Client{Transport: &http.Transport{Protocols: &p}}
	secure := &http.Client{Transport: &http.Transport{ForceAttemptHTTP2: true, TLSClientConfig: &tls.Config{InsecureSkipVerify: true}}}
	for _, c := range []struct {
		cl   *http.Client
		base string
	}{{plain, h2c}, {secure, tlsURL}} {
		get(c.cl, "GET", c.base+"/hello", "")
		get(c.cl, "GET", c.base+"/big", "")
		get(c.cl, "POST", c.base+"/echo", strings.Repeat("x", 100000))
		get(c.cl, "GET", c.base+"/missing", "")
	}
}

// ---- WebSocket framing by hand ----

func acceptKey(k string) string {
	h := sha1.Sum([]byte(k + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"))
	return base64.StdEncoding.EncodeToString(h[:])
}

func readFrame(r *bufio.Reader) (fin bool, op byte, payload []byte, err error) {
	var h [2]byte
	if _, err = io.ReadFull(r, h[:]); err != nil {
		return
	}
	fin = h[0]&0x80 != 0
	op = h[0] & 15
	n := uint64(h[1] & 127)
	if n == 126 {
		var e [2]byte
		io.ReadFull(r, e[:])
		n = uint64(binary.BigEndian.Uint16(e[:]))
	} else if n == 127 {
		var e [8]byte
		io.ReadFull(r, e[:])
		n = binary.BigEndian.Uint64(e[:])
	}
	var mask [4]byte
	masked := h[1]&0x80 != 0
	if masked {
		io.ReadFull(r, mask[:])
	}
	payload = make([]byte, n)
	if _, err = io.ReadFull(r, payload); err != nil {
		return
	}
	if masked {
		for i := range payload {
			payload[i] ^= mask[i%4]
		}
	}
	return
}

func writeFrame(w io.Writer, op byte, payload []byte, mask bool) error {
	var h []byte
	h = append(h, 0x80|op)
	m := byte(0)
	if mask {
		m = 0x80
	}
	n := len(payload)
	switch {
	case n < 126:
		h = append(h, m|byte(n))
	case n < 65536:
		h = append(h, m|126, byte(n>>8), byte(n))
	default:
		h = append(h, m|127)
		h = binary.BigEndian.AppendUint64(h, uint64(n))
	}
	p := payload
	if mask {
		key := [4]byte{1, 2, 3, 4}
		h = append(h, key[:]...)
		p = make([]byte, n)
		for i := range payload {
			p[i] = payload[i] ^ key[i%4]
		}
	}
	_, err := w.Write(append(h, p...))
	return err
}

func wsEcho(w http.ResponseWriter, r *http.Request) {
	if r.Header.Get("Sec-WebSocket-Version") != "13" {
		http.Error(w, "not a websocket request", 400)
		return
	}
	c, rw, err := w.(http.Hijacker).Hijack()
	if err != nil {
		return
	}
	defer c.Close()
	fmt.Fprintf(rw, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", acceptKey(r.Header.Get("Sec-WebSocket-Key")))
	rw.Flush()
	var msg []byte
	var typ byte
	for {
		fin, op, p, err := readFrame(rw.Reader)
		if err != nil {
			return
		}
		switch op {
		case 8:
			writeFrame(c, 8, p, false)
			return
		case 9:
			writeFrame(c, 10, p, false)
		case 0, 1, 2:
			if op != 0 {
				typ = op
				msg = nil
			}
			msg = append(msg, p...)
			if fin {
				writeFrame(c, typ, msg, false)
			}
		}
	}
}

func wsClient(url string) {
	host := strings.TrimPrefix(url, "ws://")
	path := "/"
	if i := strings.Index(host, "/"); i >= 0 {
		path = host[i:]
		host = host[:i]
	}
	c, err := net.Dial("tcp", host)
	if err != nil {
		fmt.Println("error:", err)
		return
	}
	defer c.Close()
	key := base64.StdEncoding.EncodeToString([]byte("0123456789abcdef"))
	fmt.Fprintf(c, "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n", path, host, key)
	r := bufio.NewReader(c)
	res, err := http.ReadResponse(r, nil)
	if err != nil {
		fmt.Println("error:", err)
		return
	}
	fmt.Println("handshake:", res.StatusCode, res.Header.Get("Sec-WebSocket-Accept") == acceptKey(key))
	writeFrame(c, 1, []byte("hello from go"), true)
	_, op, p, _ := readFrame(r)
	fmt.Printf("echo: %d %s\n", op, p)
	big := bytes.Repeat([]byte{7}, 70000)
	writeFrame(c, 2, big, true)
	_, op, p, _ = readFrame(r)
	fmt.Println("binary echo:", op, len(p), bytes.Equal(p, big))
	writeFrame(c, 9, []byte("ping"), true)
	_, op, p, _ = readFrame(r)
	fmt.Printf("pong: %d %s\n", op, p)
	writeFrame(c, 8, []byte{0x03, 0xe8}, true)
	_, op, p, _ = readFrame(r)
	fmt.Println("close:", op, binary.BigEndian.Uint16(p))
}

func main() {
	if len(os.Args) > 2 && os.Args[1] == "wsclient" {
		wsClient(os.Args[2])
		return
	}
	if len(os.Args) > 1 && os.Args[1] == "server" {
		server()
		return
	}
	if len(os.Args) > 3 && os.Args[1] == "client" {
		client(os.Args[2], os.Args[3])
		return
	}
	fmt.Fprintln(os.Stderr, "usage: main server | main client <h2c-url> <tls-url>")
	os.Exit(2)
}
