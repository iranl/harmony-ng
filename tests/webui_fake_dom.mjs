// A small stand-in for the browser DOM, enough for the web UI views under Node: elements with
// children, attributes, classes, listeners and simple selectors (#id, .class, tag, [attr=value]).
// No layout, no CSS. Installs document, window, localStorage and requestAnimationFrame globals.

export class FakeNode {
  constructor(tag) {
    this.tagName = String(tag).toLowerCase();
    this.children = [];
    this.parentNode = null;
    this.listeners = {};
    this.attrs = {};
    this.style = {};
    this.dataset = {};
    this._text = '';
    this.className = '';
    this.value = '';
    this.disabled = false;
    this.checked = false;
    const node = this;
    this.classList = {
      add: (...c) => { const set = node._classes(); c.forEach(x => set.add(x)); node.className = [...set].join(' '); },
      remove: (...c) => { const set = node._classes(); c.forEach(x => set.delete(x)); node.className = [...set].join(' '); },
      contains: c => node._classes().has(c),
      toggle: (c, on) => { const set = node._classes(); const want = on === undefined ? !set.has(c) : on; if (want) set.add(c); else set.delete(c); node.className = [...set].join(' '); return want; },
    };
  }
  _classes() { return new Set(String(this.className || '').split(/\s+/).filter(Boolean)); }
  get textContent() { return this._text + this.children.map(c => c.textContent).join(''); }
  set textContent(v) { this.children.forEach(c => { c.parentNode = null; }); this.children = []; this._text = String(v ?? ''); }
  get innerHTML() { return this._html ?? ''; }
  set innerHTML(v) { this.textContent = ''; this._html = String(v ?? ''); }
  get isConnected() { let n = this; while (n.parentNode) n = n.parentNode; return n === globalThis.document?.body; }
  get firstChild() { return this.children[0] || null; }
  appendChild(child) { if (child.parentNode) child.parentNode.removeChild(child); child.parentNode = this; this.children.push(child); return child; }
  append(...nodes) { nodes.forEach(n => this.appendChild(typeof n === 'string' ? globalThis.document.createTextNode(n) : n)); }
  removeChild(child) { this.children = this.children.filter(c => c !== child); child.parentNode = null; return child; }
  replaceChildren(...nodes) { this.textContent = ''; this.append(...nodes.filter(n => n !== null && n !== undefined)); }
  remove() { if (this.parentNode) this.parentNode.removeChild(this); }
  addEventListener(name, fn) { (this.listeners[name] ||= []).push(fn); }
  removeEventListener(name, fn) { this.listeners[name] = (this.listeners[name] || []).filter(f => f !== fn); }
  async fire(name, extra = {}) { const ev = { type: name, target: this, currentTarget: this, preventDefault() {}, stopPropagation() {}, ...extra }; for (const fn of [...(this.listeners[name] || [])]) await fn(ev); }
  setAttribute(k, v) { this.attrs[k] = String(v); if (k === 'id') this.id = String(v); if (k === 'class') this.className = String(v); if (k === 'value') this.value = String(v); }
  getAttribute(k) { return k in this.attrs ? this.attrs[k] : null; }
  removeAttribute(k) { delete this.attrs[k]; }
  hasAttribute(k) { return k in this.attrs; }
  focus() {} select() {} setSelectionRange() {} blur() {} click() { return this.fire('click'); }
  getContext() { return null; }
  getBoundingClientRect() { return { x: 0, y: 0, width: 0, height: 0, left: 0, top: 0 }; }
  *walk() { yield this; for (const c of this.children) yield* c.walk(); }
  matches(sel) {
    return sel.split(',').map(x => x.trim()).some(one => {
      const m = one.match(/^([a-zA-Z0-9-]*)((?:[#.][a-zA-Z0-9_-]+)*)((?:\[[^\]]+\])*)$/);
      if (!m) return false;
      if (m[1] && this.tagName !== m[1].toLowerCase()) return false;
      for (const part of m[2].match(/[#.][a-zA-Z0-9_-]+/g) || []) {
        if (part[0] === '#' && this.id !== part.slice(1)) return false;
        if (part[0] === '.' && !this._classes().has(part.slice(1))) return false;
      }
      for (const a of m[3].match(/\[[^\]]+\]/g) || []) {
        const [, k, v] = a.match(/^\[([^=\]]+)(?:="?([^"\]]*)"?)?\]$/);
        if (k === 'selected' || k === 'checked') { if (!this[k] && !(k in this.attrs)) return false; continue; }
        if (!(k in this.attrs)) return false;
        if (v !== undefined && this.attrs[k] !== v) return false;
      }
      return true;
    });
  }
  querySelectorAll(sel) {
    const parts = sel.trim().split(/\s+/);
    let scope = [this];
    for (const p of parts) {
      const next = [];
      for (const s of scope) for (const n of s.walk()) if (n !== s && n.matches(p) && !next.includes(n)) next.push(n);
      scope = next;
    }
    return scope;
  }
  querySelector(sel) { return this.querySelectorAll(sel)[0] || null; }
}

export function installFakeDom() {
  const body = new FakeNode('body');
  const html = new FakeNode('html');
  html.appendChild(body);
  globalThis.Node = FakeNode;
  globalThis.document = {
    body,
    documentElement: html,
    baseURI: 'http://192.168.1.20/',
    createElement: tag => new FakeNode(tag),
    createElementNS: (ns, tag) => new FakeNode(tag),
    createTextNode: text => Object.assign(new FakeNode('#text'), { _text: String(text) }),
    getElementById: id => [...body.walk()].find(n => n.id === id) || null,
    querySelector: sel => body.querySelector(sel),
    querySelectorAll: sel => body.querySelectorAll(sel),
    addEventListener() {},
    execCommand: () => false,
  };
  const store = new Map();
  globalThis.localStorage = {
    getItem: k => (store.has(k) ? store.get(k) : null),
    setItem: (k, v) => { store.set(k, String(v)); },
    removeItem: k => { store.delete(k); },
    clear: () => store.clear(),
  };
  const target = new EventTarget();
  globalThis.window = globalThis;
  globalThis.addEventListener = target.addEventListener.bind(target);
  globalThis.removeEventListener = target.removeEventListener.bind(target);
  globalThis.dispatchEvent = target.dispatchEvent.bind(target);
  globalThis.location = { reloads: 0, reload() { this.reloads++; }, protocol: 'http:', host: '', href: 'http://192.168.1.20/' };
  globalThis.requestAnimationFrame = fn => fn();
  globalThis.__REMOTE_RENDERER_VERSION__ = 'test';
  return { body };
}

// Runs the checks of one test file and prints them like the other suite tests
export function makeChecker(title) {
  let failures = 0;
  console.log(`[-] ${title}`);
  return {
    async check(name, fn) {
      try {
        await fn();
        console.log(`    ok   ${name}`);
      } catch (err) {
        failures++;
        console.log(`    FAIL ${name}: ${err.message}`);
      }
    },
    finish() {
      if (failures) {
        console.log(`[-] ${failures} check(s) failed`);
        process.exit(1);
      }
      console.log('    all passed');
    },
  };
}

export const tick = (ms = 0) => new Promise(r => setTimeout(r, ms));

export function texts(root, cls) {
  return [...root.walk()].filter(n => n._classes().has(cls)).map(n => n.textContent);
}

export function buttonByText(root, label) {
  return [...root.walk()].find(n => n.tagName === 'button' && n.textContent.trim() === label) || null;
}
