// New web UI (frontend/): the key editor shows the binding the Hub falls back to for a key the
// running activity does not bind. Same order as rf_keymap.c: "-1", "default", then the map's
// only activity entry; with several entries and neither of the first two, nothing.
import assert from 'node:assert/strict';
import { defaultBindings, defaultBinding } from '../frontend/src/views/setup/remote-key-fallback.js';

let failures = 0;
function check(name, fn) {
  try {
    fn();
    console.log(`    ok   ${name}`);
  } catch (err) {
    failures++;
    console.log(`    FAIL ${name}: ${err.message}`);
  }
}

const vol = (dev) => ({ VolumeUp: { action: 'device_cmd', deviceId: dev, command: 'VolumeUp' } });

console.log('[-] New web UI key editor fallback');
check('"-1" entry first', () => {
  assert.equal(defaultBindings({ 3: vol('1'), '-1': vol('9'), default: vol('8') })?.VolumeUp.deviceId, '9');
});
check('then "default"', () => {
  assert.equal(defaultBindings({ 3: vol('1'), 2: vol('2'), default: vol('8') })?.VolumeUp.deviceId, '8');
});
check('several activities without "-1" or "default": nothing', () => {
  assert.equal(defaultBindings({ 3: vol('1'), 2: vol('2'), 1: vol('3') }), undefined);
});
check('a stock activity id is not special', () => {
  assert.equal(defaultBindings({ 3: vol('1'), 53591842: vol('2') }), undefined);
});
check('a map with one activity entry: that entry', () => {
  assert.equal(defaultBindings({ 3: vol('1') })?.VolumeUp.deviceId, '1');
});
check('an empty entry is not an activity entry', () => {
  assert.equal(defaultBindings({ 3: vol('1'), 2: {} })?.VolumeUp.deviceId, '1');
});
check('"-1" present but null stops the search, as on the Hub', () => {
  assert.equal(defaultBindings({ 3: vol('1'), '-1': null }), undefined);
});
check('"default" present but null stops the search', () => {
  assert.equal(defaultBindings({ 3: vol('1'), default: null }), undefined);
});
check('a key bound to null in the fallback entry binds nothing', () => {
  assert.equal(defaultBinding({ '-1': { VolumeUp: null } }, 'VolumeUp'), undefined);
  assert.equal(defaultBinding({ '-1': vol('9') }, 'VolumeUp')?.deviceId, '9');
});
check('no map: nothing', () => {
  assert.equal(defaultBindings({}), undefined);
  assert.equal(defaultBindings(undefined), undefined);
});

if (failures) {
  console.log(`[-] test_frontend_key_fallback: ${failures} check(s) failed`);
  process.exit(1);
}
console.log('[+] test_frontend_key_fallback passed');
