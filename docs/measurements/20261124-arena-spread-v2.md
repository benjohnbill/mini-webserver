# Arena Scene: spread (20261124)

Command: `bench/arena/scene.sh spread none layers`. Commit: `67dcd15`. Duration: 10 s for each preset. Origin: `./server --model pool --delay 10`. `bench/arena/human.py` runs beside the Crawler script in each preset.

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
| none | spread | 11980 | 403=11980 | 3 | - | 11981 | 0 | 11980 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0.0 |
| none | human | 25 | 200=25 | 17 | 0.00 | 25 | 25 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 25 | 0.0 |
| layers | spread | 10240 | 000=5120 402=5120 | 3 | - | 10265 | 0 | 0 | 0 | 0 | 5120 | 0 | 5139 | 0 | 0 | 0.0 |
| layers | human | 26 | 200=25 401=1 | 12 | 0.23 | 26 | 25 | 0 | 0 | 0 | 0 | 1 | 0 | 16 | 9 | 0.0 |
