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

  // The device the new host is linked to (the Hub requires one) and what the Hub pairs as
  const devices = store.get('devices') || [];
  const isBtDev = (d) => d.type === 'BT' || d.type === 'Bluetooth' || d.isBt;
  const devSelect = h('select', { className: 'form-control', style: { flex: '1', minWidth: '140px' } }, [
    h('option', { value: '', text: t('bt_pair_select_device', 'Device to link the host to') }),
    ...[...devices].sort((a, b) => Number(isBtDev(b)) - Number(isBtDev(a))).map(d =>
      h('option', { value: String(d.id), text: `${d.name || d.id}${isBtDev(d) ? ' (BT)' : ''}` })),
  ]);
  const firstBt = devices.find(isBtDev);
  if (firstBt) devSelect.value = String(firstBt.id);
  const profileSelect = h('select', { className: 'form-control', style: { flex: '1', minWidth: '140px' } }, [
    h('option', { value: 'keyboard', text: t('bt_profile_keyboard', 'Keyboard (Android TV, Fire TV, Apple TV, PC)') }),
    h('option', { value: 'ps3', text: t('bt_profile_ps3', 'PlayStation 3 (BD Remote Control)') }),
  ]);

  const pairCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h4', { className: 'card-title', text: t('pairing_mode', 'Pairing Mode') }),
    ]),
    h('p', { className: 'text-dim', style: { fontSize: '12px', marginBottom: '12px' }, text: t('bt_pairing_mode_desc', 'Make Harmony Hub discoverable as a Bluetooth HID Keyboard to pair with Nvidia Shield, Apple TV, Fire TV, or PC.') }),
    h('div', { style: { display: 'flex', gap: '8px', flexWrap: 'wrap', marginBottom: '8px' } }, [
      devSelect,
      h('label', { className: 'form-label', style: { alignSelf: 'center', margin: 0 }, text: t('bt_profile_label', 'Pair as') }),
      profileSelect,
    ]),
    h('div', { style: { display: 'flex', gap: '8px', flexWrap: 'wrap' } }, [
      h('button', {
        className: 'btn btn-primary',
        text: `📶 ${t('enable_bt_pairing_60', 'Enable BT Pairing (180 s)')}`,
        onclick: async () => {
          if (!devSelect.value) {
            toast.error(t('select_target_bt_pairing', 'Please select a target device to assign this Bluetooth pairing to.'));
            return;
          }
          try {
            const profile = profileSelect.value;
            await api.setBtPairing(true, 'Harmony Keyboard', devSelect.value, profile);
            toast.success(profile === 'ps3'
              ? t('bt_discoverable_ps3', 'Hub is now discoverable as BD Remote Control: register it on the PS3 under Accessory Settings > Manage Bluetooth Devices.')
              : t('hub_discoverable_bt_keyboard', 'Hub is now discoverable as Bluetooth Keyboard!'));
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

  // Legacy PIN pairing: shown while a host refused 0000 (pair again) or waits for its code
  const pinInput = h('input', {
    className: 'form-control', type: 'text', inputMode: 'numeric', maxLength: 16, autocomplete: 'off',
    placeholder: t('bt_pin_placeholder', 'Code'), style: { flex: '1', minWidth: '120px', letterSpacing: '2px' },
  });
  const pinMsg = h('div', { style: { fontWeight: 'bold', marginBottom: '8px' } });
  const pinRow = h('div', { style: { display: 'flex', gap: '8px', flexWrap: 'wrap' } }, [
    pinInput,
    h('button', {
      className: 'btn btn-primary',
      text: t('bt_pin_send', 'Send code'),
      onclick: async () => {
        const pin = pinInput.value.trim();
        if (!/^\d{1,16}$/.test(pin)) {
          toast.error(t('bt_pin_invalid', 'Enter the 1 to 16 digits the device shows'));
          return;
        }
        try {
          await api.sendBtPairingPin(pin);
          pinInput.value = '';
          toast.success(t('bt_pin_sent', 'Code sent'));
        } catch (e) {
          toast.error(e.message);
        }
      },
    }),
    h('button', {
      className: 'btn btn-secondary',
      text: t('cancel', 'Cancel'),
      onclick: async () => {
        try {
          await api.cancelBtPairingPin();
          loadStatus();
        } catch (e) {
          toast.error(e.message);
        }
      },
    }),
  ]);
  const pinCard = h('div', { className: 'card', style: { display: 'none', borderColor: 'var(--accent)' } }, [pinMsg, pinRow]);
  pinInput.addEventListener('keydown', (e) => { if (e.key === 'Enter') pinRow.querySelector('.btn-primary').click(); });

  function renderPin(pin) {
    const state = pin?.state || 'none';
    pinCard.style.display = state === 'none' ? 'none' : '';
    const name = pin?.name || pin?.addr || t('bluetooth_host', 'Bluetooth Host');
    if (state === 'waiting') {
      pinMsg.textContent = t('bt_pin_waiting', 'Enter the code shown on {name}, then Send code.', { name });
      pinRow.style.display = 'flex';
      if (document.activeElement !== pinInput) pinInput.focus();
    } else if (state === 'required') {
      pinMsg.textContent = t('bt_pin_required', '{name} did not accept PIN 0000. Start the pairing again on the device: the Hub then asks here for the code it shows.', { name });
      pinRow.style.display = 'none';
    }
  }

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
      renderPin(data?.pin);

      hostsMount.innerHTML = '';
      const hosts = data?.hosts || data?.pairedHosts || [];
      if (hosts.length === 0) {
        hostsMount.textContent = t('no_paired_bt_hosts', 'No paired Bluetooth hosts.');
      } else {
        const list = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '6px' } });
        hosts.forEach(hItem => {
          // hub_btstack lists hosts as { addr, name, connected, paired }
          const bdaddr = hItem.addr || hItem.bdaddr || hItem.address || String(hItem);
          const linkState = hItem.connected ? t('connected', 'Connected') : (hItem.paired === false ? t('not_paired_no_key', 'Not paired (no link key)') : t('paired', 'Paired'));
          const state = hItem.profile === 'ps3' ? `${linkState} • ${t('bt_profile_ps3_short', 'PS3 BD Remote')}` : linkState;
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
  container.appendChild(pinCard);
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
