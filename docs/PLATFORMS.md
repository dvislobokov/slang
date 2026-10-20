# Платформы и кроссплатформенность

Slang должен работать одинаково на Linux, macOS и Windows, на amd64 и
arm64. Это требование к компилятору, рантайму, стандартной библиотеке и
инструментам, а не задача «портировать потом».

## Целевые платформы

| Tier | Платформы | Гарантии |
|---|---|---|
| 1 | linux-amd64, linux-arm64, macos-arm64, windows-amd64 | CI на каждый коммит, релизные бинарники, все тесты зелёные |
| 2 | macos-amd64, windows-arm64, linux-musl (amd64/arm64), freebsd-amd64 | CI на релиз, бинарники |
| 3 | wasm32-wasi, android-arm64, ios-arm64 | компилируется, тесты по возможности |

Имена целей — LLVM target triples: `x86_64-unknown-linux-gnu`,
`aarch64-apple-darwin`, `x86_64-pc-windows-msvc` и т. д., с короткими
псевдонимами `linux-amd64`, `macos-arm64`, `windows-amd64`.

## Кросс-компиляция

`slang build --target windows-amd64` работает с любой машины:

- LLVM генерирует код под любую цель из одного бинарника компилятора.
- Линковщик `lld` (вшит в `slang`) умеет ELF, Mach-O и PE/COFF.
- Рантайм и стандартная библиотека поставляются прекомпилированными для
  всех платформ tier 1-2 (`~/.slang/targets/<triple>/libslang_rt.a` плюс
  минимальный набор системных библиотек/импорт-библиотек: для Windows —
  `kernel32.lib`, `ws2_32.lib` и т. п. из mingw-w64 или Windows SDK; для
  macOS — `libSystem.tbd`; для Linux — glibc или musl). Недостающие цели
  скачиваются `slang target add windows-amd64`.
- Условная компиляция: `@cfg(os: "windows")`, `@cfg(arch: "arm64")`,
  `@cfg(not(os: "linux"))` на объявлениях и файлах; суффиксы файлов
  `foo_windows.sl`, `foo_linux.sl`, `foo_arm64.sl` как в Go.
- Константы `os.Name` (`"linux"|"macos"|"windows"|...`), `os.Arch`
  (`"amd64"|"arm64"|...`), `os.PathSeparator`, `os.LineSeparator`.

## Рантайм: слой абстракции

Весь платформозависимый код рантайма живёт в `runtime/src/platform/` за
одним внутренним интерфейсом `slang_platform.h`. Остальной рантайм
(строки, срезы, map, ошибки, планировщик) платформонезависим.

| Область | Linux | macOS | Windows |
|---|---|---|---|
| Потоки, мьютексы, условные переменные | pthreads | pthreads | Win32 threads, SRWLock, CONDITION_VARIABLE |
| Атомики | C11 `<stdatomic.h>` | C11 | C11 (clang) |
| Асинхронный ввод-вывод | epoll (позже io_uring) | kqueue | IOCP |
| Таймеры | timerfd / планировщик | kqueue EVFILT_TIMER | waitable timers |
| Файлы и каталоги | POSIX | POSIX | Win32 (`CreateFileW`, UTF-16 пути) |
| Процессы | fork/exec, posix_spawn | posix_spawn | `CreateProcessW` |
| Сеть | BSD sockets | BSD sockets | Winsock2 |
| Backtrace при панике | `execinfo` / libunwind | `execinfo` | `CaptureStackBackTrace` + DbgHelp |
| Случайные байты | `getrandom(2)` | `getentropy` | `BCryptGenRandom` |
| Время | `clock_gettime` | `clock_gettime` | `QueryPerformanceCounter`, `GetSystemTimePreciseAsFileTime` |
| Динамическая загрузка (FFI) | `dlopen` | `dlopen` | `LoadLibraryW` |
| Консоль | UTF-8 | UTF-8 | UTF-8 через `WriteConsoleW` конверсию, ANSI-цвета включаются |

Правила:

- Пути и строки внутри языка всегда UTF-8; на границе Win32 конвертация в
  UTF-16. `std.fs` принимает и `/`, и `\` на Windows, отдаёт платформенный
  разделитель.
- Переводы строк: `println` пишет `\n` везде; текстовые функции чтения
  понимают `\r\n`.
- Сигналы: `os.OnInterrupt(handler)` абстрагирует SIGINT/SIGTERM и
  `SetConsoleCtrlHandler`.
- `std.process`, `std.os`, `std.fs`, `std.net`, `std.time` реализуются
  поверх этого слоя, без `#ifdef` вне `platform/`.

## Компилятор и инструменты

- Сам компилятор собирается под все платформы tier 1 (Rust + статический
  LLVM), распространяется как один бинарник + архив рантаймов.
- Установка: `curl ... | sh` (Linux/macOS), `irm ... | iex` (PowerShell),
  Homebrew, winget, apt/rpm-репозитории; `slang self update`.
- LSP, `fmt`, пакетный менеджер — платформонезависимы по построению
  (Rust), хранилище пакетов использует жёсткие ссылки на NTFS/APFS/ext4
  и копирование там, где их нет.
- Отладка: DWARF на Linux/macOS, CodeView/PDB на Windows (LLVM умеет
  оба), чтобы работали lldb, gdb и Visual Studio/WinDbg.

## CI

Матрица GitHub Actions: `ubuntu-latest` (amd64), `ubuntu-24.04-arm`,
`macos-latest` (arm64), `macos-13` (amd64), `windows-latest`. На каждой:
сборка компилятора, `make test` рантайма, `cargo test`, прогон
`examples/` и `tests/spec/`, кросс-сборка hello world под все tier-1 цели
с запуском под QEMU/Wine там, где возможно. Размер бинарника hello world
и бенчмарки отслеживаются по платформам.

## Текущее состояние

- Реализовано: linux-amd64 (компилятор, рантайм на POSIX, все тесты).
- Рантайм пока содержит прямые вызовы pthreads/execinfo/getrandom вне
  слоя `platform/` — выделение слоя и порт на macOS (kqueue) и Windows
  (Win32, IOCP) — отдельный этап после generics.
- `lld` ещё не вшит; линковка через системный `clang`.

## Этапы

1. Выделить `runtime/src/platform/` (posix.c), убрать прямые вызовы из
   остального рантайма; `@cfg` и файловые суффиксы в компиляторе;
   `os.Name/Arch`.
2. macOS arm64/amd64: kqueue-бэкенд планировщика, CI на macOS.
3. Windows amd64: win32.c (потоки, файлы, консоль UTF-8, Winsock, IOCP),
   PE/COFF через lld, CodeView, CI на Windows.
4. Кросс-компиляция `--target`, `slang target add`, прекомпилированные
   рантаймы, релизные сборки под все tier-1.
5. linux-arm64, musl, FreeBSD; позже wasm32-wasi.
