CC = cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Isrc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror -pthread
LDLIBS += -pthread

ifeq ($(shell uname -s),Darwin)
CPPFLAGS += -D_DARWIN_C_SOURCE
endif

SOURCES = src/main.c src/event_loop.c src/connection.c $(COMMON)
COMMON = src/ketama.c src/proxy_policy.c src/net.c src/http.c src/handlers.c
UNIT_SOURCES = tests/unit.c tests/proxy_flow.c src/connection.c $(COMMON)
HEADERS = src/proxy_policy.h src/net.h src/http.h src/handlers.h src/connection.h src/event_loop.h
SANITIZERS = -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined

.PHONY: all test sanitize clean
all: server

server: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

tests/unit: $(UNIT_SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(UNIT_SOURCES) $(LDLIBS)

server-asan: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) $(LDFLAGS) -o $@ $(SOURCES) $(LDLIBS)

tests/unit-asan: $(UNIT_SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SANITIZERS) $(LDFLAGS) -o $@ $(UNIT_SOURCES) $(LDLIBS)

test: server tests/unit
	./tests/unit
	python3 tests/integration.py ./server

sanitize: server-asan tests/unit-asan
	./tests/unit-asan
	python3 tests/integration.py ./server-asan

clean:
	rm -f server server-asan tests/unit tests/unit-asan
	rm -rf server.dSYM server-asan.dSYM tests/unit.dSYM tests/unit-asan.dSYM
