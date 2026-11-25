/**
 * Curated device templates (harmony-ng-plugins/devices, format in its README.md): the index and
 * the templates come from GitHub through jsDelivr, else raw.githubusercontent. Template text is
 * untrusted: callers show it escaped, and the Hub checks the device it becomes.
 */
export const TEMPLATES_CDN = 'https://cdn.jsdelivr.net/gh/iranl/harmony-ng-plugins@main/';
export const TEMPLATES_RAW = 'https://raw.githubusercontent.com/iranl/harmony-ng-plugins/main/';
export const TEMPLATE_TRANSPORTS = ['ir', 'bt', 'http', 'tcp', 'mqtt', 'plugin'];

const cache = {};

async function fetchJson(rel) {
  if (cache[rel]) return cache[rel];
  let lastErr = null;
  for (const base of [TEMPLATES_CDN, TEMPLATES_RAW]) {
    try {
      const r = await fetch(base + rel);
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      cache[rel] = await r.json();
      return cache[rel];
    } catch (err) {
      lastErr = err;
    }
  }
  throw lastErr || new Error('fetch failed');
}

export async function fetchTemplateIndex() {
  const j = await fetchJson('devices/index.json');
  const list = Array.isArray(j?.templates) ? j.templates : [];
  return list.filter(e => e && typeof e.id === 'string' && typeof e.name === 'string' &&
    typeof e.file === 'string' && /^devices\/[a-z0-9-]+\.json$/.test(e.file));
}

export async function fetchTemplate(entry) {
  const t = await fetchJson(entry.file);
  if (!t || t.schema !== 1 || !t.transports || typeof t.transports !== 'object' || !Array.isArray(t.commands)) {
    throw new Error('not a device template (schema 1)');
  }
  return t;
}

/* The template's transports in its order (the priority), each {key, label, default, plugin} */
export function templateTransports(t) {
  return Object.keys(t.transports || {}).filter(k => TEMPLATE_TRANSPORTS.includes(k)).map(k => {
    const v = t.transports[k] || {};
    return { key: k, label: typeof v.label === 'string' ? v.label : '', default: !!v.default,
      plugin: k === 'plugin' && typeof v.id === 'string' ? v.id : '' };
  });
}

/* {{key}} replaced with the setup value; a string that is only {{key}} takes the value as is
   (a number stays a number) */
export function fillPlaceholders(value, values) {
  if (typeof value === 'string') {
    const whole = value.match(/^\{\{([a-zA-Z0-9_]+)\}\}$/);
    if (whole) return values[whole[1]] ?? '';
    return value.replace(/\{\{([a-zA-Z0-9_]+)\}\}/g, (_, k) => String(values[k] ?? ''));
  }
  if (Array.isArray(value)) return value.map(v => fillPlaceholders(v, values));
  if (value && typeof value === 'object') {
    return Object.fromEntries(Object.entries(value).map(([k, v]) => [k, fillPlaceholders(v, values)]));
  }
  return value;
}

/* The setup values typed in, by field type (number fields as numbers, empty: the default) */
export function setupValues(t, typed) {
  const out = {};
  for (const f of t.setup?.fields || []) {
    if (!f || typeof f.key !== 'string') continue;
    let v = typed[f.key];
    if (v === undefined || v === '') v = f.default !== undefined ? f.default : '';
    out[f.key] = f.type === 'number' && v !== '' ? Number(v) : String(v);
  }
  return out;
}

/* The setup fields that are required and empty */
export function missingSetupFields(t, values) {
  return (t.setup?.fields || []).filter(f => f && f.required && (values[f.key] === '' || values[f.key] === undefined ||
    (typeof values[f.key] === 'number' && Number.isNaN(values[f.key]))));
}

/* The combined device for the ticked transports (picked), in the template's order: of two
   commands with one name, the earlier transport's is kept. pluginDef: the installed plugin, for
   "default_commands": true. */
export function buildDeviceFromTemplate(t, picked, values, pluginDef = null) {
  const order = templateTransports(t).map(x => x.key).filter(k => picked.includes(k));
  const str = v => (typeof v === 'string' ? v : '');
  const dev = {
    name: str(t.name) || 'Device', manufacturer: str(t.manufacturer) || 'Generic', model: str(t.model) || 'Generic',
    icon: str(t.icon) || 'generic', type: 'Combined', transports: order
  };
  for (const k of ['http', 'tcp']) {
    if (order.includes(k)) dev[k] = fillPlaceholders(t.transports[k]?.connection || {}, values);
  }
  const plug = t.transports.plugin || {};
  if (order.includes('plugin')) {
    dev.plugin = plug.id;
    dev.config = fillPlaceholders(plug.config || {}, values);
  }
  if (order.includes('mqtt')) {
    dev.mqttTopic = `{base_topic}/button/${dev.name.toLowerCase().replace(/\s+/g, '_')}/{command}`;
    dev.mqttPulseMs = 1000;
  }
  const names = new Set();
  const commands = [];
  for (const k of order) {
    const list = t.commands.filter(c => c && c.transport === k && typeof c.name === 'string' && c.name);
    if (k === 'plugin' && plug.default_commands && pluginDef) {
      for (const c of pluginDef.device_definition?.default_commands || []) {
        if (!list.some(x => x.name === c.name)) {
          list.push({ name: c.name, keyCode: c.keyCode || `PLUGIN:${plug.id}:${c.name}`,
            ...(Array.isArray(c.range) ? { range: c.range } : {}) });
        }
      }
    }
    for (const c of list) {
      if (names.has(c.name)) continue;
      names.add(c.name);
      const cmd = fillPlaceholders({ ...c, transport: k }, values);
      if (cmd.keyCode && !cmd.raw) cmd.raw = cmd.keyCode;
      commands.push(cmd);
    }
  }
  dev.commands = commands;
  return dev;
}
