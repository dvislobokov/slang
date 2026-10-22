package main

// Timers: 100k goroutines each sleep 1-10 ms (time.Sleep, durations from a
// fixed LCG) and record how late they woke up. Mirrors main.zl.

import (
	"fmt"
	"os"
	"sync"
	"time"
)

func main() {
	small := os.Getenv("SCALE") == "small" // smoke workload
	n := 100000
	if small {
		n = 1000
	}
	seed := 12345
	late := make([]int64, n)
	sum := 0
	var wg sync.WaitGroup
	wg.Add(n)
	t0 := time.Now()
	for i := 0; i < n; i++ {
		seed = (seed*1103515245 + 12345) % 2147483648
		ms := 1 + seed/65536%10
		sum += ms
		go func(i, ms int) {
			s := time.Now()
			time.Sleep(time.Duration(ms) * time.Millisecond)
			late[i] = int64(time.Since(s)) - int64(ms)*1000000
			wg.Done()
		}(i, ms)
	}
	t1 := time.Now()
	wg.Wait()
	t2 := time.Now()
	var maxLate, totalLate int64
	over1ms := 0
	for _, l := range late {
		if l > maxLate {
			maxLate = l
		}
		if l > 1000000 {
			over1ms++
		}
		totalLate += l
	}
	fmt.Println("timers", n, sum)
	fmt.Println("  start_ms", t1.Sub(t0).Milliseconds(), "total_ms", t2.Sub(t0).Milliseconds(), "max_late_us", maxLate/1000, "mean_late_us", totalLate/int64(n)/1000, "late_over_1ms", over1ms)
}
