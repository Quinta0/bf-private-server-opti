"""Client table versions carrying the EU units (scripts/gen_eu_units.py).

    python3 -I scripts/build_eu_tables.py --base DIR [--base DIR ...] --out BALANCE_ROOT

Each table is rebuilt from the newest version found in the --base folders
(the archive's mst/ for the shipped tables; for prod, the live parts of the
tables already published as overlay files), with every row of deploy/mst whose
id that version lacks appended at the end. Nothing else changes.

Writes <out>/content/<TABLE>/Ver<N>_<key>[_i].dat, then manifest.json last,
for the server's mstVersions to announce. Parts are at most 400000 bytes of
JSON, like the published overlay parts. Versions are fixed below and must
be new on the target: a client never re-downloads a version number it
already has.
FE_SKILL/UNIT_FE_SKILL went 650 -> 651 when Htilit's 70270 became a Light Barrier.
"""
import argparse
import base64
import json
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_eu_units import UNITS  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
PART_BYTES = 400_000

# table: (server file, wrapper, id fields, client file key, AES key, version)
TABLES = {
    'F_UNIT_MST': ('unit_mst.json', '2r9cNSdt', ('pn16CNah',), '2r9cNSdt', '7nL1WTUb', 1106),
    'F_UNIT_CGS_MST': ('unit_cgs_mst.json', '07bx6EXN', ('pn16CNah',), '6rjmY7tV', 'Y7ezNyn2', 850),
    'F_SKILL_MST': ('skill_mst.json', '8aiBoHg5', ('nj9Lw7mV',), 'wkCyV73D', 'JA03wvHG', 1600),
    'F_SKILL_LEVEL_MST': ('skill_level_mst.json', '3ktU0D2e', ('nj9Lw7mV',), 'zLIvD5o2', 'rF97pVdT', 1504),
    'F_LEADER_SKILL_MST': ('leader_skill_mst.json', '2bGtP7vV', ('oS3kTZ2W',), '4dE8UKcw', 'HGYZ1Ag5', 760),
    'F_EXTRA_PASSIVE_SKILL_MST': ('extra_passive_skill_mst.json', 'z5U7utsm', ('cP83zNsv',),
                                  'kP4pTJ7n', 'PDGK5sk3', 800),
    'F_FE_SKILL_MST': ('fe_skill_mst.json', '2h9r3yEY', ('ri6D9yBi',), '2h9r3yEY', 'w3Z9EeUR', 651),
    'F_UNIT_FE_SKILL_MST': ('unit_fe_skill_mst.json', 'kXes8fSi', ('pn16CNah', 'ri6D9yBi'),
                            '8gu2U4Mh', 'dc0jxn8u', 651),
    'F_UNIT_FE_CATEGORY_MST': ('unit_fe_category_mst.json', 'kvSPgNAk', ('pn16CNah', 'VgU78CYj'),
                               'nd18wpsy', 'XZvyRf62', 600),
}


def aes(key: str, data: bytes, decrypt: bool) -> bytes:
    cmd = ['openssl', 'enc', '-aes-128-ecb', '-K', key.encode().ljust(16, b'\0').hex(), '-nopad']
    return subprocess.run(cmd + (['-d'] if decrypt else []), input=data, capture_output=True, check=True).stdout


def decrypt_part(blob: bytes, key: str) -> list:
    plain = aes(key, base64.b64decode(blob), True)
    return json.loads(plain[:-plain[-1]])


def encrypt_part(rows: list, key: str) -> bytes:
    plain = json.dumps(rows, ensure_ascii=False, separators=(',', ':')).encode()
    pad = 16 - len(plain) % 16
    return base64.b64encode(aes(key, plain + bytes([pad]) * pad, False))


def newest(bases: list, file_key: str) -> tuple[int, list]:
    found = {}
    for base in bases:
        for f in base.iterdir():
            m = re.fullmatch(rf'Ver(\d+)_{file_key}(?:_(\d+))?\.dat', f.name)
            if m:
                found.setdefault(int(m.group(1)), {})[int(m.group(2) or 1)] = f
    if not found:
        raise SystemExit(f'no parts for {file_key} in {bases}')
    version = max(found)
    parts = found[version]
    if sorted(parts) != list(range(1, len(parts) + 1)):
        raise SystemExit(f'Ver{version}_{file_key}: parts {sorted(parts)} are not complete')
    return version, [parts[i] for i in sorted(parts)]


def split(rows: list) -> list:
    parts, current, size = [], [], 2
    for row in rows:
        n = len(json.dumps(row, ensure_ascii=False, separators=(',', ':')).encode()) + 1
        if current and size + n > PART_BYTES:
            parts.append(current)
            current, size = [], 2
        current.append(row)
        size += n
    return parts + [current]


def server_rows(server_file: str, wrapper: str) -> list:
    return json.loads((ROOT / 'deploy/mst' / server_file).read_text(encoding='utf-8'))[wrapper]


def eu_row_test():
    """Which server rows belong to an EU unit (the server copies hold server-only rows too)."""
    units = {str(u) for u in UNITS}
    skills = units | {p + u for u in units for p in '12'}
    sp = {r['ri6D9yBi'] for r in server_rows('unit_fe_skill_mst.json', 'kXes8fSi') if r['pn16CNah'] in units}

    def test(table: str, row: dict) -> bool:
        if table == 'F_FE_SKILL_MST':
            return row['ri6D9yBi'] in sp
        return any(row.get(f) in skills for f in ('pn16CNah', 'nj9Lw7mV', 'oS3kTZ2W', 'cP83zNsv'))
    return test


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', type=Path, action='append', required=True)
    ap.add_argument('--out', type=Path, required=True)
    args = ap.parse_args()
    is_eu = eu_row_test()
    for table, (server_file, wrapper, ids, file_key, key, version) in TABLES.items():
        base_version, files = newest(args.base, file_key)
        rows = [r for f in files for r in decrypt_part(f.read_bytes(), key)]
        ident = lambda r: tuple(r[i] for i in ids)  # noqa: E731
        have = {ident(r) for r in rows}
        added = [r for r in server_rows(server_file, wrapper) if is_eu(table, r) and ident(r) not in have]
        if not added:
            print(f'{table}: Ver{base_version} already has every EU row, skipped')
            continue
        if version <= base_version:
            raise SystemExit(f'{table}: version {version} is not above the base Ver{base_version}')
        folder = args.out / 'content' / table
        folder.mkdir(parents=True, exist_ok=True)
        parts = split(rows + added)
        for i, part in enumerate(parts, 1):
            name = f'Ver{version}_{file_key}' + (f'_{i}' if len(parts) > 1 else '') + '.dat'
            if (folder / name).exists():
                raise SystemExit(f'{folder / name} exists: never rewrite a published version')
            (folder / name).write_bytes(encrypt_part(part, key))
        manifest = {'table': table, 'file_key': file_key, 'version': version,
                    'file_count': len(parts), 'record_num': len(rows) + len(added)}
        (folder / 'manifest.json').write_text(json.dumps(manifest) + '\n')
        print(f'{table}: Ver{base_version} {len(rows)} rows + {len(added)} -> Ver{version} in {len(parts)} part(s)')


if __name__ == '__main__':
    main()
