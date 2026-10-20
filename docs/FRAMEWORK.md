# Slang Framework: веб, данные, логирование, диагностика

Цель: из коробки давать уровень ASP.NET Core + EF Core + Serilog + pprof,
но проще, быстрее и без магии рантайма. Всё строится на четырёх свойствах
языка: generics (мономорфизация), compile-time рефлексия
(`reflect.Fields<T>()`), типизированные атрибуты и `async/await`.
Там, где .NET использует JIT и Reflection.Emit, Slang генерирует код при
компиляции; там, где Go использует строковые теги и `interface{}`, Slang
использует атрибуты и generics.

Разделение: стандартная библиотека `std.*` содержит базовые протоколы и
форматы; фреймворк `slang.web`, `slang.data`, `slang.log`, `slang.diag` —
отдельные пакеты в реестре с той же командой разработки.

## 1. Форматы: `std.encoding.{json,xml,yaml,toml}`

Единая модель для всех четырёх форматов:

```slang
@json(name: "user")
struct User {
    @json(name: "id")                 Id       int
    @json(omitEmpty: true)            Email    string
    @json(skip: true)                 password string
    @xml(attr: true)                  Role     string
    @yaml(flow: true)                 Tags     []string
    CreatedAt time.Time               // форматируется как RFC 3339
}

var u = json.Parse<User>(text)?          // строгая типизация, ошибка с путём: "users[2].id: expected int"
var text = json.Format(u)
var pretty = json.Format(u, json.Options{Indent: 2})

var any = json.ParseValue(text)?         // динамическое дерево json.Value, когда схемы нет
switch any {
    case json.Object o: ...
    case json.Array a:  ...
}
```

- **Compile-time кодогенерация**: `json.Parse<T>` раскрывается через
  `reflect.Fields<T>()` в прямой код чтения полей без `reflect.Value` и
  boxing. Скорость уровня `serde_json`/`System.Text.Json` с source generators.
- **Потоковый режим**: `json.Reader`/`json.Writer` для больших документов
  и NDJSON, `xml.Reader` (pull-парсер, как XmlReader).
- **Общие атрибуты**: `@serde(name:, skip:, omitEmpty:, default:, flatten:)`
  работает для всех форматов, формат-специфичные (`@json`, `@xml(attr)`,
  `@yaml`, `@toml`) уточняют.
- **Пользовательские конвертеры**: интерфейс `json.Converter<T>` с
  `Read(r *json.Reader) T!` и `Write(w *json.Writer, v T)`; регистрация
  атрибутом `@json(converter: MoneyConverter)`.
- **Валидация при разборе**: `@validate(required: true, min: 1, max: 100,
  pattern: "...")` проверяется в том же проходе, ошибки собираются списком.
- **Схемы**: `json.Schema<T>()` генерирует JSON Schema из типа и атрибутов
  (нужно для OpenAPI ниже).
- **Enum**: сериализуется по имени варианта; `@json(value: 1)` для чисел;
  enum с полями — как tagged union (`{"kind": "Ok", "value": 1}`,
  настраивается `@json(tag: "type", content: "data")`).
- Полиморфизм через интерфейсы: `@json(discriminator: "type")` на интерфейсе
  и `@json(name: "circle")` на реализациях.
- TOML и YAML дополнительно: сохранение комментариев при round-trip
  (`toml.Document`), якоря YAML, многодокументные YAML-потоки.

## 2. HTTP-сервер: `std.net.http` и `slang.web`

Два уровня, как `Kestrel` + ASP.NET Core.

### `std.net.http` (низкий уровень)

- HTTP/1.1, HTTP/2 (h2 и h2c), HTTP/3 позже; TLS через `std.net.tls`.
- Полностью асинхронный: один запрос — одна задача на пуле потоков, без
  потока на соединение. Zero-copy чтение тела, streaming ответов,
  backpressure.
- `http.Server{Addr, Handler, ReadTimeout, ...}`, интерфейс
  `Handler { async func Serve(ctx *http.Context) void! }`.
- `http.Client` с пулом соединений, таймаутами, retry-политикой, HTTP/2,
  `http.Get<T>(url)` с автоматическим разбором JSON.
- WebSocket, Server-Sent Events, graceful shutdown, Unix-сокеты.

### `slang.web` (фреймворк)

