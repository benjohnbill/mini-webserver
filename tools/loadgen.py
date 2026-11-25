#!/usr/bin/env python3
import argparse
from collections import Counter, defaultdict
from http.client import HTTPConnection
import json
import math
import re
import threading
import time


def request(args, source, path, timeout):
    conn = HTTPConnection(args.host, args.port, timeout=timeout, source_address=(source, 0))
    try:
        conn.request("GET", path, headers={"Connection": "close"})
        response = conn.getresponse()
        return str(response.status), response.getheader("X-Server-Name"), response.read()
    finally:
        conn.close()


def parse_status(body, target):
    workers = queue = watch = "-"
    for line in body.splitlines():
        fields = line.split()
        if fields[:1] == ["front"]:
            header = re.search(r"\bworkers (\d+/\d+) busy\s+queue (\d+)/\d+", line)
            if header:
                workers, queue = header.groups()
        if len(fields) >= 6 and fields[0] == target and fields[1] in ("UP", "DOWN"):
            watch = f"{fields[1]} {fields[4]}"
            if fields[5] == "FULL":
                watch += " FULL"
            break
    return workers, queue, watch


def poll_status(args):
    try:
        code, _, body = request(args, "127.0.0.1", "/_lb/status", min(args.timeout, 0.8))
        if code == "200":
            return parse_status(body.decode(), args.watch)
    except Exception:
        pass
    return "-", "-", "-"


def percentile(ordered_samples, percent):
    # Nearest rank retains an observed latency, including for small samples.
    if not ordered_samples:
        return None
    return round(ordered_samples[math.ceil(len(ordered_samples) * percent / 100) - 1], 3)


class Results:
    def __init__(self):
        self.codes = Counter()
        self.errors = 0
        self.latencies = []

    def record(self, code, latency):
        if code == "err":
            self.errors += 1
        else:
            self.codes[code] += 1
            self.latencies.append(latency)

    def summary(self, elapsed):
        requests = sum(self.codes.values()) + self.errors
        ordered = sorted(self.latencies)
        return dict(requests=requests, codes=dict(self.codes), err=self.errors,
                    p50_ms=percentile(ordered, 50), p99_ms=percentile(ordered, 99),
                    rps=round(requests / elapsed, 3))


def print_summary(label, summary):
    total = summary["requests"]
    counts = {**summary["codes"], "err": summary["err"]}
    rates = ", ".join(f"{code} {count} ({count / (total or 1):.1%})"
                      for code, count in sorted(counts.items()))
    p50, p99 = ("-" if summary[key] is None else f"{summary[key]:.3f} ms"
                for key in ("p50_ms", "p99_ms"))
    print(f"{label}: {total} requests, {rates}, p50 {p50}, p99 {p99}, "
          f"{summary['rps']:.2f} req/s")


