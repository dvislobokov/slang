namespace std.math.big

// std.math.big: arbitrary-precision Int, Rat and Decimal.
//
// Values are immutable: every operation returns a new value (unlike Go's
// z.Add(x, y)), so copies may share storage safely. The method names are
// those of the operator interfaces (Add, Sub, Mul, Div, Neg, Compare,
// Equals), so `+ - * /` can map onto them once operator overloading
// lands. Magnitudes are []uint32 limbs; the limb arithmetic (Karatsuba
// multiplication, Knuth division, base conversion) is C
// (runtime/src/std_math_big.c).

// An arbitrary-precision integer. The zero value is 0.
struct Int {
    neg bool
    mag []uint32

    // -1, 0 or +1.
    func Sign() int {
        if len(this.mag) == 0 {
            return 0
        }
        if this.neg {
            return -1
        }
        return 1
    }

    func IsZero() bool {
        return len(this.mag) == 0
    }

    func Add(y Int) Int {
        if this.neg == y.neg {
            return mk(this.neg, rtAdd(this.mag, y.mag))
        }
        var c = rtCmp(this.mag, y.mag)
        if c == 0 {
            return Int{}
        }
        if c > 0 {
            return mk(this.neg, rtSub(this.mag, y.mag))
        }
        return mk(y.neg, rtSub(y.mag, this.mag))
    }

    func Sub(y Int) Int {
        return this.Add(y.Neg())
    }

    func Mul(y Int) Int {
        return mk(this.neg != y.neg, rtMul(this.mag, y.mag))
    }

    // Truncated division (rounds toward zero, like Go's Quo and Slang's
    // int /). Panics on division by zero.
    func Quo(y Int) Int {
        var (q, _) = this.QuoRem(y)
        return q
    }

    // Remainder of Quo: has the sign of this (like Go's Rem and %).
    func Rem(y Int) Int {
        var (_, r) = this.QuoRem(y)
        return r
    }

    // (Quo(y), Rem(y)).
    func QuoRem(y Int) (Int, Int) {
        var qr = rtDivMod(this.mag, y.mag)
        var qn = int(qr[0])
        var q = mk(this.neg != y.neg, qr[1..1 + qn])
        var r = mk(this.neg, qr[1 + qn..])
        return (q, r)
    }

    // Euclidean division (Go's Div): the remainder Mod(y) is always >= 0.
    func Div(y Int) Int {
        var (q, r) = this.QuoRem(y)
        if r.neg {
            if y.neg {
                return q.Add(NewInt(1))
            }
            return q.Sub(NewInt(1))
        }
        return q
    }

    // Euclidean modulus (Go's Mod): 0 <= Mod(y) < |y|.
    func Mod(y Int) Int {
        var r = this.Rem(y)
        if r.neg {
            return r.Add(y.Abs())
        }
        return r
    }

    func Neg() Int {
        if len(this.mag) == 0 {
            return *this
        }
        return Int{neg: !this.neg, mag: this.mag}
    }

    func Abs() Int {
        return Int{neg: false, mag: this.mag}
    }

    // -1, 0 or +1 comparing this with y.
    func Cmp(y Int) int {
        if this.neg != y.neg {
            if this.neg {
                return -1
            }
            return 1
        }
        var c = rtCmp(this.mag, y.mag)
        if this.neg {
            return -c
        }
        return c
    }

    // The same as Cmp (the Ordered operator-interface name).
    func Compare(y Int) int {
        return this.Cmp(y)
    }

    // Compares absolute values.
    func CmpAbs(y Int) int {
        return rtCmp(this.mag, y.mag)
    }

    func Equals(y Int) bool {
        return this.Cmp(y) == 0
    }

    // this << n (n >= 0).
    func Lsh(n int) Int {
        return mk(this.neg, rtShl(this.mag, n))
    }

    // this >> n (n >= 0), rounding toward negative infinity like Go.
    func Rsh(n int) Int {
        if !this.neg {
            return mk(false, rtShr(this.mag, n))
        }
        // -((|x| - 1) >> n) - 1
        var t = mk(false, rtSub(this.mag, one().mag))
        return mk(false, rtShr(t.mag, n)).Add(one()).Neg()
    }

    // Number of bits of |this| (0 for 0).
    func BitLen() int {
        return rtBitLen(this.mag)
    }