```slang
namespace shop

import slang.web
import slang.log
import shop.services

@route("/api/users")
struct UsersController {
    users *services.UserService     // внедряется через DI
    log   log.Logger

    @get("{id}")
    async func Get(id int) web.Result<User>! {
        var u = await this.users.Find(id)?
        if u == nil { return web.NotFound() }
        return web.Ok(u)
    }

    @post("")
    @authorize(roles: ["admin"])
    async func Create(@body req CreateUser) web.Result<User>! {
        this.log.Info("creating user {Email}", req.Email)
        return web.Created(await this.users.Create(req)?)
    }
}

async func main() {
    var app = web.NewApp()
    app.Services.AddSingleton<services.UserService>()
    app.Services.AddScoped<db.Context>()
    app.UseLogging().UseCors().UseAuth(jwt.Options{...})
    app.MapControllers()          // находит @route через рефлексию при компиляции
    app.MapGet("/health", () => web.Ok("ok"))
    await app.Run(":8080")?
}
```

- **Маршрутизация**: атрибуты `@get/@post/@put/@delete/@patch("{param}")`,
  параметры из пути, query, заголовков, тела (`@body`, `@query`, `@header`),
  привязка типов и валидация через `@validate`. Таблица маршрутов строится
  **при компиляции** (атрибуты + compile-time рефлексия), а не сканированием
  сборок при старте, поэтому старт мгновенный и неверный маршрут — ошибка
  компиляции.
- **Minimal API**: `app.MapGet("/x/{id}", (id int, svc *Svc) => ...)` с
  выводом параметров из сигнатуры лямбды.
- **Middleware**: `app.Use(async (ctx, next) => { ...; await next(ctx)? })`,
  встроенные: логирование, CORS, сжатие, статические файлы, rate limiting,
  аутентификация (JWT, cookie, OAuth2/OIDC), авторизация по ролям и
  политикам, health checks, request id, трассировка.
- **DI-контейнер**: `AddSingleton/AddScoped/AddTransient<T>()`,
  конструктор или поля с типами резолвятся по типу. Граф зависимостей
  проверяется при компиляции там, где это возможно (неразрешимая
  зависимость в контроллере — ошибка компиляции, а не при старте).
- **Конфигурация**: `config.Bind<AppSettings>()` из `appsettings.{json,yaml,
  toml}`, переменных окружения и аргументов, с приоритетами и
  `@validate`; `IOptions<T>`-аналог: `config.Options<T>` с
  горячей перезагрузкой.
- **OpenAPI**: генерируется при компиляции из маршрутов и `json.Schema<T>`,
  `/swagger` из коробки.
- **Результаты**: `web.Result<T>` — статус, заголовки, тело; `web.Ok`,
  `Created`, `NoContent`, `BadRequest`, `NotFound`, `Problem` (RFC 7807).
  Ошибка `T!` из обработчика автоматически превращается в Problem Details
  с трассой ошибки в режиме разработки.
- **Фильтры и модель-биндинг**: `@filter(Auth)`, кастомные биндеры.
- **Тестирование**: `web.TestServer(app)` — запросы в памяти без сокетов.
- **gRPC** (`slang.grpc`) и **GraphQL** — отдельными пакетами на той же базе.

## 3. Данные: `std.database.sql` и `slang.data` (ORM уровня EF Core)

### `std.database.sql`

- Единый интерфейс `sql.DB`, `Conn`, `Tx`, `Rows`; асинхронный; пул
  соединений; драйверы PostgreSQL, MySQL, SQLite (нативные, на Slang),
  SQL Server и ClickHouse позже.
- Типизированные запросы: `db.Query<User>("select ... where id = $1", id)`
  маппит строки в структуры по именам столбцов и `@column`.

### `slang.data` (ORM)

```slang
@table("users")
struct User {
    @key @autoincrement Id      int
    @column("email") @index(unique: true) Email string
    @maxLength(100)             Name    string
    @hasMany(foreignKey: "UserId") Orders  []Order
    @default("now()")           CreatedAt time.Time
}

struct ShopContext {
    data.Context
    Users  data.Set<User>
    Orders data.Set<Order>

    func Configure(m *data.ModelBuilder) {
        m.Entity<User>().HasIndex((u) => u.Email).IsUnique()
    }
}

var users = await ctx.Users
    .Where((u) => u.CreatedAt > since && u.Name.Contains("a"))
    .Include((u) => u.Orders)
    .OrderBy((u) => u.Email)
    .Skip(20).Take(10)
    .ToList()?

var u = await ctx.Users.Find(id)?
u.Name = "new"
await ctx.SaveChanges()?        // change tracking: UPDATE только изменённых полей
```

