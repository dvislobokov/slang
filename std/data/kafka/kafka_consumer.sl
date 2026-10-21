namespace std.data.kafka

// The consumer (see kafka.sl for the file layout): the consumer group
// protocol (FindCoordinator, JoinGroup, SyncGroup with the range or
// round-robin assignor computed by the group leader, Heartbeat in a
// background task, LeaveGroup), committed offsets (OffsetFetch,
// OffsetCommit; automatic every AutoCommitInterval unless ManualCommit),
// and fetching from the partition leaders (one Fetch per leader, in
// parallel). Without a Group the consumer reads every partition of its
// topics and does not commit.

// Options of a Consumer. Durations are milliseconds; zero values take the
// defaults in brackets.
struct ConsumerOptions {
    Config Config
    Group string
    Topics []string
    // "range" [default] or "roundrobin"
    Assignor string
    // [10000]
    SessionTimeout int
    // [30000]
    RebalanceTimeout int
    // [3000]
    HeartbeatInterval int
    // commit only with Commit / CommitRecord (else every
    // AutoCommitInterval [5000], before a rebalance and on Close)
    ManualCommit bool
    AutoCommitInterval int
    // where to start without a committed offset: "latest" [default] or
    // "earliest"
    StartOffset string
    // Fetch: broker wait [500], min bytes [1], max bytes [52428800],
    // per partition [1048576]
    MaxWait int
    MinBytes int
    MaxBytes int
    MaxPartitionBytes int
    // read only committed transactional records
    ReadCommitted bool
}

struct consumerState {
    guard int
    c *client
    o ConsumerOptions
    memberID string
    generation int
    coordinator int
    assignment []TopicPartition
    // next offset to fetch / after the last record handed out
    positions map[string]int
    consumed map[string]int
    buffered []Record
    needJoin bool
    joined bool
    closed bool
    // the heartbeat loop's current sleep (Close fires it) / fired by the
    // loop when it ends
    hbSleep Task<void>
    hbDone Task<void>
    hbRunning bool
    lastCommit int
    rebalances int
    mu std.task.Mutex
    err error

    func lock() {
        std.task.rtLock(&this.guard)
    }

    func unlock() {
        std.task.rtUnlock(&this.guard)
    }

    func unlockMu() {
        this.mu.Unlock()
    }
}

// A Kafka consumer; read with `for rec in consumer` (inside an async
// function), Next or Poll. One task at a time should read it.
struct Consumer {
    st *consumerState

    // The iteration protocol: the next record; the iteration ends when the
    // consumer is closed or fails (Err tells why).
    async func Next() (Record, bool) {
        var none Record
        for {
            if len(this.st.buffered) > 0 {
                var r = this.st.buffered[0]
                this.st.buffered = this.st.buffered[1..]
                this.st.consumed[tpKey(r.Topic, r.Partition)] = r.Offset + 1
                return (r, true)
            }
            if this.st.closed {
                return (none, false)
            }
            try await pollInto(this.st) catch err {
                if this.st.closed {
                    return (none, false)
                }
                this.st.err = err
                return (none, false)
            }
        }
    }

    // Records available within about maxWait milliseconds (may be none).
    func Poll(maxWait int) Task<[]Record!> {
        return pollBatch(this.st, maxWait)
    }

    // Commits the offsets after the records handed out so far.
    func Commit() Task<void!> {
        return commitConsumed(this.st)
    }

    // Commits r.Offset + 1 for r's partition.
    func CommitRecord(r Record) Task<void!> {
        var m = map[string]int{}
        m[tpKey(r.Topic, r.Partition)] = r.Offset + 1
        return commitOffsets(this.st, m)
    }

    // Moves the fetch position of a partition (of the current assignment).
    func Seek(topic string, partition int, offset int) {
        var k = tpKey(topic, partition)
        this.st.positions[k] = offset
        this.st.consumed[k] = offset
        var keep []Record = []
        for r in this.st.buffered {
            if r.Topic != topic || r.Partition != partition {
                keep = append(keep, r)
            }
        }
        this.st.buffered = keep
    }

    // The partitions assigned to this member.
    func Assignment() []TopicPartition {
        return this.st.assignment
    }

    // The committed offset of a partition (-1 if none).
    func Committed(topic string, partition int) Task<int!> {
        return committedOffset(this.st, topic, partition)
    }

