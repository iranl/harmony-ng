/**
 * Harmony Hub Client API
 * Direct network communication with codex_daemon HTTP & WebSocket API.
 * Supports HTTP Basic Authentication, CORS, and AJAX JSON messaging.
 */

export class HarmonyClient {
  constructor(hub) {
    this.wsPendingReqs = new Map();
    this.wsMsgSeq = 1;
    this.onWsMessage = null;
    this.onWsStatus = null;
    this.updateHub(hub);
  }

  updateHub(hub) {
    this.hub = hub || {};
    const isBrowser = (typeof window !== 'undefined' && window.location && window.location.host);
    if (isBrowser && (!window.Capacitor || !window.Capacitor.isNativePlatform())) {
      this.baseUrl = '';
      this.wsUrl = `${window.location.protocol === 'https:' ? 'wss:' : 'ws:'}//${window.location.host}/`;
    } else {
      const host = this.hub.host || '127.0.0.1';
      const port = this.hub.port || 8089;
      this.baseUrl = `http://${host}:${port}`;
      this.wsUrl = `ws://${host}:${this.hub.wsPort || port}/`;
    }
    this.authHeader = (this.hub.username && this.hub.password)
      ? `Basic ${btoa(`${this.hub.username}:${this.hub.password}`)}`
      : null;

    if (!this.ws || this.ws.readyState >= 2) {
      this.connectWebSocket(true);
    }
  }

