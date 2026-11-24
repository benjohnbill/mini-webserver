# Arena Scenes on Event loops: every layer, three runs (20261124)

Commit of the code: `48bf7a0` (one Event loop for each CPU core). Command: `RUN=<n> bench/arena/scene.sh <crawler> none layers` for each of the 10 Crawler scripts, with n = 1, 2, 3. Duration: 10 s for each preset. The tables of each run are in `20261124-arena-<crawler>-r<n>.md`.

A Scene measures. It does not pass or fail. The clients and the servers run on the same machine (WSL, 22 CPUs), so the client scripts take CPU time from the servers. Compare the two presets of one Scene, not the absolute values.

## Medians of the three runs

CPU s is the CPU time of `rproxy` plus the CPU time of the Origin for the whole preset. Replies/s counts each reply with a status, also 429 and 402.

| Crawler | Runs | Origin MiB none → layers | CPU s (rproxy + Origin) none → layers | crawler replies/s none → layers | human.py replies/s none → layers | human.py p95 ms none → layers |
|---|---|---|---|---|---|---|
| repeat | 3 | 4564.2 → 8.0 | 12.09 → 11.51 | 228.0 → 886.0 | 2.5 → 2.6 | 19 → 18 |
| longtail | 3 | 338.0 → 5.7 | 6.43 → 15.97 | 370.0 → 910.0 | 2.5 → 2.6 | 27 → 19 |
| named | 3 | 0.0 → 0.0 | 15.85 → 15.96 | 900.0 → 867.0 | 3.5 → 2.6 | 21 → 20 |
| disguised | 3 | 312.2 → 0.0 | 5.94 → 0.09 | 372.0 → 0.0 | 2.5 → 2.6 | 26 → 15 |
| spread | 3 | 0.0 → 0.0 | 15.56 → 11.06 | 1028.0 → 532.0 | 3.5 → 2.6 | 23 → 19 |
| polite | 3 | 0.1 → 0.1 | 0.12 → 0.09 | 1.0 → 1.0 | 3.5 → 3.1 | 16 → 15 |
| trapped | 3 | 0.0 → 0.0 | 1.21 → 0.83 | 1.0 → 0.8 | 2.5 → 2.6 | 16 → 15 |
| revalidate | 3 | 4706.3 → 8.0 | 11.64 → 11.35 | 232.4 → 1051.4 | 2.0 → 3.1 | 17 → 16 |
| solver | 3 | 0.4 → 0.0 | 5.76 → 15.92 | 282.8 → 829.5 | 2.5 → 3.1 | 17 → 16 |
| signed | 3 | 0.0 → 16.6 | 7.07 → 7.45 | 257.4 → 214.9 | 2.5 → 3.1 | 19 → 21 |

## rproxy and the Origin, apart

| Crawler | rproxy CPU s none → layers | Origin CPU s none → layers | Crawler 200 and 304 per s none → layers | Crawler requests none → layers |
|---|---|---|---|---|
| repeat | 8.69 → 11.51 | 3.40 → 0.00 | 228.0 → 5.9 | 2280 → 8860 |
| longtail | 5.38 → 15.94 | 1.05 → 0.03 | 370.0 → 5.9 | 3700 → 9100 |
| named | 15.84 → 15.96 | 0.01 → 0.00 | 0.0 → 0.0 | 9000 → 8670 |
| disguised | 4.89 → 0.09 | 1.05 → 0.00 | 372.0 → 0.0 | 3720 → 0 |
| spread | 15.55 → 11.06 | 0.01 → 0.00 | 0.0 → 0.0 | 10280 → 10600 |
| polite | 0.11 → 0.09 | 0.01 → 0.00 | 1.0 → 1.0 | 10 → 10 |
| trapped | 1.21 → 0.83 | 0.00 → 0.00 | 1.0 → 0.8 | 7538 → 5664 |
| revalidate | 8.34 → 11.35 | 3.30 → 0.00 | 232.4 → 5.9 | 2324 → 10514 |
| solver | 5.33 → 15.91 | 0.43 → 0.01 | 282.8 → 5.5 | 2828 → 8295 |
| signed | 7.07 → 7.35 | 0.00 → 0.10 | 0.0 → 214.9 | 2574 → 2149 |

## What the numbers show

- **The rate limit moves cost from the Origin to rproxy.** For repeat, longtail, revalidate, and solver, the Origin CPU goes to about 0 and the Origin bytes go down by 59 to 588 times. But the rproxy CPU goes up 1.3 to 3 times. A 429 comes back fast, so each Crawler script sends 2.5 to 4.5 times more requests, and each request is a new TLS handshake (rproxy has no keep-alive).
- **A drop saves rproxy CPU.** spread: half of the requests drop before the TLS handshake, and the rproxy CPU goes from 15.6 s to 11.1 s.
- **The tarpit is a timer now.** disguised: the rproxy CPU goes from 4.9 s to 0.09 s, and human.py gets its replies (p95 15 ms). On the Worker pool (`f9dda4d`, `20261124-arena-layers.md`), human.py got no reply beside disguised.
- **human.py sees no change.** In all Scenes its p95 stays between 15 ms and 27 ms and it gets 2.0 to 3.5 replies each second.
