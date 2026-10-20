namespace examples.task_context

import std.io
import std

// Task-context values: typed keys, scoped with task.With, visible in the
// started task, in nested calls and in spawned children, and gone after
// With returns.

struct Span {
    Name string
}

var CurrentSpan = task.Key<Span>()

func SpanName() string {
    var s = task.Get(CurrentSpan)
    if s == nil {
        return "(none)"
    }
    return s.Name
}

async func Child(label string) {
    await task.Delay(1)
    println(label, "sees", SpanName())
}

async func Handle() {
    println("handler sees", SpanName())
    await Child("nested call")
    await task.Go(async func() {
        await Child("spawned child")
    })
    await task.With(CurrentSpan, Span{Name: "inner"}, async func() {
        await Child("inner scope")
    })
    println("handler again sees", SpanName())
}

async func main() {
    println("before:", SpanName())
    await task.With(CurrentSpan, Span{Name: "request-1"}, Handle)
    println("after:", SpanName())
}
