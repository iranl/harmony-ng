// Legacy web UI, device templates (android-app/src/device_templates.js, harmony-ng-plugins/devices):
// - the index comes from jsDelivr, else raw.githubusercontent; entries with an odd file are left out
// - the device: ticked transports in the template's order, the earlier transport's command wins a
//   name, {{key}} filled in (a whole-string number stays a number), plugin default commands added
// - required setup fields are reported; "Create device" posts the combined device as JSON
// - template text is shown escaped
// Runs in Node with a stand-in DOM and a stubbed fetch: no browser, no Hub.
import assert from 'node:assert/strict';
import { installFakeDom, allowJsonImports, makeChecker } from './webui_fake_dom.mjs';

const { body } = installFakeDom();
await allowJsonImports();
const quietWarn = console.warn;
console.warn = () => {};

const TPL = {
  schema: 1, id: 'demo-avr', name: 'Demo AVR', manufacturer: 'Demo', model: 'X1', icon: 'receiver',
  transports: {
    tcp: { label: 'Telnet', default: true, connection: { netHost: '{{host}}', netPort: 23 } },
    http: { label: 'HTTP', connection: { netHost: '{{host}}', netPort: '{{port}}',
      httpHeaders: [{ name: 'X-Auth-PSK', value: '{{psk}}' }] } },
    plugin: { id: 'demo-plug', config: { host: '{{host}}' }, default_commands: true },
    ir: { label: 'IR' }
  },
  setup: { message: 'Turn on network control.', fields: [
    { key: 'host', label: 'IP address', type: 'string', required: true },
    { key: 'port', label: 'Port', type: 'number', default: 8080 },
    { key: 'psk', label: 'Key', type: 'password' }] },
  commands: [
    { name: 'PowerOn', transport: 'tcp', tcpPayload: 'PWON\\r' },
    { name: 'PowerOn', transport: 'http', httpPath: '/p?PWON' },
    { name: 'Status', transport: 'http', httpPath: '/s/{{host}}' }]
};
const INDEX = { schema: 1, templates: [
  { id: 'demo-avr', name: 'Demo AVR', manufacturer: 'Demo', model: 'X1', transports: ['tcp', 'http'], file: 'devices/demo-avr.json' },
  { id: 'evil', name: '<img src=x onerror=alert(1)>', manufacturer: 'E', model: 'E', transports: [], file: 'devices/evil.json' },
  { id: 'odd', name: 'Odd', file: '../../etc/passwd' }] };

const calls = [];
globalThis.fetch = async (url, opts = {}) => {
  const u = String(url);
  calls.push({ url: u, body: opts.body });
  if (u.startsWith('https://cdn.jsdelivr.net/')) return new Response('down', { status: 503 });
  if (u.endsWith('devices/index.json')) return new Response(JSON.stringify(INDEX), { status: 200 });
  if (u.endsWith('devices/demo-avr.json')) return new Response(JSON.stringify(TPL), { status: 200 });
  return new Response(JSON.stringify({ ok: true, deviceId: '12' }), { status: 200 });
};

const t = makeChecker('Web UI device templates (legacy UI)');
const tpl = await import('../android-app/src/device_templates.js');
const PLUG = { id: 'demo-plug', device_definition: { default_commands: [
  { name: 'Home', keyCode: 'PLUGIN:demo-plug:home' }, { name: 'PowerOn', keyCode: 'PLUGIN:demo-plug:on' }] } };

await t.check('index from the raw fallback, odd entries left out', async () => {
  const idx = await tpl.fetchTemplateIndex();
  assert.deepEqual(idx.map(e => e.id), ['demo-avr', 'evil']);
  assert.ok(calls.some(c => c.url.startsWith('https://raw.githubusercontent.com/iranl/harmony-ng-plugins/main/devices/index.json')));
});

