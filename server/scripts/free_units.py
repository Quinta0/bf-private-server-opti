"""Lines the game gives away, so no paid summon pools them.

The owner, 2026-10-23: players who pulled Berserk Ezra, D'Artagnan, Merlin or a
Geode from a gem gate felt cheated, since a quest gives them away.  A line is
free, by base id, when
  - a mission's first clear gives one of its stages (mission_mst SiYs27Cj type 6),
  - an archive mission's monster drops a 6* or better stage of it (an evidenced
    capture, unit_drop_id with a chance: D'Artagnan, Florence, Sun Tse, the SMT
    and Guilty Gear collab units; the event dungeons stay open all the time),
  - an Event Bazaar sells it (reward_type 6 in archive/event_exchange.json or
    an archive/event_rotation.json event's offers), or
  - GIVEN names it (a Frontier Gate, Grand Quest or Challenge HR reward, an
    exchange, a battle drop, or the owner's word).
Not free here: the boss drops of ticket #82 (MissionArchiver::applyBossDrops,
2-20% for nearly every story boss) and 3-5* captures, which every summon pool
has always shared with the story.  The Dawn Summon also leaves out every line
farmable_lines() finds: any capture and every #82 boss drop (owner, 2026-10-23,
ticket #126).

gen_house_gates.py, gen_global_gate.py, gen_collab_gacha.py and
gen_unsummoned_gates.py leave these lines out; the Divine Summons follow
gates 11960 and 15850.
"""
import json
import unicodedata
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MISSION_MST = ROOT / 'deploy' / 'mst' / 'mission_mst.json'
MISSION_ARCHIVE = ROOT / 'deploy' / 'archive' / 'mission.json'
BAZAAR = ROOT / 'deploy' / 'archive' / 'event_exchange.json'
ROTATION = ROOT / 'deploy' / 'archive' / 'event_rotation.json'
BOSS_DROPS = ROOT / 'deploy' / 'archive' / 'boss_drops.json'
UNIT_ARCHIVE = ROOT / 'deploy' / 'archive' / 'unit.json'

CAPTURE_RARITY = 6   # a capture of this rarity or better makes its line free

# Lines given away outside first clears, captures and the Bazaar, by base id.
GIVEN = {
    10875: 'Noble Ember Mirfah: Grand Quest 40% completion reward',
    11087: 'Callow Flame Owen: Challenge HR reward',
    18830: 'Pumburkiny: merit exchange, 50 merit; battle drop',
    61157: 'Callow Shadow Grahdens: Challenge HR reward',
    810528: 'Vengeful Grandt: Frontier Gate reward',
    810900: 'Orebi Ignis Geode: free unit (owner, 2026-10-23)',
    820504: 'Cursed Fei: battle drop',
    820900: 'Orebi Algor Geode: free unit (owner, 2026-10-23)',
    830148: 'Andaria the Swift: Frontier Gate reward',
    830528: 'Time-Weaver Elaina: Frontier Gate reward; event exchange',
    830628: 'Grieving Dryad Alraune: Grand Quest 100% completion reward',
    830900: 'Orebi Terra Geode: free unit (owner, 2026-10-23)',
    840900: 'Orebi Fulgur Geode: free unit (owner, 2026-10-23)',
    850148: 'Aurelia the Ascended: Frontier Gate reward',
    850346: 'Gazia: guild exchange',
    850900: 'Orebi Lux Geode: free unit (owner, 2026-10-23)',
    860148: 'Hadaron the Immortal: Frontier Gate reward',
    860156: "Temptress Xie'Jing: guild exchange",
    860338: 'Mad King Ihsir: Challenge HR reward',
    860900: 'Orebi Umbra Geode: free unit (owner, 2026-10-23)',
}


def _rows(path: Path) -> list:
    data = json.loads(path.read_text(encoding='utf-8'))
    return data if isinstance(data, list) else next(v for v in data.values() if isinstance(v, list))


