// web_api with plain net/http (Go 1.22 ServeMux patterns) and
// encoding/json: the same API as ../../server.zl, the second Go baseline.
// Logging middleware: one slog text line per request into io.Discard.
// Prints its port on stderr and serves until killed.
package main

import (
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"net/mail"
	"os"
	"strconv"
	"sync/atomic"
	"time"
)

type Item struct {
	Id         int
	Name       string
	Category   string
	PriceCents int
	Stock      int
}

type Quote struct {
	ItemId     int
	Qty        int
	TotalCents int
}

type NewOrder struct {
	ItemId int
	Qty    int
	Email  string
}

type Order struct {
	Id         int
	ItemId     int
	Qty        int
	Email      string
	TotalCents int
}

var categories = []string{"tools", "books", "games", "food"}

type Catalog struct{ items []Item }

func NewCatalog() *Catalog {
	xs := make([]Item, 0, 1000)
	for i := 1; i <= 1000; i++ {
		xs = append(xs, Item{Id: i, Name: fmt.Sprintf("item-%d", i), Category: categories[i%4],
			PriceCents: (i*37)%10000 + 100, Stock: i % 50})
	}
	return &Catalog{items: xs}
}

func (c *Catalog) Get(id int) *Item {
	if id < 1 || id > len(c.items) {
		return nil
	}
	return &c.items[id-1]
}

func (c *Catalog) List(category string, limit int) []Item {
	out := make([]Item, 0, limit)
	for _, it := range c.items {
		if len(out) >= limit {
			break
		}
		if category == "" || it.Category == category {
			out = append(out, it)
		}
	}
	return out
}

type Api struct {
	catalog *Catalog
	next    atomic.Int64
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	b, err := json.Marshal(v)
	if err != nil {
		http.Error(w, err.Error(), 500)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Content-Length", strconv.Itoa(len(b)))
	w.WriteHeader(status)
	w.Write(b)
}

func problem(w http.ResponseWriter, status int, title string) {
	writeJSON(w, status, map[string]any{"status": status, "title": title})
}

func (a *Api) Get(w http.ResponseWriter, r *http.Request) {
	id, err := strconv.Atoi(r.PathValue("id"))
	if err != nil {
		problem(w, 400, "bad id")
		return
	}
	it := a.catalog.Get(id)
	if it == nil {
		problem(w, 404, "Not Found")
		return
	}
	writeJSON(w, 200, it)
}

func (a *Api) List(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	n, _ := strconv.Atoi(q.Get("limit"))
	if n <= 0 || n > 100 {
		n = 20
	}
	writeJSON(w, 200, a.catalog.List(q.Get("category"), n))
}

func (a *Api) Quote(w http.ResponseWriter, r *http.Request) {
	id, err := strconv.Atoi(r.PathValue("id"))
	if err != nil {
		problem(w, 400, "bad id")
		return
	}
	qty, err := strconv.Atoi(r.URL.Query().Get("qty"))
	if err != nil {
		problem(w, 400, "bad qty")
		return
	}
	it := a.catalog.Get(id)
	if it == nil {
		problem(w, 404, "Not Found")
		return
	}
	writeJSON(w, 200, Quote{ItemId: id, Qty: qty, TotalCents: it.PriceCents * qty})
}

func (a *Api) Create(w http.ResponseWriter, r *http.Request) {
	var o NewOrder
	if err := json.NewDecoder(r.Body).Decode(&o); err != nil {
		problem(w, 400, "bad body")
		return
	}
	if o.ItemId < 1 || o.ItemId > 1000 || o.Qty < 1 || o.Qty > 100 || o.Email == "" {
		problem(w, 400, "validation")
		return
	}
	if _, err := mail.ParseAddress(o.Email); err != nil {
		problem(w, 400, "validation")
		return
	}
	it := a.catalog.Get(o.ItemId)
	if it == nil {
		problem(w, 404, "Not Found")
		return
	}
	writeJSON(w, 201, Order{Id: int(a.next.Add(1)), ItemId: o.ItemId, Qty: o.Qty, Email: o.Email,
		TotalCents: it.PriceCents * o.Qty})
}

type statusWriter struct {
	http.ResponseWriter
	status int
}

func (s *statusWriter) WriteHeader(code int) {
	s.status = code
	s.ResponseWriter.WriteHeader(code)
}

func logging(l *slog.Logger, next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		t0 := time.Now()
		sw := &statusWriter{ResponseWriter: w, status: 200}
		next.ServeHTTP(sw, r)
		l.Info("HTTP request", "method", r.Method, "path", r.URL.Path, "status", sw.status,
			"elapsed_ms", float64(time.Since(t0).Microseconds())/1000)
	})
}

func apiKey(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("X-Api-Key") != "bench-key" {
			w.Header().Set("Content-Type", "text/plain")
			w.WriteHeader(401)
			io.WriteString(w, "unauthorized")
			return
		}
		next.ServeHTTP(w, r)
	})
}

func main() {
	a := &Api{catalog: NewCatalog()}
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/items/{id}", a.Get)
	mux.HandleFunc("GET /api/items", a.List)
	mux.HandleFunc("GET /api/items/{id}/quote", a.Quote)
	mux.HandleFunc("POST /api/orders", a.Create)
	l := slog.New(slog.NewTextHandler(io.Discard, nil))
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}
	fmt.Fprintln(os.Stderr, ln.Addr().(*net.TCPAddr).Port)
	panic(http.Serve(ln, logging(l, apiKey(mux))))
}
