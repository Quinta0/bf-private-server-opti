# Documentation

Index of the documentation in this repository. New here? Read
[GETTING_STARTED.md](GETTING_STARTED.md), then [ARCHITECTURE.md](ARCHITECTURE.md).

## Project guides

| Document | What it covers |
|---|---|
| [GETTING_STARTED.md](GETTING_STARTED.md) | Prerequisites, cloning, game data, building and running the server, tests, a patched Android client |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Components, request flow from client to server and CDN, data flow, how overlay files reach players |
| [DEPLOYMENT.md](DEPLOYMENT.md) | Running online: production image, Compose stack, CDN, backups, HTTPS client, test server |
| [ROADMAP.md](ROADMAP.md) | Planned phases and the audit of client requests the server does not implement yet (snapshot dated 2026-10-15; parts are since done) |
| [RESET_BUTTON_APK.md](RESET_BUTTON_APK.md) | The client patch that turns Menu > Credits into Reset Account |

## Components

| Component | Documentation |
|---|---|
| Game server | [server/README.MD](../server/README.MD), [server/docs/](../server/docs/README.md) (development guide, handbook, feature specifications, archive), [server/RELEASE.md](../server/RELEASE.md) (Windows release) |
| Public site | [site/README.md](../site/README.md) |
| Workers | Header comments of `cdn-worker/src/index.js`, `cdn-worker/backup/src/`, `cdn-worker/mst-publish/src/` and `cdn-worker/uploader/src/` |
| Reverse engineering | [re/notes/](../re/notes/) (boot flow, HTTPS and transfer notes, request and response key tables) |

## Community

| Document | What it covers |
|---|---|
| [CONTRIBUTING.md](../CONTRIBUTING.md) | Ground rules, development setup, pull requests, code style, tests |
| [SUPPORT.md](../SUPPORT.md) | Where to ask questions and report problems |
| [SECURITY.md](../SECURITY.md) | Reporting vulnerabilities privately |
| [CODE_OF_CONDUCT.md](../CODE_OF_CONDUCT.md) | Expected behaviour |
| [NOTICE](../NOTICE) | Disclaimer, upstream projects, third-party licenses |
