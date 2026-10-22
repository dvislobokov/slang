package main

// Scaling over cores with 1, 2 and 4 goroutines (same shape and
// checksums as main.zl; GOMAXPROCS defaults to the CPU count).
//  1. wordfreq: word frequency over 400 MB of generated text.
//  2. trees: each goroutine builds and checks its share of 2048 binary
//     trees of depth 14.
// stdout: deterministic checksums; stderr: "ms <phase> <workers> <ms>".
//   SCALE=small  4 MB of text and 32 trees (smoke test)

import (
	"fmt"
	"os"
	"time"
)

const mask = 4294967295

type Node struct{ Left, Right *Node }

func lcg(x int) int { return (x*1103515245 + 12345) & 2147483647 }

func vocabulary() [][]byte {
	var words [][]byte
	r := 42
	for k := 0; k < 1000; k++ {
		r = lcg(r)
		n := 2 + (r>>8)%8
		var w []byte
		for j := 0; j < n; j++ {
			r = lcg(r)
			w = append(w, byte(97+(r>>10)%26))
		}
		words = append(words, w)
	}
	return words
}

func generate(size int) []byte {
	words := vocabulary()
	var text []byte
	r := 7
	for len(text) < size {
		r = lcg(r)
		a := (r >> 4) % 1000
		r = lcg(r)
		b := (r >> 4) % 1000
		w := words[a*b/1000]
		for _, c := range w {
			text = append(text, c)
		}
		text = append(text, 32)
	}
	return text
}

func align(text []byte, i int) int {
	if i == 0 {
		return 0
	}
	j := i
	for j < len(text) && text[j-1] != 32 {
		j++
	}
	return j
}

func count(text []byte, lo, hi int) map[int]int {
	m := map[int]int{}
	h := 2166136261
	inWord := false
	for i := lo; i < hi; i++ {
		c := int(text[i])
		if c == 32 {
			if inWord {
				m[h] = m[h] + 1
			}
			h = 2166136261
			inWord = false
		} else {
			h = ((h ^ c) * 16777619) & mask
			inWord = true
		}
	}
	if inWord {
		m[h] = m[h] + 1
	}
	return m
}

func wordfreq(text []byte, workers int) {
	t0 := time.Now()
	chans := make([]chan map[int]int, workers)
	for k := 0; k < workers; k++ {
		lo := align(text, len(text)*k/workers)
		hi := align(text, len(text)*(k+1)/workers)
		ch := make(chan map[int]int, 1)
		chans[k] = ch
		go func() { ch <- count(text, lo, hi) }()
	}
	total := map[int]int{}
	for _, ch := range chans {
		for h, n := range <-ch {
			total[h] = total[h] + n
		}
	}
	words, sum, top := 0, 0, 0
	for h, n := range total {
		words += n
		sum = (sum + h*n) & mask
		if n > top {
			top = n
		}
	}
	fmt.Fprintf(os.Stderr, "ms wordfreq %d %d\n", workers, time.Since(t0).Milliseconds())
	fmt.Println("wordfreq", workers, len(total), words, top, sum)
}

func build(depth int) *Node {
	if depth == 0 {
		return &Node{}
	}
	return &Node{Left: build(depth - 1), Right: build(depth - 1)}
}

func check(n *Node) int {
	if n.Left == nil {
		return 1
	}
	return 1 + check(n.Left) + check(n.Right)
}

func treeTask(count, depth int) int {
	long := build(depth)
	s := 0
	for i := 0; i < count; i++ {
		s += check(build(depth))
	}
	return s + check(long)
}

func trees(total, depth, workers int) {
	t0 := time.Now()
	chans := make([]chan int, workers)
	for k := 0; k < workers; k++ {
		ch := make(chan int, 1)
		chans[k] = ch
		go func() { ch <- treeTask(total/workers, depth) }()
	}
	s := 0
	for _, ch := range chans {
		s += <-ch
	}
	fmt.Fprintf(os.Stderr, "ms trees %d %d\n", workers, time.Since(t0).Milliseconds())
	fmt.Println("trees", workers, s-workers*check(build(depth)))
}

func main() {
	size, ntrees := 400000000, 2048
	if os.Getenv("SCALE") == "small" {
		size, ntrees = 4000000, 32
	}
	text := generate(size)
	fmt.Println("text", len(text))
	for _, w := range []int{1, 2, 4} {
		wordfreq(text, w)
	}
	for _, w := range []int{1, 2, 4} {
		trees(ntrees, 14, w)
	}
}
