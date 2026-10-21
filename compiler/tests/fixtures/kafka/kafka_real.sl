// std.data.kafka against a real broker (compiler/tests/kafka.rs, ignored
// unless KAFKA_BROKERS is set; see README.md next to this file). The
// topics get a unique suffix so the program can run repeatedly against
// the same cluster.
//
// KAFKA_BROKERS       plaintext bootstrap list (required)
// KAFKA_SASL_BROKERS  optional SASL_PLAINTEXT listener with users
//                     alice/alice-secret (PLAIN), bob/bob-secret
//                     (SCRAM-SHA-256), carol/carol-secret (SCRAM-SHA-512)
namespace kafkareal

import std.io
import std
import std.data

func brokers() []string {
    return strings.Split(os.Getenv("KAFKA_BROKERS"), ",")
}

async func consumeN(c *kafka.Consumer, n int) []kafka.Record! {
    var out []kafka.Record = []
    var start = time.Now()
    for len(out) < n && time.Now() - start < 30000 {
        for r in await c.Poll(500)? {
            out = append(out, r)
        }
    }
    return out
}

func values(rs []kafka.Record) []string {
    var out []string = []
    for r in rs {
        out = append(out, strings.FromBytes(r.Value))
    }
    slices.Sort(out)
    return out
}

