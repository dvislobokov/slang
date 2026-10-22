package main

// Latency-bound loopback TCP: 1000 concurrent connections, each doing 1000
// request/response round trips of 64 bytes, 1M round trips in all. Prints
// the bytes echoed, then the throughput and the p50/p99/max round-trip
// latency. Mirrors main.zl.

import (
	"bytes"
	"fmt"
	"io"
	"net"
	"os"
	"sort"
	"time"
)

func serve(c net.Conn) {
	buf := make([]byte, 4096)
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

func client(addr string, trips int, lat []int64, res chan<- int) {
	c, err := net.Dial("tcp", addr)
	if err != nil {
		panic(err)
	}
	msg := bytes.Repeat([]byte("p"), 64)
	buf := make([]byte, 64)
	got := 0
	for k := 0; k < trips; k++ {
		t0 := time.Now()
		if _, err := c.Write(msg); err != nil {
			panic(err)
		}
		if _, err := io.ReadFull(c, buf); err != nil {
			panic(err)
		}
		lat[k] = int64(time.Since(t0))
		got += 64
	}
	c.Close()
	res <- got
}

func main() {
	small := os.Getenv("SCALE") == "small" // smoke workload
	conns, trips := 1000, 1000
	if small {
		conns, trips = 50, 50
	}
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
	lat := make([]int64, conns*trips)
	t0 := time.Now()
	res := make(chan int)
	for i := 0; i < conns; i++ {
		go client(l.Addr().String(), trips, lat[i*trips:(i+1)*trips], res)
	}
	total := 0
	for i := 0; i < conns; i++ {
		total += <-res
	}
	ns := time.Since(t0).Nanoseconds()
	l.Close()
	sort.Slice(lat, func(a, b int) bool { return lat[a] < lat[b] })
	n := int64(len(lat))
	fmt.Println("async_io", total)
	fmt.Println("  total_ms", ns/1000000, "rt_per_s", n*1000/(ns/1000000), "p50_us", lat[n/2]/1000, "p99_us", lat[n*99/100]/1000, "max_us", lat[n-1]/1000)
}
