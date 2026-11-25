<!-- readme: archetype=team -->
# mini-webserver

외부 HTTP 라이브러리 없이 POSIX 소켓 API만으로 C 웹 서버를 만들고, Forward Proxy와 Reverse Proxy를 함께 구현합니다.

Status: active

## 처음 온 개발자용 요약

| 항목 | 내용 | 근거 |
|---|---|---|
| 목적 | POSIX 소켓만으로 만든 멀티스레드 C 웹 서버입니다. 정적 파일, CGI, Forward/Reverse Proxy, 로드 밸런싱(Round-Robin, Ketama), 헬스 체크, back-pressure를 구현합니다. | 이 문서 |
| 설치 | Linux: `build-essential curl apache2-utils python3 netcat-openbsd`. Windows: Docker Desktop과 **Git Bash**. 이미지(`mini-webserver-tools`)는 `scripts/dev.sh`가 처음 실행될 때 자동으로 만듭니다. | `tools/Dockerfile`, `scripts/dev.sh` |
| 빌드 | `make`를 실행하면 `./webserver`가 생깁니다. 중간 파일은 `build/`에 쌓입니다. | `Makefile` |
| 로컬 실행 | 아래 "실행" 절의 백엔드 2대 + front 예시를 봅니다. LB 데모 전체를 띄우려면 `bash scripts/lb.sh up`을 씁니다(아래 "LB 데모"). | `scripts/lb.sh` |
| 테스트 | `make test`(단위), `make asan`(ASan/UBSan), `bash scripts/functional_test.sh`(e2e·부하). Windows에서는 각 명령 앞에 `bash scripts/dev.sh`를 붙입니다. | `Makefile`, `scripts/functional_test.sh` |
| 배포 | 정해진 배포 절차가 없습니다. 로컬과 컨테이너에서만 실행합니다. | 저장소에 배포 설정 없음 |
| 환경 변수 | 아래 "환경 변수" 절을 봅니다. | `scripts/lb.sh`, `Makefile` |
| 먼저 볼 파일 | `src/main.c` → `src/server.c`(요청 분배) → `src/route.c`(로드 밸런싱) → `src/proxy.c` → `src/health.c`. 데모는 `scripts/lb.sh`에서 시작합니다. 전체 목록은 아래 "Map" 절에 있습니다. | 이 문서 |

자주 막히는 곳:
- **Windows PowerShell의 `bash`는 WSL입니다.** 기본 배포판이 `docker-desktop`이면 `herdr-app`이나 `pwd -W`를 찾지 못해 스크립트가 실패합니다. Git Bash를 직접 실행합니다.
  ```powershell
  & "C:\Program Files\Git\bin\bash.exe" scripts/dev.sh make test
  ```
- **`make asan` 뒤의 `./webserver`는 ASan 빌드입니다.** `make`는 이 바이너리를 최신으로 보고 다시 빌드하지 않습니다. 일반 빌드가 필요하면 `make clean all`을 실행합니다.
- **다른 환경(예: devcontainer)에서 빌드한 `./webserver`는 glibc 버전이 달라** `GLIBC_2.38 not found`로 실행되지 않을 수 있습니다. 이때도 `make clean all`을 실행합니다.
- **포트가 겹칩니다.** `functional_test.sh`와 `lb.sh`는 둘 다 `:8080`을 씁니다. 같은 컨테이너에서 실행하려면 먼저 `scripts/lb.sh down`을 실행합니다. `scripts/dev.sh`는 실행할 때마다 새 컨테이너를 쓰므로 이 문제가 없습니다.
- **Windows 호스트에서는 서버에 접속할 수 없습니다.** 서버는 컨테이너 안에서 돌고, `/_lb/status`는 localhost에서 온 요청에만 응답합니다. 그래서 `curl`, `loadgen.py`, `mini-web`은 컨테이너 안에서 실행합니다.

## 환경 변수

