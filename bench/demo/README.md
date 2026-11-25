# Back pressure demo — presenter flow

The design is in [SPEC 12](../../docs/spec-cjg.md). The measured results are in [`20261125-backpressure-demo.md`](../../docs/measurements/20261125-backpressure-demo.md). This file is the flow for the 2-minute talk.

## The load

200 Phones download one 50 MB file. They start over 30 s. Each Phone reads at the speed of its tier:

| Tier | Speed | Phones |
|---|---|---|
| `fast` | 50 MB/s | 170 |
| `5mbps` | 625 KB/s | 10 |
| `3mbps` | 375 KB/s | 10 |
| `1mbps` | 125 KB/s | 6 |
| `400kbps` | 50 KB/s | 4 |

The 400 kbps, 1 Mbps, 3 Mbps, and 5 Mbps tiers are the speed caps that Korean carriers put on a phone after its data allowance (QoS). The share of each tier is an assumption. The memory limit of rproxy is 1280 MiB. The dashboard calls it "server memory (scaled)": it is a small model of a server, not a real size.

## Before the talk

Build once, and make the file of the Phones.

```bash
make all cert
bench/demo/demo.sh act 4 dry    # a rehearsal run; it also makes www/update.bin
bench/demo/demo.sh stop
```

Split one terminal in two. Right pane: the dashboard. It runs for the whole talk and resets at each Act.

```bash
python3 bench/demo/dashboard.py
```

Left pane: the flow. Make the font large enough for the room.

```bash
bench/demo/demo.sh present
```

## The flow

`demo.sh present` has four Phases. Each Enter starts the next Phase. The bystander is one `curl` of `/index.html` through rproxy. The flow runs it at the end of each Phase and prints the result as a large banner. The lines in quotes are a draft. The owner decides the lines later.

| Phase | What the flow does | Bystander | Say |
|---|---|---|---|
| 0 평소 | Starts rproxy in the mode of Act 1 (`unbounded`). No Phones yet | `HTTP 200` | "앱 업데이트가 나온 날입니다. 휴대폰 200대가 50 MB 파일을 받습니다. 대부분은 빠르고, 몇 대는 데이터를 다 써서 느려진 휴대폰입니다." |
| 1 Act 1 | Starts the Phones. It waits until rproxy dies (60 s at most) | `연결 거부` | "빠른 휴대폰은 금방 끝나지만, 느린 휴대폰의 몫은 Reverse Proxy 메모리에 쌓입니다. 서버가 죽었고, 첫 페이지만 보려던 저까지 끊겼습니다." |
| 2 Act 2 | Starts Act 2 (`reply-cap`) | `HTTP 200` | "그래서 연결 하나가 쌓는 양에 4 MB 상한을 뒀습니다. 지금 저희 코드가 실제로 이렇습니다. 서버는 살지만, 파일이 50 MB라서 아무도 받지 못합니다." |
| 3 Act 4 | Starts Act 4 (`backpressure`). The bystander runs about 10 s later | `HTTP 200` | "쌓는 속도를 받는 속도에 맞췄습니다. 버퍼가 차면 Origin에서 읽기를 멈추고, 휴대폰이 가져가면 다시 읽습니다. 메모리는 평평하고, 빠른 휴대폰은 끝났고, 느린 휴대폰도 끊기지 않고 받고 있습니다." |
| end | The last Enter stops rproxy, the Phones, and the Origin | | "막아야 할 것은 크기도 사람 수도 아니고, 속도였습니다." |

Act 3 (`conn-cap`) is not in the talk. `demo.sh act 3` still runs it.

## Acceptance (2026-11-25, 3 runs of each Act)

| Act | Condition | Result of the 3 runs |
|---|---|---|
| 1 | OOM kill in 15–30 s. At the kill, the slow tiers hold 60 % or more of the relay bytes | Killed at 24.2, 24.3, and 24.4 s. Slow share 75, 75, and 78 %. Peak 1216–1221 MiB |
| 2 | All Phones get 502. rproxy stays alive | All 200 got 502 in each run. Peak 122–134 MiB |
| 4 | No Phone cut in 60 s. All `fast` Phones done. Peak under 1/10 of Act 1 | 0 cut, 170 `fast` done, 30 slow Phones receiving. Peak 24–25 MiB |

The relay-stats sum and the cgroup memory agree within 7 % in Act 1 (for example 1155.6 and 1165 MiB). In Act 4 the relay-stats sum is about 8 MiB of 23 MiB: the rest is the TLS buffers and the base memory of rproxy.

Two values came from these runs. With `fast` at 20 MB/s, about 14 `fast` Phones held 50 MB each at the kill, and the slow share was only 53–55 %. At 50 MB/s, fewer `fast` Phones hold a reply at the same time. With 1536 MiB and `fast` at 50 MB/s, rproxy did not die (peak 1370–1417 MiB), so the limit is 1280 MiB.

In Act 1, a slow Phone can go on receiving after rproxy dies: the kernel still sends what was in the send buffer of the socket. The Phone then fails at the end of that data.

## Rules for the presenter

- Say only the measured numbers.
- "We had this" is true only for Act 2 (the code of `CJG` before the Relay modes). Act 1 is a build with the limit removed.
- Do not say that the demo has the same ratios as a real case. The demo values are a small model, not a scaled copy.

## Change a value

Each value is a variable of `demo.sh`. For example:

```bash
MIX="fast:50000:100 1mbps:125:20" ARRIVAL=15 bench/demo/demo.sh act 4 dry
PRESENT_PAUSE=10 bench/demo/demo.sh present    # each Enter becomes a 10 s sleep
```

The variables: `MIX` (the tiers, `label:KB/s:count`), `ARRIVAL`, `FILE_BYTES`, `MEMORY_MAX`, `ORIGIN_TIMEOUT`, `RELAY_CONNS`, `RELAY_BUFFER`, `ORIGIN_PORT`, `PROXY_PORT`, `RPROXY_ARGS`, `PHONE_CPUS`, `SERVER_CPUS`, `PRESENT_PAUSE`.

By default the Phones run on the upper half of the CPUs, and rproxy and the Origin on the lower half. One `phones.py` process reads at most about 300 to 400 MB/s, so `demo.sh` starts one process for each tier.

If rproxy has `--relay-stats`, `demo.sh` passes it, and the dashboard shows the buffer share of each tier. If not, the block shows "no data".

## Epilogue (only if a rehearsal leaves time)

`demo.sh act 5` runs Act 4 with a `pool` Origin of 4 Workers. A slow Phone holds a Worker, so the others wait. Act 5 has no full run with the tiers. Run it to the end before you put it in the talk. Say: "이제는 느린 휴대폰이 Origin을 붙잡습니다. 이게 저희의 다음 고민입니다."

## Backup recording

`demo.sh act N rec` writes the scene marks of the Act to `bench/demo/run/actN.marks.json` (start, rproxy killed, Phones finished), in the format of the herz hackathon demo. The recording flow of that demo (Openscreen CLI) is in `~/dev/projects/maeum-mirror/demo-video/PLAYBOOK.md`, outside this repository.

## Files

| File | What it does |
|---|---|
| `demo.sh` | Starts and stops the Acts, and runs the `present` flow |
| `phones.py` | The TLS clients of one tier. It writes `run/status-<tier>.json` |
| `dashboard.py` | The terminal dashboard. `--once` prints one frame |
| `bystander.sh` | One `curl` of `/index.html` |
| `slow_origin_probe.py` | The probe that showed the `pool` stall |
