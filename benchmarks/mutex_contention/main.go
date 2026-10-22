package main

// sync.Mutex. Contended: 8 goroutines increment a shared counter under one
// mutex, 10M increments in all. Uncontended: one goroutine takes and
// releases the mutex 10M times. Mirrors main.zl.

import (
	"fmt"
	"os"
	"sync"
	"time"
)

type counter struct {
	mu sync.Mutex
	n  int
}

func incr(c *counter, times int) {
	for i := 0; i < times; i++ {
		c.mu.Lock()
		c.n++
		c.mu.Unlock()
	}
}

func main() {
	small := os.Getenv("SCALE") == "small" // smoke workload
	total := 10000000
	if small {
		total = 100000
	}
	c := &counter{}
	t0 := time.Now()
	var wg sync.WaitGroup
	wg.Add(8)
	for i := 0; i < 8; i++ {
		go func() { incr(c, total/8); wg.Done() }()
	}
	wg.Wait()
	t1 := time.Now()
	u := &counter{}
	incr(u, total)
	t2 := time.Now()
	fmt.Println("mutex_contention", c.n, u.n)
	fmt.Println("  contended_ms", t1.Sub(t0).Milliseconds(), "uncontended_ms", t2.Sub(t1).Milliseconds())
}
