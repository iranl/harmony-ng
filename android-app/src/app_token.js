// Login of the Android app: the password is used once to create a personal access token on the
// Hub, and only that token is kept in the Hub profile. The web UI in a browser keeps its session
// login (api_system_remotes.js login()).
import { getSavedHubs, saveHub, forgetStoredPasswords } from './storage.js';
import { t, tf } from './i18n.js';

export const APP_TOKEN_NAME = 'Android app';  // i18n-ok: token name stored on the Hub

// "Android app (<phone model>, <date>)": an admin can tell the phones apart in the token list.
// The Hub keeps names of up to 63 bytes.
export function appTokenName(userAgent = (typeof navigator !== 'undefined' ? navigator.userAgent : ''), now = new Date()) {
  const m = /Android [\d.]+; ([^;)]+?)(?: Build\/|\)|;)/.exec(userAgent || '');
  const date = now.toISOString().slice(0, 10);
  const fixed = `${APP_TOKEN_NAME} (, ${date})`;
  // the model gets the bytes left, cut between characters (never inside a UTF-8 sequence)
  const model = m ? cutUtf8(m[1].trim(), 63 - utf8Length(fixed)) : '';
  return `${APP_TOKEN_NAME} (${model ? `${model}, ` : ''}${date})`;
}

const utf8Encoder = new TextEncoder();

function utf8Length(text) {
  return utf8Encoder.encode(text).length;
}

function cutUtf8(text, maxBytes) {
  let out = '';
  for (const ch of text) {
    if (utf8Length(out + ch) > maxBytes) break;
    out += ch;
  }
  return out.trim();
}
const SESSION_KEYS = ['harmony_auth_token', 'harmony_auth_user', 'harmony_auth_role'];

export function isNativeApp() {
  return Boolean(typeof window !== 'undefined' && window.Capacitor &&
    typeof window.Capacitor.isNativePlatform === 'function' && window.Capacitor.isNativePlatform());
}

export function hubBaseUrl(hub) {
  return hub && hub.host ? `http://${hub.host}:${hub.port || 8089}` : '';
}

async function hubCall(baseUrl, path, { method = 'GET', token = null, body, timeout = 6000 } = {}) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeout);
  const headers = { 'Accept': 'application/json' };
  if (token) headers['Authorization'] = `Bearer ${token}`;
  if (body !== undefined) headers['Content-Type'] = 'application/json';
  try {
    const res = await fetch(`${baseUrl}${path}`, {
      method,
      headers,
      body: body !== undefined ? JSON.stringify(body) : undefined,
      signal: controller.signal
    });
    let data = null;
    try { data = await res.json(); } catch { /* no JSON body */ }
    return { status: res.status, data: data || {} };
  } catch (err) {
    if (err && err.name === 'AbortError') throw new Error(t('conn_timed_out_check_ip'));
    throw err;
  } finally {
    clearTimeout(timer);
  }
}

// Log in with the password, create the app's token, end the session. Returns
// { token, tokenId, username, role }, or { authOff: true, username } when the Hub has no
// authentication (then nothing needs to be kept). Throws when the Hub refuses the login.
// replace: { baseUrl, hub } of the login this one replaces. Its token is deleted once the new
// login is accepted and before the new token is made (an account at its token limit can then
// log in again); the result says oldRevoked: true, or false when the Hub could not delete it.
export async function createAppToken(baseUrl, username, password, { timeout = 6000, name = appTokenName(), replace = null } = {}) {
  // Authentication off: nothing to keep. Asked first, because firmware before 2.4.1 would make
  // a standing admin token while authentication is off.
  const me = await hubCall(baseUrl, '/api/auth/me', { timeout });
  if (me.status === 200 && me.data.auth_enabled === false) return { authOff: true, username };
  const login = await hubCall(baseUrl, '/api/auth/token', {
    method: 'POST', body: { username, password }, timeout
  });
  if (login.status !== 200 || !login.data.ok || !login.data.access_token) {
    throw new Error(login.data.error || tf('login_failed_http', { status: login.status }));
  }
  const session = login.data.access_token;
  const user = login.data.username || username;
  let keepSession = false;
  let oldRevoked;
  if (replace && replace.hub && replace.hub.token) oldRevoked = await revokeAppToken(replace.baseUrl, replace.hub);
  try {
    const made = await hubCall(baseUrl, '/api/auth/tokens', {
      method: 'POST', token: session, body: { name, role: login.data.role || 'user' }, timeout
    });
    if (made.status === 409 && /authentication/i.test(made.data.error || '')) return { authOff: true, username: user, oldRevoked };
    if (made.status === 403) {
      // A Hub that lets only admins create tokens: keep the session token instead (it ends when
      // the Hub restarts or after a day, then the app asks for the login again)
      keepSession = true;
      return { token: session, tokenId: null, username: user, role: login.data.role || 'user', oldRevoked };
    }
    if ((made.status !== 201 && made.status !== 200) || !made.data.token) {
      throw new Error(made.data.error || tf('token_create_failed_http', { status: made.status }));
    }
    return { token: made.data.token, tokenId: made.data.id || null, username: user, role: made.data.role || login.data.role || 'user', oldRevoked };
  } catch (err) {
    // the previous login is gone on the Hub already: the caller must not keep it
    if (oldRevoked === true) {
      err.oldRevoked = true;
      err.message = `${err.message} ${t('app_old_login_gone', 'The previous login was deleted on the Hub: log in again.')}`;
    }
    throw err;
  } finally {
    // the session was only needed to create the token
    if (!keepSession) {
      await hubCall(baseUrl, '/api/auth/logout', { method: 'POST', token: session, timeout }).catch(() => {});
    }
  }
}

