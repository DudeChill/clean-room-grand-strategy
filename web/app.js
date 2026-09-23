// Client for the clean-room grand strategy engine.
//
// Pure observer: renders snapshots from /api/state and issues commands through
// /api/command. No gameplay logic lives here.

'use strict';

const App = {
  map: null,            // static map: {provinces:[{id,name,x,y,terrain,sea,state,adj}], states:[]}
  snap: null,           // latest world snapshot
  meta: { paused: true, speed: 0, player: 0 },
  countryColors: new Map(),
  selection: -1,
  selectedDivision: -1,
  selectedArmy: -1,
  overlay: 'political',
  cam: { x: 0, y: 0, zoom: 3 },
  tab: 'production',
  toastTimer: null,
};

const TERRAIN_COLORS = {
  plains: '#6f7f57', forest: '#4a6741', hills: '#7d7452', mountain: '#877d70',
  urban: '#7f7f8f', marsh: '#5d6b5a', desert: '#c2b280', jungle: '#3f6b46',
  ocean: '#20456b', shallow_sea: '#2f5c85', deep_ocean: '#173250', lakes: '#2f5c85',
};

function countryColor(id) {
  if (id === undefined || id === null || id < 0) return '#3a3f46';
  if (!App.countryColors.has(id)) {
    const hue = (id * 137.508) % 360;
    const sat = 45 + (id % 3) * 10;
    const light = 38 + (id % 4) * 6;
    App.countryColors.set(id, `hsl(${hue}, ${sat}%, ${light}%)`);
  }
  return App.countryColors.get(id);
}

function countryById(id) {
  if (!App.snap || !App.snap.countries) return null;
  const list = App.snap.countries;
  for (const c of list) if (c.id === id) return c;
  return null;
}

function tagOf(id) {
  const c = countryById(id);
  return c ? c.tag : '—';
}

async function api(path, options) {
  const res = await fetch(path, options);
  const text = await res.text();
  try { return JSON.parse(text); } catch (e) { return { ok: false, error: text }; }
}

async function sendCommand(payload) {
  const res = await api('/api/command', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload),
  });
  if (!res.ok) toast(res.error || 'command rejected');
  return res.ok;
}

function toast(message) {
  const el = document.getElementById('toast');
  el.textContent = message;
  clearTimeout(App.toastTimer);
  App.toastTimer = setTimeout(() => { el.textContent = ''; }, 4000);
}

async function setTime(paused, speed) {
  const body = {};
  if (paused !== undefined) body.paused = paused;
  if (speed !== undefined) body.speed = speed;
  const res = await api('/api/time', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });
  if (res.ok) { App.meta.paused = res.paused; App.meta.speed = res.speed; renderSpeedButtons(); }
}

function renderSpeedButtons() {
  for (const b of document.querySelectorAll('button.speed')) {
    const s = b.dataset.speed ? Number(b.dataset.speed) : 0;
    const active = (s === 0 && App.meta.paused) || (!App.meta.paused && s === App.meta.speed);
    b.classList.toggle('active', active);
  }
}

// ------------------------------------------------------------------ map -----

function provinceAt(worldX, worldY) {
  const px = Math.floor(worldX);
  const py = Math.floor(worldY);
  return App.byCoord.get(`${px},${py}`) || null;
}

function provinceById(id) {
  return App.byId.get(id) || null;
}

function screenToWorld(sx, sy) {
  const rect = canvas.getBoundingClientRect();
  const z = App.cam.zoom;
  return {
    x: (sx - rect.left) / z + App.cam.x,
    y: (sy - rect.top) / z + App.cam.y,
  };
}

const canvas = document.getElementById('map');
const ctx = canvas.getContext('2d');

function resize() {
  const rect = canvas.getBoundingClientRect();
  canvas.width = Math.floor(rect.width * devicePixelRatio);
  canvas.height = Math.floor(rect.height * devicePixelRatio);
  ctx.setTransform(devicePixelRatio, 0, 0, devicePixelRatio, 0, 0);
}

function provinceColor(p) {
  const snap = App.snap;
  if (!snap) return TERRAIN_COLORS[p.terrain] || '#444';
  switch (App.overlay) {
    case 'terrain': return TERRAIN_COLORS[p.terrain] || '#444';
    case 'supply': {
      if (p.sea) return TERRAIN_COLORS[p.terrain] || '#20456b';
      const s = (snap.supply[p.id] || 0) / 100;
      const v = Math.round(40 + s * 150);
      return `rgb(${Math.round(v * 0.5)},${v},${Math.round(v * 0.6)})`;
    }
    case 'control': {
      const c = snap.controller[p.id];
      return c >= 0 ? countryColor(c) : '#2c333c';
    }
    case 'air': {
      // Green where the player's side controls the sky, red where the enemy does.
      const p2 = p.region !== undefined ? App.airControl.get(p.region) : null;
      const me = snap.player ? snap.player.id : -1;
      let mine = 0, theirs = 0;
      if (p2) {
        for (const [country, share] of p2) {
          if (country === me) mine += share;
          else theirs += share;
        }
      }
      const total = mine + theirs;
      if (total <= 0.001) return p.sea ? (TERRAIN_COLORS[p.terrain] || '#20456b') : '#3a3f46';
      const balance = (mine - theirs) / total;  // -1 enemy, +1 friendly
      const t = (balance + 1) / 2;
      return `rgb(${Math.round(200 * (1 - t) + 40 * t)},${Math.round(60 + 100 * t)},${Math.round(60 + 40 * t)})`;
    }
    case 'front': {
      const c = snap.controller[p.id];
      let base = countryColor(c);
      if (!p.sea && p.adj.some((n) => snap.controller[n] >= 0 && snap.controller[n] !== c)) {
        base = '#c94f4f';
      }
      return base;
    }
    case 'political':
    default: {
      const o = snap.owner[p.id];
      if (o < 0) return TERRAIN_COLORS[p.terrain] || '#20456b';
      const ctl = snap.controller[p.id];
      const base = countryColor(o);
      if (ctl >= 0 && ctl !== o) {
        // Occupied: owner colour tinted toward the occupier.
        return mixColors(base, countryColor(ctl), 0.55);
      }
      return base;
    }
  }
}