| 변수 | 기본값 | 쓰는 곳 | 의미 |
|---|---|---|---|
| `LB_BACKENDS` | 3 | `lb.sh up` | 처음 띄울 백엔드 수(1..8). 멤버 목록(`.lb/backends`)이 없을 때만 씁니다. |
| `LB_WEIGHTS` | 모두 1 | `lb.sh up` | 백엔드별 가중치(예: `1,3,1`, 각 1..4) |
| `LB_CAP` | 2 | `lb.sh` front | 백엔드당 동시 프록시 요청 상한(`-B`, 0 = 끔) |
| `LB_WAIT` | 2000 | `lb.sh` front | 대기 큐 503 기준(ms, `-W`, 0 = 끔) |
| `LB_TIMEOUT` | 1000 | `lb.sh` front | 프록시 타임아웃(ms, `-T`). `bp_demo.sh`는 3000으로 실행합니다. |
| `LB_THREADS` | 8 | `lb.sh` front | 워커 스레드 수(`-t`) |
| `LB_STRATEGY` | `ketama` | `lb.sh` front | `ketama` 또는 `rr` |
| `LB_TEST_IN_COPY` | (없음) | `lb_test.sh` | 1이어야 실행됩니다. 저장소 복사본에서만 돌리기 위한 안전장치입니다. |
| `CC`, `CFLAGS` | `cc`, `-std=c11 -O2 -g ...` | `Makefile` | 컴파일러와 플래그 |
| `MSYS_NO_PATHCONV` | (없음) | Git Bash | 1이면 Git Bash가 `/src` 같은 인자를 Windows 경로로 바꾸지 않습니다. `docker` 명령을 직접 실행할 때 붙입니다. |

비밀 값(토큰, 비밀번호)은 쓰지 않습니다.

## 실행

Linux(또는 WSL2, Docker)에서 빌드합니다. Windows 호스트에서는 `scripts/dev.sh`가 Ubuntu 컨테이너 안에서 명령을 실행합니다. 이 컨테이너는 명령이 끝나면 지워지므로, 서버를 띄워 두고 직접 접속해 보려면 `scripts/herdr_lab.sh`(herdr 앱 필요)나 `docker run -it`으로 셸을 엽니다.

```sh
make                      # ./webserver 빌드
make test                 # 단위 테스트 (tests/test_*.c: config, health, http, mime, proxy, reload, route)
make asan                 # AddressSanitizer + UndefinedBehaviorSanitizer 빌드로 다시 빌드하고 테스트
bash scripts/functional_test.sh   # curl / nc / ab 기능·부하 테스트, reload 검사, 시뮬레이터 검증
# Windows(Git Bash): bash scripts/dev.sh make test
```

```sh
# 백엔드 두 대
./webserver -p 8001 -n b1 &
./webserver -p 8002 -n b2 &
# 프론트: 정적 파일 + CGI + /echo Round-Robin Reverse Proxy + Forward Proxy
./webserver -p 8080 -r www -c cgi-bin -F -R '/echo=127.0.0.1:8001,127.0.0.1:8002'
curl -i localhost:8080/echo                                  # X-Server-Name: b1, b2 교대
curl -x http://127.0.0.1:8080 http://127.0.0.1:8001/          # Forward Proxy
```

옵션: `-p` 포트, `-r` 정적 루트, `-c` CGI 디렉터리, `-t` 워커 스레드 수, `-q` 대기 큐 크기, `-F` Forward Proxy 허용, `-T` 프록시 타임아웃(ms), `-B` 백엔드당 동시 프록시 요청 상한(기본 max(1, 스레드/2), 0..1024, 0 = 상한 없음; 상한에 걸리면 다음 후보; 연결된 후보가 없고 하나라도 상한 때문에 건너뛰었으면 503), `-W` 대기 큐에서 이 시간(ms)보다 오래 기다린 클라이언트는 503(기본 2000, 0..600000, 0 = 끔; `-B 0 -W 0` = 백엔드별 상한과 오래 기다린 요청 503만 끔, 대기 큐가 가득 찼을 때의 503(`-q`)은 그대로), `-k` keep-alive 타임아웃(ms), `-n` 서버 이름(`X-Server-Name`), `-R` 라우팅 규칙(반복 가능, `접두사[@ketama]=host:port[*가중치],...`; `@ketama`가 없으면 Round-Robin, `*N`은 가중치 1..4; `=` 뒤를 `@파일`로 쓰면 목록을 파일에서 읽고 `SIGHUP`에 다시 읽음, 예: `-R '/echo@ketama=@.lb/front.route'`), `-h` 도움말.

## 구조

