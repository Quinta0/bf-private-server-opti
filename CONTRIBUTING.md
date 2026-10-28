# Contributing

Thanks for helping keep Brave Frontier playable. This repository is an unofficial,
community-run preservation project. It is not affiliated with or endorsed by gumi Inc.,
and it ships **code only**. Please read the two ground rules below before opening a pull
request; a change that breaks either one cannot be merged.

- [Ground rules](#ground-rules)
- [Ways to contribute](#ways-to-contribute)
- [Repository layout](#repository-layout)
- [Development setup](#development-setup)
- [Branches and pull requests](#branches-and-pull-requests)
- [Commit messages](#commit-messages)
- [Code style](#code-style)
- [Testing expectations](#testing-expectations)
- [License of contributions](#license-of-contributions)

## Ground rules

### 1. Code only: no game data, art or binaries

Do not commit anything that belongs to the original game or that was extracted from it:
client tables (`mst/`), `content/` archives, art, sounds, fonts, APKs, `libgame.so`, save
files or decrypted dumps. The `.gitignore` already excludes the usual locations
(`server/deploy/mst/`, `server/deploy/archive/`, `/re/lib/`, `*.apk`, ...); do not force-add
around it.

The decoded client tables and client content are obtained separately, from public community
preservation archives (for example BluuArc's Brave Frontier archive). Documentation should say
"obtain the game data separately" and must not link to APK downloads or other redistributions of
the client.

The curated server data (`server/deploy/archive/`: missions, enemy AI, units, gacha, events) lives
in the companion repository
[bf-private-server-data](https://github.com/joppe2001/bf-private-server-data). Send data
corrections there, not here: change or extend the generators in `server/scripts/` (`gen_*.py`)
in this repository when the fix is a generator bug, regenerate the files, and open the pull
request with the regenerated data against the data repository. Do not hand-edit generated files.

Code that *reads* or *transforms* those files is welcome. Tests should build the small
fixtures they need in code rather than check in real tables.

### 2. No real infrastructure

Never commit hostnames, IP addresses, account ids, tokens, keys, certificates or personal
data of a real deployment. Use documentation placeholders instead:

| Kind | Use |
|---|---|
| Hostnames | `example.com`, `cdn.example.com`, `api.example.com` |
| IPv4 | `203.0.113.0/24` (e.g. `203.0.113.10`) |
| IPv6 | `2001:db8::/32` |
| Anything else | `<placeholder>` in angle brackets, e.g. `<account-id>`, `<team>` |

Secrets belong in environment variables or ignored files (`*.env`, `.dev.vars`,
`.upload-token`, `*.key`, ...). If you notice a real secret or host in the history or in a
pull request, report it privately as described in [SECURITY.md](SECURITY.md) instead of
opening a public issue.

## Ways to contribute

- **Bug reports** and **feature requests**: use the issue forms. Bugs are much easier to fix
  with the client version, the request (group id) that failed and the relevant server log
  lines.
- **Missing game content**: the *Missing content* form is for quests, units, events or
  tables the server does not serve yet. Point to a public source for the data where you can.
- **Questions and ideas** that are not yet a concrete issue belong in GitHub Discussions
  (see [SUPPORT.md](SUPPORT.md)).
- **Reverse-engineering notes** under `re/notes/` and design documents under `docs/` are
  contributions too. Keep them free of copied client code and data.

## Repository layout

| Path | What it is | Language / tooling |
|---|---|---|
| `server/` | The game server (fork of Seltraeh/server, from decompfrontier). Sources in `server/gimuserver/`, packet schemas in `server/packet-generator` (vendored fork of Seltraeh/packet-generator, MPL-2.0) | C++, CMake + vcpkg, Drogon, SQLite, Rust (generator) |
| `server/scripts/` | Data generators (`gen_*.py`), the regression runner and wire tests (`test_*_wire.py`) | Python |
| `cdn-worker/` | Cloudflare Workers: CDN, uploader, backup, mst-publish | JavaScript (ES modules), wrangler |
| `site/` | Static site and the unit finder Worker | HTML/JS, wrangler |
| `tools/` | APK patcher, CDN helpers, image builders, host and wire tests | Python, shell |
| `docker/`, `prod/` | Build container and deployment configuration | Docker, shell |
| `re/` | Reverse-engineering notes | Markdown, JSON |
| `docs/` | Roadmap and design notes | Markdown |

## Development setup

Clone the repository (the packet generator is vendored, so no extra steps are needed):

```bash
git clone https://github.com/joppe2001/bf-private-server.git
cd bf-private-server
```

### Game server (`server/`)

The full guide is [server/docs/DEVELOPMENT.md](server/docs/DEVELOPMENT.md). In short:

**Windows (the supported debug path upstream).** Install Visual Studio or Build Tools with
*Desktop development with C++*, CMake 4.0+, Git and Rust stable (MSVC), then from `server/`:

```powershell
.\rebuild.bat -BootstrapVcpkg -VcpkgRoot .tools\vcpkg
& .\out\build\debug-win64\standalone_frontend\Debug\gimuserverw.exe
```

**Linux / macOS via Docker.** `docker/Dockerfile.build` is the toolchain image (Ubuntu 24.04,
GCC 14, CMake 4, Rust, vcpkg). From the repository root:

```bash
docker/build.sh            # configure + incremental build into server/out/build-linux-arm64/
docker/build.sh --clean    # wipe the build tree first
ASSETS=/path/to/assets docker/run.sh   # run on 127.0.0.1:9960
```

`ASSETS` must point at a folder containing `content/` and `mst/` from the game data you
obtained separately. `SRC`, `NAME` and `PORT` let several checkouts run side by side. The
build script targets `arm64-linux`; on another architecture adjust the triplet or use the
`debug-lnx64` CMake preset as described in `server/docs/DEVELOPMENT.md`.

A compiler build needs no game data, client or save. Packet schema changes are made directly in
`server/packet-generator/assets/` and committed together with the server changes that use them.
Files in that directory stay under MPL-2.0.

### Cloudflare Workers (`cdn-worker/`)

Requires Node.js 22 or newer (the pinned wrangler toolchain needs it).

```bash
cd cdn-worker
npm ci
npm test                   # node --test test/*.test.mjs
```

Deploying a Worker needs your own Cloudflare account; copy settings into your own
`wrangler.jsonc` values and keep secrets in `.dev.vars` or `wrangler secret`, never in the
repository.

### Tools (`tools/`)

Most tools are standalone Python 3 scripts with a usage line in their module docstring;
run them with `python3 -I`. Several need `pycryptodome` (install it into a virtual environment):

```bash
python3 -m venv .venv && .venv/bin/pip install pycryptodome
.venv/bin/python -I tools/smoke_test.py --url http://127.0.0.1:9960 --assets /path/to/assets
```

## Branches and pull requests

1. Fork the repository and create a topic branch from `main`, named after the change:
   `fix/helper-list-crash`, `feat/capture-rules`, `docs/dev-setup`.
2. Keep a pull request to one logical change. Separate refactors from behaviour changes.
3. Run the tests for every component you touched (see below) and say in the pull request
   what you ran and what you checked by hand, for example in the emulator.
4. Open the pull request against `main` and fill in the template. Link the issue it closes
   (`Closes #123`).
5. A maintainer reviews it. Address comments with new commits; the branch is squashed or
   rebased on merge, so a tidy history inside the pull request is welcome but not required.
6. Changes to the server's wire format, save schema or published client tables need extra
   care: describe how existing saves and installed clients are affected and how to roll back.

## Commit messages

Use [Conventional Commits](https://www.conventionalcommits.org/) with these types:

| Type | For |
|---|---|
| `feat` | a new feature or game content support |
| `fix` | a bug fix |
| `refactor` | code changes that neither fix a bug nor add a feature |
| `perf` | performance improvements |
| `test` | adding or fixing tests |
| `docs` | documentation only |
| `chore` | build, tooling, dependency or housekeeping changes |
| `ci` | GitHub Actions and other CI configuration |

```
fix: keep the helper list out of MissionEnd replies

MissionEnd freed the helper objects the battle scene still pointed at.
The list now rides UnitMix/Evo/Favorite only.
```

Write the subject in the imperative, lower case, without a trailing period, at most about
72 characters. Use the body to explain *why*. A scope is optional (`fix(server): ...`).
A [Developer Certificate of Origin](https://developercertificate.org/) sign-off is **not**
required.

## Code style

There is no repository-wide formatter configuration. Match the code around your change; the
conventions below describe what the existing code does.

### C++ (`server/gimuserver/`)

- C++23 (`CMAKE_CXX_STANDARD 23`); must build with MSVC (Windows CI) and GCC 14 (Docker).
  Avoid compiler-specific extensions; guard platform differences (for example
  `localtime_s` / `localtime_r`).
- Tabs for indentation, braces on their own line (Allman style).
- `#pragma once` in headers; project includes first, then standard headers.
- Types in `PascalCase`, constants as `kName`, lower-case namespaces (`forge`).
- Handlers are coroutines declared with `HANDLEF(Name)` and return
  `co_return HandleResult::...`; log through `LOG_INFO` / `LOG_WARN` / `LOG_ERROR`.
- Document behaviour in a `/*! ... */` block above the declaration: what the client sends,
  what the reply must contain and why. Name the obfuscated field keys you rely on.
- Every registered request needs exactly one `HANDLEF` body; CI runs
  `python scripts/audit_handlers.py`, which fails on duplicate ids or definitions and on a
  registration without a body.

### Python (`tools/`, `server/scripts/`)

- Python 3.12. Standard library first; the only third-party dependency the scripts use is
  `pycryptodome`. Ask before adding another.
- 4-space indentation, single-quoted strings, type hints on new functions.
- Every script starts with a module docstring that shows how to run it and what it needs.
- Use `pathlib.Path` and resolve paths from the script location, not the working directory.
- Validate input at the boundary (HTTP handlers, files read from disk) and fail with a clear
  message.

### JavaScript (`cdn-worker/`, `site/`)

- ES modules, `const`/`let`, semicolons, no build step and no framework.
- Start each Worker module with a comment that explains what it serves and why.
- Tests use the built-in `node:test` runner (`*.test.mjs`).

### Shell

- `#!/usr/bin/env bash` with `set -euo pipefail`, a usage comment at the top, and
  environment-variable overrides with defaults (`"${PORT:-9960}"`).

## Testing expectations

Every behaviour change needs a test, and every bug fix a regression test where practical.

| Component | Command | Notes |
|---|---|---|
| Workers | `cd cdn-worker && npm ci && npm test` | `node --test` |
| Server build | `docker/build.sh` or `.\rebuild.bat` | must compile on both GCC and MSVC; CI builds Windows Debug on every push and pull request |
| Server startup | `python scripts/test_debug_startup.py <path-to-debug-exe>` | Windows; fresh isolated save, no game data needed |
| Handler registration | `python scripts/audit_handlers.py` (from `server/`) | also run in CI |
| Regression suites | `python scripts/run_bugfix_regressions.py <path-to-debug-exe> [--suite NAME ...] [--list]` | from `server/`; each suite gets a disposable copy of the save and its own port |

Wire tests (`server/scripts/test_*_wire.py`, `tools/test_*.py`) talk to a running server over
HTTP with encrypted requests; they are built on `server/scripts/bf_testkit.py`. Each one
documents its own steps in its docstring. They need a built server and, for most suites, the
game data, so they are not part of CI. Never point a wire test at a server other people are
playing on: tests copy saves through the SQLite backup API and run on loopback ports for a
reason.

If a test cannot run in your environment (no emulator, no Windows), say so in the pull
request instead of skipping it silently.

## License of contributions

The project is licensed under the GNU Affero General Public License v3.0 (see
[LICENSE](LICENSE)). By submitting a contribution you agree that it is licensed under the
same terms. Please follow the [Code of Conduct](CODE_OF_CONDUCT.md).
