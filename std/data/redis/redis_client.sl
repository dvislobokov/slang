namespace std.data.redis

// Client and the typed command set (see redis.sl for the file layout).
//
// Every command method returns a started Task: `await c.Get("k")?`. The
// same methods (Cmds, embedded) exist on a Pipeline, whose commands are
// queued and complete when Exec has run (`var a = p.Incr("n")`, `await
// p.Exec()?`, then `await a?`), and on a Tx (the connection of a Watch,
// commands run at once). Values are passed as `any` and sent as text
// (strings and []byte as they are, numbers in decimal, bools as 1/0);
// several values are a slice (`c.Del(["a", "b"])`, `c.LPush("l", [1, 2])`),
// there are no variadic parameters.

// What the command methods send to: the router (Client), a pipeline queue
// or a transaction connection.
interface executor {
    func process(args []any) Task<std.data.redis.Reply!>
}

// A member of a sorted set with its score.
struct Z {
    Score float
    Member string
}

// One page of SCAN: the next cursor (0 at the end) and the keys.
struct ScanResult {
    Cursor int
    Keys []string
}

// A stream entry (XRANGE).
struct XMessage {
    ID string
    Values map[string]string
}

// Options of SetWith: TTL in milliseconds (0: none), NX (only if missing),
// XX (only if present), KeepTTL.
struct SetOptions {
    TTL int
    NX bool
    XX bool
    KeepTTL bool
}

// ---- reply conversions --------------------------------------------------------------

async func toReply(t Task<Reply!>) Reply! {
    return await t?
}

async func toText(t Task<Reply!>) string! {
    var r = await t?
    return r.Text()?
}

async func toInt(t Task<Reply!>) int! {
    var r = await t?
    return r.Integer()?
}

async func toFloat(t Task<Reply!>) float! {
    var r = await t?
    return r.Number()?
}

async func toBool(t Task<Reply!>) bool! {
    var r = await t?
    return r.Boolean()?
}

async func toOK(t Task<Reply!>) void! {
    var r = await t?
    _ = r
}

async func toStrings(t Task<Reply!>) []string! {
    var r = await t?
    return r.Strings()?
}

async func toMap(t Task<Reply!>) map[string]string! {
    var r = await t?
    return r.StringMap()?
}

async func toReplies(t Task<Reply!>) []Reply! {
    var r = await t?
    return r.Values()?
}

async func toBools(t Task<Reply!>) []bool! {
    var r = await t?
    var out []bool = []
    for e in r.Values()? {
        out = append(out, e.Boolean()?)
    }
    return out
}

// [m1, s1, m2, s2] (RESP2) or [[m1, s1], [m2, s2]] (RESP3) as []Z.
func zOf(r Reply) []Z! {
    var out []Z = []
    var es = r.Values()?
    if len(es) > 0 && es[0].Kind == Kind.Array {
        for e in es {
            if len(e.Elems) == 2 {
                out = append(out, Z{Member: e.Elems[0].Text()?, Score: e.Elems[1].Number()?})
            }
        }
        return out
    }
    var i = 0
    for i + 1 < len(es) {
        out = append(out, Z{Member: es[i].Text()?, Score: es[i + 1].Number()?})
        i += 2
    }
    return out
}

async func toZ(t Task<Reply!>) []Z! {
    var r = await t?
    return zOf(r)?
}

async func toScan(t Task<Reply!>) ScanResult! {
    var r = await t?
    var es = r.Values()?
    if len(es) != 2 {
        return error("redis: bad SCAN reply " + r.String())
    }
    return ScanResult{Cursor: std.strconv.Atoi(es[0].Text()?)?("redis: SCAN cursor"), Keys: es[1].Strings()?}
}

// BLPOP/BRPOP: [key, value]; Nil after the timeout.
async func toPop(t Task<Reply!>) []string! {
    var r = await t?
    if r.Kind == Kind.Nil {
        return Nil
    }
    if r.Kind == Kind.Map {
        return r.Strings()?
    }
    return r.Strings()?
}

// SET with NX/XX: false when the condition did not hold.
async func toSetOK(t Task<Reply!>) bool! {
    var r = await t?
    return r.Kind != Kind.Nil
}

async func toXRange(t Task<Reply!>) []XMessage! {
    var r = await t?
    var out []XMessage = []
    for e in r.Values()? {
        if len(e.Elems) != 2 {
            continue
        }
        out = append(out, XMessage{ID: e.Elems[0].Text()?, Values: e.Elems[1].StringMap()?})
    }
    return out
}

async func toScoreOrNil(t Task<Reply!>) float! {
    var r = await t?
    return r.Number()?
}

func withKeys(head []any, keys []string) []any {
    var out = head
    for k in keys {
        out = append(out, k)
    }
    return out
}

func withValues(head []any, vals []any) []any {
    var out = head
    for v in vals {
        out = append(out, v)
    }
    return out
}

func evalArgs(cmd string, script string, keys []string, args []any) []any {
    var a []any = [cmd, script, len(keys)]
    a = withKeys(a, keys)
    return withValues(a, args)
}

// ---- the command set ----------------------------------------------------------------------

// The typed commands, embedded in Client, Pipeline and Tx. A missing value
// (GET of an absent key, LPOP of an empty list...) fails with redis.Nil.
struct Cmds {
    x executor

    // Any command: `c.Do(["CLIENT", "INFO"])`.
    func Do(args []any) Task<Reply!> {
        return toReply(this.x.process(args))
    }

    // ---- connection and server ----

    func Ping() Task<string!> {
        return toText(this.x.process(["PING"]))
    }

    func Echo(message string) Task<string!> {
        return toText(this.x.process(["ECHO", message]))
    }

    func Info(section string) Task<string!> {
        if section == "" {
            return toText(this.x.process(["INFO"]))
        }
        return toText(this.x.process(["INFO", section]))
    }

