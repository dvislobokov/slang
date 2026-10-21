package main

import (
	"fmt"
	"os"
	"runtime/debug"
	"strings"
)

func rssMB() int {
	b, err := os.ReadFile("/proc/self/statm")
	if err != nil {
		return -1
	}
	var size, res int
	fmt.Sscan(string(b), &size, &res)
	return res * 4096 / (1 << 20)
}

func main() {
	total := 0
	for round := 0; round < 20; round++ {
		var small []string
		for i := 0; i < 300000; i++ {
			if i%3 == 0 {
				small = append(small, strings.Repeat("a", 100+round))
			} else {
				small = append(small, strings.Repeat("b", 20+i%16))
			}
		}
		var big [][]byte
		for j := 0; j < 32; j++ {
			big = append(big, []byte(strings.Repeat("c", 256<<10)))
		}
		total += len(small) + len(big) + len(small[len(small)-1])
	}
	_ = debug.SetGCPercent
	fmt.Println(total, "rss_after", rssMB())
}
