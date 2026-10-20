package main

// HTTP/1.1 "hello" throughput with Go's net/http (same shape as main.sl):
//   main                  server and load generator in one process
//   main -server          only the server: prints its port (stderr), serves until killed
//   main -load host:port  only the load generator

import (
	"bufio"
	"fmt"
	"io"
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

func main() {
	conns, total := 64, 200000
	if len(os.Args) > 2 && os.Args[1] == "-load" {
		load(os.Args[2], conns, total)
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
	go http.Serve(l, mux)
	port := l.Addr().(*net.TCPAddr).Port
	if len(os.Args) > 1 && os.Args[1] == "-server" {
		fmt.Fprintln(os.Stderr, port)
		select {}
	}
	load(fmt.Sprintf("127.0.0.1:%d", port), conns, total)
}
