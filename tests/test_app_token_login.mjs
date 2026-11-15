// Legacy UI as the Android app (android-app/ in Capacitor): the Hub password is never stored.
// Logging in creates a personal access token and only the token is kept in the Hub profile;
// profiles saved with a password by older versions are traded for a token at start; logging out
// and deleting a profile delete the token on the Hub. The web UI in a browser keeps its session.
// Runs in Node with a stand-in DOM and a stubbed fetch: no phone, no browser, no Hub.
import assert from 'node:assert/strict';
import { installFakeDom, allowJsonImports, makeChecker, tick } from './webui_fake_dom.mjs';

const { body } = installFakeDom();
await allowJsonImports();
globalThis.WebSocket = class { constructor(url) { this.url = url; this.readyState = 0; } close() {} };
globalThis.WebSocket.OPEN = 1;
globalThis.WebSocket.CONNECTING = 0;

const native = on => { globalThis.Capacitor = on ? { isNativePlatform: () => true } : undefined; };

// Fake Hubs by address: 192.168.1.20 answers, 192.168.1.21 is unreachable
let calls = [];
let hubState;
function resetHub({ tokensStatus = 201, authOn = true, tokensError = 'refused' } = {}) {
  hubState = { tokensStatus, authOn, tokensError, next: 1, pats: new Map(), sessions: new Set(), names: [] };
}
resetHub();
globalThis.fetch = async (url, opts = {}) => {
  const u = new URL(String(url), 'http://192.168.1.20:8089');
  const auth = (opts.headers && opts.headers.Authorization) || '';
  const call = { host: u.hostname, path: u.pathname + u.search, method: opts.method || 'GET', auth, body: opts.body };
  calls.push(call);
  if (u.hostname === '192.168.1.21') throw new TypeError('Failed to fetch');
  const reply = (status, obj) => new Response(JSON.stringify(obj), { status });
  const bearer = auth.replace(/^Bearer /, '');
  if (u.pathname === '/api/auth/me') {
    if (hubState.authOn && !bearer) return reply(401, { ok: false, error: 'Unauthorized' });
    return reply(200, { ok: true, auth_enabled: hubState.authOn });
  }
  if (u.pathname === '/health') return reply(200, { status: 'ok' });
  if (u.pathname === '/api/auth/token') {
    const b = JSON.parse(opts.body);
    if (b.password !== 'secret') return reply(401, { ok: false, error: 'Invalid username or password' });
    const s = `hht_s${hubState.next++}`;
    hubState.sessions.add(s);
    return reply(200, { ok: true, access_token: s, username: b.username, role: 'user' });
  }
  if (u.pathname === '/api/auth/tokens' && call.method === 'POST') {
    if (!hubState.sessions.has(bearer)) return reply(401, { ok: false });
    if (!hubState.authOn) return reply(409, { ok: false, error: 'Turn on authentication before creating access tokens' });
    if (hubState.tokensStatus !== 201) return reply(hubState.tokensStatus, { ok: false, error: hubState.tokensError });
    const id = `tok_${hubState.next}`;
    const tok = `pat_${hubState.next++}`;
    hubState.pats.set(id, tok);
    const b = JSON.parse(opts.body);
    hubState.names.push(b.name);
    return reply(201, { ok: true, id, name: b.name, role: 'user', token: tok });
  }
  if (u.pathname === '/api/auth/tokens' && call.method === 'DELETE') {
    const id = u.searchParams.get('id');
    if (hubState.authOn && hubState.pats.get(id) !== bearer) return reply(404, { ok: false });
    if (!hubState.pats.has(id)) return reply(404, { ok: false });
    hubState.pats.delete(id);
    return reply(200, { ok: true });
  }
  if (u.pathname === '/api/auth/logout') {
    hubState.sessions.delete(bearer);
    return reply(200, { ok: true });
  }
  return reply(200, { ok: true });
};

const storage = await import('../android-app/src/storage.js');
const appToken = await import('../android-app/src/app_token.js');
const { migrateStoredPasswords, APP_TOKEN_NAME } = appToken;
const { HarmonyClient } = await import('../android-app/src/api.js');
const { HarmonyAppHubsNetwork } = await import('../android-app/src/app/app_hubs_network.js');

