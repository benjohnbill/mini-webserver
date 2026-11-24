---
status: accepted
branch: CJG
decided: 2026-11-24
---

# The Reverse Proxy runs one Event loop for each CPU core

| Item | Content |
|---|---|
| Context | `rproxy` used the pool Concurrency model: 4 Workers, and each Worker served one connection from the first byte to the close. The `layers` Scene of 2026-11-24 showed the cost: a tarpit holds a Worker for 10 s, so 4 tarpitted connections of `disguised` held all Workers, and `human.py` got 0 replies (`docs/measurements/20261124-arena-layers.md`). A slow TLS handshake holds a Worker in the same way. |
| Decision | `rproxy` runs N Event loops, one thread each. N is the count of online CPU cores (`--loops N` changes it). Each Event loop has its own epoll instance and its own listen socket with `SO_REUSEPORT`, so the kernel spreads the new connections over the loops. Each connection is a state machine: ClientHello peek, connection Verdict, TLS handshake, request read, request Verdict, Origin exchange, reply, close. All sockets are non-blocking. A tarpit is a timer, not a sleep. The decision tables (rate limit, block table, Behavior counters, cache) stay one copy in one process, and their mutexes stay. |
| Reasons | "실전 구조로 만들고 싶어. 내가 공부하는 것은 나중이 될 지라도." and "코어 개수만큼 늘리는 것도 하고 싶고." (user said 2026-11-24). On the pool: "내가 서비스를 런칭한다고 하면, 사실 그냥 worker(pool) 구조로는 거의 쓸 일 없잖아." (user said 2026-11-24). |
| Rejected options | A tarpit thread only: it removes the measured starvation, but a slow handshake still holds a Worker, and the structure stays the pool. One process for each core with `fork` (nginx): each process gets its own copy of the decision tables, so a block in one process does not reach the others. A fix needs shared memory and a lock between processes. Envoy and HAProxy use threads for this reason. libaio: it does asynchronous file I/O, not socket I/O. io_uring: the same state machine, plus OpenSSL memory BIOs and a new library. |
| Consequences | `--workers` of `rproxy` is replaced by `--loops`. The `server` program keeps its four Concurrency models. A decision module must stay fast, because it runs on an Event loop thread and blocks that loop while it runs. Across more than one machine, the decision tables need a shared store (for example Redis). This ADR does not cover that case. |
