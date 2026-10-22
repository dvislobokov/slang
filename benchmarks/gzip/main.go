package main

import (
	"bytes"
	"compress/gzip"
	"fmt"
	"io"
	"os"
	"strings"
	"time"
)

// gzip level 6: compress then decompress a generated 20 MB text (same text as main.zl).
func main() {
	words := []string{"lorem", "ipsum", "dolor", "sit", "amet", "consectetur", "adipiscing", "elit", "sed", "do", "eiusmod", "tempor", "incididunt", "ut", "labore", "et", "dolore", "magna", "aliqua", "1234567"}
	var b strings.Builder
	seed := 12345
	n := 0
	for n < 20000000 {
		seed = (seed*1103515245 + 12345) % 2147483648
		w := words[(seed>>8)%20]
		b.WriteString(w)
		b.WriteString(" ")
		n += len(w) + 1
	}
	data := []byte(b.String())
	t0 := time.Now()
	var c bytes.Buffer
	zw, _ := gzip.NewWriterLevel(&c, 6)
	zw.Write(data)
	zw.Close()
	t1 := time.Now()
	zr, err := gzip.NewReader(bytes.NewReader(c.Bytes()))
	if err != nil {
		panic(err)
	}
	d, err := io.ReadAll(zr)
	if err != nil {
		panic(err)
	}
	t2 := time.Now()
	fmt.Println(len(data), c.Len(), len(d) == len(data))
	fmt.Fprintf(os.Stderr, "compress %d ms, decompress %d ms\n", t1.Sub(t0).Milliseconds(), t2.Sub(t1).Milliseconds())
}
