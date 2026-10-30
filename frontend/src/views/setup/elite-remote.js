// Harmony (RF) Remote Mapping, Pairing, Screen Layout & OTA View
import { h } from '../../utils/dom.js';
import { api } from '../../api/index.js';
import { store } from '../../store.js';
import { t } from '../../i18n/index.js';
import { toast } from '../../components/toast.js';
import { confirm } from '../../components/confirm.js';

export function createView() {
  const container = h('div', { className: 'setup-subview-rfremote', style: { display: 'flex', flexDirection: 'column', gap: '16px' } });

  let currentSlot = 1;
  let activeMapping = null;
  let isPairing = false;

  const headerCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('harmony_elite', 'Harmony (RF) Remote') }),
      h('div', { style: { display: 'flex', gap: '8px' } }, [
        h('button', {
          className: 'btn btn-secondary btn-sm',
          text: t('push_elite_remote', 'Push to Remote'),
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
    h('p', { className: 'text-dim', style: { fontSize: '13px' }, text: t('card_elite_sub', 'Physical RF remote pairing, touchscreen layout & button mapping') }),
  ]);

  // Pairing & Hardware Status Card
  const pairingCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h4', { className: 'card-title', text: 'RF Pairing & Status' }),
      h('div', { style: { display: 'flex', gap: '6px' } }, [
        h('button', {
          className: 'btn btn-secondary btn-sm',
          text: currentSlot === 1 ? 'Slot 1 (Active)' : 'Slot 1',
          onclick: () => { currentSlot = 1; loadMapping(); },
        }),
        h('button', {
          className: 'btn btn-secondary btn-sm',
          text: currentSlot === 2 ? 'Slot 2 (Active)' : 'Slot 2',
          onclick: () => { currentSlot = 2; loadMapping(); },
        }),
      ]),
    ]),
    h('p', { className: 'text-dim', style: { fontSize: '12px', marginBottom: '12px' }, text: t('rf_pairing_note', 'Note: On custom Elite firmware, enable pairing via Settings > Pair Remote. On Companion remotes, use the physical pairing button combination.') }),
    h('div', { style: { display: 'flex', gap: '8px', flexWrap: 'wrap' } }, [
      h('button', {
        className: 'btn btn-primary',
        text: '📡 Pair Remote (Hub Listen)',
        onclick: async () => {
          try {
            await api.pairRfRemote(currentSlot);
            isPairing = true;
            toast.info(t('rf_pairing_active_note', 'Hub pairing active! On Elite remotes: Settings > Pair Remote. On Companion: press & hold pairing buttons.'));
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
    h('p', { className: 'text-dim', style: { fontSize: '12px', marginBottom: '8px' }, text: 'Flash custom firmware to physical Elite/950 remote wirelessly over CC2544 RF channel.' }),
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

  // Mapping View Card
  const mappingCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h4', { className: 'card-title', text: `Button Mappings (Slot ${currentSlot})` }),
      h('button', {
        className: 'btn btn-primary btn-sm',
        text: t('save_mapping', 'Save Mapping'),
        onclick: async () => {
          if (!activeMapping) return;
          try {
            await api.saveRFMapping(activeMapping, currentSlot);
            toast.success(t('elite_mapping_saved', 'Harmony remote mappings saved to Hub!'));
          } catch (e) {
            toast.error(e.message);
          }
        },
      }),
    ]),
    h('div', { id: 'rf-mapping-content', className: 'text-dim', text: 'Loading mapping...' }),
  ]);

  async function loadMapping() {
    const mount = container.querySelector('#rf-mapping-content');
    if (!mount) return;
    mount.textContent = 'Loading mapping...';

    try {
      const data = await api.getRFMapping(currentSlot);
      activeMapping = data || {};
      mount.innerHTML = '';

      const activities = store.get('activities.list') || [];
      const devices = store.get('devices') || [];

      const actSelect = h('select', { className: 'form-select', style: { marginBottom: '12px' } },
        activities.map(a => h('option', { value: a.id, text: a.name }))
      );

      const buttonsList = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '8px' } });

      const BUTTONS = ['Power', 'VolumeUp', 'VolumeDown', 'Mute', 'ChannelUp', 'ChannelDown', 'DirectionUp', 'DirectionDown', 'DirectionLeft', 'DirectionRight', 'Select', 'Back', 'Home', 'Play', 'Pause', 'Stop', 'Rewind', 'FastForward'];

      BUTTONS.forEach(bName => {
        const row = h('div', {
          className: 'card',
          style: { display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '8px 12px', margin: 0 },
        }, [
          h('span', { style: { fontWeight: '600', fontSize: '13px' }, text: bName }),
          h('span', { className: 'badge badge-accent', text: 'Default' }),
        ]);
        buttonsList.appendChild(row);
      });

      mount.appendChild(actSelect);
      mount.appendChild(buttonsList);
    } catch (e) {
      mount.textContent = `Error loading mapping: ${e.message}`;
    }
  }

  container.appendChild(headerCard);
  container.appendChild(pairingCard);
  container.appendChild(otaCard);
  container.appendChild(mappingCard);

  loadMapping();
  return container;
}

export default createView;
