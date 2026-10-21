/**
 * IR Database Search & Decoder Module
 * Comprehensive client-side index search, Pronto hex decoder, and
 * full official Logitech Harmony IR Archive integration (276,236 devices).
 */
import harmonyManufacturers from './harmony_manufacturers.json';

export const HARMONY_ARCHIVE_CDN = 'https://cdn.jsdelivr.net/gh/pickysysadmin/logitech-harmony-ir-archive@main/';
export const HARMONY_ARCHIVE_RAW = 'https://raw.githubusercontent.com/pickysysadmin/logitech-harmony-ir-archive/main/';
export const HARMONY_ARCHIVE_FORK = 'https://raw.githubusercontent.com/iranl/logitech-harmony-ir-archive/main/';

export const IRDB_BASE = 'https://cdn.jsdelivr.net/gh/probonopd/irdb@master/codes/';
export const FLIPPER_BASE = 'https://cdn.jsdelivr.net/gh/Lucaslhm/Flipper-IRDB@main/';
export const FLIPPER_INDEX = 'https://api.github.com/repos/Lucaslhm/Flipper-IRDB/git/trees/main?recursive=1';
export const LIRC_BASE = 'https://raw.githubusercontent.com/probonopd/lirc-remotes/master/';
export const LIRC_INDEX = 'https://api.github.com/repos/probonopd/lirc-remotes/git/trees/master?recursive=1';
export const SMARTIR_BASE = 'https://raw.githubusercontent.com/smartHomeHub/SmartIR/master/';
export const SMARTIR_INDEX = 'https://api.github.com/repos/smartHomeHub/SmartIR/git/trees/master?recursive=1';

export const irdbCache = { irdb: null, flipper: null, lirc: null, smartir: null, harmonyModels: {} };
export let irdbIndex = [];

export function getHarmonyManufacturers() {
  return harmonyManufacturers || [];
}

export function searchHarmonyManufacturers(q, limit = 50) {
  if (!q) return harmonyManufacturers.slice(0, limit);
  const clean = String(q).trim().toLowerCase();
  const startsWith = [];
  const contains = [];
  for (const m of harmonyManufacturers) {
    const low = m.toLowerCase();
    if (low.startsWith(clean)) startsWith.push(m);
    else if (low.includes(clean)) contains.push(m);
    if (startsWith.length >= limit) break;
  }
  return startsWith.concat(contains).slice(0, limit);
}

export async function fetchHarmonyModels(mfg) {
  if (!mfg) return [];
  if (irdbCache.harmonyModels[mfg]) return irdbCache.harmonyModels[mfg];
  const url = `${HARMONY_ARCHIVE_CDN}devices/${encodeURIComponent(mfg)}/index.json`;
  try {
    const r = await fetch(url);
    if (!r.ok) {
      const r2 = await fetch(`${HARMONY_ARCHIVE_RAW}devices/${encodeURIComponent(mfg)}/index.json`);
      if (!r2.ok) throw new Error(`HTTP ${r.status}`);
      const j2 = await r2.json();
      irdbCache.harmonyModels[mfg] = j2;
      return j2;
    }
    const j = await r.json();
    irdbCache.harmonyModels[mfg] = j;
    return j;
  } catch (err) {
    console.error(`Failed to fetch models for ${mfg}:`, err);
    throw err;
  }
}

export function searchHarmonyModels(models, q, limit = 50) {
  if (!models || !models.length) return [];
  if (!q) return models.slice(0, limit);
  const clean = String(q).trim().toLowerCase();
  const tokens = clean.split(/\s+/).filter(Boolean);
  return models.filter(item => {
    const m = (item.m || '').toLowerCase();
    return tokens.every(t => m.includes(t));
  }).slice(0, limit);
}

export async function fetchHarmonyDeviceDetail(mfg, filename) {
  const url = `${HARMONY_ARCHIVE_CDN}devices/${encodeURIComponent(mfg)}/${encodeURIComponent(filename)}`;
  let r = await fetch(url);
  if (!r.ok) {
    r = await fetch(`${HARMONY_ARCHIVE_RAW}devices/${encodeURIComponent(mfg)}/${encodeURIComponent(filename)}`);
  }
  if (!r.ok) throw new Error(`Failed to load device details: HTTP ${r.status}`);
  return r.json();
}

