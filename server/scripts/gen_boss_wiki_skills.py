"""Give bosses that only attack the skills the Global wiki lists for them.

python3 -I scripts/gen_boss_wiki_skills.py --hints COVERAGE.json [COVERAGE.json ...] [--check] [--report FILE]

Run after gen_boss_own_ai.py.  The hints are the wiki quest blocks the source
survey saved (coverage_by_mission.json, coverage_regenerated.json): each holds the
quest's notes, where the wiki writes a boss's attacks as ''Name'' - effect bullets.

A boss-wave monster that just attacks (AI 1, no skills), in a record that is not a
2014 capture, gets the notes' skills that skill_mst knows by name: the exact name
first ("Texas Smash!" is not "Texas Smash!!"), then without punctuation, then
without possessives ("Goddess' Charm" finds "Goddess's Charm").  A name shared by
several skill ids (Call Bind is 9170, 2200350, 2201970, 6003760) resolves to the
cluster that keeps the boss's skills closest together: a boss's skills were
authored as one run of ids (Skramya 9170-9173), and a name the section lists
several times takes that run's ids in order (Ikhemw-sek 1400003304, 05, 06, 12).
Lines the boss SAYS are skill rows too ("Suspicious laughter fills the air..." is
Blazing Mare's 9724, the idle turn before Soul Draining); they count only inside
that run, so a quote never pulls in a stranger's skill.

Each skill's trigger is read from the headers above it and the sub-bullets under it
("**Used every 3 turns"): once below X% HP (one row per threshold of "< 85%, 70%,
55%"), every N turns (and only below X% when both are given; "every fourth turn"
too), once at turn N ("Turn 4 & every 5 turns" is turn 4, then every 5), once
("when attacked for the first time"), every turn, else a 30% chance; a skill that
ends the fight (or the boss itself) or makes the boss immune is never a random
pick; the notes' hidden <!-- data-mine --> lists are not read.  Skills listed under one timed header are
that turn's actions in order: the first takes the header's trigger and each next
one follows the one before it.  Following is a flag, as the Trial and Creation God
AIs chain theirs (the leader sets it, the next action needs it and clears it), and
so is "Next turn" after an every-N-turns telegraph or "''X'' will be used the next
turn"; after turn N, "Next turn" is turn N+1.  A follower whose leader has no
skill row takes the leader's own trigger instead, and rows with the same trigger
share it at random, so no listed skill is dead behind another.  "Phase 2" /
"Enrage" headers are parts of the boss above them (their HPs add up); reward and
tip headers are nobody's.  Every row targets the player side, as the 2014 captures'
heals and buffs did.  A boss that acts once a turn and has an every-turn skill
acts twice (most captured bosses act two or more times).  The AI goes to ai.json
as "wiki <boss>" (ids from 900,000,000, shared by identical rows), ending with a
plain attack.  HP, ATK, DEF and drops stay.
"""
import argparse
import bisect
import difflib
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / 'deploy' / 'archive'
MST = ROOT / 'deploy' / 'mst'
TUTORIAL = {1, 2}
WIKI_AI_BASE = 900_000_000
MAX_SKILLS = 20
MAX_ROWS = 23        # plus the closing attack: the client reads 24
# The wiki's skill bullets: "**''Call Bind'' - Single target Water attack".  Anchored to
# the bullet so italics inside prose ("''exactly''", unit names) never count.
SKILL_LINE = re.compile(r"^\s*[*#:]+\s*(?:turn\s*\d+\s*:?\s*)?''+\s*((?:(?!'')[^\n]){3,80}?)\s*''+\s*[-:–—]\s*(.+)", re.I)
NOT_A_SKILL = {'???', 'exactly'}
DIALOGUE = re.compile(r"^\[|…|\.\.\.|[.?]$|\btalk\b|^(?:\S+\s+){2,}\S*!+$", re.I)
WINDOW = 200         # a boss's skills sit within this many ids of each other
ORDINAL = {'other': 2, 'second': 2, 'third': 3, 'fourth': 4, 'fifth': 5, 'sixth': 6, 'seventh': 7,
           'eighth': 8, 'ninth': 9, 'tenth': 10}
