# P3-evict: eviction, --cache-bytes 4194304 (working set 17 MiB)

Made by `bench/cache/run.sh P3-evict` at 2026-11-23 20:12, commit 6b2ba59.
Upstream: `--model fork --delay 10`. Load: `wrk -t8 -c32 -d10s --latency -s bench/cache/zipf.lua` after a 3 s warm-up. Dataset: `www/bench`, 200 files, Zipf s = 1.

- hit rate: X-Cache HIT / (HIT + MISS) seen by wrk, in the measured window.
- Pss: the proxy and all its children, sampled 2 s before the end.
- errors: non-2xx replies plus socket errors. A row is valid only with 0.
- server counters: hits, misses, rejects, evictions, expired; warm-up included; summed over fork children.
- p99 and Pss are the mean of the runs. req/s shows the range when runs > 1.

| variant | model | req/s | p99 ms | hit rate | Pss MiB | errors | server counters | runs |
|---|---|---|---|---|---|---|---|---|
| evict fifo | pool | 937 | 67.53 | 62.8% | 5.2 | 0 | h=7420 m=4561 r=290 e=4264 x=0 | 1 |
| evict lru | pool | 1076 | 65.35 | 67.4% | 13.3 | 0 | h=9053 m=4612 r=206 e=4343 x=0 | 1 |
