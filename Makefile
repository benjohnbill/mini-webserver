CC = gcc
CFLAGS = -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -g -Wall -Wextra -Werror \
	-pthread -Isrc -Isrc/core -Isrc/rproxy
SANITIZE = -fsanitize=address,undefined -g
TIDY_FLAGS = -std=c11 -D_POSIX_C_SOURCE=200809L -Isrc -Isrc/core -Isrc/rproxy

CORE_SRCS = src/core/http_parse.c src/core/router.c src/core/mime.c \
	src/core/response.c src/core/serve.c src/core/handlers.c
SERVER_SRCS = $(CORE_SRCS) src/send_plan.c src/conn.c src/main.c \
	src/model_iter.c src/model_fork.c src/model_pool.c src/model_epoll.c
RPROXY_SRCS = $(CORE_SRCS) src/cache.c src/cache_options.c \
	$(wildcard src/rproxy/*.c)
RPROXY_LIBS = -lssl -lcrypto -lm
HEADERS = $(wildcard src/*.h src/core/*.h src/rproxy/*.h)
TESTS = http_parse router mime response serve
TEST_BINS = $(addprefix build/test_,$(TESTS)) build/test_cache \
	build/test_client_hello build/test_forward build/test_ja4 \
	build/test_rate_limit build/test_verdict build/test_ip_list \
	build/test_ja4_table build/test_block_table build/test_connection_verdict \
	build/test_behavior
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
	rate_limit.* verdict.* ip_list.* ja4_table.* block_table.* \
	connection_verdict.* behavior.*))
# arpa/inet.h is allowed: ip_list uses inet_pton, which turns text into
# address bytes and does no I/O.
DECISION_BANNED_INCLUDE = '\#include [<"](sys/socket|sys/epoll|poll|netinet/[a-z_]+|openssl/ssl|unistd)\.h[>"]'
CORE_BANNED_INCLUDE = '\#include [<"](sys/socket|sys/epoll|sys/select|poll|netinet/[a-z_]+|arpa/inet|conn|send_plan|model)\.h[>"]'

.PHONY: all test asan www cert format lint clean

all: server rproxy

server: $(SERVER_SRCS) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(SERVER_SRCS)

asan: server-asan

server-asan: $(SERVER_SRCS) $(HEADERS)
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ $(SERVER_SRCS)

rproxy: $(RPROXY_SRCS) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(RPROXY_SRCS) $(RPROXY_LIBS)

rproxy-asan: $(RPROXY_SRCS) $(HEADERS)
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ $(RPROXY_SRCS) $(RPROXY_LIBS)

# A self-signed certificate for localhost, for rproxy and its tests.
cert: build/cert.pem

build/cert.pem:
	@mkdir -p build
	openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
		-subj /CN=localhost -addext subjectAltName=DNS:localhost,IP:127.0.0.1 \
		-keyout build/key.pem -out build/cert.pem

build/test_%: tests/test_%.c tests/check.h $(CORE_SRCS) $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ $< $(CORE_SRCS)

# The decision modules of the Reverse Proxy.
build/test_client_hello: tests/test_client_hello.c tests/check.h \
		src/rproxy/client_hello.c $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ $< src/rproxy/client_hello.c

build/test_rate_limit: tests/test_rate_limit.c tests/check.h \
		src/rproxy/rate_limit.c $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ $< src/rproxy/rate_limit.c -lm

build/test_verdict: tests/test_verdict.c tests/check.h src/rproxy/verdict.c \
		$(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ $< src/rproxy/verdict.c

build/test_ip_list: tests/test_ip_list.c tests/check.h src/rproxy/ip_list.c \
		$(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ $< src/rproxy/ip_list.c

build/test_ja4_table: tests/test_ja4_table.c tests/check.h \
		src/rproxy/ja4_table.c $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ $< src/rproxy/ja4_table.c

build/test_block_table: tests/test_block_table.c tests/check.h \
		src/rproxy/block_table.c $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ $< src/rproxy/block_table.c

build/test_connection_verdict: tests/test_connection_verdict.c tests/check.h \
		src/rproxy/connection_verdict.c $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ $< src/rproxy/connection_verdict.c

build/test_behavior: tests/test_behavior.c tests/check.h \
		src/rproxy/behavior.c $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ $< src/rproxy/behavior.c

# forward is I/O, but its tests call only the functions with no socket.
build/test_forward: tests/test_forward.c tests/check.h src/rproxy/forward.c \
		$(CORE_SRCS) $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ $< src/rproxy/forward.c $(CORE_SRCS)

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
	@failed=; for test_bin in $(TEST_BINS); do \
		./$$test_bin || failed="$$failed $$test_bin"; done; \
	if [ -n "$$failed" ]; then echo "FAILED:$$failed"; \
		echo '(owner slots fail until typed: SPEC 11)'; exit 1; fi

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
	rm -rf server server-asan rproxy rproxy-asan build
