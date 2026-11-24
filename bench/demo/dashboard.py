#!/usr/bin/env python3
"""The terminal dashboard of the Back pressure demo (SPEC 12).

It reads the run directory that demo.sh writes:
  act.json     the Act: number, Relay mode, rproxy pid, cgroup, memory limit
  status.json  the Phones (bench/demo/phones.py)
  rproxy.exit  the exit code of rproxy, or "oom-kill", when it stopped
It reads the memory of rproxy from memory.current of its cgroup (the number
that the OOM killer uses) and the CPU from /proc/<pid>/stat.

Usage: bench/demo/dashboard.py [--run DIR] [--once [--watch S] [--width N]]
"""
import argparse
import json
import os
import time

from rich.console import Console, Group
from rich.live import Live
from rich.text import Text

POLL_PERIOD = 0.2          # seconds between two frames
CHART_ROWS = 8
CHART_SECONDS_PER_COLUMN = 0.5
GRID_COLUMNS = 40
SPEED_BARS = 5
SPEED_BAR_WIDTH = 30
MIB = 1024 * 1024
BLOCKS = " ▁▂▃▄▅▆▇█"
CLOCK_TICKS = os.sysconf("SC_CLK_TCK")

ACT_TITLES = {
    1: "Act 1  No limit",
    2: "Act 2  Reply size limit",
    3: "Act 3  Connection limit",
    4: "Act 4  Back pressure",
    5: "Act 5  Back pressure, pool Origin",
}

