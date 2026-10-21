// test: std.cloud.s3 against an in-process S3-compatible server that verifies every Signature V4 (headers and presigned queries, with the same SignV4 code) and payload hashes: buckets, Put/Get (ranges, streaming)/Head/Delete, metadata and content types, keys needing escapes, ListObjectsV2 pagination with prefixes and delimiters (iteration), CopyObject, DeleteObjects, multipart Upload with parallel parts and abort on failure, presigned GET/PUT, typed errors (NoSuchKey, NoSuchBucket, SignatureDoesNotMatch), a 503 SlowDown retried; std.cloud.blob over the same server and over a directory
// status: passing
// run-flags: --leak-check
namespace spec.stdlib5.s3_mock

import std
import std.cloud
import std.io

const AK = "AKIATESTKEY"
const SK = "test/secret+key"

struct object {
    data  string
    ctype string
    meta  map[string]string
    etag  string
}

struct upload {
    bucket string
    key    string
    parts  map[int]string
}

struct store {
    buckets    map[string]map[string]object
    uploads    map[string]upload
    nextUpload int
    // answer this many requests with 503 SlowDown
    slowDown int
    // fail UploadPart of this part number
    failPart         int
    log              []string
    maxParallelParts int
    partsInFlight    int
}

func xmlError(res *net.http.Response, status int, code string, msg string) {
    res.Text(
        status,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Error><Code>" +
            code +
            "</Code><Message>" +
            msg +
            "</Message><RequestId>req-1</RequestId></Error>",
    )
    res.SetHeader("Content-Type", "application/xml")
}

func unescape(s string) string {
    return try net.url.PathUnescape(s) else s
}

// Checks the request's signature; "" if it is valid, else the reason.
func verify(req *net.http.Request) string {
    var q = req.Query
    if strings.Contains(q, "X-Amz-Signature=") {
        var params = strings.Split(q, "&")
        var keep []string = []
        var sig = ""
        var date = ""
        for p in params {
            if strings.HasPrefix(p, "X-Amz-Signature=") {
                sig = p[16..]
            } else {
                keep = append(keep, p)
                if strings.HasPrefix(p, "X-Amz-Date=") {
                    date = p[11..]
                }
            }
        }
        var s = s3.SignV4(
            req.Method,
            req.Path,
            strings.Join(keep, "&"),
            map[string]string{"host": req.Host},
            "UNSIGNED-PAYLOAD",
            AK,
            SK,
            "us-east-1",
            "s3",
            date,
        )
        if s.Signature != sig {
            return "presigned signature mismatch"
        }
        return ""
    }
    var auth = req.Header("Authorization")
    if !strings.HasPrefix(auth, "AWS4-HMAC-SHA256 Credential=" + AK + "/") {
        return "bad credential"
    }
    var shs = auth[strings.Index(auth, "SignedHeaders=") + 14..]
    shs = shs[..strings.Index(shs, ",")]
    var sig = auth[strings.Index(auth, "Signature=") + 10..]
    var h = map[string]string{}
    for name in strings.Split(shs, ";") {
        if name == "host" {
            h[name] = req.Host
        } else {
            h[name] = req.Header(name)
        }
    }
    var payload = req.Header("X-Amz-Content-Sha256")
    if payload != "UNSIGNED-PAYLOAD" && payload != crypto.hash.Sha256Hex(req.Body) {
        return "payload hash mismatch"
    }
    var s = s3.SignV4(
        req.Method,
        req.Path,
        req.Query,
        h,
        payload,
        AK,
        SK,
        "us-east-1",
        "s3",
        req.Header("X-Amz-Date"),
    )
    if s.Signature != sig {
        return "signature mismatch"
    }
    return ""
}

func param(query string, name string) (string, bool) {
    for p in strings.Split(query, "&") {
        if p == name {
            return ("", true)
        }
        if strings.HasPrefix(p, name + "=") {
            return (unescape(strings.Replace(p[len(name) + 1..], "+", "%2B")), true)
        }
    }
    return ("", false)
}