- 동시성: accept 스레드 하나와 pthread 스레드 풀, 크기가 정해진 연결 큐. 큐가 가득 차면 503을 응답합니다.
- 요청 처리 순서: 절대 URI는 Forward Proxy, 라우팅 규칙과 맞는 경로는 Reverse Proxy, `/cgi-bin/`은 CGI, `/echo`는 내장 동적 응답, 그 밖은 정적 파일입니다.
- 로드 밸런싱: 경로마다 Round-Robin 또는 Ketama 일관 해싱(키는 클라이언트 IP)을 고릅니다. 가중치는 두 방식 모두에 적용됩니다.
- 헬스 체크: 연결 실패만 다음 백엔드로 재시도합니다(이미 요청을 보낸 뒤의 오류는 재시도하지 않음). 연속 3번 실패하면 DOWN, 1초마다 능동 검사해 연속 2번 성공하면 다시 UP이 됩니다.
- `GET /_lb/status`: 백엔드별 상태 표(text/plain). localhost에서 온 요청만 응답하고, 그 밖은 404입니다.
- 종료: `SIGINT`/`SIGTERM`을 받으면 accept를 멈추고 워커를 join합니다. `SIGPIPE`는 무시합니다.
- 백엔드 목록 다시 읽기: `SIGHUP`을 받으면 `=@파일` route의 파일을 다시 읽어 route 표를 무중단으로 교체합니다. 처리 중인 요청은 옛 표로 끝나고, host:port가 같은 백엔드는 헬스 상태와 카운터를 유지합니다. 파일은 백엔드 토큰을 쉼표·공백·줄바꿈으로 구분하고 `#`부터 줄 끝은 주석입니다. 파일이 잘못됐거나(파싱 실패, 0개, 8개 초과, 중복, 64KiB 초과) 읽을 수 없으면 기존 목록을 유지합니다. stderr에는 `reload: ok` 또는 `reload: failed: <이유>`가 남습니다.

## LB 데모

`scripts/lb.sh`가 프론트 `:8080`(`/echo@ketama`)과 백엔드(기본 b1..b3, `:8001..:8003`; bN은 `:8000+N`)를 띄웁니다. pid 파일과 로그는 `.lb/`에 둡니다. 아래 예시의 "moved 약 1/3"이나 라우팅 순서는 기본값(가중치 1인 백엔드 3개) 기준입니다.

```sh
bash scripts/lb.sh up        # 전부 시작
bash scripts/lb.sh keys      # 출발지 IP 20개(127.0.0.2..)가 어느 백엔드로 가는지
bash scripts/lb.sh stop b2   # b2를 끄면
bash scripts/lb.sh keys      # b2의 키만 옮겨짐: "b2 → b3  (moved)", moved 약 1/3
bash scripts/lb.sh status    # b2 DOWN
bash scripts/lb.sh start b2  # 몇 초 뒤 status에서 UP, keys를 다시 돌리면 b2로 돌아옴
bash scripts/lb.sh down      # 전부 종료
```

실행 중에 백엔드를 늘리고 줄일 수 있습니다. 멤버 목록은 `.lb/backends`에, front가 읽는 주소 목록은 `.lb/front.route`에 저장됩니다. `add`/`remove`는 이 파일을 원자적으로 바꾼 뒤 front에 `SIGHUP`을 보내고 `/_lb/status`에 반영될 때까지(최대 약 2초) 기다립니다. front는 재시작하지 않으므로 처리 중인 요청은 끊기지 않고, 남은 백엔드의 상태와 카운터도 유지됩니다. `remove`는 먼저 목록에서 빼고 나서 백엔드를 끕니다. 파일 route를 모르는 예전 방식으로 뜬 front(옛 바이너리)라면 `SIGHUP` 대신 예전처럼 재시작합니다. 처음 `up`의 개수는 `LB_BACKENDS`(기본 3)로 정합니다. front는 route당 백엔드를 최대 8개(`ROUTE_MAX_BACKENDS`)까지 받으므로 9번째 `add`는 거부됩니다.

```sh
bash scripts/lb.sh add         # 가장 작은 빈 bN(:8000+N)에 백엔드를 띄우고 front에 추가
bash scripts/lb.sh keys        # 일부 키만 새 백엔드로 옮겨짐
bash scripts/lb.sh remove b4   # b4를 front 목록에서 빼고 끔
```

`scripts/herdr_lab.sh`로 띄운 lab 컨테이너에서는 `mini-web backend new`, `mini-web backend rm b4`로 같은 일을 합니다.