    // Bit i of |this| (0 or 1).
    func Bit(i int) int {
        var w = i / 32
        if i < 0 || w >= len(this.mag) {
            return 0
        }
        return int((this.mag[w] >> uint32(i % 32)) & 1)
    }

    // Number of trailing zero bits of |this| (0 for 0).
    func TrailingZeroBits() int {
        for i, l in this.mag {
            if l != 0 {
                var n = 0
                var x = l
                for (x & 1) == 0 {
                    x = x >> 1
                    n += 1
                }
                return i * 32 + n
            }
        }
        return 0
    }

    // True if the value fits in int.
    func IsInt64() bool {
        if len(this.mag) <= 1 {
            return true
        }
        if len(this.mag) > 2 {
            return false
        }
        var u = (uint(this.mag[1]) << 32) | uint(this.mag[0])
        if this.neg {
            return u <= 9223372036854775808
        }
        return u <= 9223372036854775807
    }

    // True if the value fits in uint.
    func IsUint64() bool {
        return !this.neg && len(this.mag) <= 2
    }

    // The low 64 bits as an int (wraps if !IsInt64()).
    func Int64() int {
        var u = this.Uint64()
        if this.neg {
            return int(uint(0) - u)
        }
        return int(u)
    }

    // The low 64 bits of |this|.
    func Uint64() uint {
        var u uint = 0
        if len(this.mag) > 0 {
            u = uint(this.mag[0])
        }
        if len(this.mag) > 1 {
            u = u | (uint(this.mag[1]) << 32)
        }
        return u
    }

    // Nearest float (may be ±Inf for huge values).
    func Float64() float {
        var f = 0.0
        var i = len(this.mag) - 1
        for i >= 0 {
            f = f * 4294967296.0 + float(uint(this.mag[i]))
            i -= 1
        }
        if this.neg {
            return -f
        }
        return f
    }

    // Decimal text.
    func String() string {
        return this.Text(10)
    }

    // Text in base 2..36 (lower-case digits, leading - when negative).
    func Text(base int) string {
        var s = rtFormat(this.mag, base)
        if this.neg {
            return "-" + s
        }
        return s
    }

    // Big-endian bytes of |this| (empty for 0), Go's Bytes.
    func Bytes() []byte {
        return rtToBytes(this.mag)
    }

    // this^y mod m (m zero: no modulus; y < 0 with a modulus: the inverse
    // power, error if none). Panics for y < 0 without a modulus.
    func Exp(y Int, m Int) Int! {
        var base = *this
        var e = y
        if e.neg {
            if m.IsZero() {
                panic("big: negative exponent without modulus")
            }
            base = this.ModInverse(m)?
            e = e.Neg()
        }
        var result = NewInt(1)
        if !m.IsZero() {
            base = base.Mod(m)
            result = result.Mod(m)
        }
        var n = e.BitLen()
        for i in 0..n {
            if e.Bit(i) == 1 {
                result = result.Mul(base)
                if !m.IsZero() {
                    result = result.Mod(m)
                }
            }
            if i + 1 < n {
                base = base.Mul(base)
                if !m.IsZero() {
                    base = base.Mod(m)
                }
            }
        }
        return result
    }

    // this^n for n >= 0.
    func Pow(n int) Int {
        var e = NewInt(n)
        return try this.Exp(e, Int{}) else Int{}
    }

    // The inverse of this modulo m: x with this*x ≡ 1 (mod m), 0 <= x < |m|.
    func ModInverse(m Int) Int! {
        if m.IsZero() {
            return error("big: ModInverse with zero modulus")
        }
        var mm = m.Abs()
        var (g, x, _) = egcd(this.Mod(mm), mm)
        if !g.Equals(NewInt(1)) {
            return error("big: " + this.String() + " has no inverse modulo " + m.String())
        }
        return x.Mod(mm)
    }

    // The greatest common divisor of |this| and |y| (Euclid).
    func GCD(y Int) Int {
        var a = this.Abs()
        var b = y.Abs()
        for !b.IsZero() {
            var r = a.Rem(b)
            a = b
            b = r
        }
        return a
    }

    // floor(sqrt(this)); panics for negative values.
    func Sqrt() Int {
        if this.neg {
            panic("big: square root of negative number")
        }
        if this.IsZero() {
            return *this
        }
        // Newton from above: x = 2^ceil(bits/2).
        var x = one().Lsh((this.BitLen() + 1) / 2)
        for {
            var y = x.Add(this.Quo(x)).Rsh(1)
            if y.Cmp(x) >= 0 {
                return x
            }
            x = y
        }
    }

