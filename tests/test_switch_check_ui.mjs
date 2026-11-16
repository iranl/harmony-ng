// Both web UIs after a request that turned authentication on ("to_check" in the reply):
// - the account logged in is never offered
// - Delete removes one item through the usual route; one already gone (404) counts as deleted
// - Keep all just closes: no other request, nothing blocked
// Runs in Node with a minimal stand-in DOM: no browser, no Hub.
import assert from 'node:assert/strict';

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
  appendChild(child) { child.parentNode = this; this.children.push(child); return child; }
  removeChild(child) { this.children = this.children.filter(c => c !== child); child.parentNode = null; }
  remove() { if (this.parentNode) this.parentNode.removeChild(this); }
  addEventListener(name, fn) { (this.listeners[name] ||= []).push(fn); }
  removeEventListener() {}
  setAttribute(k, v) { this.attrs[k] = v; if (k === 'id') this.id = v; }
  get isConnected() { let n = this; while (n.parentNode) n = n.parentNode; return n === body; }
  *walk() { yield this; for (const c of this.children) yield* c.walk(); }
  async click() { for (const fn of this.listeners.click || []) await fn({ preventDefault() {} }); }
}

const body = new FakeNode('body');
globalThis.Node = FakeNode;
globalThis.document = {
  body,
  createElement: tag => new FakeNode(tag),
  createTextNode: text => Object.assign(new FakeNode('#text'), { textContent: text }),
  getElementById: id => [...body.walk()].find(n => n.id === id) || null,
};
globalThis.requestAnimationFrame = fn => fn();
globalThis.setTimeout = (fn) => { fn(); return 0; };
const store = { harmony_auth_user: 'owner' };
globalThis.localStorage = { getItem: k => store[k] ?? null, setItem: (k, v) => { store[k] = v; }, removeItem: k => { delete store[k]; } };

const notFound = () => Object.assign(new Error('Token not found'), { status: 404 });
const findText = (root, text) => [...root.walk()].find(n => n.textContent === text);
const toCheck = {
  users: [{ username: 'owner', role: 'admin' }, { username: 'ghost', role: 'admin' }],
  tokens: [{ id: 'tok_ghost', name: 'ghost token', role: 'admin', owner: 'ghost' }, { id: 'tok_ha', name: 'HA', role: 'user' }],
};

// Legacy UI
{
  const { checkItems, askToCheck } = await import('../android-app/src/app/decision_dialogs.js');
  assert.deepEqual(checkItems(toCheck, 'owner').map(i => i.id), ['ghost', 'tok_ghost', 'tok_ha'],
    'legacy: the account logged in is not offered');
  const calls = [];
  const client = {
    deleteUser: async (u) => { calls.push(`user ${u}`); },
    deleteToken: async (id) => { calls.push(`token ${id}`); throw notFound(); },
  };
  const done = askToCheck(client, toCheck, 'owner');
  const dels = [...body.walk()].filter(n => n.textContent === 'Delete');
  assert.equal(dels.length, 3, 'legacy: one Delete per item');
  await dels[0].click();
  await dels[1].click();
  assert.deepEqual(calls, ['user ghost', 'token tok_ghost']);
  assert.equal([...body.walk()].filter(n => n.textContent === 'Delete').length, 1, 'legacy: a 404 counts as deleted');
  await findText(body, 'Keep all').click();
  await done;
  assert.equal(findText(body, 'Keep all'), undefined, 'legacy: Keep all closes');
  assert.equal(calls.length, 2, 'legacy: Keep all sends nothing');
  console.log('    ok   legacy dialog: own account left out, Delete per item, Keep all closes');
}

