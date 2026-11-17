// Legacy web UI (android-app/): a language change redraws the open view and the Hub status
// tooltips, so a page does not mix the old and the new language (the static data-i18n texts
// switch by themselves; the texts the code builds only change when drawn again). Pages with a
// settings form or an unsaved key map are not reloaded: what the user typed or changed stays.
// Runs in Node with a stand-in DOM: no browser, no Hub.
import assert from 'node:assert/strict';
import { installFakeDom, allowJsonImports, makeChecker, tick } from './webui_fake_dom.mjs';

const { body } = installFakeDom();
await allowJsonImports();
globalThis.fetch = async () => { throw new Error('no hub in this test'); };
globalThis.location.hash = '';
const quiet = console.warn;
console.warn = () => {};
const { HarmonyApp } = await import('../android-app/src/app.js');
const i18n = await import('../android-app/src/i18n.js');
console.warn = quiet;

const t = makeChecker('Legacy web UI language change');

await t.check('the open view is drawn again in the new language', async () => {
  i18n.setLanguage('en');
  const app = new HarmonyApp();
  const drawn = [];
  app.renderTab = (tab) => drawn.push(tab);
  app.currentTab = 'bluetooth';
  app.watchLanguage();
  i18n.setLanguage('de');
  assert.deepEqual(drawn, ['bluetooth']);
});

function appOn(tab) {
  i18n.setLanguage('en');
  const app = new HarmonyApp();
  const calls = [];
  for (const m of ['loadDashboard', 'renderActivityRemote', 'loadActivitiesView', 'loadBtView', 'loadRemoteView',
    'refreshRfStatus', 'loadRemotesView', 'loadMqttView', 'loadNetworkView', 'loadBackupView', 'loadSystemView',
    'refreshServiceStatuses', 'renderHubsList', 'initLabView', 'refreshMqttStatus', 'refreshNetworkStatus',
    'refreshSystemInfo']) {
    app[m] = () => calls.push(m);
  }
  app.currentTab = tab;
  app.watchLanguage();
  return { app, calls };
}

await t.check('a settings form is not reloaded, only its status is drawn again', async () => {
  const mqtt = appOn('mqtt');
  i18n.setLanguage('de');
  assert.deepEqual(mqtt.calls, ['refreshMqttStatus']);
  const net = appOn('network');
  i18n.setLanguage('fr');
  assert.deepEqual(net.calls, ['refreshNetworkStatus']);
  const sys = appOn('system');
  i18n.setLanguage('nl');
  assert.deepEqual(sys.calls, ['refreshServiceStatuses', 'refreshSystemInfo']);
});

function el(tag, id, props = {}) {
  const e = document.createElement(tag);
  e.id = id;
  Object.assign(e, props);
  body.appendChild(e);
  return e;
}

await t.check('MQTT page in Italian: the badge reads "Connesso", the typed host stays', async () => {
  i18n.setLanguage('en');
  const badge = el('span', 'mqttConnBadge');
  const host = el('input', 'mqttHost');
  const form = ['mqttEnabled', 'mqttPort', 'mqttUsername', 'mqttBaseTopic', 'mqttClientId'].map(id => el('input', id));
  const app = new HarmonyApp();
  app.client.getMqttStatus = async () => ({ connected: true, enabled: true, config: { host: 'stored.example.org' } });
  app.currentTab = 'mqtt';
  await app.loadMqttView();
  assert.equal(badge.textContent, 'Connected');
  host.value = 'typed.example.org';
  app.watchLanguage();
  i18n.setLanguage('it');
  await tick(5);
  assert.equal(badge.textContent, 'Connesso');
  assert.equal(host.value, 'typed.example.org');
  badge.remove(); host.remove(); form.forEach(e => e.remove());
});

