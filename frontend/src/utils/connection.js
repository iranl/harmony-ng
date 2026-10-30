// Connection monitor and health checker for Harmony Hub
import { store } from '../store.js';
import { api } from '../api/index.js';

class ConnectionMonitor {
  constructor() {
    this.timer = null;
    this.intervalMs = 5000;
    this.isPolling = false;
  }

  start() {
    this.stop();
    this.checkHealth();
    this.timer = setInterval(() => this.checkHealth(), this.intervalMs);

    // Setup WS status listener
    api.onWsStatus = (connected) => {
      store.update({
        'connection.wsConnected': connected,
        'connection.online': connected || store.get('connection.online'),
      });
    };

    // Setup WS message listener
    api.onWsMessage = (msg) => {
      this.handleWsMessage(msg);
    };
  }

  stop() {
    if (this.timer) {
      clearInterval(this.timer);
      this.timer = null;
    }
  }

  async checkHealth() {
    if (this.isPolling) return;
    this.isPolling = true;

    try {
      const res = await api.testConnection();
      if (res.ok) {
        const wasOffline = !store.get('connection.online');
        store.update({
          'connection.online': true,
          'connection.failures': 0,
          'connection.reconnecting': false,
        });

        if (wasOffline) {
          // Trigger refresh of inventory and activities
          this.refreshData();
        }
      } else {
        const failures = (store.get('connection.failures') || 0) + 1;
        store.update({
          'connection.failures': failures,
          'connection.online': failures < 3,
        });
      }
    } catch {
      const failures = (store.get('connection.failures') || 0) + 1;
      store.update({
        'connection.failures': failures,
        'connection.online': failures < 3,
      });
    } finally {
      this.isPolling = false;
    }
  }

  async refreshData() {
    try {
      const [inv, acts] = await Promise.all([
        api.getInventory().catch(() => ({ devices: [] })),
        api.getActivities().catch(() => ({ activities: [], currentId: '-1' })),
      ]);

      store.update({
        devices: inv.devices || [],
        'activities.list': acts.activities || [],
        'activities.currentId': acts.currentId || '-1',
        'activities.currentName': acts.currentName || 'PowerOff',
      });
    } catch (e) {
      console.warn('Failed to refresh data on reconnect:', e);
    }
  }

  handleWsMessage(data) {
    if (!data) return;

    if (data.event === 'activity_changed' || data.event === 'activity_started') {
      const curId = String(data.id || data.activityId || '-1');
      const acts = store.get('activities.list') || [];
      const act = acts.find(a => String(a.id) === curId);
      store.update({
        'activities.currentId': curId,
        'activities.currentName': act ? act.name : (curId === '-1' ? 'PowerOff' : curId),
        'activities.transitioning': false,
        'activities.progress': null,
      });
    } else if (data.event === 'activity_transition' || data.event === 'activity_progress') {
      store.update({
        'activities.transitioning': true,
        'activities.progress': data,
      });
    } else if (data.event === 'rf_status' || data.type === 'rf_status') {
      store.set('rfStatus', data);
    } else if (data.event === 'bt_status' || data.type === 'bt_status') {
      store.set('btStatus', data);
    }
  }
}

export const connection = new ConnectionMonitor();
export default connection;