    func DBSize() Task<int!> {
        return toInt(this.x.process(["DBSIZE"]))
    }

    func FlushDB() Task<void!> {
        return toOK(this.x.process(["FLUSHDB"]))
    }

    func FlushAll() Task<void!> {
        return toOK(this.x.process(["FLUSHALL"]))
    }

    func ConfigGet(parameter string) Task<map[string]string!> {
        return toMap(this.x.process(["CONFIG", "GET", parameter]))
    }

    func ConfigSet(parameter string, value string) Task<void!> {
        return toOK(this.x.process(["CONFIG", "SET", parameter, value]))
    }

    // ---- keys ----

    func Del(keys []string) Task<int!> {
        return toInt(this.x.process(withKeys(["DEL"], keys)))
    }

    func Unlink(keys []string) Task<int!> {
        return toInt(this.x.process(withKeys(["UNLINK"], keys)))
    }

    // How many of keys exist.
    func Exists(keys []string) Task<int!> {
        return toInt(this.x.process(withKeys(["EXISTS"], keys)))
    }

    func Expire(key string, seconds int) Task<bool!> {
        return toBool(this.x.process(["EXPIRE", key, seconds]))
    }

    func PExpire(key string, ms int) Task<bool!> {
        return toBool(this.x.process(["PEXPIRE", key, ms]))
    }

    func ExpireAt(key string, unixSeconds int) Task<bool!> {
        return toBool(this.x.process(["EXPIREAT", key, unixSeconds]))
    }

    // Seconds to live: -1 without expiry, -2 for a missing key.
    func TTL(key string) Task<int!> {
        return toInt(this.x.process(["TTL", key]))
    }

    func PTTL(key string) Task<int!> {
        return toInt(this.x.process(["PTTL", key]))
    }

    func Persist(key string) Task<bool!> {
        return toBool(this.x.process(["PERSIST", key]))
    }

    func Type(key string) Task<string!> {
        return toText(this.x.process(["TYPE", key]))
    }

    func Rename(key string, newKey string) Task<void!> {
        return toOK(this.x.process(["RENAME", key, newKey]))
    }

    func RenameNX(key string, newKey string) Task<bool!> {
        return toBool(this.x.process(["RENAMENX", key, newKey]))
    }

    func Keys(pattern string) Task<[]string!> {
        return toStrings(this.x.process(["KEYS", pattern]))
    }

    // One SCAN page (match "" and count 0 leave the options out).
    func Scan(cursor int, match string, count int) Task<ScanResult!> {
        var a []any = ["SCAN", cursor]
        if match != "" {
            a = append(a, "MATCH")
            a = append(a, match)
        }
        if count > 0 {
            a = append(a, "COUNT")
            a = append(a, count)
        }
        return toScan(this.x.process(a))
    }

    func RandomKey() Task<string!> {
        return toText(this.x.process(["RANDOMKEY"]))
    }

    // ---- strings ----

    func Get(key string) Task<string!> {
        return toText(this.x.process(["GET", key]))
    }

    func Set(key string, value any) Task<void!> {
        return toOK(this.x.process(["SET", key, value]))
    }

    // SET with a TTL in seconds (SETEX).
    func SetEx(key string, value any, seconds int) Task<void!> {
        return toOK(this.x.process(["SETEX", key, seconds, value]))
    }

    // SET with a TTL in milliseconds (PSETEX).
    func PSetEx(key string, value any, ms int) Task<void!> {
        return toOK(this.x.process(["PSETEX", key, ms, value]))
    }

    // SET ... NX [PX ttl]: whether the key was set.
    func SetNX(key string, value any, ttlMs int) Task<bool!> {
        if ttlMs > 0 {
            return toSetOK(this.x.process(["SET", key, value, "PX", ttlMs, "NX"]))
        }
        return toSetOK(this.x.process(["SET", key, value, "NX"]))
    }

    // SET with options: whether the key was set (false when NX/XX failed).
    func SetWith(key string, value any, o SetOptions) Task<bool!> {
        var a []any = ["SET", key, value]
        if o.TTL > 0 {
            a = append(a, "PX")
            a = append(a, o.TTL)
        }
        if o.KeepTTL {
            a = append(a, "KEEPTTL")
        }
        if o.NX {
            a = append(a, "NX")
        }
        if o.XX {
            a = append(a, "XX")
        }
        return toSetOK(this.x.process(a))
    }

    func GetSet(key string, value any) Task<string!> {
        return toText(this.x.process(["GETSET", key, value]))
    }

    func GetDel(key string) Task<string!> {
        return toText(this.x.process(["GETDEL", key]))
    }

    func GetRange(key string, start int, end int) Task<string!> {
        return toText(this.x.process(["GETRANGE", key, start, end]))
    }

    func Append(key string, value any) Task<int!> {
        return toInt(this.x.process(["APPEND", key, value]))
    }

    func StrLen(key string) Task<int!> {
        return toInt(this.x.process(["STRLEN", key]))
    }

    func Incr(key string) Task<int!> {
        return toInt(this.x.process(["INCR", key]))
    }

    func Decr(key string) Task<int!> {
        return toInt(this.x.process(["DECR", key]))
    }

    func IncrBy(key string, n int) Task<int!> {
        return toInt(this.x.process(["INCRBY", key, n]))
    }

    func DecrBy(key string, n int) Task<int!> {
        return toInt(this.x.process(["DECRBY", key, n]))
    }

    func IncrByFloat(key string, f float) Task<float!> {
        return toFloat(this.x.process(["INCRBYFLOAT", key, f]))
    }

    // The values of keys; a missing key is a Nil reply.
    func MGet(keys []string) Task<[]Reply!> {
        return toReplies(this.x.process(withKeys(["MGET"], keys)))
    }

