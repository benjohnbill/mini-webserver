# impl-core 수정 요청 (조율자 확인 완료, review-core.md 근거)
1. reload 실패 이유에 어느 파일인지 넣기: `reload: failed: <file>: <이유>` (접두사 `reload: failed: `는 유지).
2. parse_backend: 포트를 숫자로 파싱한 뒤 정규 10진 문자열(앞자리 0 없음)로 저장해서 `:08001`과 `:8001`이 같은 백엔드로 판정되게(중복 검사와 reload 재사용 모두).
docker 실행 금지. src/ include/만 수정. 끝나면 .orchestration/impl-core.md 끝에 "수정 2차" 절을 추가하고 hc send t2q.
