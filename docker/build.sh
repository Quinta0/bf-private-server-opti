#!/usr/bin/env bash
# Builds the server inside the toolchain image. Run from the host:
#   docker/build.sh            # configure + build
#   docker/build.sh --clean    # wipe the build tree first
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="bf-server-build"
BUILD_DIR="out/build-linux-arm64"
# SRC: server checkout to build (default: server/; a worktree for parallel work).
SRC="${SRC:-$ROOT/server}"
PARALLEL="${PARALLEL:-3}"

docker build -t "$IMAGE" -f "$ROOT/docker/Dockerfile.build" "$ROOT/docker"

if [[ "${1:-}" == "--clean" ]]; then
    rm -rf "$SRC/$BUILD_DIR"
fi

docker run --rm \
    -v "$SRC:/src" \
    -v bf-vcpkg-cache:/vcpkg-cache \
    -v "$ROOT/.vcpkg-downloads:/vcpkg-downloads" \
    -e VCPKG_DOWNLOADS=/vcpkg-downloads \
    -v bf-cargo-registry:/opt/cargo/registry \
    "$IMAGE" bash -c "
        set -euo pipefail
        cmake -S /src -B /src/$BUILD_DIR -G Ninja \
            -DCMAKE_BUILD_TYPE=Debug \
            -DCMAKE_TOOLCHAIN_FILE=\$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
            -DVCPKG_TARGET_TRIPLET=arm64-linux \
            -DGIMUSRV_FRONTEND=STANDALONE
        cmake --build /src/$BUILD_DIR --parallel $PARALLEL -- ${NINJA_ARGS:-}
    "
