#!/usr/bin/env python3
"""Wire test: a Frontier Gate run costs one Hunter Orb and carries on floor to floor (ticket #37).

    .venv/bin/python -I <wt>/scripts/test_frontier_gate_run_wire.py --src <wt> --prepare   # server STOPPED
    SRC=<wt> NAME=bf-fgate PORT=9983 DETACH=1 docker/run.sh
    .venv/bin/python -I <wt>/scripts/test_frontier_gate_run_wire.py --src <wt> --url http://127.0.0.1:9983

--prepare writes the TEST save while no server holds it: the top account gets a known session token,
3 Hunter Orbs and no open run (out/qa/fgate/account.json).

The run plays the client's real request order, taken from the prod logs of 2026-10-19:
FrontierGateStart before EVERY floor (also ~6 s after each Continue and on re-entry after a Save),
with the borrowed helper's party row carrying user_unit_id "FROGATE_FRIEND_ID".  It checks that

  1. FrontierGateInfo offers Noel's supports (NungTq5g) on every gate,
  2. the first Start takes one orb, opens run N, and echoes the whole party, helper included,
  3. MissionStart / MissionEnd carry the run's floors, score and support,
  4. Continue, then the next floor's Start, resume run N with no orb and keep the floors,
  5. Save + re-entry resumes it too,
  6. Retire pays the run and closes it; the next Start opens run N+1 for one orb,
  7. with no orbs, a NEW run is refused but the open one still resumes.

Requires pycryptodome.
"""
from __future__ import annotations

import argparse
import base64
import gzip
import hashlib
import json
import secrets
import sqlite3
import sys
import time
import urllib.request
from pathlib import Path

from Crypto.Cipher import AES

INFO = ('M17pPotk', 'sochkegz')
START = ('l3lkDBSc', 'vYPCD34q')
CONTINUE = ('uiFIMUH6', 'ZiosS4cd')
SAVE = ('Ng73nFHJ', '4SdtoczN')
END = ('cAJp7U4l', 'Vvpy7qZR')
MISSION_START = ('jE6Sp0q4', 'csiVLDKkxEwBfR70')
MISSION_END = ('9TvyNR5H', 'oINq0rfUFPx5MgmT')
CHALLENGE_BASE = ('nUAW2B0a', 'uE5Tsv6P')

GATE, OTHER_GATE, MISSION = 1, 2, 3000010
SUPPORT = '100'
OFFERED = {str(i) for i in (100, 101, 102, 200, 201, 202, 300, 301, 302, 400, 401, 402,
                            500, 501, 600, 601, 602, 700, 701, 702)}
HELPER = 'FROGATE_FRIEND_ID'

failures: list[str] = []


def check(ok: bool, what: str) -> None:
    print(('ok    ' if ok else 'FAIL  ') + what)
    if not ok:
        failures.append(what)


