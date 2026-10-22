// Load generator of benchmarks/web_api (one for every server):
//
//	load -check host:port   a fixed sequence of 24 requests on one
//	                        connection (incl. 401/400/404 cases); prints
//	                        the statuses and a checksum of the bodies
//	                        (run it on a fresh server: order ids from 1)
//	load host:port          keep-alive HTTP/1.1 load: CONNS connections
//	                        (default 64), REQUESTS in all (default 200000,
//	                        SCALE=small: 4000), one in flight per
//	                        connection, mixed round-robin over the 4
//	                        endpoints; prints req/s, p50/p99 latency and an
//	                        order-independent checksum of the responses
//
// Checksum: the sum (mod 2^64) of FNV-1a of "status body" over all
// responses; for POST /api/orders the order id is taken out of the body
// (and summed separately), because which request gets which id depends on
// scheduling.
package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"hash/fnv"
	"io"
	"net"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

var categories = []string{"tools", "books", "games", "food"}

type req struct {
	method, path, body string
	key                bool
}

// The i-th request of a connection's mix.
func mixed(conn, i int) req {
	k := conn*1000003 + i
	id := k%1000 + 1
	switch i % 4 {
	case 0:
		return req{"GET", "/api/items/" + strconv.Itoa(id), "", true}
	case 1:
		return req{"GET", fmt.Sprintf("/api/items?category=%s&limit=10", categories[(i/4)%4]), "", true}
	case 2:
		return req{"GET", fmt.Sprintf("/api/items/%d/quote?qty=%d", id, (i/4)%10+1), "", true}
	default:
		return req{"POST", "/api/orders", fmt.Sprintf(`{"ItemId": %d, "Qty": %d, "Email": "user%d@example.com"}`, id, (i/4)%10+1, k%5000), true}
	}
}

type client struct {
	c net.Conn
	r *bufio.Reader
	w *bufio.Writer
}

func dial(addr string) *client {
	c, err := net.Dial("tcp", addr)
	if err != nil {
		panic(err)
	}
	return &client{c, bufio.NewReaderSize(c, 16384), bufio.NewWriterSize(c, 4096)}
}

func (cl *client) do(q req) (int, []byte) {
	w := cl.w
	w.WriteString(q.method + " " + q.path + " HTTP/1.1\r\nHost: bench\r\nUser-Agent: go-load\r\n")
	if q.key {
		w.WriteString("X-Api-Key: bench-key\r\n")
	}
	if q.method == "POST" {
		w.WriteString("Content-Type: application/json\r\nContent-Length: " + strconv.Itoa(len(q.body)) + "\r\n\r\n" + q.body)
	} else {
		w.WriteString("\r\n")
	}
	if err := w.Flush(); err != nil {
		panic(err)
	}
	status, length, chunked := 0, -1, false
	for {
		line, err := cl.r.ReadString('\n')
		if err != nil {
			panic(err)
		}
		line = strings.TrimRight(line, "\r\n")
		if line == "" {
			break
		}
		if status == 0 {
			if len(line) < 12 {
				panic("bad status line " + line)
			}
			status, _ = strconv.Atoi(line[9:12])
			continue
		}
		l := strings.ToLower(line)
		if strings.HasPrefix(l, "content-length:") {
			length, _ = strconv.Atoi(strings.TrimSpace(line[15:]))
		} else if strings.HasPrefix(l, "transfer-encoding:") && strings.Contains(l, "chunked") {
			chunked = true
		}
	}
	if chunked {
		var body []byte
		for {
			line, err := cl.r.ReadString('\n')
			if err != nil {
				panic(err)
			}
			n, err := strconv.ParseInt(strings.TrimSpace(strings.SplitN(line, ";", 2)[0]), 16, 64)
			if err != nil {
				panic("bad chunk " + line)
			}
			if n == 0 {
				cl.r.ReadString('\n')
				return status, body
			}
			buf := make([]byte, n+2)
			if _, err := io.ReadFull(cl.r, buf); err != nil {
				panic(err)
			}
			body = append(body, buf[:n]...)
		}
	}
	if length < 0 {
		panic("no content-length")
	}
	body := make([]byte, length)
	if _, err := io.ReadFull(cl.r, body); err != nil {
		panic(err)
	}
	return status, body
}

func fnv64(status int, body []byte) uint64 {
	h := fnv.New64a()
	h.Write([]byte(strconv.Itoa(status) + " "))
	h.Write(body)
	return h.Sum64()
}

type order struct {
	Id         int
	ItemId     int
	Qty        int
	Email      string
	TotalCents int
}

// Hash of a response with the order id taken out (returned separately).
func digest(q req, status int, body []byte) (uint64, int) {
	if q.method == "POST" && status == 201 {
		var o order
		if err := json.Unmarshal(body, &o); err != nil {
			panic(fmt.Sprintf("bad order %q", body))
		}
		id := o.Id
		o.Id = 0
		b, _ := json.Marshal(o)
		return fnv64(status, b), id
	}
	return fnv64(status, body), 0
}

