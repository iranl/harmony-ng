// System, Network, MQTT, Backup, Icons & Auth API methods for Harmony Hub
import { t } from '../i18n/index.js';

export function createSystemApi(client) {
  return {
    async getHealth() {
      return client.request('/health');
    },

    async getStatus() {
      return client.request('/api/status');
    },

    async getSystemInfo() {
      return client.request('/api/system-info');
    },

    async getServices() {
      return client.request('/api/services');
    },

    async toggleRfService(enable) {
      return client.postJson('/api/services/rf-toggle', { enable: Boolean(enable) });
    },

    async toggleBtService(enable) {
      return client.postJson('/api/services/bt-toggle', { enable: Boolean(enable) });
    },

    async getLogs(source = 'daemon') {
      return client.request(`/api/logs?source=${encodeURIComponent(source)}`);
    },

    async getSystemDisk() {
      const data = await client.request('/api/disk-space');
      if (!data) return { total_kb: 5120, free_kb: 2048, cache_total_kb: 5120, cache_free_kb: 1945, output: '2.0MB free' };
      const freeMb = (data.free_kb / 1024).toFixed(1);
      const totalMb = (data.total_kb / 1024).toFixed(1);
      const usedMb = ((data.total_kb - data.free_kb) / 1024).toFixed(1);
      const cacheFreeMb = data.cache_free_kb ? (data.cache_free_kb / 1024).toFixed(1) : '0.0';
      const cacheTotalMb = data.cache_total_kb ? (data.cache_total_kb / 1024).toFixed(1) : '0.0';
      const cacheUsedMb = (data.cache_total_kb && data.cache_free_kb) ? ((data.cache_total_kb - data.cache_free_kb) / 1024).toFixed(1) : '0.0';
      const totalCombinedFreeMb = (parseFloat(freeMb) + parseFloat(cacheFreeMb)).toFixed(1);
      const totalCombinedMb = (parseFloat(totalMb) + parseFloat(cacheTotalMb)).toFixed(1);

      return {
        ...data,
        freeMb,
        totalMb,
        usedMb,
        cacheFreeMb,
        cacheTotalMb,
        cacheUsedMb,
        totalCombinedFreeMb,
        totalCombinedMb,
        output: `Data: ${freeMb}MB free (${totalMb}MB) | Cache: ${cacheFreeMb}MB free (${cacheTotalMb}MB)`
      };
    },

    async rebootHub() {
      return client.postForm('/system', { action: 'reboot' });
    },

    async cleanStockResources() {
      return client.postForm('/system', { action: 'clean_stock_resources', confirm: 'EMPTY' });
    },

    async saveSystemAction(action, extra = {}) {
      return client.postForm('/system', { action, ...extra });
    },

    async getUpdateStatus() {
      return client.request('/api/update-status');
    },

    async checkUpdate(force = false, repo = '', token = '', remoteVersion = '') {
      return client.postForm('/api/update-check-state', {
        check: force ? '1' : '0',
        repo: repo || '',
        token: token || '',
        remote_version: remoteVersion || ''
      });
    },

    async applyUpdate(token = '', repo = '') {
      return client.postForm('/api/update-apply', { token: token || '', repo: repo || '' });
    },

    /* MQTT */
    async getMqttStatus() {
      const data = await client.request('/api/mqtt-status');
      if (!data) return { connected: false, enabled: false };
      const cfg = data.config || {};
      return {
        connected: Boolean(data.connected),
        /* the saved switch, not the broker host: a host stays set while MQTT is off */
        enabled: Boolean(data.connected || cfg.enabled),
        broker: data.broker || data.host || cfg.host || '',
        port: data.port || cfg.port || 1883,
        baseTopic: data.baseTopic || cfg.baseTopic || 'harmony/hub',
        config: {
          enabled: Boolean(cfg.enabled),
          host: data.broker || data.host || cfg.host || '',
          port: data.port || cfg.port || 1883,
          username: data.username || cfg.username || '',
          baseTopic: data.baseTopic || cfg.baseTopic || 'harmony/hub',
          clientId: data.clientId || cfg.clientId || 'harmony_hub',
          discoveryPrefix: data.discoveryPrefix || cfg.discoveryPrefix || 'homeassistant',
          haDiscovery: data.haDiscovery !== undefined ? Boolean(data.haDiscovery) : (cfg.haDiscovery !== undefined ? Boolean(cfg.haDiscovery) : true),
          v2: Boolean(data.v2)
        }
      };
    },

    async saveMqtt(data) {
      return client.postForm('/mqtt', data);
    },

    /* Network */
    async getNetworkStatus() {
      const data = await client.request('/api/network-status');
      const fallbackHost = (typeof window !== 'undefined' && window.location && window.location.hostname) ? window.location.hostname : '127.0.0.1';
      if (!data) return { ip: fallbackHost, connection_type: 'Wi-Fi' };
      return {
        ip: data.ip || fallbackHost,
        connection_type: data.connection_type || ((data.mode === 'wifi') ? 'Wi-Fi' : 'Ethernet'),
        active_interface: data.active_interface || ((data.mode === 'wifi') ? 'ath0' : 'eth2'),
        wifi_connected: data.wifi_connected !== undefined ? data.wifi_connected : true,
        usb_host_mode: data.usb_host_mode || false,
        eth_present: data.eth_present || false,
        eth_onboard: data.eth_onboard || false,
        eth_carrier: data.eth_carrier || false,
        eth_interface: data.eth_interface || '',
        eth_ip: data.eth_ip || '',
        eth_mask: data.eth_mask || '',
        eth_mac: data.eth_mac || '',
        wifi_interface: data.wifi_interface || 'ath0',
        wifi_ip: data.wifi_ip || '',
        wifi_mask: data.wifi_mask || '',
        wifi_mac: data.wifi_mac || '',
        wifi_ssid: data.wifi_ssid || '',
        netmask: data.netmask || '255.255.255.0',
        gateway: data.gateway || '',
        mac: data.mac || '',
        ifconfig: data.ifconfig || '',
        config: data.config || null
      };
    },

    async saveEthernet(data) {
      return client.postForm('/ethernet', data);
    },

    async scanWifi() {
      return client.request('/api/wifi-scan');
    },

    async testWifi(data) {
      return client.postForm('/api/wifi-test', data);
    },

    async saveWifi(data) {
      // apply=reconfigure: the Hub joins the network as a test first and changes nothing when
      // that fails. Without it the values are written untested and used at the next boot.
      // The test takes up to about 15 s.
      return client.postForm('/wifi', { apply: 'reconfigure', ...data }, { timeout: 45000 });
    },

    /* Backup & Restore */
    getExportUrl(target) {
      return client.buildUrl(`/export/${target}`);
    },

    async fetchExportText(target) {
      return client.request(`/export/${target}`);
    },

    async restoreBackup(target, payload, restoreNetwork = false) {
      if (target === 'all') target = 'bundle'; /* the Hub's name for the whole backup */
      let parsed = payload;
      if (typeof payload === 'string') {
        try { parsed = JSON.parse(payload); } catch { /* ignore */ }
      }
      const reply = await client.postJson('/import', { target, payload: parsed, restore_network: !!restoreNetwork }, { timeout: 30000 });
      // a restore that turned authentication on answers the accounts and tokens to check
      if (reply && reply.to_check && typeof window !== 'undefined' && window.dispatchEvent) {
        window.dispatchEvent(new CustomEvent('harmony:auth-switch-check', { detail: reply.to_check }));
      }
      return reply;
    },

    /* Icons */
    async getIcons() {
      return client.request('/api/icons');
    },

    async getIconsArchive() {
      const url = client.buildUrl('/api/icons/archive');
      const headers = {};
      if (client.authHeader) headers['Authorization'] = client.authHeader;
      const res = await fetch(url, { headers });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      return await res.arrayBuffer();
    },

    async getCustomIcons() {
      return client.request('/api/custom-icons');
    },

    async uploadCustomIcon(type, id, image_base64, name = null) {
      const payload = { type, id, image_base64 };
      if (name) payload.name = name;
      return client.request('/api/custom-icons', {
        method: 'POST',
        body: JSON.stringify(payload)
      });
    },

    async deleteCustomIcon(id, type = 'activity') {
      return client.request(`/api/custom-icons?id=${encodeURIComponent(id)}&type=${encodeURIComponent(type)}`, {
        method: 'DELETE'
      });
    },

    /* Settings */
    async getSettings() {
      return client.request('/api/settings');
    },

    async saveSettings(settings) {
      return client.postJson('/api/settings', settings);
    },

    /* Users & Auth */
    async getAuthMe() {
      return client.request('/api/auth/me');
    },

    async getUsers() {
      return client.request('/api/users');
    },

    async saveUser(user) {
      return client.postJson('/api/users', user);
    },

    async deleteUser(username) {
      return client.postJson('/api/users/delete', { username });
    },

    async getTokens() {
      return client.request('/api/auth/tokens');
    },

    async createToken(name, role = 'admin') {
      return client.request('/api/auth/tokens', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ name, role })
      });
    },

    async deleteToken(id) {
      return client.request(`/api/auth/tokens?id=${encodeURIComponent(id)}`, {
        method: 'DELETE'
      });
    },

    async login(username, password) {
      const res = await fetch(client.buildUrl('/api/auth/token'), {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ username, password })
      });
      const data = await res.json();
      if (!res.ok || !data.ok) {
        throw new Error(data.error || t('login_failed', 'Login failed'));
      }
      client.token = data.access_token;
      client.authHeader = `Bearer ${client.token}`;
      if (typeof localStorage !== 'undefined') {
        localStorage.setItem('harmony_auth_token', client.token);
        localStorage.setItem('harmony_auth_user', data.username || username);
        localStorage.setItem('harmony_auth_role', data.role || 'admin');
      }
      client.connectWebSocket(true);
      return data;
    },

    async logout() {
      // Forget the session on this device first: a slow or failed request to the Hub must
      // never leave the user logged in here
      const token = client.token;
      client.clearStoredToken();
      if (token) {
        const bearer = token.startsWith('Bearer ') ? token : `Bearer ${token}`;
        try {
          await client.request('/api/auth/logout', { method: 'POST', headers: { Authorization: bearer }, ignoreUnauthorized: true });
        } catch {
          /* the session ends on the Hub anyway when it expires */
        }
      }
      client.connectWebSocket(true);
    },

    async getPlugins() {
      return client.request('/api/plugins');
    },

    // action: start, pin or status. A PIN pairing waits up to 15 s on the Hub; a listen pairing
    // answers at once and is polled with "status".
    async pairPlugin(id, action, host = '', pin = '') {
      return client.postJson('/api/plugins/pair', { id, action, host, pin }, { timeout: 20000 });
    },

    // The Hub fetches every repository (20 s each at most); refresh skips its 10 minute cache
    async getPluginMarketplace(forceReload = false) {
      return client.request(`/api/plugins/marketplace${forceReload ? '?refresh=1' : ''}`, { timeout: 60000 });
    },

    async getPluginRepositories() {
      return client.request('/api/plugins/repositories');
    },

    async updatePluginRepositories(action, url) {
      return client.postJson('/api/plugins/repositories', { action, url });
    },

    // An install downloads for up to 120 s on the Hub. An upload needs the archive's SHA-256,
    // or isDev for a local build.
    async installPlugin(urlOrBase64, isDev = false, sha256 = '') {
      const opts = { timeout: 130000 };
      if (typeof urlOrBase64 === 'string' && urlOrBase64.startsWith('http')) {
        return client.postJson('/api/plugins/install', { url: urlOrBase64, dev: !!isDev }, opts);
      }
      const body = { archive_base64: urlOrBase64 };
      if (sha256) body.sha256 = sha256;
      if (isDev) body.dev = true;
      return client.postJson('/api/plugins/install', body, opts);
    },

    async pluginAction(id, action) {
      return client.postJson('/api/plugins/action', { id, action });
    },

    async uninstallPlugin(id) {
      return client.postJson('/api/plugins/uninstall', { id });
    }
  };
}
