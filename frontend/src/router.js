// Hash-based router with lazy view loading for Harmony Remote v3
import { store } from './store.js';

class Router {
  constructor() {
    this.routes = new Map();
    this.currentViewModule = null;
    this.mountPoint = null;
  }

  setMountPoint(el) {
    this.mountPoint = el;
  }

  init() {
    window.addEventListener('hashchange', () => this.handleRoute());
    this.handleRoute();
  }

  navigate(hash) {
    window.location.hash = hash.startsWith('#') ? hash : `#${hash}`;
  }

  async handleRoute() {
    const rawHash = window.location.hash.slice(1) || '/remote';
    const path = rawHash.startsWith('/') ? rawHash.slice(1) : rawHash;
    const parts = path.split('/');

    let tab = 'remote';
    let subView = null;

    if (parts[0] === 'devices') {
      tab = 'devices';
      if (parts[1]) subView = parts[1]; // Device ID if viewing specific device remote
    } else if (parts[0] === 'setup') {
      tab = 'setup';
      subView = parts[1] || 'menu';
    } else {
      tab = 'remote';
    }

    store.update({
      activeTab: tab,
      activeSetupView: subView,
    });

    // Notify tab change
    window.dispatchEvent(new CustomEvent('harmony-route-changed', {
      detail: { tab, subView }
    }));
  }
}

export const router = new Router();
export default router;