    // Miller-Rabin with n pseudo-random bases plus the first 12 prime
    // bases (deterministic below 3.3e24). Never wrong for primes; the
    // chance a composite passes is at most 4^-(n+12).
    func ProbablyPrime(n int) bool {
        if this.neg || this.IsZero() {
            return false
        }
        var small = [2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37]
        if this.IsInt64() && this.Int64() < 64 {
            var v = this.Int64()
            for p in small {
                if v == p {
                    return true
                }
            }
            return v == 41 || v == 43 || v == 47 || v == 53 || v == 59 || v == 61
        }
        for p in small {
            if this.Rem(NewInt(p)).IsZero() {
                return false
            }
        }
        var nm1 = this.Sub(one())
        var s = nm1.TrailingZeroBits()
        var d = nm1.Rsh(s)
        var bases []Int = []
        for p in small {
            bases = append(bases, NewInt(p))
        }
        // Deterministic pseudo-random extra bases in [2, this-2].
        var seed uint = 0x9E3779B97F4A7C15
        var span = this.Sub(NewInt(3))
        for _ in 0..n {
            seed = seed ^ (seed << 13)
            seed = seed ^ (seed >> 7)
            seed = seed ^ (seed << 17)
            var r = FromUint64(seed).Mul(FromUint64(seed ^ 0xD1B54A32D192ED03))
            bases = append(bases, r.Mod(span).Add(NewInt(2)))
        }
        for a in bases {
            var x = try a.Exp(d, *this) else Int{}
            if x.Equals(one()) || x.Equals(nm1) {
                continue
            }
            var composite = true
            for _ in 1..s {
                x = x.Mul(x).Mod(*this)
                if x.Equals(nm1) {
                    composite = false
                    break
                }
            }
            if composite {
                return false
            }
        }
        return true
    }
}

func one() Int {
    return NewInt(1)
}

func mk(neg bool, mag []uint32) Int {
    if len(mag) == 0 {
        return Int{}
    }
    return Int{neg: neg, mag: mag}
}

// (g, x, y) with a*x + b*y = g = gcd(a, b).
func egcd(a Int, b Int) (Int, Int, Int) {
    var oldR = a
    var r = b
    var oldS = NewInt(1)
    var s = Int{}
    var oldT = Int{}
    var t = NewInt(1)
    for !r.IsZero() {
        var q = oldR.Quo(r)
        var nr = oldR.Sub(q.Mul(r))
        oldR = r
        r = nr
        var ns = oldS.Sub(q.Mul(s))
        oldS = s
        s = ns
        var nt = oldT.Sub(q.Mul(t))
        oldT = t
        t = nt
    }
    return (oldR, oldS, oldT)
}

// The Int with value x.
func NewInt(x int) Int {
    if x < 0 {
        return mk(true, rtFromUint(uint(0) - uint(x)))
    }
    return mk(false, rtFromUint(uint(x)))
}

// The Int with value x.
func FromUint64(x uint) Int {
    return mk(false, rtFromUint(x))
}

// The non-negative Int of big-endian bytes b (Go's SetBytes).
func FromBytes(b []byte) Int {
    return mk(false, rtFromBytes(b))
}

// Parses an integer with optional sign in base 2..36, or base 0: prefix
// 0x, 0o, 0b selects the base (default 10). Underscores between digits
// are allowed with base 0.
func ParseInt(s string, base int) Int! {
    var body = s
    var neg = false
    if len(body) > 0 && (body[0] == 43 || body[0] == 45) {
        neg = body[0] == 45
        body = body[1..]
    }
    var b = base
    if b == 0 {
        b = 10
        var lower = std.strings.ToLower(body)
        if std.strings.HasPrefix(lower, "0x") {
            b = 16
            body = body[2..]
        } else if std.strings.HasPrefix(lower, "0o") {
            b = 8
            body = body[2..]
        } else if std.strings.HasPrefix(lower, "0b") {
            b = 2
            body = body[2..]
        }
        body = std.strings.Replace(body, "_", "")
    }
    var mag = try rtParse(body, b) catch err {
        return std.errors.Wrap(err, "big: parsing " + std.encoding.json.Quote(s))
    }
    return mk(neg, mag)
}

// ParseInt that panics on bad input (for literals in code).
func MustParseInt(s string) Int {
    return try ParseInt(s, 0) catch err {
        panic(err)
    }
}