function mixColors(a, b, t) {
  const pa = parseColor(a), pb = parseColor(b);
  if (!pa || !pb) return b;
  const r = Math.round(pa[0] + (pb[0] - pa[0]) * t);
  const g = Math.round(pa[1] + (pb[1] - pa[1]) * t);
  const bl = Math.round(pa[2] + (pb[2] - pa[2]) * t);
  return `rgb(${r},${g},${bl})`;
}

const colorCache = new Map();
function parseColor(text) {
  if (colorCache.has(text)) return colorCache.get(text);
  let out = null;
  if (text.startsWith('#')) {
    const n = parseInt(text.slice(1), 16);
    out = [(n >> 16) & 255, (n >> 8) & 255, n & 255];
  } else {
    const m = text.match(/hsl\((\d+(?:\.\d+)?),\s*(\d+)%,\s*(\d+)%\)/);
    if (m) out = hslToRgb(Number(m[1]) / 360, Number(m[2]) / 100, Number(m[3]) / 100);
  }
  colorCache.set(text, out);
  return out;
}

function hslToRgb(h, s, l) {
  const f = (n) => {
    const k = (n + h * 12) % 12;
    const a = s * Math.min(l, 1 - l);
    return Math.round(255 * (l - a * Math.max(-1, Math.min(k - 3, 9 - k, 1))));
  };
  return [f(0), f(8), f(4)];
}

function draw() {
  if (!App.map) return;
  const w = canvas.width / devicePixelRatio;
  const h = canvas.height / devicePixelRatio;
  ctx.fillStyle = '#0b0f14';
  ctx.fillRect(0, 0, w, h);

  const z = App.cam.zoom;
  const cell = z;
  ctx.save();
  ctx.translate(-App.cam.x * z, -App.cam.y * z);

  const x0 = Math.floor(App.cam.x) - 1;
  const y0 = Math.floor(App.cam.y) - 1;
  const x1 = Math.ceil(App.cam.x + w / z) + 1;
  const y1 = Math.ceil(App.cam.y + h / z) + 1;

  const snap = App.snap;
  for (const p of App.map.provinces) {
    if (p.x < x0 || p.x > x1 || p.y < y0 || p.y > y1) continue;
    ctx.fillStyle = provinceColor(p);
    ctx.fillRect(p.x * cell, p.y * cell, cell, cell);
    if (z > 6 && p.vp > 0) {
      ctx.fillStyle = 'rgba(255,255,255,0.85)';
      ctx.fillRect(p.x * cell + cell * 0.42, p.y * cell + cell * 0.42, cell * 0.16, cell * 0.16);
    }
  }

  // Supply hubs and fort markers.
  if (z > 5) {
    for (const p of App.map.provinces) {
      if (p.x < x0 || p.x > x1 || p.y < y0 || p.y > y1 || p.sea) continue;
      if (p.hub) {
        ctx.strokeStyle = 'rgba(255,255,255,0.6)';
        ctx.lineWidth = 1;
        ctx.strokeRect(p.x * cell + cell * 0.25, p.y * cell + cell * 0.25, cell * 0.5, cell * 0.5);
      }
    }
  }

  // Armies' front lines and offensives.
  if (snap && snap.player && (App.overlay === 'front' || App.overlay === 'political')) {
    for (const army of (snap.player.armies || [])) {
      if (!army.line || army.line.length === 0) continue;
      ctx.strokeStyle = army.order === 'offensive' ? '#ff9c4a' : '#7fd4ff';
      ctx.lineWidth = Math.max(1.5, z * 0.25);
      ctx.beginPath();
      const centers = army.line.map((pid) => provinceById(pid)).filter(Boolean);
      centers.forEach((p, i) => {
        const cx = (p.x + 0.5) * cell;
        const cy = (p.y + 0.5) * cell;
        if (i === 0) ctx.moveTo(cx, cy); else ctx.lineTo(cx, cy);
      });
      ctx.stroke();
      for (const pid of (army.target_line || [])) {
        const p = provinceById(pid);
        if (!p) continue;
        ctx.strokeStyle = '#ff5f3f';
        ctx.strokeRect(p.x * cell + 1, p.y * cell + 1, cell - 2, cell - 2);
      }
    }
  }

  // Divisions as unit counters at province centres.
  if (snap && z >= 4) {
    const byProvince = new Map();
    for (const d of snap.divisions) {
      if (d.province < 0 || d.training) continue;
      if (!byProvince.has(d.province)) byProvince.set(d.province, []);
      byProvince.get(d.province).push(d);
    }
    for (const [pid, list] of byProvince) {
      const p = provinceById(pid);
      if (!p) continue;
      const friendly = list.filter((d) => d.country === (snap.player ? snap.player.id : -1));
      const show = friendly.length ? friendly : list.slice(0, 1);
      const cx = (p.x + 0.5) * cell;
      const cy = (p.y + 0.5) * cell;
      const size = Math.max(5, Math.min(z * 0.7, 14));
      ctx.fillStyle = 'rgba(10,14,18,0.85)';
      ctx.strokeStyle = friendly.length ? '#e8f0ff' : '#ff9a9a';
      ctx.lineWidth = 1;
      ctx.fillRect(cx - size / 2, cy - size / 2, size, size);
      ctx.strokeRect(cx - size / 2, cy - size / 2, size, size);
      ctx.fillStyle = friendly.length ? '#e8f0ff' : '#ff9a9a';
      ctx.font = `${Math.max(7, size * 0.7)}px monospace`;
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.fillText(String(show.length), cx, cy);
    }
  }

  // Battles.
  if (snap) {
    for (const b of snap.battles) {
      const p = provinceById(b.province);
      if (!p) continue;
      const cx = (p.x + 0.5) * cell;
      const cy = (p.y + 0.5) * cell;
      const r = Math.max(3, cell * 0.45);
      ctx.beginPath();
      ctx.arc(cx, cy, r, 0, Math.PI * 2);
      ctx.strokeStyle = '#ffd166';
      ctx.lineWidth = Math.max(1.5, cell * 0.12);
      ctx.stroke();
    }
  }

  // Player-owned provinces get a bright border so their country is findable.
  if (snap && snap.player && z >= 3) {
    const me = snap.player.id;
    ctx.strokeStyle = 'rgba(255,255,255,0.85)';
    ctx.lineWidth = Math.max(1, z * 0.12);
    for (const p of App.map.provinces) {
      if (p.x < x0 || p.x > x1 || p.y < y0 || p.y > y1 || p.sea) continue;
      if (snap.controller[p.id] !== me) continue;
      const edge = (dx, dy) => {
        const q = App.byCoord.get(`${p.x + dx},${p.y + dy}`);
        return !q || snap.controller[q.id] !== me;
      };
      if (edge(1, 0)) ctx.strokeRect(p.x * cell + cell - 1, p.y * cell, 1, cell);
      if (edge(-1, 0)) ctx.strokeRect(p.x * cell, p.y * cell, 1, cell);
      if (edge(0, 1)) ctx.strokeRect(p.x * cell, p.y * cell + cell - 1, cell, 1);
      if (edge(0, -1)) ctx.strokeRect(p.x * cell, p.y * cell, cell, 1);
    }
  }

  // Selection marker.
  if (App.selection >= 0) {
    const p = provinceById(App.selection);
    if (p) {
      ctx.strokeStyle = '#ffffff';
      ctx.lineWidth = Math.max(1.5, z * 0.2);
      ctx.strokeRect(p.x * cell, p.y * cell, cell, cell);
    }
  }

  ctx.restore();
}

