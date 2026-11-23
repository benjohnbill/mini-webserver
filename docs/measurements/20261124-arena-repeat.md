# Arena Scene: repeat (20261124)

Command: `bench/arena/scene.sh repeat none cache rate-ip rate-ja4 all`. Commit: `0cd878d`. Duration: 10 s for each preset. Origin: `./server --model pool --delay 10`. `bench/arena/human.sh` runs beside the Crawler script in each preset.

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
| none | repeat | 1840 | 200=1840 | 20 | 1877 | 1873 | 0 | 0 | 0 | 0 | 1873 | 3746.2 |
| none | human | 30 | 200=30 | 17 | 30 | 30 | 0 | 0 | 0 | 0 | 30 | 0.0 |
| cache | repeat | 3760 | 200=3760 | 9 | 3782 | 3779 | 0 | 0 | 0 | 3775 | 4 | 8.0 |
| cache | human | 35 | 200=35 | 10 | 35 | 35 | 0 | 0 | 0 | 30 | 5 | 0.0 |
| rate-ip | repeat | 1650 | 200=1650 | 22 | 1665 | 1664 | 0 | 0 | 0 | 0 | 1664 | 3328.1 |
| rate-ip | human | 30 | 200=30 | 20 | 30 | 30 | 0 | 0 | 0 | 0 | 30 | 0.0 |
| rate-ja4 | repeat | 1680 | 200=1680 | 22 | 1692 | 1690 | 0 | 0 | 0 | 0 | 1690 | 3380.1 |
| rate-ja4 | human | 25 | 200=25 | 21 | 25 | 25 | 0 | 0 | 0 | 0 | 25 | 0.0 |
| all | repeat | 3250 | 200=3250 | 10 | 3277 | 3275 | 0 | 0 | 0 | 3271 | 4 | 8.0 |
| all | human | 20 | 200=20 | 11 | 20 | 20 | 0 | 0 | 0 | 15 | 5 | 0.0 |
