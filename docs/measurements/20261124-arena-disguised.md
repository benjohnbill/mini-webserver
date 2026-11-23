# Arena Scene: disguised (20261124)

Command: `bench/arena/scene.sh disguised none cache rate-ip rate-ja4 all`. Commit: `0cd878d`. Duration: 10 s for each preset. Origin: `./server --model pool --delay 10`. `bench/arena/human.sh` runs beside the Crawler script in each preset.

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
| none | disguised | 2620 | 200=2620 | 25 | 2661 | 2656 | 0 | 0 | 0 | 0 | 2656 | 217.4 |
| none | human | 30 | 200=30 | 26 | 30 | 30 | 0 | 0 | 0 | 0 | 30 | 0.0 |
| cache | disguised | 6920 | 200=6920 | 6 | 6933 | 6928 | 0 | 0 | 0 | 6728 | 200 | 16.6 |
| cache | human | 25 | 200=25 | 11 | 25 | 25 | 0 | 0 | 0 | 20 | 5 | 0.0 |
| rate-ip | disguised | 2720 | 200=2720 | 24 | 2755 | 2750 | 0 | 0 | 0 | 0 | 2750 | 226.5 |
| rate-ip | human | 25 | 200=25 | 27 | 25 | 25 | 0 | 0 | 0 | 0 | 25 | 0.0 |
| rate-ja4 | disguised | 2940 | 200=2940 | 23 | 2962 | 2959 | 0 | 0 | 0 | 0 | 2959 | 245.5 |
| rate-ja4 | human | 20 | 200=20 | 26 | 20 | 20 | 0 | 0 | 0 | 0 | 20 | 0.0 |
| all | disguised | 7130 | 200=7130 | 6 | 7152 | 7144 | 0 | 0 | 0 | 6944 | 200 | 16.6 |
| all | human | 20 | 200=20 | 13 | 20 | 20 | 0 | 0 | 0 | 15 | 5 | 0.0 |
