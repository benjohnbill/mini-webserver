# impl-tools 완료 보고

## 바뀐 파일
- `scripts/lb.sh`
  - 0단계 버그 수정: `LB_BACKENDS=9|0|x`를 넣으면 에러 문구가 BACKENDS 값이 돼 이상한 백엔드가 뜨던 문제(`$(defaults)`의 exit는 서브셸만 끝냄) → 문구는 stderr로, 두 호출부에 `|| exit 1` (verify0.md).
  - front: `-R /echo$(route_sep)@.lb/front.route`. `.lb/front.route` = 멤버마다 `127.0.0.1:<port>` 한 줄. `save_members`와 `start front`가 tmp + `mv -f`로 원자 교체.
  - `reload()`: front가 없으면 파일만 갱신. `/proc/<pid>/cmdline`에 `=@`가 없으면 HUP 대신 기존 `refront()`(재시작). 있으면 `kill -HUP` 뒤 `/_lb/status`의 주소 열이 기대 목록과 같아질 때까지 0.1초 × 20회 대기. 성공하면 `front reloaded (pid N)`을 출력하고 `.lb/front.opts`의 `backends N`만 갱신(다른 옵션은 실행 중 front 기준으로 유지).
  - `add`: 백엔드 시작 → 파일 → reload. `remove`: 파일 → reload → **그다음** stop(빠진 백엔드에 새 요청이 안 가게). reload가 실패해도 stop은 하고 rc=1.
  - `down`이 `.lb/front.route`도 지움. usage/헤더 설명을 reload 기준으로 수정. 출력 형태는 그대로(`front started …` 줄이 `front reloaded (pid N)`으로 바뀐 것만 다름). `mini-web`은 수정 불필요.
- `tests/test_route.c`: `backends[i]->`, `weight[i]`, `pick1`은 포인터 비교로 인덱스 찾기, 각 테스트 끝에 `route_table_release`.
- `tests/test_health.c`: `route_live_init` / `route_live_destroy`, `backends[1]` 포인터.
- `tests/test_config.c`: 성공한 parse 뒤 cfg 재사용 전 `route_live_destroy`(impl-core 안내대로).
- `tests/test_reload.c` (새 파일, 449 checks):
  파일 파싱(주석, `\r\n`, 탭, 가중치, v6, realpath 절대 경로, 상대 경로, 인라인 route는 file 빈 문자열) /
  시작 시 잘못된 파일(0개, 주석만, 9개, 중복, 잘못된 토큰, `*5`, `:0`, 없는 파일)이면 -1이고 표 그대로 /
  reload: 같은 host:port는 같은 포인터이고 down·changed_ms·req_ok/err·shed·inflight 유지, 가중치 변경 반영, 제거, 새 백엔드는 새 상태, 링 재구성, 인라인 route 공유, old는 release 전까지 그대로, reload의 reload도 같은 포인터 /
  잘못된 reload(위 목록 + 64KiB 초과 + 파일 삭제)는 -1, `*out` 그대로, err 채워짐, old memcmp 동일, 파일이 돌아오면 다시 성공 /
  8스레드가 acquire→match→pick→try_acquire/release→table release를 돌리는 동안 main이 300번 reload+swap, 끝에 refs==2, inflight 0.
- `scripts/functional_test.sh`: `== backend list reload (SIGHUP)` 절 추가. 새 포트는 front4 `:8083`(`-B 0`, `/echo=@$LOG/reload.route`)과 r1..r3 `:8004..:8006`. 검사 20개:
  시작 목록 / 두 백엔드 응답 / 추가 후 1초 안에 status 반영 / `reload: ok` 로그 / 기존 백엔드 OK 카운터 유지 / 추가한 백엔드가 트래픽 받음 / 제거 후 1초 안에 빠짐 / 제거한 백엔드로 트래픽 안 감 / front 생존 /
  ab 6000 요청(c=20) 중 reload 약 46회에도 Failed 0, Non-2xx 0, 6000개 완료 /
  잘못된 파일 3종(토큰, 빈 파일, 중복)마다 `reload: failed:`가 1줄씩 늘고 목록 유지 / 파일 삭제 시 목록 유지 / OK 카운터 유지 / 계속 응답.
  참고: 처음에는 `-B` 기본값(cap 4)으로 돌렸더니 부하 중 471개가 503이었습니다. 목록이 한 대(`8005`)뿐인 순간에 c=20이 cap에 걸린 것이고, `-B 0`으로 바꾸자 0이 됐습니다. reload 버그가 아니라 back-pressure 정상 동작입니다.
