#!/usr/bin/env bash
# Run a command inside the Linux build container, e.g.
#   scripts/dev.sh make test
#   scripts/dev.sh bash scripts/functional_test.sh
set -euo pipefail
cd "$(dirname "$0")/.."
IMAGE=mini-webserver-tools
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    docker build -q -t "$IMAGE" tools >/dev/null
fi
# MSYS_NO_PATHCONV keeps Git Bash on Windows from rewriting /src.
MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W 2>/dev/null || pwd):/src" -w /src "$IMAGE" "$@"
