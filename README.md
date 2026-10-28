# bf-private-server

**A community revival server for Brave Frontier (client v2.19.6): the game server and the tooling to run it online.**

[![License: AGPL-3.0](https://img.shields.io/badge/license-AGPL--3.0-blue.svg)](LICENSE)
[![CI](https://github.com/joppe2001/bf-private-server/actions/workflows/ci.yml/badge.svg)](https://github.com/joppe2001/bf-private-server/actions/workflows/ci.yml)
[![Server (Windows)](https://github.com/joppe2001/bf-private-server/actions/workflows/server-windows.yml/badge.svg)](https://github.com/joppe2001/bf-private-server/actions/workflows/server-windows.yml)
[![CodeQL](https://github.com/joppe2001/bf-private-server/actions/workflows/codeql.yml/badge.svg)](https://github.com/joppe2001/bf-private-server/actions/workflows/codeql.yml)

> [!IMPORTANT]
> **Unofficial fan project.** This project is not affiliated with, endorsed by or sponsored by
> gumi Inc. or Alim Co., Ltd. "Brave Frontier" and all related names, characters and artwork
> are trademarks of their respective owners. **No game assets are distributed here**: this
> repository contains source code only. The game client, data tables and art must be obtained
> separately. The project exists for preservation, interoperability and educational use.
> See [NOTICE](NOTICE).

## Contents

- [What this is](#what-this-is)
- [Features](#features)
- [Repository layout](#repository-layout)
- [Quick start](#quick-start)
- [Documentation](#documentation)
- [Contributing](#contributing)
- [Security](#security)
- [Code of Conduct](#code-of-conduct)
- [License](#license)
- [Acknowledgements](#acknowledgements)

## What this is

Brave Frontier's official servers are gone. This repository brings the game back as a
self-hostable service:

- a **C++23 game server** (`server/`) that answers the client's own HTTP protocol, built on
  [Drogon](https://github.com/drogonframework/drogon) with an SQLite save database. It is a fork of
  [Seltraeh/server](https://github.com/Seltraeh/server), itself a fork of the
  [decompfrontier](https://decompfrontier.github.io/) server emulator;
- **Cloudflare Workers** (`cdn-worker/`, `site/`) that serve the game's downloadable files, take
  off-site backups and host a public site;
- **tools** (`tools/`) for patching your own copy of the Android client to point at your server,
  uploading assets, building production images and end-to-end wire tests.

How the pieces talk to each other is described in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
The project's admin tooling is not included in this repository.

## Features

Each item below has a handler or tool in this tree; the linked documents describe the details and
the evidence behind them.

**Game server**

- **Online accounts.** Guest login keyed to a hash of the device's Android ID, a fresh random
  session token per login that every game request must carry, and account transfer codes
  (10 digits plus a password, stored only as hashes, single-use, expiring).
- **The game's request protocol.** 221 request registrations (`server/scripts/audit_handlers.py`
  lists them). Requests the server does not implement yet answer with a "not available yet"
  message and return the player Home instead of closing the session.
- **Story, quests and battles** served from the game's own mission data, plus generated boss
  waves and optional hand-built battle files ([BATTLE_BUILDER.md](server/docs/features/BATTLE_BUILDER.md)).
- **Summons.** Fair summoning rules ([FAIR_SUMMON.md](server/docs/features/FAIR_SUMMON.md)), a
  weekly featured-banner rotation ([SUMMON_ROTATION.md](server/docs/features/SUMMON_ROTATION.md)),
  event and collab gates, and unit selector tickets.
- **Multiplayer and social.** Friends and helpers between real players
  ([FRIENDS_SPEC.md](server/docs/features/FRIENDS_SPEC.md)), guilds
  ([GUILDS_SOCIAL_2026-10-17.md](server/docs/features/GUILDS_SOCIAL_2026-10-17.md)) and Classic Raid
  rooms, fields, camp items and raid shop ([RAIDS_SPEC.md](server/docs/features/RAIDS_SPEC.md)).
- **PvP modes.** Classic Arena against saved decks ([ARENA_SPEC.md](server/docs/features/ARENA_SPEC.md)),
  Colosseum ([COLOSSEUM.md](server/docs/features/COLOSSEUM.md)) and Frontier Hunter scoring and
  rankings ([FRONTIER_HUNTER.md](server/docs/features/FRONTIER_HUNTER.md)).
- **Single-player systems.** Research Lab trials, Frontier Gate, Vortex dungeons, daily challenges,
  Merit exchange, Summoner's Journal, town, unit fusion and evolution, spheres and crafting.
- **Live operations without restarts.** The server reads an optional `balance_root` folder of
  overlay and config files you author yourself: new client table versions are announced at the next login, and hand-built battles
  are picked up within seconds ([BALANCE_FORGE_OVERLAYS.md](server/docs/features/BALANCE_FORGE_OVERLAYS.md)).
- **In-game news board** and an **in-game account reset** ([NEWS_BOARD.md](server/docs/features/NEWS_BOARD.md),
  [ACCOUNT_RESET.md](server/docs/features/ACCOUNT_RESET.md)).

**Infrastructure and tooling**

- CDN Worker serving `/content/*` and `/mst/*` from R2 with alias and by-name fallbacks.
- Write-only backup Worker and an add-only table-publish Worker.
- `tools/patch_apk.py`: byte-checked patcher for your own copy of the v2.19.6 Android client
  (server host, optional HTTPS with a pinned private root CA).
- Docker build container, a hardened production image and a Compose stack.

## Repository layout

| Path | Contents |
|---|---|
| [`server/`](server/) | The game server: `gimuserver/` (C++ sources), `scripts/` (data generators and wire tests), `packaging/` (Windows release), `deploy/` (local configuration). Packet schemas and their Rust generator are vendored in `packet-generator/`. |
| [`cdn-worker/`](cdn-worker/) | Cloudflare Workers: the asset CDN (`src/`), `backup/`, `mst-publish/`, `uploader/`, with `node --test` tests. |
| [`site/`](site/) | Static public site and the Worker behind its unit finder page. |
| [`tools/`](tools/) | APK patcher, device helper, CDN upload and comparison tools, production image builder, end-to-end tests. |
| [`docker/`](docker/) | Toolchain image and `build.sh` / `run.sh` for local Linux builds. |
| [`prod/`](prod/) | Production `Dockerfile`, `compose.yaml` and server configuration templates. |
| [`re/`](re/) | Reverse-engineering notes (request tables, boot flow, HTTPS) and helper scripts. |
| [`docs/`](docs/) | Project documentation: architecture, getting started, deployment, roadmap. |

## Quick start

You need Git, Docker (or a native C++ toolchain, see the guide), and your own copy of the game
data. Full instructions, including Windows and native Linux builds, are in
[docs/GETTING_STARTED.md](docs/GETTING_STARTED.md).

```bash
git clone https://github.com/joppe2001/bf-private-server.git
cd bf-private-server

# Build the server in the toolchain container (arm64 hosts, e.g. Apple Silicon)
docker/build.sh

# Run it on 127.0.0.1:9960 with the game data you obtained separately
ASSETS=/path/to/assets DETACH=1 docker/run.sh
docker logs -f bf-server
```

`ASSETS` must contain the archive's `content/` and `mst/` folders. Run the test suites:

```bash
(cd cdn-worker && npm ci && npm test)
```

## Documentation

| Document | Covers |
|---|---|
| [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md) | Prerequisites, building, running, game data, tests, a patched client |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Components, request and data flow, how overlay files reach players |
| [docs/DEPLOYMENT.md](docs/DEPLOYMENT.md) | Running online: production image, Compose, CDN, backups, HTTPS client |
| [docs/README.md](docs/README.md) | Index of all documentation in the repository |
| [server/docs/README.md](server/docs/README.md) | Server developer docs and feature specifications |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Planned work and the request-coverage audit |

## Contributing

Contributions are welcome: bug fixes, missing game requests, reverse-engineering notes,
documentation and tests. Please read [CONTRIBUTING.md](CONTRIBUTING.md) first. Two rules are
absolute: **code only** (no game data, art or binaries) and **no real infrastructure** in commits
(use `example.com`, `203.0.113.x` and `<placeholders>`).

Questions and ideas go to [GitHub Discussions](https://github.com/joppe2001/bf-private-server/discussions);
bugs, feature requests and missing content go to [Issues](https://github.com/joppe2001/bf-private-server/issues/new/choose).
See [SUPPORT.md](SUPPORT.md).

## Security

Please do not report vulnerabilities in public issues. Use GitHub's private vulnerability
reporting under the repository's **Security** tab. Details are in [SECURITY.md](SECURITY.md).

## Code of Conduct

Everyone taking part in this project is expected to follow the [Code of Conduct](CODE_OF_CONDUCT.md).

## License

The code is released under the [GNU Affero General Public License v3.0](LICENSE) or later. If you
run a modified version as a network service, the AGPL requires you to offer its source to your
users. The vendored `server/packet-generator` directory is licensed separately (MPL-2.0); third-party libraries
keep their own licenses. See [NOTICE](NOTICE).

## Acknowledgements

- [decompfrontier](https://decompfrontier.github.io/), the original Brave Frontier server emulator
  and offline-mod effort, and its contributors, including Arves100.
- [Seltraeh](https://github.com/Seltraeh/server), whose fork of the server and
  [packet-generator](https://github.com/Seltraeh/packet-generator) this project builds on.
- All upstream contributors whose work is preserved in the server's history.
- BluuArc's Brave Frontier archive and the other community preservation efforts that keep the
  game's data available.
- Everyone who documented the game's behaviour over the years.
