"""Shared parts of the Python Crawler scripts and human.py.

It is lib.sh for Python, with the standard library only. Each script runs as
    bench/arena/<name>.py BASE_URL DURATION
and prints one line at the end, as lib.sh does:
    "<name> sent=N mean_ms=M p95_ms=P pow_cpu_s=S <status>=<count> ..."
A status of 000 is a request with no reply (a tarpit, a reset, or a
timeout). A request that is in flight at the end is not counted.
pow_cpu_s is the CPU time that the proof of work solutions took.
"""

import functools
import hashlib
import http.client
import math
import os
import ssl
import sys
import threading
import time
import urllib.error
import urllib.request

# The same as lib.sh: curl --max-time 30, and the dataset of
# bench/cache/make-dataset.sh.
REQUEST_TIMEOUT = 30
FILE_COUNT = 200
CHROME_AGENT = ('Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) '
                'AppleWebKit/537.36 (KHTML, like Gecko) '
                'Chrome/131.0.0.0 Safari/537.36')
# 1: keep a solution and send it again until the next 401, as a browser
# keeps the cookie of Anubis. 0: send it once only.
POW_REUSE = os.environ.get('POW_REUSE', '1') != '0'


def args_parse():
    if len(sys.argv) != 3:
        sys.exit(f'usage: {sys.argv[0]} BASE_URL DURATION')
    return sys.argv[1].rstrip('/'), float(sys.argv[2])


def page_target(index):
    """The target of dataset file index % FILE_COUNT."""
    return f'/bench/f{index % FILE_COUNT:03d}.txt'


def tls_context():
    """The context of every Python client. It must not change: the JA4
    table of the layers preset holds the JA4 of this context. The
    certificate of rproxy is self-signed, as curl -k."""
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE
    return context


class SourceHttpsHandler(urllib.request.HTTPSHandler):
    """urllib with a source address, as curl --interface."""

    def __init__(self, context, source):
        super().__init__(context=context)
        self.source = source

    def https_open(self, req):
        connection = functools.partial(http.client.HTTPSConnection,
                                       source_address=(self.source, 0))
        return self.do_open(connection, req, context=self._context)


def opener_create(source=None):
    context = tls_context()
    if source is None:
        handler = urllib.request.HTTPSHandler(context=context)
    else:
        handler = SourceHttpsHandler(context, source)
    return urllib.request.build_opener(handler)


def pow_solve(challenge):
    """Solves "NONCE BITS": the first decimal counter such that SHA-256 of
    "NONCE:COUNTER" starts with BITS zero bits. Returns the X-PoW value and
    the CPU seconds that it took."""
    nonce, bits = challenge.split()
    bits = int(bits)
    limit = 1 << (256 - bits)
    start = time.thread_time()
    counter = 0
    prefix = nonce.encode() + b':'
    while int.from_bytes(hashlib.sha256(prefix + str(counter).encode())
                         .digest(), 'big') >= limit:
        counter += 1
    return f'{nonce}:{counter}', time.thread_time() - start


class Client:
    """One client: an opener, the headers it sends, and its results. A
    Client is used by one thread."""

    def __init__(self, run, base, agent, source=None, solves_pow=False):
        self.run = run
        self.base = base
        self.agent = agent
        self.opener = opener_create(source)
        self.solves_pow = solves_pow
        self.solution = None

    def fetch(self, target, headers=None):
        """Sends one GET. If the reply is a proof of work challenge and the
        client solves it, solves it and sends the GET again. Each GET is one
        result. Returns the status of the last GET."""
        status, reply_headers = self.fetch_once(target, headers)
        challenge = reply_headers.get('X-PoW-Challenge')
        if status == 401 and challenge and self.solves_pow:
            self.solution, cpu_seconds = pow_solve(challenge)
            self.run.pow_add(cpu_seconds)
            status, reply_headers = self.fetch_once(target, headers)
            if not POW_REUSE:
                self.solution = None
        return status

    def fetch_once(self, target, headers):
        request_headers = {'User-Agent': self.agent}
        if self.solution is not None:
            request_headers['X-PoW'] = self.solution
        request_headers.update(headers or {})
        request = urllib.request.Request(self.base + target,
                                         headers=request_headers)
        start = time.monotonic()
        status, reply_headers = '000', {}
        try:
            with self.opener.open(request, timeout=REQUEST_TIMEOUT) as reply:
                reply.read()
                status, reply_headers = reply.status, reply.headers
        except urllib.error.HTTPError as error:
            error.read()
            status, reply_headers = error.code, error.headers
        except (OSError, http.client.HTTPException):
            pass
        self.run.result_add(status, time.monotonic() - start)
        return status if status == '000' else int(status), reply_headers


def p95_ms(times):
    """The 95th percentile of times, in ms, by the nearest-rank method. A
    request that is still in flight at the stop is not in times."""
    if not times:
        return 0
    ordered = sorted(times)
    return ordered[math.ceil(len(ordered) * 0.95) - 1] * 1000


class Run:
    """The loops of one script and their results, as lib.sh."""

    def __init__(self, name, duration):
        self.name = name
        self.deadline = time.monotonic() + duration
        self.lock = threading.Lock()
        self.counts = {}
        self.sent = 0
        self.seconds = 0.0
        self.times = []
        self.pow_seconds = 0.0

    def is_running(self):
        return time.monotonic() < self.deadline

    def result_add(self, status, seconds):
        # A request that ends after the stop is not counted.
        if not self.is_running():
            return
        key = f'{status:03d}' if isinstance(status, int) else status
        with self.lock:
            self.sent += 1
            self.seconds += seconds
            self.times.append(seconds)
            self.counts[key] = self.counts.get(key, 0) + 1

    def pow_add(self, cpu_seconds):
        with self.lock:
            self.pow_seconds += cpu_seconds

    def sleep(self, seconds):
        time.sleep(max(0.0, min(seconds, self.deadline - time.monotonic())))

    def loops_run(self, loop, count):
        """Runs loop(index) in count threads, waits for the duration, and
        prints the summary line. The threads are daemons: a request that a
        tarpit holds does not keep the script alive."""
        for index in range(count):
            threading.Thread(target=loop, args=(index,), daemon=True).start()
        self.sleep(self.deadline - time.monotonic())
        with self.lock:
            mean_ms = self.seconds * 1000 / self.sent if self.sent else 0
            line = (f'{self.name} sent={self.sent} mean_ms={mean_ms:.0f} '
                    f'p95_ms={p95_ms(self.times):.0f} '
                    f'pow_cpu_s={self.pow_seconds:.2f}')
            for status in sorted(self.counts):
                line += f' {status}={self.counts[status]}'
        print(line, flush=True)
        os._exit(0)