HP_UNDER = re.compile(r'(?:below|under|less than|<)\s*(\d+)\s*%((?:\s*(?:,|and|&|/|or)\s*\d+\s*%)*)', re.I)
AT_TURN = re.compile(r'(?:on|at|after|from)\s+turn\s+(\d+)|after\s+(\d+)\s+turns', re.I)
EVERY = re.compile(r'every\s+(\d+|' + '|'.join(ORDINAL) + r')\s+turns?', re.I)
EVERY_TURN = re.compile(r'\bevery\s+turn\b', re.I)
TURN_N = re.compile(r'\bturn\s*(\d+)\b', re.I)
ONCE = re.compile(r'\b(?:for the first time|only once|(?:is|be|are)\s+used\s+once)\b(?!\s+(?:per|a|an|in|alone|every))', re.I)
NEXT_TURN = re.compile(r'^\W*next\s+turn', re.I)
FLAG_SET = re.compile(r'@sets:(\d+)')
FLAG_AFTER = re.compile(r'@after:(\d+)')
TOKENS = re.compile(r'@(?:sets|after):\d+')
TIMED = re.compile('|'.join(p.pattern for p in (HP_UNDER, AT_TURN, EVERY, EVERY_TURN, TURN_N, ONCE, FLAG_AFTER)), re.I)
HAS_HP = re.compile(r'\bhas\s+([\d,]{3,})\s*hp', re.I)
HEADER = re.compile(r"^'''+((?:(?!'').){2,60}?)'''+\s*:?\s*$")   # a line that is only a bold (or bold-italic) title
PHASE_TAG = re.compile(r'@phase:(\d+)/(\d+)')
USED_AFTER = re.compile(r"used\s+(?:right\s+|immediately\s+)?after\s+(?:using\s+)?''((?:(?!'')[^\n]){3,80})''", re.I)
WILL_FOLLOW = re.compile(r"''((?:(?!'')[^\n]){3,80})''\s+(?:will be|is)\s+used\b[^.\n]*?\bnext\s+turn", re.I)
LETHAL = re.compile(r'lethal|instantly defeats (?:all|self)|defeats (?:self|itself|own)|(?:fight|battle) ends|ends (?:the )?(?:fight|battle|wave)|self[- ]?ko', re.I)
IF_ONLY = re.compile(r'\b(?:if|unless|when|only)\b', re.I)
COMMENT = re.compile(r'<!--.*?(?:-->|$)', re.S)
IMMUNE = '10004'      # skill_mst proc: immune to attack types (Spk's 999-turn "Spark it about 3 times!")
PHASE = re.compile(r'^\W*(?:phase|form|stage|part)\s*\d+|^\W*(?:first|second|third|fourth|final|last)\s+(?:phase|form)|^\W*enrage', re.I)
NOT_OWNER = re.compile(r'reward|continue|note|tip|strateg|drop|warning|recommend|mission|requirement|condition|restriction', re.I)


def norm(s: str) -> str:
    return re.sub(r'[^a-z0-9]+', ' ', (s or '').lower()).strip()


def loose(s: str) -> str:
    """norm without possessives: "Goddess' Charm" and "Goddess's Charm" meet."""
    return norm(re.sub(r"['’]s\b", '', s or '').replace("'", '').replace('’', ''))


def raw(s: str) -> str:
    return re.sub(r'\s+', ' ', (s or '').strip().lower())


def every_n(m: re.Match) -> int:
    v = m[1].lower()
    return int(v) if v.isdigit() else ORDINAL[v]


def row(conds: list, percent: float, skill: int | None, target: int = 2, flags: list | None = None) -> dict:
    if skill is not None:
        conds = [{'type': 'skill', 'parameter': skill}] + conds
    return {'priority': 0, 'percent': float(percent), 'act_target': target, 'search_term': 'random',
            'self_conditions': conds[:5], 'party_conditions': [],
            'action': {'type': 'skill' if skill is not None else 'attack', 'flag_changes': flags or [],
                       'unknown_bool': True, 'unknown_int_1': 0, 'unknown_int_2': 0}}


def once(cond: dict) -> list:
    return [cond, {'type': 'limited_act', 'parameter': 1}]


