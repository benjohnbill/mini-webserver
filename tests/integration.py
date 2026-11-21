import concurrent.futures
import contextlib
import http.client
import http.server
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time


HOST = "127.0.0.1"


class Backend(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        self.respond()

    def do_POST(self):
        self.respond()

    def respond(self):
        if self.path.endswith("/timeout"):
            time.sleep(6)
            return

        status = 404 if self.path.endswith("/missing") else 200
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        host = self.headers["Host"]
        removed = self.headers.get("X-Local", "<none>")
        payload = f"{self.server.label} {self.command} {self.path} {host} {removed}\n".encode()
        payload += body
        self.send_response(status)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Connection", "X-Backend-Local")
        self.send_header("X-Backend-Local", "not forwarded")
        self.send_header("X-Backend", self.server.label)
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, format, *args):
        pass


@contextlib.contextmanager
def running_backend(label):
    with http.server.ThreadingHTTPServer((HOST, 0), Backend) as backend:
        backend.label = label
        thread = threading.Thread(target=backend.serve_forever)
        thread.start()
        try:
            yield backend.server_port
        finally:
            backend.shutdown()
            thread.join()


@contextlib.contextmanager
def running_server(binary, root, routes, stop_signal):
    with socket.socket() as reservation:
        reservation.bind((HOST, 0))
        port = reservation.getsockname()[1]
    process = subprocess.Popen(
        [binary, str(port), str(root), *routes],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    try:
        end = time.monotonic() + 3
        while True:
            if process.poll() is not None:
                raise AssertionError(process.stderr.read().decode())
            try:
                with socket.create_connection((HOST, port), timeout=0.2):
                    break
            except OSError:
                if time.monotonic() >= end:
                    raise
                time.sleep(0.02)
        yield port
    finally:
        if process.poll() is None:
            process.send_signal(stop_signal)
        try:
            code = process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise
        with process.stderr:
            errors = process.stderr.read().decode()
        assert code == 0 and not errors, (code, errors)


def request(port, method, path, body=None, headers=None):
    connection = http.client.HTTPConnection(HOST, port, timeout=7)
    try:
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        return response.status, dict(response.getheaders()), response.read()
    finally:
        connection.close()


def check_static_and_echo(port):
    status, headers, body = request(port, "GET", "/")
    assert (status, body) == (200, b"<h1>mini</h1>")
    assert headers["Content-Type"].startswith("text/html")
    status, headers, body = request(port, "GET", "/style.css?v=2")
    assert (status, body) == (200, b"body{}\n")
    assert headers["Content-Type"].startswith("text/css")
    assert int(headers["Content-Length"]) == len(body)
    assert request(port, "GET", "/nested/image.PNG")[2] == bytes(range(256)) * 128
    assert request(port, "GET", "/empty.txt")[2] == b""
    assert request(port, "GET", "/missing")[0] == 404
    assert request(port, "GET", "/linked.txt")[0] == 403
    assert request(port, "GET", "/nested")[0] == 403
    assert request(port, "GET", "/bad%zz")[0] == 400
    assert request(port, "GET", "/" + "a" * 4097)[0] == 414
    assert request(port, "GET", "/", headers={"X-Long": "a" * 16384})[0] == 431
    status, headers, _ = request(port, "GET", "/echo")
    assert status == 405 and headers["Allow"] == "POST"

    payload = b"abc\x00def"
    status, headers, body = request(port, "POST", "/echo?mode=raw", payload)
    assert status == 200 and body == payload
    assert int(headers["Content-Length"]) == len(payload)


def check_concurrency(port):
    with socket.create_connection((HOST, port), timeout=2) as slow:
        slow.sendall(b"POST /echo HTTP/1.1\r\nHost: local\r\nContent-Len")
        slow.sendall(b"gth: 5\r\n\r\nhe")
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
            responses = pool.map(lambda _: request(port, "GET", "/")[0], range(40))
            assert all(status == 200 for status in responses)
        slow.sendall(b"llo")
        with http.client.HTTPResponse(slow) as response:
            response.begin()
            assert response.status == 200 and response.read() == b"hello"

    with socket.create_connection((HOST, port), timeout=2) as interrupted:
        interrupted.sendall(b"POST /echo HTTP/1.1\r\nHost: local\r\nContent-Length: 20\r\n\r\na")
    assert request(port, "GET", "/")[0] == 200


def check_proxy(port, api, assets):
    status, headers, body = request(
        port,
        "POST",
        "/api/echo?lang=ko",
        b"a\x00b",
        {"Connection": "X-Local", "X-Local": "private"},
    )
    assert status == 200 and "X-Backend-Local" not in headers
    assert body == f"api POST /api/echo?lang=ko {HOST}:{api} <none>\n".encode() + b"a\x00b"
    status, headers, body = request(port, "GET", "/assets/file")
    assert status == 200 and headers["X-Backend"] == "assets"
    assert body.startswith(f"assets GET /assets/file {HOST}:{assets}".encode())
    assert request(port, "GET", "/apiary")[0] == 404
    assert request(port, "GET", "/api/missing")[0] == 404
    offline_status = request(port, "GET", "/offline/test")[0]
    assert offline_status == 502, offline_status
    assert request(port, "GET", "/api/timeout")[0] == 504
    assert request(port, "GET", "/")[0] == 200


def main():
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory) / "public"
        root.mkdir()
        (root / "index.html").write_bytes(b"<h1>mini</h1>")
        (root / "style.css").write_bytes(b"body{}\n")
        (root / "empty.txt").touch()
        (root / "nested").mkdir()
        (root / "nested" / "image.PNG").write_bytes(bytes(range(256)) * 128)
        outside = Path(directory) / "outside.txt"
        outside.write_bytes(b"outside")
        (root / "linked.txt").symlink_to(outside)
        with running_backend("api") as api, running_backend("assets") as assets:
            with socket.socket() as offline:
                offline.bind((HOST, 0))
                offline_port = offline.getsockname()[1]
            routes = [
                f"/api={HOST}:{api}",
                f"/assets={HOST}:{assets}",
                f"/offline={HOST}:{offline_port}",
            ]
            with running_server(binary, root, routes, signal.SIGTERM) as port:
                check_static_and_echo(port)
                check_concurrency(port)
                check_proxy(port, api, assets)
            with running_server(binary, root, [], signal.SIGINT) as port:
                assert request(port, "GET", "/")[0] == 200
    print("Integration tests passed")


if __name__ == "__main__":
    main()
