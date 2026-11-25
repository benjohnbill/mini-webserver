# impl-tools (Claude) 작업
먼저 `.orchestration/rules.md`, `.orchestration/brief.md`, `.orchestration/contract.md`를 끝까지 읽으세요.

범위: **수정 가능: `tests/`, `scripts/`, `README.md`만.** src/, include/는 다른 작업자(impl-core, Codex)가 동시에 고치고 있으니 건드리지 마세요.

## 0단계 (먼저, lb.sh를 고치기 전에): 이미 고친 버그 검증
현재 `scripts/lb.sh`는 `include/route.h`의 `ROUTE_MAX_BACKENDS`(8)를 `MAXN`으로 읽어 멤버 8개면 `add`를 거부하고 `LB_BACKENDS`도 1..8만 받습니다.
rules.md의 **복사본 컨테이너** 명령으로만 검증하세요(그 안에서 `make -s all` 후 lb.sh 실행). 확인할 것:
`up` 후 add를 반복해 8개까지 추가됨 / 9번째 `add`는 거부되고 front pid가 그대로(재시작 안 됨) / `LB_BACKENDS=9`, `0`, `x`는 거부, `LB_BACKENDS=8 up`은 됨.
결과(명령과 출력 요약)를 `.orchestration/verify0.md`에 쓰고 `hc send t2q "verify0 done: .orchestration/verify0.md"`로 먼저 보고한 뒤 다음 단계로 갑니다.
(복사본은 지금 작업 트리를 복사하므로 impl-core가 고치는 중인 src가 섞여 빌드가 깨질 수 있습니다. 깨지면 tar 대신 `git stash` 없이
`git -C /ro archive HEAD` 로 src/include만 HEAD 것을 쓰거나 하는 식으로 복사본 안에서만 처리하고, 어떻게 했는지 적으세요. 저장소 자체는 건드리지 않습니다.)

## 1단계: 계약 7~9번 구현
- `scripts/lb.sh`: 계약 5절대로(파일 route로 front 기동, add/remove는 route 파일 원자 교체 + `kill -HUP`, 반영 대기, 옛 front 안전장치).
  `mini-web` 사용법과 출력 형태 유지. `bp_demo.sh`, `functional_test.sh` 기존 동작 유지.
- `tests/`: 계약 2절 구조 변경에 맞춰 기존 테스트 수정(`backends[i]->`, `weight[i]`, `route_table_release`로 정리, test_health는 `route_live_init` 필요).
  새 단위 테스트(기존 `tests/test.h` 방식): 파일 route 파싱, reload 후 같은 host:port는 같은 포인터·상태 유지, 새 백엔드 새 상태,
  제거 반영, 잘못된 파일(파싱 실패/0개/9개/없는 파일/중복)이면 -1이고 기존 유지, 여러 스레드가 acquire/release하는 동안 swap 반복(ASan용).
- `scripts/functional_test.sh`: reload 검사 추가 — 추가한 백엔드가 status에 보이고 트래픽을 받는다 / 제거한 백엔드가 빠진다 /
  부하 중 reload해도 실패 0 / 잘못된 파일이면 기존 목록 유지(+ stderr `reload: failed:`) / 기존 백엔드 상태(OK 카운터 등) 유지.
  기존 포트와 겹치지 않게 하세요.
- README의 lb.sh 절 갱신.
- C 코어가 아직 안 됐으면 테스트는 계약 기준으로 먼저 쓰고, 조율자가 impl-core 완료를 알려 주면 그때 `make test`/`make asan`/functional_test를 돌리세요.
- 끝나면 `.orchestration/impl-tools.md`에 바뀐 파일, 요약, 실행한 테스트 출력 요약을 쓰고 `hc send t2q "impl-tools done: .orchestration/impl-tools.md"`.