    func MSet(pairs map[string]any) Task<void!> {
        var a []any = ["MSET"]
        for k, v in pairs {
            a = append(a, k)
            a = append(a, v)
        }
        return toOK(this.x.process(a))
    }

    func MSetNX(pairs map[string]any) Task<bool!> {
        var a []any = ["MSETNX"]
        for k, v in pairs {
            a = append(a, k)
            a = append(a, v)
        }
        return toBool(this.x.process(a))
    }

    // ---- hashes ----

    func HGet(key string, field string) Task<string!> {
        return toText(this.x.process(["HGET", key, field]))
    }

    // Sets one field: 1 if it is new.
    func HSet(key string, field string, value any) Task<int!> {
        return toInt(this.x.process(["HSET", key, field, value]))
    }

    // Sets several fields: the number of new ones.
    func HSetMap(key string, values map[string]any) Task<int!> {
        var a []any = ["HSET", key]
        for k, v in values {
            a = append(a, k)
            a = append(a, v)
        }
        return toInt(this.x.process(a))
    }

    func HSetNX(key string, field string, value any) Task<bool!> {
        return toBool(this.x.process(["HSETNX", key, field, value]))
    }

    func HMGet(key string, fields []string) Task<[]Reply!> {
        return toReplies(this.x.process(withKeys(["HMGET", key], fields)))
    }

    func HGetAll(key string) Task<map[string]string!> {
        return toMap(this.x.process(["HGETALL", key]))
    }

    func HDel(key string, fields []string) Task<int!> {
        return toInt(this.x.process(withKeys(["HDEL", key], fields)))
    }

    func HExists(key string, field string) Task<bool!> {
        return toBool(this.x.process(["HEXISTS", key, field]))
    }

    func HIncrBy(key string, field string, n int) Task<int!> {
        return toInt(this.x.process(["HINCRBY", key, field, n]))
    }

    func HIncrByFloat(key string, field string, f float) Task<float!> {
        return toFloat(this.x.process(["HINCRBYFLOAT", key, field, f]))
    }

    func HKeys(key string) Task<[]string!> {
        return toStrings(this.x.process(["HKEYS", key]))
    }

    func HVals(key string) Task<[]string!> {
        return toStrings(this.x.process(["HVALS", key]))
    }

    func HLen(key string) Task<int!> {
        return toInt(this.x.process(["HLEN", key]))
    }

    // ---- lists ----

    func LPush(key string, values []any) Task<int!> {
        return toInt(this.x.process(withValues(["LPUSH", key], values)))
    }

    func RPush(key string, values []any) Task<int!> {
        return toInt(this.x.process(withValues(["RPUSH", key], values)))
    }

    func LPop(key string) Task<string!> {
        return toText(this.x.process(["LPOP", key]))
    }

    func RPop(key string) Task<string!> {
        return toText(this.x.process(["RPOP", key]))
    }

    func LLen(key string) Task<int!> {
        return toInt(this.x.process(["LLEN", key]))
    }

    func LRange(key string, start int, stop int) Task<[]string!> {
        return toStrings(this.x.process(["LRANGE", key, start, stop]))
    }

    func LIndex(key string, index int) Task<string!> {
        return toText(this.x.process(["LINDEX", key, index]))
    }

    func LSet(key string, index int, value any) Task<void!> {
        return toOK(this.x.process(["LSET", key, index, value]))
    }

    func LRem(key string, count int, value any) Task<int!> {
        return toInt(this.x.process(["LREM", key, count, value]))
    }

    func LTrim(key string, start int, stop int) Task<void!> {
        return toOK(this.x.process(["LTRIM", key, start, stop]))
    }

    // Blocking pops: [key, value], Nil after timeout seconds (0: forever).
    func BLPop(timeout float, keys []string) Task<[]string!> {
        var a = withKeys(["BLPOP"], keys)
        a = append(a, timeout)
        return toPop(this.x.process(a))
    }

    func BRPop(timeout float, keys []string) Task<[]string!> {
        var a = withKeys(["BRPOP"], keys)
        a = append(a, timeout)
        return toPop(this.x.process(a))
    }

    func RPopLPush(source string, destination string) Task<string!> {
        return toText(this.x.process(["RPOPLPUSH", source, destination]))
    }

    // LMOVE source destination LEFT|RIGHT LEFT|RIGHT.
    func LMove(source string, destination string, from string, to string) Task<string!> {
        return toText(this.x.process(["LMOVE", source, destination, from, to]))
    }

    // ---- sets ----

    func SAdd(key string, members []any) Task<int!> {
        return toInt(this.x.process(withValues(["SADD", key], members)))
    }

    func SRem(key string, members []any) Task<int!> {
        return toInt(this.x.process(withValues(["SREM", key], members)))
    }

    func SMembers(key string) Task<[]string!> {
        return toStrings(this.x.process(["SMEMBERS", key]))
    }

    func SIsMember(key string, member any) Task<bool!> {
        return toBool(this.x.process(["SISMEMBER", key, member]))
    }

    func SCard(key string) Task<int!> {
        return toInt(this.x.process(["SCARD", key]))
    }

    func SPop(key string) Task<string!> {
        return toText(this.x.process(["SPOP", key]))
    }

    func SRandMember(key string) Task<string!> {
        return toText(this.x.process(["SRANDMEMBER", key]))
    }

    func SInter(keys []string) Task<[]string!> {
        return toStrings(this.x.process(withKeys(["SINTER"], keys)))
    }

    func SUnion(keys []string) Task<[]string!> {
        return toStrings(this.x.process(withKeys(["SUNION"], keys)))
    }

    func SDiff(keys []string) Task<[]string!> {
        return toStrings(this.x.process(withKeys(["SDIFF"], keys)))
    }

