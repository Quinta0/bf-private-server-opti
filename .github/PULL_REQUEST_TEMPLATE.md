## Summary

<!-- What does this change and why? Link the issue it resolves. -->

Closes #

## Type of change

- [ ] `feat` new feature or content support
- [ ] `fix` bug fix
- [ ] `refactor` / `perf`
- [ ] `test` / `docs` / `chore` / `ci`

## Components touched

- [ ] Game server (`server/`)
- [ ] Packet schemas (`server/packet-generator`, MPL-2.0)
- [ ] Cloudflare Workers (`cdn-worker/`, `site/`)
- [ ] Tools (`tools/`)
- [ ] Build or deployment (`docker/`, `prod/`)
- [ ] Documentation (`docs/`, `re/`, READMEs)

## How was this tested?

<!-- Commands you ran and their results, plus anything checked by hand (emulator, device). -->

- [ ] `cd cdn-worker && npm test`
- [ ] Server builds (`docker/build.sh` or `rebuild.bat`)
- [ ] Wire or regression tests: <!-- which ones -->
- [ ] Checked in the client: <!-- what -->

## Compatibility

<!-- Delete if not applicable. -->

- Effect on existing saves (schema or migration):
- Effect on installed clients (wire format, published table versions):
- How to roll back:

## Checklist

- [ ] Commits follow [Conventional Commits](https://www.conventionalcommits.org/) (`feat`, `fix`, `refactor`, `docs`, `test`, `chore`, `perf`, `ci`).
- [ ] No game data, art, APKs or other binaries are added.
- [ ] No real hostnames, IPs, tokens, keys or personal data; placeholders (`example.com`, `203.0.113.x`, `<placeholder>`) only.
- [ ] Tests were added or updated for the changed behaviour.
- [ ] Documentation was updated where behaviour or commands changed.
