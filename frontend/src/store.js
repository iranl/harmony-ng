// Central reactive store for Harmony Remote v3

class Store {
  constructor() {
    this.state = {
      hub: null,
      connection: {
        online: false,
        wsConnected: false,
        failures: 0,
        reconnecting: false,
      },
      activities: {
        list: [],
        currentId: '-1',
        currentName: 'PowerOff',
        transitioning: false,
        progress: null,
      },
      devices: [],
      commandsMap: {},
      rfStatus: null,
      btStatus: null,
      systemStatus: null,
      activeTab: 'remote', // 'remote' | 'devices' | 'setup'
      activeSetupView: null,
      activeDeviceControlId: null,
    };

    this.listeners = new Map(); // path -> Set of callbacks
  }

  getState() {
    return this.state;
  }

  get(path) {
    if (!path) return this.state;
    const parts = path.split('.');
    let curr = this.state;
    for (const part of parts) {
      if (curr === null || curr === undefined) return undefined;
      curr = curr[part];
    }
    return curr;
  }

  set(path, value) {
    const parts = path.split('.');
    let curr = this.state;
    for (let i = 0; i < parts.length - 1; i++) {
      const part = parts[i];
      if (curr[part] === undefined || curr[part] === null) {
        curr[part] = {};
      }
      curr = curr[part];
    }
    const last = parts[parts.length - 1];
    const prev = curr[last];
    curr[last] = value;

    this.emit(path, value, prev);
  }

  update(batch) {
    for (const [path, value] of Object.entries(batch)) {
      this.set(path, value);
    }
  }

  on(path, callback) {
    if (!this.listeners.has(path)) {
      this.listeners.set(path, new Set());
    }
    this.listeners.get(path).add(callback);
    return () => this.off(path, callback);
  }

  off(path, callback) {
    const set = this.listeners.get(path);
    if (set) {
      set.delete(callback);
      if (set.size === 0) {
        this.listeners.delete(path);
      }
    }
  }

  emit(path, value, prev) {
    // Notify exact path listeners
    if (this.listeners.has(path)) {
      for (const cb of this.listeners.get(path)) {
        try {
          cb(value, prev);
        } catch (e) {
          console.error(`Store listener error on ${path}:`, e);
        }
      }
    }

    // Also notify root '*' listeners
    if (this.listeners.has('*')) {
      for (const cb of this.listeners.get('*')) {
        try {
          cb(path, value, prev);
        } catch (e) {
          console.error('Store listener error on *:', e);
        }
      }
    }
  }
}

export const store = new Store();
export default store;