def run(args):
    echo, probe = Results(), Results()
    stop = threading.Event()
    lock = threading.Lock()
    timeline = defaultdict(Counter)
    last_probe = "-"
    moved = set()
    down_seen = False
    start = time.monotonic()
    deadline = start + args.duration

    def client(index=None):
        is_probe = index is None
        source = "127.0.0.1" if is_probe else f"127.0.0.{index + 2}"
        path = args.probe if is_probe else args.path
        results = probe if is_probe else echo
        timeout = args.probe_timeout if is_probe else args.timeout
        first_name = None
        nonlocal last_probe
        while not stop.is_set() and (sent := time.monotonic()) < deadline:
            try:
                code, name, _ = request(args, source, path, timeout)
            except Exception:
                code, name = "err", None
            latency = (time.monotonic() - sent) * 1000
            # The socket timeout is per read, so a slow but steady probe still needs this check.
            if is_probe and latency > timeout * 1000:
                code = "err"
            with lock:
                results.record(code, latency)
                if is_probe:
                    last_probe = code if code == "err" else f"{code} {latency:.0f}ms"
                else:
                    if args.timeline:
                        timeline[int(time.monotonic() - start) + 1][code] += 1
                    # A response without a backend name cannot identify a move.
                    if name is not None:
                        if first_name is None:
                            first_name = name
                        if name != first_name:
                            moved.add(index)
            if is_probe and stop.wait(max(0, sent + args.probe_interval - time.monotonic())):
                break

    threads = [threading.Thread(target=client, args=(i,)) for i in range(args.clients)]
    if args.probe:
        threads.append(threading.Thread(target=client))
    row = "{:<4} {:<13} {:>8} {:>6} {:>6} {:>6} {:>8} {:>6}  {}"
    if args.timeline:
        print(row.format("t", "probe", "echo 200", "503", "504", "err", "workers", "queue", "watch"),
              flush=True)
    try:
        for thread in threads:
            thread.start()
        second = 1
        while (tick := start + second) <= deadline and (args.timeline or args.watch):
            time.sleep(max(0, tick - time.monotonic()))
            with lock:
                counts = timeline.pop(second, Counter())
                probe_status = last_probe
            workers, queue, watch = poll_status(args)
            down_seen |= watch.startswith("DOWN ")
            if args.timeline:
                print(row.format(f"{second}s", probe_status, counts["200"], counts["503"],
                                 counts["504"], counts["err"], workers, queue, watch), flush=True)
            second += 1
        time.sleep(max(0, deadline - time.monotonic()))
        stop.set()
        # Keep Ctrl-C interruptible while the last requests finish.
        while any(thread.is_alive() for thread in threads):
            time.sleep(0.01)
    except KeyboardInterrupt:
        print("\nInterrupted", flush=True)
    finally:
        stop.set()
        for thread in threads:
            if thread.ident is not None:
                thread.join()

    elapsed = time.monotonic() - start
    summary = {"echo": echo.summary(elapsed), "probe": probe.summary(elapsed),
               "watch": {"target": args.watch, "down_seen": down_seen} if args.watch else None}
    summary["echo"]["moved"] = len(moved)
    print_summary("probe", summary["probe"])
    print_summary("echo", summary["echo"])
    print(f"moved {len(moved)}/{args.clients} clients")
    if args.json:
        with open(args.json, "w", encoding="utf-8") as output:
            json.dump(summary, output)
            output.write("\n")


def self_test():
    assert percentile([], 99) is None
    assert percentile(list(range(1, 101)), 50) == 50
    assert percentile(list(range(1, 101)), 99) == 99
    assert percentile([1.25], 99) == 1.25
    target = "127.0.0.1:8002"
    header = "front :8080  workers 8/8 busy  queue 12/256  shed 0 (queue) 0 (stale)\n"
    for state, inflight in [("UP", "2/2 FULL"), ("UP", "3/-"), ("DOWN", "0/2")]:
        status = f"{header}  {target}  {state}  1  33.4%  {inflight}  124  0  5  0  1s ago"
        assert parse_status(status, target) == ("8/8", "12", f"{state} {inflight}")
        assert parse_status(status, "127.0.0.1:8003") == ("8/8", "12", "-")
    print("loadgen self-test passed")


def main():
    parser = argparse.ArgumentParser(description="Send closed-loop HTTP load from distinct loopback IPs.")
    parser.add_argument("--clients", type=int, default=20)
    parser.add_argument("--duration", type=float, default=10)
    parser.add_argument("--path", default="/echo")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--probe")
    parser.add_argument("--probe-interval", type=float, default=0.2)
    parser.add_argument("--timeline", action="store_true")
    parser.add_argument("--watch")
    parser.add_argument("--timeout", type=float, default=5)
    parser.add_argument("--probe-timeout", type=float, help="seconds a probe may take (default: --timeout)")
    parser.add_argument("--json", metavar="FILE")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if not 1 <= args.clients <= 250:
        parser.error("--clients must be between 1 and 250")
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if args.probe_timeout is None:
        args.probe_timeout = args.timeout
    for option in ("duration", "probe_interval", "timeout", "probe_timeout"):
        value = getattr(args, option)
        if not math.isfinite(value) or value <= 0:
            parser.error(f"--{option.replace('_', '-')} must be finite and positive")
    run(args)


if __name__ == "__main__":
    main()
