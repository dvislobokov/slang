# Slang — дизайн языка

Slang — компилируемый язык с простой структурой в духе Go, но без шумных
`if err != nil`, с выводом типов через `var`, пространствами имён вместо
пакетов, методами внутри структур и встроенным `async/await`.
Компилируется в нативный код через LLVM.

## Цели

- **Простота Go**: маленький набор конструкций, явный поток управления,
  структуры вместо классов, указатели без арифметики.
- **Меньше шума**: обработка ошибок через `!`, `?` и `try` вместо повторяющихся
  `if err != nil { return err }`.
- **Вывод типов**: `var x = 123` как в C#, без `:=`.
- **Пространства имён**: полный путь `app.services.users.UserService`, как
  namespace в C#, вместо коротких имён пакетов.
- **Методы внутри структур**: тело структуры содержит и поля, и методы.
- **Асинхронность**: `async`/`await` как языковая конструкция.
- **LLVM**: один бэкенд, нативные бинарники, оптимизации бесплатно.

## Не цели

- Наследование классов, generics на первом этапе (планируются позже).
- Исключения. Ошибки — обычные значения.
- Макросы и метапрограммирование.
- Сборщик мусора. Память управляется подсчётом ссылок с оптимизациями
  компилятора (см. «Память»).

## Файлы и пространства имён

Один файл начинается с объявления пространства имён. Имя — полный путь,
точки разделяют уровни.

```slang
namespace app.services.users

import std.io
import app.models

struct UserService {
    // ...
}
```

### Правила импорта

`import X` делает содержимое пространства имён `X` доступным по коротким
именам. Чем глубже импорт, тем короче обращение:

| Импорт | Обращение |
|--------|-----------|
| нет импорта | `std.io.println("hi")` |
| `import std` | `io.println("hi")` |
| `import std.io` | `println("hi")` |
| `import std.io as out` | `out.println("hi")` |

- Полный путь работает всегда, импорт только сокращает его.
- Конфликт коротких имён из двух импортов — ошибка компиляции; её решают
  псевдонимом (`as`) или полным путём.
- Импортировать можно и отдельный тип: `import app.models.User`.
- Точка входа — функция `main`, указанная при сборке (по умолчанию ищется в
  корневом пространстве имён проекта).
- Видимость: имя с заглавной буквы экспортируется (как в Go). Строчная —
  приватно для пространства имён. Исключение — функции стандартной
  библиотеки вроде `println`, которые для краткости экспортируются со
  строчной буквы; правило для пользовательского кода остаётся строгим.

## Объявления переменных

Только `var`. Тип выводится из инициализатора. Явный тип пишется после имени
через пробел, как в Go.

```slang
var x = 123          // int
var name = "slang"   // string
var ratio = 0.5      // float
var list []int = []  // явный тип нужен, если из значения не выводится
var count int        // без инициализатора: нулевое значение
```

- `const` — константы времени компиляции: `const Pi = 3.14159`.
- Переменные нельзя объявить и не использовать (как в Go).

## Типы

| Категория | Типы |
|-----------|------|
| Целые | `int` (64 бит), `int8`, `int16`, `int32`, `int64`, `uint*`, `byte` |
| Дробные | `float` (64 бит), `float32` |
| Прочие примитивы | `bool`, `string`, `char` (Unicode code point) |
| Составные | `[]T` (срез), `[N]T` (массив), `map[K]V`, `*T` (указатель) |
| Пользовательские | `struct`, `interface`, `enum` |
| Специальные | `void`, `T!` (результат или ошибка), `Task<T>` (асинхронный результат) |

Строки — неизменяемые, UTF-8. Срезы и map — как в Go (ссылочная семантика
на общий буфер). Подсрез записывается диапазоном: `s[1..3]`, `s[..2]`, `s[2..]`.

## Функции

Параметр — `имя тип`. Возвращаемый тип — после закрывающей скобки, без
двоеточия. `void` опускается.

```slang
func Add(a int, b int) int { return a + b }
func Add(a, b int) int { return a + b }       // группировка как в Go
func Swap(a, b int) (int, int) { return b, a }
func Log(msg string) { println(msg) }

var (x, y) = Swap(1, 2)
```

- Несколько возвращаемых значений — кортеж.
- Замыкания: `var f = func(x int) int { return x * 2 }`, короткая форма
  `var f = (x int) => x * 2`.
- Функции — значения первого класса, тип `func(int) int`.

## Структуры и методы

