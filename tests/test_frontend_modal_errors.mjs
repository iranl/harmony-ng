// New web UI (frontend/): a dialog action whose Hub call fails must show the error and keep
// the dialog open (Create user, Generate token and the other createModal() dialogs).
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
globalThis.requestAnimationFrame = fn => fn();

const { createModal } = await import('../frontend/src/components/modal.js');

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

function button(root, label) {
  return [...root.walk()].find(n => n.tagName === 'button' && n.textContent === label);
}

function toastTexts() {
  return [...body.walk()].filter(n => n.className === 'toast-message').map(n => n.textContent);
}

console.log('[-] New web UI dialog actions with a failing Hub call');

await check('a rejected action shows the error and keeps the dialog open', async () => {
  const { backdrop } = createModal({
    title: 'Create API Token',
    content: new FakeNode('div'),
    actions: [{ label: 'Generate', onClick: async () => { throw new Error('Forbidden'); } }],
  });
  const btn = button(backdrop, 'Generate');
  assert.ok(btn, 'Generate button not found');
  for (const fn of btn.listeners.click) await fn();
  assert.ok(toastTexts().includes('Forbidden'), `no error shown (toasts: ${JSON.stringify(toastTexts())})`);
  assert.ok(backdrop.classList.contains('modal-visible'), 'the dialog was closed');
});

await check('a successful action still closes the dialog', async () => {
  const { backdrop } = createModal({
    title: 'Create User Account',
    content: new FakeNode('div'),
    actions: [{ label: 'Create', onClick: async () => true }],
  });
  for (const fn of button(backdrop, 'Create').listeners.click) await fn();
  assert.ok(!backdrop.classList.contains('modal-visible'), 'the dialog stayed open');
});

if (failures) {
  console.log(`[-] ${failures} check(s) failed`);
  process.exit(1);
}
console.log('    all passed');