const t = makeChecker('Android app keeps an access token, never the Hub password');
const rawHubs = () => localStorage.getItem('harmony_saved_hubs') || '';
const stored = id => JSON.parse(rawHubs()).find(h => h.id === id);
const HUB = { id: 'hub_1', name: 'Living', host: '192.168.1.20', port: 8089 };

await t.check('start: a saved password becomes a token, no password is left', async () => {
  native(true);
  resetHub();
  localStorage.setItem('harmony_saved_hubs', JSON.stringify([
    { ...HUB, username: 'alice', password: 'secret' },
    { id: 'hub_2', name: 'Away', host: '192.168.1.21', username: 'bob', password: 'secret' },
    { id: 'hub_3', name: 'Old', host: '127.0.0.1', password: 'secret' }
  ]));
  localStorage.setItem('harmony_auth_token', 'hht_old');
  calls = [];
  const r = await migrateStoredPasswords({ timeout: 500 });
  assert.equal(r.traded, 1);
  assert.ok(!rawHubs().includes('password'), `password still stored: ${rawHubs()}`);
  const h1 = stored('hub_1');
  assert.match(h1.token || '', /^pat_/);
  assert.ok(h1.tokenId, 'no token id kept');
  assert.equal(h1.username, 'alice');
  assert.equal(stored('hub_2').token, undefined, 'unreachable Hub got a token');
  const seq = calls.filter(c => c.host === '192.168.1.20').map(c => `${c.method} ${c.path}`);
  assert.deepEqual(seq, ['GET /api/auth/me', 'POST /api/auth/token', 'POST /api/auth/tokens', 'POST /api/auth/logout']);
  const made = calls.find(c => c.path === '/api/auth/tokens');
  assert.ok(JSON.parse(made.body).name.startsWith(`${APP_TOKEN_NAME} (`), JSON.parse(made.body).name);
  assert.equal(hubState.sessions.size, 0, 'the login session was not ended');
  assert.equal(localStorage.getItem('harmony_auth_token'), null, 'old session token kept');
});

await t.check('the app sends its token, no password, and ignores the browser session token', async () => {
  native(true);
  localStorage.setItem('harmony_auth_token', 'hht_browser');
  const c = new HarmonyClient(stored('hub_1'));
  calls = [];
  await c.request('/api/status');
  assert.equal(calls[0].auth, `Bearer ${stored('hub_1').token}`);
  const c2 = new HarmonyClient(stored('hub_2'));
  calls = [];
  await c2.request('/api/status').catch(() => {});
  assert.equal(calls[0].auth, '', `sent ${calls[0].auth}`);
  // a profile that still had a password (start-up update failed): not sent either
  const c3 = new HarmonyClient({ id: 'x', host: '192.168.1.20', username: 'alice', password: 'secret' });
  calls = [];
  await c3.request('/api/status');
  assert.equal(calls[0].auth, '', `sent ${calls[0].auth}`);
  localStorage.removeItem('harmony_auth_token');
});

await t.check('login in the app keeps a new token, deletes the old one, stores no password', async () => {
  native(true);
  const before = stored('hub_1');
  const c = new HarmonyClient(before);
  calls = [];
  const r = await c.login('alice', 'secret');
  assert.equal(r.ok, true);
  const after = stored('hub_1');
  assert.match(after.token, /^pat_/);
  assert.notEqual(after.token, before.token);
  assert.equal(c.authHeader, `Bearer ${after.token}`);
  assert.ok(!rawHubs().includes('secret'), 'password stored');
  assert.ok(!hubState.pats.has(before.tokenId), 'old token not deleted on the Hub');
  assert.equal(localStorage.getItem('harmony_auth_token'), null, 'session token stored in the app');
});

await t.check('a wrong password stores nothing and keeps the current login', async () => {
  native(true);
  const before = stored('hub_1');
  const c = new HarmonyClient(before);
  await assert.rejects(c.login('alice', 'wrong'), /Invalid username or password/);
  assert.equal(stored('hub_1').token, before.token);
});

