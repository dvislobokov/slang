// test: std.compress.brotli Decompress on streams made by the brotli CLI: empty, uncompressed meta-block (quality 0), static dictionary words with identity/uppercase transforms (quality 11), back references, a generated 5000-byte binary input; corrupt and truncated streams are errors
// status: passing
// run-flags: --leak-check
namespace spec.stdlib4.brotli_decode

import std
import std.io

func main() void! {
    var vectors = [
        "3f",
        "0f018061626303",
        "1f5c00e08d54b5bf36437ba35320652d8c219d681336e088b3f0e151177e208715946d07b71f6d",
        "1f3200f825c2c28c002001",
        "1f3300f8a58c9eba126082114673528b4253f96ad01b4f1c0b",
        "1f5a00382c92c7b9afc528b4838f50e90942859664666c7986a757616f0de232971736e0c02181bc41fc28d22c66b47965f410515933c7fff62900",
    ]
    for v in vectors {
        var out = compress.brotli.DecompressString(encoding.hex.Decode(v)?)?
        println(len(out), "[" + out + "]")
    }
    var big = compress.brotli.Decompress(
        encoding.hex.Decode(
            "d1389c0060a4466a9adb0dc83c9f6fd1d6d6acadad595bb3b6898a18d8362ac7bdc27e282d2dadb4b4b458d1a9340251df6378402e881739647b690ac577a2a201702c399e2f551b6c69e939c5558d1db7b1d995eda3cbc78f7fc50a3ecd74c014388648e78814c916973f23bfacaeb567786a7163ffecf6e5fbf1f492b25f013c2a09328dd1eec9f8dceacef155bef389d090a4b8039905e53938bf7bfd11430011380a33b925d54d9d5c3e05d50eb3c432b862a52e6dbd23d34b9b79a5d5b08149d49a1cde70e6d7764fae9ff2bb50068a4e6a30abb0a23e17f76fbf921110124f6509525ad3dcc54fe4ae3951de268e2751e9ade91b9d59de3accbb1e77884c92d9e98be4657deff4e6f92b7f8c8561480965175536b4e7e1fd4f36fda00834b6509eda96eea1c985e86e5c74cb0a3d73e6cc99f3ec82100fa0a84587cf180d16f6c1c9e10322ad78c89863c6cd39404a1060d04b8a96352e82dc83a4060d1ea3d498d903239b7708b4e02265960917670028e61f3a3d246858e524c01d08aa51e1324285895d50b278034f330e12661871720a4b117fd0e82646cd0a077e6e8153850287614a8cec8090c92b389ab01133cd808313180af9854a17112a96d9f171038c4a64d80c5160601b980c5ec0d2888588297aec1c4353c00f143a0951b2c486976ba05420c1822747cf1610119ec1d0808990493a6c1c4191cf37643a0850b0c88a872b209423c2842343c72620619e40538f8180095aac1c4292c71724daf191b3f00fba5c18",
        )?,
    )?
    var ok = len(big) == 5000
    for i, b in big {
        if int(b) != (i * 7 + i / 13) % 251 {
            ok = false
        }
    }
    println("generated", len(big), ok)
    for bad in ["1f5c00e08d54b5bf36", "1f3300f8a5", "8b"] {
        try compress.brotli.Decompress(encoding.hex.Decode(bad)?) catch err {
            println("error:", strings.HasPrefix(err.Message(), "brotli: "))
            continue
        }
        println("decoded a corrupt stream")
    }
}
