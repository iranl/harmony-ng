// Both web UIs: a command without its own port follows the device's Blaster Assignment.
// - imports (new UI's Harmony search, legacy IR Lab) send no port with the commands
// - the legacy inventory keeps "port" as the Hub sends it (absent for a command without one),
//   so the Edit Saved Command dialog shows "Default (Device Setting)" and saves 0
// Runs in Node with a stand-in DOM and a stubbed fetch: no browser, no Hub.
import assert from 'node:assert/strict';
import { installFakeDom, allowJsonImports, makeChecker } from './webui_fake_dom.mjs';

const { body } = installFakeDom();
await allowJsonImports();
const quietWarn = console.warn;
console.warn = () => {};

const calls = [];
const INVENTORY = { devices: [{ id: '1', name: 'TV', type: 'IR', controlPort: 4,
  commands: [{ name: 'PowerOn', raw: 'R1' }, { name: 'VolumeUp', raw: 'R2', port: 2 }] }] };
globalThis.fetch = async (url, opts = {}) => {
  const path = String(url).replace(/^https?:\/\/[^/]+/, '');
  calls.push({ path, body: opts.body });
  const r = path.startsWith('/api/inventory') ? INVENTORY : { ok: true };
  return new Response(JSON.stringify(r), { status: 200 });
};

const t = makeChecker('Web UI command port: imports and the saved command dialog');

await t.check('new UI: codesetToBatch sends no port', async () => {
  const { codesetToBatch } = await import('../frontend/src/views/setup/harmony-search.js');
  const batch = codesetToBatch({ commands: [{ name: 'VolUp', pronto: '0000 006D' }] });
  assert.equal(batch.length, 1);
  assert.equal('port' in batch[0], false, JSON.stringify(batch));
});

const { HarmonyClient } = await import('../android-app/src/api.js');

await t.check('legacy UI: the inventory keeps a command port only when the Hub sends one', async () => {
  const inv = await new HarmonyClient().getInventory();
  const cmds = inv.devices[0].commands;
  assert.equal(cmds.find(c => c.name === 'PowerOn').port, undefined);
  assert.equal(cmds.find(c => c.name === 'VolumeUp').port, 2);
});

await t.check('legacy UI: the IR Lab import sends no port', async () => {
  const { HarmonyAppLab } = await import('../android-app/src/app/app_lab.js');
  const app = Object.create(HarmonyAppLab.prototype);
  app.client = new HarmonyClient();
  app.inventory = INVENTORY;
  app.labQueue = [{ name: 'LabKey', raw: 'R9' }];
  const sel = document.createElement('select'); sel.setAttribute('id', 'labDevice');
  const o = document.createElement('option'); o.value = '1'; sel.appendChild(o); sel.value = '1';
  body.appendChild(sel);
  const cb = document.createElement('input'); cb.className = 'lab-pick'; cb.checked = true;
  cb.setAttribute('data-index', '0'); body.appendChild(cb);
  globalThis.alert = () => {};
  calls.length = 0;
  await app.importLabSelected();
  const req = calls.find(c => c.path.startsWith('/api/commands-batch'));
  assert.ok(req, JSON.stringify(calls.map(c => c.path)));
  assert.doesNotMatch(String(req.body), /"port"/, String(req.body));
});

console.warn = quietWarn;
t.finish();
