# Arena Scenes: every layer (20261124)

Commit: `67dcd15`. Command: `bench/arena/scene.sh <crawler> none layers` for each of the 10 Crawler scripts. Duration: 10 s for each preset. The full tables are in `20261124-arena-<crawler>-v2.md`. The v1 tables (`20261124-arena-<crawler>.md`) stay as they were.

A Scene measures. It does not pass or fail.

## The `layers` preset

```
--block-list bench/arena/layers-block.txt --ja4-list bench/arena/layers-ja4.txt
--block-time 600 --rate 5 --burst 10 --pow-prefix /api/ --pow-bits 16
--pay-prefix /bench/ --bot-key tests/data/bot_test_keys.txt --cache on
```

`robots.txt`, the honeypot, the User-Agent rules, and the scanner rule are always on, also in `none`.

## Results, `layers`

| Crawler | Sent | Statuses | Layer that answered | MiB from Origin | human.py |
|---|---|---|---|---|---|
| repeat | 13740 | 200=59 429=13681 | Rate limit | 8.0 | 200=25 401=1 |
| longtail | 9430 | 200=59 429=9371 | Rate limit | 1.8 | 200=30 401=1 |
| named | 10620 | 402=10620 | Pay (402) | 0.0 | 200=25 401=1 |
| disguised | 0 | - | Tarpit (false Chrome) | 0.0 | **0 replies** |
| spread | 10240 | 000=5120 402=5120 | Block list (drop), pay | 0.0 | 200=25 401=1 |
| polite | 10 | 200=10 | None: it obeys `robots.txt` | 0.1 | 200=25 401=1 |
| trapped | 6869 | 000=6860 200=9 | Honeypot, then block table (drop) | 0.0 | 200=20 401=1 |
| revalidate | 9614 | 200=4 304=55 429=9555 | Rate limit, 304 | 8.0 | 200=20 401=1 |
| solver | 8645 | 200=55 401=4 429=8586 | Proof of work, then rate limit | 0.0 | 200=20 401=1 |
| signed | 2375 | 200=2375 | Web Bot Auth: verified, passes | 16.6 | 200=20 401=1 |

## What limits each number

- **About 59 passes in 10 s** (repeat, longtail, revalidate, solver): the token bucket gives a burst of 10 and then 5 a second. 10 + 5 × 10 = 60. The rate key is the IP, and each of these scripts sends from one IP.
- **named**: it walks `/bench/`, which is the pay prefix. With no signature, each request gets 402. In `none` the same requests got 403.
- **spread**: it sends from 127.0.0.2 to .33. `layers-block.txt` holds 127.0.0.16/28, so 16 of the 32 addresses drop before the TLS handshake (000). The other 16 get 402.
- **trapped**: one hit on `/trap/` puts the IP in the block table. After that, every connection drops before the handshake: 6860 drops in 10 s.
- **signed**: it has the same User-Agent as named (GPTBot). The signature is the only difference, and it passes. The Origin sent 16.6 MiB: the dataset is 16.7 MiB, and the cache served the rest (2176 cache hits).
- **human.py**: one 401 in each run. It solves the proof of work for `/api/time` once and then sends the solution again inside its window.
- **disguised: neither client got a reply.** disguised sends a Chrome User-Agent with the JA4 of curl, so each request gets tarpit. A tarpit holds an rproxy Worker for `--tarpit` seconds. disguised opens enough connections to hold every Worker, so human.py waits in the queue for the full 10 s. This is a cost of the tarpit design (SPEC §11: a tarpit holds a Worker). A tarpit that does not hold a Worker (an event loop timer, or a drop) removes it.

## Known limit outside this Scene

The Origin answers 404 for a static path with a query string (`/bench/f001.txt?x`). `solver.py` sends no query for this reason.
