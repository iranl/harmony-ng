// Core HTTP and WebSocket client for Harmony Hub
import { t } from '../i18n/index.js';

export class HarmonyClientCore {
  constructor(hub = null) {
    this.wsPendingReqs = new Map();
    this.wsMsgSeq = 1;
    this.onWsMessage = null;
    this.onWsStatus = null;
    this.updateHub(hub);
  }

  buildUrl(path) {
    return this.baseUrl ? `${this.baseUrl}${path}` : path;
  }

  updateHub(hub) {
    this.hub = hub || {};
    const isBrowser = (typeof window !== 'undefined' && window.location && window.location.host);

    if (isBrowser && (!window.Capacitor || !window.Capacitor.isNativePlatform())) {
      this.baseUrl = '';
      this.wsUrl = `${window.location.protocol === 'https:' ? 'wss:' : 'ws:'}//${window.location.host}/`;
    } else {
      const host = this.hub.host;
      if (host) {
        const port = this.hub.port || 8089;
        this.baseUrl = `http://${host}:${port}`;
        this.wsUrl = `ws://${host}:${this.hub.wsPort || port}/`;
      } else {
        this.baseUrl = '';
        this.wsUrl = '';
      }
    }

    const savedToken = (typeof localStorage !== 'undefined') ? localStorage.getItem('harmony_auth_token') : null;
    this.token = this.hub.token || savedToken || null;

    if (this.token) {
      this.authHeader = this.token.startsWith('Bearer ') ? this.token : `Bearer ${this.token}`;
    } else if (this.hub.username && this.hub.password) {
      this.authHeader = `Basic ${btoa(`${this.hub.username}:${this.hub.password}`)}`;
    } else {
      this.authHeader = null;
    }

    if (this.wsUrl && (!this.ws || this.ws.readyState >= 2)) {
      this.connectWebSocket(true);
    } else if (!this.wsUrl) {
      this.disconnectWebSocket();
    }
  }

  connectWebSocket(force = false) {
    if (!this.wsUrl) return;
    // A forced connect (login, logout, a new Hub) starts again after a refused login
    if (force) this.wsPaused = false;
    if (this.wsPaused) return;

    if (!force && this.ws && (this.ws.readyState === WebSocket.OPEN || this.ws.readyState === WebSocket.CONNECTING)) {
      return;
    }
    this.disconnectWebSocket();

    try {
      let finalWsUrl = this.wsUrl;
      if (this.token) {
        const cleanToken = this.token.replace(/^Bearer\s+/i, '');
        const sep = finalWsUrl.includes('?') ? '&' : '?';
        finalWsUrl = `${finalWsUrl}${sep}token=${encodeURIComponent(cleanToken)}`;
      }
      this.ws = new WebSocket(finalWsUrl);

      this.ws.onopen = () => {
        if (this.onWsStatus) this.onWsStatus(true);
      };

      this.ws.onmessage = (event) => {
        try {
          const data = JSON.parse(event.data);
          if (data && data.msgId && this.wsPendingReqs.has(data.msgId)) {
            const cb = this.wsPendingReqs.get(data.msgId);
            this.wsPendingReqs.delete(data.msgId);
            cb(data);
          }
          if (data && this.onWsMessage) {
            this.onWsMessage(data);
          }
        } catch {
          // Ignore malformed WS frames
        }
      };

      this.ws.onerror = () => {
        if (this.onWsStatus) this.onWsStatus(false);
      };

      this.ws.onclose = () => {
        if (this.onWsStatus) this.onWsStatus(false);
        this.scheduleWsReconnect();
      };
    } catch {
      if (this.onWsStatus) this.onWsStatus(false);
      this.scheduleWsReconnect();
    }
  }

  scheduleWsReconnect() {
    if (this.wsReconnectTimer) clearTimeout(this.wsReconnectTimer);
    if (this.wsPaused) return;
    this.wsReconnectTimer = setTimeout(() => {
      this.connectWebSocket();
    }, 3000);
  }

