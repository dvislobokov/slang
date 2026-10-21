// test: std.data.kafka encodings without a broker: record batches v2 byte-exact against kafka-clients 3.7.0 (MemoryRecords.withRecords / withIdempotentRecords: CRC-32C, varint records, headers, null keys), decoding them back, gzip and zstd batches, CRC corruption, a truncated trailing batch, unsupported codecs, murmur2 key partitioning (Utils.murmur2 / toPositive), range and round-robin assignors, Encoder/Decoder primitives, error names
// status: passing
// run-flags: --leak-check
namespace spec.stdlib5.kafka_codec

import std
import std.data
import std.io

func hex(b []byte) string {
    return encoding.hex.Encode(b)
}

func show(bs []kafka.Batch) {
    for b in bs {
        println(
            "batch",
            b.BaseOffset,
            b.LastOffset,
            "codec",
            b.Codec,
            "producer",
            b.Producer.ID,
            b.Producer.Epoch,
            b.Producer.BaseSequence,
        )
        for r in b.Records {
            var hs []string = []
            for h in r.Headers {
                hs = append(hs, h.Key + "=" + strings.FromBytes(h.Value))
            }
            println(
                "  ",
                r.Topic,
                r.Partition,
                r.Offset,
                r.Timestamp,
                strconv.Quote(strings.FromBytes(r.Key)),
                strconv.Quote(strings.FromBytes(r.Value)),
                hs,
            )
        }
    }
}

