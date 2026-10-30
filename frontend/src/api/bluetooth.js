// Bluetooth API methods for Harmony Hub

export function createBluetoothApi(client) {
  return {
    async getBtStatus() {
      return client.request('/api/bt-status');
    },

    async setBtPairing(enable, name = '', targetDev = '') {
      return client.postJson('/api/bt-pairing', {
        enable: enable ? 1 : 0,
        name: name || 'Harmony Keyboard',
        targetDev: targetDev || ''
      });
    },

    async connectBt(bdaddr) {
      return client.postJson('/api/bt-connect', { bdaddr });
    },

    async disconnectBt(bdaddr = '') {
      return client.postJson('/api/bt-disconnect', { bdaddr: bdaddr || '' });
    },

    async linkBtDevice(bdaddr, deviceId) {
      return client.postJson('/api/bt-link-device', { bdaddr, deviceId });
    },

    async sendBtKey(code, action = 'tap', target = '') {
      if (!code) return { ok: false };
      if (client.sendWs({ action: 'bt_key', key: String(code), keyAction: String(action), target: String(target) })) {
        return { ok: true, ws: true };
      }
      return client.postJson('/api/bt-key', {
        key: String(code),
        code: String(code),
        action: String(action),
        target: String(target)
      });
    },

    async sendBtText(text, target = '') {
      if (client.sendWs({ action: 'bt_text', text: String(text), target: String(target) })) {
        return { ok: true, ws: true };
      }
      return client.postJson('/api/bt-text', { text: String(text), target: String(target) });
    },

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
      if (client.sendWs(payload)) {
        return { ok: true, ws: true };
      }
      return client.postJson('/api/bt-mouse', payload);
    },

    async runBtScript(script, delay = 35) {
      return client.postJson('/api/bt-script', { script: String(script), delay: Number(delay) || 35 });
    },

    async saveBtCommand(deviceId, name, script) {
      return client.postForm('/bt/command', { deviceId, name, script });
    },

    async deleteBtCommand(deviceId, command) {
      return client.postForm('/bt/delete-command', { deviceId, command });
    }
  };
}