Методы объявляются внутри структуры. Приёмник неявный — `this`. Метод
по умолчанию работает с указателем на структуру, как `func (s *S)` в Go.

```slang
struct Point {
    X float
    Y float

    func Length() float {
        return math.Sqrt(this.X * this.X + this.Y * this.Y)
    }

    func Move(dx, dy float) {
        this.X += dx
        this.Y += dy
    }

    static func New(x, y float) Point {
        return Point{X: x, Y: y}
    }
}

var p = Point.New(1, 2)
var q = Point{X: 3, Y: 4}
```

- Конструкторы: литерал `Point{X: 1, Y: 2}` (в литерале двоеточие остаётся,
  как в Go) или `static func`.
- Поля с заглавной буквы — публичные, со строчной — приватные.
- Встраивание (композиция) как в Go: поле без имени `Base` поднимает его
  поля и методы.
- Свободные `func` на уровне пространства имён допустимы для утилит и `main`.

## Атрибуты

Атрибуты — метаданные на объявлениях, доступные через рефлексию и
компилятору. Пишутся через `@` перед объявлением, пространства имён,
структур, полей, методов, функций, вариантов enum и параметров.

```slang
@json(name: "user_id")
struct User {
    @json(name: "id")       Id    int
    @json(omitEmpty: true)  Email string
    @json(skip: true)       cache string

    @deprecated("use Save")
    func Store() void! { ... }
}
```

- Атрибут — это обычная структура, помеченная `@attribute`; её поля —
  параметры атрибута. Аргументы — константы времени компиляции.
- Встроенные: `@reflect` (принудительно сохранить метаданные типа),
  `@deprecated`, `@inline`, `@test`, `@attribute`.
- Библиотечные: `@json`, `@xml`, `@db(column: "...")`, `@validate(...)`.
- Неизвестный атрибут — ошибка компиляции (в отличие от строковых тегов Go).

### Свои атрибуты

Атрибут объявляется как структура с `@attribute`. Поля — параметры,
значения по умолчанию допускаются.

```slang
namespace app.validation

import std.reflect

@attribute(targets: [reflect.Target.Field])
struct range {
    min int
    max int
    message string = "value out of range"
}

@attribute(targets: [reflect.Target.Struct, reflect.Target.Func], repeatable: true)
struct tag {
    name string
}
```

```slang
@tag("entity")
@tag("audited")
struct User {
    @range(min: 0, max: 150)  Age  int
    @range(1, 64)             Name string   // позиционно, по порядку полей
}
```

- Аргументы передаются по имени или позиционно; поля со значением по
  умолчанию можно опустить. Один параметр — короткая форма `@deprecated("why")`.
- `targets` ограничивает места применения: `Namespace`, `Struct`,
  `Interface`, `Enum`, `Variant`, `Field`, `Func`, `Param`. Без указания —
  везде. Нарушение — ошибка компиляции.
- `repeatable: true` разрешает несколько одинаковых атрибутов на одном
  объявлении; иначе повтор — ошибка.
- Допустимые типы параметров: примитивы, `string`, enum, срезы из них и
  другие атрибут-структуры. Значения вычисляются при компиляции.
- Атрибут — только данные. Он не выполняет код при компиляции и не меняет
  генерируемый код; поведение живёт в библиотеке, которая его читает
  (через `f.Attribute<range>()`, `f.Attributes<tag>()` для повторяемых, или
  compile-time `reflect.Fields<T>()`, см. раздел «Рефлексия»).

## Интерфейсы

Структурная типизация, как в Go: структура реализует интерфейс, если имеет
все его методы. Явного `implements` нет.

```slang
interface Shape {
    func Area() float
}

struct Circle {
    R float
    func Area() float { return 3.14159 * this.R * this.R }
}

func Describe(s Shape) {
    println(s.Area())
}
```

## Перечисления

```slang
enum Color { Red, Green, Blue }

enum Result {
    Ok(value int)
    Fail(code int)
}

var c = Color.Red
switch r {
    case Result.Ok(v):   println(v)
    case Result.Fail(c): println("fail", c)
}
```

## Указатели

Как в Go: `&x` берёт адрес, `*p` разыменовывает, доступ к полям через
указатель автоматически (`p.X`). Арифметики указателей нет. Нулевое
значение — `nil`.

```slang
var p = Point{X: 1, Y: 2}
var ptr = &p
ptr.X = 10
var copy = *ptr
```

Структуры — значения; передаются копированием, если не брать указатель.
Выделение в куче — `new Point{...}` возвращает `*Point`. Компилятор делает
escape analysis и переносит локальные значения в кучу, если взят адрес и он
покидает функцию.

