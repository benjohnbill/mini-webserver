# Arena Scene: longtail (20261124)

Command: `bench/arena/scene.sh longtail none cache rate-ip rate-ja4 all`. Commit: `0cd878d`. Duration: 10 s for each preset. Origin: `./server --model pool --delay 10`. `bench/arena/human.sh` runs beside the Crawler script in each preset.

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
| none | longtail | 2700 | 200=2700 | 25 | 2736 | 2730 | 0 | 0 | 0 | 0 | 2730 | 219.4 |
| none | human | 20 | 200=20 | 28 | 20 | 20 | 0 | 0 | 0 | 0 | 20 | 0.0 |
| cache | longtail | 5160 | 200=5160 | 8 | 5212 | 5205 | 0 | 0 | 0 | 5002 | 203 | 16.7 |
| cache | human | 25 | 200=25 | 12 | 25 | 25 | 0 | 0 | 0 | 20 | 5 | 0.0 |
| rate-ip | longtail | 2790 | 200=2790 | 24 | 2857 | 2852 | 0 | 0 | 0 | 0 | 2852 | 252.6 |
| rate-ip | human | 35 | 200=35 | 26 | 35 | 35 | 0 | 0 | 0 | 0 | 35 | 0.0 |
| rate-ja4 | longtail | 2720 | 200=2720 | 25 | 2738 | 2735 | 0 | 0 | 0 | 0 | 2735 | 250.9 |
| rate-ja4 | human | 35 | 200=35 | 28 | 35 | 35 | 0 | 0 | 0 | 0 | 35 | 0.0 |
| all | longtail | 6960 | 200=6960 | 6 | 6983 | 6979 | 0 | 0 | 0 | 6776 | 203 | 16.7 |
| all | human | 30 | 200=30 | 10 | 30 | 30 | 0 | 0 | 0 | 25 | 5 | 0.0 |