// A rational number num/den in lowest terms with den > 0. The zero value
// is 0 (den 0 is treated as 1).
struct Rat {
    num Int
    den Int

    // The numerator (sign of the value).
    func Num() Int {
        return this.num
    }

    // The denominator (> 0).
    func Denom() Int {
        if this.den.IsZero() {
            return NewInt(1)
        }
        return this.den
    }

    func Sign() int {
        return this.num.Sign()
    }

    func IsInt() bool {
        return this.Denom().Equals(NewInt(1))
    }

    func Add(y Rat) Rat {
        return RatFromInts(this.num.Mul(y.Denom()).Add(y.num.Mul(this.Denom())), this.Denom().Mul(y.Denom()))
    }

    func Sub(y Rat) Rat {
        return this.Add(y.Neg())
    }

    func Mul(y Rat) Rat {
        return RatFromInts(this.num.Mul(y.num), this.Denom().Mul(y.Denom()))
    }

    // this / y; panics if y is zero.
    func Div(y Rat) Rat {
        if y.num.IsZero() {
            panic("division by zero")
        }
        return RatFromInts(this.num.Mul(y.Denom()), this.Denom().Mul(y.num))
    }

    func Neg() Rat {
        return Rat{num: this.num.Neg(), den: this.Denom()}
    }

    func Abs() Rat {
        return Rat{num: this.num.Abs(), den: this.Denom()}
    }

    // 1 / this; panics if this is zero.
    func Inv() Rat {
        return RatFromInts(this.Denom(), this.num)
    }

    func Cmp(y Rat) int {
        return this.num.Mul(y.Denom()).Cmp(y.num.Mul(this.Denom()))
    }

    func Compare(y Rat) int {
        return this.Cmp(y)
    }

    func Equals(y Rat) bool {
        return this.Cmp(y) == 0
    }

    // "a/b" (Go's String: the denominator is always shown).
    func String() string {
        return this.num.String() + "/" + this.Denom().String()
    }

    // "a" for integers, else "a/b" (Go's RatString).
    func RatString() string {
        if this.IsInt() {
            return this.num.String()
        }
        return this.String()
    }

    // Decimal text with prec digits after the point, rounded half away
    // from zero (Go's FloatString).
    func FloatString(prec int) string {
        var d = DecimalFromRat(*this, prec, RoundingMode.HalfUp)
        return d.String()
    }

    // Nearest float (approximately: via a 20-digit decimal).
    func Float64() float {
        return DecimalFromRat(*this, 20, RoundingMode.HalfEven).Float64()
    }
}

// num/den reduced; panics if den is zero.
func RatFromInts(num Int, den Int) Rat {
    if den.IsZero() {
        panic("big: zero denominator in Rat")
    }
    var n = num
    var d = den
    if d.neg {
        n = n.Neg()
        d = d.Neg()
    }
    var g = n.GCD(d)
    if !g.IsZero() && !g.Equals(NewInt(1)) {
        n = n.Quo(g)
        d = d.Quo(g)
    }
    return Rat{num: n, den: d}
}

// a/b reduced; panics if b is zero.
func NewRat(a int, b int) Rat {
    return RatFromInts(NewInt(a), NewInt(b))
}

// Parses "a/b", an integer, or a decimal "1.25" / "-1.5e3".
func ParseRat(s string) Rat! {
    var slash = std.strings.Index(s, "/")
    if slash >= 0 {
        var n = ParseInt(s[..slash], 10)?
        var d = ParseInt(s[slash + 1..], 10)?
        if d.IsZero() {
            return error("big: zero denominator in " + std.encoding.json.Quote(s))
        }
        return RatFromInts(n, d)
    }
    var dec = ParseDecimal(s)?
    return dec.Rat()
}

// Rounding modes of Decimal (and Rat.FloatString).
enum RoundingMode {
    // Round half away from zero (commercial rounding): 2.5 -> 3, -2.5 -> -3.
    HalfUp
    // Round half to even (banker's rounding): 2.5 -> 2, 3.5 -> 4.
    HalfEven
    // Round half toward zero: 2.5 -> 2.
    HalfDown
    // Toward zero (truncate).
    Down
    // Away from zero.
    Up
    // Toward +Inf.
    Ceiling
    // Toward -Inf.
    Floor
}