await t.check('logout deletes the token on the Hub and in the profile', async () => {
  native(true);
  const before = stored('hub_1');
  const c = new HarmonyClient(before);
  calls = [];
  await c.logout();
  const del = calls.find(x => x.method === 'DELETE');
  assert.ok(del, 'no DELETE sent');
  assert.equal(del.path, `/api/auth/tokens?id=${encodeURIComponent(before.tokenId)}`);
  assert.equal(del.auth, `Bearer ${before.token}`);
  assert.ok(!hubState.pats.has(before.tokenId), 'token still on the Hub');
  assert.equal(stored('hub_1').token, null);
  assert.equal(c.token, null);
  assert.equal(c.authHeader, null);
});

await t.check('logout with the Hub unreachable still forgets the token', async () => {
  native(true);
  storage.saveHub({ id: 'hub_2', host: '192.168.1.21', token: 'pat_far', tokenId: 'tok_far' });
  await tick();
  const c = new HarmonyClient(stored('hub_2'));
  const out = await c.logout();
  assert.equal(out.revoked, false, 'the app must be able to tell the user the token stays on the Hub');
  assert.equal(stored('hub_2').token, null);
});

await t.check('a Hub without authentication: login keeps nothing and deletes the old token', async () => {
  native(true);
  resetHub();
  const c = new HarmonyClient(stored('hub_1'));
  await c.login('alice', 'secret');
  const oldId = stored('hub_1').tokenId;
  assert.ok(hubState.pats.has(oldId));
  hubState.authOn = false;
  calls = [];
  const r = await c.login('alice', 'secret');
  assert.equal(r.auth_off, true);
  assert.equal(stored('hub_1').token, null);
  assert.ok(!hubState.pats.has(oldId), 'old token left on the Hub (valid again when authentication is back)');
  assert.ok(!calls.some(x => x.path === '/api/auth/token' || x.path === '/api/auth/tokens' && x.method === 'POST'),
    'logged in or made a token while authentication is off');
  assert.ok(!rawHubs().includes('secret'));
  resetHub();
});

await t.check('a token limit on the Hub: the reason is shown, nothing changes', async () => {
  native(true);
  resetHub();
  const c = new HarmonyClient(stored('hub_1'));
  await c.login('alice', 'secret');
  const before = rawHubs();
  hubState.tokensStatus = 409;
  hubState.tokensError = 'This account already has 10 access tokens: delete one first';
  await assert.rejects(c.login('alice', 'secret'), /10 access tokens/);
  assert.equal(rawHubs(), before);
  assert.equal(hubState.sessions.size, 0, 'session left open');
  resetHub();
});

await t.check('token name tells the phones apart', async () => {
  const { appTokenName } = appToken;
  assert.equal(typeof appTokenName, 'function', 'no appTokenName()');
  const ua = 'Mozilla/5.0 (Linux; Android 14; Pixel 7 Build/UQ1A.240205.004; wv) AppleWebKit/537.36';
  assert.equal(appTokenName(ua, new Date('2026-11-15T10:00:00Z')), 'Android app (Pixel 7, 2026-11-15)');
  assert.equal(appTokenName('', new Date('2026-11-15T10:00:00Z')), 'Android app (2026-11-15)');
  const long = 'Mozilla/5.0 (Linux; Android 13; A very long phone model name that goes on; wv)';
  assert.ok(Buffer.byteLength(appTokenName(long)) <= 63);
});

await t.check('a Hub that refuses tokens to operators: the session token is kept, logout ends it', async () => {
  native(true);
  resetHub({ tokensStatus: 403 });
  const c = new HarmonyClient(stored('hub_1'));
  await c.login('alice', 'secret');
  const h = stored('hub_1');
  assert.match(h.token, /^hht_/);
  assert.equal(h.tokenId, null);
  assert.ok(hubState.sessions.has(h.token), 'session ended right away');
  calls = [];
  const out = await c.logout();
  assert.equal(out.revoked, true);
  assert.deepEqual(calls.map(x => `${x.method} ${x.path}`), ['POST /api/auth/logout']);
  assert.equal(hubState.sessions.size, 0);
  resetHub();
});

await t.check('browser web UI keeps its session login in localStorage', async () => {
  native(false);
  const c = new HarmonyClient();
  calls = [];
  await c.login('alice', 'secret');
  assert.deepEqual(calls.map(x => x.path), ['/api/auth/token']);
  assert.match(localStorage.getItem('harmony_auth_token'), /^hht_/);
  await c.logout();
  assert.equal(localStorage.getItem('harmony_auth_token'), null);
});

