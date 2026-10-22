package main

import (
	"container/heap"
	"container/list"
	"fmt"
	"os"
	"slices"
	"strconv"
	"strings"
	"text/template"
	"time"
)

// slices, container/heap, container/list, strings.Builder, text/template:
// the same work as main.zl. Go has no deque or ordered map in the standard
// library: the deque is a growable ring buffer and the ordered map a
// sorted array with binary search, the data structures behind Zell's
// collections.Deque and collections.OrderedMap.
// SCALE=small divides every size by 100.

const M = 1000000007

type Rng struct{ s int }

func (r *Rng) Next() int {
	r.s = (r.s*1103515245 + 12345) % 2147483648
	return r.s >> 8
}

func (r *Rng) Big() int {
	a := r.Next()
	return (a << 23) ^ r.Next()
}

type Job struct{ Prio, Id int }

type Row struct {
	Id     int
	Name   string
	Price  float64
	Active bool
}

type Page struct {
	Title string
	Rows  []Row
}

type IntHeap []int

func (h IntHeap) Len() int           { return len(h) }
func (h IntHeap) Less(i, j int) bool { return h[i] < h[j] }
func (h IntHeap) Swap(i, j int)      { h[i], h[j] = h[j], h[i] }
func (h *IntHeap) Push(x any)        { *h = append(*h, x.(int)) }
func (h *IntHeap) Pop() any {
	old := *h
	x := old[len(old)-1]
	*h = old[:len(old)-1]
	return x
}

type JobQueue []Job

func (q JobQueue) Len() int           { return len(q) }
func (q JobQueue) Less(i, j int) bool { return q[i].Prio > q[j].Prio }
func (q JobQueue) Swap(i, j int)      { q[i], q[j] = q[j], q[i] }
func (q *JobQueue) Push(x any)        { *q = append(*q, x.(Job)) }
func (q *JobQueue) Pop() any {
	old := *q
	x := old[len(old)-1]
	*q = old[:len(old)-1]
	return x
}

// A ring-buffer deque (what collections.Deque is).
type Deque struct {
	buf     []int
	head, n int
}

func (d *Deque) PushBack(v int) {
	if d.n == len(d.buf) {
		c := len(d.buf) * 2
		if c < 8 {
			c = 8
		}
		nb := make([]int, c)
		for i := 0; i < d.n; i++ {
			nb[i] = d.buf[(d.head+i)%len(d.buf)]
		}
		d.buf, d.head = nb, 0
	}
	d.buf[(d.head+d.n)%len(d.buf)] = v
	d.n++
}

func (d *Deque) PopFront() int {
	v := d.buf[d.head]
	d.head = (d.head + 1) % len(d.buf)
	d.n--
	return v
}

// A sorted-array ordered map (what collections.OrderedMap is).
type OrderedMap struct {
	keys, vals []int
}

func (m *OrderedMap) Set(k, v int) {
	i, ok := slices.BinarySearch(m.keys, k)
	if ok {
		m.vals[i] = v
		return
	}
	m.keys = slices.Insert(m.keys, i, k)
	m.vals = slices.Insert(m.vals, i, v)
}

func (m *OrderedMap) Get(k int) int {
	i, ok := slices.BinarySearch(m.keys, k)
	if ok {
		return m.vals[i]
	}
	return 0
}

func itoa(n int) string { return strconv.Itoa(n) }

func report(name string, d time.Duration) {
	fmt.Fprintf(os.Stderr, "%s: %d ms\n", name, d.Milliseconds())
}