  connectWebSocket(force = false) {
    if (!this.wsUrl) return;

    if (!force && this.ws && (this.ws.readyState === WebSocket.OPEN || this.ws.readyState === WebSocket.CONNECTING)) {
      return;
    }
    this.disconnectWebSocket();

    try {
      this.ws = new WebSocket(this.wsUrl);
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
        } catch {}
      };
      this.ws.onerror = () => {};
      this.ws.onclose = () => {
        if (this.onWsStatus) this.onWsStatus(false);
        this.scheduleWsReconnect();
      };
    } catch {
      this.scheduleWsReconnect();
    }
  }

  scheduleWsReconnect() {
    if (this.wsReconnectTimer) clearTimeout(this.wsReconnectTimer);
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
      } catch {}
      this.ws = null;
    }
  }

  getHeaders(extra = {}) {
    const headers = { ...extra };
    if (this.authHeader) {
      headers['Authorization'] = this.authHeader;
    }
    return headers;
  }

  async request(path, options = {}) {
    const url = this.baseUrl ? `${this.baseUrl}${path}` : path;
    const opts = {
      ...options,
      headers: this.getHeaders(options.headers || {})
    };

    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), options.timeout || 12000);
    opts.signal = controller.signal;

    try {
      const res = await fetch(url, opts);
      clearTimeout(timeout);
      if (res.status === 401) {
        throw new Error('Authentication failed. Check hub credentials.');
      }
      const text = await res.text();
      let j;
      try {
        j = JSON.parse(text);
      } catch {
        j = null;
      }

      if (!res.ok) {
        let errMsg = `Hub returned HTTP ${res.status}`;
        if (j && (j.error || j.message || j.reply)) {
          errMsg = j.error || j.message || j.reply;
        } else if (text && text.length < 200 && !text.includes('<!DOCTYPE')) {
          errMsg = text;
        }
        if (res.status === 409 && errMsg.startsWith('Hub returned')) {
          errMsg = 'Activity switch already in progress';
        }
        throw new Error(errMsg);
      }

      return j !== null ? j : text;
    } catch (err) {
      clearTimeout(timeout);
      if (err.name === 'AbortError') {
        throw new Error('Connection timed out. Check IP and network.');
      }
      throw err;
    }
  }

  async postForm(path, data = {}) {
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
      body: params.toString()
    });
  }

  async postJson(path, data = {}) {
    return this.request(path, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'Accept': 'application/json, text/plain, */*',
        'X-Requested-With': 'XMLHttpRequest'
      },
      body: JSON.stringify(data)
    });
  }

  async testConnection() {
    try {
      const data = await this.request('/api/inventory', { timeout: 4000 });
      return { ok: true, data };
    } catch (err) {
      return { ok: false, error: err.message };
    }
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
      const msgId = this.wsMsgSeq++;
      const timer = setTimeout(() => {
        this.wsPendingReqs.delete(msgId);
        reject(new Error('WS timeout'));
      }, 4000);
      this.wsPendingReqs.set(msgId, (res) => {
        clearTimeout(timer);
        resolve(res);
      });
      try {
        this.ws.send(JSON.stringify({ action, msgId, ...payload }));
      } catch (err) {
        clearTimeout(timer);
        this.wsPendingReqs.delete(msgId);
        reject(err);
      }
    });
  }

  /* Core Inventory & Activities */
  async getInventory() {
    const data = await this.request('/api/inventory');
    if (!data) return { devices: [] };
    const rawDevs = data.DevicesWithFeatures || data.devices || data.device || (Array.isArray(data) ? data : []);
    const normalized = rawDevs.map(d => {
      const dev = d.Device || d;
      const rawCmds = d.Commands || d.commands || [];
      const devTypeStr = String(dev.type || dev.DeviceTypeDisplayName || '').toLowerCase();
      const devMfg = String(dev.Manufacturer || dev.manufacturer || '').toLowerCase();
      const isBt = Number(dev.Transport) === 32 || Number(dev.transport) === 32 || devTypeStr === 'bt' || devTypeStr.includes('bluetooth') || devMfg.includes('bluetooth');
      const isMqtt = devTypeStr === 'mqtt' || devTypeStr.includes('mqtt') || devMfg.includes('mqtt');
      const cleanType = isBt ? 'BT' : (isMqtt ? 'MQTT' : 'IR');

      return {
          id: String(dev['Id-'] || dev.id || dev.Id || ''),
          name: dev.Name || dev.name || 'Device',
          type: cleanType,
          manufacturer: dev.Manufacturer || dev.manufacturer || '',
          model: dev.Model || dev.model || '',
          controlPort: dev.ControlPort !== undefined ? Number(dev.ControlPort) : (dev.controlPort !== undefined ? Number(dev.controlPort) : 7),
          icon: dev.Icon || dev.icon || '',
          interDeviceDelay: dev.interDeviceDelay !== undefined ? Number(dev.interDeviceDelay) : 500,
          interKeyDelay: dev.interKeyDelay !== undefined ? Number(dev.interKeyDelay) : 100,
          holdInterKeyDelay: dev.holdInterKeyDelay !== undefined ? Number(dev.holdInterKeyDelay) : 200,
          defaultInterDeviceDelay: dev.defaultInterDeviceDelay !== undefined ? Number(dev.defaultInterDeviceDelay) : 500,
          powerOnDelay: dev.powerOnDelay !== undefined ? Number(dev.powerOnDelay) : 1500,
          isPowerAlwaysOn: Boolean(dev.isPowerAlwaysOn || dev.is_power_always_on),
          is_power_always_on: Boolean(dev.isPowerAlwaysOn || dev.is_power_always_on),
          btHost: dev.BTAddress || dev.bt_host || dev.btHost || dev.bdaddr || '',
          bt_host: dev.BTAddress || dev.bt_host || dev.btHost || dev.bdaddr || '',
          BTAddress: dev.BTAddress || dev.bt_host || dev.btHost || dev.bdaddr || '',
          btAutoReconnect: dev.btAutoReconnect !== undefined ? Number(dev.btAutoReconnect) : -1,
          btReconnectDuration: dev.btReconnectDuration !== undefined ? Number(dev.btReconnectDuration) : 0,
          btReconnectDelay: dev.btReconnectDelay !== undefined ? Number(dev.btReconnectDelay) : -1,
          powerOnSequence: dev.powerOnSequence || dev.PowerOnActions || [],
          powerOffSequence: dev.powerOffSequence || dev.PowerOffActions || [],
          isBt: isBt,
          transport: dev.Transport !== undefined ? Number(dev.Transport) : (dev.transport !== undefined ? Number(dev.transport) : (isBt ? 32 : 0)),
          isKeyboardAssociated: Boolean(dev.IsKeyboardAssociated || dev.isKeyboardAssociated || isBt),
          mqttEnabled: isMqtt || !!(dev.MQTTEnabled || dev.mqtt_enabled || dev.mqttEnabled),
          mqttTopic: dev.MQTTTopic || dev.mqtt_topic || dev.mqttTopic || '{base_topic}/button/{device}/{command}',
          mqttPulseMs: Number(dev.MQTTPulseMs !== undefined ? dev.MQTTPulseMs : (dev.mqttPulseMs !== undefined ? dev.mqttPulseMs : (dev.mqtt_pulse_ms !== undefined ? dev.mqtt_pulse_ms : (dev.mqttPulse !== undefined ? dev.mqttPulse : 1000)))),
          mqttPulse: Number(dev.MQTTPulseMs !== undefined ? dev.MQTTPulseMs : (dev.mqttPulseMs !== undefined ? dev.mqttPulseMs : (dev.mqtt_pulse_ms !== undefined ? dev.mqtt_pulse_ms : (dev.mqttPulse !== undefined ? dev.mqttPulse : 1000)))),
          commands: rawCmds.map(c => {
            if (typeof c === 'string') return { name: c, keyCode: '', raw: '' };
            const kCode = c.KeyCode || c.keyCode || '';
            const rawStr = c.Raw || c.raw || '';
            const isBtCmd = isBt || kCode.startsWith('HID:') || rawStr.startsWith('HID:') || kCode.startsWith('G:HID') || rawStr.startsWith('G:HID');
            const isMqttCmd = isMqtt || kCode.startsWith('G:MQTT') || rawStr.startsWith('G:MQTT');
            const cmdObj = {
              name: c.Name || c.name || '',
              id: c['Id-'] || c.id || 0,
              keyCode: kCode,
              raw: rawStr
            };
            if (!isBtCmd && !isMqttCmd) {
              cmdObj.repeats = c.Repeats || c.repeats || 3;
              cmdObj.port = c.ControlPort || c.controlPort || 0;
            }
            return cmdObj;
          })
        };
      });
      return { devices: normalized };
    }

    async getActivities(options = {}) {
      const [data, status] = await Promise.all([
        this.request('/api/activities', options),
        this.request('/api/status', options).catch(() => null)
      ]);
      if (!data) return { activities: [], currentId: '-1', currentName: 'PowerOff' };
      const rawActs = data.Activities || data.activities || data.activity || (Array.isArray(data) ? data : []);
      const normalized = rawActs.map(a => {
        const roles = a.Roles || a.roles || [];
        const startSeq = a.startSequence || a.StartSequence || a.EnterActions || a.startSteps || [];
        const normStart = startSeq.map((s, idx) => {
          const cName = s.CommandName || s.IRCommandName || s.Command || s.command || '';
          const isSw = (s.type === 'Switched' || s.type === 'ActivitySwitched' || (s.__type && s.__type.includes('Switched')) || cName === 'Switched');
          const isDl = (s.type === 'Delay' || s.__type === 'DelayActivityAction' || (!isSw && s.type === 'Delay'));
          return {
            id: s.Id || s.id || idx,
            type: isDl ? 'Delay' : (isSw ? 'Switched' : 'Command'),
            deviceId: String(s['DeviceId-'] || s.DeviceId || s.deviceId || '0'),
            command: isSw ? (cName || 'Switched') : cName,
            delay: Number(s.Duration !== undefined ? s.Duration : (s.Delay !== undefined ? s.Delay : (s.delay || 0))),
            order: s.Order !== undefined ? s.Order : (s.ActionOrder !== undefined ? s.ActionOrder : idx)
          };
        });
        const stopSeq = a.stopSequence || a.StopSequence || a.LeaveActions || a.stopSteps || [];
        const normStop = stopSeq.map((s, idx) => {
          const cName = s.CommandName || s.IRCommandName || s.Command || s.command || '';
          const isSw = (s.type === 'Switched' || s.type === 'ActivitySwitched' || (s.__type && s.__type.includes('Switched')) || cName === 'Switched');
          const isDl = (s.type === 'Delay' || s.__type === 'DelayActivityAction' || (!isSw && s.type === 'Delay'));
          return {
            id: s.Id || s.id || idx,
            type: isDl ? 'Delay' : (isSw ? 'Switched' : 'Command'),
            deviceId: String(s['DeviceId-'] || s.DeviceId || s.deviceId || '0'),
            command: isSw ? (cName || 'Switched') : cName,
            delay: Number(s.Duration !== undefined ? s.Duration : (s.Delay !== undefined ? s.Delay : (s.delay || 0))),
            order: s.Order !== undefined ? s.Order : (s.ActionOrder !== undefined ? s.ActionOrder : idx)
          };
        });

      let devIds = [];
      if (Array.isArray(a.deviceIds) && a.deviceIds.length > 0) {
        devIds = a.deviceIds.map(String).filter(Boolean);
      } else if (typeof a.deviceIds === 'string' && a.deviceIds.trim().length > 0) {
        devIds = a.deviceIds.split(',').map(s => s.trim()).filter(Boolean);
      } else if (Array.isArray(roles) && roles.length > 0) {
        const sortedRoles = [...roles].sort((r1, r2) => {
          const o1 = Number(r1.PowerOnOrder !== undefined ? r1.PowerOnOrder : (r1.order !== undefined ? r1.order : 999));
          const o2 = Number(r2.PowerOnOrder !== undefined ? r2.PowerOnOrder : (r2.order !== undefined ? r2.order : 999));
          return o1 - o2;
        });
        devIds = sortedRoles.map(r => String(r['DeviceId-'] || r.DeviceId || r.deviceId || '')).filter(Boolean);
      } else {
        const seqDevIds = [...normStart, ...normStop].map(s => String(s.deviceId || '')).filter(d => d && d !== '0');
        devIds = Array.from(new Set(seqDevIds));
      }

      return {
        ...a,
        id: String(a['Id-'] || a.id || a.Id || ''),
        name: a.Name || a.name || 'Activity',
        type: a.SuggestedDisplay || a.type || '',
        order: Number(a.ActivityOrder !== undefined ? a.ActivityOrder : (a.order || 0)),
        isDefault: Boolean(a.IsDefault || a.isDefault),
        deviceIds: devIds,
        participatingDeviceIds: devIds,
        roles: roles,
        Roles: roles,
        startSequence: normStart,
        stopSequence: normStop
      };
    });
    const curId = String((status && (status.activity || status.currentId)) || data.currentId || data.activityId || '-1');
    const curAct = normalized.find(a => String(a.id) === curId);
    return {
      activities: normalized,
      currentId: curId,
      currentName: curAct ? curAct.name : (curId === '-1' ? 'PowerOff' : curId)
    };
  }

  async startActivity(activityId) {
    if (this.isWsConnected()) {
      try {
        const j = await this.sendWsCommand('start_activity', { id: String(activityId) });
        if (j.error && !j.ok) {
          throw new Error(j.error);
        }
        return j;
      } catch (err) {
        if (err.message && (err.message.includes('busy') || err.message.includes('Interlock'))) {
          throw err;
        }
      }
    }
    return this.postJson('/api/activity-start', { id: String(activityId) });
  }

  async stopActivity() {
    if (this.isWsConnected()) {
      try {
        const j = await this.sendWsCommand('stop_activity', {});
        if (j.error && !j.ok) {
          throw new Error(j.error);
        }
        return j;
      } catch (err) {
        if (err.message && (err.message.includes('busy') || err.message.includes('Interlock'))) {
          throw err;
        }
      }
    }
    return this.postJson('/api/activity-stop', {});
  }

  async cancelActivity() {
    if (this.isWsConnected()) {
      try {
        const j = await this.sendWsCommand('cancel_activity', {});
        if (j.ok) return j;
      } catch (_) {}
    }
    return this.postJson('/api/activity-cancel', {});
  }

  async saveActivity(data) {
    return this.postJson('/api/activity-save', data);
  }

  async deleteActivity(id) {
    return this.postJson('/api/activity-delete', { id: String(id) });
  }

  async testActivitySequence(steps) {
    return this.postJson('/api/activity-test-sequence', { steps: String(steps) });
  }

  /* Device Control & Commands */
  async getDeviceCommands(deviceId) {
    if (!deviceId) return [];
    try {
      const res = await this.request(`/api/device-commands?deviceId=${encodeURIComponent(deviceId)}`);
      return Array.isArray(res) ? res : (res.commands || []);
    } catch (err) {
      console.warn(`Failed to fetch commands for device ${deviceId}:`, err);
      return [];
    }
  }

  async sendCommand(deviceId, command) {
    if (this.isWsConnected()) {
      try {
        await this.sendWsCommand('send_command', { deviceId: String(deviceId), command: String(command) });
        return { ok: true, ws: true };
      } catch {}
    }
    return this.postJson('/api/ir-send', {
      deviceId: String(deviceId),
      command: String(command)
    });
  }

  async sendBtCommand(deviceId, command) {
    return this.postJson('/api/bt-saved-command', {
      deviceId: String(deviceId),
      command: String(command)
    });
  }

  async sendBtKey(code, action = 'tap', target = '') {
    if (!code) return { ok: false };
    if (this.sendWs({ action: 'bt_key', key: String(code), keyAction: String(action), target: String(target) })) {
      return { ok: true, ws: true };
    }
    return this.postJson('/api/bt-key', {
      key: String(code),
      code: String(code),
      action: String(action),
      target: String(target)
    });
  }

  async sendBtText(text, target = '') {
    if (this.sendWs({ action: 'bt_text', text: String(text), target: String(target) })) {
      return { ok: true, ws: true };
    }
    return this.postJson('/api/bt-text', { text: String(text), target: String(target) });
  }

  async runBtScript(script, delay = 35) {
    return this.postJson('/api/bt-script', { script: String(script), delay: Number(delay) || 35 });
  }

  async saveBtCommand(deviceId, name, script) {
    return this.postForm('/bt/command', { deviceId, name, script });
  }

  async deleteBtCommand(deviceId, command) {
    return this.postForm('/bt/delete-command', { deviceId, command });
  }

  async getBtStatus() {
    return this.request('/api/bt-status');
  }

  async setBtPairing(enable, name = '', targetDev = '') {
    return this.postJson('/api/bt-pairing', {
      enable: enable ? 1 : 0,
      name: name || 'Harmony Keyboard',
      targetDev: targetDev || ''
    });
  }

  async connectBt(bdaddr) {
    return this.postJson('/api/bt-connect', { bdaddr });
  }

  async disconnectBt(bdaddr = '') {
    return this.postJson('/api/bt-disconnect', { bdaddr: bdaddr || '' });
  }

  async linkBtDevice(bdaddr, deviceId) {
    return this.postJson('/api/bt-link-device', { bdaddr, deviceId });
  }

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

  /* Harmony Elite RF Remote & Mapping */
  async getEliteMapping() {
    return this.request('/api/elite-mapping');
  }

  async saveEliteMapping(data) {
    return this.postJson('/api/elite-mapping-save', data);
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
        btRemoteEvents: Boolean(data.btRemoteEvents || cfg.btRemoteEvents)
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

  async restoreBackup(target, payload) {
    let parsed = payload;
    if (typeof payload === 'string') {
      try { parsed = JSON.parse(payload); } catch {}
    }
    return this.postJson('/import', { target, payload: parsed });
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

  async checkUpdate(force = false) {
    return this.postForm('/api/update-check-state', { check: force ? '1' : '0' });
  }

  async applyUpdate(token = '', repo = '') {
    return this.postForm('/api/update-apply', { token: token || '', repo: repo || '' });
  }

  async getIcons() {
    return this.request('/api/icons');
  }

  async getBtStatus() {
    return this.request('/api/bt-status');
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
}
