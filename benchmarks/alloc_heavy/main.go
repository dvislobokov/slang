package main

import "fmt"

// Two phases: binary trees, then small non-escaping temporaries (see main.zl).

type Node struct {
	Left, Right *Node
}

type Stats struct {
	Nodes, Trees int
}

func (s *Stats) Add(n int) {
	s.Nodes += n
	s.Trees++
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

func main() {
	maxDepth := 16
	long := build(maxDepth)
	total := 0
	for depth := 4; depth <= maxDepth; depth += 2 {
		iters := 1 << (maxDepth - depth + 4)
		acc := Stats{}
		for i := 0; i < iters; i++ {
			st := &Stats{}
			st.Add(check(build(depth)))
			acc.Add(st.Nodes)
		}
		total += acc.Nodes + acc.Trees
	}
	temps := 0
	for i := 0; i < 4000000; i++ {
		st := &Stats{}
		st.Add(i & 7)
		acc := Stats{Nodes: st.Nodes}
		acc.Add(1)
		temps += acc.Nodes + acc.Trees + st.Trees
	}
	fmt.Println(total, check(long), temps)
}
