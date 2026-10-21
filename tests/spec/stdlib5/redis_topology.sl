// test: std.data.redis Cluster and Sentinel routing against in-process mock nodes: CLUSTER SLOTS map, MOVED (map update + reload), ASK (ASKING on the target, map unchanged), hash tags and ErrCrossSlot, pipelines split by node with a redirect inside, transactions on one slot; Sentinel master discovery, +switch-master moving the pool, READONLY after an unannounced failover asking the sentinel again, ReadFromReplicas
// status: passing
// run-flags: --leak-check
namespace spec.stdlib5.redis_topology

import std
import std.data
import std.io

func bulk(s string) string {
    return "$" + strconv.Itoa(len(s)) + "\r\n" + s + "\r\n"
}

func arr(items []string) string {
    return "*" + strconv.Itoa(len(items)) + "\r\n" + strings.Join(items, "")
}

func num(n int) string {
    return ":" + strconv.Itoa(n) + "\r\n"
}

// Shared state of the mock cluster: slot owners and migrations.
struct cluster {
    owner     []string
    migrating map[int]string
}

struct node {
    l    net.Listener
    name string
    addr string
    data map[string]string
    cl   *cluster
    // sentinel mode
    sentinel bool
    master   string
    replicas []string
    readonly bool
    subs     []net.Conn
    conns    []net.Conn
    log      []string
}

func parseCmds(buf string) ([][]string, string) {
    var out [][]string = []
    var b = buf
    for len(b) > 0 && b[0] == 42 {
        var nl = strings.Index(b, "\r\n")
        if nl < 0 {
            break
        }
        var n = try strconv.Atoi(b[1..nl]) else 0
        var p = nl + 2
        var args []string = []
        var complete = true
        for _ in 0..n {
            var rest = b[p..]
            var e = strings.Index(rest, "\r\n")
            if e < 0 {
                complete = false
                break
            }
            var l = try strconv.Atoi(rest[1..e]) else 0
            if len(rest) < e + 2 + l + 2 {
                complete = false
                break
            }
            args = append(args, rest[e + 2..e + 2 + l])
            p += e + 2 + l + 2
        }
        if !complete {
            break
        }
        out = append(out, args)
        b = b[p..]
    }
    return (out, b)
}

func slotsReply(cl *cluster) string {
    var ranges []string = []
    var start = 0
    for s in 1..16385 {
        if s == 16384 || cl.owner[s] != cl.owner[start] {
            var hp = try net.SplitHostPort(cl.owner[start]) else net.HostPort{}
            ranges = append(ranges, arr([num(start), num(s - 1), arr([
                bulk(hp.Host),
                num(hp.Port),
                bulk("id-" + strconv.Itoa(hp.Port)),
            ])]))
            start = s
        }
    }
    return arr(ranges)
}

func keyCmd(n *node, a []string, asking bool) string {
    var cmd = strings.ToUpper(a[0])
    var key = a[1]
    var slot = redis.HashSlot(key)
    var cl = n.cl
    var owner = cl.owner[slot]
    if owner != n.addr {
        var target = cl.migrating[slot]
        if !(asking && target == n.addr) {
            return "-MOVED " + strconv.Itoa(slot) + " " + owner + "\r\n"
        }
    } else {
        var target = cl.migrating[slot]
        if target != "" && n.data[key] == "" {
            return "-ASK " + strconv.Itoa(slot) + " " + target + "\r\n"
        }
    }
    n.log = append(n.log, cmd + " " + key)
    if cmd == "SET" {
        n.data[key] = "=" + a[2]
        return "+OK\r\n"
    }
    if cmd == "INCR" {
        var cur = 0
        if n.data[key] != "" {
            cur = try strconv.Atoi(n.data[key][1..]) else 0
        }
        n.data[key] = "=" + strconv.Itoa(cur + 1)
        return num(cur + 1)
    }
    if cmd == "MGET" {
        var items []string = []
        for k in a[1..] {
            if n.data[k] == "" {
                items = append(items, "$-1\r\n")
            } else {
                items = append(items, bulk(n.data[k][1..]))
            }
        }
        return arr(items)
    }
    if n.data[key] == "" {
        return "$-1\r\n"
    }
    return bulk(n.data[key][1..])
}