// ------------------------------------------------------------ interaction ---

let dragging = false;
let dragMoved = false;
let dragStart = { x: 0, y: 0, camx: 0, camy: 0 };

canvas.addEventListener('mousedown', (e) => {
  dragging = true;
  dragMoved = false;
  dragStart = { x: e.clientX, y: e.clientY, camx: App.cam.x, camy: App.cam.y };
});

canvas.addEventListener('mousemove', (e) => {
  if (dragging) {
    const dx = (e.clientX - dragStart.x) / App.cam.zoom;
    const dy = (e.clientY - dragStart.y) / App.cam.zoom;
    if (Math.abs(dx) + Math.abs(dy) > 0.5) dragMoved = true;
    App.cam.x = Math.max(0, dragStart.camx - dx);
    App.cam.y = Math.max(0, dragStart.camy - dy);
  }
  const world = screenToWorld(e.clientX, e.clientY);
  const p = provinceAt(world.x, world.y);
  const tip = document.getElementById('tooltip');
  if (p && App.snap) {
    const owner = App.snap.owner[p.id];
    const ctl = App.snap.controller[p.id];
    const supply = App.snap.supply[p.id];
    const divs = App.snap.divisions.filter((d) => d.province === p.id && !d.training).length;
    tip.textContent =
      `${p.name}${p.sea ? ' (sea)' : ''}\n` +
      `terrain: ${p.terrain}   vp: ${p.vp}\n` +
      `owner: ${tagOf(owner)}   controller: ${tagOf(ctl)}\n` +
      `supply: ${supply}%   divisions: ${divs}   infra: ${p.infra}`;
    tip.style.left = `${e.clientX - canvas.getBoundingClientRect().left + 12}px`;
    tip.style.top = `${e.clientY - canvas.getBoundingClientRect().top + 12}px`;
    tip.classList.remove('hidden');
  } else {
    tip.classList.add('hidden');
  }
});

canvas.addEventListener('mouseup', async (e) => {
  const wasDragging = dragging;
  dragging = false;
  if (!wasDragging || dragMoved) return;
  const world = screenToWorld(e.clientX, e.clientY);
  const p = provinceAt(world.x, world.y);
  if (!p) return;

  if (App.selectedDivision >= 0) {
    const div = App.snap.divisions.find((d) => d.id === App.selectedDivision);
    if (div) {
      if (div.training) {
        const ok = await sendCommand({ type: 'deploy_division', division: div.id, province: p.id });
        if (ok) toast(`Deployed ${div.name}`);
      } else {
        const ok = await sendCommand({ type: 'move_division', division: div.id, province: p.id });
        if (ok) toast(`${div.name} ordered to ${p.name}`);
      }
      App.selectedDivision = -1;
      refresh();
      return;
    }
  }
  App.selection = p.id;
  renderLeft();
});

canvas.addEventListener('wheel', (e) => {
  e.preventDefault();
  const before = screenToWorld(e.clientX, e.clientY);
  App.cam.zoom = Math.max(1, Math.min(24, App.cam.zoom * (e.deltaY < 0 ? 1.15 : 0.87)));
  const after = screenToWorld(e.clientX, e.clientY);
  App.cam.x += before.x - after.x;
  App.cam.y += before.y - after.y;
}, { passive: false });

document.getElementById('speed-pause').addEventListener('click', () => setTime(!App.meta.paused));
for (const b of document.querySelectorAll('button.speed[data-speed]')) {
  b.addEventListener('click', () => setTime(false, Number(b.dataset.speed)));
}
document.addEventListener('keydown', (e) => {
  if (e.code === 'Space') { e.preventDefault(); setTime(!App.meta.paused); }
  if (e.key >= '1' && e.key <= '5') setTime(false, Number(e.key));
  if (e.key === 'Escape') {
    App.selectedDivision = -1;
    App.selection = -1;
    renderLeft();
  }
  const tabKeys = { m: 'military', p: 'production', c: 'construction', r: 'research',
                    d: 'diplomacy', l: 'log', a: 'air' };
  const tab = tabKeys[e.key.toLowerCase()];
  if (tab && !e.ctrlKey && !e.metaKey) {
    App.tab = tab;
    for (const t of document.querySelectorAll('button.tab')) {
      t.classList.toggle('active', t.dataset.tab === tab);
    }
    for (const p of document.querySelectorAll('.panel')) {
      p.classList.toggle('active', p.id === `panel-${tab}`);
    }
    renderPanels();
  }
  // Arrow-key camera panning at one province per press, ten with Shift.
  const step = e.shiftKey ? 10 : 2;
  if (e.key === 'ArrowLeft') App.cam.x = Math.max(0, App.cam.x - step);
  if (e.key === 'ArrowRight') App.cam.x += step;
  if (e.key === 'ArrowUp') App.cam.y = Math.max(0, App.cam.y - step);
  if (e.key === 'ArrowDown') App.cam.y += step;
});
document.getElementById('recenter').addEventListener('click', () => {
  const cap = App.snap && App.snap.player && App.snap.player.capital;
  const prov = provinceById(App.selection) ||
               (cap ? provinceById(cap) : null);
  if (prov) centerOn(prov.x + 0.5, prov.y + 0.5);
});
document.getElementById('save').addEventListener('click', async () => {
  const res = await api('/api/save', { method: 'POST' });
  toast(res.ok ? `saved to ${res.path}` : `save failed: ${res.error}`);
});
document.getElementById('load').addEventListener('click', async () => {
  const res = await api('/api/load', { method: 'POST' });
  if (res.ok) {
    toast(`loaded at tick ${res.tick}`);
    await refresh();
    renderPanels();
  } else {
    toast(`load failed: ${res.error}`);
  }
});
for (const b of document.querySelectorAll('button.ov')) {
  b.addEventListener('click', () => {
    App.overlay = b.dataset.overlay;
    for (const o of document.querySelectorAll('button.ov')) o.classList.toggle('active', o === b);
  });
}
for (const b of document.querySelectorAll('button.tab')) {
  b.addEventListener('click', () => {
    App.tab = b.dataset.tab;
    for (const t of document.querySelectorAll('button.tab')) t.classList.toggle('active', t === b);
    for (const p of document.querySelectorAll('.panel')) {
      p.classList.toggle('active', p.id === `panel-${App.tab}`);
    }
    renderPanels();
  });
}

