// Status Pill component showing hub connection state
import { h } from '../utils/dom.js';
import { store } from '../store.js';
import { t } from '../i18n/index.js';

export function createStatusPill() {
  const pill = h('span', { className: 'status-pill' });
  const label = h('span', { className: 'status-label', text: t('status_offline', 'Offline') });
  const container = h('div', { className: 'status-pill-container' }, [pill, label]);

  const update = () => {
    const online = store.get('connection.online');
    const ws = store.get('connection.wsConnected');
    const reconnecting = store.get('connection.reconnecting');

    pill.className = 'status-pill';
    if (online && ws) {
      pill.classList.add('online');
      label.textContent = t('hub_status_online', 'Online');
    } else if (reconnecting || (online && !ws)) {
      pill.classList.add('connecting');
      label.textContent = t('hub_status_connecting', 'Connecting...');
    } else {
      pill.classList.add('offline');
      label.textContent = t('hub_status_offline', 'Offline');
    }
  };

  store.on('connection.online', update);
  store.on('connection.wsConnected', update);
  store.on('connection.reconnecting', update);
  window.addEventListener('harmony-lang-changed', update);

  update();
  return container;
}

export default createStatusPill;
