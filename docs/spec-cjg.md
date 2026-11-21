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
| Request line, headers (`Host`, `Content-Length`, `Connection`), POST body by `Content-Length` | CGI (Handlers are C functions in the server) |
| Status line, `Content-Type`, `Content-Length`, `Connection`, body | Chunked transfer, Range, TLS, HTTP/2, authentication |
| Static files from a root folder, MIME types, 400, 403, 404, 405, 411, 413, 414, 431, 500, 505 | Team rules (team meeting) |
| `Host` missing in an `HTTP/1.1` request gets 400. The owner types this check (§10 slot) | |
| Two Handlers: `POST /echo` and `GET /api/time` | |
| Keep-alive with a timeout | |
| Four Concurrency models (§4) | |
| `SIGINT` and `SIGTERM` shutdown, `SIGPIPE` ignored, child reaping for `fork` | |

Proxy extension point: tomorrow the Proxy adds a third Route kind. The I/O layer (`conn` and the `epoll` loop) relays the bytes, because the Shared core does not use sockets.

## 3. HTTP behavior

- Checks run in this order: 400 (syntax), 505 (version not `HTTP/1.0` or `HTTP/1.1`), 405 (method not `GET` or `POST`), then the Route.
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
- No send timeout: a client that stops reading holds a Worker in `write`.
- No pipelining: bytes after the body of a request are dropped, as in lab 09.
- `epoll` is Linux only. The statement asks for portability. We accept this.
- `fork` has no limit on children.
- Each connection holds up to 8192 bytes of header and up to 1048576 bytes of body in memory.
- No percent-decoding, no symlink check, no absolute-form target. The query string is not removed.
- An error close with unread bytes can send RST, so a client can miss a 413 or 431.
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
  http_parse.c/h  buffer -> struct request, or a status code      test
  router.c/h      request -> Route (static file or Handler)        test
  mime.c/h        file extension -> Content-Type                   test
  response.c/h    status line and headers into a buffer            test
  serve.c/h       request -> Response plan; keep-alive decision    test
  handlers.c/h    POST /echo, GET /api/time
  send_plan.c/h   write a Response plan from an offset (I/O layer)
  conn.c/h        blocking: read to the empty line, body, keep-alive loop
  model.h         the four model entry points
  model_iter.c  model_fork.c  model_pool.c  model_epoll.c
tests/
  check.h  test_http_parse.c  test_router.c  test_mime.c  test_response.c
  test_serve.c
  func.sh         curl and nc against a running server
bench/
  scene.sh        scene.sh <S2|S2b|S3> <model>
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
    long content_length;    /* -1 if absent */
    bool wants_close;       /* from the version and Connection */
    char *body;
    size_t body_len;
};
#define HTTP_INCOMPLETE (-1)
/* Returns 0 when the request is complete, HTTP_INCOMPLETE when more bytes
   are needed, or a status code (400, 405, 411, 413, 414, 431, 505). */
int http_parse_request(char *buffer, size_t len, struct request *request);
/* Returns HTTP_INCOMPLETE, the total length of the request, or a status code. */
long http_frame_check(const char *buffer, size_t len);

/* router.h — pure, no files */
enum route_kind { ROUTE_STATIC, ROUTE_HANDLER };
struct route {
    enum route_kind kind;
    const char *allowed_method;
    int (*handler)(const struct request *request, struct response_plan *plan);
};
int router_find(const struct request *request, struct route *route); /* 0, 403, 404, 405 */

/* mime.h */
const char *mime_find_type(const char *path);

/* response.h */
struct response_plan {
    char header[1024];
    size_t header_len;
    const char *body;       /* memory body, or NULL */
    size_t body_len;
    int file_fd;            /* -1 if no file */
    off_t file_size;
    bool close_after;
};
void response_plan_init_error(struct response_plan *plan, int status, bool close_after);
void response_plan_destroy(struct response_plan *plan); /* closes file_fd */

/* serve.h — opens files (stat, open). */
void serve_request(const struct request *request, const struct config *config,
                   struct response_plan *plan);
bool serve_keeps_open(const struct request *request, int status);

/* send_plan.h — I/O layer. Returns SEND_DONE, SEND_AGAIN, or SEND_ERROR. */
int send_plan_some(const struct response_plan *plan, int conn_fd, size_t *offset);

/* conn.h — does not close conn_fd */
void conn_serve(int conn_fd, const struct config *config);

/* model.h — each returns 0 at a clean shutdown */
int model_iter_run(int listen_fd, const struct config *config);
int model_fork_run(int listen_fd, const struct config *config);
int model_pool_run(int listen_fd, const struct config *config);
int model_epoll_run(int listen_fd, const struct config *config);
```

- `iter`, `fork`, and `pool` call `conn_serve`. `epoll` has its own loop, but it uses `http_frame_check`, `http_parse_request`, `serve_request`, `serve_keeps_open`, and `send_plan_some`. The keep-alive logic is in one place.
- `send_plan_some` reads a file body with `pread`, not `sendfile` (not POSIX).
- `http_parse` keeps the flow of lab 09: `find_header_end`, `parse_request_line`, `end_first_line`, `split_at_space`, `is_supported_version`. These helpers keep their lab names as `static` functions in `http_parse.c`. This is an exception to CONVENTION Names rule 2. Changes from lab 09: `parse_request_line` returns a status code, not `bool`. `end_first_line` checks for NULL. The loop variable `p` becomes `cursor`.
- Lab 09 `open_listen_fd`, `bind_and_listen_by_fd`, and the `SIGPIPE` ignore move to `main.c`. `status_reason` moves to `response.c`. `create_socket_addr` becomes `socket_addr_init`.
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

## 9. Build

One `Makefile` with the targets `make`, `make test`, `make asan`, `make www`, `make format`, and `make lint`.

- Flags: `CC = gcc`, `-std=c11 -D_POSIX_C_SOURCE=200809L -O2 -g -Wall -Wextra -Werror -pthread -Isrc`. Sanitizer flags go on the compile line and the link line.
- Allowed outside POSIX: `<sys/epoll.h>`. Use `fcntl` for `O_NONBLOCK`, not `accept4`. Use `strcasecmp` from `<strings.h>`. Use `inet_ntop`, not `inet_ntoa`.
- `make lint` runs `clang-tidy --warnings-as-errors='*'` on `src/*.c` and `tests/*.c` with `-- -std=c11 -D_POSIX_C_SOURCE=200809L -Isrc`, then `clang-format --dry-run -Werror`. `make format` runs `clang-format -i` on `src/` and `tests/`.

## 10. How the code is generated

An agent generates the code in a new session, on the `CJG` branch. Its input is this SPEC and the files of §0.

- Slots: the generator does not write the bodies of the §8 rows. For each one it writes the function with its final signature, `(void)` casts for the parameters, `return -1;`, and the comment `/* SLOT(owner): SPEC §8 row N */`. For row 2 and row 6 it leaves only the comment at the place of the call or the check: `/* SLOT(owner): SO_RCVTIMEO */` and `/* SLOT(owner): Host check, HTTP/1.1 only */`. The read side of `model_epoll_run` is a slot. The generator writes its write side.
- Phases: 1, the Shared core and its unit tests. 2, `conn`, `send_plan`, `main`, `model.h`, and the slots. 3, `tests/func.sh`. 4, `bench/scene.sh` and `www/`.
- Done when `make`, `make test`, and `make lint` pass. `func.sh` cannot pass yet, because each model is a slot. It must run and report the slot error.