// ----------------------------------------------------------------- panels ---

function renderTop() {
  if (!App.snap) return;
  const p = App.snap.player || {};
  document.getElementById('date').textContent = App.snap.date;
  document.getElementById('country-badge').textContent = `${p.tag || '—'} ${p.name || ''}`;
  document.getElementById('stats').textContent =
    `MP ${Math.round(p.manpower || 0).toLocaleString()}  PP ${(p.pp || 0).toFixed(0)}` +
    `  Stab ${((p.stability || 0) * 100).toFixed(0)}%  WS ${((p.war_support || 0) * 100).toFixed(0)}%` +
    `  Fuel ${Math.round(p.fuel || 0)}`;
}

async function renderProvinceDetails(pid) {
  const battleEl = document.getElementById('battle-detail');
  const supplyEl = document.getElementById('supply-detail');
  battleEl.textContent = 'loading…';
  supplyEl.textContent = 'loading…';

  const supply = await api(`/api/supply?province=${pid}`);
  if (supply && !supply.error) {
    let html = `<b>${supply.country || '—'}</b> · level ${(supply.supply_level * 100).toFixed(0)}%` +
      ` · delivered ${supply.delivered.toFixed(1)}/hr` +
      (supply.bottleneck ? ` · bottleneck ${supply.bottleneck}` : '') + '<br>';
    html += supply.route.map((s) =>
      `<span class="dim">→</span> ${s.name} <span class="dim">${s.capacity.toFixed(1)}</span>`).join('<br>');
    if (supply.divisions.length) {
      html += '<br>' + supply.divisions.map((d) =>
        `${d.name}: supply ${(d.supply * 100).toFixed(0)}% fuel ${(d.fuel * 100).toFixed(0)}%`).join('<br>');
    }
    supplyEl.innerHTML = html;
  } else {
    supplyEl.textContent = 'no supply information';
  }

  const snapBattle = (App.snap.battles || []).find((b) => b.province === pid);
  if (!snapBattle) {
    battleEl.textContent = 'no battle here';
    return;
  }
  const battle = await api(`/api/battle?id=${snapBattle.id}`);
  if (!battle || battle.error) {
    battleEl.textContent = 'battle detail unavailable';
    return;
  }
  const side = (label, s) =>
    `<b>${label}</b> soft ${s.soft_attack.toFixed(0)} hard ${s.hard_attack.toFixed(0)} ` +
    `def ${s.defense.toFixed(0)} brk ${s.breakthrough.toFixed(0)}<br>` +
    s.divisions.map((d) =>
      `<span class="dim">${d.tag}</span> ${d.name}: org ${d.org.toFixed(0)}/${d.max_org.toFixed(0)}` +
      ` str ${(d.strength * 100).toFixed(0)}% sup ${(d.supply * 100).toFixed(0)}%` +
      ` ent ${(d.entrenchment * 100).toFixed(0)}% plan ${(d.planning * 100).toFixed(0)}%`).join('<br>');
  let html = `${battle.terrain}${battle.river_crossing ? ' · river' : ''}` +
    `${battle.encirclement ? ' · <span class="bad">encircled</span>' : ''}` +
    ` · progress ${(battle.progress * 100).toFixed(0)}%<br>` +
    side('Attackers', battle.attacker) + '<br>' + side('Defenders', battle.defender);
  if (battle.debug.length) {
    const d = battle.debug[0];
    html += '<br><span class="dim">last tick</span> base ' + d.base_attack.toFixed(1) +
      ` × terrain ${d.terrain.toFixed(2)} × supply ${d.supply.toFixed(2)}` +
      ` + planning ${d.planning.toFixed(2)} + commander ${d.commander.toFixed(2)}` +
      ` + exp ${d.experience.toFixed(2)} + air ${(d.air || 0).toFixed(2)} → ${d.final_attack.toFixed(1)} vs ` +
      `defense ${d.enemy_defense.toFixed(1)} = damage ${d.damage.toFixed(2)}` +
      ` (org ${d.org_damage.toFixed(2)}, str ${d.strength_damage.toFixed(3)})`;
  }
  battleEl.innerHTML = html;
}

