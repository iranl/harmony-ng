// Harmony (RF) Remote Mapping, Pairing, Screen Layout & OTA View
import { h } from '../../utils/dom.js';
import { api } from '../../api/index.js';
import { t } from '../../i18n/index.js';
import { toast } from '../../components/toast.js';
import { confirm } from '../../components/confirm.js';
import { createButtonEditor, RF_SLOT_COUNT } from './remote-buttons.js';

export function createView() {
  const container = h('div', { className: 'setup-subview-rfremote', style: { display: 'flex', flexDirection: 'column', gap: '16px' } });

  let currentSlot = 1;
  let isPairing = false;

  const headerCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('harmony_remote', 'Harmony (RF) Remote') }),
      h('div', { style: { display: 'flex', gap: '8px' } }, [
        h('button', {
          className: 'btn btn-secondary btn-sm',
          text: t('push_to_remote', 'Push to Remote'),
          onclick: async () => {
            try {
              toast.info(t('config_push_started', 'Configuration push started over RF to Harmony remote!'));
              await api.pushRfConfig();
              toast.success(t('pushed_config_remote', 'Pushed config to remote'));
            } catch (e) {
              toast.error(e.message);
            }
          },
        }),
      ]),
    ]),
    h('p', { className: 'text-dim', style: { fontSize: '13px' }, text: t('card_remote_sub', 'Physical RF remote pairing, touchscreen layout & button mapping') }),
  ]);

  // A button per slot: Pair below pairs the next remote into the selected slot
  const slotBar = h('div', { style: { display: 'flex', gap: '6px', flexWrap: 'wrap' } });
  function renderSlotButtons(st) {
    const paired = n => {
      const s = st?.slots?.find(x => x.slot === n);
      const addr = s?.address || s?.rf_address || '';
      return Boolean(s?.active && addr && addr !== 'None' && addr !== '00000000');
    };
    const buttonsForSlots = [];
    for (let n = 1; n <= RF_SLOT_COUNT; n++) {
      const name = st?.slots?.find(x => x.slot === n)?.name;
      let text = paired(n) ? t('slot_n', 'Slot {n}', { n }) + (name ? `: ${name}` : '') : t('slot_n_empty', 'Slot {n} (empty)', { n });
      if (n === currentSlot) text = `✓ ${text}`;
      buttonsForSlots.push(h('button', {
        className: `btn btn-sm ${n === currentSlot ? 'btn-primary' : 'btn-secondary'}`,
        text,
        onclick: () => { currentSlot = n; loadSlot(); },
      }));
    }
    slotBar.replaceChildren(...buttonsForSlots);
  }

  // RF link counters for the selected slot
  const linkStatus = h('p', { style: { fontSize: '13px', marginBottom: '12px' }, text: '' });
  async function loadLinkStatus() {
    const st = await api.getRfStatus().catch(() => null);
    renderSlotButtons(st);
    // a remote without a screen (Companion, Smart Control) cannot pick a device
    if (st) buttons.setScreenless(st.slots?.find(s => s.slot === currentSlot)?.has_screen === false);
    const link = st?.slots?.find(s => s.slot === currentSlot)?.link;
    if (!link) { linkStatus.textContent = ''; return; }
    const seen = link.last_seen_s < 0 ? t('rf_link_never', 'never') : t('seconds_ago', '{s} s ago', { s: link.last_seen_s });
    const acked = link.acked_sent ? t('percent_of_total', '{pct}% of {total}', { pct: Math.round(link.acked_ok * 100 / link.acked_sent), total: link.acked_sent }) : '-';
    linkStatus.textContent = `${t('rf_link_last_seen', 'Last frame')}: ${seen} · ${t('rf_link_frames', 'Frames received')}: ${link.frames_rx} · `
      + `${t('rf_link_acked', 'ACKed')}: ${acked} · ${t('rf_link_retries', 'Retries')}: ${link.retries} · ${t('rf_link_naks', 'Hub radio NAKs')}: ${link.naks}`;
  }
  // Views aren't unmounted, so the refresh stops once this page has gone
  const linkTimer = setInterval(() => {
    if (!container.isConnected) clearInterval(linkTimer);
    else loadLinkStatus();
  }, 5000);

  // Key bindings, on a picture of the remote with its screen
  const buttons = createButtonEditor();
  function loadSlot() {
    loadLinkStatus();
    buttons.load(currentSlot);
  }

  // Pairing & Hardware Status Card
  const pairingCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h4', { className: 'card-title', text: t('rf_pairing_status', 'RF Pairing & Status') }),
    ]),
    slotBar,
    linkStatus,
    h('p', { className: 'text-dim', style: { fontSize: '12px', marginBottom: '12px' }, text: t('rf_pairing_note', 'Note: On touchscreen remotes with custom firmware, enable pairing via Settings > Pair Remote. On Companion remotes, use the physical pairing button combination.') }),
    h('div', { style: { display: 'flex', gap: '8px', flexWrap: 'wrap' } }, [
      h('button', {
        className: 'btn btn-primary',
        text: `📡 ${t('pair_remote_hub_listen', 'Pair Remote (Hub Listen)')}`,
        onclick: async () => {
          try {
            await api.pairRfRemote(currentSlot);
            isPairing = true;
            toast.info(t('rf_pairing_into_slot', 'Pairing into slot {n}. On touchscreen remotes: Settings > Pair Remote. On Companion: press & hold pairing buttons.', { n: currentSlot }));
          } catch (e) {
            toast.error(e.message);
          }
        },
      }),
      h('button', {
        className: 'btn btn-secondary',
        text: t('stop_pairing', 'Stop Pairing'),
        onclick: async () => {
          try {
            await api.stopPairRfRemote();
            isPairing = false;
            toast.info(t('stopped_pairing', 'Stopped pairing'));
          } catch (e) {
            toast.error(e.message);
          }
        },
      }),
      h('button', {
        className: 'btn btn-danger',
        text: t('unpair_slot', 'Unpair Slot'),
        onclick: async () => {
          const ok = await confirm.ask({
            title: t('unpair_remote_slot', 'Unpair Remote Slot'),
            message: t('unpair_slot_confirm', 'Unpair RF Remote on Slot {slot}?', { slot: currentSlot }),
            danger: true,
          });
          if (ok) {
            try {
              await api.unpairRfRemote(currentSlot);
              toast.warn(t('unpaired_slot', 'Unpaired slot {slot}', { slot: currentSlot }));
              loadLinkStatus();
            } catch (e) {
              toast.error(e.message);
            }
          }
        },
      }),
    ]),
  ]);

  // OTA Firmware Card
  const otaCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h4', { className: 'card-title', text: t('remote_firmware_rf_ota', 'Remote Firmware (RF OTA)') }),
      h('button', {
        className: 'btn btn-secondary btn-sm',
        text: t('check_ota_status', 'Check OTA Status'),
        onclick: async () => {
          try {
            const st = await api.getRfOtaStatus();
            toast.info(t('ota_status_msg', 'OTA status: {status}', { status: st?.status || t('state_idle', 'Idle') }));
          } catch (e) {
            toast.error(e.message);
          }
        },
      }),
    ]),
    h('p', { className: 'text-dim', style: { fontSize: '12px', marginBottom: '8px' }, text: t('rf_ota_desc', 'Flash custom firmware to physical touchscreen remote wirelessly over CC2544 RF channel.') }),
    h('button', {
      className: 'btn btn-primary btn-sm',
      text: t('start_rf_ota_flash', 'Start RF OTA Flash'),
      onclick: async () => {
        const ok = await confirm.ask({
          title: t('start_rf_ota_flash', 'Start RF OTA Flash'),
          message: t('rf_ota_confirm', 'Ensure remote is near hub and battery is charged (over 50%). Flash will take 2-4 minutes.'),
          danger: true,
        });
        if (ok) {
          try {
            await api.startRfOta(currentSlot);
            toast.info(t('rf_ota_started', 'RF OTA process initiated'));
          } catch (e) {
            toast.error(e.message);
          }
        }
      },
    }),
  ]);

  container.appendChild(headerCard);
  container.appendChild(buttons.element);
  container.appendChild(pairingCard);
  container.appendChild(otaCard);

  loadSlot();
  return container;
}

export default createView;
