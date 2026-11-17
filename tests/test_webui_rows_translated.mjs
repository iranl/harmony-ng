// Both web UIs draw their rows (users, tokens, Bluetooth hosts, the lists of a refused device
// delete) in the chosen language. The Hub answers with sample data here, so the rows are really
// drawn, then the page text is searched for English texts of keys that have a translation: any
// such text left is hard-coded English or a missed key. Also checks that a name holding "$&" or
// "{name}" is shown as typed. Runs in Node with a stand-in DOM: no browser, no Hub.
import assert from 'node:assert/strict';
import { installFakeDom, makeChecker, tick, allowJsonImports } from './webui_fake_dom.mjs';

const { body } = installFakeDom();
await allowJsonImports();
globalThis.location.hash = '';

// The sample Hub: every route the drawn pages read
const ODD_NAME = 'Kino $& {name} TV';
const HUB = {
  '/api/users': { users: [{ username: 'admin', role: 'admin' }, { username: 'kino', role: 'user' }] },
  '/api/auth/tokens': { tokens: [{ id: 't1', name: 'Home Assistant', role: 'admin', owner: 'kino', created_at: '2026-11-01' }] },
  '/api/bt-status': {
    pid: 42, backend: 'btstack', state: 'running',
    host: { addr: '00:11:22:33:44:01', name: 'Shield', connected: true, pairing: false },
    hosts: [
      { addr: '00:11:22:33:44:01', name: 'Shield', connected: true, paired: true },
      { addr: '00:11:22:33:44:02', name: 'TV', connected: false, paired: false },
      { addr: '00:11:22:33:44:03', name: 'Laptop', connected: false, paired: true },
    ],
    remote: { addr: '00:00:00:00:00:00', connected: false },
  },
};
globalThis.fetch = async (url) => {
  const path = new URL(String(url), 'http://192.168.1.20/').pathname;
  const data = HUB[path];
  return new Response(JSON.stringify(data ?? {}), { status: data ? 200 : 404, headers: { 'Content-Type': 'application/json' } });
};

// The text a person sees: text nodes, and the markup the legacy UI writes with innerHTML
function shownText(root) {
  let out = '';
  for (const n of root.walk()) {
    out += `${n._text || ''}\n`;
    if (n._html) out += `${n._html.replace(/<[^>]*>/g, '\n').replace(/&amp;/g, '&').replace(/&quot;/g, '"').replace(/&#39;/g, "'")}\n`;
  }
  return out;
}

// English texts of the keys translated into `lang`, cut at their {placeholders}: the parts long
// enough not to appear by chance in another text
function englishParts(en, dict) {
  const parts = [];
  for (const [key, text] of Object.entries(en)) {
    if (!dict[key] || dict[key] === text) continue;
    for (const part of String(text).split(/\{[A-Za-z0-9_]+\}/)) {
      const p = part.trim();
      if (p.length >= 12 && /[A-Za-z]{3,}\s+[A-Za-z]{2,}/.test(p) && !String(dict[key]).includes(p)) parts.push({ key, p });
    }
  }
  return parts;
}

function leftovers(text, en, dict) {
  return englishParts(en, dict).filter(({ p }) => text.includes(p)).map(({ key, p }) => `${key}: "${p}"`);
}

const t = makeChecker('Web UI rows in the chosen language');
const realError = console.error, realWarn = console.warn;
const quiet = () => { console.error = console.warn = () => {}; };
const loud = () => { console.error = realError; console.warn = realWarn; };

// ---- new web UI (frontend/)
const ni18n = await import('../frontend/src/i18n/index.js');
const nen = ni18n.TRANSLATIONS.en;

async function newView(name, lang) {
  ni18n.setLanguage(lang);
  quiet();
  const mod = await import(`../frontend/src/views/setup/${name}.js`);
  const made = (mod.createView || mod.default)();
  const view = made.element || made;
  await tick(20);
  loud();
  return view;
}

for (const [name, mustShow] of [['users', ['kino', 'Home Assistant']], ['bt-hosts', ['Shield', '00:11:22:33:44:02']]]) {
  await t.check(`new UI ${name}: the rows are drawn and read in German`, async () => {
    const text = shownText(await newView(name, 'de'));
    for (const s of mustShow) assert.ok(text.includes(s), `row "${s}" not drawn: the sample data did not reach the page`);
    assert.ok(!/\bundefined\b/.test(text), 'a text shows "undefined"');
    assert.ok(!/\{[a-z_]+\}/.test(text), 'a text shows an unfilled {placeholder}');
    const left = leftovers(text, nen, ni18n.TRANSLATIONS.de);
    assert.deepEqual(left, [], `still English: ${left.slice(0, 6).join(' | ')}`);
  });
}

