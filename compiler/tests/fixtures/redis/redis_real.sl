// std.data.redis against real redis-server processes started by
// compiler/tests/redis.rs: a plain server (RESP3 and RESP2, every command
// group, pipelines, MULTI/EXEC, WATCH conflicts, scripts, pub/sub,
// SCAN, typed JSON values, timeouts, a pool under concurrent load), a TLS
// server with a password, a Sentinel setup whose master is killed
// (failover to the promoted replica), and a 3-master Cluster (slot map,
// hash tags, a slot migrated with ASK and then MOVED).
namespace redisreal

import std.io
import std
import std.data

struct Profile {
    Name string
    Tags []string
    Score int
}

func sorted(xs []string) []string {
    slices.Sort(xs)
    return xs
}

func env(name string) string {
    return os.Getenv(name)
}

async func standalone() void! {
    var url = "redis://127.0.0.1:" + env("REDIS_PORT") + "/1"
    var c = await redis.Connect(url)?
    await c.FlushDB()?
    println("ping", await c.Ping()?)

    // strings
    await c.Set("s", "hello")?
    println(await c.Get("s")?, await c.Append("s", " world")?, await c.StrLen("s")?, await c.GetRange("s", 0, 4)?)
    try await c.Get("missing") catch err {
        println("nil", errors.Is(err, redis.Nil))
    }
    await c.SetEx("tmp", "x", 100)?
    var ttl = await c.TTL("tmp")?
    println("ttl", ttl > 90 && ttl <= 100, await c.PTTL("missing")?)
    println("setnx", await c.SetNX("lock", "me", 5000)?, await c.SetNX("lock", "you", 5000)?)
    println("setwith xx", await c.SetWith("nokey", "v", redis.SetOptions{XX: true})?)
    println(await c.Incr("n")?, await c.IncrBy("n", 41)?, await c.DecrBy("n", 2)?, await c.IncrByFloat("f", 1.25)?)
    await c.MSet(map[string]any{"a": 1, "b": "two"})?
    var vs = await c.MGet(["a", "b", "zz"])?
    println(vs[0].String(), vs[1].String(), vs[2].String())
    println("getset", await c.GetSet("a", 10)?, await c.GetDel("a")?, await c.Exists(["a", "b"])?)
    await c.Rename("b", "b2")?
    println("type", await c.Type("b2")?, await c.Expire("b2", 50)?, await c.Persist("b2")?)

    // hashes
    _ = await c.HSetMap("h", map[string]any{"name": "ann", "age": 41})?
    println(await c.HGet("h", "name")?, await c.HIncrBy("h", "age", 1)?, await c.HExists("h", "x")?, await c.HLen("h")?)
    var hm = await c.HGetAll("h")?
    println(hm["name"], hm["age"], sorted(await c.HKeys("h")?))
    println(await c.HDel("h", ["age", "nope"])?)

    // lists
    println(await c.RPush("l", ["a", "b", "c"])?, await c.LPush("l", ["z"])?)
    println(await c.LRange("l", 0, -1)?, await c.LIndex("l", 1)?, await c.LLen("l")?)
    println(await c.LPop("l")?, await c.RPop("l")?, await c.LRem("l", 0, "a")?, await c.LRange("l", 0, -1)?)
    var popped = await c.BLPop(1, ["l"])?
    println("blpop", popped)
    try await c.BLPop(0.1, ["l"]) catch err {
        println("blpop timeout nil", errors.Is(err, redis.Nil))
    }

    // sets
    println(await c.SAdd("s1", ["a", "b", "c"])?, await c.SAdd("s2", ["b", "c", "d"])?)
    println(sorted(await c.SInter(["s1", "s2"])?), sorted(await c.SUnion(["s1", "s2"])?), sorted(await c.SDiff(["s1", "s2"])?))
    println(await c.SIsMember("s1", "a")?, await c.SCard("s1")?, await c.SRem("s1", ["a"])?, sorted(await c.SMembers("s1")?))

    // sorted sets
    _ = await c.ZAdd("z", [redis.Z{Score: 1, Member: "one"}, redis.Z{Score: 2, Member: "two"}, redis.Z{Score: 3.5, Member: "three"}])?
    println(await c.ZRange("z", 0, -1)?, await c.ZRevRange("z", 0, 0)?, await c.ZCard("z")?, await c.ZScore("z", "three")?)
    for z in await c.ZRangeWithScores("z", 0, 1)? {
        println(" ", z.Member, z.Score)
    }
    println(await c.ZRangeByScore("z", "(1", "+inf")?, await c.ZCount("z", "-inf", "2")?, await c.ZRank("z", "three")?, await c.ZIncrBy("z", 10, "one")?)
    try await c.ZScore("z", "nope") catch err {
        println("zscore nil", errors.Is(err, redis.Nil))
    }
    var mins = await c.ZPopMin("z", 1)?
    println("zpopmin", mins[0].Member, mins[0].Score)

    // streams and HyperLogLog
    var id = await c.XAdd("st", "*", map[string]any{"k": "v"})?
    var xs = await c.XRange("st", "-", "+")?
    println("stream", len(xs), xs[0].ID == id, xs[0].Values["k"], await c.XLen("st")?)
    _ = await c.PFAdd("hll", ["a", "b", "c", "a"])?
    println("pfcount", await c.PFCount(["hll"])?)

    // errors
    try await c.LPush("s", ["x"]) catch err {
        println("error", redis.ErrorCode(err))
    }
    try await c.Do(["NOSUCHCMD"]) catch err {
        println("error", redis.ErrorCode(err))
    }

    // typed values
    await c.SetJSON<Profile>("p", Profile{Name: "bob", Tags: ["a", "b"], Score: 7}, 0)?
    var pr = await c.GetJSON<Profile>("p")?
    println("json", pr.Name, pr.Tags, pr.Score)
    await redis.Set<Profile>(c, "p2", Profile{Name: "eve"})?
    println("json", (await redis.Get<Profile>(c, "p2")?).Name)

    // pipeline
    var p = c.Pipeline()
    var ts []Task<int!> = []
    for _ in 0..100 {
        ts = append(ts, p.Incr("pc"))
    }
    var last = p.Get("pc")
    var rs = await p.Exec()?
    println("pipeline", len(rs), await ts[99]?, await last?)

    // transactions
    var tp = c.TxPipeline()
    var t1 = tp.Incr("tx")
    var t2 = tp.Get("tx")
    _ = await tp.Exec()?
    println("multi", await t1?, await t2?)
    var other = await redis.Connect(url)?
    var attempts = 0
    for {
        attempts += 1
        var conflict = attempts == 1
        try await c.Watch(["wk"], async func(tx *redis.Tx) void! {
            var cur = try await tx.Get("wk") else "0"
            var n = strings.ToInt(cur)?
            if conflict {
                await other.Set("wk", "100")?
            }
            var q = tx.TxPipeline()
            _ = q.Set("wk", n + 1)
            _ = await q.Exec()?
        }) catch err {
            if errors.Is(err, redis.ErrTxFailed) {
                println("watch conflict, retrying")
                continue
            }
            return err
        }
        break
    }
    println("watch", attempts, await c.Get("wk")?)
    await other.Close()

    // scripts
    var sc = redis.NewScript("return redis.call('INCRBY', KEYS[1], ARGV[1])")
    await c.ScriptFlush()?
    println("exists before", await c.ScriptExists([sc.Hash])?)
    println((await c.Run(sc, ["sk"], [5])?).String(), (await c.Run(sc, ["sk"], [5])?).String())
    println("exists after", await c.ScriptExists([sc.Hash])?)
    var ev = await c.Eval("return {KEYS[1], ARGV[1], 3}", ["k"], ["v"])?
    println(ev.String())

    // pub/sub
    var sub = await c.Subscribe(["news"])?
    await sub.PSubscribe(["ev.*"])?
    println("receivers", await c.Publish("news", "n1")?, await c.Publish("ev.login", "u1")?)
    var got = 0
    for m in sub {
        println(" ", m.Kind, m.Channel, m.Pattern, m.Payload)
        got += 1
        if got == 2 {
            break
        }
    }
    await sub.Unsubscribe(["news"])?
    println("receivers after unsubscribe", await c.Publish("news", "n2")?)
    await sub.Close()

    // SCAN
    for i in 0..50 {
        await c.Set(fmt.Format("scan:{0}", i), i)?
    }
    var n = 0
    for k in c.Iter("scan:*", 7) {
        if strings.HasPrefix(k, "scan:") {
            n += 1
        }
    }
    println("scan", n, len(await c.Keys("scan:*")?))

    // concurrency through the pool (10 connections, 200 tasks)
    var tasks []Task<int!> = []
    for _ in 0..200 {
        tasks = append(tasks, c.Incr("conc"))
    }
    var max = 0
    for t in tasks {
        max = math.IMax(max, await t?)
    }
    var st = c.PoolStats()
    println("concurrent", max, st.TotalConns <= 10)

    // timeouts through the task context (the connection is discarded)
    var start = time.Now()
    try await task.WithTimeout(150, func() Task<[]string!> {
        return c.BLPop(10, ["never"])
    }) catch err {
        println("timeout", errors.Is(err, task.ErrCancelled), time.Now() - start < 2000)
    }
    println("after timeout", await c.Ping()?)

    // RESP2
    var c2 = await redis.Connect("redis://127.0.0.1:" + env("REDIS_PORT") + "/1?protocol=2")?
    var h2 = await c2.HGetAll("h")?
    println("resp2", h2["name"], (await c2.ZRangeWithScores("z", 0, 0)?)[0].Member, (await c2.Do(["HELLO"])?).Kind == redis.Kind.Array)
    await c2.Close()
    await c.Close()
}

