# review-core (impl-tools가 impl-core의 C 변경 리뷰, 읽기만 함)

대상: `git diff -- src include` (route.h/route.c, server.h, config.c, health.c, server.c)

## 결론
**중요 문제 없음.** 사소 2건. 동적 확인: make test, make asan(경고 0, Sanitizer 보고 0), functional_test 74/0,
test_reload의 8스레드 acquire/release + 300회 swap(ASan)까지 모두 깨끗합니다.

## 확인한 것 (문제 없음)
- 참조 카운트 / UAF
  - `route_live_acquire`는 mutex 안에서 cur를 읽고 refs++ 합니다(route.c:163-169).
  - `route_live_swap`은 lock 밖에서 old를 release하고(172-179), 감소는 ACQ_REL, 0이면 free합니다(139-155).
  - 워커 `dispatch`는 `proxy_route`가 끝난 뒤에만 release하고, release 후에는 `r`나 백엔드를 쓰지 않습니다(server.c:510-520).
  - 헬스 스레드는 한 sweep 동안 표를 잡고(health.c:38-49), status 렌더링은 모든 return 경로에서 release합니다(`too_small` 포함).
  - `cfg->routes`를 직접 쓰는 곳은 config 파싱과 시작 로그의 `count`뿐입니다(grep 확인).
- 새 backend 공개: main 스레드에서 다 쓴 뒤 swap의 mutex를 통과해 publish되고, 읽는 쪽도 mutex로 acquire하므로 happens-before가 성립합니다.
  host/port는 생성 뒤 읽기 전용이고, 카운터와 상태는 전부 `__atomic`입니다.
- reload 실패 시: 부분 route도 `t->count++` 뒤에 채우므로 `route_table_release(t)`가 이미 incref한 백엔드와 새로 만든 백엔드를 모두 놓습니다(route.c:286-296).
  `*out`은 성공할 때만 씁니다. 시작 시 `route_table_add` 실패는 `release_backends(&entry)`로 정리됩니다.
  config의 모든 실패 경로(`goto fail`, `-h`)에서 표를 release합니다.
- 계약 1절
  - `=@` 판정, realpath, 256 제한, 구분자 `, \t\r\n`, `#` 주석(토큰 바로 뒤 `a:1#x`도 됨), 0개, 9개, 중복, 64KiB 초과, 못 여는 파일 모두 OK.
  - 추가로 FIFO나 일반 파일이 아닌 것, NUL 바이트도 거부합니다.
- 계약 2절과 4절: 필드, API, `heap`/`refs` 의미가 일치하고, 같은 host:port는 같은 포인터이며, 가중치는 route별입니다.
- 계약 3절과 시그널
  - SIGHUP 핸들러는 플래그만 세웁니다. 워커와 헬스 스레드는 SIGHUP이 블록된 상태로 생성됩니다(server.c:785-799).
  - poll이 EINTR이면 `continue`로 루프 처음의 `g_reload` 검사로 돌아갑니다(711-721). reload 중에 HUP이 또 오면 플래그를 먼저 지웠기 때문에 다음 바퀴에 다시 처리됩니다.
  - CGI 자식은 fork 뒤 시그널 마스크를 비웁니다(cgi.c:270).
  - 로그 접두사 `reload: ok` / `reload: failed: <이유>` 확인. 종료 경로와 시작 실패 경로 모두 `route_live_destroy`.

## 사소
1. **reload 실패 로그에 어느 route·파일인지가 없음**
   - 위치: route.c:274-296(`snprintf(err, …, "%s", error)`), server.c:704.
   - 시나리오: 파일 route가 둘 이상일 때 `reload: failed: No such file or directory`나 `duplicate backend`만 찍혀서, 어느 파일이 문제인지 알 수 없습니다.
   - 제안: `err`에 `r->file`(또는 prefix)을 붙이기. 예: `"%s: %s", r->file, error`.
   - 계약 위반은 아닙니다(이유 형식은 자유).
2. **포트 앞자리 0이면 같은 주소가 다른 백엔드가 됨**
   - 위치: route.c:35-97 `parse_backend`는 `08001`을 받아들이고(5자, 값 8001), port를 **문자열 그대로** 저장합니다. 반면 `find_backend`(187-195)는 문자열로 비교합니다.
   - 시나리오:
     - 파일에 `127.0.0.1:8001`과 `127.0.0.1:08001`이 같이 있으면 중복 검사를 통과해 같은 서버가 두 행이 됩니다.
     - 파일이 `:8001` → `:08001`로 바뀌면 상태와 카운터가 초기화됩니다.
   - lb.sh는 항상 정규형을 쓰므로 실사용 영향은 없습니다. 고친다면 port를 숫자로 다시 써서 정규화하면 됩니다.

## 참고 (문제 아님)
- 인라인 route만 있는 front에 HUP을 보내도 이제 죽지 않고 `reload: ok`만 남습니다. 옛 바이너리와 다르지만 해롭지 않습니다. lb.sh의 `=@` 안전장치는 옛 바이너리 대비용이라 계속 필요합니다.
- 헬스 sweep은 표를 sweep 내내 잡으므로, 제거된 백엔드도 그 sweep이 끝날 때까지는 probe됩니다. 길어야 probe timeout × 개수 정도입니다.
- TSan은 컨테이너 권한 문제로 실행하지 못했습니다(impl-tools.md 참고).
