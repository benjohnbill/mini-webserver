# P3-share: fork, private and shared cache

Made by `bench/cache/run.sh P3-share` at 2026-11-23 20:15, commit 6b2ba59.
Upstream: `--model fork --delay 10`. Load: `wrk -t8 -c32 -d10s --latency -s bench/cache/zipf.lua` after a 3 s warm-up. Dataset: `www/bench`, 200 files, Zipf s = 1.

- hit rate: X-Cache HIT / (HIT + MISS) seen by wrk, in the measured window.
- Pss: the proxy and all its children, sampled 2 s before the end.
- shmem delta: /proc/meminfo Shmem at the Pss sample minus before the proxy started. It holds a shared cache that Pss misses. It is machine-wide, so about +-1 MiB is noise.
- errors: non-2xx replies plus socket errors. A row is valid only with 0.
- server counters: hits, misses, rejects, evictions, expired; warm-up included; summed over fork children; b = bytes in the cache at exit.
- p99 and Pss are the mean of the runs. req/s shows the range when runs > 1.

| variant | model | req/s | p99 ms | hit rate | Pss MiB | shmem delta MiB | errors | server counters | runs |
|---|---|---|---|---|---|---|---|---|---|
| share private | fork | 2617 | 20.49 | 0.0% | 0.1 | 0.2 | 0 | h=0 m=34033 r=0 e=0 x=0 b=1935.3MiB | 1 |
| share shared | fork | 5773 | 10.87 | 100.0% | 0.1 | 17.2 | 0 | h=74561 m=237 r=37 e=0 x=0 b=28.5MiB | 1 |
