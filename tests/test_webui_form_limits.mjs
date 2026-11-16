// Legacy web UI (android-app/): the forms check the limits the Hub enforces before sending, so a
// user name the Hub cannot keep (and that could never log in), a Wi-Fi pass phrase WPA refuses
// or a device delay over 10 s is caught with a reason instead of being sent.
// Runs in Node with a stand-in DOM and a stubbed fetch: no browser, no Hub.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { installFakeDom, allowJsonImports, makeChecker } from './webui_fake_dom.mjs';

const { body } = installFakeDom();
await allowJsonImports();
let calls = [];
globalThis.fetch = async (url, opts = {}) => {
  calls.push({ path: String(url).replace(/^https?:\/\/[^/]+/, ''), method: opts.method || 'GET', body: opts.body });
  return new Response(JSON.stringify({ ok: true, users: [], tokens: [] }));
};
const posts = () => calls.filter(c => c.method === 'POST');

const { HarmonyAppHubsNetwork } = await import('../android-app/src/app/app_hubs_network.js');
const { HarmonyClient } = await import('../android-app/src/api.js');
const limits = await import('../android-app/src/app/form_limits.js');

function app() {
  const a = Object.create(HarmonyAppHubsNetwork.prototype);
  a.client = new HarmonyClient();
  a.loadUserAccounts = async () => {};
  a.checkAuthStatus = () => {};
  a.applyRoleRestrictions = () => {};
  a.showLoginModal = () => {};
  return a;
}
function input(id, value, extra = {}) {
  let el = document.getElementById(id);
  if (!el) { el = document.createElement('input'); el.setAttribute('id', id); body.appendChild(el); }
  el.value = value;
  Object.assign(el, extra);
  return el;
}

const t = makeChecker('Legacy web UI form limits');

await t.check('a user name of 32 bytes or more is not sent', async () => {
  input('newUserName', 'living-room-harmony-hub-owner-2026');
  input('newUserPassword', 'secret');
  input('newUserStatus', '');
  calls = [];
  await app().addNonAdminUser();
  assert.equal(posts().length, 0, `sent: ${posts().map(c => c.path).join(', ')}`);
  assert.match(document.getElementById('newUserStatus').innerHTML, /too long/);
});

await t.check('the admin account card refuses such a name before turning auth on', async () => {
  input('authEnabledCheck', '', { checked: true });
  input('authUsername', 'living-room-harmony-hub-owner-2026');
  input('authPassword', 'secret');
  input('authSaveStatus', '');
  calls = [];
  await app().saveWebUiAuth();
  assert.equal(posts().length, 0, `sent: ${posts().map(c => c.path).join(', ')}`);
});

await t.check('a short Wi-Fi pass phrase is not sent, a good one is', async () => {
  input('wifiSsidInput', 'HomeNet');
  input('wifiPskInput', 'short');
  input('wifiOpen', '', { checked: false });
  input('wifiSaveStatus', '');
  calls = [];
  await app().saveWifiConfig('save');
  assert.equal(posts().length, 0, 'the short pass phrase was sent');
  input('wifiPskInput', 'long enough phrase');
  await app().saveWifiConfig('save');
  assert.equal(posts().length, 1);
});

await t.check('delay and name limits match the Hub', async () => {
  assert.equal(limits.deviceDelayError('Inter-key delay', 10000), null);
  assert.match(limits.deviceDelayError('Inter-key delay', 150000), /0 to 10000/);
  assert.match(limits.deviceDelayError('Inter-key delay', -5), /0 to 10000/);
  assert.equal(limits.credentialsError('a'.repeat(31), 'x'), null);
  assert.match(limits.credentialsError('é'.repeat(16), 'x'), /too long/); // 32 bytes
  assert.match(limits.credentialsError('ab:c', 'x'), /":"/);
  assert.match(limits.credentialsError('abc', 'p'.repeat(129)), /Password too long/);
  assert.equal(limits.wifiPassphraseError('g'.repeat(64)), 'Wi-Fi password must be 8 to 63 characters (or 64 hex digits)');
  assert.equal(limits.wifiPassphraseError('ab'.repeat(32).replace(/b/g, 'f')), null);
  assert.equal(limits.wifiPassphraseError('', { open: false }), null);
});

await t.check('the user name fields and delay fields carry the limits', async () => {
  const html = readFileSync(new URL('../android-app/index.html', import.meta.url), 'utf8');
  for (const id of ['authUsername', 'newUserName', 'loginUsername']) {
    assert.match(html, new RegExp(`id="${id}"[^>]*maxlength="31"`), `${id} has no maxlength`);
  }
  for (const id of ['devInterDeviceDelay', 'devInterKeyDelay', 'devHoldInterKeyDelay']) {
    assert.match(html, new RegExp(`id="${id}"[^>]*max="10000"`), `${id} max differs from the Hub's 10000`);
  }
});

t.finish();
