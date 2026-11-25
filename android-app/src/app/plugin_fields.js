// Plugin device settings and commands in the legacy web UI (docs/PLUGINS.md):
// - a config field of "type": "boolean" is a checkbox, saved as true / false (a field a command's
//   irWhen names turns its infrared alternative on)
// - a command created from the manifest's default_commands keeps its range and its infrared
//   alternative (irKeyCode, irWhen)
import { escapeHtml } from '../html_escape.js';
import { tf } from '../i18n.js';

export function pluginFieldIsBoolean(f) {
  return !!f && String(f.type || '').toLowerCase() === 'boolean';
}

// A stored config value that turns a boolean field on (as the Hub reads it): true, a number
// other than 0, "true" or "1"
export function configFlagOn(v) {
  if (v === true) return true;
  if (typeof v === 'number') return v !== 0;
  if (typeof v === 'string') return v.toLowerCase() === 'true' || v === '1';
  return false;
}

// The label and input of one config field. idPrefix: 'plug_cfg_' (new device) or
// 'edit_plug_cfg_' (device editor); small: the editor's smaller controls
export function pluginFieldHtml(f, idPrefix, curVal, small = false) {
  const id = `${idPrefix}${f.key}`;
  const labelStyle = `font-size: ${small ? 11 : 12}px; margin-bottom: 2px;`;
  // A checkbox takes a whole row of the grid; other fields put their input at the bottom of the
  // cell, so inputs line up when the labels of a row wrap to a different number of lines
  if (pluginFieldIsBoolean(f)) {
    return `
            <div style="grid-column: 1 / -1;">
              <label for="${escapeHtml(id)}" style="${labelStyle} display: flex; align-items: center; gap: 6px; cursor: pointer;">
                <input id="${escapeHtml(id)}" type="checkbox"${configFlagOn(curVal) ? ' checked' : ''}>
                <span>${escapeHtml(f.label || f.key)}</span>
              </label>
            </div>`;
  }
  const type = f.type === 'number' ? 'number' : (f.type === 'password' ? 'password' : 'text');
  const val = curVal === undefined || curVal === null ? '' : String(curVal);
  return `
            <div style="display: flex; flex-direction: column; justify-content: flex-end;">
              <label for="${escapeHtml(id)}" style="${labelStyle}">${escapeHtml(f.label || f.key)}</label>
              <input id="${escapeHtml(id)}" class="form-control${small ? ' form-control-sm' : ''}" type="${type}"
                     value="${escapeHtml(val)}" placeholder="${escapeHtml(f.placeholder || '')}">
            </div>`;
}

// The value of a config input as it is saved: a checkbox true / false, a number field a number
export function pluginInputValue(inp) {
  if (!inp) return undefined;
  if (inp.type === 'checkbox') return !!inp.checked;
  if (inp.type === 'number') return Number(inp.value);
  return inp.value;
}

// A device command made from a default_commands entry of the plugin's manifest
export function pluginDefaultCommand(c, pluginId) {
  const code = c.keyCode || `PLUGIN:${pluginId}:${c.name}`;
  return {
    name: c.name,
    keyCode: code,
    raw: c.raw || code,
    ...(Array.isArray(c.range) ? { range: c.range } : {}),
    ...(typeof c.irKeyCode === 'string' && c.irKeyCode ? { irKeyCode: c.irKeyCode } : {}),
    ...(typeof c.irWhen === 'string' && c.irWhen ? { irWhen: c.irWhen } : {}),
  };
}

// An "IR" badge for a command with an infrared alternative (irKeyCode, irWhen): lit while the
// device's config field irWhen is on (the command then goes out as infrared), dimmed while off.
// cfg: the device's config (null: dimmed)
export function irAltBadgeHtml(c, cfg) {
  if (!c || typeof c.irKeyCode !== 'string' || !c.irKeyCode || typeof c.irWhen !== 'string' || !c.irWhen) return '';
  const on = !!cfg && typeof cfg === 'object' && configFlagOn(cfg[c.irWhen]);
  const title = tf('plg_ir_alt_title', { field: c.irWhen, code: c.irKeyCode });
  return `<span class="badge ${on ? 'ok' : 'subtle'} mini" style="font-size: 10px;" title="${escapeHtml(title)}">IR</span>`;
}
