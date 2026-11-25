#!/usr/bin/env python3
"""The clients of the c10k Scene.

N TLS connections to rproxy, split over P processes. Each process opens its
connections evenly over the ramp time. Then all connections wait, idle and
open, until one go time. At the go time each connection sends one GET and
reads the reply to EOF. The parent process samples rproxy (/proc/PID) while
the clients run, and prints one JSON summary.

rproxy cuts a connection with no request after 5 s (CLIENT_TIMEOUT), so the
ramp plus the hold must stay under 5 s.

Usage: bench/c10k/clients.py --pid PID [--host H] [--port P] [--count N]
       [--procs P] [--ramp S] [--hold S] [--path P] [--size BYTES]
"""
import argparse
import asyncio
import json
import multiprocessing
import os
import ssl
import time

SAMPLE_PERIOD = 0.1       # seconds between two samples of rproxy
START_DELAY = 1.0         # seconds for the worker processes to start
READ_TIMEOUT = 30.0       # seconds for one reply
CHUNK_MAX = 65536
PAGE_BYTES = os.sysconf("SC_PAGE_SIZE")


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--pid", type=int, required=True,
                        help="pid of rproxy, to sample its memory")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=19443)
    parser.add_argument("--count", type=int, default=10000)
    parser.add_argument("--procs", type=int, default=8)
    parser.add_argument("--ramp", type=float, default=3.0,
                        help="seconds to open all connections")
    parser.add_argument("--hold", type=float, default=0.5,
                        help="seconds between the ramp end and the go time")
    parser.add_argument("--path", default="/bench/c10k.bin")
    parser.add_argument("--size", type=int, default=10240,
                        help="bytes of the file, to count full bodies")

    return parser.parse_args()


def make_tls_context():
    # rproxy uses the self-signed build/cert.pem.
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE

    return context


async def client_open(args, context, at):
    """Open one TLS connection at time `at`. Return (reader, writer, ms)."""
    await asyncio.sleep(max(0.0, at - time.time()))
    start = time.monotonic()
    reader, writer = await asyncio.wait_for(asyncio.open_connection(
        args.host, args.port, ssl=context, server_hostname="localhost",
        limit=CHUNK_MAX), READ_TIMEOUT)

    return reader, writer, (time.monotonic() - start) * 1000


async def client_fetch(reader, writer, path):
    """Send one GET and read to EOF. Return (status, body bytes)."""
    writer.write(f"GET {path} HTTP/1.1\r\nHost: localhost\r\n"
                 "Connection: close\r\n\r\n".encode())
    await writer.drain()
    reply = await asyncio.wait_for(reader.read(), READ_TIMEOUT)
    head, _, body = reply.partition(b"\r\n\r\n")
    status = int(head.split(b" ", 2)[1]) if head else 0

    return status, len(body)


async def client_run(args, context, at, go):
    result = {"connect_ms": None, "late": False, "status": 0, "bytes": 0,
              "latency_ms": None, "error": None, "stage": "connect"}
    writer = None
    try:
        reader, writer, result["connect_ms"] = await client_open(
            args, context, at)
        result["late"] = time.time() > go
        await asyncio.sleep(max(0.0, go - time.time()))
        result["stage"] = "fetch"
        start = time.monotonic()
        result["status"], result["bytes"] = await client_fetch(
            reader, writer, args.path)
        result["latency_ms"] = (time.monotonic() - start) * 1000
    except (OSError, ssl.SSLError, asyncio.TimeoutError, ValueError,
            IndexError) as error:
        result["error"] = f"{result['stage']}:{type(error).__name__}"
    finally:
        if writer is not None:
            writer.close()

    return result


async def worker_main(args, index, count, t0, go):
    context = make_tls_context()
    step = args.ramp / max(1, count)
    # Shift each process by a fraction of a step, so the processes do not
    # connect in lockstep.
    offset = step * index / args.procs
    tasks = [client_run(args, context, t0 + offset + i * step, go)
             for i in range(count)]

    return await asyncio.gather(*tasks)


