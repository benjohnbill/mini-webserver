---
scope: Design of the CJG branch of mini-webserver, for the work of 2026-11-21 (to the 20:00 review)
status: reviewed (egm, 5 stages, 2026-11-21). Open items are marked "open".
decided: 2026-11-21, in a design interview with the owner of the branch
grounds: LMS learning/1309 (a copy is statement-1309.md in the owner's week folder, outside this repository), and the ADRs in docs/adr/
---

# SPEC — mini-webserver, branch CJG

## 0. Read with

- [`GLOSSARY.md`](../GLOSSARY.md): the terms. Capitalized terms in this SPEC (Stuck connection, Worker, Scene, Route, Handler, Shared core, Response plan, Concurrency model) are defined there.
- [`CONVENTION.md`](../CONVENTION.md): code rules, test rules, ADR rules.
- [`docs/adr/`](adr/): decisions that are hard to reverse, with their rejected options.
- Lab 09 parser: `lab/09-http-parse/parse_server.c` in the owner's week folder, outside this repository. A generation session gets a copy.

## 1. Users and the problem

Our users are the Verification clients of the LMS statement: curl, a browser, a load tool, and a sender of invalid requests ([ADR](adr/users-are-lms-verification-clients.md)). These users ask three questions about the server:

- How does throughput (requests per second) change as the number of concurrent connections goes up? (load tool)
- Can one Stuck connection stop the service for the other clients? (sender of an invalid request)
- How much of HTTP does the server accept? (the edge cases of the statement, and what the statement does not ask for)

**The focus of the owner today is the second question.** Lab 09 answered a different question: how one connection turns bytes into a request, and a response into bytes. The second question is between connections: while connection A waits in `read()`, can connections B and C go on? A Concurrency model answers it by its choice of who blocks on a Stuck connection.

Fallback: if S2 on `pool` has no result by a time that the owner sets (open), the focus changes to the third question. The generated parser code is the same for both focuses.

## 2. Scope

| In today | Out today |
|---|---|
| TCP socket server (`socket`, `bind`, `listen` with backlog `SOMAXCONN`, `accept`, `SO_REUSEADDR`) | Forward Proxy and Reverse Proxy (2026-11-22) |
| Request line, headers (`Host`, `Content-Length`, `Connection`, `Content-Type`, `Transfer-Encoding`), POST body by `Content-Length`. `Transfer-Encoding` is read only to answer 400 | CGI (Handlers are C functions in the server) |
| Status line, `Content-Type`, `Content-Length`, `Connection`, body | Chunked transfer, Range, HTTP/2, authentication. TLS is in the Reverse Proxy (§11) |
| Static files from a root folder, MIME types, 400, 403, 404, 405, 411, 413, 414, 431, 500, 505 | Team rules (team meeting) |
| `Host` missing in an `HTTP/1.1` request gets 400. The owner types this check (§10 slot) | |
| Two Handlers: `POST /echo` and `GET /api/time` | |
| Keep-alive with a timeout | |
| Four Concurrency models (§4) | |
| `SIGINT` and `SIGTERM` shutdown, `SIGPIPE` ignored, child reaping for `fork` | |

Proxy: the Reverse Proxy is a separate program, `rproxy` (§11). The server has no Route that forwards a request. It is the Origin behind `rproxy`. `--delay MS` makes it a slow Origin for the cache Scenes.

## 3. HTTP behavior

- Checks run in this order: 400 (syntax of the request line, then of the header lines), 505 (version not `HTTP/1.0` or `HTTP/1.1`), 405 (method not `GET` or `POST`), 414 (target over 2048 bytes), then the Route. Before the header section ends, only the request line can give a status: 400, or 414 (§6). 505 and 405 wait for the end of the header section.
- A line ends with `\r\n` or a bare `\n`. RFC 9112 section 2.2 lets a server accept a bare `\n` ("MAY"). Lab 09 does this.
- The header section ends at the first empty line (`\r\n\r\n` or `\n\n`).
- A request with a body must have exactly one decimal `Content-Length`. Otherwise it gets 400. A `POST` with no `Content-Length` gets 411. A value above 1048576 gets 413 before the server reads the body. A request with `Transfer-Encoding` gets 400.
- Body bytes that arrive with the header count toward `Content-Length`. Bytes after the body are dropped (no pipelining).
- Keep-alive: `HTTP/1.1` keeps the connection open unless the request has `Connection: close`. `HTTP/1.0` closes unless the request has `Connection: keep-alive`. An error response closes the connection. Each response has `Connection: keep-alive` or `Connection: close`.
- If the client closes in the middle of a request, the server closes with no response. A timeout closes with no response.
- Routes: `POST /echo` returns the request body with status 200 and the request's `Content-Type` (or `application/octet-stream`). `GET /api/time` returns `{"now":"<UTC time, RFC 3339>"}` as `application/json`. A static file allows only `GET`. A Route that gets the wrong method returns 405 with `Allow: <its method>`.
- Static files: the server does not percent-decode. It uses the target as raw bytes. A target that does not start with `/` gets 400. A target with a `..` segment gets 403. A folder serves its `index.html`, or gets 404 if there is none. A missing file gets 404. A file that the server cannot read gets 403. Another `open` error gets 500.
- MIME: `html`, `css`, `js`, `json`, `txt`, `png`, `jpg`, `jpeg`, `gif`, `svg`, `ico`. Other: `application/octet-stream`.

## 4. Concurrency models

[ADR](adr/pool-default-with-four-models-kept.md). `--model` selects one. The table shows Scene S2 (§7): 8 Stuck connections, then one `curl`. Defaults from §6: 4 Workers, 5 s timeout.

| `--model` | Who blocks on a Stuck connection | What curl gets in S2 | Size |
|---|---|---|---|
| `iter` | The only Worker (the main loop) | A response after about 40 s (8 × 5 s) | 0 |
| `fork` | One child process for each connection | A response without waiting. The 8 children stay for 5 s | about 20 |
| `pool` (default) | 4 Workers. 4 more connections wait in the queue | A response after about 10 s | about 100 |
| `epoll` | No Worker. The loop blocks only in `epoll_wait` | A response without waiting for the timeout | about 200 |

- Size: estimated C lines added on top of `conn` and the Shared core.
- One value, `--timeout`, limits each wait for bytes: the wait between two requests and the wait inside one request. It limits one `read`, not a whole request.
- In `iter`, `fork`, and `pool`, the timeout is `SO_RCVTIMEO` on the connection socket. The `epoll` loop checks all connections for the timeout after every return of `epoll_wait`, at most once per second. The `epoll_wait` timeout is 1000 ms.
- `pool`: the queue holds 64 connections. If it is full, the main thread waits. The listen backlog then holds new connections.
- `epoll` keeps one heap buffer for each connection. It grows to the header limit, then to the body size from `Content-Length`. A write can stop with `EAGAIN`. The connection keeps its offset and calls `send_plan_some` again at the next `EPOLLOUT`.
- Expected S3: with keep-alive, a `wrk` connection holds a Worker for the whole run. `iter` serves 1 connection and `pool` serves 4. The others wait. We measure and explain this. We do not fix it.

Shutdown:

- Install every handler with `sigaction` and `sa_flags = 0`. Do not use `signal()` for a handler: with the build flags of §9 it runs only once. `SIGPIPE` is set to `SIG_IGN`.
- The `SIGINT` and `SIGTERM` handler sets a `volatile sig_atomic_t` flag. It does nothing else.
- Each loop stops on the flag. `EINTR` from `accept` or `epoll_wait` only means: check the flag, then continue.
- `pool` blocks `SIGINT`, `SIGTERM`, and `SIGCHLD` with `pthread_sigmask` before `pthread_create`, so only the main thread gets them. At shutdown the main thread sets the queue to closed and broadcasts the condition variable. Each Worker finishes its connection within `--timeout` and exits. The main thread joins them.
- `fork` reaps children in its `SIGCHLD` handler with `waitpid(-1, NULL, WNOHANG)` in a loop. A child sets `SIGINT` and `SIGTERM` back to `SIG_DFL` after `fork`, so Ctrl-C ends it at once.
- The model that accepted a `conn_fd` closes it.

## 4a. Known limits

- A slow sender (one byte every few seconds) is not closed, because the timeout limits one `read`. Scene S2b shows it.
- No send timeout in `iter`, `fork`, and `pool`: a client that stops reading holds a Worker in `write`.
- `epoll` closes a connection when its send makes no progress for `--timeout`.
- No pipelining: bytes after the body of a request are dropped, as in lab 09.
- `epoll` is Linux only. The statement asks for portability. We accept this.
- `fork` has no limit on children.
- Each connection holds up to 8192 bytes of header and up to 1048576 bytes of body in memory.
- No percent-decoding, no symlink check, no absolute-form target. The query string is not removed.
- After an error response, `conn` stops writing (`shutdown`) and drops unread bytes: at most 16 reads of 4096 bytes, each after a wait of at most 1 s. Then it closes. A client that sends more than that can still get RST and miss a 413 or 431. `epoll` does not drain: its close after an error can send RST.
- `.htm`, upper-case extensions, and `charset` are not handled.
- No `Transfer-Encoding` support (400).
- `accept` on `EMFILE` is not handled. `ulimit -n` here is 1048576.
- `func.sh` does not test 500 or an unreadable file.

## 5. Modules

[ADR](adr/shared-core-does-not-know-sockets.md). The Shared core is `http_parse`, `router`, `mime`, `response`, `serve`, and `handlers`. The word "test" marks a unit test target. All paths are new (Create).

```text
Makefile
src/
  main.c          arguments, signals, listen socket, model selection
  config.h        struct config and all default values (§6)
  core/           the Shared core. `make lint` fails on a socket include
    http_parse.c/h  buffer -> struct request, or a status code    test
    router.c/h      request -> Route (static file or Handler)      test
    mime.c/h        file extension -> Content-Type                 test
    response.c/h    status line and headers into a buffer          test
    serve.c/h       request -> Response plan; keep-alive decision  test
    handlers.c/h    POST /echo, GET /api/time
  send_plan.c/h   write a Response plan from an offset (I/O layer)
  conn.c/h        blocking: read to the empty line, body, keep-alive loop
  model.h         the four model entry points
  model_iter.c  model_fork.c  model_pool.c  model_epoll.c
  cache.c/h       the Reverse Proxy cache. Only rproxy links it (§11) test
  cache_options.c/h  the --cache options of rproxy
tests/
  check.h  test_http_parse.c  test_router.c  test_mime.c  test_response.c
  test_serve.c  test_cache.c
  func.sh         curl and nc against a running server
bench/
  scene.sh        scene.sh <S2|S2b|S3> <model>
  cache/run.sh    the cache Scenes: rproxy in front of a slow Origin
www/              index.html, style.css, app.js, image.png, large.bin
docs/measurements/
```

Interfaces:

```c
/* http_parse.h — Shared core. struct request points into the caller's buffer. */
struct request {
    char *method;
    char *target;
    char *version;
    char *host;             /* NULL if absent */
    char *content_type;     /* NULL if absent. POST /echo sends it back */
    long content_length;    /* -1 if absent */
    bool wants_close;       /* from the version and Connection */
    char *body;
    size_t body_len;
};
#define HTTP_INCOMPLETE (-1)
/* Returns 0 when the request is complete, HTTP_INCOMPLETE when more bytes
   are needed, or a status code (400, 405, 411, 413, 414, 431, 505). */
int http_parse_request(char *buffer, size_t len, struct request *request);
/* Returns HTTP_INCOMPLETE, the total length of the request (header and body)
   as soon as the header is complete, or a status code as a negative number
   (-400). A length can be equal to a status code, so the sign is necessary.
   Does not change the buffer. */
long http_frame_measure(const char *buffer, size_t len);

/* router.h — pure, no files */
enum route_kind { ROUTE_STATIC, ROUTE_HANDLER };
struct route {
    enum route_kind kind;
    const char *allowed_method;
    int (*handler)(const struct request *request, struct response_plan *plan);
};
/* 0, 403, or 405. No 404: the router does not look at files. serve gets 404
   when stat fails. */
int router_resolve(const struct request *request, struct route *route);

/* mime.h */
const char *mime_find_type(const char *path);

/* response.h */
#define RESPONSE_HEADER_MAX 1024
struct response_plan {
    char header[RESPONSE_HEADER_MAX];
    size_t header_len;
    const char *body;       /* memory body, or NULL */
    size_t body_len;
    int file_fd;            /* -1 if no file */
    off_t file_size;
    bool close_after;
};
/* False: the header did not fit, and the plan is a 500 now. */
bool response_plan_init(struct response_plan *plan, int status,
                        const char *content_type, off_t content_length,
                        bool close_after);
/* A short generated body goes into header[], so the plan owns it. */
void response_plan_init_text(struct response_plan *plan, int status,
                             const char *content_type, const char *text,
                             bool close_after);
void response_plan_init_error(struct response_plan *plan, int status, bool close_after);
/* 405 with "Allow: <allowed_method>". It closes the connection. */
void response_plan_init_not_allowed(struct response_plan *plan,
                                    const char *allowed_method);
void response_plan_close(struct response_plan *plan); /* closes file_fd */

/* serve.h — opens files (stat, open). */
void serve_request(const struct request *request, const struct config *config,
                   struct response_plan *plan);
bool serve_keeps_open(const struct request *request, int status);

/* handlers.h — each returns 0, or a status code when it made no plan. */
int handlers_echo(const struct request *request, struct response_plan *plan);
int handlers_tell_time(const struct request *request,
                       struct response_plan *plan);

/* send_plan.h — I/O layer. Returns SEND_DONE, SEND_AGAIN, or SEND_ERROR. */
#define SEND_DONE 0
#define SEND_AGAIN 1
#define SEND_ERROR (-1)
int send_plan_some(const struct response_plan *plan, int conn_fd, size_t *offset);

/* conn.h — does not close conn_fd */
void conn_serve(int conn_fd, const struct config *config);
/* One line for --log. request is NULL if the request did not parse. */
void conn_log(const struct request *request, const struct response_plan *plan);

/* model.h — each returns 0 at a clean shutdown, -1 while it is a slot, 1 on an error */
/* Set by the SIGINT and SIGTERM handler in main.c. Each loop stops on it. */
extern volatile sig_atomic_t stop_requested;
int model_iter_run(int listen_fd, const struct config *config);
int model_fork_run(int listen_fd, const struct config *config);
int model_pool_run(int listen_fd, const struct config *config);
int model_epoll_run(int listen_fd, const struct config *config);
```

- `iter`, `fork`, and `pool` call `conn_serve`. `epoll` has its own loop, but it uses `http_frame_measure`, `http_parse_request`, `serve_request`, `serve_keeps_open`, and `send_plan_some`. The keep-alive logic is in one place.
- `send_plan_some` reads a file body with `pread`, not `sendfile` (not POSIX).
- `http_parse` keeps the flow of lab 09: `find_header_end`, `parse_request_line`, `end_first_line`, `split_at_space`, `is_supported_version`. These helpers keep their lab names as `static` functions in `http_parse.c`. This is an exception to CONVENTION Names rule 2. Changes from lab 09: `parse_request_line` returns a status code, not `bool`. `end_first_line` checks for NULL. The loop variable `p` becomes `cursor`.
- Lab 09 `open_listen_fd`, `bind_and_listen_by_fd`, and the `SIGPIPE` ignore move to `main.c`. There, `open_listen_fd` becomes `listen_socket_open` and `bind_and_listen_by_fd` becomes `listen_socket_bind`. Lab 09 `status_reason` moves to `response.c` as `status_get_reason`. `create_socket_addr` becomes `socket_addr_init`.
- The function pointer in `struct route` is for Handlers only. Model selection is one `if` chain in `main.c`.
- `www/`: `index.html` loads `style.css`, `app.js`, and `image.png`. `app.js` fetches `/api/time` and shows it. `make www` writes `image.png` (a 1×1 PNG) from a base64 literal, and `large.bin` (10 MB, for S4) with `head -c`.

## 6. Settings and limits

```bash
./server <port> <root> [--model iter|fork|pool|epoll] [--workers N] [--timeout S] [--log]
```

Only `--model`, `--workers`, `--timeout`, and `--log` are arguments. The other values are constants in `config.h`. A bad argument prints the usage line and the server exits with code 2. The parser is hand-written.

A boundary value is a limit that a request is above or not. It gets a pass or fail test. A degree value is an experiment variable of the Scenes.

| Value | Default | Source | Trade-off | Kind |
|---|---|---|---|---|
| `--workers` | 4 | Our choice. S2 then needs only 8 `nc` connections | Few: Stuck connections hold all Workers until the timeout. Many: each thread uses stack memory | degree (`pool` only) |
| `--timeout` | 5 s, must be ≥ 1 | Apache `KeepAliveTimeout` default. nginx `keepalive_timeout` is 75 s | Short: the server closes slow but valid connections. Long: Stuck connections hold Workers longer | degree |
| `--log` | off | Our choice. A line per request changes S3 throughput | On: each request prints one line | — |
| Header limit | 8192 bytes, from the first byte to the end of the empty line. More gets 431 | Our choice. nginx limits each header line to 8 KB. Apache `LimitRequestFieldSize` is 8190 | Small: valid requests fail. Large: each connection uses more memory | boundary |
| Request-target limit | 2048 bytes. More gets 414 when the request line ends. A request line with no end inside 8192 bytes gets 431 | Our choice, below the header limit | Small: long valid URLs fail. Large: no extra cost, the header limit still holds | boundary |
| Body limit | 1048576 bytes, decided from `Content-Length` | nginx `client_max_body_size` | Small: uploads fail. Large: each connection uses more memory | boundary |
| Line end | `\r\n` or `\n` | Lab 09. RFC 9112 section 2.2 | Strict would break requests typed in `nc` | boundary |

## 7. Tests and Scenes

- `make test` builds the unit tests with `-fsanitize=address,undefined -g` and runs them.
- `tests/check.h` gives `CHECK(cond)` and `CHECK_EQ_INT(a, b)`. A failure prints the file, the line, and the test name. The test program then exits with code 1.
- Minimum test names:
  - `http_parse`: a full GET is complete. A request with no empty line is incomplete. A request with bare `\n` line ends is complete. An unknown version gets 505. An unknown method gets 405. A target over 2048 bytes gets 414. A header over 8192 bytes gets 431. A POST with no `Content-Length` gets 411. Two `Content-Length` headers get 400. `Transfer-Encoding` gets 400.
  - `router`: `/` is static. `/echo` with POST is a Handler. `/echo` with GET gets 405. A `..` segment gets 403.
  - `mime`: `.html`, `.png`, and an unknown extension.
  - `response`: the status line and `Content-Length` of a 200 and a 404.
  - `serve`: `HTTP/1.0` closes, `HTTP/1.1` keeps the connection open, `Connection: close` closes, an error closes.
- `tests/func.sh <model>` starts `./server 18080 www --model <model>`, runs the checks, stops the server, and exits with code 1 at the first failure. It checks each status code of §3 except 500 and the unreadable-file 403, and each boundary value of §6 on both sides. It uses `curl --path-as-is` for `..`, and `nc -N` so that `nc` stops at the end of its input.
- `make asan` builds `server-asan` with `-fsanitize=address,undefined -g`. Run `func.sh` and S4 against `server-asan` once before 20:00. We do not use valgrind.
- `bench/scene.sh <S2|S2b|S3> <model>` starts and stops the server in the same way. S1 and S4 are manual steps.
- Scene priority: if time runs out, run the Scenes in this order: S2, S3, S1, S4.

| Scene | What it does | What it shows |
|---|---|---|
| S1 | Open `http://localhost:18080/` in a browser (manual) | One page needs several connections and requests, including one Handler |
| S2 | 8 `nc` connections send a request line and stop. Then one `curl`. The script prints `curl -w '%{time_total}'` | Who blocks on a Stuck connection |
| S2b | Like S2, but each `nc` sends one byte every 3 s (optional) | The slow-sender limit |
| S3 | `wrk -t4 -c<10, 100, 1000> -d10s --timeout 30s http://127.0.0.1:18080/index.html`, keep-alive on. The script prints requests per second and the timeout count | Throughput as the number of connections goes up |
| S4 | `curl` `large.bin` and stop it with Ctrl-C (manual) | `SIGPIPE` is ignored. The server closes the socket and the file, and the Worker serves the next connection |

- `wrk`, `nc` (OpenBSD), `curl`, and `valgrind` are installed (checked 2026-11-21). `ulimit -n` is 1048576.
- One run is one file: `docs/measurements/NNN-<scene>.md`, with the command, the commit, and the result. For the 20:00 review, the main output is one table: one row for each Concurrency model, one column for each Scene.

## 8. Code the owner types

The generator writes all code except the rows of this table. It leaves a slot for each row (§10). The owner types the rows in this order. If time runs out, the owner skips rows from the bottom.

| Order | Code | Question it answers |
|---|---|---|
| 1 | `model_pool_run`: the queue, the mutex and the condition variable, the Worker loop | How do threads share the blocking? |
| 2 | `conn_serve`: the `SO_RCVTIMEO` call | Who closes a silent Stuck connection? |
| 3 | `model_fork_run`: `fork`, closing fds in parent and child, the `SIGCHLD` handler | What stays behind when a process takes the blocking? |
| 4 | `model_epoll_run`: the read side of the event loop | What must the server remember so that it never blocks on one connection? |
| 5 | `model_iter_run`: the accept loop | Where does one connection stop everyone? |
| 6 | The `Host` check in `http_parse.c` (`HTTP/1.1` only) | The task from 2026-11-21 |

Row 4: for `POST /echo`, the `body` of the plan points into the buffer of the connection. The read side must not reuse, move, or free that buffer until the send of the plan is done.

## 9. Build

One `Makefile` with the targets `make`, `make test`, `make asan`, `make www`, `make format`, and `make lint`.

- Flags: `CC = gcc`, `-std=c11 -D_POSIX_C_SOURCE=200809L -O2 -g -Wall -Wextra -Werror -pthread -Isrc -Isrc/core`. Sanitizer flags go on the compile line and the link line.
- Allowed outside POSIX: `<sys/epoll.h>`. Use `fcntl` for `O_NONBLOCK`, not `accept4`. Use `strcasecmp` from `<strings.h>`. Use `inet_ntop`, not `inet_ntoa`.
- `make lint` first fails if a file in `src/core/` includes a socket header (`sys/socket.h`, `sys/epoll.h`, `sys/select.h`, `poll.h`, `netinet/*`, `arpa/inet.h`) or an I/O layer header (`conn.h`, `send_plan.h`, `model.h`). Then it runs `clang-tidy --warnings-as-errors='*'` on `src/*.c`, `src/core/*.c`, and `tests/*.c` with `-- -std=c11 -D_POSIX_C_SOURCE=200809L -Isrc -Isrc/core`, then `clang-format --dry-run -Werror`. `make format` runs `clang-format -i` on `src/`, `src/core/`, and `tests/`.

## 10. How the code is generated

An agent generates the code in a new session, on the `CJG` branch. Its input is this SPEC and the files of §0.

- Slots: the generator does not write the bodies of the §8 rows. For each one it writes the function with its final signature, `(void)` casts for the parameters, `return -1;`, and the comment `/* SLOT(owner): SPEC §8 row N */`. For row 2 and row 6 it leaves only the comment at the place of the call or the check: `/* SLOT(owner): SO_RCVTIMEO */` and `/* SLOT(owner): Host check, HTTP/1.1 only */`. The read side of `model_epoll_run` is a slot. The generator writes its write side.
- Phases: 1, the Shared core and its unit tests. 2, `conn`, `send_plan`, `main`, `model.h`, and the slots. 3, `tests/func.sh`. 4, `bench/scene.sh` and `www/`.
- Done when `make`, `make test`, and `make lint` pass. `func.sh` cannot pass yet, because each model is a slot. It must run and report the slot error.

## 11. Reverse Proxy (2026-11-24)

[ADR: separate process](adr/reverse-proxy-is-a-separate-process.md), [ADR: OpenSSL](adr/openssl-for-the-reverse-proxy.md).

The topic is the fight between crawlers and the server. A Crawler script attacks. The Reverse Proxy decides a Verdict. A Scene counts the Verdicts and the load on the Origin.

```text
client --TLS--> rproxy --HTTP--> server (the Origin, 127.0.0.1)
```

### Modules

The four decision modules do not use sockets or OpenSSL I/O. Unit tests call them with bytes and values. `ja4` uses only `SHA256()` from OpenSSL.

```text
src/rproxy/
  main.c            arguments, start, one thread for each Event loop,
                    stop (step 13, code done)
  rproxy.h          the settings and the tables that all loops share
  event_loop.c/h    one Event loop: epoll, listen socket, timers, and the
                    state machine of one connection: peek, Verdict, TLS,
                    request, Verdict, cache or Origin, reply, close (I/O)
                    (step 13, code done)
  timer_heap.c/h    the timers of one loop, time is an argument     test
                    (step 13, code done)
  tls.c/h           OpenSSL context, and non-blocking handshake, read,
                    and send steps (I/O)
  forward.c/h       the Origins: resolve, non-blocking connect, and the
                    request with X-Forwarded-For and X-JA4 (I/O)
  client_hello.c/h  ClientHello bytes -> struct client_hello        test
  ja4.c/h           struct client_hello -> JA4 string             test
  rate_limit.c/h    token bucket for each key, time is an argument test
  verdict.c/h       signals -> Verdict                            test
  behavior.c/h      Behavior counters for each key, time is an argument
                                                                  test
tests/
  test_client_hello.c  test_ja4.c  test_rate_limit.c  test_verdict.c
  test_timer_heap.c
  rproxy_func.sh    curl -k through rproxy to a running server
bench/arena/        Crawler scripts
```

`client_hello` keeps the values of the wire in their order, GREASE included. `ja4` removes GREASE, sorts, and hashes.

### Origins and forwarding (user said 2026-11-24: "1~5번 전부 기본안으로 가자.")

- `--origin HOST:PORT` can occur more than once (at most `ORIGIN_MAX`, 8). The Origins get the requests in turn (Round-Robin), as the default of nginx `upstream` and of Envoy.
- If the connect to an Origin fails, `rproxy` tries the next Origin once. If that also fails, the client gets 502. An Origin that does not answer in `--origin-timeout` seconds gives 504. There is no active health check.
- Order for one connection: ClientHello and JA4, TLS handshake, request, rate limit, Verdict, cache, Origin. The Verdict comes after the request, because the User-Agent and the target are in the request, and 403 and 429 are HTTP replies. A denied request and a cache hit do not reach an Origin.
- `--rate R --burst B` turns on the rate limit for each key. `--rate-key ip|ja4` selects the key: the client IP (the default) or the JA4. `--rate 0` (the default) turns it off. `--tarpit S` sets how long a tarpit holds a connection (default 10 s). A tarpit is a timer of the Event loop. It holds no thread (step 13).
- `rproxy` builds the request to the Origin from the parsed fields: the request line, `Host`, `User-Agent`, `Content-Type`, `Content-Length`, the body, `X-Forwarded-For`, `X-JA4`, and `Connection: close`. It does not copy other headers, so no hop-by-hop header reaches the Origin. nginx copies all headers except the hop-by-hop headers. The list here is shorter, because the Shared core keeps only these fields.
- Size limits are the limits of the Shared core (§6): 431, 413, and 414 come from `http_parse_request`.
- One request for each client connection. `rproxy` closes after the reply.

### Code the owner types

The owner types the decision code. The generator writes the byte parser and the I/O. Each owner file starts as a slot (§10): the tests exist first and fail.

| Order | Code | Question it answers |
|---|---|---|
| 1 | `ja4_format`: GREASE, the version, the counts, the ALPN characters, the two hashes | Which parts of a ClientHello tell one TLS library from another? |
| 2 | `rate_limit`: the token bucket and the key table | How much may one key ask, and what does the table forget? |
| 3 | `verdict`: the table from signals to a Verdict | Which signal is strong enough to deny, and which Verification client must still pass? |

### Arena

`bench/arena/scene.sh <crawler> [preset...]` runs one Crawler script and `human.py` side by side against `rproxy`, for each preset, and writes `docs/measurements/<date>-arena-<crawler>.md`. The Origin is `./server --model pool --delay 10`. The Scene counts the rproxy log lines of each client kind: the Verdicts, the cache hits, the requests to the Origin, and the bytes from the Origin. It does not pass or fail.

| Crawler script | What it does |
|---|---|
| `repeat` | It downloads one 2 MiB file again and again, with no conditional headers (the Read the Docs case). |
| `longtail` | It reads dataset pages (`bench/cache/make-dataset.sh`) in a uniform random order. |
| `named` | It sends the honest User-Agent `GPTBot/1.1` and walks all pages as fast as it can. |
| `disguised` | It sends a Chrome-on-macOS User-Agent, but its TLS is curl, so its JA4 is the JA4 of curl. |
| `spread` | It is `named`, but it sends from 32 source addresses, 127.0.0.2 to 127.0.0.33. |

`human.sh` is a slow Verification client. It loads the home page and its four parts, then waits 1 to 3 s. It sends from 127.0.0.200, so the Scene can find its log lines. It shows if a defense also stops a real user. It is curl, so its JA4 is the JA4 of all Crawler scripts.

| Preset | rproxy flags | Owner slot that it needs |
|---|---|---|
| `none` | no flags | none |
| `cache` | `--cache on` | none: the cache is generated code |
| `rate-ip` | `--rate 5 --burst 10` | `rate_limit_take` (row 2) and `verdict_decide` (row 3), which turns `retry_after` into a 429 |
| `rate-ja4` | `rate-ip` and `--rate-key ja4` | `ja4_format` (row 1) for the key, and rows 2 and 3 |
| `all` | `cache` and `rate-ja4` | rows 1, 2, and 3 |

A Verdict from the User-Agent or the JA4 (deny or tarpit for `named` or `disguised`) needs `verdict_decide` (row 3). Until the owner types a slot, the presets that need it give the same rows as `none` or `cache`.

With `--rate-key ja4`, all clients with one JA4 share one token bucket. All Crawler scripts and `human.sh` use curl, so they share a bucket. While `ja4_format` is a slot, the JA4 of each client is the empty string, so all clients share the key "".

### v2: a place for each defense layer (2026-11-24)

User said 2026-11-24: "1~4번 전부 추천대로 가자." The goal: each layer of the study page "crawler 트래픽과 서버의 방어" has a place in `rproxy` and a Crawler script that shows it. CAPTCHA is out: a person must solve it, so a Scene cannot measure it. Reverse DNS is out: it is not in the layer figure, and Web Bot Auth does the same job (a good bot proves its name).

#### Two Verdict stages

| Stage | When | Signals | Verdicts |
|---|---|---|---|
| Connection | after the ClientHello peek, before the TLS handshake | client IP, JA4, the block table | pass, drop (close with no reply), tarpit |
| Request | after the request parse | the connection signals, User-Agent, target, rate limit, behavior counters, proof of work token, bot signature | pass, deny (403), slow (429), tarpit, challenge (proof of work), pay (402) |

Before the handshake there is no HTTP, so the connection stage can only close or hold. A dropped connection costs no RSA signature (0.9 ms each, measured in the parity note).

New order for one connection: ClientHello and JA4, connection Verdict, TLS handshake, request, rate limit and behavior counters, request Verdict, cache, Origin.

#### The layers

| Layer (study page) | Defense | Stage | Module, with unit tests | Owner decides |
|---|---|---|---|---|
| IP, network | Block list of CIDR ranges, `--block-list FILE`. On localhost a CIDR list stands in for an ASN list | connection | `ip_list`: parse, match | the ranges in the file |
| TLS fingerprint | JA4 table, `--ja4-list FILE`: JA4 to a label (`curl`, `python`, `browser`) | connection and request | `ja4` (row 1); the table lookup | the labels, and what a label means |
| HTTP header | User-Agent table. `www/robots.txt` on the Origin, with `Disallow: /trap/` | request | `verdict` (row 3) | which User-Agent and JA4 pairs are a lie |
| Volume and behavior | Rate limit. Behavior counters for each key: requests, distinct targets, 404s, robots.txt read, honeypot hits | request | `rate_limit` (row 2); `behavior` | the limits and which counter means a crawler |
| Test | Proof of work on targets under `--pow-prefix`: a 401 with `X-PoW-Challenge: NONCE BITS`. The client sends `X-PoW: NONCE:COUNTER`, and SHA-256 of that text must start with BITS zero bits. The nonce is an HMAC of the client IP and the time window, so `rproxy` keeps no state | request | `pow`: make and check | where and how hard |
| Trap | Tarpit (exists). Honeypot: a hidden link in `www/index.html` to `/trap/`, which `robots.txt` disallows. A hit puts the key in the block table for `--block-time` seconds | request, then connection | `block_table`: time is an argument | the trap paths, the block time |
| Deal | Web Bot Auth: an RFC 9421 signature with Ed25519 over `@authority` and `signature-agent`, keys from `--bot-key FILE`. A verified bot skips the User-Agent and JA4 rules. A crawler User-Agent with no valid signature gets 402 on `--pay-prefix` targets | request | `bot_auth`: verify (test vector: RFC 9421 Appendix B.2.6) | which bots, which targets |
| Cache | Hit (exists). New: the Origin sends `ETag` for static files, and the cache answers `If-None-Match` with 304 | request | Shared core and `cache` | none |

The Shared core keeps only some headers (§11 "Origins and forwarding"). v2 needs more (`If-None-Match`, `X-PoW`, `Signature`, `Signature-Input`, `Signature-Agent`), so `struct request` keeps the header block, and `http_find_header` finds one header by name.

#### Code the owner types, v2

Rows 1 to 3 stay. Row 3 gets the new signals. One row is new:

| Order | Code | Question it answers |
|---|---|---|
| 4 | `connection_decide`: IP, JA4, and block table to a connection Verdict | What may the server decide before it spends a TLS handshake? |

The generator writes `ip_list`, `block_table`, `behavior`, `pow`, `bot_auth`, the header lookup, the 304 path, and the I/O. Each has its tests first. The owner also writes the files that hold decisions: the block list, the JA4 table, the trap paths in `robots.txt`.

#### Arena, v2

`human.sh` becomes `human.py`, a Python `urllib` client with a Chrome User-Agent. Its JA4 differs from curl: no ALPN (`00`), and other extensions (captured 2026-11-24 with lab probe 10). It is still OpenSSL, not the BoringSSL of a real Chrome, so it is a stand-in for a browser.

| New Crawler script | What it does | Layer it shows |
|---|---|---|
| `polite` | It reads `robots.txt`, skips each `Disallow` path, sends 1 request per second, and names itself | robots.txt works for a crawler that obeys |
| `trapped` | It follows every link, the hidden `/trap/` link too | honeypot, then the block table at the connection stage |
| `revalidate` | It is `repeat` with `If-None-Match` | 304 |
| `solver` | Python. It solves each proof of work challenge | the cost of each page for a crawler |
| `signed` | Python. It signs each request with Ed25519 (Web Bot Auth) and names itself | a good bot passes |

`named`, `disguised`, and `spread` stay. With the JA4 table, `disguised` (a Chrome User-Agent with the JA4 of curl) becomes visible. A new preset `layers` turns on every defense.

The generator chose these parts of step 12 by common practice (2026-11-24). The owner can change each row.

| Choice | Value | Source |
|---|---|---|
| Python scripts | `human.py`, `solver.py`, `signed.py`, with `bench/arena/arena.py` for the shared parts. `scene.sh` runs `<name>.sh` or `<name>.py`. The summary line is the line of `lib.sh`, with `pow_cpu_s` added | The extension says the language. Standard library only |
| JA4 table of `layers` | `bench/arena/layers-ja4.txt`: the JA4 of curl is `curl`, the JA4 of Python `urllib` is `browser` (a stand-in). Captured from the rproxy log at `eb4fc59` | SPEC "Arena, v2" |
| Block list of `layers` | `bench/arena/layers-block.txt`: `127.0.0.16/28`, 16 of the 32 addresses of `spread`. It does not hold 127.0.0.1 or 127.0.0.200 | A stand-in for the range of one hosting network |
| Rate key of `layers` | The client IP, not the JA4: `human.py`, `solver.py`, and `signed.py` share one JA4 | Our choice |
| Proof of work of `layers` | `--pow-prefix /api/ --pow-bits 16`. `human.py` loads `/api/time`, so it meets the challenge | 16 bits is difficulty 4 of Anubis, its default |
| Pay of `layers` | `--pay-prefix /bench/`: the dataset, which `named`, `spread`, and `signed` walk | Pay for each crawl protects content |
| A solution again | `human.py` and `solver.py` send a solution again until the next 401 (`POW_REUSE=0`: once only) | A browser keeps the Anubis cookie. A crawler that solves does the same |
| CPU time of a solution | `time.thread_time()` of the solving thread | Process time counts the other threads too |
| `signed` | The User-Agent of `named` (GPTBot/1.1), so the signature is the only difference. The Dictionary form of `Signature-Agent` (`agent1`). `expires` is `created` + 60. The key is `tests/data/bot_test_key.pem` | draft-meunier-web-bot-auth-architecture-05 |
| Ed25519 in Python | `openssl pkeyutl -sign -rawin -in FILE`, one process for each request. The base goes into a temporary file: Ed25519 signs in one shot, and `pkeyutl` cannot read it from a pipe | No new pip package. `tests/rproxy_func.sh` signs the same way |
| `polite` | Reads `robots.txt` once, then the links of `/` and the dataset. It skips a target that a `Disallow` path starts. One request, then 1 s | RFC 9309 section 2.2.2. A crawl delay of 1 s is common |
| `trapped` | `/`, every `href` and `src` of it, then `BATCH` dataset pages, again and again. The User-Agent of curl | Our choice |
| `revalidate` | Each loop keeps the `ETag` of its first reply, then sends `If-None-Match` with it | RFC 9110 section 13.1.2 |
| `www/image.png` | `scene.sh` runs `make www` when it is missing. Without it, each page load of `human.py` gets a 404, and the scanner rule blocks `human.py` | Found in a fresh checkout |

#### Defaults taken from practice

The owner asked the generator to fill steps 8 to 12 by common practice (2026-11-24). Each row is a value that the generator chose. The owner can change each row.

| Choice | Value | Source |
|---|---|---|
| Connection Verdict drop | `rproxy` closes the socket with the ClientHello unread. The kernel then sends RST. No TLS byte goes out | nginx `return 444` (close with no reply) and `ssl_reject_handshake on` (stop before the handshake) |
| Rule order in `connection_decide` | A table of rules. The first rule that matches wins. No match: pass. Block list hit: drop. Block table hit: drop. A JA4 label alone: pass. Tarpit has no rule yet | nginx `ngx_http_access_module`: "rules are checked in sequence until the first match is found" |
| Range format | `ADDRESS/BITS` | RFC 4632 section 3.1 (IPv4), RFC 4291 section 2.3 (IPv6) |
| An address with no `/BITS` | It is one address: `/32` or `/128` | nginx `allow` and `deny` take an address or a CIDR range |
| Bits after the prefix (`10.9.9.9/8`) | Accepted and set to zero | nginx accepts them and warns "low address bits are meaningless" |
| An IPv4-mapped IPv6 client (`::ffff:a.b.c.d`) | It matches the IPv4 ranges | RFC 4291 section 2.5.5.2. nginx `ngx_http_access_module` does the same |
| Comments and blank lines in the two list files | `#` starts a comment to the end of the line. Blank lines are skipped. `\r\n` is accepted | nginx configuration files and `geo` include files |
| JA4 table line | `JA4 LABEL`, two words. The JA4 has 36 characters. A JA4 two times: the first line wins | nginx `map` include file (`key value`). The first-line rule is our choice |
| A bad line in a list file | `rproxy` prints the file and the line number, and does not start | nginx `-t`: a bad configuration stops the start |
| List files reload | No reload. `rproxy` reads them once at the start | Our choice. nginx reloads on `SIGHUP`. A reload needs a lock or a swap of the table |
| Block table keys | The client IP and the JA4. The empty JA4 (no fingerprint) is never a key | Our choice. SPEC v2 "The layers", row "Trap" |
| `--block-time` default | 600 s | fail2ban `bantime` default (10 minutes) |
| Block table bound | 4096 keys, the same as the rate limit table | Our choice |
| Block table eviction | When it is full, the expired keys leave first. If none expired, the key that expires first leaves | Our choice. nginx `limit_req_zone` also removes old state when its zone is full, by least recent use |
| Block table concurrency | One mutex for the whole table. A linear search under the lock | Our choice. The rate limit table has the same shape. A search of 4096 keys is cheap next to a TLS handshake |
| `--block-key KEY` | A key in the block table from the start, for `--block-time`. At most 16 | Our choice, like `fail2ban-client set JAIL banip`. The functional tests use it to test the expiry |
| Size bounds of the list files | 1024 ranges, 256 JA4 entries, labels of 32 characters, lines of 255 characters | Our choice |
| `ja4_format` (row 1) | The FoxIO rules: `t`, version, `d` or `i` (SNI), cipher count, extension count, ALPN, then the cipher hash and the extension hash. GREASE values leave every list and count | FoxIO `technical_details/JA4.md`; RFC 8701 |
| `rate_limit` (row 2) | A token bucket for each key. `rate` tokens come back each second, up to `burst`. A new key starts full. A refused request takes no token. Retry-After is ceil((1 - tokens) / rate), at least 1 | nginx `limit_req`, Envoy `local_ratelimit`; RFC 9110 §10.2.3 |
| Rate limit table full | It forgets the key used longest ago | nginx `limit_req_zone` |
| `verdict_decide` (row 3) | An ordered rule table. The first rule that matches decides. The default is pass | Cloudflare custom rules |
| Crawler User-Agents | GPTBot, CCBot, ClaudeBot, Bytespider, PerplexityBot, AhrefsBot get deny (403). Deny comes before slow | Cloudflare "AI Scrapers and Crawlers"; ai.robots.txt |
| A false browser | Step 9: a `Chrome/` User-Agent whose JA4 label (the JA4 table) is known and is not `browser` gets tarpit. A JA4 that is not in the table passes, so a real browser that the table does not know is not punished. It replaces the rule of step 8, which compared the cipher hash with the hash of Chromium | JA4 against the User-Agent, as bot management products check it. FoxIO: a JA4 database maps a JA4 to an application |
| `robots.txt` | `User-agent: *` and `Disallow: /trap/` | RFC 9309 sections 2.1 and 2.2 |
| Honeypot link | `<a href="/trap/" rel="nofollow" hidden aria-hidden="true" tabindex="-1">` before the script of `www/index.html`. `hidden` takes it out of the page and of the reading order. `tabindex="-1"` keeps it out of the tab order | The common honeypot link: a person cannot see it, a crawler that parses links finds it. `rel="nofollow"`: HTML link types |
| Trap page | `www/trap/index.html`, a short page with `<meta name="robots" content="noindex, nofollow">`. The Origin answers 200 | Our choice. A 200 shows the crawler nothing unusual |
| Honeypot hit | A target whose path starts with `/trap/`: tarpit, and the client IP goes in the block table for `--block-time`. The JA4 does not go in, because all curl clients share one JA4 | SPEC v2 "The layers", row "Trap". fail2ban: one ban for each IP |
| Rule order in `verdict_decide` | Steps 10 and 11: honeypot, scanner, verified bot (pass), pay, crawler names, false browser, Proof of work, rate limit. The two rules that block come first, so a named crawler that ignores `robots.txt` is caught too. They also hold for a verified bot | Our choice |
| Behavior counter key | The client IP | fail2ban counts failures for each IP. The block table blocks the client IP |
| Behavior counter window | A fixed window for each key: 600 s from its first request. Then all counts start at zero | fail2ban `findtime` default (10 minutes). A fixed window is the simplest window |
| Behavior counter bound | 4096 keys. When the table is full, the key used longest ago leaves. One mutex, a linear search | The same shape as the rate limit table (nginx `limit_req_zone`) |
| Distinct targets | A set of 256 bits for each key. A target sets the bit of the FNV-1a hash of the whole target, query too. A new bit counts one. So the count can be low, never high, and it stops at 256 | Linear counting (Whang, Vander-Zanden, Taylor 1990): a Bloom filter with one hash function |
| `robots.txt` read and trap hit | The path before `?`. A read is the path `/robots.txt` exactly. A hit is a path that starts with `/trap/` | RFC 9309 section 2.3 (the file is at `/robots.txt`), section 2.2.2 (a rule matches a path prefix) |
| 404 replies | `rproxy` counts the status of its reply to the client after the request Verdict, from the Origin or from the cache | fail2ban `nginx-botsearch`: 404 replies of a client are its failures |
| Scanner rule | 5 replies of 404 in the window: the next request gets deny (403), and the client IP goes in the block table for `--block-time` | fail2ban `jail.conf` defaults: `maxretry = 5`, `findtime = 10m`, `bantime = 10m` |
| Counters with no rule | Requests, distinct targets, and `robots.txt` reads go to the log only. The trap hits count too, but the rule reads the target | No common threshold exists. The rate limit covers the volume of requests |
| Behavior log line | After the 7-field line of a request: `behavior IP requests=N targets=N not_found=N robots=N trap=N`, the counts with this request and before its reply. `scene.sh` skips the lines that start with `behavior` | Our choice. The 7 fields stay for `scene.sh` |
| Over the rate limit | Slow (429 with Retry-After) | RFC 6585 §4; nginx `limit_req` |
| Host check | An HTTP/1.1 request with no Host gets 400. HTTP/1.0 with no Host passes | RFC 9112 §3.2 |
| A verified bot | A bot with a valid Web Bot Auth signature passes after the honeypot and scanner rules. So it skips the User-Agent, JA4, Proof of work, and rate limit rules | Cloudflare verified bots: a verified bot skips the bot challenges and blocks |
| Pay rule | A crawler User-Agent (the crawler names) with no valid signature, on a `--pay-prefix` target, gets pay (402). It comes before the crawler deny, so a pay target shows 402 and other targets still show 403 | Cloudflare pay per crawl: "Charge" takes the place of "Block" for a crawler on that content |
| 402 reply | `crawler-price: USD 0.01` and a short text body. `rproxy` takes no payment: it does not read `crawler-exact-price` or `crawler-max-price`. A crawler gets in when it signs (Web Bot Auth) | Cloudflare pay per crawl, "Crawl pages": `HTTP 402`, `crawler-price: USD 0.01` |
| `--pow-prefix`, `--pay-prefix` | Each flag can occur up to 8 times. A prefix starts with `/`. A target matches when it starts with the prefix, as text (`/data/` matches `/data/1.html?x=1`) | nginx `location /data/` (a prefix match). The repeat is the same as `--origin` |
| `--pow-bits` default | 16 | Anubis `DefaultDifficulty = 4`: 4 leading zero hex digits (`web/js/worker/sha256.ts`), so 16 bits |
| Pow secret | 32 random bytes from OpenSSL `RAND_bytes` at each start. A restart makes the old challenges invalid | RFC 2104 section 3: an HMAC key as long as the hash output |
| 401 challenge reply | `X-PoW-Challenge: NONCE BITS`, `WWW-Authenticate: PoW challenge="NONCE BITS"`, and a short text body | RFC 9110 §15.5.2: a 401 has `WWW-Authenticate`. RFC 9577 §2.1 (`PrivateToken challenge=...`): a challenge as an auth-param |
| When the pow and the signature are checked | `X-PoW` only on a `--pow-prefix` target. The signature only with `--bot-key`, and only when the request has Host, because the signature covers `@authority` | Our choice: no hash and no signature check that no rule reads |
| Clock of the pow and the signature | The wall clock (`CLOCK_REALTIME`). The rate limit and the counters keep the monotonic clock | RFC 9421 `created` and `expires` are wall clock times |
| `--bot-key FILE` | Read once at the start, as `--ja4-list`. A bad line prints the file and the line number, and stops the start | nginx `-t` |

#### Event loops (step 13)

[ADR: one Event loop for each core](adr/reverse-proxy-runs-one-event-loop-per-core.md). The owner asked for the structure of a real edge proxy and left the details to the generator (2026-11-24). Each row is a value that the generator chose. The owner can change each row.

| Choice | Value | Source |
|---|---|---|
| Event loops | One thread for each online CPU core (`sysconf(_SC_NPROCESSORS_ONLN)`). `--loops N` (1 to 256) changes the count | nginx `worker_processes auto`; Envoy `--concurrency` (default: the hardware threads) |
| Threads, not processes | The loops are threads of one process, so the decision tables and the cache stay one copy with their mutexes | Envoy and HAProxy: worker threads. nginx needs shared memory zones (`limit_req_zone`) for the same tables |
| Spread of new connections | Each loop has its own listen socket with `SO_REUSEPORT`. The kernel picks the loop. Before the loops open their sockets, a plain bind checks the port. If another socket holds it, `rproxy` prints `port N is in use` and stops, so a second `rproxy` cannot share the port | nginx `listen ... reuseport`; Envoy listeners on Linux |
| Connections for each loop | `--connections N` (default 1024). A new connection over the limit is closed at once and logs the Verdict `full` | nginx `worker_connections 1024` (the `nginx.conf` that nginx ships) |
| Timers | One binary min-heap of deadlines for each loop (`timer_heap`). `epoll_wait` waits until the first deadline, at most 1 s, so a stop does not wait | libuv timers (a min-heap); nginx keeps its timers in a red-black tree |
| Client timeout | 5 s for each step that waits for the client: the ClientHello, the handshake, the request, each send. The deadline moves when bytes move | The value of the pool (`CLIENT_TIMEOUT`); nginx `client_header_timeout` and `send_timeout` reset on each read or write |
| Tarpit | A timer of `--tarpit` seconds. The connection stays open and the loop does not read it. At the deadline, the connection closes. No thread waits | Our design: the tarpit must cost the server less than the client (`20261124-arena-layers.md`) |
| Origin addresses | Resolved once at the start (`getaddrinfo`), up to 4 addresses for each name, in the order of `getaddrinfo`. A failed connect tries the next address of the same Origin first. A name that does not resolve stops the start | nginx resolves `upstream` server names when it reads the configuration |
| Origin exchange | A non-blocking `connect`, then the request, then the reply to EOF, inside `--origin-timeout`. A failed connect tries the next Origin once, as the pool did | The behavior of the pool (`forward_fetch`); nginx `proxy_next_upstream error` |
| TLS | OpenSSL on a non-blocking socket. `SSL_ERROR_WANT_READ` and `SSL_ERROR_WANT_WRITE` set the epoll events of the connection. `SSL_MODE_ENABLE_PARTIAL_WRITE` and `SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER` are on | OpenSSL `SSL_get_error(3)`: the way nginx and HAProxy drive OpenSSL |
| Stop | `SIGTERM` or `SIGINT` sets a flag. Each loop sees it within 1 s, closes its connections, and ends. A connection with no Verdict yet logs the Verdict `stop`. Then `main` prints the cache line and frees the tables | The stop of the pool. nginx graceful shutdown waits for the open requests; `rproxy` does not |

### Order of work

1. `client_hello` and its tests. Test data: ClientHello bytes that `openssl s_client` and curl sent, and records made by hand.
2. `ja4` tests (they fail), and the slot. The expected values come from the FoxIO JA4 document (`technical_details/JA4.md`), checked with `sha256sum`.
3. `tls`, `forward`, `main`, and `tests/rproxy_func.sh`.
4. `rate_limit` and `verdict`: tests first, then the slots.
5. Done (2026-11-24). The cache moves to `rproxy`. The cache Scenes of 2026-11-23 run again. Then the Proxy Route leaves the server. Parity: [`20261124-rproxy-cache-parity.md`](measurements/20261124-rproxy-cache-parity.md).
6. Crawler scripts and the Scenes of the arena (section "Arena").
7. Shared core: `http_find_header`, and `ETag` with 304 for static files. Tests first.
8. Two Verdict stages: `connection_decide` (slot, row 4) with its tests, `ip_list`, `block_table`, and the JA4 table. Done (2026-11-24). The owner chose that the generator writes row 4 by practice. The files: `ip_list`, `ja4_table`, `block_table`, and `connection_verdict` (it holds `connection_decide`). The flags: `--block-list FILE`, `--ja4-list FILE`, `--block-time S`, `--block-key KEY`. A dropped connection logs the Verdict `drop`.
9. `robots.txt`, the honeypot link and `/trap/`, and the behavior counters. Done (2026-11-24). The generator chose the rules by practice ("Defaults taken from practice"). `struct verdict_signals` now has the JA4 label and the Behavior counters, and `struct verdict` has `blocks_client`.
10. Proof of work. Done (2026-11-24). The flags: `--pow-prefix PATH` and `--pow-bits N`. A challenge logs the Verdict `challenge`.
11. Web Bot Auth and 402. Done (2026-11-24). The flags: `--pay-prefix PATH` and `--bot-key FILE`. A 402 logs the Verdict `pay`. `tests/data/bot_test_key.pem` is a test key for `tests/rproxy_func.sh` only.
12. `human.sh` in Python, the new Crawler scripts, the `layers` preset, and the Scenes. Done (2026-11-24). Results: [`20261124-arena-layers.md`](measurements/20261124-arena-layers.md). The Scene shows one cost: a tarpit holds an rproxy Worker, so `disguised` starves `human.py`.
13. Event loops in place of the pool (section "Event loops"). Done (2026-11-24). Results: [`20261124-arena-layers.md`](measurements/20261124-arena-layers.md), section "After step 13". Two rows changed in the build: the handshake deadline is 5 s from its start and does not move, because OpenSSL does not show the progress of a handshake. The drain has 1 s in all, not 1 s for each read. `timer_heap` and its tests first. The flag `--workers` of `rproxy` becomes `--loops`, and `--connections` is new. `tests/rproxy_func.sh` checks that a tarpit and a Stuck connection do not hold another client on one loop. Then the `layers` Scene of `disguised` runs again.