// New UI
{
  const { showSwitchCheck } = await import('../frontend/src/components/hub-decisions.js');
  const calls = [];
  const api = {
    deleteUser: async (u) => { calls.push(`user ${u}`); },
    deleteToken: async (id) => { calls.push(`token ${id}`); throw notFound(); },
  };
  const modal = showSwitchCheck(api, toCheck);
  const labels = [...body.walk()].map(n => n.textContent).filter(s => /^(Account|Access token): /.test(s));
  assert.deepEqual(labels, ['Account: ghost (admin)', 'Access token: ghost token (admin, ghost)', 'Access token: HA (user)'],
    'new UI: the account logged in is not listed');
  const dels = [...body.walk()].filter(n => n.textContent === 'Delete');
  await dels[1].click();
  assert.deepEqual(calls, ['token tok_ghost']);
  assert.equal([...body.walk()].filter(n => n.textContent === 'Delete').length, 2, 'new UI: a 404 counts as deleted');
  await findText(body, 'Keep all').click();
  assert.equal(calls.length, 1, 'new UI: Keep all sends nothing');
  assert.ok(modal && typeof modal.close === 'function');
  assert.equal(showSwitchCheck(api, { users: [{ username: 'owner', role: 'admin' }], tokens: [] }), null,
    'new UI: nothing to show when only the own account is listed');
  console.log('    ok   new UI dialog: own account left out, Delete per item, Keep all closes');
}
// New UI flow: restore -> reply with to_check -> list kept in sessionStorage -> names only while
// nobody is logged in (the import ended every session), nothing for a user-role login, the full
// dialog once an admin is logged in (after the Users page login reloads the page)
{
  const session = {};
  globalThis.sessionStorage = { getItem: k => session[k] ?? null, setItem: (k, v) => { session[k] = String(v); }, removeItem: k => { delete session[k]; } };
  const target = new EventTarget();
  globalThis.window = { addEventListener: (...a) => target.addEventListener(...a), dispatchEvent: (e) => target.dispatchEvent(e) };
  let who = { authenticated: false };
  const sent = [];
  const client = {
    postJson: async (path, body) => { sent.push([path, body.target]); return { ok: true, auth_enabled: true, to_check: toCheck }; },
    request: async (path) => { if (path === '/api/auth/me') return who; throw new Error('unexpected ' + path); },
  };
  const { createSystemApi } = await import('../frontend/src/api/system.js');
  const sys = createSystemApi(client);
  const api = { ...sys, deleteUser: async () => {}, deleteToken: async () => {} };
  const { installSwitchCheck, showPendingSwitchCheck } = await import('../frontend/src/components/hub-decisions.js');
  const deletes = () => [...body.walk()].filter(n => n.textContent === 'Delete').length;
  const clear = () => { body.children = []; };
  clear();
  installSwitchCheck(api);
  await sys.restoreBackup('all', '{"users": {}}', false);
  await new Promise(r => globalThis.setImmediate ? setImmediate(r) : r());
  await new Promise(r => setImmediate(r));
  assert.deepEqual(sent, [['/import', 'bundle']], 'new UI: the restore sends target bundle');
  assert.ok(session.harmony_auth_switch_check, 'new UI: the list is kept for after the login');
  assert.ok(findText(body, 'Account: ghost (admin)') && deletes() === 0, 'new UI: no login yet: names only, no Delete');
  clear();
  who = { authenticated: true, role: 'user' };
  assert.equal(await showPendingSwitchCheck(api), null, 'new UI: a user-role login gets nothing');
  assert.ok(session.harmony_auth_switch_check, 'new UI: the list waits for an admin');
  who = { authenticated: true, role: 'admin' };
  await showPendingSwitchCheck(api);  // as installSwitchCheck does after the login reload
  assert.equal(deletes(), 3, 'new UI: an admin gets the dialog with Delete');
  assert.equal(session.harmony_auth_switch_check, undefined, 'new UI: shown once');
  clear();
  assert.equal(await showPendingSwitchCheck(api), null, 'new UI: not again after that');
  console.log('    ok   new UI flow: restore, list kept, names only before the login, dialog for an admin');
}
// "Device in use" (409 "users" of a refused device delete) as text people can read, both UIs
{
  const users = [
    { type: 'activity', id: '1', name: 'Shield TV', fields: ['deviceIds', 'startSequence'] },
    { type: 'activity', id: '2', name: 'TV (DTT)', fields: ['deviceIds'] },
    { type: 'activity', id: '3', name: 'Xbox', fields: ['deviceIds', 'startSequence'] },
    { type: 'device', id: '5', name: 'Receiver', field: 'btTriggerDeviceId' },
    { type: 'remote_map', slot: 1, count: 47, keys: ['activities/2/Menu', 'activities/2/screen_buttons/0'],
      groups: [{ type: 'activity', id: '2', name: 'TV (DTT)', count: 46 }, { type: 'device_page', id: '7', name: 'Shield', count: 1 }] },
    { type: 'bt_remote_map', count: 2, keys: ['activities/-1/OK'], groups: [{ type: 'no_activity', id: '-1', count: 2 }] },
  ];
  const expected = [
    'Activities: Shield TV, TV (DTT), Xbox (device list, start steps)',
    'Bluetooth trigger of Receiver',
    'Remote slot 1: 46 keys in TV (DTT), 1 key in the Shield page',
    'Bluetooth remote: 2 keys in the default page',
  ];
  const legacy = await import('../android-app/src/app/decision_dialogs.js');
  const fresh = await import('../frontend/src/components/hub-decisions.js');
  assert.deepEqual(legacy.deviceUsersLines(users), expected, 'legacy: readable lines');
  assert.deepEqual(fresh.deviceUsersLines(users), expected, 'new UI: readable lines');
  const text = legacy.deviceUsersText(users);
  assert.ok(text.startsWith('This device is still used:\n') && text.endsWith('Remove the device from these first, then delete it.'),
    'legacy: intro and hint around the lines');
  assert.ok(!/activities\/|deviceIds|startSequence/.test(text), 'legacy: no raw paths or field names');
  const older = [{ type: 'remote_map', slot: 2, count: 3, keys: ['activities/2/A'] }];
  assert.deepEqual(fresh.deviceUsersLines(older), ['Remote slot 2: 3 keys'], 'a Hub without groups: the count');
  body.children = [];
  fresh.showDeviceInUse(Object.assign(new Error('Device is in use'), { status: 409, data: { users } }));
  const shown = [...body.walk()].map(n => n.textContent);
  assert.ok(shown.includes('This device is still used:') && expected.every(l => shown.includes(l)) && !shown.includes('Device is in use'),
    'new UI: the dialog shows the readable lines');
  console.log('    ok   device in use: readable lines in both UIs');
}
console.log('    all passed');
process.exit(0);
