# Стандартная библиотека: карта Go → Slang

Все пакеты стандартной библиотеки Go, их место в Slang, статус и чем
версия Slang лучше. Уровни — порядок реализации по зависимостям: пакет
уровня N зависит только от уровней < N. Внутри уровня порядок свободный.

Статусы: **есть** — реализовано и покрыто тестами; **частично** — есть
ядро; **нет** — не начато; **не нужно** — сознательно не переносится.

Общие улучшения относительно Go, которые касаются всех пакетов:
- ошибки `T!` + `?` вместо `(T, error)`; типизированные sentinel-ошибки с
  `errors.Is/As` и трассой;
- generics везде, где Go использовал `interface{}` или кодогенерацию
  (`sort`, `container`, `sync.Map`, `heap`);
- `async` для всего ввода-вывода, без потока на соединение;
- атрибуты вместо строковых тегов (`@json(name: "id")`, проверяются
  компилятором);
- compile-time рефлексия для сериализации (скорость ручного кода);
- шаблоны логов/форматов проверяются при компиляции;
- пространства имён с полным путём вместо коротких пакетов.

## Уровень 0 — без зависимостей

| Go | Slang | Статус | Лучше чем в Go |
|---|---|---|---|
| `builtin` (len, append, panic…) | встроенные | есть | `?`, `try`, generics |
| `errors` | `std.errors` | есть | `Trace`/`Frames`, типизированные ошибки, `As<T>` |
| `cmp` | `Ordered`/`Comparable` constraints, `cmp.Compare/Min/Max` | частично (constraints есть) | встроенные ограничения |
| `math` | `std.math` | есть | int-варианты (`IMin`), константы `MaxInt` |
| `math/bits` | `std.math.bits` | нет | — |
| `math/cmplx` | `std.math.complex` | нет | тип `complex128` с операторами |
| `math/big` | `std.math.big` | нет | `Int`, `Rat`, `Float`, `Decimal` с операторами через интерфейсы |
| `unicode`, `unicode/utf8`, `unicode/utf16` | `std.unicode`, `std.unicode.utf8/utf16` | частично (классификация в strings) | полные таблицы категорий, нормализация NFC/NFD |
| `strings` | `std.strings` | есть | `Builder`, `Runes`, `Reverse`, `Pad*`, `Title` |
| `bytes` | `std.bytes` | нет | те же функции над `[]byte`, `bytes.Buffer` |
| `strconv` | `std.strconv` | частично (ToInt/ToFloat, в fmt) | `Parse<T>`/`Format<T>` generic, ошибки с позицией |
| `slices` | `std.slices` | нет | `Map`, `Filter`, `Reduce`, `Sort`, `Chunk`, `Window`, `Unique`, `Zip` generic |
| `maps` | `std.maps` | нет | `Keys`, `Values`, `Sorted`, `Merge`, `Invert` |
| `sort` | `std.slices.Sort*` | нет | generic, стабильная сортировка по умолчанию, `SortBy(key)` |
| `container/list`, `ring`, `heap` | `std.collections` (`List`, `Map`, `Set`, `Queue`, `Stack`) + `LinkedList`, `Ring`, `Heap<T>`, `PriorityQueue<T>`, `Deque<T>`, `OrderedMap`, `LRU` | частично | generic, без `any` |
| `iter` | `for x in` + `iter.Seq<T>` интерфейс | нет | ленивые генераторы через async? — нет; итератор-интерфейс |
| `hash`, `hash/fnv`, `crc32`, `crc64`, `adler32`, `maphash` | `std.hash` | частично (crc32) | generic `Hasher<T>` |
| `encoding/binary` | `std.encoding.binary` | нет | `Read<T>`/`Write<T>` структур через compile-time рефлексию, `@binary(endian)` |
| `time` (типы) | `std.time` | частично (функции над int ms) | типы `Time`, `Duration` с операторами, `Instant` монотонный, часовые пояса (tzdata встроен) |
| `unsafe` | `std.unsafe` (только FFI) | нет | минимальный, помечает capability |
| `weak`, `unique` | `weak *T` в языке; `std.intern` | есть (weak) / нет | язык |

## Уровень 1 — базовый ввод-вывод, ОС, синхронизация