def call(url: str, acc: dict, api: tuple[str, str], body: dict) -> dict:
    group, key = api
    # Login first, as the client sends it: a body that fails to parse part-way still names its user.
    body = {'IKqx1Cn9': [{'iN7buP2h': acc['login'], 'h7eY3sAK': acc['user'], 'iN7buP1i': acc['token']}], **body}
    k = key.encode().ljust(16, b'\0')[:16]
    raw = json.dumps(body).encode()
    pad = 16 - len(raw) % 16
    cipher = AES.new(k, AES.MODE_ECB)
    envelope = {'F4q6i9xe': {'Hhgi79M1': group, 'aV6cLn3v': 'fgate'},
                'a3vSYuq2': {'Kn51uR4Y': base64.b64encode(cipher.encrypt(raw + bytes([pad]) * pad)).decode()}}
    req = urllib.request.Request(url, json.dumps(envelope).encode(), {'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=120) as r:
        data = r.read()
        if r.headers.get('Content-Encoding') == 'gzip':
            data = gzip.decompress(data)
    outer = json.loads(data)
    if 'a3vSYuq2' not in outer:
        raise SystemExit(f'{group} refused: {outer}')
    plain = cipher.decrypt(base64.b64decode(outer['a3vSYuq2']['Kn51uR4Y']))
    return json.loads(plain[:-plain[-1]])


# One unit row exactly as the client sends it in FrontierGateStart (52xDBRGr, prod 2026-10-19):
# 38 fields, every value a quoted string -- unit_img_type (2pAyFjmZ) and omni_level (49sa3sld)
# included, which an int-typed parse once choked on after the first unit.
UNIT_ROW = {
    'nBTx56W9': '1', 'D9wXQI2V': '150', 'd96tuT2E': '0', 'gQInj3H6': '5557931', 'e7DK0FQT': '9550',
    'cuIWp89g': '0', 'TokWs1B3': '1250', 'ISj9u5VL': '0', '67CApcti': '3967', 'RT4CtH5d': '0', 't4m1RH6Y': '740',
    'D6bKH5eV': '0', 'q08xLEsy': '3524', 'GcMD0hy6': '0', 'e6mY8Z0k': '400', '3CsiQA0h': '0', 'PWXu25cg': '2958',
    'C1HZr3pb': '0', 'X6jf8DUw': '500', 'XJs2rPx0': '0', 'iNy0ZU5M': '6', '3NbeC8AB': '10', 'RQ5GnFE2': '10',
    'Ge8Yo32T': '37200', '0R3qTPK9': '19689', 'mZA7fH2v': '818911', 'RXfC31FA': '21983', '2pAyFjmZ': '0',
    'LjY4DfRg': '8900008', 'Fnxab5CN': '1@1000000155/11@1010100007:1010100052:1010100032', '49sa3sld': '0',
}


def party(user: str) -> tuple[list, list]:
    """Two own units and a borrowed helper, shaped like a real FrontierGateStart."""
    deck, units = [], []
    for order, (uid, unit) in enumerate((('900001', '860638'), ('900002', '851018'))):
        deck.append({'zsiAn9P1': '0', 'edy7fq3L': uid, 'gr48vsdJ': '0' if order == 0 else '1', 'XuJL4pc5': str(order),
                     'a3qJ6QhX': '0', '3WMz78t6': '0', 'gU2xtQ0V': '0', 'nIGZ1X9C': '0'})
        units.append({'h7eY3sAK': user, 'edy7fq3L': uid, 'pn16CNah': unit, **UNIT_ROW,
                      'oS3kTZ2W': unit, 'nj9Lw7mV': unit, 'iEFZ6H19': '1' + unit, 'cP83zNsv': unit})
    deck.append({'zsiAn9P1': '1', 'edy7fq3L': HELPER, '98WfKiyA': 'DCF00004', 'gr48vsdJ': '1', 'XuJL4pc5': '5',
                 'a3qJ6QhX': '0', '3WMz78t6': '0', 'gU2xtQ0V': '0', 'nIGZ1X9C': '0'})
    units.append({'h7eY3sAK': 'dcfhelper', 'edy7fq3L': HELPER, '98WfKiyA': 'DCF00004', 'pn16CNah': '850768', **UNIT_ROW,
                  'oS3kTZ2W': '850768', 'nj9Lw7mV': '850768', 'iEFZ6H19': '1850768', 'cP83zNsv': '850768'})
    return deck, units


def orbs(url: str, acc: dict) -> int:
    team = call(url, acc, CHALLENGE_BASE, {})['kN2i7qds']
    team = team[0] if isinstance(team, list) else team
    return int(team['38sHatGk'])


def start(url: str, acc: dict, gate: int, deck: list, units: list, support: str | None = None) -> dict:
    entry = {'hBNPQAU0': str(gate)}
    if support:
        entry['NungTq5g'] = support
    return call(url, acc, START, {'6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}], 'Mg8K8Y1a': [entry],
                                  'MxmCpDRC': deck, '52xDBRGr': units})


def num(reply: dict) -> dict:
    n = reply.get('Mg8K8Y1a') or {}
    return n[0] if isinstance(n, list) else n


def battle(url: str, acc: dict, run: dict, floors: int, score: int) -> tuple[dict, dict]:
    """One floor: MissionStart in the run, then a won MissionEnd reporting `floors` and `score`."""
    if '2FQdEbG3' not in run:
        return {}, {}  # no run was opened: the checks around it already failed
    started = call(url, acc, MISSION_START, {
        '6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}],
        '9Q1Lq5FS': [{'h7eY3sAK': '0', 'J3stQ7jd': '0', 'j28VNcUW': str(MISSION), 'jkldTrhL': '0', 'Z0Y4RoD7': '0',
                      'nA95Bdj6': '0', '5Z1LNoyH': '0', 'u1iPEVUq': '-1'}],
        'JzS3uxsZ': [{'0b4efi1W': '1'}],
        'Mg8K8Y1a': [{'hBNPQAU0': run['hBNPQAU0'], 'NungTq5g': run.get('NungTq5g', '0'), '2FQdEbG3': run['2FQdEbG3']}]})
    serial = int(started['Kz7qfSs5'][0]['k9cxD7Ba'])
    info = (started.get('eIQ79KO2') or [{}])[0]
    report = {'TPR79fyI': '0', '7cev0fzQ': '', 'pG2n1A28': str(floors), 'H7f9NAXu': '', 'PYbfxpTp': str(score)}
    if info.get('NungTq5g'):
        report['NungTq5g'] = info['NungTq5g']
    fight = {k: '0' for k in ('Najhr8m6', 'HTVh8a65', 'hoG2ieT5', '6PLsn8xo', '5NRJQ1LU', 'XP06YWdT', 'U8uZLA34',
                              'rZQJF5G9', 'TW1Mrtp5', 'e6BKoYy9')}
    fight.update({'47cJBUxz': '', '4T0Q2Bh5': '', '3MAT6quo': ''})
    ended = call(url, acc, MISSION_END, {
        '6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}], 'Kz7qfSs5': [{'k9cxD7Ba': str(serial), 'j3g5P4cq': '2'}],
        'rXvA1E5y': [fight], 'dPM7oJDl': [{'hBNPQAU0': run['hBNPQAU0']}], 'eIQ79KO2': [report]})
    return info, ended


def helper_kept(reply: dict) -> bool:
    deck = [d for d in reply.get('MxmCpDRC', []) if d.get('edy7fq3L') == HELPER and d.get('98WfKiyA') == 'DCF00004']
    units = [u for u in reply.get('52xDBRGr', []) if u.get('edy7fq3L') == HELPER]
    whole = all(all(k in u for k in ('pn16CNah', '2pAyFjmZ', 'LjY4DfRg', 'Fnxab5CN', '49sa3sld'))
                for u in reply.get('52xDBRGr', []))
    return (len(reply.get('MxmCpDRC', [])) == 3 and len(reply.get('52xDBRGr', [])) == 3
            and len(deck) == 1 and len(units) == 1 and whole)


def prepare(src: Path) -> None:
    token = secrets.token_hex(16)
    with sqlite3.connect(src / 'deploy/gme.sqlite', timeout=30) as db:
        user, login = db.execute('SELECT id, gumi_user_id FROM user_info ORDER BY level DESC LIMIT 1').fetchone()
        db.execute('UPDATE gumi_live_users SET token_hash=? WHERE id=?', (hashlib.sha256(token.encode()).hexdigest(), login))
        # 3 orbs and a rest deadline a day away: nothing regenerates during the run.
        db.execute('UPDATE user_info SET hunter_orbs = 3, hunter_orb_rest_ts = ? WHERE id = ?',
                   (int(time.time()) + 86400, user))
        db.execute('DELETE FROM user_frontier_gate_run WHERE user_id = ?', (user,))
    out = src / 'out/qa/fgate'
    out.mkdir(parents=True, exist_ok=True)
    (out / 'account.json').write_text(json.dumps({'login': str(login), 'user': str(user), 'token': token}))
    print(f'prepared {user}: 3 Hunter Orbs, no open run')


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--src', type=Path, required=True)
    ap.add_argument('--url', default='http://127.0.0.1:9983')
    ap.add_argument('--prepare', action='store_true')
    args = ap.parse_args()
    if args.prepare:
        prepare(args.src)
        return 0
    acc = json.loads((args.src / 'out/qa/fgate/account.json').read_text())
    url = f'{args.url.rstrip("/")}/bf/gme/action.php'
    deck, units = party(acc['user'])

    gates = call(url, acc, INFO, {})['dPM7oJDl']
    offered = [set(filter(None, str(g.get('NungTq5g', '')).split(','))) for g in gates]
    check(bool(gates) and all(OFFERED <= o for o in offered), f'Noel offers the 20 boosts on all {len(gates)} gates')

    o0 = orbs(url, acc)
    first = start(url, acc, GATE, deck, units, SUPPORT)
    run = num(first)
    n = int(run.get('2FQdEbG3', 0))
    check(n > 0 and run.get('NungTq5g') == SUPPORT, f'first Start opens run {n} with support {run.get("NungTq5g")}')
    check(orbs(url, acc) == o0 - 1, f'first Start takes one orb ({o0} -> {orbs(url, acc)})')
    check(helper_kept(first), 'first Start echoes 3 deck rows and 3 units, helper included')

    info, ended = battle(url, acc, run, floors=5, score=1000)
    check(info.get('pG2n1A28') == '0' and info.get('NungTq5g') == SUPPORT,
          f'floor 1 starts at 0 floors with the support ({info.get("pG2n1A28")}, {info.get("NungTq5g")})')
    back = (ended.get('eIQ79KO2') or [{}])[0]
    check(back.get('pG2n1A28') == '5', f'MissionEnd keeps 5 floors ({back.get("pG2n1A28")})')

    cont = call(url, acc, CONTINUE, {'Mg8K8Y1a': [{'2FQdEbG3': str(n)}]})
    check(int(num(cont).get('2FQdEbG3', 0)) == n and helper_kept(cont), 'Continue hands back run and party, helper included')

    # The client's next request, ~6 s later: Start again with the party it was given and no support.
    again = start(url, acc, GATE, cont.get('MxmCpDRC', []), cont.get('52xDBRGr', []))
    run2 = num(again)
    check(int(run2.get('2FQdEbG3', 0)) == n and run2.get('NungTq5g') == SUPPORT,
          f'next floor Start resumes run {run2.get("2FQdEbG3")}, support {run2.get("NungTq5g")} kept')
    check(orbs(url, acc) == o0 - 1, 'next floor Start takes no orb')
    check(helper_kept(again), 'next floor keeps the helper')
    info, ended = battle(url, acc, run2, floors=10, score=2500)
    check(info.get('pG2n1A28') == '5' and info.get('PYbfxpTp') == '1000',
          f'floor 2 starts at 5 floors, 1000 points ({info.get("pG2n1A28")}, {info.get("PYbfxpTp")})')

    call(url, acc, SAVE, {'Mg8K8Y1a': [{'hBNPQAU0': str(GATE), 'j3g5P4cq': '4'}], 'MxmCpDRC': deck, '52xDBRGr': units})
    back_in = start(url, acc, GATE, deck, units)
    check(int(num(back_in).get('2FQdEbG3', 0)) == n and orbs(url, acc) == o0 - 1, 'Save + re-entry resumes, no orb')
    info, _ = battle(url, acc, num(back_in), floors=12, score=3000)
    check(info.get('pG2n1A28') == '10', f're-entered floor starts at 10 floors ({info.get("pG2n1A28")})')

    end = call(url, acc, END, {'Mg8K8Y1a': [{'2FQdEbG3': str(n), 'j3g5P4cq': '3'}]})
    check(bool(end.get('mu0kXAlV')), f'Retire returns the result (grade {(end.get("mu0kXAlV") or [{}])[0].get("Cdv07KEU")})')

    # Retire deletes the run row, so the next run's handle may start over; what
    # matters is that it is charged and starts from nothing.
    fresh = start(url, acc, GATE, deck, units, SUPPORT)
    m = int(num(fresh).get('2FQdEbG3', 0))
    check(m > 0 and orbs(url, acc) == o0 - 2, f'after Retire a Start opens a new run ({m}) for one orb')
    info, _ = battle(url, acc, num(fresh), floors=3, score=100)
    check(info.get('pG2n1A28') == '0', 'the new run starts at 0 floors')

    other = start(url, acc, OTHER_GATE, deck, units)
    k = int(num(other).get('2FQdEbG3', 0))
    check(k > 0 and k != m and orbs(url, acc) == o0 - 3, f'another gate opens its own run ({k}) for one orb')
    check(orbs(url, acc) == 0, 'orbs are now 0')
    refused = start(url, acc, GATE, deck, units)
    check('Mg8K8Y1a' not in refused, 'with no orbs a NEW run is refused')
    resumed = start(url, acc, OTHER_GATE, deck, units)
    check(int(num(resumed).get('2FQdEbG3', 0)) == k, 'with no orbs the open run still resumes')
    call(url, acc, END, {'Mg8K8Y1a': [{'2FQdEbG3': str(k), 'j3g5P4cq': '3'}]})

    print(f'{len(failures)} failure(s)')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
