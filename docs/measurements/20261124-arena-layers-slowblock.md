# Arena Scenes: five 429s put a client in the block table (20261124)

Commit of the code: `1170ba3`. The rule of `77200d7`: with `--rate-key ip`, a request over the limit from a client with five 429s in the window gets 429, and the client IP goes in the block table for `--block-time` (600 s in the layers preset). Command: `RUN=block<n> bench/arena/scene.sh <crawler> none layers` for repeat, longtail, revalidate, and solver, with n = 1, 2, 3. Duration: 10 s for each preset. The tables of each run are in `20261124-arena-<crawler>-rblock<n>.md`.

The other six Crawler scripts got no 429 in `20261124-arena-layers-eventloop.md`, so this rule does not change them. human.py got no 429 in any Scene.

A Scene measures. It does not pass or fail. The clients and the servers run on the same machine (WSL, 22 CPUs). Compare the presets of one Scene, not the absolute values.

## Medians of the three runs

"Before" is the layers column of `20261124-arena-layers-eventloop.md` (commit `48bf7a0`, no block after 429s).

| Crawler | rproxy CPU s none → layers before → layers now | Crawler requests layers before → now | Drop now | Origin MiB none → layers now | human.py p95 ms none → layers now |
|---|---|---|---|---|---|
| repeat | 8.97 → 11.51 → 3.39 | 8860 → 15920 | 15917 | 4442.2 → 8.0 | 18 → 18 |
| longtail | 5.70 → 15.94 → 3.77 | 9100 → 12280 | 12264 | 316.7 → 0.4 | 26 → 31 |
| revalidate | 10.56 → 11.35 → 3.40 | 10514 → 18984 | 18990 | 3772.2 → 8.0 | 21 → 16 |
| solver | 5.55 → 15.91 → 2.26 | 8295 → 17695 | 17680 | 0.4 → 0.0 | 17 → 16 |

## What the numbers show

- **The block removes the cost that the rate limit moved to rproxy.** The rproxy CPU in layers goes down 3.4 to 7 times, to less than the rproxy CPU with no defense. Each Crawler script gets 10 passes (the burst), about 8 replies of 429, and then only drops.
- **A drop is cheap, so the Crawler scripts send more.** They send 1.3 to 2.1 times more requests than before. Each request now ends before the TLS handshake, so the rproxy CPU still goes down.
- **The Origin saves the same as before.** The Origin CPU stays at about 0.
- **human.py sees no change.** It got no 429, so it never went in the block table. Its p95 stays between 16 ms and 31 ms. Its one 401 in each layers run is the Proof of work challenge, the same as before.
