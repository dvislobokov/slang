# Slang — дизайн языка

Slang — компилируемый язык с простой структурой в духе Go, но без шумных
`if err != nil`, с выводом типов через `var`, пространствами имён вместо
пакетов, методами внутри структур и встроенным `async/await`.
Компилируется в нативный код через LLVM.

## Цели

- **Простота Go**: маленький набор конструкций, явный поток управления,
  структуры вместо классов, указатели без арифметики.
- **Меньше шума**: обработка ошибок через `!` и `try` вместо повторяющихся
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
- Сборщик мусора на первом этапе: подсчёт ссылок (ARC) для heap-объектов,
  стек для всего остального. GC возможен в будущем.

## Файлы и пространства имён

Один файл начинается с объявления пространства имён. Имя — полный путь,
точки разделяют уровни. Импорт ссылается на пространство имён целиком или на
конкретный тип.

```slang
namespace app.services.users

import std.io
import std.collections.List
import app.models.User

struct UserService {
    // ...
}
```

- Обращение к импортированному: `io.println(...)`, `List<User>` (generics
  позже), `User`.
- Разрешение конфликтов — через полный путь: `app.models.User`.
- Псевдонимы: `import app.models.User as Model`.
- Точка входа — функция `main` в любом пространстве имён, указанная при сборке.
- Видимость: имя с заглавной буквы экспортируется (как в Go). Строчная — приватно
  для пространства имён.

## Объявления переменных

Только `var`. Тип выводится из инициализатора; явная аннотация опциональна.

```slang
var x = 123          // int
var name = "slang"   // string
var ratio = 0.5      // float
var list: []int = [] // явный тип нужен, если из значения не выводится
```

- `const` — константы времени компиляции: `const Pi = 3.14159`.
- Переменные нельзя объявить и не использовать (как в Go).
- Переменная без инициализатора требует типа и получает нулевое значение:
  `var count: int`.

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
на общий буфер).

## Структуры и методы

Методы объявляются внутри структуры. Приёмник неявный — `this`. Метод
по умолчанию работает с указателем на структуру, как `func (s *S)` в Go.

```slang
struct Point {
    X: float
    Y: float

    func Length(): float {
        return math.Sqrt(this.X * this.X + this.Y * this.Y)
    }

    func Move(dx: float, dy: float) {
        this.X += dx
        this.Y += dy
    }
}
```

- Конструкторы: литерал `Point{X: 1, Y: 2}` или статическая функция.
- Статические функции: `static func New(x: float, y: float): Point { ... }`,
  вызов `Point.New(1, 2)`.
- Поля с заглавной буквы — публичные, со строчной — приватные.
- Встраивание (композиция) как в Go: поле без имени `Base` поднимает его
  поля и методы.
- Функций уровня пространства имён нет внутри структур, но свободные `func`
  на уровне namespace допустимы для утилит и `main`.

## Интерфейсы

Структурная типизация, как в Go: структура реализует интерфейс, если имеет
все его методы. Явного `implements` нет.

```slang
interface Shape {
    func Area(): float
}

struct Circle {
    R: float
    func Area(): float { return 3.14159 * this.R * this.R }
}

func Describe(s: Shape) {
    io.println(s.Area())
}
```

## Перечисления

```slang
enum Color { Red, Green, Blue }

enum Result {
    Ok(value: int)
    Fail(code: int)
}

var c = Color.Red
match r {
    Result.Ok(v)   => io.println(v)
    Result.Fail(c) => io.println("fail", c)
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

Ключевая идея: ошибки остаются значениями, как в Go, но синтаксис убирает
повторяющиеся проверки.

Функция, которая может завершиться ошибкой, помечает возвращаемый тип `!`:

```slang
func ReadConfig(path: string): Config! {
    var text = io.ReadFile(path)?      // ? — пробросить ошибку наверх
    var cfg = ParseConfig(text)?
    return cfg
}
```

- `T!` — либо значение `T`, либо `error`. `void!` — только успех или ошибка.
- `expr?` — если `expr` вернул ошибку, немедленно вернуть её из текущей
  функции. Допустимо только в функциях с типом `!`.
- `return error("message")` или `return fmt.Error("...: {0}", x)` создаёт ошибку.
- Обработать на месте — `try ... catch`:

```slang
var cfg = try ReadConfig("app.toml") catch err {
    io.println("config:", err)
    return
}
```

- `try expr else defaultValue` — вернуть значение по умолчанию при ошибке.
- `error` — встроенный интерфейс с методом `Message(): string`. Любая структура
  с таким методом может быть ошибкой. Оборачивание: `error.Wrap(err, "ctx")`,
  проверка типа — через `match err { NotFound(e) => ... }`.
- Тип `T!` нельзя использовать, не обработав: компилятор запрещает
  игнорировать ошибку (`_ = f()` — явное игнорирование).

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

match value {
    1 | 2    => ...
    3..10    => ...
    _        => ...
}
```

- `break`, `continue`, `return`. Меток нет.
- `defer` — выполнить при выходе из функции, как в Go.

## Функции

```slang
func Add(a: int, b: int): int { return a + b }
func Swap(a: int, b: int): (int, int) { return b, a }
var (x, y) = Swap(1, 2)
```

- Параметры: `имя: тип`. Возвращаемый тип после `:`; `void` можно опустить.
- Несколько возвращаемых значений — кортеж.
- Замыкания: `var f = func(x: int): int { return x * 2 }`, короткая форма
  `var f = (x: int) => x * 2`.
- Функции — значения первого класса, тип `func(int): int`.

## Асинхронность

Модель — задачи (tasks) на пуле потоков, как в C#/Tokio, с
языковым `async`/`await`.

