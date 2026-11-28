/**
 * Curated device templates (harmony-ng-plugins/devices, format in its README.md): the index and
 * the templates come from GitHub through jsDelivr, else raw.githubusercontent. Template text is
 * untrusted: callers show it escaped, and the Hub checks the device it becomes.
 */
export const TEMPLATES_CDN = 'https://cdn.jsdelivr.net/gh/iranl/harmony-ng-plugins@main/';
export const TEMPLATES_RAW = 'https://raw.githubusercontent.com/iranl/harmony-ng-plugins/main/';
export const TEMPLATE_TRANSPORTS = ['ir', 'bt', 'http', 'tcp', 'mqtt', 'plugin'];

const cache = {};
const FETCH_TIMEOUT_MS = 10000;

async function fetchJson(rel) {
  if (cache[rel]) return cache[rel];
  let lastErr = null;
  for (const base of [TEMPLATES_CDN, TEMPLATES_RAW]) {
    try {
      const r = await fetch(base + rel, typeof AbortSignal?.timeout === 'function' ? { signal: AbortSignal.timeout(FETCH_TIMEOUT_MS) } : {});
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
   (a number stays a number). Only the values' own keys: {{constructor}} is no value. */
export function fillPlaceholders(value, values) {
  const get = k => (Object.prototype.hasOwnProperty.call(values, k) ? values[k] ?? '' : '');
  if (typeof value === 'string') {
    const whole = value.match(/^\{\{([a-zA-Z0-9_]+)\}\}$/);
    if (whole) return get(whole[1]);
    return value.replace(/\{\{([a-zA-Z0-9_]+)\}\}/g, (_, k) => String(get(k)));
  }
  if (Array.isArray(value)) return value.map(v => fillPlaceholders(v, values));
  if (value && typeof value === 'object') {
    return Object.fromEntries(Object.entries(value).map(([k, v]) => [k, fillPlaceholders(v, values)]));
  }
  return value;
}

/* The template's setup fields: objects with a key, each key once (a template is untrusted) */
export function setupFields(t) {
  const seen = new Set();
  return (Array.isArray(t?.setup?.fields) ? t.setup.fields : []).filter(f => {
    if (!f || typeof f !== 'object' || typeof f.key !== 'string' || !/^[a-zA-Z0-9_]+$/.test(f.key) || seen.has(f.key)) return false;
    seen.add(f.key);
    return true;
  });
}

/* The setup values typed in, by field type, trimmed (empty: the default). A number field takes
   only a whole number (else NaN, reported by badSetupFields); a host field drops a URL's
   scheme and path ("http://10.0.0.5/" is 10.0.0.5). */
export function setupValues(t, typed) {
  const out = {};
  for (const f of setupFields(t)) {
    let v = typed[f.key];
    v = v === undefined || v === null ? '' : String(v).trim();
    if (v === '') v = f.default !== undefined ? String(f.default).trim() : '';
    if (f.type === 'number') {
      out[f.key] = v === '' ? '' : (/^-?\d+$/.test(v) ? Number(v) : NaN);
    } else {
      if ((f.type === 'host' || f.key === 'host') && v) v = v.replace(/^[a-z][a-z0-9+.-]*:\/\//i, '').replace(/\/.*$/, '');
      out[f.key] = v;
    }
  }
  return out;
}

/* The setup fields that are required and empty */
export function missingSetupFields(t, values) {
  return setupFields(t).filter(f => f.required && (values[f.key] === '' || values[f.key] === undefined));
}

/* The number fields with a value that is not a whole number in the field's min..max */
export function badSetupFields(t, values) {
  return setupFields(t).filter(f => {
    const v = values[f.key];
    if (f.type !== 'number' || v === '' || v === undefined) return false;
    return !Number.isInteger(v) || (typeof f.min === 'number' && v < f.min) || (typeof f.max === 'number' && v > f.max);
  });
}

/* name, or "name 2", "name 3", ... when a device has it already (case ignored) */
export function uniqueDeviceName(name, taken) {
  const used = new Set((taken || []).map(n => String(n).toLowerCase()));
  if (!used.has(name.toLowerCase())) return name;
  for (let i = 2; ; i++) {
    if (!used.has(`${name} ${i}`.toLowerCase())) return `${name} ${i}`;
  }
}

/* The combined device for the ticked transports (picked), in the template's order. The
   template's own commands come first, in that order (of two with one name, case ignored, the
   earlier transport's is kept: the Hub takes one command per name); the plugin's default
   commands ("default_commands": true, pluginDef: the installed plugin) only fill names still
   free, so a template command always wins over a plugin default. name: the device's name
   (default the template's). */
export function buildDeviceFromTemplate(t, picked, values, pluginDef = null, name = '') {
  const order = templateTransports(t).map(x => x.key).filter(k => picked.includes(k));
  const str = v => (typeof v === 'string' ? v : '');
  const dev = {
    name: str(name).trim() || str(t.name) || 'Device', manufacturer: str(t.manufacturer) || 'Generic', model: str(t.model) || 'Generic',
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
    dev.mqttTopic = `{base_topic}/button/${dev.name.toLowerCase().replace(/[^a-z0-9_-]+/g, '_')}/{command}`;
    dev.mqttPulseMs = 1000;
  }
  const names = new Set();
  const commands = [];
  const add = (c, k) => {
    const key = c.name.toLowerCase();
    if (names.has(key)) return;
    names.add(key);
    const cmd = fillPlaceholders({ ...c, transport: k }, values);
    if (cmd.keyCode && !cmd.raw) cmd.raw = cmd.keyCode;
    commands.push(cmd);
  };
  for (const k of order) {
    for (const c of t.commands) {
      if (c && c.transport === k && typeof c.name === 'string' && c.name) add(c, k);
    }
  }
  if (order.includes('plugin') && plug.default_commands && pluginDef) {
    for (const c of pluginDef.device_definition?.default_commands || []) {
      if (!c || typeof c.name !== 'string' || !c.name) continue;
      add({ name: c.name, keyCode: c.keyCode || `PLUGIN:${plug.id}:${c.name}`,
        ...(Array.isArray(c.range) ? { range: c.range } : {}) }, 'plugin');
    }
  }
  dev.commands = commands;
  return dev;
}