// The hub profile dialog: the stand-in DOM does not parse innerHTML, so the dialog's fields are
// created from the ids in its HTML.
function hubDialog() {
  let modal = document.getElementById('hubModal');
  if (!modal) {
    modal = document.createElement('div');
    modal.setAttribute('id', 'hubModal');
    body.appendChild(modal);
    Object.defineProperty(modal, 'innerHTML', {
      get() { return this._html || ''; },
      set(v) {
        this.textContent = '';
        this._html = String(v);
        for (const m of this._html.matchAll(/<(input|button|div)\b[^>]*\bid="([^"]+)"/g)) {
          const el = document.createElement(m[1]);
          el.setAttribute('id', m[2]);
          this.appendChild(el);
        }
      }
    });
  }
  return modal;
}

function hubApp() {
  const a = Object.create(HarmonyAppHubsNetwork.prototype);
  a.client = new HarmonyClient();
  a.loadActiveHub = async () => {};
  a.renderHubsList = async () => {};
  return a;
}

await t.check('profile dialog: the password is never shown or saved, a token is', async () => {
  native(true);
  resetHub();
  const modal = hubDialog();
  const h = { ...stored('hub_1'), token: 'pat_shown', password: 'secret' };
  hubApp().openEditHubModal(h);
  assert.ok(!modal.innerHTML.includes('secret'), 'password put into the dialog');
  assert.ok(!modal.innerHTML.includes('pat_shown'), 'token put into the dialog');
  modal.querySelector('#hubFormName').value = 'Living';
  modal.querySelector('#hubFormHost').value = '192.168.1.20';
  modal.querySelector('#hubFormPort').value = '8089';
  modal.querySelector('#hubFormUser').value = 'alice';
  modal.querySelector('#hubFormPass').value = 'secret';
  await modal.querySelector('#btnHubSave').onclick();
  const saved = stored('hub_1');
  assert.match(saved.token, /^pat_/);
  assert.notEqual(saved.token, 'pat_shown');
  assert.ok(!rawHubs().includes('secret'), 'password stored');
});

await t.check('profile dialog: a refused login shows the reason and saves nothing', async () => {
  native(true);
  const modal = hubDialog();
  const before = rawHubs();
  hubApp().openEditHubModal({ ...stored('hub_1') });
  modal.querySelector('#hubFormHost').value = '192.168.1.20';
  modal.querySelector('#hubFormPass').value = 'wrong';
  await modal.querySelector('#btnHubSave').onclick();
  assert.equal(rawHubs(), before);
  assert.match(modal.querySelector('#hubTestStatus').innerHTML, /Invalid username or password/);
});

await t.check('profile dialog: saving without a password keeps the current token', async () => {
  native(true);
  const modal = hubDialog();
  const before = stored('hub_1');
  hubApp().openEditHubModal({ ...before });
  modal.querySelector('#hubFormName').value = 'Renamed';
  modal.querySelector('#hubFormHost').value = '192.168.1.20';
  modal.querySelector('#hubFormPort').value = '8089';
  modal.querySelector('#hubFormUser').value = 'alice';
  modal.querySelector('#hubFormPass').value = '';
  await modal.querySelector('#btnHubSave').onclick();
  assert.equal(stored('hub_1').token, before.token);
  assert.equal(stored('hub_1').name, 'Renamed');
});

await t.check('profile dialog: a login made since the dialog opened is kept (stale copy)', async () => {
  native(true);
  resetHub();
  const modal = hubDialog();
  const stale = { ...stored('hub_1') };
  // log in again through the login dialog while the Hubs page keeps the old copy
  const c = new HarmonyClient(stored('hub_1'));
  await c.login('alice', 'secret');
  const fresh = stored('hub_1').token;
  hubApp().openEditHubModal(stale);
  modal.querySelector('#hubFormName').value = 'Living';
  modal.querySelector('#hubFormHost').value = '192.168.1.20';
  modal.querySelector('#hubFormPort').value = '8089';
  modal.querySelector('#hubFormUser').value = 'alice';
  modal.querySelector('#hubFormPass').value = '';
  await modal.querySelector('#btnHubSave').onclick();
  assert.equal(stored('hub_1').token, fresh);
});