async func sentinelCmd(n *node, c net.Conn, a []string) string {
    var cmd = strings.ToUpper(a[0])
    if cmd == "SENTINEL" && strings.ToLower(a[1]) == "get-master-addr-by-name" {
        if a[2] != "mymaster" {
            return "*-1\r\n"
        }
        var hp = try net.SplitHostPort(n.master) else net.HostPort{}
        return arr([bulk(hp.Host), bulk(strconv.Itoa(hp.Port))])
    }
    if cmd == "SENTINEL" && strings.ToLower(a[1]) == "replicas" {
        var items []string = []
        for r in n.replicas {
            var hp = try net.SplitHostPort(r) else net.HostPort{}
            items = append(items, arr([
                bulk("ip"),
                bulk(hp.Host),
                bulk("port"),
                bulk(strconv.Itoa(hp.Port)),
                bulk("flags"),
                bulk("slave"),
            ]))
        }
        return arr(items)
    }
    if cmd == "SUBSCRIBE" {
        var out = ""
        for i, ch in a[1..] {
            out += arr([bulk("subscribe"), bulk(ch), num(i + 1)])
        }
        n.subs = append(n.subs, c)
        return out
    }
    return "-ERR unknown sentinel command\r\n"
}

async func serve(n *node, c net.Conn) {
    var buf = ""
    var asking = false
    for {
        var chunk = try await c.Read(65536) catch _ {
            break
        }
        if len(chunk) == 0 {
            break
        }
        buf += strings.FromBytes(chunk)
        var (cmds, rest) = parseCmds(buf)
        buf = rest
        var out = ""
        for a in cmds {
            var cmd = strings.ToUpper(a[0])
            if n.sentinel {
                out += await sentinelCmd(n, c, a)
                continue
            }
            switch cmd {
                case "HELLO":
                    out += "-ERR unknown command 'HELLO'\r\n"
                case "PING":
                    out += "+PONG\r\n"
                case "ASKING":
                    asking = true
                    out += "+OK\r\n"
                    continue
                case "CLUSTER":
                    out += slotsReply(n.cl)
                case "MULTI":
                    out += "+OK\r\n"
                case "EXEC":
                    out += "*0\r\n"
                case "ROLENAME":
                    out += bulk(n.name)
                default:
                    if n.readonly && cmd == "SET" {
                        out += "-READONLY You can't write against a read only replica.\r\n"
                    } else if n.cl != nil {
                        out += keyCmd(n, a, asking)
                    } else if cmd == "SET" {
                        n.data[a[1]] = "=" + a[2]
                        out += "+OK\r\n"
                    } else if cmd == "GET" {
                        if n.data[a[1]] == "" {
                            out += "$-1\r\n"
                        } else {
                            out += bulk(n.data[a[1]][1..])
                        }
                    } else {
                        out += "-ERR unknown command\r\n"
                    }
            }
            asking = false
        }
        if out != "" {
            try await c.Write(strings.Bytes(out)) catch _ {
                break
            }
        }
    }
    c.Close()
}

async func acceptLoop(n *node) {
    for {
        var c = try await n.l.Accept() catch _ {
            return
        }
        n.conns = append(n.conns, c)
        _ = serve(n, c)
    }
}

func newNode(name string, cl *cluster) *node! {
    var l = net.Listen("127.0.0.1:0")?
    var n = new node{
        l: l,
        name: name,
        addr: "127.0.0.1:" + strconv.Itoa(l.Port()),
        data: map[string]string{},
        cl: cl,
    }
    _ = acceptLoop(n)
    return n
}

func stop(n *node) {
    n.l.Close()
    for c in n.conns {
        c.Close()
    }
    n.conns = []
    n.subs = []
    n.cl = nil
}

async func publishSwitch(s *node, from string, to string) void! {
    var f = net.SplitHostPort(from)?
    var t = net.SplitHostPort(to)?
    var payload = "mymaster " +
        f.Host +
        " " +
        strconv.Itoa(f.Port) +
        " " +
        t.Host +
        " " +
        strconv.Itoa(t.Port)
    for c in s.subs {
        _ = await c.Write(strings.Bytes(arr([
            bulk("message"),
            bulk("+switch-master"),
            bulk(payload),
        ])))?
    }
}

