# 계약: SIGHUP으로 백엔드 목록 무중단 교체

조율자가 정한 인터페이스입니다. 두 작업자 모두 이 문서를 기준으로 일하고, 바꿔야 하면 먼저 조율자에게 알립니다.

## 1. 파일 문법
- `-R` 값에서 `=` 바로 뒤가 `@`이면 나머지는 **파일 경로**입니다.
  - `-R '/echo@ketama=@.lb/front.route'`, `-R '/api/=@/etc/x.list'`
  - `@` 없는 기존 문법(`/echo=127.0.0.1:8001,...`)은 그대로 동작하고, 그 route는 reload 대상이 아닙니다.
- 경로는 시작 시 `realpath()`로 절대 경로로 바꿔 `route.file`에 저장합니다(`sizeof route.file` = 256, 넘으면 파싱 실패).
- 파일 내용: 백엔드 토큰을 쉼표, 공백, 탭, 줄바꿈(`\r` 포함)으로 구분합니다. `#`부터 줄 끝까지는 주석입니다.
  토큰 문법은 기존과 같습니다: `host:port`, `[v6]:port`, `*N` 가중치(1..4).
- 오류(시작 시에는 config 오류로 종료, reload 시에는 기존 목록 유지 + stderr 한 줄):
  파일을 열 수 없음 / 잘못된 토큰 / 0개 / `ROUTE_MAX_BACKENDS` 초과 / 같은 파일 안에 같은 host:port 중복 / 파일이 64KiB 초과.
- 시작 시 파일 오류 메시지: 기존 `invalid route spec: ...` 앞이나 뒤에 이유 한 줄 추가 가능.

## 2. 데이터 구조 (include/route.h)
- `backend`는 **개별 할당 + 참조 카운트**. 같은 host:port 백엔드는 reload 후에도 **같은 객체**를 공유하므로
  헬스 상태, 카운터, `inflight`가 그대로 이어집니다(복사 없음).
  - `backend`에 `unsigned refs;`(atomic) 추가. `weight` 필드는 backend에서 **빼서** route로 옮깁니다(아래).
- `route`:
  - `backend *backends[ROUTE_MAX_BACKENDS];` (값 배열 → **포인터 배열**)
  - `unsigned weight[ROUTE_MAX_BACKENDS];` (가중치는 route별, 그래서 reload로 가중치만 바뀌어도 백엔드 상태 유지)
  - `char file[256];` (빈 문자열이면 파일 route 아님)
  - 나머지(`prefix`, `backend_count`, `next`, `ketama`, `ring_len`, `ring_hash`, `ring_backend`)는 그대로.
- `route_table`: `routes[]`, `count` + `unsigned refs;`(atomic) + `int heap;`(1이면 마지막 release 때 `free`).
- 기존 API는 이름과 의미 유지: `route_table_init`(refs=1, heap=0), `route_table_add`, `route_match`, `route_pick`,
  `route_share`, `backend_report`, `backend_is_up`, `backend_try_acquire`, `backend_release`.

### 새 API
```c
/* Drop one reference. At zero: drop every backend reference (free a backend
 * whose refs hit 0), then free(t) if t->heap. Also the way to clean up a
 * stack/static table built with route_table_init + route_table_add. */
void route_table_release(route_table *t);

/* Build a new heap table (refs=1, heap=1) from old: routes without a file
 * share old's backends (incref); routes with a file re-read it, reusing the
 * old backend object for every host:port already in that route (incref),
 * new ones start fresh. The ring is rebuilt. Returns 0 and *out, or -1 with
 * a one-line reason in err (old untouched, nothing leaked). */
int route_table_reload(const route_table *old, route_table **out,
                       char *err, size_t err_size);

typedef struct {
    pthread_mutex_t lock;     /* guards cur and the load+incref step only */
    route_table *cur;
} route_live;

void route_live_init(route_live *l, route_table *t);   /* takes t's reference */
route_table *route_live_acquire(route_live *l);        /* lock; cur->refs++; unlock */
void route_live_swap(route_live *l, route_table *t);   /* takes t's ref; releases old cur */
void route_live_destroy(route_live *l);                /* releases cur, destroys lock */
```
- 규칙: `backend *`는 그것을 꺼낸 table 참조를 들고 있는 동안만 씁니다. table 참조가 backend 참조를 보장합니다.