    func SMove(source string, destination string, member any) Task<bool!> {
        return toBool(this.x.process(["SMOVE", source, destination, member]))
    }

    // ---- sorted sets ----

    func ZAdd(key string, members []Z) Task<int!> {
        var a []any = ["ZADD", key]
        for m in members {
            a = append(a, m.Score)
            a = append(a, m.Member)
        }
        return toInt(this.x.process(a))
    }

    func ZIncrBy(key string, increment float, member string) Task<float!> {
        return toFloat(this.x.process(["ZINCRBY", key, increment, member]))
    }

    func ZScore(key string, member string) Task<float!> {
        return toScoreOrNil(this.x.process(["ZSCORE", key, member]))
    }

    func ZRank(key string, member string) Task<int!> {
        return toInt(this.x.process(["ZRANK", key, member]))
    }

    func ZRevRank(key string, member string) Task<int!> {
        return toInt(this.x.process(["ZREVRANK", key, member]))
    }

    func ZRem(key string, members []any) Task<int!> {
        return toInt(this.x.process(withValues(["ZREM", key], members)))
    }

    func ZCard(key string) Task<int!> {
        return toInt(this.x.process(["ZCARD", key]))
    }

    // Members with min <= score <= max ("-inf", "(1.5", "+inf"...).
    func ZCount(key string, min string, max string) Task<int!> {
        return toInt(this.x.process(["ZCOUNT", key, min, max]))
    }

    func ZRange(key string, start int, stop int) Task<[]string!> {
        return toStrings(this.x.process(["ZRANGE", key, start, stop]))
    }

    func ZRangeWithScores(key string, start int, stop int) Task<[]Z!> {
        return toZ(this.x.process(["ZRANGE", key, start, stop, "WITHSCORES"]))
    }

    func ZRevRange(key string, start int, stop int) Task<[]string!> {
        return toStrings(this.x.process(["ZREVRANGE", key, start, stop]))
    }

    func ZRangeByScore(key string, min string, max string) Task<[]string!> {
        return toStrings(this.x.process(["ZRANGEBYSCORE", key, min, max]))
    }

    func ZRangeByScoreWithScores(key string, min string, max string) Task<[]Z!> {
        return toZ(this.x.process(["ZRANGEBYSCORE", key, min, max, "WITHSCORES"]))
    }

    func ZRevRangeByScore(key string, max string, min string) Task<[]string!> {
        return toStrings(this.x.process(["ZREVRANGEBYSCORE", key, max, min]))
    }

    func ZRemRangeByScore(key string, min string, max string) Task<int!> {
        return toInt(this.x.process(["ZREMRANGEBYSCORE", key, min, max]))
    }

    func ZPopMin(key string, count int) Task<[]Z!> {
        return toZ(this.x.process(["ZPOPMIN", key, count]))
    }

    // ---- HyperLogLog ----

    func PFAdd(key string, elements []any) Task<int!> {
        return toInt(this.x.process(withValues(["PFADD", key], elements)))
    }

    func PFCount(keys []string) Task<int!> {
        return toInt(this.x.process(withKeys(["PFCOUNT"], keys)))
    }

    // ---- streams ----

    // XADD stream id field value...; id "*" lets the server choose.
    func XAdd(stream string, id string, values map[string]any) Task<string!> {
        var a []any = ["XADD", stream, id]
        for k, v in values {
            a = append(a, k)
            a = append(a, v)
        }
        return toText(this.x.process(a))
    }

    func XLen(stream string) Task<int!> {
        return toInt(this.x.process(["XLEN", stream]))
    }

    func XRange(stream string, start string, end string) Task<[]XMessage!> {
        return toXRange(this.x.process(["XRANGE", stream, start, end]))
    }

    // ---- pub/sub and scripting ----

    // Publishes message: the number of subscribers that received it.
    func Publish(channel string, message any) Task<int!> {
        return toInt(this.x.process(["PUBLISH", channel, message]))
    }

    func Eval(script string, keys []string, args []any) Task<Reply!> {
        return toReply(this.x.process(evalArgs("EVAL", script, keys, args)))
    }

    func EvalSha(sha string, keys []string, args []any) Task<Reply!> {
        return toReply(this.x.process(evalArgs("EVALSHA", sha, keys, args)))
    }

    // Loads a script into the script cache: its SHA1.
    func ScriptLoad(script string) Task<string!> {
        return toText(this.x.process(["SCRIPT", "LOAD", script]))
    }

    func ScriptExists(shas []string) Task<[]bool!> {
        return toBools(this.x.process(withKeys(["SCRIPT", "EXISTS"], shas)))
    }

    func ScriptFlush() Task<void!> {
        return toOK(this.x.process(["SCRIPT", "FLUSH"]))
    }
}

// ---- Client ------------------------------------------------------------------------

// A Redis client: a pool of connections to one server, the master named
// by Sentinel, or a Cluster. Safe for concurrent use by many tasks.
struct Client {
    Cmds
    c *core

    // A pipeline: commands are queued and sent in one round trip by Exec.
    func Pipeline() *Pipeline {
        return newPipeline(this.c, nil, false)
    }

    // A pipeline wrapped in MULTI/EXEC (atomic).
    func TxPipeline() *Pipeline {
        return newPipeline(this.c, nil, true)
    }

    // Optimistic locking: WATCH keys on one connection, run fn with a Tx
    // on it, then UNWATCH. Inside fn, read with tx.Get..., then queue the
    // writes on tx.TxPipeline() and Exec them: Exec fails with ErrTxFailed
    // if a watched key changed in between.
    func Watch(keys []string, fn func(*Tx) Task<void!>) Task<void!> {
        return watchRun(this.c, keys, fn)
    }

    // Subscribes to channels on a dedicated connection.
    func Subscribe(channels []string) Task<*Subscription!> {
        return subscribeNew(this.c, channels, false)
    }

