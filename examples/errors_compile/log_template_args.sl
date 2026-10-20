// error: Logger.Info: the template has 2 placeholders ({UserId}, {Amount}) but 3 arguments are given
namespace examples.bad

import std

func main() {
    var logger = log.New().WriteTo.Console(log.ConsoleOptions{Stdout: true}).CreateLogger()
    logger.Info("user {UserId} paid {Amount}", 42, 9.5, "extra")
}
