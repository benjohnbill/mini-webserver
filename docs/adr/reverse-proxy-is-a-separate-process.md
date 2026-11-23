---
status: proposed
branch: CJG
decided: 2026-11-24
---

# The Reverse Proxy is a separate process in front of the Origin

| Item | Content |
|---|---|
| Context | The server has a Proxy Route (`ROUTE_PROXY`, `proxy_fetch`, and the cache in `src/conn.c`). The owner wants the topic of the Reverse Proxy to be the fight between crawlers and the server: TLS termination, JA4, rate limit, cache, and a Verdict for each connection. These checks must run before the Origin spends work on a request. |
| Decision | The Reverse Proxy is a second program, `rproxy`, built from `src/rproxy/` in the same repository. It terminates TLS, computes JA4, decides a Verdict, and forwards the request to the Origin with `X-Forwarded-For` and `X-JA4`. The Origin is the existing `server`. It can bind to `127.0.0.1` only. `rproxy` starts on the `pool` Concurrency model. The cache moves to `rproxy`. The Proxy Route leaves the server after `rproxy` gives the same results on the cache Scenes of 2026-11-23. |
| Reasons | "Reverse Proxy 설계에 있어서 crawler vs. server의 구도가 핵심적인 차별점이자 토픽이 될거 같거든" (user said 2026-11-24). Same repository: "ok. 같은 레포." (user said 2026-11-24). |
| Rejected options | A Route in the server: the checks then share the Worker and the code of the Origin, and the Origin cannot hide behind the proxy. A separate repository: it must copy the Shared core, so the project has two parsers. `epoll` for `rproxy` first: a TLS read can need a write (`SSL_ERROR_WANT_WRITE`), so the event loop gets more states. |
| Consequences | The decision functions (`client_hello`, `ja4`, `rate_limit`, `verdict`) do not use sockets or OpenSSL I/O, so unit tests call them with bytes. `make lint` checks this. Until the Proxy Route leaves, the repository has two proxy paths. |
