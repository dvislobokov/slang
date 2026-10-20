// error: the message template must be a string literal
// status: passing
namespace spec.diagnostics.log_template_not_literal

import std

func main() {
    var t = "user {UserId}"
    log.Info(t, 42)
}
