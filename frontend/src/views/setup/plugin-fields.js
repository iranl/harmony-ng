// Plugin device settings (docs/PLUGINS.md): a config field of "type": "boolean" is a checkbox,
// saved as true / false (a field a command's irWhen names turns its infrared alternative on)
import { h } from '../../utils/dom.js';
import { t } from '../../i18n/index.js';

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

// The input of one config field, and the row that shows it with its label
export function pluginFieldInput(f, curVal, placeholder) {
  if (pluginFieldIsBoolean(f)) {
    const box = h('input', { type: 'checkbox', checked: configFlagOn(curVal) });
    box.checked = configFlagOn(curVal);
    return box;
  }
  const inp = h('input', {
    type: f.type === 'number' ? 'number' : (f.type === 'password' ? 'password' : 'text'),
    className: 'form-input',
    placeholder: placeholder !== undefined ? placeholder : (f.placeholder || ''),
  });
  if (curVal !== undefined && curVal !== null) inp.value = String(curVal);
  return inp;
}

export function pluginFieldRow(f, inp) {
  if (inp.type === 'checkbox') {
    // a checkbox takes a whole row when the fields sit in a grid
    return h('label', { style: { display: 'flex', alignItems: 'center', gap: '8px', cursor: 'pointer', gridColumn: '1 / -1' } }, [
      inp,
      h('span', { className: 'form-label', style: { margin: '0' }, text: f.label || f.key }),
    ]);
  }
  // the input at the bottom of its cell: inputs line up when labels wrap to different heights
  return h('div', { style: { display: 'flex', flexDirection: 'column', justifyContent: 'flex-end', gap: '4px' } }, [
    h('label', { className: 'form-label', text: f.label || f.key }),
    inp,
  ]);
}

// The value of a config input as it is saved: a checkbox true / false, a number field a number
export function pluginInputValue(inp) {
  if (inp.type === 'checkbox') return !!inp.checked;
  if (inp.type === 'number') return Number(inp.value);
  return inp.value.trim();
}

// An "IR" badge for a command with an infrared alternative (irKeyCode, irWhen): lit while the
// device's config field irWhen is on (the command then goes out as infrared), dimmed while off.
// null for a command without one. cfg: the device's config
export function irAltBadge(c, cfg) {
  if (!c || typeof c.irKeyCode !== 'string' || !c.irKeyCode || typeof c.irWhen !== 'string' || !c.irWhen) return null;
  const on = !!cfg && typeof cfg === 'object' && configFlagOn(cfg[c.irWhen]);
  return h('span', {
    className: on ? 'badge badge-success' : 'badge',
    style: { marginLeft: '8px', fontSize: '10px', opacity: on ? '1' : '0.6' },
    title: t('plg_ir_alt_title', 'Sent as infrared while "{field}" is on: {code}', { field: c.irWhen, code: c.irKeyCode }),
    text: 'IR',
  });
}
