# rproxy cache parity with the Proxy Route

SPEC §11, step 5. The question: does the cache in `rproxy` behave like the cache behind the Proxy Route of the server? The old rows are the `pool` rows of `20261123-cache-*.md`. The new rows are `20261124-rproxy-cache-*.md`. Both use the same dataset, the same Origin (`./server --model fork --delay 10`), and `wrk -t8 -c32 -d10s` after a 3 s warm-up. The binaries are commit `fabafb5`.

Criterion (decided by the lead): the hit rate and the cache counters must match within run noise. req/s and p99 need not match.

Verdict: **parity holds.** Each hit rate matches within 1.8 points, except TTL 1 s. The TTL 1 s gap comes from req/s, and a model predicts it (below). The counters match in shape: the same misses when the working set fits, and the same ratios when it does not.

## Noise floor

`admit all` (P2) and `evict fifo` (P3-evict) are the same settings: admit all, FIFO, 4 MiB. On 2026-11-23 they gave 61.4% and 62.8%, a spread of 1.4 points. In rproxy they gave 61.0% and 61.4%. So a difference of about 1.5 points is run noise.

## Working set fits (16.6 MiB in 64 MiB)

| variant | old hit rate | new hit rate | old counters | new counters |
|---|---|---|---|---|
| P1 on | 100.0% | 100.0% | m=216 r=16 e=0 x=0 b=16.6MiB | m=210 r=10 e=0 x=0 b=16.6MiB |
| P3-ttl 60 | 100.0% | 100.0% | m=213 r=13 e=0 x=0 | m=208 r=8 e=0 x=0 b=16.6MiB |
| lock global, 4 Workers | 100.0% | 100.0% | m=214 r=14 b=16.6MiB | m=210 r=10 b=16.6MiB |
| lock striped, 4 Workers | 100.0% | 100.0% | m=213 r=13 b=16.6MiB | m=211 r=11 b=16.6MiB |
| lock global, 16 Workers | 100.0% | 100.0% | m=237 r=37 b=16.6MiB | m=224 r=24 b=16.6MiB |
| lock striped, 16 Workers | 100.0% | 100.0% | m=243 r=43 b=16.6MiB | m=218 r=18 b=16.6MiB |

In both programs, misses = 200 + rejects. The dataset has 200 files, so each file misses once. The rejects are duplicate stores: two Workers miss the same file in the same 10 ms Origin delay, and the second store is rejected. rproxy has fewer duplicates, because fewer requests arrive in each 10 ms window (lower req/s). The hit counts are not compared. They scale with the request count.

## Working set does not fit (`--cache-bytes 4194304`)

The counts scale with the request count, so the table compares ratios of the server counters (warm-up included).

| variant | old hit rate | new hit rate | old miss share | new miss share | old evictions per miss | new evictions per miss | old rejects per miss | new rejects per miss |
|---|---|---|---|---|---|---|---|---|
| admit all | 61.4% | 61.0% | 39.2% | 39.2% | 0.93 | 0.93 | 6.6% | 5.2% |
| admit size | 82.9% | 82.6% | 17.4% | 18.2% | 0.81 | 0.80 | 16.6% | 13.2% |
| admit second | 63.4% | 61.6% | 37.4% | 38.3% | 0.84 | 0.87 | 15.1% | 12.0% |
| evict fifo | 62.8% | 61.4% | 38.1% | 38.6% | 0.93 | 0.94 | 6.4% | 5.2% |
| evict lru | 67.4% | 65.7% | 33.7% | 34.4% | 0.94 | 0.94 | 4.5% | 3.4% |

- miss share: misses / (hits + misses). evictions per miss: evictions / misses. rejects per miss: rejects / misses.
- The order of the variants is the same: size admission is best, LRU beats FIFO, second-hit admission is near admit all.
- rejects per miss is 1 to 4 points lower in rproxy. A part of the rejects is the duplicate stores of the section above, and rproxy has fewer of them for the same reason (lower req/s).

## TTL 1 s: the hit rate depends on req/s

