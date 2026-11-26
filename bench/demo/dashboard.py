#!/usr/bin/env python3
"""The terminal dashboard of the Back pressure demo (SPEC 12, FINAL-10k).

One skeleton for every scene of the talk. Each scene makes one block large
and bright, and dims the others:

  header        the Phase, the Relay mode, Phones per second, the scaling
  formula       Relay buffer = read() from the Origin - write() to the Phone
  bystander     one small page through rproxy, and how long it waits
  memory        rproxy memory over time: the base of the open connections
                (dim) and the Relay buffer bytes on top (bright)
  relay buffer  the bytes that rproxy holds, and its limit in this mode
  Phones        one bar for all Phones: done, receiving, queued, failed
  rproxy        the connections of each Event loop
  Origin        the Origin threads: asleep in write(), idle, running.
                Phase 4 draws each thread and compares with Phase 3
  scores        one row for each Phase

It reads the run directory that demo.sh writes:
  act.json          the Act: Relay mode, rproxy pid and cgroup, the values
  scene             the scene of `present` (P0, P1, 2A, 2B, P3, P4)
  origin.pid        the Origin; its threads come from /proc/<pid>/task
  status-*.json     the Phones of one phones.py process (bench/demo/phones.py)
  relay/loop-*.txt  one line for each rproxy connection: "client_port
                    origin_port pending capacity paused slot queued"
  origin.stats      "connections C workers W busy B" (server --stats)
  rproxy.log        the "cause" lines: why a reply failed
  rproxy.exit       the exit code of rproxy, or "oom-kill"
  bystander.json    the last bystander curl (bench/demo/bystander.sh)
  scores.json       one row for each Act that ended (this file writes it)

Usage: bench/demo/dashboard.py [--run DIR] [--once [--watch S] [--width N]]
       [--scene NAME]
"""
import argparse
import glob
import json
import os
import time

from rich.console import Console, Group
from rich.live import Live
from rich.text import Text

POLL_PERIOD = 0.2          # seconds between two frames
CHART_ROWS_SMALL = 5
CHART_ROWS_LARGE = 12
CHART_SECONDS_PER_COLUMN = 0.5
CHART_AXIS = 10            # columns of the axis labels
MIB = 1024 * 1024
BLOCKS = " ▁▂▃▄▅▆▇█"
LABEL = 14                 # columns of a block label
BAR_RIGHT = 34             # columns right of a bar for its numbers

# The scenes of the talk. act: the Act that runs it. focus: the blocks that
# are large and bright.
SCENES = {
    "P0": {"act": 1, "title": "Phase 0  평소", "note": "rproxy up, no Phones",
           "focus": {"bystander"}},
    "P1": {"act": 1, "title": "Phase 1  unbounded",
           "note": "no limit",
           "focus": {"memory", "relay"}},
    "2A": {"act": 2, "title": "Phase 2-A  memory-cap",
           "note": "a cap on all reply bytes, 502 over it",
           "focus": {"memory", "phones"}},
    "2B": {"act": 3, "title": "Phase 2-B  conn-cap",
           "note": "a cap on the connections, a queue for the rest",
           "focus": {"bystander", "phones"}},
    "P3": {"act": 4, "title": "Phase 3  backpressure",
           "note": "read the Origin only when there is room",
           "focus": {"memory", "relay"}},
    "P4": {"act": 5, "title": "Phase 4  pool",
           "note": "the same load; a slow Phone holds a Worker thread",
           "focus": {"origin"}},
}
SCENE_OF_ACT = {1: "P1", 2: "2A", 3: "2B", 4: "P3", 5: "P4"}

# The parts of the Phone bar, in order: label, colour, plain char.
PARTS = {
    "done": ("done", "green3", "#"),
    "receiving": ("receiving", "dodger_blue1", ">"),
    "queued": ("queued in rproxy", "gold1", "q"),
    "http_502": ("502", "red1", "2"),
    "http_503": ("503", "orange1", "3"),
    "http_other": ("other code", "red3", "o"),
    "cut": ("cut", "magenta1", "x"),
    "refused": ("refused", "dark_red", "r"),
    "waiting": ("not started", "grey30", "."),
}
BASE_COLOR = "grey42"      # the memory of the open connections
RELAY_COLOR = "bold white"  # the Relay buffer bytes on top
LIMIT_COLOR = "red"

