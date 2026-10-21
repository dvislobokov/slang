namespace std.cloud.blob

// std.cloud.blob: one interface for object storage (docs/STDLIB.md level
// 3b), implemented over std.cloud.s3 (OpenS3) and over a local directory
// (OpenDir), so code and tests can switch between a bucket and the file
// system. Keys are "/"-separated; a missing key fails with
// blob.ErrNotFound (errors.Is) in both implementations.

// Metadata of a blob.
struct Attributes {
    Key string
    Size int
    ModTime std.time.Time
    ETag string
    ContentType string
}

// A bucket of blobs.
interface Bucket {
    func Write(key string, data []byte, contentType string) Task<void!>
    func Read(key string) Task<[]byte!>
    func Delete(key string) Task<void!>
    func Exists(key string) Task<bool!>
    func Attributes(key string) Task<std.cloud.blob.Attributes!>
    // Every blob whose key starts with prefix, sorted by key.
    func List(prefix string) Task<[]std.cloud.blob.Attributes!>
    func Copy(dst string, src string) Task<void!>
    // A URL that grants method on key for expires seconds (S3 only).
    func SignedURL(key string, method string, expires int) string!
}

func notFound(key string) error {
    return std.errors.Wrap(ErrNotFound, "blob: " + key)
}

// ---- S3 -----------------------------------------------------------------------

// A Bucket over an S3 bucket; keys live under prefix ("" or "dir/").
struct S3Bucket {
    c *std.cloud.s3.Client
    bucket string
    prefix string

    func Write(key string, data []byte, contentType string) Task<void!> {
        return s3Write(this, key, data, contentType)
    }

    func Read(key string) Task<[]byte!> {
        return s3Read(this, key)
    }

    func Delete(key string) Task<void!> {
        return this.c.DeleteObject(this.bucket, this.prefix + key)
    }

    func Exists(key string) Task<bool!> {
        return s3Exists(this, key)
    }

    func Attributes(key string) Task<Attributes!> {
        return s3Attrs(this, key)
    }

    func List(prefix string) Task<[]Attributes!> {
        return s3List(this, prefix)
    }

    func Copy(dst string, src string) Task<void!> {
        return s3Copy(this, dst, src)
    }

    func SignedURL(key string, method string, expires int) string! {
        return this.c.Presign(method, this.bucket, this.prefix + key, expires)?
    }
}

func OpenS3(c *std.cloud.s3.Client, bucket string, prefix string) *S3Bucket {
    return new S3Bucket{c: c, bucket: bucket, prefix: prefix}
}

func mapS3(err error, key string) error {
    if std.errors.Is(err, std.cloud.s3.ErrNoSuchKey) || std.cloud.s3.ErrorCode(err) == "NotFound" {
        return notFound(key)
    }
    return err
}

async func s3Write(b *S3Bucket, key string, data []byte, contentType string) void! {
    _ = await b.c.PutObject(b.bucket, b.prefix + key, data, std.cloud.s3.PutOptions{ContentType: contentType})?
}

async func s3Read(b *S3Bucket, key string) []byte! {
    var d = try await b.c.GetObjectBytes(b.bucket, b.prefix + key) catch err {
        return mapS3(err, key)
    }
    return d
}

async func s3Exists(b *S3Bucket, key string) bool! {
    try await b.c.HeadObject(b.bucket, b.prefix + key) catch err {
        if std.errors.Is(mapS3(err, key), ErrNotFound) {
            return false
        }
        return err
    }
    return true
}

func attrsOf(o std.cloud.s3.ObjectInfo, key string) Attributes {
    return Attributes{Key: key, Size: o.Size, ModTime: o.LastModified, ETag: o.ETag, ContentType: o.ContentType}
}

async func s3Attrs(b *S3Bucket, key string) Attributes! {
    var o = try await b.c.HeadObject(b.bucket, b.prefix + key) catch err {
        return mapS3(err, key)
    }
    return attrsOf(o, key)
}

async func s3List(b *S3Bucket, prefix string) []Attributes! {
    var out []Attributes = []
    var it = b.c.List(b.bucket, b.prefix + prefix, "")
    for o in it {
        out = append(out, attrsOf(o, o.Key[len(b.prefix)..]))
    }
    if it.Err() != nil {
        return it.Err()
    }
    return out
}

async func s3Copy(b *S3Bucket, dst string, src string) void! {
    try await b.c.CopyObject(b.bucket, b.prefix + src, b.bucket, b.prefix + dst, std.cloud.s3.PutOptions{}) catch err {
        return mapS3(err, src)
    }
}

// ---- local directory -----------------------------------------------------------------

// A Bucket over a directory: key "a/b.txt" is the file dir/a/b.txt.
// Keys with "..", "." or empty segments, or a leading "/", are refused.
struct DirBucket {
    dir string

    func path(key string) string! {
        if key == "" || std.strings.HasPrefix(key, "/") || std.strings.Contains(key, "\\") {
            return error("blob: invalid key " + std.strconv.Quote(key))
        }
        for seg in std.strings.Split(key, "/") {
            if seg == "" || seg == "." || seg == ".." {
                return error("blob: invalid key " + std.strconv.Quote(key))
            }
        }
        return this.dir + "/" + key
    }

    func Write(key string, data []byte, contentType string) Task<void!> {
        return dirWrite(this, key, data)
    }

