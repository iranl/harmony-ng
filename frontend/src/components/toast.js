// Toast notification component for Harmony Remote v3
import { h } from '../utils/dom.js';

class ToastManager {
  constructor() {
    this.container = null;
    this.ensureContainer();
  }

  ensureContainer() {
    if (typeof document === 'undefined') return;
    let el = document.getElementById('toast-container');
    if (!el) {
      el = h('div', { id: 'toast-container', className: 'toast-container' });
      document.body.appendChild(el);
    }
    this.container = el;
  }

  show(message, type = 'info', duration = 3500) {
    this.ensureContainer();
    if (!this.container) return;

    const toast = h('div', { className: `toast-item toast-${type}` }, [
      h('span', { className: 'toast-message', text: message }),
      h('button', {
        className: 'toast-close',
        text: '×',
        onclick: () => this.dismiss(toast),
      }),
    ]);

    this.container.appendChild(toast);

    requestAnimationFrame(() => {
      toast.classList.add('toast-visible');
    });

    if (duration > 0) {
      setTimeout(() => {
        this.dismiss(toast);
      }, duration);
    }
    return toast;
  }

  dismiss(toast) {
    if (!toast || !toast.parentNode) return;
    toast.classList.remove('toast-visible');
    toast.classList.add('toast-leaving');
    setTimeout(() => {
      if (toast.parentNode) toast.parentNode.removeChild(toast);
    }, 250);
  }

  success(msg, duration) {
    return this.show(msg, 'success', duration);
  }

  error(msg, duration) {
    return this.show(msg, 'error', duration);
  }

  warn(msg, duration) {
    return this.show(msg, 'warning', duration);
  }

  info(msg, duration) {
    return this.show(msg, 'info', duration);
  }
}

export const toast = new ToastManager();
export default toast;