// Rounds q = n / d (d > 0) to an integer with mode.
func roundQuo(n Int, d Int, mode RoundingMode) Int {
    var (q, r) = n.QuoRem(d)
    if r.IsZero() {
        return q
    }
    var away = false
    var c = r.Abs().Lsh(1).Cmp(d)
    switch mode {
        case RoundingMode.HalfUp: away = c >= 0
        case RoundingMode.HalfDown: away = c > 0
        case RoundingMode.HalfEven: away = c > 0 || (c == 0 && q.Bit(0) == 1)
        case RoundingMode.Down: away = false
        case RoundingMode.Up: away = true
        case RoundingMode.Ceiling: away = !n.neg
        case RoundingMode.Floor: away = n.neg
    }
    if !away {
        return q
    }
    if n.neg {
        return q.Sub(NewInt(1))
    }
    return q.Add(NewInt(1))
}

func pow10(n int) Int {
    return NewInt(10).Pow(n)
}

// A fixed-point decimal: unscaled * 10^-scale (scale >= 0), exact for
// Add, Sub and Mul; Div rounds to a chosen scale. The zero value is 0.
// String keeps the scale ("1.50" stays "1.50").
struct Decimal {
    unscaled Int
    scale int

    // Digits after the point.
    func Scale() int {
        return this.scale
    }

    // The integer unscaled * 10^-scale is built from.
    func Unscaled() Int {
        return this.unscaled
    }

    func Sign() int {
        return this.unscaled.Sign()
    }

    func IsZero() bool {
        return this.unscaled.IsZero()
    }

    // The value with scale s (s >= scale: exact; smaller: rounded HalfUp).
    func Rescale(s int) Decimal {
        return this.Round(s, RoundingMode.HalfUp)
    }

    func Add(y Decimal) Decimal {
        var s = this.scale
        if y.scale > s {
            s = y.scale
        }
        return Decimal{unscaled: this.up(s).Add(y.up(s)), scale: s}
    }

    func Sub(y Decimal) Decimal {
        return this.Add(y.Neg())
    }

    // Exact product (scale = sum of the scales).
    func Mul(y Decimal) Decimal {
        return Decimal{unscaled: this.unscaled.Mul(y.unscaled), scale: this.scale + y.scale}
    }

    // this / y rounded to scale digits after the point with mode; error
    // on division by zero.
    func Div(y Decimal, scale int, mode RoundingMode) Decimal! {
        if y.IsZero() {
            return error("big: decimal division by zero")
        }
        // this/y = (a/10^sa) / (b/10^sb); scaled by 10^scale:
        // a * 10^(scale + sb - sa) / b
        var e = scale + y.scale - this.scale
        var n = this.unscaled
        var d = y.unscaled
        if e >= 0 {
            n = n.Mul(pow10(e))
        } else {
            d = d.Mul(pow10(-e))
        }
        if d.neg {
            n = n.Neg()
            d = d.Neg()
        }
        return Decimal{unscaled: roundQuo(n, d, mode), scale: scale}
    }

    // this / y with 16 digits after the point (or more if the operands
    // have more), HalfEven.
    func Quo(y Decimal) Decimal! {
        var s = 16
        if this.scale > s {
            s = this.scale
        }
        if y.scale > s {
            s = y.scale
        }
        return this.Div(y, s, RoundingMode.HalfEven)?
    }

    // Remainder of truncated division (sign of this).
    func Rem(y Decimal) Decimal! {
        if y.IsZero() {
            return error("big: decimal division by zero")
        }
        var s = this.scale
        if y.scale > s {
            s = y.scale
        }
        return Decimal{unscaled: this.up(s).Rem(y.up(s)), scale: s}
    }

    func Neg() Decimal {
        return Decimal{unscaled: this.unscaled.Neg(), scale: this.scale}
    }

    func Abs() Decimal {
        return Decimal{unscaled: this.unscaled.Abs(), scale: this.scale}
    }

    // Rounded to s digits after the point (s may be negative: 1234 at -2
    // is 1200). A larger s only appends zeros.
    func Round(s int, mode RoundingMode) Decimal {
        if s >= this.scale {
            return Decimal{unscaled: this.up(s), scale: s}
        }
        var q = roundQuo(this.unscaled, pow10(this.scale - s), mode)
        if s < 0 {
            return Decimal{unscaled: q.Mul(pow10(-s)), scale: 0}
        }
        return Decimal{unscaled: q, scale: s}
    }

    // Truncated toward zero to s digits.
    func Truncate(s int) Decimal {
        return this.Round(s, RoundingMode.Down)
    }

