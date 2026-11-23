# Arena Scene: named (20261124)

Command: `bench/arena/scene.sh named none cache rate-ip rate-ja4 all`. Commit: `0cd878d`. Duration: 10 s for each preset. Origin: `./server --model pool --delay 10`. `bench/arena/human.sh` runs beside the Crawler script in each preset.

A Scene measures. It does not pass or fail. The owner slots `ja4_format`, `rate_limit_take`, and `verdict_decide` decide what a preset can do (SPEC §11 "Arena").

Presets:

| Preset | rproxy flags |
|---|---|
| none | `` |
| cache | `--cache on` |
| rate-ip | `--rate 5 --burst 10` |
| rate-ja4 | `--rate 5 --burst 10 --rate-key ja4` |
| all | `--cache on --rate 5 --burst 10 --rate-key ja4` |

Columns:

- Sent, Statuses, and Mean ms are the view of the client: the requests that ended before the stop, their status codes (000: no reply), and their mean time.
- The other columns count the rproxy log lines of the client. Pass, Deny, Slow, and Tarpit are the Verdicts. To Origin counts the requests that rproxy sent to the Origin. MiB from Origin is the sum of the reply bytes of those requests.

| Preset | Client | Sent | Statuses | Mean ms | Log lines | Pass | Deny | Slow | Tarpit | Cache hits | To Origin | MiB from Origin |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| none | named | 2630 | 200=2630 | 25 | 2678 | 2671 | 0 | 0 | 0 | 0 | 2671 | 219.9 |
| none | human | 25 | 200=25 | 27 | 25 | 25 | 0 | 0 | 0 | 0 | 25 | 0.0 |
| cache | named | 7200 | 200=7200 | 6 | 7216 | 7213 | 0 | 0 | 0 | 7013 | 200 | 16.6 |
| cache | human | 30 | 200=30 | 12 | 30 | 30 | 0 | 0 | 0 | 25 | 5 | 0.0 |
| rate-ip | named | 2950 | 200=2950 | 23 | 2992 | 2987 | 0 | 0 | 0 | 0 | 2987 | 248.2 |
| rate-ip | human | 25 | 200=25 | 27 | 25 | 25 | 0 | 0 | 0 | 0 | 25 | 0.0 |
| rate-ja4 | named | 2880 | 200=2880 | 24 | 2925 | 2921 | 0 | 0 | 0 | 0 | 2921 | 240.2 |
| rate-ja4 | human | 30 | 200=30 | 26 | 30 | 30 | 0 | 0 | 0 | 0 | 30 | 0.0 |
| all | named | 6700 | 200=6700 | 7 | 6736 | 6732 | 0 | 0 | 0 | 6532 | 200 | 16.6 |
| all | human | 25 | 200=25 | 13 | 25 | 25 | 0 | 0 | 0 | 20 | 5 | 0.0 |
