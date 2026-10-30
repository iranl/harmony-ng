import { HarmonyClientCore } from './api_core.js';

export class HarmonyClientActivitiesDevices extends HarmonyClientCore {

  /* Core Inventory & Activities */
  async getInventory() {
    const data = await this.request('/api/inventory');
    if (!data) return { devices: [] };
    const rawDevs = data.devices || (Array.isArray(data) ? data : []);
    const normalized = rawDevs.map(dev => {
      const rawCmds = dev.commands || [];
      const cleanType = dev.type === 'BT' ? 'BT' : (dev.type === 'MQTT' ? 'MQTT' : 'IR');
      const isBt = cleanType === 'BT';
      const isMqtt = cleanType === 'MQTT';
      const hostAddr = dev.bdaddr || dev.btHost || '';

      return {
        id: String(dev.id || ''),
        name: dev.name || 'Device',
        type: cleanType,
        manufacturer: dev.manufacturer || '',
        model: dev.model || '',
        controlPort: dev.controlPort !== undefined ? Number(dev.controlPort) : 7,
        icon: dev.icon || '',
        interDeviceDelay: dev.interDeviceDelay !== undefined ? Number(dev.interDeviceDelay) : 500,
        interKeyDelay: dev.interKeyDelay !== undefined ? Number(dev.interKeyDelay) : 100,
        holdInterKeyDelay: dev.holdInterKeyDelay !== undefined ? Number(dev.holdInterKeyDelay) : 200,
        defaultInterDeviceDelay: dev.defaultInterDeviceDelay !== undefined ? Number(dev.defaultInterDeviceDelay) : 500,
        powerOnDelay: dev.powerOnDelay !== undefined ? Number(dev.powerOnDelay) : 1500,
        isPowerAlwaysOn: Boolean(dev.isPowerAlwaysOn),
        bdaddr: hostAddr,
        btHost: hostAddr,
        btAutoReconnect: dev.btAutoReconnect !== undefined ? Number(dev.btAutoReconnect) : 1,
        btReconnectDurationSec: dev.btReconnectDurationSec !== undefined ? Number(dev.btReconnectDurationSec) : 0,
        btReconnectDelaySec: dev.btReconnectDelaySec !== undefined ? Number(dev.btReconnectDelaySec) : -1,
        btTriggerDeviceId: dev.btTriggerDeviceId || '',
        btTriggerCommandName: dev.btTriggerCommandName || '',
        powerOnSequence: dev.powerOnSequence || [],
        powerOffSequence: dev.powerOffSequence || [],
        isBt: isBt,
        transport: isBt ? 32 : (dev.transport !== undefined ? Number(dev.transport) : 0),
        isKeyboardAssociated: isBt,
        mqttEnabled: isMqtt || !!dev.mqttEnabled,
        mqttTopic: dev.mqttTopic || '{base_topic}/button/{device}/{command}',
        mqttPulseMs: Number(dev.mqttPulseMs !== undefined ? dev.mqttPulseMs : 1000),
        commands: rawCmds.map((c, idx) => {
          if (typeof c === 'string') return { name: c, keyCode: '', raw: '' };
          const kCode = c.keyCode || '';
          const rawStr = c.raw || '';
          const isBtCmd = isBt || kCode.startsWith('HID:') || rawStr.startsWith('HID:');
          const isMqttCmd = isMqtt || kCode.startsWith('G:MQTT') || rawStr.startsWith('G:MQTT');
          const cmdObj = {
            name: c.name || '',
            id: c.id !== undefined ? c.id : idx,
            keyCode: kCode,
            raw: rawStr
          };
          if (!isBtCmd && !isMqttCmd) {
            cmdObj.repeats = c.repeats !== undefined ? c.repeats : 3;
            cmdObj.port = c.port !== undefined ? c.port : (c.controlPort || dev.controlPort || 7);
          }
          if (c.isMacro) cmdObj.isMacro = true;
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
    const rawActs = data.activities || (Array.isArray(data) ? data : []);
    const normalized = rawActs.map(a => {
      const normStart = (a.startSequence || []).map((s, idx) => ({
        id: s.id !== undefined ? s.id : idx,
        type: s.type || 'Command',
        deviceId: String(s.deviceId || '0'),
        command: s.command || '',
        delay: Number(s.delay !== undefined ? s.delay : 0),
        order: s.order !== undefined ? s.order : idx
      }));
      const normStop = (a.stopSequence || []).map((s, idx) => ({
        id: s.id !== undefined ? s.id : idx,
        type: s.type || 'Command',
        deviceId: String(s.deviceId || '0'),
        command: s.command || '',
        delay: Number(s.delay !== undefined ? s.delay : 0),
        order: s.order !== undefined ? s.order : idx
      }));
      const devIds = Array.isArray(a.deviceIds) ? a.deviceIds.map(String).filter(Boolean) : [];

      return {
        ...a,
        id: String(a.id || ''),
        name: a.name || 'Activity',
        type: a.type || '',
        icon: a.icon || '',
        order: Number(a.order || 0),
        isDefault: Boolean(a.isDefault),
        deviceIds: devIds,
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
      } catch (err) {
        /* WS command failed, fall back to HTTP */
      }
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

  async sendBtMouse({ dx = 0, dy = 0, buttons = 0, wheel = 0, action = '', button = '', target = '' } = {}) {
    const payload = {
      action: 'bt_mouse',
      dx: Number(dx) || 0,
      dy: Number(dy) || 0,
      buttons: Number(buttons) || 0,
      wheel: Number(wheel) || 0,
      mouseAction: String(action || ''),
      button: String(button || ''),
      target: String(target || '')
    };
    if (this.sendWs(payload)) {
      return { ok: true, ws: true };
    }
    return this.postJson('/api/bt-mouse', payload);
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

}
