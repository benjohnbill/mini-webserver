import collections
import concurrent.futures
import hashlib
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
        with self.server.gate_condition:
            self.server.requests[self.path] += 1
        if self.path.startswith("/shared/"):
            self.serve_cache_fixture()
            return
        if self.path.endswith("/timeout"):
            time.sleep(6)
            return

        if self.path == "/api/gate":
            with self.server.gate_condition:
                self.server.arrivals += 1
                self.server.gate_condition.notify_all()
            if not self.server.gate.wait(timeout=4):
                return
        if self.path == "/api/interim":
            self.wfile.write(b"HTTP/1.1 103 Early Hints\r\n\r\n"
                             b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nclose-delimited")
            self.close_connection = True
            return
        if self.path == "/api/upload":
            length = int(self.headers["Content-Length"])
            first = self.rfile.read(3)
            self.server.upload_started.set()
            body = first + self.rfile.read(length - len(first))
            if len(body) < length:
                self.server.upload_aborted.set()
                self.close_connection = True
                return
            self.send_response(200)
            self.send_header("Content-Length", str(length))
            self.end_headers()
            self.wfile.write(body)
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

    def serve_cache_fixture(self):
        path = self.path.partition("?")[0]
        self.rfile.read(int(self.headers.get("Content-Length", "0")))
        payload = bytes(range(256)) * (33 if path.endswith("/large") else 32)
        self.send_response(200)
        controls = {"private": "private, max-age=60", "nostore": "public, max-age=60, no-store",
                    "short": "public, max-age=1", "normal": "public, max-age=60"}
        kind = path.rsplit("/", 1)[-1]
        self.send_header("Cache-Control", controls.get(kind, controls["normal"]))
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("X-Backend", self.server.label)
        if kind == "cookie":
            self.send_header("Set-Cookie", "a=b")
        if kind == "vary":
            self.send_header("Vary", "Accept")
        self.end_headers()
        if kind == "incomplete":
            self.wfile.write(payload[:17])
            self.close_connection = True
            return
        self.wfile.write(payload[:17])
        self.wfile.flush()
        time.sleep(0.01)
        self.wfile.write(payload[17:])

    def log_message(self, format, *args):
        pass


@contextlib.contextmanager
def running_backend(label):
    with http.server.ThreadingHTTPServer((HOST, 0), Backend) as backend:
        backend.label = label
        backend.requests = collections.Counter()
        backend.gate = threading.Event()
        backend.gate_condition = threading.Condition()
        backend.arrivals = 0
        backend.upload_started = threading.Event()
        backend.upload_aborted = threading.Event()
        thread = threading.Thread(target=backend.serve_forever)
        thread.start()
        try:
            yield backend
        finally:
            backend.shutdown()
            thread.join()


@contextlib.contextmanager
def running_server(binary, root, routes, stop_signal, model=None, options=()):
    with socket.socket() as reservation:
        reservation.bind((HOST, 0))
        port = reservation.getsockname()[1]
    process = subprocess.Popen(
        [binary, *(["-m", model] if model else []), *options, str(port), str(root), *routes],
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


def check_incomplete_request(port):
    with socket.create_connection((HOST, port), timeout=2) as client:
        client.sendall(b"POST /echo HTTP/1.1\r\nHost: local\r\nContent-Length: 10\r\n\r\nabc")
        client.shutdown(socket.SHUT_WR)
        with http.client.HTTPResponse(client) as response:
            response.begin()
            assert response.status == 400


def check_slow_reader(port):
    # A client pauses its download while another connection fetches a small file.
    with socket.socket() as slow:
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8192)
        slow.settimeout(3)
        slow.connect((HOST, port))
        slow.sendall(b"GET /large.bin HTTP/1.0\r\n\r\n")
        with http.client.HTTPResponse(slow) as response:
            response.begin()
            assert response.status == 200
            assert response.read(16) == b"x" * 16
            assert request(port, "GET", "/")[0] == 200
            assert response.read() == b"x" * (4 * 1024 * 1024 - 16)


def check_worker_capacity(port, backend, model):
    # The backend acknowledges four normal requests, then waits for us to release them.
    with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
        slow = [pool.submit(request, port, "GET", "/api/gate") for _ in range(4)]
        try:
            with backend.gate_condition:
                assert backend.gate_condition.wait_for(lambda: backend.arrivals == 4, timeout=3)
            fast = pool.submit(request, port, "GET", "/")
            if model == "pthread":
                try:
                    fast.result(timeout=0.25)
                except concurrent.futures.TimeoutError:
                    pass
                else:
                    raise AssertionError("a fifth request ran while all four workers were busy")
            else:
                assert fast.result(timeout=2)[0] == 200
        finally:
            backend.gate.set()
        assert all(f.result()[0] == 200 for f in slow)
        assert fast.result()[0] == 200


def check_streamed_upload(port, backend):
    payload = bytes(range(256)) * 4096
    head = f"POST /api/upload HTTP/1.1\r\nHost: local\r\nContent-Length: {len(payload)}\r\n\r\n".encode()
    with socket.create_connection((HOST, port), timeout=3) as client:
        client.sendall(head + payload[:3])
        # The backend must receive the prefix before the client sends the rest.
        assert backend.upload_started.wait(timeout=2)
        client.sendall(payload[3:])
        with http.client.HTTPResponse(client) as response:
            response.begin()
            assert response.status == 200 and response.read() == payload

    backend.upload_started.clear()
    with socket.create_connection((HOST, port), timeout=3) as client:
        client.sendall(head + payload[:3])
        assert backend.upload_started.wait(timeout=2)
        client.shutdown(socket.SHUT_WR)
        with http.client.HTTPResponse(client) as response:
            response.begin()
            assert response.status == 400
        assert backend.upload_aborted.wait(timeout=2)


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
    assert request(port, "GET", "/api/interim")[2] == b"close-delimited"
    offline_status = request(port, "GET", "/offline/test")[0]
    assert offline_status == 502, offline_status
    assert request(port, "GET", "/api/timeout")[0] == 504
    assert request(port, "GET", "/")[0] == 200


def check_proxy_policy(binary, root, model, one, two):
    ports = [one.server_port, two.server_port]
    ring = []
    for port in ports:
        for replica in range(40):
            digest = hashlib.md5(f"{HOST}:{port}-{replica}".encode()).digest()
            for offset in range(0, 16, 4):
                ring.append((int.from_bytes(digest[offset:offset+4], "little"), port))
    ring.sort()
    key = int.from_bytes(hashlib.md5(HOST.encode()).digest()[:4], "little")
    selected = next((port for point, port in ring if point >= key), ring[0][1])
    origin = one if selected == ports[0] else two
    group = f"/shared@ketama={HOST}:{ports[0]},{HOST}:{ports[1]}"
    def calls(path):
        return sum(b.requests[path] for b in (one, two))

    with running_server(binary, root, [group], signal.SIGTERM, model, ["-c"]) as front:
        payload = bytes(range(256)) * 32
        path = "/shared/normal?q=1"
        for i in range(3):
            status, headers, body = request(front, "GET", path)
            assert (status, headers["X-Backend"], body) == (200, origin.label, payload)
            assert ("Age" in headers) == (i > 0)
        assert calls(path) == 1
        assert request(front, "GET", "/shared/normal?q=2")[2] == payload
        assert calls("/shared/normal?q=2") == 1
        for headers in ({"Cookie": "session=example"}, {"Authorization": "example"},
                        {"Cache-Control": "no-cache"}, {"Range": "bytes=0-4"}):
            assert "Age" not in request(front, "GET", path, headers=headers)[1]
        assert calls(path) == 5
        assert request(front, "POST", path, b"update")[0] == 200
        assert "Age" not in request(front, "GET", path)[1]
        assert calls(path) == 7
        for kind in ("private", "nostore", "cookie", "vary", "large"):
            target = "/shared/" + kind
            for _ in range(2):
                assert "Age" not in request(front, "GET", target)[1]
            assert calls(target) == 2
        for _ in range(2):
            try:
                request(front, "GET", "/shared/incomplete")
            except http.client.IncompleteRead as error:
                assert len(error.partial) == 17
            else:
                raise AssertionError("the origin closed before the declared response length")
        assert calls("/shared/incomplete") == 2
        assert request(front, "GET", "/shared/short")[0] == 200
        time.sleep(1.05)
        assert "Age" not in request(front, "GET", "/shared/short")[1]
        assert calls("/shared/short") == 2

    # One IP opens independent connections. Cache hits also consume tokens.
    with running_server(binary, root, [group], signal.SIGTERM, model,
                        ["-c", "-r", "1", "-B", "2"]) as front:
        path = "/shared/quota"
        with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
            responses = list(pool.map(lambda _: request(front, "GET", path), range(5)))
        assert sorted(s for s, _, _ in responses) == [200, 200, 429, 429, 429]
        assert all(h.get("Retry-After") == "1" for s, h, _ in responses if s == 429)
        assert 1 <= calls(path) <= 2  # simultaneous misses may both fetch
        assert request(front, "GET", "/")[0] == 200
        time.sleep(1.05)
        before = calls(path)
        assert request(front, "GET", path)[1]["Age"].isdigit()
        assert calls(path) == before


def check_model(binary, model):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory) / "public"
        root.mkdir()
        (root / "index.html").write_bytes(b"<h1>mini</h1>")
        (root / "style.css").write_bytes(b"body{}\n")
        (root / "empty.txt").touch()
        (root / "large.bin").write_bytes(b"x" * (4 * 1024 * 1024))
        (root / "nested").mkdir()
        (root / "nested" / "image.PNG").write_bytes(bytes(range(256)) * 128)
        outside = Path(directory) / "outside.txt"
        outside.write_bytes(b"outside")
        (root / "linked.txt").symlink_to(outside)
        with running_backend("api") as api_backend, running_backend("assets") as assets_backend:
            api = api_backend.server_port
            assets = assets_backend.server_port
            with socket.socket() as offline:
                offline.bind((HOST, 0))
                offline_port = offline.getsockname()[1]
            routes = [
                f"/api={HOST}:{api}",
                f"/assets={HOST}:{assets}",
                f"/offline={HOST}:{offline_port}",
            ]
            with running_server(binary, root, routes, signal.SIGTERM, model) as port:
                check_static_and_echo(port)
                check_concurrency(port)
                check_incomplete_request(port)
                check_slow_reader(port)
                check_worker_capacity(port, api_backend, model)
                check_streamed_upload(port, api_backend)
                check_proxy(port, api, assets)
            check_proxy_policy(binary, root, model, api_backend, assets_backend)
            with running_server(binary, root, [], signal.SIGINT, model) as port:
                assert request(port, "GET", "/")[0] == 200
    print(f"{model}: integration tests passed", flush=True)


def main():
    binary = str(Path(sys.argv[1]).resolve())
    models = ["pthread", "fork", "select", "poll"]
    if sys.platform.startswith("linux"):
        models.append("epoll")
    elif sys.platform == "darwin" or sys.platform.startswith("freebsd"):
        models.append("kqueue")
    for model in sys.argv[2:] or models:
        check_model(binary, model)


if __name__ == "__main__":
    main()