def free_lines(units: dict, line) -> dict[str, str]:
    """base id -> why its line is free.  units: unit_mst rows by id; line: collab.evolution_lines()."""
    out: dict[str, str] = {}

    def add(uid, why):
        if str(uid) in units:
            out.setdefault(line(str(uid))[0], why)

    missions = _rows(MISSION_MST)
    for row in missions:
        for part in (row.get('SiYs27Cj') or '').split(','):
            kind, _, rest = part.partition(':')
            if kind == '6':
                add(rest.split(':')[0], f'first clear of mission {row["j28VNcUW"]}')
    live = {row['j28VNcUW'] for row in missions}
    for record in json.loads(MISSION_ARCHIVE.read_text(encoding='utf-8')):
        if str(record['id']) not in live or record['id'] in (1, 2):
            continue
        for stage in record['stages']:
            for monster in stage['battle_monsters']:
                drop = str(monster.get('unit_drop_id') or 0)
                if (monster.get('unit_drop_chance') and drop in units
                        and int(units[drop]['7ofj5xa1']) >= CAPTURE_RARITY):
                    add(drop, f'{monster["unit_drop_chance"]}% capture in mission {record["id"]}')
    events = json.loads(ROTATION.read_text(encoding='utf-8'))['pool']
    offers = [*json.loads(BAZAAR.read_text(encoding='utf-8')), *(o for e in events for o in e.get('offers', []))]
    for offer in offers:
        if offer.get('reward_type') == 6:
            add(offer['target_id'], f'Event Bazaar ({offer["id"]})')
    for base, why in GIVEN.items():
        add(base, why)
    return out


def _folded(name: str) -> str:
    """MissionArchiver's lowerTrimmed: trimmed, lower case, Latin-1 accents folded."""
    return unicodedata.normalize('NFKD', name.strip()).encode('ascii', 'ignore').decode().lower()


def farmable_lines(units: dict, line) -> dict[str, str]:
    """base id -> how its line is farmed: a capture of any stage (the archive's own
    chance or the capture floor), or a #82 boss drop.

    The boss drop mirrors MissionArchiver::applyBossDrops: in an is_boss stage, a
    monster with at least half the stage's top HP, drawn as a kind 0 unit (no
    visuals) under one of its line's names, with no capture of its own, drops its
    line's base form when boss_drops.json gives the line's top rarity a chance."""
    chance = json.loads(BOSS_DROPS.read_text(encoding='utf-8'))['chance_by_line_rarity']
    archived = {str(u['id']) for u in json.loads(UNIT_ARCHIVE.read_text(encoding='utf-8'))}
    names: dict[str, set] = {}
    for uid, u in units.items():
        names.setdefault(line(uid)[0], set()).add(_folded(u['utP1c0CD']))
    out: dict[str, str] = {}
    live = {row['j28VNcUW'] for row in _rows(MISSION_MST)}
    for record in json.loads(MISSION_ARCHIVE.read_text(encoding='utf-8')):
        if str(record['id']) not in live or record['id'] in (1, 2):
            continue
        for stage in record['stages']:
            top = max((m.get('hp') or 0 for m in stage['battle_monsters']), default=0)
            for monster in stage['battle_monsters']:
                drop, uid = str(monster.get('unit_drop_id') or 0), str(monster.get('unit_id') or 0)
                # MissionArchiver::applyCaptureFloor: a monster not flagged authored_capture
                # is caught (40% at least) as its drop unit, else as itself, when
                # archive/unit.json has it; an authored one only at its own chance.
                caught = (drop if monster.get('unit_drop_chance') and drop != '0' else '0') \
                    if monster.get('authored_capture') else (drop if drop != '0' else uid)
                if caught in archived and caught in units:
                    out.setdefault(line(caught)[0], f'capture in mission {record["id"]}')
                    continue
                if monster.get('unit_drop_chance') and drop != '0':
                    continue
                if (not stage['is_boss'] or not monster.get('hp') or monster['hp'] * 2 < top
                        or monster.get('visuals') or units.get(uid, {}).get('xP9oi5Z2') != '0'):
                    continue
                stages = line(uid)
                kind0 = [s for s in stages if units[s]['xP9oi5Z2'] == '0']
                base = min(kind0, key=lambda s: (int(units[s]['7ofj5xa1']), int(s)))
                top_rarity = max(int(units[s]['7ofj5xa1']) for s in stages)
                if (_folded(monster['name']) in names[stages[0]] and base in archived
                        and 0 < top_rarity < len(chance) and chance[top_rarity]):
                    out.setdefault(stages[0], f'boss drop in mission {record["id"]}')
    return out