func listXML(st *store, bucket string, query string) string {
    var (prefix, _) = param(query, "prefix")
    var (delim, _) = param(query, "delimiter")
    var (token, _) = param(query, "continuation-token")
    var (mk, _) = param(query, "max-keys")
    var max = try strconv.Atoi(mk) else 1000
    var keys = maps.SortedKeys(st.buckets[bucket])
    var contents = ""
    var prefixes []string = []
    var n = 0
    var truncated = false
    var next = ""
    for k in keys {
        if !strings.HasPrefix(k, prefix) || (token != "" && k <= token) {
            continue
        }
        if n >= max {
            truncated = true
            break
        }
        if delim != "" {
            var rest = k[len(prefix)..]
            var d = strings.Index(rest, delim)
            if d >= 0 {
                var cp = prefix + rest[..d + len(delim)]
                if !slices.Contains(prefixes, cp) {
                    prefixes = append(prefixes, cp)
                    n += 1
                }
                next = k
                continue
            }
        }
        var o = st.buckets[bucket][k]
        contents += "<Contents><Key>" +
            encoding.xml.Escape(k) +
            "</Key><LastModified>2026-01-02T03:04:05.000Z</LastModified><ETag>" +
            o.etag +
            "</ETag><Size>" +
            strconv.Itoa(len(o.data)) +
            "</Size><StorageClass>STANDARD</StorageClass></Contents>"
        n += 1
        next = k
    }
    var cps = ""
    for p in prefixes {
        cps += "<CommonPrefixes><Prefix>" + p + "</Prefix></CommonPrefixes>"
    }
    var tail = "<IsTruncated>false</IsTruncated>"
    if truncated {
        tail = "<IsTruncated>true</IsTruncated><NextContinuationToken>" +
            encoding.xml.Escape(next) +
            "</NextContinuationToken>"
    }
    return "<ListBucketResult xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\"><Name>" +
        bucket +
        "</Name><KeyCount>" +
        strconv.Itoa(n) +
        "</KeyCount>" +
        tail +
        contents +
        cps +
        "</ListBucketResult>"
}

