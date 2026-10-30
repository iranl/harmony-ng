// System, Network, MQTT, Backup, Icons & Auth API methods for Harmony Hub

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
        enabled: Boolean(data.connected || data.broker || data.enabled || cfg.enabled),
        broker: data.broker || data.host || cfg.host || '',
        port: data.port || cfg.port || 1883,
        baseTopic: data.baseTopic || cfg.baseTopic || 'harmony/hub',
        config: {
          enabled: Boolean(data.connected || data.broker || data.enabled || cfg.enabled),
          host: data.broker || data.host || cfg.host || '',
          port: data.port || cfg.port || 1883,
          username: data.username || cfg.username || '',
          baseTopic: data.baseTopic || cfg.baseTopic || 'harmony/hub',
          discoveryPrefix: data.discoveryPrefix || cfg.discoveryPrefix || 'homeassistant',
          clientId: data.clientId || cfg.clientId || 'harmony_hub',
          name: data.name || cfg.name || 'Harmony Hub',
          keepAlive: data.keepAlive || cfg.keepAlive || 60,
          haDiscovery: data.haDiscovery !== undefined ? Boolean(data.haDiscovery) : (cfg.haDiscovery !== undefined ? Boolean(cfg.haDiscovery) : true),
          btRemoteEvents: Boolean(data.btRemoteEvents !== undefined ? data.btRemoteEvents : (cfg.btRemoteEvents !== undefined ? cfg.btRemoteEvents : false)),
          btRemoteTopic: data.btRemoteTopic || cfg.btRemoteTopic || '',
          slot1RemoteEvents: Boolean(data.slot1RemoteEvents !== undefined ? data.slot1RemoteEvents : (cfg.slot1RemoteEvents !== undefined ? cfg.slot1RemoteEvents : (data.eliteRemoteEvents !== undefined ? data.eliteRemoteEvents : true))),
          slot1RemoteTopic: data.slot1RemoteTopic || cfg.slot1RemoteTopic || data.eliteRemoteTopic || cfg.eliteRemoteTopic || '',
          slot2RemoteEvents: Boolean(data.slot2RemoteEvents !== undefined ? data.slot2RemoteEvents : (cfg.slot2RemoteEvents !== undefined ? cfg.slot2RemoteEvents : false)),
          slot2RemoteTopic: data.slot2RemoteTopic || cfg.slot2RemoteTopic || '',
          eliteRemoteEvents: Boolean(data.eliteRemoteEvents !== undefined ? data.eliteRemoteEvents : (cfg.eliteRemoteEvents !== undefined ? cfg.eliteRemoteEvents : true)),
          eliteRemoteTopic: data.eliteRemoteTopic || cfg.eliteRemoteTopic || ''
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
      return client.postForm('/wifi', data);
    },

    /* Backup & Restore */
    getExportUrl(target) {
      return client.buildUrl(`/export/${target}`);
    },

    async fetchExportText(target) {
      return client.request(`/export/${target}`);
    },

    async restoreBackup(target, payload, restoreNetwork = false) {
      let parsed = payload;
      if (typeof payload === 'string') {
        try { parsed = JSON.parse(payload); } catch { /* ignore */ }
      }
      return client.postJson('/import', { target, payload: parsed, restore_network: !!restoreNetwork }, { timeout: 30000 });
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
        throw new Error(data.error || 'Login failed');
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
      try {
        if (client.token) {
          await client.request('/api/auth/logout', { method: 'POST' });
        }
      } catch {
        /* ignore */
      }
      client.token = null;
      client.authHeader = null;
      if (typeof localStorage !== 'undefined') {
        localStorage.removeItem('harmony_auth_token');
        localStorage.removeItem('harmony_auth_user');
        localStorage.removeItem('harmony_auth_role');
      }
      client.connectWebSocket(true);
    }
  };
}