    func MemberID() string {
        return this.st.memberID
    }

    func Generation() int {
        return this.st.generation
    }

    // How many times this member (re)joined its group.
    func Rebalances() int {
        return this.st.rebalances
    }

    // The error that ended the iteration (nil if none).
    func Err() error {
        return this.st.err
    }

    // Commits (unless ManualCommit), leaves the group and closes the
    // connections.
    func Close() Task<void> {
        return closeConsumer(this.st)
    }
}

// A consumer of topics in group (Group "": every partition, no commits).
async func NewConsumer(brokers []string, group string, topics []string) *Consumer! {
    return await NewConsumerWith(brokers, ConsumerOptions{Group: group, Topics: topics})?
}

async func NewConsumerWith(brokers []string, opts ConsumerOptions) *Consumer! {
    if len(opts.Topics) == 0 {
        return error("kafka: a consumer needs topics")
    }
    var o = opts
    if o.Assignor == "" {
        o.Assignor = "range"
    }
    if o.Assignor != "range" && o.Assignor != "roundrobin" {
        return error("kafka: unknown assignor " + std.strconv.Quote(o.Assignor))
    }
    if o.SessionTimeout <= 0 {
        o.SessionTimeout = 10000
    }
    if o.RebalanceTimeout <= 0 {
        o.RebalanceTimeout = 30000
    }
    if o.HeartbeatInterval <= 0 {
        o.HeartbeatInterval = 3000
    }
    if o.AutoCommitInterval <= 0 {
        o.AutoCommitInterval = 5000
    }
    if o.StartOffset == "" {
        o.StartOffset = "latest"
    }
    if o.StartOffset != "latest" && o.StartOffset != "earliest" {
        return error("kafka: StartOffset must be \"latest\" or \"earliest\"")
    }
    if o.MaxWait <= 0 {
        o.MaxWait = 500
    }
    if o.MinBytes <= 0 {
        o.MinBytes = 1
    }
    if o.MaxBytes <= 0 {
        o.MaxBytes = 52428800
    }
    if o.MaxPartitionBytes <= 0 {
        o.MaxPartitionBytes = 1048576
    }
    var cfg = o.Config
    if cfg.RequestTimeout > 0 && cfg.RequestTimeout < o.RebalanceTimeout + 5000 {
        cfg.RequestTimeout = o.RebalanceTimeout + 5000
    }
    if cfg.RequestTimeout <= 0 {
        cfg.RequestTimeout = o.RebalanceTimeout + 5000
    }
    var c = newClient(cfg, brokers)?
    var st = new consumerState{c: c, o: o, generation: -1, coordinator: -1, positions: map[string]int{}, consumed: map[string]int{}, needJoin: true, lastCommit: monoMs(), hbDone: std.task.rtWaker()}
    try {
        await refreshMetadata(c, o.Topics)?
        await st.mu.Lock()
        try await rejoin(st) catch err {
            st.mu.Unlock()
            return err
        }
        st.mu.Unlock()
    } catch err {
        c.closeAll()
        std.task.rtFire(st.hbDone)
        return err
    }
    return new Consumer{st: st}
}

// ---- assignors ------------------------------------------------------------------

// The range assignor: for each topic, its partitions split into
// contiguous ranges over the members subscribed to it (sorted by id; the
// first ones get one more when it does not divide).
func AssignRange(members map[string][]string, partitions map[string]int) map[string][]TopicPartition {
    var out = map[string][]TopicPartition{}
    var ids = std.maps.SortedKeys(members)
    for id in ids {
        out[id] = []
    }
    for topic in std.maps.SortedKeys(partitions) {
        var subs []string = []
        for id in ids {
            if std.slices.Contains(members[id], topic) {
                subs = append(subs, id)
            }
        }
        if len(subs) == 0 {
            continue
        }
        var n = partitions[topic]
        var per = n / len(subs)
        var extra = n % len(subs)
        var p = 0
        for i, id in subs {
            var cnt = per
            if i < extra {
                cnt += 1
            }
            for _ in 0..cnt {
                out[id] = append(out[id], TopicPartition{Topic: topic, Partition: p})
                p += 1
            }
        }
    }
    return out
}

