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
MIN_WIDTH = 60
MIB = 1024 * 1024
SHARE_MIN = 100 * MIB       # the least held memory that shows shares
CLOCK_TICKS = os.sysconf("SC_CLK_TCK")
FOOTNOTE = " speed tiers = KR carrier QoS caps; tier shares = assumption"
EMPTY_COLOR = "grey19"
SLOW_COLOR = "magenta"
FAST_COLOR = "cyan"

ACT_TITLES = {
    1: "Act 1  No limit",
    2: "Act 2  Reply size limit",
    3: "Act 3  Connection limit",
    4: "Act 4  Back pressure",
    5: "Act 5  Back pressure, pool Origin",
}

# The color of each state, and its character in a plain frame (--once).
STATE_COLORS = {
    "done": ("green", "#"),
    "receiving": ("blue", ">"),
    "failed": ("red", "x"),
    "waiting": ("grey35", "."),
}
FAILED_STATES = ("http_502", "http_503", "http_other", "cut", "refused")

# One line under the tiers: what the Relay mode does with a reply.
REASONS = {
    "unbounded": "rproxy keeps each whole reply until the Phone takes it",
    "reply-cap": "a reply over 4 MiB gets 502: 50 MB fails for all",
    "conn-cap": "over the open reply limit, a Phone gets 503",
    "backpressure": "rproxy reads the Origin as fast as the Phone reads",
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
        self.relay = {}           # last relay stats: port -> heap + kernel
        self.tiers = None         # the tiers of the last frame before death

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
    width = max(MIN_WIDTH, width)
    paint = Paint(plain)
    parts = [render_title(monitor.act, width), render_status(
        monitor, exit_code, paint), Text("")]
    parts += render_memory(monitor, width, paint)
    parts.append(Text(""))
    parts += render_holders(monitor, tiers, width, paint)
    parts.append(Text(""))
    parts += render_tiers(monitor, tiers, width, paint)
    parts.append(render_legend(tiers, paint))
    parts.append(Text(""))
    parts.append(Text(" " + REASONS.get(monitor.act.get("relay"), ""),
                      style="italic"))
    parts.append(Text(FOOTNOTE, style="dim"))

    return Group(*parts)


class Paint:
    """A bar is a run of cells with a background color. A colored space has
    a width of one column in every terminal; glyphs like ● and ▶ do not."""

    def __init__(self, plain):
        self.plain = plain

    def bar(self, segments, width):
        """segments: (share 0..1, color, plain char). The rest is empty."""
        text = Text()
        used = 0
        for share, color, char in segments:
            cells = min(width - used, round(share * width))
            self.cells(text, cells, color, char)
            used += cells
        self.cells(text, width - used, EMPTY_COLOR, ".")

        return text

    def cells(self, text, count, color, char):
        if count <= 0:
            return
        if self.plain:
            text.append(char * count)
        else:
            text.append(" " * count, style=f"on {color}")

    def swatch(self, color, char):
        return Text(char if self.plain else " ",
                    style=None if self.plain else f"on {color}")


def render_title(act, width):
    number = act.get("act")
    title = ACT_TITLES.get(number, "Waiting for an Act")
    text = Text(" " + title, style="bold")
    if act.get("relay"):
        text.append("   --relay ", style="dim")
        text.append(act["relay"], style="bold cyan")
    text.truncate(width)

    return text


def render_status(monitor, exit_code, paint):
    text = Text(" rproxy ")
    if monitor.death is None:
        text.append(" ALIVE ", style="bold black on green")
        return text
    word = death_word(monitor, exit_code).upper()
    text.append(f" {word} at {monitor.death:.1f} s ",
                style="bold white on red")

    return text


def render_memory(monitor, width, paint):
    limit = monitor.act.get("memory_max", 1024 * MIB)
    current = monitor.memory[-1][1] if monitor.memory else 0
    head = Text(" server memory (scaled)", style="bold")
    figure = f"{current / MIB:.0f} / {limit / MIB:.0f} MiB "
    head.append(" " * max(1, width - len(head) - len(figure)))
    head.append(figure, style="bold red" if current >= limit * 0.8
                else "bold yellow")
    share = min(1.0, current / limit) if limit else 0
    color = "red" if share >= 0.8 else "yellow"
    bar = Text(" ")
    bar.append_text(paint.bar([(share, color, "#")], width - 2))
    note = Text(f" peak {monitor.peak / MIB:.0f} MiB", style="dim")
    if monitor.death is not None:
        note.append("   (the last value before rproxy died)", style="dim")
    if not monitor.memory:
        note = Text(" no sample yet", style="dim")

    return [head, bar, note]


def render_holders(monitor, tiers, width, paint):
    """Slow tiers against the fast tier: the share of the Phones next to
    the share of the relay memory that they hold."""
    lines = [Text(" who holds the relay memory", style="bold")]
    sums = buffer_sums(monitor.relay, tiers)
    held = sum(sums.values())
    phones = sum(tier.count for tier in tiers)
    if not tiers or not phones:
        lines.append(Text("   no Phones", style="dim"))
        return lines
    total = f"{held / MIB:.1f} MiB" if held >= MIB else "under 1 MiB"
    lines[0].append(f"   total {total}", style="dim")
    fast = max(tiers, key=lambda tier: tier.rate)
    groups = (("slow", [tier for tier in tiers if tier is not fast],
               SLOW_COLOR, "s"), ("fast", [fast], FAST_COLOR, "f"))
    limit = monitor.act.get("memory_max", 1024 * MIB)
    bar_width = max(6, (width - 36) // 2)
    lines.append(Text(" " * 11 + "Phones".ljust(bar_width + 7)
                      + "memory, against the limit", style="dim"))
    for name, group, color, char in groups:
        count = sum(tier.count for tier in group)
        memory = sum(sums.get(tier.label, 0) for tier in group)
        line = Text(f" {name} {count:>4}  ", style="bold")
        line.append_text(paint.bar([(count / phones, color, char)],
                                   bar_width))
        line.append(f" {count / phones * 100:3.0f}%  ")
        # The bar is against the memory limit, so Act 4 shows near nothing.
        line.append_text(paint.bar([(memory / limit, color, char)],
                                   bar_width))
        line.append(f" {memory / MIB:5.0f} MiB", style="bold")
        if held >= SHARE_MIN:     # a share of a few MiB tells nothing
            line.append(f" {memory / held * 100:3.0f}%", style="dim")
        lines.append(line)

    return lines


def render_tiers(monitor, tiers, width, paint):
    """One row for each tier: the state of its Phones as one stacked bar,
    and the relay memory that the tier holds."""
    lines = [Text(" Phones by tier", style="bold")]
    sums = buffer_sums(monitor.relay, tiers)
    bar_width = max(10, width - 30)
    for tier in tiers:
        line = Text(f" {tier.label:>8} {tier.count:>4}  ", style="bold")
        segments = []
        for state, (color, char) in STATE_COLORS.items():
            states = FAILED_STATES if state == "failed" else (state,)
            if tier.count:
                segments.append((tier.total(*states) / tier.count, color,
                                 char))
        line.append_text(paint.bar(segments, bar_width))
        if tier.label in sums:
            line.append(f" {sums[tier.label] / MIB:7.1f} MiB")
        lines.append(line)

    return lines


def render_legend(tiers, paint):
    text = Text(" ")
    for state, (color, char) in STATE_COLORS.items():
        states = FAILED_STATES if state == "failed" else (state,)
        count = sum(tier.total(*states) for tier in tiers)
        text.append_text(paint.swatch(color, char))
        text.append(f" {state} {count}  ")

    return text


def death_word(monitor, exit_code):
    if monitor.oom_kills or exit_code == "oom-kill":
        return "killed by OOM"
    if exit_code == "137":
        return "killed"
    if exit_code is None:
        return "gone"

    return f"exited ({exit_code})"


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
        # After rproxy dies, show the Phones as they were at that moment.
        if monitor.death is None or monitor.tiers is None:
            monitor.tiers = tiers

        return render(monitor, monitor.tiers, exit_code, width, plain)

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
