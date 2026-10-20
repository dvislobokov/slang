package main

// HTTP/1.1 "hello" throughput with Go's net/http (same shape as main.sl):
//   main                  server and load generator in one process
//   main -server          only the server: prints its port (stderr), serves until killed
//   main -load host:port  only the load generator
// HTTP/2 over TLS (self-signed certificate, ALPN h2):
//   main -server-tls      a TLS server (net/http enables h2): prints its port (stderr)
//   main -load2 host:port HTTP/2 load: 64 goroutines share one connection
//                         (http.Transport{ForceAttemptHTTP2}, InsecureSkipVerify)

import (
	"bufio"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"fmt"
	"io"
	"math/big"
	"net"
	"net/http"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
)

func worker(addr string, n int) int {
	c, err := net.Dial("tcp", addr)
	if err != nil {
		panic(err)
	}
	r := bufio.NewReaderSize(c, 16384)
	req := []byte("GET / HTTP/1.1\r\nHost: bench\r\nUser-Agent: go-load\r\n\r\n")
	for i := 0; i < n; i++ {
		if _, err := c.Write(req); err != nil {
			panic(err)
		}
		length := -1
		status := ""
		for {
			line, err := r.ReadString('\n')
			if err != nil {
				panic(err)
			}
			line = strings.TrimRight(line, "\r\n")
			if line == "" {
				break
			}
			if status == "" {
				status = line
			}
			if strings.HasPrefix(strings.ToLower(line), "content-length:") {
				length, _ = strconv.Atoi(strings.TrimSpace(line[15:]))
			}
		}
		if length < 0 || !strings.HasPrefix(status, "HTTP/1.1 200") {
			panic("bad response " + status)
		}
		if _, err := io.CopyN(io.Discard, r, int64(length)); err != nil {
			panic(err)
		}
	}
	c.Close()
	return n
}

func load(addr string, conns, total int) {
	worker(addr, 100)
	t0 := time.Now()
	var wg sync.WaitGroup
	counts := make([]int, conns)
	for i := 0; i < conns; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			counts[i] = worker(addr, total/conns)
		}(i)
	}
	wg.Wait()
	n := 0
	for _, c := range counts {
		n += c
	}
	secs := time.Since(t0).Seconds()
	fmt.Fprintf(os.Stderr, "http_hello go load: %d requests over %d connections in %.3f s: %.0f req/s\n", n, conns, secs, float64(n)/secs)
	fmt.Println("requests", n)
}

func selfSigned() tls.Certificate {
	key, _ := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	tmpl := &x509.Certificate{
		SerialNumber: big.NewInt(1),
		Subject:      pkix.Name{CommonName: "localhost"},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().Add(24 * time.Hour),
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

// HTTP/2 load: `conns` goroutines issue requests concurrently on the
// transport's single h2 connection to addr.
func load2(addr string, conns, total int) {
	tr := &http.Transport{ForceAttemptHTTP2: true, TLSClientConfig: &tls.Config{InsecureSkipVerify: true}, MaxIdleConnsPerHost: conns}
	c := &http.Client{Transport: tr}
	url := "https://" + addr + "/"
	one := func() {
		res, err := c.Get(url)
		if err != nil {
			panic(err)
		}
		b, _ := io.ReadAll(res.Body)
		res.Body.Close()
		if res.StatusCode != 200 || res.ProtoMajor != 2 || string(b) != "hello, world\n" {
			panic("bad response " + res.Proto + " " + res.Status)
		}
	}
	for i := 0; i < 100; i++ {
		one()
	}
	t0 := time.Now()
	var wg sync.WaitGroup
	for i := 0; i < conns; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for j := 0; j < total/conns; j++ {
				one()
			}
		}()
	}
	wg.Wait()
	n := total / conns * conns
	secs := time.Since(t0).Seconds()
	fmt.Fprintf(os.Stderr, "http_hello go h2 load: %d requests, %d streams on one connection in %.3f s: %.0f req/s\n", n, conns, secs, float64(n)/secs)
	fmt.Println("requests", n)
}

func main() {
	conns, total := 64, 200000
	if len(os.Args) > 2 && os.Args[1] == "-load" {
		load(os.Args[2], conns, total)
		return
	}
	if len(os.Args) > 2 && os.Args[1] == "-load2" {
		load2(os.Args[2], conns, total/2)
		return
	}
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/plain; charset=utf-8")
		io.WriteString(w, "hello, world\n")
	})
	if len(os.Args) > 1 && os.Args[1] == "-server-tls" {
		srv := &http.Server{Handler: mux, TLSConfig: &tls.Config{Certificates: []tls.Certificate{selfSigned()}}}
		go srv.ServeTLS(l, "", "")
		fmt.Fprintln(os.Stderr, l.Addr().(*net.TCPAddr).Port)
		select {}
	}
	go http.Serve(l, mux)
	port := l.Addr().(*net.TCPAddr).Port
	if len(os.Args) > 1 && os.Args[1] == "-server" {
		fmt.Fprintln(os.Stderr, port)
		select {}
	}
	load(fmt.Sprintf("127.0.0.1:%d", port), conns, total)
}