# The Origin threads by what the kernel says they wait in (wchan).
# wait_woken: a send() that waits for room in the socket send buffer.
# futex_do_wait: a Worker that waits for a connection, or a lock.
# The order is the order of the grid: the few that work first, bright.
THREAD_KINDS = {
    "running": ("running", "bold green3", "R"),
    "write": ("asleep in write()", "dark_goldenrod", "z"),
    "idle": ("asleep, no connection", "grey42", "."),
    "other": ("other wait", "grey30", "o"),
}
THREAD_WCHAN = {"wait_woken": "write", "futex_do_wait": "idle"}
THREAD_POLL_PERIOD = 1.0   # seconds; 10k threads take a moment to read
THREADS_PER_CELL = 50
CELL = "● "                # one cell and its gap, so each one reads as one


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--run", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "run"))
    parser.add_argument("--once", action="store_true",
                        help="print one frame with no color and exit")
    parser.add_argument("--watch", type=float, default=0.0,
                        help="with --once: poll for S seconds first")
    parser.add_argument("--width", type=int, default=113,
                        help="width of the --once frame")
    parser.add_argument("--scene", help="show this scene, not the one of "
                        "the run directory")

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


def file_time(path):
    try:
        return os.stat(path).st_mtime
    except OSError:
        return 0.0


class Monitor:
    """What the dashboard remembers between two frames of one Act. After
    rproxy dies, every block shows the last frame before the death."""

    def __init__(self, act):
        self.act = act or {}
        self.memory = []      # (seconds since act start, bytes, relay bytes)
        self.peak = 0
        self.oom_kills = 0
        self.death = None     # seconds since act start when rproxy died
        self.was_dead_at_start = False
        self.relay = []       # lines of the relay stats, as dicts
        self.loops = []       # connections of each Event loop
        self.phones = {}      # state -> count, frozen at the death
        self.seen = 0.0       # seconds since act start of the last frame
        self.polls = 0
        self.exit_code = None
        self.threads = None   # the last threads_read(), or None
        # The reading with the most RSS: at the end of an Act the Origin
        # stops and frees its memory, so the last reading is too small.
        self.threads_peak = None
        self.threads_time = 0.0

    def poll(self, run_dir):
        start = self.act.get("started", time.time())
        now = time.time() - start
        pid = self.act.get("rproxy_pid")
        self.polls += 1
        if pid and self.death is None:
            if self.is_alive(pid):
                self.relay, self.loops = relay_read(run_dir)
                self.poll_memory(now)
                self.phones = phones_read(run_dir, self.act, self.relay)
                self.seen = now
            else:
                self.death = self.death_time(run_dir, start, now)
                self.was_dead_at_start = self.polls == 1
        if not self.phones:
            self.phones = phones_read(run_dir, self.act, self.relay)
        if self.death is None and \
                time.time() - self.threads_time >= THREAD_POLL_PERIOD:
            self.threads = threads_read(run_dir) or self.threads
            if self.threads and (self.threads_peak is None or
                                 self.threads["rss"] >
                                 self.threads_peak["rss"]):
                self.threads_peak = self.threads
            self.threads_time = time.time()

        self.exit_code = read_text(os.path.join(run_dir, "rproxy.exit"))

        return self.exit_code

    @staticmethod
    def death_time(run_dir, start, now):
        """demo.sh writes rproxy.exit when rproxy stops: use its time."""
        stamp = file_time(os.path.join(run_dir, "rproxy.exit"))

        return stamp - start if stamp else now

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
            relay = sum(item["pending"] for item in self.relay)
            self.memory.append((now, int(current), relay))
            self.peak = max(self.peak, int(current))

    def death_word(self, exit_code):
        if self.oom_kills or exit_code == "oom-kill":
            return "killed by OOM"
        if exit_code == "137":
            return "killed"
        if exit_code is None:
            return "stopped"

        return f"exited ({exit_code})"


