// std.data.kafka: a producer with batching and key partitioning, and a
// consumer reading it back. So that the example runs anywhere it talks to
// a tiny in-process broker (bottom of the file: one topic, in-memory log,
// no consumer groups); point the bootstrap list at a real cluster
// ("localhost:9092") to use groups (kafka.NewConsumer(brokers, group, topics)).
namespace examples.kafka

import std.io
import std
import std.data

async func main() void! {
    var b = startBroker("orders", 3)?
    var brokers = [b.addr]

    // records with the same key go to the same partition (murmur2, like
    // the Java client); Send completes when the broker acknowledged it
    var p = await kafka.NewProducer(brokers, kafka.ProducerOptions{Compression: "gzip", LingerMs: 10})?
    var sent []Task<kafka.RecordMetadata!> = []
    for i in 0..6 {
        var customer = fmt.Format("customer-{0}", i % 3)
        sent = append(sent, p.Send("orders", customer, fmt.Format("order #{0}", i)))
    }
    for t in sent {
        var m = await t?
        println("stored in partition", m.Partition, "at offset", m.Offset)
    }
    await p.Close()

    // a consumer of every partition from the start
    var c = await kafka.NewConsumerWith(brokers, kafka.ConsumerOptions{Topics: ["orders"], StartOffset: "earliest"})?
    var got []string = []
    for rec in c {
        got = append(got, fmt.Format("{0}: {1}", strings.FromBytes(rec.Key), strings.FromBytes(rec.Value)))
        if len(got) == 6 {
            break
        }
    }
    slices.Sort(got)
    for g in got {
        println(g)
    }
    await c.Close()
    b.stop()
}

// ---- a tiny broker (not part of the client) ----------------------------------------

struct broker {
    l net.Listener
    addr string
    topic string
    logs [][]byte
    next []int
    conns []net.Conn

    func stop() {
        this.l.Close()
        for c in this.conns {
            c.Close()
        }
        this.conns = []
    }
}

async func reply(b *broker, key int, d kafka.Decoder) []byte! {
    var dd = d
    var e = kafka.Encoder{}
    switch key {
        case 18:
            // ApiVersions: no list, the client then uses its preferred versions
            e.Int16(0)
            e.ArrayLen(0)
        case 3:
            // Metadata v1: this broker leads every partition
            e.ArrayLen(1)
            e.Int32(1)
            var hp = net.SplitHostPort(b.addr)?
            e.String(hp.Host)
            e.Int32(hp.Port)
            e.NullableString("", true)
            e.Int32(1)
            e.ArrayLen(1)
            e.Int16(0)
            e.String(b.topic)
            e.Bool(false)
            e.ArrayLen(len(b.logs))
            for i in 0..len(b.logs) {
                e.Int16(0)
                e.Int32(i)
                e.Int32(1)
                e.ArrayLen(1)
                e.Int32(1)
                e.ArrayLen(1)
                e.Int32(1)
            }
        case 0:
            // Produce v7: append each batch with its base offset rewritten
            _ = dd.Int16()
            _ = dd.Int16()
            _ = dd.Int32()
            _ = dd.ArrayLen()
            e.ArrayLen(1)
            e.String(dd.String())
            var n = dd.ArrayLen()
            e.ArrayLen(n)
            for _ in 0..n {
                var part = dd.Int32()
                var batch = dd.Bytes()
                var count = len(kafka.DecodeBatches(batch, b.topic, part)?[0].Records)
                var base = b.next[part]
                for k in 0..8 {
                    batch[k] = byte((base >> (8 * (7 - k))) & 255)
                }
                b.logs[part] = bytes.Join([b.logs[part], batch], [])
                b.next[part] += count
                e.Int32(part)
                e.Int16(0)
                e.Int64(base)
                e.Int64(-1)
                e.Int64(0)
            }
            e.Int32(0)
        case 2:
            // ListOffsets v1: earliest 0, latest the end of the log
            _ = dd.Int32()
            _ = dd.ArrayLen()
            e.ArrayLen(1)
            e.String(dd.String())
            var n = dd.ArrayLen()
            e.ArrayLen(n)
            for _ in 0..n {
                var part = dd.Int32()
                var ts = dd.Int64()
                e.Int32(part)
                e.Int16(0)
                e.Int64(-1)
                if ts == -2 {
                    e.Int64(0)
                } else {
                    e.Int64(b.next[part])
                }
            }
        case 1:
            // Fetch v10: the whole log of each partition asked from offset 0
            for _ in 0..4 {
                _ = dd.Int32()
            }
            _ = dd.Int8()
            _ = dd.Int32()
            _ = dd.Int32()
            await task.Delay(10)
            e.Int32(0)
            e.Int16(0)
            e.Int32(0)
            _ = dd.ArrayLen()
            e.ArrayLen(1)
            e.String(dd.String())
            var n = dd.ArrayLen()
            e.ArrayLen(n)
            for _ in 0..n {
                var part = dd.Int32()
                _ = dd.Int32()
                var off = dd.Int64()
                _ = dd.Int64()
                _ = dd.Int32()
                e.Int32(part)
                e.Int16(0)
                e.Int64(b.next[part])
                e.Int64(b.next[part])
                e.Int64(0)
                e.Int32(-1)
                if off == 0 {
                    e.Bytes(b.logs[part])
                } else {
                    e.Bytes([])
                }
            }
    }
    return e.B
}

async func serve(b *broker, c net.Conn) {
    var buf []byte = []
    for {
        var chunk = try await c.Read(65536) catch _ {
            break
        }
        if len(chunk) == 0 {
            break
        }
        buf = bytes.Join([buf, chunk], [])
        for len(buf) >= 4 {
            var size = kafka.Decoder{B: buf}.Int32()
            if len(buf) < 4 + size {
                break
            }
            var d = kafka.Decoder{B: bytes.Clone(buf[4..4 + size])}
            buf = bytes.Clone(buf[4 + size..])
            var key = d.Int16()
            _ = d.Int16()
            var corr = d.Int32()
            _ = d.String()
            var body = try await reply(b, key, d) catch _ {
                c.Close()
                return
            }
            var e = kafka.Encoder{}
            e.Int32(4 + len(body))
            e.Int32(corr)
            e.Raw(body)
            _ = try await c.Write(e.B) else 0
        }
    }
    c.Close()
}

async func acceptAll(b *broker) {
    for {
        var c = try await b.l.Accept() catch _ {
            return
        }
        b.conns = append(b.conns, c)
        _ = serve(b, c)
    }
}

func startBroker(topic string, partitions int) *broker! {
    var l = net.Listen("127.0.0.1:0")?
    var b = new broker{l: l, addr: "127.0.0.1:" + strconv.Itoa(l.Port()), topic: topic}
    for _ in 0..partitions {
        b.logs = append(b.logs, [])
        b.next = append(b.next, 0)
    }
    _ = acceptAll(b)
    return b
}