## Ошибки

Ошибки — значения, как в Go. Исключений нет. Отличие от Go в том, что язык
даёт три коротких конструкции вместо `if err != nil`.

### 1. Объявление: тип `T!`

Функция, которая может завершиться ошибкой, добавляет `!` к возвращаемому
типу. `void!` — функция без результата, но с возможной ошибкой.

```slang
func ParseInt(s string) int! {
    if s == "" {
        return error("empty string")
    }
    ...
    return n
}

func Save(path string, data []byte) void! { ... }
```

`error(msg)` создаёт ошибку. `return` в такой функции принимает либо значение
`T`, либо ошибку; компилятор различает их по типу.

### 2. Проброс наверх: оператор `?`

`expr?` возвращает значение, если ошибки нет, и немедленно выходит из текущей
функции с этой ошибкой, если она есть. Это замена `if err != nil { return err }`.
Работает только внутри функции с типом `T!`.

```slang
func ReadConfig(path string) Config! {
    var text = io.ReadFile(path)?
    var cfg = ParseConfig(text)?
    return cfg
}
```

Чтобы добавить контекст при пробросе: `io.ReadFile(path)?("reading config")`
оборачивает ошибку сообщением.

### 3. Обработка на месте: `try`

```slang
// Обработать и выйти / продолжить
var cfg = try ReadConfig("app.toml") catch err {
    println("config:", err)
    return
}

// Значение по умолчанию
var port = try ParseInt(env.Get("PORT")) else 8080

// Проверка без значения
try Save("out.txt", data) catch err {
    println("save failed:", err)
}
```

- Блок `catch` обязан либо завершить функцию (`return`, `panic`), либо не
  использовать значение `try`. Так переменная после `try ... catch` всегда
  инициализирована.
- `try ... else` подставляет значение по умолчанию и отбрасывает ошибку.

### Блочная форма `try`

Когда нужно обработать ошибку для нескольких операций разом, `try` берёт
блок. Внутри него `?` переходит в `catch` этого блока, а не возвращает из
функции:

```slang
try {
    var text = io.ReadFile(path)?
    var cfg = ParseConfig(text)?
    Apply(cfg)?
} catch err {
    println("config:", err)
    return
}
```

- Ошибки внутри блока всё так же помечаются `?`; неявного проброса нет.
- Чтобы пробросить дальше, в `catch` пишут `return err` (в функции с `T!`).
- Переменные блока видны только в нём; результат выносят через переменную,
  объявленную до `try`.
- `catch` блочной формы не обязан завершать функцию: после него выполнение
  продолжается. `else` у блочной формы нет.
- Блоки вкладываются; `?` идёт в ближайший `catch`.
- Если из многострочного кода нужно значение, идиоматично вынести его в
  функцию с типом `T!` и использовать `?` или `try f() catch`.

### Игнорирование и обязательность

Значение типа `T!` нельзя использовать, не обработав: `var x = ParseInt(s)`
без `?` или `try` — ошибка компиляции. Явно отбросить результат и ошибку
можно через `_ = f()`.

### Тип ошибки

`error` — встроенный интерфейс:

```slang
interface error {
    func Message() string
}
```

Любая структура с методом `Message()` может быть ошибкой. Это позволяет
делать типизированные ошибки и разбирать их через `switch` по типу:

```slang
struct NotFound {
    Path string
    func Message() string { return "not found: " + this.Path }
}

func Open(path string) File! {
    if !fs.Exists(path) {
        return NotFound{Path: path}
    }
    ...
}

var f = try Open("a.txt") catch err {
    switch err {
        case NotFound e: println("no such file:", e.Path)
        default:         println("open failed:", err)
    }
    return
}
```

### Проверка через цепочку обёрток

Если ошибку обернули контекстом (`?("loading config")` или `errors.Wrap`),
исходная ошибка сохраняется внутри. Для проверки по цепочке служат
`errors.Is` и `errors.As`:

```slang
// Ошибки-значения объявляются как константы и сравниваются через Is
const ErrNotFound = error("not found")
const ErrTimeout  = error("timeout")

var cfg = try ReadConfig("app.toml") catch err {
    if errors.Is(err, fs.ErrNotFound) {
        cfg = DefaultConfig()           // нет файла: это нормально
    } else {
        return err                      // пробросить дальше
    }
}

// Достать типизированную ошибку из цепочки
var nf NotFound
if errors.As(err, &nf) {
    println(nf.Path)
}

// Несколько условий в одном месте
try Connect(addr) catch err {
    switch {
        case errors.Is(err, net.ErrTimeout): retry()
        case errors.Is(err, net.ErrRefused): return err
        default:                             panic(err)
    }
}
```

