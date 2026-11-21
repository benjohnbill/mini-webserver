CC = gcc
CFLAGS = -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -g -Wall -Wextra -Werror \
	-pthread -Isrc
SANITIZE = -fsanitize=address,undefined -g
TIDY_FLAGS = -std=c11 -D_POSIX_C_SOURCE=200809L -Isrc

CORE_SRCS = src/http_parse.c src/router.c src/mime.c src/response.c \
	src/serve.c src/handlers.c
SERVER_SRCS = $(CORE_SRCS) src/send_plan.c src/conn.c src/main.c \
	src/model_iter.c src/model_fork.c src/model_pool.c src/model_epoll.c
HEADERS = $(wildcard src/*.h)
TESTS = http_parse router mime response serve
TEST_BINS = $(addprefix build/test_,$(TESTS))
FORMAT_FILES = $(wildcard src/*.c src/*.h tests/*.c tests/*.h)

# A 1x1 PNG.
PNG_BASE64 = iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg==
LARGE_BIN_SIZE = 10485760

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
	clang-tidy --quiet --warnings-as-errors='*' src/*.c tests/*.c -- \
		$(TIDY_FLAGS)
	clang-format --dry-run -Werror $(FORMAT_FILES)

clean:
	rm -rf server server-asan build
