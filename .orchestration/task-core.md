# impl-core (Codex) 작업
먼저 `.orchestration/rules.md`, `.orchestration/brief.md`(배경과 요구사항 1~6), `.orchestration/contract.md`를 끝까지 읽으세요.

범위: 계약 2~3절의 C 코어 구현. **수정 가능: `src/`, `include/`만.** tests/, scripts/, README.md는 다른 작업자(impl-tools) 담당이니 건드리지 마세요.
- 파일 route 파싱(`=@path`), `route_table_reload`, 참조 카운트(`backend.refs`, `route_table.refs/heap`), `route_live`,
  워커/헬스/status의 acquire/release, SIGHUP 처리(main 스레드에서 reload), stderr `reload: ok` / `reload: failed: <이유>`.
- 데이터 레이스와 use-after-free가 없어야 합니다. 바뀐 이유가 코드로 안 보이는 곳만 한 줄 주석.
- 참고: 기존 tests/는 `backends[i].host`처럼 값 접근을 하므로 당신 변경 후 컴파일이 깨질 수 있습니다. impl-tools가 계약대로 고칩니다.
  당신은 `bash scripts/dev.sh make all`로 빌드 경고 0을 확인하고, 가능하면 스스로 짧은 확인(컨테이너에서 front를 파일 route로 띄우고 HUP)을 해 보세요.
  docker가 샌드박스에 막히면 조율자에게 알리세요.
- 끝나면 `.orchestration/impl-core.md`에 바뀐 파일, 설계 요약, 확인한 것/못한 것을 쓰고 `hc send t2q "impl-core done: .orchestration/impl-core.md"`.