// The round-robin assignor: every (topic, partition), sorted, dealt in
// turn to the members (sorted) subscribed to its topic.
func AssignRoundRobin(members map[string][]string, partitions map[string]int) map[string][]TopicPartition {
    var out = map[string][]TopicPartition{}
    var ids = std.maps.SortedKeys(members)
    for id in ids {
        out[id] = []
    }
    if len(ids) == 0 {
        return out
    }
    var next = 0
    for topic in std.maps.SortedKeys(partitions) {
        for p in 0..partitions[topic] {
            for _ in 0..len(ids) {
                var id = ids[next % len(ids)]
                next += 1
                if std.slices.Contains(members[id], topic) {
                    out[id] = append(out[id], TopicPartition{Topic: topic, Partition: p})
                    break
                }
            }
        }
    }
    return out
}

// ConsumerProtocolSubscription v0 / ConsumerProtocolAssignment v0.
func encodeSubscription(topics []string) []byte {
    var e = Encoder{}
    e.Int16(0)
    e.ArrayLen(len(topics))
    for t in topics {
        e.String(t)
    }
    e.NullBytes()
    return e.B
}

func decodeSubscription(b []byte) []string {
    var d = Decoder{B: b}
    _ = d.Int16()
    var out []string = []
    var n = d.ArrayLen()
    for _ in 0..n {
        out = append(out, d.String())
    }
    return out
}

func encodeAssignment(tps []TopicPartition) []byte {
    var byTopic = map[string][]int{}
    for tp in tps {
        byTopic[tp.Topic] = append(byTopic[tp.Topic], tp.Partition)
    }
    var e = Encoder{}
    e.Int16(0)
    var topics = std.maps.SortedKeys(byTopic)
    e.ArrayLen(len(topics))
    for t in topics {
        e.String(t)
        e.ArrayLen(len(byTopic[t]))
        for p in byTopic[t] {
            e.Int32(p)
        }
    }
    e.NullBytes()
    return e.B
}

func decodeAssignment(b []byte) []TopicPartition {
    var out []TopicPartition = []
    if len(b) == 0 {
        return out
    }
    var d = Decoder{B: b}
    _ = d.Int16()
    var n = d.ArrayLen()
    for _ in 0..n {
        var t = d.String()
        var np = d.ArrayLen()
        for _ in 0..np {
            out = append(out, TopicPartition{Topic: t, Partition: d.Int32()})
        }
    }
    return out
}

// ---- the group protocol ------------------------------------------------------------

async func groupConn(st *consumerState) *brokerConn! {
    if st.coordinator < 0 {
        st.coordinator = await findCoordinator(st.c, st.o.Group)?
    }
    return await connToBroker(st.c, st.coordinator, "group")?
}

// Codes after which the coordinator must be looked up again.
func coordinatorMoved(code int) bool {
    return code == 14 || code == 15 || code == 16
}