- `errors.Wrap(err, "context")` оборачивает, `errors.Unwrap(err)` достаёт
  исходную ошибку.
- `errors.Is(err, target)` — есть ли `target` в цепочке (сравнение по
  значению).
- `errors.As(err, &target)` — есть ли в цепочке ошибка типа `target`;
  если да, записывает её.
- `panic(msg)` — для невосстановимых ситуаций (баг, нарушение инварианта);
  не используется для ожидаемых ошибок.

### Как это выглядит вместе

```slang
func LoadUsers(path string) []User! {
    var text = io.ReadFile(path)?("loading users")
    var lines = strings.Split(text, "\n")
    var users []User = []
    for line in lines {
        var u = try ParseUser(line) else continue
        users = append(users, u)
    }
    return users
}
```

Три строки вместо примерно десяти в Go, при той же явности: по `?` и `try`
видно каждое место, где может произойти ошибка.

## Управление потоком

Как в Go, без скобок вокруг условий, с обязательными фигурными скобками.

```slang
if x > 0 {
    ...
} else if x < 0 {
    ...
} else {
    ...
}

for i in 0..10 { ... }          // диапазон [0, 10)
for item in list { ... }
for i, item in list { ... }
for cond { ... }                // как while
for { ... }                     // бесконечный
```

### switch

`switch` — единственная конструкция ветвления по значению. Без `fallthrough`,
без `break` в конце каждого `case`. Поддерживает значения, диапазоны,
несколько вариантов, деструктуризацию enum и проверку типа.

```slang
switch n {
    case 1, 2:       println("small")
    case 3..10:      println("medium")
    case n > 100:    println("huge")     // условие
    default:         println("other")
}

switch shape {
    case Circle c:    println("circle", c.R)
    case Rect r:      println("rect", r.W, r.H)
}

switch res {
    case Result.Ok(v):   println(v)
    case Result.Fail(c): println("fail", c)
}
```

- `break`, `continue`, `return`. Меток нет.
- `defer` — выполнить при выходе из функции, как в Go.

## Асинхронность

Модель — задачи (tasks) на пуле потоков, как в C#/Tokio, с
языковым `async`/`await`.

```slang
async func Fetch(url string) string! {
    var resp = await http.Get(url)?
    return await resp.Text()?
}

async func main() {
    var a = Fetch("https://a")      // Task<string!>, запущена сразу
    var b = Fetch("https://b")
    var (ra, rb) = await task.All(a, b)?
    println(ra, rb)
}
```

- `async func` возвращает `Task<T>`; вызов запускает задачу, `await`
  дожидается результата.
- `await` допустим только внутри `async`.
- `?` и `await` комбинируются: `await f()?`.
- Планировщик — часть рантайма: пул потоков по числу ядер, кооперативное
  переключение в точках `await`. Блокирующий ввод-вывод в стандартной
  библиотеке асинхронный.
- Примитивы: `task.All`, `task.Any`, `task.Delay`, `Channel<T>`, `Mutex`.
- Реализация в LLVM: через LLVM coroutines (`llvm.coro.*`), каждая `async`
  функция превращается в конечный автомат с выделенным фреймом.

## Рефлексия

Цель: удобнее, чем в Go (типизированные атрибуты, методы, enum,
создание значений, generics-дружественный API) и быстрее, чем в C#
(никакого JIT, никаких `MethodInfo.Invoke` через boxing: метаданные —
статические таблицы в бинарнике, доступ к полям по смещению, вызов
методов через указатель на функцию).

### Устройство

- Для каждого типа компилятор генерирует **статическую таблицу**
  `TypeInfo` в секции только для чтения: имя, полный путь, размер,
  выравнивание, вид (struct/enum/interface/...), поля (имя, тип, смещение,
  атрибуты), методы (имя, сигнатура, указатель на функцию), варианты enum,
  реализуемые интерфейсы, атрибуты типа. Таблицы ссылаются друг на друга
  указателями, поэтому обход графа типов — обычное чтение памяти без
  аллокаций и без поиска по строкам.
- Атрибуты хранятся как готовые значения своих структур, а не как строки:
  `field.Attribute<json>()` возвращает `*json` или `nil` за O(1) по
  количеству атрибутов поля.