| Go | Slang | Статус | Лучше чем в Go |
|---|---|---|---|
| `io` | `std.io` (`Reader`, `Writer`, `Closer`, `Seeker`, `Copy`, `ReadAll`, `LimitReader`, `MultiWriter`, `Pipe`, `TeeReader`) | частично (println, файлы) | интерфейсы async-совместимые (`AsyncReader`), `Copy` с `SendFile` |
| `bufio` | `std.io.Buffered*`, `Scanner` | нет | `Lines()`/`Words()` итераторы |
| `fmt` | `std.fmt` | есть (Format, Sprintf) | `{0}`-шаблоны проверяются при компиляции, `Stringer` интерфейс |
| `os` | `std.os` | есть (args, env, exit, cwd, hostname…) | константы платформы, `OnInterrupt` |
| `os/signal` | `std.os.signal` | нет | async `await signal.Wait(SIGTERM)` |
| `os/exec` | `std.process` | нет | async `Run`, пайпы как `Reader/Writer`, таймауты через контекст |
| `os/user` | `std.os.user` | нет | — |
| `io/fs`, `path`, `path/filepath` | `std.fs` (+ `fs.Path` функции) | есть | `Walk` async, glob, `Watch` (inotify/FSEvents) |
| `embed` | `@embed("file")` атрибут | нет | проверяется при компиляции, `embed.FS` |
| `flag` | `std.cli` | нет | подкоманды, help из атрибутов `@arg`, env fallback |
| `sync`, `sync/atomic` | `std.task` (`Mutex`, `WaitGroup`) + `std.sync` (`RwMutex`, `Once`, `Atomic<T>`, `Pool<T>`) | частично | async-aware мьютексы, generic атомики |
| `context` | контекст задачи в `std.task` | есть | неявная передача, типизированные ключи, отмена дерева |
| `math/rand`, `crypto/rand` | `std.random`, `std.crypto.rand` | есть | — |
| `crypto/*` примитивы | `std.crypto.{hash,hmac,aes,chacha,x25519,ecdsa,ed25519,rsa,hkdf,kdf,password}` | есть (hash, hmac); в работе (aes…rsa); нет (kdf, password) | `password.Hash/Verify` на Argon2 из коробки |
| `encoding/base64`, `base32`, `hex`, `ascii85`, `pem`, `asn1` | `std.encoding.*` | есть (base64, hex); в работе (pem, asn1 для x509); нет (base32, ascii85) | — |
| `runtime` | `std.runtime` (`NumCPU`, `Version`, `GC` → нет) | частично (`diag.Runtime()`) | — |
| `testing` | `std.testing` + `slang test` | нет | `@test`, `@bench`, табличные тесты, `assert`, snapshot |
| `log` | `std.log` | есть | уровень Serilog: шаблоны при компиляции, sinks, enrichers |
| `log/slog` | то же | есть | — |
| `log/syslog` | `log.WriteTo.Syslog` | нет | — |
| `expvar` | `std.diag` метрики | есть | Prometheus/OTLP |
| `runtime/pprof`, `runtime/trace`, `runtime/debug` | `std.diag` | частично (cpu, heap) | pprof-совместимо, спаны OTel |

## Уровень 2 — форматы, текст, сжатие, сеть (ядро)

| Go | Slang | Статус | Лучше чем в Go |
|---|---|---|---|
| `encoding/json` | `std.encoding.json` | есть (Parse<T>/Format/Value) | compile-time, пути в ошибках, атрибуты |
| `encoding/xml` | `std.encoding.xml` | нет | та же модель, pull-reader |
| — (yaml, toml в Go нет) | `std.encoding.yaml`, `toml` | нет | в стандартной библиотеке, round-trip с комментариями |
| `encoding/csv` | `std.encoding.csv` | нет | `Read<T>`/`Write<T>` в структуры |
| `encoding/gob` | `std.encoding.binary` схема-типизированный | нет | — |
| `regexp`, `regexp/syntax` | `std.regex` | нет | литералы проверяются при компиляции, именованные группы, `Iter` |
| `text/template`, `html/template` | `std.text.template`, `std.html.template` | нет | типизированные шаблоны (проверка полей при компиляции), автоэкранирование |
| `text/scanner`, `text/tabwriter` | `std.text.scanner`, `std.text.table` | нет | — |
| `html` | `std.html` (escape/unescape) | нет | — |
| `mime`, `mime/multipart`, `mime/quotedprintable` | `std.mime` | нет | — |
| `compress/gzip`, `zlib`, `flate`, `bzip2`, `lzw` + zstd, brotli | `std.compress.{gzip,zlib,deflate,zstd,brotli}` | нет | zstd и brotli из коробки, async потоки |
| `archive/zip`, `archive/tar` | `std.archive.{zip,tar}` | нет | — |
| `net` (TCP/UDP/Unix/DNS/IP) | `std.net` | есть; в работе (закрытие пробелов) | async без потока на соединение, типизированные ошибки, Happy Eyeballs |
| `net/netip` | `net.IP`, `net.Addr` | в работе | — |
| `net/url` | `std.net.url` | частично (ParseURL в http) | — |
| `net/textproto` | `std.net.textproto` | нет | — |
| `image`, `image/png`, `jpeg`, `gif`, `color`, `draw` | `std.image` | нет | + webp, avif декодеры позже |
| `index/suffixarray` | `std.text.suffixarray` | не нужно (пакет) | — |
| `database/sql`, `sql/driver` | `std.database.sql` | нет | async, `Query<T>` в структуры, пул |
| `go/*` (ast, parser, types…) | `std.slang.{ast,parser,types}` — экспорт компилятора как библиотеки | частично (внутри компилятора) | — |
| `debug/elf`, `dwarf`, `pe`, `macho` | `std.debug.*` | нет (для `diag` частично) | — |
| `plugin` | `std.plugin` (dlopen) | нет | capability-помечен |

