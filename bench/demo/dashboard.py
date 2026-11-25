#!/usr/bin/env python3
"""The terminal dashboard of the Back pressure demo (SPEC 12).

It reads the run directory that demo.sh writes:
  act.json            the Act: number, Relay mode, rproxy pid, cgroup, ports,
                      memory limit, and the speed tiers of the Phones
  status-TIER.json    the Phones of one tier (bench/demo/phones.py)
  relay/loop-*.txt    one line for each client connection of rproxy:
                      "client_port origin_port pending capacity paused slot"
  rproxy.log          the "cause" lines: why a reply failed
  rproxy.exit         the exit code of rproxy, or "oom-kill", when it stopped
  scores.json         one row for each Act that ended (this file writes it)
It reads the memory of rproxy from memory.current of its cgroup, and the
kernel buffers of each socket from `ss -tmnHO`.

Usage: bench/demo/dashboard.py [--run DIR] [--once [--watch S] [--width N]]
"""
import argparse
import glob
import json
import os
import re
import subprocess
import time

from rich.console import Console, Group
from rich.live import Live
from rich.text import Text

POLL_PERIOD = 0.2          # seconds between two frames
CHART_ROWS = 7
CHART_SECONDS_PER_COLUMN = 0.5
CHART_AXIS = 9             # columns of the axis labels
MIB = 1024 * 1024
BLOCKS = " ▁▂▃▄▅▆▇█"
GRID_PREFIX = 27           # columns of " <tier> <count> <speed> <done> "
FOOTNOTE = (" speed tiers = KR carrier QoS caps; tier shares = assumption;"
            " server memory is scaled")
SKMEM = re.compile(r"([a-z]+)(\d+)")

# One palette for the speed tiers (the pipe) and one for the results (the
# grid). They share no colour.
SLOW_COLOR = "medium_purple1"
FAST_COLOR = "dark_cyan"
STATES = {      # state: (label, colour, darker colour, plain char)
    "done": ("200 OK", "green3", "green4", "#"),
    "receiving": ("receiving", "dodger_blue1", "dodger_blue3", ">"),
    "http_502": ("502", "red1", "red3", "2"),
    "http_503": ("503", "orange1", "dark_orange3", "3"),
    "http_other": ("other code", "red1", "red3", "o"),
    "cut": ("cut", "magenta1", "magenta3", "x"),
    "refused": ("refused", "gold1", "gold3", "r"),
    "waiting": ("waiting", "grey30", "grey23", "."),
}
SHORT = {"done": "OK", "receiving": "recv", "http_502": "502",
         "http_503": "503", "http_other": "other", "cut": "cut",
         "refused": "refused", "waiting": "wait"}
STATE_ORDER = ("done", "receiving", "http_502", "http_503", "http_other",
               "cut", "refused", "waiting")

# The talk calls each Act a Phase; the screen uses the Phase and the mode.
PHASES = {
    1: ("Phase 1", "unbounded", "no limit"),
    2: ("Phase 2-A", "reply-cap", "the 4 MiB limit we had"),
    3: ("Phase 2-B", "conn-cap", "a connection limit, set for memory"),
    4: ("Phase 3", "backpressure", "read the Origin as fast as the Phone"),
    5: ("Epilogue", "backpressure", "pool Origin"),
}


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--run", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "run"))
    parser.add_argument("--once", action="store_true",
                        help="print one frame with no color and exit")
    parser.add_argument("--watch", type=float, default=0.0,
                        help="with --once: poll for S seconds first")
    parser.add_argument("--width", type=int, default=95,
                        help="width of the --once frame")

    return parser.parse_args()


def read_json(path):
    try:
        with open(path) as file:
            return json.load(file)
    except (OSError, ValueError):
        return None


def read_text(path):
    try:
        with open(path) as file:
            return file.read().strip()
    except OSError:
        return None