async func serve(st *store, req *net.http.Request, res *net.http.Response) void! {
    var why = verify(req)
    if why != "" {
        st.log = append(st.log, "rejected " + req.Method + " " + req.Path + ": " + why)
        xmlError(res, 403, "SignatureDoesNotMatch", why)
        return
    }
    if st.slowDown > 0 {
        st.slowDown -= 1
        xmlError(res, 503, "SlowDown", "Please reduce your request rate.")
        return
    }
    var parts = strings.SplitN(req.Path[1..], "/", 2)
    var bucket = unescape(parts[0])
    var key = ""
    if len(parts) == 2 {
        key = unescape(parts[1])
    }
    var q = req.Query
    if bucket == "" {
        var out = "<ListAllMyBucketsResult><Buckets>"
        for b in maps.SortedKeys(st.buckets) {
            out += "<Bucket><Name>" +
                b +
                "</Name><CreationDate>2026-01-02T03:04:05.000Z</CreationDate></Bucket>"
        }
        res.Text(200, out + "</Buckets></ListAllMyBucketsResult>")
        return
    }
    var objs = st.buckets[bucket]
    if key == "" {
        switch req.Method {
            case "PUT":
                st.buckets[bucket] = map[string]object{}
                res.Text(200, "")
            case "HEAD":
                if objs == nil {
                    res.Text(404, "")
                } else {
                    res.Text(200, "")
                }
            case "DELETE":
                if objs == nil {
                    xmlError(res, 404, "NoSuchBucket", "The specified bucket does not exist")
                } else if len(objs) > 0 {
                    xmlError(
                        res,
                        409,
                        "BucketNotEmpty",
                        "The bucket you tried to delete is not empty",
                    )
                } else {
                    delete(st.buckets, bucket)
                    res.Text(204, "")
                }
            case "GET":
                if objs == nil {
                    xmlError(res, 404, "NoSuchBucket", "The specified bucket does not exist")
                } else {
                    res.Text(200, listXML(st, bucket, q))
                }
            case "POST":
                if req.Header("Content-Md5") != encoding.base64.Encode(crypto.hash.Md5(req.Body)) {
                    xmlError(res, 400, "InvalidDigest", "bad Content-MD5")
                    return
                }
                var root = encoding.xml.ParseValue(req.Body)?
                var errs = ""
                for o in root.Elements("Object") {
                    var k = o.Child("Key").InnerText()
                    if k == "locked" {
                        errs += "<Error><Key>locked</Key><Code>AccessDenied</Code><Message>Access Denied</Message></Error>"
                    } else {
                        delete(objs, k)
                    }
                }
                res.Text(200, "<DeleteResult>" + errs + "</DeleteResult>")
        }
        return
    }
    if objs == nil {
        xmlError(res, 404, "NoSuchBucket", "The specified bucket does not exist")
        return
    }
    var (uploadID, multi) = param(q, "uploadId")
    var (_, initiate) = param(q, "uploads")
    if req.Method == "POST" && initiate {
        st.nextUpload += 1
        var id = "upload-" + strconv.Itoa(st.nextUpload)
        st.uploads[id] = upload{bucket: bucket, key: key, parts: map[int]string{}}
        res.Text(
            200,
            "<InitiateMultipartUploadResult><Bucket>" +
                bucket +
                "</Bucket><Key>" +
                encoding.xml.Escape(key) +
                "</Key><UploadId>" +
                id +
                "</UploadId></InitiateMultipartUploadResult>",
        )
        return
    }
    if multi {
        var up = st.uploads[uploadID]
        if req.Method == "PUT" {
            var (pn, _) = param(q, "partNumber")
            var n = strconv.Atoi(pn)?
            st.partsInFlight += 1
            st.maxParallelParts = math.IMax(st.maxParallelParts, st.partsInFlight)
            // hold the part until another one is in flight (or 2 s): a
            // client that uploads parts one by one shows max parallel 1
            var waited = 0
            for st.maxParallelParts < 2 && waited < 2000 {
                await task.Delay(10)
                waited += 10
            }
            st.partsInFlight -= 1
            if n == st.failPart {
                xmlError(res, 400, "InvalidPart", "injected failure")
                return
            }
            up.parts[n] = req.Body
            st.uploads[uploadID] = up
            res.SetHeader("ETag", "\"" + crypto.hash.Md5Hex(req.Body) + "\"")
            res.Text(200, "")
            return
        }
        if req.Method == "DELETE" {
            delete(st.uploads, uploadID)
            st.log = append(st.log, "aborted " + uploadID)
            res.Text(204, "")
            return
        }
        // complete: the parts in order
        var root = encoding.xml.ParseValue(req.Body)?
        var data = ""
        var count = 0
        for p in root.Elements("Part") {
            var n = strconv.Atoi(p.Child("PartNumber").InnerText())?
            if p.Child("ETag").InnerText() != "\"" + crypto.hash.Md5Hex(up.parts[n]) + "\"" {
                xmlError(res, 400, "InvalidPart", "etag mismatch")
                return
            }
            data += up.parts[n]
            count += 1
        }
        var etag = "\"" + crypto.hash.Md5Hex(data) + "-" + strconv.Itoa(count) + "\""
        objs[key] = object{
            data: data,
            ctype: "application/octet-stream",
            meta: map[string]string{},
            etag: etag,
        }
        delete(st.uploads, uploadID)
        res.Text(
            200,
            "<CompleteMultipartUploadResult><Key>" +
                encoding.xml.Escape(key) +
                "</Key><ETag>" +
                etag +
                "</ETag></CompleteMultipartUploadResult>",
        )
        return
    }
    switch req.Method {
        case "PUT":
            var src = req.Header("X-Amz-Copy-Source")
            if src != "" {
                var sp = strings.SplitN(src[1..], "/", 2)
                var so = st.buckets[unescape(sp[0])][unescape(sp[1])]
                if so.etag == "" {
                    xmlError(res, 404, "NoSuchKey", "The specified key does not exist.")
                    return
                }
                objs[key] = so
                res.Text(
                    200,
                    "<CopyObjectResult><LastModified>2026-01-02T03:04:05.000Z</LastModified><ETag>" +
                        so.etag +
                        "</ETag></CopyObjectResult>",
                )
                return
            }
            if req.Header("Content-Md5") != encoding.base64.Encode(crypto.hash.Md5(req.Body)) {
                xmlError(
                    res,
                    400,
                    "BadDigest",
                    "The Content-MD5 you specified did not match what we received.",
                )
                return
            }
            if req.Header("If-None-Match") == "*" && objs[key].etag != "" {
                xmlError(
                    res,
                    412,
                    "PreconditionFailed",
                    "At least one of the pre-conditions you specified did not hold",
                )
                return
            }
            var meta = map[string]string{}
            for k, v in req.Headers {
                if strings.HasPrefix(strings.ToLower(k), "x-amz-meta-") {
                    meta[strings.ToLower(k)[11..]] = v
                }
            }
            var ct = req.Header("Content-Type")
            if ct == "" {
                ct = "binary/octet-stream"
            }
            var etag = "\"" + crypto.hash.Md5Hex(req.Body) + "\""
            objs[key] = object{data: req.Body, ctype: ct, meta: meta, etag: etag}
            res.SetHeader("ETag", etag)
            res.Text(200, "")
        case "GET", "HEAD":
            var o = objs[key]
            if o.etag == "" {
                if req.Method == "HEAD" {
                    res.Text(404, "")
                } else {
                    xmlError(res, 404, "NoSuchKey", "The specified key does not exist.")
                }
                return
            }
            res.SetHeader("ETag", o.etag)
            res.SetHeader("Content-Type", o.ctype)
            res.SetHeader("Last-Modified", "Fri, 02 Jan 2026 03:04:05 GMT")
            for k, v in o.meta {
                res.SetHeader("X-Amz-Meta-" + k, v)
            }
            var rng = req.Header("Range")
            if rng != "" {
                var spec = rng[6..]
                var dash = strings.Index(spec, "-")
                var from = strconv.Atoi(spec[..dash])?
                var to = len(o.data) - 1
                if dash + 1 < len(spec) {
                    to = math.IMin(strconv.Atoi(spec[dash + 1..])?, to)
                }
                res.SetHeader(
                    "Content-Range",
                    "bytes " +
                        strconv.Itoa(from) +
                        "-" +
                        strconv.Itoa(to) +
                        "/" +
                        strconv.Itoa(len(o.data)),
                )
                res.Write(o.data[from..to + 1])
                res.Status = 206
                return
            }
            // (for HEAD the server sends the length of the body it drops)
            res.Write(o.data)
            res.Status = 200
        case "DELETE":
            delete(objs, key)
            res.Text(204, "")
    }
}

