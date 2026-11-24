# Arena Scene: signed (20261124)

Command: `bench/arena/scene.sh signed none layers`. Commit: `1e271c7`. Duration: 10 s for each preset. Origin: `./server --model pool --delay 10`. `bench/arena/human.py` runs beside the Crawler script in each preset.

A Scene measures. It does not pass or fail. The owner slots `ja4_format`, `rate_limit_take`, and `verdict_decide` decide what a preset can do (SPEC §11 "Arena").

Presets:

| Preset | rproxy flags |
|---|---|
| none | `` |
| layers | `--block-list bench/arena/layers-block.txt --ja4-list bench/arena/layers-ja4.txt --block-time 600 --rate 5 --burst 10 --pow-prefix /api/ --pow-bits 16 --pay-prefix /bench/ --bot-key tests/data/bot_test_keys.txt --cache on` |

Columns:

- Sent, Statuses, Mean ms, and P95 ms are the view of the client: the requests that ended before the stop, their status codes (000: no reply), their mean time, and their 95th percentile time. A request that is still in flight at the stop is in none of them. PoW CPU s is the CPU time that the client spent on proof of work solutions (Python clients only).
- The other columns count the rproxy log lines of the client. Pass, Deny, Slow, Tarpit, Drop, Challenge, and Pay are the Verdicts. Drop is the connection Verdict: no TLS byte goes out. To Origin counts the requests that rproxy sent to the Origin. MiB from Origin is the sum of the reply bytes of those requests.
- rproxy CPU s and Origin CPU s are the CPU time (user and system, all threads) of each program for the whole preset. Both client rows of a preset show the same two values.

| Preset | Client | Sent | Statuses | Mean ms | P95 ms | PoW CPU s | Log lines | Pass | Deny | Slow | Tarpit | Drop | Challenge | Pay | Cache hits | To Origin | MiB from Origin | rproxy CPU s | Origin CPU s |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| none | signed | 3177 | 403=3177 | 11 | 16 | 0.00 | 3182 | 0 | 3180 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 | 7.21 | 0.01 |
| none | human | 30 | 200=30 | 15 | 17 | 0.00 | 30 | 30 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 30 | 0.0 | 7.21 | 0.01 |
| layers | signed | 2463 | 200=2463 | 16 | 22 | 0.00 | 2467 | 2464 | 0 | 0 | 0 | 0 | 0 | 0 | 2264 | 200 | 16.6 | 7.10 | 0.04 |
| layers | human | 31 | 200=30 401=1 | 9 | 21 | 0.01 | 31 | 30 | 0 | 0 | 0 | 0 | 1 | 0 | 20 | 10 | 0.0 | 7.10 | 0.04 |
