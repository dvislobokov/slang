namespace std.compress.brotli

// Brotli (RFC 7932) decoding; the decoder is C (runtime/src/std_brotli.c)
// with the RFC's static dictionary and transforms. There is no encoder.
// Large-window streams and shared dictionaries are not supported.

// Decompresses a whole brotli stream.
func Decompress(data []byte) []byte! {
    return std.compress.rtBrotliDecode(data)?
}

// Decompress to a string.
func DecompressString(data []byte) string! {
    return std.strings.FromBytes(std.compress.rtBrotliDecode(data)?)
}
