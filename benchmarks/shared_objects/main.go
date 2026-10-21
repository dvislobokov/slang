package main

import (
	"fmt"
	"sync"
	"time"
)

type Tag struct {
	Label string
	N     int
}

type Item struct {
	Name  string
	Value int
	Tag   *Tag
}

func build(n, tag int) []*Item {
	var items []*Item
	t := &Tag{Label: "t", N: tag}
	for i := 0; i < n; i++ {
		if i%16 == 0 {
			t = &Tag{Label: fmt.Sprintf("tag%d", i%7), N: i}
		}
		items = append(items, &Item{Name: fmt.Sprintf("item%d", i%100), Value: i + tag, Tag: t})
	}
	return items
}

func sumItems(items []*Item) int {
	s := 0
	for _, it := range items {
		cur := it
		s += cur.Value + len(cur.Name)
		t := cur.Tag
		s += t.N + len(t.Label)
	}
	return s
}

func churn(rounds int) int {
	s := 0
	for r := 0; r < rounds; r++ {
		items := build(2000, r)
		s += sumItems(items)
		var keep []*Item
		for _, it := range items {
			if it.Value%3 == 0 {
				keep = append(keep, it)
			}
		}
		s += len(keep)
	}
	return s
}

func main() {
	t0 := time.Now()
	shared := build(200000, 0)
	res := make([]int, 8)
	var wg sync.WaitGroup
	for k := 0; k < 8; k++ {
		wg.Add(1)
		go func(k int) {
			defer wg.Done()
			for r := 0; r < 10; r++ {
				res[k] += sumItems(shared)
			}
		}(k)
	}
	wg.Wait()
	total := 0
	for _, r := range res {
		total += r
	}
	t1 := time.Now()
	all := make([][]*Item, 8)
	for k := 0; k < 8; k++ {
		wg.Add(1)
		go func(k int) { defer wg.Done(); all[k] = build(25000, k) }(k)
	}
	wg.Wait()
	sum2 := 0
	for r := 0; r < 10; r++ {
		for _, items := range all {
			sum2 += sumItems(items)
		}
	}
	t2 := time.Now()
	for k := 0; k < 8; k++ {
		wg.Add(1)
		go func(k int) { defer wg.Done(); res[k] = churn(100) }(k)
	}
	wg.Wait()
	sum3 := 0
	for _, r := range res {
		sum3 += r
	}
	t3 := time.Now()
	fmt.Println(total, sum2, sum3, "ms:", t1.Sub(t0).Milliseconds(), t2.Sub(t1).Milliseconds(), t3.Sub(t2).Milliseconds())
}
