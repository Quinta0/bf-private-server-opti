# Game server

An unofficial, **experimental** C++23 game server that emulates the official server used by
Brave Frontier (client v2.19.6.0). It is built on [Drogon](https://github.com/drogonframework/drogon)
with SQLite storage, CMake and vcpkg, and a Rust build-time packet generator.

This directory is a fork of [Seltraeh/server](https://github.com/Seltraeh/server), which itself
derives from the [decompfrontier](https://decompfrontier.github.io/) server emulator. The emulator
work started in late 2022 as part of a "Brave Frontier Offline Mod", inspired by the original
GimuFrontier C# proof of concept. For background, see
[How the offline mod works](https://decompfrontier.github.io/pages/Architecture/Offlinemod-architecture.html)
and [Emulator (Server) architecture](https://decompfrontier.github.io/pages/Architecture/Emulator-architecture.html).

> No game data is included. The curated archive (`deploy/archive/`) is the `archive/` folder of
> the companion repository [bf-private-server-data](https://github.com/joppe2001/bf-private-server-data).
> The decoded tables (`deploy/mst/`) and the client content tree are obtained separately (see the
> [root README](../README.md#game-data)). `deploy/system/` (server config and response fixtures)
> is part of the source.

## Layout

| Path | Contents |
|---|---|
| `gimuserver/` | the server library: request handlers (`gme/handlers/`), shared game logic (`gme/common/`), database migrations (`db/`), archive and overlays (`archive/`), packets |
| `standalone_frontend/` | the `gimuserverw` executable (`-DGIMUSRV_FRONTEND=STANDALONE`) |
| `game_frontend/` | the in-client (APPX) frontend target, built only with the APPX frontend option |
| `packet-generator/` | Vendored Rust generator (fork of [Seltraeh/packet-generator](https://github.com/Seltraeh/packet-generator) with project-specific schemas, MPL-2.0) that generates the packet headers at build time |
| `deploy/` | runtime configs (`config.docker.json`, `config.json`) and `system/`; your save database and logs are written here |
| `packaging/` | Windows release packaging ([RELEASE.md](RELEASE.md)) |
| `scripts/` | data generators (`gen_*.py`), audits and wire tests (`test_*_wire.py`) |
| `docs/` | handbook, feature specs (`docs/features/`) and working notes |

## Requirements

- CMake 4.0+
- Rust 1.87+ (build-time packet generator)
- vcpkg
- Windows: Visual Studio 2022 or Build Tools with Desktop development with C++
- Linux: Ninja and GCC 14 (the Docker toolchain image uses GCC 14) or Clang
- The vendored `packet-generator/` directory, which ships with the repository. Upstream
  Seltraeh/packet-generator lacks this project's schema changes, so do not replace it with a
  fresh upstream clone.

## Build

**Windows.** Follow the [fresh-checkout development guide](docs/DEVELOPMENT.md).
`rebuild.bat` configures CMake and builds Debug; generated Ninja files do not need to be copied
from another machine:

```powershell
.\rebuild.bat -BootstrapVcpkg -VcpkgRoot .tools\vcpkg
```

**Linux / macOS via Docker.** The repo's [docker/](../docker/README.md) folder builds a Debug server
for arm64 in a toolchain container (`docker/build.sh`) and runs it on port 9960 (`docker/run.sh`).
The production amd64 Release image is [prod/Dockerfile](../prod/README.md).

**Linux natively.** The equivalent CMake invocation (see `docker/build.sh`):

```bash
cmake -S server -B server/out/build-linux -G Ninja -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
    -DGIMUSRV_FRONTEND=STANDALONE
cmake --build server/out/build-linux
```

**Portable Windows release.** See [RELEASE.md](RELEASE.md) to build the standalone server ZIP and
separate game-content downloads. Players run `Start Server.bat`; they do not need the
packet-generator or a development toolchain.

## Run

`gimuserverw <config.json>` starts the server with the given configuration. The important keys
under `plugins[0].config.server`:

| Key | Meaning |
|---|---|
| `public_host` | host[:port] the client is told to use in the `/dls` reply |
| `resource_host` | host serving game assets (this server, or a CDN) |
| `mst_root`, `system_root`, `archive_root` | decoded tables, server fixtures, curated archive |
| `balance_root` | optional folder of overlay and config files you author yourself ([format](docs/features/BALANCE_OVERLAYS.md)); remove to turn overlays off |
| `game_version` | must be `21900` for client v2.19.6 |

The SQLite save is set in `db_clients[0].filename`. At start the server switches it to WAL mode
and sets a busy timeout; both are logged (`SQLite journal_mode: wal`, `SQLite busy_timeout: ...`).

## Test

There is no unit-test target; behaviour is checked by wire tests that drive a running server the
way the client does. They need `pycryptodome` and game data.

- `scripts/test_*_wire.py`: per-feature wire tests (for example
  `python scripts/test_balance_overlay_wire.py --port 9964 --name bf-overlay-test`). Read each script's
  docstring for its setup.
- `scripts/bf_testkit.py`: helpers that run an isolated server on a backup-API copy of a save, so the
  original is only opened read-only.
- `../tools/smoke_test.py` and the other `../tools/test_*.py` wire tests
  ([tools/README.md](../tools/README.md)).

Run wire tests against a disposable server and save, never against a save with real players.

## Further reading

- [Gameplay handbook](docs/BF_OFFLINE_SERVER_HANDBOOK.md) and [gameplay work queue](docs/GAMEPLAY_WORK_QUEUE.md)
- Feature specs in [docs/features/](docs/features/)
- [Root README](../README.md): the whole project, data sources and client setup
- [docs/GETTING_STARTED.md](../docs/GETTING_STARTED.md) and [docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md)
- [CONTRIBUTING.md](../CONTRIBUTING.md) and [SECURITY.md](../SECURITY.md)

## Legal

The code of this project is released under the GNU Affero General Public License v3.0 or later
([LICENSE](LICENSE)).

This is an unofficial fan project for preservation and educational purposes. It is not associated
with, affiliated with, sponsored or endorsed by Alim Co., Ltd. or gumi Inc. Brave Frontier and
related trademarks belong to their respective owners. No game assets are distributed with this
repository.