  disconnectWebSocket() {
    if (this.wsReconnectTimer) {
      clearTimeout(this.wsReconnectTimer);
      this.wsReconnectTimer = null;
    }
    if (this.ws) {
      try {
        this.ws.onclose = null;
        this.ws.onerror = null;
        this.ws.close();
      } catch {
        // Socket close ignored
      }
      this.ws = null;
    }
  }

  // Drops the local session data only; the caller tells the Hub (logout) if it wants to
  clearStoredToken() {
    const hadToken = Boolean(this.token);
    this.token = null;
    if (this.authHeader && this.authHeader.startsWith('Bearer ')) this.authHeader = null;
    if (typeof localStorage !== 'undefined') {
      localStorage.removeItem('harmony_auth_token');
      localStorage.removeItem('harmony_auth_user');
      localStorage.removeItem('harmony_auth_role');
    }
    return hadToken;
  }

  // The Hub refused the credentials (expired or revoked session, Hub restarted, auth turned on):
  // forget the token, stop retrying the WebSocket with it until the next login, and let the app
  // ask for a login
  handleUnauthorized() {
    this.clearStoredToken();
    this.wsPaused = true;
    this.disconnectWebSocket();
    if (this.onWsStatus) this.onWsStatus(false);
    if (typeof window !== 'undefined' && window.dispatchEvent) {
      window.dispatchEvent(new CustomEvent('harmony:unauthorized'));
    }
  }

  getHeaders(extra = {}) {
    const headers = { ...extra };
    if (this.authHeader && !headers['Authorization']) {
      headers['Authorization'] = this.authHeader;
    }
    return headers;
  }

