"""Run the README demo against the C proxy and two local HTTP origins."""
from pathlib import Path
import signal

from integration import HOST, request, running_backend, running_server

ROOT = Path(__file__).resolve().parents[1]


def main():
    with running_backend("A") as a, running_backend("B") as b:
        group = f"/shared@ketama={HOST}:{a.server_port},{HOST}:{b.server_port}"
        with running_server(str(ROOT / "server"), ROOT / "public", [group],
                            signal.SIGINT, options=["-c", "-r", "1", "-B", "2"]) as port:
            print("요청                 HTTP  응답 출처          백엔드 누적 A/B")
            for path in ["/", "/style.css", "/shared/normal", "/shared/normal", "/shared/normal"]:
                status, headers, _ = request(port, "GET", path)
                if status == 429:
                    source = "요청 제한"
                elif "Age" in headers:
                    source = "프록시 캐시"
                else:
                    source = headers.get("X-Backend", "로컬 파일")
                counts = [sum(origin.requests.values()) for origin in (a, b)]
                print(f"{path:20} {status}   {source:12} {counts[0]}/{counts[1]}")


if __name__ == "__main__":
    main()