- **LINQ-подобные запросы с переводом в SQL при компиляции.** Лямбда в
  `Where` — не замыкание, а выражение, которое компилятор превращает в
  дерево выражений (`data.Expr`) на этапе компиляции, когда параметр имеет
  тип `data.Set<T>`. Что нельзя перевести в SQL — ошибка компиляции, а не
  молчаливая загрузка всей таблицы в память.
- Change tracking, unit of work, транзакции, оптимистичная блокировка
  (`@concurrencyToken`), ленивые и явные загрузки, split queries,
  пакетные вставки, raw SQL с маппингом, compiled queries по умолчанию.
- **Миграции**: `slang data migration add AddUsers` сравнивает модель со
  снимком и генерирует миграцию на Slang; `slang data update` применяет;
  миграции — обычный код в репозитории.
- Провайдеры: PostgreSQL, MySQL, SQLite; шаблон провайдера для остальных.
- Лёгкая альтернатива в том же пакете: `data.Dapper`-стиль
  (`db.Query<T>(sql, args)`) без трекинга.
- NoSQL: `slang.data.redis`, `slang.data.mongo` отдельными пакетами.

## 4. Логирование: `slang.log` уровня Serilog

```slang
var logger = log.New()
    .MinimumLevel(log.Debug)
    .MinimumLevelOverride("slang.web", log.Warning)
    .Enrich.WithProperty("App", "shop")
    .Enrich.WithMachineName()
    .Enrich.WithRequestId()
    .Enrich.FromContext()
    .WriteTo.Console(log.ConsoleOptions{Template: "{Timestamp:HH:mm:ss} [{Level:u3}] {Message}{NewLine}{Exception}"})
    .WriteTo.File("logs/app-.log", log.FileOptions{RollingInterval: log.Day, Retain: 7})
    .WriteTo.Seq("http://seq:5341")
    .WriteTo.OpenTelemetry()
    .Filter.ByExcluding((e) => e.Properties["Path"] == "/health")
    .CreateLogger()

logger.Info("user {UserId} bought {@Order} for {Amount:0.00}", id, order, total)
logger.Error(err, "payment failed for {UserId}", id)

using (log.Context.Push("RequestId", reqId)) { ... }   // или через async-контекст
```

- **Структурированные события, не строки**: шаблон сообщения разбирается
  **при компиляции** (это константа), имена свойств и их значения
  становятся полями события. `{@Order}` — сериализовать объект целиком,
  `{Amount:0.00}` — формат. Несовпадение числа плейсхолдеров и аргументов —
  ошибка компиляции.
- **Нулевая стоимость для отключённых уровней**: проверка уровня до
  вычисления аргументов, без аллокаций.
- **Sinks** (приёмники): Console (цветной, шаблон), File (ролл по времени и
  размеру, retention), Seq, Elasticsearch/OpenSearch, Loki, Splunk, Syslog,
  OpenTelemetry (OTLP), Email, Slack/Telegram webhook, любой через
  интерфейс `log.Sink` с батчингом и асинхронной очередью
  (`WriteTo.Async(...)`), пользовательские форматтеры (JSON, Compact JSON,
  Logfmt, текстовый шаблон).
- **Enrichers** (обогатители): свойство, машина, процесс, поток, задача,
  request id, trace/span id из `slang.diag`, окружение, версия, `FromContext`
  для `log.Context.Push` (контекст живёт в async-задаче, не в потоке).
- **Фильтры**, уровни по источникам, динамическое изменение уровня в
  рантайме, `log.Logger` как интерфейс для DI с категорией `log.For<T>()`.
- **Destructuring**: `{@Obj}` через compile-time рефлексию с учётом
  `@log(skip: true)` для секретов, политики для типов
  (`Destructure.ByTransforming<User>((u) => {Id: u.Id})`).
- Интеграция: `slang.web` пишет запросы одной строкой на запрос
  (как Serilog RequestLogging), `slang.data` логирует SQL с параметрами.

## 5. Диагностика: `slang.diag` (аналог pprof и dotnet-counters)

- **Профилирование CPU**: сэмплирующий профилировщик в рантайме
  (сигнал/таймер, стек через frame pointers, без остановки мира):
  `diag.StartCPUProfile(w)`, HTTP-эндпоинт `/debug/pprof/profile` в
  формате pprof (`profile.proto`), совместим с `go tool pprof`, `pprof`
  (Google) и Speedscope; `slang tool pprof` — своя обёртка с flame graph.
- **Память**: подсчёт ссылок даёт точные данные без снимков GC: живые
  объекты по типу и месту аллокации (`/debug/pprof/heap`), аллокации с
  сэмплированием (`allocs`), детектор утечек (`--leak-check`) и
  циклов.
