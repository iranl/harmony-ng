// Browser side of the WebAssembly remote renderer: loads files into the module's filesystem
// and draws screens into a 240x320 canvas. The Hub's web UIs import it from their assets
// with loadHubRenderer(), writeRemoteConfig() and render().
const ROOT = '/r';
export const STOCK_ASSET_DIR = '/opt/luadraws/appscommon/asset';

// The query this module was imported with (a build's version), passed on to the module and
// its WebAssembly so a new build isn't served from a cache that holds the last one
const VERSION = new URL(import.meta.url).search;

export async function loadRenderer() {
  const { default: createRemoteRenderer } = await import(`./remote_render.js${VERSION}`);
  return createRemoteRenderer({ locateFile: file => new URL(file, import.meta.url).href + VERSION });
}

let hubRenderer = null;

// The renderer with the Hub's stock assets loaded (once per page)
export function loadHubRenderer(hubBaseUrl = '') {
  if (!hubRenderer) {
    hubRenderer = (async () => {
      const module = await loadRenderer();
      try {
        writeTar(module, await fetchTar(`${hubBaseUrl}/api/icons/archive`), STOCK_ASSET_DIR, 1); // drop "icons/"
      } catch (e) {
        console.warn('Remote preview: no stock assets from the Hub', e);
      }
      return module;
    })();
  }
  return hubRenderer;
}

// The activity ID as the remote stores it: the RF frame keeps 3 characters of short IDs
// and 8 of longer ones
export function remoteActivityId(id) {
  const s = String(id);
  return s.slice(0, s.length <= 2 ? 3 : 8);
}

// Writes /data/remote_rf_config.json the way the remote stores the Hub's config push
// (payload/source/rf_config_push.c, decoded by remote_rf_sync.c), including the field
// lengths the RF frames cut text to. activities and devices are the Hub's
// /api/activities and /api/devices lists; remoteMap is a slot's /api/rf-mapping. Custom
// icons (which the Hub numbers from 0x8001 and streams to the remote) aren't known here,
// so those entries show their stock fallback.
export function writeRemoteConfig(module, { activities = [], devices = [], remoteMap = {} }) {
  // name: as the Hub reads it; stored: as the list chunk cut it, which the remote picks the
  // icon from before the rest of the name arrives (RF_MSG_NAME_TAIL_CHUNK)
  const iconFields = (icon, name, type, stored) => {
    const iconId = stockIconId(module, icon, name);
    return { icon: iconKeyword(module, icon, name, type, stored), ...(iconId ? { icon_id: iconId } : {}) };
  };
  const pushedDevices = devices.filter((d) => d.id != null && (d.name || d.Name)).slice(0, 16);
  const deviceIds = pushedDevices.map((d) => String(d.id));
  const acts = activities.filter((a) => String(a.id) !== '-1' && (a.name || a.Name)).slice(0, 16).map((a) => {
    const name = String(a.name || a.Name);
    const stored = name.slice(0, String(a.id).length <= 2 ? 17 : 12); // short IDs leave 17 characters
    // The devices the activity uses, as indexes into the device list, as the Hub pushes them
    const devs = (a.deviceIds || []).map((id) => deviceIds.indexOf(String(id))).filter((i) => i >= 0).slice(0, 16);
    return { id: remoteActivityId(a.id), name: name.slice(0, 39), ...iconFields(a.icon || a.Icon, name, a.type, stored), ...(devs.length ? { devs } : {}) };
  });
  const devs = pushedDevices.map((d) => {
    const name = String(d.name || d.Name);
    const stored = name.slice(0, 16);
    const type = d.type || d.Type || d.deviceType || d.DeviceTypeDisplayName;
    return {
      id: String(d.id).slice(0, 4),
      name: name.slice(0, 39),
      ...iconFields(d.icon || d.Icon, name, type, stored),
      commands: (d.commands || []).slice(0, 255).map((c) => String(typeof c === 'string' ? c : c.name || '').slice(0, 22)),
    };
  });

  const buttons = [];
  let hasPowerOff = false;
  for (const [ctx, entry] of Object.entries(remoteMap.activities || {})) {
    if (!entry || typeof entry !== 'object') continue;
    const screenButtons = Array.isArray(entry.screen_buttons) ? entry.screen_buttons : [];
    if (screenButtons.length > 0) {
      let i = 0;
      for (const b of screenButtons) {
        if (buttons.length >= 48) break;
        if (!b || typeof b !== 'object') continue;
        buttons.push({ ctx: ctx.slice(0, 9), label: String(b.label || 'Button').slice(0, 12), ctype: 1, atype: 1, bidx: i++ });
      }
      continue;
    }
    let i = 0;
    for (const [name, b] of Object.entries(entry)) {
      if (buttons.length >= 48) break;
      if (name === 'screen_buttons' || !b || typeof b !== 'object') continue;
      if (/^(pwroff|poweroffactivity)$/i.test(name)) hasPowerOff = true;
      let atype = 1;
      let target = ctx;
      if (b.action === 'activity_start') { atype = 2; target = String(b.activityId || ctx); }
      if (b.action === 'activity_stop') { atype = 3; target = '-1'; }
      buttons.push({ ctx: target.slice(0, 9), label: name.slice(0, 12), ctype: 0, atype, bidx: i++ });
    }
  }
  if (!hasPowerOff && buttons.length < 48) buttons.push({ ctx: '-1', label: 'PwrOff', ctype: 0, atype: 3, bidx: 0 });

  const config = { version: 0, slot: 1, activities: acts, devices: devs, buttons, steps: [] };
  writeFile(module, '/data/remote_rf_config.json', new TextEncoder().encode(JSON.stringify(config)));
  return config;
}