```slang
async func Fetch(url: string): string! {
    var resp = await http.Get(url)?
    return await resp.Text()?
}

async func main() {
    var a = Fetch("https://a")      // Task<string!>, запущена сразу
    var b = Fetch("https://b")
    var (ra, rb) = await task.All(a, b)?
    io.println(ra, rb)
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

## Память

- Значения (структуры, массивы фиксированной длины, примитивы) — на стеке.
- `new T{}`, срезы, map, строки, замыкания, фреймы корутин — в куче.
- Управление: автоматический подсчёт ссылок без циклов-сборщика на первом
  этапе. `weak *T` для разрыва циклов. Переход на GC рассматривается позже;
  семантика языка от этого не зависит.
- Нет `free`, нет ручного управления, нет UB при обращении к `nil`
  (паника с трассировкой).

## Стандартная библиотека (минимум для первой версии)

| Пространство | Содержимое |
|--------------|------------|
| `std.io` | `println`, `print`, `ReadFile`, `WriteFile`, stdin/stdout |
| `std.fmt` | форматирование строк `fmt.Format("{0} {1}", a, b)`, `fmt.Error` |
| `std.strings` | `Split`, `Join`, `Trim`, `Contains`, `ToUpper`, ... |
| `std.math` | `Sqrt`, `Abs`, `Min`, `Max`, `Pow`, константы |
| `std.collections` | `List<T>`, `Map<K,V>`, `Set<T>` (после generics) |
| `std.task` | `All`, `Any`, `Delay`, `Spawn`, `Channel<T>` |
| `std.os` | аргументы, env, exit, процессы |
| `std.time` | `Now`, `Duration`, `Sleep` |

## Компилятор и инструменты

- Реализация компилятора: **Rust** (крейт `inkwell` для LLVM API) — безопасно,
  быстро, хорошие библиотеки для парсинга.
- Пайплайн: лексер → парсер (AST) → разрешение имён и типов → проверка
  владения/ошибок → понижение в LLVM IR → `llc`/`clang` для линковки
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
- Проект описывается файлом `slang.toml` (имя, версия, зависимости, точка
  входа).

## Пример программы

```slang
namespace app

import std.io
import std.strings

struct Counter {
    count: int

    func Inc() { this.count += 1 }
    func Value(): int { return this.count }
}

struct Parser {
    static func ParseInt(s: string): int! {
        if s == "" {
            return error("empty string")
        }
        return strings.ToInt(s)?
    }
}

async func Load(path: string): []string! {
    var text = await io.ReadFileAsync(path)?
    return strings.Split(text, "\n")
}

async func main() {
    var lines = try await Load("numbers.txt") catch err {
        io.println("cannot load:", err)
        return
    }

    var c = Counter{}
    for line in lines {
        var n = try Parser.ParseInt(line) else 0
        if n > 0 {
            c.Inc()
        }
    }
    io.println("positive numbers:", c.Value())
}
```

## Грамматика (упрощённо, EBNF)

```
file        = "namespace" path { import } { decl } ;
import      = "import" path [ "as" ident ] ;
decl        = struct | interface | enum | func | var | const ;
struct      = "struct" ident "{" { field | method } "}" ;
field       = ident ":" type ;
method      = [ "static" ] func ;
func        = [ "async" ] "func" ident "(" [ params ] ")" [ ":" type ] block ;
params      = param { "," param } ;
param       = ident ":" type ;
type        = path [ "!" ] | "*" type | "[" [ int ] "]" type
            | "map" "[" type "]" type | "func" "(" [ types ] ")" [ ":" type ]
            | "(" types ")" ;
block       = "{" { stmt } "}" ;
stmt        = var | const | if | for | match | "return" [ exprs ]
            | "break" | "continue" | "defer" expr | expr [ assignop expr ] ;
var         = "var" ident [ ":" type ] [ "=" expr ] ;
if          = "if" expr block [ "else" ( if | block ) ] ;
for         = "for" [ ident [ "," ident ] "in" expr | expr ] block ;
match       = "match" expr "{" { pattern "=>" ( expr | block ) } "}" ;
expr        = unary { binop unary } ;
unary       = ( "-" | "!" | "&" | "*" | "await" | "try" ) unary | postfix ;
postfix     = primary { "." ident | "(" [ exprs ] ")" | "[" expr "]" | "?" } ;
primary     = literal | ident | path "{" [ fields ] "}" | "new" primary
            | "(" expr ")" | lambda ;
```

## Этапы реализации

1. **Лексер и парсер** — полный синтаксис, AST, сообщения об ошибках с
   позициями.
2. **Семантика** — пространства имён, разрешение имён, вывод типов, проверка
   `!`/`?`, структурные интерфейсы.
3. **Кодогенерация** — LLVM IR для структур, функций, указателей, срезов,
   строк, `match`, ошибок; линковка с рантаймом на C.
4. **Рантайм** — аллокатор с подсчётом ссылок, строки, срезы, map, паника.
5. **Async** — LLVM coroutines, планировщик, `std.task`.
6. **Стандартная библиотека и инструменты** — `std.*`, `slang fmt`,
   `slang test`, `slang.toml`.
7. **Generics** — `List<T>`, ограничения через интерфейсы (мономорфизация).

## Открытые вопросы

- Подсчёт ссылок vs GC: решить после замеров на реальных программах.
- Нужен ли `null`-safety для указателей (`*T` vs `?*T`) — склоняюсь к тому,
  что указатель всегда может быть `nil`, как в Go, но с паникой вместо UB.
- Семантика `defer` внутри `async` (при каком `await` выполняется).
- Формат модулей и менеджер зависимостей.
