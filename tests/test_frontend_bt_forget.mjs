// Both web UIs: "Forget" on a Bluetooth host asks first, then calls POST /api/bt-forget-host with
// the host's address; the confirmation says where the keys of the host's devices go now; the texts
// exist in every language.
// Runs in Node with a minimal stand-in DOM: no browser, no Hub.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

class FakeNode {
  constructor(tag) {
    this.tagName = tag;
    this.children = [];
    this.parentNode = null;
    this.listeners = {};
    this.attrs = {};
    this.style = {};
    this.dataset = {};
    this.textContent = '';
    this.className = '';
    const classes = new Set();
    this.classList = { add: c => classes.add(c), remove: c => classes.delete(c), contains: c => classes.has(c) };
  }
  set innerHTML(v) { this.children = []; this._html = v; }
  get innerHTML() { return this._html || ''; }
  get isConnected() { return true; }
  appendChild(child) { child.parentNode = this; this.children.push(child); return child; }
  removeChild(child) { this.children = this.children.filter(c => c !== child); child.parentNode = null; }
  remove() { if (this.parentNode) this.parentNode.removeChild(this); }
  addEventListener(name, fn) { (this.listeners[name] ||= []).push(fn); }
  removeEventListener() {}
  setAttribute(k, v) { this.attrs[k] = v; if (k === 'id') this.id = v; }
  getAttribute(k) { return this.attrs[k]; }
  querySelector(sel) { return sel.startsWith('#') ? [...this.walk()].find(n => n.id === sel.slice(1)) || null : null; }
  querySelectorAll() { return []; }
  get firstChild() { return this.children[0] || null; }
  *walk() { yield this; for (const c of this.children) yield* c.walk(); }
}

const body = new FakeNode('body');
globalThis.Node = FakeNode;
globalThis.document = {
  body,
  createElement: tag => new FakeNode(tag),
  createTextNode: text => Object.assign(new FakeNode('#text'), { textContent: text }),
  getElementById: id => [...body.walk()].find(n => n.id === id) || null,
};
const store = {};
globalThis.localStorage = { getItem: k => store[k] ?? null, setItem: (k, v) => { store[k] = String(v); }, removeItem: k => { delete store[k]; } };
globalThis.requestAnimationFrame = fn => fn();

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
const tick = () => new Promise(r => setTimeout(r, 0));
const ADDR = '00:11:22:33:44:01'; // test address, not a real device

// ---- new UI: the host list of the Bluetooth view ----
const { api } = await import('../frontend/src/api/index.js');
const { confirm } = await import('../frontend/src/components/confirm.js');
const enNew = (await import('../frontend/src/i18n/en.js')).default;
let forgotten = [];
let asked = [];
let answerYes = true;
api.getBtStatus = async () => ({ state: 'ready', hosts: [{ addr: ADDR, name: 'Old TV', connected: false, paired: true }] });
api.forgetBtHost = async addr => { forgotten.push(addr); return { ok: true }; };
confirm.ask = async opts => { asked.push(opts); return answerYes; };

async function newUiForgetButton() {
  let mod;
  try { mod = await import('../frontend/src/views/setup/bt-hosts.js'); } catch (e) { throw new Error('view does not load: ' + e.message); }
  const view = mod.createView();
  for (let i = 0; i < 5; i++) await tick();
  const btn = [...view.walk()].find(n => n.tagName === 'button' && n.textContent === (enNew.bt_forget_host || 'Forget'));
  assert.ok(btn, 'no Forget button on the host');
  return btn;
}

await check('new UI: Forget asks, then posts the host address', async () => {
  const btn = await newUiForgetButton();
  forgotten = []; asked = []; answerYes = true;
  for (const fn of btn.listeners.click || []) await fn();
  assert.equal(asked.length, 1, 'no confirmation');
  assert.ok(asked[0].danger && asked[0].message.includes('Old TV') && asked[0].message.includes(ADDR), 'confirmation without name or address');
  assert.ok(!asked[0].message.includes('{name}'), 'a {name} left in the text');
  assert.deepEqual(forgotten, [ADDR]);
});

await check('new UI: Cancel forgets nothing', async () => {
  const btn = await newUiForgetButton();
  forgotten = []; answerYes = false;
  for (const fn of btn.listeners.click || []) await fn();
  assert.deepEqual(forgotten, []);
});

await check('new UI: the API client posts the address', async () => {
  const { createBluetoothApi } = await import('../frontend/src/api/bluetooth.js');
  const calls = [];
  const c = createBluetoothApi({ postJson: async (path, b) => { calls.push([path, b]); return { ok: true }; } });
  await c.forgetBtHost(ADDR);
  assert.deepEqual(calls, [['/api/bt-forget-host', { addr: ADDR }]]);
});

// ---- legacy UI ----
await check('legacy UI: the host list has a Forget button wired to the flow', async () => {
  const src = readFileSync(new URL('../android-app/src/app/app_bluetooth.js', import.meta.url), 'utf8');
  assert.ok(/class="btn btn-xs btn-danger btn-bt-forget" data-addr="\$\{escapeHtml\(h\.addr\)\}"/.test(src), 'no Forget button in the host row');
  assert.ok(/querySelectorAll\('\.btn-bt-forget'\)[\s\S]{0,400}forgetBtHostFlow\(/.test(src), 'the button does not run the forget flow');
});

await check('legacy UI: the flow asks, posts, and reports', async () => {
  const { forgetBtHostFlow } = await import('../android-app/src/app/bt_forget.js');
  const enOld = (await import('../android-app/src/i18n/en.js')).default;
  const t = (k, d) => enOld[k] ?? d;
  const posted = [];
  const toasts = [];
  const msgs = [];
  const client = { forgetBtHost: async a => { posted.push(a); return { ok: true }; } };
  const showToast = (m, type) => toasts.push(type);
  let ok = await forgetBtHostFlow({ client, addr: ADDR, name: 'Old TV', t, showConfirm: async m => { msgs.push(m); return true; }, showToast });
  assert.ok(ok && posted[0] === ADDR && toasts[0] === 'success', 'not forgotten');
  assert.ok(msgs[0].includes('Old TV') && msgs[0].includes(ADDR) && !msgs[0].includes('{name}'), 'confirmation text: ' + msgs[0]);
  ok = await forgetBtHostFlow({ client, addr: ADDR, name: 'Old TV', t, showConfirm: async () => false, showToast });
  assert.ok(!ok && posted.length === 1, 'Cancel still forgot');
  const failing = { forgetBtHost: async () => { throw new Error('Bluetooth is off'); } };
  ok = await forgetBtHostFlow({ client: failing, addr: ADDR, name: 'Old TV', t, showConfirm: async () => true, showToast });
  assert.ok(!ok && toasts[toasts.length - 1] === 'error', 'an error is not shown');
});

await check('legacy UI: the API client posts the address', async () => {
  const { HarmonyClientActivitiesDevices } = await import('../android-app/src/api/api_activities_devices.js');
  const calls = [];
  await HarmonyClientActivitiesDevices.prototype.forgetBtHost.call({ postJson: async (path, b) => { calls.push([path, b]); return { ok: true }; } }, ADDR);
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
process.exit(0);