function renderLeft() {
  const alerts = (App.snap && App.snap.player && App.snap.player.alerts) || [];
  const el = document.getElementById('alerts');
  el.innerHTML = '';
  if (alerts.length === 0) el.innerHTML = '<div class="small">no alerts</div>';
  for (const a of alerts) {
    const div = document.createElement('div');
    div.textContent = a.text;
    div.addEventListener('click', () => {
      App.tab = a.action;
      for (const t of document.querySelectorAll('button.tab')) {
        t.classList.toggle('active', t.dataset.tab === a.action);
      }
      for (const p of document.querySelectorAll('.panel')) {
        p.classList.toggle('active', p.id === `panel-${a.action}`);
      }
      renderPanels();
    });
    el.appendChild(div);
  }

  const sel = document.getElementById('selection');
  const pd = document.getElementById('province-divisions');
  pd.innerHTML = '';
  if (App.selection < 0 || !App.map) {
    sel.textContent = 'Click a province.';
    return;
  }
  const p = provinceById(App.selection);
  if (!p) return;
  const owner = App.snap.owner[p.id];
  const ctl = App.snap.controller[p.id];
  sel.innerHTML =
    `<b>${p.name}</b><br>` +
    `terrain ${p.terrain} · vp ${p.vp} · infra ${p.infra}<br>` +
    `owner ${tagOf(owner)} · controller ${tagOf(ctl)}<br>` +
    `supply ${App.snap.supply[p.id]}%<br>` +
    `<span class="dim">state ${p.state}</span>`;
  const list = App.snap.divisions.filter((d) => d.province === p.id && !d.training);
  for (const d of list) {
    const row = document.createElement('div');
    row.className = 'row' + (d.id === App.selectedDivision ? ' selected' : '');
    row.innerHTML = `<span>${d.name} <span class="dim">${tagOf(d.country)}</span></span>` +
      `<span class="num">${d.org.toFixed(0)}/${d.max_org.toFixed(0)} · ${(d.strength * 100).toFixed(0)}%</span>`;
    row.addEventListener('click', (ev) => {
      // Plain click selects the division for a move order; shift-click assigns it to
      // the player's first army (fast way to get units under a commander).
      if (ev.shiftKey && d.country === App.snap.player.id) {
        const armies = App.snap.player.armies || [];
        if (armies.length > 0) {
          sendCommand({ type: 'assign_division_to_army', army: armies[0].id, division_ids: [d.id] })
            .then(() => refresh());
        } else {
          toast('create an army first (Military tab)');
        }
        return;
      }
      App.selectedDivision = App.selectedDivision === d.id ? -1 : d.id;
      toast(App.selectedDivision >= 0 ? 'division selected — click a target province' : '');
      renderLeft();
    });
    pd.appendChild(row);
  }
  if (list.length === 0) pd.innerHTML = '<div class="small">none</div>';
  pd.insertAdjacentHTML('beforeend',
    '<div class="small dim">click = select for orders · shift-click = assign to first army</div>');

  const now = Date.now();
  if (App.detailProvince !== App.selection || now - (App.detailAt || 0) > 1500) {
    App.detailProvince = App.selection;
    App.detailAt = now;
    renderProvinceDetails(App.selection);
  }
}

function productionPanel() {
  const p = App.snap.player;
  const el = document.getElementById('panel-production');
  const options = App.snap.equipment_defs
    .map((e) => `<option value="${e.key}">${e.name} (${e.category}, ${e.cost})</option>`)
    .join('');
  let html = `<div class="section"><div class="row-actions">
      <select id="line-equipment">${options}</select>
      <input id="line-factories" type="number" min="0" max="50" value="5" style="width:60px" />
      <button id="add-line">Assign</button>
    </div></div>`;
  html += '<table><tr><th>Equipment</th><th class="num">Factories</th><th class="num">Efficiency</th>' +
          '<th class="num">Output</th><th class="num">Resources</th><th></th></tr>';
  for (const l of p.lines) {
    html += `<tr><td>${l.equipment}</td><td class="num">${l.factories}</td>` +
      `<td class="num">${(l.efficiency * 100).toFixed(1)}%${l.efficiency < l.efficiency_cap ? '' : ' (max)'}</td>` +
      `<td class="num">${l.output_total.toFixed(0)}</td>` +
      `<td class="num ${l.resource_shortage > 0.01 ? 'bad' : 'good'}">` +
      `${(100 - l.resource_shortage * 100).toFixed(0)}%</td>` +
      `<td><button data-remove="${l.equipment}">×</button></td></tr>`;
  }
  html += '</table>';
  const stock = Object.entries(p.stockpile || {})
    .map(([k, v]) => `${k}: ${Math.round(v)}`).join(' · ');
  html += `<div class="section small" style="margin-top:8px">stockpile — ${stock || 'empty'}</div>`;
  el.innerHTML = html;

  el.querySelector('#add-line').addEventListener('click', async () => {
    const equipment = el.querySelector('#line-equipment').value;
    const factories = Number(el.querySelector('#line-factories').value);
    if (await sendCommand({ type: 'set_production_line', equipment, factories })) refresh();
  });
  for (const b of el.querySelectorAll('button[data-remove]')) {
    b.addEventListener('click', async () => {
      if (await sendCommand({ type: 'remove_production_line', equipment: b.dataset.remove })) refresh();
    });
  }
}

function constructionPanel() {
  const p = App.snap.player;
  const el = document.getElementById('panel-construction');
  const kinds = ['civilian_factory', 'military_factory', 'dockyard', 'infrastructure', 'railway',
                 'supply_hub', 'air_base', 'naval_base', 'radar', 'fort', 'anti_air',
                 'synthetic_refinery'];
  const mine = (App.snap.player.states || []).slice().sort((a, b) => a.name.localeCompare(b.name));
  let html = `<div class="section"><div class="row-actions">
    <select id="build-kind">${kinds.map((k) => `<option>${k}</option>`).join('')}</select>
    <select id="build-state">${mine.map((s) => `<option value="${s.id}">${s.name} (civ ${s.civ}, mil ${s.mil}, slots ${s.slots})</option>`).join('')}</select>
    <button id="start-build">Queue</button></div></div>`;
  html += '<table><tr><th>Project</th><th class="num">Progress</th><th></th></tr>';
  for (const c of p.construction) {
    const pct = c.cost > 0 ? Math.min(100, (c.progress / c.cost) * 100) : 0;
    html += `<tr><td>${c.kind} (lvl ${c.level})</td>` +
      `<td class="num"><div class="bar"><span style="width:${pct.toFixed(1)}%"></span></div>` +
      `${pct.toFixed(0)}%</td>` +
      `<td><button data-cancel="${c.kind}:${c.state}:${c.province}">×</button></td></tr>`;
  }
  html += '</table>';
  el.innerHTML = html;
  el.querySelector('#start-build').addEventListener('click', async () => {
    const kindName = el.querySelector('#build-kind').value;
    const state = Number(el.querySelector('#build-state').value);
    const kinds2 = { civilian_factory: 0, military_factory: 1, dockyard: 2, infrastructure: 3,
                     railway: 4, supply_hub: 5, air_base: 6, naval_base: 7, radar: 8, fort: 9,
                     anti_air: 10, synthetic_refinery: 11 };
    const payload = { type: 'start_construction', kind: kinds2[kindName], state, province: stateProvince(state) };
    if (await sendCommand(payload)) refresh();
  });
  for (const b of el.querySelectorAll('button[data-cancel]')) {
    b.addEventListener('click', async () => {
      const [kind, state, province] = b.dataset.cancel.split(':').map(Number);
      const kinds2 = { civilian_factory: 0, military_factory: 1, dockyard: 2, infrastructure: 3,
                       railway: 4, supply_hub: 5, air_base: 6, naval_base: 7, radar: 8, fort: 9,
                       synthetic_refinery: 11 };
      if (await sendCommand({ type: 'cancel_construction', kind, state, province })) refresh();
    });
  }
}

