# Local development build (Docker)

Build and run the game server in a Linux container, without installing the toolchain on your
machine. The source tree is bind-mounted, so builds are incremental and the binary, save database
and logs stay in your checkout.

| File | Purpose |
|---|---|
| [`Dockerfile.build`](Dockerfile.build) | Toolchain image `bf-server-build`: Ubuntu 24.04, GCC 14, CMake 4, Ninja, Rust (stable), vcpkg pinned to the manifest baseline. |
| [`build.sh`](build.sh) | Builds the image, then configures and builds a Debug `gimuserverw` into `server/out/build-linux-arm64/`. |
| [`run.sh`](run.sh) | Runs that binary with `server/deploy/config.docker.json` and your game data mounted read-only. |

`build.sh` uses the vcpkg triplet `arm64-linux`, so it targets arm64 hosts (Apple Silicon, arm64
Linux). For an amd64 image use [`prod/Dockerfile`](../prod/README.md).

## Requirements

- Docker.
- `server/packet-generator` checked out (see [server/README.MD](../server/README.MD)).
- A `.vcpkg-downloads/` directory at the repo root (`mkdir -p .vcpkg-downloads`); it is mounted as
  vcpkg's download cache.
- Game data you obtained separately: an `assets/` folder containing `content/` and `mst/`, plus
  the server's own `server/deploy/mst/` and `server/deploy/archive/`. None of it is in this
  repository (see the [root README](../README.md)).

## Build

```bash
docker/build.sh            # configure + incremental build
docker/build.sh --clean    # wipe the build tree first
```

Settings: `SRC=<server checkout>` builds another checkout (for example a git worktree),
`PARALLEL=<n>` sets compiler jobs (default 3). vcpkg binaries and the Cargo registry are cached in
the Docker volumes `bf-vcpkg-cache` and `bf-cargo-registry`.

## Run

```bash
ASSETS=/path/to/assets docker/run.sh             # foreground, Ctrl+C to stop
DETACH=1 ASSETS=/path/to/assets docker/run.sh    # background; stop with: docker stop bf-server
docker logs -f bf-server
```

The server listens on `127.0.0.1:9960` by default. Settings: `BIND=0.0.0.0` exposes it on your LAN,
`PORT`, `NAME` (container name, default `bf-server`) and `SRC` let several checkouts run side by
side. The save database is `server/deploy/gme.sqlite` and logs go to `server/deploy/log/`; the
game data mount is read-only, so the server can never modify it.

## Smoke test

With the server running, check content and table downloads, the by-name content fallback and an
encrypted request round trip (needs `pycryptodome`):

```bash
python3 -I tools/smoke_test.py --url http://127.0.0.1:9960 --assets /path/to/assets
```

## Related documents

- [Root README](../README.md)
- [server/README.MD](../server/README.MD): native builds on Windows and Linux
- [prod/README.md](../prod/README.md): the production image and Compose stack
- [tools/README.md](../tools/README.md): wire tests that run against this server
- [docs/GETTING_STARTED.md](../docs/GETTING_STARTED.md): first local setup
- [CONTRIBUTING.md](../CONTRIBUTING.md)