    // Subscribes to channel patterns ("news.*").
    func PSubscribe(patterns []string) Task<*Subscription!> {
        return subscribeNew(this.c, patterns, true)
    }

    // The value of key decoded from JSON (std.encoding.json).
    func GetJSON<T>(key string) Task<T!> {
        return getJSON<T>(this.c, key)
    }

    // Stores v as JSON; ttlMs > 0 sets an expiry.
    func SetJSON<T>(key string, v T, ttlMs int) Task<void!> {
        var text = std.encoding.json.Format<T>(v)
        if ttlMs > 0 {
            return toOK(this.c.process(["SET", key, text, "PX", ttlMs]))
        }
        return toOK(this.c.process(["SET", key, text]))
    }

    // Iterates over the keys matching pattern with SCAN (every master of
    // a Cluster): `for key in c.Iter("user:*", 100)`.
    func Iter(pattern string, count int) *ScanIterator {
        return new ScanIterator{c: this.c, match: pattern, count: count}
    }

    // Runs a script with EVALSHA, loading it with EVAL when the server
    // does not have it cached (NOSCRIPT).
    func Run(s *Script, keys []string, args []any) Task<Reply!> {
        return runScript(this.c, s, keys, args)
    }

    // Statistics of the connection pools.
    func PoolStats() PoolStats {
        var s = PoolStats{}
        for p in this.c.allPools() {
            var x = p.snapshot()
            s.Hits += x.Hits
            s.Misses += x.Misses
            s.Timeouts += x.Timeouts
            s.TotalConns += x.TotalConns
            s.IdleConns += x.IdleConns
            s.StaleConns += x.StaleConns
        }
        return s
    }

    // The current master address (Sentinel) or the server address.
    func MasterAddr() string {
        return this.c.main.currentAddr()
    }

    // How many times the Sentinel client moved to another master.
    func Failovers() int {
        this.c.lock()
        var n = this.c.failovers
        this.c.unlock()
        return n
    }

    // The cluster masters (Cluster clients).
    func ClusterMasters() []string {
        return this.c.masterAddrs()
    }

    // The node serving key's slot in the client's slot map (Cluster).
    func NodeFor(key string) string {
        return this.c.slotAddr(HashSlot(key))
    }

    // How many times the slot map was loaded (Cluster).
    func SlotReloads() int {
        this.c.lock()
        var n = this.c.reloads
        this.c.unlock()
        return n
    }

    // The options in effect (with defaults).
    func Options() Options {
        return this.c.opts
    }

    // Closes every connection; later commands fail with ErrClosed. The
    // task completes when background readers (Sentinel watcher) stopped.
    func Close() Task<void> {
        return this.c.close()
    }
}

func clientOf(c *core) *Client {
    return new Client{Cmds: Cmds{x: c}, c: c}
}

// A client for opts; connections are made on first use.
func NewClient(opts Options) *Client {
    return clientOf(newCore(opts))
}

// Connects to the server of a redis://, rediss:// or unix:// URL
// (ParseURL) and checks the connection with PING.
async func Connect(url string) *Client! {
    var o = ParseURL(url)?
    return await ConnectWith(o)?
}

// NewClient + PING.
async func ConnectWith(opts Options) *Client! {
    var c = NewClient(opts)
    try await c.Ping() catch err {
        await c.Close()
        return err
    }
    return c
}

// A Sentinel client for master name, discovering it through sentinels.
async func ConnectSentinel(sentinels []string, master string) *Client! {
    return await ConnectSentinelWith(SentinelOptions{Sentinels: sentinels, MasterName: master})?
}

async func ConnectSentinelWith(so SentinelOptions) *Client! {
    if len(so.Sentinels) == 0 {
        return error("redis: no sentinel addresses")
    }
    var c = newCore(so.Options)
    c.mode = 1
    c.sopts = so
    c.sentOpts = sentinelOptions(so)
    var cl = clientOf(c)
    try {
        var addr = await masterAddr(c)?
        c.main.setAddr(addr)
        if so.ReadFromReplicas {
            await refreshReplicas(c)?
        }
        await watchSentinels(c)?
        _ = await cl.Ping()?
    } catch err {
        await cl.Close()
        return err
    }
    return cl
}

// A Cluster client starting from some of its nodes.
async func ConnectCluster(nodes []string) *Client! {
    return await ConnectClusterWith(ClusterOptions{Nodes: nodes})?
}

async func ConnectClusterWith(co ClusterOptions) *Client! {
    if len(co.Nodes) == 0 {
        return error("redis: no cluster nodes")
    }
    var c = newCore(co.Options)
    c.mode = 2
    c.seeds = co.Nodes
    c.maxRedirects = co.MaxRedirects
    if c.maxRedirects <= 0 {
        c.maxRedirects = 8
    }
    var cl = clientOf(c)
    try await reloadSlots(c) catch err {
        await cl.Close()
        return err
    }
    return cl
}

async func getJSON<T>(c *core, key string) T! {
    var r = await c.process(["GET", key])?
    return std.encoding.json.Parse<T>(r.Text()?)?
}

// The typed value of key (JSON): redis.Get<User>(c, "user:1").
func Get<T>(c *Client, key string) Task<T!> {
    return getJSON<T>(c.c, key)
}

// Stores v as JSON (no expiry).
func Set<T>(c *Client, key string, v T) Task<void!> {
    return toOK(c.c.process(["SET", key, std.encoding.json.Format<T>(v)]))
}

// ---- scripts --------------------------------------------------------------------------

// A Lua script with its SHA1 (for EVALSHA).
struct Script {
    Src string
    Hash string

    // Runs the script on c (EVALSHA, EVAL on NOSCRIPT).
    func Run(c *Client, keys []string, args []any) Task<Reply!> {
        return runScript(c.c, this, keys, args)
    }

