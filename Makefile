CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Iinclude
LDLIBS  += -pthread

BIN      := webserver
SRCS     := $(wildcard src/*.c)
OBJS     := $(SRCS:src/%.c=build/%.o)
LIB_OBJS := $(filter-out build/main.o,$(OBJS))
TESTS    := $(patsubst tests/%.c,build/%,$(wildcard tests/test_*.c))

.PHONY: all test asan clean

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c $(wildcard include/*.h) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

build/test_%: tests/test_%.c $(LIB_OBJS) tests/test.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LIB_OBJS) $(LDLIBS)

build:
	mkdir -p build

test: $(TESTS)
	@set -e; for t in $(TESTS); do ./$$t; done

# Rebuild everything with AddressSanitizer + LeakSanitizer.
asan:
	$(MAKE) clean
	$(MAKE) all test CFLAGS="-std=c11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer"

clean:
	rm -rf build $(BIN)