// Joins (or rejoins) the group and loads the starting offsets (mu held).
async func rejoin(st *consumerState) void! {
    if st.o.Group == "" {
        var tps []TopicPartition = []
        for t in st.o.Topics {
            var ti = await topicPartitions(st.c, t)?
            for p in ti.Partitions {
                tps = append(tps, TopicPartition{Topic: t, Partition: p.ID})
            }
        }
        await setAssignment(st, tps)?
        st.needJoin = false
        st.joined = true
        st.rebalances += 1
        return
    }
    if st.joined && !st.o.ManualCommit {
        try await commitPositions(st, st.consumed) catch _ {
        }
    }
    var attempt = 0
    for {
        attempt += 1
        if attempt > 30 {
            return error("kafka: could not join group " + std.strconv.Quote(st.o.Group))
        }
        var bc = try await groupConn(st) catch err {
            if isNetErr(err) {
                st.coordinator = -1
                await backoff(st.c.cfg, attempt)
                continue
            }
            return err
        }
        var e = Encoder{}
        e.String(st.o.Group)
        e.Int32(st.o.SessionTimeout)
        e.Int32(st.o.RebalanceTimeout)
        e.String(st.memberID)
        e.String("consumer")
        e.ArrayLen(1)
        e.String(st.o.Assignor)
        e.Bytes(encodeSubscription(st.o.Topics))
        var resp = try await request(st.c, bc, 11, 2, e.B, false) catch err {
            if isNetErr(err) {
                st.coordinator = -1
                await backoff(st.c.cfg, attempt)
                continue
            }
            return err
        }
        var d = Decoder{B: resp}
        _ = d.Int32()
        var code = d.Int16()
        var gen = d.Int32()
        var proto = d.String()
        var leader = d.String()
        var member = d.String()
        var subs = map[string][]string{}
        var nm = d.ArrayLen()
        for _ in 0..nm {
            var id = d.String()
            subs[id] = decodeSubscription(d.Bytes())
        }
        if code == 25 {
            st.memberID = ""
            continue
        }
        if code == 79 {
            st.memberID = member
            continue
        }
        if code == 27 {
            await backoff(st.c.cfg, 1)
            continue
        }
        if coordinatorMoved(code) {
            st.coordinator = -1
            await backoff(st.c.cfg, attempt)
            continue
        }
        if code != 0 {
            return kerr(code)
        }
        st.memberID = member
        st.generation = gen
        var assignments = map[string][]byte{}
        if leader == member {
            var counts = map[string]int{}
            var all []string = []
            for _, ts in subs {
                for t in ts {
                    if !std.slices.Contains(all, t) {
                        all = append(all, t)
                    }
                }
            }
            for t in all {
                var ti = await topicPartitions(st.c, t)?
                counts[t] = len(ti.Partitions)
            }
            var plan map[string][]TopicPartition
            if proto == "roundrobin" {
                plan = AssignRoundRobin(subs, counts)
            } else {
                plan = AssignRange(subs, counts)
            }
            for id, tps in plan {
                assignments[id] = encodeAssignment(tps)
            }
        }
        var s = Encoder{}
        s.String(st.o.Group)
        s.Int32(gen)
        s.String(member)
        var ids = std.maps.SortedKeys(assignments)
        s.ArrayLen(len(ids))
        for id in ids {
            s.String(id)
            s.Bytes(assignments[id])
        }
        var sresp = try await request(st.c, bc, 14, 1, s.B, false) catch err {
            if isNetErr(err) {
                st.coordinator = -1
                await backoff(st.c.cfg, attempt)
                continue
            }
            return err
        }
        var sd = Decoder{B: sresp}
        _ = sd.Int32()
        var scode = sd.Int16()
        var mine = sd.Bytes()
        if scode == 27 || scode == 22 {
            continue
        }
        if scode == 25 {
            st.memberID = ""
            continue
        }
        if coordinatorMoved(scode) {
            st.coordinator = -1
            continue
        }
        if scode != 0 {
            return kerr(scode)
        }
        await setAssignment(st, decodeAssignment(mine))?
        st.needJoin = false
        st.joined = true
        st.rebalances += 1
        startHeartbeats(st)
        return
    }
}

// Takes a new assignment: committed offsets, else StartOffset.
async func setAssignment(st *consumerState, tps []TopicPartition) void! {
    st.assignment = tps
    st.buffered = []
    st.positions = map[string]int{}
    st.consumed = map[string]int{}
    var committed = map[string]int{}
    if st.o.Group != "" && len(tps) > 0 {
        committed = await fetchCommitted(st, tps)?
    }
    for tp in tps {
        var k = tpKey(tp.Topic, tp.Partition)
        var off = -1
        if st.o.Group != "" {
            off = committed[k] - 1
        }
        if off < 0 {
            off = await resetOffset(st, tp.Topic, tp.Partition)?
        }
        st.positions[k] = off
        st.consumed[k] = off
    }
}

async func resetOffset(st *consumerState, topic string, partition int) int! {
    var ts = -1
    if st.o.StartOffset == "earliest" {
        ts = -2
    }
    return await listOffset(st.c, topic, partition, ts)?
}

