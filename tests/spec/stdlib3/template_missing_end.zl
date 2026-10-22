// error: invalid template: template: template:1: unexpected EOF: missing {{end}} for {{if}}
// status: passing
namespace spec.stdlib3.template_missing_end

import std

struct Page {
    Show bool
}

func main() {
    _ = html.template.Parse<Page>("{{if .Show}}<b>shown</b>")
}
