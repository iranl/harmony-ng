// The "Wi-Fi setup over USB" switch in both web UIs (USB_WIFI_SETUP in ethernet.conf).
// The save route rewrites the whole Ethernet file, so the switch must never reset the Ethernet
// settings, and it must not turn the USB setup back on when the stored value could not be read.
// Runs in Node with a stand-in DOM and a stubbed Hub: no browser, no Hub.
import assert from 'node:assert/strict';
import { installFakeDom, makeChecker, tick, allowJsonImports } from './webui_fake_dom.mjs';

const { body } = installFakeDom();
await allowJsonImports();
globalThis.location.hash = '';

let statusReply = null;          // what GET /api/network-status answers (null = 500)
const posts = [];                // bodies of the saves
let saveStatus = 200;
globalThis.fetch = async (url, opts = {}) => {
  const path = new URL(String(url), 'http://192.168.1.20/').pathname;
  if ((opts.method || 'GET') === 'POST') {
    posts.push({ path, body: new URLSearchParams(opts.body) });
    return new Response(JSON.stringify(saveStatus === 200 ? { ok: true } : { error: 'refused' }),
      { status: saveStatus, headers: { 'Content-Type': 'application/json' } });
  }
  if (path === '/api/network-status' && statusReply) {
    return new Response(JSON.stringify(statusReply), { status: 200, headers: { 'Content-Type': 'application/json' } });
  }
  return new Response('{}', { status: path === '/api/network-status' ? 500 : 404, headers: { 'Content-Type': 'application/json' } });
};

const STORED = {
  ip: '192.168.1.20', mode: 'wifi',
  config: {
    eth_enabled: true, eth_fallback_wifi: true, eth_mode: 'static', eth_ip: '192.168.1.20',
    eth_netmask: '255.255.255.0', eth_gateway: '192.168.1.1', eth_dns: '1.1.1.1', usb_wifi_setup: true,
  },
};

const check = makeChecker('Web UI switch for Wi-Fi setup over USB');
const realError = console.error, realWarn = console.warn;
const quiet = () => { console.error = console.warn = () => {}; };
const loud = () => { console.error = realError; console.warn = realWarn; };

// ---- new web UI (frontend/)
const { createView } = await import('../frontend/src/views/setup/network.js');

async function drawNew() {
  quiet();
  const view = createView();
  body.appendChild(view);
  await tick(20);
  loud();
  const toggle = [...view.walk()].find(n => n.id === 'usb-wifi-setup-toggle');
  return { view, toggle };
}

async function flip(toggle, on) {
  toggle.checked = on;
  quiet();
  await toggle.fire('change');
  await tick(20);
  loud();
}

await check.check('new UI: switch disabled when the stored value cannot be read', async () => {
  statusReply = null;
  const { toggle } = await drawNew();
  assert.ok(toggle, 'switch drawn');
  assert.equal(toggle.disabled, true);
});

await check.check('new UI: switch shows the stored value and turning it off keeps the Ethernet settings', async () => {
  statusReply = STORED;
  posts.length = 0;
  const { toggle } = await drawNew();
  assert.equal(toggle.disabled, false);
  assert.equal(toggle.checked, true);
  await flip(toggle, false);
  assert.equal(posts.length, 1, 'one save');
  const b = posts[0].body;
  assert.equal(b.get('usb_wifi_setup'), '0');
  assert.equal(b.get('apply'), 'save');
  assert.equal(b.get('eth_mode'), 'static');
  assert.equal(b.get('eth_ip'), '192.168.1.20');
  assert.equal(b.get('eth_gateway'), '192.168.1.1');
  assert.equal(b.get('eth_dns'), '1.1.1.1');
  assert.equal(b.get('eth_enabled'), '1');
  assert.equal(b.get('eth_fallback_wifi'), '1');
});

await check.check('new UI: a refused save puts the switch back', async () => {
  statusReply = STORED;
  posts.length = 0;
  saveStatus = 400;
  const { toggle } = await drawNew();
  await flip(toggle, false);
  saveStatus = 200;
  assert.equal(posts.length, 1);
  assert.equal(toggle.checked, true, 'back on after the refusal');
  assert.equal(toggle.disabled, false);
});

// ---- legacy web UI (android-app/): the switch is part of the Ethernet form
const { HarmonyAppSettingsSystem } = await import('../android-app/src/app/app_settings_system.js');

function legacyForm(loaded, checked) {
  for (const n of [...body.walk()]) if (n.id && /^(eth|usbWifi)/.test(n.id) && n.parentNode) n.parentNode.removeChild(n);
  const add = (tag, id, props) => { const el = document.createElement(tag); el.id = id; Object.assign(el, props); body.appendChild(el); return el; };
  add('select', 'ethModeSelect', { value: 'dhcp' });
  for (const id of ['ethIp', 'ethNetmask', 'ethGateway', 'ethDns']) add('input', id, { value: '' });
  add('span', 'ethSaveStatus', {});
  const toggle = add('input', 'usbWifiSetupToggle', { type: 'checkbox', checked });
  if (loaded) toggle.dataset.loaded = '1';
  return toggle;
}

async function legacySave() {
  const app = Object.create(HarmonyAppSettingsSystem.prototype);
  const sent = [];
  app.client = { saveEthernet: async (data) => { sent.push(data); return { ok: true }; } };
  await app.saveEthernetConfig('save');
  return sent[0];
}

await check.check('legacy UI: the switch is sent only after the stored value was read', async () => {
  legacyForm(false, true);
  const data = await legacySave();
  assert.ok(data, 'save sent');
  assert.equal('usb_wifi_setup' in data, false);
});

await check.check('legacy UI: the switch is sent with the Ethernet settings', async () => {
  legacyForm(true, false);
  let data = await legacySave();
  assert.equal(data.usb_wifi_setup, 0);
  legacyForm(true, true);
  data = await legacySave();
  assert.equal(data.usb_wifi_setup, 1);
});

check.finish();
