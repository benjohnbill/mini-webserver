# verify0 (impl-tools): ROUTE_MAX_BACKENDS / LB_BACKENDS 검증

환경: rules.md의 복사본 컨테이너(`mini-webserver-tools`, `/ro:ro` → tar로 `/work`에 복사, `.lb/.git/build/webserver` 제외).
`make -s all`은 작업 트리 src 그대로 빌드 성공(HEAD 대체 불필요. 참고로 이미지에 git이 없어 `git archive` 대체는 어차피 안 됨).
스크립트: 컨테이너 stdin으로 넘김(`bash -s`). 저장소/.lb/mini-lab 안 건드림.

## 1차 결과
- `up` → b1..b3 + front 기동. `add` 5번 → b4..b8 추가, members `b1 … b8`, status 8행. ✅
- 9번째 `add` → `the front takes at most 8 backends (ROUTE_MAX_BACKENDS); remove one first`, rc=1,
  front pid 713 → 713 그대로(살아 있음), b9.pid 없음, members 그대로. ✅
- `LB_BACKENDS=9|0|x up` → **버그** ❌:
  ```
  LB_BACKENDS started (pid 964, :8009, webserver)
  scripts/lb.sh: line 57: must: unbound variable
  must failed to start, see .lb/must.log
  ```
  원인: `BACKENDS=$(defaults)`에서 `defaults`의 `exit 1`은 `$(...)` 서브셸만 끝냄. 에러 문구(stdout)가 BACKENDS 값이 되어
  "LB_BACKENDS"라는 이름의 백엔드가 :8009(`port LB_BACKENDS` → 8000+9…)로 떠 버림.
- `LB_BACKENDS=8 up` → 8개 + front, rc=0, status 8행. ✅

## 수정 (scripts/lb.sh, 3줄)
- `defaults`의 에러 문구를 `>&2`로.
- 두 호출부(최상단 members 없을 때, `up`에서 front가 안 돌 때)에 `|| exit 1`.

## 2차 결과 (수정 후)
```
== LB_BACKENDS=9 up   → LB_BACKENDS must be 1..8 (got '9')  rc=1 pids: (없음)
== LB_BACKENDS=0 up   → LB_BACKENDS must be 1..8 (got '0')  rc=1 pids: (없음)
== LB_BACKENDS=x up   → LB_BACKENDS must be 1..8 (got 'x')  rc=1 pids: (없음)
== LB_BACKENDS=8 up   → front started, rc=0, members b1..b8, status 8행
== add 9th            → 거부 rc=1, front pid 290 -> 290
== LB_BACKENDS=x up (front 실행 중) → "front is already running", rc=0 (기존 멤버 유지, 검증 안 함: 의도대로)
```
부작용: members 파일이 없을 때는 `LB_BACKENDS`가 잘못되면 `status`/`down` 등 다른 명령도 같은 문구로 rc=1 (기존에도 최상단에서 defaults를 부르던 자리).
