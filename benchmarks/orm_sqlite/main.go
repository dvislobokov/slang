// orm_sqlite, Go side: the phases of main.zl with GORM (default) or plain
// database/sql (-impl=sql), both over mattn/go-sqlite3 (cgo, its bundled
// SQLite). Usage: main [-impl=gorm|sql] [dsn]  (default ":memory:"; a file
// path is deleted first). SCALE=small: N=10k, L=20k, Q=1k, U=1k.
// Checksums on stdout (equal to main.zl), "phase <name> <ms>" on stderr.
package main

import (
	"database/sql"
	"flag"
	"fmt"
	"os"
	"strings"
	"time"

	_ "github.com/mattn/go-sqlite3"
	"gorm.io/driver/sqlite"
	"gorm.io/gorm"
	"gorm.io/gorm/logger"
)

type Product struct {
	Id       int `gorm:"primaryKey"`
	Name     string
	Category string `gorm:"index:ix_products_category"`
	Price    int
	Stock    int
}

var cats = []string{"a", "b", "c", "d", "e", "f", "g", "h"}

var t0 time.Time

func phase(name string) {
	fmt.Fprintf(os.Stderr, "phase %s %.3f\n", name, float64(time.Since(t0).Microseconds())/1000)
	t0 = time.Now()
}

func check(err error) {
	if err != nil {
		panic(err)
	}
}

type sizes struct{ n, lookups, queries, updates int }

func newProduct(j int) Product {
	return Product{Name: fmt.Sprintf("p-%d", j), Category: cats[j%8], Price: (j * 7919) % 100000, Stock: j % 100}
}

func runGorm(dsn string, s sizes) {
	db, err := gorm.Open(sqlite.Open(dsn), &gorm.Config{Logger: logger.Discard})
	check(err)
	sqlDB, _ := db.DB()
	if dsn == ":memory:" {
		sqlDB.SetMaxOpenConns(1)
	}
	check(db.AutoMigrate(&Product{}))
	phase("migrate")

	for i := 0; i < s.n; i += 1000 {
		rows := make([]Product, 0, 1000)
		for j := i; j < i+1000; j++ {
			rows = append(rows, newProduct(j))
		}
		check(db.Create(&rows).Error)
	}
	phase("insert")

	sum := 0
	for k := 0; k < s.lookups; k++ {
		var p Product
		check(db.Take(&p, (k*7919)%s.n+1).Error)
		sum += p.Price + p.Stock
	}
	phase("lookup")
	fmt.Println("lookup", sum)

	found, idsum := 0, 0
	for k := 0; k < s.queries; k++ {
		var rows []Product
		check(db.Where("category = ? AND price > ?", cats[k%8], (k*37)%100000).Order("id").Limit(10).Find(&rows).Error)
		found += len(rows)
		for _, r := range rows {
			idsum += r.Id
		}
	}
	phase("filter")
	fmt.Println("filter", found, idsum)

	changed := 0
	for lo := 1; changed < s.updates; lo += 100 {
		var rows []Product
		check(db.Where("id >= ? AND id < ?", lo, lo+100).Find(&rows).Error)
		check(db.Transaction(func(tx *gorm.DB) error {
			for i := range rows {
				rows[i].Stock++
				rows[i].Name += "*"
				// GORM has no change tracking: update the changed columns.
				if err := tx.Model(&rows[i]).Updates(map[string]any{"stock": rows[i].Stock, "name": rows[i].Name}).Error; err != nil {
					return err
				}
			}
			return nil
		}))
		changed += len(rows)
	}
	phase("update")
	var stock, starred, count int64
	check(db.Model(&Product{}).Select("coalesce(sum(stock), 0)").Scan(&stock).Error)
	check(db.Model(&Product{}).Where("name LIKE ?", "%*").Count(&starred).Error)
	check(db.Model(&Product{}).Count(&count).Error)
	fmt.Println("update", changed, stock, starred)
	fmt.Println("rows", count)
}

