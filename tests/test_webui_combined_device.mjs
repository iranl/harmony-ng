// Legacy web UI, combined devices (a transport per command, payload/source/net_device.h):
// - the inventory keeps transports, the http/tcp connections and each command's transport
// - studioView shows a combined device as a device of one transport (type, connection, plugin)
// - a command saved for the combined device open in the studio carries the transport picked
//   there; one that names its own keeps it; another device's command gets none
// - a new combined device is saved with its transports and the plugin's default commands
// - the HTTP connection of a combined device is saved inside "http"
// Runs in Node with a stand-in DOM and a stubbed fetch: no browser, no Hub.
import assert from 'node:assert/strict';
import { installFakeDom, allowJsonImports, makeChecker } from './webui_fake_dom.mjs';

const { body } = installFakeDom();
await allowJsonImports();
const quietWarn = console.warn;
console.warn = () => {};

const calls = [];
const INVENTORY = { devices: [
  { id: '7', name: 'Receiver', type: 'Combined', transports: ['tcp', 'http', 'ir'], controlPort: 2,
    http: { netHost: '10.0.0.5', netPort: 8080, httpHeaders: [], hasHttpPassword: true },
    tcp: { netHost: '10.0.0.5', netPort: 23 },
    commands: [
      { name: 'PowerOn', transport: 'tcp', keyCode: 'TCP:PowerOn', tcpPayload: 'PWON\\r', tcpFormat: 'text' },
      { name: 'Play', transport: 'http', keyCode: 'HTTP:Play', httpMethod: 'POST', httpPath: '/x',
        httpHeaders: [{ name: 'SOAPACTION', value: 'a#Play' }] },
      { name: 'HDMI1', transport: 'ir', keyCode: '0000 006D', port: 4 }] },
  { id: '8', name: 'TV', type: 'IR', commands: [] }] };
globalThis.fetch = async (url, opts = {}) => {
  const path = String(url).replace(/^https?:\/\/[^/]+/, '');
  calls.push({ path, body: opts.body });
  const r = path.startsWith('/api/inventory') ? INVENTORY : { ok: true, deviceId: '9' };
  return new Response(JSON.stringify(r), { status: 200 });
};
const bodyOf = c => {
  const b = String(c.body || '');
  if (b.startsWith('{')) return JSON.parse(b);
  return Object.fromEntries(new URLSearchParams(b));
};

const t = makeChecker('Web UI combined devices (legacy UI)');
const { HarmonyClient } = await import('../android-app/src/api.js');
const { HarmonyAppDevicesStudio } = await import('../android-app/src/app/app_devices_studio.js');

let inv;
await t.check('inventory keeps transports, connections and command transports', async () => {
  inv = await new HarmonyClient().getInventory();
  const d = inv.devices[0];
  assert.deepEqual(d.transports, ['tcp', 'http', 'ir']);
  assert.equal(d.http.netPort, 8080);
  assert.equal(d.tcp.netPort, 23);
  const byName = Object.fromEntries(d.commands.map(c => [c.name, c]));
  assert.equal(byName.PowerOn.transport, 'tcp');
  assert.equal(byName.PowerOn.tcpPayload, 'PWON\\r');
  assert.deepEqual(byName.Play.httpHeaders, [{ name: 'SOAPACTION', value: 'a#Play' }]);
  assert.equal(byName.HDMI1.port, 4);
  assert.equal(byName.PowerOn.port, undefined);
});

const app = Object.create(HarmonyAppDevicesStudio.prototype);
app.client = new HarmonyClient();
app.inventory = { devices: [] };
app.installedPlugins = [{ id: 'kodi', name: 'Kodi', device_definition: {
  display_name: 'Kodi', config_fields: [{ key: 'host', type: 'string', default: '' }],
  default_commands: [{ name: 'Home', keyCode: 'PLUGIN:kodi:home' }] } }];

await t.check('studioView: one transport at a time, first by default', async () => {
  const d = inv.devices[0];
  const v = app.studioView(d);
  assert.equal(v.transport, 'tcp');
  assert.equal(v.type, 'TCP');
  assert.equal(v.netPort, 23);
  assert.equal(app.isNetDevice(v), true);
  const h = app.studioView(d, 'http');
  assert.equal(h.type, 'HTTP');
  assert.equal(h.netPort, 8080);
  assert.equal(h.hasHttpPassword, true);
  const ir = app.studioView(d, 'ir');
  assert.equal(ir.type, 'IR');
  assert.equal(ir.plugin, '');
  assert.equal(app.isNetDevice(ir), false);
  app.studioTransports['7'] = 'http';
  assert.equal(app.studioView(d).transport, 'http');
  assert.equal(app.studioView(inv.devices[1]), inv.devices[1], 'a device that is not combined is shown as is');
});

