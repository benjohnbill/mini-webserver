# Arena Scene: spread (20261124)

Command: `bench/arena/scene.sh spread none cache rate-ip rate-ja4 all`. Commit: `0cd878d`. Duration: 10 s for each preset. Origin: `./server --model pool --delay 10`. `bench/arena/human.sh` runs beside the Crawler script in each preset.

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
| none | spread | 2670 | 200=2670 | 25 | 2705 | 2701 | 0 | 0 | 0 | 0 | 2701 | 224.9 |
| none | human | 25 | 200=25 | 28 | 30 | 29 | 0 | 0 | 0 | 0 | 29 | 0.0 |
| cache | spread | 5750 | 200=5750 | 8 | 5767 | 5761 | 0 | 0 | 0 | 5561 | 200 | 16.6 |
| cache | human | 25 | 200=25 | 16 | 25 | 25 | 0 | 0 | 0 | 20 | 5 | 0.0 |
| rate-ip | spread | 2790 | 200=2790 | 24 | 2838 | 2833 | 0 | 0 | 0 | 0 | 2833 | 235.1 |
| rate-ip | human | 20 | 200=20 | 26 | 20 | 20 | 0 | 0 | 0 | 0 | 20 | 0.0 |
| rate-ja4 | spread | 3180 | 200=3180 | 22 | 3223 | 3219 | 0 | 0 | 0 | 0 | 3219 | 263.9 |
| rate-ja4 | human | 30 | 200=30 | 24 | 30 | 30 | 0 | 0 | 0 | 0 | 30 | 0.0 |
| all | spread | 8400 | 200=8400 | 5 | 8445 | 8438 | 0 | 0 | 0 | 8238 | 200 | 16.6 |
| all | human | 30 | 200=30 | 9 | 30 | 30 | 0 | 0 | 0 | 25 | 5 | 0.0 |