func runSQL(dsn string, s sizes) {
	db, err := sql.Open("sqlite3", dsn)
	check(err)
	if dsn == ":memory:" {
		db.SetMaxOpenConns(1)
	}
	// the migration, recorded like data's __migrations
	tx, err := db.Begin()
	check(err)
	for _, q := range []string{
		`CREATE TABLE IF NOT EXISTS __migrations (id TEXT PRIMARY KEY, applied_at TEXT NOT NULL)`,
		`CREATE TABLE products (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, category TEXT NOT NULL, price INTEGER NOT NULL, stock INTEGER NOT NULL)`,
		`CREATE INDEX ix_products_category ON products (category)`,
	} {
		_, err = tx.Exec(q)
		check(err)
	}
	_, err = tx.Exec(`INSERT INTO __migrations (id, applied_at) VALUES (?, datetime('now'))`, "20260101000000_CreateProducts")
	check(err)
	check(tx.Commit())
	phase("migrate")

	values := strings.TrimSuffix(strings.Repeat("(?, ?, ?, ?),", 1000), ",")
	for i := 0; i < s.n; i += 1000 {
		args := make([]any, 0, 4000)
		for j := i; j < i+1000; j++ {
			p := newProduct(j)
			args = append(args, p.Name, p.Category, p.Price, p.Stock)
		}
		tx, err := db.Begin()
		check(err)
		_, err = tx.Exec("INSERT INTO products (name, category, price, stock) VALUES "+values, args...)
		check(err)
		check(tx.Commit())
	}
	phase("insert")

	get, err := db.Prepare("SELECT id, name, category, price, stock FROM products WHERE id = ?")
	check(err)
	sum := 0
	for k := 0; k < s.lookups; k++ {
		var p Product
		check(get.QueryRow((k*7919)%s.n+1).Scan(&p.Id, &p.Name, &p.Category, &p.Price, &p.Stock))
		sum += p.Price + p.Stock
	}
	phase("lookup")
	fmt.Println("lookup", sum)

	filter, err := db.Prepare("SELECT id, name, category, price, stock FROM products WHERE category = ? AND price > ? ORDER BY id LIMIT 10")
	check(err)
	found, idsum := 0, 0
	for k := 0; k < s.queries; k++ {
		rows, err := filter.Query(cats[k%8], (k*37)%100000)
		check(err)
		for rows.Next() {
			var p Product
			check(rows.Scan(&p.Id, &p.Name, &p.Category, &p.Price, &p.Stock))
			found++
			idsum += p.Id
		}
		check(rows.Err())
		rows.Close()
	}
	phase("filter")
	fmt.Println("filter", found, idsum)

	changed := 0
	for lo := 1; changed < s.updates; lo += 100 {
		rows, err := db.Query("SELECT id, name, category, price, stock FROM products WHERE id >= ? AND id < ?", lo, lo+100)
		check(err)
		var ps []Product
		for rows.Next() {
			var p Product
			check(rows.Scan(&p.Id, &p.Name, &p.Category, &p.Price, &p.Stock))
			ps = append(ps, p)
		}
		rows.Close()
		tx, err := db.Begin()
		check(err)
		upd, err := tx.Prepare("UPDATE products SET stock = ?, name = ? WHERE id = ?")
		check(err)
		for _, p := range ps {
			_, err = upd.Exec(p.Stock+1, p.Name+"*", p.Id)
			check(err)
		}
		upd.Close()
		check(tx.Commit())
		changed += len(ps)
	}
	phase("update")
	var stock, starred, count int
	check(db.QueryRow("SELECT coalesce(sum(stock), 0) FROM products").Scan(&stock))
	check(db.QueryRow("SELECT count(*) FROM products WHERE name LIKE '%*'").Scan(&starred))
	check(db.QueryRow("SELECT count(*) FROM products").Scan(&count))
	fmt.Println("update", changed, stock, starred)
	fmt.Println("rows", count)
}

func main() {
	impl := flag.String("impl", "gorm", "gorm or sql")
	flag.Parse()
	s := sizes{100000, 1000000, 10000, 10000}
	if os.Getenv("SCALE") == "small" {
		s = sizes{10000, 20000, 1000, 1000}
	}
	dsn := ":memory:"
	if flag.NArg() > 0 {
		dsn = flag.Arg(0)
		os.Remove(dsn)
	}
	t0 = time.Now()
	switch *impl {
	case "gorm":
		runGorm(dsn, s)
	case "sql":
		runSQL(dsn, s)
	default:
		panic("unknown -impl " + *impl)
	}
}
