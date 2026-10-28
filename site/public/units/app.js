// Unit finder: reads /units/data.json (the game server's unitFinder.php, cached by the
// site Worker) and answers "where do I get this unit?". Read-only; no storage, no trackers.
'use strict';

const ELEMENTS = ['', 'Fire', 'Water', 'Earth', 'Thunder', 'Light', 'Dark'];
const ICON = id => `https://cdn.example.com/content/unit/img/unit_ills_thum_${id}.png`;
const LIST_CAP = 300;
const $ = id => document.getElementById(id);

let D = null;          // the JSON
let index = null;      // unit id -> sources
let names = [];        // [{id, norm, tokens}]
const filters = new Set();

function h(tag, attrs, ...kids) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v == null || v === false) continue;
    if (k === 'class') el.className = v; else el.setAttribute(k, v);
  }
  for (const kid of kids.flat()) if (kid != null && kid !== false) el.append(kid);
  return el;
}

const norm = s => s.normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase()
  .replace(/[^a-z0-9]+/g, ' ').trim();

function rarity(r) { return r >= 8 ? 'Omni' : `${r}★`; }
function unitName(id) { const u = D.units[id]; return u ? u[0] : `Unit ${id}`; }

function areaChain(areaId) {
  const out = [];
  const seen = new Set();
  for (let a = areaId; a && D.areas[a] && !seen.has(a); a = D.areas[a][1]) { seen.add(a); out.unshift(a); }
  return out;
}
function missionInfo(m) {
  const mi = D.missions[m] || ['Quest ' + m, 0];
  const dg = D.dungeons[mi[1]] || ['', 0];
  const chain = areaChain(dg[1]);
  return { name: mi[0] || 'Quest ' + m, dungeon: dg[0], chain, path: [...chain.map(a => D.areas[a][0]), dg[0]].filter(Boolean).join(' › ') };
}
function inArea(m, area) { return !area || missionInfo(m).chain.includes(Number(area)); }

function buildIndex() {
  index = {};
  const get = id => (index[id] ||= { enemy: [], caught: [], reward: [], shop: [], summon: [] });
  for (const r of D.enemy) {
    get(r[0]).enemy.push(r);
    if (r[2] && r[2] !== r[0]) get(r[2]).caught.push(r);
  }
  for (const r of D.reward) get(r[0]).reward.push(r);
  for (const r of D.shop) get(r[0]).shop.push(r);
  for (const u of (D.summon && D.summon.units) || []) get(u.id).summon = u.in;
  names = Object.entries(D.units).map(([id, u]) => {
    const n = norm(u[0]);
    return { id, norm: n, tokens: n.split(' ') };
  });
  // Areas that hold at least one listed quest, as "Parent › Area".
  const areas = new Map();
  for (const m of Object.keys(D.missions)) for (const a of missionInfo(m).chain) if (!areas.has(a))
    areas.set(a, areaChain(a).map(x => D.areas[x][0]).join(' › '));
  const sel = $('area');
  [...areas].sort((x, y) => x[1].localeCompare(y[1], 'en', { numeric: true })).forEach(([id, label]) => sel.append(h('option', { value: id }, label)));
}

// Sources of one unit, narrowed to the chosen area (shops and gates have none).
function sources(id, area) {
  const s = index[id] || { enemy: [], caught: [], reward: [], shop: [], summon: [] };
  const keep = rows => rows.filter(r => inArea(r[1], area));
  const enemy = keep(s.enemy), caught = keep(s.caught), reward = keep(s.reward);
  return {
    enemy, caught, reward, shop: s.shop, summon: s.summon,
    has: {
      enemy: enemy.length > 0,
      capture: enemy.some(r => r[2] === Number(id) && r[3] > 0) || caught.some(r => r[3] > 0),
      reward: reward.length > 0,
      shop: s.shop.length > 0,
      summon: s.summon.length > 0,
    },
  };
}
function passes(id, area) {
  if (!filters.size && !area) return true;
  const s = sources(id, area);
  if (area && !s.has.enemy && !s.caught.length && !s.has.reward && !filters.has('shop') && !filters.has('summon')) return false;
  for (const f of filters) if (!s.has[f]) return false;
  return true;
}

