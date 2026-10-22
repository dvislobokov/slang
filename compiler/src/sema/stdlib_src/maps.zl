// std.maps: generic helpers over the builtin map[K]V. Builtin maps have
// no defined iteration order, so Keys and Values come back in an
// unspecified order; SortedKeys sorts them.
namespace std.maps

// The keys of m (unspecified order).
func Keys<K: Comparable, V>(m map[K]V) []K {
    var out []K = []
    for k, _ in m {
        out = append(out, k)
    }
    return out
}

// The values of m (unspecified order).
func Values<K: Comparable, V>(m map[K]V) []V {
    var out []V = []
    for _, v in m {
        out = append(out, v)
    }
    return out
}

// The keys of m, sorted ascending.
func SortedKeys<K: Ordered, V>(m map[K]V) []K {
    var ks = Keys(m)
    std.slices.Sort(ks)
    return ks
}

// The values of m in ascending key order.
func SortedValues<K: Ordered, V>(m map[K]V) []V {
    var out []V = []
    for k in SortedKeys(m) {
        out = append(out, m[k])
    }
    return out
}

// A new map with the entries of a and then b (b wins on equal keys).
func Merge<K: Comparable, V>(a map[K]V, b map[K]V) map[K]V {
    var out = map[K]V{}
    for k, v in a {
        out[k] = v
    }
    for k, v in b {
        out[k] = v
    }
    return out
}

// Copies every entry of src into dst (overwriting equal keys).
func Copy<K: Comparable, V>(dst map[K]V, src map[K]V) {
    for k, v in src {
        dst[k] = v
    }
}

// A new map from value to key. With duplicate values, the entry of the
// largest key wins when K is Ordered (deterministic); see InvertAll.
func Invert<K: Ordered, V: Comparable>(m map[K]V) map[V]K {
    var out = map[V]K{}
    for k in SortedKeys(m) {
        out[m[k]] = k
    }
    return out
}

// A new map from value to all its keys (sorted ascending).
func InvertAll<K: Ordered, V: Comparable>(m map[K]V) map[V][]K {
    var out = map[V][]K{}
    for k in SortedKeys(m) {
        var v = m[k]
        out[v] = append(out[v], k)
    }
    return out
}

// A new map with the entries satisfying keep.
func Filter<K: Comparable, V>(m map[K]V, keep func(K, V) bool) map[K]V {
    var out = map[K]V{}
    for k, v in m {
        if keep(k, v) {
            out[k] = v
        }
    }
    return out
}

// Deletes the entries satisfying del, in place.
func DeleteFunc<K: Comparable, V>(m map[K]V, del func(K, V) bool) {
    var ks []K = []
    for k, v in m {
        if del(k, v) {
            ks = append(ks, k)
        }
    }
    for k in ks {
        delete(m, k)
    }
}

// A new map with f applied to every value.
func MapValues<K: Comparable, V, W>(m map[K]V, f func(V) W) map[K]W {
    var out = map[K]W{}
    for k, v in m {
        out[k] = f(v)
    }
    return out
}

// True if a and b have the same keys with equal values.
func Equal<K: Comparable, V: Comparable>(a map[K]V, b map[K]V) bool {
    if len(a) != len(b) {
        return false
    }
    var bk = map[K]bool{}
    for k, _ in b {
        bk[k] = true
    }
    for k, v in a {
        if !bk[k] || b[k] != v {
            return false
        }
    }
    return true
}

// Like Equal with a value equality function.
func EqualFunc<K: Comparable, V, W>(a map[K]V, b map[K]W, eq func(V, W) bool) bool {
    if len(a) != len(b) {
        return false
    }
    var bk = map[K]bool{}
    for k, _ in b {
        bk[k] = true
    }
    for k, v in a {
        if !bk[k] || !eq(v, b[k]) {
            return false
        }
    }
    return true
}

// A shallow copy of m.
func Clone<K: Comparable, V>(m map[K]V) map[K]V {
    var out = map[K]V{}
    for k, v in m {
        out[k] = v
    }
    return out
}
