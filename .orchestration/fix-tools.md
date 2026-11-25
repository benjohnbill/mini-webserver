# impl-tools 수정 요청 (조율자 확인 완료, review-tools.md 근거)
1. remove: route/멤버 파일 갱신 실패(save_members)나 HUP 실패, 반영 확인 실패 시 백엔드를 stop하지 말고 멤버 목록도 원래대로 두고 rc=1. add도 save_members 실패를 처리.
2. remove 경로도 `make -s all || exit 1`을 먼저 해서, 안전장치(refront) 분기에서 옛 바이너리로 `=@` 기동에 실패하지 않게.
3. remove에서 반영 확인 후 stop 전 유예: 이전 테이블을 잡은 요청이 끝나도록 최대 LB_TIMEOUT ms까지(front -T가 상한) 기다린 뒤 stop. 단순하게 해도 됨(예: slow 백엔드일 때만 LB_TIMEOUT 대기, 일반 webserver는 TERM 시 진행 중 요청을 마치므로 그대로). tools/는 수정하지 말 것.
4. functional_test 부하 검사: 부하가 도는 동안 최소 1회 `reload: ok` 증가와 달라진 목록을 확인(부하 프로세스 종료 전).
5. 반영 대기: lb.sh와 functional_test 모두 전체 마감 시각(lb.sh 약 2초, 테스트 1초)을 정하고 각 curl의 -m을 남은 시간으로 제한.
6. 파일 삭제 테스트도 `reload: failed:` 줄 증가를 기다린 뒤 목록 유지를 검사.
7. 출력 형태 유지: `front reloaded (pid N, :8080, webserver)`처럼 기존 `front started` 줄의 필드 유지.
끝나면 변경한 테스트를 다시 돌려(make test, functional_test, lb.sh 복사본 검증) .orchestration/impl-tools.md 끝에 "수정 2차" 절로 추가하고 hc send t2q.
