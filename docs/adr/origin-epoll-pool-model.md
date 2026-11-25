---
status: proposed
branch: CJG
decided: 2026-11-26
---

# The Origin gets an epoll-pool Concurrency model

| Item | Content |
|---|---|
| Context | With back pressure, the Reverse Proxy reads the Origin only as fast as the client reads. So the Origin connection stays open for the whole download of a slow client. In `pool`, that connection holds a Worker for the whole download. With 2 Workers and 10 slow readers, one fast request got no reply in 15 s (2026-11-26). In `epoll`, a slow client holds no thread, but the Handler time of each request blocks the only loop. The demo runs 10k connections. |
| Decision | `server --model epoll-pool`: one epoll loop reads the requests and sends the replies. `--workers` threads make the Response plans (the Shared core and the Handler time). A Worker gives the connection back to the loop through an eventfd. `--work MS` burns CPU time before each answer in every model, as the Handler time of a real Origin. |
| Reasons | "동시성 모델을 epoll + pool(nginx나 enboy, cloudflare 등이 채택하는 구조)로. .. 사실. 어쩌면 10k로 데모 돌리기와 back pressure의 부담을 줄이기 위해 이런 동시성 모델을 선택한다고도 말할 수 있겠지." and "Origin에서 epoll + pool 구조를 적용해야지. 그게 맞지 그럼." (user said 2026-11-26). |
| Rejected options | `epoll-pool` only in `rproxy`: `rproxy` already has one Event loop for each core, and back pressure moves the cost to the Origin, not to `rproxy`. A Worker that also sends: then a slow client holds the Worker again, as in `pool`. |
| Consequences | The server has five Concurrency models. The same check (2 Workers, `--work 20`, 10 slow readers) gave the fast request 200 in 0.021 s. The Workers of `epoll-pool` do not touch a socket, so the Shared core rule stays. `eventfd` is Linux only, as `epoll` is. |
