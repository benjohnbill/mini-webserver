#!/usr/bin/env python3
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import math
import signal
import time


def main():
    parser = argparse.ArgumentParser(description="Serve GET requests with a fixed delay.")
    parser.add_argument("port", type=int)
    parser.add_argument("name")
    parser.add_argument("delay_ms", type=float)
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("PORT must be between 1 and 65535")
    if not math.isfinite(args.delay_ms) or args.delay_ms < 0:
        parser.error("DELAY_MS must be finite and nonnegative")
    if not args.name or any(not 32 <= ord(char) < 127 for char in args.name):
        parser.error("NAME must use printable ASCII characters")

    body = f"{args.name}\n".encode()

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):
            time.sleep(args.delay_ms / 1000)
            self.close_connection = True
            try:
                self.send_response(200)
                self.send_header("X-Server-Name", args.name)
                self.send_header("Content-Type", "text/plain; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(body)
            except ConnectionError:
                pass

        def log_message(self, format, *args):
            pass

    def stop(signum, frame):
        raise SystemExit(0)

    signal.signal(signal.SIGTERM, stop)
    with ThreadingHTTPServer(("0.0.0.0", args.port), Handler) as server:
        server.serve_forever()


if __name__ == "__main__":
    main()
