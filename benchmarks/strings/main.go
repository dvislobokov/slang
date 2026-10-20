package main

import (
	"fmt"
	"strings"
)

func main() {
	parts := make([]string, 0)
	for i := 0; i < 200000; i++ {
		parts = append(parts, "item"+string(rune('a'+i%26)))
	}
	joined := strings.Join(parts, ",")
	total := 0
	for round := 0; round < 20; round++ {
		split := strings.Split(joined, ",")
		upper := strings.ToUpper(joined)
		total += len(split) + len(upper)
		if strings.Contains(upper, "ITEMZ") {
			total++
		}
	}
	fmt.Println(len(joined), total)
}