await t.check('device: template order decides, placeholders filled', async () => {
  const values = tpl.setupValues(TPL, { host: '10.0.0.5', port: '', psk: 's3cret' });
  assert.deepEqual(values, { host: '10.0.0.5', port: 8080, psk: 's3cret' });
  const d = tpl.buildDeviceFromTemplate(TPL, ['http', 'tcp', 'plugin'], values, PLUG);
  assert.equal(d.type, 'Combined');
  assert.deepEqual(d.transports, ['tcp', 'http', 'plugin'], 'template order, not tick order');
  assert.deepEqual(d.tcp, { netHost: '10.0.0.5', netPort: 23 });
  assert.equal(d.http.netPort, 8080, 'a whole {{port}} stays a number');
  assert.deepEqual(d.http.httpHeaders, [{ name: 'X-Auth-PSK', value: 's3cret' }]);
  assert.equal(d.plugin, 'demo-plug');
  assert.deepEqual(d.config, { host: '10.0.0.5' });
  const byName = Object.fromEntries(d.commands.map(c => [c.name, c]));
  assert.equal(byName.PowerOn.transport, 'tcp', 'the earlier transport keeps PowerOn');
  assert.equal(byName.Status.httpPath, '/s/10.0.0.5');
  assert.equal(byName.Home.transport, 'plugin');
  assert.equal(byName.Home.raw, 'PLUGIN:demo-plug:home');
  assert.equal(d.commands.length, 3);
  const httpOnly = tpl.buildDeviceFromTemplate(TPL, ['http'], values);
  assert.equal(httpOnly.commands.find(c => c.name === 'PowerOn').httpPath, '/p?PWON');
  assert.equal(httpOnly.tcp, undefined);
});

await t.check('required setup fields reported', async () => {
  const missing = tpl.missingSetupFields(TPL, tpl.setupValues(TPL, { host: '' }));
  assert.deepEqual(missing.map(f => f.key), ['host']);
});

const { HarmonyClient } = await import('../android-app/src/api.js');
const { HarmonyAppDevicesStudio } = await import('../android-app/src/app/app_devices_studio.js');
const app = Object.create(HarmonyAppDevicesStudio.prototype);
app.client = new HarmonyClient();
app.inventory = { devices: [] };

await t.check('template list shows names escaped', async () => {
  const list = document.createElement('div'); list.setAttribute('id', 'tplList'); body.appendChild(list);
  app.templateIndex = await tpl.fetchTemplateIndex();
  app.renderTemplateList();
  assert.match(list.innerHTML, /&lt;img src=x onerror=alert\(1\)&gt;/);
  assert.doesNotMatch(list.innerHTML, /<img/);
});

await t.check('Create device posts the combined device as JSON', async () => {
  const mk = (tag, attrs, props = {}) => {
    const n = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs)) n.setAttribute(k, v);
    Object.assign(n, props);
    body.appendChild(n);
    return n;
  };
  for (const [tr, on] of [['tcp', true], ['http', false], ['plugin', true]]) mk('input', { class: 'tpl-tr-chk', value: tr }, { checked: on });
  mk('input', { id: 'tpl_f_host' }, { value: '10.0.0.9' });
  mk('input', { id: 'tpl_f_port' }, { value: '' });
  mk('input', { id: 'tpl_f_psk' }, { value: '' });
  mk('div', { id: 'tplSetupStatus' });
  app.activeTemplate = TPL;
  let asked = '';
  app.ensurePluginInstalled = async id => { asked = id; return PLUG; };
  app.refreshData = async () => {};
  app.renderIrView = () => {};
  calls.length = 0;
  await app.createDeviceFromTemplate();
  assert.equal(asked, 'demo-plug');
  const req = calls.find(c => c.url.includes('/api/new-device'));
  assert.ok(req, JSON.stringify(calls.map(c => c.url)));
  const p = JSON.parse(req.body);
  assert.deepEqual(p.transports, ['tcp', 'plugin']);
  assert.equal(p.tcp.netHost, '10.0.0.9');
  assert.equal(p.http, undefined);
  assert.deepEqual(p.commands.map(c => `${c.name}:${c.transport}`), ['PowerOn:tcp', 'Home:plugin']);
});

console.warn = quietWarn;
t.finish();
