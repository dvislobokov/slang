// test: inventory system: structs in Map<K, V> updated through Get/Set, a List log, generic helpers, typed and wrapped errors
// status: passing
namespace spec.programs.inventory

import std
import std.collections
import std.io

struct Item {
    Sku        string
    Name       string
    Qty        int
    PriceCents int
}

struct OutOfStock {
    Sku  string
    Want int
    Have int
    func Message() string {
        return fmt.Format("out of stock: {0} (want {1}, have {2})", this.Sku, this.Want, this.Have)
    }
}

const ErrUnknown = error("unknown sku")

struct Line {
    Sku string
    Qty int
}

struct Inventory {
    items Map<string, Item>
    log   List<string>

    func Add(sku, name string, qty, price int) {
        if this.items.Has(sku) {
            var it = this.items.Get(sku)
            it.Qty += qty
            this.items.Set(sku, it)
        } else {
            this.items.Set(sku, Item{Sku: sku, Name: name, Qty: qty, PriceCents: price})
        }
        this.log.Add(fmt.Format("add {0} x{1}", sku, qty))
    }

    func Take(sku string, qty int) int! {
        if !this.items.Has(sku) {
            return errors.Wrap(ErrUnknown, sku)
        }
        var it = this.items.Get(sku)
        if it.Qty < qty {
            return OutOfStock{Sku: sku, Want: qty, Have: it.Qty}
        }
        it.Qty -= qty
        this.items.Set(sku, it)
        this.log.Add(fmt.Format("take {0} x{1}", sku, qty))
        return qty * it.PriceCents
    }

    func Value() int {
        return SumBy(this.items.Values(), (it Item) => it.Qty * it.PriceCents)
    }
}

func Filter<T>(xs []T, keep func(T) bool) []T {
    var out []T = []
    for x in xs {
        if keep(x) { out = append(out, x) }
    }
    return out
}

func SumBy<T>(xs []T, f func(T) int) int {
    var total = 0
    for x in xs { total += f(x) }
    return total
}

func Money(cents int) string {
    return fmt.Sprintf("%d.%02d", cents / 100, cents % 100)
}

func Fulfil(inv *Inventory, order []Line) int! {
    var total = 0
    for l in order {
        total += inv.Take(l.Sku, l.Qty)?("order line " + l.Sku)
    }
    return total
}

func main() {
    var inv = Inventory{}
    inv.Add("A1", "bolt", 100, 25)
    inv.Add("B2", "nut", 50, 10)
    inv.Add("C3", "gear", 5, 1250)
    inv.Add("A1", "bolt", 20, 25)
    println("stock value", Money(inv.Value()))

    var orders = [
        [Line{Sku: "A1", Qty: 10}, Line{Sku: "C3", Qty: 2}],
        [Line{Sku: "B2", Qty: 5}, Line{Sku: "C3", Qty: 4}],
        [Line{Sku: "Z9", Qty: 1}],
    ]
    for n, order in orders {
        var cost = try Fulfil(&inv, order) catch err {
            println("order", n + 1, "failed:", err)
            var oos OutOfStock
            if errors.As(err, &oos) {
                println("  short by", oos.Want - oos.Have)
            }
            if errors.Is(err, ErrUnknown) {
                println("  unknown item")
            }
            continue
        }
        println("order", n + 1, "ok", Money(cost))
    }

    for sku, it in inv.items {
        println(sku, it.Name, it.Qty, Money(it.PriceCents))
    }
    var low = Filter(inv.items.Values(), (it Item) => it.Qty < 50)
    var names []string = []
    for it in low { names = append(names, it.Name) }
    println("low stock", names)
    println("units", SumBy(inv.items.Values(), (it Item) => it.Qty))
    println("log", inv.log.Len())
    for i, entry in inv.log {
        println(" ", i, entry)
    }
    println("stock value", Money(inv.Value()))
}
