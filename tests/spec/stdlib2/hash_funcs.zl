// test: std.hash interfaces with fnv, crc32, crc64, adler32 and seeded maphash (SipHash-1-3)
// status: passing
// run-flags: --leak-check
namespace spec.stdlib2.hash_funcs

import std
import std.hash
import std.io

func sum64(h hash.Hash64, s string) uint {
    h.Reset()
    _ = h.WriteString(s)
    return h.Sum64()
}

func hex(h hash.Hasher) string {
    return encoding.hex.Encode(h.Sum())
}

func main() {
    var a = fnv.New32()
    var b = fnv.New32a()
    var c = fnv.New64()
    var d = fnv.New64a()
    _ = a.WriteString("hello")
    _ = b.WriteString("hello")
    _ = c.Write(strings.Bytes("hello"))
    _ = d.WriteString("hello")
    println(a.Sum32(), b.Sum32(), c.Sum64(), d.Sum64(), fnv.String64a("hello"))
    println(hex(a), hex(d), a.Size(), d.Size(), d.BlockSize())
    println(
        sum64(fnv.New64a(), ""),
        sum64(crc64.New(crc64.MakeTable(crc64.ECMA)), "123456789"),
        sum64(crc64.New(crc64.MakeTable(crc64.ISO)), "123456789"),
    )
    var data = strings.Bytes("The quick brown fox jumps over the lazy dog")
    println(
        crc32.ChecksumIEEE(data),
        crypto.hash.Crc32("The quick brown fox jumps over the lazy dog"),
        crc32.Checksum(data, crc32.MakeTable(crc32.Castagnoli)),
    )
    var cd = crc32.NewIEEE()
    _ = cd.Write(data[..10])
    _ = cd.Write(data[10..])
    println(cd.Sum32(), hex(cd))
    println(adler32.Checksum(strings.Bytes("Wikipedia")), adler32.Checksum(strings.Bytes("")))
    var big []byte = []
    for i in 0..20000 {
        big = append(big, byte(i % 251))
    }
    var ad = adler32.New()
    _ = ad.Write(big)
    println(ad.Sum32())
    var seed = maphash.SeedFrom([byte(0), 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15])
    var input []byte = []
    for i in 0..15 {
        input = append(input, byte(i))
    }
    println(
        maphash.Bytes(seed, input),
        maphash.String(seed, "abc") == maphash.Bytes(seed, strings.Bytes("abc")),
    )
    var mh = maphash.Hash{}
    mh.SetSeed(seed)
    _ = mh.WriteString("ab")
    _ = mh.WriteString("c")
    println(
        mh.Sum64() == maphash.String(seed, "abc"),
        sum64(mh, "abc") == maphash.String(seed, "abc"),
    )
    var r1 = maphash.MakeSeed()
    var r2 = maphash.MakeSeed()
    println(maphash.String(r1, "x") != maphash.String(r2, "x"))
    println(
        hash.Combine(1, 2),
        hash.Combine(hash.Combine(0, 1), 2) != hash.Combine(hash.Combine(0, 2), 1),
    )
}
