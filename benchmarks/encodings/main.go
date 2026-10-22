package main

import (
	"encoding/xml"
	"fmt"
	"math"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/BurntSushi/toml"
	"gopkg.in/yaml.v3"
)

// encoding/xml, gopkg.in/yaml.v3, github.com/BurntSushi/toml: a generated
// ~20 MB document of each format parsed into typed structs, formatted back
// and parsed again to check the round trip. Same data as main.zl.
// SCALE=small uses 200 KB documents.

type Catalog struct {
	XMLName xml.Name `xml:"catalog" yaml:"-" toml:"-"`
	Items   []Item   `xml:"item" yaml:"items" toml:"items"`
}

type Item struct {
	Id     int      `xml:"id,attr" yaml:"id" toml:"id"`
	Name   string   `xml:"name" yaml:"name" toml:"name"`
	Price  float64  `xml:"price" yaml:"price" toml:"price"`
	Qty    int      `xml:"qty" yaml:"qty" toml:"qty"`
	Active bool     `xml:"active" yaml:"active" toml:"active"`
	Tags   []string `xml:"tag" yaml:"tags" toml:"tags"`
}

type Rng struct{ s int }

func (r *Rng) Next() int {
	r.s = (r.s*1103515245 + 12345) % 2147483648
	return r.s >> 8
}

var ws = []string{
	"lorem", "ipsum", "dolor", "sit", "amet", "consectetur", "adipiscing", "elit", "sed", "do",
	"eiusmod", "tempor", "incididunt", "ut", "labore", "et", "dolore", "magna", "aliqua", "veniam",
}

func itoa(n int) string { return strconv.Itoa(n) }

func money(cents int) string {
	c := cents % 100
	if c < 10 {
		return itoa(cents/100) + ".0" + itoa(c)
	}
	return itoa(cents/100) + "." + itoa(c)
}

type Rec struct {
	id, name, price, qty, active string
	tags                         []string
}

func rec(r *Rng, i int) Rec {
	name := ws[r.Next()%20]
	name = name + "-" + ws[r.Next()%20] + "-" + itoa(i)
	price := money(r.Next() % 100000)
	qty := itoa(r.Next() % 1000)
	active := "false"
	if r.Next()%2 == 0 {
		active = "true"
	}
	tags := []string{}
	nt := r.Next() % 4
	for t := 0; t < nt; t++ {
		tags = append(tags, ws[r.Next()%20])
	}
	return Rec{itoa(i*7 + 1), name, price, qty, active, tags}
}

// format: 0 = XML, 1 = YAML, 2 = TOML.
func gen(format int, target int) string {
	r := Rng{4242}
	var b strings.Builder
	switch format {
	case 0:
		b.WriteString("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<catalog>\n")
	case 1:
		b.WriteString("# generated catalog\nitems:\n")
	default:
		b.WriteString("# generated catalog\n")
	}
	for i := 0; b.Len() < target; i++ {
		x := rec(&r, i)
		switch format {
		case 0:
			b.WriteString("  <item id=\"" + x.id + "\"><name>" + x.name + "</name><price>" + x.price)
			b.WriteString("</price><qty>" + x.qty + "</qty><active>" + x.active + "</active>")
			for _, t := range x.tags {
				b.WriteString("<tag>" + t + "</tag>")
			}
			b.WriteString("</item>\n")
		case 1:
			b.WriteString("  - id: " + x.id + "\n    name: " + x.name + "\n    price: " + x.price)
			b.WriteString("\n    qty: " + x.qty + "\n    active: " + x.active + "\n    tags: [")
			b.WriteString(strings.Join(x.tags, ", "))
			b.WriteString("]\n")
		default:
			b.WriteString("[[items]]\nid = " + x.id + "\nname = \"" + x.name + "\"\nprice = " + x.price)
			b.WriteString("\nqty = " + x.qty + "\nactive = " + x.active + "\ntags = [")
			for k, t := range x.tags {
				if k > 0 {
					b.WriteString(", ")
				}
				b.WriteString("\"" + t + "\"")
			}
			b.WriteString("]\n\n")
		}
	}
	if format == 0 {
		b.WriteString("</catalog>\n")
	}
	return b.String()
}

func checksum(c Catalog) string {
	sumId, sumQty, sumCents, active, tags, bytes := 0, 0, 0, 0, 0, 0
	for _, it := range c.Items {
		sumId += it.Id
		sumQty += it.Qty
		sumCents += int(math.Round(it.Price * 100.0))
		if it.Active {
			active++
		}
		tags += len(it.Tags)
		bytes += len(it.Name)
		for _, t := range it.Tags {
			bytes += len(t)
		}
	}
	return fmt.Sprintf("items=%d id=%d qty=%d cents=%d active=%d tags=%d bytes=%d",
		len(c.Items), sumId, sumQty, sumCents, active, tags, bytes)
}

func parse(format int, text string) Catalog {
	var c Catalog
	var err error
	switch format {
	case 0:
		err = xml.Unmarshal([]byte(text), &c)
	case 1:
		err = yaml.Unmarshal([]byte(text), &c)
	default:
		_, err = toml.Decode(text, &c)
	}
	if err != nil {
		panic(err)
	}
	return c
}

func format(f int, c Catalog) string {
	var out []byte
	var err error
	switch f {
	case 0:
		out, err = xml.Marshal(c)
	case 1:
		out, err = yaml.Marshal(c)
	default:
		var b strings.Builder
		err = toml.NewEncoder(&b).Encode(c)
		out = []byte(b.String())
	}
	if err != nil {
		panic(err)
	}
	return string(out)
}

func mbs(bytes int, d time.Duration) int64 {
	n := d.Nanoseconds()
	if n <= 0 {
		n = 1
	}
	return int64(bytes) * 1000 / n
}

func main() {
	size := 20000000
	if os.Getenv("SCALE") == "small" {
		size = 200000
	}
	only := os.Getenv("FORMAT")
	names := []string{"xml", "yaml", "toml"}
	for f := 0; f < 3; f++ {
		if only != "" && only != names[f] {
			continue
		}
		text := gen(f, size)
		t0 := time.Now()
		c := parse(f, text)
		dParse := time.Since(t0)
		sum := checksum(c)
		fmt.Println(names[f], "parse:", sum)
		t2 := time.Now()
		out := format(f, c)
		dFormat := time.Since(t2)
		again := parse(f, out)
		fmt.Println(names[f], "roundtrip:", checksum(again) == sum)
		fmt.Fprintf(os.Stderr, "%s: %d bytes, parse %d ms (%d MB/s), format %d ms (%d bytes, %d MB/s)\n",
			names[f], len(text), dParse.Milliseconds(), mbs(len(text), dParse),
			dFormat.Milliseconds(), len(out), mbs(len(out), dFormat))
	}
}