await t.check('network page in Italian: Wi-Fi state redrawn, the Ethernet form keeps what was typed', async () => {
  i18n.setLanguage('en');
  const ids = ['netConnType', 'netActiveIf', 'netIp', 'netMask', 'netGw', 'netMac', 'netUsbMode', 'netEthStatus', 'netWifiStatus', 'netIfconfigPre'];
  const els = ids.map(id => el('div', id));
  const ethIp = el('input', 'ethIp');
  const app = new HarmonyApp();
  app.client.getNetworkStatus = async () => ({ wifi_connected: true, wifi_ssid: 'Home', config: { eth_ip: '192.168.1.20' } });
  app.client.getSystemInfo = async () => ({});
  app.currentTab = 'network';
  await app.loadNetworkView();
  assert.equal(ethIp.value, '192.168.1.20');
  ethIp.value = '192.168.1.30';
  app.watchLanguage();
  i18n.setLanguage('it');
  await tick(5);
  assert.match(document.getElementById('netWifiStatus').innerHTML, /Connesso/);
  assert.equal(ethIp.value, '192.168.1.30');
  els.forEach(e => e.remove()); ethIp.remove();
});

await t.check('the remote key editor keeps an unsaved map; only its status is refreshed', async () => {
  const { app, calls } = appOn('rf-remote');
  app.remoteMapData = { unsaved: true };
  i18n.setLanguage('fr');
  assert.deepEqual(calls, ['refreshRfStatus']);
  assert.deepEqual(app.remoteMapData, { unsaved: true });
  const b25 = appOn('remotes');
  i18n.setLanguage('it');
  assert.deepEqual(b25.calls, []);
});

await t.check('devices studio: an unsaved blaster port stays chosen when the page is drawn in Italian', async () => {
  i18n.setLanguage('en');
  const view = el('section', 'view-ir');
  const port = document.createElement('select');
  port.id = 'curDevPortSelect';
  for (const v of ['7', '3', '1']) {
    const o = document.createElement('option');
    o.value = v;
    port.appendChild(o);
  }
  const devSel = document.createElement('select');
  devSel.id = 'manageDeviceSelect';
  const name = document.createElement('div');
  name.id = 'curDevName';
  view.append(port, devSel, name);
  const app = new HarmonyApp();
  app.inventory = { devices: [{ id: '1', name: 'TV', type: 'IR', controlPort: 7, commands: [] }] };
  app.selectedIrDeviceId = '1';
  app.currentTab = 'ir';
  app.renderTab('ir');
  assert.equal(port.value, '7');
  port.value = '3'; // chosen, not saved yet
  name.textContent = '';
  app.watchLanguage();
  i18n.setLanguage('it');
  await tick(5);
  assert.equal(name.textContent, 'TV', 'the page was not drawn again');
  assert.equal(port.value, '3', 'the language change put the saved port back');
  view.remove();
});

await t.check('the Hub status tooltip follows the language', async () => {
  i18n.setLanguage('en');
  const dot = document.createElement('span');
  dot.id = 'activeHubStatusDot';
  body.appendChild(dot);
  const app = new HarmonyApp();
  app.renderTab = () => {};
  app.isHubOnline = true;
  app.watchLanguage();
  app.updateHubStatusTitles();
  assert.equal(dot.getAttribute('title') ?? dot.title, 'Hub is online');
  i18n.setLanguage('it');
  assert.notEqual(dot.getAttribute('title') ?? dot.title, 'Hub is online');
  dot.remove();
});

await t.check('a Hub that answers 401 stays "online" (login needed) in every language', async () => {
  const { HarmonyClient } = await import('../android-app/src/api.js');
  const realFetch = globalThis.fetch;
  globalThis.fetch = async () => new Response('{}', { status: 401 });
  try {
    i18n.setLanguage('de');
    const client = new HarmonyClient({ host: '192.168.1.20', port: 8089 });
    const res = await client.testConnection();
    assert.equal(res.ok, false);
    assert.equal(res.status, 401);
    const app = new HarmonyApp();
    app.renderTab = () => {};
    app.isHubOnline = true;
    for (let i = 0; i < 4; i++) {
      const err = new Error(res.error);
      err.status = res.status;
      app.recordFailure(err);
    }
    assert.equal(app.isHubOnline, true);
  } finally {
    globalThis.fetch = realFetch;
  }
});

i18n.setLanguage('en');
t.finish();
// the app objects keep their reconnect and health timers running: end here
process.exit(0);
