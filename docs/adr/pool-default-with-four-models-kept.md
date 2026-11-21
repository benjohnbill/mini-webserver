---
status: proposed
branch: CJG
decided: 2026-11-21
---

# The server keeps four Concurrency models, and `pool` is the default

| Item | Content |
|---|---|
| Context | The LMS statement asks for one or more of `fork()`, `pthread` ("요청 처리 스레드 풀 권장"), and I/O multiplexing. Each model keeps a Stuck connection away from the other connections in a different place: a process, a thread, or no wait at all. The owner learns the trade-offs best by use: "실제 사용을 해야 사실 각각의 동시성 모델이 뭐가 다르고, 장단점이 어떻게 되는 지를 알 수도 있긴 하겠고" (user said 2026-11-21). |
| Decision | The server has four Concurrency models: `iter`, `fork`, `pool`, and `epoll`. The command-line option `--model` selects one at start. The default is `pool`. All four stay in the code after the comparison. |
| Reasons | The statement recommends a thread pool. `iter` costs no code and shows the problem most clearly. The same Scene on all four models gives a table that the owner can explain. |
| Rejected options | One model only: then the owner cannot compare the models by use. `epoll` as the default: it has the most code, and the owner types it last. |
| Consequences | `iter`, `fork`, and `pool` share the blocking connection code (`conn`). `epoll` has its own loop, but it shares the framing check, the keep-alive decision, and `send_plan_some` with `conn`. The `--workers` option has an effect only on `pool`. |
