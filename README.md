<!-- readme: archetype=team -->
# mini-webserver

외부 HTTP 라이브러리 없이 POSIX 소켓 API만으로 C 웹 서버를 만들고, Forward Proxy 또는 Reverse Proxy를 함께 구현합니다.

Status: active

## 실행

```sh
make
./server 8080 ./public
```

기본 바인드 주소는 `127.0.0.1`입니다. 다른 IPv4 주소에서 연결을 받으려면
`-b 0.0.0.0`처럼 포트 앞에 바인드 주소를 지정합니다.

```sh
curl -i http://127.0.0.1:8080/
curl -i -H 'Content-Type: text/plain' --data-binary 'hello' http://127.0.0.1:8080/echo
```

`/`는 정적 루트의 `index.html`을 반환합니다. `POST /echo`는 요청 본문을
바이트 그대로 반환합니다.

## Reverse Proxy

백엔드 두 개를 각각 `8001`, `8002`에서 시작한 뒤 서버를 실행합니다.

```sh
./server 8080 ./public /api=127.0.0.1:8001 /assets=127.0.0.1:8002
curl -i http://127.0.0.1:8080/api/users
curl -i http://127.0.0.1:8080/assets/logo.png
```

라우팅 규칙은 경로 요소 단위로 매칭합니다. `/api`와 `/api/users`는
`/api` 규칙에 매칭되고, `/apiary`는 정적 파일 서빙으로 넘어갑니다.
여러 규칙이 매칭되면 가장 긴 경로를 선택합니다. 백엔드에는 원래 경로와
쿼리를 유지하고, `Host`는 백엔드 주소로 설정합니다. `/echo`는 내장 핸들러가
처리합니다.

## Map (파일 지도)

- `src/main.c`: TCP 연결 수락, 연결 큐와 pthread 워커, 종료 처리
- `src/net.c`: 소켓 생성·연결, 타임아웃, 바이트 송수신
- `src/http.c`: 요청과 백엔드 응답 파싱, HTTP 응답 헤더 생성
- `src/handlers.c`: MIME 매핑, 정적 파일 서빙, echo, 라우팅과 프록시 전달

워크 큐는 소켓 64개를 담고, 워커 4개가 연결을 처리합니다. 큐가 가득 차면
새로 수락한 연결을 닫습니다. 요청 수신과 백엔드 통신에는 각각 5초 제한을
둡니다. SIGINT 또는 SIGTERM을 받으면 새 연결 받기를 멈추고 큐의 대기 연결을
닫은 뒤 워커가 처리 중인 연결을 정리할 때까지 기다립니다.

연결당 요청 한 건을 처리하며 `Connection: close`로 종료합니다. 요청 헤더
16 KiB, 요청 대상 4,096바이트, 본문 1 MiB까지 지원합니다. 전송 코딩 없는
요청과 백엔드 응답을 지원하며, 백엔드 응답 본문은 `Content-Length` 또는
연결 종료를 기준으로 중계합니다.

## 테스트

```sh
make test       # C 단위 테스트와 Python 3 통합 테스트
make sanitize   # AddressSanitizer와 UndefinedBehaviorSanitizer
ab -n 2000 -c 16 http://127.0.0.1:8080/
```

## 다음에 읽을 것

- 과제 명세: Krafton Jungle LMS `learning/1309` (웹 서버 만들기)

Agents: if a path named here does not exist, report it to the owner.

## 작업 방식

- 각 팀원은 자기 이름의 개인 브랜치에서 구현합니다.
- `main`에는 직접 push하지 않습니다. PR을 열고 merge commit으로 합칩니다.
