# Slang runtime library (v0.1)
#   make        -> build/libslang_rt.a (also copied to ./libslang_rt.a)
#   make test   -> build and run tests/*.c
#   make clean
#   make asan   -> clean rebuild + tests under AddressSanitizer
#   make tsan   -> build/tsan/: the poller/network/scheduler tests under ThreadSanitizer
#   CFLAGS_EXTRA=... is appended to CFLAGS (and to the coro test link, done with $(CC)),
#   e.g. make clean && make CFLAGS_EXTRA=-fsanitize=address test
#   make CROSS=x86_64-w64-mingw32 -> build/win64/libslang_rt.a (Windows, mingw ABI;
#               also copied to build/x86_64-pc-windows-gnu/ where `slang build
#               --target windows-amd64` looks), compiled with clang -target
#               $(CROSS) (or $(CROSS)-gcc without clang) against the mingw-w64 headers
#   make CROSS=x86_64-w64-mingw32 test-wine -> the C unit tests built for Windows
#               and run under wine (WINE=wine); prints which pass

CC      ?= cc
AR      ?= ar
CFLAGS  ?= -O2
CROSS   ?=
CLANG   ?= clang
HAVE_CLANG := $(shell command -v $(CLANG) >/dev/null 2>&1 && echo yes)

ifneq ($(CROSS),)
# Cross build. clang is preferred: one compiler for every target, and the
# same TLS model (native, not emutls) as the code LLVM generates for Slang.
ifeq ($(HAVE_CLANG),yes)
CC      := $(CLANG) -target $(CROSS)
AR      := $(shell command -v llvm-ar-18 >/dev/null 2>&1 && echo llvm-ar-18 || (command -v llvm-ar >/dev/null 2>&1 && echo llvm-ar || echo $(CROSS)-ar))
else
CC      := $(CROSS)-gcc
AR      := $(CROSS)-ar
endif
ifneq ($(findstring mingw,$(CROSS)),)
TARGET_OS := windows
BUILD   := build/win64
TRIPLE  := x86_64-pc-windows-gnu
EXE     := .exe
PICFLAG :=
OSFLAGS := -D_WIN32_WINNT=0x0A00
LDLIBS  = -lws2_32 -liphlpapi -lbcrypt -lshell32 -ladvapi32 -lm
RDYNAMIC :=
else
TARGET_OS := other
BUILD   := build/$(CROSS)
TRIPLE  := $(CROSS)
EXE     :=
PICFLAG := -fPIC
OSFLAGS :=
LDLIBS  = -lpthread -lm
RDYNAMIC := -rdynamic
endif
else
TARGET_OS := host
BUILD   := build
EXE     :=
PICFLAG := -fPIC
OSFLAGS :=
LDLIBS  = -lpthread -lm
RDYNAMIC := -rdynamic
endif