async func tls() void! {
    if env("REDIS_TLS_PORT") == "" {
        println("tls skipped")
        return
    }
    var o = redis.ParseURL("rediss://:sekret@localhost:" + env("REDIS_TLS_PORT") + "/2")?
    var pool = crypto.x509.NewCertPool()
    _ = pool.AppendPEM(io.ReadFile(env("REDIS_TLS_CA"))?)
    o.TLS.RootCAs = pool
    var c = await redis.ConnectWith(o)?
    await c.Set("t", "over tls")?
    println("tls", await c.Get("t")?)
    await c.Close()
    var bad = o
    bad.Password = "wrong"
    try await redis.ConnectWith(bad) catch err {
        println("tls auth", redis.ErrorCode(err))
    }
    var noca = redis.ParseURL("rediss://:sekret@localhost:" + env("REDIS_TLS_PORT"))?
    try await redis.ConnectWith(noca) catch err {
        println("tls untrusted certificate rejected")
    }
}

async func sentinel() void! {
    var s = "127.0.0.1:" + env("SENTINEL_PORT")
    var c = await redis.ConnectSentinel(["127.0.0.1:1", s], "mymaster")?
    var first = c.MasterAddr()
    println("sentinel master", first == "127.0.0.1:" + env("MASTER_PORT"))
    await c.Set("fk", "before")?
    // wait until the replica has the key, then kill the master
    var r = redis.NewClient(redis.Options{Addr: "127.0.0.1:" + env("REPLICA_PORT")})
    for _ in 0..100 {
        var v = try await r.Get("fk") else ""
        if v == "before" {
            break
        }
        await task.Delay(50)
    }
    await r.Close()
    _ = await process.Run("kill", ["-9", env("MASTER_PID")])?
    for {
        var dead = try await net.Dial(first) catch _ {
            break
        }
        dead.Close()
        await task.Delay(20)
    }
    println("master killed")
    var ok = false
    var start = time.Now()
    for time.Now() - start < 30000 {
        var v = try await c.Get("fk") catch _ {
            await task.Delay(100)
            continue
        }
        try await c.Set("fk", "after") catch _ {
            await task.Delay(100)
            continue
        }
        println("failover", v, await c.Get("fk")?, c.MasterAddr() == "127.0.0.1:" + env("REPLICA_PORT"), c.Failovers() >= 1)
        ok = true
        break
    }
    println("reconnected to the promoted replica", ok)
    await c.Close()
}

