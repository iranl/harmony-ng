// Both web UIs: "Forget" on a Bluetooth host calls POST /api/bt-forget-host with its address, and
// the texts of the button and its confirmation exist in every language.
// Runs in Node: no browser, no Hub.
import assert from 'node:assert/strict';

let failures = 0;
async function check(name, fn) {
  try {
    await fn();
    console.log(`    ok   ${name}`);
  } catch (err) {
    failures++;
    console.log(`    FAIL ${name}: ${err.message}`);
  }
}

const ADDR = '00:11:22:33:44:01'; // test address, not a real device

await check('new UI: forgetBtHost posts the address', async () => {
  const { createBluetoothApi } = await import('../frontend/src/api/bluetooth.js');
  const calls = [];
  const api = createBluetoothApi({ postJson: async (path, body) => { calls.push([path, body]); return { ok: true }; } });
  assert.equal(typeof api.forgetBtHost, 'function', 'no forgetBtHost');
  await api.forgetBtHost(ADDR);
  assert.deepEqual(calls, [['/api/bt-forget-host', { addr: ADDR }]]);
});

await check('legacy UI: forgetBtHost posts the address', async () => {
  const { HarmonyClientActivitiesDevices } = await import('../android-app/src/api/api_activities_devices.js');
  const proto = HarmonyClientActivitiesDevices.prototype;
  assert.equal(typeof proto.forgetBtHost, 'function', 'no forgetBtHost');
  const calls = [];
  await proto.forgetBtHost.call({ postJson: async (path, body) => { calls.push([path, body]); return { ok: true }; } }, ADDR);
  assert.deepEqual(calls, [['/api/bt-forget-host', { addr: ADDR }]]);
});

const KEYS = ['bt_forget_host', 'bt_forget_host_title', 'bt_forget_host_confirm', 'bt_host_forgotten', 'bt_forget_failed'];
for (const app of ['frontend', 'android-app']) {
  for (const lang of ['en', 'de', 'fr', 'nl', 'es', 'it']) {
    await check(`${app} ${lang}: the Forget texts exist`, async () => {
      const dict = (await import(`../${app}/src/i18n/${lang}.js`)).default;
      for (const k of KEYS) assert.ok(typeof dict[k] === 'string' && dict[k].length, `missing ${k}`);
      assert.ok(dict.bt_forget_host_confirm.includes('{name}') && dict.bt_forget_host_confirm.includes('{addr}'), 'confirmation without {name} or {addr}');
    });
  }
}

if (failures) {
  console.log(`[-] test_frontend_bt_forget: ${failures} failed`);
  process.exit(1);
}
console.log('[+] test_frontend_bt_forget: both UIs can forget a Bluetooth host');
