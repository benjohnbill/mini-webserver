# impl-core 수정 3차 (문맥 없는 리뷰 .orchestration/fresh-review.md, 조율자 확인)
1. 시작 중 SIGHUP으로 죽는 창: `src/main.c` 맨 앞(config_parse_args 전)에 SIGHUP을 SIG_IGN으로 두고, server_run이 기존대로 on_reload를 설치하게 하세요. 이유 한 줄 주석.
2. route.c의 재사용 판정(prev 테이블만 봄) 위치에 `shortcut: 제거 직후 다시 넣으면 이전 테이블을 쥔 요청이 남은 동안 같은 host:port 객체가 둘(inflight 분리), 문제되면 전역 backend 레지스트리로` 형태의 한 줄 주석만 추가. 동작 변경 없음.
docker 금지, src/ include/만. 끝나면 impl-core.md 끝에 "수정 3차" 절 추가 후 hc send t2q.
