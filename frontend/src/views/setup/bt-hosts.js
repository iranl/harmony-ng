// Bluetooth Host Pairing and Supervision View
import { h } from '../../utils/dom.js';
import { api } from '../../api/index.js';
import { store } from '../../store.js';
import { t } from '../../i18n/index.js';
import { toast } from '../../components/toast.js';

export function createView() {
  const container = h('div', { className: 'setup-subview-bthosts', style: { display: 'flex', flexDirection: 'column', gap: '16px' } });

  const statusCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('bt_hosts', 'Bluetooth Hosts & Pairing') }),
      h('button', {
        className: 'btn btn-secondary btn-sm',
        text: t('refresh', 'Refresh'),
        onclick: () => loadStatus(),
      }),
    ]),
    h('div', { id: 'bt-status-content', className: 'text-dim', text: 'Loading BT status...' }),
  ]);

  const pairCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h4', { className: 'card-title', text: 'Pairing Mode' }),
    ]),
    h('p', { className: 'text-dim', style: { fontSize: '12px', marginBottom: '12px' }, text: 'Make Harmony Hub discoverable as a Bluetooth HID Keyboard to pair with Nvidia Shield, Apple TV, Fire TV, or PC.' }),
    h('div', { style: { display: 'flex', gap: '8px' } }, [
      h('button', {
        className: 'btn btn-primary',
        text: '📶 Enable BT Pairing (60s)',
        onclick: async () => {
          try {
            await api.setBtPairing(true, 'Harmony Keyboard');
            toast.success('Hub is now discoverable as Bluetooth Keyboard!');
            loadStatus();
          } catch (e) {
            toast.error(e.message);
          }
        },
      }),
      h('button', {
        className: 'btn btn-secondary',
        text: 'Disable Pairing',
        onclick: async () => {
          try {
            await api.setBtPairing(false);
            toast.info('Disabled BT pairing mode');
            loadStatus();
          } catch (e) {
            toast.error(e.message);
          }
        },
      }),
    ]),
  ]);

  const hostsCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h4', { className: 'card-title', text: 'Connected Hosts' }),
    ]),
    h('div', { id: 'bt-hosts-mount', className: 'text-dim', text: 'Loading hosts...' }),
  ]);

  async function loadStatus() {
    const statusMount = container.querySelector('#bt-status-content');
    const hostsMount = container.querySelector('#bt-hosts-mount');
    if (!statusMount || !hostsMount) return;

    try {
      const data = await api.getBtStatus();
      statusMount.innerHTML = '';

      const isPairing = Boolean(data?.pairing);
      const isConnected = Boolean(data?.connected);

      const grid = h('div', { style: { display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(140px, 1fr))', gap: '8px' } }, [
        h('div', { className: 'card', style: { margin: 0, padding: '8px' } }, [
          h('div', { className: 'card-sub', text: 'Stack State' }),
          h('div', { style: { fontWeight: 'bold' }, text: data?.state || 'Running' }),
        ]),
        h('div', { className: 'card', style: { margin: 0, padding: '8px' } }, [
          h('div', { className: 'card-sub', text: 'Pairing' }),
          h('div', { style: { fontWeight: 'bold', color: isPairing ? 'var(--accent)' : 'inherit' }, text: isPairing ? 'Discoverable' : 'Off' }),
        ]),
        h('div', { className: 'card', style: { margin: 0, padding: '8px' } }, [
          h('div', { className: 'card-sub', text: 'Active Link' }),
          h('div', { style: { fontWeight: 'bold', color: isConnected ? 'var(--success)' : 'inherit' }, text: isConnected ? 'Connected' : 'Idle' }),
        ]),
      ]);

      statusMount.appendChild(grid);

      hostsMount.innerHTML = '';
      const hosts = data?.hosts || data?.pairedHosts || [];
      if (hosts.length === 0) {
        hostsMount.textContent = 'No paired Bluetooth hosts.';
      } else {
        const list = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '6px' } });
        hosts.forEach(hItem => {
          const bdaddr = hItem.bdaddr || hItem.address || hItem;
          const row = h('div', {
            className: 'card',
            style: { display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: '8px 12px', margin: 0 },
          }, [
            h('div', {}, [
              h('div', { style: { fontWeight: 'bold' }, text: hItem.name || 'Bluetooth Host' }),
              h('div', { className: 'text-dim', style: { fontSize: '11px' }, text: bdaddr }),
            ]),
            h('div', { style: { display: 'flex', gap: '6px' } }, [
              h('button', {
                className: 'btn btn-secondary btn-sm',
                text: 'Connect',
                onclick: async () => {
                  await api.connectBt(bdaddr);
                  toast.info(`Connecting to ${bdaddr}...`);
                },
              }),
              h('button', {
                className: 'btn btn-danger btn-sm',
                text: 'Disconnect',
                onclick: async () => {
                  await api.disconnectBt(bdaddr);
                  toast.info('Disconnected');
                },
              }),
            ]),
          ]);
          list.appendChild(row);
        });
        hostsMount.appendChild(list);
      }
    } catch (e) {
      statusMount.textContent = `Error: ${e.message}`;
    }
  }

  container.appendChild(statusCard);
  container.appendChild(pairCard);
  container.appendChild(hostsCard);
  loadStatus();

  return container;
}

export default createView;