# state: (live style, glyph for plain output)
STATE_STYLES = {
    "waiting": ("on grey23", "."),
    "receiving": ("on blue", "r"),
    "done": ("on green", "D"),
    "http_502": ("on red", "2"),
    "http_503": ("on red", "3"),
    "http_other": ("on red", "?"),
    "cut": ("on red", "x"),
    "refused": ("on red", "R"),
}


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--run", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "run"))
    parser.add_argument("--once", action="store_true",
                        help="print one frame with no color and exit")
    parser.add_argument("--watch", type=float, default=0.0,
                        help="with --once: poll for S seconds first")
    parser.add_argument("--width", type=int, default=88,
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
    """What the dashboard remembers between two frames of one Act."""

    def __init__(self, act):
        self.act = act or {}
        self.memory = []          # (seconds since act start, bytes)
        self.peak = 0
        self.oom_kills = 0
        self.parts = {}           # memory.stat: anon (heap), sock (TCP buffers)
        self.death = None         # seconds since act start when rproxy died
        self.cpu_last = None      # (time, ticks)
        self.cpu = 0.0

    def poll(self, run_dir):
        start = self.act.get("started", time.time())
        now = time.time() - start
        pid = self.act.get("rproxy_pid")
        if pid and self.death is None:
            if self.is_alive(pid):
                self.poll_memory(now)
                self.poll_cpu(pid)
            else:
                self.death = self.death_time(run_dir, start, now)
                self.cpu = 0.0
        exit_code = read_text(os.path.join(run_dir, "rproxy.exit"))

        return exit_code

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
        current = read_text(os.path.join(cgroup, "memory.current"))
        if current is not None:
            self.memory.append((now, int(current)))
            self.peak = max(self.peak, int(current))
        stat = read_text(os.path.join(cgroup, "memory.stat")) or ""
        for line in stat.splitlines():
            name, _, value = line.partition(" ")
            if name in ("anon", "sock"):
                self.parts[name] = int(value)
        events = read_text(os.path.join(cgroup, "memory.events")) or ""
        for line in events.splitlines():
            name, _, value = line.partition(" ")
            if name == "oom_kill":
                self.oom_kills = int(value)

    def poll_cpu(self, pid):
        stat = read_text(f"/proc/{pid}/stat")
        if stat is None:
            return
        fields = stat.rsplit(")", 1)[1].split()
        ticks = int(fields[11]) + int(fields[12])    # utime + stime
        now = time.monotonic()
        if self.cpu_last:
            last_time, last_ticks = self.cpu_last
            if now > last_time:
                self.cpu = ((ticks - last_ticks) / CLOCK_TICKS
                            / (now - last_time) * 100)
        self.cpu_last = (now, ticks)


def render(monitor, status, exit_code, width, plain):
    act = monitor.act
    parts = [render_title(act, width), Text("")]
    parts += render_grid(status, plain)
    parts.append(render_legend(status))
    parts.append(Text(""))
    parts += render_chart(monitor, exit_code, width - 2)
    parts.append(Text(""))
    parts += render_speeds(status)
    parts.append(Text(""))
    parts.append(render_counters(monitor, status, exit_code))

    return Group(*parts)


def render_title(act, width):
    number = act.get("act")
    title = ACT_TITLES.get(number, "Waiting for an Act")
    text = Text(title, style="bold")
    if act.get("relay"):
        text.append("   --relay ", style="dim")
        text.append(act["relay"], style="bold cyan")
    if act.get("origin_model"):
        text.append("   Origin ", style="dim")
        text.append(act["origin_model"], style="cyan")
    text.truncate(width)

    return text


def render_grid(status, plain):
    phones = (status or {}).get("phones", [])
    count = (status or {}).get("count", len(phones)) or 200
    rows = []
    for row_start in range(0, count, GRID_COLUMNS):
        row = Text(" ")
        for i in range(row_start, min(row_start + GRID_COLUMNS, count)):
            state = phones[i]["state"] if i < len(phones) else "waiting"
            style, glyph = STATE_STYLES.get(state, STATE_STYLES["cut"])
            if plain:
                row.append(glyph + " ")
            else:
                row.append(" ", style=style)
                row.append(" ")
        rows.append(row)

    return rows


def render_legend(status):
    totals = (status or {}).get("totals", {})
    text = Text(" ")
    for label, style, key in (("waiting", "grey50", "waiting"),
                              ("receiving", "blue", "receiving"),
                              ("done", "green", "done")):
        text.append(f"{label} {totals.get(key, 0)}", style=style)
        text.append("   ")
    failed = sum(totals.get(key, 0) for key in
                 ("http_502", "http_503", "http_other", "cut", "refused"))
    text.append(f"failed {failed}", style="red")
    if status and status.get("finished"):
        elapsed = status["now"] - status["start"]
        text.append(f"   all ended at {elapsed:.0f} s", style="bold")

    return text


def render_chart(monitor, exit_code, width):
    limit = monitor.act.get("memory_max", 1024 * MIB)
    top = limit * 1.2
    columns = width - 8                     # 8 columns of axis labels
    buckets = chart_buckets(monitor, columns)
    death_column = None
    if monitor.death is not None:
        death_column = min(int(monitor.death / CHART_SECONDS_PER_COLUMN),
                           columns - 1)
        death_column -= chart_offset(monitor, columns)
    limit_row = int(limit / top * CHART_ROWS)          # rows from the bottom
    current = monitor.memory[-1][1] if monitor.memory else 0
    header = Text(" rproxy memory  ", style="bold")
    header.append(f"{current / MIB:.0f} MiB", style="bold yellow")
    if monitor.parts and monitor.death is None:
        header.append(f"  = heap {monitor.parts.get('anon', 0) / MIB:.0f}"
                      f" + TCP buffers {monitor.parts.get('sock', 0) / MIB:.0f}"
                      " + other", style="dim")
    header.append(f"   peak {monitor.peak / MIB:.0f}", style="dim")
    lines = [header]
    for row in range(CHART_ROWS - 1, -1, -1):
        label = chart_label(row, limit_row, limit, top)
        line = Text(label.rjust(7) + " ", style="dim")
        for column in range(columns):
            if column == death_column:
                line.append("X" if row == CHART_ROWS - 1 else "|",
                            style="bold red")
                continue
            value = buckets[column] if column < len(buckets) else None
            line.append(*chart_cell(value, row, limit_row, top))
        lines.append(line)
    lines.append(chart_footer(monitor, exit_code, columns))

    return lines


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


def chart_label(row, limit_row, limit, top):
    if row == limit_row:
        return f"{limit / MIB / 1024:.0f} GiB" if limit >= 1024 * MIB \
            else f"{limit / MIB:.0f} MiB"
    if row == 0:
        return "0"

    return ""


def chart_cell(value, row, limit_row, top):
    """The text and the style of one cell of the memory chart."""
    if value is None:
        if row == limit_row:
            return ("-", "red")
        return (" ",)
    height = value / top * CHART_ROWS               # in rows, as a float
    fill = height - row
    style = "red" if value >= top / 1.2 * 0.8 else "yellow"
    if fill >= 1:
        return ("█", style)
    if fill > 0:
        return (BLOCKS[max(1, int(fill * 8))], style)
    if row == limit_row:
        return ("-", "red")
    # A non-zero value shows at least one bar on the bottom row.
    if row == 0 and value > 0:
        return (BLOCKS[1], style)

    return (" ",)


def chart_footer(monitor, exit_code, columns):
    text = Text(" " * 8)
    seconds = (chart_offset(monitor, columns) + columns) \
        * CHART_SECONDS_PER_COLUMN
    text.append(f"time, {seconds:.0f} s across", style="dim")
    if monitor.death is not None:
        text.append(f"   rproxy {death_word(monitor, exit_code)}"
                    f" at {monitor.death:.1f} s", style="bold red")

    return text


def death_word(monitor, exit_code):
    if monitor.oom_kills or exit_code == "oom-kill":
        return "killed by OOM"
    if exit_code == "137":
        return "killed"
    if exit_code is None:
        return "gone"

    return f"exited ({exit_code})"


def render_speeds(status):
    phones = (status or {}).get("phones", [])
    rate = (status or {}).get("rate", 400000)
    receiving = [(i, phone) for i, phone in enumerate(phones)
                 if phone["state"] == "receiving"]
    lines = [Text(" speed of receiving Phones", style="bold")]
    for i, phone in receiving[:SPEED_BARS]:
        speed = phone.get("speed", 0)
        filled = min(SPEED_BAR_WIDTH,
                     round(speed / (rate * 1.25) * SPEED_BAR_WIDTH))
        line = Text(f" #{i:03d} ")
        line.append("━" * filled, style="blue")
        line.append("━" * (SPEED_BAR_WIDTH - filled), style="grey23")
        length = phone.get("length")
        progress = (f"{phone['bytes'] / 1e6:4.1f}/{length / 1e6:.1f} MB"
                    if length else f"{phone['bytes'] / 1e6:4.1f} MB")
        line.append(f" {speed / 1000:4.0f} KB/s  {progress}")
        lines.append(line)
    if len(receiving) > SPEED_BARS:
        lines.append(Text(f"      and {len(receiving) - SPEED_BARS} more",
                          style="dim"))
    while len(lines) < SPEED_BARS + 2:
        lines.append(Text(""))

    return lines


def render_counters(monitor, status, exit_code):
    totals = (status or {}).get("totals", {})
    text = Text(" ")
    pairs = (("CPU", f"{monitor.cpu:.0f}%", "bold"),
             ("done", str(totals.get("done", 0)), "bold green"),
             ("502", str(totals.get("http_502", 0)), "bold red"),
             ("503", str(totals.get("http_503", 0)), "bold red"),
             ("cut", str(totals.get("cut", 0) + totals.get("refused", 0)
                         + totals.get("http_other", 0)), "bold red"))
    for label, value, style in pairs:
        text.append(label + " ", style="dim")
        text.append(value, style=style)
        text.append("   ")
    text.append("killed ", style="dim")
    if monitor.death is not None and death_word(
            monitor, exit_code).startswith("killed"):
        text.append("YES", style="bold white on red")
    else:
        text.append("no", style="bold")

    return text


def act_key(act):
    return (act or {}).get("started")


def main():
    args = parse_args()
    act_path = os.path.join(args.run, "act.json")
    status_path = os.path.join(args.run, "status.json")
    monitor = Monitor(read_json(act_path))

    def frame(plain, width):
        nonlocal monitor
        act = read_json(act_path)
        if act_key(act) != act_key(monitor.act):
            monitor = Monitor(act)                 # a new Act: reset
        exit_code = monitor.poll(args.run)
        status = read_json(status_path)
        if status and act and status.get("start", 0) < act.get("started", 0):
            status = None                          # left from the last Act

        return render(monitor, status, exit_code, width, plain)

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
