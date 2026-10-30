// Browser side of the WebAssembly Elite renderer: loads files into the module's filesystem
// and draws screens into a 240x320 canvas.
import createEliteRenderer from './elite_render.js';

const ROOT = '/r';
export const STOCK_ASSET_DIR = '/opt/luadraws/appscommon/asset';

export async function loadRenderer() {
  return createEliteRenderer();
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
    return JSON.parse(module.FS.readFile(`${ROOT}/data/elite_rf_config.json`, { encoding: 'utf8' }));
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

// state: { view, select, active, icons, battery, usb, charging, noHub, minutes }
export function render(module, canvas, state = {}) {
  const ptr = module.ccall('er_render', 'number',
    ['string', 'string', 'number', 'string', 'string', 'number', 'number', 'number', 'number', 'number'],
    [ROOT, state.view || 'activities', state.select || 0, state.active || '', state.icons || 'classic',
     state.battery ?? 80, state.usb ? 1 : 0, state.charging ? 1 : 0, state.noHub ? 1 : 0, state.minutes ?? 21 * 60 + 30]);
  const pixels = new ImageData(new Uint8ClampedArray(module.HEAPU8.buffer, ptr, 240 * 320 * 4).slice(), 240, 320);
  canvas.getContext('2d').putImageData(pixels, 0, 0);
}
