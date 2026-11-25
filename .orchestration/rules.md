# 모든 작업자 공통 규칙
- 조율자 터미널: t2q. 보고: `hc send t2q "<짧은 요약>"`. 긴 내용은 `.orchestration/<당신 이름>.md`에 쓰고 그 경로만 보냅니다.
- 계약: `.orchestration/contract.md`. 바꿔야 하면 구현 전에 조율자에게 먼저 알립니다.
- hc로 독립 에이전트를 더 띄우지 마세요(`hc run` 금지). 자기 범위 안에서 조사·검색용 내부 서브에이전트는 허용합니다.
- 사용자 화면 포커스를 가져가지 마세요: terminal.keep, terminal.peek, agent.focus, pane.focus, tab.select, space.select 금지.
  herdr-app do tab.new / space.new는 쓰지 마세요(꼭 필요하면 `--focus false`).
- 금지: 커밋, 푸시, 브랜치 변경, `git reset/checkout/stash/clean/restore` 등 파괴적 git 명령. 사용자의 커밋 안 된 변경을 되돌리지 않습니다.
- 금지: `~/.config/herdr-app`, `~/.config/herdr-core` 수정. 맡은 범위 밖 파일 수정.
- 빌드/테스트는 Linux 컨테이너에서만: `bash scripts/dev.sh make test`, `bash scripts/dev.sh make asan`,
  `bash scripts/dev.sh bash scripts/functional_test.sh`.
- `scripts/lb.sh`, `bp_demo.sh`, `mini-web`은 저장소를 마운트한 컨테이너에서 절대 실행 금지(사용자 lab `mini-lab`이 `.lb/`를 씁니다).
  반드시 복사본에서:
  MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W):/ro:ro" mini-webserver-tools bash -c 'mkdir /work && cd /ro && tar --exclude=./.lb --exclude=./build --exclude=./webserver -cf - . | tar -xf - -C /work && cd /work && <명령>'
- `mini-lab` 컨테이너와 그 안의 프로세스는 건드리지 마세요.
- 한국어로 보고합니다.
