# Arena Scene: longtail (20261124)

Command: `bench/arena/scene.sh longtail none cache`. Commit: `026d93d`. Duration: 10 s for each preset. Origin: `./server --model pool --delay 10`. `bench/arena/human.sh` runs beside the Crawler script in each preset.

A Scene measures. It does not pass or fail. The owner slots `ja4_format`, `rate_limit_take`, and `verdict_decide` decide what a preset can do (SPEC §11 "Arena").

Presets:

| Preset | rproxy flags |
|---|---|
| none | `` |
| cache | `--cache on --cache-bytes 4194304` |

Columns:

- Sent, Statuses, and Mean ms are the view of the client: the requests that ended before the stop, their status codes (000: no reply), and their mean time.
- The other columns count the rproxy log lines of the client. Pass, Deny, Slow, and Tarpit are the Verdicts. To Origin counts the requests that rproxy sent to the Origin. MiB from Origin is the sum of the reply bytes of those requests.

| Preset | Client | Sent | Statuses | Mean ms | Log lines | Pass | Deny | Slow | Tarpit | Cache hits | To Origin | MiB from Origin |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| none | longtail | 2900 | 200=2900 | 24 | 2920 | 2917 | 0 | 0 | 0 | 0 | 2917 | 215.4 |
| none | human | 30 | 200=30 | 27 | 30 | 30 | 0 | 0 | 0 | 0 | 30 | 0.0 |
| cache | longtail | 3870 | 200=3870 | 18 | 3893 | 3890 | 0 | 0 | 0 | 967 | 2923 | 260.3 |
| cache | human | 30 | 200=30 | 24 | 30 | 30 | 0 | 0 | 0 | 0 | 30 | 0.0 |
