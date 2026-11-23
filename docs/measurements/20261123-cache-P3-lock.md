# P3-lock: lock global and striped, pool with 4 and 16 Workers

Made by `bench/cache/run.sh P3-lock` at 2026-11-23 20:17, commit 6b2ba59.
Upstream: `--model fork --delay 10`. Load: `wrk -t8 -c32 -d10s --latency -s bench/cache/zipf.lua` after a 3 s warm-up. Dataset: `www/bench`, 200 files, Zipf s = 1.

- hit rate: X-Cache HIT / (HIT + MISS) seen by wrk, in the measured window.
- Pss: the proxy and all its children, sampled 2 s before the end.
- shmem delta: /proc/meminfo Shmem at the Pss sample minus before the proxy started. It holds a shared cache that Pss misses. It is machine-wide, so about +-1 MiB is noise.
- errors: non-2xx replies plus socket errors. A row is valid only with 0.
- server counters: hits, misses, rejects, evictions, expired; warm-up included; summed over fork children; b = bytes in the cache at exit.
- p99 and Pss are the mean of the runs. req/s shows the range when runs > 1.

| variant | model | req/s | p99 ms | hit rate | Pss MiB | shmem delta MiB | errors | server counters | runs |
|---|---|---|---|---|---|---|---|---|---|
| lock global, workers 4 | pool | 28660 (27491 to 29322) | 4.00 | 100.0% | 27.9 | 0.0 | 0 | h=359596 m=214 r=14 e=0 x=0 b=16.6MiB | 3 |
| lock striped, workers 4 | pool | 29890 (29192 to 30468) | 4.55 | 100.0% | 28.0 | 0.0 | 0 | h=365899 m=213 r=13 e=0 x=0 b=16.6MiB | 3 |
| lock global, workers 16 | pool | 19960 (18769 to 22036) | 5.94 | 100.0% | 56.5 | 0.0 | 0 | h=278596 m=237 r=37 e=0 x=0 b=16.6MiB | 3 |
| lock striped, workers 16 | pool | 22821 (22431 to 23229) | 4.81 | 100.0% | 57.6 | -0.0 | 0 | h=294328 m=243 r=43 e=0 x=0 b=16.6MiB | 3 |