// OffsetFetch v1: committed offset + 1 per partition key (0 if none).
async func fetchCommitted(st *consumerState, tps []TopicPartition) map[string]int! {
    var byTopic = map[string][]int{}
    for tp in tps {
        byTopic[tp.Topic] = append(byTopic[tp.Topic], tp.Partition)
    }
    var attempt = 0
    for {
        attempt += 1
        var bc = await groupConn(st)?
        var e = Encoder{}
        e.String(st.o.Group)
        var topics = std.maps.SortedKeys(byTopic)
        e.ArrayLen(len(topics))
        for t in topics {
            e.String(t)
            e.ArrayLen(len(byTopic[t]))
            for p in byTopic[t] {
                e.Int32(p)
            }
        }
        var resp = try await request(st.c, bc, 9, 1, e.B, false) catch err {
            if isNetErr(err) && attempt < 5 {
                st.coordinator = -1
                await backoff(st.c.cfg, attempt)
                continue
            }
            return err
        }
        var d = Decoder{B: resp}
        var out = map[string]int{}
        var retry = false
        var nt = d.ArrayLen()
        for _ in 0..nt {
            var t = d.String()
            var np = d.ArrayLen()
            for _ in 0..np {
                var p = d.Int32()
                var off = d.Int64()
                _ = d.String()
                var code = d.Int16()
                if code != 0 {
                    if (coordinatorMoved(code) || code == 14) && attempt < 10 {
                        retry = true
                    } else {
                        return kerr(code)
                    }
                }
                out[tpKey(t, p)] = off + 1
            }
        }
        if retry {
            st.coordinator = -1
            await backoff(st.c.cfg, attempt)
            continue
        }
        return out
    }
}

async func committedOffset(st *consumerState, topic string, partition int) int! {
    if st.o.Group == "" {
        return -1
    }
    var m = await fetchCommitted(st, [TopicPartition{Topic: topic, Partition: partition}])?
    return m[tpKey(topic, partition)] - 1
}

// OffsetCommit v2 of offsets (key -> next offset).
async func commitPositions(st *consumerState, offsets map[string]int) void! {
    if st.o.Group == "" {
        return error("kafka: a consumer without a group cannot commit")
    }
    var byTopic = map[string][]int{}
    for tp in st.assignment {
        var k = tpKey(tp.Topic, tp.Partition)
        if offsets[k] > 0 {
            byTopic[tp.Topic] = append(byTopic[tp.Topic], tp.Partition)
        }
    }
    if len(byTopic) == 0 {
        return
    }
    var bc = await groupConn(st)?
    var e = Encoder{}
    e.String(st.o.Group)
    e.Int32(st.generation)
    e.String(st.memberID)
    e.Int64(-1)
    var topics = std.maps.SortedKeys(byTopic)
    e.ArrayLen(len(topics))
    for t in topics {
        e.String(t)
        e.ArrayLen(len(byTopic[t]))
        for p in byTopic[t] {
            e.Int32(p)
            e.Int64(offsets[tpKey(t, p)])
            e.NullableString("", true)
        }
    }
    var resp = await request(st.c, bc, 8, 2, e.B, false)?
    var d = Decoder{B: resp}
    var nt = d.ArrayLen()
    for _ in 0..nt {
        _ = d.String()
        var np = d.ArrayLen()
        for _ in 0..np {
            _ = d.Int32()
            var code = d.Int16()
            if code == 27 || code == 22 || code == 25 {
                st.needJoin = true
                if code == 25 {
                    st.memberID = ""
                }
                return Error{Code: code, Text: "commit failed: the group is rebalancing"}
            }
            if coordinatorMoved(code) {
                st.coordinator = -1
                return kerr(code)
            }
            if code != 0 {
                return kerr(code)
            }
        }
    }
    st.lastCommit = monoMs()
}

async func commitConsumed(st *consumerState) void! {
    await st.mu.Lock()
    defer st.unlockMu()
    await commitPositions(st, st.consumed)?
}

async func commitOffsets(st *consumerState, offsets map[string]int) void! {
    await st.mu.Lock()
    defer st.unlockMu()
    await commitPositions(st, offsets)?
}

// ---- heartbeats -----------------------------------------------------------------

func startHeartbeats(st *consumerState) {
    st.lock()
    if st.hbRunning {
        st.unlock()
        return
    }
    st.hbRunning = true
    st.hbSleep = std.task.Delay(0)
    st.unlock()
    _ = heartbeatLoop(st)
}

