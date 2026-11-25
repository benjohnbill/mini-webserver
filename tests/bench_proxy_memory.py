"""Compare two local proxy binaries under identical, gated uploads (macOS/Linux)."""

import argparse
import csv
import hashlib
import http.client
import http.server
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
from concurrent.futures import ThreadPoolExecutor


class Backend(http.server.ThreadingHTTPServer):
    request_queue_size = 128

    def server_bind(self):
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 32 * 1024)
        super().server_bind()


class Upload(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        self.connection.settimeout(5)
        self.server.arrivals.wait()
        self.server.release.wait(5)
        body = self.rfile.read(int(self.headers["Content-Length"]))
        digest = hashlib.sha256(body).digest()
        self.send_response(200)
        self.send_header("Content-Length", str(len(digest)))
        self.end_headers()
        self.wfile.write(digest)

    def log_message(self, *args):
        pass


def upload(port, body, digest):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=8)
    try:
        conn.request("POST", "/api/upload", body)
        response = conn.getresponse()
        if response.status != 200 or response.read() != digest:
            raise RuntimeError("upload status or body checksum mismatch")
    finally:
        conn.close()


def wait_ready(port):
    deadline = time.monotonic() + 3
    while True:
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=1)
        try:
            conn.request("GET", "/warmup")
            response = conn.getresponse()
            response.read()
            if response.status != 404:
                raise RuntimeError("unexpected warmup response")
            return
        except ConnectionRefusedError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.01)
        finally:
            conn.close()


def rss_kib(pid):
    return int(subprocess.check_output(["ps", "-o", "rss=", "-p", str(pid)]))


def measure(binary, model, count, body, root):
    digest = hashlib.sha256(body).digest()
    with Backend(("127.0.0.1", 0), Upload) as backend:
        backend.arrivals = threading.Barrier(count + 1, timeout=4)
        backend.release = threading.Event()
        thread = threading.Thread(target=backend.serve_forever, args=(0.02,))
        thread.start()
        try:
            with socket.socket() as reserve:
                reserve.bind(("127.0.0.1", 0))
                port = reserve.getsockname()[1]
            route = f"/api=127.0.0.1:{backend.server_port}"
            proxy = subprocess.Popen([binary, "-m", model, str(port), root, route],
                                     stdout=subprocess.DEVNULL)
            try:
                wait_ready(port)
                idle = rss_kib(proxy.pid)
                with ThreadPoolExecutor(max_workers=count) as clients:
                    futures = [clients.submit(upload, port, body, digest) for _ in range(count)]
                    try:
                        backend.arrivals.wait()
                        time.sleep(0.25)
                        paused = rss_kib(proxy.pid)
                    finally:
                        backend.release.set()
                    for future in futures:
                        future.result()
            finally:
                proxy.terminate()
                _, status, usage = os.wait4(proxy.pid, 0)
                proxy.returncode = os.waitstatus_to_exitcode(status)
            if proxy.returncode != 0:
                raise RuntimeError(f"proxy exited with status {proxy.returncode}")
            peak = usage.ru_maxrss // 1024 if sys.platform == "darwin" else usage.ru_maxrss
            return idle, paused, peak
        finally:
            backend.shutdown()
            thread.join()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("--repeats must be positive")
    variants = [("before", args.before.resolve()), ("after", args.after.resolve())]
    cases = [("poll", n, 1024 * 1024) for n in (1, 4, 16, 64)]
    cases.extend([("pthread", 4, 1024 * 1024), ("poll", 64, 4096)])
    out = csv.writer(sys.stdout, lineterminator="\n")
    out.writerow(["trial", "model", "uploads", "body_bytes", "variant",
                  "idle_rss_kib", "paused_rss_kib", "peak_rss_kib"])
    with tempfile.TemporaryDirectory() as root:
        for trial in range(1, args.repeats + 1):
            for model, count, size in cases:
                body = bytes(range(256)) * (size // 256)
                for label, binary in (variants if trial % 2 else reversed(variants)):
                    metrics = measure(binary, model, count, body, root)
                    out.writerow([trial, model, count, size, label, *metrics])
                    sys.stdout.flush()


if __name__ == "__main__":
    main()