CFLAGS  += -std=c11 -Wall -Wextra -Werror $(PICFLAG) -fno-omit-frame-pointer -ffunction-sections -fdata-sections -D_GNU_SOURCE $(OSFLAGS) -Iinclude -g $(CFLAGS_EXTRA)
SRCS    := $(wildcard src/*.c) $(wildcard src/platform/*.c) $(wildcard src/crypto/*.c)
OBJS    := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))
LIB     := $(BUILD)/libslang_rt.a
TESTS   := $(patsubst tests/%.c,$(BUILD)/tests/%,$(wildcard tests/test_*.c))
# Extra environment for test_task only. `make asan` sets SLANG_NO_BACKTRACE=1:
# under ASan, backtrace() in a panicking fork()ed child can deadlock on the
# sanitizer allocator lock inherited from a worker thread (intermittent hang).
TASK_TEST_ENV ?=

.PHONY: all test clean asan tsan test-wine

ifeq ($(CROSS),)
all: libslang_rt.a

libslang_rt.a: $(LIB)
	cp $(LIB) $@
else
all: build/$(TRIPLE)/libslang_rt.a

build/$(TRIPLE)/libslang_rt.a: $(LIB)
	@mkdir -p build/$(TRIPLE)
	cp $(LIB) $@
endif

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

$(BUILD)/%.o: src/%.c include/slang_rt.h src/rt_internal.h src/platform/platform.h $(wildcard src/crypto/*.h) | $(BUILD) $(BUILD)/platform $(BUILD)/crypto
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/tests/%: tests/%.c tests/test.h $(wildcard tests/vectors/*.h) $(LIB) | $(BUILD)/tests
	$(CC) $(CFLAGS) $(RDYNAMIC) $< -o $@$(EXE) $(LIB) $(LDLIBS)

$(BUILD) $(BUILD)/tests $(BUILD)/platform $(BUILD)/crypto:
	mkdir -p $@

# Real LLVM-coroutine lowering example (tests/coro_lowering.ll), see ABI.md.
$(BUILD)/tests/coro_lowering: tests/coro_lowering.ll $(LIB) | $(BUILD)/tests
	$(CLANG) -O2 -Wno-override-module -c $< -o $@.o
	$(CC) $(CFLAGS_EXTRA) $@.o -o $@ $(LIB) $(LDLIBS)

ifeq ($(HAVE_CLANG),yes)
CORO_TEST := $(BUILD)/tests/coro_lowering
endif

test: $(TESTS) $(CORO_TEST)
	@set -e; pass=0; for t in $(TESTS); do \
	  echo "== $$t"; \
	  case "$$t" in *test_task) env $(TASK_TEST_ENV) ./$$t;; *) ./$$t;; esac; \
	  pass=$$((pass+1)); \
	done; \
	if [ -n "$(CORO_TEST)" ]; then \
	  echo "== $(CORO_TEST)"; \
	  out=$$(./$(CORO_TEST) | tr '\n' ' '); \
	  if [ "$$out" != "21 41 " ]; then echo "coro_lowering: unexpected output '$$out'"; exit 1; fi; \
	  echo "  ok  llvm.coro lowering prints 21 41"; pass=$$((pass+1)); \
	else echo "(clang not found: skipping coro_lowering)"; fi; \
	echo "ALL TESTS PASSED ($$pass binaries)"

clean:
	rm -rf $(BUILD) libslang_rt.a

# The C unit tests cross-built for Windows and run under wine (the test code
# itself may use mingw's winpthreads and POSIX shims from tests/test.h; the
# runtime does not). A test that
# does not build for Windows (POSIX-only test code) or fails is reported, not
# fatal; the summary line lists both. WINEDEBUG=-all keeps wine quiet.
WINE ?= wine
WINE_TESTS := $(patsubst tests/%.c,%,$(wildcard tests/test_*.c))
test-wine: $(LIB) | $(BUILD)/tests
	@if [ -z "$(CROSS)" ]; then echo "test-wine needs CROSS=x86_64-w64-mingw32"; exit 2; fi
	@WINEDEBUG=-all $(WINE) cmd /c "if not exist %TEMP% mkdir %TEMP%" >/dev/null 2>&1 || true  # fresh prefixes lack it; wine needs a UTF-8 locale for non-ASCII file names (LC_ALL below)
	@pass=""; fail=""; nobuild=""; for t in $(WINE_TESTS); do \
	  if ! $(CC) $(CFLAGS) -static tests/$$t.c -o $(BUILD)/tests/$$t.exe $(LIB) $(LDLIBS) -lpthread > $(BUILD)/tests/$$t.build.log 2>&1; then \
	    nobuild="$$nobuild $$t"; echo "== $$t: does not build for Windows (see $(BUILD)/tests/$$t.build.log)"; continue; fi; \
	  echo "== $$t (wine)"; \
	  if LC_ALL=C.UTF-8 WINEDEBUG=-all timeout 300 $(WINE) $(BUILD)/tests/$$t.exe > $(BUILD)/tests/$$t.log 2>&1; then \
	    pass="$$pass $$t"; tail -1 $(BUILD)/tests/$$t.log; \
	  else fail="$$fail $$t"; tail -5 $(BUILD)/tests/$$t.log; fi; \
	done; \
	echo "WINE PASS:$$pass"; echo "WINE FAIL:$$fail"; echo "WINE NOT BUILT:$$nobuild"; \
	[ -z "$$fail" ]

asan:
	$(MAKE) clean
	LSAN_OPTIONS=suppressions=$(CURDIR)/tests/lsan.supp:print_suppressions=0 \
	  $(MAKE) CFLAGS_EXTRA="-fsanitize=address -fno-omit-frame-pointer" \
	  TASK_TEST_ENV=SLANG_NO_BACKTRACE=1 test

# ThreadSanitizer run of the concurrency-heavy tests (sharded pollers, socket
# close races, resolver pool, scheduler) in its own build directory, so it
# does not disturb the normal build.
TSAN_TESTS ?= test_net test_platform test_task
tsan:
	$(MAKE) BUILD=build/tsan CFLAGS_EXTRA="-fsanitize=thread" $(addprefix build/tsan/tests/,$(TSAN_TESTS))
	@set -e; for t in $(TSAN_TESTS); do \
	  echo "== tsan $$t"; \
	  TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1 suppressions=$(CURDIR)/tests/tsan.supp" \
	    SLANG_NO_BACKTRACE=1 ./build/tsan/tests/$$t; \
	done; echo "TSAN TESTS PASSED"
