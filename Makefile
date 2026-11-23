CC = gcc
CFLAGS = -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -g -Wall -Wextra -Werror \
	-pthread -Isrc -Isrc/core -Isrc/rproxy
SANITIZE = -fsanitize=address,undefined -g
TIDY_FLAGS = -std=c11 -D_POSIX_C_SOURCE=200809L -Isrc -Isrc/core -Isrc/rproxy

CORE_SRCS = src/core/http_parse.c src/core/router.c src/core/mime.c \
	src/core/response.c src/core/serve.c src/core/handlers.c
SERVER_SRCS = $(CORE_SRCS) src/send_plan.c src/conn.c src/proxy.c src/cache.c src/main.c \
	src/model_iter.c src/model_fork.c src/model_pool.c src/model_epoll.c
HEADERS = $(wildcard src/*.h src/core/*.h src/rproxy/*.h)
TESTS = http_parse router mime response serve
TEST_BINS = $(addprefix build/test_,$(TESTS)) build/test_cache \
	build/test_client_hello build/test_ja4
FORMAT_FILES = $(wildcard src/*.c src/*.h src/core/*.c src/core/*.h \
	src/rproxy/*.c src/rproxy/*.h tests/*.c tests/*.h)

# A 1x1 PNG.
PNG_BASE64 = iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==
LARGE_BIN_SIZE = 10485760
# The Shared core does not use sockets or the I/O layer
# (docs/adr/shared-core-does-not-know-sockets.md).
# The decision modules of the Reverse Proxy use no sockets and no OpenSSL I/O
# (docs/adr/reverse-proxy-is-a-separate-process.md).
DECISION_FILES = $(wildcard $(addprefix src/rproxy/,client_hello.* ja4.* \
	rate_limit.* verdict.*))
DECISION_BANNED_INCLUDE = '\#include [<"](sys/socket|sys/epoll|poll|netinet/[a-z_]+|arpa/inet|openssl/ssl|unistd)\.h[>"]'
CORE_BANNED_INCLUDE = '\#include [<"](sys/socket|sys/epoll|sys/select|poll|netinet/[a-z_]+|arpa/inet|conn|send_plan|model)\.h[>"]'

.PHONY: all test asan www format lint clean

all: server

server: $(SERVER_SRCS) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(SERVER_SRCS)

asan: server-asan

server-asan: $(SERVER_SRCS) $(HEADERS)
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ $(SERVER_SRCS)

build/test_%: tests/test_%.c tests/check.h $(CORE_SRCS) $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ $< $(CORE_SRCS)

# The decision modules of the Reverse Proxy.
build/test_client_hello: tests/test_client_hello.c tests/check.h \
		src/rproxy/client_hello.c $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ $< src/rproxy/client_hello.c

# ja4 uses SHA256() of OpenSSL (docs/adr/openssl-for-the-reverse-proxy.md).
build/test_ja4: tests/test_ja4.c tests/check.h src/rproxy/ja4.c \
		src/rproxy/client_hello.c $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ $< src/rproxy/ja4.c \
		src/rproxy/client_hello.c -lcrypto

# The cache is in the I/O layer, so its test links src/cache.c, not the
# Shared core.
build/test_cache: tests/test_cache.c tests/check.h src/cache.c $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ $< src/cache.c

test: $(TEST_BINS)
	@for test_bin in $(TEST_BINS); do ./$$test_bin || exit 1; done

www: www/image.png www/large.bin

www/image.png:
	printf '%s' '$(PNG_BASE64)' | base64 -d > $@

www/large.bin:
	head -c $(LARGE_BIN_SIZE) /dev/zero > $@

format:
	clang-format -i $(FORMAT_FILES)

lint:
	@! grep -nE $(CORE_BANNED_INCLUDE) src/core/*.c src/core/*.h || \
		{ echo 'lint: the Shared core must not use sockets'; false; }
	@! grep -nE $(DECISION_BANNED_INCLUDE) $(DECISION_FILES) /dev/null || \
		{ echo 'lint: a decision module must not do I/O'; false; }
	clang-tidy --quiet --warnings-as-errors='*' src/*.c src/core/*.c \
		$(wildcard src/rproxy/*.c) \
		tests/*.c -- \
		$(TIDY_FLAGS)
	clang-format --dry-run -Werror $(FORMAT_FILES)

clean:
	rm -rf server server-asan build
