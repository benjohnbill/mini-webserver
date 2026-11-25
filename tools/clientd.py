#!/usr/bin/env python3
"""Keep source-IP-bound clients running while the lab state changes."""
import argparse
from collections import Counter
from dataclasses import dataclass
from http.client import HTTPException
import math
from pathlib import Path
import re
from select import select
import signal
from socket import socketpair
import sys
import threading
import time

# Keep the mounted repository free of cache files when importing loadgen.
sys.dont_write_bytecode = True
from loadgen import request


MAX_CLIENTS = 250
FIELDS = ("sent", "200", "503", "504", "other", "err")


def parse_state(text):
    values = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) != 2 or parts[0] not in ("count", "rps") or parts[0] in values:
            raise ValueError("expected one count line and one rps line")
        values[parts[0]] = parts[1]
    count = values.get("count", "")
    rps = values.get("rps", "")
    if not re.fullmatch(r"[0-9]+", count) or not 0 <= int(count) <= MAX_CLIENTS:
        raise ValueError("count must be 0..250")
    if not re.fullmatch(r"(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)", rps) or not 0.1 <= float(rps) <= 20:
        raise ValueError("rps must be 0.1..20")
    return int(count), float(rps)


def source_ip(index):
    return f"127.0.0.{index + 2}"


def format_stats(count, rps, uptime, total, last, sticky, requests):
    def counters(label, values):
        return label + "  ".join(f"{field} {values[field]}" for field in FIELDS)

    rate = str(rps).removesuffix(".0")
    lines = [
        f"clients {count}  rps {rate}  uptime {uptime}s",
        counters("total  ", total),
        counters("last1s ", last),
        "backend NAME  CLIENTS_STICKY  REQS",
    ]
    lines.extend(f"backend {name}  {sticky[name]}  {requests[name]}" for name in sorted(requests))
    return "\n".join(lines) + "\n"


@dataclass
class Client:
    thread: threading.Thread = None
    stopping: bool = False
    backend: str = ""


def run(args):
    changed = threading.Condition()
    clients = {}
    total, previous, requests = Counter(), Counter(), Counter()
    count, rps = 0, 2.0
    stopped_at = None
    last_stats_error = None

    def stop_handler(_signum, _frame):
        nonlocal stopped_at
        if stopped_at is None:
            stopped_at = time.monotonic()

    def report_stats_error(exc):
        nonlocal last_stats_error
        message = " ".join(str(exc).splitlines())
        if message != last_stats_error:
            print(f"clientd stats: {message}", file=sys.stderr, flush=True)
            last_stats_error = message

    def worker(index, client):
        source = source_ip(index)
        last_start = -math.inf
        while True:
            with changed:
                while not client.stopping and stopped_at is None:
                    now = time.monotonic()
                    remaining = last_start + 1 / rps - now
                    if remaining <= 0:
                        last_start = now
                        total["sent"] += 1
                        break
                    changed.wait(remaining)
                else:
                    return

            try:
                code, name, _ = request(args, source, args.path, args.timeout)
                name = " ".join(name.split()) if name else ""
                bucket = code if code in ("200", "503", "504") else "other"
            except (OSError, HTTPException, ValueError):
                bucket, name = "err", None
            with changed:
                total[bucket] += 1
                if name:
                    client.backend = name
                    requests[name] += 1

    def resize(next_count, next_rps):
        nonlocal rps
        with changed:
            rate_changed = rps != next_rps
            rps = next_rps
            for index, client in list(clients.items()):
                if not client.thread.is_alive():
                    del clients[index]
                elif index >= next_count and not client.stopping:
                    client.stopping = True
                    rate_changed = True
            if rate_changed:
                changed.notify_all()
            # A retiring client must finish before that source IP can be reused.
            for index in range(next_count):
                if index not in clients and stopped_at is None:
                    client = Client()
                    client.thread = threading.Thread(target=worker, args=(index, client), daemon=True)
                    client.thread.start()
                    clients[index] = client

    def publish(uptime):
        nonlocal previous
        with changed:
            active = [client for client in clients.values() if not client.stopping]
            sticky = Counter(client.backend for client in active if client.backend)
            report = format_stats(len(active), rps, uptime, total, total - previous, sticky, requests)
            previous = total.copy()
        temp.write_text(report, encoding="utf-8")
        temp.replace(args.stats)

    signal.signal(signal.SIGTERM, stop_handler)
    signal.signal(signal.SIGINT, stop_handler)
    wakeup, notify = socketpair()
    notify.setblocking(False)
    old_wakeup = signal.set_wakeup_fd(notify.fileno())
    temp = args.stats.with_name(args.stats.name + ".tmp")
    start = next_stats = time.monotonic()
    last_error = None
    try:
        while stopped_at is None:
            try:
                count, next_rps = parse_state(args.state.read_text(encoding="utf-8"))
            except (OSError, ValueError) as exc:
                if str(exc) != last_error:
                    print(f"clientd state: {exc}", file=sys.stderr, flush=True)
                last_error = str(exc)
                next_rps = rps
            else:
                last_error = None
            resize(count, next_rps)
            now = time.monotonic()
            if now >= next_stats:
                try:
                    publish(int(now - start))
                except OSError as exc:
                    report_stats_error(exc)
                else:
                    last_stats_error = None
                next_stats = now + 1
            if stopped_at is None:
                select([wakeup], [], [], 0.5)
    finally:
        signal.set_wakeup_fd(old_wakeup)
        wakeup.close()
        notify.close()
        deadline = (time.monotonic() if stopped_at is None else stopped_at) + args.timeout
        with changed:
            for client in clients.values():
                client.stopping = True
            changed.notify_all()
        for client in clients.values():
            client.thread.join(max(0, deadline - time.monotonic()))
        try:
            temp.unlink(missing_ok=True)
        except OSError as exc:
            report_stats_error(exc)


def self_test():
    assert parse_state("count 0\nrps .1\n") == (0, 0.1)
    assert parse_state("rps 20\ncount 250\n") == (250, 20)
    for invalid in ("count 251\nrps 2", "count 1\nrps nan", "count 0\nrps 0",
                    "count 1\ncount 2\nrps 2", "count 2\n", "count 2\nrps 2\nextra 1"):
        try:
            parse_state(invalid)
        except ValueError:
            continue
        raise AssertionError(f"accepted invalid state {invalid!r}")
    ips = [source_ip(index) for index in range(MAX_CLIENTS)]
    assert len(set(ips)) == 250 and (ips[0], ips[-1]) == ("127.0.0.2", "127.0.0.251")
    assert format_stats(1, 2.5, 8, Counter({"sent": 5, "200": 3, "err": 1}),
                        Counter({"sent": 1, "503": 1}), Counter(b1=1), Counter(b1=3, b2=1)) == (
        "clients 1  rps 2.5  uptime 8s\n"
        "total  sent 5  200 3  503 0  504 0  other 0  err 1\n"
        "last1s sent 1  200 0  503 1  504 0  other 0  err 0\n"
        "backend NAME  CLIENTS_STICKY  REQS\n"
        "backend b1  1  3\n"
        "backend b2  0  1\n"
    )
    print("clientd self-test passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--state", type=Path)
    parser.add_argument("--stats", type=Path)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--path", default="/echo")
    parser.add_argument("--timeout", type=float, default=2)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if args.state is None or args.stats is None:
        parser.error("--state and --stats are required")
    if not 1 <= args.port <= 65535 or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("port must be 1..65535 and timeout must be positive and finite")
    run(args)


if __name__ == "__main__":
    main()
