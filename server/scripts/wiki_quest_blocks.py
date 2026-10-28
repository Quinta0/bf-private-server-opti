"""The Global wiki's {{Quest}} and {{SQuest}} blocks, read from saved page wikitext.

A block gives a quest's name, energy, battle count, its bosses (boss1..boss7 as unit
ids or names, each with its own bossNhp, often empty) and the notes, where the
wiki writes each boss's attacks under a bold header, and the page's zone element
({{Zone:Header |element = Earth}}).  The source survey's coverage files dropped
the HP positions and never read the {{SQuest}} pages (the Season 2 story:
Rokkzalm, Bariura, ...), so this reads the pages themselves.
"""
import pathlib
import re

BLOCK_START = re.compile(r'\{\{\s*(?:Template:)?S?Quest\s*(?=[|\n])')
FIELD = re.compile(r'^\s*([A-Za-z#]+\d*(?:hp|rate)?)\s*=\s*(.*)$', re.S)
ANIME_ID = re.compile(r'unit[ _]ills[ _]anime[ _](\d+)', re.I)
ZONE_ELEMENT = re.compile(r'\{\{\s*Zone:Header[^{}]*?\|\s*element\s*=\s*([A-Za-z]+)', re.I)
MAX_BOSSES = 7


def _block_end(text: str, start: int) -> int:
    depth, i = 0, start
    while i < len(text) - 1:
        pair = text[i:i + 2]
        if pair in ('{{', '[['):
            depth += 1
            i += 2
            continue
        if pair in ('}}', ']]'):
            depth -= 1
            i += 2
            if depth == 0:
                return i
            continue
        i += 1
    return len(text)


def _fields(body: str) -> dict:
    """Top-level |key = value pairs (values may span lines and hold templates)."""
    out, depth, cur = {}, 0, []
    parts = []
    i = 0
    while i < len(body):
        pair = body[i:i + 2]
        if pair in ('{{', '[['):
            depth += 1
            cur.append(pair)
            i += 2
            continue
        if pair in ('}}', ']]'):
            depth -= 1
            cur.append(pair)
            i += 2
            continue
        if body[i] == '|' and depth == 0:
            parts.append(''.join(cur))
            cur = []
        else:
            cur.append(body[i])
        i += 1
    parts.append(''.join(cur))
    for p in parts[1:]:
        m = FIELD.match(p)
        if m:
            out[m[1].strip().lower()] = m[2].strip()
    return out


def page_blocks(text: str, page: str) -> list:
    blocks = []
    zone = ZONE_ELEMENT.search(text)
    for m in BLOCK_START.finditer(text):
        end = _block_end(text, m.start())
        f = _fields(text[m.start() + 2:end - 2])
        if not f.get('quest'):
            continue
        bosses = []
        for n in range(1, MAX_BOSSES + 1):
            v = f.get(f'boss{n}', '')
            if v:
                bosses.append({'ref': v, 'hp': f.get(f'boss{n}hp', '')})
        blocks.append({'page': page, 'quest': f['quest'], 'energy': f.get('energy', ''),
                       'battles': f.get('battles', ''), 'bossname': f.get('bossname', ''),
                       'bosses': bosses, 'notes': f.get('notes', ''), 'zone_element': zone[1] if zone else '',
                       'captures': [v for k, v in f.items() if k.startswith('capture') and k[7:].isdigit() and v]})
    return blocks


def load(dirs: list) -> list:
    blocks = []
    for d in dirs:
        for p in sorted(pathlib.Path(d).glob('*.txt')):
            page = p.stem.replace('%2F', '/')
            if page.lower().startswith(('user:', 'user blog:', 'user_blog:')):
                continue        # sandboxes and blogs copy quests, sometimes wrongly
            blocks += page_blocks(p.read_text(encoding='utf-8', errors='replace'), page)
    return blocks


def hp_value(text: str) -> int | None:
    digits = re.sub(r'[^\d]', '', re.split(r'[(<~/]|\bto\b', text or '')[0])
    return int(digits) if digits and len(digits) >= 3 else None
