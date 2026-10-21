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
  скачиваются `slang target add windows-amd64` (см. «Установка и цели»).
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
- Установка: `curl ... | sh` (Linux/macOS, `scripts/install.sh`), `irm ...
  | iex` (PowerShell, `scripts/install.ps1`) — сделано, см. «Установка и
  цели» ниже; Homebrew, winget, apt/rpm-репозитории, `slang self update` —
  позже.
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

Что работает на каждой платформе и **как это проверено** (здесь — Linux
x86-64 с clang-18, mingw-w64, Wine 9.0, qemu-user 8, musl-tools; macOS и
настоящего Windows нет):

| Платформа | Рантайм (C-тесты) | Примеры (`.out` байт-в-байт) | Как проверено |
|---|---|---|---|
| linux-amd64 | 19/19 `make -C runtime test` | 49/49 (`cargo test`) | нативно |
| linux-arm64 | 19/19 `make -C runtime CROSS=aarch64-linux-gnu test-cross` | 47/49 (`--leak-check`) | кросс-сборка + `qemu-aarch64-static -L /usr/aarch64-linux-gnu`; CI — ещё и нативно на `ubuntu-24.04-arm` |
| linux-amd64-musl | 19/19 `make -C runtime CROSS=x86_64-linux-musl test-cross` | 47/49, полностью статические | нативно (статический ELF без `PT_INTERP`) |
| windows-amd64 (MinGW) | 19/19 `make -C runtime CROSS=x86_64-w64-mingw32 test-wine` (IOCP и `SLANG_POLLER=wsapoll`) | 38 + 5 сетевых (IOCP и WSAPoll) + sql с `sqlite3.dll` | Wine 9.0 |
| macos-arm64 / amd64 | все 94 файла и C-тесты компилируются (`-Werror`) против `runtime/stub-sdk/macos` | 49/49 → Mach-O объекты | без SDK: ничего не слинковано и не запущено; на Mac — `scripts/macos-verify.sh` |

Не прошедшие 2 из 49 на arm64/musl — `sql` и `web_app`: им нужен
`libsqlite3` цели (в кросс-sysroot arm64 его нет, статического для musl
тоже; рантайм musl собран без `<sqlite3.h>`, `std.database.sqlite`
отвечает «not available»). Всё перечисленное — `compiler/tests/cross.rs`
(тесты пропускаются с объяснением, если инструмента нет) и кросс-джоб CI.

Размер hello world (`scripts/bench-size.sh`, `--release`): linux-amd64 —
34 КБ (glibc динамически; `--static` — 843 КБ), **linux-amd64-musl — 70 КБ
полностью статический** (C hello через musl-gcc — 17 КБ), linux-arm64 —
23 КБ, windows-amd64 — 81 КБ (msvcrt.dll динамически, рантайм и CRT
mingw статически; C hello — 14 КБ).

- **Слой `runtime/src/platform/` завершён**: вне `platform/` в рантайме нет
  ни одного `#ifdef` ОС и ни одного прямого вызова POSIX/Win32
  (`runtime/ABI.md` §15). Через слой идут потоки, мьютексы/условные
  переменные/once/TLS (`slang__plat_mutex` и т. д.: pthreads inline в
  `platform.h`; SRWLOCK/CONDITION_VARIABLE/INIT_ONCE/FLS в `win32.c`; имени
  `slang_mutex` нет — оно занято ABI-типом `Mutex`), часы (`mono_ns`,
  `wall_ns`, `sleep_ns`), календарь (`utc_tm`/`timegm` на чистом C),
  `strftime`/`strptime` (свои в `timefmt.c` там, где libc неполна:
  Windows и musl — `SLANG_PLAT_LIBC_TIMEFMT`), файлы и каталоги,
  процессы, сигналы, пользователи, окружение, консоль, aligned-память,
  сокеты, поллер, backtrace с именами функций, сэмплер CPU, часовые пояса,
  хранилище корневых сертификатов, `dlopen`.

### windows-amd64 (MinGW ABI): кросс-сборка с Linux, запуск под Wine

- `make -C runtime CROSS=x86_64-w64-mingw32` собирает
  `runtime/build/x86_64-pc-windows-gnu/libslang_rt.a` через `clang -target
  x86_64-w64-mingw32` с `-Wall -Wextra -Werror` (без clang —
  `x86_64-w64-mingw32-gcc`; clang предпочтителен: тот же native TLS, что у
  кода LLVM, gcc для mingw использует emutls).