    // The integer part (truncated toward zero).
    func IntPart() Int {
        return this.Round(0, RoundingMode.Down).unscaled
    }

    // Value comparison (1.50 equals 1.5).
    func Cmp(y Decimal) int {
        var s = this.scale
        if y.scale > s {
            s = y.scale
        }
        return this.up(s).Cmp(y.up(s))
    }

    func Compare(y Decimal) int {
        return this.Cmp(y)
    }

    func Equals(y Decimal) bool {
        return this.Cmp(y) == 0
    }

    // Without trailing fractional zeros (1.500 -> 1.5).
    func Normalize() Decimal {
        var u = this.unscaled
        var s = this.scale
        var ten = NewInt(10)
        for s > 0 && !u.IsZero() {
            var (q, r) = u.QuoRem(ten)
            if !r.IsZero() {
                break
            }
            u = q
            s -= 1
        }
        if u.IsZero() {
            s = 0
        }
        return Decimal{unscaled: u, scale: s}
    }

    // Plain decimal text with exactly Scale() digits after the point.
    func String() string {
        var digits = this.unscaled.Abs().String()
        var sign = ""
        if this.unscaled.neg {
            sign = "-"
        }
        if this.scale <= 0 {
            return sign + digits
        }
        for len(digits) <= this.scale {
            digits = "0" + digits
        }
        var cut = len(digits) - this.scale
        return sign + digits[..cut] + "." + digits[cut..]
    }

    // The exact rational value.
    func Rat() Rat {
        return RatFromInts(this.unscaled, pow10(this.scale))
    }

    // Nearest float.
    func Float64() float {
        return try std.strconv.ParseFloat(this.String()) else 0
    }

    func up(s int) Int {
        if s == this.scale {
            return this.unscaled
        }
        return this.unscaled.Mul(pow10(s - this.scale))
    }
}

// unscaled * 10^-scale.
func NewDecimal(unscaled int, scale int) Decimal {
    if scale < 0 {
        return Decimal{unscaled: NewInt(unscaled).Mul(pow10(-scale)), scale: 0}
    }
    return Decimal{unscaled: NewInt(unscaled), scale: scale}
}

// The integer x as a Decimal with scale 0.
func DecimalFromInt(x Int) Decimal {
    return Decimal{unscaled: x, scale: 0}
}

// r rounded to scale digits.
func DecimalFromRat(r Rat, scale int, mode RoundingMode) Decimal {
    var n = r.num
    var d = r.Denom()
    if scale >= 0 {
        n = n.Mul(pow10(scale))
        return Decimal{unscaled: roundQuo(n, d, mode), scale: scale}
    }
    d = d.Mul(pow10(-scale))
    return Decimal{unscaled: roundQuo(n, d, mode).Mul(pow10(-scale)), scale: 0}
}

// Parses "-12.340", "+5", ".5", "1e-3", "2.5E2"; the scale is the number
// of digits after the point minus the exponent (at least 0).
func ParseDecimal(s string) Decimal! {
    var body = s
    var exp = 0
    var e = std.strings.Index(std.strings.ToLower(body), "e")
    if e >= 0 {
        exp = std.strconv.ParseInt(body[e + 1..], 10)?("big: parsing decimal " + std.encoding.json.Quote(s))
        body = body[..e]
    }
    var neg = false
    if len(body) > 0 && (body[0] == 43 || body[0] == 45) {
        neg = body[0] == 45
        body = body[1..]
    }
    var dot = std.strings.Index(body, ".")
    var scale = 0
    var digits = body
    if dot >= 0 {
        digits = body[..dot] + body[dot + 1..]
        scale = len(body) - dot - 1
    }
    if digits == "" || std.strings.Contains(digits, ".") {
        return error("big: invalid decimal " + std.encoding.json.Quote(s))
    }
    var u = try rtParse(digits, 10) catch err {
        return std.errors.Wrap(err, "big: parsing decimal " + std.encoding.json.Quote(s))
    }
    var d = Decimal{unscaled: mk(neg, u), scale: scale - exp}
    if d.scale < 0 {
        return Decimal{unscaled: d.unscaled.Mul(pow10(-d.scale)), scale: 0}
    }
    return d
}

// ParseDecimal that panics on bad input (for literals in code).
func MustParseDecimal(s string) Decimal {
    return try ParseDecimal(s) catch err {
        panic(err)
    }
}
