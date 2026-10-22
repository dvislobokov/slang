// logging, Go side: the scenarios of main.zl with zap (default), zerolog
// (-impl=zerolog) or log/slog (-impl=slog), each with a JSON encoder.
// Usage: main [-impl=zap|zerolog|slog] [file]  (default /tmp/zell-bench.log).
// RECORDS records per scenario (default 5M, SCALE=small: 100k). stdout:
// the record counts (equal to main.zl); stderr: "scenario <name>
// <records/s> <allocs/record>" (allocations: runtime.MemStats.Mallocs).
package main

import (
	"context"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"os"
	"runtime"
	"strconv"
	"time"

	"github.com/rs/zerolog"
	"go.uber.org/zap"
	"go.uber.org/zap/zapcore"
)

var methods = []string{"GET", "POST", "PUT", "DELETE"}
var statuses = []int{200, 201, 404, 500, 302}

func mallocs() uint64 {
	var m runtime.MemStats
	runtime.ReadMemStats(&m)
	return m.Mallocs
}

func report(name string, n int, t0 time.Time, a0 uint64) {
	secs := time.Since(t0).Seconds()
	fmt.Fprintf(os.Stderr, "scenario %s %d %g\n", name, int(float64(n)/secs), float64(mallocs()-a0)/float64(n))
	fmt.Println(name, n)
}

// A logger of one implementation writing to w with the two context
// properties (Service, RequestId) bound, and its Info/Debug calls.
type logger struct {
	info, debug func(i int, path string)
	close       func()
}

func newLogger(impl string, w io.Writer, closer func()) logger {
	switch impl {
	case "zap":
		enc := zap.NewProductionEncoderConfig()
		enc.TimeKey = "@t"
		enc.EncodeTime = zapcore.ISO8601TimeEncoder
		core := zapcore.NewCore(zapcore.NewJSONEncoder(enc), zapcore.AddSync(w), zapcore.InfoLevel)
		l := zap.New(core).With(zap.String("Service", "bench"), zap.String("RequestId", "req-1"))
		return logger{
			info: func(i int, path string) {
				l.Info("HTTP request", zap.String("Method", methods[i%4]), zap.String("Path", path),
					zap.Int("Status", statuses[i%5]), zap.Float64("Elapsed", float64(i%100)*0.25))
			},
			debug: func(i int, path string) {
				l.Debug("HTTP request", zap.String("Method", methods[i%4]), zap.String("Path", path),
					zap.Int("Status", statuses[i%5]), zap.Float64("Elapsed", float64(i%100)*0.25))
			},
			close: func() { l.Sync(); closer() },
		}
	case "zerolog":
		zerolog.TimeFieldFormat = time.RFC3339Nano
		l := zerolog.New(w).Level(zerolog.InfoLevel).With().Timestamp().Str("Service", "bench").Str("RequestId", "req-1").Logger()
		return logger{
			info: func(i int, path string) {
				l.Info().Str("Method", methods[i%4]).Str("Path", path).Int("Status", statuses[i%5]).
					Float64("Elapsed", float64(i%100)*0.25).Msg("HTTP request")
			},
			debug: func(i int, path string) {
				l.Debug().Str("Method", methods[i%4]).Str("Path", path).Int("Status", statuses[i%5]).
					Float64("Elapsed", float64(i%100)*0.25).Msg("HTTP request")
			},
			close: closer,
		}
	case "slog":
		l := slog.New(slog.NewJSONHandler(w, &slog.HandlerOptions{Level: slog.LevelInfo})).With("Service", "bench", "RequestId", "req-1")
		ctx := context.Background()
		return logger{
			info: func(i int, path string) {
				l.LogAttrs(ctx, slog.LevelInfo, "HTTP request", slog.String("Method", methods[i%4]), slog.String("Path", path),
					slog.Int("Status", statuses[i%5]), slog.Float64("Elapsed", float64(i%100)*0.25))
			},
			debug: func(i int, path string) {
				l.LogAttrs(ctx, slog.LevelDebug, "HTTP request", slog.String("Method", methods[i%4]), slog.String("Path", path),
					slog.Int("Status", statuses[i%5]), slog.Float64("Elapsed", float64(i%100)*0.25))
			},
			close: closer,
		}
	}
	panic("unknown -impl " + impl)
}

func open(path string) (*os.File, func()) {
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_APPEND, 0o644)
	if err != nil {
		panic(err)
	}
	return f, func() { f.Close() }
}

func run(impl, name, path string, paths []string, n int) {
	f, closer := open(path)
	l := newLogger(impl, f, closer)
	t0, a0 := time.Now(), mallocs()
	for i := 0; i < n; i++ {
		l.info(i, paths[i%1000])
	}
	l.close()
	report(name, n, t0, a0)
}

func main() {
	impl := flag.String("impl", "zap", "zap, zerolog or slog")
	flag.Parse()
	n := 5000000
	if os.Getenv("SCALE") == "small" {
		n = 100000
	}
	if v := os.Getenv("RECORDS"); v != "" {
		if x, err := strconv.Atoi(v); err == nil {
			n = x
		}
	}
	file := "/tmp/zell-bench.log"
	if flag.NArg() > 0 {
		file = flag.Arg(0)
	}
	os.Remove(file)
	paths := make([]string, 1000)
	for i := range paths {
		paths[i] = "/api/items/" + strconv.Itoa(i)
	}
	run(*impl, "devnull", "/dev/null", paths, n)
	run(*impl, "file", file, paths, n)

	f, closer := open("/dev/null")
	off := newLogger(*impl, f, closer)
	t0, a0 := time.Now(), mallocs()
	for i := 0; i < n; i++ {
		off.debug(i, paths[i%1000])
	}
	report("disabled", n, t0, a0)
	off.close()
}
