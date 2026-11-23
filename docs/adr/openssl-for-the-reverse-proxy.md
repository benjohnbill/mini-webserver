---
status: proposed
branch: CJG
decided: 2026-11-24
---

# The Reverse Proxy uses OpenSSL for TLS and SHA-256

| Item | Content |
|---|---|
| Context | `CONVENTION.md` allows only the C standard library and POSIX. The LMS statement bans only complete HTTP and event loop libraries (libevent, libuv, nginx, Apache, mongoose). The Reverse Proxy must terminate TLS, and JA4 needs SHA-256. A TLS implementation in our own code is not safe and is not the topic of the week. |
| Decision | `rproxy` links OpenSSL (`-lssl -lcrypto`, version 3). OpenSSL does the TLS handshake, the record encryption, and SHA-256. Our code reads the plaintext ClientHello with `recv(..., MSG_PEEK)` before the handshake, and computes JA4 itself. The `server` program does not link OpenSSL. |
| Reasons | "TLS 라이브러리는 사용할 거야. 이건 확정." and, on who decided it, "내 결정." (user said 2026-11-24). |
| Rejected options | No TLS: then JA4 has no ClientHello to read, and the Reverse Proxy cannot see the TLS fingerprint of a crawler. A JA4 library: the owner wants to write the JA4 code. |
| Consequences | This is an exception to the "Code" rule of `CONVENTION.md`. The team decides it in the PR to `main`. Until then, the exception is on `CJG` only. OpenSSL also has Ed25519, so proof of work and Web Bot Auth become possible. Proof of work stays off by default, because curl, ab, wrk, and hey cannot pass it. |