def triggers(text: str, index: int) -> list:
    """The AI rows one wiki skill line asks for."""
    sets = [int(x) for x in FLAG_SET.findall(text)]
    if m := FLAG_AFTER.search(text):
        flags = [int(m[1]), 0] + [x for k in sets for x in (k, 1)]
        return [row([{'type': 'flg_on', 'parameter': int(m[1])}], 100, index, flags=flags)]
    hp = HP_UNDER.search(text)
    every = EVERY.search(text)
    turn = AT_TURN.search(text) or TURN_N.search(text)
    first = int(turn[1] or turn[2]) if turn and turn.re is AT_TURN else int(turn[1]) if turn else None
    if hp and every and not hp[2]:
        out = [row([{'type': 'hp_pr_under', 'parameter': int(hp[1])}, {'type': 'actbetween', 'parameter': every_n(every)},
                    {'type': 'turn_limited_act', 'parameter': 1}], 100, index)]
    elif hp:
        out = [row(once({'type': 'hp_pr_under', 'parameter': t}), 100, index)
               for t in [int(hp[1])] + [int(x) for x in re.findall(r'(\d+)\s*%', hp[2] or '')]]
    elif every and turn and turn.start() < every.start():      # "Turn 4 & every 5 turns afterwards"
        out = [row(once({'type': 'act', 'parameter': first}), 100, index),
               row([{'type': 'actbetween', 'parameter': every_n(every)}, {'type': 'turn_limited_act', 'parameter': 1}], 100, index)]
    elif every:
        out = [row([{'type': 'actbetween', 'parameter': every_n(every)}, {'type': 'turn_limited_act', 'parameter': 1}], 100, index)]
    elif m := AT_TURN.search(text):
        out = [row(once({'type': 'actbetween', 'parameter': int(m[1] or m[2])}), 100, index)]
    elif m := TURN_N.search(text):
        out = [row(once({'type': 'act', 'parameter': int(m[1])}), 100, index)]
    elif LETHAL.search(text):
        out = []        # a fight-ending skill is never a random pick
    elif EVERY_TURN.search(text):
        out = [row([{'type': 'turn_limited_act', 'parameter': 1}], 100, index)]
    elif ONCE.search(text):
        out = [row([{'type': 'limited_act', 'parameter': 1}], 100, index)]
    else:
        out = [row([], 30, index)]
    for r in out:
        r['action']['flag_changes'] = [x for k in sets for x in (k, 1)]
    if (ph := PHASE_TAG.search(text)) and not FLAG_AFTER.search(text):
        out = [_in_phase(r, int(ph[1]), int(ph[2])) for r in out]
    return out


def _in_phase(r: dict, k: int, n: int) -> dict:
    """A phase-k skill of an n-phase boss, flattened into one HP bar: phase k starts
    below (n-k+1)/n of it (Hilda's phase-3 "Turn 1" brume: once below 33%), and its
    own HP thresholds are read inside that phase's share."""
    start = round(100 * (n - k + 1) / n)
    conds = [c for c in r['self_conditions'] if c['type'] != 'skill']
    skill = [c for c in r['self_conditions'] if c['type'] == 'skill']
    hp = [c for c in conds if c['type'] == 'hp_pr_under']
    if hp:
        for c in hp:
            c['parameter'] = max(1, round(100 * ((n - k) + c['parameter'] / 100) / n))
    elif any(c['type'] == 'act' for c in conds):
        conds = [{'type': 'hp_pr_under', 'parameter': start}] + [c for c in conds if c['type'] != 'act']
    else:
        conds = [{'type': 'hp_pr_under', 'parameter': start}] + conds
    r['self_conditions'] = (skill + conds)[:5]
    return r


def trigger(text: str, index: int) -> dict:
    return triggers(text, index)[0]


def _turn_after(before: str | None) -> str | None:
    """'Turn 5' / 'On turn 5' -> 'turn 6'; None for anything else."""
    if not before or FLAG_AFTER.search(before):
        return None
    if m := AT_TURN.search(before):
        return f'turn {int(m[1] or m[2]) + 1}'
    if not EVERY.search(before) and (m := TURN_N.search(before)):
        return f'turn {int(m[1]) + 1}'
    return None