class Monitor:
    """What the dashboard remembers between two frames of one Act. After
    rproxy dies, every block shows the last frame before the death."""

    def __init__(self, act):
        self.act = act or {}
        self.memory = []          # (seconds since act start, bytes)
        self.peak = 0
        self.oom_kills = 0
        self.death = None         # seconds since act start when rproxy died
        self.relay = []           # lines of the relay stats, as dicts
        self.kernel = {}          # stage -> sums of the sockets (ss)
        self.tiers = None         # list of Tier, frozen at the death
        self.seen = 0.0           # seconds since act start of the last frame

    def poll(self, run_dir):
        start = self.act.get("started", time.time())
        now = time.time() - start
        pid = self.act.get("rproxy_pid")
        if pid and self.death is None:
            if self.is_alive(pid):
                self.poll_memory(now)
                self.relay = relay_read(run_dir)
                self.kernel = kernel_read(self.act, self.relay)
                self.seen = now
            else:
                self.death = self.death_time(run_dir, start, now)

        return read_text(os.path.join(run_dir, "rproxy.exit"))

    @staticmethod
    def death_time(run_dir, start, now):
        """demo.sh writes rproxy.exit when rproxy stops: use its time."""
        try:
            return os.stat(os.path.join(run_dir, "rproxy.exit")).st_mtime \
                - start
        except OSError:
            return now

    @staticmethod
    def is_alive(pid):
        stat = read_text(f"/proc/{pid}/stat")
        # A zombie (state Z) has no memory any more.
        return stat is not None and stat.rsplit(")", 1)[1].split()[0] != "Z"

    def poll_memory(self, now):
        cgroup = self.act.get("cgroup")
        if not cgroup:
            return
        events = read_text(os.path.join(cgroup, "memory.events")) or ""
        for line in events.splitlines():
            name, _, value = line.partition(" ")
            if name == "oom_kill":
                self.oom_kills = int(value)
        # After the OOM kill the kernel frees the memory before the process
        # is gone. Keep the last value before the kill, not the drop to 0.
        if self.oom_kills:
            return
        current = read_text(os.path.join(cgroup, "memory.current"))
        if current is not None and int(current) > 0:
            self.memory.append((now, int(current)))
            self.peak = max(self.peak, int(current))


def relay_read(run_dir):
    """The relay stats of all loops, one dict for each connection."""
    lines = []
    for path in glob.glob(os.path.join(run_dir, "relay", "loop-*.txt")):
        for line in (read_text(path) or "").splitlines():
            fields = line.split()
            if line.startswith("#") or len(fields) != 6:
                continue
            try:
                port, origin, pending, capacity, paused, slot = map(
                    int, fields)
            except ValueError:
                continue
            lines.append({"port": port, "origin_port": origin,
                          "pending": pending, "capacity": capacity,
                          "paused": paused == 1, "slot": slot == 1})

    return lines


def kernel_read(act, relay):
    """The four kernel buffers of the pipe, from one `ss` call. Fill is the
    socket memory over the socket buffer size: w/tb for a send buffer, r/rb
    for a receive buffer. Each stage keeps the Phone port of each socket."""
    proxy_port = act.get("proxy_port")
    origin_port = act.get("origin_port")
    if not proxy_port or not origin_port:
        return {}
    try:
        out = subprocess.run(["ss", "-tmnHO", "state", "established"],
                             capture_output=True, text=True,
                             timeout=1).stdout
    except (OSError, subprocess.TimeoutExpired):
        return {}
    client_of = {line["origin_port"]: line["port"] for line in relay}
    stages = {}
    for line in out.splitlines():
        socket = ss_parse(line)
        if socket is None:
            continue
        stage, client = ss_stage(socket, proxy_port, origin_port, client_of)
        if stage is None:
            continue
        sums = stages.setdefault(stage, {"used": 0, "size": 0, "queue": 0,
                                         "clients": [], "blocked": []})
        is_send = stage in ("origin send", "proxy send")
        used = socket["w"] if is_send else socket["r"]
        sums["used"] += used
        sums["size"] += socket["tb"] if is_send else socket["rb"]
        sums["queue"] += socket["send_q"] if is_send else socket["recv_q"]
        sums["clients"].append((client, used))
        if stage == "origin send" and socket["send_q"] > 0:
            sums["blocked"].append(client)

    return stages


def ss_parse(line):
    """One line of `ss -tmnHO`: Recv-Q Send-Q local peer skmem:(...)."""
    fields = line.split()
    if len(fields) < 5 or not fields[4].startswith("skmem:("):
        return None
    try:
        memory = {key: int(value) for key, value in SKMEM.findall(fields[4])}
        return {"recv_q": int(fields[0]), "send_q": int(fields[1]),
                "local": int(fields[2].rsplit(":", 1)[1]),
                "peer": int(fields[3].rsplit(":", 1)[1]),
                "r": memory.get("r", 0), "rb": memory.get("rb", 0),
                "w": memory.get("w", 0), "tb": memory.get("tb", 0)}
    except (ValueError, IndexError):
        return None


