---
status: proposed
branch: CJG
decided: 2026-11-21
---

# Our users are the verification clients of the LMS statement

| Item | Content |
|---|---|
| Context | Each design decision of the server (the Concurrency model, the timeouts, the size limits) needs a reason. The owner wanted that reason to come from an analysis of what the users expect from the server (user said 2026-11-21). The LMS statement (`learning/1309`) does not name the users of the server. It names only the clients that test the server. |
| Decision | Our users are the Verification clients of the LMS statement: curl and a browser ("기능 테스트"), a load tool ("부하 테스트 — ab, wrk, hey"), and a sender of the invalid requests in the edge case list ("불완전한 헤더", "매우 긴 URI", "갑작스러운 연결 종료"). We tell clients apart by the bytes that they send, not by the tool. A protocol that is not HTTP/1.0 or HTTP/1.1 (HTTP/2, gRPC, WebSocket, RESP) is out of scope and gets an error status (400 or 505). |
| Reasons | The owner chose to base the users on the LMS requirements: "LMS에 적힌 요구사항에 근거해서, 우리 서버의 클라이언트와 기대치를 생각하는게 좋아보여" (user said 2026-11-21). Each requirement that follows from this decision can cite one LMS sentence. |
| Rejected options | We define our own user, for example a visitor of a team portfolio site. That gives a stronger story, but each requirement then rests on our guess, not on the statement. |
| Consequences | The expectations come from the statement. A browser and curl expect correct responses. A load tool expects throughput that it can measure as the count of connections goes up. An invalid sender must not stop the service for the other clients. The last expectation makes the Stuck connection a central problem: an incomplete header in a blocking `read()` waits forever, the same as a slowloris attack. |
