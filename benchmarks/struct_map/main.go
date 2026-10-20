package main

import "fmt"

type Point struct {
	X int
	Y int
}

func main() {
	n := 100000
	var keys []string
	for i := 0; i < n; i++ {
		keys = append(keys, fmt.Sprintf("point-%d", i))
	}
	m := map[string]Point{}
	for r := 0; r < 20; r++ {
		for i := 0; i < n; i++ {
			k := keys[(i*7919+r)%n]
			p := m[k]
			m[k] = Point{X: p.X + i, Y: p.Y + r}
		}
	}
	sx, sy := 0, 0
	for i := 0; i < n; i++ {
		p := m[keys[i]]
		sx += p.X
		sy += p.Y
	}
	fmt.Println(len(m), sx, sy)
}
