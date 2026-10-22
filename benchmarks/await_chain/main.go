package main

// Deep call chains, the Go counterpart of await chains: plain recursive
// calls 1000 deep, 10k times (stackful: no frame allocation), and the same
// with a runtime.Gosched() at the deepest level (1k times). Mirrors main.zl.

import (
	"fmt"
	"os"
	"runtime"
	"time"
)

//go:noinline
func chain(d, x int) int {
	if d == 0 {
		return x & 1023
	}
	return chain(d-1, x+d) + 1
}

//go:noinline
func chainYield(d, x int) int {
	if d == 0 {
		runtime.Gosched()
		return x & 1023
	}
	return chainYield(d-1, x+d) + 1
}

func main() {
	small := os.Getenv("SCALE") == "small" // smoke workload
	outer, outerYield := 10000, 1000
	if small {
		outer, outerYield = 100, 10
	}
	t0 := time.Now()
	a := 0
	for i := 0; i < outer; i++ {
		a += chain(1000, i)
	}
	t1 := time.Now()
	b := 0
	for i := 0; i < outerYield; i++ {
		b += chainYield(1000, i)
	}
	t2 := time.Now()
	fmt.Println("await_chain", a, b)
	fmt.Println("  sync_ms", t1.Sub(t0).Milliseconds(), "yield_ms", t2.Sub(t1).Milliseconds())
}
