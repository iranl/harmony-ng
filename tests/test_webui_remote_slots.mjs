// Both web UIs' RF remote pages:
// - legacy UI: "Auto-Map Standard Keys" is wired to its button and maps each key from the first
//   alias the device has (Back is not taken by an Exit command listed before it)
// - both UIs: a remote without a screen (Companion, Smart Control) cannot pick a device, so the
//   editor offers no per-device bindings for it
// Runs in Node with a stand-in DOM and a stubbed fetch: no browser, no Hub.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { installFakeDom, allowJsonImports, makeChecker } from './webui_fake_dom.mjs';

const { body } = installFakeDom();
await allowJsonImports();
const quietWarn = console.warn;
console.warn = () => {};

const SHIELD = { id: '7', name: 'SHIELD TV', commands: ['Exit', 'Back', 'DirectionUp', 'Select', 'PlayPause', 'VolumeUp', 'PowerOff'].map(name => ({ name })) };
const slots = (model) => Array.from({ length: 6 }, (_, i) => ({ slot: i + 1, active: i === 2, address: i === 2 ? '0A0B0C0D' : 'None',
  model: i === 2 ? model : 'unknown', has_screen: !(i === 2 && model === 'smart_control'), name: i === 2 ? 'Remote (Slot 3)' : '' }));
let rfModel = 'smart_control';
globalThis.fetch = async (url) => {
  const path = String(url).replace(/^https?:\/\/[^/]+/, '');
  let r = { ok: true };
  if (path.startsWith('/api/rf/status')) r = { ok: true, running: true, slots: slots(rfModel) };
  else if (path.startsWith('/api/rf-mapping?')) r = { activities: {}, devices: {} };
  else if (path.startsWith('/api/devices')) r = { devices: [SHIELD] };
  else if (path.startsWith('/api/services')) r = { rf_enabled: true };
  return new Response(JSON.stringify(r), { status: 200 });
};

const t = makeChecker('Web UI remote slots: auto-map, screenless remotes');

function el(tag, id, parent = body) {
  const e = document.createElement(tag);
  e.setAttribute('id', id);
  parent.appendChild(e);
  return e;
}

// ── Legacy UI ──
const { HarmonyAppRemotesScreen } = await import('../android-app/src/app/app_remotes_screen.js');
const { HarmonyClient } = await import('../android-app/src/api.js');

function legacyApp() {
  const app = Object.create(HarmonyAppRemotesScreen.prototype);
  app.client = new HarmonyClient();
  app.inventory = { devices: [SHIELD] };
  app.activitiesData = { activities: [{ id: '5', name: 'Watch SHIELD' }] };
  app.remoteMapData = { activities: {}, devices: {} };
  app.remoteContextMode = 'activity';
  app.remoteSelectedActivityId = '5';
  return app;
}

await t.check('legacy UI: the Auto-Map button is wired to autoMapRemotePreset', async () => {
  const src = readFileSync(new URL('../android-app/src/app/app_events_actions.js', import.meta.url), 'utf8');
  assert.match(src, /getElementById\('remoteRunPresetBtn'\)\?\.addEventListener\('click', \(\) => this\.autoMapRemotePreset\(\)\)/);
  const html = readFileSync(new URL('../android-app/index.html', import.meta.url), 'utf8');
  assert.match(html, /id="remoteRunPresetBtn"/);
});

await t.check('legacy UI: auto-map takes the first alias, PlayPause for Play and Pause, no PowerOffActivity', async () => {
  const app = legacyApp();
  const preset = el('select', 'remotePresetDevice');
  const o = document.createElement('option'); o.value = '7'; preset.appendChild(o);
  preset.value = '7';
  app.autoMapRemotePreset();
  const m = app.remoteMapData.activities['5'];
  assert.equal(m.Back?.command, 'Back', `Back got ${m.Back?.command}`);
  assert.equal(m.Exit?.command, 'Exit');
  assert.equal(m.Play?.command, 'PlayPause');
  assert.equal(m.Pause?.command, 'PlayPause');
  assert.equal(m.DirectionUp?.deviceId, '7');
  assert.equal(m.PowerOffActivity, undefined, 'hub_rf never reads a PowerOffActivity mapping');
  preset.remove();
});

await t.check('legacy UI: auto-map in device scope takes the edited device, not the preset one', async () => {
  const TV = { id: '9', name: 'TV', commands: ['Return', 'Up', 'Enter'].map(name => ({ name })) };
  const app = legacyApp();
  app.inventory = { devices: [SHIELD, TV] };
  app.remoteContextMode = 'device';
  app.remoteSelectedDeviceId = '9';
  const preset = el('select', 'remotePresetDevice');
  for (const v of ['7', '9']) { const o = document.createElement('option'); o.value = v; preset.appendChild(o); }
  preset.value = '7';
  app.autoMapRemotePreset();
  const m = app.remoteMapData.devices['9'];
  assert.equal(preset.value, '9', 'the preset select follows the edited device');
  assert.equal(m.Back?.command, 'Return');
  assert.equal(m.DirectionUp?.deviceId, '9');
  assert.equal(app.remoteMapData.devices['7'], undefined);
  preset.remove();
});

await t.check('legacy UI: a Smart Control slot hides the per-device scope and leaves device scope', async () => {
  const app = legacyApp();
  app.selectedRemoteSlot = 3;
  app.remoteContextMode = 'device';
  const wrap = el('div', 'ctxWrap');
  const ctxSel = el('select', 'remoteContextModeSelect', wrap);
  for (const v of ['activity', 'device']) { const o = document.createElement('option'); o.value = v; ctxSel.appendChild(o); }
  ctxSel.value = 'device';
  const aW = el('div', 'remoteActivitySelectWrap'); aW.style.display = 'none';
  const dW = el('div', 'remoteDeviceSelectWrap'); dW.style.display = 'block';
  rfModel = 'smart_control';
  await app.loadRemoteView();
  assert.equal(wrap.style.display, 'none');
  assert.equal(app.remoteContextMode, 'activity');
  assert.equal(ctxSel.value, 'activity');
  assert.equal(aW.style.display, 'block');
  assert.equal(dW.style.display, 'none');
  rfModel = 'juniper';
  await app.loadRemoteView();
  assert.equal(wrap.style.display, '', 'a touchscreen remote keeps the scope choice');
  wrap.remove(); aW.remove(); dW.remove();
});

// ── New UI ──
const { createButtonEditor } = await import('../frontend/src/views/setup/remote-buttons.js');

await t.check('new UI: no device bindings for a remote without a screen', async () => {
  const editor = createButtonEditor();
  body.appendChild(editor.element);
  await editor.load(3);
  const ctxValues = () => [...editor.element.querySelectorAll('option')].map(o => o.value).filter(v => /^[ad]:/.test(v));
  assert.ok(ctxValues().includes('d:7'), 'the touchscreen editor lists the device');
  editor.setScreenless(true);
  assert.ok(!ctxValues().some(v => v.startsWith('d:')), `device contexts left: ${ctxValues()}`);
  editor.setScreenless(false);
  assert.ok(ctxValues().includes('d:7'));
  editor.element.remove();
});

console.warn = quietWarn;
t.finish();
