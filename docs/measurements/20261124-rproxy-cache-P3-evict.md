# P3-evict: eviction, --cache-bytes 4194304 (working set 17 MiB)

Made by `PROXY=rproxy bench/cache/run.sh P3-evict` at 2026-11-24 02:54, commit fabafb5.
Upstream: `--model fork --delay 10`. Load: `wrk -t8 -c32 -d10s --latency -s bench/cache/zipf.lua https://localhost:19180` after a 3 s warm-up. Dataset: `www/bench`, 200 files, Zipf s = 1.

- hit rate: X-Cache HIT / (HIT + MISS) seen by wrk, in the measured window.
- Pss: the proxy and all its children, sampled 2 s before the end.
- shmem delta: /proc/meminfo Shmem at the Pss sample minus before the proxy started. It holds a shared cache that Pss misses. It is machine-wide, so about +-1 MiB is noise.
- errors: non-2xx replies plus socket errors. A row is valid only with 0.
- server counters: hits, misses, rejects, evictions, expired; warm-up included; summed over fork children; b = bytes in the cache at exit.
- p99 and Pss are the mean of the runs. req/s shows the range when runs > 1.

| variant | model | req/s | p99 ms | hit rate | Pss MiB | shmem delta MiB | errors | server counters | runs |
|---|---|---|---|---|---|---|---|---|---|
| evict fifo | pool | 564 | 23.82 | 61.4% | 17.8 | 0.0 | 0 | h=4555 m=2864 r=149 e=2682 x=0 b=3.1MiB | 1 |
| evict lru | pool | 654 | 18.90 | 65.7% | 15.7 | 0.0 | 0 | h=5321 m=2789 r=94 e=2616 x=0 b=2.5MiB | 1 |
