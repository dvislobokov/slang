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
`aarch64-apple-darwin`, `x86_64-pc-windows-gnu` и т. д., с короткими
псевдонимами `linux-amd64`, `macos-arm64`, `windows-amd64`. Сейчас
`windows-amd64` — это ABI MinGW (`x86_64-pc-windows-gnu`: рантайм собран с
заголовками mingw-w64, линковка с его CRT и импорт-библиотеками); ABI MSVC
(`x86_64-pc-windows-msvc`, CodeView/PDB, библиотеки Windows SDK) — позже,
пока только `check` и `--emit=obj`.

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

- **linux-amd64**: компилятор, рантайм, все тесты (`make -C runtime test`,
  `cargo test --workspace`).
- **Слой `runtime/src/platform/` завершён**: вне `platform/` в рантайме нет
  ни одного `#ifdef` ОС и ни одного прямого вызова POSIX/Win32
  (`runtime/ABI.md` §15). Через слой идут потоки, мьютексы/условные
  переменные/once/TLS (`slang__plat_mutex` и т. д.: pthreads inline в
  `platform.h`; SRWLOCK/CONDITION_VARIABLE/INIT_ONCE/FLS в `win32.c`; имени
  `slang_mutex` нет — оно занято ABI-типом `Mutex`), часы (`mono_ns`,
  `wall_ns`, `sleep_ns`), календарь (`utc_tm`/`timegm` на чистом C),
  `strftime`/`strptime`, файлы и каталоги (`fs_posix.c`/`fs_win32.c`),
  процессы (`fs_posix.c`/`proc_win32.c`), сигналы, пользователи,
  окружение, консоль, aligned-память, сокеты, поллер, backtrace, `dlopen`.
