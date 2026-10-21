// test: @cli.flag(default: "...") gives a flag its default as command-line text (also for []string), after the env variable; `default` is accepted as an attribute parameter and field name
// status: passing
// run-flags: --leak-check
namespace spec.stdlib2.cli_flag_default

import std
import std.io

struct Flags {
    @cli.flag(default: "9090", env: "SLANG_CLI_DEFAULT_TEST_PORT", help: "listen port") Port  int
    @cli.flag(default: "a,b")                                                           Tags  []string
    @cli.flag(default: "info")                                                          Level string = "ignored"
    Plain int = 3
}

func main() {
    var cmd = cli.Command{
        Name: "run",
        Flags: cli.FlagsOf<Flags>(),
        Run: func(c *cli.Context) void! {
            var f = cli.Bind<Flags>(c)?
            println(
                "port",
                f.Port,
                "tags",
                f.Tags,
                "level",
                f.Level,
                "plain",
                f.Plain,
                "ctx",
                c.String("port"),
            )
        },
    }
    var app = cli.App{Name: "tool", Commands: [cmd]}
    println("=>", app.Run(["tool", "run"]))
    println("=>", app.Run(["tool", "run", "--port", "1", "--tags", "x", "--level=debug"]))
    try os.Setenv("SLANG_CLI_DEFAULT_TEST_PORT", "7070") catch err {}
    println("=>", app.Run(["tool", "run"]))
    for f in cli.FlagsOf<Flags>() {
        println("flag", f.Name, "default", f.Default)
    }
}
