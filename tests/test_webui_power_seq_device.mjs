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

await t.check('the power controls (power sequences, power-on delay, keep powered on) show for every device type', () => {
  // main showed them only inside the IR-only timing section, so a Shield's Home / Sleep steps
  // could not be opened: they must sit outside every section that JS hides by device type
  const html = readFileSync(new URL('../android-app/index.html', import.meta.url), 'utf8');
  const timings = html.indexOf('id="devEditTimingsBox"');
  const power = html.indexOf('id="devEditPowerBox"');
  assert.ok(power > 0, 'no power section of its own');
  for (const id of ['btnOpenPowerSeqModal', 'devPowerOnDelay', 'devIsPowerAlwaysOn']) {
    const at = html.indexOf(`id="${id}"`);
    assert.ok(at > power, `${id} is not in the power section`);
    assert.ok(!(at > timings && at < power), `${id} is still in the IR-only timing section`);
  }
  for (const file of ['app_devices_studio.js', 'app_events_nav.js', 'app_core.js']) {
    const src = readFileSync(new URL(`../android-app/src/app/${file}`, import.meta.url), 'utf8');
    assert.ok(!/devEditPowerBox[^;]*style\.display/.test(src), `${file} hides the power section`);
  }
});

await t.check('closing the dialog forgets its device, and the section headings match their content', () => {
  const studio = readFileSync(new URL('../android-app/src/app/app_devices_studio.js', import.meta.url), 'utf8');
  const close = studio.slice(studio.indexOf('closePowerSequenceModal() {'));
  assert.ok(studio.includes('closePowerSequenceModal() {'), 'no close method');
  assert.match(close.slice(0, close.indexOf('\n  }')), /this\.powerSeqOwnerId = null/, 'the close does not clear the device');
  const nav = readFileSync(new URL('../android-app/src/app/app_events_nav.js', import.meta.url), 'utf8');
  for (const id of ['btnPowerSeqClose', 'btnPowerSeqCancel']) {
    const h = nav.slice(nav.indexOf(`'${id}'`), nav.indexOf(`'${id}'`) + 200);
    assert.match(h, /this\.closePowerSequenceModal\(\)/, `${id} does not use the close method`);
  }
  const html = readFileSync(new URL('../android-app/index.html', import.meta.url), 'utf8');
  assert.ok(!html.includes('data-i18n="device_timings_power"'), 'the IR timing section still says "Timings & Power"');
  const power = html.slice(html.indexOf('id="devEditPowerBox"'));
  assert.ok(power.indexOf('data-i18n="device_power"') > 0 && power.indexOf('data-i18n="device_power"') < power.indexOf('devPowerOnDelay'),
    'the power section has no heading');
  for (const lang of ['en', 'de', 'es', 'fr', 'it', 'nl']) {
    const src = readFileSync(new URL(`../android-app/src/i18n/${lang}.js`, import.meta.url), 'utf8');
    assert.ok(src.includes('"device_timings":') && src.includes('"device_power":'), `${lang}: heading strings missing`);
  }
});

t.finish();
