// Both web UIs' remote key editors: when the slot's key map cannot be read, the editor must not
// save. Save posts the whole map and the Hub replaces remote_slotN_map.json with it, so a save
// after a failed read would drop every other binding of the slot.
// Runs in Node with a stand-in DOM and a stubbed fetch: no browser, no Hub.
import assert from 'node:assert/strict';
import { installFakeDom, allowJsonImports, makeChecker, texts, buttonByText } from './webui_fake_dom.mjs';

const { body } = installFakeDom();
await allowJsonImports();
const quietWarn = console.warn;
console.warn = () => {}; // the renderer is not in this build: it warns once

let calls = [];
let mapReply = { status: 503, body: { ok: false, error: 'Service Unavailable' } };
globalThis.fetch = async (url, opts = {}) => {
  const path = String(url).replace(/^https?:\/\/[^/]+/, '');
  calls.push({ path, method: opts.method || 'GET', body: opts.body });
  let r = { body: { ok: true } };
  if (path.startsWith('/api/rf-mapping?')) r = mapReply;
  else if (path.startsWith('/api/devices')) r = { body: { devices: [{ id: '3', name: 'TV', commands: [{ name: 'VolumeUp' }] }] } };
  return new Response(JSON.stringify(r.body), { status: r.status || 200 });
};
const saves = () => calls.filter(c => c.path.startsWith('/api/rf-mapping-save'));
const HUB_MAP = { activities: { 10: { VolumeUp: { type: 'command', deviceId: '3', command: 'VolumeUp' } } }, devices: {} };

const t = makeChecker('Web UI key editors after a failed key map read');

// ── New UI (frontend/src/views/setup/remote-buttons.js) ──
const { createButtonEditor } = await import('../frontend/src/views/setup/remote-buttons.js');

await t.check('new UI: a failed read shows the error and does not save', async () => {
  const editor = createButtonEditor();
  body.appendChild(editor.element);
  calls = [];
  mapReply = { status: 503, body: { ok: false, error: 'Service Unavailable' } };
  await editor.load(1);
  assert.ok(texts(body, 'toast-message').some(m => /Service Unavailable/.test(m)), 'the read error is not shown');
  const save = buttonByText(editor.element, 'Save to Hub');
  assert.equal(save.disabled, true, 'Save stays enabled');
  await save.fire('click');
  assert.equal(saves().length, 0, `the empty map was saved: ${saves()[0]?.body}`);
  editor.element.remove();
});

await t.check('new UI: the Hub\'s map and its "no map yet" reply still save', async () => {
  const editor = createButtonEditor();
  body.appendChild(editor.element);
  mapReply = { body: HUB_MAP };
  await editor.load(1);
  calls = [];
  const save = buttonByText(editor.element, 'Save to Hub');
  assert.equal(save.disabled, false);
  await save.fire('click');
  assert.equal(saves().length, 1);
  assert.deepEqual(JSON.parse(saves()[0].body).activities, HUB_MAP.activities);
  mapReply = { body: { ok: true, mappings: [] } };
  await editor.load(2);
  calls = [];
  await save.fire('click');
  assert.equal(saves().length, 1, 'an empty slot cannot be saved');
  editor.element.remove();
});

// ── Legacy UI (android-app/src/app/app_remotes_screen.js) ──
const { HarmonyAppRemotesScreen } = await import('../android-app/src/app/app_remotes_screen.js');
const { HarmonyClient } = await import('../android-app/src/api.js');

function legacyApp() {
  const app = Object.create(HarmonyAppRemotesScreen.prototype);
  app.client = new HarmonyClient();
  app.selectedRemoteSlot = 1;
  return app;
}

function legacySaveButton() {
  let btn = document.getElementById('remoteSaveAllBtn');
  if (!btn) {
    btn = document.createElement('button');
    btn.setAttribute('id', 'remoteSaveAllBtn');
    btn.textContent = 'Save All to Hub';
    body.appendChild(btn);
  }
  return btn;
}

await t.check('legacy UI: a failed read does not save', async () => {
  const app = legacyApp();
  const btn = legacySaveButton();
  mapReply = { status: 503, body: { ok: false, error: 'Service Unavailable' } };
  await app.loadRemoteMap();
  assert.equal(btn.disabled, true, 'Save All stays enabled');
  calls = [];
  await app.saveAllRemoteMappings();
  assert.equal(saves().length, 0, `the empty map was saved: ${saves()[0]?.body}`);
});

await t.check('legacy UI: a good read saves the Hub\'s map', async () => {
  const app = legacyApp();
  const btn = legacySaveButton();
  mapReply = { body: HUB_MAP };
  await app.loadRemoteMap();
  assert.equal(btn.disabled, false);
  calls = [];
  await app.saveAllRemoteMappings();
  assert.equal(saves().length, 1);
  assert.deepEqual(JSON.parse(saves()[0].body).activities, HUB_MAP.activities);
});

console.warn = quietWarn;
t.finish();
