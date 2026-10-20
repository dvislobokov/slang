package main

import "fmt"

func main() {
	m := map[int]int{}
	for i := 0; i < 2000000; i++ {
		m[i*7919%1000003] += i
	}
	sum := 0
	for i := 0; i < 2000000; i++ {
		sum += m[i%1000003]
	}
	fmt.Println(len(m), sum)
}