- Интерфейсные значения и `*T` несут указатель на `TypeInfo`, поэтому
  `reflect.TypeOf(x)` — одно чтение.
- **Урезание**: таблицы генерируются только для типов, которые достижимы
  из `reflect.TypeOf`, `typeof`, сериализаторов и `@reflect`. Остальные
  типы не раздувают бинарник. Линковщик удаляет неиспользуемые таблицы.

### API (`std.reflect`)

```slang
var t = reflect.TypeOf<User>()         // статически, без значения
var t2 = typeof(u)                     // по значению, встроенная функция

t.Name                                 // "User"
t.FullName                             // "app.models.User"
t.Kind                                 // reflect.Kind.Struct
for f in t.Fields {
    println(f.Name, f.Type.Name, f.Offset)
    var tag = f.Attribute<json>()      // *json или nil
    if tag != nil && tag.skip { continue }
}

var v = reflect.ValueOf(&u)            // reflect.Value: {ptr, type}
v.Field("Email").Set("a@b.c")          // проверка типа во время выполнения
var email = v.Field("Email").Get<string>()

var m = t.Method("Store")              // *MethodInfo или nil
var res = m.Call(&u)?                  // вызов через указатель на функцию
var fresh = t.New()                    // новое нулевое значение в куче, *User как any

for variant in reflect.TypeOf<Color>().Variants { println(variant.Name) }
```

- `reflect.Value` — пара `{указатель, *TypeInfo}` на стеке, без boxing.
  Чтение поля — сложение смещения; запись проверяет совместимость типов.
- `Call` принимает аргументы как `[]any` только в универсальной форме;
  типизированная форма `m.Call<func(*User) void!>()` возвращает обычный
  указатель на функцию и вызывает его напрямую, без накладных расходов.
- `any` — пустой интерфейс (`interface {}`); значения в нём несут
  `TypeInfo`, поэтому `switch x { case User u: }` работает за одно сравнение
  указателей.

### Статическая рефлексия (нулевая стоимость)

Для сериализаторов и ORM обычная рефлексия всё же обходит таблицы во
время выполнения. Вторая половина механизма — **compile-time рефлексия**
в generic-коде: компилятор раскрывает обход полей на этапе компиляции,
когда тип известен (мономорфизация).

```slang
func ToJson<T>(value T) string {
    var b = strings.Builder{}
    b.Write("{")
    for f in reflect.Fields<T>() {       // разворачивается при компиляции
        var tag = f.Attribute<json>()    // константа
        if tag != nil && tag.skip { continue }
        var name = f.Name
        if tag != nil && tag.name != "" { name = tag.name }
        b.Write(fmt.Format("\"{0}\":", name))
        b.Write(ToJson(f.Get(value)))   // прямой доступ к полю, без reflect.Value
    }
    b.Write("}")
    return b.String()
}
```