func main() {
	div := 1
	if os.Getenv("SCALE") == "small" {
		div = 100
	}
	r := Rng{2024}

	// 1. sort ints
	n := 10000000 / div
	xs := []int{}
	for i := 0; i < n; i++ {
		xs = append(xs, r.Big())
	}
	t0 := time.Now()
	slices.Sort(xs)
	d := time.Since(t0)
	sum := 0
	for i := 0; i < n; i += 997 {
		sum = (sum*31 + xs[i]%M) % M
	}
	fmt.Println("sort ints:", n, slices.IsSorted(xs), xs[0], xs[n-1], sum)
	report("sort 10M ints", d)

	// 2. binary search
	nq := 10000000 / div
	found, isum := 0, 0
	t0 = time.Now()
	for k := 0; k < nq; k++ {
		q := 0
		if k%2 == 0 {
			q = xs[(k*7919)%n]
		} else {
			q = r.Big()
		}
		pos, ok := slices.BinarySearch(xs, q)
		if ok {
			found++
		}
		isum = (isum + pos) % M
	}
	d = time.Since(t0)
	fmt.Println("binary search:", nq, found, isum)
	report("binary search 10M", d)
	xs = nil

	// 3. sort strings
	ns := 2000000 / div
	ss := []string{}
	for i := 0; i < ns; i++ {
		ss = append(ss, "key-"+itoa(r.Big()%1000000000000))
	}
	t0 = time.Now()
	slices.Sort(ss)
	d = time.Since(t0)
	ssum := 0
	for i := 0; i < ns; i += 101 {
		ssum = (ssum*31 + len(ss[i]) + int(ss[i][len(ss[i])-1])) % M
	}
	fmt.Println("sort strings:", ns, slices.IsSorted(ss), ss[0], ss[ns-1], ssum)
	report("sort 2M strings", d)
	ss = nil

	// 4. deque
	nd := 10000000 / div
	t0 = time.Now()
	dq := &Deque{}
	dsum := 0
	for k := 0; k < nd; k++ {
		dq.PushBack(k)
		if k%3 == 0 {
			dsum = (dsum + dq.PopFront()*3) % M
		}
	}
	maxLen := dq.n
	for dq.n > 0 {
		dsum = (dsum*7 + dq.PopFront()) % M
	}
	d = time.Since(t0)
	fmt.Println("deque:", nd, maxLen, dsum)
	report("deque 10M", d)

	// 5. heap of ints
	nh := 1000000 / div
	t0 = time.Now()
	h := &IntHeap{}
	for i := 0; i < nh; i++ {
		heap.Push(h, r.Big())
	}
	hsum, prev, ordered := 0, -1, true
	for h.Len() > 0 {
		x := heap.Pop(h).(int)
		if x < prev {
			ordered = false
		}
		prev = x
		hsum = (hsum*31 + x%M) % M
	}
	d = time.Since(t0)
	fmt.Println("heap:", nh, ordered, hsum)
	report("heap 1M", d)

	// 6. priority queue of jobs (unique priorities; highest first)
	t0 = time.Now()
	pq := &JobQueue{}
	for k := 0; k < nh; k++ {
		heap.Push(pq, Job{(r.Next() << 20) | k, k})
	}
	psum := 0
	for pq.Len() > 0 {
		j := heap.Pop(pq).(Job)
		psum = (psum*31 + j.Id) % M
	}
	d = time.Since(t0)
	fmt.Println("priority queue:", nh, psum)
	report("priority queue 1M", d)

	// 7. linked list
	nl := 1000000 / div
	t0 = time.Now()
	ll := list.New()
	for k := 0; k < nl; k++ {
		ll.PushBack(k)
		ll.PushFront(-k)
	}
	lsum := 0
	for e := ll.Front(); e != nil; e = e.Next() {
		lsum = (lsum*31 + e.Value.(int) + M) % M
	}
	d = time.Since(t0)
	fmt.Println("linked list:", ll.Len(), lsum)
	report("linked list 2M", d)

	// 8. ordered map
	no, nlk := 50000/div, 1000000/div
	t0 = time.Now()
	om := &OrderedMap{}
	keys := []int{}
	for k := 0; k < no; k++ {
		key := r.Big() % 10000000
		keys = append(keys, key)
		om.Set(key, k)
	}
	osum := 0
	for k := 0; k < nlk; k++ {
		osum = (osum + om.Get(keys[(k*31)%no])) % M
	}
	d = time.Since(t0)
	fmt.Println("ordered map:", len(om.keys), om.keys[0], om.keys[len(om.keys)-1], osum)
	report("ordered map 50k+1M", d)

	// 9. strings.Builder: 100 MB from ~10-byte pieces
	target := 100000000 / div
	t0 = time.Now()
	var b strings.Builder
	pieces := 0
	for k := 0; b.Len() < target; k++ {
		b.WriteString("item")
		b.WriteString(itoa(k % 100000))
		b.WriteString(";")
		pieces += 3
	}
	built := b.String()
	d = time.Since(t0)
	fmt.Println("builder:", len(built), pieces, built[0:20], built[len(built)-20:])
	report("builder 100MB", d)
	built = ""

	// 10. text/template, 1M rows
	nr := 1000000 / div
	words := []string{"lorem", "ipsum", "dolor", "sit", "amet", "elit", "sed", "magna"}
	rows := []Row{}
	for q := 0; q < nr; q++ {
		cents := r.Next() % 100000
		rows = append(rows, Row{q, words[q%8] + "-" + itoa(q), float64(cents) / 100.0, q%3 != 0})
	}
	page := Page{"report", rows}
	tpl := template.Must(template.New("t").Parse(
		"{{.Title}}\n{{range .Rows}}{{.Id}};{{.Name}};{{.Price}};{{if .Active}}yes{{else}}no{{end}}\n{{end}}"))
	t0 = time.Now()
	var ob strings.Builder
	if err := tpl.Execute(&ob, page); err != nil {
		panic(err)
	}
	out := ob.String()
	d = time.Since(t0)
	tsum := 0
	for i := 0; i < len(out); i += 1009 {
		tsum = (tsum*31 + int(out[i])) % M
	}
	fmt.Println("template:", nr, len(out), tsum)
	report("template 1M rows", d)
}