def sections(notes: str) -> dict:
    """norm(boss) -> {'name', 'skills': [(skill, trigger text)], 'hp'}, one per bold header.

    The wiki gives each boss its own section, opened by its name in bold
    ("{{UnitImage|element|Thunder}} '''Demon Duglere'''"), and writes a skill's
    trigger on the bullet above it ("*Every 3 turns" / "**''Thunder Windstorm'' -
    20 hit AoE") or under it ("***Used every 3 turns"); "Has 220,000 HP" in a section
    (or in each of its phases) is that boss's HP.  Skills before any boss header
    belong to '' (the quest's only boss, when it has one)."""
    notes = COMMENT.sub('', notes)
    out, owner = {}, ''
    context, last, opened, flag, skill_depth, phase_hp = {}, {}, {}, 0, None, defaultdict(list)
    follows = defaultdict(dict)       # owner -> {skill named "used the next turn": flag}
    leads = defaultdict(dict)         # owner -> {skill another is "used after using": flag}
    placed = {}                       # owner -> (depth, header text) of its last skill
    headed = Counter()                # owner -> times its header opened a section (2nd = its phase 2)
    phase_of = {}                     # (owner, skill index) -> phase
    phase = 0                         # the quest's '''Phase N''' header we are under (0: none)

    def section(key: str, name: str) -> dict:
        return out.setdefault(key, {'name': name, 'skills': [], 'hp': None})

    for line in notes.splitlines():
        depth = len(re.match(r'^\s*([*#:]*)', line)[1])
        m = SKILL_LINE.search(line)
        if not m:
            bare = re.sub(r'\{\{[^{}]*\}\}|<br\s*/?>', '', line).strip().lstrip('*#:').strip()
            bold = HEADER.match(bare)
            if bold and depth <= 1:
                title = bold[1].strip()
                if PHASE.search(title):
                    if owner is None:
                        owner = ''
                    phase_hp[owner].append(None)
                    if n := re.search(r'\d+', title):
                        phase = int(n[0])
                elif NOT_OWNER.search(title):
                    owner = None
                else:
                    owner = norm(title)
                    section(owner, title)
                    headed[owner] += 1
                context, last, opened, skill_depth = {}, {}, {}, None
                placed.pop(owner, None)
                continue
            if owner is None or (not depth and not (TIMED.search(line) or NEXT_TURN.search(line))):
                continue
            listed = section(owner, owner)['skills']
            if depth and skill_depth is not None and depth > skill_depth:
                # a sub-bullet of the skill above: its trigger, if it names one
                listed[-1] = (listed[-1][0], listed[-1][1] + ' ' + line.strip())
                if (f := WILL_FOLLOW.search(line)) and not IF_ONLY.search(line[:f.start()]):
                    flag += 1
                    listed[-1] = (listed[-1][0], listed[-1][1] + f' @sets:{flag}')
                    follows[owner][loose(f[1])] = flag
                if f := USED_AFTER.search(line):
                    # "**Used after using ''Urgh, the party ends now!!''": this skill
                    # follows that one (Sweet Harlequin's turn-10 self-defeat)
                    flag += 1
                    listed[-1] = (listed[-1][0], f'@after:{flag} ' + listed[-1][1])
                    leads[owner][loose(f[1])] = flag
            header = line
            if NEXT_TURN.search(line):
                header = _turn_after(last.get(depth))
                if header is None:
                    # the last skill under the header above sets a flag; these need it
                    flag += 1
                    if opened.get(depth, len(listed)) < len(listed):
                        listed[-1] = (listed[-1][0], listed[-1][1] + f' @sets:{flag}')
                    header = f'@after:{flag} {TOKENS.sub("", last.get(depth) or "")}'
            context = {d: t for d, t in context.items() if d < depth}
            context[depth] = header
            last = {d: t for d, t in last.items() if d <= depth}
            last[depth] = header
            opened = {d: i for d, i in opened.items() if d < depth}
            opened[depth] = len(listed)
            if skill_depth is not None and depth <= skill_depth:
                skill_depth = None
            placed.pop(owner, None)
            if h := HAS_HP.search(line):
                hp = int(h[1].replace(',', ''))
                if phase_hp[owner]:
                    phase_hp[owner][-1] = phase_hp[owner][-1] or hp
                else:
                    section(owner, owner)['hp'] = hp
            continue
        name = m[1].strip()
        if owner is None or name.lower() in NOT_A_SKILL:
            continue
        listed = section(owner, owner)['skills']
        above = ' '.join(t for d, t in sorted(context.items()) if d < depth)
        text = above + ' ' + line.strip()
        if TIMED.search(above) and placed.get(owner) == (depth, above):
            # the next action of this header's turn: it follows the skill before it
            flag += 1
            listed[-1] = (listed[-1][0], listed[-1][1] + f' @sets:{flag}')
            text = f'@after:{flag} ' + text
        phase_of[(owner, len(listed))] = phase or headed[owner] or 1
        listed.append((name, text))
        placed[owner] = (depth, above)
        skill_depth = depth
    for key, chained in follows.items():
        listed = out.get(key, {}).get('skills', [])
        for i, (name, text) in enumerate(listed):
            if loose(name) in chained and not FLAG_AFTER.search(text):
                listed[i] = (name, f'@after:{chained[loose(name)]} ' + text)
    phases = max([p for (_, _), p in phase_of.items()] or [1])
    for key, n in headed.items():
        # "Chilling Beauty Hilda" headed three times: phases 1-3, each a third of her HP;
        # a quest with '''Phase N''' headers counts those for every section
        n = phases if phase else n
        if n > 1:
            listed = out[key]['skills']
            for i, (name, text) in enumerate(listed):
                if phase_of.get((key, i), 1) > 1:
                    listed[i] = (name, text + f" @phase:{phase_of[(key, i)]}/{n}")
    for key, chained in leads.items():
        listed = out.get(key, {}).get('skills', [])
        for lead, k in chained.items():
            i = next((i for i, (name, _) in enumerate(listed) if loose(name) == lead), None)
            if i is not None:
                listed[i] = (listed[i][0], listed[i][1] + f' @sets:{k}')
    for key, hps in phase_hp.items():
        if key in out and any(hps):
            out[key]['hp'] = (out[key]['hp'] or 0) + sum(h for h in hps if h)
    return {k: v for k, v in out.items() if v['skills'] or v['hp']}