async func clusterPart() void! {
    var cl = new cluster{migrating: map[int]string{}}
    var a = newNode("A", cl)?
    var b = newNode("B", cl)?
    for _ in 0..16384 {
        cl.owner = append(cl.owner, a.addr)
    }
    // B serves the upper half
    for s in 8192..16384 {
        cl.owner[s] = b.addr
    }
    var c = await redis.ConnectCluster([a.addr])?
    println("masters", len(c.ClusterMasters()), "reloads", c.SlotReloads())
    println("hello ->", c.NodeFor("hello") == a.addr, "foo ->", c.NodeFor("foo") == b.addr)
    await c.Set("hello", "1")?
    await c.Set("foo", "2")?
    println(await c.Get("hello")?, await c.Get("foo")?, a.log, b.log)

    // MOVED: slot of {hello} moves from A to B behind the client's back
    var ms = redis.HashSlot("{hello}")
    println("slot {hello}", ms, c.NodeFor("{hello}x") == a.addr)
    cl.owner[ms] = b.addr
    await c.Set("{hello}x", "moved")?
    println("after MOVED:", await c.Get("{hello}x")?, c.NodeFor("{hello}x") == b.addr)

    // ASK: slot of {bar} is migrating from A to B
    var askSlot = redis.HashSlot("{bar}")
    await c.Set("{bar}old", "stays")?
    cl.migrating[askSlot] = b.addr
    try await c.Get("{bar}new") catch err {
        println("ask miss:", errors.Is(err, redis.Nil))
    }
    await c.Set("{bar}new", "on-b")?
    println(
        await c.Get("{bar}old")?,
        await c.Get("{bar}new")?,
        "map unchanged:",
        c.NodeFor("{bar}new") == a.addr,
    )
    println("B saw:", b.log)

    // multi-key commands
    try await c.MGet(["hello", "foo"]) catch err {
        println("cross slot:", errors.Is(err, redis.ErrCrossSlot))
    }
    var vs = await c.MGet(["{hello}x", "{hello}y"])?
    println(vs[0].String(), vs[1].String())

    // pipeline split by node; one command is redirected
    cl.owner[redis.HashSlot("foo")] = a.addr
    var p = c.Pipeline()
    var p1 = p.Incr("hello")
    var p2 = p.Incr("foo")
    var p3 = p.Incr("bar")
    var p4 = p.Get("{hello}x")
    _ = await p.Exec()?
    println("pipeline", await p1?, await p2?, await p3?, await p4?)
    var t = c.TxPipeline()
    _ = t.Incr("{t}1")
    _ = t.Incr("{t}2")
    _ = await t.Exec()?
    var bad = c.TxPipeline()
    _ = bad.Incr("x1")
    _ = bad.Incr("x2")
    try await bad.Exec() catch err {
        println("tx across slots:", errors.Is(err, redis.ErrCrossSlot))
    }
    await c.Close()
    stop(a)
    stop(b)
}

async func sentinelPart() void! {
    var m1 = newNode("m1", nil)?
    var m2 = newNode("m2", nil)?
    var r1 = newNode("r1", nil)?
    var s = newNode("sentinel", nil)?
    s.sentinel = true
    s.master = m1.addr
    s.replicas = [r1.addr]
    r1.data["k"] = "=from-replica"
    var c = await redis.ConnectSentinelWith(redis.SentinelOptions{
        Sentinels: ["127.0.0.1:1", s.addr],
        MasterName: "mymaster",
        ReadFromReplicas: true,
    })?
    println("master is m1:", c.MasterAddr() == m1.addr)
    await c.Set("k", "v1")?
    println("read from replica:", await c.Get("k")?)
    println("written to m1:", m1.data["k"])

    // announced failover
    s.master = m2.addr
    await publishSwitch(s, m1.addr, m2.addr)?
    for _ in 0..200 {
        if c.MasterAddr() == m2.addr {
            break
        }
        await task.Delay(5)
    }
    println("switched to m2:", c.MasterAddr() == m2.addr, "failovers", c.Failovers())
    await c.Set("k", "v2")?
    println("m2 has", m2.data["k"], "m1 still", m1.data["k"])

    // unannounced failover back to m1: m2 refuses writes with READONLY
    s.master = m1.addr
    m2.readonly = true
    await c.Set("k", "v3")?
    println(
        "READONLY -> asked the sentinel:",
        c.MasterAddr() == m1.addr,
        m1.data["k"],
        "failovers",
        c.Failovers(),
    )
    await c.Close()
    try await redis.ConnectSentinel(["127.0.0.1:1"], "mymaster") catch err {
        println("no sentinel:", errors.Is(err, net.ErrRefused))
    }
    try await redis.ConnectSentinel([s.addr], "other") catch err {
        println(
            "unknown master:",
            strings.Contains(err.Message(), "does not know master \"other\""),
        )
    }
    stop(m1)
    stop(m2)
    stop(r1)
    stop(s)
}

async func main() void! {
    await clusterPart()?
    await sentinelPart()?
}