    // Loads it into the script cache of c's server(s).
    func Load(c *Client) Task<string!> {
        return toText(c.c.process(["SCRIPT", "LOAD", this.Src]))
    }
}

func NewScript(src string) *Script {
    return new Script{Src: src, Hash: std.crypto.hash.Sha1Hex(src)}
}

async func runScript(c *core, s *Script, keys []string, args []any) Reply! {
    var r = try await c.process(evalArgs("EVALSHA", s.Hash, keys, args)) catch err {
        if ErrorCode(err) == "NOSCRIPT" {
            return await c.process(evalArgs("EVAL", s.Src, keys, args))?
        }
        return err
    }
    return r
}

// ---- pipelines ----------------------------------------------------------------------------

struct pipeQueue {
    c *core
    tx *txState
    multi bool
    cmds [][]any
    results []Reply
    err error
    done Task<void>
    ran bool

    func process(args []any) Task<Reply!> {
        if this.ran {
            return pipeFail(error("redis: command queued on a pipeline that has already run"))
        }
        this.cmds = append(this.cmds, args)
        return pipeWait(this, len(this.cmds) - 1)
    }
}

async func pipeFail(err error) Reply! {
    return err
}

async func pipeWait(q *pipeQueue, i int) Reply! {
    await q.done
    if q.err != nil {
        return q.err
    }
    if i >= len(q.results) {
        return error("redis: pipeline was discarded")
    }
    var r = q.results[i]
    if r.Kind == Kind.Error {
        return r.Err()
    }
    return r
}

// Queued commands (Client.Pipeline / TxPipeline, Tx.TxPipeline). Each
// command method returns a task that completes when Exec has run; Exec
// itself yields every reply (a server error as an Error reply) and fails
// only when the round trip fails, or with ErrTxFailed when a WATCHed key
// changed. Call Exec or Discard: queued tasks wait until then.
struct Pipeline {
    Cmds
    q *pipeQueue

    // Sends the queued commands in one round trip.
    func Exec() Task<[]Reply!> {
        return pipeExec(this.q)
    }

    // Drops the queued commands (their tasks fail).
    func Discard() {
        if !this.q.ran {
            this.q.ran = true
            this.q.err = error("redis: pipeline was discarded")
            std.task.rtFire(this.q.done)
        }
    }

    // The number of queued commands.
    func Len() int {
        return len(this.q.cmds)
    }
}

func newPipeline(c *core, tx *txState, multi bool) *Pipeline {
    var q = new pipeQueue{c: c, tx: tx, multi: multi, done: std.task.rtWaker()}
    return new Pipeline{Cmds: Cmds{x: q}, q: q}
}

async func pipeExec(q *pipeQueue) []Reply! {
    if q.ran {
        return error("redis: pipeline has already run")
    }
    q.ran = true
    var rs = try await runQueue(q) catch err {
        q.err = err
        std.task.rtFire(q.done)
        return err
    }
    q.results = rs
    std.task.rtFire(q.done)
    return rs
}

async func runQueue(q *pipeQueue) []Reply! {
    if len(q.cmds) == 0 {
        return []
    }
    if q.tx != nil {
        if q.tx.cn == nil {
            return error("redis: the transaction has finished")
        }
        return await pipelineConn(q.tx.cn, q.cmds, q.multi)?
    }
    return await corePipeline(q.c, q.cmds, q.multi)?
}

// ---- transactions (WATCH) --------------------------------------------------------------

struct txState {
    c *core
    p *pool
    cn *conn

    func process(args []any) Task<Reply!> {
        return txDo(this, args)
    }
}

async func txDo(st *txState, args []any) Reply! {
    if st.cn == nil {
        return error("redis: the transaction has finished")
    }
    var r = await st.cn.do(args)?
    if r.Kind == Kind.Error {
        return r.Err()
    }
    return r
}

// The connection of a Client.Watch: commands run on it at once.
struct Tx {
    Cmds
    st *txState

    // MULTI/EXEC on the watched connection.
    func TxPipeline() *Pipeline {
        return newPipeline(this.st.c, this.st, true)
    }

    // A plain pipeline on the watched connection.
    func Pipeline() *Pipeline {
        return newPipeline(this.st.c, this.st, false)
    }

    func Unwatch() Task<void!> {
        return toOK(this.st.process(["UNWATCH"]))
    }
}

async func watchRun(c *core, keys []string, fn func(*Tx) Task<void!>) void! {
    if len(keys) == 0 {
        return error("redis: Watch needs at least one key")
    }
    var args = withKeys(["WATCH"], keys)
    var p = await route(c, "WATCH", args)?
    var cn = await p.get()?
    var st = new txState{c: c, p: p, cn: cn}
    var tx = new Tx{Cmds: Cmds{x: st}, st: st}
    var r = try await cn.do(args) catch err {
        st.cn = nil
        p.put(cn)
        return err
    }
    if r.Kind == Kind.Error {
        st.cn = nil
        p.put(cn)
        return r.Err()
    }
    var failed error
    try await fn(tx) catch err {
        failed = err
    }
    st.cn = nil
    if !cn.bad {
        try {
            _ = await cn.do(["UNWATCH"])?
        } catch _ {
        }
    }
    p.put(cn)
    if failed != nil {
        return failed
    }
}

// ---- SCAN iteration ---------------------------------------------------------------------

// Iterates SCAN over the server (every master of a Cluster): the
// iteration protocol, `for key in c.Iter(pattern, count)`. A failed page
// ends the iteration; Err() tells why.
struct ScanIterator {
    c *core
    match string
    count int
    nodes []string
    node int
    started bool
    cursor int
    buf []string
    done bool
    err error