def relay_read(run_dir):
    """The relay stats of all loops: one dict for each connection, and the
    connection count of each loop."""
    lines = []
    loops = []
    for path in sorted(glob.glob(os.path.join(run_dir, "relay",
                                              "loop-*.txt"))):
        count = 0
        for line in (read_text(path) or "").splitlines():
            fields = line.split()
            if line.startswith("#") or len(fields) not in (6, 7):
                continue
            try:
                values = list(map(int, fields)) + [0]
            except ValueError:
                continue
            count += 1
            lines.append({"port": values[0], "pending": values[2],
                          "capacity": values[3], "paused": values[4] == 1,
                          "slot": values[5] == 1, "queued": values[6] == 1})
        loops.append(count)

    return lines, loops


def phones_read(run_dir, act, relay):
    """The Phone count of each state, from the totals of all status files
    of this Act. A receiving Phone whose connection waits in the conn-cap
    queue of rproxy counts as queued."""
    counts = {state: 0 for state in PARTS}
    started = (act or {}).get("started", 0)
    for path in glob.glob(os.path.join(run_dir, "status-*.json")):
        status = read_json(path)
        if not status or status.get("start", 0) < started:
            continue                        # left from the last Act
        for state, count in status.get("totals", {}).items():
            counts[state if state in counts else "http_other"] += count
    queued = min(counts["receiving"], sum(1 for item in relay
                                          if item["queued"]))
    counts["receiving"] -= queued
    counts["queued"] = queued
    started_phones = sum(counts.values())
    counts["waiting"] += max(0, (act or {}).get("phones", 0) - started_phones)

    return counts


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


def origin_read(run_dir):
    """The last line of server --stats, as a dict, or None."""
    fields = (read_text(os.path.join(run_dir, "origin.stats")) or "").split()
    try:
        return {fields[i]: int(fields[i + 1])
                for i in range(0, len(fields) - 1, 2)}
    except ValueError:
        return None


def threads_read(run_dir):
    """The Origin threads: the count of each kind, and the RSS. None if
    the Origin is not running."""
    pid = (read_text(os.path.join(run_dir, "origin.pid")) or "").strip()
    status = read_text(f"/proc/{pid}/status") if pid.isdigit() else None
    if status is None:
        return None
    counts = {kind: 0 for kind in THREAD_KINDS}
    for task in glob.glob(f"/proc/{pid}/task/*"):
        stat = read_text(os.path.join(task, "stat"))
        if stat is None:
            continue
        if stat.rsplit(")", 1)[1].split()[0] == "R":
            counts["running"] += 1
            continue
        wchan = read_text(os.path.join(task, "wchan")) or ""
        counts[THREAD_WCHAN.get(wchan, "other")] += 1
    rss = 0
    for line in status.splitlines():
        if line.startswith("VmRSS:"):
            rss = int(line.split()[1]) * 1024

    return {"counts": counts, "total": sum(counts.values()), "rss": rss}


def scene_get(run_dir, act, forced):
    """The scene: --scene, then the scene file of `present` if it is newer
    than the Act, then the scene of the Act."""
    if forced:
        return forced
    path = os.path.join(run_dir, "scene")
    name = read_text(path)
    started = (act or {}).get("started", 0)
    if name in SCENES and file_time(path) >= started - 1:
        return name
    if (act or {}).get("held"):
        return "P0"

    return SCENE_OF_ACT.get((act or {}).get("act"), "P1")


# --- render -----------------------------------------------------------------

