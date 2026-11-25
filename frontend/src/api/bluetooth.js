// Bluetooth API methods for Harmony Hub

export function createBluetoothApi(client) {
  return {
    async getBtStatus() {
      return client.request('/api/bt-status');
    },

    // profile: 'keyboard' (default) or 'ps3' (the Hub pairs as the PS3's BD Remote Control)
    async setBtPairing(enable, name = '', targetDev = '', profile = 'keyboard') {
      return client.postJson('/api/bt-pairing', {
        enable: enable ? 1 : 0,
        name: name || 'Harmony Keyboard',
        targetDev: targetDev || '',
        profile: profile || 'keyboard'
      });
    },

    // The code a host shows while it pairs (bt-status pin.state "waiting"), or cancel it
    async sendBtPairingPin(pin) {
      return client.postJson('/api/bt-pairing-pin', { pin: String(pin || '') });
    },

    async cancelBtPairingPin() {
      return client.postJson('/api/bt-pairing-pin', { cancel: true });
    },

    async connectBt(bdaddr) {
      return client.postJson('/api/bt-connect', { bdaddr });
    },

    async disconnectBt(bdaddr = '') {
      return client.postJson('/api/bt-disconnect', { bdaddr: bdaddr || '' });
    },

    // Remove a paired host: its link key, its link and the devices' links to it (admin)
    async forgetBtHost(addr) {
      return client.postJson('/api/bt-forget-host', { addr: String(addr || '') });
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

    async saveBtCommand(deviceId, name, script) {
      return client.postForm('/bt/command', { deviceId, name, script });
    },

    async deleteBtCommand(deviceId, command) {
      return client.postForm('/bt/delete-command', { deviceId, command });
    }
  };
}
