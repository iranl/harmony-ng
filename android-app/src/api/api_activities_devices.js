import { HarmonyClientCore } from './api_core.js';

export class HarmonyClientActivitiesDevices extends HarmonyClientCore {

  /* Core Inventory & Activities */
  async getInventory() {
    const data = await this.request('/api/inventory');
    if (!data) return { devices: [] };
    const rawDevs = data.devices || data.DevicesWithFeatures || data.device || (Array.isArray(data) ? data : []);
    const normalized = rawDevs.map(d => {
      const dev = d.Device || d;
      const rawCmds = dev.commands || d.commands || d.Commands || [];
      const cleanType = dev.type === 'BT' ? 'BT' : (dev.type === 'MQTT' ? 'MQTT' : (dev.type === 'IR' ? 'IR' : (dev.transport === 32 ? 'BT' : 'IR')));
      const isBt = cleanType === 'BT';
      const isMqtt = cleanType === 'MQTT';
      const hostAddr = dev.bdaddr || dev.btHost || dev.bt_host || dev.BTAddress || '';

      return {
        id: String(dev.id || dev['Id-'] || dev.Id || ''),
        name: dev.name || dev.Name || 'Device',
        type: cleanType,
        manufacturer: dev.manufacturer || dev.Manufacturer || '',
        model: dev.model || dev.Model || '',
        controlPort: dev.controlPort !== undefined ? Number(dev.controlPort) : 7,
        icon: dev.icon || dev.Icon || '',
        interDeviceDelay: dev.interDeviceDelay !== undefined ? Number(dev.interDeviceDelay) : 500,
        interKeyDelay: dev.interKeyDelay !== undefined ? Number(dev.interKeyDelay) : 100,
        holdInterKeyDelay: dev.holdInterKeyDelay !== undefined ? Number(dev.holdInterKeyDelay) : 200,
        defaultInterDeviceDelay: dev.defaultInterDeviceDelay !== undefined ? Number(dev.defaultInterDeviceDelay) : 500,
        powerOnDelay: dev.powerOnDelay !== undefined ? Number(dev.powerOnDelay) : 1500,
        isPowerAlwaysOn: Boolean(dev.isPowerAlwaysOn || dev.is_power_always_on),
        bdaddr: hostAddr,
        btHost: hostAddr,
        btAutoReconnect: dev.btAutoReconnect !== undefined ? Number(dev.btAutoReconnect) : 1,
        btReconnectDurationSec: dev.btReconnectDurationSec !== undefined ? Number(dev.btReconnectDurationSec) : (dev.btReconnectDuration !== undefined ? Number(dev.btReconnectDuration) : 0),
        btReconnectDelaySec: dev.btReconnectDelaySec !== undefined ? Number(dev.btReconnectDelaySec) : (dev.btReconnectDelay !== undefined ? Number(dev.btReconnectDelay) : -1),
        btTriggerDeviceId: dev.btTriggerDeviceId || dev.bt_trigger_device_id || '',
        btTriggerCommandName: dev.btTriggerCommandName || dev.bt_trigger_command_name || '',
        powerOnSequence: dev.powerOnSequence || dev.PowerOnActions || [],
        powerOffSequence: dev.powerOffSequence || dev.PowerOffActions || [],
        isBt: isBt,
        transport: isBt ? 32 : (dev.transport !== undefined ? Number(dev.transport) : 0),
        isKeyboardAssociated: isBt,
        mqttEnabled: isMqtt || !!(dev.mqttEnabled || dev.mqtt_enabled),
        mqttTopic: dev.mqttTopic || dev.mqtt_topic || '{base_topic}/button/{device}/{command}',
        mqttPulseMs: Number(dev.mqttPulseMs !== undefined ? dev.mqttPulseMs : 1000),
        commands: rawCmds.map(c => {
          if (typeof c === 'string') return { name: c, keyCode: '', raw: '' };
          const kCode = c.keyCode || c.KeyCode || '';
          const rawStr = c.raw || c.Raw || '';
          const isBtCmd = isBt || kCode.startsWith('HID:') || rawStr.startsWith('HID:');
          const isMqttCmd = isMqtt || kCode.startsWith('G:MQTT') || rawStr.startsWith('G:MQTT');
          const cmdObj = {
            name: c.name || c.Name || '',
            id: c.id || c['Id-'] || 0,
            keyCode: kCode,
            raw: rawStr
          };
          if (!isBtCmd && !isMqttCmd) {
            cmdObj.repeats = c.repeats !== undefined ? c.repeats : (c.Repeats || 3);
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
      const rawActs = data.Activities || data.activities || data.activity || (Array.isArray(data) ? data : []);
      const normalized = rawActs.map(a => {
        const roles = a.Roles || a.roles || [];
        const startSeq = a.startSequence || a.StartSequence || a.EnterActions || a.startSteps || [];
        const normStart = startSeq.map((s, idx) => {
          const cName = s.CommandName || s.IRCommandName || s.Command || s.command || '';
          const isSw = (s.type === 'Switched' || s.type === 'ActivitySwitched' || (s.__type && s.__type.includes('Switched')) || cName === 'Switched');
          const isDl = (s.type === 'Delay' || s.__type === 'DelayActivityAction' || (!isSw && s.type === 'Delay'));
          const rawDelay = s.delay !== undefined ? s.delay :
                           (s.delay_ms !== undefined ? s.delay_ms :
                           (s.delayMs !== undefined ? s.delayMs :
                           (s.Delay !== undefined ? s.Delay :
                           (s.Duration !== undefined ? s.Duration : 0))));
          return {
            id: s.Id || s.id || idx,
            type: isDl ? 'Delay' : (isSw ? 'Switched' : (s.type === 'BTCommand' ? 'BTCommand' : 'Command')),
            deviceId: String(s['DeviceId-'] || s.DeviceId || s.deviceId || '0'),
            command: isSw ? (cName || 'Switched') : cName,
            delay: Number(rawDelay) || 0,
            order: s.Order !== undefined ? s.Order : (s.ActionOrder !== undefined ? s.ActionOrder : idx)
          };
        });
        const stopSeq = a.stopSequence || a.StopSequence || a.LeaveActions || a.stopSteps || [];
        const normStop = stopSeq.map((s, idx) => {
          const cName = s.CommandName || s.IRCommandName || s.Command || s.command || '';
          const isSw = (s.type === 'Switched' || s.type === 'ActivitySwitched' || (s.__type && s.__type.includes('Switched')) || cName === 'Switched');
          const isDl = (s.type === 'Delay' || s.__type === 'DelayActivityAction' || (!isSw && s.type === 'Delay'));
          const rawDelay = s.delay !== undefined ? s.delay :
                           (s.delay_ms !== undefined ? s.delay_ms :
                           (s.delayMs !== undefined ? s.delayMs :
                           (s.Delay !== undefined ? s.Delay :
                           (s.Duration !== undefined ? s.Duration : 0))));
          return {
            id: s.Id || s.id || idx,
            type: isDl ? 'Delay' : (isSw ? 'Switched' : (s.type === 'BTCommand' ? 'BTCommand' : 'Command')),
            deviceId: String(s['DeviceId-'] || s.DeviceId || s.deviceId || '0'),
            command: isSw ? (cName || 'Switched') : cName,
            delay: Number(rawDelay) || 0,
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
