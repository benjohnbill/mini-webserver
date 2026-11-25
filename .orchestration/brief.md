# 작업 지시서: front 백엔드 목록 무중단 교체 (SIGHUP reload)

당신은 **조율자**입니다. 직접 코드를 고치지 말고, 작업자를 만들어 일을 나누고, 교차 검증을 돌리고,
보고를 모아 사용자에게 이 대화(당신의 터미널)로 알립니다. 사용자는 당신의 대화만 봅니다. 한국어로 씁니다.

## 배경
- 저장소: 이 폴더 (C 멀티스레드 웹서버, 브랜치 `hjm`). `README.md`, `scripts/lb.sh`, `src/`, `include/`, `tests/`를 먼저 읽으세요.
- `./webserver`를 `-R /echo@ketama=127.0.0.1:8001,...`로 띄우면 front(:8080)가 되어 `/echo`를 백엔드로 프록시합니다.
  헬스 체크(`src/health.c`, 1초 주기, `HEALTH_FAILS_TO_DOWN 3` / `HEALTH_OKS_TO_UP 2`)로 죽고 살아나는 것은 이미 실시간 반영됩니다.
- 문제: **멤버 목록**은 시작 시 `-R`로 고정됩니다. 그래서 `scripts/lb.sh add|remove`(lab에서는 `mini-web backend new|rm bN`)가
  front를 **재시작**합니다(`lb.sh`의 `refront()`). 재시작은 처리 중 요청을 끊고, 헬스 상태와 카운터를 초기화합니다.

## 할 일
### 0. 이미 고친 버그 검증 (먼저)
`include/route.h`의 `ROUTE_MAX_BACKENDS`는 8입니다. `scripts/lb.sh`는 이 값을 `MAXN`으로 읽어서
멤버가 8개면 `add`를 거부하고, `LB_BACKENDS`도 1..8만 받도록 이미 수정돼 있습니다(테스트는 아직 안 했습니다).
작업자 한 명에게 아래 "테스트 환경" 규칙대로 검증시키세요. 8개까지는 추가되고, 9번째는 front를 건드리지 않고 거부돼야 합니다.

### 1. B 방식: SIGHUP으로 백엔드 목록 다시 읽기
요구사항:
1. front는 route의 백엔드 목록을 **파일**에서도 읽을 수 있어야 합니다. 기존 `-R` 문법은 그대로 동작해야 합니다.
   파일을 가리키는 문법(예: `-R '/echo@ketama=@path'` 같은 것)은 계약 단계에서 정하세요.
2. `SIGHUP`을 받으면 파일을 다시 읽고 route를 **무중단으로** 교체합니다. 처리 중 요청은 끊기지 않아야 합니다.
3. host:port가 같은 기존 백엔드는 헬스 상태(down/fails/oks/changed)와 카운터(ok/err/shed 등)를 그대로 유지합니다.
   새 백엔드는 새 상태로 시작합니다.
4. 메모리 안전: 워커는 요청 동안 `backend *`를 들고 `inflight`를 증감하고, 헬스 스레드는 백엔드 배열을 순회합니다.
   새 테이블을 원자적으로 바꿔 끼우고, 이전 테이블은 마지막 사용자가 끝날 때 해제하세요(참조 카운트 등).
   use-after-free나 데이터 레이스가 없어야 하고, `make asan`이 깨끗해야 합니다.
5. 새 파일이 잘못됐거나(파싱 실패, 0개, `ROUTE_MAX_BACKENDS` 초과) 읽을 수 없으면 기존 목록을 유지하고 stderr에 이유를 남깁니다.
6. `/_lb/status`는 reload 후 1초 안에 새 목록을 보여 줘야 합니다. 대시보드가 이 표를 1초마다 읽습니다.
7. `scripts/lb.sh`: front를 파일 기반 route로 띄우고, `add`/`remove`는 재시작 대신 `kill -HUP`을 씁니다.
   `mini-web` 사용법과 출력 형태는 그대로 유지합니다. `bp_demo.sh`와 `functional_test.sh`의 기존 동작도 깨지면 안 됩니다.
8. 테스트: 단위 테스트(`tests/`, 기존 `tests/test.h` 방식)와 `scripts/functional_test.sh`에 reload 검사를 추가합니다.
   검사 항목: 추가한 백엔드가 status에 보이고 트래픽을 받는다 / 제거한 백엔드가 빠진다 / 부하 중 reload해도 실패 0 /
   잘못된 파일이면 기존 목록이 유지된다 / 기존 백엔드의 상태가 유지된다.
