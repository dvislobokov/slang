namespace std.collections

struct List<T> {
    items []T

    static func New() List<T> {
        return List<T>{}
    }

    func Add(x T) {
        this.items = append(this.items, x)
    }

    func Len() int {
        return len(this.items)
    }

    func Get(i int) T {
        return this.items[i]
    }

    func Set(i int, x T) {
        this.items[i] = x
    }

    func Insert(i int, x T) {
        var n = len(this.items)
        if i < 0 || i > n {
            panic("List.Insert: index out of range")
        }
        var zero T
        this.items = append(this.items, zero)
        var j = n
        for j > i {
            this.items[j] = this.items[j - 1]
            j -= 1
        }
        this.items[i] = x
    }

    func Remove(i int) {
        var n = len(this.items)
        _ = this.items[i]
        for j in i..n - 1 {
            this.items[j] = this.items[j + 1]
        }
        var zero T
        this.items[n - 1] = zero
        this.items = this.items[0..n - 1]
    }

    func IndexOf(x T) int {
        for i, y in this.items {
            if y == x {
                return i
            }
        }
        return -1
    }

    func Contains(x T) bool {
        return this.IndexOf(x) >= 0
    }

    func Clear() {
        this.items = nil
    }

    func Items() []T {
        var out []T = []
        for x in this.items {
            out = append(out, x)
        }
        return out
    }
}

struct Map<K: Comparable, V> {
    index map[K]int
    keys []K
    vals []V

    static func New() Map<K, V> {
        return Map<K, V>{}
    }

    func Set(k K, v V) {
        var i = this.index[k]
        if i > 0 {
            this.vals[i - 1] = v
            return
        }
        if this.index == nil {
            this.index = map[K]int{}
        }
        this.keys = append(this.keys, k)
        this.vals = append(this.vals, v)
        this.index[k] = len(this.keys)
    }

    func Get(k K) V {
        var i = this.index[k]
        if i == 0 {
            var zero V
            return zero
        }
        return this.vals[i - 1]
    }

    func Has(k K) bool {
        return this.index[k] > 0
    }

    func Remove(k K) {
        var i = this.index[k]
        if i == 0 {
            return
        }
        delete(this.index, k)
        var n = len(this.keys)
        for j in i..n {
            this.keys[j - 1] = this.keys[j]
            this.vals[j - 1] = this.vals[j]
            this.index[this.keys[j - 1]] = j
        }
        var zk K
        var zv V
        this.keys[n - 1] = zk
        this.vals[n - 1] = zv
        this.keys = this.keys[0..n - 1]
        this.vals = this.vals[0..n - 1]
    }

    func Len() int {
        return len(this.keys)
    }

    func Clear() {
        this.index = nil
        this.keys = nil
        this.vals = nil
    }

    func Keys() []K {
        var out []K = []
        for k in this.keys {
            out = append(out, k)
        }
        return out
    }

    func Values() []V {
        var out []V = []
        for v in this.vals {
            out = append(out, v)
        }
        return out
    }
}

struct Set<T: Comparable> {
    index map[T]int
    items []T

    static func New() Set<T> {
        return Set<T>{}
    }

    func Add(x T) {
        if this.index[x] > 0 {
            return
        }
        if this.index == nil {
            this.index = map[T]int{}
        }
        this.items = append(this.items, x)
        this.index[x] = len(this.items)
    }

    func Has(x T) bool {
        return this.index[x] > 0
    }

    func Remove(x T) {
        var i = this.index[x]
        if i == 0 {
            return
        }
        delete(this.index, x)
        var n = len(this.items)
        for j in i..n {
            this.items[j - 1] = this.items[j]
            this.index[this.items[j - 1]] = j
        }
        var zero T
        this.items[n - 1] = zero
        this.items = this.items[0..n - 1]
    }

    func Len() int {
        return len(this.items)
    }

    func Clear() {
        this.index = nil
        this.items = nil
    }

    func Items() []T {
        var out []T = []
        for x in this.items {
            out = append(out, x)
        }
        return out
    }
}

struct Queue<T> {
    items []T

    static func New() Queue<T> {
        return Queue<T>{}
    }

    func Push(x T) {
        this.items = append(this.items, x)
    }

    func Pop() T! {
        if len(this.items) == 0 {
            return error("queue is empty")
        }
        var x = this.items[0]
        var zero T
        this.items[0] = zero
        this.items = this.items[1..]
        return x
    }

    func Peek() T! {
        if len(this.items) == 0 {
            return error("queue is empty")
        }
        return this.items[0]
    }

    func Len() int {
        return len(this.items)
    }
}

struct Stack<T> {
    items []T

    static func New() Stack<T> {
        return Stack<T>{}
    }

    func Push(x T) {
        this.items = append(this.items, x)
    }

    func Pop() T! {
        var n = len(this.items)
        if n == 0 {
            return error("stack is empty")
        }
        var x = this.items[n - 1]
        var zero T
        this.items[n - 1] = zero
        this.items = this.items[0..n - 1]
        return x
    }

    func Peek() T! {
        if len(this.items) == 0 {
            return error("stack is empty")
        }
        return this.items[len(this.items) - 1]
    }

    func Len() int {
        return len(this.items)
    }
}
