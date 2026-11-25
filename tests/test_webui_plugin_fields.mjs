// Plugin devices in both web UIs (docs/PLUGINS.md):
// - a config field of "type": "boolean" is a checkbox, saved as true / false: a command's irWhen
//   names such a field to turn its infrared alternative on (the inputs were text fields, saved
//   "on" or "")
// - a command made from the manifest's default_commands keeps irKeyCode and irWhen (the legacy
//   UI kept name, keyCode, raw and range only)
// Runs in Node with a stand-in DOM: no browser, no Hub.
import assert from 'node:assert/strict';
import { installFakeDom, makeChecker } from './webui_fake_dom.mjs';

installFakeDom();
const legacy = await import('../android-app/src/app/plugin_fields.js');
const fresh = await import('../frontend/src/views/setup/plugin-fields.js');

const BOOL = { key: 'hdmi_via_ir', label: 'Send HDMI keys as infrared', type: 'boolean' };
const HOST = { key: 'host', label: 'Host', type: 'string' };
const IR = 'G:GoVideoO1 32 Bit:()(0xE0E07D82)():3';
const t = makeChecker('Web UIs: plugin boolean fields and infrared alternative of default commands');

await t.check('a stored value turns a boolean field on as the Hub reads it', () => {
  for (const ui of [legacy, fresh]) {
    for (const v of [true, 1, 2, 'true', 'TRUE', '1']) assert.equal(ui.configFlagOn(v), true, String(v));
    for (const v of [false, 0, '0', 'false', '', 'on', null, undefined]) assert.equal(ui.configFlagOn(v), false, String(v));
  }
});

await t.check('legacy UI: a boolean field is a checkbox, checked from the stored value', () => {
  const on = legacy.pluginFieldHtml(BOOL, 'edit_plug_cfg_', true, true);
  assert.match(on, /id="edit_plug_cfg_hdmi_via_ir" type="checkbox" checked>/);
  const off = legacy.pluginFieldHtml(BOOL, 'plug_cfg_', undefined);
  assert.match(off, /id="plug_cfg_hdmi_via_ir" type="checkbox">/);
  assert.match(legacy.pluginFieldHtml(HOST, 'plug_cfg_', '10.0.0.5'), /type="text"\s+value="10\.0\.0\.5"/);
});

await t.check('legacy UI: a boolean field takes a whole grid row, other inputs sit at the bottom of their cell', () => {
  assert.match(legacy.pluginFieldHtml(BOOL, 'plug_cfg_', true), /grid-column: 1 \/ -1/);
  assert.match(legacy.pluginFieldHtml(HOST, 'plug_cfg_', ''), /justify-content: flex-end/);
});

await t.check('legacy UI: a command with an infrared alternative shows an IR badge, lit while its switch is on', () => {
  const cmd = { name: 'HDMI 2', keyCode: 'PLUGIN:samsung-tv:HDMI 2', irKeyCode: IR, irWhen: 'hdmi_via_ir' };
  assert.match(legacy.irAltBadgeHtml(cmd, { hdmi_via_ir: true }), /class="badge ok mini"[^>]*>IR</);
  assert.match(legacy.irAltBadgeHtml(cmd, { hdmi_via_ir: false }), /class="badge subtle mini"/);
  assert.match(legacy.irAltBadgeHtml(cmd, null), /class="badge subtle mini"/);
  assert.ok(legacy.irAltBadgeHtml(cmd, {}).includes('0xE0E07D82'), 'the code is in the title');
  assert.equal(legacy.irAltBadgeHtml({ name: 'Home', keyCode: 'PLUGIN:samsung-tv:Home' }, { hdmi_via_ir: true }), '');
  assert.equal(legacy.irAltBadgeHtml({ name: 'X', irKeyCode: IR }, { hdmi_via_ir: true }), '', 'no irWhen: no badge');
});

await t.check('legacy UI: a checkbox saves true / false, a number field a number', () => {
  assert.equal(legacy.pluginInputValue({ type: 'checkbox', checked: true, value: 'on' }), true);
  assert.equal(legacy.pluginInputValue({ type: 'checkbox', checked: false, value: 'on' }), false);
  assert.equal(legacy.pluginInputValue({ type: 'number', value: '8060' }), 8060);
  assert.equal(legacy.pluginInputValue({ type: 'text', value: 'x' }), 'x');
});

await t.check('legacy UI: a command from default_commands keeps irKeyCode and irWhen', () => {
  const c = legacy.pluginDefaultCommand({ name: 'HDMI 2', keyCode: 'PLUGIN:samsung-tv:KEY_HDMI2', irKeyCode: IR, irWhen: 'hdmi_via_ir' }, 'samsung-tv');
  assert.deepEqual(c, { name: 'HDMI 2', keyCode: 'PLUGIN:samsung-tv:KEY_HDMI2', raw: 'PLUGIN:samsung-tv:KEY_HDMI2', irKeyCode: IR, irWhen: 'hdmi_via_ir' });
  const plain = legacy.pluginDefaultCommand({ name: 'Level', range: [0, 31] }, 'fan');
  assert.deepEqual(plain, { name: 'Level', keyCode: 'PLUGIN:fan:Level', raw: 'PLUGIN:fan:Level', range: [0, 31] });
});

await t.check('new UI: a boolean field is a checkbox and saves true / false', () => {
  const on = fresh.pluginFieldInput(BOOL, true);
  assert.equal(on.type, 'checkbox');
  assert.equal(on.checked, true);
  assert.equal(fresh.pluginInputValue(on), true);
  const off = fresh.pluginFieldInput(BOOL, 'false');
  assert.equal(off.checked, false);
  assert.equal(fresh.pluginInputValue(off), false);
  const row = fresh.pluginFieldRow(BOOL, on);
  assert.equal(row.tagName, 'label');
  assert.ok(row.textContent.includes('Send HDMI keys as infrared'));
});

await t.check('new UI: text and number fields are unchanged', () => {
  const host = fresh.pluginFieldInput(HOST, ' 10.0.0.5 ');
  assert.equal(host.type, 'text');
  assert.equal(fresh.pluginInputValue(host), '10.0.0.5');
  const port = fresh.pluginFieldInput({ key: 'port', type: 'number' }, 8060);
  assert.equal(fresh.pluginInputValue(port), 8060);
});

await t.check('new UI: a command with an infrared alternative shows an IR badge, lit while its switch is on', () => {
  const cmd = { name: 'HDMI 2', keyCode: 'PLUGIN:samsung-tv:HDMI 2', irKeyCode: IR, irWhen: 'hdmi_via_ir' };
  const on = fresh.irAltBadge(cmd, { hdmi_via_ir: true });
  assert.equal(on.textContent, 'IR');
  assert.equal(on.className, 'badge badge-success');
  assert.equal(fresh.irAltBadge(cmd, { hdmi_via_ir: false }).className, 'badge');
  assert.equal(fresh.irAltBadge(cmd, null).className, 'badge');
  assert.ok(String(on.getAttribute('title')).includes('0xE0E07D82'), 'the code is in the title');
  assert.equal(fresh.irAltBadge({ name: 'Home', keyCode: 'PLUGIN:samsung-tv:Home' }, { hdmi_via_ir: true }), null);
  assert.equal(fresh.irAltBadge({ name: 'X', irKeyCode: IR }, { hdmi_via_ir: true }), null, 'no irWhen: no badge');
});

t.finish();
