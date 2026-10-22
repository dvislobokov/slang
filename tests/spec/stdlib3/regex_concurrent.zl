// test: one std.regex.Regex shared by 8 concurrent tasks (the DFA cache is locked; a task that finds it busy runs the Pike VM) gives the same results everywhere
// status: passing
namespace spec.stdlib3.regex_concurrent

import std
import std.io

async func worker(re regex.Regex, text string, rounds int) int {
    var total = 0
    for _ in 0..rounds {
        total += len(re.FindAll(text))
        await task.Yield()
    }
    return total
}

async func main() {
    var re = regex.Pattern(`[a-z]+@[a-z]+\.(com|org)`)
    var text = strings.Repeat("mail bob@example.com or eve@test.org now ", 200)
    var tasks []Task<int> = []
    for _ in 0..8 {
        tasks = append(tasks, task.Spawn(() => worker(re, text, 20)))
    }
    var sum = 0
    for t in tasks {
        sum += await t
    }
    println(sum, 8 * 20 * 400)
}