const USERS_409 = [
  { type: 'activity', id: '2', name: ODD_NAME, fields: ['startSequence'] },
  { type: 'device', id: '5', name: ODD_NAME },
  { type: 'remote_map', slot: 1, groups: [{ type: 'activity', id: '2', name: ODD_NAME, count: 3 }, { type: 'device_page', id: '5', name: ODD_NAME, count: 1 }] },
  { type: 'bt_remote_map', count: 2 },
];

await t.check('new UI: the lists of a refused device delete read in Italian, names shown as typed', async () => {
  ni18n.setLanguage('it');
  const { deviceUsersLines } = await import('../frontend/src/components/hub-decisions.js');
  const text = deviceUsersLines(USERS_409).join('\n');
  assert.equal(text.split(ODD_NAME).length - 1, 4, `a name is not shown as typed:\n${text}`);
  const left = leftovers(text.split(ODD_NAME).join(''), nen, ni18n.TRANSLATIONS.it);
  assert.deepEqual(left, [], `still English: ${left.join(' | ')}`);
  ni18n.setLanguage('en');
});

// ---- legacy web UI (android-app/)
const li18n = await import('../android-app/src/i18n.js');
const len = li18n.TRANSLATIONS.en;

await t.check('legacy UI: the lists of a refused device delete read in Italian, names shown as typed', async () => {
  li18n.setLanguage('it');
  const { deviceUsersText } = await import('../android-app/src/app/decision_dialogs.js');
  const text = deviceUsersText(USERS_409);
  assert.equal(text.split(ODD_NAME).length - 1, 4, `a name is not shown as typed:\n${text}`);
  const left = leftovers(text.split(ODD_NAME).join(''), len, li18n.TRANSLATIONS.it);
  assert.deepEqual(left, [], `still English: ${left.join(' | ')}`);
  li18n.setLanguage('en');
});

await t.check('legacy UI: a token owner holding "$&" is shown as typed', async () => {
  li18n.setLanguage('de');
  const { showTokenOwners } = await import('../android-app/src/app/app_auth_users.js');
  const list = document.createElement('div');
  const row = document.createElement('div');
  const meta = document.createElement('div');
  meta.className = 'mini muted';
  const del = document.createElement('button');
  del.className = 'btn-del-pat';
  del.setAttribute('data-id', 't1');
  row.append(meta, del);
  list.appendChild(row);
  showTokenOwners(list, [{ id: 't1', owner: ODD_NAME }]);
  assert.ok(meta.textContent.includes(ODD_NAME), meta.textContent);
  li18n.setLanguage('en');
});

const BT_IDS = ['btStatusBadge', 'btHostDesc', 'btDevicesList', 'btTargetHostSelect', 'btSaveDeviceSelect',
  'btPairTargetDev', 'btStatusPid', 'btStatusBackend', 'btStatusTargetName', 'btStatusTargetAddr',
  'btStatusHostState', 'btStatusHostDetail', 'btStatusRemoteState', 'btStatusRemoteAddr', 'btPairToggleBtn'];

await t.check('legacy UI Bluetooth page: status and host rows read in German', async () => {
  quiet();
  const { HarmonyApp } = await import('../android-app/src/app.js');
  loud();
  li18n.setLanguage('de');
  const section = document.createElement('section');
  section.id = 'view-bluetooth';
  for (const id of BT_IDS) {
    const e = document.createElement(id.endsWith('Select') || id === 'btPairTargetDev' ? 'select' : 'div');
    e.id = id;
    section.appendChild(e);
  }
  body.appendChild(section);
  const app = new HarmonyApp();
  app.inventory = { devices: [{ id: '2', name: 'Shield TV', type: 'BT', bdaddr: '00:11:22:33:44:01' }] };
  quiet();
  await app.loadBtView(HUB['/api/bt-status']);
  await tick(5);
  loud();
  const text = shownText(section);
  assert.ok(text.includes('00:11:22:33:44:03'), 'host rows not drawn');
  assert.ok(!/\bundefined\b/.test(text), 'a text shows "undefined"');
  assert.ok(!/\{[a-z_]+\}/.test(text), 'a text shows an unfilled {placeholder}');
  const left = leftovers(text, len, li18n.TRANSLATIONS.de);
  assert.deepEqual(left, [], `still English: ${left.slice(0, 6).join(' | ')}`);
  section.remove();
  li18n.setLanguage('en');
});

t.finish();
// the legacy app object keeps its reconnect and health timers running: end here
process.exit(0);