def worker_run(args, index, count, t0, go):
    return asyncio.run(worker_main(args, index, count, t0, go))


def rproxy_sample(pid):
    """Return (rss bytes, open fds, kernel TCP memory bytes, TCP in use)."""
    rss = 0
    with open(f"/proc/{pid}/status") as status:
        for line in status:
            if line.startswith("VmRSS:"):
                rss = int(line.split()[1]) * 1024
    fds = len(os.listdir(f"/proc/{pid}/fd"))
    tcp_mem = tcp_inuse = 0
    with open("/proc/net/sockstat") as sockstat:
        for line in sockstat:
            if line.startswith("TCP:"):
                fields = line.split()
                tcp_inuse = int(fields[fields.index("inuse") + 1])
                tcp_mem = int(fields[fields.index("mem") + 1]) * PAGE_BYTES

    return {"time": time.time(), "rss": rss, "fds": fds,
            "tcp_mem": tcp_mem, "tcp_inuse": tcp_inuse}


def percentile(values, share):
    if not values:
        return None
    ordered = sorted(values)

    return ordered[min(len(ordered) - 1, int(share * len(ordered)))]


def samples_at(samples, moment):
    """The last sample at or before `moment`."""
    before = [s for s in samples if s["time"] <= moment]

    return before[-1] if before else samples[0]


def summary_make(args, results, samples, t0, go):
    connected = [r for r in results if r["connect_ms"] is not None]
    fetched = [r for r in results if r["latency_ms"] is not None]
    errors = {}
    for result in results:
        if result["error"]:
            errors[result["error"]] = errors.get(result["error"], 0) + 1
    statuses = {}
    for result in fetched:
        statuses[str(result["status"])] = (
            statuses.get(str(result["status"]), 0) + 1)
    idle = samples_at(samples, t0)
    held = samples_at(samples, go)
    latencies = [r["latency_ms"] for r in fetched]
    connects = [r["connect_ms"] for r in connected]
    end = max(s["time"] for s in samples)

    return {
        "count": args.count, "procs": args.procs, "ramp": args.ramp,
        "hold": args.hold, "path": args.path,
        "connected": len(connected),
        "late": sum(1 for r in connected if r["late"]),
        "full_body": sum(1 for r in fetched if r["bytes"] == args.size),
        "statuses": statuses, "errors": errors,
        "connect_ms": {"p50": percentile(connects, 0.50),
                       "p99": percentile(connects, 0.99),
                       "max": max(connects, default=None)},
        "latency_ms": {"p50": percentile(latencies, 0.50),
                       "p95": percentile(latencies, 0.95),
                       "p99": percentile(latencies, 0.99),
                       "max": max(latencies, default=None)},
        "burst_s": (max(latencies) / 1000) if latencies else None,
        "rss_idle": idle["rss"], "rss_held": held["rss"],
        "rss_peak": max(s["rss"] for s in samples),
        "fds_held": held["fds"], "fds_peak": max(s["fds"] for s in samples),
        "tcp_mem_idle": idle["tcp_mem"], "tcp_mem_held": held["tcp_mem"],
        "tcp_inuse_held": held["tcp_inuse"],
        "run_s": end - t0,
    }


def main():
    args = parse_args()
    t0 = time.time() + START_DELAY
    go = t0 + args.ramp + args.hold
    shares = [args.count // args.procs + (1 if i < args.count % args.procs
                                          else 0)
              for i in range(args.procs)]
    samples = [rproxy_sample(args.pid)]
    with multiprocessing.Pool(args.procs) as pool:
        pending = pool.starmap_async(
            worker_run,
            [(args, i, shares[i], t0, go) for i in range(args.procs)])
        while not pending.ready():
            samples.append(rproxy_sample(args.pid))
            pending.wait(SAMPLE_PERIOD)
        results = [r for part in pending.get() for r in part]
    samples.append(rproxy_sample(args.pid))
    print(json.dumps(summary_make(args, results, samples, t0, go)))


if __name__ == "__main__":
    main()
