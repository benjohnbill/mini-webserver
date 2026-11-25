# Back pressure demo — presenter flow

The design is in [SPEC 12](../../docs/spec-cjg.md). The measured results are in [`20261125-backpressure-demo.md`](../../docs/measurements/20261125-backpressure-demo.md). This file is the flow for the 2-minute talk.

## Before the talk

Build once, and make the file of the Phones.

```bash
make all cert
bench/demo/demo.sh act 4 dry    # a full rehearsal run; it also makes www/update.bin
bench/demo/demo.sh stop
```

Split one terminal in two. Right pane: the dashboard. It runs for the whole talk and resets at each Act.

```bash
python3 bench/demo/dashboard.py
```

Left pane: the commands below. Make the font large enough for the room.

## The flow

The times are from the start of the demo. Each Act command returns at once. The Act goes on in the background, and the dashboard shows it. The lines in quotes are the plan of the presenter.

| Time | Do | Screen | Say |
|---|---|---|---|
| 0–10 s | nothing | empty grid | "앱 업데이트가 나온 날입니다. 휴대폰 200대가 느린 망으로 8 MB 파일을 한꺼번에 받습니다." |
| 10 s | `demo.sh act 1` | the grid turns blue, the memory line climbs | "Origin은 빠르게 보내고, 휴대폰은 천천히 받습니다. 그 차이를 Reverse Proxy가 메모리에 전부 쌓습니다." |
| about 18 s | wait for the red X (about 8 s after the start), then `bystander.sh` | killed, the grid turns red | "서버가 죽었습니다. 업데이트와 상관없이 첫 페이지만 보려던 저까지 끊겼습니다." |
| 35 s | `demo.sh act 2` | memory stays low, the grid turns red (502) | "그래서 연결 하나가 쌓는 양에 4 MB 상한을 뒀습니다. 지금 저희 코드가 실제로 이렇습니다. 서버는 살지만, 업데이트 파일이 8 MB라서 아무도 받지 못합니다." |
| 50 s | `demo.sh act 3`, then `bystander.sh` | 20 blue, the rest red (503), CPU near 0 % | "그러면 한 번에 스무 명만 받으면 어떨까요? 서버는 놀고 있는데, 저도 거절당했습니다." |
| 65 s | `demo.sh act 4` | all 200 blue, each at its own speed, the memory line stays near the floor | "진짜 원인은 받아 가는 속도보다 빨리 쌓는 것이었습니다. 그래서 쌓는 속도를 받는 속도에 맞췄습니다. 버퍼가 차면 Origin에서 읽기를 멈추고, 휴대폰이 가져가면 다시 읽습니다." |
| about 80 s | `bystander.sh` | HTTP 200 | "저도 바로 받습니다." |
| about 100 s | nothing | all green | "200명 모두 자기 속도로 다 받았습니다. 메모리는 약 90 MB, 1막이 터진 1 GB의 십분의 일도 안 됩니다." |
| 110–120 s | `demo.sh stop` | | "서버를 지키려고 크기를 막고, 사람 수를 막았지만, 막아야 할 것은 속도였습니다." |

Commands in the left pane, in order:

```bash
bench/demo/demo.sh act 1
bench/demo/bystander.sh
bench/demo/demo.sh act 2
bench/demo/demo.sh act 3
bench/demo/bystander.sh
bench/demo/demo.sh act 4
bench/demo/bystander.sh
bench/demo/demo.sh stop
```

## Epilogue (only if a rehearsal leaves time)

`demo.sh act 5` runs Act 4 with a `pool` Origin of 4 Workers. A slow Phone holds a Worker, so the others wait. Act 5 ran once, for 12 s only: 72 Phones receiving and 88 failed, with no record of the status of the failed Phones. Run it to the end before you put it in the talk. Say: "이제는 느린 휴대폰이 Origin을 붙잡습니다. 이게 저희의 다음 고민입니다."

```bash
bench/demo/demo.sh act 5
```

## Rules for the presenter

- In Act 1, run `bystander.sh` only after the red X. Before the kill it gets 200.
- Say only the measured numbers: about 8 s and about 100 Phones for Act 1, about 90 MB for Act 4.
- "We had this" is true only for Act 2 (the code of `CJG` before the Relay modes). Do not say that Act 3 is the branch `hjm`: with its defaults (8 threads, a queue of 256), `hjm` rejects none of 200 Phones. Act 1 is a build with the limit removed.
- Do not say that the demo has the same ratios as a real case. The demo values (200 Phones, 8 MB, 400 KB/s) are a small model, not a scaled copy.

## Change a value

Each value is a variable of `demo.sh`. For example:

```bash
PHONES=100 RATE=300 bench/demo/demo.sh act 4 dry
```

The variables: `PHONES`, `ARRIVAL`, `RATE`, `FILE_BYTES`, `MEMORY_MAX`, `RELAY_CONNS`, `RELAY_BUFFER`, `ORIGIN_PORT`, `PROXY_PORT`, `RPROXY_ARGS`.

## Backup recording

`demo.sh act N rec` writes the scene marks of the Act to `bench/demo/run/actN.marks.json` (start, rproxy killed, Phones finished), in the format of the herz hackathon demo. The recording flow of that demo (Openscreen CLI) is in `~/dev/projects/maeum-mirror/demo-video/PLAYBOOK.md`, outside this repository.

## Files

| File | What it does |
|---|---|
| `demo.sh` | Starts and stops the Acts |
| `phones.py` | The slow TLS clients. It writes `run/status.json` |
| `dashboard.py` | The terminal dashboard. `--once` prints one frame |
| `bystander.sh` | One `curl` of `/index.html` |
| `slow_origin_probe.py` | The probe that showed the `pool` stall |
