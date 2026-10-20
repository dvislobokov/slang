namespace app

import std
import std.io
import std.math

// An invoice with std.math.big.Decimal (exact cents, explicit rounding),
// std.strconv for input, std.time for dates (fixed, UTC) and durations.

struct Line {
    Item string
    Qty int
    Price big.Decimal
}

func parseLine(s string) Line! {
    var f = strings.Split(s, ";")
    if len(f) != 3 {
        return error("bad line " + strconv.Quote(s))
    }
    var qty = strconv.Atoi(strings.Trim(f[1]))?("quantity")
    var price = big.ParseDecimal(strings.Trim(f[2]))?("price")
    return Line{Item: strings.Trim(f[0]), Qty: qty, Price: price}
}

func main() void! {
    var input = ["Widget; 3; 19.99", "Gadget; 1; 249.50", "Cable; 12; 0.35"]
    var subtotal = big.NewDecimal(0, 2)
    for s in input {
        var l = parseLine(s)?
        var total = l.Price.Mul(big.NewDecimal(l.Qty, 0))
        subtotal = subtotal.Add(total)
        println(strings.PadRight(l.Item, 8, " "), strings.PadLeft(strconv.Itoa(l.Qty), 3, " "), "x", strings.PadLeft(l.Price.String(), 7, " "), "=", strings.PadLeft(total.Rescale(2).String(), 8, " "))
    }
    var vat = subtotal.Mul(big.MustParseDecimal("0.19")).Round(2, big.RoundingMode.HalfUp)
    var total = subtotal.Add(vat)
    println("subtotal", subtotal.String(), "vat", vat.String(), "total", total.String())
    var share = total.Div(big.NewDecimal(3, 0), 2, big.RoundingMode.HalfEven)?
    println("split in 3:", share.String(), "remainder:", total.Sub(share.Mul(big.NewDecimal(3, 0))).String())
    var issued = time.Date(2024, 3, 28, 9, 30, 0, 0, time.UTC())
    var due = issued.AddDate(0, 0, 30)
    println("issued", issued.Format(time.RFC3339), "due", due.Format("Mon Jan 2, 2006"), "in", due.Sub(issued).String())
    var paid = try time.ParseTime(time.DateOnly, "2024-05-02") else time.Time{}
    println("paid late:", paid.After(due), "by", paid.Sub(due).Hours(), "hours")
    var bad = try parseLine("Thing; two; 1.00") catch err {
        println("rejected:", err)
        return
    }
    println(bad.Item)
}
