# 문맥 없는 리뷰: SIGHUP reload의 동시성과 메모리 안전

근거: brief.md, contract.md, core.diff, 그리고 src/·include/ 현재 파일(줄 번호는 현재 작업 트리 기준).
빌드나 실행은 하지 않았습니다. 정적으로 읽고 판단했습니다.

## 결론
중요 등급 문제는 찾지 못했습니다. use-after-free, 이중 해제, 누수, 데이터 레이스로 이어지는 경로는 보이지 않습니다.
사소한 지적 2건이 있습니다.

## 지적

### 1. [사소] 옛 테이블이 아직 살아 있을 때 제거했다가 다시 추가하면 같은 host:port 객체가 둘이 됨
- 위치: src/route.c:208 (`add_backend`의 `find_backend(old, &parsed)`), src/route.c:287-300 (`route_table_reload`가 직전 테이블 `prev`만 봄)
- 시나리오:
  1. T0에 B(127.0.0.1:8001)가 있음. 워커 W1이 T0를 acquire하고 B로 오래 걸리는 요청을 처리 중(B.inflight=1).
  2. 파일에서 B를 빼고 HUP, main이 T1(B 없음)을 만들어 swap. T0는 W1이 들고 있어 살아 있고, B 객체도 살아 있음.
  3. 파일에 B를 다시 넣고 HUP, main이 T2를 만듦. `find_backend(T1, B)`가 NULL이라 B'를 새로 malloc(inflight=0, 헬스 상태 초기화).
  4. 이제 같은 host:port에 B(inflight=1, T0)와 B'(inflight=0, T2)가 함께 있음. `-B` cap이 W1의 요청을 세지 못해 그 순간 cap+1개까지 보낼 수 있고, status에는 inflight 0으로 보임.
- 메모리 안전에는 문제가 없습니다(각 객체의 refs와 inflight는 따로 맞게 증감되고 T0가 해제될 때 B가 free됨).
  계약도 "already in that route"로 정했으므로 계약 위반은 아닙니다. 다만 "inflight가 이어진다"는 보장은 직전 테이블에 있는 백엔드에만 해당합니다.
  잠깐 빠졌다가 다시 들어온 백엔드는 헬스 상태와 카운터가 초기화되는데, 요구사항 3의 "새 백엔드는 새 상태"에 해당하므로 정상입니다.
- 확신: 높음(동작이 실제로 그렇게 됨). 영향: 낮음(과도기의 cap 초과가 최대 요청 1개 수준이고 곧 해소됨).

### 2. [사소] `server_run`의 `sigaction(SIGHUP)` 전에 HUP이 오면 프로세스가 죽음
- 위치: src/server.c:767-768. 그 전 구간인 `main` → `config_parse_args`(route 파일 `realpath`와 읽기 포함)에서는 SIGHUP이 기본 동작(종료)입니다.
- 시나리오: lb.sh가 front를 띄우고 cmdline에 `=@`가 보이자마자(`/proc/<pid>/cmdline`은 exec 직후부터 보임) `add`로 `kill -HUP`을 보냅니다. 그 시점에 front가 아직 인자를 파싱하고 있으면 SIGHUP 기본 동작으로 종료됩니다.
  옛 바이너리를 위한 안전장치(`=@` 검사)로는 이 경우를 막지 못합니다.
- 해결 예: `main` 맨 앞(또는 `config_parse_args` 전)에 핸들러를 설치하거나 SIGHUP을 블록해 두고 `server_run`에서 풉니다. 보류된 HUP은 `g_reload`로 이어집니다.
- 확신: 중간(구간은 확실히 있음. lb.sh가 시작 직후 곧바로 HUP을 보내는지는 scripts/를 보지 않아 확인하지 못함). 발생 확률: 낮음.