## Уровень 3 — протоколы и сервисы

| Go | Slang | Статус | Лучше чем в Go |
|---|---|---|---|
| `crypto/tls` | `std.net.tls` | в работе (1.3 + 1.2 ECDHE/AEAD) | только современные наборы, без C-зависимостей |
| `crypto/x509`, `x509/pkix` | `std.crypto.x509` | в работе | — |
| `net/http` | `std.net.http` | есть (1.1 клиент/сервер, роутер); нет (пул, HTTP/2, HTTP/3) | async, роутер с параметрами в stdlib, graceful shutdown |
| `net/http/httputil`, `httptest`, `cookiejar`, `fcgi`, `cgi`, `pprof` | `std.net.http.{util,test,cookies}`, `diag` endpoints | нет | — |
| — (websocket в Go нет) | `std.net.ws` | нет | в stdlib |
| `net/mail`, `net/smtp` | `std.net.mail` | нет | — |
| `net/rpc`, `rpc/jsonrpc` | `slang.grpc`, `std.net.jsonrpc` | нет | gRPC вместо net/rpc |
| `testing/fstest`, `iotest`, `quick` | `std.testing.*` | нет | property-based `quick` с generics |
| `syscall` | `std.sys` (слой платформы) | частично (внутри рантайма) | единый API на всех ОС |
| `time` (таймеры, тикеры) | `task.Delay`, `task.Ticker` | частично | async |

## Уровень 4 — фреймворк (`docs/FRAMEWORK.md`)

`slang.web`, `slang.data`, `slang.log` (расширенные sinks), `slang.diag`
(endpoints, trace), `slang.config`, `slang.cache`, `slang.jobs`,
`slang.auth`, `slang.messaging`, `slang.cli`, `slang.test`.

## Чего в Go нет, а в Slang будет в stdlib

`collections` (полный набор), `yaml`/`toml`, `regex` с проверкой при
компиляции, `websocket`, `zstd`/`brotli`, `uuid`, `password` (Argon2),
`decimal`/`big` с операторами, `log` уровня Serilog, `diag` с OTel и
Prometheus, `cli` с подкомандами, `arena`, типизированный `template`.

## План волн

1. **Уровень 0 + 1 пробелы**: `slices`, `maps`, `sort`, `bytes`, `strconv`,
   `time.Time/Duration`, `math.bits/big/complex`, `unicode`, `hash`,
   `encoding.binary`, `collections` (heap, deque, LRU, ordered map), `io`
   интерфейсы + `bufio`, `process`, `signal`, `cli`, `sync`, `testing`,
   `embed`, `base32/pem/asn1`.
2. **Уровень 2**: `regex`, `compress`, `archive`, `csv`, `xml`, `yaml`,
   `toml`, `template`, `html`, `mime`, `url`, `textproto`, `image` (png,
   jpeg).
3. **Уровень 3**: HTTP/2, пул соединений, websocket, `mail`, `sql` +
   PostgreSQL/SQLite, `httptest`, `debug/elf`.
4. **Уровень 4**: фреймворк.

Таблица обновляется при каждом изменении статуса.
