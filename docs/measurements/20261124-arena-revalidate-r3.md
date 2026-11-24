# Arena Scene: revalidate (20261124)

Command: `bench/arena/scene.sh revalidate none layers`. Commit: `48bf7a0`. Duration: 10 s for each preset. Origin: `./server --model pool --delay 10`. `bench/arena/human.py` runs beside the Crawler script in each preset.

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
| none | revalidate | 2214 | 200=2214 | 17 | 20 | - | 2245 | 2242 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 2242 | 4484.2 | 8.87 | 3.47 |
| none | human | 20 | 200=20 | 15 | 19 | 0.00 | 20 | 20 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 20 | 0.0 | 8.87 | 3.47 |
| layers | revalidate | 10514 | 200=4 304=55 429=10455 | 3 | 6 | - | 10517 | 59 | 0 | 10457 | 0 | 0 | 0 | 0 | 55 | 4 | 8.0 | 11.35 | 0.00 |
| layers | human | 36 | 200=35 401=1 | 7 | 15 | 0.12 | 36 | 35 | 0 | 0 | 0 | 0 | 1 | 0 | 24 | 11 | 0.0 | 11.35 | 0.00 |
