namespace examples.stdlib_formats

import std
import std.io

// Tier-2 formats: a TOML config read into a struct and edited in place
// (comments kept), CSV records into structs and back, an HTML template
// checked at compile time, a text table, and a gzip round trip.

struct Server {
    Host string
    Port int
    @toml(name: "debug_mode")
    Debug bool
}

struct Config {
    Name   string
    Server Server
    Tags   []string
}

struct Item {
    @csv(name: "sku")
    Sku   string
    Qty   int
    Price float
}

struct Page {
    Title string
    Items []Item
}

func main() void! {
    var src = `# service config
Name = "shop"
Tags = ["web", "eu"]

[Server]
Host = "0.0.0.0"   # all interfaces
Port = 8080
debug_mode = false
`
    var cfg = encoding.toml.Parse<Config>(src)?
    println(cfg.Name, cfg.Server.Host, cfg.Server.Port, cfg.Server.Debug, len(cfg.Tags))

    // Document edits the text: comments and layout stay as they were.
    var doc = encoding.toml.ParseDocument(src)?
    doc.Set("Server.Port", encoding.toml.NewInt(9090))?
    doc.Set("Server.Timeout", encoding.toml.NewString("30s"))?
    print(doc.String())

    // CSV <-> structs (header names from the field names or @csv(name:)).
    var items = encoding.csv.Read<Item>("sku,Qty,Price\nA-1,3,1.5\nB-7,10,0.25\n\"C,9\",1,99\n")?
    var total = 0.0
    for it in items {
        total += float(it.Qty) * it.Price
    }
    println(len(items), "items, total", total)
    print(encoding.csv.Write<Item>(items))

    // The template literal is checked against Page when compiling: a
    // misspelled {{.Titel}} would be a build error.
    var page = html.template.Parse<Page>(
        `<h1>{{.Title}}</h1>
<ul>{{range .Items}}<li><a href="/item?sku={{.Sku}}">{{.Sku}}</a> x{{.Qty}}</li>{{end}}</ul>
`,
    )?
    print(page.Execute(Page{Title: "Tom & Jerry's <shop>", Items: items})?)

    var t = text.table.New(["SKU", "Qty", "Price"])
    t.Style = text.table.Unicode
    t.SetAlign(1, text.table.Right)
    t.SetAlign(2, text.table.Right)
    for it in items {
        t.AddRow([it.Sku, fmt.Format("{0}", it.Qty), fmt.Sprintf("%.2f", it.Price)])
    }
    print(t.String())

    var text = strings.Repeat("all work and no play makes jack a dull boy\n", 1000)
    var gz = compress.gzip.Compress(strings.Bytes(text), compress.BestCompression)
    var back = compress.gzip.Decompress(gz)?
    println(len(text), "->", len(gz), "bytes, round trip ok:", strings.FromBytes(back) == text)
}
