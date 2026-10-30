import { HarmonyClientActivitiesDevices } from './api_activities_devices.js';

export class HarmonyClientSystemRemotes extends HarmonyClientActivitiesDevices {
  /* Homatics B25 / Physical BT Remote Mapping */
  async getRemoteMappings() {
    return this.request('/api/remote-mapping');
  }

  async saveRemoteMappings(data) {
    return this.postJson('/api/remote-mapping-save', data);
  }

  async getUiRemoteLayout() {
    return this.request('/api/ui-remote-layout');
  }

  async saveUiRemoteLayout(layouts) {
    return this.postJson('/api/ui-remote-layout', layouts);
  }

  /* Harmony Elite / Companion RF Remote & Mapping */
  async getRFMapping(slot = 1) {
    return this.request(`/api/rf-mapping?slot=${slot}`);
  }

  async saveRFMapping(data, slot = 1) {
    return this.postJson(`/api/rf-mapping-save?slot=${slot}`, data);
  }

  async copyRFMapping(fromSlot, toSlot) {
    return this.request(`/api/rf-mapping-copy?from=${fromSlot}&to=${toSlot}`, { method: 'POST' });
  }

  async getHealth() {
    return this.request('/health');
  }

  async getRfStatus() {
    return this.request('/api/rf/status');
  }

  async pushRfConfig() {
    return this.request('/api/rf/push-config', { method: 'POST' });
  }

  async pullRfConfig(slot = 1) {
    return this.request(`/api/rf/pull-config?slot=${slot}`, { method: 'POST' });
  }

  async getRfOtaStatus() {
    return this.request('/api/rf/ota-status');
  }

  async startRfOta(slot = 1) {
    return this.request(`/api/rf/ota-start?slot=${slot}`, { method: 'POST' });
  }

  async pairRfRemote(slot = 0) {
    const url = slot > 0 ? `/api/rf/pair?slot=${slot}` : '/api/rf/pair';
    return this.request(url, { method: 'POST' });
  }

  async stopPairRfRemote() {
    return this.request('/api/rf/stop-pair', { method: 'POST' });
  }

  async unpairRfRemote(slot = 0) {
    const url = slot > 0 ? `/api/rf/unpair?slot=${slot}` : '/api/rf/unpair';
    return this.request(url, { method: 'POST' });
  }

  async scanBtRemote() {
    return this.request('/api/remote-scan', { method: 'POST' });
  }

  async pairBtRemote(bdaddr, transport = 32) {
    return this.postForm('/api/remote-pair', { bdaddr: bdaddr || '', transport: String(transport) });
  }

  async getBtRemotePairStatus() {
    return this.request('/api/remote-pair-status');
  }

  /* IR Setup & Learning */
  async captureIr() {
    return this.request('/api/capture', { method: 'POST' });
  }

  async testLearnedIr(data) {
    return this.postJson('/api/ir-test-learned', data);
  }

  async saveLearnedCommand(data) {
    return this.postForm('/ir/command', data);
  }

  async saveNewDevice(data) {
    return this.postForm('/ir/new-device', data);
  }

  async saveDeviceDetails(data) {
    return this.postForm('/ir/device', data);
  }

  async deleteDevice(deviceId) {
    return this.postForm(`/ir/delete-device?id=${encodeURIComponent(deviceId)}`, { deviceId, id: deviceId });
  }

  async saveDevicePower(data) {
    return this.postForm('/ir/device-power', data);
  }

  async saveDeviceMqtt(data) {
    return this.postForm('/ir/device-mqtt', data);
  }

  async testDeviceMqtt(params = {}) {
    const data = typeof params === 'object' ? params : { deviceId: String(params) };
    return this.postJson('/api/device-mqtt-test', data);
  }

  async deleteCommand(deviceId, command) {
    return this.postForm('/ir/delete-command', { deviceId, command });
  }

  async importIrdb(data) {
    return this.postForm('/ir/irdb-import', data);
  }

  async saveBatchCommands(deviceId, commands) {
    return this.postJson('/api/commands-batch', { deviceId: String(deviceId), commands });
  }

  /* Bulk IR Lab */
  async sendBatchIr(data) {
    return this.postJson('/api/ir-batch-send', data);
  }

  async cancelBatchIr(runId) {
    return this.postJson('/api/ir-cancel', { runId });
  }

  async clearLabTarget(deviceId) {
    return this.postJson('/api/ir-lab-clear', { deviceId });
  }

  /* MQTT */
  async getMqttStatus() {
    const data = await this.request('/api/mqtt-status');
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
  }

  async saveMqtt(data) {
    return this.postForm('/mqtt', data);
  }

  /* Network (Wi-Fi & Ethernet) */
  async getNetworkStatus() {
    const data = await this.request('/api/network-status');
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
  }

  async saveEthernet(data) {
    return this.postForm('/ethernet', data);
  }

  async scanWifi() {
    return this.request('/api/wifi-scan');
  }

  async testWifi(data) {
    return this.postForm('/api/wifi-test', data);
  }

  async saveWifi(data) {
    return this.postForm('/wifi', data);
  }