    func Read(key string) Task<[]byte!> {
        return dirRead(this, key)
    }

    func Delete(key string) Task<void!> {
        return dirDelete(this, key)
    }

    func Exists(key string) Task<bool!> {
        return dirExists(this, key)
    }

    func Attributes(key string) Task<Attributes!> {
        return dirAttrs(this, key)
    }

    func List(prefix string) Task<[]Attributes!> {
        return dirList(this, prefix)
    }

    func Copy(dst string, src string) Task<void!> {
        return dirCopy(this, dst, src)
    }

    func SignedURL(key string, method string, expires int) string! {
        return error("blob: signed URLs are not supported by a directory bucket")
    }
}

// A Bucket over dir (created if missing).
func OpenDir(dir string) *DirBucket! {
    std.fs.MkdirAll(dir)?("blob: " + dir)
    var abs = std.fs.Abs(dir)?
    return new DirBucket{dir: abs}
}

async func dirWrite(b *DirBucket, key string, data []byte) void! {
    var p = b.path(key)?
    std.fs.MkdirAll(std.fs.Dir(p))?
    // write a temporary file, then rename: readers never see a partial blob
    var tmp = p + ".tmp-" + std.strconv.Itoa(std.random.IntN(1000000000))
    std.io.WriteFile(tmp, std.strings.FromBytes(data))?
    try std.fs.Rename(tmp, p) catch err {
        try std.fs.Remove(tmp) catch _ {
        }
        return err
    }
}

async func dirRead(b *DirBucket, key string) []byte! {
    var p = b.path(key)?
    if !std.fs.Exists(p) || std.fs.IsDir(p) {
        return notFound(key)
    }
    return std.strings.Bytes(std.io.ReadFile(p)?)
}

async func dirDelete(b *DirBucket, key string) void! {
    var p = b.path(key)?
    if !std.fs.Exists(p) || std.fs.IsDir(p) {
        return
    }
    std.fs.Remove(p)?
    // drop directories the key left empty
    var d = std.fs.Dir(p)
    for d != b.dir && std.strings.HasPrefix(d, b.dir) {
        var names = try std.fs.ReadDir(d) else ["x"]
        if len(names) > 0 {
            break
        }
        try std.fs.Remove(d) catch _ {
            break
        }
        d = std.fs.Dir(d)
    }
}

async func dirExists(b *DirBucket, key string) bool! {
    var p = b.path(key)?
    return std.fs.Exists(p) && !std.fs.IsDir(p)
}

func fileAttrs(path string, key string) Attributes! {
    var size = std.fs.Size(path)?
    var mt = std.fs.ModTime(path)?
    var ct = std.mime.TypeByExtension(std.fs.Ext(path))
    return Attributes{Key: key, Size: size, ModTime: std.time.UnixMilli(mt), ContentType: ct}
}

async func dirAttrs(b *DirBucket, key string) Attributes! {
    var p = b.path(key)?
    if !std.fs.Exists(p) || std.fs.IsDir(p) {
        return notFound(key)
    }
    return fileAttrs(p, key)?
}

func walk(dir string, rel string, out []Attributes) []Attributes! {
    var acc = out
    for name in std.fs.ReadDir(dir)? {
        if std.strings.Contains(name, ".tmp-") {
            continue
        }
        var full = dir + "/" + name
        var key = name
        if rel != "" {
            key = rel + "/" + name
        }
        if std.fs.IsDir(full) {
            acc = walk(full, key, acc)?
        } else {
            acc = append(acc, fileAttrs(full, key)?)
        }
    }
    return acc
}

async func dirList(b *DirBucket, prefix string) []Attributes! {
    var all = walk(b.dir, "", [])?
    var out []Attributes = []
    for a in all {
        if std.strings.HasPrefix(a.Key, prefix) {
            out = append(out, a)
        }
    }
    std.slices.SortFunc(out, func(x Attributes, y Attributes) bool {
        return x.Key < y.Key
    })
    return out
}

async func dirCopy(b *DirBucket, dst string, src string) void! {
    var data = await dirRead(b, src)?
    await dirWrite(b, dst, data)?
}

// ---- URLs ------------------------------------------------------------------------------

// Opens a bucket by URL: "file:///path/to/dir", or
// "s3://bucket/prefix/?endpoint=http://localhost:9000&region=us-east-1&path_style=true"
// (credentials from the environment or ~/.aws/credentials).
func Open(raw string) Bucket! {
    if std.strings.HasPrefix(raw, "file://") {
        return OpenDir(raw[7..])?
    }
    if !std.strings.HasPrefix(raw, "s3://") {
        return error("blob: unsupported URL " + std.strconv.Quote(raw) + " (want file:// or s3://)")
    }
    var u = std.net.url.Parse(raw)?("blob: invalid URL")
    var q = std.net.url.ParseQuery(u.RawQuery)?("blob: invalid URL query")
    var cfg = std.cloud.s3.Config{Endpoint: q.Get("endpoint"), Region: q.Get("region"), PathStyle: q.Get("path_style") == "true"}
    var c = std.cloud.s3.NewClient(cfg)?
    var prefix = u.Path
    if std.strings.HasPrefix(prefix, "/") {
        prefix = prefix[1..]
    }
    return OpenS3(c, u.Host, prefix)
}