async func heartbeatLoop(st *consumerState) {
    for {
        // (a cancellation scope would keep the task that created it, and
        // the consumer it returns, alive: Close fires the sleep instead)
        st.lock()
        if st.closed {
            st.unlock()
            break
        }
        var sleep = std.task.Delay(st.o.HeartbeatInterval)
        st.hbSleep = sleep
        st.unlock()
        await sleep
        if st.closed {
            break
        }
        if st.needJoin || st.generation < 0 {
            continue
        }
        var bc = try await groupConn(st) catch _ {
            st.coordinator = -1
            continue
        }
        var e = Encoder{}
        e.String(st.o.Group)
        e.Int32(st.generation)
        e.String(st.memberID)
        var resp = try await request(st.c, bc, 12, 1, e.B, false) catch _ {
            continue
        }
        var d = Decoder{B: resp}
        _ = d.Int32()
        var code = d.Int16()
        if code == 27 || code == 22 {
            st.needJoin = true
        } else if code == 25 {
            st.memberID = ""
            st.needJoin = true
        } else if coordinatorMoved(code) {
            st.coordinator = -1
        }
    }
    st.hbRunning = false
    std.task.rtFire(st.hbDone)
}

// ---- fetching -----------------------------------------------------------------

struct fetchPart {
    topic string
    partition int
    offset int
}

struct fetchResult {
    records []Record
    // new positions per key
    next map[string]int
    // partitions whose leader moved / offsets out of range
    stale bool
    reset []fetchPart
}

// One Fetch to a leader.
async func fetchFrom(st *consumerState, leader int, parts []fetchPart) fetchResult! {
    var res = fetchResult{next: map[string]int{}}
    var bc = try await connToBroker(st.c, leader, "fetch") catch err {
        if isNetErr(err) {
            res.stale = true
            return res
        }
        return err
    }
    var v = bc.version(1, 10, 4)
    if v < 0 {
        return error("kafka: broker " + bc.addr + " does not support Fetch v4+")
    }
    var e = Encoder{}
    e.Int32(-1)
    e.Int32(st.o.MaxWait)
    e.Int32(st.o.MinBytes)
    e.Int32(st.o.MaxBytes)
    if st.o.ReadCommitted {
        e.Int8(1)
    } else {
        e.Int8(0)
    }
    if v >= 7 {
        e.Int32(0)
        e.Int32(-1)
    }
    var byTopic = map[string][]fetchPart{}
    var topics []string = []
    for p in parts {
        if byTopic[p.topic] == nil {
            topics = append(topics, p.topic)
            byTopic[p.topic] = []
        }
        byTopic[p.topic] = append(byTopic[p.topic], p)
    }
    e.ArrayLen(len(topics))
    for t in topics {
        e.String(t)
        e.ArrayLen(len(byTopic[t]))
        for p in byTopic[t] {
            e.Int32(p.partition)
            if v >= 9 {
                e.Int32(-1)
            }
            e.Int64(p.offset)
            if v >= 5 {
                e.Int64(-1)
            }
            e.Int32(st.o.MaxPartitionBytes)
        }
    }
    if v >= 7 {
        e.ArrayLen(0)
    }
    var resp = try await request(st.c, bc, 1, v, e.B, false) catch err {
        if isNetErr(err) {
            res.stale = true
            return res
        }
        return err
    }
    var d = Decoder{B: resp}
    _ = d.Int32()
    if v >= 7 {
        var top = d.Int16()
        _ = d.Int32()
        if top != 0 {
            return kerr(top)
        }
    }
    var nt = d.ArrayLen()
    for _ in 0..nt {
        var t = d.String()
        var np = d.ArrayLen()
        for _ in 0..np {
            var p = d.Int32()
            var code = d.Int16()
            _ = d.Int64()
            _ = d.Int64()
            if v >= 5 {
                _ = d.Int64()
            }
            var aborted = map[int]int{}
            var na = d.Int32()
            for _ in 0..na {
                var pid = d.Int64()
                aborted[pid] = d.Int64() + 1
            }
            var data = d.Bytes()
            var k = tpKey(t, p)
            if code == 1 {
                res.reset = append(res.reset, fetchPart{topic: t, partition: p})
                continue
            }
            if code != 0 {
                if retriableCode(code) {
                    res.stale = true
                    continue
                }
                return kerr(code)
            }
            var pos = st.positions[k]
            var batches = DecodeBatches(data, t, p)?
            for b in batches {
                if b.Control {
                    if b.LastOffset + 1 > pos {
                        pos = b.LastOffset + 1
                    }
                    // a marker ends that producer's aborted range
                    delete(aborted, b.Producer.ID)
                    continue
                }
                var skip = st.o.ReadCommitted && b.Transactional && aborted[b.Producer.ID] > 0 && aborted[b.Producer.ID] - 1 <= b.BaseOffset
                for r in b.Records {
                    if r.Offset >= pos && !skip {
                        res.records = append(res.records, r)
                    }
                }
                if b.LastOffset + 1 > pos {
                    pos = b.LastOffset + 1
                }
            }
            res.next[k] = pos
        }
    }
    if d.Err {
        return error("kafka: malformed Fetch response")
    }
    return res
}