await t.check('profile dialog: a second profile for the same Hub deletes the replaced token', async () => {
  native(true);
  const modal = hubDialog();
  const oldId = stored('hub_1').tokenId;
  hubApp().openEditHubModal({ id: 'hub_new', name: 'Again', host: '', port: 8089 }, false);
  modal.querySelector('#hubFormName').value = 'Again';
  modal.querySelector('#hubFormHost').value = '192.168.1.20';
  modal.querySelector('#hubFormPort').value = '8089';
  modal.querySelector('#hubFormUser').value = 'alice';
  modal.querySelector('#hubFormPass').value = 'secret';
  await modal.querySelector('#btnHubSave').onclick();
  assert.ok(!hubState.pats.has(oldId), 'replaced token still on the Hub');
  assert.equal(hubState.pats.size, 1);
});

await t.check('profile dialog: another user name without a password is refused', async () => {
  native(true);
  const modal = hubDialog();
  const before = rawHubs();
  hubApp().openEditHubModal({ ...stored('hub_1') });
  modal.querySelector('#hubFormName').value = 'Living';
  modal.querySelector('#hubFormHost').value = '192.168.1.20';
  modal.querySelector('#hubFormPort').value = '8089';
  modal.querySelector('#hubFormUser').value = 'bob';
  modal.querySelector('#hubFormPass').value = '';
  await modal.querySelector('#btnHubSave').onclick();
  assert.equal(rawHubs(), before);
  assert.match(modal.querySelector('#hubTestStatus').innerHTML, /password/i);
});

await t.check('profile dialog: Test sends no password with requests, checks the login once', async () => {
  native(true);
  const modal = hubDialog();
  hubApp().openEditHubModal({ ...stored('hub_1') });
  modal.querySelector('#hubFormHost').value = '192.168.1.20';
  modal.querySelector('#hubFormPort').value = '8089';
  modal.querySelector('#hubFormUser').value = 'alice';
  modal.querySelector('#hubFormPass').value = 'secret';
  const before = rawHubs();
  calls = [];
  await modal.querySelector('#btnHubTest').onclick();
  assert.ok(!calls.some(x => x.auth.startsWith('Basic')), 'Basic sent');
  assert.ok(calls.some(x => x.path === '/api/auth/token'), 'login not checked');
  assert.equal(hubState.sessions.size, 0, 'test session left open');
  assert.match(modal.querySelector('#hubTestStatus').innerHTML, /Connected/);
  assert.equal(rawHubs(), before);
  modal.querySelector('#hubFormPass').value = 'wrong';
  await modal.querySelector('#btnHubTest').onclick();
  assert.match(modal.querySelector('#hubTestStatus').innerHTML, /Invalid username or password/);
});

await t.check('header badge shows the account, not the token name', async () => {
  native(true);
  const { HarmonyAppActivitiesState } = await import('../android-app/src/app/app_activities_state.js');
  const a = Object.create(HarmonyAppActivitiesState.prototype);
  a.client = new HarmonyClient({ id: 'hub_1', host: '192.168.1.20', username: 'alice', token: 'pat_x' });
  a.client.getInventory = async () => ({ devices: [] });
  a.client.getActivities = async () => ({ activities: [] });
  a.client.getStatus = async () => ({ user: 'pat:Android app (Pixel 7, 2026-11-15)', role: 'user', auth_enabled: true });
  a.applyRoleRestrictions = () => {};
  a.updateIconCacheUI = () => {};
  a.updateActivityBanner = () => {};
  a.loadAvailableIcons = () => {};
  a.currentTab = 'none';
  await a.refreshData().catch(() => {});
  assert.equal(a.currentUser, 'alice');
});

await t.check('token list rows show the owner', async () => {
  const { showTokenOwners } = await import('../android-app/src/app/app_auth_users.js');
  const list = document.createElement('div');
  const row = document.createElement('div');
  const info = document.createElement('div');
  const meta = document.createElement('div');
  meta.className = 'mini muted';
  info.appendChild(meta);
  row.appendChild(info);
  const btn = document.createElement('button');
  btn.className = 'btn-del-pat';
  btn.setAttribute('data-id', 'tok_1');
  row.appendChild(btn);
  list.appendChild(row);
  showTokenOwners(list, [{ id: 'tok_1', owner: 'alice' }]);
  assert.match(meta.textContent, /Owner: alice/);
});

t.finish();
process.exit(0);
