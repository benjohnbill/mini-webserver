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
The method that the server uses to serve more than one connection at the same time. The server has four: `iter`, `fork`, `pool`, and `epoll`.
_Avoid_: mode, strategy, dispatcher (the code may use `dispatch` as a verb only)

**Worker**:
An execution unit (a thread or a process) that serves one connection at a time. In `iter`, the main loop is the only worker. In `fork`, each child process is a worker. In `pool`, each thread of the pool is a worker. `epoll` has no worker.
_Avoid_: thread (when you mean a worker of any model), handler

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
The decision of the Reverse Proxy for one request: pass, deny (403), slow down (429), or tarpit.
_Avoid_: judgment, action, result

**Crawler script**:
A program in `bench/arena/` that acts as one kind of crawler against the Reverse Proxy. A Scene runs Crawler scripts and counts the Verdicts and the load on the Origin.
_Avoid_: bot, attacker, attack script
