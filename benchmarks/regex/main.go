package main

import (
	"fmt"
	"os"
	"regexp"
	"strings"
	"time"
)

// Finds all e-mail addresses in a generated 10 MB text (same text as main.sl).
func main() {
	words := []string{"lorem", "ipsum", "dolor", "sit", "amet", "consectetur", "adipiscing", "elit", "sed", "do", "eiusmod", "tempor", "incididunt", "ut", "labore", "et", "dolore", "magna", "aliqua", "x@y"}
	names := []string{"john.smith", "alice", "bob_42", "carol+news", "dave-ops"}
	hosts := []string{"example.com", "mail.example.org", "test.io", "uni.edu", "corp.co.uk"}
	var b strings.Builder
	seed := 12345
	n := 0
	for n < 10000000 {
		seed = (seed*1103515245 + 12345) % 2147483648
		r := seed >> 8
		w := ""
		if r%50 == 0 {
			w = names[r%5] + "@" + hosts[(r>>3)%5]
		} else {
			w = words[r%20]
		}
		b.WriteString(w)
		b.WriteString(" ")
		n += len(w) + 1
	}
	text := b.String()
	re := regexp.MustCompile(`[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}`)
	t0 := time.Now()
	all := re.FindAllString(text, -1)
	total := 0
	for _, m := range all {
		total += len(m)
	}
	ms := time.Since(t0).Milliseconds()
	fmt.Println(len(text), len(all), total)
	fmt.Fprintf(os.Stderr, "regex: %d ms\n", ms)
}
