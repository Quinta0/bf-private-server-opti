# Security Policy

## Supported versions

This project has no tagged releases. Security fixes are made on the `main` branch only.

| Version | Supported |
|---|---|
| `main` | Yes |
| forks, older commits, third-party deployments | No |

## Reporting a vulnerability

**Please do not open a public issue, discussion or pull request for a security problem.**

Report it privately through GitHub's private vulnerability reporting:

1. Open the repository's **Security** tab.
2. Choose **Report a vulnerability**.
3. Describe the issue, the affected component and path, how to reproduce it against a local
   build, and the impact you expect.

The report is visible only to the maintainers. We aim to acknowledge reports within a week.
We will keep you informed while a fix is prepared, agree a disclosure date with you,
and credit you in the advisory unless you prefer to stay anonymous. This is a volunteer
project: there is no bug bounty.

## Scope

In scope: the code in this repository, including

- the game server (`server/gimuserver/`): request decryption and parsing, authentication and
  account handling, save-database access, file paths derived from requests;
- the Cloudflare Workers (`cdn-worker/`, `site/src/`): upload, backup and publish
  authorization, path handling, cache behaviour;
- deployment scripts and configuration in `prod/`, `docker/` and `tools/`;
- secrets, real hostnames or personal data accidentally committed to the repository or its
  history.

Out of scope:

- the original Brave Frontier client and its protocol design (for example the client's fixed
  request keys), which this project cannot change except through the APK patcher;
- vulnerabilities in third-party dependencies that are already public, unless this project's
  use of them makes them exploitable; please report those upstream as well;
- deployments run by other people. Contact their operators directly;
- social engineering, denial-of-service volume testing and physical attacks.

## Rules for testing

- **Test only against a server or Worker you run yourself**, built from this
  repository on your own machine or account. Do not probe, scan or load-test any live
  deployment, including servers run by the maintainers or the community.
- Do not access, modify or delete other players' accounts or data.
- Do not use automated scanners against infrastructure you do not own.
- Stop and report as soon as you have shown the issue; do not keep or share data you
  obtained.
- Give us reasonable time to fix the problem before any public disclosure.

Research done in good faith under these rules is welcome.
