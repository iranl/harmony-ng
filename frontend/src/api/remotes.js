// Physical Remotes (RF & BT) API methods for Harmony Hub

export function createRemotesApi(client) {
  return {
    async getRemoteMappings() {
      return client.request('/api/remote-mapping');
    },

    async saveRemoteMappings(data) {
      return client.postJson('/api/remote-mapping-save', data);
    },

    async getUiRemoteLayout() {
      return client.request('/api/ui-remote-layout');
    },

    async saveUiRemoteLayout(layouts) {
      return client.postJson('/api/ui-remote-layout', layouts);
    },

    async getRFMapping(slot = 1) {
      return client.request(`/api/rf-mapping?slot=${slot}`);
    },

    async saveRFMapping(data, slot = 1) {
      return client.postJson(`/api/rf-mapping-save?slot=${slot}`, data);
    },

    async copyRFMapping(fromSlot, toSlot) {
      return client.request(`/api/rf-mapping-copy?from=${fromSlot}&to=${toSlot}`, { method: 'POST' });
    },

    async getRfStatus() {
      return client.request('/api/rf/status');
    },

    async pushRfConfig() {
      return client.request('/api/rf/push-config', { method: 'POST' });
    },

    async pullRfConfig(slot = 1) {
      return client.request(`/api/rf/pull-config?slot=${slot}`, { method: 'POST' });
    },

    async getRfOtaStatus() {
      return client.request('/api/rf/ota-status');
    },

    async startRfOta(slot = 1) {
      return client.request(`/api/rf/ota-start?slot=${slot}`, { method: 'POST' });
    },

    async pairRfRemote(slot = 0) {
      const url = slot > 0 ? `/api/rf/pair?slot=${slot}` : '/api/rf/pair';
      return client.request(url, { method: 'POST' });
    },

    async stopPairRfRemote() {
      return client.request('/api/rf/stop-pair', { method: 'POST' });
    },

    async unpairRfRemote(slot = 0) {
      const url = slot > 0 ? `/api/rf/unpair?slot=${slot}` : '/api/rf/unpair';
      return client.request(url, { method: 'POST' });
    },

    async scanBtRemote() {
      return client.request('/api/remote-scan', { method: 'POST' });
    },

    async pairBtRemote(bdaddr, transport = 32) {
      return client.postForm('/api/remote-pair', { bdaddr: bdaddr || '', transport: String(transport) });
    },

    async getBtRemotePairStatus() {
      return client.request('/api/remote-pair-status');
    }
  };
}