`reflect.Fields<T>()` в generic-функции — не вызов, а инструкция
компилятору: цикл раскрывается в прямые обращения `value.Id`, `value.Email`
и т. д. Результат — скорость ручного кода (как у `serde` в Rust или
source generators в C#), без генерации исходников и без отдельного шага
сборки. Тот же `std.encoding.json` использует это и не падает в
медленный путь, если тип известен статически.

### Сравнение

| | Go | C# | Slang |
|---|---|---|---|
| Метаданные | runtime таблицы | metadata + JIT | статические таблицы, урезаемые |
| Теги полей | строки `json:"id"` | типизированные атрибуты | типизированные атрибуты, проверяются компилятором |
| Вызов метода | `Value.Call` с boxing | `MethodInfo.Invoke`, boxing, медленно | указатель на функцию, типизированный `Call<F>` |
| Enum | нет | есть | варианты с полями |
| Нулевая стоимость | нет | source generators (отдельный шаг) | `reflect.Fields<T>()` в generics |
| Создание типов в рантайме | нет | `Reflection.Emit` | нет (сознательно) |

## Память

Сборщика мусора нет и не планируется. Модель — подсчёт ссылок (ARC) с
оптимизациями компилятора, как в Swift и Koka (Perceus).

### Что где живёт

- Значения (структуры, массивы фиксированной длины, примитивы, кортежи) —
  на стеке, копируются при присваивании.
- `new T{}`, срезы, map, строки, замыкания, фреймы корутин, ошибки,
  значения в интерфейсах — в куче, со счётчиком ссылок в заголовке.
- `&local`: если адрес не покидает функцию, объект остаётся на стеке;
  если покидает (escape analysis), компилятор переносит его в кучу.

### Подсчёт ссылок без накладных расходов там, где он не нужен

Компилятор выводит владение статически и убирает большинство операций
со счётчиком:

- аргумент, который функция не сохраняет, передаётся как заимствованный —
  без `retain`/`release`;
- объект, не убегающий из функции, вообще не получает счётчика;
- при последнем использовании переменной объект передаётся дальше по
  владению, без копии и без инкремента;
- обновление на месте: если на срез или строку ровно одна ссылка,
  `append` и подобные операции не копируют буфер.

Счётчик реально меняется только там, где объект действительно разделяется
между несколькими владельцами. Операции атомарные, потому что задачи
выполняются на нескольких потоках.

### Циклы

Единственная слабость ARC — циклические ссылки. Решение:

- `weak *T` — слабая ссылка, не удерживает объект, становится `nil` после
  его освобождения. Используется для обратных ссылок (родитель ↔ ребёнок,
  наблюдатели, кэши).
- Детектор циклов в отладочной сборке (`slang run --leak-check`) находит
  утечки и указывает поле, которое стоит сделать `weak`.
- Циклы через замыкания и фреймы корутин — та же модель: захват по
  ссылке удерживает объект, `weak` разрывает.

### Арены

Для серверов, парсеров и других задач с чётким жизненным циклом есть
библиотечные арены: `var a = arena.New()`, `a.New<T>()`, освобождение всей
арены разом. Это дополнение к ARC, а не замена.

### Гарантии

- Детерминированное освобождение: деструктор (метод `Drop()`) вызывается
  сразу, когда счётчик обнулился; файлы и сокеты закрываются вовремя.
- Нет пауз, нет фонового потока, нет барьеров записи, маленький рантайм.
- Нет UB при обращении к `nil` (паника с трассировкой), нет use-after-free
  (ссылка удерживает объект), нет арифметики указателей.

### Почему не иначе

- Rust-подобное владение даёт нулевую стоимость, но противоречит цели
  «простота Go»: lifetimes и borrow checker — не для этого языка.
- Трассирующий GC решает циклы, но приносит паузы, большой рантайм,
  сложный FFI и неопределённое время освобождения ресурсов.

## Стандартная библиотека

Всё лежит под `std`. Первая версия реализует разделы, отмеченные «ядро»;
остальное добавляется поэтапно поверх рантайма.

### Ядро

| Пространство | Содержимое |
|--------------|------------|
| `std.io` | `println`, `print`, `ReadFile`, `WriteFile`, потоки stdin/stdout/stderr, `Reader`/`Writer` |
| `std.fmt` | `Format("{0} {1}", a, b)`, `Sprintf`, форматирование чисел и дат |
| `std.strings` | `Split`, `Join`, `Trim`, `Contains`, `Replace`, `ToUpper`, `ToInt`, `Builder` |
| `std.math` | `Sqrt`, `Abs`, `Min`, `Max`, `Pow`, `Floor`, тригонометрия, константы |
| `std.collections` | `List<T>`, `Map<K,V>`, `Set<T>`, `Queue<T>`, `Stack<T>` (после generics) |
| `std.task` | `All`, `Any`, `Delay`, `Spawn`, `Channel<T>`, `Mutex`, `WaitGroup`, `Timeout` |
| `std.os` | аргументы, переменные окружения, `Exit`, запуск процессов, сигналы |
| `std.fs` | файлы и каталоги: `Exists`, `ReadDir`, `Mkdir`, `Remove`, `Stat`, пути |
| `std.time` | `Now`, `Duration`, `Sleep`, `Timer`, форматирование и парсинг дат |
| `std.errors` | `Wrap`, `Unwrap`, `Is`, `As`, `New` |

### Сеть и протоколы

| Пространство | Содержимое |
|--------------|------------|
| `std.net` | TCP/UDP сокеты, `Dial`, `Listen`, DNS, IP-адреса; всё асинхронное |
| `std.net.http` | клиент: `Get`, `Post`, `Request`, `Client` с таймаутами и заголовками; сервер: `Server`, `Router`, `Handler`, middleware; HTTP/1.1 и HTTP/2 |
| `std.net.tls` | TLS-соединения поверх `net`, сертификаты, конфигурация |
| `std.net.ws` | WebSocket клиент и сервер |
| `std.net.url` | разбор и сборка URL, query-параметры |
| `std.net.mail` | отправка писем по SMTP |

### Криптография

| Пространство | Содержимое |
|--------------|------------|
| `std.crypto.hash` | `Sha256`, `Sha512`, `Sha3`, `Blake2`, `Md5` (только для совместимости), потоковый интерфейс `Hasher` |
| `std.crypto.hmac` | HMAC поверх любого хэша |
| `std.crypto.aes` | AES-GCM, AES-CBC; `ChaCha20-Poly1305` |
| `std.crypto.rsa` | генерация ключей, подпись, шифрование, PKCS#1/OAEP |
| `std.crypto.ecdsa`, `std.crypto.ed25519` | ключи на эллиптических кривых, подпись и проверка |
| `std.crypto.ecdh`, `std.crypto.x25519` | обмен ключами |
| `std.crypto.kdf` | `Pbkdf2`, `Argon2`, `Scrypt`, `Hkdf` |
| `std.crypto.rand` | криптографически стойкие случайные числа и байты |
| `std.crypto.x509` | разбор и проверка сертификатов, PEM/DER |
| `std.crypto.password` | `Hash`/`Verify` на базе Argon2 для паролей |

Реализация: первая версия линкуется с проверенными C-библиотеками (BoringSSL
или libsodium), чтобы не писать криптографию самостоятельно. Интерфейс
стабилен, бэкенд можно заменить.

### Данные и форматы

| Пространство | Содержимое |
|--------------|------------|
| `std.encoding.json` | `Marshal`/`Unmarshal` в структуры, потоковый парсер, теги полей |
| `std.encoding.xml`, `std.encoding.yaml`, `std.encoding.toml` | аналогично |
| `std.encoding.base64`, `std.encoding.hex` | кодировки |
| `std.encoding.csv` | чтение и запись CSV |
| `std.compress` | gzip, zlib, zstd |
| `std.regex` | регулярные выражения (RE2-подобный синтаксис, линейное время) |
| `std.unicode` | классификация символов, нормализация |
| `std.database.sql` | единый интерфейс к SQL, драйверы PostgreSQL/SQLite/MySQL отдельными пакетами |

### Прочее

| Пространство | Содержимое |
|--------------|------------|
| `std.log` | структурированное логирование, уровни, вывод в JSON/текст |
| `std.testing` | `Test*` функции, `assert`, бенчмарки, табличные тесты |
| `std.random` | ГПСЧ для не-криптографических задач |
| `std.sync` | атомики, `Once`, `RwMutex` |
| `std.reflect` | `TypeOf<T>`, `ValueOf`, `TypeInfo`/`FieldInfo`/`MethodInfo`, атрибуты, compile-time `Fields<T>()` (см. раздел «Рефлексия») |
| `std.process` | запуск внешних процессов, пайпы |
| `std.uuid` | генерация и разбор UUID |

## Компилятор и инструменты

- Реализация компилятора: **Rust** (крейт `inkwell` для LLVM API) — безопасно,
  быстро, хорошие библиотеки для парсинга.
- Дистрибутив для пользователя — один бинарник `slang`: LLVM и `lld`
  линкуются статически, рантайм и стандартная библиотека лежат рядом.
  Пользователю не нужно ставить LLVM, clang или системный линковщик.
- Пайплайн: лексер → парсер (AST) → разрешение имён и типов → проверка
  ошибок (`!`/`?`/`try`) → понижение в LLVM IR → `llc`/`clang` для линковки
  с рантаймом.
- Рантайм (`runtime/`): на C или Rust — аллокатор, подсчёт ссылок,
  планировщик задач, строки, срезы, map, паника.
- CLI:

```
slang build ./src -o app      # собрать
slang run main.sl             # собрать и запустить
slang fmt ./src               # форматировать
slang test ./src              # тесты (func Test*)
```

- Расширение файлов: `.sl`.
- Компилятор — библиотека; LSP-сервер, форматтер, отладчик и IDE-плагины
  строятся на ней. Подробно: `docs/TOOLING.md`. Каноническая грамматика
  для IDE: `grammar/slang.bnf`; подсветка VS Code: `editors/vscode/`.
- Проект описывается файлом `slang.toml` (имя, версия, зависимости, точка
  входа). Менеджер пакетов встроен в `slang`: `docs/PACKAGES.md`.

## Пример программы

```slang
namespace app

import std
import std.io

struct Counter {
    count int

    func Inc() { this.count += 1 }
    func Value() int { return this.count }
}

struct Parser {
    static func ParseInt(s string) int! {
        if s == "" {
            return error("empty string")
        }
        return strings.ToInt(s)?
    }
}

async func Load(path string) []string! {
    var text = await io.ReadFileAsync(path)?
    return strings.Split(text, "\n")
}

async func main() {
    var lines = try await Load("numbers.txt") catch err {
        println("cannot load:", err)
        return
    }

    var c = Counter{}
    for line in lines {
        var n = try Parser.ParseInt(line) else 0
        if n > 0 {
            c.Inc()
        }
    }
    println("positive numbers:", c.Value())
}
```

## Грамматика (упрощённо, EBNF)

```
file        = "namespace" path { import } { decl } ;
import      = "import" path [ "as" ident ] ;
decl        = { attribute } ( struct | interface | enum | func | var | const ) ;
attribute   = "@" path [ "(" [ attrarg { "," attrarg } ] ")" ] ;
attrarg     = [ ident ":" ] expr ;
struct      = "struct" ident "{" { field | method } "}" ;
field       = { attribute } ident type ;
method      = [ "static" ] func ;
func        = [ "async" ] "func" ident "(" [ params ] ")" [ type ] block ;
params      = param { "," param } ;
param       = ident { "," ident } type ;
type        = path [ "!" ] | "*" type | "[" [ int ] "]" type
            | "map" "[" type "]" type | "func" "(" [ types ] ")" [ type ]
            | "(" types ")" ;
block       = "{" { stmt } "}" ;
stmt        = var | const | if | for | switch | trystmt | "return" [ exprs ]
            | "break" | "continue" | "defer" expr | expr [ assignop expr ] ;
var         = "var" ident [ type ] [ "=" expr ] ;
if          = "if" expr block [ "else" ( if | block ) ] ;
for         = "for" [ ident [ "," ident ] "in" ( expr | range ) | expr ] block ;
switch      = "switch" expr "{" { "case" pattern { "," pattern } ":" stmts }
              [ "default" ":" stmts ] "}" ;
pattern     = expr | expr ".." expr | path "(" [ idents ] ")" | type ident ;
expr        = unary { binop unary } ;
unary       = ( "-" | "!" | "&" | "*" | "await" ) unary | tryexpr | postfix ;
tryexpr     = "try" expr ( "catch" ident block | "else" expr ) ;
trystmt     = "try" block "catch" ident block ;
postfix     = primary { "." ident | "(" [ exprs ] ")" | "[" ( expr | range ) "]"
            | "?" [ "(" expr ")" ] } ;
range       = [ expr ] ".." [ expr ] ;
primary     = literal | ident | path "{" [ fields ] "}" | "new" primary
            | "(" expr ")" | lambda ;
```

## Этапы реализации

1. **Лексер и парсер** — полный синтаксис, AST, сообщения об ошибках с
   позициями.
2. **Семантика** — пространства имён и импорты, разрешение имён, вывод типов,
   проверка `!`/`?`/`try`, структурные интерфейсы.
3. **Кодогенерация** — LLVM IR для структур, функций, указателей, срезов,
   строк, `switch`, ошибок; линковка с рантаймом на C.
4. **Рантайм** — аллокатор с подсчётом ссылок, строки, срезы, map, паника.
5. **Async** — LLVM coroutines, планировщик, `std.task`.
6. **Стандартная библиотека (ядро) и инструменты** — `std.io`, `std.strings`,
   `std.fmt`, `std.fs`, `std.os`, `std.time`, `slang fmt`, `slang test`.
7. **Generics** — `List<T>`, ограничения через интерфейсы (мономорфизация).
8. **Сеть и криптография** — `std.net`, `std.net.http`, `std.crypto.*` через
   BoringSSL/libsodium, `std.encoding.json`.
9. **Остальная стандартная библиотека** — форматы, сжатие, regex, SQL, log.
10. **Рефлексия** — атрибуты, таблицы `TypeInfo`, `std.reflect`, compile-time
    `Fields<T>()`; `std.encoding.json` поверх неё.
11. **Инструменты** — `slang lsp`, `slang fmt`, DWARF и DAP, плагины для
    VS Code и IDEA (`docs/TOOLING.md`).

## Открытые вопросы

- Точный алгоритм вывода владения (Perceus-подобный) и его взаимодействие с
  корутинами: уточнить при реализации.
- Нужен ли `null`-safety для указателей (`*T` vs `?*T`) — склоняюсь к тому,
  что указатель всегда может быть `nil`, как в Go, но с паникой вместо UB.
- Семантика `defer` внутри `async` (при каком `await` выполняется).
- Детали реестра пакетов и capabilities: `docs/PACKAGES.md`.
- Собственная реализация криптографии vs постоянная зависимость от C-библиотек.