def rank(r: dict) -> tuple:
    """Row order: an exact turn first (it cannot wait; a follower's flag can), then
    followers, HP thresholds, other once-rows, every-N rows rarest first (every 4
    wins turn 4 from every 2), the random rows."""
    conds = {c['type']: c['parameter'] for c in r['self_conditions']}
    if 'act' in conds and 'limited_act' in conds:
        return (-2, 0)
    if 'flg_on' in conds:
        return (-1, 0)
    if 'hp_pr_under' in conds:
        return (0, 0)
    if 'limited_act' in conds:
        return (1, 0)
    if 'actbetween' in conds:
        return (2, -conds['actbetween'])
    return (3, 0)


def needs_second_action(rows: list) -> bool:
    """An every-turn skill would take a one-action boss's only action every turn."""
    return any(r['percent'] >= 100 and [c['type'] for c in r['self_conditions'] if c['type'] != 'skill'] == ['turn_limited_act']
               for r in rows)


class SkillIndex:
    """skill_mst names -> ids, resolved the way a boss's skills were authored."""

    def __init__(self, mst_rows: list):
        self.by = [defaultdict(list), defaultdict(list), defaultdict(list)]
        self.name, self.procs = {}, {}
        for r in mst_rows:
            sid = int(r['nj9Lw7mV'])
            for index, key in zip(self.by, (raw, norm, loose)):
                index[key(r['0nxpBDz2'])].append(sid)
            self.name[sid] = raw(r['0nxpBDz2'])
            self.procs[sid] = set(str(r.get('hjAy9St3') or '').split('@'))
        self.ids = sorted(self.name)

    def near_spelling(self, name: str, anchor: int) -> int | None:
        """A skill of this run spelt a little differently ("Try atttacking me!")."""
        lo, hi = bisect.bisect_left(self.ids, anchor - WINDOW), bisect.bisect_right(self.ids, anchor + WINDOW)
        best = max(self.ids[lo:hi], key=lambda i: difflib.SequenceMatcher(None, raw(name), self.name[i]).ratio(), default=None)
        if best is not None and difflib.SequenceMatcher(None, raw(name), self.name[best]).ratio() >= 0.88:
            return best
        return None

    def candidates(self, name: str) -> list:
        for index, key in zip(self.by, (raw, norm, loose)):
            if found := index.get(key(name)):
                return found
        return []

    def resolve(self, names: list, rare_anchors: bool = False) -> list:
        """One id per name from the densest run of ids: the anchor with the most
        names within WINDOW of it.  Only the names that are not quotes choose the
        anchor; a name listed several times takes the run's ids in order; a name
        with no id in that run is unresolved.  With rare_anchors only the names
        with the fewest ids choose it: a stray copy of a common name 150 ids below
        the run reaches it too, and the lowest anchor wins ties (Barion's "Empty
        Seal" is 2102750 in his run of one-copy names, not Kalon's 2102600).
        gen_boss_waves asks for that for the bosses it places itself; the kits
        already live keep the old choice until they are checked again."""
        cands = [self.candidates(n) for n in names]
        spoken = [bool(DIALOGUE.search(n)) for n in names]
        repeats = Counter(raw(n) for n in names)
        named = [c for c, s in zip(cands, spoken) if not s and c]
        if rare_anchors:
            named = [c for c in named if len(c) == min(map(len, named))]
        anchors = sorted({i for c in named for i in c}) or sorted({i for c in cands for i in c})
        if not anchors:
            return [None] * len(names)
        def pick(anchor):
            out, seen = [], Counter()
            for n, c in zip(names, cands):
                near = sorted(i for i in c if abs(i - anchor) <= WINDOW)
                k = seen[raw(n)]
                seen[raw(n)] += 1
                if not near:
                    out.append(None)
                elif repeats[raw(n)] > 1 and len(near) > 1:
                    out.append(near[min(k, len(near) - 1)])
                else:
                    out.append(min(near, key=lambda i: abs(i - anchor)))
            return out
        def score(anchor):
            return (sum(i is not None for i, s in zip(pick(anchor), spoken) if not s), -anchor)
        best = max(anchors, key=score)
        return [i if i is not None else self.near_spelling(n, best) for n, i in zip(names, pick(best))]


