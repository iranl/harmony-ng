import { HarmonyClientCore } from './api_core.js';

export class HarmonyClientActivitiesDevices extends HarmonyClientCore {

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
          btAutoReconnect: dev.bt_auto_reconnect !== undefined ? Number(dev.bt_auto_reconnect) : (dev.btAutoReconnect !== undefined ? Number(dev.btAutoReconnect) : 1),
          bt_auto_reconnect: dev.bt_auto_reconnect !== undefined ? Number(dev.bt_auto_reconnect) : (dev.btAutoReconnect !== undefined ? Number(dev.btAutoReconnect) : 1),
          btReconnectDuration: dev.bt_reconnect_duration_sec !== undefined ? Number(dev.bt_reconnect_duration_sec) : (dev.btReconnectDuration !== undefined ? Number(dev.btReconnectDuration) : (dev.btReconnectDurationSec !== undefined ? Number(dev.btReconnectDurationSec) : 0)),
          bt_reconnect_duration_sec: dev.bt_reconnect_duration_sec !== undefined ? Number(dev.bt_reconnect_duration_sec) : (dev.btReconnectDuration !== undefined ? Number(dev.btReconnectDuration) : (dev.btReconnectDurationSec !== undefined ? Number(dev.btReconnectDurationSec) : 0)),
          btReconnectDelay: dev.bt_reconnect_delay_sec !== undefined ? Number(dev.bt_reconnect_delay_sec) : (dev.btReconnectDelay !== undefined ? Number(dev.btReconnectDelay) : (dev.btReconnectDelaySec !== undefined ? Number(dev.btReconnectDelaySec) : -1)),
          bt_reconnect_delay_sec: dev.bt_reconnect_delay_sec !== undefined ? Number(dev.bt_reconnect_delay_sec) : (dev.btReconnectDelay !== undefined ? Number(dev.btReconnectDelay) : (dev.btReconnectDelaySec !== undefined ? Number(dev.btReconnectDelaySec) : -1)),
          bt_trigger_device_id: dev.bt_trigger_device_id || dev.btTriggerDeviceId || '',
          bt_trigger_command_name: dev.bt_trigger_command_name || dev.btTriggerCommandName || dev.bt_trigger_command || dev.btTriggerCommand || '',
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
