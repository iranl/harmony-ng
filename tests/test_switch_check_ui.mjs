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
console.log('    all passed');
process.exit(0);
