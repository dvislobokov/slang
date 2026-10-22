// web_api with Gin: the same API as ../../server.zl (routes, data,
// logging + API-key middleware, validation). Prints its port on stderr
// and serves until killed.
package main

import (
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"strconv"
	"sync/atomic"

	"github.com/gin-gonic/gin"
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
	ItemId int    `binding:"required,min=1,max=1000"`
	Qty    int    `binding:"required,min=1,max=100"`
	Email  string `binding:"required,email"`
}

type Order struct {
	Id         int
	ItemId     int
	Qty        int
	Email      string
	TotalCents int
}

var categories = []string{"tools", "books", "games", "food"}

// Catalog and Orders are the services, injected by construction (Go has
// no container; this is the usual Gin style).
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

type Orders struct{ next atomic.Int64 }

type Api struct {
	catalog *Catalog
	orders  *Orders
}

func (a *Api) Get(c *gin.Context) {
	id, err := strconv.Atoi(c.Param("id"))
	if err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"title": "bad id"})
		return
	}
	it := a.catalog.Get(id)
	if it == nil {
		c.JSON(http.StatusNotFound, gin.H{"title": "Not Found"})
		return
	}
	c.JSON(http.StatusOK, it)
}

func (a *Api) List(c *gin.Context) {
	n, _ := strconv.Atoi(c.Query("limit"))
	if n <= 0 || n > 100 {
		n = 20
	}
	c.JSON(http.StatusOK, a.catalog.List(c.Query("category"), n))
}

func (a *Api) Quote(c *gin.Context) {
	id, err := strconv.Atoi(c.Param("id"))
	if err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"title": "bad id"})
		return
	}
	qty, err := strconv.Atoi(c.Query("qty"))
	if err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"title": "bad qty"})
		return
	}
	it := a.catalog.Get(id)
	if it == nil {
		c.JSON(http.StatusNotFound, gin.H{"title": "Not Found"})
		return
	}
	c.JSON(http.StatusOK, Quote{ItemId: id, Qty: qty, TotalCents: it.PriceCents * qty})
}

func (a *Api) Create(c *gin.Context) {
	var o NewOrder
	if err := c.ShouldBindJSON(&o); err != nil {
		c.JSON(http.StatusBadRequest, gin.H{"title": "validation", "detail": err.Error()})
		return
	}
	it := a.catalog.Get(o.ItemId)
	if it == nil {
		c.JSON(http.StatusNotFound, gin.H{"title": "Not Found"})
		return
	}
	c.JSON(http.StatusCreated, Order{Id: int(a.orders.next.Add(1)), ItemId: o.ItemId, Qty: o.Qty,
		Email: o.Email, TotalCents: it.PriceCents * o.Qty})
}

func apiKey(c *gin.Context) {
	if c.GetHeader("X-Api-Key") != "bench-key" {
		c.String(http.StatusUnauthorized, "unauthorized")
		c.Abort()
		return
	}
	c.Next()
}

func main() {
	gin.SetMode(gin.ReleaseMode)
	r := gin.New()
	r.Use(gin.LoggerWithWriter(io.Discard), apiKey)
	a := &Api{catalog: NewCatalog(), orders: &Orders{}}
	r.GET("/api/items/:id", a.Get)
	r.GET("/api/items", a.List)
	r.GET("/api/items/:id/quote", a.Quote)
	r.POST("/api/orders", a.Create)
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}
	fmt.Fprintln(os.Stderr, ln.Addr().(*net.TCPAddr).Port)
	panic(http.Serve(ln, r))
}
