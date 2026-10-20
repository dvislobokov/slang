// test: std.cli subcommands, typed @flag struct binding with defaults/env/short names, help, errors with suggestions, completion
// status: passing
// run-flags: --leak-check
namespace spec.stdlib2.cli_app

import std
import std.io

struct ServeFlags {
    @cli.flag(name: "port", short: "p", env: "SLANG_CLI_TEST_PORT", help: "listen port") Port int = 8080
    @cli.flag(help: "log every request") Verbose bool
    @cli.flag(help: "allowed origin (repeatable)") Origin []string
    MaxConns int = 100
    @cli.flag(skip: true) internal string
    Ratio float = 0.5
}

struct GlobalFlags {
    @cli.flag(short: "q", help: "no output") Quiet bool
}

func app() cli.App {
    var serve = cli.Command{Name: "serve", Aliases: ["s"], Help: "start the server", Args: "[dir]", Flags: cli.FlagsOf<ServeFlags>(),
        Run: func(c *cli.Context) void! {
            var f = cli.Bind<ServeFlags>(c)?
            var g = cli.Bind<GlobalFlags>(c)?
            println("serve port", f.Port, "verbose", f.Verbose, "origins", f.Origin, "max", f.MaxConns, "ratio", f.Ratio, "quiet", g.Quiet, "args", c.Args, "path", c.Path)
            println("ctx:", c.String("port"), c.IsSet("port"), c.Bool("verbose"), try c.Int("max-conns") else -1)
        }}
    var fail = cli.Command{Name: "fail", Help: "always fails", Run: func(c *cli.Context) void! {
        return error("it broke")
    }}
    var db = cli.Command{Name: "db", Help: "database tools", Commands: [
        cli.Command{Name: "migrate", Help: "run migrations", Flags: [cli.Flag{Name: "dry-run", Kind: "bool", Help: "only print"}], Run: func(c *cli.Context) void! {
            println("migrate dry:", c.Bool("dry-run"))
        }},
    ]}
    return cli.App{Name: "tool", Version: "1.2.0", Help: "tool does things", Flags: cli.FlagsOf<GlobalFlags>(), Commands: [serve, fail, db]}
}

func run(args []string) {
    var all = ["tool"]
    for a in args {
        all = append(all, a)
    }
    println(">", strings.Join(args, " "), "=>", app().Run(all))
}

func main() {
    run(["serve", "-p", "9000", "--verbose", "--origin", "a", "--origin=b", "-q", "www", "--ratio=1.25"])
    run(["s", "--port=1", "--max-conns", "0x10", "--", "--not-a-flag"])
    run(["serve", "--prot", "1"])
    run(["serv"])
    run(["serve", "--port"])
    run(["serve", "--port", "abc"])
    run(["serve", "--verbose=maybe"])
    run(["fail"])
    run(["db", "migrate", "--dry-run"])
    run(["db"])
    run(["--version"])
    run(["serve", "--help"])
    run([])
    run(["help", "db"])
    try os.Setenv("SLANG_CLI_TEST_PORT", "7777") catch err {
    }
    run(["serve"])
    var a = app()
    var bash = try a.Completion("bash") else ""
    println(strings.Contains(bash, "complete -F _tool_complete tool"), strings.Contains(bash, "db) words=\"migrate"), strings.Contains(bash, "serve) words=\"--port --verbose"))
    var fish = try a.Completion("fish") else ""
    println(strings.Contains(fish, "-a serve -d \"start the server\""), strings.Contains(fish, "-l port -s p -r"))
    try {
        _ = a.Completion("tcsh")?
    } catch err {
        println(err)
    }
    println(a.HelpText(["db"]) != "")
}
