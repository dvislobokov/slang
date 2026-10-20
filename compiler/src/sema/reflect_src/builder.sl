namespace std.strings

// strings.Builder: accumulates text; String() joins the pieces once.
struct Builder {
    parts []string
    n int

    func Write(s string) {
        this.parts = append(this.parts, s)
        this.n += len(s)
    }

    func String() string {
        if len(this.parts) == 0 {
            return ""
        }
        if len(this.parts) > 1 {
            var s = Join(this.parts, "")
            this.parts = [s]
            return s
        }
        return this.parts[0]
    }

    func Len() int {
        return this.n
    }

    func Reset() {
        this.parts = nil
        this.n = 0
    }
}
