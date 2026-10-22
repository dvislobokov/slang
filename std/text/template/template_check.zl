// Run-time type check of a parsed template against its data type (the
// same rules as the compile-time check of literal templates in
// compiler/src/sema/tmplcheck.rs): every field path must exist in the
// static type it is applied to; range needs a list, array, map or int.
// Unknown types (any, interfaces, function results) end checking of a path.
namespace std.text.template

struct checker {
    t     *tmpl
    names []string
    types []*std.reflect.TypeInfo

    func fail(line int, msg string) error {
        return this.t.fail(line, msg)
    }

    func list(nodes []int, dot *std.reflect.TypeInfo) void! {
        for idx in nodes {
            var n = this.t.nodes[idx]
            if n.kind == 2 {
                _ = this.pipe(n.pipe, dot, true)?
            } else if n.kind == 3 || n.kind == 5 {
                var mark = len(this.names)
                var tp = this.pipe(n.pipe, dot, true)?
                var inner = dot
                if n.kind == 5 {
                    inner = tp
                }
                this.list(n.list, inner)?
                if n.hasElse {
                    this.list(n.elseList, dot)?
                }
                this.names = this.names[..mark]
                this.types = this.types[..mark]
            } else if n.kind == 4 {
                var mark = len(this.names)
                var tp = this.pipe(n.pipe, dot, false)?
                var p = this.t.pipes[n.pipe]
                var elem = this.elem(tp, n.line)?
                var key = keyType(tp)
                if len(p.vars) == 1 {
                    this.bind(p.vars[0], elem)
                } else if len(p.vars) == 2 {
                    this.bind(p.vars[0], key)
                    this.bind(p.vars[1], elem)
                }
                this.list(n.list, elem)?
                this.names = this.names[..mark]
                this.types = this.types[..mark]
                if n.hasElse {
                    this.list(n.elseList, dot)?
                }
            } else if n.kind == 6 {
                if n.pipe >= 0 {
                    _ = this.pipe(n.pipe, dot, true)?
                }
            }
        }
    }

    func bind(name string, t *std.reflect.TypeInfo) {
        this.names = append(this.names, name)
        this.types = append(this.types, t)
    }

    func pipe(pi int, dot *std.reflect.TypeInfo, declare bool) *std.reflect.TypeInfo! {
        var p = this.t.pipes[pi]
        var t *std.reflect.TypeInfo = nil
        for c in p.cmds {
            t = this.command(c, dot, p.line)?
        }
        if declare && len(p.vars) > 0 && !p.assign {
            this.bind(p.vars[0], t)
        }
        return t
    }

    func command(c tcmd, dot *std.reflect.TypeInfo, line int) *std.reflect.TypeInfo! {
        var first = c.args[0]
        if first.kind == 4 {
            var argTypes []*std.reflect.TypeInfo = []
            for k in 1..len(c.args) {
                argTypes = append(argTypes, this.arg(c.args[k], dot, line)?)
            }
            if first.name == "index" && len(argTypes) >= 1 {
                // the element type when known
                var t = argTypes[0]
                for _ in 1..len(argTypes) {
                    t = elemOf(t)
                }
                return t
            }
            return nil
        }
        return this.arg(first, dot, line)?
    }

    func arg(a targ, dot *std.reflect.TypeInfo, line int) *std.reflect.TypeInfo! {
        var t *std.reflect.TypeInfo = nil
        if a.kind == 1 {
            t = dot
        } else if a.kind == 2 {
            var k = len(this.names) - 1
            for k >= 0 {
                if this.names[k] == a.name {
                    t = this.types[k]
                    break
                }
                k -= 1
            }
        } else if a.kind == 3 {
            return dot
        } else if a.kind == 10 {
            t = this.pipe(a.pipe, dot, false)?
        } else {
            return nil
        }
        for f in a.fields {
            t = this.field(t, f, line)?
        }
        return t
    }

    func field(t0 *std.reflect.TypeInfo, name string, line int) *std.reflect.TypeInfo! {
        var t = deref(t0)
        if t == nil || t.Kind == std.reflect.Kind.Any || t.Kind == std.reflect.Kind.Interface {
            return nil
        }
        if t == std.reflect.TypeOf<Value>() {
            return nil
        }
        if t.Kind == std.reflect.Kind.Struct {
            var f = t.Field(name)
            if f == nil {
                return this.fail(line, "can't evaluate field " + name + " in type " + t.Name)
            }
            return f.Type
        }
        if t.Kind == std.reflect.Kind.Map {
            if t.Key != nil && t.Key.Kind == std.reflect.Kind.String {
                return t.Elem
            }
            return this.fail(
                line,
                "can't evaluate field " +
                    name +
                    " in type " +
                    t.Name +
                    " (map keys are not strings)",
            )
        }
        return this.fail(line, "can't evaluate field " + name + " in type " + t.Name)
    }

    func elem(t0 *std.reflect.TypeInfo, line int) *std.reflect.TypeInfo! {
        var t = deref(t0)
        if t == nil ||
            t.Kind == std.reflect.Kind.Any ||
            t.Kind == std.reflect.Kind.Interface ||
            t == std.reflect.TypeOf<Value>() {
            return nil
        }
        if t.Kind == std.reflect.Kind.Slice ||
            t.Kind == std.reflect.Kind.Array ||
            t.Kind == std.reflect.Kind.Map {
            return t.Elem
        }
        if t.Kind == std.reflect.Kind.Int {
            return t
        }
        return this.fail(line, "range can't iterate over " + t.Name)
    }
}

func deref(t0 *std.reflect.TypeInfo) *std.reflect.TypeInfo {
    var t = t0
    for t != nil && t.Kind == std.reflect.Kind.Pointer {
        t = t.Elem
    }
    return t
}

func elemOf(t0 *std.reflect.TypeInfo) *std.reflect.TypeInfo {
    var t = deref(t0)
    if t != nil &&
        (t.Kind == std.reflect.Kind.Slice ||
            t.Kind == std.reflect.Kind.Array ||
            t.Kind == std.reflect.Kind.Map) {
        return t.Elem
    }
    return nil
}

func keyType(t0 *std.reflect.TypeInfo) *std.reflect.TypeInfo {
    var t = deref(t0)
    if t != nil && t.Kind == std.reflect.Kind.Map {
        return t.Key
    }
    return nil
}

func checkTypes(t *tmpl, root *std.reflect.TypeInfo) void! {
    var c = checker{t: t}
    c.names = ["$"]
    c.types = [root]
    c.list(t.nodes[t.root].list, root)?
}
