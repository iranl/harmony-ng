// Both web UIs and the accounts and tokens made while authentication was off ("confirmed": false):
// - the account logged in is never offered (legacy dialog and new UI card)
// - "Delete all" goes through every item: a token already deleted with its account (404) counts as
//   deleted, the legacy dialog then closes, the new UI card does not stop at the first error
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
  insertBefore(child, ref) {
    child.parentNode = this;
    const i = this.children.indexOf(ref);
    if (i < 0) this.children.push(child); else this.children.splice(i, 0, child);
    return child;
  }
  removeChild(child) { this.children = this.children.filter(c => c !== child); child.parentNode = null; }
  remove() { if (this.parentNode) this.parentNode.removeChild(this); }
  replaceChildren(...nodes) { this.children = []; for (const n of nodes) this.appendChild(n); }
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
const store = { harmony_auth_user: 'owner' };
globalThis.localStorage = { getItem: k => store[k] ?? null, setItem: (k, v) => { store[k] = v; }, removeItem: k => { delete store[k]; } };

const notFound = () => Object.assign(new Error('Token not found'), { status: 404 });
const findText = (root, text) => [...root.walk()].find(n => n.textContent === text);

// Legacy UI
{
  const { unconfirmedItems, askUnconfirmed } = await import('../android-app/src/app/decision_dialogs.js');
  const unconfirmed = {
    users: [{ username: 'owner', role: 'admin' }, { username: 'ghost', role: 'admin' }],
    tokens: [{ id: 'tok_ghost', name: 'ghost token', role: 'admin', owner: 'ghost' }],
  };
  assert.deepEqual(unconfirmedItems(unconfirmed, 'owner').map(i => i.id), ['ghost', 'tok_ghost'],
    'legacy: the account logged in is not offered');

  const calls = [];
  const client = {
    deleteUser: async (u) => { calls.push(`user ${u}`); },
    deleteToken: async (id) => { calls.push(`token ${id}`); throw notFound(); },
    confirmAuthItems: async () => { calls.push('keep'); },
  };
  const done = askUnconfirmed(client, unconfirmed, 'owner');
  const deleteAll = findText(body, 'Delete all');
  assert.ok(deleteAll, 'legacy: dialog shown');
  await deleteAll.click();
  await done;
  assert.deepEqual(calls, ['user ghost', 'token tok_ghost']);
  assert.equal(findText(body, 'Delete all'), undefined, 'legacy: a 404 counts as deleted and the dialog closes');
  console.log('    ok   legacy dialog: own account left out, Delete all with a token gone with its account');
}

// New UI
{
  const { createUnconfirmedCard } = await import('../frontend/src/components/hub-decisions.js');
  const calls = [];
  const api = {
    getUsers: async () => ({ auth_enabled: true, users: [
      { username: 'owner', role: 'admin', confirmed: false },
      { username: 'ghost', role: 'admin', confirmed: false },
      { username: 'kid', role: 'user', confirmed: false },
      { username: 'wife', role: 'user', confirmed: true }] }),
    getTokens: async () => ({ tokens: [{ id: 'tok_ghost', name: 'ghost token', role: 'admin', confirmed: false }] }),
    deleteUser: async (u) => { calls.push(`user ${u}`); if (u === 'ghost') throw new Error('refused'); },
    deleteToken: async (id) => { calls.push(`token ${id}`); throw notFound(); },
    confirmAuthItems: async () => { calls.push('keep'); },
  };
  const { card, load } = createUnconfirmedCard(api);
  body.appendChild(card);
  await load();
  const labels = [...card.walk()].map(n => n.textContent).filter(s => /^(Account|Access token): /.test(s));
  assert.deepEqual(labels, ['Account: ghost (admin)', 'Account: kid (user)', 'Access token: ghost token (admin)'],
    'new UI: the account logged in is not listed');
  await findText(card, 'Delete all').click();
  assert.deepEqual(calls, ['user ghost', 'user kid', 'token tok_ghost'], 'new UI: Delete all does not stop at an error');
  console.log('    ok   new UI card: own account left out, Delete all goes through every item');
}
console.log('    all passed');
process.exit(0);