async func clusterTest() void! {
    var ports = strings.Split(env("CLUSTER_PORTS"), ",")
    var seeds []string = []
    for p in ports {
        seeds = append(seeds, "127.0.0.1:" + p)
    }
    var c = await redis.ConnectCluster(seeds[..1])?
    println("cluster masters", len(c.ClusterMasters()))
    await c.FlushAll()?
    for i in 0..100 {
        await c.Set(fmt.Format("ck{0}", i), i)?
    }
    var sum = 0
    for i in 0..100 {
        sum += strings.ToInt(await c.Get(fmt.Format("ck{0}", i))?)?
    }
    println("cluster sum", sum, await c.DBSize()?)
    var nodes = map[string]bool{}
    for i in 0..100 {
        nodes[c.NodeFor(fmt.Format("ck{0}", i))] = true
    }
    println("keys on", len(nodes), "nodes")
    await c.MSet(map[string]any{"{u}a": 1, "{u}b": 2})?
    var vs = await c.MGet(["{u}a", "{u}b"])?
    println("hash tag", vs[0].String(), vs[1].String())
    try await c.MGet(["ck1", "ck2"]) catch err {
        println("cross slot", errors.Is(err, redis.ErrCrossSlot))
    }
    var p = c.Pipeline()
    var ts []Task<int!> = []
    for i in 0..30 {
        ts = append(ts, p.Incr(fmt.Format("pc{0}", i)))
    }
    _ = await p.Exec()?
    var tot = 0
    for t in ts {
        tot += await t?
    }
    println("cluster pipeline", tot)
    var tp = c.TxPipeline()
    var x = tp.Incr("{t}x")
    var y = tp.IncrBy("{t}y", 5)
    _ = await tp.Exec()?
    println("cluster tx", await x?, await y?)
    var n = 0
    for k in c.Iter("ck*", 20) {
        _ = k
        n += 1
    }
    println("cluster scan", n)
    var sc = redis.NewScript("return redis.call('GET', KEYS[1])")
    println("cluster script", (await c.Run(sc, ["ck7"], [])?).String())

    // migrate the slot of {mv}: ASK while migrating, MOVED afterwards
    var key = "{mv}k"
    var slot = redis.HashSlot(key)
    await c.Set(key, "migrated value")?
    var src = c.NodeFor(key)
    var dst = ""
    for m in c.ClusterMasters() {
        if m != src && dst == "" {
            dst = m
        }
    }
    var sc1 = redis.NewClient(redis.Options{Addr: src})
    var dc = redis.NewClient(redis.Options{Addr: dst})
    var srcID = (await sc1.Do(["CLUSTER", "MYID"])?).Text()?
    var dstID = (await dc.Do(["CLUSTER", "MYID"])?).Text()?
    _ = await dc.Do(["CLUSTER", "SETSLOT", slot, "IMPORTING", srcID])?
    _ = await sc1.Do(["CLUSTER", "SETSLOT", slot, "MIGRATING", dstID])?
    println("during migration", await c.Get(key)?)
    try await c.Get("{mv}new") catch err {
        println("ask nil", errors.Is(err, redis.Nil))
    }
    await c.Set("{mv}new", "written via ASK")?
    println("ask", await c.Get("{mv}new")?, c.NodeFor(key) == src)
    var dh = net.SplitHostPort(dst)?
    _ = await sc1.Do(["MIGRATE", dh.Host, dh.Port, "", 0, 5000, "KEYS", key])?
    for m in c.ClusterMasters() {
        var mc = redis.NewClient(redis.Options{Addr: m})
        _ = await mc.Do(["CLUSTER", "SETSLOT", slot, "NODE", dstID])?
        await mc.Close()
    }
    println("after MOVED", await c.Get(key)?, await c.Get("{mv}new")?, c.NodeFor(key) == dst)
    await sc1.Close()
    await dc.Close()
    await c.Close()
}

async func main() void! {
    await standalone()?
    await tls()?
    await sentinel()?
    await clusterTest()?
}
