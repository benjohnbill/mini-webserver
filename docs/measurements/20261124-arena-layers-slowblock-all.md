# Arena Scenes on Event loops with the block after five 429s: every layer, three runs (20261124)

Commit of the measured tree: `1e271c7`. Its code is `77200d7`: one Event loop for each CPU core, and with `--rate-key ip` a client over the limit after five 429s in the window goes in the block table. rproxy has no keep-alive: one request for each client connection. Command: `RUN=slowblock<n> bench/arena/scene.sh <crawler> none layers` for each of the 10 Crawler scripts, with n = 1, 2, 3. Duration: 10 s for each preset. The tables of each run are in `20261124-arena-<crawler>-rslowblock<n>.md`.

This run replaces `20261124-arena-layers-eventloop.md` (commit `48bf7a0`, no block after 429s) as the current baseline.

A Scene measures. It does not pass or fail. The clients and the servers run on the same machine (WSL, 22 CPUs), so the client scripts take CPU time from the servers. Compare the two presets of one Scene, not the absolute values.

## Medians of the three runs

CPU s is the CPU time of `rproxy` plus the CPU time of the Origin for the whole preset. Replies/s counts each reply with a status, also 429 and 402. A drop has no status.

| Crawler | Runs | Origin MiB none → layers | CPU s (rproxy + Origin) none → layers | crawler replies/s none → layers | human.py replies/s none → layers | human.py p95 ms none → layers |
|---|---|---|---|---|---|---|
| repeat | 3 | 4672.3 → 8.0 | 11.82 → 3.38 | 232.0 → 1.7 | 2.0 → 3.1 | 17 → 16 |
| longtail | 3 | 313.9 → 0.4 | 4.99 → 3.81 | 377.0 → 1.6 | 3.0 → 3.1 | 31 → 19 |
| named | 3 | 0.0 → 0.0 | 15.87 → 16.05 | 983.0 → 974.0 | 2.0 → 3.1 | 22 → 20 |
| disguised | 3 | 315.3 → 0.0 | 5.63 → 0.09 | 378.0 → 0.0 | 2.5 → 3.1 | 30 → 15 |
| spread | 3 | 0.0 → 0.0 | 15.63 → 11.11 | 1125.0 → 590.0 | 3.0 → 3.6 | 20 → 18 |
| polite | 3 | 0.1 → 0.1 | 0.10 → 0.10 | 1.0 → 1.0 | 3.0 → 3.1 | 16 → 15 |
| trapped | 3 | 0.0 → 0.0 | 0.86 → 0.89 | 0.8 → 0.8 | 2.5 → 2.6 | 16 → 14 |
| revalidate | 3 | 4842.3 → 8.0 | 11.07 → 3.41 | 240.4 → 1.7 | 3.0 → 3.1 | 20 → 15 |
| solver | 3 | 0.4 → 0.0 | 5.77 → 2.22 | 280.1 → 1.9 | 2.5 → 2.6 | 17 → 14 |
| signed | 3 | 0.0 → 16.6 | 7.21 → 7.16 | 297.0 → 261.2 | 2.5 → 3.1 | 18 → 21 |

## rproxy and the Origin, apart

| Crawler | rproxy CPU s none → layers | Origin CPU s none → layers | Crawler 200 and 304 per s none → layers | Crawler requests none → layers | Crawler drops layers |
|---|---|---|---|---|---|
| repeat | 8.49 → 3.38 | 3.33 → 0.00 | 232.0 → 1.0 | 2320 → 18330 | 18314 |
| longtail | 4.22 → 3.81 | 0.78 → 0.00 | 377.0 → 1.0 | 3770 → 14410 | 14394 |
| named | 15.86 → 16.05 | 0.00 → 0.00 | 0.0 → 0.0 | 9830 → 9740 | 0 |
| disguised | 4.72 → 0.09 | 0.91 → 0.00 | 378.0 → 0.0 | 3780 → 0 | 0 |
| spread | 15.62 → 11.11 | 0.01 → 0.00 | 0.0 → 0.0 | 11250 → 11770 | 5870 |
| polite | 0.10 → 0.10 | 0.00 → 0.00 | 1.0 → 1.0 | 10 → 10 | 0 |
| trapped | 0.85 → 0.89 | 0.01 → 0.00 | 0.8 → 0.8 | 6106 → 5862 | 5854 |
| revalidate | 8.04 → 3.41 | 3.03 → 0.00 | 240.4 → 1.0 | 2404 → 19144 | 19160 |
| solver | 5.32 → 2.22 | 0.44 → 0.00 | 280.1 → 0.7 | 2801 → 17941 | 17922 |
| signed | 7.21 → 7.11 | 0.00 → 0.05 | 0.0 → 261.2 | 2970 → 2612 | 0 |

## What the numbers show

- **The rate limit now saves rproxy CPU too.** For repeat, longtail, revalidate, and solver, the rproxy CPU in layers is 0.4 to 0.9 times the rproxy CPU in none. In `20261124-arena-layers-eventloop.md` it was 1.3 to 3 times. Each Crawler script gets 10 passes (the burst), about 8 replies of 429, and then drops before the TLS handshake.
- **A drop is fast, so the Crawler scripts send more.** They send 3.8 to 8 times more requests than in none. Almost all of them drop, so the rproxy CPU still goes down.
- **The other Scenes stay the same.** named, disguised, spread, polite, trapped, and signed get the same Verdicts as in `20261124-arena-layers-eventloop.md`.
- **human.py sees no change.** It got no 429 and no drop in any run. Its p95 stays between 14 ms and 21 ms in layers, and it gets 2.6 to 3.6 replies each second.
