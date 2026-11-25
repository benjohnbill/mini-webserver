# 최종 전체 테스트 (impl-tools, 실행만, 코드 수정 없음)

전체 출력: `.orchestration/final-test.log` (명령마다 `===== [시각] 명령` … `===== rc=N`. 5번은 실행 스크립트와 출력을 함께 남김).
대상: C 코어 수정 3·4차까지 반영된 현재 작업 트리.

| # | 명령 | rc | 결과 | 경고 | Sanitizer |
|---|------|----|------|------|-----------|
| 1 | `bash scripts/dev.sh make clean all` | 0 | 빌드 성공 | 0 | - |
| 2 | `bash scripts/dev.sh make test` | 0 | 7개 파일 전부 0 failures (config 15, health 17, http 118, mime 26, proxy 64, reload 449, route 41723) | 0 | - |
| 3 | `bash scripts/dev.sh make asan` | 0 | 같은 7개 전부 0 failures | 0 | 0 (`Sanitizer`/`runtime error` 0건) |
| 4 | `bash scripts/dev.sh bash scripts/functional_test.sh` | 0 | **passed 76, failed 0** | 0 | 0 |
| 5 | 복사본 컨테이너 lb.sh 회귀 | 0 | **PASS 12, FAIL 0** | - | - |

4번 참고: 3번(asan) 직후라 functional_test 안의 `make -s all`이 아무것도 다시 빌드하지 않았습니다. 그래서 e2e도 **ASan 빌드 webserver**로 돌았습니다.
각 서버의 stderr는 테스트 임시 폴더에 쓰이고 테스트가 끝나면 지워지므로, 서버 쪽 Sanitizer 출력은 따로 확인하지 못했습니다. 테스트 출력 자체에는 0건입니다.

## 5. lb.sh 회귀 상세 (복사본)
- `up`: front cmdline `-R /echo@ketama=@.lb/front.route`.
- 부하(curl, 출발지 IP 50개, 160 요청) 중 add, add, remove b2 → 각 `front reloaded (pid 156, :8080, webserver)`, rc=0.
  - PASS: front pid 불변, 200 아닌 응답 0, status 행 = 멤버. `reload: ok` 3줄.
- 8개까지 채운 뒤 9번째 add → `the front takes at most 8 backends …`.
  - PASS: rc=1, front pid 불변, 멤버 8개 유지.
- 옛 인라인 front(`-R /echo@ketama=127.0.0.1:8001,…`)에서 remove b3 → 재시작(`front started (pid 1037 …)`).
  - PASS: 새 pid, 새 front는 `=@` 방식, status 행 = 멤버.
- front를 띄운 직후 0.05초에 add → PASS: front 같은 프로세스, 살아 있음, status 행 = 멤버, `reload: ok`.
  - **주의:** 0.05초 시점에 front가 이미 연결을 받고 있었고, add가 bN을 먼저 띄운 뒤에야 HUP을 보냅니다. 그래서 이 검사는 "시작 중 HUP" 창을 실제로 건드리지 못했습니다.

### 추가 검사 (요청 밖, 시작 중 창을 직접 노림)
`./webserver … -R "/echo=@file" &` 직후 지연 d를 두고 바로 `kill -HUP`을 보낸 뒤, 0.6초 후 생존 여부와 `/echo`, `reload: ok`를 확인했습니다. 20회 실행.
- d = 1ms, 2ms, 5ms, 10ms, 20ms, 30ms, 50ms, 100ms, 200ms (15회): **모두 생존, /echo 200, `reload: ok` 1줄.** main.c의 SIGHUP 블록이 시작 중에 온 HUP을 보류했다가 처리한 것으로 보입니다.
- d = 0 (5회): 5회 모두 `Hangup`으로 종료. 다만 이 5회는 `2>/tmp/f.log` 리다이렉트 파일조차 생기지 않았습니다.
  즉 bash가 fork한 자식이 **exec하기 전에** 죽은 것이고, webserver의 main은 실행되지도 않았습니다. 프로그램 코드로는 막을 수 없는 구간입니다.
  - lb.sh 영향: 이 구간에는 `/proc/<pid>/cmdline`이 bash 것이라 `=@`가 없습니다. 그래서 lb.sh는 HUP을 보내지 않고 refront 쪽으로 갑니다. 이론상 `start front`와 `add`가 동시에 돌 때만 생기는 아주 좁은 경쟁입니다.

## 실패
없음.
