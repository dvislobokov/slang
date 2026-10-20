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
  `@cfg(not(os: "linux"))`, `@cfg(any(...))`, `@cfg(all(...))`,
  `@cfg(feature: "tls")` на объявлениях (и их полях, методах, вариантах) и
  перед `namespace` (весь файл); суффиксы файлов `foo_windows.sl`,
  `foo_linux.sl`, `foo_macos.sl`, `foo_amd64.sl`, `foo_arm64.sl`,
  `foo_linux_arm64.sl` как в Go. Правила — `DESIGN.md` «Платформы».
- Константы `os.Name` (`"linux"|"macos"|"windows"|...`), `os.Arch`
  (`"amd64"|"arm64"|...`), `os.PathSeparator`, `os.LineSeparator`.

## Рантайм: слой абстракции

Весь платформозависимый код рантайма живёт в `runtime/src/platform/` за
одним внутренним интерфейсом `platform/platform.h`. Остальной рантайм
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
- Слой `runtime/src/platform/` начат (интерфейс `platform/platform.h`,
  `runtime/ABI.md` §15): `posix.c` — потоки, число CPU, монотонные часы,
  случайные байты (`getrandom`/`getentropy`), backtrace; `poll_epoll.c` —
  поллер готовности на epoll (level-triggered, one-shot) + eventfd;
  `poll_kqueue.c` — тот же поллер на kqueue для macOS/BSD
  (`EVFILT_READ`/`EVFILT_WRITE` с `EV_ONESHOT`, пробуждение через
  `EVFILT_USER`, где его нет — self-pipe); `net_posix.c` — неблокирующие
  TCP/UDP/Unix-сокеты, опции, sendfile, `getaddrinfo`/`getnameinfo`,
  `getifaddrs`, игнорирование SIGPIPE и подъём лимита открытых файлов.
  Через слой идут планировщик (потоки воркеров и таймера), шардированные
  поллеры `task_io.c` (§10.9), паника (backtrace),
  `std.crypto.rand`/`std.random` (энтропия) и `std.net` (§16).
- **kqueue не запускался**: здесь нет macOS/BSD. `poll_kqueue.c` и
  BSD/macOS-ветки `net_posix.c` проверены `clang -fsyntax-only -Wall
  -Wextra -Werror -D__APPLE__` (и `-D__OpenBSD__` без `EVFILT_USER`) на
  Linux с заглушкой `<sys/event.h>` (struct kevent, EV_*/EVFILT_*,
  kqueue/kevent); `-target arm64-apple-macos12` без SDK невозможен. Остаётся
  прогнать `make -C runtime test` (test_net, test_platform) на macOS в CI.
- Ещё вне слоя: мьютексы и условные переменные (pthreads прямо в `task.c`),
  `std_time.c`, `std_fs.c`, `std_os.c`, `io.c` (POSIX) — переносятся вместе с
  портом на Windows; IOCP для Windows не написан (план ниже).

### Асинхронный ввод-вывод: интерфейс поллера и план для IOCP

Интерфейс (`platform.h`) — **готовность** (readiness): `poller_arm(fd,
READ|WRITE, token)` просит одно уведомление, `poller_wait` отдаёт
`{token, events}`, после чего вызывающий сам делает неблокирующий
`recv`/`send`, а при `EAGAIN` снова ждёт. epoll и kqueue ложатся на него
напрямую. IOCP — модель **завершения** (completion): операция
(`WSARecv`/`WSASend`/`AcceptEx`/`ConnectEx`) запускается с буфером, а порт
сообщает о её окончании. Адаптер для Windows (`platform/poll_iocp.c` +
`net_win32.c`) — по шагам:

1. **Готовность через «нулевое» чтение.** Для `READ` поллер запускает
   `WSARecv` с буфером нулевой длины (`MSG_PEEK` не нужен): завершение
   означает «данные есть», и `poller_wait` отдаёт `{token, READ}` —
   дальше работает обычный неблокирующий `recv`. Так делают libuv и
   (через AFD) wepoll/mio; это сохраняет весь код `task_io.c`/`std_net.c`
   без изменений. Для `WRITE` — `WSASend` нулевой длины либо (точнее)
   `IOCTL_AFD_POLL` через `\Device\Afd` (как wepoll): одна операция
   опроса на сокет, отменяемая `CancelIoEx` при `poller_remove`.
2. **Токены и one-shot.** Каждое армирование — одна OVERLAPPED-операция,
   несущая токен (поле в структуре-обёртке вокруг OVERLAPPED); её
   завершение и есть one-shot-уведомление. `poller_remove` делает
   `CancelIoEx` и ждёт отменённого завершения (оно приходит с
   `ERROR_OPERATION_ABORTED` и отбрасывается по поколению токена, как
   устаревшие события сейчас).
3. **Пробуждение** — `PostQueuedCompletionStatus` со служебным ключом
   (`SLANG_PLAT_WAKE_TOKEN`), ожидание — `GetQueuedCompletionStatusEx`
   (пачкой, как `epoll_wait`).
4. **Шаг 2 (производительность)**: прямой completion-путь для больших
   передач — `ReadInto`/`Write`/`SendFile` (`TransmitFile`) запускают
   настоящий `WSARecv`/`WSASend` в буфер Slang и ждут завершения; для
   этого `std_net.c` получает вторую реализацию операций
   (`platform_ops_completion`), а Slang-код не меняется, потому что он и
   сейчас видит только «результат или ждать» (`-1` → await).
5. Сокеты Winsock создаются `WSASocketW(..., WSA_FLAG_OVERLAPPED |
   WSA_FLAG_NO_HANDLE_INHERIT)`, неблокирующие через `ioctlsocket(FIONBIO)`;
   `SIGPIPE` на Windows не существует; Unix-сокеты — `AF_UNIX` (Windows
   10 1803+), без абстрактного пространства имён.

- `lld` ещё не вшит; линковка через системный `clang`.
- Условная компиляция реализована (`compiler/src/cfg.rs`, `target.rs`):
  `@cfg` с `os`/`arch`/`feature`/`not`/`any`/`all` на объявлениях, членах и
  файлах, суффиксы имён файлов (в `driver::collect_files`, то есть и в
  пакетах, и в LSP), константы `os.Name`/`Arch`/`PathSeparator`/
  `LineSeparator`. Цель — хост или `--target <triple|alias>`; другая цель
  пока только для `slang check` (генерация кода и линковка — для хоста),
  так что `slang check --target windows-amd64` проверяет Windows-варианты
  кода на Linux.

## Этапы

1. Выделить `runtime/src/platform/` (posix.c), убрать прямые вызовы из
   остального рантайма; ✅ `@cfg` и файловые суффиксы в компиляторе;
   ✅ `os.Name/Arch`.
2. macOS arm64/amd64: kqueue-бэкенд планировщика, CI на macOS.
3. Windows amd64: win32.c (потоки, файлы, консоль UTF-8, Winsock, IOCP),
   PE/COFF через lld, CodeView, CI на Windows.
4. Кросс-компиляция `--target`, `slang target add`, прекомпилированные
   рантаймы, релизные сборки под все tier-1.
5. linux-arm64, musl, FreeBSD; позже wasm32-wasi.
