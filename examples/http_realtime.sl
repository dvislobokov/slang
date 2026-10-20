namespace examples.http_realtime

import std.io
import std
import std.net

// A small realtime web service on std.net.http: HTTP/2 (h2c, multiplexed
// requests), middleware (Recover, CORS, Gzip), a WebSocket chat room
// (std.net.ws) and a Server-Sent Events feed, tested with
// std.net.http.test.

struct Room {
    mu task.Mutex
    members []ws.Conn
    history []string
}

async func broadcast(room *Room, text string) void! {
    await room.mu.Lock()
    room.history = append(room.history, text)
    var members = room.members
    room.mu.Unlock()
    for m in members {
        try await m.SendText(text) catch err {}
    }
}

async func chat(room *Room, req *http.Request, res *http.Response) void! {
    var conn = try await ws.Upgrade(req, res) catch err {
        return
    }
    var who = req.QueryValue("name")
    await room.mu.Lock()
    room.members = append(room.members, conn)
    room.mu.Unlock()
    await conn.SendText("welcome, " + who)?
    for msg in conn {
        await broadcast(room, who + ": " + msg.Text())?
    }
}

async func feed(room *Room, req *http.Request, res *http.Response) void! {
    var sse = http.ServerSentEvents(res)
    await room.mu.Lock()
    var lines = room.history
    room.mu.Unlock()
    for i, line in lines {
        await sse.SendEvent(http.SSEEvent{ID: fmt.Format("{0}", i + 1), Event: "chat", Data: line})?
    }
}

async func main() void! {
    var room = new Room{}
    var r = http.NewRouter()
    r.Use(http.Recover())
    r.Use(http.CORS(http.CORSOptions{}))
    r.Use(http.Gzip(0))
    r.Get("/hello/{name}", async func(req *http.Request, res *http.Response) void! {
        res.Text(200, "hello, " + req.Param("name") + " (" + req.Proto + ")\n")
    })
    r.Get("/chat", func(req *http.Request, res *http.Response) Task<void!> {
        return chat(room, req, res)
    })
    r.Get("/feed", func(req *http.Request, res *http.Response) Task<void!> {
        return feed(room, req, res)
    })
    var ts = http.test.NewH2CServer(r)?

    // HTTP/2: three requests at once on one connection
    var (a, b, c) = await task.All(ts.Client.Get(ts.URL + "/hello/ann"), ts.Client.Get(ts.URL + "/hello/bob"), ts.Client.Get(ts.URL + "/hello/cy"))?
    print(a.Body, b.Body, c.Body)
    println("connections:", ts.Transport.Stats().Dials)

    // WebSocket chat (HTTP/1.1 upgrade on the same server)
    var wsURL = strings.Replace(ts.URL, "http://", "ws://") + "/chat"
    var ann = await ws.Dial(wsURL + "?name=ann")?
    var bob = await ws.Dial(wsURL + "?name=bob")?
    println((await ann.Recv()?).Text(), "/", (await bob.Recv()?).Text())
    await ann.SendText("hi bob")?
    println("bob got:", (await bob.Recv()?).Text())
    println("ann got:", (await ann.Recv()?).Text())
    await bob.SendText("hi ann")?
    println("ann got:", (await ann.Recv()?).Text())
    println("bob got:", (await bob.Recv()?).Text())
    await ann.Close()?
    await bob.Close()?

    // the history as Server-Sent Events
    var es = http.NewEventSource(ts.URL + "/feed")
    es.Client = ts.Client
    es.MaxRetries = -1
    for ev in es {
        println("event", ev.ID, ev.Event, ev.Data)
    }
    await ts.Close()
}