def render(view):
    """view: a dict with monitor, scene, exit_code, causes, scores,
    bystander, origin, width, plain."""
    focus = SCENES[view["scene"]]["focus"]
    blocks = [
        ("header", render_header(view)),
        ("formula", render_formula(view)),
        ("bystander", render_bystander(view, "bystander" in focus)),
        ("memory", render_memory(view, "memory" in focus)),
        ("relay", render_relay(view)),
        ("phones", render_phones(view)),
        ("loops", render_loops(view)),
        ("origin", render_origin(view)),
        ("scores", render_scores(view)),
    ]
    parts = []
    for name, lines in blocks:
        is_dim = name not in focus and name not in ("header", "formula")
        if name in ("memory", "phones", "loops", "scores"):
            parts.append(Text(""))
        for line in lines:
            if is_dim and not view["plain"]:
                line.stylize("dim")
            parts.append(line)
    parts.append(Text(""))
    parts.append(Text(footnote(view["monitor"].act), style="dim"))

    return Group(*parts)


def block_title(name, is_focus, plain):
    mark = ">> " if is_focus and plain else ("▶ " if is_focus else "  ")
    text = Text(f" {mark}{name}", style="bold" if is_focus else "")

    return text


def render_header(view):
    monitor = view["monitor"]
    scene = SCENES[view["scene"]]
    act = monitor.act
    text = Text(f" {scene['title']}", style="bold")
    text.append(f"   {scene['note']}", style="dim")
    status = rproxy_status(monitor, view["exit_code"])
    if status:
        word, style = status
        text.append(" " * max(2, view["width"] - text.cell_len - len(word)
                              - 9))
        text.append("rproxy ")
        text.append(word, style="" if view["plain"] else style)
    text.truncate(view["width"])

    return [text]


def render_formula(view):
    act = view["monitor"].act
    text = Text(" Relay buffer = read() from the Origin − write() to the"
                " Phone", style="bold cyan")
    if act.get("act") and not act.get("held"):
        load = (f"{act.get('phones', 0):,} Phones start at"
                f" {act.get('phone_rate', 0)}/s")
        text.append(" " * max(2, view["width"] - text.cell_len - len(load)
                              - 1))
        text.append(load, style="dim")

    return [text]


def rproxy_status(monitor, exit_code):
    if not monitor.act.get("act"):
        return None
    if monitor.death is None:
        return " ALIVE ", "bold black on green3"
    if monitor.was_dead_at_start:
        return " ENDED EARLIER ", "bold white on grey30"
    word = f" {monitor.death_word(exit_code).upper()} at {monitor.death:.1f} s "

    return word, "bold white on red3"


def render_bystander(view, is_focus):
    """The last bystander curl of this Act. While it waits, the seconds of
    the wait, large when the scene is about waiting."""
    monitor = view["monitor"]
    bystander = view["bystander"]
    text = block_title("bystander", is_focus, view["plain"])
    text.append("   GET /index.html  ")
    started = monitor.act.get("started", 0)
    if not bystander or bystander.get("start", bystander.get("time", 0)) \
            < started - 1:
        text.append("-", style="dim")
        return [text]
    if bystander.get("pending"):
        waited = time.time() - bystander.get("start", time.time())
        text.append(f" waiting {waited:4.1f} s ",
                    style="" if view["plain"] else "bold black on gold1")
        lines = [text]
        if is_focus:
            lines.append(Text(f"     still no page after {waited:.0f} s",
                              style="bold gold1"))
        return lines
    style = "bold black on green3" if bystander.get("ok") \
        else "bold white on red3"
    text.append(f" {bystander.get('line', '?')} ",
                style="" if view["plain"] else style)
    text.append(f"   at {bystander['time'] - started:.0f} s", style="dim")

    return [text]