9. README의 lb.sh 절을 갱신합니다.

## 작업자와 진행 방식
- **먼저 당신이 계약을 정하세요**: 파일 문법, 새 함수와 구조체 이름, 시그널 처리 위치, 테스트가 의존할 동작.
  그 내용을 `.orchestration/contract.md`에 쓴 뒤 작업자를 만드세요.
- 작업자 두 명, 고치는 파일이 겹치지 않게 나눕니다(같은 작업 트리를 씁니다):
  - **Codex 작업자** (`hc run --bg --name impl-core --cwd <이 폴더> codex "..."`): C 코어 담당. `src/`, `include/`만 수정합니다.
  - **Claude 작업자** (`hc run --bg --name impl-tools --cwd <이 폴더> claude "..."`): `tests/`, `scripts/`, `README.md`만 수정합니다.
    0번 검증도 이 작업자에게 맡기세요.
- **검증은 교차로 합니다**: 구현이 끝나면 Claude 작업자가 Codex의 C 변경을 리뷰하고, Codex 작업자가 Claude의 테스트와 스크립트를 리뷰합니다.
  리뷰어는 읽기만 하고 수정하지 않습니다. 지적 사항은 당신에게 보고하고, 당신이 원작성자에게 수정을 넘깁니다.
  마지막 전체 테스트(아래 명령 전부)는 **구현하지 않은 쪽**이 실행해서 출력과 함께 보고합니다.
- 작업자에게 주는 첫 메시지에 범위, 수정 가능한 파일, 금지 사항(아래), 보고 방법을 모두 넣으세요.
  보고는 `hc send $HC_TERMINAL_ID "..."`로 받고, 긴 내용은 `.orchestration/<이름>.md`에 쓰고 그 경로만 보내게 합니다.
  작업자는 다른 에이전트를 만들면 안 됩니다.
- 기다릴 때는 `hc wait --children any`를 쓰고 sleep이나 폴링은 하지 마세요. 작업자가 막히면(권한 창, Codex 샌드박스가 docker를 막는 경우 등)
  `hc read`로 화면을 확인하고, 직접 풀 수 없으면 사용자에게 무엇이 필요한지 알리세요. 보고만 믿지 말고 실제 결과(diff, 테스트 출력)를 확인하세요.

## 테스트 환경 (반드시 지킬 것)
- 호스트는 Windows입니다. 빌드와 테스트는 Linux 컨테이너에서만 합니다:
  `bash scripts/dev.sh make test`, `bash scripts/dev.sh make asan`, `bash scripts/dev.sh bash scripts/functional_test.sh`.
  (`functional_test.sh`는 자체 임시 폴더와 컨테이너 네트워크를 써서 안전합니다.)
- **`scripts/lb.sh`, `bp_demo.sh`, `mini-web`은 저장소를 마운트한 컨테이너에서 절대 실행하지 마세요.**
  사용자의 lab 컨테이너 `mini-lab`이 저장소의 `.lb/`(pid 파일, 로그, 백엔드 목록)를 쓰고 있어서, 그걸 지우거나 덮어쓰게 됩니다.
  복사본에서 실행하세요:
  `MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W):/ro:ro" mini-webserver-tools bash -c 'mkdir /work && cd /ro && tar --exclude=./.lb --exclude=./build --exclude=./webserver -cf - . | tar -xf - -C /work && cd /work && <명령>'`
- `mini-lab` 컨테이너와 그 안의 프로세스는 건드리지 마세요. 사용자가 보고 있는 데모입니다.

## 금지 사항
- 커밋, 푸시, 브랜치 변경, `git reset/checkout/stash/clean` 같은 파괴적 git 명령. 사용자의 커밋 안 된 변경(`git status`)은 되돌리지 않습니다.
- `~/.config/herdr-app`, `~/.config/herdr-core` 수정.
- 맡은 범위 밖 파일 수정.

## 끝나면 사용자에게 보고
바뀐 파일과 설계 요약, 0번 검증 결과, 교차 리뷰에서 나온 지적과 처리 결과, 전체 테스트 출력 요약(통과/실패 수 포함),
남은 위험이나 하지 않은 것. 그리고 lab 반영 방법: `mini-lab`의 front는 옛 바이너리로 돌고 있으니,
새 lb.sh로 다시 띄우는 절차를 안내하세요. 실행은 사용자가 원할 때만 합니다.