struct handler {
    st *store

    func Serve(req *net.http.Request, res *net.http.Response) Task<void!> {
        return serve(this.st, req, res)
    }
}

func logName(i int) string {
    if i < 10 {
        return "app/0" + strconv.Itoa(i) + ".log"
    }
    return "app/" + strconv.Itoa(i) + ".log"
}

async func blobChecks(name string, b cloud.blob.Bucket) void! {
    await b.Write("notes/a.txt", strings.Bytes("alpha"), "text/plain")?
    await b.Write("notes/sub/b.txt", strings.Bytes("beta"), "text/plain")?
    await b.Write("other.bin", strings.Bytes("x"), "")?
    println(
        name,
        "read",
        strings.FromBytes(await b.Read("notes/a.txt")?),
        await b.Exists("notes/a.txt")?,
        await b.Exists("nope")?,
    )
    var keys []string = []
    for a in await b.List("notes/")? {
        keys = append(keys, a.Key + "=" + strconv.Itoa(a.Size))
    }
    println(name, "list", keys)
    await b.Copy("notes/c.txt", "notes/a.txt")?
    var at = await b.Attributes("notes/c.txt")?
    println(name, "copy", at.Key, at.Size)
    try await b.Read("missing.txt") catch err {
        println(name, "missing:", errors.Is(err, cloud.blob.ErrNotFound))
    }
    for k in ["notes/a.txt", "notes/sub/b.txt", "notes/c.txt", "other.bin"] {
        await b.Delete(k)?
    }
    println(name, "after delete", len(await b.List("")?))
}

