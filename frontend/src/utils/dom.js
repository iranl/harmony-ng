// Safe DOM helper utilities for Harmony Remote v3

export function escapeHtml(str) {
  if (str === null || str === undefined) return '';
  return String(str)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#039;');
}

export function qs(selector, parent = document) {
  return parent.querySelector(selector);
}

export function qsa(selector, parent = document) {
  return Array.from(parent.querySelectorAll(selector));
}

export function on(el, event, handler, options) {
  if (!el) return () => {};
  el.addEventListener(event, handler, options);
  return () => el.removeEventListener(event, handler, options);
}

export function h(tag, attrs = {}, children = []) {
  const el = document.createElement(tag);

  for (const [key, val] of Object.entries(attrs)) {
    if (val === null || val === undefined) continue;
    if (key === 'text') {
      el.textContent = val;
    } else if (key === 'html') {
      el.innerHTML = val;
    } else if (key === 'className' || key === 'class') {
      el.className = val;
    } else if (key.startsWith('on') && typeof val === 'function') {
      const eventName = key.slice(2).toLowerCase();
      el.addEventListener(eventName, val);
    } else if (key === 'dataset' && typeof val === 'object') {
      for (const [dk, dv] of Object.entries(val)) {
        el.dataset[dk] = dv;
      }
    } else if (key === 'style' && typeof val === 'object') {
      Object.assign(el.style, val);
    } else if (typeof val === 'boolean') {
      if (val) el.setAttribute(key, '');
    } else {
      el.setAttribute(key, String(val));
    }
  }

  const childList = Array.isArray(children) ? children : [children];
  for (const child of childList) {
    if (child === null || child === undefined || child === false) continue;
    if (typeof child === 'string' || typeof child === 'number') {
      el.appendChild(document.createTextNode(String(child)));
    } else if (child instanceof Node) {
      el.appendChild(child);
    }
  }

  return el;
}

export function clearChildren(el) {
  if (!el) return;
  while (el.firstChild) {
    el.removeChild(el.firstChild);
  }
}
