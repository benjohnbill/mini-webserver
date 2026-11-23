# P2: admission, --cache-bytes 4194304

Made by `PROXY=rproxy bench/cache/run.sh P2` at 2026-11-24 02:53, commit fabafb5.
Upstream: `--model fork --delay 10`. Load: `wrk -t8 -c32 -d10s --latency -s bench/cache/zipf.lua https://localhost:19180` after a 3 s warm-up. Dataset: `www/bench`, 200 files, Zipf s = 1.

- hit rate: X-Cache HIT / (HIT + MISS) seen by wrk, in the measured window.
- Pss: the proxy and all its children, sampled 2 s before the end.
- shmem delta: /proc/meminfo Shmem at the Pss sample minus before the proxy started. It holds a shared cache that Pss misses. It is machine-wide, so about +-1 MiB is noise.
- errors: non-2xx replies plus socket errors. A row is valid only with 0.
- server counters: hits, misses, rejects, evictions, expired; warm-up included; summed over fork children; b = bytes in the cache at exit.
- p99 and Pss are the mean of the runs. req/s shows the range when runs > 1.

| variant | model | req/s | p99 ms | hit rate | Pss MiB | shmem delta MiB | errors | server counters | runs |
|---|---|---|---|---|---|---|---|---|---|
| admit all | pool | 537 | 22.78 | 61.0% | 12.0 | 0.5 | 0 | h=4412 m=2841 r=148 e=2629 x=0 b=4.0MiB | 1 |
| admit size | pool | 724 | 30.18 | 82.6% | 17.3 | 0.0 | 0 | h=7594 m=1686 r=222 e=1344 x=0 b=4.0MiB | 1 |
| admit second | pool | 512 | 31.17 | 61.6% | 12.0 | 0.2 | 0 | h=4327 m=2684 r=322 e=2325 x=0 b=3.2MiB | 1 |
