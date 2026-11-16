// New web UI (frontend/): the Homatics B25 page shows what each key does from the Hub's
// bt_remote_map.json, and has no "Save Mapping" button that saves nothing.
// Runs in Node with a stand-in DOM and a stubbed fetch: no browser, no Hub.
import assert from 'node:assert/strict';
import { installFakeDom, makeChecker, tick, buttonByText } from './webui_fake_dom.mjs';

const { body } = installFakeDom();
const { store } = await import('../frontend/src/store.js');
const { createView } = await import('../frontend/src/views/setup/b25-remote.js');

const MAP = {
  remote: { name: 'Homatics B25', bdaddr: '00:11:22:33:44:55' },
  activities: {
    '-1': { power: { action: 'device_cmd', deviceId: '3', command: 'PowerToggle' } },
    10: { vol_up: { action: 'device_cmd', deviceId: '1', command: 'VolumeUp' }, home: { action: 'activity_stop' } },
  },
};
let calls = [];
globalThis.fetch = async (url, opts = {}) => {
  calls.push({ path: String(url), method: opts.method || 'GET' });
  return new Response(JSON.stringify(String(url).includes('/api/remote-mapping') ? MAP : { ok: true }));
};
store.update({
  devices: [{ id: '1', name: 'Soundbar' }, { id: '3', name: 'TV' }],
  'activities.list': [{ id: '10', name: 'Watch TV' }],
  'activities.currentId': '10',
});

const t = makeChecker('New web UI B25 remote page');
const view = createView();
body.appendChild(view);
await tick(10);
const badge = key => view.querySelector('#b25-mapping-content').children[0].children
  .find(r => r.dataset.key === key)?.children[1].textContent;

await t.check('each key shows its binding in the running activity', async () => {
  assert.equal(badge('vol_up'), 'Soundbar › VolumeUp');
  assert.equal(badge('home'), 'Turn everything off');
});

await t.check('a key not set in the activity shows the default binding', async () => {
  assert.equal(badge('power'), 'TV › PowerToggle (default)');
  assert.equal(badge('mute'), 'Not mapped');
});

await t.check('there is no Save button that saves nothing', async () => {
  assert.ok(!buttonByText(view, 'Save Mapping'), 'a Save Mapping button is still there');
});

t.finish();
