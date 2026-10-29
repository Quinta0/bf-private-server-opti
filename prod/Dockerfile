# syntax=docker/dockerfile:1
# Production image for the Brave Frontier server emulator (Seltraeh/server).
# Build context: a directory holding server/ (source), .vcpkg-downloads/ and prod/.
#   docker build -f prod/Dockerfile -t bf-server:prod .

# ---------- build ----------
FROM ubuntu:24.04 AS build

ARG DEBIAN_FRONTEND=noninteractive
ARG VCPKG_COMMIT=efa4634bd526b87559684607d2cbbdeeec0f07d8
ARG JOBS=1

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential g++-14 gcc-14 ninja-build git curl ca-certificates \
        zip unzip tar pkg-config python3 python3-venv \
        autoconf automake autoconf-archive libtool bison flex perl linux-libc-dev \
    && rm -rf /var/lib/apt/lists/* \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100

RUN python3 -m venv /opt/cmake && /opt/cmake/bin/pip install --no-cache-dir "cmake>=4.0,<5" \
    && ln -s /opt/cmake/bin/cmake /usr/local/bin/cmake

ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo PATH=/opt/cargo/bin:$PATH
RUN curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain stable

ENV VCPKG_ROOT=/opt/vcpkg VCPKG_FORCE_SYSTEM_BINARIES=1 VCPKG_DOWNLOADS=/opt/vcpkg-downloads
RUN mkdir -p /opt/vcpkg-downloads \
    && git clone --filter=blob:none https://github.com/microsoft/vcpkg.git /opt/vcpkg \
    && git -C /opt/vcpkg checkout ${VCPKG_COMMIT} \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics

# Seeded sources (e.g. zlib 1.3.1, whose GitHub archive URL now 404s).
COPY .vcpkg-downloads/ /opt/vcpkg-downloads/

# Dependencies first so source edits do not rebuild them. vcpkg's binary cache
# (~/.cache/vcpkg) is a cache mount, so when this layer does rerun, only ports
# whose inputs changed are rebuilt.
WORKDIR /src
COPY server/vcpkg.json server/vcpkg-configuration.json ./
RUN --mount=type=cache,target=/root/.cache/vcpkg \
    /opt/vcpkg/vcpkg install --x-install-root=/opt/vcpkg_installed --triplet x64-linux-release

COPY server/ /src/
# /build is a BuildKit cache mount: object files survive between image builds,
# so a source change only recompiles what it touches. /build/pkgen keeps what the
# packet generator needs for that to hold:
# - target/: the compiled generator (CARGO_TARGET_DIR), else rebuilt every time;
# - the two headers it writes into the source tree, keyed on a hash of
#   packet-generator/ and the Rust version. A fresh /src has no headers, so they
#   would be regenerated with a new mtime and Ninja would recompile everything
#   that includes them (nearly all of gimuserver). Restored with cp -p when the
#   key matches, they keep the mtime the cached objects were built against.
RUN --mount=type=cache,target=/build \
    --mount=type=cache,target=/opt/cargo/registry \
    set -e; \
    gen='gimuserver/packets/all.hpp gimuserver/archive/archive.hpp'; \
    key=$({ rustc --version; cd /src && find packet-generator -path packet-generator/target -prune -o -type f -print0 \
        | LC_ALL=C sort -z | xargs -0 sha256sum; } | sha256sum | cut -d' ' -f1); \
    if [ "$(cat /build/pkgen/key 2>/dev/null)" = "$key" ]; then \
        for f in $gen; do cp -p "/build/pkgen/${f##*/}" "/src/$f"; done; \
    fi; \
    export CARGO_TARGET_DIR=/build/pkgen/target; \
    cmake -S /src -B /build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_TARGET_TRIPLET=x64-linux-release \
        -DVCPKG_INSTALLED_DIR=/opt/vcpkg_installed \
        -DVCPKG_MANIFEST_INSTALL=OFF \
        -DGIMUSRV_FRONTEND=STANDALONE \
        -DGIMU_ENABLE_DEBUG_CLI=OFF \
    && cmake --build /build --parallel ${JOBS} \
    && mkdir -p /build/pkgen && rm -f /build/pkgen/key \
    && for f in $gen; do cp -p "/src/$f" /build/pkgen/; done \
    && echo "$key" > /build/pkgen/key \
    && install -D -s /build/standalone_frontend/gimuserverw /out/gimuserverw

# ---------- runtime ----------
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates tzdata \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --home /app --shell /usr/sbin/nologin bf \
    && install -d -o bf -g bf /data

# Rarely changing layers first: the game data stays cached (and shared between
# image tags on the host) when only the binary or the config changes.
WORKDIR /app
RUN mkdir -p /app/game_content/content /app/game_content/mst /app/balance
COPY server/deploy/mst/ /app/mst/
COPY server/deploy/archive/ /app/archive/
COPY server/deploy/system/ /app/system/
COPY prod/config.json /app/config.json
COPY --from=build /out/gimuserverw /app/gimuserverw

ENV TZ=Asia/Tokyo
USER bf
EXPOSE 9960
VOLUME ["/data"]
ENTRYPOINT ["/app/gimuserverw", "/app/config.json"]
