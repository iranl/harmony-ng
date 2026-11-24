// Legacy web UI, device power dialog (Edit Device Details > Configure Power On / Off Sequences):
// every step row shows the device the dialog is for, locked. The Hub sends a device's power steps
// to that device whatever their deviceId says, and migrated steps store "0"; the rows used to show
// the first device in the list (the soundbar on a Shield), and a new step offered its commands.
// Runs in Node with a stand-in DOM: no browser, no Hub.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { installFakeDom, allowJsonImports, makeChecker } from './webui_fake_dom.mjs';

installFakeDom();
await allowJsonImports();
const quietWarn = console.warn;
console.warn = () => {};
const { stepDeviceChoice } = await import('../android-app/src/app/app_activities_editor.js');
console.warn = quietWarn;

const DEVICES = [{ id: '1', name: 'Soundbar' }, { id: '2', name: 'Shield' }, { id: '3', name: 'TV' }];
const t = makeChecker('Legacy web UI: device power dialog shows its own device');

await t.check('migrated power step ("0") shows the dialog\'s device, locked', () => {
  for (const list of ['powerOnStepsList', 'powerOffStepsList']) {
    assert.deepEqual(stepDeviceChoice(list, { command: 'Home', deviceId: '0' }, DEVICES, '2'),
      { devId: '2', locked: true }, list);
  }
});

await t.check('a power step saved with another device still shows the dialog\'s device', () => {
  assert.deepEqual(stepDeviceChoice('powerOnStepsList', { command: 'Home', deviceId: '1' }, DEVICES, '2'),
    { devId: '2', locked: true });
});

await t.check('a new power step (+ Add Command) starts on the dialog\'s device', () => {
  assert.deepEqual(stepDeviceChoice('powerOffStepsList', null, DEVICES, '2'), { devId: '2', locked: true });
});

await t.check('activity steps are unchanged: own device, else the first one, not locked', () => {
  assert.deepEqual(stepDeviceChoice('actStartStepsList', { deviceId: '3' }, DEVICES, '2'), { devId: '3', locked: false });
  assert.deepEqual(stepDeviceChoice('actStartStepsList', null, DEVICES, '2'), { devId: '1', locked: false });
  assert.deepEqual(stepDeviceChoice('actStartStepsList', null, [], undefined), { devId: '', locked: false });
});

await t.check('the dialog records its device before it draws the rows, and the select is locked', () => {
  const studio = readFileSync(new URL('../android-app/src/app/app_devices_studio.js', import.meta.url), 'utf8');
  const open = studio.slice(studio.indexOf('openPowerSequenceModal() {'));
  const owner = open.indexOf('this.powerSeqOwnerId = String(dev.id)');
  const draw = open.indexOf("renderSequenceList('powerOnStepsList'");
  assert.ok(owner > 0, 'openPowerSequenceModal does not record the device');
  assert.ok(draw > owner, 'the rows are drawn before the device is recorded');
  const editor = readFileSync(new URL('../android-app/src/app/app_activities_editor.js', import.meta.url), 'utf8');
  assert.match(editor, /step-dev-select" \$\{devLocked \? 'disabled' : ''\}/, 'the device select is not locked');
});

t.finish();
