// Browser side of the WebAssembly Elite renderer: loads a filesystem root into the
// module and draws screens into a 240x320 canvas.
import createEliteRenderer from './elite_render.mjs';

const ROOT = '/r';

export async function loadRenderer() {
  return createEliteRenderer();
}

// Writes the files of an uncompressed tar archive under ROOT + prefix
export function writeTar(module, buffer, prefix = '') {
  const bytes = new Uint8Array(buffer);
  const text = (from, len) => {
    let s = '';
    for (let i = from; i < from + len && bytes[i]; i++) s += String.fromCharCode(bytes[i]);
    return s;
  };
  let count = 0;
  for (let off = 0; off + 512 <= bytes.length;) {
    const name = text(off, 100);
    if (!name) break;
    const size = parseInt(text(off + 124, 12).trim() || '0', 8);
    const type = String.fromCharCode(bytes[off + 156] || 48);
    const full = (text(off + 345, 155) ? text(off + 345, 155) + '/' : '') + name;
    const path = `${ROOT}${prefix}/${full.replace(/^\.\//, '')}`.replace(/\/+/g, '/');
    if (type === '0') {
      module.FS.mkdirTree(path.substring(0, path.lastIndexOf('/')));
      module.FS.writeFile(path, bytes.subarray(off + 512, off + 512 + size));
      count++;
    }
    off += 512 + Math.ceil(size / 512) * 512;
  }
  module.ccall('er_assets_changed', null, [], []);
  return count;
}

export function writeFile(module, path, data) {
  const full = `${ROOT}${path}`;
  module.FS.mkdirTree(full.substring(0, full.lastIndexOf('/')));
  module.FS.writeFile(full, data);
  module.ccall('er_assets_changed', null, [], []);
}

export function readConfig(module) {
  try {
    return JSON.parse(module.FS.readFile(`${ROOT}/data/elite_rf_config.json`, { encoding: 'utf8' }));
  } catch {
    return null;
  }
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