func check(addr string) {
	cl := dial(addr)
	seq := []req{
		{"GET", "/api/items/1", "", true},
		{"GET", "/api/items/1000", "", true},
		{"GET", "/api/items/1001", "", true},
		{"GET", "/api/items/abc", "", true},
		{"GET", "/api/items/5", "", false},
		{"GET", "/api/items?category=books&limit=3", "", true},
		{"GET", "/api/items?category=food&limit=0", "", true},
		{"GET", "/api/items?category=none&limit=5", "", true},
		{"GET", "/api/items/7/quote?qty=3", "", true},
		{"GET", "/api/items/9999/quote?qty=3", "", true},
		{"POST", "/api/orders", `{"ItemId": 3, "Qty": 2, "Email": "ann@example.com"}`, true},
		{"POST", "/api/orders", `{"ItemId": 4, "Qty": 1, "Email": "bob@example.com"}`, true},
		{"POST", "/api/orders", `{"ItemId": 0, "Qty": 2, "Email": "ann@example.com"}`, true},
		{"POST", "/api/orders", `{"ItemId": 3, "Qty": 500, "Email": "ann@example.com"}`, true},
		{"POST", "/api/orders", `{"ItemId": 3, "Qty": 2, "Email": "not-an-email"}`, true},
		{"POST", "/api/orders", `{"ItemId": 3, "Qty": 2, "Email": "ann@example.com"}`, false},
	}
	for i := 0; i < 8; i++ {
		seq = append(seq, mixed(0, i))
	}
	var sum uint64
	var statuses []string
	for _, q := range seq {
		status, body := cl.do(q)
		statuses = append(statuses, strconv.Itoa(status))
		if status == 200 && string(body) == "null" {
			// Zell's web writes an empty list ([] is the nil slice) as
			// null, Go's servers as []: an open item of
			// benchmarks/RESULTS-framework.md, normalised here.
			body = []byte("[]")
		}
		if status == 200 || status == 201 {
			sum += fnv64(status, body)
		}
		if os.Getenv("VERBOSE") != "" {
			fmt.Fprintf(os.Stderr, "%s %s -> %d %s\n", q.method, q.path, status, body)
		}
	}
	fmt.Printf("statuses %s\nchecksum %016x\n", strings.Join(statuses, " "), sum)
}

func worker(addr string, conn, n int, lat []int64) (uint64, int, int) {
	cl := dial(addr)
	var sum uint64
	ids, ok := 0, 0
	for i := 0; i < n; i++ {
		q := mixed(conn, i)
		t0 := time.Now()
		status, body := cl.do(q)
		if lat != nil {
			lat[i] = int64(time.Since(t0))
		}
		h, id := digest(q, status, body)
		sum += h
		ids += id
		if status == 200 || status == 201 {
			ok++
		}
	}
	cl.c.Close()
	return sum, ids, ok
}

func envInt(name string, def int) int {
	if v := os.Getenv(name); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil {
			panic(err)
		}
		return n
	}
	return def
}

func load(addr string) {
	conns := envInt("CONNS", 64)
	total := 200000
	if os.Getenv("SCALE") == "small" {
		total = 4000
	}
	total = envInt("REQUESTS", total)
	per := total / conns
	worker(addr, conns, 1000, nil) // warm-up (connection index outside the measured ones)
	lats := make([][]int64, conns)
	sums := make([]uint64, conns)
	idsum := make([]int, conns)
	oks := make([]int, conns)
	var wg sync.WaitGroup
	t0 := time.Now()
	for i := 0; i < conns; i++ {
		lats[i] = make([]int64, per)
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			sums[i], idsum[i], oks[i] = worker(addr, i, per, lats[i])
		}(i)
	}
	wg.Wait()
	secs := time.Since(t0).Seconds()
	var all []int64
	var sum uint64
	ok := 0
	for i := 0; i < conns; i++ {
		all = append(all, lats[i]...)
		sum += sums[i]
		ok += oks[i]
	}
	sort.Slice(all, func(a, b int) bool { return all[a] < all[b] })
	n := len(all)
	pct := func(p float64) float64 { return float64(all[int(float64(n-1)*p)]) / 1e6 }
	fmt.Fprintf(os.Stderr, "web_api load: %d requests over %d connections in %.3f s: %.0f req/s, p50 %.3f ms, p99 %.3f ms\n",
		n, conns, secs, float64(n)/secs, pct(0.50), pct(0.99))
	fmt.Printf("requests %d ok %d checksum %016x\n", n, ok, sum)
	fmt.Printf("result req_s=%.0f p50_ms=%.3f p99_ms=%.3f\n", float64(n)/secs, pct(0.50), pct(0.99))
}

func main() {
	if len(os.Args) == 3 && os.Args[1] == "-check" {
		check(os.Args[2])
		return
	}
	if len(os.Args) != 2 {
		fmt.Fprintln(os.Stderr, "usage: load [-check] host:port")
		os.Exit(2)
	}
	load(os.Args[1])
}
