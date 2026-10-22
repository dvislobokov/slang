// test: finished tasks, their frames and the objects they allocated or captured are released (--leak-check clean)
// status: passing
// run-flags: --leak-check
namespace spec.async2.task_leak_check

import std
import std.io

struct Payload {
    Data []int
    Name string
}

async func Work(p *Payload, i int) int {
    await task.Delay(i % 3)
    for k in 0..i {
        p.Data = append(p.Data, k)
    }
    var extra = new Payload{Name: "tmp"}
    extra.Data = p.Data
    return len(extra.Data)
}

async func main() {
    var ts []Task<int> = []
    for i in 0..20 {
        ts = append(ts, Work(new Payload{Name: "p"}, i))
    }
    var total = 0
    for t in ts {
        total += await t
    }
    println(total)
}
