#!/usr/bin/env python3
"""Usage: bench/arena/human.py BASE_URL DURATION

A slow Verification client. It loads the home page as a browser does (the
page, then its CSS, image, script, and /api/time), then reads for 1 to 3
seconds, then loads it again. It is Python urllib with a Chrome
User-Agent, so its JA4 is not the JA4 of curl. It is still OpenSSL, not
the BoringSSL of a real Chrome, so it is a stand-in for a browser (SPEC
11 "Arena, v2").

A proof of work challenge (401 with X-PoW-Challenge) gets the answer of a
browser that runs Anubis-style JavaScript: it solves the challenge, sends
the request again with X-PoW, and keeps the solution until the next 401.

A person does not see the hidden link to /trap/, so this client never
requests it: it does not read links, it loads a fixed list.

It sends from 127.0.0.200, so a Scene can tell its lines in the rproxy log
from the lines of a crawler.
"""
import random

import arena

HUMAN_ADDRESS = '127.0.0.200'
PAGE = ['/', '/style.css', '/image.png', '/app.js', '/api/time']

base, duration = arena.args_parse()
run = arena.Run('human', duration)


def human_loop(_index):
    client = arena.Client(run, base, arena.CHROME_AGENT, HUMAN_ADDRESS,
                          solves_pow=True)
    while run.is_running():
        for target in PAGE:
            client.fetch(target)
        run.sleep(random.randint(1, 3))


run.loops_run(human_loop, 1)