// One round of fetching from every leader (mu held): new records are
// appended to the buffer.
async func fetchRound(st *consumerState) void! {
    var byLeader = map[int][]fetchPart{}
    var leaders []int = []
    var stale = false
    for tp in st.assignment {
        var l = st.c.leaderOf(tp.Topic, tp.Partition)
        if l < 0 {
            stale = true
            continue
        }
        if byLeader[l] == nil {
            leaders = append(leaders, l)
            byLeader[l] = []
        }
        byLeader[l] = append(byLeader[l], fetchPart{topic: tp.Topic, partition: tp.Partition, offset: st.positions[tpKey(tp.Topic, tp.Partition)]})
    }
    var tasks []Task<fetchResult!> = []
    for l in leaders {
        tasks = append(tasks, fetchFrom(st, l, byLeader[l]))
    }
    var firstErr error
    for t in tasks {
        var r = try await t catch err {
            if firstErr == nil {
                firstErr = err
            }
            continue
        }
        for k, v in r.next {
            st.positions[k] = v
        }
        for rec in r.records {
            st.buffered = append(st.buffered, rec)
        }
        if r.stale {
            stale = true
        }
        for fp in r.reset {
            var off = await resetOffset(st, fp.topic, fp.partition)?
            st.positions[tpKey(fp.topic, fp.partition)] = off
        }
    }
    if stale {
        try await refreshMetadata(st.c, st.o.Topics) catch _ {
        }
        if len(st.buffered) == 0 {
            await backoff(st.c.cfg, 1)
        }
    }
    if firstErr != nil {
        return firstErr
    }
}

// Fetches until something is buffered or about maxWait passed (mu taken).
async func pollInto(st *consumerState) void! {
    await st.mu.Lock()
    defer st.unlockMu()
    await pollLocked(st, -1)?
}

async func pollLocked(st *consumerState, maxWait int) void! {
    var start = monoMs()
    for {
        if st.closed {
            return ErrClosed
        }
        if st.needJoin {
            await rejoin(st)?
        }
        if !st.o.ManualCommit && st.o.Group != "" && monoMs() - st.lastCommit >= st.o.AutoCommitInterval {
            try await commitPositions(st, st.consumed) catch _ {
            }
            st.lastCommit = monoMs()
        }
        if len(st.assignment) == 0 {
            await std.task.Delay(st.o.MaxWait)
        } else {
            await fetchRound(st)?
        }
        if len(st.buffered) > 0 || (maxWait >= 0 && monoMs() - start >= maxWait) {
            return
        }
    }
}

async func pollBatch(st *consumerState, maxWait int) []Record! {
    await st.mu.Lock()
    defer st.unlockMu()
    if len(st.buffered) == 0 {
        await pollLocked(st, maxWait)?
    }
    var out = st.buffered
    st.buffered = []
    for r in out {
        st.consumed[tpKey(r.Topic, r.Partition)] = r.Offset + 1
    }
    return out
}

async func closeConsumer(st *consumerState) {
    if st.closed {
        return
    }
    await st.mu.Lock()
    st.lock()
    st.closed = true
    st.unlock()
    if st.o.Group != "" && st.joined {
        if !st.o.ManualCommit {
            try await commitPositions(st, st.consumed) catch _ {
            }
        }
        try {
            var bc = await groupConn(st)?
            var e = Encoder{}
            e.String(st.o.Group)
            e.String(st.memberID)
            _ = await request(st.c, bc, 13, 1, e.B, false)?
        } catch _ {
        }
    }
    st.mu.Unlock()
    st.lock()
    var running = st.hbRunning
    var sleep = st.hbSleep
    st.unlock()
    if running {
        std.task.rtFire(sleep)
        await st.hbDone
    }
    st.c.closeAll()
}