### 가중치

백엔드마다 가중치 1..4(front의 `*N`)를 줄 수 있습니다. 1이 아닌 값은 `.lb/weights`(`bN W` 줄)에 저장되고, route 파일에는 `127.0.0.1:8002*3`처럼 들어갑니다. `up`/`add`/`remove`/reload 뒤에도 유지되고, `/_lb/status`의 WEIGHT/SHARE 열과 `keys` 끝의 요약(`b2  weight 3  keys K (P%)`)에 비례가 보입니다. 범위를 벗어난 값은 거부되며, 그때는 파일도 front도 바뀌지 않습니다.

```sh
LB_WEIGHTS=1,3,1 bash scripts/lb.sh up   # 백엔드 3개(개수 = 가중치 개수), b2만 3; LB_BACKENDS와 개수가 다르면 거부
bash scripts/lb.sh add --weight 2         # mini-web backend new --weight 2
bash scripts/lb.sh weight b2 1            # 실행 중 변경(SIGHUP reload, front 재시작 없음)
bash scripts/lb.sh keys 60                # 키 분포 + 백엔드별 weight/keys 요약
```

`LB_BACKENDS`/`LB_WEIGHTS`는 멤버 목록(`.lb/backends`)이 없을 때(처음 `up`이나 `down` 뒤)만 씁니다. 목록이 있으면 front가 멈춰 있어도(lab 컨테이너 재시작 등) `up`은 기존 멤버와 가중치를 그대로 띄우고, 두 변수가 주어졌으면 `keeping members from .lb/backends (lb.sh down to reset)`를 stderr에 남깁니다. 처음부터 다시 정하려면 `lb.sh down` 뒤 `up`을 실행합니다.

### 지속 클라이언트

`tools/clientd.py` 데몬 하나가 클라이언트 N개를 스레드로 돌립니다. 클라이언트 i는 출발지 IP `127.0.0.(i+2)`로 `/echo`를 보내므로 ketama 키가 서로 달라 백엔드에 퍼집니다. 각 클라이언트는 한 번에 요청 하나만 보내고(`Connection: close`), 초당 R번(기본 2) 보냅니다. 수를 줄이면 번호가 큰 클라이언트부터 멈추므로 남은 클라이언트의 키는 그대로입니다. 상태는 `.lb/clients`, 통계는 `.lb/clients.stats`(1초마다 갱신), 로그는 `.lb/clients.log`에 있습니다. `down`도 클라이언트를 정리합니다.

```sh
mini-web client new 20            # = lb.sh client new 20; "clients: 20 (rps 2)"
mini-web client new 0 --rps 5     # 수는 그대로 두고 rps만 변경
mini-web client rm 5              # 번호가 큰 5개 정지
mini-web client ls                # 누적/최근 1초 응답 코드, 백엔드별 고정 클라이언트 수와 요청 수
mini-web client stop
```

제한:
- 최대 250개(출발지 IP 127.0.0.2..251)이고, lab 컨테이너처럼 127.0.0.x로 bind할 수 있는 Linux에서만 씁니다(호스트 Windows에서는 안 됨).
- 동시 요청은 많아야 클라이언트 수(≤ 250)이므로 기본 큐(256)에 들어갑니다. 다만 백엔드가 멈추거나 느리면 요청이 큐에 쌓여 `-W`로 503이 나고, `/_lb/status`도 같은 워커를 쓰므로 대시보드 갱신이 늦어질 수 있습니다.
- 수 × rps가 200을 넘으면 경고를 출력하고, 실행은 그대로 합니다.

### 분포 비교 (오프라인 시뮬레이터)

`tools/lbsim.py`(`mini-web sim ...`)는 같은 키 집합에서 여러 방식의 서버별 몫, 서버 하나를 빼거나 더할 때 이동한 키의 비율, 빠진 서버의 키가 어디로 갔는지를 텍스트 막대와 표로 보여 줍니다. 한 서버가 그 키의 80% 이상을 받으면 `<- concentrated`가 붙습니다.

