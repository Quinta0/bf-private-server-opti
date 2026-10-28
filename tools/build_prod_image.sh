#!/usr/bin/env bash
# Builds the production server image (linux/amd64) on a build machine and uploads it to the
# server host as bf-server:next. Build off the host: a full compile can starve a
# small production VM of CPU.
#   tools/build_prod_image.sh                 # build server/ and upload
#   SRC=wt/x tools/build_prod_image.sh        # build a worktree instead
#   UPLOAD=0 tools/build_prod_image.sh        # build only
# Needs Docker Desktop with "Use Rosetta for x86_64/amd64 emulation" switched on.
# Swapping bf-server:next into production stays a separate, deliberate step.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$(cd "${SRC:-$ROOT/server}" && pwd)"
JOBS="${JOBS:-6}"            # 10 ran out of memory in a 20 GB Docker VM
TAG="${TAG:-bf-server:next}"
UPLOAD="${UPLOAD:-1}"
VPS="${VPS:-bf@203.0.113.11}"
CTX="${CTX:-$HOME/.cache/bf-prod-ctx}"
LOG_DIR="$ROOT/logs"

die() { echo "error: $*" >&2; exit 1; }

# A production image comes from a commit, never from uncommitted edits.
[[ -z "$(git -C "$SRC" status --porcelain --untracked-files=no)" ]] || [[ "${ALLOW_DIRTY:-0}" == 1 ]] \
    || die "$SRC has uncommitted changes (ALLOW_DIRTY=1 to build anyway)"
COMMIT="$(git -C "$SRC" rev-parse --short HEAD)"
echo "building $SRC at $COMMIT ($(git -C "$SRC" branch --show-current)) as $TAG, $JOBS jobs"

docker run --rm --platform linux/amd64 alpine:3.20 uname -m 2>/dev/null | grep -qx x86_64 \
    || die "Docker cannot run amd64 containers: switch on Rosetta in Docker Desktop"

# Same layout as /opt/bf on the VPS: server/ (README rsync excludes, plus the two
# headers the build generates), prod/ and .vcpkg-downloads/.
mkdir -p "$CTX" "$LOG_DIR"
rsync -a --delete --exclude .git --exclude out/ --exclude deploy/log/ --exclude deploy/game_content/ \
    --exclude 'deploy/gme.sqlite*' --exclude packet-generator/target/ \
    --exclude gimuserver/packets/all.hpp --exclude gimuserver/archive/archive.hpp \
    "$SRC/" "$CTX/server/"
rsync -a --delete "$ROOT/prod/" "$CTX/prod/"
rsync -a --delete "$ROOT/.vcpkg-downloads/" "$CTX/.vcpkg-downloads/"

if [[ "$UPLOAD" == 1 ]]; then
    ssh -o BatchMode=yes "$VPS" 'cat /opt/bf/prod/config.json' </dev/null | cmp -s - "$CTX/prod/config.json" \
        || die "prod/config.json differs from /opt/bf/prod/config.json on the VPS"
fi

LOG="$LOG_DIR/prod-image-$COMMIT-$(date -u +%Y%m%dT%H%M).log"
echo "build log: $LOG"
docker buildx build --platform linux/amd64 --progress=plain --load \
    --build-arg JOBS="$JOBS" -f "$CTX/prod/Dockerfile" -t "$TAG" \
    --label "bf.commit=$COMMIT" "$CTX" >"$LOG" 2>&1 \
    || { tail -30 "$LOG"; die "build failed, see $LOG"; }

ARCH="$(docker image inspect -f '{{.Architecture}}' "$TAG")"
[[ "$ARCH" == amd64 ]] || die "$TAG is $ARCH, not amd64"
LOCAL_ID="$(docker image inspect -f '{{.Id}}' "$TAG")"
echo "built $TAG $LOCAL_ID ($ARCH, commit $COMMIT)"

if [[ "$UPLOAD" == 1 ]]; then
    echo "uploading to $VPS ..."
    docker save "$TAG" | gzip -1 | ssh -o BatchMode=yes "$VPS" 'gunzip | sudo -n docker load' | tail -1
    REMOTE_ID="$(ssh -o BatchMode=yes "$VPS" "sudo -n docker image inspect -f '{{.Id}}' $TAG" </dev/null)"
    [[ "$REMOTE_ID" == "$LOCAL_ID" ]] || die "uploaded image id $REMOTE_ID does not match $LOCAL_ID"
    echo "on the VPS: $TAG = $REMOTE_ID (commit $COMMIT); deploy it with the swap steps in docs/DEPLOYMENT.md"
fi

# Docker's VM keeps the build's memory until it restarts; give it back when idle.
if [[ "${FREE_MEMORY:-1}" == 1 && -z "$(docker ps -q)" ]]; then
    docker desktop restart >/dev/null 2>&1 && echo "restarted Docker Desktop to free its memory"
fi