// Typo-tolerant match: substring first, else every typed word close to a word of the name.
function osa(a, b) {
  const m = a.length, n = b.length;
  if (Math.abs(m - n) > 2) return 3;
  const d = Array.from({ length: m + 1 }, (_, i) => [i, ...Array(n).fill(0)]);
  for (let j = 1; j <= n; j++) d[0][j] = j;
  for (let i = 1; i <= m; i++) for (let j = 1; j <= n; j++) {
    const c = a[i - 1] === b[j - 1] ? 0 : 1;
    d[i][j] = Math.min(d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + c);
    if (i > 1 && j > 1 && a[i - 1] === b[j - 2] && a[i - 2] === b[j - 1]) d[i][j] = Math.min(d[i][j], d[i - 2][j - 2] + 1);
  }
  return d[m][n];
}
function score(q, qt, u) {
  if (u.norm === q) return 100;
  const at = u.norm.indexOf(q);
  if (at === 0) return 90;
  if (at > 0) return 80 - Math.min(at, 20) / 2;
  let total = 0;
  for (const t of qt) {
    const allow = t.length <= 3 ? 0 : t.length <= 6 ? 1 : 2;
    let best = 9;
    for (const w of u.tokens) {
      if (w.startsWith(t)) { best = 0; break; }
      best = Math.min(best, osa(t, w), w.length > t.length ? osa(t, w.slice(0, t.length)) : 9);
    }
    if (best > allow) return 0;
    total += best;
  }
  return 60 - total * 5;
}

function unitLink(id, extra) {
  const u = D.units[id] || [`Unit ${id}`, 0, 0];
  return h('a', { href: `#u=${id}` },
    h('img', { class: 'ico', src: ICON(id), alt: '', loading: 'lazy', width: 44, height: 44 }),
    h('span', {}, h('span', { class: 'name' }, u[0]),
      h('span', { class: 'el' }, h('span', { class: 'star' }, rarity(u[1])), ` ${ELEMENTS[u[2]] || ''}`, extra || '')));
}

function renderList() {
  const area = $('area').value;
  const q = norm($('q').value);
  const box = $('results');
  box.replaceChildren();
  if (!q && !filters.size && !area) {
    $('status').textContent = `${names.length.toLocaleString()} units. Type a name, or pick a filter or an area to browse.`;
    return;
  }
  const qt = q.split(' ').filter(Boolean);
  let hits = names.map(u => ({ u, s: q ? score(q, qt, u) : 1 })).filter(x => x.s > 0 && passes(x.u.id, area));
  hits.sort((a, b) => b.s - a.s || a.u.norm.localeCompare(b.u.norm) || a.u.id - b.u.id);
  $('status').textContent = hits.length ? (hits.length > LIST_CAP ? `${hits.length.toLocaleString()} units match; showing the first ${LIST_CAP}.` : `${hits.length} unit${hits.length === 1 ? '' : 's'} match.`)
    : 'No unit matches. Check the spelling or clear a filter.';
  box.append(h('ul', { class: 'list' }, hits.slice(0, LIST_CAP).map(x => h('li', {}, unitLink(x.u.id)))));
}

function questRow(r, extra) {
  const info = missionInfo(r[1]);
  return h('li', {}, h('span', { class: 'quest' }, info.name), h('span', { class: 'path' }, info.path), h('div', { class: 'tags' }, extra));
}
function captureTags(r, self) {
  const tags = [];
  tags.push(r[3] > 0 ? h('span', {}, 'Capture ', h('span', { class: 'pct' }, `${r[3]}%`)) : h('span', { class: 'no' }, 'Not capturable'));
  if (r[2] && r[2] !== self) tags.push(h('span', { class: 'joins' }, 'joins as ',
    h('img', { class: 'ico', src: ICON(r[2]), alt: '', loading: 'lazy', width: 24, height: 24 }),
    h('a', { href: `#u=${r[2]}` }, `${unitName(r[2])} (${rarity((D.units[r[2]] || [0, 0])[1])})`)));
  if (r[6]) tags.push(h('span', { class: 'badge' }, 'Boss'));
  if (r[5] > 1) tags.push(h('span', { class: 'el' }, `×${r[5]} in this quest`));
  if (r[4] < 100) tags.push(h('span', { class: 'el' }, `appears in ${r[4]}% of runs`));
  return tags;
}
function sortRows(rows) {
  const by = $('sort').value;
  const name = m => missionInfo(m).name;
  return [...rows].sort(by === 'game' ? (a, b) => a[1] - b[1]
    : by === 'name' ? (a, b) => name(a[1]).localeCompare(name(b[1])) || a[1] - b[1]
    : (a, b) => b[3] - a[3] || a[1] - b[1]);
}