| 방식 | 내용 |
|---|---|
| `mod` | `hash % 슬롯 수`(가중치만큼 슬롯을 펼침). 서버 수가 바뀌면 키 대부분이 이동합니다. |
| `rr` | 가중치를 반영한 원형 리스트(라운드 로빈). 키 고정이 없어 키 단위 이동이 정의되지 않습니다. |
| `random` | 키마다 한 번 가중치 비례 확률로 서버를 정해 두는 배치(`--seed N`, 기본 1). |
| `ring1`, `ring10` | 서버당 점 1개 또는 10개 × weight인 ring. 점이 적으면 몫이 불균형합니다. 빠진 서버의 arc는 저마다 ring에서 다음 생존 점의 서버로 가므로 목적지가 여러 서버로 나뉠 수 있습니다. 전부 한 서버로 가는 것이 보장되는 경우는 빠진 서버의 arc가 하나일 때(ring1, 가중치 1)와 남는 서버가 하나일 때뿐입니다. |
| `ketama` | 현재 front와 같은 구현(같은 해시, 서버당 160 × weight 점). |
| `hrw` | 가중 rendezvous 해싱. |

키를 254개만 세면 표본 오차가 커서, 보이는 숫자가 방식의 성질인지 우연인지 알기 어렵습니다. 그래서 실측 옆에 **수식 기대값(E)과 95% 범위**를 항상 같이 보여 줍니다.
- 가정: 해시는 균등하고 서로 독립입니다.
- 범위는 두 종류입니다.
  - (a) 배치 편차만: ring의 arc 몫.
  - (b) 배치 편차 + 키 K개의 표본 오차: keys 몫과 moved. 정확한 BetaBinomial/Binomial 분포의 2.5%·97.5% 분위수를 씁니다.
- 실측이 범위 밖이면 `!`, 근사식이면 `≈`가 붙습니다.
- 맨 아래 `summary (theory)` 표는 방식마다 한 줄로 이론 기준 차이를 요약합니다.

기호: 가중치 w_i, W = Σw, 제거하는 서버 r, p_i = w_i/W, 남은 서버의 재분배 비율 q_j = w_j/(W − w_r).

| 방식 | 몫 | 제거 시 이동(moved) | 재분배·쏠림 |
|---|---|---|---|
| `rr` | 정확히 p_i, 편차 0 | 정의 안 됨(키 고정 없음) | 정확히 q_j. 쏠림은 max q_j ≥ 0.8이면 1 |
| `random` | p_i, keys sd √(p(1−p)/K) | p_r, sd √(p_r(1−p_r)/K) | 키마다 q_j. r의 키 수 n은 Binomial(K, p_r)이므로 P(쏠림) = Σ_{n≥1} Bin(n; K, p_r)·Σ_j P(Bin(n, q_j) ≥ ⌈0.8n⌉)을 n ≥ 1의 질량으로 나눈 값(r에 키가 있을 때의 조건부 확률, 정확). 요청마다 새로 뽑으면 같은 키의 두 요청이 다른 서버로 갈 확률은 1 − Σp_i² |
| `mod` | p_i, keys sd √(p(1−p)/K) | **정확**: 0..lcm(S, S′)−1의 나머지를 전부 세어 owner(x mod S) ≠ owner′(x mod S′)인 비율 | 같은 전수 계산으로 정확 |
| `ring1`/`ring10`/`ketama` (점 m_i = P·w_i, M = P·W) | arc 몫 ~ Beta(m_i, M−m_i): 기대 p_i, 분산 m(M−m)/(M²(M+1)), 95%는 Beta 분위수. keys 개수는 정확히 BetaBinomial(K, m_i, M−m_i)(분산 = 배치 분산 + (p(1−p) − 배치 분산)/K), 95%는 그 분위수 | 기대 p_r | 기대는 정확히 q_j(점 주인의 순서가 무작위라 r의 각 arc는 다음 점의 주인 j에게 확률 m_j/(M−m_r)로 감). 쏠림: r의 arc 중 j에게 가는 개수 c_j ~ BetaBinomial(m_r, n_j, N−n_j)(N = M−m_r, n_j = P·w_j; 연속된 r 점은 함께 다음 서버로 감), arc 크기는 Dirichlet라 j의 몫 F_j ~ Beta(c_j, m_r−c_j)(양 끝: c_j = 0이면 F_j = 0, c_j = m_r이면 F_j = 1). P(쏠림) = Σ_j P(F_j ≥ 0.8)로 정확. 이 식을 모든 m_r에 씁니다. r의 각 arc는 다음 생존 점의 서버로 따로 가므로 목적지가 나뉠 수 있고, **m_r = 1이거나 남는 서버가 하나일 때만 정확히 1**(arc가 하나이거나 받을 서버가 하나뿐이라 전부 한 서버로 감) |
| `hrw` | 정확히 p_i(지수 경주) | p_r(r의 키만 움직임) | 키마다 q_j. 쏠림은 `random`과 같은 식 |

