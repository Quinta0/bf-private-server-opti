#!/usr/bin/env python3
"""Wire test: Balance Forge MST and archive overlays, and forgeStatus.php.

    PYTHONPATH=<repo>/tools <repo>/.venv/bin/python \
        scripts/test_forge_overlays_wire.py [--src wt/forge] [--port 9987] [--assets DIR] [--keep]

(PYTHONPATH brings in the fair-summon harness, tools/test_fair_summon.py.)

Boots its own server in Docker (container bf-forge-overlays from bf-server-build,
running <src>/out/build-linux-arm64/standalone_frontend/gimuserverw), with the
save in a fresh named volume and a scratch balance root holding:

  server/mst/gacha_mst.json            banner 18060: price 3 -> 7 gems, a new name
  server/mst/achievement_trade_mst.json  Merit offer 91000059 (Ignis Shard): 50 -> 77 points, limit 400 -> 3
  server/mst/unit_evo_mst.json         Fencer Vargas 10011 -> 10012: 2,500 -> 12,345 Zel, Fire Nymph -> Miracle Totem
  server/mst/mission_mst.json          mission 10: first-clear rewards 4,321 Zel + 2 gems
  server/mst/defines_mst.json          object mode: energy refill gem cost
  server/mst/unit_fe_skill_mst.json    groups mode: one unit's tree written back
  server/archive/gacha.json            18060 pools one unit; a record with an unknown unit (rejected)
  server/archive/event_exchange.json   a new Brave Insignia offer, a price change, a delete, two bad records
  server/archive/boss_drops.json       a valid whole-file replacement
  server/archive/login_campaign.json   day 1 pays 9 gems (the image: 1 gem + 200 Honor)
  BROKEN: server/mst/item_mst.json (a row with a column the table lacks),
          server/mst/dungeon_mst.json (text in a number column: passes the column checks,
          fails the typed parse after the merge),
          server/mst/unit_mst.json (refused: the unit balance file owns it),
          server/mst/gacha_info_mst.json (a table the server never reads),
          server/archive/summon_rotation.json (first_week a Tuesday),
          server/archive/not_a_table.json (nothing reads it)

and checks over the wire: the server boots; Initialize/GachaList/UserInfo carry
the new price and name; a 10-pull costs 70 gems and gives only the pooled unit;
the Merit shop charges 77 points and stops at 3; UnitEvo takes the new recipe's
material and Zel and refuses the old material; a first clear of mission 10
queues the two presents; the Bazaar lists, prices and sells the new offer and
drops the deleted one; the base summon rotation is still served; and
forgeStatus.php reports every file with the sha256 of the bytes written, the
counts, applied flags and plain reasons.  Removes its container and volume
(unless --keep).  Run it against nothing but its own throwaway server.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from test_fair_summon import (INITIALIZE, Database, Player, fetch, gme_call,  # noqa: E402
                              wire_error)

IMAGE = 'bf-server-build'
CONTAINER = 'bf-forge-overlays'
VOLUME = 'bf-forge-overlays-data'
COMMIT = 'forge-test-commit'

GATE, GATE_PRICE, GATE_NAME = '18060', '7', 'Forge Dawn Summon'
POOL_UNIT = 10715                       # Scarlet Wanderer Reda, the only unit 18060 rolls now
BAD_GATE, BAD_UNIT = 15850, 99999999
TYPO_GATE = 13330                       # a Divine gate; its record carries a misspelt field
MERIT, MERIT_PRICE, MERIT_LIMIT = '91000059', 77, 3
EVO_BASE, EVO_TARGET, EVO_ZEL, OLD_MATERIAL, NEW_MATERIAL = '10011', '10012', 12345, '10130', '50354'
MISSION, FIRST_CLEAR = '10', '3:0:4321:0:0,8:0:2:0:0'
TOKEN = 13                              # Brave Insignia: a permanent Bazaar
NEW_OFFER = {'id': '13-7-30004', 'token_id': TOKEN, 'name': 'Forge Divine Sword', 'price': 3, 'limit': 2,
             'reward_type': 7, 'target_id': 30004, 'quantity': 1, 'source': 'forge'}
PRICED_OFFER, NEW_PRICE, DELETED_OFFER = '13-6-730312', 11, '13-6-730322'
LOGIN_GEMS = 9                          # login calendar day 1 (the image pays 1 gem + 200 Honor)

GACHA_ACTION = ('F7JvPk5H', 'bL9fipzaSy7xN2w1')
GACHA_LIST = ('Uo86DcRh', '8JbxFvuSaB2CK7Ln')
USER_INFO = ('cTZ3W2JG', 'ScJx6ywWEb0A3njT')
MERIT_LIST = ('YPBU7MD8', 'AKjzyZ81')
MERIT_TRADE = ('m9LiF6P2', '0IWC9LVq')
UNIT_EVO = ('0gUSE84e', 'biHf01DxcrPou5Qt')
MISSION_START = ('jE6Sp0q4', 'csiVLDKkxEwBfR70')
MISSION_END = ('9TvyNR5H', 'oINq0rfUFPx5MgmT')
BAZAAR_LIST = ('49zxdfl3', 'v2DfDSFl')
BAZAAR_BUY = ('SLf48fs0', 'Odiel30s')
EVO_SLOTS = ['85X6JHQA', 'wh3YRU08', '7MxucW2J', 'j7fTS3ca', 'Hb8yfmv7', 'Voht18AP', '3g8brFoq', 'agp4CKEV', 'eKPWNoLn']


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def table(src: Path, name: str) -> tuple[str, list]:
    data = json.loads((src / 'deploy' / 'mst' / name).read_text(encoding='utf-8'))
    wrapper = next(iter(data))
    return wrapper, data[wrapper]


def row_of(rows: list, key: str, value: str) -> dict:
    return copy.deepcopy(next(r for r in rows if r[key] == value))


class Balance:
    """The scratch balance root; remembers the bytes of every file written."""

    def __init__(self, root: Path):
        self.root = root
        self.written: dict[str, bytes] = {}

    def write(self, kind: str, name: str, payload) -> None:
        data = payload if isinstance(payload, bytes) else json.dumps(payload, ensure_ascii=False, indent=1).encode()
        path = self.root / 'server' / kind / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        self.written[f'{kind}/{name}'] = data


def build_overlays(src: Path, balance: Balance) -> dict:
    """Writes every overlay; returns facts the checks need."""
    facts = {}

    wrapper, gachas = table(src, 'gacha_mst.json')
    gate = row_of(gachas, '7Ffmi96v', GATE)
    facts['gate_was'] = (gate['03UGMHxF'], gate['4N27mkt1'])
    gate.update({'03UGMHxF': GATE_PRICE, '4N27mkt1': GATE_NAME})
    balance.write('mst', 'gacha_mst.json', {'format': 1, 'table': 'gacha_mst.json', 'wrapper': wrapper,
                                            'key': ['7Ffmi96v'], 'rows': [gate]})

    wrapper, trades = table(src, 'achievement_trade_mst.json')
    offer = row_of(trades, 'Mdgsh04u', MERIT)
    offer.update({'3EWLm0sA': str(MERIT_PRICE), 'S8rdp9zk': str(MERIT_LIMIT)})
    balance.write('mst', 'achievement_trade_mst.json', {'format': 1, 'table': 'achievement_trade_mst.json',
                                                        'key': ['Mdgsh04u'], 'rows': [offer]})

    wrapper, evos = table(src, 'unit_evo_mst.json')
    recipe = row_of(evos, 'pn16CNah', EVO_BASE)
    facts['evo_was'] = (recipe['Rs7bCE3t'], recipe['85X6JHQA'])
    recipe.update({'Rs7bCE3t': str(EVO_ZEL), '85X6JHQA': NEW_MATERIAL})
    balance.write('mst', 'unit_evo_mst.json', {'format': 1, 'table': 'unit_evo_mst.json', 'rows': [recipe]})

    wrapper, missions = table(src, 'mission_mst.json')
    mission = row_of(missions, 'j28VNcUW', MISSION)
    mission['SiYs27Cj'] = FIRST_CLEAR
    balance.write('mst', 'mission_mst.json', {'format': 1, 'table': 'mission_mst.json', 'key': ['j28VNcUW'],
                                              'rows': [mission]})

    defines = json.loads((src / 'deploy' / 'mst' / 'defines_mst.json').read_text(encoding='utf-8'))
    facts['refill_was'] = defines['C8KkHGa7']
    facts['refill'] = '9' if defines['C8KkHGa7'] != '9' else '8'
    balance.write('mst', 'defines_mst.json', {'format': 1, 'table': 'defines_mst.json',
                                              'object': {'C8KkHGa7': facts['refill']}})

    wrapper, fe = table(src, 'unit_fe_skill_mst.json')
    unit = fe[0]['pn16CNah']
    group = [r for r in fe if r['pn16CNah'] == unit]
    facts['fe_rows'] = len(group)
    balance.write('mst', 'unit_fe_skill_mst.json', {'format': 1, 'table': 'unit_fe_skill_mst.json',
                                                    'group_key': ['pn16CNah'],
                                                    'groups': [{'key': [unit], 'rows': group}]})

    # Broken ones.
    wrapper, items = table(src, 'item_mst.json')
    item = row_of(items, 'kixHbe54', '30004')
    facts['item_name'] = item['c7Z6xDB2']
    item['c7Z6xDB2'] = 'SHOULD NOT SHOW'
    item['qp37xTDx'] = 'a typo'
    balance.write('mst', 'item_mst.json', {'format': 1, 'table': 'item_mst.json', 'rows': [item]})
    wrapper, dungeons = table(src, 'dungeon_mst.json')
    dungeon = row_of(dungeons, 'MHx05sXt', '10')
    dungeon['VjCY7rX4'] = 'one hundred'
    balance.write('mst', 'dungeon_mst.json', {'format': 1, 'table': 'dungeon_mst.json', 'rows': [dungeon]})
    balance.write('mst', 'unit_mst.json', {'format': 1, 'table': 'unit_mst.json', 'rows': []})
    balance.write('mst', 'gacha_info_mst.json', {'format': 1, 'table': 'gacha_info_mst.json', 'rows': []})

    # Archive.
    records = json.loads((src / 'deploy' / 'archive' / 'gacha.json').read_text(encoding='utf-8'))
    pooled = next(r for r in records if r['id'] == int(GATE))
    pooled = dict(pooled, pool=[{'unit_id': POOL_UNIT, 'weight': 100, 'name': 'Scarlet Wanderer Reda'}])
    bad = dict(next(r for r in records if r['id'] == BAD_GATE), pool=[{'unit_id': BAD_UNIT, 'weight': 5, 'name': 'nobody'}])
    typo = dict(next(r for r in records if r['id'] == TYPO_GATE), pool_typo=[])
    balance.write('archive', 'gacha.json', {'format': 1, 'file': 'gacha.json', 'records': [pooled, bad, typo]})

    offers = json.loads((src / 'deploy' / 'archive' / 'event_exchange.json').read_text(encoding='utf-8'))
    priced = dict(next(o for o in offers if o['id'] == PRICED_OFFER), price=NEW_PRICE)
    unknown_token = dict(NEW_OFFER, id='999-7-30004', token_id=999)
    missing_limit = {k: v for k, v in NEW_OFFER.items() if k != 'limit'} | {'id': '13-7-30005', 'target_id': 30005}
    balance.write('archive', 'event_exchange.json', {
        'format': 1, 'file': 'event_exchange.json', 'records': [NEW_OFFER, priced, unknown_token, missing_limit],
        'delete': [DELETED_OFFER]})

    drops = json.loads((src / 'deploy' / 'archive' / 'boss_drops.json').read_text(encoding='utf-8'))
    drops['chance_by_line_rarity'] = [min(100, c * 2) for c in drops['chance_by_line_rarity']]
    balance.write('archive', 'boss_drops.json', drops)

    calendar = json.loads((src / 'deploy' / 'archive' / 'login_campaign.json').read_text(encoding='utf-8'))
    calendar[0] = {'day': 1, 'rewards': [{'kind': 'gem', 'name': 'Gem', 'id': 0, 'count': LOGIN_GEMS}]}
    balance.write('archive', 'login_campaign.json', calendar)

    rotation = json.loads((src / 'deploy' / 'archive' / 'summon_rotation.json').read_text(encoding='utf-8'))
    rotation['first_week'] = '2026-10-27'   # a Tuesday
    balance.write('archive', 'summon_rotation.json', rotation)
    balance.write('archive', 'not_a_table.json', {'format': 1})
    return facts


def config(src_config: Path) -> dict:
    """deploy/config.docker.json with absolute paths, the save in the volume and the scratch balance root."""
    text = src_config.read_text(encoding='utf-8')
    # The file carries // comments (whole lines and after values); no value has " //" in it.
    lines = [line.split(' //')[0] for line in text.splitlines() if not line.strip().startswith('//')]
    cfg = json.loads('\n'.join(lines))
    cfg['app']['document_root'] = '/game_content'
    cfg['app']['log']['log_level'] = 'INFO'
    cfg['db_clients'][0]['filename'] = '/data/gme.sqlite'
    plugin = cfg['plugins'][0]['config']
    plugin['extra_log']['enable'] = False
    plugin['extra_log']['directory'] = '/tmp/bf-log'
    server = plugin['server']
    server.update({'mst_root': '/src/deploy/mst', 'system_root': '/src/deploy/system',
                   'archive_root': '/src/deploy/archive', 'balance_root': '/balance'})
    return cfg


def run(cmd: list[str], check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, check=check, timeout=600)


def start_server(src: Path, work: Path, port: int, assets: Path) -> str:
    run(['docker', 'rm', '-f', CONTAINER], check=False)
    run(['docker', 'volume', 'rm', '-f', VOLUME], check=False)
    run(['docker', 'volume', 'create', VOLUME])
    run(['docker', 'run', '-d', '--name', CONTAINER, '-p', f'127.0.0.1:{port}:9960',
         '-e', f'BF_SERVER_COMMIT={COMMIT}', '-w', '/app',
         '-v', f'{src}:/src:ro', '-v', f'{assets}:/game_content:ro',
         '-v', f'{work / "balance"}:/balance:ro', '-v', f'{work / "config.json"}:/app/config.json:ro',
         '-v', f'{VOLUME}:/data', IMAGE,
         '/src/out/build-linux-arm64/standalone_frontend/gimuserverw', '/app/config.json'])
    url = f'http://127.0.0.1:{port}'
    deadline = time.time() + 600
    while time.time() < deadline:
        state = run(['docker', 'inspect', '-f', '{{.State.Running}}', CONTAINER], check=False).stdout.strip()
        if state != 'true':
            raise RuntimeError('the server exited during boot:\n' + run(['docker', 'logs', '--tail', '40', CONTAINER],
                                                                        check=False).stderr[-3000:])
        try:
            status, body = fetch(f'{url}/bf/gme/forgeStatus.php')
            if status == 200 and json.loads(body).get('booted_at'):
                return url
        except Exception:  # noqa: BLE001 - not listening yet
            pass
        time.sleep(2)
    raise RuntimeError('the server did not answer within 600 s')


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--src', type=Path, default=Path(__file__).resolve().parents[1])
    ap.add_argument('--port', type=int, default=9987)
    ap.add_argument('--assets', type=Path, default=Path.home() / 'bf-archive' / 'assets')
    ap.add_argument('--keep', action='store_true', help='leave the container and its volume running')
    args = ap.parse_args()
    src = args.src.resolve()
    failures = 0

    def check(label: str, ok, detail='') -> bool:
        nonlocal failures
        failures += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {label}{'' if ok else '  -- ' + str(detail)[:400]}", flush=True)
        return bool(ok)

    work = Path(tempfile.mkdtemp(prefix='forge-overlays-'))
    try:
        balance = Balance(work / 'balance')
        facts = build_overlays(src, balance)
        (work / 'config.json').write_text(json.dumps(config(src / 'deploy' / 'config.docker.json'), indent=1))
        url = start_server(src, work, args.port, args.assets)
        check('the server boots with good, broken and unread overlays', True)

        # ---- forgeStatus.php ----------------------------------------------------------
        status = json.loads(fetch(f'{url}/bf/gme/forgeStatus.php')[1])
        mst, archive = status.get('mst', {}), status.get('archive', {})
        check('status: format 1, the commit from BF_SERVER_COMMIT, a balance root, a boot time',
              status.get('format') == 1 and status.get('server_commit') == COMMIT and status.get('balance_root') is True
              and isinstance(status.get('booted_at'), int), {k: status.get(k) for k in ('format', 'server_commit', 'balance_root', 'booted_at')})
        for name in ('gacha_mst.json', 'achievement_trade_mst.json', 'unit_evo_mst.json', 'mission_mst.json',
                     'defines_mst.json', 'unit_fe_skill_mst.json', 'item_mst.json'):
            entry = mst.get(name, {})
            base = sha((src / 'deploy' / 'mst' / name).read_bytes())
            check(f'status mst {name}: sha256 of the bytes written and of the image table',
                  entry.get('sha256') == sha(balance.written[f'mst/{name}']) and entry.get('base_sha256') == base, entry)
        good = {'gacha_mst.json': ('rows', 1, 0, 0), 'achievement_trade_mst.json': ('rows', 1, 0, 0),
                'unit_evo_mst.json': ('rows', 1, 0, 0), 'mission_mst.json': ('rows', 1, 0, 0),
                'defines_mst.json': ('object', 1, 0, 0)}
        for name, (mode, replaced, added, deleted) in good.items():
            entry = mst.get(name, {})
            check(f'status mst {name}: applied, {mode}, {replaced} replaced',
                  entry.get('applied') is True and entry.get('error') is None and entry.get('mode') == mode
                  and (entry.get('rows_replaced'), entry.get('rows_added'), entry.get('rows_deleted')) == (replaced, added, deleted),
                  entry)
        fe = mst.get('unit_fe_skill_mst.json', {})
        check('status mst unit_fe_skill_mst.json: groups mode, one group replaced',
              fe.get('applied') is True and fe.get('mode') == 'groups' and fe.get('groups_replaced') == 1
              and fe.get('rows_added') == facts['fe_rows'] and fe.get('rows_deleted') == facts['fe_rows'], fe)
        item = mst.get('item_mst.json', {})
        check('status mst item_mst.json (broken): not applied, the unknown column named',
              item.get('applied') is False and 'qp37xTDx' in (item.get('error') or '')
              and 'does not have' in (item.get('error') or ''), item)
        dungeon = mst.get('dungeon_mst.json', {})
        check('status mst dungeon_mst.json (broken): passes the merge, fails the typed parse, not applied',
              dungeon.get('applied') is False and 'no longer reads' in (dungeon.get('error') or '')
              and '\n' not in (dungeon.get('error') or ''), dungeon)
        unit = mst.get('unit_mst.json', {})
        check('status mst unit_mst.json: refused, pointing at the unit balance file',
              unit.get('applied') is False and 'server/unit_mst.json' in (unit.get('error') or ''), unit)
        info = mst.get('gacha_info_mst.json', {})
        check('status mst gacha_info_mst.json: reported as a table the server does not read',
              info.get('applied') is False and 'does not read gacha_info_mst.json' in (info.get('error') or '')
              and info.get('sha256') == sha(balance.written['mst/gacha_info_mst.json']), info)

        g = archive.get('gacha.json', {})
        reasons = {r.get('id'): r.get('reason', '') for r in g.get('rejected', [])}
        check('status archive gacha.json: applied, 18060 replaced, the unknown-unit and misspelt records rejected',
              g.get('applied') is True and g.get('records_replaced') == 1 and g.get('records_added') == 0
              and set(reasons) == {str(BAD_GATE), str(TYPO_GATE)}
              and 'not in the unit archive' in reasons.get(str(BAD_GATE), '')
              and '"pool_typo" is not a field' in reasons.get(str(TYPO_GATE), '')
              and g.get('sha256') == sha(balance.written['archive/gacha.json']), g)
        ex = archive.get('event_exchange.json', {})
        rejected = {r.get('id'): r.get('reason', '') for r in ex.get('rejected', [])}
        check('status archive event_exchange.json: 1 replaced, 1 added, 1 deleted, the two bad records rejected',
              ex.get('applied') is True and (ex.get('records_replaced'), ex.get('records_added'), ex.get('records_deleted')) == (1, 1, 1)
              and set(rejected) == {'999-7-30004', '13-7-30005'} and 'token 999' in rejected.get('999-7-30004', '')
              and '"limit" is missing' in rejected.get('13-7-30005', ''), ex)
        bd = archive.get('boss_drops.json', {})
        check('status archive boss_drops.json: the whole file applied',
              bd.get('applied') is True and bd.get('mode') == 'file' and bd.get('sha256') == sha(balance.written['archive/boss_drops.json']), bd)
        rot = archive.get('summon_rotation.json', {})
        check('status archive summon_rotation.json (broken): not applied, "not a YYYY-MM-DD Monday"',
              rot.get('applied') is False and 'Monday' in (rot.get('error') or ''), rot)
        unread = archive.get('not_a_table.json', {})
        check('status archive not_a_table.json: reported as unread', unread.get('applied') is False
              and 'nothing was applied' in (unread.get('error') or ''), unread)
        rules = status.get('rules', {})
        check('status rules: no rules.json, no error', rules.get('present') is False and rules.get('error') is None, rules)

        rotation = json.loads(fetch(f'{url}/bf/gme/summonRotation.php')[1])
        check('the image\'s summon rotation is still served', len(rotation.get('featured') or []) == 3, rotation)

        # ---- a player -----------------------------------------------------------------
        db = Database(src / 'deploy' / 'gme.sqlite', CONTAINER, '/data/gme.sqlite')
        player = Player(url, db)
        player.set(tutorial_status=12, tutorial_end_flag=1, energy=200, energy_full_ts=0, gems=1000,
                   zel=99_999_999, karma=99_999_999, achieve_point=100_000)

        init = gme_call(url, *INITIALIZE, {'IKqx1Cn9': player.login})
        gate_row = next((r for r in init.get('5Y4GJeo3') or [] if r.get('7Ffmi96v') == GATE), {})
        check('Initialize: banner 18060 has the new price and name',
              (gate_row.get('03UGMHxF'), gate_row.get('4N27mkt1')) == (GATE_PRICE, GATE_NAME), gate_row)
        defines = init.get('VkoZ5t3K') or {}
        defines = defines[0] if isinstance(defines, list) else defines
        check('Initialize: defines carry the object overlay (energy refill gems)',
              str(defines.get('C8KkHGa7')) == facts['refill'], (facts['refill_was'], defines.get('C8KkHGa7')))

        for label, call in (('GachaList', GACHA_LIST), ('UserInfo', USER_INFO)):
            reply = gme_call(url, *call, {'6FrKacq7': [{'Kn51uR4Y': 'forge'}], 'IKqx1Cn9': player.login})
            info_row = next((r for r in reply.get('1IR86sAv') or [] if r.get('7Ffmi96v') == GATE), {})
            check(f'{label}: GachaInfo 18060 shows {GATE_PRICE} gems and "{GATE_NAME}"',
                  (str(info_row.get('03UGMHxF')), info_row.get('4N27mkt1')) == (GATE_PRICE, GATE_NAME), info_row)

        # The login calendar's day 1 was paid at the first UserInfo, from the Forge's file.
        login = archive.get('login_campaign.json', {})
        day_one = sorted((int(r[0]), int(r[1])) for r in db.rows(
            "SELECT present_type, target_cnt FROM user_presents WHERE user_id = ? AND description = 'Login Bonus Day 1'",
            player.user_id))
        check('login_campaign.json: applied (sha256 of the bytes); day 1 paid the Forge\'s 9 gems',
              login.get('applied') is True and login.get('mode') == 'file'
              and login.get('sha256') == sha(balance.written['archive/login_campaign.json'])
              and day_one == [(8, LOGIN_GEMS)], (login, day_one))

        def units_of(unit_id) -> int:
            return db.one('SELECT COUNT(*) FROM user_units WHERE user_id = ? AND unit_id = ?', player.user_id, str(unit_id))[0]

        def total_units() -> int:
            return db.one('SELECT COUNT(*) FROM user_units WHERE user_id = ?', player.user_id)[0]

        gems, pooled, total = player.balance()['gems'], units_of(POOL_UNIT), total_units()
        pull = gme_call(url, *GACHA_ACTION, {'6FrKacq7': [{'Kn51uR4Y': 'forge'}], 'IKqx1Cn9': player.login,
                                             '1IR86sAv': [{'7Ffmi96v': GATE, 'a329kbl8': '10', '324b023k': '0'}]})
        check('GachaAction 18060 x10: accepted', wire_error(pull) is None, wire_error(pull))
        check('GachaAction 18060 x10: costs 70 gems (7 each)', gems - player.balance()['gems'] == 70,
              gems - player.balance()['gems'])
        check('GachaAction 18060 x10: all ten are the one pooled unit',
              units_of(POOL_UNIT) - pooled == 10 and total_units() - total == 10,
              (units_of(POOL_UNIT) - pooled, total_units() - total))

        # ---- Merit shop -----------------------------------------------------------------
        def merit_buy(quantity: int) -> dict:
            return gme_call(url, *MERIT_TRADE, {'IKqx1Cn9': player.login,
                                                'rX74GNsm': [{'Mdgsh04u': MERIT, 'H6k1LIxC': str(quantity)}]})

        def points() -> int:
            return db.one('SELECT achieve_point FROM user_info WHERE id = ?', player.user_id)[0]

        listing = gme_call(url, *MERIT_LIST, {'IKqx1Cn9': player.login})
        check('Merit list: offer 91000059 is listed', any(r.get('Mdgsh04u') == MERIT for r in listing.get('9j3ALx8I') or []),
              wire_error(listing))
        before = points()
        one = merit_buy(1)
        check('Merit shop: one Ignis Shard costs 77 points', wire_error(one) is None and before - points() == MERIT_PRICE,
              (wire_error(one), before - points()))
        two = merit_buy(2)
        check('Merit shop: two more reach the new limit of 3', wire_error(two) is None and before - points() == 3 * MERIT_PRICE,
              (wire_error(two), before - points()))
        held = points()
        fourth = merit_buy(1)
        check('Merit shop: a 4th is refused and costs nothing', wire_error(fourth) is not None and points() == held,
              (wire_error(fourth), held - points()))

        # ---- UnitEvo --------------------------------------------------------------------
        cols = [r[1] for r in db.rows('PRAGMA table_info(user_units)') if r[1] != 'user_unit_id']
        _, unit_rows = table(src, 'unit_mst.json')
        unit_mst = {r['pn16CNah']: r for r in unit_rows}
        template = db.one('SELECT user_unit_id FROM user_units WHERE user_id = ? ORDER BY user_unit_id LIMIT 1', player.user_id)[0]

        def give_unit(unit_id: str, level: int) -> int:
            db.rows(f'INSERT INTO user_units ({", ".join(cols)}) SELECT {", ".join(cols)} FROM user_units WHERE user_unit_id = ?',
                    template)
            new = db.one('SELECT MAX(user_unit_id) FROM user_units WHERE user_id = ?', player.user_id)[0]
            row = unit_mst[unit_id]
            db.rows('UPDATE user_units SET unit_id = ?, unit_lvl = ?, favorite_flg = 0, bb_id = ?, sbb_id = ?,'
                    ' eqip_item_id = 0, eqip_item_id2 = 0 WHERE user_unit_id = ?',
                    unit_id, level, row['nj9Lw7mV'], row['iEFZ6H19'], new)
            return new

        def evo(base: int, material: int) -> dict:
            return gme_call(url, *UNIT_EVO, {'IKqx1Cn9': player.login,
                                             '8Z2NQrx1': [{'inU8Q4gL': str(base), 'mnZ5K4Ii': '1', '29MgiJIQ': '1'},
                                                          {'inU8Q4gL': str(material), 'mnZ5K4Ii': '2', '29MgiJIQ': '1'}],
                                             'I82p0wCL': [{'pn16CNah': EVO_TARGET}], 'mCE3rUu5': [{'Rs7bCE3t': str(EVO_ZEL)}]})

        def zel() -> int:
            return db.one('SELECT zel FROM user_info WHERE id = ?', player.user_id)[0]

        base = give_unit(EVO_BASE, int(unit_mst[EVO_BASE]['EI1DF8Yt']))
        old = give_unit(OLD_MATERIAL, 1)
        before = (zel(), db.one('SELECT unit_id FROM user_units WHERE user_unit_id = ?', base)[0], units_of(OLD_MATERIAL))
        refused = evo(base, old)
        check('UnitEvo with the image\'s material (Fire Nymph) is refused and changes nothing',
              wire_error(refused) is not None and (zel(), db.one('SELECT unit_id FROM user_units WHERE user_unit_id = ?', base)[0],
                                                   units_of(OLD_MATERIAL)) == before, wire_error(refused))
        new = give_unit(NEW_MATERIAL, 1)
        before_zel = zel()
        done = evo(base, new)
        evolved = db.one('SELECT unit_id FROM user_units WHERE user_unit_id = ?', base)
        check('UnitEvo with the Forge recipe\'s material (Miracle Totem) evolves 10011 into 10012',
              wire_error(done) is None and evolved and str(evolved[0]) == EVO_TARGET, (wire_error(done), evolved))
        check('UnitEvo charges the Forge recipe\'s 12,345 Zel and takes the Totem',
              before_zel - zel() == EVO_ZEL and db.one('SELECT 1 FROM user_units WHERE user_unit_id = ?', new) is None,
              before_zel - zel())

        # ---- First clear --------------------------------------------------------------------
        db.rows('DELETE FROM user_decks WHERE user_id = ? AND deck_type = 1 AND deck_num = 0', player.user_id)
        db.rows('INSERT INTO user_decks (user_id, user_unit_id, deck_type, deck_num, member_type, disp_order)'
                ' VALUES (?, ?, 1, 0, 0, 0)', player.user_id, template)
        started = gme_call(url, *MISSION_START, {
            'IKqx1Cn9': player.login, '6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}],
            '9Q1Lq5FS': [{'h7eY3sAK': '0', 'J3stQ7jd': '0', 'j28VNcUW': MISSION, 'jkldTrhL': '0', 'Z0Y4RoD7': '0',
                          'nA95Bdj6': '0', '5Z1LNoyH': '0', 'u1iPEVUq': '-1'}],
            'JzS3uxsZ': [{'0b4efi1W': '1'}]})
        check('MissionStart 10: accepted', wire_error(started) is None and 'Kz7qfSs5' in started, wire_error(started))
        serial = started.get('Kz7qfSs5', [{}])[0].get('k9cxD7Ba', '0')
        battle = {k: '0' for k in ('Najhr8m6', 'HTVh8a65', 'hoG2ieT5', '6PLsn8xo', '5NRJQ1LU', 'XP06YWdT',
                                   'U8uZLA34', 'rZQJF5G9', 'TW1Mrtp5', 'e6BKoYy9')}
        battle.update({'47cJBUxz': '', '4T0Q2Bh5': '', '3MAT6quo': ''})
        ended = gme_call(url, *MISSION_END, {'IKqx1Cn9': player.login, '6FrKacq7': [{'Kn51uR4Y': '5EdKHavF'}],
                                             'Kz7qfSs5': [{'k9cxD7Ba': str(serial), 'j3g5P4cq': '2'}], 'rXvA1E5y': [battle]})
        check('MissionEnd 10 (won): accepted', wire_error(ended) is None, wire_error(ended))
        presents = sorted((int(r[0]), int(r[1])) for r in db.rows(
            "SELECT present_type, target_cnt FROM user_presents WHERE user_id = ? AND description = 'First clear reward'",
            player.user_id))
        check('MissionEnd 10: the Forge\'s first-clear rewards are in the present box (4,321 Zel, 2 gems)',
              presents == [(3, 4321), (8, 2)], presents)

        # ---- Bazaar ------------------------------------------------------------------------
        db.rows('INSERT INTO user_event_tokens (user_id, token_id, count) VALUES (?, ?, 50)'
                ' ON CONFLICT (user_id, token_id) DO UPDATE SET count = 50', player.user_id, TOKEN)
        bazaar = gme_call(url, *BAZAAR_LIST, {'IKqx1Cn9': player.login, 'l234vdKs': [{'Slkc395l': str(TOKEN)}]})
        rows = {r.get('Kd3DL39d'): r for r in bazaar.get('c2Sls4DD') or []}
        added = rows.get(NEW_OFFER['id'], {})
        check('Bazaar: the new offer is listed at 3 tokens with 2 in stock',
              (str(added.get('3EWLm0sA')), str(added.get('S8rdp9zk')), added.get('qBAb07rh')) == ('3', '2', '7:30004:1:0:0'),
              (wire_error(bazaar), added))
        check('Bazaar: the changed offer costs 11, the deleted one is gone, the bad ones never appear',
              str(rows.get(PRICED_OFFER, {}).get('3EWLm0sA')) == str(NEW_PRICE) and DELETED_OFFER not in rows
              and '13-7-30005' not in rows, (rows.get(PRICED_OFFER), DELETED_OFFER in rows))
        swords = db.one('SELECT COALESCE(SUM(item_num), 0) FROM user_items WHERE user_id = ? AND item_id = 30004', player.user_id)[0]
        bought = gme_call(url, *BAZAAR_BUY, {'IKqx1Cn9': player.login, 'c2Sls4DD': [
            {'Kd3DL39d': NEW_OFFER['id'], 'H6k1LIxC': '1', 'Slkc395l': str(TOKEN)}]})
        tokens = db.one('SELECT count FROM user_event_tokens WHERE user_id = ? AND token_id = ?', player.user_id, TOKEN)[0]
        got = db.one('SELECT COALESCE(SUM(item_num), 0) FROM user_items WHERE user_id = ? AND item_id = 30004', player.user_id)[0]
        check('Bazaar: buying the new offer takes 3 tokens and gives the Divine Sword (item_mst overlay ignored)',
              wire_error(bought) is None and tokens == 47 and got == swords + 1, (wire_error(bought), tokens, got - swords))
    except Exception as e:  # noqa: BLE001 - report it, then clean up
        check(f'{type(e).__name__}: {e}', False)
    finally:
        if not args.keep:
            subprocess.run(['docker', 'rm', '-f', CONTAINER], capture_output=True)
            subprocess.run(['docker', 'volume', 'rm', '-f', VOLUME], capture_output=True)
            shutil.rmtree(work, ignore_errors=True)
        else:
            print(f'kept: container {CONTAINER}, volume {VOLUME}, balance root {work / "balance"}')
    print(f"\n{'OK' if not failures else f'{failures} FAILED'}")
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