    async func Next() (string, bool) {
        for {
            if len(this.buf) > 0 {
                var k = this.buf[0]
                this.buf = this.buf[1..]
                return (k, true)
            }
            if this.done {
                return ("", false)
            }
            try await this.page() catch err {
                this.err = err
                this.done = true
            }
        }
    }

    // The error that ended the iteration (nil at a normal end).
    func Err() error {
        return this.err
    }

    async func page() void! {
        if !this.started {
            this.started = true
            if this.c.mode == 2 {
                this.nodes = this.c.masterAddrs()
                if len(this.nodes) == 0 {
                    await reloadSlots(this.c)?
                    this.nodes = this.c.masterAddrs()
                }
            }
        }
        var a []any = ["SCAN", this.cursor]
        if this.match != "" {
            a = append(a, "MATCH")
            a = append(a, this.match)
        }
        if this.count > 0 {
            a = append(a, "COUNT")
            a = append(a, this.count)
        }
        var r Reply
        if this.c.mode == 2 {
            if this.node >= len(this.nodes) {
                this.done = true
                return
            }
            r = await once(this.c.nodePool(this.nodes[this.node]), a, false)?
            if r.Kind == Kind.Error {
                return r.Err()
            }
        } else {
            r = await this.c.process(a)?
        }
        var es = r.Values()?
        if len(es) != 2 {
            return error("redis: bad SCAN reply")
        }
        this.cursor = std.strconv.Atoi(es[0].Text()?)?
        this.buf = es[1].Strings()?
        if this.cursor == 0 {
            if this.c.mode == 2 && this.node + 1 < len(this.nodes) {
                this.node += 1
            } else {
                this.done = true
            }
        }
    }
}

// ---- pub/sub --------------------------------------------------------------------------------

// A pub/sub message: Kind "message" or "pmessage" (then Pattern is the
// matching pattern).
struct Message {
    Kind string
    Channel string
    Pattern string
    Payload string
}

struct subState {
    guard int
    cn *conn
    dialFn func() Task<*conn!>
    // for internal watchers: called for every message instead of queuing it
    onMessage func(Message) Task<void>
    onReconnect func() Task<void>
    channels []string
    patterns []string
    msgs std.task.Channel<Message>
    acks std.task.Channel<int>
    wmu std.task.Mutex
    closed bool
    // confirmations of a resubscription after a reconnect (not acks)
    swallow int
    reconnects int
    dropped int
    // fired when the read loop has ended
    loopDone Task<void>

    func lock() {
        std.task.rtLock(&this.guard)
    }

    func unlock() {
        std.task.rtUnlock(&this.guard)
    }
}

func newSubState(dialFn func() Task<*conn!>) *subState {
    return new subState{dialFn: dialFn, msgs: std.task.Channel<Message>(1024), acks: std.task.Channel<int>(1024), loopDone: std.task.rtWaker()}
}

func removeAll(xs []string, del []string) []string {
    var out []string = []
    for x in xs {
        var keep = true
        for d in del {
            if d == x {
                keep = false
            }
        }
        if keep {
            out = append(out, x)
        }
    }
    return out
}

// A subscription on its own connection. Messages are queued (up to 1024;
// more are dropped and counted) and read with Receive or `for msg in sub`
// (ends when the subscription is closed). A lost connection is redialed
// and every channel and pattern subscribed again.
struct Subscription {
    st *subState
    // the subscription's read timeout for acknowledgements
    ackTimeout int

    func Subscribe(channels []string) Task<void!> {
        return subCmd(this.st, "SUBSCRIBE", channels, this.ackTimeout)
    }

    func PSubscribe(patterns []string) Task<void!> {
        return subCmd(this.st, "PSUBSCRIBE", patterns, this.ackTimeout)
    }

    func Unsubscribe(channels []string) Task<void!> {
        return subCmd(this.st, "UNSUBSCRIBE", channels, this.ackTimeout)
    }

    func PUnsubscribe(patterns []string) Task<void!> {
        return subCmd(this.st, "PUNSUBSCRIBE", patterns, this.ackTimeout)
    }

    // The next message; fails with ErrClosed once the subscription is
    // closed and its queue drained.
    func Receive() Task<Message!> {
        return subRecv(this.st)
    }

    // The iteration protocol.
    async func Next() (Message, bool) {
        var m = try await this.st.msgs.Recv() catch _ {
            var none Message
            return (none, false)
        }
        return (m, true)
    }

    // The channel the messages are queued on.
    func Channel() std.task.Channel<Message> {
        return this.st.msgs
    }

    // Subscribed channels and patterns.
    func Channels() []string {
        this.st.lock()
        var c = this.st.channels
        this.st.unlock()
        return c
    }

    func Patterns() []string {
        this.st.lock()
        var p = this.st.patterns
        this.st.unlock()
        return p
    }

    // Times the connection was redialed.
    func Reconnects() int {
        this.st.lock()
        var n = this.st.reconnects
        this.st.unlock()
        return n
    }

    // Messages dropped because the queue was full.
    func Dropped() int {
        this.st.lock()
        var n = this.st.dropped
        this.st.unlock()
        return n
    }

    // Closes the connection; queued messages can still be read, then the
    // iteration ends. The task completes when the reader has stopped.
    func Close() Task<void> {
        var st = this.st
        st.lock()
        if !st.closed {
            st.closed = true
            var cn = st.cn
            st.unlock()
            if cn != nil {
                cn.close()
            }
        } else {
            st.unlock()
        }
        return st.loopDone
    }
}

async func subRecv(st *subState) Message! {
    var m = try await st.msgs.Recv() catch err {
        if std.errors.Is(err, std.task.ErrCancelled) {
            return err
        }
        return ErrClosed
    }
    return m
}