"가장 많이 떠안는 서버의 몫"(top)은 경우에 따라 정확하기도 하고 근사(`≈`)이기도 합니다.
- 정확: `mod`(전수 계산한 재분배의 최댓값), `rr`(max q_j), 남는 서버가 하나이거나 m_r = 1인 ring(100%).
- 중점 적분(`≈`), ring(모든 m_r): top = ∫₀¹ P(max F ≥ t) dt를 100개 중점으로 계산합니다. t ≥ 0.5 구간은 Σ_j P(F_j ≥ t)로 정확하고, t < 0.5 구간은 min(1, Σ_j P(F_j ≥ t))라는 상한을 씁니다.
- 독립 이항 근사(`≈`), `hrw`/`random`: n개 키에서 X_j ~ Bin(n, q_j)를 서로 독립으로 보고 E[max_j X_j]/n을 구한 뒤, n ~ Binomial(K, p_r)(n ≥ 1)에 대해 평균합니다.
  - 이 근사는 실제보다 **낮게** 나오고, r의 키 수 n(≈ K·p_r)이 작을수록 차이가 커집니다. 실제로는 X_j의 합이 n으로 묶여 있어 서로 독립이 아니기 때문입니다. 예: 가중치 1×8에서 b1 제거는 K = 254일 때 근사 0.23 대 MC 0.24, `--keys 10`일 때 근사 0.57 대 MC 0.77입니다.
  - 남는 서버가 하나면 top = P(쏠림) = 100%로 정확합니다(`≈` 없음).
  - `--keys`가 매우 크면(10⁶ 이상) hrw/random의 이론 계산(n에 대한 합)이 수 초에서 수십 초 걸립니다(가중치 1,3,1, `--keys 1000000`에서 약 60초).

처음에는 ring 쏠림도 "arc를 같은 크기의 독립 배정"으로 근사했습니다. 그런데 몬테카를로에서 ring1, w=3 제거의 P(쏠림)이 근사 0.088 대 실제 0.46으로 크게 틀려, 위의 정확식으로 바꿨습니다.

이 식들은 `tests/lbsim_mc.py`가 lbsim의 식을 쓰지 않는 독립 몬테카를로로 검증합니다(고정 시드, 단독 약 20초, `functional_test.sh` 안에서는 서버와 함께 돌아 약 27초). 확인하는 항목은 다음과 같습니다.
- 무작위 ring 배치 수천 개의 arc 몫 평균, sd, 멤버별 arc 95%·keys 95% 적중률.
- 해시로 만든 ring의 arc 몫, 그리고 키 254개 keys 몫의 sd.
- 작은 K(7, 20)에서 keys·moved 95% 범위가 따로 계산한 정확한 Binomial/BetaBinomial 분위수와 완전히 같은지.
- 제거된 arc가 실제로 어디로 가는지에서 나오는 재분배, 쏠림 확률, top.
- 해시 키 20만 개로 본 mod, hrw, random의 이동과 재분배.

```sh
mini-web sim --backends 5 --remove b3               # 기본 키: 127.0.0.2..255(lb.sh keys와 같은 범위)
mini-web sim --backends 3 --weights 1,3,1 --add     # b4를 가중치 1로 추가
mini-web sim --backends 4 --keys 10000 --remove b2 --methods ring1,ketama
```

`ketama` 행이 실제 front와 같은지는 `functional_test.sh`가 검사합니다(`/_lb/status`의 SHARE = `--share`, 실제 응답 백엔드 = `--map`). jump hash는 임의의 서버 하나를 뺄 수 없어(마지막 버킷만 뺄 수 있음) 넣지 않았습니다. 대체 알고리즘을 front(C)에 넣지 않은 이유는 비교가 오프라인으로 충분하고, ketama 행은 실제 front와의 일치 검사로 확인되기 때문입니다.

### bp_demo.sh 옵션

