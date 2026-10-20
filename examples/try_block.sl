namespace examples.try_block

import std.io
import std

struct Config {
    Port int
    Workers int
}

func Field(line string, name string) string! {
    var parts = strings.Split(line, "=")
    if len(parts) != 2 || strings.Trim(parts[0]) != name {
        return error("expected " + name + " in `" + line + "`")
    }
    return strings.Trim(parts[1])
}

// A non-fallible function: the block try handles every error locally.
func Parse(text string) Config {
    var cfg = Config{Port: 80, Workers: 1}
    try {
        var lines = strings.Split(text, "\n")
        var port = strings.ToInt(Field(lines[0], "port")?)?
        cfg.Port = port
        cfg.Workers = strings.ToInt(Field(lines[1], "workers")?)?("workers")
    } catch err {
        println("using defaults:", err)
    }
    return cfg
}

// Nested blocks: `?` goes to the nearest catch; the catch of a fallible
// function may re-propagate with `return err`.
func Total(items []string) int! {
    var sum = 0
    var bad = 0
    for item in items {
        try {
            sum += strings.ToInt(item)?
        } catch _ {
            bad += 1
        }
    }
    try {
        if bad > 1 {
            return error(fmt.Format("{0} bad items", bad))
        }
        _ = strings.ToInt("1")?
    } catch err {
        return err
    }
    return sum
}

func main() {
    var a = Parse("port = 8080\nworkers = 4")
    println(a.Port, a.Workers)
    var b = Parse("port = 9000\nworkers = many")
    println(b.Port, b.Workers)
    var c = Parse("host = x\nworkers = 2")
    println(c.Port, c.Workers)
    println(try Total(["1", "2", "x"]) else -1)
    try Total(["1", "y", "z"]) catch err {
        println("total:", err)
    }
}
