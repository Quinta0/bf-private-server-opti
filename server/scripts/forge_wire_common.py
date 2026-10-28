"""Shared harness for the overlay wire tests (scripts/test_forge_*_wire.py).

A throwaway server in Docker (container from bf-server-build running
<src>/out/build-linux-arm64/standalone_frontend/gimuserverw), its save in a fresh
named volume, a scratch balance root bind-mounted read-only, and quest helpers.
"""
from __future__ import annotations

import hashlib
import json
import os
import subprocess
import time
from pathlib import Path

from test_fair_summon import fetch, gme_call, wire_error  # noqa: F401 - re-exported

IMAGE = 'bf-server-build'
MISSION_START = ('jE6Sp0q4', 'csiVLDKkxEwBfR70')
MISSION_END = ('9TvyNR5H', 'oINq0rfUFPx5MgmT')
GACHA_LIST = ('Uo86DcRh', '8JbxFvuSaB2CK7Ln')
GACHA_ACTION = ('F7JvPk5H', 'bL9fipzaSy7xN2w1')
USER_INFO = ('cTZ3W2JG', 'ScJx6ywWEb0A3njT')
PERMIT_KEYS = ('yXNM8kL3', 'Y73tHKS8', 'Y73mHKS8')


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def table(src: Path, name: str) -> tuple[str, list]:
    data = json.loads((src / 'deploy' / 'mst' / name).read_text(encoding='utf-8'))
    wrapper = next(iter(data))
    return wrapper, data[wrapper]


def run(cmd: list[str], check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, check=check, timeout=600)


def write_atomic(path: Path, data: bytes) -> None:
    """A new inode every time, so the server's file key always changes."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.tmp')
    tmp.write_bytes(data)
    os.replace(tmp, path)


def config(src_config: Path) -> dict:
    """deploy/config.docker.json with absolute paths, the save in the volume and the scratch balance root."""
    text = src_config.read_text(encoding='utf-8')
    lines = [line.split(' //')[0] for line in text.splitlines() if not line.strip().startswith('//')]
    cfg = json.loads('\n'.join(lines))
    cfg['app']['document_root'] = '/game_content'
    cfg['app']['log']['log_level'] = 'INFO'
    cfg['db_clients'][0]['filename'] = '/data/gme.sqlite'
    plugin = cfg['plugins'][0]['config']
    plugin['extra_log']['enable'] = False
    plugin['extra_log']['directory'] = '/tmp/bf-log'
    plugin['server'].update({'mst_root': '/src/deploy/mst', 'system_root': '/src/deploy/system',
                             'archive_root': '/src/deploy/archive', 'balance_root': '/balance'})
    return cfg


class Server:
    def __init__(self, src: Path, work: Path, port: int, assets: Path, container: str, volume: str):
        self.src, self.work, self.port, self.assets = src, work, port, assets
        self.container, self.volume = container, volume
        self.url = f'http://127.0.0.1:{port}'
        (work / 'balance' / 'server').mkdir(parents=True, exist_ok=True)
        (work / 'config.json').write_text(json.dumps(config(src / 'deploy' / 'config.docker.json'), indent=1))

    def start(self) -> None:
        self.remove()
        run(['docker', 'volume', 'create', self.volume])
        run(['docker', 'run', '-d', '--name', self.container, '-p', f'127.0.0.1:{self.port}:9960', '-w', '/app',
             '-v', f'{self.src}:/src:ro', '-v', f'{self.assets}:/game_content:ro',
             '-v', f'{self.work / "balance"}:/balance:ro', '-v', f'{self.work / "config.json"}:/app/config.json:ro',
             '-v', f'{self.volume}:/data', IMAGE,
             '/src/out/build-linux-arm64/standalone_frontend/gimuserverw', '/app/config.json'])
        deadline = time.time() + 600
        while time.time() < deadline:
            state = run(['docker', 'inspect', '-f', '{{.State.Running}}', self.container], check=False).stdout.strip()
            if state != 'true':
                raise RuntimeError('the server exited during boot:\n'
                                   + run(['docker', 'logs', '--tail', '40', self.container], check=False).stderr[-3000:])
            try:
                if json.loads(fetch(f'{self.url}/bf/gme/forgeStatus.php')[1]).get('booted_at'):
                    return
            except Exception:  # noqa: BLE001 - not listening yet
                pass
            time.sleep(2)
        raise RuntimeError('the server did not answer within 600 s')

    def remove(self) -> None:
        run(['docker', 'rm', '-f', self.container], check=False)
        run(['docker', 'volume', 'rm', '-f', self.volume], check=False)

    def status(self) -> dict:
        return json.loads(fetch(f'{self.url}/bf/gme/forgeStatus.php')[1])


def start_body(login: list, mission: int, deck: int = 0, helper: str = '0') -> dict:
    return {'IKqx1Cn9': login, '6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}],
            '9Q1Lq5FS': [{'h7eY3sAK': helper, 'J3stQ7jd': '0', 'j28VNcUW': str(mission), 'jkldTrhL': '0',
                          'Z0Y4RoD7': str(deck), 'nA95Bdj6': '0', '5Z1LNoyH': '0', 'u1iPEVUq': '-1'}],
            'JzS3uxsZ': [{'0b4efi1W': '1'}]}


def end_body(login: list, serial: str, won: bool = True) -> dict:
    battle = {k: '0' for k in ('Najhr8m6', 'HTVh8a65', 'hoG2ieT5', '6PLsn8xo', '5NRJQ1LU', 'XP06YWdT',
                               'U8uZLA34', 'rZQJF5G9', 'TW1Mrtp5', 'e6BKoYy9')}
    battle.update({'47cJBUxz': '', '4T0Q2Bh5': '', '3MAT6quo': ''})
    return {'IKqx1Cn9': login, '6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}],
            'Kz7qfSs5': [{'k9cxD7Ba': str(serial), 'j3g5P4cq': '2' if won else '3'}], 'rXvA1E5y': [battle]}


def permit_rows(reply: dict) -> list[dict]:
    return [row for key in PERMIT_KEYS for row in (reply.get(key) or [])]


def permitted(reply: dict, key: str, ident: int) -> bool:
    return any(row.get(key) == str(ident) for row in permit_rows(reply))
