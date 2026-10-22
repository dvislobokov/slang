package main

// Fan-in: 10k producer goroutines each send 100 values into one channel
// (capacity 1024) read by one consumer. Worker pool: 8 workers take 1M jobs
// from a jobs channel and send results to a results channel (both capacity
// 1024). Mirrors main.zl.

import (
	"fmt"
	"os"
	"sync"
	"time"
)

func fanin(producers, each int) int {
	ch := make(chan int, 1024)
	var wg sync.WaitGroup
	wg.Add(producers)
	for p := 0; p < producers; p++ {
		go func(id int) {
			for i := 0; i < each; i++ {
				ch <- id*each + i
			}
			wg.Done()
		}(p)
	}
	go func() { wg.Wait(); close(ch) }()
	sum := 0
	for v := range ch {
		sum += v
	}
	return sum
}

func job(x int) int {
	h := x
	for i := 0; i < 8; i++ {
		h = (h*1103515245 + 12345) % 2147483648
	}
	return h % 1000
}

func pool(workers, n int) int {
	jobs := make(chan int, 1024)
	results := make(chan int, 1024)
	var wg sync.WaitGroup
	wg.Add(workers)
	for w := 0; w < workers; w++ {
		go func() {
			for j := range jobs {
				results <- job(j)
			}
			wg.Done()
		}()
	}
	done := make(chan int)
	go func() {
		sum := 0
		for v := range results {
			sum += v
		}
		done <- sum
	}()
	go func() { wg.Wait(); close(results) }()
	for i := 0; i < n; i++ {
		jobs <- i
	}
	close(jobs)
	return <-done
}

func main() {
	small := os.Getenv("SCALE") == "small" // smoke workload
	producers, jobs := 10000, 1000000
	if small {
		producers, jobs = 100, 10000
	}
	t0 := time.Now()
	a := fanin(producers, 100)
	t1 := time.Now()
	b := pool(8, jobs)
	t2 := time.Now()
	fmt.Println("fanout_fanin", a, b)
	fmt.Println("  fanin_ms", t1.Sub(t0).Milliseconds(), "pool_ms", t2.Sub(t1).Milliseconds())
}