def ss_stage(socket, proxy_port, origin_port, client_of):
    """Which buffer of the pipe a socket is, and the Phone port it serves."""
    if socket["local"] == origin_port:
        return "origin send", client_of.get(socket["peer"])
    if socket["peer"] == origin_port:
        return "proxy recv", client_of.get(socket["local"])
    if socket["local"] == proxy_port:
        return "proxy send", socket["peer"]
    if socket["peer"] == proxy_port:
        return "client recv", socket["local"]

    return None, None


class Tier:
    """One speed tier: its place in act.json and its status file."""

    def __init__(self, label, rate, count, status):
        self.label = label
        self.rate = rate                    # bytes per second
        self.count = count
        self.status = status
        self.phones = (status or {}).get("phones", [])[:count]
        self.states = [phone["state"] for phone in self.phones]
        self.states += ["waiting"] * (count - len(self.states))

    def total(self, *states):
        return sum(1 for state in self.states if state in states)

    def start_of(self, index):
        """The start time of Phone `index` (phones.py: index × step)."""
        status = self.status or {}
        step = status.get("arrival", 0) / self.count if self.count else 0

        return status.get("start", 0) + index * step


def tiers_read(run_dir, act):
    """The tiers of the Act, slowest first, joined to their status files."""
    statuses = {}
    started = (act or {}).get("started", 0)
    for path in glob.glob(os.path.join(run_dir, "status-*.json")):
        status = read_json(path)
        if not status or status.get("start", 0) < started:
            continue                        # left from the last Act
        statuses[status.get("tier", "")] = status
    specs = [(tier["label"], tier["rate"] * 1000, tier["count"])
             for tier in (act or {}).get("tiers", [])]
    tiers = [Tier(label, rate, count, statuses.get(label))
             for label, rate, count in specs]

    return sorted(tiers, key=lambda tier: tier.rate)


def tier_of_ports(tiers):
    """Phone port -> "slow" or "fast". The fastest tier is fast."""
    if not tiers:
        return {}
    fast = max(tiers, key=lambda tier: tier.rate)
    groups = {}
    for tier in tiers:
        for phone in tier.phones:
            if phone.get("port"):
                groups[phone["port"]] = "fast" if tier is fast else "slow"

    return groups


def causes_read(run_dir):
    """Counts of the "cause" lines of rproxy.log: (status, cause) -> n."""
    counts = {}
    for line in (read_text(os.path.join(run_dir, "rproxy.log")) or
                 "").splitlines():
        fields = line.split()
        if len(fields) == 5 and fields[0] == "cause":
            key = (fields[3], fields[4])
            counts[key] = counts.get(key, 0) + 1

    return counts


def render(monitor, tiers, exit_code, causes, scores, width, plain):
    parts = [render_title(monitor, exit_code, width, plain)]
    parts += render_chart(monitor, width)
    parts.append(Text(""))
    parts += render_pipe(monitor, tiers, width, plain)
    parts.append(Text(""))
    parts += render_grid(tiers, width, plain,
                         monitor.act.get("file_bytes", 0))
    parts += render_legend(monitor, tiers, causes, plain)
    parts.append(Text(""))
    parts += render_scores(scores, score_row(monitor, tiers), width)
    parts.append(Text(FOOTNOTE, style="dim"))

    return Group(*parts)


def render_title(monitor, exit_code, width, plain):
    phase, mode, note = PHASES.get(monitor.act.get("act"),
                                   ("Waiting", "-", ""))
    text = Text(f" {phase}  ", style="bold")
    text.append(mode, style="bold cyan")
    text.append(f"   {note}", style="dim")
    if monitor.act.get("act"):
        status = (" ALIVE " if monitor.death is None else
                  f" {death_word(monitor, exit_code).upper()}"
                  f" at {monitor.death:.1f} s ")
        style = "bold black on green3" if monitor.death is None \
            else "bold white on red3"
        text.append(" " * max(2, width - len(text) - len(status) - 8))
        text.append("rproxy ")
        text.append(status, style="" if plain else style)
    text.truncate(width)

    return text


