<!-- readme: archetype=team -->
# mini-webserver

외부 HTTP 라이브러리 없이 POSIX 소켓 API만으로 C 웹 서버를 만들고, Forward Proxy와 Reverse Proxy를 함께 구현합니다.

Status: active

## 실행

Linux(또는 WSL2, Docker)에서 빌드합니다. Windows 호스트에서는 `scripts/dev.sh`가 Ubuntu 컨테이너 안에서 명령을 실행합니다.

```sh
make                      # ./webserver 빌드
make test                 # 단위 테스트 (파서, MIME, 라우팅, 프록시)
make asan                 # AddressSanitizer + LeakSanitizer 빌드로 테스트
bash scripts/functional_test.sh   # curl / nc / ab 기능·부하 테스트
# Windows: bash scripts/dev.sh make test
```

```sh
# 백엔드 두 대
./webserver -p 8001 -n b1 &
./webserver -p 8002 -n b2 &
# 프론트: 정적 파일 + CGI + /echo Round-Robin Reverse Proxy + Forward Proxy
./webserver -p 8080 -r www -c cgi-bin -F -R '/echo=127.0.0.1:8001,127.0.0.1:8002'
curl -i localhost:8080/echo                                  # X-Server-Name: b1, b2 교대
curl -x http://127.0.0.1:8080 http://127.0.0.1:8001/          # Forward Proxy
```

옵션: `-p` 포트, `-r` 정적 루트, `-c` CGI 디렉터리, `-t` 워커 스레드 수, `-q` 대기 큐 크기, `-F` Forward Proxy 허용, `-T` 프록시 타임아웃(ms), `-k` keep-alive 타임아웃(ms), `-n` 서버 이름(`X-Server-Name`), `-R` 라우팅 규칙(반복 가능), `-h` 도움말.

## 구조

- 동시성: accept 스레드 하나와 pthread 스레드 풀, 크기가 정해진 연결 큐. 큐가 가득 차면 503을 응답합니다.
- 요청 처리 순서: 절대 URI는 Forward Proxy, 라우팅 규칙과 맞는 경로는 Reverse Proxy, `/cgi-bin/`은 CGI, `/echo`는 내장 동적 응답, 그 밖은 정적 파일입니다.
- 종료: `SIGINT`/`SIGTERM`을 받으면 accept를 멈추고 워커를 join합니다. `SIGPIPE`는 무시합니다.

## Map (파일 지도)

- `README.md` (entry): 이 문서
- `include/`: 모듈 간 계약 헤더 (`http.h`, `route.h`, `proxy.h`, `server.h`, `cgi.h`, `server_run.h`)
- `src/http.c`: 요청 파싱(요청 라인, 헤더, Content-Length 본문, keep-alive, 파이프라이닝)과 응답 생성
- `src/server.c`, `src/main.c`: 소켓, 스레드 풀, 요청 분배, 정적 파일, 시그널 처리
- `src/cgi.c`: CGI/1.1 실행 (fork/execve, 타임아웃, 프로세스 그룹 정리)
- `src/route.c`: 최장 접두사 라우팅과 원자적 Round-Robin
- `src/proxy.c`: Reverse/Forward Proxy, hop-by-hop 헤더 제거, X-Forwarded-For, 502/504
- `src/config.c`, `src/mime.c`: CLI 인자 처리, MIME 타입 매핑
- `tests/`: 단위 테스트, `scripts/functional_test.sh`: 기능·엣지 케이스·부하 테스트
- `www/`, `cgi-bin/`: 데모 정적 파일과 CGI 스크립트
- `tools/Dockerfile`, `scripts/dev.sh`: 빌드·테스트용 컨테이너

## 다음에 읽을 것

- 과제 명세: Krafton Jungle LMS `learning/1309` (웹 서버 만들기)

Agents: if a path named here does not exist, report it to the owner.

## 작업 방식

- 각 팀원은 자기 이름의 개인 브랜치에서 구현합니다.
- `main`에는 직접 push하지 않습니다. PR을 열고 merge commit으로 합칩니다.
