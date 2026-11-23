# Arena Scene: signed (20261124)

Command: `bench/arena/scene.sh signed none layers`. Commit: `67dcd15`. Duration: 10 s for each preset. Origin: `./server --model pool --delay 10`. `bench/arena/human.py` runs beside the Crawler script in each preset.

A Scene measures. It does not pass or fail. The owner slots `ja4_format`, `rate_limit_take`, and `verdict_decide` decide what a preset can do (SPEC §11 "Arena").

Presets:

| Preset | rproxy flags |
|---|---|
| none | `` |
| layers | `--block-list bench/arena/layers-block.txt --ja4-list bench/arena/layers-ja4.txt --block-time 600 --rate 5 --burst 10 --pow-prefix /api/ --pow-bits 16 --pay-prefix /bench/ --bot-key tests/data/bot_test_keys.txt --cache on` |

Columns:

- Sent, Statuses, and Mean ms are the view of the client: the requests that ended before the stop, their status codes (000: no reply), and their mean time. PoW CPU s is the CPU time that the client spent on proof of work solutions (Python clients only).
- The other columns count the rproxy log lines of the client. Pass, Deny, Slow, Tarpit, Drop, Challenge, and Pay are the Verdicts. Drop is the connection Verdict: no TLS byte goes out. To Origin counts the requests that rproxy sent to the Origin. MiB from Origin is the sum of the reply bytes of those requests.

| Preset | Client | Sent | Statuses | Mean ms | PoW CPU s | Log lines | Pass | Deny | Slow | Tarpit | Drop | Challenge | Pay | Cache hits | To Origin | MiB from Origin |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| none | signed | 2536 | 403=2536 | 13 | 0.00 | 2540 | 0 | 2536 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| none | human | 25 | 200=25 | 16 | 0.00 | 25 | 25 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 25 | 0.0 |
| layers | signed | 2375 | 200=2375 | 17 | 0.00 | 2382 | 2376 | 0 | 0 | 0 | 0 | 0 | 0 | 2176 | 200 | 16.6 |
| layers | human | 21 | 200=20 401=1 | 11 | 0.05 | 21 | 20 | 0 | 0 | 0 | 0 | 1 | 0 | 12 | 8 | 0.0 |
