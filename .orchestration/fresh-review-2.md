# fresh-review-2: SIGHUP reload 동시성·메모리 안전 리뷰

근거: brief.md, contract.md, core.diff, 그리고 맥락 확인용 src/·include/ (읽기만 함, 빌드·실행 안 함).
결론: **중요(UAF, 레이스, 이중 해제, 누수) 지적 없음.** 사소 1건, shortcut 주석 판정 1건.

## 지적

### 1. [사소] 시작 직후 SIGHUP가 버려져 파일 변경이 반영되지 않을 수 있음
- 위치: `src/main.c:10` (`signal(SIGHUP, SIG_IGN)`), `src/server.c:767-768` (핸들러 설치)
- 시나리오:
  1. front 시작. `config_parse_args` → `route_table_add` → `read_route_file`가 파일 내용 A를 읽음.
  2. 그 직후, `server_run`이 `sigaction(SIGHUP, on_reload)`를 하기 전에 lb.sh가 파일을 B로 바꾸고 `kill -HUP`.
  3. 이때 처리 방식이 SIG_IGN이라 커널이 시그널을 버림(펜딩도 안 남음). front는 A로 계속 돌고, 다음 HUP이 올 때까지 B가 반영되지 않음.
- 메모리 안전 문제는 아님. 창이 짧아서(파싱 끝 ~ sigaction, 수 ms) 실제로 걸릴 확률은 낮음. 다만 "front 띄우자마자 add" 흐름에서 lb.sh가 status 반영을 약 2초 기다리다 타임아웃할 수 있음.
- 고치는 방법(참고): 무시하지 말고 **블록**해 두기. 즉 main에서 `sigprocmask(SIG_BLOCK, {SIGHUP})`를 하고, server_run에서 핸들러를 설치한 다음, 워커·헬스 스레드를 블록 상태로 만들고 나서 `SIG_SETMASK, &old`로 풀 때 SIGHUP까지 풀면 됨. 그러면 펜딩된 HUP이 main 스레드로 전달되어 첫 루프에서 reload함.
- 확신: 높음 (메커니즘). 실제 영향: 낮음.

### 2. [판정] `shortcut:` 주석 (`src/route.c:208`)이 정확한가
- 주석: "a quick re-add splits inflight across objects while old requests run".
- 판정: **정확함.** 시나리오: T1{b} 상태에서 워커가 T1과 b로 긴 요청 처리 중 → remove로 T2{} 교체 → 곧바로 add로 T3 생성. 이때 `find_backend(old=T2)`는 b를 못 찾으므로 새 객체 b'를 만듦. 그 결과 같은 host:port에 b(inflight 1)와 b'(inflight 0)가 동시에 존재하고, cap이 일시적으로 최대 2배까지 허용됨. 메모리 안전 문제는 없음(b는 T1 참조로 살아 있음).
- 덧붙일 점(사소, 주석 범위 밖): remove 뒤 add는 시간 간격과 상관없이 헬스·카운터도 항상 새로 시작함. brief 3번("기존 백엔드"만 유지)에 비춰 보면 의도된 동작으로 봄.
- 확신: 높음.

