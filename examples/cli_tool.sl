namespace app

import std
import std.io

// A command-line tool with std.cli: subcommands, flags declared on
// structs with @cli.flag (defaults from the field values, env fallback),
// generated help and "did you mean" errors. The arguments are fixed lists
// here so the output is deterministic, and error messages are collected
// (App.Errors) instead of going to stderr; a real tool calls
// os.Exit(app.Run(os.Args())).

struct GreetFlags {
    @cli.flag(short: "n", help: "who to greet") Name string = "world"
    @cli.flag(help: "repeat the greeting") Times int = 1
    @cli.flag(short: "l", help: "shout") Loud bool
}

struct SumFlags {
    @cli.flag(help: "decimal places") Precision int = 2
}

func app() cli.App {
    var greet = cli.Command{Name: "greet", Help: "print a greeting", Flags: cli.FlagsOf<GreetFlags>(),
        Run: func(c *cli.Context) void! {
            var f = cli.Bind<GreetFlags>(c)?
            var msg = "hello, " + f.Name
            if f.Loud {
                msg = strings.ToUpper(msg) + "!"
            }
            for _ in 0..f.Times {
                println(msg)
            }
        }}
    var sum = cli.Command{Name: "sum", Help: "add numbers", Args: "<numbers...>", Flags: cli.FlagsOf<SumFlags>(),
        Run: func(c *cli.Context) void! {
            var f = cli.Bind<SumFlags>(c)?
            var total = 0.0
            for a in c.Args {
                total += strconv.ParseFloat(a)?("argument " + strconv.Quote(a))
            }
            println(strconv.FormatFloat(total, 'f', f.Precision))
        }}
    return cli.App{Name: "demo", Version: "0.3.0", Help: "a demo tool", Commands: [greet, sum]}
}

func run(args []string) {
    var all = ["demo"]
    for a in args {
        all = append(all, a)
    }
    var errs = io.NewBytesWriter()
    var a = app()
    a.Errors = errs
    var code = a.Run(all)
    print(errs.String())
    println("[exit", code, "]")
}

func main() {
    run(["greet"])
    run(["greet", "-n", "Slang", "--times=2", "--loud"])
    run(["sum", "--precision", "1", "1.25", "2.5", "3"])
    run(["sum", "x"])
    run(["greet", "--nmae", "x"])
    run(["sun"])
    run(["--help"])
    run(["greet", "-h"])
}
