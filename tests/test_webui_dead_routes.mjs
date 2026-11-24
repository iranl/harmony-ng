// The web UI features that posted to routes the Hub never had now work with routes it has:
// legacy Bluetooth macro script (bt-key / bt-text per step), legacy IR Lab import
// (/api/commands-batch), new UI IR Lab batch run (one send per command) and new UI Harmony DB
// search (archive read in the browser, import with /api/commands-batch).
// Runs in Node with a stand-in DOM and a stubbed fetch: no browser, no Hub, no Internet.
import assert from 'node:assert/strict';
import { installFakeDom, allowJsonImports, makeChecker, tick, buttonByText } from './webui_fake_dom.mjs';

const { body } = installFakeDom();
await allowJsonImports();

const ARCHIVE = {
  'devices/Acme/index.json': [{ m: 'TV-100', f: 'TV-100.json' }, { m: 'Radio-7', f: 'Radio-7.json' }],
  'devices/Acme/TV-100.json': { manufacturer: 'Acme', model: 'TV-100', deviceType: 1, codeset: '/codesets/acme1.json' },
  'codesets/acme1.json': { commands: [{ name: 'PowerOn', pronto: '0000 006D 0001 0000 0010 0020' }, { name: 'NoCode' }] },
};
let calls = [];
globalThis.fetch = async (url, opts = {}) => {
  const u = String(url);
  calls.push({ url: u, method: opts.method || 'GET', body: opts.body });
  const archive = Object.keys(ARCHIVE).find(k => u.endsWith(k));
  if (archive) return new Response(JSON.stringify(ARCHIVE[archive]));
  if (/^https?:\/\/(?!192\.168\.)/.test(u)) return new Response('{}', { status: 404 });
  return new Response(JSON.stringify({ ok: true }));
};
const hubCalls = () => calls.filter(c => !/^https?:\/\/(?!192\.168\.)/.test(c.url)).map(c => ({ ...c, path: c.url.replace(/^https?:\/\/[^/]+/, '') }));

const t = makeChecker('Web UI features without a Hub route');

// ── Legacy: Bluetooth macro script ──
const { HarmonyClient } = await import('../android-app/src/api.js');
const legacy = new HarmonyClient();

await t.check('legacy macro script sends each step as a Bluetooth key or text', async () => {
  calls = [];
  await legacy.runBtScript('TEXT hello world\nWAIT 5\nKEY enter', 0, '00:11:22:33:44:55');
  const sent = hubCalls().map(c => [c.path, JSON.parse(c.body)]);
  assert.deepEqual(sent.map(s => s[0]), ['/api/bt-text', '/api/bt-key']);
  assert.equal(sent[0][1].text, 'hello world');
  assert.equal(sent[1][1].key, 'enter');
  assert.equal(sent[1][1].target, '00:11:22:33:44:55');
});

await t.check('legacy macro script with a bad line sends nothing', async () => {
  calls = [];
  await assert.rejects(legacy.runBtScript('KEY up\nJUMP 3', 0), /line 2/);
  await assert.rejects(legacy.runBtScript('WAIT 99999', 0), /0 to 10000/);
  assert.equal(hubCalls().length, 0);
});

// ── Legacy: IR Lab import ──
const { HarmonyAppLab } = await import('../android-app/src/app/app_lab.js');

await t.check('legacy IR Lab import saves through /api/commands-batch', async () => {
  const app = Object.create(HarmonyAppLab.prototype);
  app.client = legacy;
  app.inventory = { devices: [{ id: '3', name: 'TV', controlPort: 2 }] };
  app.labQueue = [{ name: 'Power', raw: '0000 006D' }, { name: 'Input', keycode: 'NEC:0x04:0x0B' }];
  app.refreshData = async () => {};
  const sel = document.createElement('select');
  sel.setAttribute('id', 'labDevice');
  sel.value = '3';
  body.appendChild(sel);
  [0, 1].forEach(i => {
    const cb = document.createElement('input');
    cb.className = 'lab-pick';
    cb.setAttribute('data-index', String(i));
    cb.checked = true;
    body.appendChild(cb);
  });
  globalThis.alert = () => {};
  calls = [];
  await app.importLabSelected();
  const post = hubCalls().find(c => c.path === '/api/commands-batch');
  assert.ok(post, `no batch save (${hubCalls().map(c => c.path).join(', ')})`);
  const sent = JSON.parse(post.body);
  assert.equal(sent.deviceId, '3');
  assert.deepEqual(sent.commands.map(c => [c.name, c.raw, c.port]), [['Power', '0000 006D', undefined], ['Input', 'NEC:0x04:0x0B', undefined]],
    'no port: an imported command follows the device');
});