`--backends N`(1..8, 기본 3), `--target bN`(장애를 걸 대상, 기본 b2), `--weights 1,3,1`(N개)을 받습니다. 인자 없이 실행하면 예전과 같습니다(b1..b3, 대상 b2, 가중치 모두 1). 결과 표 위에 `backends N  weights ...  target bN` 줄이 나옵니다.

```sh
bash scripts/bp_demo.sh --backends 5 --target b4 --weights 1,2,1,3,1 --scenarios hung --caps 0,2
```

`scripts/lb_test.sh`는 lb.sh/mini-web/bp_demo 회귀 검사입니다. `.lb/`와 :8001..:8080을 쓰므로 저장소 복사본 컨테이너에서만 실행합니다(`LB_TEST_IN_COPY=1`이 없으면 거부, 명령은 파일 머리말 참고).

`bp_demo.sh`도 실행할 때마다 `lb.sh down`/`up`을 반복하므로, 같은 컨테이너에서 돌고 있던 LB 데모는 꺼집니다.

### back-pressure 수동 데모

백엔드당 프록시 요청을 2개로 제한한 상태에서 b2를 일시 정지합니다. 부하가 계속되는 동안 b2는 `UP`, `2/2 FULL`로 표시되고 `SHED`가 증가합니다. `GET /`은 `200`을 반환하며, b2의 슬롯이 가득 차면 해당 키의 요청은 다음 백엔드로 넘어갑니다.

```sh
bash scripts/lb.sh up
bash scripts/lb.sh keys
bash scripts/lb.sh pause b2
ab -t 15 -c 20 http://127.0.0.1:8080/echo &
load=$!
sleep 1
bash scripts/lb.sh status
curl -i http://127.0.0.1:8080/
bash scripts/lb.sh keys
bash scripts/lb.sh resume b2
wait "$load"
bash scripts/lb.sh keys
bash scripts/lb.sh down
```

ApacheBench의 출발지 `127.0.0.1`은 b3 → b2 → b1 순서로 라우팅됩니다. 부하 중에는 b1과 b3도 `FULL`로 표시될 수 있고, 모든 후보가 가득 찬 요청은 `503`을 받습니다. `keys`가 `ERR`를 표시하면 해당 키의 마지막 성공 기록을 보존합니다. 재개 후 부하가 끝나면 우회했던 키가 b2로 돌아옵니다.

## Map (파일 지도)

- `README.md` (entry): 이 문서
- `include/`: 모듈 간 계약 헤더 (`http.h`, `route.h`, `proxy.h`, `server.h`, `cgi.h`, `server_run.h`)
- `src/http.c`: 요청 파싱(요청 라인, 헤더, Content-Length 본문, keep-alive, 파이프라이닝)과 응답 생성
- `src/server.c`, `src/main.c`: 소켓, 스레드 풀, 요청 분배, 정적 파일, 시그널 처리
- `src/cgi.c`: CGI/1.1 실행 (fork/execve, 타임아웃, 프로세스 그룹 정리)
- `src/route.c`: 최장 접두사 라우팅, Round-Robin과 Ketama 선택, 백엔드 health 상태
- `src/proxy.c`: Reverse/Forward Proxy, hop-by-hop 헤더 제거, X-Forwarded-For, 502/504
- `src/health.c`: 백엔드 능동 헬스 체크 스레드
- `src/config.c`, `src/mime.c`: CLI 인자 처리, MIME 타입 매핑
- `tests/`: 단위 테스트, `scripts/functional_test.sh`: 기능·엣지 케이스·부하 테스트, `scripts/lb.sh`: LB 데모 CLI, `scripts/lb_test.sh`: lb.sh 회귀 검사(복사본 전용)
- `tools/lbsim.py`: 분포 비교 시뮬레이터, `tools/clientd.py`: 지속 클라이언트 데몬, `tools/loadgen.py`: 일회성 부하
- `www/`, `cgi-bin/`: 데모 정적 파일과 CGI 스크립트
- `tools/Dockerfile`, `scripts/dev.sh`: 빌드·테스트용 컨테이너

## 다음에 읽을 것

- 과제 명세: Krafton Jungle LMS `learning/1309` (웹 서버 만들기)

Agents: if a path named here does not exist, report it to the owner.

## 작업 방식

- 각 팀원은 자기 이름의 개인 브랜치에서 구현합니다.
- `main`에는 직접 push하지 않습니다. PR을 열고 merge commit으로 합칩니다.
