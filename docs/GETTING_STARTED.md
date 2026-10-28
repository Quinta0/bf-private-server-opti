# Getting started

This guide takes you from a fresh clone to a local game server and the test suites. For how the parts fit together, read [ARCHITECTURE.md](ARCHITECTURE.md); for
running a public server, read [DEPLOYMENT.md](DEPLOYMENT.md).

- [Prerequisites](#prerequisites)
- [Clone](#clone)
- [Obtain the game data](#obtain-the-game-data)
- [Build the server](#build-the-server)
- [Run the server](#run-the-server)
- [Optional overlay files](#optional-overlay-files)
- [Run the tests](#run-the-tests)
- [Connect an Android client](#connect-an-android-client)
- [Troubleshooting](#troubleshooting)

## Prerequisites

| For | You need |
|---|---|
| Everything | Git |
| Server, Docker build (recommended on Linux/macOS) | Docker; the build script targets **arm64** hosts (Apple Silicon, arm64 Linux) |
| Server, native Windows build | Visual Studio 2022 or Build Tools with *Desktop development with C++*, CMake 4.0+, Ninja, Rust stable (MSVC), vcpkg |
| Server, native Linux build | CMake 4.0+, Ninja, GCC 14 (or another C++23 compiler), Rust 1.87+, vcpkg |
| Tools and wire tests | Python 3.12 and `pycryptodome` |
| Workers | Node.js 22 or newer |
| Patched client (optional) | Your own copy of the v2.19.6 Android client, the Android SDK build tools, a JDK (`keytool`) |

The Docker toolchain image (`docker/Dockerfile.build`) installs Ubuntu 24.04, GCC 14, CMake 4,
Rust and a pinned vcpkg, so a Docker build needs nothing else on the host.

## Clone

The packet schemas and their code generator are vendored in `server/packet-generator`, so a plain
clone has everything:

```bash
git clone https://github.com/joppe2001/bf-private-server.git
cd bf-private-server
```

## Obtain the game data

This repository is code only. **Obtain the game data separately**, for example from a public
community preservation archive such as BluuArc's Brave Frontier archive. Nothing in this project
links to or redistributes the game client.

The server reads three kinds of data, set in its configuration (`server/deploy/config.json`, or
`server/deploy/config.docker.json` in Docker):

| Config key | Default path | What it holds | In git? |
|---|---|---|---|
| `document_root` | `server/deploy/game_content/` | The client's downloadable files: the archive's `content/` and `mst/` folders | No |
| `mst_root` | `server/deploy/mst/` | Decoded master tables as JSON (`unit_mst.json`, `area_mst.json`, ...) that the server reads at startup | No |
| `archive_root` | `server/deploy/archive/` | Curated server data (`mission.json`, `gacha.json`, `ai.json`, ...) built by the generators in `server/scripts/gen_*.py` | No |
| `system_root` | `server/deploy/system/` | Server settings and reply fixtures that are not game tables (`features.json`, `notice_info.json`, ...) | Yes |

`docker/run.sh` mounts the folder you pass as `ASSETS` read-only as `game_content`, so the server
can never modify your copy. The server fails at startup when a table it needs from `mst_root` is
missing. Keep all of these out of commits; the root `.gitignore` already excludes them.

## Build the server

A compiler build needs no game data, client or save.

### Docker (Linux or macOS on arm64)

```bash
docker/build.sh            # configure and build incrementally
docker/build.sh --clean    # wipe the build tree first
```

The binary lands in `server/out/build-linux-arm64/standalone_frontend/gimuserverw`. Useful
variables: `SRC` builds another checkout (for example a git worktree), `PARALLEL` sets compiler
jobs (default 3), `NINJA_ARGS` passes extra arguments to Ninja. vcpkg and Cargo caches are kept in
Docker volumes, so later builds only recompile what changed.

`docker/build.sh` passes `-DVCPKG_TARGET_TRIPLET=arm64-linux`. On an x86_64 host, use the
native Linux preset below, or build the production image (`prod/Dockerfile`, triplet
`x64-linux-release`) as described in [DEPLOYMENT.md](DEPLOYMENT.md).

### CMake presets

`server/CMakePresets.json` defines these configure presets (each expects `VCPKG_ROOT` in the
environment):

| Preset | Platform | Notes |
|---|---|---|
| `debug-win64` | Windows | Ninja Multi-Config, Debug and Release; run from an x64 Native Tools shell |
| `debug-lnx64` | Linux | Debug, Ninja, `x64-linux` triplet |
| `standalone-release-win64` | Windows | Portable release ZIP, see [server/RELEASE.md](../server/RELEASE.md) |
| `release-win32` | Windows | The APPX proxy target |

Linux:

```bash
cd server
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset debug-lnx64
cmake --build --preset debug-lnx64
```

Windows (PowerShell, from `server/`). `rebuild.bat` finds Visual Studio, imports the x64
environment, can bootstrap a repository-local vcpkg, configures and builds Debug:

```powershell
.\rebuild.bat -BootstrapVcpkg -VcpkgRoot .tools\vcpkg
# or, from an x64 Native Tools shell with VCPKG_ROOT set:
cmake --preset debug-win64
cmake --build --preset debug-win64-debug --target gimuserverw --parallel 2
```

The full Windows guide, including Visual Studio debugging and troubleshooting, is
[server/docs/DEVELOPMENT.md](../server/docs/DEVELOPMENT.md).

## Run the server

### Docker

```bash
ASSETS=/path/to/assets docker/run.sh            # foreground, Ctrl+C to stop
ASSETS=/path/to/assets DETACH=1 docker/run.sh   # background
docker logs -f bf-server                        # follow the log
docker stop bf-server                           # stop
```

`ASSETS` must point at a folder that contains `content/` and `mst/`; the script refuses to start
otherwise. The server listens on `127.0.0.1:9960`. Set `BIND=0.0.0.0` to expose it on your LAN,
and `NAME` / `PORT` to run several checkouts side by side. The save database is
`server/deploy/gme.sqlite` and logs go to `server/deploy/log/`, both on the host.

### Native

Debug builds read `deploy/config.json`; all paths in it resolve from the configuration's folder.
On Windows:

```powershell
& .\out\build\debug-win64\standalone_frontend\Debug\gimuserverw.exe
```

The first start creates and migrates a new save. On Windows, this checks that a Debug build
starts on a fresh, temporary save without any game data (it is also a CI step):

```powershell
python scripts/test_debug_startup.py out/build/debug-win64/standalone_frontend/Debug/gimuserverw.exe
```

### Smoke test

`tools/smoke_test.py` checks content and table downloads, the by-name content fallback and an
encrypted game request against a running server. It needs `pycryptodome`:

```bash
python3 -m venv .venv && .venv/bin/pip install pycryptodome
.venv/bin/python -I tools/smoke_test.py --url http://127.0.0.1:9960 --assets /path/to/assets
```

## Optional overlay files

The server reads an optional `balance_root` folder (`server/deploy/balance/` in the local
configuration) for overlay and config files you author yourself: client table versions, server
table overlays, hand-built battles and news posts. Without it the server runs on its own data.
The file formats are in
[BALANCE_FORGE_OVERLAYS.md](../server/docs/features/BALANCE_FORGE_OVERLAYS.md),
[BATTLE_BUILDER.md](../server/docs/features/BATTLE_BUILDER.md) and
[NEWS_BOARD.md](../server/docs/features/NEWS_BOARD.md).

## Run the tests

These are the same commands the CI workflow (`.github/workflows/ci.yml`) runs.

```bash
# Cloudflare Workers (node --test)
cd cdn-worker
npm ci
npm test

# Python syntax of tools/ and server/scripts/
python -m compileall -q tools server/scripts

# Request handler registrations (no compiler needed)
python server/scripts/audit_handlers.py
```

Wire tests (`server/scripts/test_*_wire.py`, `tools/test_*.py`) run against a real server binary
and are described in the scripts themselves and in the feature documents under
[server/docs/features/](../server/docs/features/).

## Connect an Android client

You need your own copy of the Brave Frontier v2.19.6 Android client. `tools/patch_apk.py` checks
every original byte before it writes and refuses on any mismatch, so it only accepts that exact
build. It points the client at your server, switches it to plain HTTP (or keeps HTTPS with a
pinned root, see [DEPLOYMENT.md](DEPLOYMENT.md#https-client-build)), adds the own-unit helper and
Reset Account patches, drops the 32-bit libraries and re-signs the APK.

1. Create a signing key and describe it in `signing/keystore.properties` (the `signing/` folder is
   git-ignored):

   ```bash
   mkdir -p signing
   keytool -genkeypair -keystore signing/bf-local.keystore -alias bf-local \
       -keyalg RSA -keysize 2048 -validity 10000
   printf 'storeFile=signing/bf-local.keystore\nalias=bf-local\n' > signing/keystore.properties
   export BF_KS_PASS='<your keystore password>'
   ```

2. Patch the APK. The script calls `zipalign` and `apksigner` from
   `~/Library/Android/sdk/build-tools/36.1.0` (the `BUILD_TOOLS` constant at the top of the script):

   ```bash
   python3 -I tools/patch_apk.py --apk /path/to/your-client.apk \
       --out build/BraveFrontier-local-127.0.0.1.apk --host 127.0.0.1:9960
   ```

3. Install and launch over USB. `tools/device_run.sh` installs the APK, runs
   `adb reverse tcp:9960 tcp:9960` so the phone reaches your machine as `127.0.0.1:9960`, and
   starts the game (`--fresh` clears its data first). It expects `adb` under
   `~/Library/Android/sdk/platform-tools/`, the macOS Android SDK location:

   ```bash
   tools/device_run.sh -s <adb serial> --fresh
   ```

The game will not log in if the phone's "Phone" permission prompt is denied on first launch.
The patch offsets and the reverse-engineering notes behind them are in
[re/notes/android_boot_re.json](../re/notes/android_boot_re.json).

## Troubleshooting

| Symptom | Fix |
|---|---|
| CMake cannot find the packet-generator runtime or `Cargo.lock` | The vendored `server/packet-generator` directory is missing or incomplete: re-clone the repository or restore it with `git checkout -- server/packet-generator` |
| `ASSETS must point at a folder containing content/ and mst/` | Pass the archive's `assets/` folder, not its parent |
| The server exits at startup reading a table | `server/deploy/mst/` or `server/deploy/archive/` is missing a file; see [Obtain the game data](#obtain-the-game-data) |
| Docker build fails on an x86_64 host | `docker/build.sh` targets `arm64-linux`; use the `debug-lnx64` preset or `prod/Dockerfile` |
| vcpkg cannot download a dependency's source (for example zlib, whose GitHub archive URL no longer serves) | Place the tarball vcpkg asks for in `.vcpkg-downloads/` at the repository root; `docker/build.sh` mounts it as `VCPKG_DOWNLOADS` |
| Compiler killed / out of memory | Lower `PARALLEL` (Docker) or `-Jobs` (Windows) to 1 or 2 |
| `patch_apk.py`: `missing signing/keystore.properties` | Create the keystore as in step 1 above |