// ── New UI: IR Lab batch run ──
const { store } = await import('../frontend/src/store.js');
store.set('devices', [{ id: '3', name: 'TV', type: 'IR', controlPort: 7 }]);
const { createView: createIrLab } = await import('../frontend/src/views/setup/ir-lab.js');

await t.check('new UI IR Lab sends each command in turn', async () => {
  const view = createIrLab();
  body.appendChild(view);
  view.querySelector('textarea').value = 'PowerOn\n\nInputHdmi1\n';
  view.querySelector('input').value = '0';
  calls = [];
  await buttonByText(view, '▶ Run Batch Send').fire('click');
  const sends = hubCalls().filter(c => c.path === '/api/ir-send').map(c => JSON.parse(c.body));
  assert.deepEqual(sends.map(s => [s.deviceId, s.command]), [['3', 'PowerOn'], ['3', 'InputHdmi1']]);
  view.remove();
});

await t.check('new UI IR Lab abort stops the run', async () => {
  const view = createIrLab();
  body.appendChild(view);
  view.querySelector('textarea').value = 'A\nB\nC';
  view.querySelector('input').value = '50';
  calls = [];
  const running = buttonByText(view, '▶ Run Batch Send').fire('click');
  await tick(10);
  await buttonByText(view, '⏹ Abort Run').fire('click');
  await running;
  assert.equal(hubCalls().filter(c => c.path === '/api/ir-send').length, 1);
  view.remove();
});

await t.check('new UI IR Lab can run again after a send fails in an odd way', async () => {
  const { api } = await import('../frontend/src/api/index.js');
  const send = api.sendCommand;
  const view = createIrLab();
  body.appendChild(view);
  view.querySelector('textarea').value = 'A';
  view.querySelector('input').value = '0';
  api.sendCommand = () => Promise.reject(undefined); // not an Error: no .message
  try {
    await buttonByText(view, '▶ Run Batch Send').fire('click').catch(() => {});
  } finally {
    api.sendCommand = send;
  }
  calls = [];
  await buttonByText(view, '▶ Run Batch Send').fire('click');
  assert.equal(hubCalls().filter(c => c.path === '/api/ir-send').length, 1, 'the next run was refused');
  view.remove();
});

// ── New UI: Harmony DB search ──
const { createView: createSearch } = await import('../frontend/src/views/setup/harmony-search.js');

await t.check('new UI Harmony DB search imports the codes into a device', async () => {
  const view = createSearch();
  body.appendChild(view);
  view.querySelector('#search-mfg-input').value = 'Acme';
  view.querySelector('#search-model-input').value = 'tv';
  calls = [];
  await buttonByText(view, '🔍 Search Harmony Database').fire('click');
  assert.ok(!hubCalls().some(c => c.path.startsWith('/api/harmony')), 'still asks the Hub to search');
  const load = buttonByText(view, 'Load Codes');
  assert.ok(load, `no result row (${view.querySelector('#search-results-mount').textContent})`);
  await load.fire('click');
  await buttonByText(view, 'Import').fire('click');
  const post = hubCalls().find(c => c.path === '/api/commands-batch');
  assert.ok(post, `no import (${hubCalls().map(c => c.path).join(', ')})`);
  const sent = JSON.parse(post.body);
  assert.equal(sent.deviceId, '3');
  assert.deepEqual(sent.commands, [{ name: 'PowerOn', raw: '0000 006D 0001 0000 0010 0020', repeats: 3 }]);
  view.remove();
});

t.finish();