function stateProvince(stateId) {
  for (const p of App.map.provinces) if (p.state === stateId && !p.sea) return p.id;
  return -1;
}

function researchPanel() {
  const p = App.snap.player;
  const el = document.getElementById('panel-research');
  let html = '<h3>Slots</h3><table>';
  p.research.forEach((s, i) => {
    if (s.active && s.tech) {
      const pct = s.cost > 0 ? Math.min(100, (s.progress / s.cost) * 100) : 0;
      html += `<tr><td>${s.name || s.tech}</td><td class="num"><div class="bar">` +
        `<span style="width:${pct.toFixed(1)}%"></span></div>${pct.toFixed(0)}%</td>` +
        `<td><button data-cancel="${s.tech}">×</button></td></tr>`;
    } else {
      html += `<tr><td class="dim">idle slot ${i + 1}</td><td></td><td></td></tr>`;
    }
  });
  html += '</table><h3>Available</h3><table>';
  for (const t of p.available_techs.slice(0, 60)) {
    html += `<tr><td>${t.name} <span class="dim">${t.category}</span></td>` +
      `<td class="num dim">${t.days.toFixed(0)}d</td>` +
      `<td><button data-tech="${t.key}">Start</button></td></tr>`;
  }
  html += '</table><h3>Completed</h3><div class="small">' +
    (p.completed_techs.join(', ') || 'none') + '</div>';
  el.innerHTML = html;
  for (const b of el.querySelectorAll('button[data-tech]')) {
    b.addEventListener('click', async () => {
      if (await sendCommand({ type: 'start_research', tech: b.dataset.tech })) refresh();
    });
  }
  for (const b of el.querySelectorAll('button[data-cancel]')) {
    b.addEventListener('click', async () => {
      if (await sendCommand({ type: 'cancel_research', tech: b.dataset.cancel })) refresh();
    });
  }
}

function militaryPanel() {
  const p = App.snap.player;
  const el = document.getElementById('panel-military');
  let html = '<h3>Templates</h3><table><tr><th>Name</th><th class="num">W</th><th class="num">Org</th>' +
             '<th class="num">Soft</th><th class="num">Def</th><th class="num">Manpower</th><th></th></tr>';
  for (const t of p.templates) {
    html += `<tr><td>${t.name}</td><td class="num">${t.width}</td><td class="num">${t.org.toFixed(0)}</td>` +
      `<td class="num">${t.soft.toFixed(0)}</td><td class="num">${t.defense.toFixed(0)}</td>` +
      `<td class="num">${Math.round(t.manpower)}</td>` +
      `<td><button data-recruit="${t.key}">Train</button></td></tr>`;
  }
  html += '</table>';

  html += '<h3>Training</h3><div class="list">';
  if (p.training.length === 0) html += '<div class="small">none</div>';
  for (const t of p.training) {
    html += `<div class="row" data-train="${t.division}"><span>${t.template} #${t.division}</span>` +
      `<span class="num">${t.ready ? '<span class="good">ready — click, then click a province</span>' : t.days_left.toFixed(1) + 'd'}` +
      ` · ${(t.strength * 100).toFixed(0)}%</span></div>`;
  }
  html += '</div>';

  html += '<h3>Armies</h3>';
  html += `<div class="row-actions"><input id="army-name" placeholder="army name" />` +
          `<button id="create-army">Create</button></div>`;
  for (const a of p.armies) {
    const general = a.general >= 0 ? (p.generals.find((g) => g.id === a.general) || {}).name : 'no general';
    html += `<div class="section"><b>${a.name}</b> <span class="pill">${a.order}</span> ` +
      `<span class="dim">${general} · ${a.divisions.length} div · motorisation ${a.motorization}</span><br>` +
      `<div class="row-actions">` +
      `<button data-army-order="${a.id}:1">Front</button>` +
      `<button data-army-order="${a.id}:2">Offensive</button>` +
      `<button data-army-order="${a.id}:3">Fallback</button>` +
      `<button data-army-order="${a.id}:4">Garrison</button>` +
      `<button data-army-stance="${a.id}:0">Aggressive</button>` +
      `<button data-army-stance="${a.id}:1">Defensive</button>` +
      `<button data-army-motor="${a.id}">Motorise</button>` +
      `</div></div>`;
  }

  html += '<h3>Divisions</h3><div class="list">';
  for (const d of App.snap.divisions) {
    if (d.country !== p.id) continue;
    const prov = provinceById(d.province);
    html += `<div class="row${d.id === App.selectedDivision ? ' selected' : ''}" data-div="${d.id}">` +
      `<span>${d.name} <span class="dim">${d.training ? 'training' : (prov ? prov.name : '—')}</span></span>` +
      `<span class="num">${d.org.toFixed(0)}/${d.max_org.toFixed(0)} · ${(d.strength * 100).toFixed(0)}%` +
      `${d.battle >= 0 ? ' ⚔' : ''}</span></div>`;
  }
  html += '</div>';
  el.innerHTML = html;

  for (const b of el.querySelectorAll('button[data-recruit]')) {
    b.addEventListener('click', async () => {
      if (await sendCommand({ type: 'recruit_division', template: b.dataset.recruit, count: 1 })) refresh();
    });
  }
  el.querySelector('#create-army').addEventListener('click', async () => {
    const name = el.querySelector('#army-name').value || 'New Army';
    if (await sendCommand({ type: 'create_army', name })) refresh();
  });
  for (const b of el.querySelectorAll('button[data-army-order]')) {
    b.addEventListener('click', async () => {
      const [army, kind] = b.dataset.armyOrder.split(':').map(Number);
      if (await sendCommand({ type: 'set_division_order', army, order_kind: kind })) refresh();
    });
  }
  for (const b of el.querySelectorAll('button[data-army-stance]')) {
    b.addEventListener('click', async () => {
      const [army, stance] = b.dataset.armyStance.split(':').map(Number);
      if (await sendCommand({ type: 'set_stance', army, stance })) refresh();
    });
  }
  for (const b of el.querySelectorAll('button[data-army-motor]')) {
    b.addEventListener('click', async () => {
      const army = Number(b.dataset.armyMotor);
      if (await sendCommand({ type: 'motorize_supply', army, motorization: 3 })) refresh();
    });
  }
  for (const row of el.querySelectorAll('div[data-train]')) {
    row.addEventListener('click', () => {
      App.selectedDivision = Number(row.dataset.train);
      toast('division selected — click a province to deploy it');
      renderLeft();
    });
  }
  for (const row of el.querySelectorAll('div[data-div]')) {
    row.addEventListener('click', () => {
      const id = Number(row.dataset.div);
      const div = App.snap.divisions.find((d) => d.id === id);
      const armies = p.armies;
      if (armies.length > 0) {
        sendCommand({ type: 'assign_division_to_army', army: armies[0].id, division_ids: [id] })
          .then(() => refresh());
      } else {
        App.selectedDivision = id;
        toast('division selected — click a target province');
      }
    });
  }
}