def render_chart(monitor, width):
    limit = monitor.act.get("memory_max", 1024 * MIB)
    top = limit * 1.15
    columns = max(10, width - CHART_AXIS - 1)
    buckets = chart_buckets(monitor, columns)
    death_column = None
    if monitor.death is not None:
        death_column = min(int(monitor.death / CHART_SECONDS_PER_COLUMN),
                           columns - 1) - chart_offset(monitor, columns)
    limit_row = min(CHART_ROWS - 1, int(limit / top * CHART_ROWS))
    lines = [chart_header(monitor, limit)]
    for row in range(CHART_ROWS - 1, -1, -1):
        label = f"{limit / MIB:.0f} MiB" if row == limit_row else \
            ("0" if row == 0 else "")
        line = Text(label.rjust(CHART_AXIS - 1) + " ", style="dim")
        for column in range(columns):
            if column == death_column:
                line.append("X" if row == CHART_ROWS - 1 else "|",
                            style="bold red")
                continue
            value = buckets[column] if column < len(buckets) else None
            line.append(*chart_cell(value, row, limit_row, top, limit))
        lines.append(line)
    seconds = (chart_offset(monitor, columns) + columns) \
        * CHART_SECONDS_PER_COLUMN
    lines.append(Text(" " * CHART_AXIS + f"time, {seconds:.0f} s across",
                      style="dim"))

    return lines


def chart_header(monitor, limit):
    header = Text(" rproxy memory  ", style="bold")
    if not monitor.memory:
        header.append("no sample yet", style="dim")
        return header
    current = monitor.memory[-1][1]
    style = "bold red" if current >= limit * 0.8 else "bold white"
    header.append(f"{current / MIB:.0f} MiB", style=style)
    header.append(f"   peak {monitor.peak / MIB:.0f} MiB", style="dim")
    header.append(f"   - - limit {limit / MIB:.0f} MiB = server memory"
                  " (scaled)", style="red")

    return header


def chart_offset(monitor, columns):
    """The first column on screen, so the chart scrolls on a long Act."""
    if not monitor.memory:
        return 0
    last = int(monitor.memory[-1][0] / CHART_SECONDS_PER_COLUMN)
    if monitor.death is not None:
        last = max(last, int(monitor.death / CHART_SECONDS_PER_COLUMN))

    return max(0, last - columns + 1)


def chart_buckets(monitor, columns):
    offset = chart_offset(monitor, columns)
    buckets = []
    for seconds, value in monitor.memory:
        column = int(seconds / CHART_SECONDS_PER_COLUMN) - offset
        if column < 0:
            continue
        while len(buckets) <= column:
            buckets.append(None)
        buckets[column] = max(buckets[column] or 0, value)

    return buckets


def chart_cell(value, row, limit_row, top, limit):
    """The text and the style of one cell of the memory chart."""
    if value is None or value <= 0:
        return ("-", "red") if row == limit_row else (" ",)
    fill = value / top * CHART_ROWS - row
    style = "red" if value >= limit * 0.8 else "white"
    if fill >= 1:
        return ("█", style)
    if fill > 0:
        return (BLOCKS[max(1, int(fill * 8))], style)
    if row == limit_row:
        return ("-", "red")
    if row == 0:
        return (BLOCKS[1], style)    # a small value still shows

    return (" ",)


def death_word(monitor, exit_code):
    if monitor.oom_kills or exit_code == "oom-kill":
        return "killed by OOM"
    if exit_code == "137":
        return "killed"
    if exit_code is None:
        return "gone"

    return f"exited ({exit_code})"


def render_pipe(monitor, tiers, width, plain):
    """The five buffers from the Origin to the Phone. Each bar is the fill
    of that buffer against its own size, split into slow and fast."""
    groups = tier_of_ports(tiers)
    relay_mode = monitor.act.get("relay", "")
    slow_count = sum(tier.count for tier in tiers[:-1])
    fast_count = tiers[-1].count if tiers else 0
    title = Text(" buffer pipe", style="bold")
    title.append("   relay buffer = read() − write()   fill vs own size  ",
                 style="dim")
    title.append_text(swatch(SLOW_COLOR, "s", plain))
    title.append(f" slow {slow_count}  ")
    title.append_text(swatch(FAST_COLOR, "f", plain))
    title.append(f" fast {fast_count}")
    lines = [title]
    bar_width = max(10, width - 60)
    for stage in ("origin send", "proxy recv"):
        lines.append(pipe_kernel_row(stage, monitor.kernel, groups,
                                     bar_width, plain))
    lines.append(pipe_relay_row(monitor.relay, groups, relay_mode,
                                bar_width, plain))
    for stage in ("proxy send", "client recv"):
        lines.append(pipe_kernel_row(stage, monitor.kernel, groups,
                                     bar_width, plain))
    lines += pipe_status(monitor, tiers, groups, relay_mode)

    return lines


