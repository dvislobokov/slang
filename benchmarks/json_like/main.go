package main

import (
	"fmt"
	"strings"
)

func main() {
	total := 0
	for round := 0; round < 5; round++ {
		var records []string
		for i := 0; i < 100000; i++ {
			rec := fmt.Sprintf("{\"id\":%d,\"name\":\"user%d\",\"score\":%v,\"active\":%v}", i, (i+round)%1000, float64(i)*0.5, i%2 == 0)
			records = append(records, rec)
		}
		doc := "[" + strings.Join(records, ",") + "]"
		total += len(doc)
	}
	fmt.Println(total)
}
