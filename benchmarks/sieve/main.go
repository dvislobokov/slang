package main

import "fmt"

func countPrimes(n int) int {
	sieve := make([]bool, 0)
	for i := 0; i <= n; i++ {
		sieve = append(sieve, true)
	}
	count := 0
	for i := 2; i <= n; i++ {
		if sieve[i] {
			count++
			for j := i * i; j <= n; j += i {
				sieve[j] = false
			}
		}
	}
	return count
}

func main() {
	total := 0
	for round := 0; round < 5; round++ {
		total += countPrimes(10000000)
	}
	fmt.Println(total)
}