def pipe_kernel_row(stage, kernel, groups, bar_width, plain):
    line = Text(f" {stage:<13}")
    sums = kernel.get(stage)
    if not sums or not sums["size"]:
        line.append("·" * bar_width, style="grey23")
        line.append("  no connection", style="dim")
        return line
    slow = sum(used for client, used in sums["clients"]
               if groups.get(client) == "slow")
    fast = sum(used for client, used in sums["clients"]
               if groups.get(client) == "fast")
    other = max(0, sums["used"] - slow - fast)
    line.append_text(split_bar(slow / sums["size"], fast / sums["size"],
                               bar_width, plain, other / sums["size"]))
    line.append(f" {sums['used'] / sums['size'] * 100:4.0f}%", style="bold")
    line.append(f"  {mib(sums['used'])} of {mib(sums['size'])}",
                style="dim")
    line.append(" · kernel limit", style="dim")

    return line


def pipe_relay_row(relay, groups, relay_mode, bar_width, plain):
    """rproxy's own memory. With back pressure it has a limit (the relay
    buffer, read() - write()); in the other modes it has none."""
    line = Text(f" {'relay buffer':<13}", style="bold")
    slow = sum(item["pending"] for item in relay
               if groups.get(item["port"]) == "slow")
    fast = sum(item["pending"] for item in relay
               if groups.get(item["port"]) == "fast")
    held = sum(item["pending"] for item in relay)
    size = sum(item["capacity"] for item in relay)
    if relay_mode == "backpressure":
        if size:
            line.append_text(split_bar(slow / size, fast / size, bar_width,
                                       plain))
            line.append(f" {held / size * 100:4.0f}%", style="bold")
            line.append(f"  {mib(held)} of {mib(size)}", style="dim")
        else:
            line.append("·" * bar_width + "      ", style="grey23")
        line.append(" · 64 KiB each: the limit", style="bold yellow")
        return line
    if held:
        line.append_text(split_bar(slow / held, fast / held, bar_width,
                                   plain))
    else:
        line.append("·" * bar_width, style="grey23")
    line.append(f" {mib(held):>9}", style="bold red" if held > 100 * MIB
                else "bold")
    line.append(" · whole reply, cut at 4 MiB" if relay_mode == "reply-cap"
                else " · whole reply, no limit, grows", style="red")

    return line


def pipe_status(monitor, tiers, groups, relay_mode):
    """One line in the same form for every mode: signal -> what rproxy
    does. Then the count that proves it."""
    relay = monitor.relay
    full_send = sum(1 for client, used in
                    monitor.kernel.get("proxy send", {}).get("clients", [])
                    if used > 0)
    signal = {
        "unbounded": f"proxy send full on {full_send} → rproxy reads the "
                     "Origin anyway → relay grows",
        "reply-cap": "reply > 4 MiB whole-reply limit → 502",
        "conn-cap": f"{sum(1 for item in relay if item['slot'])}/"
                    f"{monitor.act.get('relay_conns', 20)} slots held → "
                    "next request 503",
        "backpressure": "relay full (64 KiB) → rproxy stops reading the "
                        "Origin → the Origin waits",
    }.get(relay_mode, "")
    lines = [Text(" → " + signal, style="bold")]
    if relay_mode == "backpressure":
        paused = [item for item in relay if item["paused"]]
        lines.append(count_line("Origin read paused now", paused,
                                len(relay), groups))
        blocked = monitor.kernel.get("origin send", {}).get("blocked", [])
        oldest = oldest_age(blocked, tiers, monitor)
        text = Text(f"   {len(blocked)} Origin connections wait on write()",
                    style="dim")
        if oldest is not None:
            text.append(f" · oldest {oldest:.0f} s", style="dim")
        lines.append(text)
    elif relay_mode == "conn-cap":
        holders = [item for item in relay if item["slot"]]
        lines.append(count_line("slots held", holders, None, groups))

    return lines


def count_line(name, items, open_count, groups):
    slow = sum(1 for item in items if groups.get(item["port"]) == "slow")
    fast = sum(1 for item in items if groups.get(item["port"]) == "fast")
    other = len(items) - slow - fast
    text = Text(f"   {name}: {len(items)}", style="bold")
    if open_count is not None:
        text.append(f" of {open_count} open")
    text.append(f"  (slow {slow} · fast {fast}"
                + (f" · other {other})" if other else ")"))

    return text