## 문제없다고 확인한 항목
- **acquire와 swap의 경쟁**: `route_live_acquire`가 lock 안에서 `cur`를 읽고 incref하고, `route_live_swap`은 lock 안에서 `cur`를 바꾼 뒤 lock 밖에서 old를 release합니다. 그래서 refs가 0이 된 테이블을 acquire하는 경우가 없습니다(src/route.c:162-178).
- **publish 순서(happens-before)**: 새 테이블의 모든 필드와 ring은 main 스레드가 swap 전에 씁니다. mutex unlock/lock 쌍이 독자와의 순서를 보장합니다. 이후 route와 backend의 비원자 필드(host, port, weight, backend_count, ring)는 바뀌지 않고, `next`와 헬스·카운터 필드는 `__atomic`으로만 접근합니다. reload도 `prev->next`를 atomic load로 복사합니다(src/route.c 약 291줄).
- **refs 순서**: incref는 이미 참조를 가진 쪽(lock 안, 또는 old를 들고 있는 main)만 하므로 RELAXED로 충분합니다. decref는 ACQ_REL이고 0일 때만 free합니다(src/route.c:137-154). 표준 패턴입니다.
- **워커가 쓰는 backend\***: `dispatch`가 `proxy_route` 전체(`route_pick`, `backend_try_acquire/release`, `req_ok/err` 증가)를 table 참조 안에서 처리하고 끝난 뒤 release합니다(src/server.c:510-520, 447-473). 테이블 밖으로 새는 포인터는 없습니다.
- **헬스 스레드**: 한 바퀴(sweep)마다 acquire/release하고, 그동안 `r->backends[j]`만 씁니다(src/health.c:38-49). 같은 backend 객체가 한 테이블 안에서 두 번 나오지 않으므로(재사용은 같은 인덱스의 route에서만, 파일 안 중복은 거부) 같은 객체에 report가 중복으로 들어가지 않습니다.
- **status 렌더링**: acquire 후 정상 경로와 `too_small` 경로 모두 release합니다(src/health.c:108-168). const를 떼는 cast는 mutex와 refcount에만 쓰입니다.
- **이전 테이블 해제 시점**: main의 `reload_routes`가 old를 따로 acquire하고 swap 뒤 release하므로, 마지막 워커나 헬스 스레드가 release할 때 해제됩니다. 이중 해제는 없습니다.
- **정적 `cfg->routes`(heap=0)**: 마지막 release에서 backend 참조만 놓고 free하지 않습니다. 그 뒤 `cfg->routes`를 읽는 곳은 src/server.c:809의 `count`뿐인데, accept_loop 전(첫 reload 전)이고 release는 count를 바꾸지 않으므로 안전합니다. `main`이 따로 release하지 않으므로 이중 release도 없습니다(src/ 전체 grep으로 확인).
- **reload 실패 시 정리**: 새 테이블은 `t->count++`를 먼저 해서, 만들다 만 route까지 `route_table_release(t)` 대상에 들어갑니다. reuse한 객체는 old도 참조하고 있어서 0이 되지 않고, 새로 만든 객체(refs=1)만 free됩니다. 중복이나 파싱 오류일 때 `backend_count`가 늘지 않으므로 넣지 않은 객체를 release하는 일도 없습니다. `*out`은 바꾸지 않습니다.
- **inflight와 헬스 상태 이어받기**: 같은 route, 같은 host:port(포트는 `%hu`로 정규화)이면 같은 객체를 incref해서 공유하므로 down/fails/oks/changed_ms, 카운터, inflight가 복사 없이 이어집니다. 가중치만 바뀐 경우에도 route별 `weight[]`라서 객체가 유지됩니다.
- **종료 순서**: `health_stop`과 모든 worker join이 끝난 뒤 `route_live_destroy`를 호출합니다(src/server.c:818-824). 조기 실패 경로(listener, queue)에서도 한 번씩만 destroy합니다.
- **핸들러의 async-signal-safety**: `on_reload`는 `volatile sig_atomic_t`에 대입만 합니다. reload 자체(malloc, 파일 I/O, fprintf)는 main 스레드의 일반 문맥에서 합니다.
- **시그널 마스크**: worker와 헬스 스레드를 모두 SIGINT/SIGTERM/SIGHUP을 블록한 상태에서 만들고, 그 뒤 main만 마스크를 되돌립니다(src/server.c:784-800). 블록하는 동안 들어온 HUP은 보류됐다가 unmask할 때 전달되어 `g_reload`로 처리됩니다. CGI 자식은 exec 전에 마스크를 비웁니다(src/cgi.c:270).
- **놓치는 HUP과 재진입**: `if (g_reload) { g_reload = 0; reload }` 순서라서, 확인과 지우기 사이에 들어온 HUP은 합쳐지지만 그 뒤 reload가 최신 파일을 읽으므로 내용은 놓치지 않습니다. reload 중에 들어온 HUP은 다음 반복에서 처리됩니다. 핸들러는 플래그만 세우므로 재진입해도 문제없습니다. poll이 EINTR로 깨면 `continue`해서 바로 확인하고, poll 직전에 들어왔으면 최대 500ms 뒤에 확인합니다.
