# Стандартная библиотека: карта Go → Slang

Все пакеты стандартной библиотеки Go, их место в Slang, статус и чем
версия Slang лучше. Уровни — порядок реализации по зависимостям: пакет
уровня N зависит только от уровней < N. Внутри уровня порядок свободный.

Статусы: ✅ есть — реализовано и покрыто тестами; 🟡 частично — есть ядро;
🔧 в работе; ❌ нет — не начато; ⛔ не нужно — сознательно не переносится.

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
| `builtin` (len, append, panic…) | встроенные | ✅ есть | `?`, `try`, generics |
| `errors` | `std.errors` | ✅ есть | `Trace`/`Frames`, типизированные ошибки, `As<T>` |
| `cmp` | `Ordered`/`Comparable` constraints, `std.cmp` | ✅ есть (`Compare`, `Less`, `Min`, `Max`, `Clamp`, `Or`; NaN как в Go) | встроенные ограничения |
| `math` | `std.math` | ✅ есть | int-варианты (`IMin`), константы `MaxInt` |
| `math/bits` | `std.math.bits` | ✅ есть (`LeadingZeros/TrailingZeros/OnesCount/Len/RotateLeft/Reverse/ReverseBytes` (+`32`), `Add64/Sub64/Mul64/Div64` кортежами, `Add32/Mul32`; C-builtins) | кортежи вместо пар результатов |
| `math/cmplx` | `std.math.complex` | ✅ есть (`Complex{Re, Im}`: `Add/Sub/Mul/Div/Neg/Conj/Abs/Phase/Scale/String`; `Exp/Log/Sqrt/Pow/Sin/Cos/Polar/Rect/IsNaN/IsInf`) | методы с именами интерфейсов операторов (операторы — когда появятся) |
| `math/big` | `std.math.big` | ✅ есть (`Int`: арифметика, `Quo/Rem/Div/Mod`, `Exp`, `ModInverse`, `GCD`, `Sqrt`, `Lsh/Rsh`, `ProbablyPrime`, `Text/ParseInt` 2..36, `Bytes/FromBytes`; `Rat`; `Decimal` с 7 режимами округления; лимбы на C: Karatsuba, Knuth D); нет `Float` | неизменяемые значения, `Decimal` в stdlib, методы с именами интерфейсов операторов |
| `unicode`, `unicode/utf8`, `unicode/utf16` | `std.unicode`, `std.unicode.utf8/utf16` | ✅ есть (полные таблицы Unicode 15 из Go: `Is*`, `ToUpper/ToLower/ToTitle`, `SimpleFold`; utf8 `RuneLen/EncodeRune/AppendRune/DecodeRune*/Valid*/RuneCount*/FullRune`; utf16 `Encode/Decode/EncodeRune/DecodeRune`); нет нормализации NFC/NFD | генератор таблиц `scripts/gen-unicode-tables.py` |
| `strings` | `std.strings` | ✅ есть | `Builder`, `Runes`, `Reverse`, `Pad*`, `Title` |
| `bytes` | `std.bytes` | ✅ есть (`Buffer` + функции `strings` над `[]byte`) | `Buffer` реализует `io.Reader/Writer` |
| `strconv` | `std.strconv` | ✅ есть (`ParseInt/ParseUint` с базами и `_`, `*Bits`, `ParseFloat`, `ParseBool`, `Format*`, `Itoa/Atoi`, `Quote/QuoteToASCII/QuoteRune/Unquote`, `Parse<T>`/`Format<T>`) | generic `Parse<T>`/`Format<T>`, `errors.Is(err, strconv.ErrSyntax)` |
| `slices` | `std.slices` | ✅ есть (`Sort/SortBy/SortFunc` стабильные, `BinarySearch*`, `Index*/Contains*`, `Map/Filter/Reduce`, `Unique/Compact`, `Chunk/Window/Zip/Flatten/GroupBy`, `Min/Max/Sum`, `Equal/Compare`, `Insert/Delete`, `Fill/Repeat/Shuffle`) | `Map`, `Filter`, `Reduce`, `Chunk`, `Window`, `Unique`, `Zip` generic |
| `maps` | `std.maps` | ✅ есть (`Keys`, `Values`, `SortedKeys/Values`, `Merge`, `Copy`, `Invert/InvertAll`, `Filter`, `DeleteFunc`, `MapValues`, `Equal*`, `Clone`) | `SortedKeys`, `Invert` |
| `sort` | `std.slices.Sort*` | ✅ есть | generic, стабильная сортировка по умолчанию, `SortBy(key)` |
| `container/list`, `ring`, `heap` | `std.collections` (`List`, `Map`, `Set`, `Queue`, `Stack`) + `LinkedList`, `Ring`, `Heap<T>`, `PriorityQueue<T>`, `Deque<T>`, `OrderedMap`, `LRU`, `BitSet` | ✅ есть (новые типы обходятся через `Items()`/`Keys()`) | generic, без `any` |
| `iter` | `for x in` + `iter.Seq<T>` интерфейс | ❌ нет | ленивые генераторы через async? — нет; итератор-интерфейс |
| `hash`, `hash/fnv`, `crc32`, `crc64`, `adler32`, `maphash` | `std.hash` (+ `.fnv`, `.crc32`, `.crc64`, `.adler32`, `.maphash`) | ✅ есть (интерфейсы `Hasher/Hash32/Hash64`, `hash.Combine`; maphash — SipHash-1-3 на C) | — |
| `encoding/binary` | `std.encoding.binary` | ✅ есть (`BigEndian/LittleEndian.*`, varint, `Encode/Decode/Read/Write<T>` через compile-time рефлексию, `@binary(endian:)`) | `Read<T>`/`Write<T>` структур через compile-time рефлексию, `@binary(endian)` |
| `time` (типы) | `std.time` | ✅ есть (`Duration`, `Time` с раскладками Go и strftime, `ParseTime`, `Location` из tzdata + POSIX-правила, `Instant`, `Ticker/Timer` async); имена `Now/Since/Sleep/Parse` заняты старыми int-функциями: `Time.Now()`, `Elapsed`, `SleepFor`, `ParseTime` | типы `Time`, `Duration`, `Instant` монотонный |
| `unsafe` | `std.unsafe` (только FFI) | ❌ нет | минимальный, помечает capability |
| `weak`, `unique` | `weak *T` в языке; `std.intern` | ✅ есть (weak) / нет | язык |