func main() void! {
    // MemoryRecords.withRecords(MAGIC_VALUE_V2, 0L, NONE, CREATE_TIME,
    //   SimpleRecord(1700000000000L, "k1", "v1", [h=x]), SimpleRecord(1700000000005L, null, "v2"))
    var java = "000000000000000000000049ffffffff0297f4442a0000000000010000018bcfe568000000018bcfe56805ffffffffffffffffffffffffffff000000021c000000046b31047631020268027810000a020104763200"
    // withIdempotentRecords(..., producerId 1000, epoch 0, baseSequence 5, partitionLeaderEpoch 0, ...)
    var javaIdem = "0000000000000000000000490000000002eeca44ea0000000000010000018bcfe568000000018bcfe5680500000000000003e8000000000005000000021c000000046b31047631020268027810000a020104763200"
    var recs = [
        kafka.Record{
            Key: strings.Bytes("k1"),
            Value: strings.Bytes("v1"),
            Headers: [kafka.Header{Key: "h", Value: strings.Bytes("x")}],
            Timestamp: 1700000000000,
        },
        kafka.Record{Value: strings.Bytes("v2"), Timestamp: 1700000000005},
    ]
    var b = kafka.EncodeBatch(recs, 0, 0, 0, kafka.BatchProducer{
        ID: -1,
        Epoch: -1,
        BaseSequence: -1,
    })?
    println("plain batch matches the Java client:", hex(b) == java)
    var bi = kafka.EncodeBatch(recs, 0, 0, 0, kafka.BatchProducer{
        ID: 1000,
        Epoch: 0,
        BaseSequence: 5,
    })?
    // the Java vector has partitionLeaderEpoch 0 where ours is -1 (set by the broker)
    var javaIdemBytes = try encoding.hex.Decode(javaIdem) else []
    var ours = hex(bi)
    println(
        "idempotent batch matches except the leader epoch:",
        ours[..24] == javaIdem[..24],
        ours[32..] == javaIdem[32..],
        ours[24..32],
        javaIdem[24..32],
    )
    show(kafka.DecodeBatches(javaIdemBytes, "orders", 3)?)

    // two batches back to back with offsets 10 and 12, and a cut-off third
    var b2 = kafka.EncodeBatch(
        [kafka.Record{Value: strings.Bytes("x"), Timestamp: 1}],
        12,
        0,
        0,
        kafka.BatchProducer{ID: -1, Epoch: -1, BaseSequence: -1},
    )?
    var b1 = kafka.EncodeBatch(recs, 10, 0, 0, kafka.BatchProducer{
        ID: -1,
        Epoch: -1,
        BaseSequence: -1,
    })?
    var stream = bytes.Join([b1, b2, b2[..30]], [])
    show(kafka.DecodeBatches(stream, "t", 0)?)

    // compression: the records section is gzip / zstd
    var many []kafka.Record = []
    for i in 0..200 {
        many = append(many, kafka.Record{
            Key: strings.Bytes(fmt.Format("key-{0}", i % 7)),
            Value: strings.Bytes(strings.Repeat("payload ", 20)),
            Timestamp: 1700000000000 + i,
        })
    }
    var plain = kafka.EncodeBatch(many, 0, 0, 0, kafka.BatchProducer{
        ID: -1,
        Epoch: -1,
        BaseSequence: -1,
    })?
    for codec in [1, 4] {
        var z = kafka.EncodeBatch(many, 100, codec, 0, kafka.BatchProducer{
            ID: -1,
            Epoch: -1,
            BaseSequence: -1,
        })?
        var back = kafka.DecodeBatches(z, "t", 1)?
        var rs = back[0].Records
        println(
            "codec",
            codec,
            "smaller",
            len(z) * 5 < len(plain),
            "records",
            len(rs),
            rs[0].Offset,
            rs[199].Offset,
            strings.FromBytes(rs[199].Key),
            rs[199].Timestamp,
        )
    }

    // corruption and unsupported codecs
    var bad = bytes.Clone(b)
    bad[len(bad) - 3] = 0x41
    try kafka.DecodeBatches(bad, "t", 0) catch err {
        println(err)
    }
    var snappy = bytes.Clone(b)
    snappy[22] = 2
    // fix the CRC so that only the codec is wrong
    var crc = std.data.rtCrc32c(0, snappy[21..])
    snappy[17] = byte((crc >> 24) & 255)
    snappy[18] = byte((crc >> 16) & 255)
    snappy[19] = byte((crc >> 8) & 255)
    snappy[20] = byte(crc & 255)
    try kafka.DecodeBatches(snappy, "t", 0) catch err {
        println(err)
    }
    try kafka.EncodeBatch(recs, 0, 3, 0, kafka.BatchProducer{
        ID: -1,
        Epoch: -1,
        BaseSequence: -1,
    }) catch err {
        println(err)
    }
    try kafka.EncodeBatch([], 0, 0, 0, kafka.BatchProducer{
        ID: -1,
        Epoch: -1,
        BaseSequence: -1,
    }) catch err {
        println(err)
    }

    // partitioning like the Java client: toPositive(murmur2(key)) % n
    var parts []int = []
    for i in 0..10 {
        parts = append(parts, kafka.HashPartition(strings.Bytes(fmt.Format("key{0}", i)), 3))
    }
    println("partitions of key0..key9 over 3:", parts)
    println(
        "murmur2:",
        kafka.Murmur2(strings.Bytes("21")),
        kafka.Murmur2(strings.Bytes("foobar")),
        kafka.Murmur2([]),
    )

    // assignors
    var members = map[string][]string{"c1": ["a", "b"], "c2": ["a", "b"], "c3": ["a"]}
    var counts = map[string]int{"a": 4, "b": 3}
    for name in ["range", "roundrobin"] {
        var plan map[string][]kafka.TopicPartition
        if name == "range" {
            plan = kafka.AssignRange(members, counts)
        } else {
            plan = kafka.AssignRoundRobin(members, counts)
        }
        for m in maps.SortedKeys(plan) {
            var tps []string = []
            for tp in plan[m] {
                tps = append(tps, tp.Topic + strconv.Itoa(tp.Partition))
            }
            println(name, m, tps)
        }
    }

    // primitives
    var e = kafka.Encoder{}
    e.Int16(-2)
    e.Int32(-1)
    e.Int64(-3)
    e.String("hé")
    e.NullableString("", true)
    e.Varint(-1)
    e.Varint(300)
    e.Bytes(strings.Bytes("xyz"))
    println(hex(e.B))
    var d = kafka.Decoder{B: e.B}
    println(
        d.Int16(),
        d.Int32(),
        d.Int64(),
        d.String(),
        strconv.Quote(d.String()),
        d.Varint(),
        d.Varint(),
        strings.FromBytes(d.Bytes()),
        d.Err,
    )
    _ = d.Int32()
    println("past the end:", d.Err)

    println(
        kafka.Error{Code: 6}.Message(),
        kafka.Error{Code: 6}.Retriable(),
        kafka.Error{Code: 36, Text: "topic \"x\""}.Message(),
        kafka.Error{Code: 36}.Retriable(),
    )
    println(
        kafka.ErrorCode(errors.Wrap(kafka.Error{Code: 27}, "join")),
        kafka.ErrorCode(error("x")),
    )
}