function diplomacyPanel() {
  const p = App.snap.player;
  const el = document.getElementById('panel-diplomacy');
  let html = '<h3>Wars</h3><table><tr><th>Attackers</th><th>Defenders</th></tr>';
  for (const w of App.snap.wars) {
    html += `<tr><td>${w.attackers.map(tagOf).join(', ')}</td>` +
            `<td>${w.defenders.map(tagOf).join(', ')}</td></tr>`;
  }
  if (App.snap.wars.length === 0) html += '<tr><td colspan="2" class="dim">at peace</td></tr>';
  html += '</table><h3>Countries</h3><table><tr><th>Tag</th><th>Name</th><th class="num">States</th>' +
          '<th class="num">Factories</th><th class="num">Divisions</th><th></th></tr>';
  const myStates = new Set();
  for (const pr of App.map.provinces) if (App.snap.controller[pr.id] === p.id) myStates.add(pr.state);
  for (const c of App.snap.countries) {
    if (!c.alive) continue;
    const atWar = App.snap.wars.some((w) =>
      (w.attackers.includes(p.id) && w.defenders.includes(c.id)) ||
      (w.defenders.includes(p.id) && w.attackers.includes(c.id)));
    html += `<tr><td>${c.tag}</td><td>${c.name}</td><td class="num">${c.states}</td>` +
      `<td class="num">${c.civ + c.mil}</td><td class="num">${c.divisions}</td>` +
      `<td>${c.id === p.id ? '<span class="dim">you</span>' :
        (atWar ? '<span class="bad">war</span>' :
         `<button data-war="${c.id}">Declare war</button>`)}</td></tr>`;
  }
  html += '</table>';
  html += '<h3>Factions</h3><table>';
  const factions = App.snap.factions || [];
  if (factions.length === 0) html += '<tr><td class="dim">none</td></tr>';
  const me = App.snap.countries.find((c) => c.id === p.id);
  for (const f of factions) {
    const inIt = f.members.includes(p.tag);
    html += `<tr><td>${f.name}</td><td class="dim">${f.leader} + ${f.members.filter((m) => m !== f.leader).join(', ') || 'no members'}</td>` +
      `<td>${inIt ? '<span class="good">member</span>' :
        `<button data-join-faction="${f.leader_id}">Join</button>`}</td></tr>`;
  }
  html += '</table>';
  html += '<h3>Laws</h3><table>';
  for (const l of p.laws) {
    html += `<tr><td>${l.name}</td><td class="num dim">${l.cost.toFixed(0)} pp</td>` +
      `<td>${l.enacted ? '<span class="good">enacted</span>' :
        `<button data-law="${l.key}">Enact</button>`}</td></tr>`;
  }
  html += '</table>';
  el.innerHTML = html;
  for (const b of el.querySelectorAll('button[data-war]')) {
    b.addEventListener('click', async () => {
      if (await sendCommand({ type: 'declare_war', target_country: Number(b.dataset.war) })) refresh();
    });
  }
  for (const b of el.querySelectorAll('button[data-law]')) {
    b.addEventListener('click', async () => {
      if (await sendCommand({ type: 'set_law', law: b.dataset.law })) refresh();
    });
  }
  for (const b of el.querySelectorAll('button[data-join-faction]')) {
    b.addEventListener('click', async () => {
      if (await sendCommand({ type: 'join_faction', target_country: Number(b.dataset.joinFaction) })) refresh();
    });
  }
}

function logPanel() {
  const el = document.getElementById('panel-log');
  const events = (App.snap.events || []).slice(-80).reverse();
  el.innerHTML = events.map((e) =>
    `<div class="small">[${e.tick}] <b>${e.kind}</b> ${e.text}</div>`).join('') || '<div class="small">no events</div>';
}