## Уровень 1 — базовый ввод-вывод, ОС, синхронизация

| Go | Slang | Статус | Лучше чем в Go |
|---|---|---|---|
| `io` | `std.io` (`Reader`, `Writer`, `Closer`, `Seeker`, `Copy`, `ReadAllFrom`, `LimitReader`, `MultiWriter`, `Pipe`, `TeeReader`) | ✅ есть (+ `AsyncReader/AsyncWriter`, их реализует `net.Conn`; `os.File`) | интерфейсы async-совместимые (`AsyncReader`); нет `SendFile` в `Copy` |
| `bufio` | `std.io.Buffered*`, `Scanner` | ✅ есть (`ReadLine/ReadString/ReadBytes/Peek/ReadRune`, `Flush`, `Scanner` с `ScanLines/Words/Runes/Bytes`, `io.Lines`/`Words`) | `Lines()`/`Words()` (срезами, итераторов нет) |
| `fmt` | `std.fmt` | ✅ есть (Format, Sprintf) | `{0}`-шаблоны проверяются при компиляции, `Stringer` интерфейс |
| `os` | `std.os` | ✅ есть (args, env, exit, cwd, hostname…) | константы платформы, `OnInterrupt` |
| `os/signal` | `std.os.signal` | ✅ есть (`Wait`, `WaitAny`, `Notify` → `Channel<int>`, `Catch`, `Ignore`, `Reset`, `Raise`; self-pipe + поток) | async `await signal.Wait(SIGTERM)` |
| `os/exec` | `std.process` | ✅ есть (`Run/RunWith/Output`, `Start` с пайпами `AsyncReader/AsyncWriter`, `Wait/Kill/Signal`, env/dir, `Timeout` и отмена задачи, `LookPath`; pidfd) | async `Run`, пайпы как `Reader/Writer`, таймауты через контекст |
| `os/user` | `std.os.user` | ✅ есть (`Current`, `Lookup`, `LookupId`, `LookupGroup(Id)`) | — |
| `io/fs`, `path`, `path/filepath` | `std.fs` (+ `fs.Path` функции) | ✅ есть | `Walk` async, glob, `Watch` (inotify/FSEvents) |
| `embed` | `@embed("file")` атрибут | ✅ есть (`string`, `[]byte`, `const`, `embed.FS` из каталога или `dir/*.ext`) | проверяется при компиляции, `embed.FS` |
| `flag` | `std.cli` | ✅ есть (подкоманды, флаги из полей структуры `@cli.flag(...)`, env fallback, help, «did you mean», completion bash/zsh/fish) | подкоманды, help из атрибутов, env fallback |
| `sync`, `sync/atomic` | `std.task` (`Mutex`, `WaitGroup`) + `std.sync` (`RwMutex`, `Semaphore`, `Barrier`, `Once`, `AtomicInt/AtomicBool`, `Atomic<T>`, `Pool<T>`) | ✅ есть | async-aware мьютексы, generic атомики |
| `context` | контекст задачи в `std.task` | ✅ есть | неявная передача, типизированные ключи, отмена дерева |
| `math/rand`, `crypto/rand` | `std.random`, `std.crypto.rand` | ✅ есть | — |
| `crypto/*` примитивы | `std.crypto.{hash,hmac,aes,chacha,x25519,ecdsa,ed25519,rsa,hkdf,kdf,password}` | 🟡 есть hash, hmac, aes-gcm, chacha, x25519, p256, ed25519, rsa, hkdf; нет kdf (argon2/pbkdf2), password | `password.Hash/Verify` на Argon2 из коробки |
| `encoding/base64`, `base32`, `hex`, `ascii85`, `pem`, `asn1` | `std.encoding.*` | 🟡 есть base64, hex, base32, pem (с заголовками), asn1 (DER reader/Builder поверх ридера x509); нет ascii85 | — |
| `runtime` | `std.runtime` (`NumCPU`, `Version`, `GC` → нет) | 🟡 частично (`diag.Runtime()`) | — |
| `testing` | `std.testing` + `slang test` | ✅ есть (`@test` с `*testing.T`/без параметров, бенчмарки `@test func B(b *testing.B)`, `t.Run`, `Fatal/Error/Skip/Log`, assert-функции, `--filter/--json/--bench/-v/--parallel`, тест в отдельном процессе) | `@test`, табличные тесты, assert; нет snapshot |
| `log` | `std.log` | ✅ есть | уровень Serilog: шаблоны при компиляции, sinks, enrichers |
| `log/slog` | то же | ✅ есть | — |
| `log/syslog` | `log.WriteTo.Syslog` | ❌ нет | — |
| `expvar` | `std.diag` метрики | ✅ есть | Prometheus/OTLP |
| `runtime/pprof`, `runtime/trace`, `runtime/debug` | `std.diag` | 🟡 частично (cpu, heap) | pprof-совместимо, спаны OTel |