- **Задачи**: `/debug/pprof/tasks` — все живые async-задачи с их трассами
  ожидания (аналог goroutine dump), блокировки (`mutex`, `block`).
- **Трассировка выполнения**: `diag.Trace` — события планировщика,
  ввода-вывода, GC-подобных пауз нет, но есть паузы на синхронизацию;
  просмотр в Perfetto UI (формат Chrome trace).
- **Метрики**: `diag.Counter/Gauge/Histogram`, экспорт Prometheus
  (`/metrics`) и OpenTelemetry; встроенные метрики рантайма (память,
  задачи, очередь планировщика, HTTP, SQL).
- **Распределённая трассировка**: OpenTelemetry-совместимые `Span`,
  распространение контекста через async-задачи, W3C traceparent в HTTP и
  SQL автоматически.
- **CLI**: `slang tool pprof`, `slang tool trace`, `slang tool counters
  <pid>` (как dotnet-counters: живые метрики процесса), `slang tool dump
  <pid>` (снимок задач и памяти), `slang tool stack <pid>`. Подключение к
  запущенному процессу через Unix-сокет диагностики, включается
  `SLANG_DIAG=1` или `diag.Enable()`.
- **Бенчмарки**: `@bench` функции, `slang bench` с статистикой
  (среднее, p50/p99, аллокации), сравнение с базой (`--compare`).
- **Отладка**: DWARF по умолчанию, `slang tool debug` поверх lldb с
  форматтерами типов Slang (см. docs/TOOLING.md).

## 6. Прочие «плюшки» уровня .NET

- `slang.config` — конфигурация с провайдерами (файлы, env, args, Vault,
  Consul), горячая перезагрузка.
- `slang.cache` — in-memory и distributed (Redis) cache с единым API.
- `slang.jobs` — фоновые задачи, cron, очереди (аналог Hangfire).
- `slang.validation` — `@validate` плюс FluentValidation-подобный API.
- `slang.mapper` — маппинг DTO через compile-time рефлексию, без
  рефлексии в рантайме (аналог Mapster/AutoMapper, но проверяется при
  компиляции).
- `slang.mediator` — CQRS, пайплайны обработчиков.
- `slang.test` — `@test`, `@fact`/`@theory` с данными, моки через
  интерфейсы (`mock.Of<UserRepo>()` генерируется при компиляции),
  snapshot-тесты, `web.TestServer`.
- `slang.cli` — парсер команд и аргументов с генерацией help из атрибутов.
- `slang.auth` — JWT, OIDC, API keys, политики.
- `slang.realtime` — SignalR-подобные хабы поверх WebSocket.
- `slang.messaging` — Kafka, RabbitMQ, NATS клиенты и абстракция шины.
- Шаблоны проектов: `slang new web-api`, `slang new worker`, `slang new lib`.

## Что для этого нужно от языка (требования к компилятору)

| Возможность | Кто использует |
|---|---|
| Generics с мономорфизацией и ограничениями через интерфейсы | collections, `data.Set<T>`, `json.Parse<T>`, DI |
| Compile-time рефлексия `reflect.Fields<T>()`, `Methods<T>()`, атрибуты как значения | json/xml/yaml/toml, маршруты, ORM, логгер, mapper, mock |
| Compile-time вычисление константных строк (разбор шаблона лога, маршрута, SQL) | log, web, data |
| Лямбда как дерево выражений при параметре типа `data.Expr<func(T) bool>` | ORM-запросы |
| Async-контекст (аналог AsyncLocal) в рантайме | log.Context, трассировка, DI scope |
| Статические таблицы типов и урезание неиспользуемых | рефлексия в рантайме для `json.ParseValue`, OpenAPI |
| Frame pointers и DWARF по умолчанию, таймер-сэмплирование в рантайме | pprof |
| Async-сокеты и таймеры в планировщике | http, sql, messaging |

## Порядок реализации

1. Generics, compile-time рефлексия, атрибуты как значения (компилятор).
2. `std.encoding.json` (+ Value, Reader/Writer), `std.net` async-сокеты,
   `std.net.http` 1.1, `slang.log` с Console/File/JSON.
3. `slang.web`: маршруты, биндинг, middleware, DI, конфигурация, OpenAPI.
4. `std.database.sql` + PostgreSQL/SQLite, `slang.data` базовый
   (запросы, трекинг, миграции).
5. `slang.diag`: CPU/heap/tasks профили в формате pprof, метрики,
   OpenTelemetry; `slang tool pprof/trace/counters`.
6. xml/yaml/toml, HTTP/2, TLS, auth, cache, jobs, messaging, остальное.