async func main() void! {
    var suffix = strconv.Itoa(time.Now())
    var topic = "slang-it-" + suffix
    var admin = await kafka.NewAdmin(brokers())?
    println("brokers", len(admin.Brokers()) > 0)
    await admin.CreateTopic(topic, 3, 1)?
    try await admin.CreateTopic(topic, 3, 1) catch err {
        println("create again:", kafka.ErrorCode(err), err.Message() == "kafka: TOPIC_ALREADY_EXISTS (36): topic \"" + topic + "\": Topic '" + topic + "' already exists.")
    }
    var info = await admin.DescribeTopic(topic)?
    println("partitions", len(info.Partitions), info.Partitions[0].Leader >= 0)
    var listed = false
    for t in await admin.ListTopics()? {
        if t.Name == topic {
            listed = true
        }
    }
    println("listed", listed)

    // idempotent producer, key partitioning like the Java client
    var p = await kafka.NewProducer(brokers(), kafka.ProducerOptions{Idempotent: true, LingerMs: 20})?
    println("producer id", p.ProducerID() >= 0)
    var ts []Task<kafka.RecordMetadata!> = []
    for i in 0..30 {
        ts = append(ts, p.Send(topic, fmt.Format("key{0}", i % 10), fmt.Format("v{0}", i)))
    }
    var byPart = map[int]int{}
    var keyPart = true
    for i, t in ts {
        var m = await t?
        byPart[m.Partition] += 1
        if m.Partition != kafka.HashPartition(strings.Bytes(fmt.Format("key{0}", i % 10)), 3) {
            keyPart = false
        }
    }
    println("partitioned by murmur2", keyPart, byPart[0] + byPart[1] + byPart[2])
    var (batches, retries) = p.Stats()
    println("batched", batches < 30, retries)
    await p.Close()
    try await p.Send(topic, "", "late") catch err {
        println("closed:", errors.Is(err, kafka.ErrClosed))
    }

    // compression and acks
    for codec in ["gzip", "zstd"] {
        var cp = await kafka.NewProducer(brokers(), kafka.ProducerOptions{Compression: codec, Acks: "leader"})?
        var big = strings.Repeat("compressible ", 2000)
        var m = await cp.SendTo(topic, 0, "", codec + ":" + big)?
        println(codec, m.Partition, m.Offset >= 0)
        await cp.Close()
    }
    var np = await kafka.NewProducer(brokers(), kafka.ProducerOptions{Acks: "none"})?
    var nm = await np.SendTo(topic, 1, "", "fire and forget")?
    println("acks none offset", nm.Offset)
    await np.Close()
    var hp = await kafka.NewProducer(brokers(), kafka.ProducerOptions{})?
    _ = await hp.SendRecord(kafka.Record{Topic: topic, Partition: 2, Key: strings.Bytes("hk"), Value: strings.Bytes("with headers"), Headers: [kafka.Header{Key: "trace", Value: strings.Bytes("abc")}], Timestamp: 1700000000123}, true)?
    await hp.Close()

    // a consumer group reading everything from the start
    var group = "slang-g-" + suffix
    var c1 = await kafka.NewConsumerWith(brokers(), kafka.ConsumerOptions{Group: group, Topics: [topic], StartOffset: "earliest", HeartbeatInterval: 500, ManualCommit: true})?
    println("member assignment", len(c1.Assignment()))
    var got = await consumeN(c1, 34)?
    var gz = 0
    var hdr = ""
    for r in got {
        var v = strings.FromBytes(r.Value)
        if strings.HasPrefix(v, "gzip:") || strings.HasPrefix(v, "zstd:") {
            gz += len(v)
        }
        if len(r.Headers) == 1 {
            hdr = r.Headers[0].Key + "=" + strings.FromBytes(r.Headers[0].Value) + fmt.Format(" ts={0}", r.Timestamp)
        }
    }
    println("consumed", len(got), "compressed bytes", gz, "header", hdr)
    await c1.Commit()?
    var committed = 0
    for i in 0..3 {
        committed += await c1.Committed(topic, i)?
    }
    println("committed", committed)

    // a second member joins: the group rebalances to 2 + 1 partitions
    // (c1 rejoins inside Poll, so it polls while c2 joins)
    var c2t = kafka.NewConsumerWith(brokers(), kafka.ConsumerOptions{Group: group, Topics: [topic], StartOffset: "earliest", HeartbeatInterval: 500, Assignor: "range"})
    var start = time.Now()
    for time.Now() - start < 30000 {
        _ = await c1.Poll(200)?
        if await task.Timeout(c2t, 0) {
            break
        }
    }
    var c2 = await c2t?
    _ = await c1.Poll(200)?
    println("rebalanced", len(c1.Assignment()) + len(c2.Assignment()), c1.Generation() == c2.Generation(), c1.Rebalances() >= 2)
    // new records go to both members, nothing already committed is redelivered
    var p2 = await kafka.NewProducer(brokers(), kafka.ProducerOptions{})?
    for i in 0..9 {
        _ = await p2.SendTo(topic, i % 3, "", fmt.Format("after{0}", i))?
    }
    await p2.Close()
    var both []kafka.Record = []
    start = time.Now()
    for len(both) < 9 && time.Now() - start < 20000 {
        for r in await c1.Poll(200)? {
            both = append(both, r)
        }
        for r in await c2.Poll(200)? {
            both = append(both, r)
        }
    }
    println("after rebalance", values(both))
    await c2.Close()
    await c1.Close()

    // a consumer without a group, from the end, then iteration
    var tail = await kafka.NewConsumerWith(brokers(), kafka.ConsumerOptions{Topics: [topic], StartOffset: "latest"})?
    var p3 = await kafka.NewProducer(brokers(), kafka.ProducerOptions{})?
    _ = await p3.SendTo(topic, 0, "x", "newest")?
    await p3.Close()
    for r in tail {
        println("tail", r.Partition, strings.FromBytes(r.Key), strings.FromBytes(r.Value))
        break
    }
    await tail.Close()

    // SASL
    var sasl = os.Getenv("KAFKA_SASL_BROKERS")
    if sasl != "" {
        var users = [["PLAIN", "alice", "alice-secret"], ["SCRAM-SHA-256", "bob", "bob-secret"], ["SCRAM-SHA-512", "carol", "carol-secret"]]
        for u in users {
            var cfg = kafka.Config{SASL: kafka.SASL{Mechanism: u[0], Username: u[1], Password: u[2]}}
            var sp = await kafka.NewProducer(strings.Split(sasl, ","), kafka.ProducerOptions{Config: cfg})?
            var m = await sp.SendTo(topic, 0, "", "via " + u[0])?
            println("sasl", u[0], m.Offset > 0)
            await sp.Close()
            var bad = kafka.Config{SASL: kafka.SASL{Mechanism: u[0], Username: u[1], Password: "wrong"}}
            try await kafka.NewAdminWith(strings.Split(sasl, ","), bad) catch err {
                println("sasl", u[0], "wrong password:", kafka.ErrorCode(err) == 58 || strings.Contains(err.Message(), "SCRAM"))
            }
        }
    } else {
        println("sasl skipped")
    }

    await admin.DeleteTopic(topic)?
    println("deleted")
    admin.Close()
}
