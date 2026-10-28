#!/usr/bin/env bash
# Runs the built server in Docker.
#   docker/run.sh                 # foreground, Ctrl+C to stop
#   DETACH=1 docker/run.sh        # background; stop with: docker stop bf-server
#   ASSETS=/path/to/assets docker/run.sh
#   BIND=0.0.0.0 docker/run.sh   # expose on the LAN (default: localhost only)
#
# game_content is your archive's assets/ folder (content/ + mst/), mounted
# read-only so the server can never modify the original dump.
# The save database (deploy/gme.sqlite) and logs (deploy/log/) stay on the host.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="bf-server-build"
# SRC/NAME/PORT let several checkouts (git worktrees) run side by side.
SRC="${SRC:-$ROOT/server}"
NAME="${NAME:-bf-server}"
BIN="/src/out/build-linux-arm64/standalone_frontend/gimuserverw"
ASSETS="${ASSETS:-$HOME/bf-archive/assets}"
PORT="${PORT:-9960}"

if [[ ! -d "$ASSETS/content" || ! -d "$ASSETS/mst" ]]; then
    echo "ASSETS must point at a folder containing content/ and mst/ (got: $ASSETS)" >&2
    exit 1
fi

mkdir -p "$SRC/deploy/game_content" "$SRC/deploy/log"

if [[ "${DETACH:-0}" == "1" ]]; then MODE=(-d); else MODE=(-it); fi

exec docker run --rm "${MODE[@]}" --name "$NAME" \
    -p "${BIND:-127.0.0.1}:${PORT}:9960" \
    -v "$SRC:/src" \
    -v "$ASSETS:/src/deploy/game_content:ro" \
    "$IMAGE" "$BIN" /src/deploy/config.docker.json