| variant | old req/s | new req/s | old hit rate | new hit rate | old counters | new counters |
|---|---|---|---|---|---|---|
| ttl 1 | 11451 | 952 | 98.1% | 83.0% | h=135516 m=2895 r=168 x=2527 | h=10406 m=2162 r=67 x=1895 |

TTL is the only axis with time in it. After an entry expires, the next request for that file misses. So the misses per second have an upper limit (about one for each file for each TTL), and the hits per second grow with req/s. The hit rate then grows with req/s.

Model: a file with request rate λp (p from Zipf s = 1, 200 files) misses once in each cycle of TTL + 1/(λp). The check is `now - stored_at >= ttl` with `time(NULL)`, which has 1 s steps. So the effective TTL is between 0.5 s and 1 s.

| req/s | predicted, TTL 0.5 s | predicted, TTL 1 s | observed |
|---|---|---|---|
| 952 (rproxy) | 78.9% | 86.3% | 83.0% |
| 11451 (Proxy Route) | 96.8% | 98.3% | 98.1% |

Both observed values are in the predicted band. The gap is from req/s, not from the cache.

## req/s: what limits it

| variant | old req/s | new req/s |
|---|---|---|
| P1 on, 4 Workers | 29384 | 1357 |
| lock global, 4 Workers | 28660 | 1392 (1230 to 1511) |
| lock striped, 4 Workers | 29890 | 1326 (1211 to 1498) |
| lock global, 16 Workers | 19960 | 2456 (2294 to 2557) |
| lock striped, 16 Workers | 22821 | 2989 (2734 to 3142) |

rproxy does one TLS handshake for each request: it closes after each reply (SPEC §11), and wrk opens a new connection. So req/s is also handshakes per second. The old path was plain HTTP with keep-alive and no handshake.

Measured during a 10 s run at the bench settings (`/proc/<pid>/stat` and `/proc/<pid>/task/*/stat`, 8 s window, 22 cores):

- Each of the 4 rproxy Workers used 66% of a core. The machine was 30% idle. So no Worker was CPU-bound.
- wrk used about 800% CPU: its 8 threads were full.
- rproxy CPU for each request: 2.7 cores / 1290 req/s = 2.1 ms. `openssl speed rsa2048` gives 1113 signs/s on one core, so the RSA-2048 signature of the handshake alone is 0.9 ms. The old path used 240% CPU for 29384 req/s, which is 0.08 ms for each request. rproxy spends about 25 times more CPU for each request.

Control runs (same settings, `--cache on`, one 10 s run each):

| wrk threads | rproxy Workers | req/s | rproxy CPU | wrk CPU |
|---|---|---|---|---|
| 8 | 4 | 2468 | 276% | 795% |
| 16 | 4 | 796 | 214% | 1525% |
| 8 | 16 | 3054 | 649% | 706% |
| 16 | 16 | 2061 | 475% | 1021% |

- More rproxy Workers give more req/s. A Worker blocks on one connection for the full handshake, so 4 Workers serve at most 4 handshakes at a time.
- More wrk threads give less req/s and much more wrk CPU. wrk CPU for each request goes from 3.2 ms to 19 ms. The client work for one request does not change, so most of this wrk CPU is wait, not work. Likely cause (inferred, not profiled: `perf` is not available on this WSL kernel): wrk polls a socket that is writable while its TLS handshake waits for the server. The extra wrk threads take CPU time that rproxy needs.
- The run-to-run spread of req/s is large: the same settings gave 1290 to 2468. Compare req/s only inside one table.

The limiter: the TLS handshake for each connection, on both sides. On the rproxy side, the 4 blocking Workers and the handshake CPU (2.1 ms, of which 0.9 ms is the RSA signature). On the wrk side, its 8 threads are full. To measure the cache and not the handshake, a future run needs keep-alive in rproxy or TLS session resumption.

## Other columns

- shmem delta: rproxy has no shared cache (threads share the heap), so this column shows only other processes on the machine. It moved by up to 200 MiB in this session.
- Pss with the cache on is 29 to 30 MiB with 4 Workers, as on 2026-11-23 (28 MiB).
- All rows have 0 errors.
