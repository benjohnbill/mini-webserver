#!/usr/bin/env python3
"""Usage: bench/arena/solver.py BASE_URL DURATION

Crawler script: a scraper that pays the proof of work. It sends a Chrome
User-Agent and reads /api/time again and again (the layers preset puts
the proof of work on /api/). It sends no query string: the Router of the
Origin does not strip it, so /api/time?page=1 is a 404. Each 401 with X-PoW-Challenge gets a
solution, and the request goes again with X-PoW. The summary line counts
the CPU seconds of all solutions (pow_cpu_s), so a Scene shows what each
page costs the crawler.

It keeps a solution until the next 401, as a browser keeps the cookie of
Anubis. POW_REUSE=0 sends each solution once only.
"""
import os

import arena

PARALLEL = int(os.environ.get('PARALLEL', '4'))

base, duration = arena.args_parse()
run = arena.Run('solver', duration)


def solver_loop(_index):
    client = arena.Client(run, base, arena.CHROME_AGENT, solves_pow=True)
    while run.is_running():
        client.fetch('/api/time')


run.loops_run(solver_loop, PARALLEL)