- **windows-amd64 (MinGW ABI) — кросс-сборка с Linux и запуск под Wine**:
  - `make -C runtime CROSS=x86_64-w64-mingw32` собирает
    `runtime/build/win64/libslang_rt.a` (копия в
    `runtime/build/x86_64-pc-windows-gnu/`) через `clang -target
    x86_64-w64-mingw32` с `-Wall -Wextra -Werror`; без clang —
    `x86_64-w64-mingw32-gcc`. clang предпочтителен: тот же native TLS, что
    у кода LLVM (gcc для mingw использует emutls).
  - `slang build --target windows-amd64 x.sl` → `x.exe` (PE32+, COFF-объект
    от LLVM с datalayout и соглашением вызовов Win64 из triple; на границе
    ABI только скаляры и указатели, так что структуры по значению и varargs
    Win64 не участвуют). Линковка: `clang -target x86_64-w64-mingw32
    -fuse-ld=lld` + рантайм + ws2_32, iphlpapi, bcrypt, shell32, advapi32.
    Рантайм цели ищется в `~/.slang/targets/<triple>/libslang_rt.a`
    (`$SLANG_TARGETS`), затем `runtime/build/<triple>/`.
    `slang run --target windows-amd64` запускает через Wine (`$SLANG_WINE`).
    Отладочная информация — DWARF (gdb/lldb); для MSVC ABI выставляется
    флаг модуля CodeView.
  - Под Wine 9.0 работают байт-в-байт с Linux-ными `.out` (без
    нормализации CRLF: `println` пишет `\n`, stdout/stderr в двоичном
    режиме): 37 примеров в отладочной сборке с `--leak-check` (hello,
    arith, control, functions, structs, switch, enums, slices_maps, strings,
    interfaces, errors, errors_trace, defer, bank, async, cancel, channels,
    drop, generics, weak, json, reflection, regex, sealed_shapes, try_block,
    task_context, tracing, log, metrics, cli_tool, stdlib_* ×6, multi),
    7 в `--release` и сетевые tcp_echo, http_server, tls_echo, redis,
    http_realtime (Winsock2 + поллер, loopback; http_realtime — HTTP/2,
    WebSocket, SSE — один раз упал с connection reset в самом первом
    запуске на свежем префиксе Wine, в следующих 16 прошёл) —
    `compiler/tests/cross.rs`. Не
    применимы: cfg (Windows-вариант ссылается на несуществующий `winapi`),
    stdlib_time_os_fs (пути `\` и буквы дисков против Unix-путей в `.out`),
    sql (нет libsqlite3 для mingw — `std.database.sqlite` на Windows
    опционален: нужна своя сборка sqlite3 под mingw), cpuprofile (нет
    сэмплера).
  - C-тесты рантайма под Wine (`make -C runtime CROSS=x86_64-w64-mingw32
    test-wine`): проходят 18 из 19 — arena, crypto, error, http, infra, io,
    json, map, mem, panic, platform (потоки, локи, once/TLS, файлы с
    не-ASCII именами, `strftime`/`strptime`, поллер, ожидания
    готовности, сокеты), slice, std, std2, std3, std4, string, task;
    `test_net` написан на POSIX-сокетах и `fork`/сигналах и под Windows не
    собирается. Тестовый код (не рантайм) использует winpthreads и
    POSIX-шимы из `tests/test.h`; `run_child` вместо `fork` перезапускает
    тест с `SLANG_TEST_CHILD`. Wine нужен UTF-8-локаль (`LC_ALL=C.UTF-8`)
    для не-ASCII имён файлов.
  - Что сделано в Windows-слое: потоки `CreateThread`; часы
    `QueryPerformanceCounter` и `GetSystemTimePreciseAsFileTime`; энтропия
    `BCryptGenRandom`; backtrace `CaptureStackBackTrace` (модуль+смещение;
    символов нет: DbgHelp не читает DWARF); `LoadLibraryW`; argv в UTF-8
    из `CommandLineToArgvW`; консоль — `WriteConsoleW` с переносом
    неполных UTF-8-последовательностей между вызовами, `CP_UTF8` и
    `ENABLE_VIRTUAL_TERMINAL_PROCESSING` (ANSI-цвета); файлы через
    `CreateFileW` + `_open_osfhandle` (0/1/2 остаются стандартными
    потоками), UTF-16 и оба разделителя, `pread` без сдвига позиции;
    `std.fs` на Windows разбирает `/` и `\`, буквы дисков и отдаёт `\`;
    процессы `CreateProcessW` (кавычки по правилам `CommandLineToArgvW`,
    UTF-16-окружение), каналы ребёнка ретранслируются потоком в loopback-
    сокет, чтобы их видел поллер; сигналы — `SetConsoleCtrlHandler`
    (Ctrl+C/Break → SIGINT, close/logoff/shutdown → SIGTERM); часовой пояс —
    UTC, либо TZif из `$TZ` (абсолютный путь или имя в `$ZONEINFO`);
    пользователи — только текущий (`GetUserNameW`, `USERPROFILE`), групп
    нет; корневые сертификаты — только `$SSL_CERT_FILE`; профилировщик CPU
    — «не поддерживается».
  - Сеть (`net_win32.c`): `WSASocketW` (overlapped, не наследуется),
    `FIONBIO`, `getaddrinfo`/`getnameinfo`, `GetAdaptersAddresses`, опции
    (SO_REUSEADDR в смысле POSIX — поведение Windows по умолчанию, плюс
    `SO_EXCLUSIVEADDRUSE`; REUSEPORT и CORK — ENOPROTOOPT), AF_UNIX
    (Windows 10 1803+, без абстрактных имён), сообщения об ошибках как на
    Linux. `sendfile` — `pread`+`send`: `TransmitFile` на неблокирующем
    сокете требует OVERLAPPED-завершения и придёт вместе с IOCP.
  - Поллер v1 — `poll_wsapoll.c`: `WSAPoll` по взведённым сокетам и
    UDP-сокет пробуждения; one-shot — снятием взвода при отчёте; взвод
    во время ожидания будит ожидающий поток. Это O(взведённых сокетов) на
    ожидание — нормально для клиентов и умеренных серверов; IOCP-бэкенд
    (ниже) — для тысяч соединений, интерфейс тот же.
  - Размер hello world (`scripts/bench-size.sh`): `--release` для Windows —
    79 КБ (msvcrt.dll динамически, рантайм и CRT mingw статически), для
    Linux — 34 КБ; C hello через mingw — 14 КБ.
- **macos-arm64/amd64 — только объектные файлы**: `slang build --target
  macos-arm64 --emit=obj` даёт Mach-O arm64 (`-target`-машина LLVM с CPU
  `apple-m1`; явные секции и comdat для Mach-O не ставятся, мёртвый код
  убирает `ld64 -dead_strip`). Линковка требует macOS SDK (`$SDKROOT` или
  `xcrun`) и рантайм, собранный с ним (`make -C runtime` на Mac), — без них
  `slang build` объясняет, чего не хватает. **kqueue не запускался**: здесь
  нет macOS/BSD. `poll_kqueue.c` и BSD/macOS-ветки `net_posix.c`
  проверены только синтаксически (`-D__APPLE__` со стаб-заголовками), а
  macOS-ветки `posix.c`/`fs_posix.c` (`malloc_size`, `st_mtimespec`, поиск
  в `PATH` вместо `execvpe`) не компилировались вовсе; CI на macOS
  (`.github/workflows/ci.yml`) это закроет.
- Другие цели: `--target linux-arm64 --emit=obj` (ELF aarch64), линковка
  через `clang --target=<triple> -fuse-ld=lld` с `$SLANG_SYSROOT`.
- CI: `.github/workflows/ci.yml` — Linux amd64/arm64, macOS arm64/amd64,
  Windows (MSYS2 MINGW64: рантайм и C-тесты нативно, компилятор с LLVM 18),
  кросс-джоб на Linux (рантайм для Windows с `-Werror`, `test-wine`,
  `cargo test --test cross -- --include-ignored`, размер hello.exe).
  Здесь он не запускался.

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

- `lld` ещё не вшит: линковка через системный `clang` (драйвер находит
  CRT цели) и `ld.lld`/`lld-link`, если установлены.
- Условная компиляция (`compiler/src/cfg.rs`, `target.rs`): `@cfg` с
  `os`/`arch`/`feature`/`not`/`any`/`all` на объявлениях, членах и файлах,
  суффиксы имён файлов (в `driver::collect_files`, то есть и в пакетах, и
  в LSP), константы `os.Name`/`Arch`/`PathSeparator`/`LineSeparator` —
  все следуют `--target` и в `check`, и в `build`/`run`.

**Пробелы**: IOCP-поллер (шаги 1–4 выше), `TransmitFile`, сэмплер CPU для
Windows (`SuspendThread`/`GetThreadContext`), символы в backtrace
(DbgHelp + PDB для MSVC ABI), ABI MSVC целиком (рантайм под MSVC, Windows
SDK, `lld-link`, PDB), часовые пояса Windows (`GetDynamicTimeZoneInformation`
→ IANA), хранилище сертификатов Windows/keychain macOS, `test_net` для
Windows, macOS SDK для линковки и запуск на настоящих macOS/Windows.

## Этапы

1. ✅ Выделить `runtime/src/platform/` (posix.c, fs_posix.c), убрать прямые
   вызовы из остального рантайма; ✅ `@cfg` и файловые суффиксы в
   компиляторе; ✅ `os.Name/Arch`.
2. macOS arm64/amd64: kqueue-бэкенд планировщика, CI на macOS.
3. Windows amd64: ✅ win32.c (потоки, файлы, консоль UTF-8, Winsock,
   процессы, сигналы), ✅ PE/COFF через lld (MinGW ABI), поллер v1 на
   WSAPoll; IOCP, CodeView/MSVC ABI, CI на Windows — дальше.
4. Кросс-компиляция: ✅ `--target` в `build`/`run` (Windows через Wine,
   macOS — объектники), ✅ поиск рантайма `~/.slang/targets/<triple>/`;
   `slang target add`, прекомпилированные рантаймы, релизные сборки под
   все tier-1 — дальше.
5. linux-arm64, musl, FreeBSD; позже wasm32-wasi.
