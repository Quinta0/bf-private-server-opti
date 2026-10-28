# Reverse-engineering notes

Interoperability notes about the Brave Frontier v2.19.6 Android client, written while making the
[server](../server/README.MD) speak the client's protocol and while building
[`tools/patch_apk.py`](../tools/patch_apk.py). They record *what the client expects* (request
routes, keys, offsets of the bytes the patcher changes) so the server and tools can be checked
against it.

> **Analysis notes only.** This folder contains no client binaries, no decompiled source and no
> game assets. The client library (`re/lib/`), full disassembly listings (`re/full.s`), string dumps
> and scratch work (`re/work/`) are gitignored and must never be committed. To reproduce anything
> here you need your own copy of the client, obtained separately. This is an unofficial fan
> project for preservation and educational purposes, not affiliated with or endorsed by gumi Inc.;
> see the [root README](../README.md).

## Notes

| File | Contents |
|---|---|
| [`notes/req_table.tsv`](notes/req_table.tsv) | One row per client request class: class name, `/actionSymbol/...php` route, request group id and AES key. |
| [`notes/resp_keys.tsv`](notes/resp_keys.tsv) | Response group ids and the response class each one maps to. |
| [`notes/reqmap.json`](notes/reqmap.json) | Per class, the obfuscated keys the client uses for each accessor (for example `getRequestID`, `getEncodeKey`). |
| [`notes/missing_requests.json`](notes/missing_requests.json) | Client requests the server did not implement at the time of writing, with route, group id and key. |
| [`notes/android_boot_re.json`](notes/android_boot_re.json) | Verified findings on Android boot, in four entries: service discovery (`/dls`), login, the patch sites used by `patch_apk.py`, and installation. |
| [`notes/https_transfer_re.json`](notes/https_transfer_re.json) | Verified findings in two entries: an HTTPS build (the pinned root certificate slot and certificate requirements) and the account-transfer flow. |

The two `*_re.json` files are lists of findings; each entry has a `key`, a `verified` section that
re-checks an earlier analysis against the binary, a running server and the crypto, and a
`verifier_ok` flag.

## Scripts

Small helpers that work on your own local disassembly. `W` is a work folder holding `full.s`, the
output of `llvm-objdump -d` (or similar) for the client's `libgame.so`.

| Script | Usage |
|---|---|
| [`scripts/callers.sh`](scripts/callers.sh) | `callers.sh W pattern`: the containing function of every branch whose target matches `pattern`, counted and demangled. |
| [`scripts/func.sh`](scripts/func.sh) | `func.sh W pattern`: print the disassembly of functions whose label matches `pattern`, demangled. |
| [`scripts/dxref.py`](scripts/dxref.py) | `dxref.py full.s lo hi`: list `(function, instruction, target)` where an `adrp` + `add`/`ldr` pair resolves into `[lo, hi)`. |
| [`scripts/cstr.py`](scripts/cstr.py) | `cstr.py libgame.so s|q|b VA...`: read a C string (`s`), a 64-bit word (`q`) or 64 bytes (`b`) at virtual addresses of an ELF file. |

They need a POSIX shell, `awk`, `c++filt` and Python 3.

## Related

- [server/README.MD](../server/README.MD): the server that implements these requests
- [tools/README.md](../tools/README.md): `patch_apk.py`, `test_patch_apk.py`, `own_helper_cave.s`
- [CONTRIBUTING.md](../CONTRIBUTING.md)
