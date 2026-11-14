// New web UI (frontend/): the Wi-Fi form asks the Hub to test the network first.
// Runs in Node with a stubbed fetch: no Hub, no network.
import assert from 'node:assert/strict';
import { HarmonyClientCore } from '../frontend/src/api/client.js';
import { createSystemApi } from '../frontend/src/api/system.js';
import { wifiPassphraseError } from '../frontend/src/views/setup/network.js';

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

function makeClient(reply, status = 200) {
  // No hub address: no WebSocket is opened, and fetch below is a stub
  const client = new HarmonyClientCore();
  Object.assign(client, createSystemApi(client));
  const seen = { body: null, timeouts: [] };
  globalThis.fetch = async (url, opts) => {
    seen.body = new URLSearchParams(opts.body);
    return new Response(JSON.stringify(reply), { status });
  };
  const realSetTimeout = globalThis.setTimeout;
  globalThis.setTimeout = (fn, ms, ...args) => {
    seen.timeouts.push(ms);
    return realSetTimeout(fn, ms, ...args);
  };
  seen.restore = () => { globalThis.setTimeout = realSetTimeout; };
  return { client, seen };
}

console.log('[-] New web UI Wi-Fi save');

await check('saveWifi asks the Hub to test before saving', async () => {
  const { client, seen } = makeClient({ ok: true, message: 'Wi-Fi settings verified and applied' });
  try {
    const res = await client.saveWifi({ ssid: 'Home', password: 'secret123' });
    assert.equal(seen.body.get('apply'), 'reconfigure');
    assert.equal(seen.body.get('ssid'), 'Home');
    assert.equal(res.message, 'Wi-Fi settings verified and applied');
  } finally {
    seen.restore();
  }
});

await check('saveWifi waits long enough for the Hub test (about 15 s)', async () => {
  const { client, seen } = makeClient({ ok: true });
  try {
    await client.saveWifi({ ssid: 'Home', password: 'secret123' });
    assert.ok(Math.max(...seen.timeouts) >= 30000, `request timeout ${Math.max(...seen.timeouts)} ms`);
  } finally {
    seen.restore();
  }
});

await check('a failed Hub test reaches the caller as an error', async () => {
  const { client, seen } = makeClient({ ok: false, error: 'Wi-Fi test connection failed: Incorrect Wi-Fi password. Settings not changed.' }, 400);
  try {
    await assert.rejects(client.saveWifi({ ssid: 'Home', password: 'wrongpass' }), /Settings not changed/);
  } finally {
    seen.restore();
  }
});

await check('pass phrase length is checked before sending', async () => {
  assert.equal(wifiPassphraseError(''), null);
  assert.equal(wifiPassphraseError('12345678'), null);
  assert.equal(wifiPassphraseError('a'.repeat(63)), null);
  assert.equal(wifiPassphraseError('ab'.repeat(32)), null);
  assert.notEqual(wifiPassphraseError('short'), null);
  assert.notEqual(wifiPassphraseError('x'.repeat(64)), null);
});

if (failures) {
  console.log(`[-] ${failures} check(s) failed`);
  process.exit(1);
}
console.log('[+] New web UI Wi-Fi save: all checks passed');