def _share(rows: list) -> list:
    """Rows that need the same moment share it at random instead of the first one
    always winning it (on a one-action boss the others would never fire)."""
    groups = defaultdict(list)
    for r in rows:
        conds = [c for c in r['self_conditions'] if c['type'] != 'skill']
        if r['percent'] >= 100 and conds and not any(c['type'] == 'flg_on' for c in conds):
            groups[json.dumps(conds, sort_keys=True)].append(r)
    for group in groups.values():
        for i, r in enumerate(group):
            r['percent'] = round(100.0 / (len(group) - i), 1)
    return rows


def build_kit(listed: list, index: SkillIndex, rare_anchors: bool = False) -> tuple:
    """(skills, AI rows sorted by rank, unresolved names) for one boss's wiki section."""
    skills, rows, unresolved = [], [], []
    for (name, text), sid in zip(listed, index.resolve([n for n, _ in listed], rare_anchors)):
        if sid is None:
            unresolved.append(name)
            continue
        if sid not in skills:
            if len(skills) >= MAX_SKILLS:
                continue
            skills.append(sid)
        made = triggers(text, skills.index(sid) + 1)
        if IMMUNE in index.procs.get(sid, ()):
            made = [r for r in made if [c for c in r['self_conditions'] if c['type'] != 'skill']]
        rows += [(r, text) for r in made]
    while True:
        set_flags = {fl[i] for r, _ in rows for fl in [r['action']['flag_changes']]
                     for i in range(0, len(fl) - 1, 2) if fl[i + 1] == 1}
        dead = [(r, text) for r, text in rows
                if any(c['type'] == 'flg_on' and c['parameter'] not in set_flags for c in r['self_conditions'])]
        if not dead:
            break
        # a follower whose leader has no row keeps the leader's trigger; its own sets stay
        kept = []
        for r, text in rows:
            if any(r is d for d, _ in dead):
                own = ' '.join(f'@sets:{k}' for k in FLAG_SET.findall(text))
                kept += [(x, own) for x in triggers(TOKENS.sub('', text) + ' ' + own, r['self_conditions'][0]['parameter'])]
            else:
                kept.append((r, text))
        rows = kept
    return skills, sorted(_share([r for r, _ in rows]), key=rank), unresolved


def _sets(rows: list) -> set:
    return {fl[i] for r in rows for fl in [r['action']['flag_changes']] for i in range(0, len(fl) - 1, 2) if fl[i + 1] == 1}