- `slang build --target windows-amd64 x.sl` → `x.exe` (PE32+, COFF от LLVM,
  соглашение Win64; на границе ABI только скаляры и указатели). Линковка:
  `clang -target x86_64-w64-mingw32 -fuse-ld=lld` + рантайм + ws2_32,
  iphlpapi, bcrypt, shell32, advapi32; ntdll, crypt32, dbghelp и
  sqlite3/winsqlite3 загружаются во время выполнения (`LoadLibraryW`/
  `GetProcAddress`), импорт-библиотеки им не нужны. `slang run --target
  windows-amd64` запускает через Wine (`$SLANG_WINE`).
- Под Wine 9.0 байт-в-байт с Linux-ными `.out` (без нормализации CRLF:
  `println` пишет `\n`, stdout/stderr в двоичном режиме): 38 примеров в
  отладочной сборке с `--leak-check` (hello, arith, control, functions,
  structs, switch, enums, slices_maps, strings, interfaces, errors,
  errors_trace, defer, bank, async, cancel, channels, drop, generics, weak,
  json, reflection, regex, sealed_shapes, try_block, task_context, tracing,
  log, metrics, cli_tool, stdlib_* ×6, multi, **cpuprofile**), 7 в
  `--release`, сетевые tcp_echo, http_server, tls_echo, redis,
  http_realtime — с IOCP-поллером и ещё раз с `SLANG_POLLER=wsapoll`;
  `sql` — с `sqlite3.dll` рядом с программой (`$SLANG_TEST_SQLITE3_DLL`,
  собран из amalgamation через mingw); процессы, часовые пояса, корневые
  сертификаты и backtrace — отдельным тестом (`cross.rs`
  `windows_processes_zones_roots_backtraces`, вывод совпадает с Linux).
  Не применимы: cfg (Windows-вариант ссылается на несуществующий `winapi`),
  stdlib_time_os_fs (пути `\` и буквы дисков против Unix-путей в `.out`).
- C-тесты рантайма под Wine: **19 из 19** (в т. ч. `test_net`: он переписан
  на сокеты платформенного слоя и собирается для Windows; AF_UNIX в Wine 9
  нет — sendfile там проверяется по loopback TCP; Windows 10 1803+ AF_UNIX
  умеет). Тестовый код (не рантайм) использует winpthreads и POSIX-шимы из
  `tests/test.h`; `run_child` вместо `fork` перезапускает тест с
  `SLANG_TEST_CHILD`. Wine нужна UTF-8-локаль (`LC_ALL=C.UTF-8`).
- Что есть в Windows-слое: потоки `CreateThread`; часы
  `QueryPerformanceCounter`/`GetSystemTimePreciseAsFileTime`; энтропия
  `BCryptGenRandom`; `LoadLibraryW`; argv в UTF-8 из `CommandLineToArgvW`;
  консоль — `WriteConsoleW` с переносом неполных UTF-8-последовательностей,
  `CP_UTF8` и ANSI-цвета; файлы через `CreateFileW` + `_open_osfhandle`,
  UTF-16 и оба разделителя, `pread` без сдвига позиции; `std.fs` разбирает
  `/` и `\`, буквы дисков и отдаёт `\`; сигналы — `SetConsoleCtrlHandler`;
  пользователи — только текущий (`GetUserNameW`, `USERPROFILE`), групп нет.
  Новое:
  - **Поллер IOCP** (`poll_iocp.c`, ниже) вместо WSAPoll; WSAPoll остался
    запасным (`poll_wsapoll.c`: если `\Device\Afd` не открывается, или
    `SLANG_POLLER=wsapoll`). Исправлено в нём: `remove` теперь будит
    ожидающий `WSAPoll` и ждёт его возврата, прежде чем вызывающий закроет
    сокет — под Wine закрытие сокета внутри идущего `WSAPoll` вешало
    стресс-тест `test_net` (раньше `test_net` под Windows не собирался, и
    этого не было видно).
  - **`SendFile` — `TransmitFile`** (указатель через
    `SIO_GET_EXTENSION_FUNCTION_POINTER`, без mswsock.lib): OVERLAPPED с
    событием, по куску до 64 КБ за вызов (вызывающий уже дождался
    готовности к записи, ядро берёт кусок сразу; если пир не читает 30 с —
    `CancelIoEx`, отдаётся отправленное). Запасной путь — `pread`+`send`.
  - **Процессы без потоков-ретрансляторов**: с IOCP-поллером каналы
    ребёнка — именованные каналы (`\\.\pipe\slang-<pid>-<n>`, один
    экземпляр, `PIPE_REJECT_REMOTE_CLIENTS`), родительский конец
    OVERLAPPED. Дескриптор — `0x400000 + слот` таблицы каналов (не путается
    с сокетами, CRT-дескрипторами и HANDLE). Готовность к чтению —
    нулевое OVERLAPPED-чтение `ReadFile` (как «zero read» в libuv), его
    завершение приходит в порт шарда; чтение неблокирующее
    (`PeekNamedPipe` + `ReadFile` того, что есть, EOF по
    `ERROR_BROKEN_PIPE`); запись отдаёт до 64 КБ OVERLAPPED-`WriteFile` и
    сразу возвращается, следующая ждёт (EAGAIN) завершения предыдущей, а
    готовность к записи — это завершение записи «в полёте». С WSAPoll
    остаются ретрансляторы (WSAPoll видит только сокеты). Проверено под
    Wine (нулевое чтение ждёт данных и завершается в порт, как в Windows):
    3 МБ в stdin ребёнка, stdout и stderr — в обоих режимах.
  - **Сэмплер CPU** (`prof_win32.c`): поток-сэмплер с
    высокоточным waitable-таймером `hz` раз в секунду; по
    `GetThreadTimes` считает, сколько CPU потратил каждый поток процесса
    (список — снимок Toolhelp раз в 10 тиков) и берёт по сэмплу на каждые
    1/hz CPU-секунды: `SuspendThread` → `GetThreadContext` → раскрутка
    стека `RtlLookupFunctionEntry`/`RtlVirtualUnwind` по `.pdata` (цепочка
    rbp кода Win64 ненадёжна: clang ставит rbp со смещением) →
    `ResumeThread`. Пока поток заморожен, сэмплер не выделяет память и не
    берёт локов (раскрутчик может взять лок таблиц функций загрузчика — это
    важно только для JIT с динамическими таблицами). Профиль — тот же
    pprof, что на Linux (`examples/cpuprofile.sl` проходит;
    `go tool pprof -top` показывает функции Slang).
  - **Имена в backtrace и профиле**: своя COFF-таблица символов exe (lld и
    ld пишут её для mingw, если не `-s`: все функции, локальные тоже — как
    ELF `.symtab` на Linux), затем DbgHelp `SymFromAddr` (загружается по
    требованию: экспорт и PDB системных DLL, под Wine — и DWARF). Формат
    как у glibc: `app.exe(slang_main__inner+0x101) [0x14002f7f1]`.
    `--release` (без символов) — модуль+смещение. Тот же формат и та же
    таблица символов теперь и на Linux/macOS/musl (`posix.c`: ELF
    `.symtab`, Mach-O `LC_SYMTAB` из `__LINKEDIT`; без `<execinfo.h>`, как
    в musl, — проход по frame pointer).
  - **Часовые пояса** (`tz_win32.c`): правила из реестра
    (`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Time Zones\<ключ>`:
    `TZI` и `Dynamic DST` по годам) превращаются в TZif v2 (переходы за
    годы из `Dynamic DST` + POSIX-строка текущего правила для будущих лет),
    который `time.sl` разбирает как файл tzdata. Локальная зона —
    `GetDynamicTimeZoneInformation` (`TimeZoneKeyName`; выключенный
    переход на летнее время — круглый год стандартное). IANA-имена
    (`time.LoadLocation("Europe/Berlin")`) — по встроенной таблице из 182
    имён для 139 ключей Windows (CLDR `windowsZones.xml`, территория 001,
    и частые синонимы) с аббревиатурами (`CET`/`CEST`; у Windows только
    длинные имена; без известных букв — числовые `+03`, как в tzdata). `$TZ` с именем и
    `$ZONEINFO` с каталогом tzdata имеют приоритет. Под Wine 9 вывод
    совпадает с Linux-tzdata для 9 зон × 6 моментов (в т. ч. южное
    полушарие и St_Johns −3:30), локальная зона — по `TZ` хоста.
    Ограничение: история до первого года `Dynamic DST` неизвестна (раньше —
    стандартное смещение этого года).
  - **Корневые сертификаты**: хранилище `ROOT` (`CertOpenSystemStoreW`,
    crypt32 по требованию) выгружается раз за процесс в PEM-файл
    `%LOCALAPPDATA%\slang\roots.pem` (атомарная замена), путь которого и
    отдаётся `x509.SystemRoots` (кэш по размеру/mtime работает как с любым
    бандлом). `$SSL_CERT_FILE` — в приоритете. Оговорка: Windows
    докачивает редкие корни по требованию (Automatic Root Update), корня,
    который ни разу не использовался на машине, в хранилище может не быть.
  - **SQLite**: `std.database.sqlite` на Windows связывается во время
    выполнения (`platform/sqlite_dyn.h`, `sqlite_win32.c`): сначала
    `sqlite3.dll` приложения (рядом с exe или в `PATH`), затем системный
    `winsqlite3.dll` (Windows 10+, System32); без них `rtAvailable()` —
    false и `Open` возвращает «not available». `-lsqlite3` для Windows не
    передаётся. `sqlite3_changes64` (3.37+) — с откатом на
    `sqlite3_changes`.
- Сеть (`net_win32.c`): `WSASocketW` (overlapped, не наследуется),
  `FIONBIO`, `getaddrinfo`/`getnameinfo`, `GetAdaptersAddresses`, опции
  (SO_REUSEADDR в смысле POSIX — поведение Windows по умолчанию, плюс
  `SO_EXCLUSIVEADDRUSE`; REUSEPORT и CORK — ENOPROTOOPT), AF_UNIX
  (Windows 10 1803+, без абстрактных имён), сообщения об ошибках как на
  Linux.

### Асинхронный ввод-вывод на Windows: IOCP за интерфейсом готовности

Интерфейс (`platform.h`) — **готовность** (readiness): `poller_arm(fd,
READ|WRITE, token)` просит одно уведомление, `poller_wait` отдаёт
`{token, events}`, после чего вызывающий сам делает неблокирующий
`recv`/`send`, а при `EAGAIN` снова ждёт. epoll и kqueue ложатся на него
напрямую; IOCP — модель **завершения**. Выбран адаптер готовности (а не
перевод сокетов на настоящий overlapped-ввод-вывод с байтами в
завершении): он меньше, сохраняет ABI и весь код `task_io.c`/`std_net.c`,
а Slang-код и так видит только «результат или ждать». `poll_iocp.c`:

1. **Порт на шард.** Каждый поллер (= шард `task_io.c`, как шарды epoll)
   создаёт свой порт завершения и открывает свой хэндл драйвера AFD
   (`\Device\Afd` — ядро Winsock; `NtCreateFile` из ntdll по
   `GetProcAddress`), привязанный к порту.
2. **Готовность — `IOCTL_AFD_POLL`** (как wepoll, mio/tokio, `uv_poll` в
   libuv): взвод сокета — одна операция опроса по его базовому хэндлу
   (`SIO_BASE_HANDLE`) с событиями AFD интереса (receive/accept/disconnect
   для READ, send для WRITE, abort/connect-fail/local-close для обоих).
   Драйвер завершает её, как только сокет в этом состоянии
   (level-triggered: уже имеющаяся готовность — сразу), завершение
   приходит в порт с записью сокета в качестве контекста — это и есть
   one-shot-уведомление. Нулевые `WSARecv`/`WSASend` отвергнуты: нулевой
   `WSASend` завершается сразу, есть место в буфере или нет (холостой цикл
   на полном сокете), а нулевой `WSARecv` не сообщает о `accept`.
3. **Смена интереса и снятие.** Если взвод добавляет направление, пока
   опрос в полёте (писатель к читателю), опрос отменяется (`CancelIoEx`), и
   его отменённое завершение перевзводит с текущим интересом. `remove`
   отменяет и освобождает запись по последнему завершению (до него
   `IO_STATUS_BLOCK` принадлежит ядру); токены с поколением отбрасывают
   устаревшие отчёты, как раньше.
4. **Пробуждение** — `PostQueuedCompletionStatus` со служебным ключом (как
   eventfd), схлопывается флагом; ожидание — `GetQueuedCompletionStatusEx`
   пачкой до 256. **Таймеры** — прежние кучи дедлайнов `task_io.c`
   (таймаут ожидания), как на Linux.
5. **Каналы процессов** ждутся тем же портом (см. выше): нулевое
   OVERLAPPED-чтение для READ, завершение записи «в полёте» для WRITE.
6. `AcceptEx`/`ConnectEx` не нужны: `accept`/`connect` неблокирующие, а
   их готовность сообщает AFD (`AFD_POLL_ACCEPT`, `AFD_POLL_SEND` /
   `AFD_POLL_CONNECT_FAIL`).

Под Wine 9.0 `IOCTL_AFD_POLL` работает (Wine сам реализует на нём
`WSAPoll`): `test_net` (one-shot, пробуждения, шарды, гонки закрытия,
2400 сокетных пар в 8 потоках, sendfile) и все сетевые примеры проходят с
обоими бэкендами. Бенчмарки под Wine (только ориентир: Wine, общая машина
с другими процессами, разброс между прогонами до 2×), `--release`:

| | IOCP (AFD) | WSAPoll |
|---|---|---|
| `tcp_echo` (1000 соединений × 1 МБ) | 118–219 МБ/с | 67–90 МБ/с |
| `http_hello` (64 соединения, 200 000 запросов) | 1670–2810 запросов/с | 2210–2510 запросов/с |

`tcp_echo` выигрывает за счёт того, что ожидание не перебирает 1000+
взведённых сокетов; `http_hello` на 64 соединениях упирается в другое
(эмуляция Wine), разница в пределах шума. Настоящие числа — на
Windows (CI-джоб windows-amd64).

### linux-arm64 и musl

- `make -C runtime CROSS=aarch64-linux-gnu` (clang `-target
  aarch64-linux-gnu` находит sysroot `gcc-aarch64-linux-gnu`) →
  `runtime/build/aarch64-unknown-linux-gnu/`; `slang build --target
  linux-arm64` линкует `clang --target=aarch64-unknown-linux-gnu
  -fuse-ld=lld` (с `$SLANG_SYSROOT`, если задан), `slang run --target
  linux-arm64` запускает через `qemu-aarch64[-static] -L
  /usr/aarch64-linux-gnu` (`$SLANG_QEMU`, `$SLANG_SYSROOT`). CI: нативный
  джоб `ubuntu-24.04-arm` (полный `cargo test`) и кросс-джоб (C-тесты и
  примеры под QEMU).
- `--target linux-amd64-musl` (`x86_64-unknown-linux-musl`; также
  `linux-arm64-musl` при наличии musl-sysroot): всегда `-static`.
  Рантайм — `make -C runtime CROSS=x86_64-linux-musl` (clang с
  заголовками musl-tools), линковка — `musl-gcc` (`$SLANG_MUSL_CC`), если
  он для того же CPU, иначе clang с `--sysroot=$SLANG_SYSROOT`. musl нет
  `<execinfo.h>` (backtrace — по frame pointer), его `strftime` без
  `%k`/`%l`, `strptime` без `%z` (используются свои из `timefmt.c`);
  `-lsqlite3` не передаётся.

### macOS: что сделано без Mac и что проверить на настоящем

Сделано: `make -C runtime CROSS=aarch64-apple-darwin` (и
`x86_64-apple-darwin`) без SDK компилирует **все 94 файла рантайма и все
C-тесты** (`-Wall -Wextra -Werror`, `clang -target arm64-apple-macos12
-nostdlibinc`) против стаб-заголовков `runtime/stub-sdk/macos/include/`
(75 заголовков, только используемые объявления, по публичным заголовкам
SDK: `sys/event.h`, `mach-o/*`, `malloc/malloc.h`, `pthread.h`,
`sys/socket.h`, `sys/_mcontext.h`…). Объекты кладутся в
`build/<triple>-stubcheck/` и никогда не линкуются (архива в
`build/<triple>/` нет, `slang build` его не найдёт). С `SDKROOT` или на Mac
та же команда собирает настоящий рантайм. Найдено и исправлено:
`SOCK_NONBLOCK` в тестах (его нет на macOS), неиспользуемая переменная в
ветке символов; macOS-ветки `posix.c`/`fs_posix.c`/`net_posix.c` и
`poll_kqueue.c` компилируются. Добавлено: символы из `LC_SYMTAB` главного
образа (`_dyld_get_image_header` + `__LINKEDIT`) для профиля и backtrace,
`exe_range` для pprof; корни — `/etc/ssl/cert.pem` (поддерживается macOS;
пользовательские корни Keychain не входят). `slang build --target
macos-arm64|macos-amd64 --emit=obj` даёт Mach-O для всех 49 примеров.

Не проверено (нужен настоящий Mac; `scripts/macos-verify.sh` делает всё
это и печатает PASS/FAIL по пунктам, macOS-джоб CI его запускает):

1. Сборка рантайма с настоящим SDK и `make test` (19 тестов): поведение
   kqueue (one-shot через `EV_DISPATCH`, `EVFILT_USER`-пробуждение,
   EV_EOF/ошибки), `pthread_cond_timedwait` с `CLOCK_REALTIME`,
   `malloc_size`, `st_mtimespec`, поиск в `PATH` вместо `execvpe`,
   `sendfile(2)` BSD-сигнатуры, `SO_NOSIGPIPE`, `TCP_KEEPALIVE`;
   `make tsan`.
2. Расположение структур и констант стабов против SDK (проверка — тот же
   стаб-check на Mac с `SDKROOT=`: должен проходить и дальше).
3. Линковка `slang build` (`-isysroot`, `ld64`/`ld64.lld`,
   `-dead_strip`) и `cargo test --workspace` (все примеры байт-в-байт).
4. Сэмплер `SIGPROF` (`ITIMER_PROF`, регистры из `__darwin_mcontext64`)
   и символы `LC_SYMTAB` — `examples/cpuprofile.sl` и backtrace с именами.
5. Часовые пояса из `/usr/share/zoneinfo`/`/etc/localtime`, корни из
   `/etc/ssl/cert.pem`, `std.process` (`posix_spawn`), сетевые примеры.
6. `scripts/package-toolchain.sh` на Mac (архив `slang-<ver>-macos-<arch>`).

### Установка и цели (дистрибуция тулчейна)

- `scripts/package-toolchain.sh [--no-build] [--out dist]` →
  `slang-<версия>-<хост>.tar.gz` (`bin/slang`, `bin/slang-lsp`, `std/`,
  `targets/<triple хоста>/libslang_rt.a`, `tools/` — формат-скрипты
  lldb/gdb и клиент VS Code, `README.md`, `VERSION`) и по
  `slang-runtime-<версия>-<triple>.tar.gz` (распаковывается в
  `<triple>/libslang_rt.a`) на каждый рантайм из `runtime/build/<triple>/`,
  плюс `.sha256` и `VERSION`. CI выкладывает их артефактами.
- Компилятор ищет рантайм хоста в `$SLANG_RUNTIME`, затем
  `<exe>/../targets/<triple>/` установленного тулчейна,
  `$SLANG_TARGETS/<triple>/`, затем в репозитории; `std/` — в
  `$SLANG_STD`, `<exe>/../std`, репозитории (иначе встроенные копии).
- `slang target list` — известные цели (tier 1–2) и где лежит их рантайм;
  `slang target add <цель>... [--url шаблон]` скачивает
  `slang-runtime-<версия>-<triple>.tar.gz` в `$SLANG_TARGETS` (по
  умолчанию `$SLANG_HOME/targets` = `~/.slang/targets`), сверяет с
  `<url>.sha256`, если он есть, и распаковывает системным `tar`. Шаблон —
  `$SLANG_TARGETS_URL`, по умолчанию
  `https://github.com/dvislobokov/slang/releases/download/v{version}/slang-runtime-{version}-{triple}.tar.gz`
  (`{version}`, `{triple}`; `https://` — через curl, `http://` и `file://`
  — встроенно, путь без схемы — локальный архив).
- `scripts/install.sh` (Linux/macOS: `curl … | sh`, `--version`,
  `--dir`, `--bin`, `--from <архив>`, `$SLANG_DIST_URL`) распаковывает в
  `~/.slang/toolchain/slang-<ver>-<host>/`, ставит ссылку `current` и
  ссылки `slang`/`slang-lsp` в `~/.local/bin`; `scripts/install.ps1`
  (Windows, `irm … | iex`; здесь не запускался) — в
  `%USERPROFILE%\.slang\toolchain`, `bin\` в пользовательский `PATH`,
  распаковка встроенным `tar.exe`.
- Проверено: архив распакован в чистый каталог, `env -i … SLANG_HOME=…
  bin/slang build hello.sl` берёт рантайм и `std/` из тулчейна (видно в
  `-v`), `slang target add windows-amd64 linux-amd64-musl` с
  `file://`-шаблоном ставит рантаймы в `$SLANG_HOME/targets`, после чего
  `hello.exe` работает под Wine, а musl-бинарник — статически;
  `install.sh --from` и `SLANG_DIST_URL=file://…` ставят и запускают.
  Компилятору для линковки по-прежнему нужен системный `clang` (`lld` для
  кросс-целей): `lld` в бинарник не вшит.

### Общее

- Условная компиляция (`compiler/src/cfg.rs`, `target.rs`): `@cfg` с
  `os`/`arch`/`feature`/`not`/`any`/`all` на объявлениях, членах и файлах,
  суффиксы имён файлов (в `driver::collect_files`, то есть и в пакетах, и
  в LSP), константы `os.Name`/`Arch`/`PathSeparator`/`LineSeparator` —
  все следуют `--target` и в `check`, и в `build`/`run`.
- CI (`.github/workflows/ci.yml`): Linux amd64/arm64 (нативно, артефакт
  тулчейна), macOS arm64/amd64 (`cargo test`, `scripts/macos-verify.sh
  --quick`, артефакт), Windows (MSYS2 MINGW64: рантайм и C-тесты нативно с
  IOCP и с WSAPoll, компилятор с LLVM 18), кросс-джоб на Linux (Windows +
  Wine, linux-arm64 + QEMU, musl, стаб-check macOS, `cross.rs` целиком,
  размеры, архивы рантаймов). Здесь он не запускался.

**Пробелы**: ABI MSVC целиком (рантайм под MSVC, Windows SDK, `lld-link`,
PDB/CodeView-символы); запуск на настоящих macOS и Windows (всё выше —
Wine и стабы); Keychain macOS (только `/etc/ssl/cert.pem`); история
часовых поясов Windows до `Dynamic DST`; `libsqlite3` для кросс-целей
linux-arm64/musl; `lld` не вшит; `AcceptEx`/`ConnectEx` и настоящий
completion-путь передачи данных (`ReadInto`/`Write` прямо в буфер) — не
нужны для корректности, могут дать производительность; FreeBSD и
wasm32-wasi.

## Этапы

1. ✅ Выделить `runtime/src/platform/` (posix.c, fs_posix.c), убрать прямые
   вызовы из остального рантайма; ✅ `@cfg` и файловые суффиксы в
   компиляторе; ✅ `os.Name/Arch`.
2. macOS arm64/amd64: kqueue-бэкенд планировщика ✅ (компилируется против
   стабов), символы `LC_SYMTAB` ✅; проверка на Mac и CI на macOS —
   `scripts/macos-verify.sh`.
3. Windows amd64: ✅ win32.c (потоки, файлы, консоль UTF-8, Winsock,
   процессы, сигналы), ✅ PE/COFF через lld (MinGW ABI), ✅ поллер IOCP
   (AFD poll) с запасным WSAPoll, ✅ `TransmitFile`, ✅ каналы процессов
   через IOCP, ✅ сэмплер CPU, ✅ символы, ✅ часовые пояса, ✅ хранилище
   сертификатов, ✅ sqlite через DLL; CodeView/MSVC ABI — дальше.
4. Кросс-компиляция: ✅ `--target` в `build`/`run` (Windows через Wine,
   linux-arm64 через QEMU, macOS — объектники), ✅ поиск рантайма в
   `~/.slang/targets/<triple>/` и в тулчейне, ✅ `slang target add/list`,
   ✅ `package-toolchain.sh`, `install.sh`/`install.ps1`; релизные сборки
   под все tier-1 — по артефактам CI.
5. ✅ linux-arm64 (QEMU + нативный CI), ✅ linux-musl (статический); FreeBSD,
   позже wasm32-wasi.