// Test button: does the Hub accept this user name and password? Logs in and out again, keeps
// nothing. Resolves to { ok, authOff, error }.
export async function checkLogin(baseUrl, username, password, { timeout = 4000 } = {}) {
  const me = await hubCall(baseUrl, '/api/auth/me', { timeout });
  if (me.status === 200 && me.data.auth_enabled === false) return { ok: true, authOff: true };
  const login = await hubCall(baseUrl, '/api/auth/token', { method: 'POST', body: { username, password }, timeout });
  if (login.status !== 200 || !login.data.access_token) {
    return { ok: false, error: login.data.error || tf('login_failed_http', { status: login.status }) };
  }
  await hubCall(baseUrl, '/api/auth/logout', { method: 'POST', token: login.data.access_token, timeout }).catch(() => {});
  return { ok: true };
}

// Test button: client.testConnection() that also checks a typed password with one login
export function withLoginCheck(client, username, password) {
  const plain = client.testConnection.bind(client);
  return async () => {
    const res = await plain();
    if (!res.ok || !password) return res;
    const lr = await checkLogin(client.baseUrl, username, password).catch(err => ({ ok: false, error: err.message }));
    return lr.ok ? res : { ok: false, error: lr.error };
  };
}

// Delete the profile's token on the Hub. False when the Hub could not be reached or refused;
// the caller forgets the token anyway.
export async function revokeAppToken(baseUrl, hub, { timeout = 4000 } = {}) {
  if (!baseUrl || !hub || !hub.token) return false;
  try {
    if (!hub.tokenId) {
      // a session token (see createAppToken)
      const r = await hubCall(baseUrl, '/api/auth/logout', { method: 'POST', token: hub.token, timeout });
      return r.status === 200 || r.status === 401;
    }
    const r = await hubCall(baseUrl, `/api/auth/tokens?id=${encodeURIComponent(hub.tokenId)}`, {
      method: 'DELETE', token: hub.token, timeout
    });
    // 404: already deleted; 401: the token no longer works (deleted, or its account is gone)
    return r.status === 200 || r.status === 404 || r.status === 401;
  } catch {
    return false;
  }
}

// After a login that replaced old (createAppToken with replace): delete the old login now if that
// was not possible before. Returns true when the Hub may still hold it (tell the user).
export async function finishReplace(result, old) {
  if (!old || !old.hub || !old.hub.token || old.hub.token === result.token) return false;
  if (result.oldRevoked === true) return false;
  return !(await revokeAppToken(old.baseUrl, old.hub));
}

// The profile fields that hold the login (never a password)
export function loginFields(result) {
  if (!result || !result.token) return { token: null, tokenId: null };
  return { token: result.token, tokenId: result.tokenId, username: result.username, role: result.role };
}

// App start: profiles saved by older versions hold the password. Trade each for a token while
// the Hub answers; in every case the password is removed (a Hub that could not be reached asks
// for a login when it next refuses a request). Old session tokens of the app go too: the app
// logs in with its token now.
export async function migrateStoredPasswords({ create = createAppToken, timeout = 4000 } = {}) {
  const hubs = await getSavedHubs();
  let traded = 0;
  for (const hub of hubs) {
    if (!hub.password) continue;
    let fields = {};
    try {
      const r = await create(hubBaseUrl(hub), hub.username || '', hub.password,
        { timeout, replace: { baseUrl: hubBaseUrl(hub), hub } });
      if (r && r.token) {
        if (r.oldRevoked === false) await revokeAppToken(hubBaseUrl(hub), hub);
        fields = loginFields(r);
        traded++;
      } else if (r && r.authOff) {
        fields = { token: null, tokenId: null };
      }
    } catch {
      /* refused or unreachable: log in again when the Hub asks */
    }
    await saveHub({ ...hub, ...fields });
  }
  const dropped = await forgetStoredPasswords();
  if (typeof localStorage !== 'undefined') {
    SESSION_KEYS.forEach(k => localStorage.removeItem(k));
  }
  return { traded, dropped };
}