def finish_rows(rows: list) -> list:
    """The first MAX_ROWS rows and the closing attack; a follower whose leader did not
    make the cut goes too (and the rows behind it move up)."""
    rows = list(rows)
    while True:
        kept = rows[:MAX_ROWS]
        flags = _sets(kept)
        dead = [r for r in kept if any(c['type'] == 'flg_on' and c['parameter'] not in flags for c in r['self_conditions'])]
        if not dead:
            break
        rows = [r for r in rows if all(r is not d for d in dead)]
    rows = [json.loads(json.dumps(x)) for x in rows[:MAX_ROWS]] + [row([], 100, None)]
    for i, x in enumerate(rows, 1):
        x['priority'] = i
    return rows


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('--hints', type=Path, nargs='+', required=True)
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--report', type=Path)
    args = ap.parse_args()

    records = json.loads((ARCHIVE / 'mission.json').read_text(encoding='utf-8'))
    sources = json.loads((ARCHIVE / 'mission_sources.json').read_text(encoding='utf-8'))
    ai_text = (ARCHIVE / 'ai.json').read_text(encoding='utf-8')
    ais = json.loads(ai_text)
    taken = {a['id'] for a in ais}
    by_rows = {json.dumps(a['actions'], sort_keys=True): a['id'] for a in ais if a['id'] >= WIKI_AI_BASE}
    sk = json.loads((MST / 'skill_mst.json').read_text(encoding='utf-8'))
    index = SkillIndex(sk[next(iter(sk))])
    notes = {}
    for f in args.hints:
        for c in json.loads(f.read_text(encoding='utf-8')):
            q = c.get('quest') or {}
            if q.get('notes'):
                notes.setdefault(int(c['mission_id']), q)

    changed, unresolved = [], defaultdict(int)
    def apply(r, bosses, listed):
        changed = []
        skills, rows, missing = build_kit(listed, index)
        for name in missing:
            unresolved[name] += 1
        if not rows:
            return changed
        two = needs_second_action(rows)
        rows = finish_rows(rows)
        key = json.dumps(rows, sort_keys=True)
        if key not in by_rows:
            new_id = WIKI_AI_BASE
            while new_id in taken:
                new_id += 1
            taken.add(new_id)
            ais.append({'id': new_id, 'name': f"wiki {bosses[0]['name']}", 'actions': rows})
            by_rows[key] = new_id
        for m in bosses:
            m['skills'] = [{'skill_id': s, 'show_name': True} for s in skills]
            m['ai_id'] = by_rows[key]
            if two and m['act_max'] < 2:
                m['act_min'], m['act_max'] = 2, 2
            changed.append({'mission': r['id'], 'boss': m['name'], 'skills': len(skills), 'ai': m['ai_id']})
        return changed

    for r in records:
        if r['id'] in TUTORIAL or (sources.get(str(r['id'])) or {}).get('method') == 'captured':
            continue
        q = notes.get(r['id'])
        if not q:
            continue
        # A section's skills go only to the boss of that name: a stand-in boss (a
        # template's Keymaster Gilnea, a regenerated wave that lost the real boss)
        # is never named by it, so it gets nothing.
        wave = [m for s in r['stages'] if s.get('is_boss') for m in s['battle_monsters']]
        bosses = [m for m in wave if m['ai_id'] == 1 and not m.get('skills') and norm(m['name'])]
        if not bosses:
            continue
        by_boss = {k: v['skills'] for k, v in sections(q['notes']).items() if v['skills']}
        for boss_name, listed in by_boss.items():
            targets = [m for m in bosses if norm(m['name']) == boss_name]
            if not boss_name and len(wave) == 1:
                targets = bosses            # skills under no header: the quest's only boss
            if not targets:
                continue
            done = apply(r, targets, listed)
            changed.extend(done)
    print(f'{len(changed)} boss slots in {len({c["mission"] for c in changed})} missions get wiki skills; '
          f'{len(ais) - len(json.loads(ai_text))} new AI(s); {sum(unresolved.values())} skill names not in skill_mst')
    if args.report:
        args.report.write_text(json.dumps({'changed': changed, 'unresolved': unresolved}, indent=1) + '\n', encoding='utf-8')
    if not args.check:
        (ARCHIVE / 'mission.json').write_text(json.dumps(records, indent=1, ensure_ascii=False) + '\n', encoding='utf-8')
        (ARCHIVE / 'ai.json').write_text(json.dumps(ais, indent=4, ensure_ascii=False) + ('\n' if ai_text.endswith('\n') else ''), encoding='utf-8')
    return 0


if __name__ == '__main__':
    sys.exit(main())
