package main

// Micro-benchmarks of language mechanisms (same rows and checksums as
// main.zl). stdout: "<row> <n> <checksum>", stderr: "us <row> <microseconds>".
//   SCALE=small  divides every iteration count by 100 (smoke test)

import (
	"errors"
	"fmt"
	"os"
	"time"
)

type Op interface{ Apply(x int) int }

type AddOp struct{ K int }

func (o *AddOp) Apply(x int) int { return (x + o.K) & 1023 }

type XorOp struct{ K int }

func (o *XorOp) Apply(x int) int { return (x ^ o.K) & 1023 }

type Counter struct{ N int }

func (c *Counter) Inc() { c.N++ }

type Valuer interface{ Value() int }

type Val struct{ N int }

func (v *Val) Value() int { return v.N }

var errOdd = errors.New("odd")

var start = time.Now()

func now() int64 { return int64(time.Since(start)) }

func report(name string, n int, t0 int64, sum int) {
	fmt.Fprintf(os.Stderr, "us %s %d\n", name, (now()-t0)/1000)
	fmt.Println(name, n, sum)
}

func direct(n int) {
	a := AddOp{K: 7}
	b := XorOp{K: 5}
	t0 := now()
	s := 0
	for i := 0; i < n; i++ {
		if i&1 == 0 {
			s += a.Apply(i)
		} else {
			s += b.Apply(i)
		}
	}
	report("direct_call", n, t0, s)
}

func iface(n int) {
	ops := []Op{&AddOp{K: 7}, &XorOp{K: 5}}
	t0 := now()
	s := 0
	for i := 0; i < n; i++ {
		s += ops[i&1].Apply(i)
	}
	report("iface_call", n, t0, s)
}

func closures(n int) {
	k := 7
	fs := []func(int) int{
		func(x int) int { return (x + k) & 1023 },
		func(x int) int { return (x ^ (k - 2)) & 1023 },
	}
	t0 := now()
	s := 0
	for i := 0; i < n; i++ {
		s += fs[i&1](i)
	}
	report("closure_call", n, t0, s)
}

func withDefer(c *Counter, i int) int {
	defer c.Inc()
	return i & 3
}

func noDefer(c *Counter, i int) int {
	r := i & 3
	c.Inc()
	return r
}

func defers(n int) {
	c := Counter{}
	t0 := now()
	s := 0
	for i := 0; i < n; i++ {
		s += withDefer(&c, i)
	}
	report("defer_call", n, t0, s+c.N)
	d := Counter{}
	t0 = now()
	s = 0
	for i := 0; i < n; i++ {
		s += noDefer(&d, i)
	}
	report("no_defer_call", n, t0, s+d.N)
}

func leafNew(i int) (int, error) {
	if i&7 == 0 {
		return 0, errors.New("odd")
	}
	return i & 15, nil
}

func midNew(i int) (int, error) {
	v, err := leafNew(i)
	if err != nil {
		return 0, err
	}
	return v + 1, nil
}

func topNew(i int) (int, error) {
	v, err := midNew(i)
	if err != nil {
		return 0, err
	}
	return v * 2, nil
}

func leafConst(i int) (int, error) {
	if i&7 == 0 {
		return 0, errOdd
	}
	return i & 15, nil
}

func midConst(i int) (int, error) {
	v, err := leafConst(i)
	if err != nil {
		return 0, err
	}
	return v + 1, nil
}

func topConst(i int) (int, error) {
	v, err := midConst(i)
	if err != nil {
		return 0, err
	}
	return v * 2, nil
}

func errorsNew(n int) {
	t0 := now()
	s, fails := 0, 0
	for i := 0; i < n; i++ {
		v, err := topNew(i)
		if err != nil {
			fails += len(err.Error())
			continue
		}
		s += v
	}
	report("error_new", n, t0, s+fails)
}

func errorsConst(n int) {
	t0 := now()
	s, fails := 0, 0
	for i := 0; i < n; i++ {
		v, err := topConst(i)
		if err != nil {
			fails += len(err.Error())
			continue
		}
		s += v
	}
	report("error_sentinel", n, t0, s+fails)
}

type Number interface{ ~int | ~float64 }

func SumG[T Number](xs []T, zero T) T {
	s := zero
	for _, x := range xs {
		s = s + x
	}
	return s
}

func SumI(xs []Valuer) int {
	s := 0
	for _, x := range xs {
		s += x.Value()
	}
	return s
}

func generics(n, passes int) {
	var xs []int
	var vs []Valuer
	for i := 0; i < n; i++ {
		xs = append(xs, i&255)
		vs = append(vs, &Val{N: i & 255})
	}
	t0 := now()
	s := 0
	for p := 0; p < passes; p++ {
		s += SumG(xs, 0)
	}
	report("generic_sum", n*passes, t0, s)
	t0 = now()
	s = 0
	for p := 0; p < passes; p++ {
		s += SumI(vs)
	}
	report("iface_sum", n*passes, t0, s)
}

func main() {
	scale := 1
	if os.Getenv("SCALE") == "small" {
		scale = 100
	}
	calls := 100000000 / scale
	direct(calls)
	iface(calls)
	closures(calls)
	defers(calls / 5)
	errorsNew(calls / 5)
	errorsConst(calls / 5)
	generics(1000000/scale, 100)
}
