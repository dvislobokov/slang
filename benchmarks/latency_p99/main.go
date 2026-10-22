package main

// Tail latency of net/http while a goroutine churns a ~200 MB live heap
// (same shape and checksums as main.zl):
//   main                deterministic self-check of the churn
//   main -server        only the server: prints its port (stderr)
//   main -server-churn  the server plus the background churn; the port is
//                       printed once the heap is built, then every second
//                       "churn <buckets rebuilt/s>" (stderr)
//   main -load host:port  the load generator (http_hello's, plus latency
//                       percentiles): 64 keep-alive connections, 200k
//                       GET / in all; stdout "requests N bytes B",
//                       stderr "latency req/s=.. p50=.. p90=.. p99=..
//                       p999=.. max=.." (microseconds)
//   SCALE=small  a 2 MB heap and 20k requests (smoke test)

import (
	"bufio"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

const body = `{"message":"hello, world","id":1}`

type Obj struct {
	Id   int
	Name string
	Prev *Obj
}

type Bucket struct {
	Items []*Obj
	Index map[int]*Obj
}

type Config struct{ Slots, Objs int }

func lcg(x int) int { return (x*1103515245 + 12345) & 2147483647 }

func newBucket(seed, n int) *Bucket {
	b := &Bucket{Index: map[int]*Obj{}}
	var prev *Obj
	for i := 0; i < n; i++ {
		id := seed*4096 + i
		o := &Obj{Id: id, Name: fmt.Sprintf("o%d", id%1000), Prev: prev}
		b.Items = append(b.Items, o)
		if i%2 == 0 {
			b.Index[id] = o
		}
		prev = o
	}
	return b
}

func newHeap(cfg Config) []*Bucket {
	var slots []*Bucket
	for s := 0; s < cfg.Slots; s++ {
		slots = append(slots, newBucket(s, cfg.Objs))
	}
	return slots
}

func rebuild(slots []*Bucket, cfg Config, r int) int {
	next := lcg(r)
	slots[next%cfg.Slots] = newBucket(next&1048575, cfg.Objs)
	return next
}

func checksum(slots []*Bucket) int {
	s, objs := 0, 0
	for _, b := range slots {
		for _, o := range b.Items {
			s = (s + o.Id + len(o.Name)) & 4294967295
			objs++
		}
		s = (s + len(b.Index)) & 4294967295
	}
	fmt.Println("objects", objs, "checksum", s)
	return s
}

func churn(slots []*Bucket, cfg Config) {
	r, n := 1, 0
	t0 := time.Now()
	for {
		r = rebuild(slots, cfg, r)
		n++
		if d := time.Since(t0); d >= time.Second {
			fmt.Fprintf(os.Stderr, "churn %d\n", int64(n)*int64(time.Second)/int64(d))
			n = 0
			t0 = time.Now()
		}
	}
}

// One keep-alive connection: n requests, latency of each in ns into lat.
func worker(addr string, n int, lat []int64) int {
	c, err := net.Dial("tcp", addr)
	if err != nil {
		panic(err)
	}
	r := bufio.NewReaderSize(c, 16384)
	req := []byte("GET / HTTP/1.1\r\nHost: bench\r\nUser-Agent: go-load\r\n\r\n")
	buf := make([]byte, 4096)
	bytes := 0
	for i := 0; i < n; i++ {
		t0 := time.Now()
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
		if length < 0 || length > len(buf) || !strings.HasPrefix(status, "HTTP/1.1 200") {
			panic("bad response " + status)
		}
		if _, err := io.ReadFull(r, buf[:length]); err != nil {
			panic(err)
		}
		if lat != nil {
			lat[i] = int64(time.Since(t0))
		}
		if string(buf[:length]) != body {
			panic("bad body " + string(buf[:length]))
		}
		bytes += length
	}
	c.Close()
	return bytes
}

func load(addr string, conns, total int) {
	worker(addr, 100, nil)
	per := total / conns
	lats := make([]int64, per*conns)
	sizes := make([]int, conns)
	t0 := time.Now()
	var wg sync.WaitGroup
	for i := 0; i < conns; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			sizes[i] = worker(addr, per, lats[i*per:(i+1)*per])
		}(i)
	}
	wg.Wait()
	secs := time.Since(t0).Seconds()
	sort.Slice(lats, func(a, b int) bool { return lats[a] < lats[b] })
	pct := func(p float64) float64 {
		i := int(p * float64(len(lats)))
		if i >= len(lats) {
			i = len(lats) - 1
		}
		return float64(lats[i]) / 1e3
	}
	n := len(lats)
	b := 0
	for _, s := range sizes {
		b += s
	}
	fmt.Fprintf(os.Stderr, "latency req/s=%.0f p50=%.0f p90=%.0f p99=%.0f p999=%.0f max=%.0f\n",
		float64(n)/secs, pct(0.50), pct(0.90), pct(0.99), pct(0.999), float64(lats[n-1])/1e3)
	fmt.Println("requests", n, "bytes", b)
}

func main() {
	cfg := Config{Slots: 1000, Objs: 3200}
	conns, total := 64, 200000
	if os.Getenv("SCALE") == "small" {
		cfg = Config{Slots: 100, Objs: 150}
		total = 20000
	}
	mode := ""
	if len(os.Args) > 1 {
		mode = os.Args[1]
	}
	switch mode {
	case "":
		slots := newHeap(cfg)
		checksum(slots)
		r := 1
		for i := 0; i < cfg.Slots*2; i++ {
			r = rebuild(slots, cfg, r)
		}
		checksum(slots)
		runtime.GC()
		var ms runtime.MemStats
		runtime.ReadMemStats(&ms)
		fmt.Fprintf(os.Stderr, "heap_in_use_mb %d\n", ms.HeapAlloc/1048576)
		runtime.KeepAlive(slots)
		return
	case "-load":
		load(os.Args[2], conns, total)
		return
	}
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		io.WriteString(w, body)
	})
	go http.Serve(l, mux)
	if mode == "-server-churn" {
		slots := newHeap(cfg)
		go churn(slots, cfg)
	}
	fmt.Fprintln(os.Stderr, l.Addr().(*net.TCPAddr).Port)
	select {}
}
