namespace std.math.complex

// std.math.complex (Go's complex128 + math/cmplx): Complex{Re, Im} with
// arithmetic methods (Add, Sub, Mul, Div, Neg: the operator-interface
// names, so `+ - * /` can map onto them once operator overloading lands)
// and the cmplx functions.

struct Complex {
    Re float
    Im float

    func Add(o Complex) Complex {
        return Complex{Re: this.Re + o.Re, Im: this.Im + o.Im}
    }

    func Sub(o Complex) Complex {
        return Complex{Re: this.Re - o.Re, Im: this.Im - o.Im}
    }

    func Mul(o Complex) Complex {
        return Complex{Re: this.Re * o.Re - this.Im * o.Im, Im: this.Re * o.Im + this.Im * o.Re}
    }

    // Division (Smith's algorithm, avoids overflow for large parts).
    func Div(o Complex) Complex {
        var a = this.Re
        var b = this.Im
        var c = o.Re
        var d = o.Im
        if std.math.Abs(c) >= std.math.Abs(d) {
            if c == 0 && d == 0 {
                return Complex{Re: a / c, Im: b / c}
            }
            var r = d / c
            var den = c + d * r
            return Complex{Re: (a + b * r) / den, Im: (b - a * r) / den}
        }
        var r = c / d
        var den = c * r + d
        return Complex{Re: (a * r + b) / den, Im: (b * r - a) / den}
    }

    func Neg() Complex {
        return Complex{Re: -this.Re, Im: -this.Im}
    }

    // Multiplication by a real number.
    func Scale(k float) Complex {
        return Complex{Re: this.Re * k, Im: this.Im * k}
    }

    func Conj() Complex {
        return Complex{Re: this.Re, Im: -this.Im}
    }

    // |c|.
    func Abs() float {
        return std.math.Hypot(this.Re, this.Im)
    }

    // The argument in (-Pi, Pi].
    func Phase() float {
        return std.math.Atan2(this.Im, this.Re)
    }

    func Equals(o Complex) bool {
        return this.Re == o.Re && this.Im == o.Im
    }

    // Go's format: (1+2i), (1.5-0.5i).
    func String() string {
        var sign = "+"
        if this.Im < 0 {
            sign = ""
        }
        return "(" + std.fmt.Format("{0}", this.Re) + sign + std.fmt.Format("{0}", this.Im) + "i)"
    }
}

// re + im·i.
func New(re float, im float) Complex {
    return Complex{Re: re, Im: im}
}

// The complex number with modulus r and argument theta.
func Rect(r float, theta float) Complex {
    return Complex{Re: r * std.math.Cos(theta), Im: r * std.math.Sin(theta)}
}

// (|c|, Phase(c)).
func Polar(c Complex) (float, float) {
    return (c.Abs(), c.Phase())
}

func Abs(c Complex) float {
    return c.Abs()
}

func Phase(c Complex) float {
    return c.Phase()
}

func Conj(c Complex) Complex {
    return c.Conj()
}

// e^c.
func Exp(c Complex) Complex {
    var r = std.math.Exp(c.Re)
    return Complex{Re: r * std.math.Cos(c.Im), Im: r * std.math.Sin(c.Im)}
}

// The principal natural logarithm.
func Log(c Complex) Complex {
    return Complex{Re: std.math.Log(c.Abs()), Im: c.Phase()}
}

// The principal square root (Re >= 0; branch cut on the negative axis).
func Sqrt(c Complex) Complex {
    if c.Im == 0 {
        if c.Re == 0 {
            return Complex{Re: 0, Im: c.Im}
        }
        if c.Re < 0 {
            var t = std.math.Sqrt(-c.Re)
            if signbit(c.Im) {
                t = -t
            }
            return Complex{Re: 0, Im: t}
        }
        return Complex{Re: std.math.Sqrt(c.Re), Im: c.Im}
    }
    var r = c.Abs()
    if c.Re >= 0 {
        var t = std.math.Sqrt(0.5 * (r + c.Re))
        return Complex{Re: t, Im: c.Im / (2 * t)}
    }
    var t = std.math.Sqrt(0.5 * (r - c.Re))
    if c.Im < 0 {
        t = -t
    }
    return Complex{Re: c.Im / (2 * t), Im: t}
}

// x^y (principal value; 0^y is 0 for Re(y) > 0).
func Pow(x Complex, y Complex) Complex {
    if x.Re == 0 && x.Im == 0 {
        if y.Re == 0 && y.Im == 0 {
            return Complex{Re: 1, Im: 0}
        }
        return Complex{Re: 0, Im: 0}
    }
    return Exp(y.Mul(Log(x)))
}

func Sin(c Complex) Complex {
    return Complex{Re: std.math.Sin(c.Re) * cosh(c.Im), Im: std.math.Cos(c.Re) * sinh(c.Im)}
}

func Cos(c Complex) Complex {
    return Complex{Re: std.math.Cos(c.Re) * cosh(c.Im), Im: -std.math.Sin(c.Re) * sinh(c.Im)}
}

// True if either part is NaN and neither is infinite.
func IsNaN(c Complex) bool {
    if std.math.IsInf(c.Re, 0) || std.math.IsInf(c.Im, 0) {
        return false
    }
    return std.math.IsNaN(c.Re) || std.math.IsNaN(c.Im)
}

// True if either part is infinite.
func IsInf(c Complex) bool {
    return std.math.IsInf(c.Re, 0) || std.math.IsInf(c.Im, 0)
}

func signbit(x float) bool {
    return x < 0 || (x == 0 && 1 / x < 0)
}

func cosh(x float) float {
    return (std.math.Exp(x) + std.math.Exp(-x)) / 2
}

func sinh(x float) float {
    return (std.math.Exp(x) - std.math.Exp(-x)) / 2
}
