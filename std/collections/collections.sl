namespace std.collections

// List is a growable list of T backed by a slice.
struct List<T> {
    items []T

    // New returns an empty list (the zero value works too).
    static func New() List<T> {
        return List<T>{}
    }

    // Add appends x.
    func Add(x T) {
        this.items = append(this.items, x)
    }

    // Len returns the number of elements.
    func Len() int {
        return len(this.items)
    }

    // Get returns element i; it panics if i is out of range.
    func Get(i int) T {
        return this.items[i]
    }

    // Set replaces element i; it panics if i is out of range.
    func Set(i int, x T) {
        this.items[i] = x
    }

    // Insert inserts x before element i (i == Len() appends); it panics if i is out of range.
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

    // Remove deletes element i, shifting the rest down; it panics if i is out of range.
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

    // IndexOf returns the index of the first element equal to x, or -1.
    func IndexOf(x T) int {
        for i, y in this.items {
            if y == x {
                return i
            }
        }
        return -1
    }

    // Contains reports whether an element equals x.
    func Contains(x T) bool {
        return this.IndexOf(x) >= 0
    }

    // Clear removes every element.
    func Clear() {
        this.items = nil
    }

    // Items returns the elements as a new slice.
    func Items() []T {
        var out []T = []
        for x in this.items {
            out = append(out, x)
        }
        return out
    }
}

// Map is a hash map that keeps insertion order (Keys and Values iterate in that order).
struct Map<K: Comparable, V> {
    index map[K]int
    keys []K
    vals []V

    // New returns an empty map (the zero value works too).
    static func New() Map<K, V> {
        return Map<K, V>{}
    }

    // Set inserts or replaces the value of k; a new key goes last.
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

    // Get returns the value of k, or the zero value of V if it is missing.
    func Get(k K) V {
        var i = this.index[k]
        if i == 0 {
            var zero V
            return zero
        }
        return this.vals[i - 1]
    }

    // Has reports whether k is present.
    func Has(k K) bool {
        return this.index[k] > 0
    }

    // Remove deletes k (nothing if it is missing).
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

    // Len returns the number of entries.
    func Len() int {
        return len(this.keys)
    }

    // Clear removes every entry.
    func Clear() {
        this.index = nil
        this.keys = nil
        this.vals = nil
    }

    // Keys returns the keys in insertion order.
    func Keys() []K {
        var out []K = []
        for k in this.keys {
            out = append(out, k)
        }
        return out
    }

    // Values returns the values in insertion order.
    func Values() []V {
        var out []V = []
        for v in this.vals {
            out = append(out, v)
        }
        return out
    }
}

// Set is a set of T that keeps insertion order.
struct Set<T: Comparable> {
    index map[T]int
    items []T

    // New returns an empty set (the zero value works too).
    static func New() Set<T> {
        return Set<T>{}
    }

    // Add inserts x (nothing if it is present).
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

    // Has reports whether x is in the set.
    func Has(x T) bool {
        return this.index[x] > 0
    }

    // Remove deletes x (nothing if it is missing).
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

    // Len returns the number of elements.
    func Len() int {
        return len(this.items)
    }

    // Clear removes every element.
    func Clear() {
        this.index = nil
        this.items = nil
    }

    // Items returns the elements in insertion order, as a new slice.
    func Items() []T {
        var out []T = []
        for x in this.items {
            out = append(out, x)
        }
        return out
    }
}

// Queue is a first-in, first-out queue.
struct Queue<T> {
    items []T

    // New returns an empty queue (the zero value works too).
    static func New() Queue<T> {
        return Queue<T>{}
    }

    // Push adds x at the back.
    func Push(x T) {
        this.items = append(this.items, x)
    }

    // Pop removes and returns the front element; an error if the queue is empty.
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

    // Peek returns the front element without removing it; an error if the queue is empty.
    func Peek() T! {
        if len(this.items) == 0 {
            return error("queue is empty")
        }
        return this.items[0]
    }

    // Len returns the number of elements.
    func Len() int {
        return len(this.items)
    }
}

// Stack is a last-in, first-out stack.
struct Stack<T> {
    items []T

    // New returns an empty stack (the zero value works too).
    static func New() Stack<T> {
        return Stack<T>{}
    }

    // Push puts x on top.
    func Push(x T) {
        this.items = append(this.items, x)
    }

    // Pop removes and returns the top element; an error if the stack is empty.
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

    // Peek returns the top element without removing it; an error if the stack is empty.
    func Peek() T! {
        if len(this.items) == 0 {
            return error("stack is empty")
        }
        return this.items[len(this.items) - 1]
    }

    // Len returns the number of elements.
    func Len() int {
        return len(this.items)
    }
}