function airPanel() {
  const p = App.snap.player;
  const el = document.getElementById('panel-air');
  const wings = (App.snap.wings || []).filter((w) => w.country === p.id);
  const regions = (App.snap.air_regions || []);
  const regionName = (id) => {
    const r = regions.find((x) => x.id === id);
    return r ? r.name : `region ${id}`;
  };

  // Air bases the player controls, for the "form wing" form.
  const bases = [];
  for (const prov of App.map.provinces) {
    if (prov.sea || !prov.air_base) continue;
    if (App.snap.controller[prov.id] !== p.id) continue;
    bases.push(prov);
  }
  const fighters = (App.snap.equipment_defs || []).filter((e) => e.category === 'aircraft');
  const stock = p.stockpile || {};

  let html = '<div class="section"><b>Form a wing</b><div class="row-actions">' +
    `<select id="wing-equipment">${fighters.map((f) =>
      `<option value="${f.key}">${f.name} (stock ${Math.round(stock[f.key] || 0)})</option>`).join('')}</select>` +
    `<select id="wing-base">${bases.map((b) =>
      `<option value="${b.id}">${b.name} (base ${b.air_base})</option>`).join('')}</select>` +
    '<input id="wing-size" type="number" min="10" max="1000" value="100" style="width:70px" />' +
    '<button id="create-wing">Form</button></div>' +
    '<div class="small dim">a province needs an air base and free capacity; aircraft come from the stockpile</div></div>';

  html += '<h3>Wings</h3><table>' +
    '<tr><th>Wing</th><th>Model</th><th class="num">Planes</th><th class="num">Eff</th>' +
    '<th class="num">Losses</th><th>Base / region</th><th>Mission</th><th></th></tr>';
  for (const w of wings) {
    const missions = ['none', 'air_superiority', 'interception', 'close_air_support',
                      'strategic_bombing', 'logistics_strike', 'reconnaissance'];
    const options = missions.map((m, i) =>
      `<option value="${i}"${i === w.mission_id ? ' selected' : ''}>${m}</option>`).join('');
    html += `<tr><td>${w.name}</td><td>${w.equipment}</td>` +
      `<td class="num">${w.planes}/${w.max_planes}</td>` +
      `<td class="num">${(w.efficiency * 100).toFixed(0)}%</td>` +
      `<td class="num">${w.losses}</td>` +
      `<td>${w.base_name}<br><span class="dim">${w.region_name}</span></td>` +
      `<td><select data-mission="${w.id}">${options}</select></td>` +
      `<td><button data-disband="${w.id}">×</button></td></tr>`;
  }
  if (wings.length === 0) html += '<tr><td colspan="8" class="dim">no wings</td></tr>';
  html += '</table>';

  html += '<h3>Contested regions</h3><table><tr><th>Region</th><th>Air control</th></tr>';
  let contested = 0;
  for (const r of regions) {
    if (!r.control || r.control.length === 0) continue;
    ++contested;
    const bars = r.control.map((c) =>
      `${c.tag} ${(c.share * 100).toFixed(0)}%`).join(' · ');
    html += `<tr><td>${r.name}</td><td>${bars}</td></tr>`;
  }
  if (contested === 0) html += '<tr><td colspan="2" class="dim">no air activity</td></tr>';
  html += '</table>';

  el.innerHTML = html;

  el.querySelector('#create-wing').addEventListener('click', async () => {
    const equipment = el.querySelector('#wing-equipment').value;
    const province = Number(el.querySelector('#wing-base').value);
    const value = Number(el.querySelector('#wing-size').value);
    if (await sendCommand({ type: 'create_air_wing', equipment, province, value })) refresh();
  });
  for (const sel of el.querySelectorAll('select[data-mission]')) {
    sel.addEventListener('change', async () => {
      const wing = Number(sel.dataset.mission);
      const w = (App.snap.wings || []).find((x) => x.id === wing);
      const region = w ? w.region : 0;
      if (await sendCommand({ type: 'set_air_mission', wing, region, value: Number(sel.value) })) refresh();
    });
  }
  for (const b of el.querySelectorAll('button[data-disband]')) {
    b.addEventListener('click', async () => {
      if (await sendCommand({ type: 'disband_air_wing', wing: Number(b.dataset.disband) })) refresh();
    });
  }
}

function renderPanels() {
  if (!App.snap || !App.snap.player) return;
  if (App.tab === 'production') productionPanel();
  if (App.tab === 'construction') constructionPanel();
  if (App.tab === 'research') researchPanel();
  if (App.tab === 'military') militaryPanel();
  if (App.tab === 'air') airPanel();
  if (App.tab === 'diplomacy') diplomacyPanel();
  if (App.tab === 'log') logPanel();
}

// ------------------------------------------------------------------ loop ----

let lastPanelRender = 0;

async function refresh() {
  const snap = await api('/api/state');
  if (!snap || !snap.tick) return;
  const before = App.snap ? App.snap.tick : -1;
  App.snap = snap;
  App.airControl = new Map();
  for (const r of (snap.air_regions || [])) {
    const m = new Map();
    for (const entry of (r.control || [])) m.set(entry.country, entry.share);
    App.airControl.set(r.id, m);
  }
  renderTop();
  renderLeft();
  const now = Date.now();
  if (now - lastPanelRender > 900 || App.snap.tick !== before) {
    lastPanelRender = now;
    renderPanels();
  }
}

function centerOn(worldX, worldY) {
  const w = canvas.width / devicePixelRatio;
  const h = canvas.height / devicePixelRatio;
  App.cam.x = Math.max(0, worldX - w / (2 * App.cam.zoom));
  App.cam.y = Math.max(0, worldY - h / (2 * App.cam.zoom));
}

function fitMap() {
  if (!App.map || App.map.provinces.length === 0) return;
  let maxX = 1, maxY = 1;
  for (const p of App.map.provinces) {
    if (p.x + 1 > maxX) maxX = p.x + 1;
    if (p.y + 1 > maxY) maxY = p.y + 1;
  }
  const w = canvas.width / devicePixelRatio;
  const h = canvas.height / devicePixelRatio;
  App.cam.zoom = Math.max(2, Math.min(20, Math.min(w / maxX, h / maxY) * 0.92));
}

async function init() {
  resize();
  const map = await api('/api/map');
  if (map && map.provinces) {
    App.map = map;
    App.byId = new Map();
    App.byCoord = new Map();
    for (const p of map.provinces) {
      App.byId.set(p.id, p);
      App.byCoord.set(`${p.x},${p.y}`, p);
    }
  }
  const meta = await api('/api/meta');
  if (meta) {
    App.meta.paused = meta.paused;
    App.meta.speed = meta.speed;
    renderSpeedButtons();
  }
  await refresh();
  fitMap();
  const cap = App.snap && App.snap.player && App.snap.player.capital;
  const prov = cap ? provinceById(cap) : null;
  if (prov) centerOn(prov.x + 0.5, prov.y + 0.5);
  requestAnimationFrame(function frame() {
    draw();
    requestAnimationFrame(frame);
  });
  setInterval(refresh, 700);
  window.addEventListener('resize', () => { resize(); });
}

init();
