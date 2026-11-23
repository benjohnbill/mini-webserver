#!/usr/bin/env python3
"""Usage: bench/arena/signed.py BASE_URL DURATION

Crawler script: a good bot that proves its name with Web Bot Auth. It is
named.sh with a signature: the same User-Agent (GPTBot/1.1) and the same
walk of all pages of the dataset. So a Scene shows what the signature
changes. Each request has an RFC 9421 signature with Ed25519 over
"@authority" and the member agent1 of Signature-Agent
(draft-meunier-web-bot-auth-architecture-05).

The Python standard library has no Ed25519, so the signature comes from
openssl pkeyutl -sign -rawin, one process for each request, as
tests/rproxy_func.sh signs. The key is the TEST KEY
tests/data/bot_test_key.pem. tests/data/bot_test_keys.txt holds its
public half for rproxy --bot-key.
"""
import base64
import os
import subprocess
import tempfile
import time
import urllib.parse

import arena

PARALLEL = int(os.environ.get('PARALLEL', '8'))
HERE = os.path.dirname(os.path.abspath(__file__))
PRIVATE_KEY = os.path.join(HERE, '..', '..', 'tests', 'data',
                           'bot_test_key.pem')
# The JWK SHA-256 thumbprint (RFC 7638) of the test key, as in
# tests/data/bot_test_keys.txt.
KEYID = 'C40IKiW5iEzzt2B5DdKbd4pP26xcG8mbp_gDsQ93cnQ'
# The User-Agent of named.sh: the one that OpenAI documents for GPTBot.
AGENT = ('Mozilla/5.0 AppleWebKit/537.36 (KHTML, like Gecko; compatible; '
         'GPTBot/1.1; +https://openai.com/gptbot)')
SIGNATURE_AGENT = ('agent1="https://crawler.example/.well-known/'
                   'http-message-signatures-directory"')
# The draft recommends a short life. 60 s, as tests/rproxy_func.sh.
SIGNATURE_LIFE = 60
HTTPS_DEFAULT_PORT = ':443'


def authority_get(base):
    """The value of @authority (RFC 9421 section 2.2.3): the host in
    lowercase, with no default port. urllib sends the same text as Host."""
    authority = urllib.parse.urlsplit(base).netloc.lower()
    if authority.endswith(HTTPS_DEFAULT_PORT):
        authority = authority[:-len(HTTPS_DEFAULT_PORT)]
    return authority


def signature_params_format(created):
    return ('("@authority" "signature-agent";key="agent1")'
            f';created={created};expires={created + SIGNATURE_LIFE}'
            f';keyid="{KEYID}";alg="ed25519";tag="web-bot-auth"')


def signature_base_format(authority, params):
    """RFC 9421 section 2.5. The value of a Dictionary member is its
    RFC 8941 serialization: the quoted string."""
    member = SIGNATURE_AGENT.split('=', 1)[1]
    return (f'"@authority": {authority}\n'
            f'"signature-agent";key="agent1": {member}\n'
            f'"@signature-params": {params}')


def sign(base_text):
    # Ed25519 signs in one shot, so pkeyutl must know the input size. It
    # cannot read a pipe: the base goes into a file.
    with tempfile.NamedTemporaryFile() as base_file:
        base_file.write(base_text.encode())
        base_file.flush()
        signature = subprocess.run(
            ['openssl', 'pkeyutl', '-sign', '-inkey', PRIVATE_KEY, '-rawin',
             '-in', base_file.name],
            capture_output=True, check=True).stdout
    return base64.b64encode(signature).decode()


def signature_headers(authority, now):
    params = signature_params_format(int(now))
    signature = sign(signature_base_format(authority, params))
    return {
        'Signature-Agent': SIGNATURE_AGENT,
        'Signature-Input': f'sig1={params}',
        'Signature': f'sig1=:{signature}:',
    }


def main():
    base, duration = arena.args_parse()
    run = arena.Run('signed', duration)
    authority = authority_get(base)

    def signed_loop(index):
        client = arena.Client(run, base, AGENT)
        page = index * arena.FILE_COUNT // PARALLEL
        while run.is_running():
            client.fetch(arena.page_target(page),
                         signature_headers(authority, time.time()))
            page += 1

    run.loops_run(signed_loop, PARALLEL)


if __name__ == '__main__':
    main()