// Fetches a tar archive, gunzipping it when it is gzip-compressed
export async function fetchTar(url) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${url}: HTTP ${res.status}`);
  const buf = new Uint8Array(await res.arrayBuffer());
  if (buf[0] !== 0x1f || buf[1] !== 0x8b) return buf;
  const stream = new Blob([buf]).stream().pipeThrough(new DecompressionStream('gzip'));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

// Writes the files of an uncompressed tar archive under ROOT + prefix, dropping the first
// `strip` path components of each name
export function writeTar(module, buffer, prefix = '', strip = 0) {
  const bytes = buffer instanceof Uint8Array ? buffer : new Uint8Array(buffer);
  const text = (from, len) => {
    let s = '';
    for (let i = from; i < from + len && bytes[i]; i++) s += String.fromCharCode(bytes[i]);
    return s;
  };
  let count = 0;
  let longName = '';
  for (let off = 0; off + 512 <= bytes.length;) {
    const name = text(off, 100);
    if (!name) break;
    const size = parseInt(text(off + 124, 12).trim() || '0', 8);
    const type = String.fromCharCode(bytes[off + 156] || 48);
    const ustarPrefix = text(off + 345, 155);
    const data = bytes.subarray(off + 512, off + 512 + size);
    off += 512 + Math.ceil(size / 512) * 512;
    if (type === 'L') { // GNU long name for the next entry
      longName = new TextDecoder().decode(data).replace(/\0+$/, '');
      continue;
    }
    const full = longName || (ustarPrefix ? `${ustarPrefix}/${name}` : name);
    longName = '';
    if (type !== '0' && type !== '\0') continue;
    const parts = full.replace(/^\.\//, '').split('/').filter(Boolean).slice(strip);
    if (!parts.length) continue;
    const path = `${ROOT}${prefix}/${parts.join('/')}`;
    module.FS.mkdirTree(path.substring(0, path.lastIndexOf('/')));
    module.FS.writeFile(path, data);
    count++;
  }
  module.ccall('er_assets_changed', null, [], []);
  return count;
}

export function writeFile(module, path, data) {
  const full = `${ROOT}${path}`;
  module.FS.mkdirTree(full.substring(0, full.lastIndexOf('/')));
  module.FS.writeFile(full, data);
  if (/\.png$/i.test(path)) module.ccall('er_assets_changed', null, [], []);
}

export function readConfig(module) {
  try {
    return JSON.parse(module.FS.readFile(`${ROOT}/data/remote_rf_config.json`, { encoding: 'utf8' }));
  } catch {
    return null;
  }
}

// The icon keyword the remote stores for an activity or device after the Hub's config push
export function iconKeyword(module, icon, name, type, storedName) {
  return module.ccall('er_icon_keyword', 'string', ['string', 'string', 'string', 'string'],
    [icon || '', name || '', type || '', storedName || '']);
}

// The stock icon index the Hub sends for an icon or name (0 when it isn't a stock icon)
export function stockIconId(module, icon, name) {
  return module.ccall('er_stock_icon_id', 'number', ['string', 'string'], [icon || '', name || '']);
}

// Loads a theme package (assets/themes/<name>.tar, built by tools/remote_render/build_theme.py)
// into the module, where the remote has its themes; resolves to false when there is none.
// Classic is built in.
const themes = new Map();
export function loadTheme(module, name) {
  if (!name || name === 'classic') return Promise.resolve(true);
  if (!themes.has(name)) {
    themes.set(name, fetchTar(new URL(`themes/${name}.tar${VERSION}`, import.meta.url).href)
      .then((tar) => { writeTar(module, tar, `/data/themes/${name}`); return true; })
      .catch((e) => { console.warn(`Remote preview: no theme ${name}`, e); return false; }));
  }
  return themes.get(name);
}

// The themes built into assets/themes ([{ id, name }]), Classic first
export async function themeList() {
  try {
    const res = await fetch(new URL(`themes/index.json${VERSION}`, import.meta.url).href);
    if (res.ok) return await res.json();
  } catch { /* no themes in this build */ }
  return [{ id: 'classic', name: 'Classic' }];
}

// The image the remote draws for an activity in the config writeRemoteConfig() last wrote, in
// a theme loadTheme() loaded, as an object URL ('' when there is none). Like the remote
// (blit_png_file in remote/remote_fb.c), it recolours dark line art in the theme's line_art
// colour, except in theme icons.
const iconUrls = new Map();
export function activityIconUrl(module, activityId, theme = 'classic') {
  module.screenDrawn = false; // reloads the config: hitTest() needs a new render()
  return iconUrl(module, module.ccall('er_activity_icon', 'string', ['string', 'string', 'string'], [ROOT, remoteActivityId(activityId), theme]), theme);
}

// The image the remote draws for a device, like activityIconUrl
export function deviceIconUrl(module, deviceId, theme = 'classic') {
  module.screenDrawn = false;
  return iconUrl(module, module.ccall('er_device_icon', 'string', ['string', 'string', 'string'], [ROOT, String(deviceId).slice(0, 4), theme]), theme);
}

// The theme's line_art colour as [r, g, b], read as load_theme in remote/remote_theme.c reads it
function lineArt(module, theme) {
  try {
    const cfg = JSON.parse(module.FS.readFile(`${ROOT}/data/themes/${theme}/theme.json`, { encoding: 'utf8' }));
    const m = /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(cfg.line_art || '');
    if (m) return [1, 2, 3].map(i => parseInt(m[i], 16));
  } catch { /* Classic, or no theme.json: the default */ }
  return [245, 245, 250];
}

function iconUrl(module, path, theme) {
  if (!path) return Promise.resolve('');
  const [r, g, b] = lineArt(module, theme);
  const key = `${path}|${r},${g},${b}`;
  if (!iconUrls.has(key)) {
    iconUrls.set(key, (async () => {
      const bitmap = await createImageBitmap(new Blob([module.FS.readFile(`${ROOT}${path}`)], { type: 'image/png' }));
      const canvas = document.createElement('canvas');
      canvas.width = bitmap.width;
      canvas.height = bitmap.height;
      const ctx = canvas.getContext('2d');
      ctx.drawImage(bitmap, 0, 0);
      if (!path.includes('/themes/')) {
        const img = ctx.getImageData(0, 0, canvas.width, canvas.height);
        const px = img.data;
        for (let i = 0; i < px.length; i += 4) {
          if (px[i] + px[i + 1] + px[i + 2] < 100 && px[i + 3] > 30) {
            px[i] = r; px[i + 1] = g; px[i + 2] = b;
          }
        }
        ctx.putImageData(img, 0, 0);
      }
      const blob = await new Promise(resolve => canvas.toBlob(resolve, 'image/png'));
      return blob ? URL.createObjectURL(blob) : '';
    })().catch(() => '')); // the image isn't in this module's filesystem
  }
  return iconUrls.get(key);
}

// state: { view: 'activities'|'commands'|'devices'|'device'|'settings'|'sync', select (the
// row, or the device for 'device'), active (remote activity ID), theme (loaded with
// loadTheme()), layout ('theme', 'list', 'grid' or 'carousel'), icons, battery, usb, charging, noHub, minutes }
// Draws a screen. state.listTop scrolls the list on it that many pixels down (clamped to its
// ends); without it a list opens on the page holding its selection. Returns where the list
// ended up ({ listTop }, 0 without a list) and the home layout's page size ({ homePageSize }).
export function render(module, canvas, state = {}) {
  const ptr = module.ccall('er_render', 'number',
    ['string', 'string', 'number', 'string', 'string', 'number', 'number', 'number', 'number', 'number', 'string', 'string', 'number'],
    [ROOT, state.view || 'activities', state.select || 0, state.active || '', state.icons || 'classic',
     state.battery ?? 80, state.usb ? 1 : 0, state.charging ? 1 : 0, state.noHub ? 1 : 0, state.minutes ?? 21 * 60 + 30,
     state.theme || 'classic', state.layout || 'theme', state.listTop ?? -1]);
  const pixels = new ImageData(new Uint8ClampedArray(module.HEAPU8.buffer, ptr, 240 * 320 * 4).slice(), 240, 320);
  canvas.getContext('2d').putImageData(pixels, 0, 0);
  module.screenDrawn = true;
  return {
    listTop: module.ccall('er_list_top', 'number', [], []),
    homePageSize: module.ccall('er_home_page_size', 'number', [], []),
  };
}

// What a tap at (x, y), in the remote's screen pixels, opens on the screen render() last drew,
// found with the remote's own touch handling: { activity: remote activity ID }, { device:
// index into the config's devices }, or null. Undefined when the module has loaded other
// state since (activityIconUrl, deviceIconUrl): render() again first.
export function hitTest(module, x, y) {
  if (!module.screenDrawn) return undefined;
  const hit = module.ccall('er_hit', 'string', ['number', 'number'], [Math.floor(x), Math.floor(y)]);
  const [kind, id] = hit.split(' ');
  if (kind === 'activity') return { activity: id };
  if (kind === 'device') return { device: Number(id) };
  return null;
}
