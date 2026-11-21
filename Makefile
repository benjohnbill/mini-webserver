CC = gcc
CFLAGS = -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -g -Wall -Wextra -Werror \
	-pthread -Isrc
SANITIZE = -fsanitize=address,undefined -g
TIDY_FLAGS = -std=c11 -D_POSIX_C_SOURCE=200809L -Isrc

CORE_SRCS = src/http_parse.c src/router.c src/mime.c src/response.c \
	src/serve.c src/handlers.c
HEADERS = $(wildcard src/*.h)
TESTS = http_parse router mime response serve
TEST_BINS = $(addprefix build/test_,$(TESTS))
FORMAT_FILES = $(wildcard src/*.c src/*.h tests/*.c tests/*.h)

.PHONY: all test format lint clean

all: $(TEST_BINS)

build/test_%: tests/test_%.c tests/check.h $(CORE_SRCS) $(HEADERS)
	@mkdir -p build
	$(CC) $(CFLAGS) $(SANITIZE) -o $@ $< $(CORE_SRCS)

test: $(TEST_BINS)
	@for test_bin in $(TEST_BINS); do ./$$test_bin || exit 1; done

format:
	clang-format -i $(FORMAT_FILES)

lint:
	clang-tidy --quiet --warnings-as-errors='*' src/*.c tests/*.c -- \
		$(TIDY_FLAGS)
	clang-format --dry-run -Werror $(FORMAT_FILES)

clean:
	rm -rf build
