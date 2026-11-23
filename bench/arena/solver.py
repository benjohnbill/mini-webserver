#!/usr/bin/env python3
"""Usage: bench/arena/solver.py BASE_URL DURATION

Crawler script: a scraper that pays the proof of work. It sends a Chrome
User-Agent and reads /api/time?page=N, N = 0, 1, 2, ... (the layers preset
puts the proof of work on /api/). Each 401 with X-PoW-Challenge gets a
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


def solver_loop(index):
    client = arena.Client(run, base, arena.CHROME_AGENT, solves_pow=True)
    page = index
    while run.is_running():
        client.fetch(f'/api/time?page={page}')
        page += PARALLEL


run.loops_run(solver_loop, PARALLEL)
