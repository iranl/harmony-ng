// New web UI (frontend/): what happens when the Hub refuses the saved login (401), Log Out, the
// token role and the user name limits of the Users page.
// Runs in Node with a stand-in DOM and a stubbed fetch: no browser, no Hub.
import assert from 'node:assert/strict';
import { installFakeDom, makeChecker, tick, texts, buttonByText } from './webui_fake_dom.mjs';

const { body } = installFakeDom();
const { api } = await import('../frontend/src/api/index.js');
const { installUnauthorizedHandler } = await import('../frontend/src/components/login-modal.js');
const { createView: createUsersView } = await import('../frontend/src/views/setup/users.js');

const t = makeChecker('New web UI login handling');
installUnauthorizedHandler();

// fetch stub: route -> reply; records every call
let calls = [];
function stubFetch(routes) {
  calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    const path = String(url).replace(/^https?:\/\/[^/]+/, '');
    calls.push({ path, method: opts.method || 'GET', headers: opts.headers || {}, body: opts.body });
    const key = Object.keys(routes).find(k => path.startsWith(k));
    const r = key ? routes[key] : { status: 404, body: { ok: false, error: 'Not Found' } };
    if (r.delay) await tick(r.delay);
    return new Response(JSON.stringify(r.body ?? { ok: true }), { status: r.status || 200 });
  };
}

function login(token = 'tok-123') {
  localStorage.setItem('harmony_auth_token', token);
  localStorage.setItem('harmony_auth_user', 'alice');
  localStorage.setItem('harmony_auth_role', 'admin');
  api.token = token;
  api.authHeader = `Bearer ${token}`;
}

function dialogs() {
  return [...body.walk()].filter(n => n._classes().has('modal-backdrop') && n.parentNode);
}

function closeDialogs() {
  dialogs().forEach(d => d.remove());
}

await t.check('a 401 drops the saved token and stops the WebSocket retries', async () => {
  login();
  stubFetch({ '/api/settings': { status: 401, body: { ok: false, error: 'Unauthorized' } } });
  await assert.rejects(api.request('/api/settings'));
  assert.equal(localStorage.getItem('harmony_auth_token'), null, 'token still stored');
  assert.equal(localStorage.getItem('harmony_auth_user'), null, 'user still stored');
  assert.equal(api.token, null, 'client still holds the token');
  assert.equal(api.wsPaused, true, 'WebSocket retries not stopped');
  assert.equal(api.wsReconnectTimer || null, null, 'a WebSocket retry is still scheduled');
  // the next request goes out without the dead token
  await assert.rejects(api.request('/api/settings'));
  assert.equal(calls[1].headers.Authorization, undefined, `still sends ${calls[1].headers.Authorization}`);
});

await t.check('a 401 opens the login dialog once', async () => {
  stubFetch({ '/api/activities': { status: 401 } });
  for (let i = 0; i < 3; i++) await assert.rejects(api.request('/api/activities'));
  const open = dialogs().filter(d => texts(d, 'modal-title').includes('Log In to Harmony Hub'));
  assert.equal(open.length, 1, `${open.length} login dialogs open`);
  closeDialogs();
});

await t.check('Log Out forgets the session before the Hub answers and reloads after', async () => {
  login('tok-slow');
  location.reloads = 0;
  stubFetch({ '/api/users': { body: [] }, '/api/auth/tokens': { body: [] }, '/api/auth/logout': { delay: 600, body: { ok: true } } });
  const view = createUsersView();
  body.appendChild(view);
  await tick(10);
  const btn = buttonByText(view, 'Log Out');
  assert.ok(btn, 'no Log Out button');
  const done = btn.fire('click');
  await tick(20);
  assert.equal(localStorage.getItem('harmony_auth_token'), null, 'token still stored while the logout request runs');
  await tick(400);
  assert.equal(location.reloads, 0, 'page reloaded before the logout request finished');
  await done;
  const logout = calls.find(c => c.path === '/api/auth/logout');
  assert.equal(logout?.headers.Authorization, 'Bearer tok-slow', 'logout request without the session token');
  await tick(350);
  assert.equal(location.reloads, 1, 'no reload after the logout');
  view.remove();
});

await t.check('a failed user list shows the Hub error, not a guess', async () => {
  login();
  stubFetch({ '/api/users': { status: 403, body: { ok: false, error: 'Forbidden' } }, '/api/auth/tokens': { body: [] } });
  const view = createUsersView();
  body.appendChild(view);
  await tick(10);
  const mount = view.querySelector('#users-list-mount');
  assert.match(mount.textContent, /Forbidden/);
  view.remove();
});

await t.check('the token dialog offers the operator role', async () => {
  login();
  closeDialogs();
  stubFetch({ '/api/users': { body: [] }, '/api/auth/tokens': { body: { ok: true, token: 'pat-1' } } });
  const view = createUsersView();
  body.appendChild(view);
  await tick(10);
  await buttonByText(view, '+ Create Token').fire('click');
  const dlg = dialogs().find(d => texts(d, 'modal-title').includes('Create API Token'));
  assert.ok(dlg, 'no token dialog');
  const sel = dlg.querySelector('select');
  assert.ok(sel, 'no role choice in the dialog');
  dlg.querySelector('input').value = 'Kitchen script';
  sel.value = 'user';
  await buttonByText(dlg, 'Generate').fire('click');
  const post = calls.find(c => c.path === '/api/auth/tokens' && c.method === 'POST');
  assert.equal(JSON.parse(post.body).role, 'user');
  closeDialogs();
  view.remove();
});

await t.check('a user name the Hub cannot keep is refused before saving', async () => {
  login();
  closeDialogs();
  stubFetch({ '/api/users': { body: [] }, '/api/auth/tokens': { body: [] } });
  const view = createUsersView();
  body.appendChild(view);
  await tick(10);
  await buttonByText(view, '+ Add User').fire('click');
  const dlg = dialogs().find(d => texts(d, 'modal-title').includes('Create User Account'));
  const [user, pass] = dlg.querySelectorAll('input');
  assert.equal(user.getAttribute('maxlength'), '31');
  user.value = 'living-room-harmony-hub-owner-2026';
  pass.value = 'secret';
  await buttonByText(dlg, 'Create').fire('click');
  assert.ok(!calls.some(c => c.method === 'POST' && c.path.startsWith('/api/users')), 'the long name was sent');
  assert.ok(dlg.parentNode && dlg.classList.contains('modal-visible'), 'the dialog closed');
  assert.ok(texts(body, 'toast-message').some(m => /too long/.test(m)), 'no reason shown');
  closeDialogs();
  view.remove();
});

await t.check('opening the Users page again does not add another 401 listener', async () => {
  login();
  closeDialogs();
  stubFetch({ '/api/users': { body: [] }, '/api/auth/tokens': { body: [] } });
  const add = window.addEventListener;
  let added = 0;
  window.addEventListener = (name, fn, opts) => {
    if (name === 'harmony:unauthorized') added++;
    return add(name, fn, opts);
  };
  let view;
  try {
    for (let i = 0; i < 4; i++) {
      if (view) view.remove();
      view = createUsersView();
      body.appendChild(view);
    }
  } finally {
    window.addEventListener = add;
  }
  assert.ok(added <= 1, `${added} listeners added for 4 visits`);
  await tick(10);
  // the open view still follows a 401: its card says logged out
  stubFetch({ '/api/settings': { status: 401 } });
  await assert.rejects(api.request('/api/settings'));
  assert.ok(buttonByText(view, 'Log In'), 'the open view did not redraw after the 401');
  closeDialogs();
  view.remove();
});

t.finish();
