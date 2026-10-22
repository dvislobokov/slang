package main

// Channels. Ping-pong: two goroutines exchange 5M messages (2.5M each way)
// over two unbuffered channels. Pipeline: 1M values through 4 stages
// connected by buffered channels (capacity 128). Mirrors main.zl.

import (
	"fmt"
	"os"
	"time"
)

func ponger(in <-chan int, out chan<- int) {
	for v := range in {
		out <- v + 1
	}
	close(out)
}

func pingpong(msgs int) int {
	a := make(chan int)
	b := make(chan int)
	go ponger(a, b)
	sum := 0
	for i := 0; i < msgs/2; i++ {
		a <- i
		sum += <-b
	}
	close(a)
	for range b {
	}
	return sum
}

func source(out chan<- int, n int) {
	for i := 0; i < n; i++ {
		out <- i
	}
	close(out)
}

func stage(in <-chan int, out chan<- int, kind int) {
	for v := range in {
		switch kind {
		case 0:
			out <- v + 1
		case 1:
			out <- v * 3
		default:
			out <- v ^ 0x5555
		}
	}
	close(out)
}

func pipeline(n int) int {
	c0 := make(chan int, 128)
	c1 := make(chan int, 128)
	c2 := make(chan int, 128)
	c3 := make(chan int, 128)
	go source(c0, n)
	go stage(c0, c1, 0)
	go stage(c1, c2, 1)
	go stage(c2, c3, 2)
	sum := 0
	for v := range c3 {
		sum += v
	}
	return sum
}

func main() {
	small := os.Getenv("SCALE") == "small" // smoke workload
	msgs, items := 5000000, 1000000
	if small {
		msgs, items = 50000, 10000
	}
	t0 := time.Now()
	a := pingpong(msgs)
	t1 := time.Now()
	b := pipeline(items)
	t2 := time.Now()
	fmt.Println("channel_pingpong", a, b)
	fmt.Println("  pingpong_ms", t1.Sub(t0).Milliseconds(), "pipeline_ms", t2.Sub(t1).Milliseconds())
}