function renderUnit(id) {
  const box = $('unit');
  const u = D.units[id];
  box.replaceChildren();
  if (!u) { box.append(h('p', {}, 'That unit is not in the game data.')); return; }
  const area = $('area').value;
  const s = sources(id, area);
  const show = f => !filters.size || filters.has(f);
  const self = Number(id);
  box.append(h('div', { class: 'unit-head' },
    h('img', { class: 'ico big', src: ICON(id), alt: '', width: 72, height: 72 }),
    h('div', {}, h('h2', { style: 'margin:0' }, u[0]),
      h('p', {}, h('span', { class: 'star' }, rarity(u[1])), ` ${ELEMENTS[u[2]] || ''} · unit ${id}`))));
  let shown = 0;
  const section = (title, rows) => { if (!rows.length) return; shown++; box.append(h('h2', {}, title), h('ul', { class: 'rows' }, rows)); };

  if (show('enemy') || show('capture')) {
    const own = sortRows(s.enemy.filter(r => show('enemy') || r[3] > 0));
    section(`Fights as an enemy in ${own.length} quest${own.length === 1 ? '' : 's'}`, own.map(r => questRow(r, captureTags(r, self))));
    const from = sortRows(s.caught.filter(r => r[3] > 0 || show('enemy')));
    section(`Joins your squad from another enemy in ${from.length} quest${from.length === 1 ? '' : 's'}`,
      from.map(r => questRow(r, [h('span', { class: 'joins' }, 'from ',
        h('img', { class: 'ico', src: ICON(r[0]), alt: '', loading: 'lazy', width: 24, height: 24 }),
        h('a', { href: `#u=${r[0]}` }, `${unitName(r[0])} (${rarity((D.units[r[0]] || [0, 0])[1])})`)),
        ...captureTags(r, r[2]).slice(0, 1), r[6] ? h('span', { class: 'badge' }, 'Boss') : null])));
  }
  if (show('reward')) section('Clear reward', [...s.reward].sort((a, b) => a[2] - b[2] || a[1] - b[1]).map(r => questRow([r[0], r[1]],
    [h('span', {}, r[2] === 1 ? 'First clear' : 'Every clear'), r[3] > 1 ? h('span', { class: 'el' }, `×${r[3]}`) : null])));
  if (show('shop')) section('Event shops', s.shop.map(r => h('li', {},
    h('span', { class: 'quest' }, `${D.tokens[r[1]] || 'Event token'} shop`),
    h('div', { class: 'tags' }, h('span', {}, `${r[2]} ${D.tokens[r[1]] || 'tokens'}${r[4] > 1 ? ` for ${r[4]}` : ''}`), h('span', { class: 'el' }, `buy up to ${r[3]}`)))));
  if (show('summon')) {
    const gates = new Map(((D.summon && D.summon.gates) || []).map(g => [g.gate, g]));
    section('Summon gates', [...s.summon].sort((a, b) => b[1] - a[1]).map(([g, pct]) => {
      const gate = gates.get(g) || { title: `Gate ${g}` };
      return h('li', {}, h('a', { class: 'quest', href: `/gates/${g}` }, gate.title),
        h('div', { class: 'tags' }, h('span', {}, h('span', { class: 'pct' }, `${pct}%`), ' per pull'),
          gate.rotating ? h('span', { class: 'el' }, gate.featured ? 'featured this week' : 'rotating gate') : null));
    }));
  }
  if (!shown) box.append(h('p', {}, filters.size || area ? 'Nothing for this unit with these filters.' : 'No live source for this unit right now: no quest, reward, event shop or summon gate gives it.'));
  box.append(h('p', { class: 'note' }, h('a', { href: '#' }, '← Back to the list')));
}

function render() {
  if (!D) return;
  const m = location.hash.match(/^#u=(\d+)$/);
  $('unit').classList.toggle('hide', !m);
  $('results').classList.toggle('hide', !!m);
  if (m) { $('status').textContent = ''; renderUnit(m[1]); window.scrollTo(0, 0); } else renderList();
}

async function load() {
  $('status').textContent = 'Loading the live game data…';
  try {
    const res = await fetch('/units/data.json');
    if (!res.ok) throw new Error(res.status);
    D = await res.json();
    if (!D.units || !D.enemy) throw new Error('bad data');
  } catch (e) {
    $('status').replaceChildren('Could not reach the game server. ', h('button', { type: 'button', class: 'chip', id: 'retry' }, 'Try again'));
    $('retry').addEventListener('click', load);
    return;
  }
  buildIndex();
  $('built').textContent = `Data from ${new Date(D.built * 1000).toLocaleString()}.`;
  render();
}

let typing = 0;
$('q').addEventListener('input', () => {
  clearTimeout(typing);
  typing = setTimeout(() => { if (location.hash) history.replaceState(null, '', location.pathname); render(); }, 120);
});
$('filters').addEventListener('click', e => {
  const b = e.target.closest('button[data-f]');
  if (!b) return;
  const on = b.getAttribute('aria-pressed') !== 'true';
  b.setAttribute('aria-pressed', on);
  if (on) filters.add(b.dataset.f); else filters.delete(b.dataset.f);
  render();
});
$('area').addEventListener('change', render);
$('sort').addEventListener('change', render);
window.addEventListener('hashchange', render);
// A unit without CDN art: keep the layout, drop the broken image.
document.addEventListener('error', e => { if (e.target.tagName === 'IMG') e.target.style.visibility = 'hidden'; }, true);
load();