def oldest_age(ports, tiers, monitor):
    """Seconds since the oldest of these Phones started."""
    starts = []
    for tier in tiers:
        for index, phone in enumerate(tier.phones):
            if phone.get("port") in ports:
                starts.append(tier.start_of(index))
    if not starts:
        return None
    now = monitor.act.get("started", 0) + monitor.seen

    return now - min(starts)


def render_grid(tiers, width, plain, file_bytes):
    """One cell for each Phone, by tier. Neighbours alternate between two
    shades of the state colour, so single cells stay apart."""
    columns = max(10, width - GRID_PREFIX - 1)
    lines = [Text(" Phones", style="bold")]
    lines[0].append("   1 cell = 1 Phone   speed  done", style="dim")
    fast = max(tiers, key=lambda tier: tier.rate) if tiers else None
    for tier in tiers:
        for row_start in range(0, max(tier.count, 1), columns):
            if row_start == 0:
                line = Text(f" {tier.label:>8} {tier.count:>3} ",
                            style="bold")
                line.append(tier_progress(tier, tier is fast, file_bytes),
                            style="dim")
            else:
                line = Text(" " * GRID_PREFIX)
            for index, state in enumerate(
                    tier.states[row_start:row_start + columns]):
                line.append_text(cell(state, row_start + index, plain))
            lines.append(line)

    return lines


def tier_progress(tier, is_fast, file_bytes):
    """The measured speed and the share done of a slow tier: 14 columns."""
    if is_fast:
        return " " * 14
    speeds = [phone.get("speed", 0) for phone in tier.phones
              if phone["state"] == "receiving"]
    done = sum(phone.get("bytes", 0) for phone in tier.phones
               if phone["state"] in ("receiving", "done", "cut"))
    share = done / (file_bytes * tier.count) * 100 \
        if file_bytes and tier.count else 0
    speed = f"{sum(speeds) / len(speeds) / 1000:4.0f} KB/s" if speeds \
        else "    - KB/s"

    return f"{speed} {share:2.0f}% "


def cell(state, index, plain):
    label, color, darker, char = STATES.get(state, STATES["cut"])
    if plain:
        return Text(char)

    return Text(" ", style=f"on {color if index % 2 == 0 else darker}")


def swatch(color, char, plain):
    return Text(char if plain else " ", style="" if plain else f"on {color}")


def render_legend(monitor, tiers, causes, plain):
    """Each state with its count. A failed state names its causes, from the
    "cause" lines of rproxy; a cut after the death of rproxy is "rproxy
    died"."""
    text = Text(" ")
    for state in STATE_ORDER:
        count = sum(tier.total(state) for tier in tiers)
        if not count and state in ("http_other", "refused", "cut"):
            continue
        label, color, _, char = STATES[state]
        text.append_text(swatch(color, char, plain))
        text.append(f" {label} {count}  ")
    lines = [text]
    reasons = cause_text(monitor, tiers, causes)
    if reasons:
        lines.append(Text("   why: " + reasons, style="dim"))

    return lines


def cause_text(monitor, tiers, causes):
    parts = []
    for status, state in (("502", "http_502"), ("503", "http_503")):
        count = sum(tier.total(state) for tier in tiers)
        if count:
            named = {cause: n for (code, cause), n in causes.items()
                     if code == status}
            parts.append(f"{status} " + (", ".join(
                f"{cause} {n}" for cause, n in named.items())
                or "cause unknown"))
    cuts = sum(tier.total("cut") for tier in tiers)
    if cuts:
        timed_out = min(cuts, sum(n for (code, cause), n in causes.items()
                                  if cause == "client-timeout"))
        rest = cuts - timed_out
        words = [f"client-timeout {timed_out}"] if timed_out else []
        if rest:
            words.append(f"rproxy died {rest}" if monitor.death is not None
                         else f"cause unknown {rest}")
        parts.append("cut " + ", ".join(words))

    return "   ".join(parts)