  /* Backup & Restore */
  getExportUrl(target) {
    return `/export/${target}`;
  }

  async fetchExportText(target) {
    return this.request(`/export/${target}`);
  }

  async restoreBackup(target, payload, restoreNetwork = false) {
    let parsed = payload;
    if (typeof payload === 'string') {
      try { parsed = JSON.parse(payload); } catch { }
    }
    return this.postJson('/import', { target, payload: parsed, restore_network: !!restoreNetwork }, { timeout: 30000 });
  }

  /* System & Maintenance */
  async getSystemDisk() {
    const data = await this.request('/api/disk-space');
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
  }

  async getWebUiHtml() {
    return this.request('/');
  }

  async saveSystemAction(action, extra = {}) {
    return this.postForm('/system', { action, ...extra });
  }

  async getAuthMe() {
    return this.request('/api/auth/me');
  }

  async getUsers() {
    return this.request('/api/users');
  }

  async saveUser(user) {
    return this.postJson('/api/users', user);
  }

  async deleteUser(username) {
    return this.postJson('/api/users/delete', { username });
  }

  async getServices() {
    return this.request('/api/services');
  }

  async toggleRfService(enable) {
    return this.postJson('/api/services/rf-toggle', { enable: Boolean(enable) });
  }

  async toggleBtService(enable) {
    return this.postJson('/api/services/bt-toggle', { enable: Boolean(enable) });
  }

  async getLogs(source = 'daemon') {
    return this.request(`/api/logs?source=${encodeURIComponent(source)}`);
  }

  async getSystemInfo() {
    return this.request('/api/system-info');
  }

  async getBtStatus() {
    return this.request('/api/bt-status');
  }

  async rebootHub() {
    return this.saveSystemAction('reboot');
  }

  async getUpdateStatus() {
    return this.request('/api/update-status');
  }

  async checkUpdate(force = false, repo = '', token = '', remoteVersion = '') {
    return this.postForm('/api/update-check-state', {
      check: force ? '1' : '0',
      repo: repo || '',
      token: token || '',
      remote_version: remoteVersion || ''
    });
  }

  async applyUpdate(token = '', repo = '') {
    return this.postForm('/api/update-apply', { token: token || '', repo: repo || '' });
  }

  async getIcons() {
    return this.request('/api/icons');
  }

  async getCustomIcons() {
    return this.request('/api/custom-icons');
  }

  async uploadCustomIcon(type, id, image_base64, name = null) {
    const payload = { type, id, image_base64 };
    if (name) payload.name = name;
    return this.request('/api/custom-icons', {
      method: 'POST',
      body: JSON.stringify(payload)
    });
  }

  async deleteCustomIcon(id, type = 'activity') {
    return this.request(`/api/custom-icons?id=${encodeURIComponent(id)}&type=${encodeURIComponent(type)}`, {
      method: 'DELETE'
    });
  }

  async getIconsArchive() {
    const url = this.buildUrl('/api/icons/archive');
    const headers = {};
    if (this.authHeader) headers['Authorization'] = this.authHeader;
    const res = await fetch(url, { headers });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    return await res.arrayBuffer();
  }

  async getStatus() {
    return this.request('/api/status');
  }

  async getSettings() {
    return this.request('/api/settings');
  }

  async saveSettings(settings) {
    return this.postJson('/api/settings', settings);
  }

  async sendDirectPronto(prontoHex, port = 7, repeats = 1) {
    return this.postJson('/api/ir-send', { pronto: String(prontoHex), port: Number(port) || 7, repeats: Number(repeats) || 1 });
  }

  async login(username, password) {
    const res = await fetch(this.baseUrl ? `${this.baseUrl}/api/auth/token` : '/api/auth/token', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ username, password })
    });
    const data = await res.json();
    if (!res.ok || !data.ok) {
      throw new Error(data.error || 'Login failed');
    }
    this.token = data.access_token;
    this.authHeader = `Bearer ${this.token}`;
    if (typeof localStorage !== 'undefined') {
      localStorage.setItem('harmony_auth_token', this.token);
      localStorage.setItem('harmony_auth_user', data.username || username);
      localStorage.setItem('harmony_auth_role', data.role || 'admin');
    }
    this.connectWebSocket(true);
    return data;
  }

  async logout() {
    try {
      if (this.token) {
        await this.request('/api/auth/logout', { method: 'POST' });
      }
    } catch (e) {
      /* ignore */
    }
    this.token = null;
    this.authHeader = null;
    if (typeof localStorage !== 'undefined') {
      localStorage.removeItem('harmony_auth_token');
      localStorage.removeItem('harmony_auth_user');
      localStorage.removeItem('harmony_auth_role');
    }
    this.connectWebSocket(true);
  }

  async getTokens() {
    return this.request('/api/auth/tokens');
  }

  async createToken(name, role = 'admin') {
    return this.request('/api/auth/tokens', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name, role })
    });
  }

  async deleteToken(id) {
    return this.request(`/api/auth/tokens?id=${encodeURIComponent(id)}`, {
      method: 'DELETE'
    });
  }
}