export async function fetchHarmonyCodeset(codesetPath) {
  if (!codesetPath) throw new Error('No codeset specified');
  const cleanPath = codesetPath.replace(/^\//, '');
  const url = `${HARMONY_ARCHIVE_CDN}${cleanPath}`;
  let r = await fetch(url);
  if (!r.ok) {
    r = await fetch(`${HARMONY_ARCHIVE_RAW}${cleanPath}`);
  }
  if (!r.ok) throw new Error(`Failed to load codeset: HTTP ${r.status}`);
  return r.json();
}

export function mapHarmonyDeviceType(typeId) {
  const map = {
    1: 'Television',
    2: 'VCR',
    3: 'CD Player',
    4: 'DVD Player',
    5: 'Laserdisc',
    6: 'Tape Deck',
    7: 'Tuner',
    8: 'MiniDisc',
    9: 'DAT',
    10: 'Satellite',
    11: 'Cable',
    12: 'PVR',
    13: 'Game Console',
    14: 'Media Player',
    15: 'Audio Receiver',
    16: 'Amplifier',
    17: 'Projector',
    18: 'Home Automation',
    19: 'Lighting',
    20: 'Climate',
    21: 'Audio Switcher',
    22: 'Video Switcher'
  };
  return map[typeId] || 'Generic';
}

export function buildCodexDeviceFromHarmony(deviceDetail, codesetData, customName) {
  const mfg = deviceDetail.manufacturer || 'Generic';
  const model = deviceDetail.model || 'Device';
  const timing = deviceDetail.timing || {};
  const devId = 'd_' + (deviceDetail.globalDeviceId || Math.floor(Date.now() / 1000));

  const commands = (codesetData.commands || []).map((cmd, idx) => ({
    id: 'c_' + (idx + 1),
    name: safeImportName(cmd.name),
    raw: cmd.pronto || '',
    keycode: cmd.keycode || '',
    has_raw: cmd.pronto ? 1 : 0
  }));

  return {
    id: devId,
    name: customName || `${mfg} ${model}`,
    manufacturer: mfg,
    model: model,
    type: mapHarmonyDeviceType(deviceDetail.deviceType),
    controlPort: 7, // All IR ports
    power_on_delay: timing.powerOnDelay || 1500,
    inter_device_delay: timing.interDeviceDelay || 500,
    power: deviceDetail.power || null,
    inputs: deviceDetail.inputs || null,
    commands: commands
  };
}

export function sourceLabel(s) {
  return ({
    harmony: 'Logitech Harmony Official Archive (276k devices)',
    irdb: 'probonopd/irdb CSV',
    flipper: 'Flipper-IRDB',
    lirc: 'LIRC remotes',
    smartir: 'SmartIR JSON',
    remotecentral: 'RemoteCentral',
    custom: 'Pasted file or URL',
    stored: 'Saved device'
  }[s] || s || 'Unknown source');
}

export function safeImportName(s) {
  s = String(s || 'Command').replace(/[|"\\\r\n]/g, ' ').replace(/\s+/g, ' ').trim().slice(0, 96);
  return s || 'Command';
}

export function decodeHtml(s) {
  const e = document.createElement('textarea');
  e.innerHTML = String(s || '');
  return e.value;
}

export function normText(s) {
  return String(s || '').toLowerCase().replace(/[^a-z0-9]+/g, ' ').trim();
}

export function queryTokens(s) {
  return normText(s).split(/\s+/).filter(Boolean);
}

export function rev8(v) {
  v = ((v & 240) >> 4) | ((v & 15) << 4);
  v = ((v & 204) >> 2) | ((v & 51) << 2);
  v = ((v & 170) >> 1) | ((v & 85) << 1);
  return v & 255;
}

export function keyFromParts(proto, d, s, f) {
  if (!/^(NEC|Samsung32|Pioneer)/i.test(proto)) return '';
  d = Number(d);
  const ss = String(s === undefined ? '' : s).trim();
  s = (ss === '' || ss === '-1') ? (d ^ 255) : Number(s);
  f = Number(f);
  if ([d, s, f].some(x => !Number.isFinite(x) || x < 0 || x > 255)) return '';
  if (/^Samsung32/i.test(proto)) s = d;
  const val = ((rev8(d) << 24) | (rev8(s) << 16) | (rev8(f) << 8) | rev8((~f) & 255)) >>> 0;
  return 'G:Toshiba 32 Bit:(0x' + val.toString(16).toUpperCase().padStart(8, '0') + ')(Repeat)():3';
}

export function harmonyRawFromTimings(freq, vals) {
  freq = Math.max(10000, Math.min(60000, Math.round(Number(freq) || 38000)));
  vals = (vals || []).map(v => Math.max(1, Math.min(0xfffff, Math.round(Math.abs(Number(v) || 0))))).filter(Boolean);
  if (vals.length === 3) vals.push(100000);
  if (vals.length < 4) return '';
  let raw = 'F' + freq.toString(16).toUpperCase();
  vals.forEach((v, i) => {
    raw += (i % 2 ? 'S' : 'P') + v.toString(16).toUpperCase();
  });
  return raw.length <= 4096 ? raw : '';
}

export function prontoToHarmonyRaw(hex) {
  const words = String(hex || '').trim().split(/\s+/).filter(Boolean).map(x => parseInt(x, 16));
  if (words.length < 8 || words.some(x => !Number.isFinite(x))) return '';
  if (words[0] !== 0) return '';
  const unit = (words[1] || 1) * 0.241246;
  const freq = Math.round(1000000 / unit);
  const pairs = (words[2] || 0) + (words[3] || 0);
  const vals = words.slice(4, 4 + pairs * 2).map(w => Math.round(w * unit));
  return harmonyRawFromTimings(freq, vals);
}

function hexBytes(v) {
  return String(v || '').trim().split(/\s+/).filter(Boolean).map(x => parseInt(x, 16) || 0);
}

function csvCells(line) {
  const out = [];
  let cur = '', q = false;
  for (let i = 0; i < line.length; i++) {
    const ch = line[i];
    if (ch === '"') {
      if (q && line[i + 1] === '"') {
        cur += '"';
        i++;
      } else q = !q;
    } else if (ch === ',' && !q) {
      out.push(cur);
      cur = '';
    } else cur += ch;
  }
  out.push(cur);
  return out.map(x => x.trim());
}

export function flipperEntries(t, path = '') {
  const out = [];
  let cur = {};
  function push() {
    if (!cur.name) { cur = {}; return; }
    let key = '', raw = '', meta = (cur.protocol || cur.type || 'raw');
    if (String(cur.type || '').toLowerCase() === 'parsed') {
      const a = hexBytes(cur.address), c = hexBytes(cur.command), p = cur.protocol || '';
      if (/^Samsung32/i.test(p)) key = keyFromParts(p, a[0], a[0], c[0]);
      else if (/^NEC/i.test(p)) key = keyFromParts(p, a[0], (a[0] ^ 255), c[0]);
      else if (/^Pioneer/i.test(p)) key = keyFromParts(p, a[0], (a[0] ^ 255), c[0]);
    } else if (String(cur.type || '').toLowerCase() === 'raw') {
      const vals = String(cur.data || '').trim().split(/\s+/).filter(Boolean).map(Number);
      raw = harmonyRawFromTimings(cur.frequency || 38000, vals);
      meta = 'raw timings ' + (cur.frequency || 38000) + ' Hz';
    }
    out.push({ name: cur.name, meta, keycode: key, raw });
    cur = {};
  }
  t.replace(/\r/g, '').split('\n').forEach(line => {
    line = line.trim();
    if (!line) return;
    if (line[0] === '#') { push(); return; }
    const i = line.indexOf(':');
    if (i < 0) return;
    const k = line.slice(0, i).trim().toLowerCase(), v = line.slice(i + 1).trim();
    if (k === 'name' && cur.name) push();
    cur[k] = cur[k] && k === 'data' ? cur[k] + ' ' + v : v;
  });
  push();
  return out;
}

export function lircEntries(t) {
  const out = [];
  const lines = t.replace(/\r/g, '').split('\n');
  let inCodes = false;
  for (let line of lines) {
    line = line.trim();
    if (/^begin\s+codes/i.test(line)) {
      inCodes = true;
      continue;
    }
    if (/^end\s+codes/i.test(line)) {
      inCodes = false;
      continue;
    }
    if (inCodes && line && !line.startsWith('#')) {
      const parts = line.split(/\s+/);
      if (parts.length >= 2) {
        const name = parts[0];
        const hex = parts[1];
        out.push({ name: safeImportName(name), meta: 'LIRC code', keycode: hex, raw: '' });
      }
    }
  }
  return out;
}

export function parseIrText(t, source, path) {
  t = String(t || '');
  let rows = [];
  const low = String(path || '').toLowerCase(), hint = String(source || '').toLowerCase();
  if (hint === 'flipper' || /\.ir$/.test(low) || /^Filetype:\s*IR/i.test(t)) {
    rows = rows.concat(flipperEntries(t, path));
  }
  if (hint === 'irdb' || /\.csv$/.test(low)) {
    rows = rows.concat(csvEntries(t));
  }
  if (hint === 'lirc' || /\.conf$/i.test(low) || /begin\s+remote/i.test(t)) {
    rows = rows.concat(lircEntries(t));
  }
  if (hint === 'smartir' || /\.json$/.test(low) || /^\s*[[{]/.test(t)) {
    try {
      const j = JSON.parse(t);
      const cmds = j.commands || j;
      if (typeof cmds === 'object') {
        Object.entries(cmds).forEach(([k, v]) => {
          if (typeof v === 'string') {
            const raw = prontoToHarmonyRaw(v);
            rows.push({ name: k, meta: raw ? 'Pronto timing' : 'Code', keycode: raw ? '' : v, raw: raw || '' });
          }
        });
      }
    } catch {}
  }
  if (!rows.length) {
    const pronto = /((?:0000|0100|5000|6000|7000)(?:\s+[0-9a-fA-F]{4}){6,})/g;
    t.replace(/\r/g, '').split('\n').forEach((line, i) => {
      let m;
      while ((m = pronto.exec(line))) {
        const hex = m[1].replace(/\s+/g, ' ').trim();
        const raw = prontoToHarmonyRaw(hex);
        rows.push({ name: safeImportName(line.slice(0, m.index).trim() || `Command ${i + 1}`), meta: 'Pronto converted', raw });
      }
    });
  }
  return dedupeCommandRows(rows);
}

function csvEntries(t) {
  return t.replace(/\r/g, '').split('\n').map(x => x.trim()).filter(Boolean).map(line => {
    const p = csvCells(line), proto = p[1] || '', key = p[0] === 'functionname' ? '' : keyFromParts(proto, p[2] || '', p[3] || '', p[4] || '');
    return { name: p[0] || '', meta: proto, keycode: key, raw: '' };
  }).filter(r => r.name && r.name !== 'functionname');
}

function dedupeCommandRows(rows) {
  const seen = new Set(), names = new Set(), out = [];
  (rows || []).forEach(r => {
    if (!r || !r.name) return;
    const fp = (r.raw ? 'raw:' + r.raw : 'key:' + r.keycode).toLowerCase();
    if (seen.has(fp)) return;
    seen.add(fp);
    let base = safeImportName(r.name), name = base, n = 2;
    while (names.has(name.toLowerCase())) name = `${base} ${n++}`;
    names.add(name.toLowerCase());
    out.push({ ...r, name });
  });
  return out;
}

export async function loadIrdSource(src) {
  if (irdbCache[src]) return irdbCache[src];
  if (src === 'flipper') {
    const r = await fetch(FLIPPER_INDEX);
    const j = await r.json();
    const files = j.tree || j.files || [];
    irdbCache[src] = files.filter(x => !x.type || x.type === 'blob').map(x => (x.path || x.name || x).replace(/^\//, '')).filter(x => x.endsWith('.ir')).map(p => ({ source: src, path: p }));
  } else if (src === 'irdb') {
    const r = await fetch(IRDB_BASE + 'index');
    const t = await r.text();
    irdbCache[src] = t.replace(/\r/g, '').split('\n').map(x => x.trim()).filter(x => x.endsWith('.csv')).map(p => ({ source: src, path: p }));
  } else if (src === 'lirc') {
    try {
      const r = await fetch(LIRC_INDEX);
      const j = await r.json();
      const files = j.tree || j.files || [];
      irdbCache[src] = files.filter(x => !x.type || x.type === 'blob').map(x => (x.path || x.name || x).replace(/^\//, '')).filter(x => x.endsWith('.conf') || x.endsWith('.lircd.conf')).map(p => ({ source: src, path: p }));
    } catch {
      irdbCache[src] = [];
    }
  } else if (src === 'smartir') {
    try {
      const r = await fetch(SMARTIR_INDEX);
      const j = await r.json();
      const files = j.tree || j.files || [];
      irdbCache[src] = files.filter(x => !x.type || x.type === 'blob').map(x => (x.path || x.name || x).replace(/^\//, '')).filter(x => x.endsWith('.json') && x.includes('codes/')).map(p => ({ source: src, path: p }));
    } catch {
      irdbCache[src] = [];
    }
  } else {
    irdbCache[src] = [];
  }
  return irdbCache[src];
}

export function rankIrdEntries(entries, q) {
  if (!q) return entries.slice(0, 100);
  const toks = queryTokens(q);
  return entries.filter(e => {
    const p = normText(e.path || '');
    return toks.every(t => p.includes(t));
  }).slice(0, 100);
}

export async function fetchIrdFile(path, source) {
  let url = path;
  if (source === 'flipper') url = FLIPPER_BASE + path;
  else if (source === 'irdb') url = IRDB_BASE + path;
  else if (source === 'lirc') url = LIRC_BASE + path;
  else if (source === 'smartir') url = SMARTIR_BASE + path;
  const res = await fetch(url);
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return res.text();
}
