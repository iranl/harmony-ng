// Network (Wi-Fi & Ethernet) Configuration View
import { h } from '../../utils/dom.js';
import { api } from '../../api/index.js';
import { t } from '../../i18n/index.js';
import { toast } from '../../components/toast.js';

export function createView() {
  const container = h('div', { className: 'setup-subview-network', style: { display: 'flex', flexDirection: 'column', gap: '16px' } });

  const statusCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('network_status', 'Network Status') }),
      h('button', {
        className: 'btn btn-secondary btn-sm',
        text: t('refresh', 'Refresh'),
        onclick: () => loadNetStatus(),
      }),
    ]),
    h('div', { id: 'net-status-mount', className: 'text-dim', text: 'Loading network info...' }),
  ]);

  const wifiCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h4', { className: 'card-title', text: 'Wi-Fi Connection & Scan' }),
      h('button', {
        className: 'btn btn-secondary btn-sm',
        text: '📡 Scan Networks',
        onclick: () => scanWifiNetworks(),
      }),
    ]),
    h('form', { onsubmit: (e) => saveWifi(e) }, [
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', text: 'Network SSID' }),
        h('input', { id: 'wifi-ssid-input', type: 'text', className: 'form-input', placeholder: 'MyHomeWiFi' }),
      ]),
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', text: 'Wi-Fi Password' }),
        h('input', { id: 'wifi-pass-input', type: 'password', className: 'form-input' }),
      ]),
      h('button', {
        type: 'submit',
        className: 'btn btn-primary',
        text: 'Connect Wi-Fi',
      }),
    ]),
    h('div', { id: 'wifi-scan-results', style: { marginTop: '12px' } }),
  ]);

  async function loadNetStatus() {
    const mount = container.querySelector('#net-status-mount');
    if (!mount) return;
    try {
      const net = await api.getNetworkStatus();
      mount.innerHTML = '';

      const grid = h('div', { style: { display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(140px, 1fr))', gap: '8px' } }, [
        h('div', { className: 'card', style: { margin: 0, padding: '8px' } }, [
          h('div', { className: 'card-sub', text: 'IP Address' }),
          h('div', { style: { fontWeight: 'bold' }, text: net.ip || 'Unknown' }),
        ]),
        h('div', { className: 'card', style: { margin: 0, padding: '8px' } }, [
          h('div', { className: 'card-sub', text: 'Type' }),
          h('div', { style: { fontWeight: 'bold' }, text: net.connection_type || 'Wi-Fi' }),
        ]),
        h('div', { className: 'card', style: { margin: 0, padding: '8px' } }, [
          h('div', { className: 'card-sub', text: 'Interface' }),
          h('div', { style: { fontWeight: 'bold' }, text: net.active_interface || 'ath0' }),
        ]),
        h('div', { className: 'card', style: { margin: 0, padding: '8px' } }, [
          h('div', { className: 'card-sub', text: 'MAC Address' }),
          h('div', { style: { fontWeight: 'bold', fontSize: '11px' }, text: net.mac || net.wifi_mac || 'Unknown' }),
        ]),
      ]);

      mount.appendChild(grid);
    } catch (e) {
      mount.textContent = `Error: ${e.message}`;
    }
  }

  async function scanWifiNetworks() {
    const resultsMount = container.querySelector('#wifi-scan-results');
    if (!resultsMount) return;
    resultsMount.innerHTML = '<div class="text-dim">Scanning Wi-Fi bands...</div>';
    try {
      const res = await api.scanWifi();
      const networks = Array.isArray(res) ? res : (res?.networks || res?.aps || []);
      resultsMount.innerHTML = '';
      if (networks.length === 0) {
        resultsMount.textContent = 'No networks found.';
        return;
      }
      const list = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '6px' } });
      networks.forEach(n => {
        const ssid = n.ssid || n.name;
        if (!ssid) return;
        const row = h('div', {
          className: 'card',
          style: { display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: '8px 12px', margin: 0, cursor: 'pointer' },
          onclick: () => {
            const input = container.querySelector('#wifi-ssid-input');
            if (input) input.value = ssid;
          },
        }, [
          h('span', { style: { fontWeight: '600' }, text: ssid }),
          h('span', { className: 'badge', text: `${n.signal || n.rssi || ''} dBm` }),
        ]);
        list.appendChild(row);
      });
      resultsMount.appendChild(list);
    } catch (e) {
      resultsMount.textContent = `Scan failed: ${e.message}`;
    }
  }

  async function saveWifi(e) {
    e.preventDefault();
    const ssid = container.querySelector('#wifi-ssid-input')?.value.trim();
    const password = container.querySelector('#wifi-pass-input')?.value;
    if (!ssid) return toast.warn('Enter Wi-Fi SSID');

    try {
      toast.info('Applying Wi-Fi configuration...');
      await api.saveWifi({ ssid, password });
      toast.success('Wi-Fi configuration applied!');
    } catch (err) {
      toast.error(err.message);
    }
  }

  container.appendChild(statusCard);
  container.appendChild(wifiCard);
  loadNetStatus();

  return container;
}

export default createView;
