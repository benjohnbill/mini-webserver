# mini-webserver

The names that the code and the docs of mini-webserver use. One concept has one name. Add a term when a design discussion settles it. General programming terms are here only when this project makes their meaning narrower.

## Users

**Verification client**:
A program that the LMS statement names to test the server: curl, a browser, a load tool (ab, wrk, hey), or a sender of an invalid request. Our users are the verification clients.
_Avoid_: user persona, customer

**Stuck connection**:
An open connection that does not send its full request. The server does not ask if the cause is an attack or a fault.
_Avoid_: slow client, dead connection, slowloris (use slowloris only for the attack by that name)

## Concurrency

**Concurrency model**:
The method that the server uses to serve more than one connection at the same time. The server has five: `iter`, `fork`, `pool`, `epoll`, and `epoll-pool`.
_Avoid_: mode, strategy, dispatcher (the code may use `dispatch` as a verb only)

**Worker**:
An execution unit (a thread or a process) that serves one connection at a time. In `iter`, the main loop is the only worker. In `fork`, each child process is a worker. In `pool`, each thread of the pool is a worker. `epoll` has no worker. In `epoll-pool`, each thread of the pool is a worker, but it only makes the Response plan of one request: the loop reads the request and sends the reply, so a slow client holds no worker.
_Avoid_: thread (when you mean a worker of any model), handler

**Event loop**:
A thread of `rproxy` that waits on one epoll instance and serves many connections at the same time. A connection that waits (a tarpit, a slow client) holds no thread. `rproxy` has one Event loop for each CPU core. The `server` program calls its own single loop the `epoll` Concurrency model.
_Avoid_: worker (in `rproxy`), reactor, event thread

**Scene**:
One experiment that we run with the same commands on each Concurrency model, to compare the models. The Scenes are S1 to S4.
_Avoid_: scenario, benchmark (a Scene can also have no measurement)

## Requests

**Route**:
A rule that sends a request to a static file or to a Handler.
_Avoid_: path rule, mapping

**Handler**:
A C function in the server that makes the response for one Route. It is the "dynamic request processing" of the LMS statement. It is not CGI.
_Avoid_: controller, endpoint, CGI

**Shared core**:
The modules that change a full request buffer into a Response plan. They do not use sockets. All four Concurrency models use the same Shared core.
_Avoid_: business logic, engine

**Response plan**:
The output of the Shared core for one request: the bytes of the status line and the headers, and the source of the body (bytes in memory, or an open file and its size).
_Avoid_: response object, reply

## Reverse Proxy

**Reverse Proxy**:
The program `rproxy`. It stands in front of the Origin, terminates TLS, decides a Verdict for each connection, and forwards the accepted requests to the Origin.
_Avoid_: edge, front proxy, gateway, shield

**Origin**:
The program `server` when it sits behind the Reverse Proxy. It serves the files and the Handlers.
_Avoid_: upstream, backend

**ClientHello**:
The first TLS message of a client. It is plaintext. The Reverse Proxy reads it with `MSG_PEEK` before the TLS handshake.
_Avoid_: hello packet, handshake (the handshake is all the messages)

**JA4**:
A fingerprint of a client, made from its ClientHello by the FoxIO JA4 rules, for example `t13d1516h2_8daaf6152771_e5627efa2ab1`. Two clients with the same TLS library usually have the same JA4.
_Avoid_: TLS hash, JA3 (an older fingerprint with different rules)

**Verdict**:
A decision of the Reverse Proxy for a client. It has two stages: the connection Verdict and the request Verdict.
_Avoid_: judgment, action, result

**Connection Verdict**:
The Verdict before the TLS handshake. It uses the client IP, the JA4, and the block table. It is pass, drop (close with no reply), or tarpit.
_Avoid_: pre-check, early Verdict, L4 Verdict

**Request Verdict**:
The Verdict after the request parse. It is pass, deny (403), slow down (429), tarpit, challenge (proof of work), or pay (402).
_Avoid_: HTTP Verdict, late Verdict

**Block table**:
The keys (a client IP or a JA4) that the Reverse Proxy blocks until a time. The connection Verdict reads it. A honeypot hit, a scanner, and a client over the rate limit after five 429 replies add a key.
_Avoid_: ban list, jail, deny list (the block list is the file of CIDR ranges, which does not change at run time)

**Block list**:
The file of CIDR ranges that `--block-list` names. A client IP in a range gets the connection Verdict drop. It does not change at run time.
_Avoid_: ban list, deny list, IP blacklist

**JA4 table**:
The file that `--ja4-list` names: a JA4 and a label on each line, for example `curl` or `browser`.
_Avoid_: fingerprint database, JA4 list (the flag name only)

**Behavior counter**:
A count for each key of the Reverse Proxy: requests, distinct targets, 404 replies, 429 replies, `robots.txt` reads, and honeypot hits. The key is the client IP. The counts start again at zero when the window of the key ends.
_Avoid_: stats, metrics, score

**Honeypot**:
The target `/trap/`. A hidden link in `www/index.html` points to it, and `robots.txt` disallows it. A person does not see the link, and a crawler that obeys `robots.txt` does not follow it. So a client that requests it is a crawler that does not obey.
_Avoid_: trap page, tripwire

**Proof of work**:
A challenge of the Reverse Proxy: the client must find a counter so that SHA-256 of the nonce and the counter starts with a given number of zero bits. It makes each page cost CPU time for the client.
_Avoid_: PoW captcha, puzzle, hashcash (the older scheme with this idea)

**Web Bot Auth**:
A signature on a request by a bot that names itself: RFC 9421 HTTP Message Signatures with an Ed25519 key that the Reverse Proxy knows. A verified bot skips the User-Agent and JA4 rules.
_Avoid_: bot token, signed agent, verified bot (use it only for a bot whose signature passed)

**Crawler script**:
A program in `bench/arena/` that acts as one kind of crawler against the Reverse Proxy. A Scene runs Crawler scripts and counts the Verdicts and the load on the Origin.
_Avoid_: bot, attacker, attack script

## Back pressure demo

**Back pressure**:
The rule "read the Origin only when the Relay buffer has room". The rule always applies. A connection is caught when its Relay buffer is full: then the Reverse Proxy stops reading the Origin socket, and TCP carries the stop back to the Origin `write()`. At the limit, back pressure waits; a cap refuses.
_Avoid_: flow control (the TCP mechanism under it), throttling, rate limit, "back pressure turns on"

**Relay buffer**:
The buffer of one client connection that holds the gap between `read()` from the Origin and `write()` to the client (`used - sent`). In the Relay mode `backpressure` it has a limit (`--relay-buffer`, 64 KiB). It is memory of the Reverse Proxy process, not a kernel buffer.
_Avoid_: send buffer (the kernel buffer of the socket), ring, window

**Relay mode**:
The way the Reverse Proxy moves a reply from the Origin to the client: `unbounded`, `reply-cap`, `conn-cap`, `backpressure`, or `memory-cap`. The flag `--relay` selects it.
_Avoid_: buffer mode, proxy mode

**Act**:
One part of the Back pressure demo. Each Act runs the same load with one Relay mode. The Acts are 1 to 4, and 5 is the epilogue.
_Avoid_: chapter, scene (a Scene is a measurement)

**Phone**:
One slow client of the demo load program `bench/demo/phones.py`. It reads at a fixed speed.
_Avoid_: bot, crawler, user