await t.check('a command saved from the studio carries the picked transport', async () => {
  app.client.commandTransport = { deviceId: '7', transport: 'ir' };
  calls.length = 0;
  await app.client.saveLearnedCommand({ deviceId: '7', name: 'Back', raw: 'R1' });
  await app.client.saveLearnedCommand({ deviceId: '7', name: 'Odd', raw: 'R2', transport: 'tcp' });
  await app.client.saveLearnedCommand({ deviceId: '8', name: 'Up', raw: 'R3' });
  await app.client.saveBatchCommands('7', [{ name: 'B1', raw: 'R4' }]);
  const [a, b, c, d] = calls.map(bodyOf);
  assert.equal(a.transport, 'ir');
  assert.equal(b.transport, 'tcp');
  assert.equal(c.transport, undefined);
  assert.equal(d.commands[0].transport, 'ir');
  assert.equal(d.commands[0].deviceId, undefined);
  app.client.commandTransport = null;
});

await t.check('new combined device: transports, plugin and its default commands', async () => {
  const mk = (tag, attrs) => {
    const n = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs)) n.setAttribute(k, v);
    body.appendChild(n);
    return n;
  };
  for (const tr of ['ir', 'http', 'plugin', 'mqtt']) {
    const c = mk('input', { class: 'new-tr-chk', value: tr });
    c.checked = tr !== 'mqtt';
  }
  const pick = mk('select', { id: 'newDevicePluginPick' });
  const o = document.createElement('option'); o.setAttribute('value', 'kodi'); pick.appendChild(o);
  pick.value = 'kodi';
  app._newDeviceConfigFor = 'kodi';
  mk('input', { id: 'plug_cfg_host' }).value = '10.0.0.7';
  app.ensurePluginInstalled = async id => app.installedPlugins.find(p => p.id === id) || null;
  app.refreshData = async () => {};
  app.renderIrView = () => {};
  globalThis.alert = m => { throw new Error('alert: ' + m); };
  calls.length = 0;
  await app.submitNewCombinedDevice({ name: 'Media PC', manufacturer: 'Team Kodi', model: 'Kodi 21', status: null });
  const req = calls.find(c => c.path.startsWith('/ir/new-device'));
  assert.ok(req, JSON.stringify(calls.map(c => c.path)));
  const p = bodyOf(req);
  assert.equal(p.type, 'Combined');
  // a JSON list: the Hub refuses "transports" sent as the form string "ir,http,plugin"
  assert.deepEqual(p.transports, ['ir', 'http', 'plugin']);
  assert.equal(p.plugin, 'kodi');
  assert.deepEqual(p.config, { host: '10.0.0.7' });
  assert.deepEqual(p.commands, [{ name: 'Home', keyCode: 'PLUGIN:kodi:home', raw: 'PLUGIN:kodi:home', transport: 'plugin' }]);
  assert.equal(p.mqttTopic, undefined, 'MQTT not ticked');
});

await t.check('HTTP connection of a combined device saved inside "http"', async () => {
  app.inventory = inv;
  app.selectedDeviceId = '7';
  app.studioTransports['7'] = 'http';
  const mk = (id, v) => { const n = document.createElement('input'); n.setAttribute('id', id); n.value = v; body.appendChild(n); };
  mk('netHost', '10.0.0.6'); mk('netPort', '8080'); mk('netTimeoutMs', '3000'); mk('netTlsVerify', 'verify');
  mk('netAuthType', 'none'); mk('netAuthUser', ''); mk('netAuthPass', ''); mk('netAuthToken', '');
  const hdr = document.createElement('textarea'); hdr.setAttribute('id', 'netHeaders'); hdr.value = 'X-A: 1'; body.appendChild(hdr);
  app.netStatus = () => {};
  calls.length = 0;
  await app.saveNetConnection(false);
  const req = calls.find(c => c.path.startsWith('/api/device-save'));
  const p = bodyOf(req);
  assert.equal(p.id, '7');
  assert.equal(p.type, undefined, 'the type stays Combined');
  assert.equal(p.netHost, undefined);
  assert.equal(p.http.netHost, '10.0.0.6');
  assert.deepEqual(p.http.httpHeaders, [{ name: 'X-A', value: '1' }]);
});

console.warn = quietWarn;
t.finish();
