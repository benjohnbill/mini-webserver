<!-- readme: archetype=team -->
# mini-webserver

외부 HTTP 라이브러리 없이 POSIX 소켓 API만으로 C 웹 서버를 만들고, Forward Proxy와 Reverse Proxy를 함께 구현합니다.

Status: active

## 실행

Linux(또는 WSL2, Docker)에서 빌드합니다. Windows 호스트에서는 `scripts/dev.sh`가 Ubuntu 컨테이너 안에서 명령을 실행합니다.

```sh
make                      # ./webserver 빌드
make test                 # 단위 테스트 (파서, MIME, 라우팅, 프록시)
make asan                 # AddressSanitizer + LeakSanitizer 빌드로 테스트
bash scripts/functional_test.sh   # curl / nc / ab 기능·부하 테스트
# Windows: bash scripts/dev.sh make test
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

`scripts/lb.sh`가 프론트 `:8080`(`/echo@ketama`)과 백엔드 b1..b3(`:8001..:8003`)을 띄웁니다. pid 파일과 로그는 `.lb/`에 둡니다.

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
- `tests/`: 단위 테스트, `scripts/functional_test.sh`: 기능·엣지 케이스·부하 테스트, `scripts/lb.sh`: LB 데모 CLI
- `www/`, `cgi-bin/`: 데모 정적 파일과 CGI 스크립트
- `tools/Dockerfile`, `scripts/dev.sh`: 빌드·테스트용 컨테이너

## 다음에 읽을 것

- 과제 명세: Krafton Jungle LMS `learning/1309` (웹 서버 만들기)

Agents: if a path named here does not exist, report it to the owner.

## 작업 방식

- 각 팀원은 자기 이름의 개인 브랜치에서 구현합니다.
- `main`에는 직접 push하지 않습니다. PR을 열고 merge commit으로 합칩니다.
