// websocket_echo, Go side (gorilla/websocket): the same shape as main.zl.
//
//	main                server and client in one process
//	main -server        only the server: prints its port (stderr), serves until killed
//	main -load url      only the client (ws://host:port/ws)
//
// CONNS (default 1000) x MSGS (default 10000; SCALE=small: 100 x 200)
// binary 128-byte messages, one in flight per connection, every echo
// checked. stdout: "messages <n> bytes <n>"; stderr: messages/s.
package main

import (
	"bytes"
	"fmt"
	"net"
	"net/http"
	"os"
	"strconv"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

var upgrader = websocket.Upgrader{ReadBufferSize: 4096, WriteBufferSize: 4096}

func echo(w http.ResponseWriter, r *http.Request) {
	c, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		return
	}
	defer c.Close()
	for {
		t, m, err := c.ReadMessage()
		if err != nil {
			return
		}
		if err := c.WriteMessage(t, m); err != nil {
			return
		}
	}
}

var payloads [26][]byte

func init() {
	for s := range payloads {
		b := make([]byte, 128)
		for k := range b {
			b[k] = byte(97 + (s+k)%26)
		}
		payloads[s] = b
	}
}

func client(url string, c, msgs int) int {
	conn, _, err := websocket.DefaultDialer.Dial(url, nil)
	if err != nil {
		panic(err)
	}
	n := 0
	for i := 0; i < msgs; i++ {
		p := payloads[(c+i)%26]
		if err := conn.WriteMessage(websocket.BinaryMessage, p); err != nil {
			panic(err)
		}
		t, m, err := conn.ReadMessage()
		if err != nil {
			panic(err)
		}
		if t != websocket.BinaryMessage || !bytes.Equal(m, p) {
			panic(fmt.Sprintf("connection %d: bad echo of message %d", c, i))
		}
		n += len(m)
	}
	conn.WriteMessage(websocket.CloseMessage, websocket.FormatCloseMessage(websocket.CloseNormalClosure, ""))
	conn.Close()
	return n
}

func load(url string, conns, msgs int) {
	client(url, 0, 100) // warm-up
	t0 := time.Now()
	totals := make([]int, conns)
	var wg sync.WaitGroup
	for c := 0; c < conns; c++ {
		wg.Add(1)
		go func(c int) {
			defer wg.Done()
			totals[c] = client(url, c, msgs)
		}(c)
	}
	wg.Wait()
	total := 0
	for _, t := range totals {
		total += t
	}
	secs := time.Since(t0).Seconds()
	n := conns * msgs
	fmt.Fprintf(os.Stderr, "websocket_echo go client: %d messages over %d connections in %.3f s: %.0f msg/s\n", n, conns, secs, float64(n)/secs)
	fmt.Println("messages", n, "bytes", total)
}

func envInt(name string, def int) int {
	if v := os.Getenv(name); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			return n
		}
	}
	return def
}

func main() {
	conns, msgs := 1000, 10000
	if os.Getenv("SCALE") == "small" {
		conns, msgs = 100, 200
	}
	conns, msgs = envInt("CONNS", conns), envInt("MSGS", msgs)
	if len(os.Args) > 2 && os.Args[1] == "-load" {
		load(os.Args[2], conns, msgs)
		return
	}
	mux := http.NewServeMux()
	mux.HandleFunc("/ws", echo)
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}
	port := ln.Addr().(*net.TCPAddr).Port
	if len(os.Args) > 1 && os.Args[1] == "-server" {
		fmt.Fprintln(os.Stderr, port)
		panic(http.Serve(ln, mux))
	}
	go http.Serve(ln, mux)
	load(fmt.Sprintf("ws://127.0.0.1:%d/ws", port), conns, msgs)
}
