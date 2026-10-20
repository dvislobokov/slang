// test: tokenizer, recursive-descent parser and evaluator for integer arithmetic (enums with pointers, recursion, errors with context)
// status: passing
namespace spec.programs.calculator

import std.io
import std

enum Token {
    Num(value int)
    Op(sym char)
    LParen
    RParen
    End
}

enum Expr {
    Lit(value int)
    Neg(inner *Expr)
    Bin(op char, left *Expr, right *Expr)
}

func Ptr(e Expr) *Expr { var b = e; return &b }

func IsDigit(c char) bool { return c >= '0' && c <= '9' }

func Tokenize(src string) []Token! {
    var toks []Token = []
    var num = 0
    var inNum = false
    for i, c in src {
        if IsDigit(c) {
            num = num * 10 + (int(c) - int('0'))
            inNum = true
            continue
        }
        if inNum {
            toks = append(toks, Token.Num(num))
            num = 0
            inNum = false
        }
        switch c {
            case ' ': continue
            case '+', '-', '*', '/': toks = append(toks, Token.Op(c))
            case '(': toks = append(toks, Token.LParen)
            case ')': toks = append(toks, Token.RParen)
            default: return error(fmt.Format("unexpected '{0}' at {1}", c, i))
        }
    }
    if inNum { toks = append(toks, Token.Num(num)) }
    toks = append(toks, Token.End)
    return toks
}

struct Parser {
    toks []Token
    pos int

    func Peek() Token { return this.toks[this.pos] }

    func Next() Token {
        var t = this.toks[this.pos]
        if this.pos < len(this.toks) - 1 { this.pos += 1 }
        return t
    }

    func OpSym() char {
        switch this.Peek() {
            case Token.Op(s): return s
            default: return ' '
        }
    }

    func AtEnd() bool {
        switch this.Peek() {
            case Token.End: return true
            default: return false
        }
    }

    // expr := term (('+' | '-') term)*
    func Expr() *Expr! {
        var left = this.Term()?
        for this.OpSym() == '+' || this.OpSym() == '-' {
            var op = this.OpSym()
            this.Next()
            var right = this.Term()?
            left = Ptr(Expr.Bin(op, left, right))
        }
        return left
    }

    // term := factor (('*' | '/') factor)*
    func Term() *Expr! {
        var left = this.Factor()?
        for this.OpSym() == '*' || this.OpSym() == '/' {
            var op = this.OpSym()
            this.Next()
            var right = this.Factor()?
            left = Ptr(Expr.Bin(op, left, right))
        }
        return left
    }

    // factor := number | '-' factor | '(' expr ')'
    func Factor() *Expr! {
        switch this.Next() {
            case Token.Num(v): return Ptr(Expr.Lit(v))
            case Token.Op(s):
                if s == '-' {
                    var inner = this.Factor()?
                    return Ptr(Expr.Neg(inner))
                }
                return error(fmt.Format("unexpected operator {0}", s))
            case Token.LParen:
                var e = this.Expr()?
                switch this.Next() {
                    case Token.RParen: return e
                    default: return error("missing )")
                }
            default: return error("unexpected end")
        }
    }
}

func Eval(e *Expr) int! {
    switch *e {
        case Expr.Lit(v): return v
        case Expr.Neg(x): return -Eval(x)?
        case Expr.Bin(op, l, r):
            var a = Eval(l)?
            var b = Eval(r)?
            switch op {
                case '+': return a + b
                case '-': return a - b
                case '*': return a * b
                default:
                    if b == 0 { return error("division by zero") }
                    return a / b
            }
        default: return error("bad expression")
    }
}

func Show(e *Expr) string {
    switch *e {
        case Expr.Lit(v): return fmt.Format("{0}", v)
        case Expr.Neg(x): return "-" + Show(x)
        case Expr.Bin(op, l, r): return "(" + Show(l) + " " + string(op) + " " + Show(r) + ")"
        default: return "?"
    }
}

func Calc(src string) int! {
    var toks = Tokenize(src)?("tokenize")
    var p = Parser{toks: toks}
    var e = p.Expr()?("parse")
    if !p.AtEnd() { return error("trailing input") }
    println("  tree:", Show(e))
    return Eval(e)?
}

func main() {
    var inputs = ["1 + 2 * 3", "(1 + 2) * 3", "10 / (5 - 5)", "2 * -3 + 20 / 4", "7 $ 3", "(4 + 4", "8 - 3 - 2", "1 2", "100"]
    var ok = 0
    for src in inputs {
        println(src)
        var v = try Calc(src) catch err {
            println("  error:", err)
            continue
        }
        println("  value:", v)
        ok += 1
    }
    println(ok, "of", len(inputs), "evaluated")
}
