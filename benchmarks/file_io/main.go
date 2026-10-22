package main

import (
	"bufio"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

// File I/O (os, bufio, io, path/filepath): write and read a 1 GB file in
// 1 MB chunks, the same through bufio.Writer / bufio.Reader with 1 KB
// calls, io.Copy of the file (copy_file_range on Linux), bufio.Scanner over
// a 1 GB text file line by line, and a directory tree of 100k small files:
// create, filepath.WalkDir with d.Info(), remove. Same work as main.zl.
// SCALE=small: 16 MB files, 2 000 small files.

func itoa(n int) string { return strconv.Itoa(n) }

func chunk() []byte {
	out := make([]byte, 1048576)
	s := 12345
	for i := range out {
		s = (s*1103515245 + 12345) % 2147483648
		out[i] = byte((s >> 16) & 255)
	}
	return out
}

func sample(sum int, buf []byte, n int, pos int) int {
	k := (pos + 4095) / 4096 * 4096
	s := sum
	for k < pos+n {
		s = (s*31 + int(buf[k-pos])) % 1000000007
		k += 4096
	}
	return s
}

func report(name string, d time.Duration, bytes int) {
	ns := d.Nanoseconds()
	if ns <= 0 {
		ns = 1
	}
	if bytes > 0 {
		fmt.Fprintf(os.Stderr, "%s: %d ms, %d MB/s\n", name, ns/1000000, int64(bytes)*1000/ns)
	} else {
		fmt.Fprintf(os.Stderr, "%s: %d ms\n", name, ns/1000000)
	}
}

func must(err error) {
	if err != nil {
		panic(err)
	}
}

func readChecked(path string, small bool) (int, int) {
	f, err := os.Open(path)
	must(err)
	total, sum := 0, 0
	var r io.Reader = f
	size := 1048576
	if small {
		r = bufio.NewReader(f)
		size = 1024
	}
	buf := make([]byte, size)
	for {
		n, err := r.Read(buf)
		if n > 0 {
			sum = sample(sum, buf, n, total)
			total += n
		}
		if err == io.EOF {
			break
		}
		must(err)
	}
	must(f.Close())
	return total, sum
}

func main() {
	mb, topDirs, subDirs := 1024, 100, 10
	if os.Getenv("SCALE") == "small" {
		mb, topDirs, subDirs = 16, 10, 2
	}
	root := filepath.Join(os.TempDir(), "go-bench-file_io-"+itoa(os.Getpid()))
	must(os.MkdirAll(root, 0o755))
	big := filepath.Join(root, "big.bin")
	buf := chunk()
	size := mb * 1048576

	// 1. write in 1 MB chunks
	t0 := time.Now()
	f, err := os.Create(big)
	must(err)
	for i := 0; i < mb; i++ {
		buf[0] = byte(i & 255)
		buf[1] = byte(i >> 8)
		_, err = f.Write(buf)
		must(err)
	}
	must(f.Close())
	report("write 1MB chunks", time.Since(t0), size)

	// 2. read in 1 MB chunks
	t0 = time.Now()
	n1, s1 := readChecked(big, false)
	d := time.Since(t0)
	fmt.Println("read chunks:", n1, s1)
	report("read 1MB chunks", d, size)

	// 3. buffered write, 1 KB per call
	big2 := filepath.Join(root, "big2.bin")
	t0 = time.Now()
	f2, err := os.Create(big2)
	must(err)
	w := bufio.NewWriter(f2)
	for i := 0; i < mb; i++ {
		buf[0] = byte(i & 255)
		buf[1] = byte(i >> 8)
		for off := 0; off < 1048576; off += 1024 {
			_, err = w.Write(buf[off : off+1024])
			must(err)
		}
	}
	must(w.Flush())
	must(f2.Close())
	report("buffered write 1KB", time.Since(t0), size)
	must(os.Remove(big2))

	// 4. buffered read, 1 KB per call
	t0 = time.Now()
	n2, s2 := readChecked(big, true)
	d = time.Since(t0)
	fmt.Println("read buffered:", n2, s2)
	report("buffered read 1KB", d, size)

	// 5. io.Copy
	cp := filepath.Join(root, "copy.bin")
	t0 = time.Now()
	src, err := os.Open(big)
	must(err)
	dst, err := os.Create(cp)
	must(err)
	copied, err := io.Copy(dst, src)
	must(err)
	must(dst.Close())
	must(src.Close())
	d = time.Since(t0)
	st, err := os.Stat(cp)
	must(err)
	fmt.Println("copy:", copied, st.Size())
	report("io.Copy", d, size)
	must(os.Remove(cp))
	must(os.Remove(big))

	// 6. line scan of a text file (lines of 1..15 words)
	words := []string{
		"lorem", "ipsum", "dolor", "sit", "amet", "consectetur", "adipiscing", "elit", "sed", "do",
		"eiusmod", "tempor", "incididunt", "ut", "labore", "et", "dolore", "magna", "aliqua", "veniam",
	}
	text := filepath.Join(root, "text.txt")
	tf, err := os.Create(text)
	must(err)
	tw := bufio.NewWriter(tf)
	seed, written, lines := 999, 0, 0
	for written < size {
		seed = (seed*1103515245 + 12345) % 2147483648
		nw := 1 + (seed>>8)%15
		var line strings.Builder
		for k := 0; k < nw; k++ {
			seed = (seed*1103515245 + 12345) % 2147483648
			if k > 0 {
				line.WriteString(" ")
			}
			line.WriteString(words[(seed>>8)%20])
		}
		line.WriteString("\n")
		s := line.String()
		_, err = tw.WriteString(s)
		must(err)
		written += len(s)
		lines++
	}
	must(tw.Flush())
	must(tf.Close())
	t0 = time.Now()
	rf, err := os.Open(text)
	must(err)
	sc := bufio.NewScanner(rf)
	nlines, nbytes, longest := 0, 0, 0
	for sc.Scan() {
		b := sc.Bytes()
		nlines++
		nbytes += len(b)
		if len(b) > longest {
			longest = len(b)
		}
	}
	must(sc.Err())
	must(rf.Close())
	d = time.Since(t0)
	fmt.Println("scan:", nlines, nbytes, longest, nlines == lines)
	report("scan lines", d, written)
	must(os.Remove(text))

	// 7. tree of small files
	tree := filepath.Join(root, "tree")
	t0 = time.Now()
	made := 0
	for dd := 0; dd < topDirs; dd++ {
		for s := 0; s < subDirs; s++ {
			sub := filepath.Join(tree, "d"+itoa(dd)+"/s"+itoa(s))
			must(os.MkdirAll(sub, 0o755))
			for k := 0; k < 100; k++ {
				must(os.WriteFile(filepath.Join(sub, "f"+itoa(k)+".txt"), []byte(strings.Repeat("x", (made*7)%200)), 0o644))
				made++
			}
		}
	}
	report("create files", time.Since(t0), 0)
	t0 = time.Now()
	files, bytes, dirs := 0, 0, 0
	must(filepath.WalkDir(tree, func(path string, e fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if path == tree {
			return nil
		}
		if e.IsDir() {
			dirs++
			return nil
		}
		info, err := e.Info()
		if err != nil {
			return err
		}
		files++
		bytes += int(info.Size())
		return nil
	}))
	d = time.Since(t0)
	fmt.Println("walk:", files, dirs, bytes)
	report("walk + stat", d, 0)
	t0 = time.Now()
	must(os.RemoveAll(root))
	report("remove tree", time.Since(t0), 0)
}
