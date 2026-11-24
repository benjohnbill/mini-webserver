#!/usr/bin/env python3
"""The Phones of the Back pressure demo (SPEC 12).

Each Phone opens one TLS connection to rproxy, sends one GET, and reads the
body at a fixed speed. The Phones start one by one over the arrival time.
The program writes the state of each Phone to a JSON status file.

Usage: bench/demo/phones.py [--host H] [--port P] [--count N]
       [--arrival S] [--rate KBPS] [--path P] [--status FILE]
"""
import argparse
import asyncio
import enum
import json
import os
import socket
import ssl
import time

RCVBUF = 65536            # a small receive window, so TCP flow control is real
CHUNK_MAX = 16384         # the largest read of one step
STATUS_PERIOD = 0.2       # seconds between two status writes
SPEED_WINDOW = 1.0        # seconds of history for the speed of a Phone
HEADER_MAX = 16384


class State(str, enum.Enum):
    WAITING = "waiting"
    RECEIVING = "receiving"
    DONE = "done"
    HTTP_502 = "http_502"
    HTTP_503 = "http_503"
    HTTP_OTHER = "http_other"
    CUT = "cut"          # the connection closed before the full body
    REFUSED = "refused"  # the connect failed


STATUS_STATES = {502: State.HTTP_502, 503: State.HTTP_503}


class Phone:
    def __init__(self, index):
        self.index = index
        self.state = State.WAITING
        self.bytes = 0          # body bytes read
        self.length = None      # Content-Length of the reply
        self.status = None      # HTTP status code
        self.error = None       # short text of the failure
        self.samples = []       # (time, bytes) for the speed

    def speed(self, now):
        """Body bytes per second over the last SPEED_WINDOW seconds."""
        self.samples.append((now, self.bytes))
        while len(self.samples) > 2 and now - self.samples[1][0] >= SPEED_WINDOW:
            self.samples.pop(0)
        first_time, first_bytes = self.samples[0]
        if now - first_time <= 0:
            return 0
        return int((self.bytes - first_bytes) / (now - first_time))

    def to_json(self, now):
        speed = self.speed(now) if self.state == State.RECEIVING else 0
        return {"state": self.state.value, "bytes": self.bytes,
                "length": self.length, "status": self.status,
                "speed": speed, "error": self.error}


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=19443)
    parser.add_argument("--count", type=int, default=200)
    parser.add_argument("--arrival", type=float, default=15.0,
                        help="seconds to start all Phones")
    parser.add_argument("--rate", type=float, default=400.0,
                        help="KB/s that each Phone reads")
    parser.add_argument("--path", default="/update.bin")
    parser.add_argument("--status", default="bench/demo/run/status.json")
    parser.add_argument("--stall", type=float, default=30.0,
                        help="seconds with no byte before a Phone gives up")

    return parser.parse_args()


def make_tls_context():
    # rproxy uses the self-signed build/cert.pem.
    context = ssl.create_default_context()
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE

    return context


async def phone_connect(phone, args, context):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, RCVBUF)
    sock.setblocking(False)
    loop = asyncio.get_running_loop()
    try:
        await loop.sock_connect(sock, (args.host, args.port))
    except OSError:
        sock.close()
        raise

    return await asyncio.open_connection(
        sock=sock, ssl=context, server_hostname="localhost", limit=RCVBUF)


async def phone_read_head(phone, reader, stall):
    head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), stall)
    lines = head.decode("latin-1").split("\r\n")
    phone.status = int(lines[0].split()[1])
    for line in lines[1:]:
        name, _, value = line.partition(":")
        if name.strip().lower() == "content-length":
            phone.length = int(value.strip())


async def phone_read_body(phone, reader, rate, stall):
    """Read the body at `rate` bytes per second. Return at EOF."""
    start = time.monotonic()
    while phone.length is None or phone.bytes < phone.length:
        ahead = phone.bytes - rate * (time.monotonic() - start)
        if ahead > 0:
            await asyncio.sleep(ahead / rate)
        chunk = await asyncio.wait_for(reader.read(CHUNK_MAX), stall)
        if not chunk:
            return
        phone.bytes += len(chunk)


async def phone_run(phone, args, context, delay):
    await asyncio.sleep(delay)
    try:
        reader, writer = await phone_connect(phone, args, context)
    except OSError as error:
        phone.state = State.REFUSED
        phone.error = error.strerror or type(error).__name__
        return
    phone.state = State.RECEIVING
    request = (f"GET {args.path} HTTP/1.1\r\nHost: localhost\r\n"
               "Connection: close\r\n\r\n")
    try:
        writer.write(request.encode())
        await writer.drain()
        await phone_read_head(phone, reader, args.stall)
        await phone_read_body(phone, reader, args.rate * 1000, args.stall)
        phone.state = phone_decide_end(phone)
    except (OSError, ssl.SSLError, asyncio.IncompleteReadError,
            asyncio.TimeoutError, ValueError, IndexError) as error:
        phone.state = State.CUT
        phone.error = type(error).__name__
    finally:
        writer.close()


def phone_decide_end(phone):
    if phone.length is not None and phone.bytes < phone.length:
        return State.CUT
    if phone.status == 200:
        return State.DONE

    return STATUS_STATES.get(phone.status, State.HTTP_OTHER)


def status_write(path, phones, args, start, finished):
    now = time.time()
    totals = {state.value: 0 for state in State}
    for phone in phones:
        totals[phone.state.value] += 1
    status = {
        "start": start, "now": now, "finished": finished,
        "count": args.count, "arrival": args.arrival,
        "rate": args.rate * 1000, "path": args.path,
        "totals": totals,
        "bytes": sum(phone.bytes for phone in phones),
        "phones": [phone.to_json(now) for phone in phones],
    }
    temp = path + ".tmp"
    with open(temp, "w") as file:
        json.dump(status, file)
    os.replace(temp, path)


async def status_loop(path, phones, args, start):
    while True:
        status_write(path, phones, args, start, False)
        await asyncio.sleep(STATUS_PERIOD)


async def main():
    args = parse_args()
    os.makedirs(os.path.dirname(os.path.abspath(args.status)), exist_ok=True)
    context = make_tls_context()
    phones = [Phone(i) for i in range(args.count)]
    start = time.time()
    step = args.arrival / args.count if args.count else 0
    writer_task = asyncio.create_task(
        status_loop(args.status, phones, args, start))
    await asyncio.gather(*(phone_run(phone, args, context, i * step)
                           for i, phone in enumerate(phones)))
    writer_task.cancel()
    status_write(args.status, phones, args, start, True)
    totals = {}
    for phone in phones:
        totals[phone.state.value] = totals.get(phone.state.value, 0) + 1
    print(f"phones: {totals} in {time.time() - start:.1f} s")


if __name__ == "__main__":
    asyncio.run(main())