- `README.md`: `-R`의 `=@파일` 문법, SIGHUP reload 동작(파일 문법, 실패 시 유지, 로그 접두사), LB 데모 절(`.lb/front.route`, HUP, 무중단, remove 순서, 옛 front는 재시작).

## 실행한 테스트 (마지막 실행, 현재 작업 트리 src 포함)
- `bash scripts/dev.sh make test`(clean 후): 7개 파일 모두 0 failures (config 15, health 17, http 118, mime 26, proxy 64, reload 449, route 41723). **컴파일 경고 0.**
- `bash scripts/dev.sh make asan`: rc=0, 같은 7개 0 failures, `warning`/`Sanitizer` 0건.
- `bash scripts/dev.sh bash scripts/functional_test.sh`(clean 빌드): **passed 74, failed 0** (`ab 6000 req with 46 reloads, 0 failed`).
- lb.sh는 복사본 컨테이너에서 실행:
  - `up` → front cmdline `-R /echo@ketama=@.lb/front.route`.
  - curl 부하(출발지 IP 50개) 중 add, add, remove b2 → front pid 155 그대로, `load: 129 requests, 0 non-200`, `reload: ok` 3줄, opts `backends 4`.
  - 8개까지 채우고 9번째 add는 거부, pid 그대로. `remove b42` → rc=1.
  - 옛 방식(인라인 `-R`)으로 띄운 front에 remove b3 → HUP 대신 재시작(pid 903 → 990). 새 front는 `=@` 방식.
  - front가 꺼진 상태에서 add → 파일만 갱신, 이후 `start front` 시 8행.
  - `down` → pid·route 파일 정리.
  - `bp_demo.sh --scenarios normal,hung --caps 2 --duration 2 --clients 10` 정상. hung의 504:2는 멈춘 b2에 이미 들어가 있던 요청의 기존 동작.
- TSan: 컨테이너에서 `unexpected memory mapping`, `setarch -R`도 권한이 없어 **실행 못 함**. 레이스 검사는 ASan과 위 스레드 테스트로만 했습니다.

## 남은 것 / 위험
- src 쪽 컴파일 에러·경고 없음. C 코어 리뷰(교차 검증)는 아직 안 했습니다. 요청하면 하겠습니다.
- mini-lab의 front는 옛 바이너리와 인라인 `-R`로 돌고 있습니다. 새 lb.sh로 처음 add/remove하면 안전장치 때문에 한 번은 재시작되고(그때 새 바이너리 `=@` 방식으로 전환), 그 뒤부터는 HUP입니다. 단, add/remove는 `make -s all`을 하므로 lab의 바이너리도 그때 새로 빌드됩니다.
- `dev.sh`로 돌린 테스트 때문에 저장소의 `build/`와 `webserver`가 다시 생성됐습니다(gitignore 대상, `.lb/`는 건드리지 않음).

