# Kafka interop test

`compiler/tests/kafka.rs` runs `kafka_real.zl` against a real broker. It is
`#[ignore]`d because a normal test run has no broker:

```sh
KAFKA_BROKERS=localhost:9092 cargo test --test kafka -- --ignored
```

With `KAFKA_SASL_BROKERS` it also checks SASL PLAIN, SCRAM-SHA-256 and
SCRAM-SHA-512 (users `alice`/`alice-secret`, `bob`/`bob-secret`,
`carol`/`carol-secret`, wrong passwords rejected).

## A local single-node broker (KRaft, no ZooKeeper)

Kafka 3.x (tested with 3.7.0, Java 21) from
<https://archive.apache.org/dist/kafka/>. `server.properties` here listens on
`127.0.0.1:19092` (PLAINTEXT), `19093` (controller) and `19094`
(SASL_PLAINTEXT with PLAIN and SCRAM):

```sh
KAFKA=/opt/kafka   # unpacked kafka_2.13-3.7.0
rm -rf /tmp/zell-kafka-data
$KAFKA/bin/kafka-storage.sh format -c compiler/tests/fixtures/kafka/server.properties \
  -t "$($KAFKA/bin/kafka-storage.sh random-uuid)" \
  --add-scram 'SCRAM-SHA-256=[name=bob,password=bob-secret]' \
  --add-scram 'SCRAM-SHA-512=[name=carol,password=carol-secret]'
KAFKA_HEAP_OPTS=-Xmx768m $KAFKA/bin/kafka-server-start.sh compiler/tests/fixtures/kafka/server.properties &

KAFKA_BROKERS=127.0.0.1:19092 KAFKA_SASL_BROKERS=127.0.0.1:19094 \
  cargo test --test kafka -- --ignored
```

What the program covers: admin (create with TOPIC_ALREADY_EXISTS, describe,
list, delete), an idempotent producer with batching and the Java client's
murmur2 partitioning, gzip and zstd batches, acks `leader` and `none`,
headers and explicit timestamps, a consumer group with manual commits, a
second member joining (range rebalance, no redelivery of committed
records), a group-less consumer from the end with `for rec in consumer`,
and SASL. Topic and group names get a timestamp suffix, so it can run
repeatedly against one broker.
