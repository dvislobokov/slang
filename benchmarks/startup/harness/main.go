// Startup harness of benchmarks/startup (a small `perf stat -r`):
//
//	harness -genconfig file      writes the 1 MB JSON configuration of ../cli
//	harness [-n 300] label=cmd...
//
// For every label=cmd (cmd split on spaces; a "stderr:" prefix watches
// stderr instead of stdout), runs it n times, the commands alternating, and
// measures the time from just before exec (fork+exec included) to the
// first byte on the watched stream. A process that keeps running after its
// first line (a server) is killed then; others are waited for. Prints the
// first output line of each command once (to check that the
// implementations agree), then min, median, p90 and mean in ms.
package main

import (
	"bufio"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"os"
	"os/exec"
	"sort"
	"strings"
	"time"
)

type target struct {
	label  string
	args   []string
	stderr bool
	first  string
	times  []time.Duration
}

func once(t *target) {
	cmd := exec.Command(t.args[0], t.args[1:]...)
	var r io.ReadCloser
	var err error
	if t.stderr {
		r, err = cmd.StderrPipe()
	} else {
		r, err = cmd.StdoutPipe()
	}
	if err != nil {
		panic(err)
	}
	t0 := time.Now()
	if err := cmd.Start(); err != nil {
		panic(err)
	}
	br := bufio.NewReader(r)
	if _, err := br.Peek(1); err != nil {
		panic(fmt.Sprintf("%s: no output: %v", t.label, err))
	}
	t.times = append(t.times, time.Since(t0))
	line, _ := br.ReadString('\n')
	if t.first == "" {
		t.first = strings.TrimSpace(line)
	}
	if t.stderr {
		cmd.Process.Kill()
		io.Copy(io.Discard, br)
		cmd.Wait()
		return
	}
	io.Copy(io.Discard, br)
	if err := cmd.Wait(); err != nil {
		panic(fmt.Sprintf("%s: %v", t.label, err))
	}
}

func ms(d time.Duration) float64 { return float64(d.Microseconds()) / 1000 }

type route struct {
	Path      string
	Upstream  string
	TimeoutMs int
	Retries   int
	Tags      []string
}

// genConfig writes about 1 MB of compact JSON: a header and routes until
// the size.
func genConfig(path string) {
	cfg := struct {
		Name    string
		Version int
		Debug   bool
		Routes  []route
	}{Name: "gateway", Version: 3, Debug: false}
	size := 0
	for i := 0; size < 1<<20; i++ {
		r := route{
			Path:      fmt.Sprintf("/api/v%d/service-%d/items/{id}", i%3+1, i),
			Upstream:  fmt.Sprintf("http://backend-%d.internal:%d", i%17, 8000+i%100),
			TimeoutMs: 100 + (i*37)%4900,
			Retries:   i % 4,
			Tags:      []string{"team-" + fmt.Sprint(i%9), "tier-" + fmt.Sprint(i%3)},
		}
		b, _ := json.Marshal(r)
		size += len(b) + 1
		cfg.Routes = append(cfg.Routes, r)
	}
	b, err := json.Marshal(cfg)
	if err != nil {
		panic(err)
	}
	if err := os.WriteFile(path, b, 0o644); err != nil {
		panic(err)
	}
}

func main() {
	n := flag.Int("n", 300, "runs per command")
	gen := flag.String("genconfig", "", "write the configuration file and exit")
	flag.Parse()
	if *gen != "" {
		genConfig(*gen)
		return
	}
	var ts []*target
	for _, a := range flag.Args() {
		label, cmd, ok := strings.Cut(a, "=")
		if !ok {
			panic("want label=cmd: " + a)
		}
		t := &target{label: label}
		if c, ok := strings.CutPrefix(cmd, "stderr:"); ok {
			t.stderr, cmd = true, c
		}
		t.args = strings.Fields(cmd)
		ts = append(ts, t)
	}
	for i := 0; i < *n; i++ {
		for _, t := range ts {
			once(t)
		}
	}
	fmt.Printf("%-16s %8s %8s %8s %8s  %s\n", "program", "min_ms", "med_ms", "p90_ms", "mean_ms", "first line")
	for _, t := range ts {
		sort.Slice(t.times, func(a, b int) bool { return t.times[a] < t.times[b] })
		var sum time.Duration
		for _, d := range t.times {
			sum += d
		}
		k := len(t.times)
		fmt.Printf("%-16s %8.3f %8.3f %8.3f %8.3f  %s\n", t.label, ms(t.times[0]), ms(t.times[k/2]), ms(t.times[k*9/10]),
			ms(sum/time.Duration(k)), t.first)
	}
}
