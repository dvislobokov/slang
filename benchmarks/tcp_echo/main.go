package main

// 1000 concurrent TCP connections through loopback, each sending 1 MB in
// 64 KB writes and reading the echo back (same shape as main.sl).

import (
	"fmt"
	"io"
	"net"
	"os"
	"sync"
	"time"
)

func serve(c net.Conn) {
	buf := make([]byte, 65536)
	for {
		n, err := c.Read(buf)
		if n > 0 {
			if _, werr := c.Write(buf[:n]); werr != nil {
				break
			}
		}
		if err != nil {
			break
		}
	}
	c.Close()
}

func client(addr string, total int) int {
	c, err := net.Dial("tcp", addr)
	if err != nil {
		panic(err)
	}
	done := make(chan struct{})
	go func() {
		chunk := make([]byte, 65536)
		for sent := 0; sent < total; sent += len(chunk) {
			if _, err := c.Write(chunk); err != nil {
				panic(err)
			}
		}
		c.(*net.TCPConn).CloseWrite()
		close(done)
	}()
	buf := make([]byte, 65536)
	got := 0
	for {
		n, err := c.Read(buf)
		got += n
		if err == io.EOF {
			break
		}
		if err != nil {
			panic(err)
		}
	}
	<-done
	c.Close()
	return got
}

func main() {
	conns, size := 1000, 1<<20
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}
	go func() {
		for i := 0; i < conns; i++ {
			c, err := l.Accept()
			if err != nil {
				panic(err)
			}
			go serve(c)
		}
	}()
	t0 := time.Now()
	var wg sync.WaitGroup
	results := make([]int, conns)
	for i := 0; i < conns; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			results[i] = client(l.Addr().String(), size)
		}(i)
	}
	wg.Wait()
	total := 0
	for _, r := range results {
		total += r
	}
	secs := time.Since(t0).Seconds()
	fmt.Fprintf(os.Stderr, "tcp_echo go: %d MB echoed in %.3f s (%.0f MB/s)\n", total/size, secs, float64(total/size)/secs)
	fmt.Println("echoed", total/size, "MB")
}
