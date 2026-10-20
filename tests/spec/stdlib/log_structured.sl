// test: std.log compiled templates: destructuring, formats, @log(skip), context, JSON lines
// status: passing
// run-flags: --leak-check
namespace spec.stdlib.log_structured

import std.io
import std

struct Card {
    Holder string
    @log(skip: true) Number string
}

func clock() int {
    return 0
}

func main() {
    var logger = log.New().Clock(clock).Enrich.WithProperty("App", "spec").Enrich.FromContext().WriteTo.Console(log.ConsoleOptions{Template: "{Level:u3} {Message:lj} {Properties}{NewLine}", Stdout: true}).WriteTo.Json(log.JsonOptions{Stdout: true}).CreateLogger()
    var s = log.Context.Push("Req", 7)
    logger.Info("{@Card} pays {Sum:0.00} as {Name}", Card{Holder: "Ann", Number: "4111"}, 3.14159, "ann")
    s.Pop()
    logger.Debug("hidden {X}", 1)
    logger.Warning("{N,4}|{H:x}|{$V}", 5, 255, [1, 2])
}
