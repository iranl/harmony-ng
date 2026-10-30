// Devices and IR API methods for Harmony Hub

export function createDevicesApi(client) {
  return {
    async getInventory() {
      const data = await client.request('/api/inventory');
      if (!data) return { devices: [] };
      const rawDevs = data.devices || (Array.isArray(data) ? data : []);
      const normalized = rawDevs.map((dev) => {
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
          isBt,
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
    },

    async getDeviceCommands(deviceId) {
      if (!deviceId) return [];
      try {
        const res = await client.request(`/api/device-commands?deviceId=${encodeURIComponent(deviceId)}`);
        return Array.isArray(res) ? res : (res.commands || []);
      } catch (err) {
        console.warn(`Failed to fetch commands for device ${deviceId}:`, err);
        return [];
      }
    },

    async sendCommand(deviceId, command) {
      if (client.isWsConnected()) {
        try {
          await client.sendWsCommand('send_command', { deviceId: String(deviceId), command: String(command) });
          return { ok: true, ws: true };
        } catch {
          // Fall back to HTTP
        }
      }
      return client.postJson('/api/ir-send', {
        deviceId: String(deviceId),
        command: String(command)
      });
    },

    async sendBtCommand(deviceId, command) {
      return client.postJson('/api/bt-saved-command', {
        deviceId: String(deviceId),
        command: String(command)
      });
    },

    async captureIr() {
      return client.request('/api/capture', { method: 'POST' });
    },

    async testLearnedIr(data) {
      return client.postJson('/api/ir-test-learned', data);
    },

    async saveLearnedCommand(data) {
      return client.postForm('/ir/command', data);
    },

    async saveNewDevice(data) {
      return client.postForm('/ir/new-device', data);
    },

    async saveDeviceDetails(data) {
      return client.postForm('/ir/device', data);
    },

    async deleteDevice(deviceId) {
      return client.postForm(`/ir/delete-device?id=${encodeURIComponent(deviceId)}`, { deviceId, id: deviceId });
    },

    async saveDevicePower(data) {
      return client.postForm('/ir/device-power', data);
    },

    async saveDeviceMqtt(data) {
      return client.postForm('/ir/device-mqtt', data);
    },

    async testDeviceMqtt(params = {}) {
      const data = typeof params === 'object' ? params : { deviceId: String(params) };
      return client.postJson('/api/device-mqtt-test', data);
    },

    async deleteCommand(deviceId, command) {
      return client.postForm('/ir/delete-command', { deviceId, command });
    },

    async importIrdb(data) {
      return client.postForm('/ir/irdb-import', data);
    },

    async saveBatchCommands(deviceId, commands) {
      return client.postJson('/api/commands-batch', { deviceId: String(deviceId), commands });
    },

    async sendDirectPronto(prontoHex, port = 7, repeats = 1) {
      return client.postJson('/api/ir-send', {
        pronto: String(prontoHex),
        port: Number(port) || 7,
        repeats: Number(repeats) || 1
      });
    },

    // Alias for backwards compatibility
    async blastPronto(prontoHex, port = 7, repeats = 1) {
      return this.sendDirectPronto(prontoHex, port, repeats);
    },

    async sendBatchIr(data) {
      return client.postJson('/api/ir-batch-send', data);
    },

    async cancelBatchIr(runId) {
      return client.postJson('/api/ir-cancel', { runId });
    },

    async clearLabTarget(deviceId) {
      return client.postJson('/api/ir-lab-clear', { deviceId });
    }
  };
}
