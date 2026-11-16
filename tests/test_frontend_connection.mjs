// New web UI (frontend/): the WebSocket messages the Hub really sends (snapshot, activity_state,
// activity_progress, all with "type") keep the current activity right, and a reconnect after a
// drop reads the state again. Uses the Hub's recorded frames in tests/fixtures/ha/.
// Runs in Node with a stand-in DOM and a stubbed fetch: no browser, no Hub.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { installFakeDom, makeChecker, tick } from './webui_fake_dom.mjs';

installFakeDom();
const { store } = await import('../frontend/src/store.js');
const { api } = await import('../frontend/src/api/index.js');
const { connection } = await import('../frontend/src/utils/connection.js');

const fixture = name => new URL(`./fixtures/ha/${name}`, import.meta.url);
const snapshot = JSON.parse(readFileSync(fixture('snapshot.json'), 'utf8'));
const frames = readFileSync(fixture('ws_activity_switch.ndjson'), 'utf8').trim().split('\n').map(l => JSON.parse(l));

let calls = [];
globalThis.fetch = async (url) => {
  const path = String(url).replace(/^https?:\/\/[^/]+/, '');
  calls.push(path);
  if (path.startsWith('/api/activities')) {
    return new Response(JSON.stringify({ activities: [{ id: '10', name: 'Watch TV' }], currentId: '10', currentName: 'Watch TV' }));
  }
  if (path.startsWith('/api/inventory')) return new Response(JSON.stringify({ devices: [] }));
  return new Response(JSON.stringify({ ok: true }));
};

const t = makeChecker('New web UI WebSocket state');
connection.start();
connection.stop(); // no health poll in the test, the handlers stay installed
store.update({ 'activities.list': [{ id: '10', name: 'Watch TV' }] });

await t.check('the snapshot sets the current activity', async () => {
  store.update({ 'activities.currentId': '10', 'activities.currentName': 'Watch TV' });
  api.onWsMessage(snapshot); // idle in the recording
  assert.equal(store.get('activities.currentId'), '-1');
  const running = { ...snapshot, activity_state: { ...snapshot.activity_state, activity: '10', state: 2, stateName: 'running' } };
  api.onWsMessage(running);
  assert.equal(store.get('activities.currentId'), '10');
  assert.equal(store.get('activities.currentName'), 'Watch TV');
});

await t.check('the snapshot fills the radio, Bluetooth and MQTT status', async () => {
  store.update({ rfStatus: null, btStatus: null, mqttStatus: null });
  api.onWsMessage(snapshot);
  assert.deepEqual(store.get('rfStatus'), snapshot.rf_status);
  assert.deepEqual(store.get('btStatus'), snapshot.bt_status);
  assert.deepEqual(store.get('mqttStatus'), snapshot.mqtt_status);
});

await t.check('activity_state alone moves the current activity', async () => {
  store.update({ 'activities.currentId': '-1' });
  api.onWsMessage({ type: 'activity_state', activity: '10', state: 2, stateName: 'running', busy: false });
  assert.equal(store.get('activities.currentId'), '10');
  api.onWsMessage({ type: 'activity_state', activity: '10', state: 3, stateName: 'stopping', busy: true });
  assert.equal(store.get('activities.transitioning'), true);
  api.onWsMessage({ type: 'activity_state', activity: '-1', state: 0, stateName: 'idle', busy: false });
  assert.equal(store.get('activities.currentId'), '-1');
  assert.equal(store.get('activities.transitioning'), false);
});

await t.check('a recorded switch on and off ends on PowerOff', async () => {
  store.update({ 'activities.currentId': '-1' });
  let seen10 = false;
  for (const f of frames) {
    api.onWsMessage(f);
    if (store.get('activities.currentId') === '10') seen10 = true;
  }
  assert.ok(seen10, 'never showed the started activity');
  assert.equal(store.get('activities.currentId'), '-1');
  assert.equal(store.get('activities.transitioning'), false);
});

await t.check('a WebSocket that comes back after a drop reloads the activities', async () => {
  api.onWsStatus(true);
  await tick(5);
  calls = [];
  store.update({ 'activities.currentId': '-1' });
  api.onWsStatus(false);
  api.onWsStatus(true);
  await tick(20);
  assert.ok(calls.some(p => p.startsWith('/api/activities')), `no reload after the reconnect (${calls.join(', ')})`);
  assert.equal(store.get('activities.currentId'), '10');
});

t.finish();
