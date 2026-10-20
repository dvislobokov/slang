// test: fields may declare default values used when a literal omits them
// status: pending
namespace spec.structs.field_defaults

import std.io

struct Config {
    Name string = "app"
    Port int = 8080
    Debug bool
    Tags []string
}

func main() {
    var a = Config{}
    var b = Config{Port: 9000, Debug: true}
    var c Config
    println(a.Name, a.Port, a.Debug, len(a.Tags))
    println(b.Name, b.Port, b.Debug)
    println("[" + c.Name + "]", c.Port)
}