## 3. 사용 위치 (src/)
- `server_config`(include/server.h)에 `route_live live;` 추가. `routes`(값)는 파싱 대상으로 남깁니다.
  `config_parse_args` 성공 끝에 `route_live_init(&cfg->live, &cfg->routes)`.
  (`cfg->routes`는 static이라 heap=0: release돼도 free하지 않고 백엔드 참조만 놓습니다.)
- 워커(`dispatch`): `route_live_acquire` → `route_match` → `proxy_route`(요청 끝까지) → `route_table_release`.
  route가 없으면 바로 release하고 기존 3번 경로로.
- 헬스 스레드: 한 바퀴(sweep)마다 acquire/release.
- `lb_status_render(out, size, cfg, ...)`: 시그니처 유지, 내부에서 `cfg->live`로 acquire/release.
- 시그널: `server_run`에서 `SIGHUP` 핸들러는 `volatile sig_atomic_t g_reload = 1`만 합니다.
  워커/헬스 스레드는 SIGINT/SIGTERM과 함께 **SIGHUP도 블록**해서 main 스레드가 받습니다.
  `accept_loop`가 매 반복(poll이 EINTR로 깨어난 직후 포함) `g_reload`를 확인하고 main 스레드에서
  `route_table_reload` → 성공 시 `route_live_swap`. 그래서 reload 후 `/_lb/status`는 즉시(≤ poll 500ms) 새 목록을 보여 줍니다.
- stderr 로그(테스트가 grep함, 정확히 이 접두사):
  - 성공: `reload: ok` 로 시작하는 한 줄 (뒤에 route별 개수 등 자유)
  - 실패: `reload: failed: <이유>` 한 줄, 기존 목록 유지
- 종료 시 `server_run` 끝(스레드 join 후)에 `route_live_destroy(&cfg->live)`.

## 4. 테스트가 의존하는 동작
- `-R` 기존 문법 결과(필드 값)는 같습니다. 단, 접근은 `r->backends[i]->host`, 가중치는 `r->weight[i]`.
- `route_table_release`로 정리하면 ASan/LSan 누수 없음.
- `route_table_reload` 후 같은 host:port는 **같은 포인터**(`new->routes[k].backends[i] == old->routes[k].backends[j]`).
- 잘못된 파일이면 `route_table_reload`가 -1, `*out` 미변경, 누수 없음.
- `/_lb/status` 표 형식은 그대로(열, 순서). 행 순서는 파일 순서.
- 실행 중 `kill -HUP <front>` → 1초 안에 status에 새 목록, 부하 중에도 클라이언트 실패 0.

## 5. scripts/lb.sh
- front는 `-R /echo$(route_sep)@.lb/front.route`로 띄웁니다. `.lb/front.route`는 멤버마다 `127.0.0.1:<port>` 한 줄.
  멤버 목록 `.lb/backends`(이름 목록)는 그대로 유지합니다.
- `add`/`remove`: route 파일을 새로 쓰고(임시 파일 후 `mv`로 원자적 교체) front가 돌고 있으면 `kill -HUP`,
  그다음 `/_lb/status`에 반영될 때까지 최대 약 2초 기다립니다. front가 안 돌면 파일만 갱신.
- **안전장치**: 실행 중 front의 `/proc/<pid>/cmdline`에 `=@`가 없으면(옛 바이너리/옛 방식으로 뜬 front)
  HUP을 보내지 말고 기존 `refront()`(재시작)로 처리합니다. 옛 바이너리는 SIGHUP에 죽기 때문입니다.
- 사용법 문구와 `mini-web` 출력 형태는 유지(“restart the front” 같은 설명만 reload로 수정).
