---
status: proposed
branch: CJG
decided: 2026-11-21
---

# The Shared core does not use sockets

| Item | Content |
|---|---|
| Context | Four Concurrency models must serve requests in the same way. `epoll` receives a request in pieces, one event at a time. The blocking models read until the blank line. The LMS statement asks for unit tests of the HTTP parser, the MIME mapping, and the routing. The parser of `lab/09-http-parse` reads from the socket and parses in the same function chain. |
| Decision | The Shared core (`http_parse`, `router`, `mime`, `response`, `serve`, `handlers`) takes a full request buffer and gives a Response plan. No Shared core function takes a socket fd. The code that reads and writes sockets (`conn` and `model_epoll`) collects the full header and body in a buffer first, and then calls `serve`. |
| Reasons | Unit tests can call the Shared core with a string, with no socket. `epoll` can use the same parser as the blocking models, because the parser does not care how the bytes arrived. |
| Rejected options | A parser that reads from the socket (the shape of lab 09): it is shorter, but each Concurrency model then needs its own parser, and a unit test needs a socket. |
| Consequences | Each connection holds the full header (up to 8 KB) and body (up to 1 MB) in memory before the server parses it. The size limits stop a client from making this memory large. The Shared core files are in `src/core/`. `make lint` fails if one of them includes a socket header or an I/O layer header. |
