// Bluetooth Host Pairing and Supervision View
import { h } from '../../utils/dom.js';
import { api } from '../../api/index.js';
import { store } from '../../store.js';
import { t } from '../../i18n/index.js';
import { toast } from '../../components/toast.js';
import { confirm } from '../../components/confirm.js';

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
    h('div', { id: 'bt-status-content', className: 'text-dim', text: t('loading_bt_status', 'Loading BT status...') }),
  ]);

  const pairCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h4', { className: 'card-title', text: t('pairing_mode', 'Pairing Mode') }),
    ]),
    h('p', { className: 'text-dim', style: { fontSize: '12px', marginBottom: '12px' }, text: t('bt_pairing_mode_desc', 'Make Harmony Hub discoverable as a Bluetooth HID Keyboard to pair with Nvidia Shield, Apple TV, Fire TV, or PC.') }),
    h('div', { style: { display: 'flex', gap: '8px' } }, [
      h('button', {
        className: 'btn btn-primary',
        text: `📶 ${t('enable_bt_pairing_60', 'Enable BT Pairing (60s)')}`,
        onclick: async () => {
          try {
            await api.setBtPairing(true, 'Harmony Keyboard');
            toast.success(t('hub_discoverable_bt_keyboard', 'Hub is now discoverable as Bluetooth Keyboard!'));
            loadStatus();
          } catch (e) {
            toast.error(e.message);
          }
        },
      }),
      h('button', {
        className: 'btn btn-secondary',
        text: t('disable_pairing', 'Disable Pairing'),
        onclick: async () => {
          try {
            await api.setBtPairing(false);
            toast.info(t('bt_pairing_disabled', 'Disabled BT pairing mode'));
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
      h('h4', { className: 'card-title', text: t('connected_hosts', 'Connected Hosts') }),
    ]),
    h('div', { id: 'bt-hosts-mount', className: 'text-dim', text: t('loading_hosts', 'Loading hosts...') }),
  ]);

  // pushed: a bt_status object from the Hub (WebSocket); fetched when absent
  async function loadStatus(pushed = null) {
    const statusMount = container.querySelector('#bt-status-content');
    const hostsMount = container.querySelector('#bt-hosts-mount');
    if (!statusMount || !hostsMount) return;

    try {
      const data = pushed || await api.getBtStatus();
      statusMount.innerHTML = '';

      // hub_btstack reports pairing and the active link under "host"
      const isPairing = Boolean(data?.host?.pairing ?? data?.pairing);
      const isConnected = Boolean(data?.host?.connected ?? data?.connected);

      const grid = h('div', { style: { display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(140px, 1fr))', gap: '8px' } }, [
        h('div', { className: 'card', style: { margin: 0, padding: '8px' } }, [
          h('div', { className: 'card-sub', text: t('stack_state', 'Stack State') }),
          h('div', { style: { fontWeight: 'bold' }, text: data?.state || t('state_running', 'Running') }),
        ]),
        h('div', { className: 'card', style: { margin: 0, padding: '8px' } }, [
          h('div', { className: 'card-sub', text: t('pairing', 'Pairing') }),
          h('div', { style: { fontWeight: 'bold', color: isPairing ? 'var(--accent)' : 'inherit' }, text: isPairing ? t('discoverable', 'Discoverable') : t('pairing_state_off', 'Off') }),
        ]),
        h('div', { className: 'card', style: { margin: 0, padding: '8px' } }, [
          h('div', { className: 'card-sub', text: t('active_link', 'Active Link') }),
          h('div', { style: { fontWeight: 'bold', color: isConnected ? 'var(--success)' : 'inherit' }, text: isConnected ? t('connected', 'Connected') : t('state_idle', 'Idle') }),
        ]),
      ]);

      statusMount.appendChild(grid);

      hostsMount.innerHTML = '';
      const hosts = data?.hosts || data?.pairedHosts || [];
      if (hosts.length === 0) {
        hostsMount.textContent = t('no_paired_bt_hosts', 'No paired Bluetooth hosts.');
      } else {
        const list = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '6px' } });
        hosts.forEach(hItem => {
          // hub_btstack lists hosts as { addr, name, connected, paired }
          const bdaddr = hItem.addr || hItem.bdaddr || hItem.address || String(hItem);
          const state = hItem.connected ? t('connected', 'Connected') : (hItem.paired === false ? t('not_paired_no_key', 'Not paired (no link key)') : t('paired', 'Paired'));
          const row = h('div', {
            className: 'card',
            style: { display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: '8px 12px', margin: 0 },
          }, [
            h('div', {}, [
              h('div', { style: { fontWeight: 'bold' }, text: hItem.name || t('bluetooth_host', 'Bluetooth Host') }),
              h('div', { className: 'text-dim', style: { fontSize: '11px' }, text: `${bdaddr} • ${state}` }),
            ]),
            h('div', { style: { display: 'flex', gap: '6px' } }, [
              h('button', {
                className: 'btn btn-secondary btn-sm',
                text: t('connect', 'Connect'),
                onclick: async () => {
                  await api.connectBt(bdaddr);
                  toast.info(t('connecting_to', 'Connecting to {name}...', { name: bdaddr }));
                },
              }),
              h('button', {
                className: 'btn btn-danger btn-sm',
                text: t('disconnect', 'Disconnect'),
                onclick: async () => {
                  await api.disconnectBt(bdaddr);
                  toast.info(t('disconnected', 'Disconnected'));
                },
              }),
              h('button', {
                className: 'btn btn-danger btn-sm',
                text: t('bt_forget_host', 'Forget'),
                onclick: async () => {
                  const name = hItem.name || bdaddr;
                  const ok = await confirm.ask({
                    title: t('bt_forget_host_title', 'Forget Bluetooth host'),
                    message: t('bt_forget_host_confirm', 'Forget {name} ({addr})?', { name, addr: bdaddr }),
                    confirmText: t('bt_forget_host', 'Forget'),
                    danger: true,
                  });
                  if (!ok) return;
                  try {
                    await api.forgetBtHost(bdaddr);
                    toast.success(t('bt_host_forgotten', 'Bluetooth host forgotten'));
                    loadStatus();
                  } catch (e) {
                    toast.error(t('bt_forget_failed', 'Forget failed: {error}', { error: e.message }));
                  }
                },
              }),
            ]),
          ]);
          list.appendChild(row);
        });
        hostsMount.appendChild(list);
      }
    } catch (e) {
      statusMount.textContent = t('error_msg', 'Error: {error}', { error: e.message });
    }
  }

  container.appendChild(statusCard);
  container.appendChild(pairCard);
  container.appendChild(hostsCard);
  loadStatus();

  // The Hub pushes bt_status when the Bluetooth state changes (connection.js puts it in the store)
  const off = store.on('btStatus', (data) => {
    if (!container.isConnected) { off(); return; }
    loadStatus(data);
  });

  return container;
}

export default createView;