## 수정 2차 (review-tools.md 7건, fix-tools.md 기준)
1. **실패 시 아무것도 바꾸지 않음** (`scripts/lb.sh`)
   - `save_members`는 route 파일을 먼저 쓰고(tmp+mv) 그다음 members를 씁니다. 실패하면 메시지와 함께 rc=1.
   - 새 `apply OLD`: 저장 → reload 순서로 진행하다 어디서든 실패하면 BACKENDS를 OLD로 되돌리고, 두 파일을 다시 쓰고, front도 다시 reload(원복)한 뒤 `member change not applied; backends stay: …`를 출력하고 rc=1.
   - `remove`는 apply가 성공했을 때만 drain → stop. `add`는 apply가 실패하면 방금 띄운 bN을 stop하고 rc=1. `up`도 save_members 실패 시 exit 1.
2. `remove`도 `backend` 검사 뒤 `make -s all || exit 1`을 먼저 합니다(refront 분기가 새 바이너리로 뜨도록).
3. `drain`: 제거 대상이 slow_backend(`.lb/bN.slow`)이고 front가 돌고 있을 때만, front cmdline의 `-T` 값(없으면 LB_TIMEOUT)만큼 기다린 뒤 stop합니다. 일반 webserver는 TERM을 받으면 처리 중 요청을 마치므로 기다리지 않습니다. tools/는 수정하지 않았습니다.
4. functional_test 부하 검사
   - OK 카운터가 늘어나는 것으로 부하가 시작됐음을 확인한 뒤부터 목록을 바꿉니다.
   - 매번 `hup_rows`(새 목록 확인) + `reload: ok` 증가 확인 + ab가 아직 살아 있음을 모두 만족해야 1회로 셉니다.
   - 새 검사 `reload took effect under load (L of N)`은 L≥1이어야 통과합니다.
5. 전체 마감 시각
   - lb.sh `reload`: 2000ms 마감, 각 curl `-m`은 남은 시간.
   - 테스트 `hup_rows`: 1000ms 마감, `rows`의 `-m`은 남은 시간.
   - 로그 대기 `wait_more`도 1초 마감.
6. 파일 삭제 테스트도 `reload: failed:` 줄 증가를 기다린 뒤 검사합니다(새 검사 `missing file logs reload: failed`).
7. 출력: `front reloaded (pid N, :8080, webserver)`.

### 재실행 결과
- `bash scripts/dev.sh make test`(clean 후): 7개 파일 모두 0 failures.
- `bash scripts/dev.sh bash scripts/functional_test.sh`: **passed 76, failed 0**
  - `reload took effect under load (19 of 20)`, `ab 6000 req with 20 reloads, 0 failed`, `0 non-2xx`, `missing file logs reload: failed`.
  - 이제 core의 실패 로그에 파일 경로가 붙습니다: `reload: failed: /tmp/…/reload.route: duplicate backend`.
- lb.sh, 복사본 컨테이너 (새 시나리오):
  - `add` 출력: `front reloaded (pid 161, :8080, webserver)`.
  - `.lb/front.route.tmp`를 디렉터리로 만들어 쓰기 실패를 냄 → remove b2 rc=1, members 그대로, b2 살아 있음, status 4행.
  - front를 SIGSTOP해 반영 확인을 실패시킴 → remove b2 rc=1, b2 살아 있음, members와 route 파일이 원래 목록. CONT 후 front 생존, status도 원래 4행.
  - `LB_TIMEOUT=1500`, `slow b2 900`에서 b2로 가는 키로 요청을 보내 놓고 remove b2 → `waiting 1500ms for requests already sent to b2` 출력, **진행 중 요청 200(0.90s)**, 그 뒤 stop, 전체 1.7s.
  - 이전 회귀 스크립트:
    - 부하 중 add/add/remove에서 pid 불변, `0 non-200`.
    - 9번째 add 거부.
    - 옛 front(인라인 -R)에서 remove 시 재시작 후 `=@` 방식(pid 939 → 1027).
    - front가 꺼져 있을 때 add는 파일만 갱신.
    - bp_demo normal/hung 정상. hung의 `503:1 504:2`는 cap 2와 멈춘 b2 때문에 생긴 back-pressure 결과로, reload와 무관합니다(bp_demo는 add/remove를 쓰지 않음).
