package main

// Cancellation: 10k goroutines blocked under a context (half receiving
// from a channel, half in a 100 s timer), cancelled at once; measures the
// time from the cancel to the last goroutine finishing. 10 rounds with
// context.WithCancel + cancel(), 10 rounds with context.WithTimeout(20 ms).
// Mirrors main.zl.

import (
	"context"
	"fmt"
	"os"
	"sync"
	"time"
)

func waiter(ctx context.Context, ch chan int, i int) int {
	if i%2 == 0 {
		select {
		case <-ch:
			return 0
		case <-ctx.Done():
			return 1
		}
	}
	t := time.NewTimer(100 * time.Second)
	defer t.Stop()
	select {
	case <-t.C:
		return 0
	case <-ctx.Done():
		return 1
	}
}

func startAll(ctx context.Context, ch chan int, n int) int {
	res := make([]int, n)
	var wg sync.WaitGroup
	wg.Add(n)
	for i := 0; i < n; i++ {
		go func(i int) {
			res[i] = waiter(ctx, ch, i)
			wg.Done()
		}(i)
	}
	wg.Wait()
	c := 0
	for _, v := range res {
		c += v
	}
	return c
}

func main() {
	small := os.Getenv("SCALE") == "small" // smoke workload
	n, rounds := 10000, 10
	if small {
		n, rounds = 1000, 2
	}
	ch := make(chan int)
	cancelled := 0
	var drain, maxDrain time.Duration
	for r := 0; r < rounds; r++ {
		ctx, cancel := context.WithCancel(context.Background())
		done := make(chan int)
		go func() { done <- startAll(ctx, ch, n) }()
		time.Sleep(20 * time.Millisecond)
		c0 := time.Now()
		cancel()
		cancelled += <-done
		d := time.Since(c0)
		drain += d
		if d > maxDrain {
			maxDrain = d
		}
	}
	var timeout time.Duration
	for r := 0; r < rounds; r++ {
		s0 := time.Now()
		ctx, cancel := context.WithTimeout(context.Background(), 20*time.Millisecond)
		cancelled += startAll(ctx, ch, n)
		cancel()
		timeout += time.Since(s0) - 20*time.Millisecond
	}
	fmt.Println("cancel_tree", cancelled)
	fmt.Println("  cancel_drain_us_avg", int64(drain/time.Duration(rounds))/1000, "max", int64(maxDrain)/1000, "timeout_drain_us_avg", int64(timeout/time.Duration(rounds))/1000)
}