async func subCmd(st *subState, cmd string, names []string, timeout int) void! {
    if len(names) == 0 {
        return
    }
    st.lock()
    if st.closed {
        st.unlock()
        return ErrClosed
    }
    if cmd == "SUBSCRIBE" {
        st.channels = removeAll(st.channels, names)
        for n in names {
            st.channels = append(st.channels, n)
        }
    } else if cmd == "PSUBSCRIBE" {
        st.patterns = removeAll(st.patterns, names)
        for n in names {
            st.patterns = append(st.patterns, n)
        }
    } else if cmd == "UNSUBSCRIBE" {
        st.channels = removeAll(st.channels, names)
    } else {
        st.patterns = removeAll(st.patterns, names)
    }
    var cn = st.cn
    st.unlock()
    await st.wmu.Lock()
    try await cn.send(appendCommand([], withKeys([cmd], names))) catch err {
        st.wmu.Unlock()
        return err
    }
    st.wmu.Unlock()
    var t = timeout
    if t <= 0 {
        t = 5000
    }
    for _ in names {
        var ack = st.acks.Recv()
        if !await std.task.Timeout(ack, t) {
            return error("redis: no confirmation of " + cmd + " within " + std.strconv.Itoa(t) + " ms")
        }
        _ = await ack?
    }
}

// Handles one pushed reply of the subscription connection.
async func subHandle(st *subState, r Reply) {
    if r.Kind != Kind.Array && r.Kind != Kind.Push {
        return
    }
    if len(r.Elems) < 2 {
        return
    }
    var kind = std.strings.ToLower(try r.Elems[0].Text() else "")
    var m = Message{Kind: kind}
    if kind == "message" && len(r.Elems) >= 3 {
        m.Channel = try r.Elems[1].Text() else ""
        m.Payload = try r.Elems[2].Text() else ""
    } else if kind == "pmessage" && len(r.Elems) >= 4 {
        m.Pattern = try r.Elems[1].Text() else ""
        m.Channel = try r.Elems[2].Text() else ""
        m.Payload = try r.Elems[3].Text() else ""
    } else {
        if kind == "subscribe" || kind == "psubscribe" || kind == "unsubscribe" || kind == "punsubscribe" {
            st.lock()
            var swallowed = st.swallow > 0
            if swallowed {
                st.swallow -= 1
            }
            st.unlock()
            if !swallowed {
                await st.acks.Send(1)
            }
        }
        return
    }
    if st.onMessage != nil {
        await st.onMessage(m)
        return
    }
    if st.msgs.Len() >= st.msgs.Cap() {
        st.lock()
        st.dropped += 1
        st.unlock()
        return
    }
    await st.msgs.Send(m)
}

// Redials and subscribes everything again (backoff up to 1 s).
async func subReconnect(st *subState) bool {
    var delay = 20
    for {
        st.lock()
        var closed = st.closed
        st.unlock()
        if closed || std.task.IsCancelled() {
            return false
        }
        var cn = try await st.dialFn() catch _ {
            await std.task.Delay(delay)
            if delay < 1000 {
                delay = delay * 2
            }
            continue
        }
        st.lock()
        if st.closed {
            st.unlock()
            cn.close()
            return false
        }
        var chans = st.channels
        var pats = st.patterns
        st.swallow = len(chans) + len(pats)
        cn.s.SetTimeout(0)
        st.cn = cn
        st.reconnects += 1
        st.unlock()
        var b []byte = []
        if len(chans) > 0 {
            b = appendCommand(b, withKeys(["SUBSCRIBE"], chans))
        }
        if len(pats) > 0 {
            b = appendCommand(b, withKeys(["PSUBSCRIBE"], pats))
        }
        if len(b) > 0 {
            await st.wmu.Lock()
            try await cn.send(b) catch _ {
                st.wmu.Unlock()
                continue
            }
            st.wmu.Unlock()
        }
        if st.onReconnect != nil {
            await st.onReconnect()
        }
        return true
    }
}

async func subLoop(st *subState) {
    for {
        st.lock()
        var cn = st.cn
        var closed = st.closed
        st.unlock()
        if closed {
            break
        }
        var r = try await cn.read() catch _ {
            st.lock()
            closed = st.closed
            st.unlock()
            if closed || !await subReconnect(st) {
                break
            }
            continue
        }
        await subHandle(st, r)
    }
    st.msgs.Close()
    st.acks.Close()
    st.dialFn = nil
    st.onMessage = nil
    st.onReconnect = nil
    std.task.rtFire(st.loopDone)
}

async func startSubscription(st *subState) *Subscription! {
    st.cn = await st.dialFn()?
    // a subscriber waits for messages without a read timeout
    st.cn.s.SetTimeout(0)
    _ = subLoop(st)
    return new Subscription{st: st}
}

async func dialFor(c *core) *conn! {
    var p = c.main
    if c.mode == 2 {
        var ms = c.masterAddrs()
        if len(ms) == 0 {
            await reloadSlots(c)?
            ms = c.masterAddrs()
        }
        return await dial(c.opts, ms[std.random.IntN(len(ms))])?
    }
    return await dial(c.opts, p.currentAddr())?
}

struct subDialer {
    c weak *core
}

async func subDial(d subDialer) *conn! {
    var c = d.c
    if c == nil {
        return ErrClosed
    }
    return await dialFor(c)?
}

async func subscribeNew(c *core, names []string, patterns bool) *Subscription! {
    var d = subDialer{c: c}
    var st = newSubState(func() Task<*conn!> {
        return subDial(d)
    })
    var sub = await startSubscription(st)?
    sub.ackTimeout = c.opts.ReadTimeout
    if patterns {
        try await sub.PSubscribe(names) catch err {
            await sub.Close()
            return err
        }
    } else {
        try await sub.Subscribe(names) catch err {
            await sub.Close()
            return err
        }
    }
    return sub
}
