# Slang runtime library (v0.1)
#   make        -> build/libslang_rt.a (also copied to ./libslang_rt.a)
#   make test   -> build and run tests/*.c
#   make clean

CC      ?= cc
AR      ?= ar
CFLAGS  ?= -O2
CFLAGS  += -std=c11 -Wall -Wextra -Werror -fPIC -D_GNU_SOURCE -Iinclude -g
LDLIBS  = -lpthread

BUILD   := build
SRCS    := $(wildcard src/*.c)
OBJS    := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))
LIB     := $(BUILD)/libslang_rt.a
TESTS   := $(patsubst tests/%.c,$(BUILD)/tests/%,$(wildcard tests/test_*.c))
CLANG   ?= clang
HAVE_CLANG := $(shell command -v $(CLANG) >/dev/null 2>&1 && echo yes)

.PHONY: all test clean

all: libslang_rt.a

libslang_rt.a: $(LIB)
	cp $(LIB) $@

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

$(BUILD)/%.o: src/%.c include/slang_rt.h src/rt_internal.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/tests/%: tests/%.c tests/test.h $(LIB) | $(BUILD)/tests
	$(CC) $(CFLAGS) -rdynamic $< -o $@ $(LIB) $(LDLIBS)

$(BUILD) $(BUILD)/tests:
	mkdir -p $@

# Real LLVM-coroutine lowering example (tests/coro_lowering.ll), see ABI.md.
$(BUILD)/tests/coro_lowering: tests/coro_lowering.ll $(LIB) | $(BUILD)/tests
	$(CLANG) -O2 -Wno-override-module $< -o $@ $(LIB) $(LDLIBS)

ifeq ($(HAVE_CLANG),yes)
CORO_TEST := $(BUILD)/tests/coro_lowering
endif

test: $(TESTS) $(CORO_TEST)
	@set -e; pass=0; for t in $(TESTS); do \
	  echo "== $$t"; ./$$t; pass=$$((pass+1)); \
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