## Уровень 2 — форматы, текст, сжатие, сеть (ядро)

| Go | Slang | Статус | Лучше чем в Go |
|---|---|---|---|
| `encoding/json` | `std.encoding.json` | ✅ есть (Parse<T>/Format/Value) | compile-time, пути в ошибках, атрибуты |
| `encoding/xml` | `std.encoding.xml` | ✅ есть (pull-`Reader` с пространствами имён, дерево `Value`, `Writer`, `Parse<T>/Decode<T>/Format<T>` с `@xml(name, attr, text, ns, omitEmpty, skip)`; C-токенизатор; DTD не раскрывается) | та же модель, pull-reader, атрибуты вместо тегов |
| — (yaml, toml в Go нет) | `std.encoding.yaml`, `toml` | ✅ есть: yaml (YAML 1.2 core: блоки, flow, якоря/алиасы с лимитом, merge `<<`, теги, многодокументность; `Value`, `Parse<T>/Format<T>` с `@yaml`; сверено с PyYAML), toml (TOML 1.0 полностью, даты, `Parse<T>/Format<T>` с `@toml`; `Document` правит текст с сохранением комментариев и порядка; сверено с tomllib) | в стандартной библиотеке, round-trip с комментариями (toml `Document`; в yaml комментарии не сохраняются) |
| `encoding/csv` | `std.encoding.csv` | ✅ есть (`Reader`/`Writer` как в Go, `LazyQuotes`, `FieldsPerRecord`, `Read<T>/ReadNoHeader<T>/Write<T>` с `@csv(name, index, skip, omitEmpty)`; сверено с Go) | `Read<T>`/`Write<T>` в структуры |
| `encoding/gob` | `std.encoding.binary` схема-типизированный | ❌ нет | — |
| `regexp`, `regexp/syntax` | `std.regex` | ✅ есть (синтаксис RE2, Unicode-классы и скрипты, ленивый DFA + Pike VM, линейное время, без ReDoS; `Find/FindAll/Iter/Groups/Replace/ReplaceFunc/Split`; `regex.Pattern("...")` проверяется при компиляции; сверено с Go на 336 случаях) | литералы проверяются при компиляции, именованные группы, `Iter`; в 7 раз быстрее Go на `FindAll` |
| `text/template`, `html/template` | `std.text.template`, `std.html.template` | ✅ есть (`Parse<T>("литерал")` — поля, функции и `{{end}}` проверяются при компиляции; pipelines, `range/with/if/define/template/block`, встроенные функции Go; html — контекстное автоэкранирование HTML/атрибутов/URL/JS/CSS, `HTML/URL/JS` безопасные типы; сверено с Go) | типизированные шаблоны (проверка полей при компиляции), автоэкранирование |
| `text/scanner`, `text/tabwriter` | `std.text.scanner`, `std.text.table` | ✅ есть (scanner — токены и сообщения об ошибках как в Go, сверено с Go; table — стили Plain/ASCII/Unicode/Markdown, выравнивание, ширина CJK/комбинирующих символов, многострочные ячейки) | таблицы с рамками и Markdown вместо `tabwriter` |
| `html` | `std.html` (escape/unescape) | ✅ есть (`EscapeString/UnescapeString`, все 2231 сущности HTML5; сверено с Go) | — |
| `mime`, `mime/multipart`, `mime/quotedprintable` | `std.mime` | ✅ есть (`TypeByExtension/ExtensionsByType`, `ParseMediaType` с RFC 2231, `EncodeWord/DecodeHeader`; `std.mime.multipart` — `Reader/Writer`, `ReadForm`; `std.mime.quotedprintable`; сверено с Go) | — |
| `compress/gzip`, `zlib`, `flate`, `bzip2`, `lzw` + zstd, brotli | `std.compress.{gzip,zlib,deflate,zstd,brotli}` | 🟡 есть deflate/zlib/gzip (кодер и декодер на C, уровни 0–9, потоковые `Writer/Reader` по кускам `[]byte` и адаптеры `std.io`: `gzip.NewStreamReader/NewStreamWriter`), декодер zstd (RFC 8878, XXH64); нет кодера zstd, brotli, bzip2, lzw | zstd из коробки; gzip быстрее Go в 1.4 раза (сжатие) и в 3 раза (распаковка) |
| `archive/zip`, `archive/tar` | `std.archive.{zip,tar}` | ✅ есть (zip: чтение/запись, Zip64, deflate/store, расширенные метки времени, `ExtractTo` с защитой от zip-slip, `FromDir`; tar: ustar/pax/GNU, длинные имена, base-256, ссылки; совместимо с zip(1)/unzip/tar) | защита от zip-slip по умолчанию |
| `net` (TCP/UDP/Unix/DNS/IP) | `std.net` | ✅ есть; в работе (закрытие пробелов) | async без потока на соединение, типизированные ошибки, Happy Eyeballs |
| `net/netip` | `net.IP`, `net.Addr` | 🔧 в работе | — |
| `net/url` | `std.net.url` | ✅ есть (`Parse/ParseRequestURI`, `URL` со всеми полями Go, `ResolveReference`, `JoinPath`, `Values`, `ParseQuery`, `QueryEscape/PathEscape`, IPv6 и зоны; сверено с Go). `http.ParseURL` пока отдельный | — |
| `net/textproto` | `std.net.textproto` | ✅ есть (`Header` с каноническими ключами, `ReadHeader` с продолжениями строк, `DotEncode/DotDecode`, `ReadResponse` многострочных ответов; на строках, без `Reader`) | — |
| `image`, `image/png`, `jpeg`, `gif`, `color`, `draw` | `std.image` | ❌ нет | + webp, avif декодеры позже |
| `index/suffixarray` | `std.text.suffixarray` | ⛔ не нужно (пакет) | — |
| `database/sql`, `sql/driver` | `std.database.sql` | ❌ нет | async, `Query<T>` в структуры, пул |
| `go/*` (ast, parser, types…) | `std.slang.{ast,parser,types}` — экспорт компилятора как библиотеки | 🟡 частично (внутри компилятора) | — |
| `debug/elf`, `dwarf`, `pe`, `macho` | `std.debug.*` | ❌ нет (для `diag` частично) | — |
| `plugin` | `std.plugin` (dlopen) | ❌ нет | capability-помечен; используется `std.debug` |
| `runtime/debug` (частично), — | `std.debug` — отладочный агент на Slang (DAP, DWARF, точки, задачи) | ❌ нет | см. `docs/DEBUGGER.md` |