def render_memory(view, is_focus):
    monitor = view["monitor"]
    width = view["width"]
    rows = CHART_ROWS_LARGE if is_focus else CHART_ROWS_SMALL
    limit = monitor.act.get("memory_max", 4096 * MIB)
    top = limit * 1.1
    columns = max(10, width - CHART_AXIS - 1)
    offset = chart_offset(monitor, columns)
    buckets = chart_buckets(monitor, columns, offset)
    limit_row = min(rows - 1, int(limit / top * rows))
    death_column = None
    if monitor.death is not None and not monitor.was_dead_at_start:
        death_column = min(int(monitor.death / CHART_SECONDS_PER_COLUMN)
                           - offset, columns - 1)
    lines = [memory_header(monitor, limit, is_focus, view["plain"])]
    for row in range(rows - 1, -1, -1):
        label = f"{limit / MIB / 1024:.0f} GiB" if row == limit_row else \
            ("0" if row == 0 else "")
        line = Text(label.rjust(CHART_AXIS - 1) + " ", style="dim")
        for column in range(columns):
            if column == death_column:
                line.append("X" if row == rows - 1 else "|", style="bold red")
                continue
            value = buckets[column] if column < len(buckets) else None
            line.append(*chart_cell(value, row, rows, limit_row, top))
        lines.append(line)
    seconds = (offset + columns) * CHART_SECONDS_PER_COLUMN
    lines.append(Text(" " * CHART_AXIS + f"time, {seconds:.0f} s across",
                      style="dim"))

    return lines


def memory_header(monitor, limit, is_focus, plain):
    header = block_title("rproxy memory", is_focus, plain)
    if not monitor.memory:
        header.append("   no sample yet", style="dim")
        return header
    _, current, relay = monitor.memory[-1]
    style = "bold red" if current >= limit * 0.8 else "bold"
    header.append(f"   {mib(current)}", style=style)
    header.append(f"  peak {mib(monitor.peak)}", style="dim")
    header.append("    ")
    header.append("▒" if plain else "█", style=BASE_COLOR)
    header.append(f" connections {mib(current - relay)}  ")
    header.append("█", style=RELAY_COLOR)
    header.append(f" Relay buffer {mib(relay)}  ")
    header.append(f"- - limit {limit / MIB / 1024:.0f} GiB", style=LIMIT_COLOR)

    return header


def chart_offset(monitor, columns):
    """The first column on screen, so the chart scrolls on a long Act."""
    if not monitor.memory:
        return 0
    last = int(monitor.memory[-1][0] / CHART_SECONDS_PER_COLUMN)
    if monitor.death is not None:
        last = max(last, int(monitor.death / CHART_SECONDS_PER_COLUMN))

    return max(0, last - columns + 1)


def chart_buckets(monitor, columns, offset):
    """One (total, relay) for each column: the sample with the most memory."""
    buckets = []
    for seconds, total, relay in monitor.memory:
        column = int(seconds / CHART_SECONDS_PER_COLUMN) - offset
        if column < 0:
            continue
        while len(buckets) <= column:
            buckets.append(None)
        if buckets[column] is None or total > buckets[column][0]:
            buckets[column] = (total, relay)

    return buckets


def chart_cell(value, row, rows, limit_row, top):
    """One cell: dim for the open connections, bright for the Relay buffer
    bytes above them."""
    if value is None:
        return ("-", LIMIT_COLOR) if row == limit_row else (" ",)
    total, relay = value
    base = max(0, total - relay)
    fill = total / top * rows - row
    if fill <= 0:
        return ("-", LIMIT_COLOR) if row == limit_row else (" ",)
    is_base = (row + 1) <= base / top * rows
    style = BASE_COLOR if is_base else RELAY_COLOR
    if fill >= 1:
        return ("█", style)

    return (BLOCKS[max(1, int(fill * 8))], style)


