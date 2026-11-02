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
    linkStatus,
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
      h('div', { style: { display: 'flex', gap: '8px' } }, [
        h('button', {
          className: 'btn btn-secondary btn-sm',
          text: `📋 Copy from Slot ${currentSlot === 1 ? 2 : 1}`,
          onclick: async () => {
            const otherSlot = currentSlot === 1 ? 2 : 1;
            const ok = await confirm.ask({
              title: 'Copy Remote Mappings',
              message: `Copy all button mappings from Slot ${otherSlot} to Slot ${currentSlot}? Any existing mappings on Slot ${currentSlot} will be overwritten.`,
              danger: true,
            });
            if (ok) {
              try {
                await api.copyRFMapping(otherSlot, currentSlot);
                toast.success(`Copied mappings from Slot ${otherSlot} to Slot ${currentSlot}`);
                loadMapping();
              } catch (e) {
                toast.error(e.message);
              }
            }
          },
        }),
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
    ]),
    h('div', { id: 'rf-mapping-content', className: 'text-dim', text: 'Loading mapping...' }),
  ]);

  // Screen preview: the remote's own drawing code (tools/elite_render, built into
  // public/assets/elite_preview.js) draws its screens from the config the Hub pushes to it.
  // Builds without the renderer leave the card hidden.
  const SCREEN_VIEWS = [
    ['activities', t('activities', 'Activities')],
    ['commands', 'Buttons'],
    ['devices', t('devices', 'Devices')],
  ];
  let screenView = 'activities';
  const screenChoice = {}; // the select's value in each view
  let screenDevices = [];
  let screenRenderer = null;
  const screenCanvas = h('canvas', {
    width: 240,
    height: 320,
    style: { width: '100%', maxWidth: '360px', aspectRatio: '3 / 4', imageRendering: 'pixelated', borderRadius: '10px', background: '#000' },
  });
  const screenSelect = h('select', {
    className: 'form-select',
    onchange: () => { screenChoice[screenView] = screenSelect.value; drawScreen(); },
  });
  const screenViewButtons = SCREEN_VIEWS.map(([view, label]) => h('button', {
    className: 'btn btn-secondary btn-sm',
    text: label,
    onclick: () => { screenView = view; drawScreen(true); },
  }));
  const screenCard = h('div', { className: 'card', style: { display: 'none' } }, [
    h('div', { className: 'card-header' }, [
      h('h4', { className: 'card-title', text: 'Remote Screen' }),
      h('div', { style: { display: 'flex', gap: '6px' } }, screenViewButtons),
    ]),
    h('p', { className: 'text-dim', style: { fontSize: '12px', marginBottom: '12px' }, text: 'Drawn by the remote\'s firmware from the configuration the Hub pushes to it.' }),
    h('div', { style: { display: 'flex', flexDirection: 'column', alignItems: 'center', gap: '12px' } }, [screenSelect, screenCanvas]),
  ]);

  // The Hub's push lists devices from devices.json (/api/devices), whose order and commands
  // differ from the inventory
  async function loadScreenDevices() {
    screenDevices = (await api.request('/api/devices').catch(() => null))?.devices || [];
  }

  async function drawScreen(rebuildSelect = false) {
    if (!screenRenderer) {
      screenRenderer = (async () => {
        const preview = await import(/* @vite-ignore */ new URL(`assets/elite_preview.js?v=${__ELITE_RENDERER_VERSION__}`, document.baseURI).href);
        return { preview, module: await preview.loadHubRenderer(api.baseUrl) };
      })().catch((e) => {
        console.warn('Elite preview: no renderer in this build', e);
        return null;
      });
    }
    const renderer = await screenRenderer;
    if (!renderer) return;
    const { preview, module } = renderer;

    const activities = (store.get('activities.list') || []).filter(a => String(a.id) !== '-1');
    const devices = screenDevices.slice(0, 16); // the push sends the first 16
    const choices = screenView === 'commands'
      ? activities.map(a => [a.id, a.name])
      : screenView === 'devices' ? [['', 'All devices'], ...devices.map(d => [String(d.id), d.name])] : [];
    if (rebuildSelect) {
      screenSelect.replaceChildren(...choices.map(([value, text]) => h('option', { value, text })));
      const want = screenChoice[screenView] ?? (screenView === 'commands' ? String(store.get('activities.currentId')) : '');
      if (choices.some(([v]) => v === want)) screenSelect.value = want;
    }
    screenSelect.style.display = choices.length ? '' : 'none';
    screenViewButtons.forEach((b, i) => {
      b.className = `btn btn-sm ${SCREEN_VIEWS[i][0] === screenView ? 'btn-primary' : 'btn-secondary'}`;
    });

    preview.writeRemoteConfig(module, { activities, devices, eliteMap: activeMapping || {} });
    const running = String(store.get('activities.currentId') || '-1');
    const state = { view: screenView, select: 0, active: running === '-1' ? '' : preview.remoteActivityId(running) };
    if (screenView === 'commands' && screenSelect.value) {
      state.active = preview.remoteActivityId(screenSelect.value);
    } else if (screenView === 'devices' && screenSelect.value) {
      state.view = 'device';
      state.select = devices.findIndex(d => String(d.id) === screenSelect.value);
    }
    const now = new Date();
    preview.render(module, screenCanvas, { ...state, battery: 100, minutes: now.getHours() * 60 + now.getMinutes() });
    screenCard.style.display = '';
  }

  // Redraw when the Hub's activities or devices change; views aren't unmounted, so each
  // listener removes itself once this page has gone
  const redrawOn = (path, reload) => {
    const off = store.on(path, async () => {
      if (!container.isConnected) { off(); return; }
      if (reload) await reload();
      drawScreen(true);
    });
  };
  redrawOn('activities.list');
  redrawOn('activities.currentId');
  redrawOn('devices', loadScreenDevices);

  async function loadMapping() {
    loadLinkStatus();
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
    drawScreen();
  }

  container.appendChild(headerCard);
  container.appendChild(screenCard);
  container.appendChild(pairingCard);
  container.appendChild(otaCard);
  container.appendChild(mappingCard);

  loadMapping();
  loadScreenDevices().then(() => drawScreen(true));
  return container;
}

export default createView;