def score_row(monitor, tiers):
    """The row of this Act for the score table, as it is now."""
    act = monitor.act
    number = act.get("act")
    if not number or not tiers:
        return None
    counts = {state: sum(tier.total(state) for tier in tiers)
              for state in STATE_ORDER}
    phones = sum(tier.count for tier in tiers)
    file_mb = act.get("file_bytes", 0) / 1e6
    ceiling = {
        "unbounded": f"{phones} × {file_mb:.0f} MB",
        "reply-cap": f"{phones} × 4 MiB",
        "conn-cap": f"{act.get('relay_conns', 20)} × {file_mb:.0f} MB",
        "backpressure": f"{phones} × 64 KiB",
    }.get(act.get("relay"), "-")
    note = ""
    if monitor.death is not None:
        note = f"OOM at {monitor.death:.0f} s"
    elif act.get("relay") == "backpressure":
        note = "Origin waits"

    return {"act": number, "at": round(monitor.seen),
            "peak_mib": round(monitor.peak / MIB), "ceiling": ceiling,
            "counts": {k: v for k, v in counts.items() if v},
            "note": note}


def render_scores(scores, live, width):
    lines = [Text(f" {'Phase':<10}{'mode':<13}{'at':>5}{'peak':>10}"
                  f"  {'ceiling':<14}Phones, sum 200", style="bold")]
    rows = [(row, False) for row in scores]
    if live:
        rows.append((live, True))
    for row, is_live in rows:
        phase, mode, _ = PHASES.get(row["act"], ("?", "?", ""))
        counts = " ".join(f"{SHORT[state]} {n}" for state, n in
                          row["counts"].items())
        text = Text(f" {phase:<10}{mode:<13}{row['at']:>4}s"
                    f"{row['peak_mib']:>6} MiB  {row['ceiling']:<14}",
                    style="bold" if is_live else "")
        text.append(counts)
        if row["note"]:
            text.append(f" · {row['note']}", style="dim")
        text.truncate(width)
        lines.append(text)

    return lines


def scores_save(run_dir, monitor):
    """Keep the last row of an Act when the next Act starts."""
    row = score_row(monitor, monitor.tiers or [])
    if row is None:
        return
    path = os.path.join(run_dir, "scores.json")
    scores = [old for old in (read_json(path) or [])
              if old.get("act") != row["act"]]
    scores.append(row)
    temp = path + ".tmp"
    with open(temp, "w") as file:
        json.dump(scores, file)
    os.replace(temp, path)


def mib(value):
    if value >= 10 * MIB:
        return f"{value / MIB:.0f} MiB"
    if value >= MIB:
        return f"{value / MIB:.1f} MiB"

    return f"{value / 1024:.0f} KiB"


def split_bar(slow_share, fast_share, width, plain, other_share=0.0):
    """A bar in three parts: slow, fast, and bytes of no known Phone (a
    socket that the relay stats do not list yet)."""
    text = Text()
    used = 0
    for share, color, char in ((slow_share, SLOW_COLOR, "s"),
                               (fast_share, FAST_COLOR, "f"),
                               (other_share, "grey50", "o")):
        count = min(width - used, round(share * width))
        if count > 0:
            text.append(char * count if plain else " " * count,
                        style="" if plain else f"on {color}")
            used += count
    text.append("·" * (width - used), style="grey23")

    return text


def act_key(act):
    return (act or {}).get("started")


def main():
    args = parse_args()
    act_path = os.path.join(args.run, "act.json")
    monitor = Monitor(read_json(act_path))

    def frame(plain, width):
        nonlocal monitor
        act = read_json(act_path)
        if act_key(act) != act_key(monitor.act):
            scores_save(args.run, monitor)
            monitor = Monitor(act)                 # a new Act: reset
        exit_code = monitor.poll(args.run)
        tiers = tiers_read(args.run, act)
        # After rproxy dies, show the Phones as they were at that moment.
        if monitor.death is None or monitor.tiers is None:
            monitor.tiers = tiers
        scores = [row for row in read_json(
            os.path.join(args.run, "scores.json")) or []
            if row.get("act") != (act or {}).get("act")]

        return render(monitor, monitor.tiers, exit_code,
                      causes_read(args.run), scores, width, plain)

    if args.once:
        end = time.time() + args.watch
        while time.time() < end:
            frame(True, args.width)
            time.sleep(POLL_PERIOD)
        console = Console(width=args.width, no_color=True,
                          force_terminal=False)
        console.print(frame(True, args.width))
        return
    console = Console()
    with Live(frame(False, console.width), console=console, screen=True,
              auto_refresh=False) as live:
        while True:
            time.sleep(POLL_PERIOD)
            live.update(frame(False, console.width), refresh=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