async func main() void! {
    var st = new store{buckets: map[string]map[string]object{}, uploads: map[string]upload{}}
    var srv = net.http.test.NewServer(handler{st: st})?
    var c = s3.NewClient(s3.Config{
        Endpoint: srv.URL,
        AccessKey: AK,
        SecretKey: SK,
        PathStyle: true,
    })?

    // buckets
    println("exists", await c.BucketExists("photos")?)
    await c.CreateBucket("photos")?
    await c.CreateBucket("logs")?
    var names []string = []
    for b in await c.ListBuckets()? {
        names = append(names, b.Name + "@" + strconv.Itoa(b.CreationDate.Year()))
    }
    println("buckets", names, await c.BucketExists("photos")?)

    // objects
    var pr = await c.PutObject(
        "photos",
        "2026/summer trip/ü+1.jpg",
        strings.Bytes("JPEGDATA"),
        s3.PutOptions{ContentType: "image/jpeg", Metadata: map[string]string{"Camera": "x100"}},
    )?
    println("put", pr.ETag)
    var info = await c.HeadObject("photos", "2026/summer trip/ü+1.jpg")?
    println(
        "head",
        info.Size,
        info.ContentType,
        info.Metadata["camera"],
        info.ETag == pr.ETag,
        info.LastModified.Year(),
    )
    println("get", strings.FromBytes(await c.GetObjectBytes("photos", "2026/summer trip/ü+1.jpg")?))
    var part = await c.GetObject("photos", "2026/summer trip/ü+1.jpg", s3.GetOptions{
        HasRange: true,
        RangeStart: 2,
        RangeEnd: 4,
    })?
    println("range", strings.FromBytes(await part.ReadAll()?), part.Info.Size)
    var big = strings.Repeat("0123456789", 20000)
    _ = await c.PutObject("photos", "big.txt", big, s3.PutOptions{})?
    var stream = await c.GetObject("photos", "big.txt", s3.GetOptions{})?
    var total = 0
    for {
        var chunk = await stream.Read(65536)?
        if len(chunk) == 0 {
            break
        }
        total += len(chunk)
    }
    stream.Close()
    println("streamed", total)
    try await c.GetObjectBytes("photos", "nope.jpg") catch err {
        println("get missing:", errors.Is(err, s3.ErrNoSuchKey), s3.ErrorCode(err))
        var se s3.Error
        println(errors.As(err, &se), se.Status, se.RequestID, se.Resource)
    }
    try await c.HeadObject("photos", "nope.jpg") catch err {
        println("head missing:", errors.Is(err, s3.ErrNoSuchKey))
    }
    try await c.PutObject("nobucket", "k", "v", s3.PutOptions{}) catch err {
        println("no bucket:", errors.Is(err, s3.ErrNoSuchBucket))
    }
    try await c.PutObject("photos", "big.txt", "again", s3.PutOptions{
        IfNoneMatch: true,
    }) catch err {
        println("if-none-match:", errors.Is(err, s3.ErrPreconditionFailed))
    }
    var bad = s3.NewClient(s3.Config{
        Endpoint: srv.URL,
        AccessKey: AK,
        SecretKey: "wrong",
        PathStyle: true,
    })?
    try await bad.ListBuckets() catch err {
        println("wrong secret:", s3.ErrorCode(err), err)
    }

    // listing: 25 keys, pages of 10, a delimiter
    for i in 0..25 {
        _ = await c.PutObject("logs", logName(i), "line", s3.PutOptions{ContentType: "text/plain"})?
    }
    _ = await c.PutObject("logs", "db/1.log", "x", s3.PutOptions{})?
    _ = await c.PutObject("logs", "readme", "x", s3.PutOptions{})?
    var page = await c.ListObjectsV2("logs", s3.ListOptions{Prefix: "app/", MaxKeys: 10})?
    println(
        "page",
        len(page.Objects),
        page.IsTruncated,
        page.Objects[0].Key,
        page.Objects[9].Key,
        page.Objects[0].Size,
    )
    var it = c.List("logs", "app/", "")
    var n = 0
    var last = ""
    for o in it {
        n += 1
        last = o.Key
    }
    println("all", n, last, it.Err() == nil)
    var top []string = []
    for o in c.List("logs", "", "/") {
        if o.IsPrefix {
            top = append(top, o.Key + "(dir)")
        } else {
            top = append(top, o.Key)
        }
    }
    println("delimited", top)

    // copy and batch delete
    _ = await c.CopyObject(
        "photos",
        "2026/summer trip/ü+1.jpg",
        "logs",
        "copied.jpg",
        s3.PutOptions{},
    )?
    println("copy", strings.FromBytes(await c.GetObjectBytes("logs", "copied.jpg")?))
    var keys []string = ["copied.jpg", "readme", "db/1.log"]
    for i in 0..25 {
        keys = append(keys, logName(i))
    }
    st.buckets["logs"]["locked"] = object{data: "x", etag: "\"e\""}
    keys = append(keys, "locked")
    var failed = await c.DeleteObjects("logs", keys)?
    println(
        "delete objects failed",
        len(failed),
        failed[0].Key,
        failed[0].Code,
        "left",
        len(st.buckets["logs"]),
    )

    // multipart: 4 parts, 3 in parallel; then a failing part aborts
    var data = strings.Repeat("abcdefghij", 1100000)
    var up = await c.Upload(
        "photos",
        "video.bin",
        io.NewBytesReader(strings.Bytes(data)),
        3 << 20,
        3,
    )?
    println(
        "multipart",
        up.ETag == "\"" + crypto.hash.Md5Hex(data) + "-4\"",
        "parallel parts",
        st.maxParallelParts >= 2,
        len(st.uploads),
    )
    var back = await c.GetObjectBytes("photos", "video.bin")?
    println("multipart content", len(back) == len(data))
    st.failPart = 2
    try await c.Upload(
        "photos",
        "broken.bin",
        io.NewBytesReader(strings.Bytes(data)),
        3 << 20,
        2,
    ) catch err {
        println(
            "upload failure:",
            s3.ErrorCode(err),
            "pending uploads",
            len(st.uploads),
            st.log[len(st.log) - 1],
        )
    }
    st.failPart = 0
    var small = await c.Upload(
        "photos",
        "small.bin",
        io.NewBytesReader(strings.Bytes("tiny")),
        5 << 20,
        2,
    )?
    println("small upload is one PUT", small.ETag == "\"" + crypto.hash.Md5Hex("tiny") + "\"")

    // retries
    st.slowDown = 2
    println(
        "after SlowDown",
        strings.FromBytes(await c.GetObjectBytes("photos", "small.bin")?),
        "retries",
        c.Retries(),
    )
    st.slowDown = 10
    try await c.GetObjectBytes("photos", "small.bin") catch err {
        println("gave up:", s3.ErrorCode(err))
    }
    st.slowDown = 0

    // presigned URLs
    var getURL = c.Presign("GET", "photos", "small.bin", 300)?
    var r1 = await net.http.Get(getURL)?
    println("presigned get", r1.Status, r1.Body)
    var putURL = c.Presign("PUT", "photos", "uploaded.txt", 300)?
    var r2 = await net.http.NewRequest("PUT", putURL).Body("via presigned").Header(
        "Content-Md5",
        encoding.base64.Encode(crypto.hash.Md5("via presigned")),
    ).Send()?
    println(
        "presigned put",
        r2.Status,
        strings.FromBytes(await c.GetObjectBytes("photos", "uploaded.txt")?),
    )
    var tampered = strings.Replace(getURL, "small.bin", "big.txt")
    var r3 = await net.http.Get(tampered)?
    println("tampered", r3.Status, strings.Contains(r3.Body, "SignatureDoesNotMatch"))

    // blob: the same calls over S3 and over a directory
    await blobChecks("s3", cloud.blob.OpenS3(c, "photos", "blobs/"))?
    var dir = fs.TempDir() + "/slang_blob_" + strconv.Itoa(os.Pid())
    var db = cloud.blob.OpenDir(dir)?
    await blobChecks("dir", db)?
    try await db.Write("../escape", strings.Bytes("x"), "") catch err {
        println(err)
    }
    try db.SignedURL("k", "GET", 60) catch err {
        println(err)
    }
    fs.RemoveAll(dir)?

    // cleanup
    for k in maps.SortedKeys(st.buckets["photos"]) {
        await c.DeleteObject("photos", k)?
    }
    await c.DeleteBucket("photos")?
    try await c.DeleteBucket("photos") catch err {
        println("deleted twice:", errors.Is(err, s3.ErrNoSuchBucket))
    }
    println("rejected requests:", st.log[0])
    await srv.Close()
}