def render_relay(view):
    """The Relay buffer bytes and the limit of this mode."""
    monitor = view["monitor"]
    act = monitor.act
    is_focus = "relay" in SCENES[view["scene"]]["focus"]
    relay = monitor.relay
    held = sum(item["pending"] for item in relay)
    mode = act.get("relay", "")
    text = block_title("Relay buffer", is_focus, view["plain"])
    bar_width = max(10, view["width"] - LABEL - BAR_RIGHT - 4)
    line = Text(" " * LABEL)
    if mode == "backpressure":
        room = sum(item["capacity"] for item in relay)
        paused = sum(1 for item in relay if item["paused"])
        line.append_text(bar([(held, "cyan")], room, bar_width, view["plain"]))
        line.append(f"  {mib(held)} of {len(relay):,} × "
                    f"{act.get('relay_buffer', 0) // 1024} KiB")
        text.append(f"   at most {act.get('relay_buffer', 0) // 1024} KiB for"
                    f" each connection · Origin read paused on {paused:,}",
                    style="bold")
        return [text, line]
    if mode == "memory-cap":
        cap = act.get("relay_memory", 0) * MIB
        line.append_text(bar([(held, "cyan")], cap, bar_width,
                             view["plain"]))
        line.append(f"  {mib(held)} of the {mib(cap)} cap")
        text.append("   the whole reply of each connection · new exchanges"
                    " get 502 over the cap", style="bold")
        return [text, line]
    holders = sum(1 for item in relay if item["pending"] > 0)
    text.append(f"   the whole reply of each connection · no limit ·"
                f" {mib(held)} in {holders:,} connections",
                style="bold red" if held > MIB * 1024 else "bold")
    if mode == "conn-cap":
        slots = sum(1 for item in relay if item["slot"])
        queued = sum(1 for item in relay if item["queued"])
        text.append(f"   slots {slots:,}/{act.get('relay_conns', 0):,}"
                    f" · queued {queued:,}", style="bold gold1")

    return [text]


def render_phones(view):
    """One bar for all Phones, and the count of each part."""
    monitor = view["monitor"]
    counts = monitor.phones
    total = max(1, sum(counts.values()))
    is_focus = "phones" in SCENES[view["scene"]]["focus"]
    text = block_title("Phones", is_focus, view["plain"])
    text.append(f"   {total:,} Phones", style="dim")
    bar_width = max(10, view["width"] - LABEL - 4)
    parts = [(counts.get(state, 0), PARTS[state][1]) for state in PARTS]
    line = Text(" " * LABEL)
    line.append_text(bar(parts, total, bar_width, view["plain"],
                         [PARTS[state][2] for state in PARTS]))
    legend = Text(" " * LABEL)
    for state, (label, color, char) in PARTS.items():
        count = counts.get(state, 0)
        if not count and state not in ("done", "receiving", "waiting"):
            continue
        legend.append(char if view["plain"] else "■", style=color)
        legend.append(f" {label} {count:,}   ")
    lines = [text, line, legend]
    why = cause_text(monitor, counts, view["causes"])
    if why:
        lines.append(Text(" " * LABEL + "why: " + why, style="dim"))

    return lines


def cause_text(monitor, counts, causes):
    parts = []
    for status, state in (("502", "http_502"), ("503", "http_503")):
        if counts.get(state):
            named = {cause: n for (code, cause), n in causes.items()
                     if code == status}
            parts.append(f"{status} " + (", ".join(
                f"{cause} {n:,}" for cause, n in named.items())
                or "cause unknown"))
    if counts.get("cut") and monitor.death is not None:
        parts.append("cut: rproxy died")
    if counts.get("refused") and monitor.death is not None:
        parts.append("refused: rproxy died")

    return "   ".join(parts)


def render_loops(view):
    loops = view["monitor"].loops
    is_focus = "loops" in SCENES[view["scene"]]["focus"]
    text = block_title("rproxy", is_focus, view["plain"])
    if not loops:
        text.append("   no Event loop stats", style="dim")
        return [text]
    text.append(f"   {len(loops)} Event loops (threads) hold"
                f" {sum(loops):,} connections:  ")
    text.append(" ".join(f"{count:,}" for count in loops), style="cyan")
    text.truncate(view["width"])

    return [text]


