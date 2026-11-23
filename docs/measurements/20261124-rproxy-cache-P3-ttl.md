# P3-ttl: TTL 1 s and 60 s

Made by `PROXY=rproxy bench/cache/run.sh P3-ttl` at 2026-11-24 02:55, commit fabafb5.
Upstream: `--model fork --delay 10`. Load: `wrk -t8 -c32 -d10s --latency -s bench/cache/zipf.lua https://localhost:19180` after a 3 s warm-up. Dataset: `www/bench`, 200 files, Zipf s = 1.

- hit rate: X-Cache HIT / (HIT + MISS) seen by wrk, in the measured window.
- Pss: the proxy and all its children, sampled 2 s before the end.
- shmem delta: /proc/meminfo Shmem at the Pss sample minus before the proxy started. It holds a shared cache that Pss misses. It is machine-wide, so about +-1 MiB is noise.
- errors: non-2xx replies plus socket errors. A row is valid only with 0.
- server counters: hits, misses, rejects, evictions, expired; warm-up included; summed over fork children; b = bytes in the cache at exit.
- p99 and Pss are the mean of the runs. req/s shows the range when runs > 1.

| variant | model | req/s | p99 ms | hit rate | Pss MiB | shmem delta MiB | errors | server counters | runs |
|---|---|---|---|---|---|---|---|---|---|
| ttl 1 | pool | 952 | 14.06 | 83.0% | 35.7 | 9.8 | 0 | h=10406 m=2162 r=67 e=0 x=1895 b=16.6MiB | 1 |
| ttl 60 | pool | 1583 | 7.92 | 100.0% | 29.9 | -0.6 | 0 | h=19724 m=208 r=8 e=0 x=0 b=16.6MiB | 1 |
