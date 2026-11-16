import { HarmonyClientCore } from './api_core.js';
import { t, tf } from '../i18n.js';

const sleep = ms => new Promise(r => setTimeout(r, ms));

// One step per line: TEXT <words>, KEY <name>, WAIT <ms> (0..10000). Blank lines and lines
// starting with # are skipped; anything else is an error naming the line.
export function parseBtScript(script) {
  const steps = [];
  String(script || '').split(/\r?\n/).forEach((raw, idx) => {
    const line = raw.trim();
    if (!line || line.startsWith('#')) return;
    const m = line.match(/^(TEXT|KEY|WAIT)(?:\s+(.*))?$/i);
    const op = m ? m[1].toUpperCase() : '';
    const arg = m && m[2] ? m[2].trim() : '';
    if (!m || !arg) throw new Error(tf('bt_script_line_bad', { line: idx + 1 }));
    if (op === 'WAIT') {
      const ms = Number(arg);
      if (!Number.isInteger(ms) || ms < 0 || ms > 10000) throw new Error(tf('bt_script_wait_range', { line: idx + 1 }));
      steps.push({ op, ms });
    } else {
      steps.push({ op, arg: op === 'TEXT' ? m[2] : arg });
    }
  });
  if (!steps.length) throw new Error(t('bt_script_no_steps'));
  return steps;
}

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

  /* action: 'tap' (default, a full keystroke), or 'press' / 'hold' / 'release' from a button
     that is held down. A 'press' must be followed by a 'release': a Bluetooth device keeps
     the key down until then. */
  async sendCommand(deviceId, command, action = 'tap') {
    if (this.isWsConnected()) {
      try {
        const res = await this.sendWsCommand('send_command', {
          deviceId: String(deviceId),
          command: String(command),
          cmdAction: String(action)
        });
        if (res && res.ok === false) {
          throw new Error(res.error || 'WS send_command failed');
        }
        return { ok: true, ws: true };
      } catch (err) {
        /* If WS socket is still connected, don't duplicate request over HTTP unless WS rejected */
        if (this.isWsConnected() && !err?.message?.includes('failed') && !err?.message?.includes('Unknown action')) {
          throw err;
        }
      }
    }
    return this.postJson('/api/ir-send', {
      deviceId: String(deviceId),
      command: String(command),
      action: String(action)
    });
  }

  async sendDeviceCommand(deviceId, command) {
    return this.sendCommand(deviceId, command);
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

  // Macro script (TEXT words, KEY name, WAIT ms, one per line), run from here: the Hub has no
  // script route, so each step goes out as a Bluetooth key or text like the keyboard page sends.
  // The whole script is checked before the first step is sent.
  async runBtScript(script, delay = 35, target = '') {
    const steps = parseBtScript(script);
    const gap = Math.min(Math.max(Number(delay) || 0, 0), 10000);
    for (let i = 0; i < steps.length; i++) {
      const s = steps[i];
      if (s.op === 'WAIT') await sleep(s.ms);
      else if (s.op === 'KEY') await this.sendBtKey(s.arg, 'tap', target);
      else await this.sendBtText(s.arg, target);
      if (i < steps.length - 1 && gap) await sleep(gap);
    }
    return { ok: true, steps: steps.length };
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

  // Remove a paired host: its link key, its link and the devices' links to it (admin)
  async forgetBtHost(addr) {
    return this.postJson('/api/bt-forget-host', { addr: String(addr || '') });
  }

  async linkBtDevice(bdaddr, deviceId) {
    return this.postJson('/api/bt-link-device', { bdaddr, deviceId });
  }

  async reconnectBt(target = '', duration = 30, delay = 0) {
    return this.postJson('/api/bt/reconnect', { target, duration, delay });
  }
}