## Уровень 3 — протоколы и сервисы

| Go | Slang | Статус | Лучше чем в Go |
|---|---|---|---|
| `crypto/tls` | `std.net.tls` | ✅ есть (1.3 + 1.2 ECDHE/AEAD, interop с OpenSSL) | только современные наборы, без C-зависимостей |
| `crypto/x509`, `x509/pkix` | `std.crypto.x509` | ✅ есть (без OCSP/CRL, без P-384) | — |
| `net/http` | `std.net.http` | ✅ есть (1.1 клиент/сервер, роутер); нет (пул, HTTP/2, HTTP/3) | async, роутер с параметрами в stdlib, graceful shutdown |
| `net/http/httputil`, `httptest`, `cookiejar`, `fcgi`, `cgi`, `pprof` | `std.net.http.{util,test,cookies}`, `diag` endpoints | ❌ нет | — |
| — (websocket в Go нет) | `std.net.ws` | ❌ нет | в stdlib |
| `net/mail`, `net/smtp` | `std.net.mail` | ❌ нет | — |
| `net/rpc`, `rpc/jsonrpc` | `slang.grpc`, `std.net.jsonrpc` | ❌ нет | gRPC вместо net/rpc |
| `testing/fstest`, `iotest`, `quick` | `std.testing.*` | ❌ нет | property-based `quick` с generics |
| `syscall` | `std.sys` (слой платформы) | 🟡 частично (внутри рантайма) | единый API на всех ОС |
| `time` (таймеры, тикеры) | `task.Delay`, `time.NewTicker/NewTimer/After` | ✅ есть | async (`await t.Tick()`) |

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
