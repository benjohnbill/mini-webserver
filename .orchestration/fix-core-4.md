# impl-core 수정 4차 (.orchestration/fresh-review-2.md 사소 1, 조율자 확인)
main.c의 `signal(SIGHUP, SIG_IGN)`은 시작 중 온 HUP을 버립니다(펜딩 안 됨). 대신:
- main 맨 앞에서 SIGHUP을 **블록**(sigprocmask/pthread_sigmask)합니다.
- server_run은 지금처럼 on_reload 설치 → 워커/헬스 스레드 생성(블록 상속) 후 마스크 복원 시, `old`에 SIGHUP이 들어 있으므로 main 스레드에서 SIGHUP을 명시적으로 **언블록**하세요. 그러면 펜딩된 HUP이 즉시 핸들러로 와서 첫 루프에서 reload됩니다.
- 시작 실패로 server_run이 일찍 끝나는 경로는 그대로 종료하므로 상관없음. 주석은 이유 한 줄.
docker 금지, src/만. 끝나면 impl-core.md에 "수정 4차" 추가 후 hc send t2q.
