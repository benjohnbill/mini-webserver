#!/usr/bin/env python3
"""The terminal dashboard of the Back pressure demo (SPEC 12).

It reads the run directory that demo.sh writes:
  act.json            the Act: number, Relay mode, rproxy pid, cgroup,
                      memory limit, and the speed tiers of the Phones
  status-TIER.json    the Phones of one tier (bench/demo/phones.py)
  relay/loop-*.txt    the buffer bytes of each client connection, if rproxy
                      has --relay-stats: "<client_port> <heap> <kernel>"
  rproxy.exit         the exit code of rproxy, or "oom-kill", when it stopped
It reads the memory of rproxy from memory.current of its cgroup (the number
that the OOM killer uses) and the CPU from /proc/<pid>/stat.

Usage: bench/demo/dashboard.py [--run DIR] [--once [--watch S] [--width N]]
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
CHART_ROWS = 8
CHART_SECONDS_PER_COLUMN = 0.5
CHART_AXIS = 8             # columns of the axis labels
TIER_PREFIX = 14           # columns of " <label> <count> " before the grid
LIMIT_LABEL = " server memory (scaled) "
MIB = 1024 * 1024
BLOCKS = " ▁▂▃▄▅▆▇█"
CLOCK_TICKS = os.sysconf("SC_CLK_TCK")
FOOTNOTE = " speed tiers = KR carrier QoS caps; tier shares = assumption"

ACT_TITLES = {
    1: "Act 1  No limit",
    2: "Act 2  Reply size limit",
    3: "Act 3  Connection limit",
    4: "Act 4  Back pressure",
    5: "Act 5  Back pressure, pool Origin",
}

# A state shows by color and by glyph, for a dim projector or a color-blind
# viewer. The failed states share one glyph; the counter line splits them.
STATE_GLYPHS = {
    "waiting": ("·", "grey50"),
    "receiving": ("▶", "bold bright_blue"),
    "done": ("●", "bold green"),
    "http_502": ("✗", "bold red"),
    "http_503": ("✗", "bold red"),
    "http_other": ("✗", "bold red"),
    "cut": ("✗", "bold red"),
    "refused": ("✗", "bold red"),
}
FAILED_STATES = ("http_502", "http_503", "http_other", "cut", "refused")


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
        self.relay = {}           # last relay stats: port -> heap + kernel

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
        self.poll_relay(run_dir)
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
        stat = read_text(os.path.join(cgroup, "memory.stat")) or ""
        for line in stat.splitlines():
            name, _, value = line.partition(" ")
            if name in ("anon", "sock"):
                self.parts[name] = int(value)

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

    def poll_relay(self, run_dir):
        """Keep the last snapshot that has data: the files stay after rproxy
        dies, and a read can meet a file that rproxy is writing."""
        snapshot = {}
        for path in glob.glob(os.path.join(run_dir, "relay", "loop-*.txt")):
            for line in (read_text(path) or "").splitlines():
                fields = line.split()
                if len(fields) != 3 or line.startswith("#"):
                    continue
                try:
                    port, heap, kernel = (int(field) for field in fields)
                except ValueError:
                    continue
                snapshot[port] = snapshot.get(port, 0) + heap + kernel
        if snapshot:
            self.relay = snapshot


class Tier:
    """One speed tier: its place in act.json and its status file."""

    def __init__(self, label, rate, count, status):
        self.label = label
        self.rate = rate                    # bytes per second
        self.count = count
        self.status = status
        phones = (status or {}).get("phones", [])
        self.phones = phones[:count]
        self.states = [phone["state"] for phone in self.phones]
        self.states += ["waiting"] * (count - len(self.states))

    def total(self, *states):
        return sum(1 for state in self.states if state in states)


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
    if not specs:
        specs = [(label, status.get("rate", 0), status.get("count", 0))
                 for label, status in statuses.items()]
    tiers = [Tier(label, rate, count, statuses.get(label))
             for label, rate, count in specs]

    return sorted(tiers, key=lambda tier: tier.rate)


def render(monitor, tiers, exit_code, width, plain):
    parts = [render_title(monitor.act, width), Text("")]
    parts += render_grid(tiers, width, plain)
    parts.append(render_legend(tiers))
    parts.append(Text(""))
    parts += render_chart(monitor, exit_code, width - 2)
    parts.append(Text(""))
    parts += render_buffer_share(monitor, tiers, width)
    parts.append(Text(""))
    parts += render_progress(monitor, tiers, width)
    parts.append(Text(""))
    parts.append(render_counters(monitor, tiers, exit_code))
    parts.append(Text(FOOTNOTE, style="dim"))

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


def render_grid(tiers, width, plain):
    columns = max(1, (width - TIER_PREFIX - 1) // 2)
    rows = []
    for tier in tiers:
        for row_start in range(0, max(tier.count, 1), columns):
            if row_start == 0:
                row = Text(f" {tier.label:>8} {tier.count:>3}  ", style="bold")
            else:
                row = Text(" " * TIER_PREFIX)
            for state in tier.states[row_start:row_start + columns]:
                glyph, style = STATE_GLYPHS.get(state, STATE_GLYPHS["cut"])
                row.append(glyph, style=None if plain else style)
                row.append(" ")
            rows.append(row)

    return rows


def render_legend(tiers):
    text = Text(" ")
    for state, label in (("done", "done"), ("receiving", "receiving"),
                         ("cut", "failed"), ("waiting", "waiting")):
        states = FAILED_STATES if state == "cut" else (state,)
        glyph, style = STATE_GLYPHS[state]
        count = sum(tier.total(*states) for tier in tiers)
        text.append(f"{glyph} {label} {count}", style=style)
        text.append("   ")
    statuses = [tier.status for tier in tiers]
    if statuses and all(status and status.get("finished")
                        for status in statuses):
        elapsed = (max(status["now"] for status in statuses)
                   - min(status["start"] for status in statuses))
        text.append(f"all ended at {elapsed:.0f} s", style="bold")

    return text


def render_chart(monitor, exit_code, width):
    limit = monitor.act.get("memory_max", 1024 * MIB)
    top = limit * 1.2
    columns = width - CHART_AXIS
    buckets = chart_buckets(monitor, columns)
    death_column = None
    if monitor.death is not None:
        death_column = min(int(monitor.death / CHART_SECONDS_PER_COLUMN),
                           columns - 1)
        death_column -= chart_offset(monitor, columns)
    limit_row = int(limit / top * CHART_ROWS)          # rows from the bottom
    lines = [chart_header(monitor)]
    for row in range(CHART_ROWS - 1, -1, -1):
        label = chart_label(row, limit_row, limit)
        cells = []
        for column in range(columns):
            if column == death_column:
                cells.append(("X" if row == CHART_ROWS - 1 else "|",
                              "bold red"))
                continue
            value = buckets[column] if column < len(buckets) else None
            cells.append(chart_cell(value, row, limit_row, top))
        if row == limit_row:
            chart_label_limit(cells)
        line = Text(label.rjust(CHART_AXIS - 1) + " ", style="dim")
        for cell in cells:
            line.append(*cell)
        lines.append(line)
    lines.append(chart_footer(monitor, exit_code, columns))

    return lines


def chart_header(monitor):
    header = Text(" rproxy memory  ", style="bold")
    if not monitor.memory:
        header.append("no sample", style="dim")
        return header
    current = monitor.memory[-1][1]
    header.append(f"{current / MIB:.0f} MiB", style="bold yellow")
    if monitor.death is not None:
        header.append("  before it died", style="bold red")
    elif monitor.parts:
        header.append(f"  = heap {monitor.parts.get('anon', 0) / MIB:.0f}"
                      f" + TCP buffers {monitor.parts.get('sock', 0) / MIB:.0f}"
                      " + other", style="dim")
    header.append(f"   peak {monitor.peak / MIB:.0f}", style="dim")

    return header


def chart_label_limit(cells):
    """Write the name of the limit on the dashed limit line, at its right."""
    start = len(cells) - len(LIMIT_LABEL) - 1
    if start < 0:
        return
    for i, char in enumerate(LIMIT_LABEL):
        if cells[start + i][0] == "-":
            cells[start + i] = (char, "red")


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


def chart_label(row, limit_row, limit):
    if row == limit_row:
        return mib_text(limit)
    if row == 0:
        return "0"

    return ""


def mib_text(value):
    if value >= 1024 * MIB:
        return f"{value / MIB / 1024:.3g} GiB"

    return f"{value / MIB:.0f} MiB"


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
    text = Text(" " * CHART_AXIS)
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


def render_buffer_share(monitor, tiers, width):
    """The relay buffer bytes of rproxy (heap + kernel), summed by tier."""
    lines = [Text(" buffer share in rproxy (heap + kernel)", style="bold")]
    sums = buffer_sums(monitor.relay, tiers)
    total = sum(sums.values())
    if not total:
        lines.append(Text("   no data", style="dim"))
        return lines
    lines[0].append(f"   total {total / MIB:.1f} MiB", style="dim")
    bar_width = max(10, width - 35)
    labels = [tier.label for tier in tiers] + ["other"]
    for label in labels:
        if label not in sums:
            continue
        share = sums[label] / total
        filled = round(share * bar_width)
        line = Text(f" {label:>8}  ")
        line.append("█" * filled, style="magenta")
        line.append("·" * (bar_width - filled), style="grey23")
        line.append(f" {sums[label] / MIB:7.1f} MiB {share * 100:4.0f}%")
        lines.append(line)

    return lines


def buffer_sums(relay, tiers):
    """Join the relay stats to the tiers by the local port of each Phone.
    A port that no Phone has (the bystander, an old port) is "other"."""
    tier_of = {}
    for tier in tiers:
        for phone in tier.phones:
            port = phone.get("port")
            if port and (port not in tier_of or phone["state"] == "receiving"):
                tier_of[port] = tier.label
    sums = {}
    for port, size in relay.items():
        label = tier_of.get(port, "other")
        sums[label] = sums.get(label, 0) + size
    if set(sums) == {"other"}:
        return {}                           # no match: no data

    return sums


def render_progress(monitor, tiers, width):
    file_bytes = monitor.act.get("file_bytes")
    lines = [Text(" progress of each tier (average Phone)", style="bold")]
    bar_width = max(10, width - 50)
    for tier in tiers:
        received = sum(phone.get("bytes", 0) for phone in tier.phones)
        average = received / tier.count if tier.count else 0
        speeds = [phone.get("speed", 0) for phone in tier.phones
                  if phone["state"] == "receiving"]
        line = Text(f" {tier.label:>8}  ")
        filled = round(average / file_bytes * bar_width) if file_bytes else 0
        filled = min(bar_width, filled)
        line.append("━" * filled, style="blue")
        line.append("·" * (bar_width - filled), style="grey23")
        speed = (f"{sum(speeds) / len(speeds) / 1000:6.0f} KB/s"
                 if speeds else "      - KB/s")
        size = f"{file_bytes / 1e6:.0f}" if file_bytes else "?"
        line.append(f" {speed}  {average / 1e6:5.1f}/{size} MB")
        line.append(f"  cap {tier.rate / 1000:.0f}", style="dim")
        lines.append(line)

    return lines


def render_counters(monitor, tiers, exit_code):
    def total(*states):
        return str(sum(tier.total(*states) for tier in tiers))

    text = Text(" ")
    pairs = (("CPU", f"{monitor.cpu:.0f}%", "bold"),
             ("done", total("done"), "bold green"),
             ("502", total("http_502"), "bold red"),
             ("503", total("http_503"), "bold red"),
             ("cut", total("cut", "refused", "http_other"), "bold red"))
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
    monitor = Monitor(read_json(act_path))

    def frame(plain, width):
        nonlocal monitor
        act = read_json(act_path)
        if act_key(act) != act_key(monitor.act):
            monitor = Monitor(act)                 # a new Act: reset
        exit_code = monitor.poll(args.run)
        tiers = tiers_read(args.run, act)

        return render(monitor, tiers, exit_code, width, plain)

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