def render_origin(view):
    monitor = view["monitor"]
    act = monitor.act
    is_focus = "origin" in SCENES[view["scene"]]["focus"]
    text = block_title("Origin", is_focus, view["plain"])
    text.append(f"   {act.get('origin_model', '?')}", style="bold")
    threads = monitor.threads
    if not threads:
        text.append("   not running", style="dim")
        return [text]
    text.append_text(threads_summary(threads, view["plain"]))
    stats = view["origin"]
    if stats and act.get("origin_model") != "pool":
        # pool writes no --stats; epoll-pool counts the sockets it holds.
        text.append(f" · {stats.get('connections', 0):,} sockets")
    text.truncate(view["width"])
    if not is_focus:
        return [text]
    model = act.get("origin_model", "?")
    lines = [text, Text(""), threads_headline(threads, view["plain"]),
             Text(f"   ● = {THREADS_PER_CELL} threads", style="dim")]
    columns = max(10, (view["width"] - LABEL - 12) // len(CELL))
    lines += threads_grid(f"Phase 4  {model}", threads, columns,
                          view["plain"])
    before = next((row for row in view["scores"]
                   if row.get("scene") == "P3" and row.get("origin")), None)
    if before:
        lines += threads_grid("Phase 3  epoll-pool", before["origin"],
                              columns, view["plain"])
    lines.append(Text(""))
    lines += rss_bars(threads, before, model, view)

    return lines


def threads_summary(threads, plain):
    text = Text(f"   {threads['total']:,} threads: ")
    for kind, (label, color, char) in THREAD_KINDS.items():
        count = threads["counts"].get(kind, 0)
        if not count and kind != "write":
            continue
        text.append(char if plain else "■", style=color)
        text.append(f" {label} {count:,}  ",
                    style="bold" if kind == "write" and count else "")
    text.append(f" RSS {mib(threads['rss'])}")

    return text


def threads_headline(threads, plain):
    """How many threads sleep, and how few work."""
    counts = threads["counts"]
    asleep = counts.get("write", 0) + counts.get("idle", 0)
    share = asleep * 100 // max(1, threads["total"])
    text = Text("   ")
    text.append(f" {asleep:,} of {threads['total']:,} threads asleep"
                f" ({share} %) ",
                style="" if plain else "bold black on gold1")
    text.append("   ")
    text.append(f" {counts.get('running', 0):,} running ",
                style="" if plain else "bold black on green3")
    text.append(f"   {counts.get('write', 0):,} hold a Phone in write()",
                style="bold")

    return text


def threads_grid(title, threads, columns, plain):
    """One cell for each THREADS_PER_CELL threads, in the order of
    THREAD_KINDS, `columns` cells on a row."""
    cells = []
    for kind, (_, color, char) in THREAD_KINDS.items():
        count = threads["counts"].get(kind, 0)
        cells += [(char, color)] * -(-count // THREADS_PER_CELL)
    lines = []
    for start in range(0, max(1, len(cells)), columns):
        label = title if start == 0 else ""
        line = Text(f"   {label:<{LABEL + 8}}", style="bold" if label else "")
        for char, color in cells[start:start + columns]:
            line.append(char + " " if plain else CELL, style=color)
        lines.append(line)

    return lines


def rss_bars(threads, before, model, view):
    """The Origin RSS now and in Phase 3, on one scale."""
    rows = [(f"Phase 4  {model}", threads["rss"])]
    if before:
        rows.append(("Phase 3  epoll-pool", before["origin"]["rss"]))
    top = max(rss for _, rss in rows) or 1
    width = max(10, view["width"] - LABEL - 8 - 16)
    lines = [Text("   Origin memory (RSS; Phase 3 at its peak)",
                  style="bold")]
    for title, rss in rows:
        line = Text(f"   {title:<{LABEL + 8}}")
        line.append_text(bar([(rss, "grey62")], top, width, view["plain"]))
        line.append(f"  {mib(rss)}")
        lines.append(line)

    return lines


def score_row(monitor, scene):
    """The row of this Act for the score table, as it is now."""
    act = monitor.act
    if not act.get("act") or act.get("held") or monitor.was_dead_at_start:
        return None
    counts = monitor.phones
    failed = {state: n for state, n in counts.items()
              if n and state in ("http_502", "http_503", "http_other", "cut",
                                 "refused")}
    note = ""
    if monitor.death is not None:
        word = monitor.death_word(monitor.exit_code)
        if word != "stopped":    # demo.sh stopped it for the next Act
            note = f"{word} at {monitor.death:.0f} s"

    return {"scene": scene, "at": round(monitor.seen),
            "peak_mib": round(monitor.peak / MIB),
            "done": counts.get("done", 0),
            "open": counts.get("receiving", 0) + counts.get("queued", 0),
            "failed": failed, "note": note, "origin": monitor.threads_peak}


def render_scores(view):
    is_focus = "scores" in SCENES[view["scene"]]["focus"]
    lines = [block_title("Phases", is_focus, view["plain"])]
    lines[0].append(f"   {'':10}{'at':>6}{'peak':>11}{'done':>8}"
                    f"{'open':>8}   failed", style="bold")
    # A row of the dashboard before FINAL-10k has no "failed": skip it.
    rows = [(row, False) for row in view["scores"] if "failed" in row]
    live = score_row(view["monitor"], view["scene"])
    if live:
        rows.append((live, True))
    for row, is_live in rows:
        title = SCENES.get(row.get("scene"), {}).get("title", "?")
        failed = " ".join(f"{PARTS[state][0]} {n:,}"
                          for state, n in row["failed"].items()) or "-"
        text = Text(f"   {title:<24}{row['at']:>4} s{row['peak_mib']:>7} MiB"
                    f"{row['done']:>8,}{row['open']:>8,}   {failed}",
                    style="bold" if is_live else "")
        if row["note"]:
            text.append(f"   {row['note']}", style="red")
        text.truncate(view["width"])
        lines.append(text)

    return lines


def footnote(act):
    return (f" speeds = KR carrier QoS caps ÷ {act.get('speed_scale', 1)};"
            " tier shares = assumption; server memory is scaled")


def mib(value):
    if value >= 10 * MIB:
        return f"{value / MIB:,.0f} MiB"
    if value >= MIB:
        return f"{value / MIB:.1f} MiB"

    return f"{value / 1024:.0f} KiB"


def bar(parts, total, width, plain, chars=None):
    """A bar of `width` cells: each part (count, colour) takes its share of
    `total`. A part with a count gets at least one cell."""
    text = Text()
    used = 0
    for index, (count, color) in enumerate(parts):
        if count <= 0 or total <= 0:
            continue
        cells = max(1, round(count / total * width))
        cells = min(cells, width - used)
        char = (chars[index] if chars else "#") if plain else " "
        text.append(char * cells, style="" if plain else f"on {color}")
        used += cells
    text.append("·" * (width - used), style="grey23")

    return text


# --- main -------------------------------------------------------------------

def scores_save(run_dir, monitor, scene):
    """Keep the last row of an Act when the next Act starts."""
    row = score_row(monitor, scene)
    if row is None:
        return
    path = os.path.join(run_dir, "scores.json")
    scores = [old for old in (read_json(path) or [])
              if old.get("scene") != row["scene"]]
    scores.append(row)
    temp = path + ".tmp"
    with open(temp, "w") as file:
        json.dump(scores, file)
    os.replace(temp, path)


def scores_clear(run_dir):
    try:
        os.remove(os.path.join(run_dir, "scores.json"))
    except FileNotFoundError:
        pass


def main():
    args = parse_args()
    act_path = os.path.join(args.run, "act.json")
    monitor = Monitor(read_json(act_path))
    last_scene = [None]

    def frame(plain, width):
        nonlocal monitor
        act = read_json(act_path)
        if (act or {}).get("started") != monitor.act.get("started"):
            if args.once:
                pass                     # a check frame changes no file
            elif (act or {}).get("held"):
                scores_clear(args.run)   # Phase 0: a new `present`
            elif last_scene[0]:
                scores_save(args.run, monitor, last_scene[0])
            monitor = Monitor(act)
        exit_code = monitor.poll(args.run)
        scene = scene_get(args.run, act, args.scene)
        last_scene[0] = scene
        scores = [row for row in read_json(
            os.path.join(args.run, "scores.json")) or []
            if row.get("scene") != scene]

        return render({
            "monitor": monitor, "scene": scene, "exit_code": exit_code,
            "causes": causes_read(args.run), "scores": scores,
            "bystander": read_json(os.path.join(args.run, "bystander.json")),
            "origin": origin_read(args.run), "width": width, "plain": plain})

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
