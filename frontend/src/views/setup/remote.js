// Harmony (RF) Remote Mapping, Pairing, Screen Layout & OTA View
import { h } from '../../utils/dom.js';
import { api } from '../../api/index.js';
import { t } from '../../i18n/index.js';
import { toast } from '../../components/toast.js';
import { confirm } from '../../components/confirm.js';
import { createButtonEditor } from './remote-buttons.js';

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
              toast.success('Pushed config to remote');
            } catch (e) {
              toast.error(e.message);
            }
          },
        }),
      ]),
    ]),
    h('p', { className: 'text-dim', style: { fontSize: '13px' }, text: t('card_remote_sub', 'Physical RF remote pairing, touchscreen layout & button mapping') }),
  ]);

  // RF link counters for the selected slot
  const linkStatus = h('p', { style: { fontSize: '13px', marginBottom: '12px' }, text: '' });
  async function loadLinkStatus() {
    const st = await api.getRfStatus().catch(() => null);
    const link = st?.slots?.find(s => s.slot === currentSlot)?.link;
    if (!link) { linkStatus.textContent = ''; return; }
    const seen = link.last_seen_s < 0 ? t('rf_link_never', 'never') : `${link.last_seen_s} s ago`;
    const acked = link.acked_sent ? `${Math.round(link.acked_ok * 100 / link.acked_sent)}% of ${link.acked_sent}` : '-';
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
      h('h4', { className: 'card-title', text: 'RF Pairing & Status' }),
      h('div', { style: { display: 'flex', gap: '6px' } }, [
        h('button', {
          className: 'btn btn-secondary btn-sm',
          text: currentSlot === 1 ? 'Slot 1 (Active)' : 'Slot 1',
          onclick: () => { currentSlot = 1; loadSlot(); },
        }),
        h('button', {
          className: 'btn btn-secondary btn-sm',
          text: currentSlot === 2 ? 'Slot 2 (Active)' : 'Slot 2',
          onclick: () => { currentSlot = 2; loadSlot(); },
        }),
      ]),
    ]),
    linkStatus,
    h('p', { className: 'text-dim', style: { fontSize: '12px', marginBottom: '12px' }, text: t('rf_pairing_note', 'Note: On touchscreen remotes with custom firmware, enable pairing via Settings > Pair Remote. On Companion remotes, use the physical pairing button combination.') }),
    h('div', { style: { display: 'flex', gap: '8px', flexWrap: 'wrap' } }, [
      h('button', {
        className: 'btn btn-primary',
        text: '📡 Pair Remote (Hub Listen)',
        onclick: async () => {
          try {
            await api.pairRfRemote(currentSlot);
            isPairing = true;
            toast.info(t('rf_pairing_active_note', 'Hub pairing active! On touchscreen remotes: Settings > Pair Remote. On Companion: press & hold pairing buttons.'));
          } catch (e) {
            toast.error(e.message);
          }
        },
      }),
      h('button', {
        className: 'btn btn-secondary',
        text: 'Stop Pairing',
        onclick: async () => {
          try {
            await api.stopPairRfRemote();
            isPairing = false;
            toast.info('Stopped pairing');
          } catch (e) {
            toast.error(e.message);
          }
        },
      }),
      h('button', {
        className: 'btn btn-danger',
        text: 'Unpair Slot',
        onclick: async () => {
          const ok = await confirm.ask({
            title: 'Unpair Remote Slot',
            message: `Unpair RF Remote on Slot ${currentSlot}?`,
            danger: true,
          });
          if (ok) {
            try {
              await api.unpairRfRemote(currentSlot);
              toast.warn(`Unpaired slot ${currentSlot}`);
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
      h('h4', { className: 'card-title', text: 'Remote Firmware (RF OTA)' }),
      h('button', {
        className: 'btn btn-secondary btn-sm',
        text: 'Check OTA Status',
        onclick: async () => {
          try {
            const st = await api.getRfOtaStatus();
            toast.info(`OTA status: ${st?.status || 'Idle'}`);
          } catch (e) {
            toast.error(e.message);
          }
        },
      }),
    ]),
    h('p', { className: 'text-dim', style: { fontSize: '12px', marginBottom: '8px' }, text: 'Flash custom firmware to physical touchscreen remote wirelessly over CC2544 RF channel.' }),
    h('button', {
      className: 'btn btn-primary btn-sm',
      text: 'Start RF OTA Flash',
      onclick: async () => {
        const ok = await confirm.ask({
          title: 'Start RF OTA Flash',
          message: 'Ensure remote is near hub and battery is charged (>50%). Flash will take 2-4 minutes.',
          danger: true,
        });
        if (ok) {
          try {
            await api.startRfOta(currentSlot);
            toast.info('RF OTA process initiated');
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