  async request(path, options = {}) {
    const url = this.buildUrl(path);
    const { ignoreUnauthorized, ...fetchOptions } = options;
    const opts = {
      ...fetchOptions,
      headers: this.getHeaders(options.headers || {})
    };

    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), options.timeout || 12000);
    opts.signal = controller.signal;

    try {
      const res = await fetch(url, opts);
      clearTimeout(timeout);
      if (res.status === 401) {
        if (!ignoreUnauthorized) this.handleUnauthorized();
        throw new Error(t('auth_failed_check_credentials', 'Authentication failed. Check hub credentials.'));
      }
      const text = await res.text();
      let j = null;
      try {
        j = JSON.parse(text);
      } catch {
        j = null;
      }

      if (!res.ok) {
        let errMsg = null;
        if (j && (j.error || j.message || j.reply)) {
          errMsg = j.error || j.message || j.reply;
        } else if (text && text.length < 200 && !text.includes('<!DOCTYPE')) {
          errMsg = text;
        }
        if (!errMsg) {
          errMsg = res.status === 409
            ? t('activity_switch_in_progress', 'Activity switch already in progress')
            : t('hub_returned_http', 'Hub returned HTTP {status}', { status: res.status });
        }
        // the reply stays readable for a caller that shows more than the message (lists of a 409)
        const httpErr = new Error(errMsg);
        httpErr.status = res.status;
        httpErr.data = j;
        throw httpErr;
      }

      return j !== null ? j : text;
    } catch (err) {
      clearTimeout(timeout);
      if (err.name === 'AbortError') {
        throw new Error(t('connection_timed_out', 'Connection timed out. Check IP and network.'));
      }
      throw err;
    }
  }

  async postForm(path, data = {}, extraOpts = {}) {
    const params = new URLSearchParams();
    for (const [k, v] of Object.entries(data)) {
      if (v !== undefined && v !== null) {
        params.append(k, String(v));
      }
    }
    return this.request(path, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/x-www-form-urlencoded',
        'Accept': 'application/json, text/plain, */*',
        'X-Requested-With': 'XMLHttpRequest'
      },
      body: params.toString(),
      ...extraOpts
    });
  }

  async postJson(path, data = {}, extraOpts = {}) {
    return this.request(path, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'Accept': 'application/json, text/plain, */*',
        'X-Requested-With': 'XMLHttpRequest'
      },
      body: JSON.stringify(data),
      ...extraOpts
    });
  }

  async testConnection() {
    try {
      if (!this.baseUrl && !this.hub.host) {
        return { ok: false, error: t('hub_not_configured', 'No hub configured') };
      }
      const data = await this.request('/health', { timeout: 3000 });
      return { ok: true, data };
    } catch {
      try {
        const data = await this.request('/api/status', { timeout: 3000 });
        return { ok: true, data };
      } catch (innerErr) {
        return { ok: false, error: innerErr.message };
      }
    }
  }

  static async checkHubHealth(host, port = 8089, timeout = 1500) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeout);
    try {
      const res = await fetch(`http://${host}:${port}/health`, {
        method: 'GET',
        signal: controller.signal,
        headers: { 'Accept': 'application/json, text/plain, */*' }
      });
      clearTimeout(timer);
      if (!res.ok) return { ok: false };
      const data = await res.json().catch(() => null);
      if (data && (data.service === 'hub_daemon' || data.status === 'ok')) {
        return { ok: true, data };
      }
      return { ok: res.status === 200, data };
    } catch {
      clearTimeout(timer);
      return { ok: false };
    }
  }

  static async fetchHubName(host, port = 8089, timeout = 2000) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeout);
    try {
      const res = await fetch(`http://${host}:${port}/api/status`, {
        method: 'GET',
        signal: controller.signal,
        headers: { 'Accept': 'application/json' }
      });
      clearTimeout(timer);
      if (res.ok) {
        const data = await res.json().catch(() => null);
        if (data && data.hub_name) return data.hub_name;
      }
    } catch {
      clearTimeout(timer);
    }
    return `Harmony Hub (${host})`;
  }

  isWsConnected() {
    return Boolean(this.ws && this.ws.readyState === WebSocket.OPEN);
  }

  sendWs(data) {
    if (!this.isWsConnected()) return false;
    try {
      this.ws.send(JSON.stringify(data));
      return true;
    } catch {
      return false;
    }
  }

  sendWsCommand(action, payload = {}) {
    return new Promise((resolve, reject) => {
      if (!this.isWsConnected()) {
        return reject(new Error('WS offline'));
      }
      const msgId = ++this.wsMsgSeq > 0x7FFFFFFF ? (this.wsMsgSeq = 1) : this.wsMsgSeq;
      const timer = setTimeout(() => {
        this.wsPendingReqs.delete(msgId);
        reject(new Error('WS timeout'));
      }, 4000);
      this.wsPendingReqs.set(msgId, (res) => {
        clearTimeout(timer);
        if (res && res.ok === false) {
          reject(new Error(res.error || 'WS command failed'));
        } else {
          resolve(res);
        }
      });
      try {
        this.ws.send(JSON.stringify({ ...payload, action, msgId }));
      } catch (err) {
        clearTimeout(timer);
        this.wsPendingReqs.delete(msgId);
        reject(err);
      }
    });
  }

  async ping(timeout = 3000) {
    return new Promise((resolve, reject) => {
      if (!this.isWsConnected()) {
        return reject(new Error('WS offline'));
      }
      const msgId = this.wsMsgSeq++;
      const timer = setTimeout(() => {
        this.wsPendingReqs.delete(msgId);
        reject(new Error('Ping timeout'));
      }, timeout);
      this.wsPendingReqs.set(msgId, (res) => {
        clearTimeout(timer);
        resolve(res);
      });
      try {
        this.ws.send(JSON.stringify({ action: 'ping', msgId }));
      } catch (err) {
        clearTimeout(timer);
        this.wsPendingReqs.delete(msgId);
        reject(err);
      }
    });
  }
}
