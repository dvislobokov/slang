// test: two producers and one consumer; the last producer to finish closes the channel and the consumer drains it
// status: passing
namespace spec.async2.producer_consumer_close

import std.io
import std

struct State {
    Mu task.Mutex
    Live int
}

async func Produce(name string, n int, ch task.Channel<string>, st *State) {
    for _ in 0..n {
        await ch.Send(name)
        await task.Delay(1)
    }
    await st.Mu.Lock()
    st.Live -= 1
    var last = st.Live == 0
    st.Mu.Unlock()
    if last {
        ch.Close()
    }
}

async func Consume(ch task.Channel<string>) map[string]int {
    var counts = map[string]int{}
    for {
        var v = try await ch.Recv() else break
        counts[v] += 1
    }
    return counts
}

async func main() {
    var ch = task.Channel<string>(1)
    var st = new State{Live: 2}
    var c = Consume(ch)
    var a = Produce("a", 5, ch, st)
    var b = Produce("b", 8, ch, st)
    await a
    await b
    var counts = await c
    println(counts["a"], counts["b"], len(counts))
}
