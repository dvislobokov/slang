package main

// Task creation and scheduling. Phase 1: 1M goroutines that each compute a
// trivial value and finish; main waits for them all. Phase 2: 1M goroutines
// parked at the same time on a gate channel (memory per parked goroutine:
// peak RSS), then released and waited for. Mirrors main.zl.

import (
	"fmt"
	"os"
	"sync"
	"time"
)

func main() {
	small := os.Getenv("SCALE") == "small" // smoke workload
	n := 1000000
	if small {
		n = 10000
	}
	res := make([]int, n)
	t0 := time.Now()
	var wg sync.WaitGroup
	wg.Add(n)
	for i := 0; i < n; i++ {
		go func(i int) {
			res[i] = (i*7 + 3) % 1000
			wg.Done()
		}(i)
	}
	wg.Wait()
	sum := 0
	for _, v := range res {
		sum += v
	}
	t1 := time.Now()
	gate := make(chan struct{})
	var started sync.WaitGroup
	started.Add(n)
	wg.Add(n)
	for i := 0; i < n; i++ {
		go func(i int) {
			started.Done()
			<-gate
			res[i] = i % 1000
			wg.Done()
		}(i)
	}
	started.Wait()
	t2 := time.Now()
	close(gate)
	wg.Wait()
	for _, v := range res {
		sum += v
	}
	t3 := time.Now()
	fmt.Println("spawn_tasks", sum)
	fmt.Println("  run1M_ms", t1.Sub(t0).Milliseconds(), "park1M_ms", t2.Sub(t1).Milliseconds(), "wake1M_ms", t3.Sub(t2).Milliseconds())
}
