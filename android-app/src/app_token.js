// Login of the Android app: the password is used once to create a personal access token on the
// Hub, and only that token is kept in the Hub profile. The web UI in a browser keeps its session
// login (api_system_remotes.js login()).
import { getSavedHubs, saveHub, forgetStoredPasswords } from './storage.js';

export const APP_TOKEN_NAME = 'Android app';
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
    if (err && err.name === 'AbortError') throw new Error('Connection timed out. Check IP and network.');
    throw err;
  } finally {
    clearTimeout(timer);
  }
}

// Log in with the password, create the app's token, end the session. Returns
// { token, tokenId, username, role }, or { authOff: true, username } when the Hub has no
// authentication (then nothing needs to be kept). Throws when the Hub refuses the login.
export async function createAppToken(baseUrl, username, password, { timeout = 6000 } = {}) {
  const login = await hubCall(baseUrl, '/api/auth/token', {
    method: 'POST', body: { username, password }, timeout
  });
  if (login.status !== 200 || !login.data.ok || !login.data.access_token) {
    throw new Error(login.data.error || `Login failed (HTTP ${login.status})`);
  }
  const session = login.data.access_token;
  const user = login.data.username || username;
  let keepSession = false;
  try {
    const made = await hubCall(baseUrl, '/api/auth/tokens', {
      method: 'POST', token: session, body: { name: APP_TOKEN_NAME, role: login.data.role || 'user' }, timeout
    });
    if (made.status === 409) return { authOff: true, username: user };
    if (made.status === 403) {
      // A Hub that lets only admins create tokens: keep the session token instead (it ends when
      // the Hub restarts or after a day, then the app asks for the login again)
      keepSession = true;
      return { token: session, tokenId: null, username: user, role: login.data.role || 'user' };
    }
    if ((made.status !== 201 && made.status !== 200) || !made.data.token) {
      throw new Error(made.data.error || `Cannot create an access token (HTTP ${made.status})`);
    }
    return { token: made.data.token, tokenId: made.data.id || null, username: user, role: made.data.role || login.data.role || 'user' };
  } finally {
    // the session was only needed to create the token
    if (!keepSession) {
      await hubCall(baseUrl, '/api/auth/logout', { method: 'POST', token: session, timeout }).catch(() => {});
    }
  }
}

// Delete the profile's token on the Hub. False when the Hub could not be reached or refused;
// the caller forgets the token anyway.
export async function revokeAppToken(baseUrl, hub, { timeout = 4000 } = {}) {
  if (!baseUrl || !hub || !hub.token) return false;
  try {
    if (!hub.tokenId) {
      // a session token (see createAppToken)
      const r = await hubCall(baseUrl, '/api/auth/logout', { method: 'POST', token: hub.token, timeout });
      return r.status === 200;
    }
    const r = await hubCall(baseUrl, `/api/auth/tokens?id=${encodeURIComponent(hub.tokenId)}`, {
      method: 'DELETE', token: hub.token, timeout
    });
    return r.status === 200 || r.status === 404;
  } catch {
    return false;
  }
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
      const r = await create(hubBaseUrl(hub), hub.username || '', hub.password, { timeout });
      if (r && r.token) {
        await revokeAppToken(hubBaseUrl(hub), hub);
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
