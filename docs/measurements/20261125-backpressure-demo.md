---
scope: The Acts of the Back pressure demo (SPEC 12), run on the laptop of the owner on 2026-11-25
build: commit c004265 (rproxy Relay modes) and 149f3c7 (demo tool)
---

# Back pressure demo — Act results

Load in every Act: 200 Phones over 15 s, each reads `/update.bin` (8,000,000 bytes) at 400 KB/s over TLS. `rproxy` runs in a scope with `MemoryMax=1G` and no swap. The Origin is `./server --model epoll` (Act 5: `--model pool`). Command: `bench/demo/demo.sh act N rec`. Memory is the cgroup `memory.current` of the scope, the number that the OOM killer uses.

| Act | Relay mode | Runs | Server | Phones | Bystander | Memory peak | Ends at |
|---|---|---|---|---|---|---|---|
| 1 | `unbounded` | 3 | OOM-killed at 7.9, 8.0, 8.7 s (about 100 Phones in) | 200 fail | 200 before the kill, connection refused after it | 1 GiB (the limit) | 16 s |
| 2 | `reply-cap` | 1 | alive | 200 get 502 | 200 | 124 MiB | 15 s |
| 3 | `conn-cap` (20) | 1 | alive, CPU near 0 % | 20 done, 180 get 503 | 503 | 166 MiB during the run (from the run of the rproxy work) | 22 s |
| 4 | `backpressure` (64 KB) | 3 | alive | 200 done | 200 | 88, 88, 89 MiB (heap 30 MiB, TCP buffers 45–52 MiB) | 35 s |
| 5 | `backpressure`, `pool` Origin | 1 (12 s seen) | alive | at 12 s: 72 receiving, 40 waiting, 88 failed (status not recorded) | not run | 17 MiB | not run to the end |

## What limits each number

- Act 1 dies earlier than the SPEC estimate (10 s, 128 Phones). Each connection costs more than the 8 MB body: the reply buffer grows by doubling, and TLS and kernel buffers add to it.
- Act 4 memory is mostly kernel socket buffers, not the 64 KB relay buffers (200 × 64 KB = 12.5 MiB). The kernel doubles each `SO_SNDBUF` and `SO_RCVBUF` value. Before `rproxy` capped them, Act 4 peaked at 419–567 MiB, and the Origin closed some transfers after its 5 s `--timeout`, so Phones got short bodies.
- Act 3 ends at 22 s because 20 Phones take 20 s at 400 KB/s. The slot of a connection stays taken until its reply is sent, so no 21st Phone gets in.
- Act 5: a `pool` Origin has 4 Workers. A slow reader holds a Worker, and the other requests wait in the queue. The expected failure is 504 when the Origin does not reply in `--origin-timeout`. The run did not record the status, so this is not measured.

## Numbers for the talk

- Act 1: "about 8 seconds, about 100 phones".
- Act 4: "200 of 200, about 90 MB, against a 1 GB limit that Act 1 hit".
- Do not say 13 MB for Act 4.