## 문제없음으로 확인한 항목
- **워커의 backend\*/route\***: `dispatch`가 acquire → `route_match` → `proxy_route` → release 순서로 처리함 (server.c:510-519). `route_pick`이 돌려준 `c[]`, `backend_release`, `req_ok/req_err` 증가가 모두 table 참조를 쥔 동안 일어남. route가 없을 때도 release함.
- **헬스 스레드**: sweep마다 acquire/release함 (health.c:38,49). 빠진 백엔드는 다음 sweep부터 순회하지 않음. sweep 중에는 옛 객체에 report해도 객체가 살아 있으므로 안전함.
- **status 렌더링**: acquire 뒤의 모든 오류 경로가 `too_small`로 가서 release함. 그 앞의 early return(헤더 append)은 acquire 전이라 문제없음. `(route_live *)&cfg->live`로 const를 벗기지만, cfg 실체가 `static server_config`(const 아님)라서 UB가 아님.
- **publish happens-before**: main 스레드가 새 테이블을 다 만든 뒤 mutex 안에서 `cur`를 교체하고, reader도 같은 mutex 안에서 읽음. 그래서 초기화가 끝난 테이블만 보임. refs 증가를 RELAXED로 해도 mutex 순서 덕분에 swap 쪽 감소보다 앞섬.
- **refs 증감 순서**: 감소는 ACQ_REL이라 0이 된 쪽이 다른 사용자의 이전 접근을 모두 보고 나서 해제함. 테이블 해제 → 백엔드 감소 순서도 맞음. reload 중 재사용 incref(RELAXED)는 main이 `old` 참조를 쥔 동안(server.c:698) 하므로 0에서 되살아나는 경우가 없음. 다른 스레드가 동시에 감소하는 것은 원자 RMW라 안전함.
- **비원자 필드**: `host/port/weight/ring/prefix/file`은 생성 후 바뀌지 않음. `next`, 헬스 필드, 카운터, `inflight`는 모두 `__atomic`으로 접근함. reload가 `prev->next`를 읽는 것도 atomic load.
- **이전 테이블 해제 시점·이중 해제**: swap은 live가 가진 참조만 놓고, reload_routes는 자기 acquire 참조를 따로 놓으므로 짝이 맞음. `route_live_destroy`는 정상 종료 경로에서 health_stop과 워커 join 이후에만 호출되고, 오류 경로(listener, queue 실패)에서는 스레드를 만들기 전에 한 번만 호출됨. main은 따로 destroy하지 않음.
- **정적 테이블 heap=0**: 첫 swap 뒤 마지막 사용자가 끝나면 정적 `cfg->routes`의 백엔드만 놓고 `free`는 하지 않음. 그 뒤 `cfg->routes`를 읽는 곳은 server.c:809 로그 한 줄뿐이고, 이것은 accept_loop 진입 전(즉 첫 reload 전)에 실행됨.
- **reload 실패 시 부분 incref 정리**: `t->count++`를 파일을 읽기 전에 하므로, 실패한 route가 그때까지 추가한 백엔드도 `route_table_release(t)`가 정리함. 재사용한 객체는 old가 쥐고 있어 refs ≥ 2라서 감소만 되고, 새로 만든 객체는 free됨. `*out`은 바뀌지 않음. `add_backend`의 malloc 실패와 중복 검사는 incref나 할당 전에 return하므로 누수가 없음. `route_table_add` 실패 시에는 `release_backends(&entry)`로 정리하고, config 실패·`-h` 경로에서도 정적 테이블을 release함.
- **inflight 이전**: 같은 host:port는 같은 객체를 공유하므로 복사가 없고, 옛 요청의 `backend_release`도 같은 카운터를 줄임. 재사용 판정은 같은 index의 old route를 대상으로 하는데, route 순서가 old에서 그대로 복사되므로 일치함. ketama ring은 테이블마다 다시 만들고 index도 그 테이블의 `backends[]` 기준이라 섞이지 않음.
- **async-signal-safety**: `on_reload`는 `volatile sig_atomic_t` 저장만 하고, `on_signal`의 `__atomic_store_n`(lock-free int)도 핸들러에서 써도 안전함. reload 작업 자체는 main 스레드 루프에서 함.
- **시그널 마스크**: 핸들러를 설치한 다음 SIGHUP을 블록하고 워커·헬스 스레드를 만든 뒤 main만 마스크를 복원함. 그래서 HUP은 main으로만 감. 블록 전에 들어와도 그때는 스레드가 main 하나뿐이라 문제없음. CGI 자식은 `sigprocmask(SIG_SETMASK, &none)`(cgi.c:270)으로 블록 마스크를 풀고, exec가 잡힌 핸들러를 SIG_DFL로 되돌림.
- **놓치는 HUP**: `g_reload = 0`을 reload보다 먼저 하므로, reload 중에 온 HUP은 다음 반복에서 다시 처리됨. 여러 번 온 HUP이 한 번으로 합쳐져도 최신 파일을 읽으므로 괜찮음. `SA_RESTART`가 없어서 poll이 EINTR로 바로 깨어나고, check와 poll 사이에 들어온 경우에도 최대 500ms라 1초 요구를 충족함. read 루프는 EINTR을 처리하고, FIFO는 `O_NONBLOCK`과 `S_ISREG` 검사로 막음.

## 확인 안 한 것
- `proxy_reverse_fd`가 `b`를 요청이 끝난 뒤까지 저장하는지는 시그니처와 호출 위치만 보고 판단함(요청 안에서만 쓰는 것으로 가정).
- tests/, scripts/는 지시대로 열지 않음. 실제 ASan 결과는 확인하지 않음.
