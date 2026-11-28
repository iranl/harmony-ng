// Both web UIs: Test Live (POST /api/activity-test-sequence) sends the steps as JSON text, held
// steps included, and waits for the Hub to send them all (their delays and holds on top of the
// usual 12 s); a step's hold is kept when the activities are read back for the editor.
// Runs in Node: no browser, no Hub.
import assert from 'node:assert/strict';
import { createActivitiesApi, testSequenceTimeoutMs as legacyTimeout } from '../frontend/src/api/activities.js';
import { HarmonyClientActivitiesDevices, testSequenceTimeoutMs as appTimeout } from '../android-app/src/api/api_activities_devices.js';

const steps = [
  { type: 'Command', deviceId: '7', command: 'PowerToggle', delay: 500, hold: 2000 },
  { type: 'Delay', deviceId: '0', command: '', delay: 9000 },
  { type: 'Command', deviceId: '7', command: 'Ok', delay: 0, hold: 60000 },
];
// 12 s, then per step 100 ms and its delay (at most 3 s) and hold (at most 10 s)
const expected = 12000 + (100 + 500 + 2000) + (100 + 3000) + (100 + 0 + 10000);

for (const [name, timeoutMs] of [['legacy UI', legacyTimeout], ['app', appTimeout]]) {
  assert.equal(timeoutMs(steps), expected, `${name}: the timeout counts every delay and hold`);
  assert.equal(timeoutMs(JSON.stringify(steps)), expected, `${name}: the same for the steps as JSON text`);
  assert.equal(timeoutMs('Delay|||3000'), 12000, `${name}: text lines keep the usual timeout`);
}

// legacy UI: the views pass JSON text
{
  const sent = [];
  const api = createActivitiesApi({ postJson: (path, data, opts) => { sent.push({ path, data, opts }); return {}; } });
  await api.testActivitySequence(JSON.stringify(steps));
  assert.equal(sent[0].path, '/api/activity-test-sequence');
  assert.deepEqual(JSON.parse(sent[0].data.steps), steps, 'legacy UI: the steps go out as JSON text, holds included');
  assert.equal(sent[0].opts.timeout, expected, 'legacy UI: Test Live waits for the whole sequence');
}

// app: the editor passes the array of steps (it went out as "[object Object]")
{
  const sent = [];
  const client = Object.create(HarmonyClientActivitiesDevices.prototype);
  client.postJson = (path, data, opts) => { sent.push({ path, data, opts }); return {}; };
  await client.testActivitySequence(steps);
  assert.equal(typeof sent[0].data.steps, 'string');
  assert.deepEqual(JSON.parse(sent[0].data.steps), steps, 'app: the steps go out as JSON text, holds included');
  assert.equal(sent[0].opts.timeout, expected, 'app: Test Live waits for the whole sequence');
}

// both UIs: a held step keeps its hold when the activities are read for the editor
const hubActivities = {
  activities: [{
    id: '18', name: 'Watch TV',
    startSequence: [{ type: 'Command', deviceId: '7', command: 'PowerToggle', delay: 0, hold: 2000 }],
    stopSequence: [{ type: 'Command', deviceId: '7', command: 'PowerToggle', delay: 0 }],
  }],
};
{
  const api = createActivitiesApi({ request: async (path) => (path === '/api/activities' ? hubActivities : null) });
  const { activities } = await api.getActivities();
  assert.equal(activities[0].startSequence[0].hold, 2000, 'legacy UI: a held step keeps its hold');
  assert.ok(!('hold' in activities[0].stopSequence[0]), 'legacy UI: a tap gets no hold');
}
{
  const client = Object.create(HarmonyClientActivitiesDevices.prototype);
  client.request = async (path) => (path === '/api/activities' ? hubActivities : null);
  const res = await client.getActivities();
  const acts = res.activities || res;
  assert.equal(acts[0].startSequence[0].hold, 2000, 'app: a held step keeps its hold');
  assert.ok(!('hold' in acts[0].stopSequence[0]), 'app: a tap gets no hold');
}

console.log('all Test Live client checks passed');
