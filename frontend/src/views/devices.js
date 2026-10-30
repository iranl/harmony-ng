// Devices Tab View for Harmony Remote v3
import { h } from '../utils/dom.js';
import { store } from '../store.js';
import { api } from '../api/index.js';
import { t } from '../i18n/index.js';
import { toast } from '../components/toast.js';
import { renderDeviceIcon } from '../icons/render.js';
import { RemoteEngine } from '../remote/engine.js';
import { createBtInputView } from '../remote/bt-input.js';

export function createDevicesView(options = {}) {
  const container = h('div', { className: 'devices-view-container', style: { padding: '12px' } });
  const engine = new RemoteEngine(api);

  let selectedDevice = null;
  let activeSubTab = 'remote'; // 'remote' | 'commands' | 'bt'
  let currentLayout = null;

  function renderList() {
    container.innerHTML = '';

    const devices = store.get('devices') || [];
    const header = h('div', { className: 'card-header' }, [
      h('h2', { className: 'card-title', text: t('tab_devices', 'Devices') }),
      h('span', { className: 'badge badge-accent', text: `${devices.length} devices` }),
    ]);

    const devGrid = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '8px' } });

    if (devices.length === 0) {
      devGrid.appendChild(h('div', { className: 'card text-dim', text: 'No devices found on hub.' }));
    } else {
      devices.forEach(dev => {
        const typeBadgeClass = dev.type === 'BT' ? 'badge-accent' : (dev.type === 'MQTT' ? 'badge-warning' : 'badge');
        const iconEl = renderDeviceIcon(dev, 'device-icon');

        const card = h('div', {
          className: 'card',
          style: { cursor: 'pointer', display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '12px' },
          onclick: () => {
            selectedDevice = dev;
            renderDeviceRemote();
          },
        }, [
          h('div', { style: { display: 'flex', alignItems: 'center', gap: '12px' } }, [
            iconEl,
            h('div', {}, [
              h('div', { style: { fontWeight: '700', fontSize: '14px' }, text: dev.name }),
              h('div', { className: 'text-dim', style: { fontSize: '12px' }, text: `${dev.manufacturer || ''} ${dev.model || ''}`.trim() || 'Custom device' }),
            ]),
          ]),
          h('div', { style: { display: 'flex', alignItems: 'center', gap: '8px' } }, [
            h('span', { className: `badge ${typeBadgeClass}`, text: dev.type || 'IR' }),
            h('span', { className: 'text-dim', text: '›', style: { fontSize: '18px', fontWeight: 'bold' } }),
          ]),
        ]);

        devGrid.appendChild(card);
      });
    }

    container.appendChild(header);
    container.appendChild(devGrid);
  }

  async function renderDeviceRemote() {
    container.innerHTML = '';

    const backBtn = h('button', {
      className: 'btn btn-secondary btn-sm',
      text: `‹ ${t('back', 'Back to Devices')}`,
      onclick: () => {
        selectedDevice = null;
        renderList();
      },
    });

    const isBt = selectedDevice.type === 'BT' || selectedDevice.isBt;

    const header = h('div', {
      className: 'card-header',
      style: { display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '12px' },
    }, [
      backBtn,
      h('div', { style: { textAlign: 'right' } }, [
        h('span', { style: { fontWeight: 'bold', fontSize: '14px' }, text: selectedDevice.name }),
        h('span', { className: 'badge badge-accent', style: { marginLeft: '6px' }, text: selectedDevice.type || 'IR' }),
      ]),
    ]);

    // Subtabs
    const subtabs = h('div', { className: 'remote-subtabs' }, [
      h('div', { className: 'subtabs-group' }, [
        h('button', {
          className: `subtab-btn ${activeSubTab === 'remote' ? 'active' : ''}`,
          text: t('tab_remote', 'Remote'),
          onclick: () => {
            activeSubTab = 'remote';
            renderDeviceRemote();
          },
        }),
        h('button', {
          className: `subtab-btn ${activeSubTab === 'commands' ? 'active' : ''}`,
          text: t('tab_commands', 'Commands'),
          onclick: () => {
            activeSubTab = 'commands';
            renderDeviceRemote();
          },
        }),
        isBt ? h('button', {
          className: `subtab-btn ${activeSubTab === 'bt' ? 'active' : ''}`,
          text: 'BT Input',
          onclick: () => {
            activeSubTab = 'bt';
            renderDeviceRemote();
          },
        }) : null,
      ]),
    ]);

    const contentArea = h('div', {});

    if (activeSubTab === 'remote') {
      const commandsMap = store.get('commandsMap') || {};
      currentLayout = await engine.loadLayout('device', selectedDevice, [selectedDevice], commandsMap);

      const gridEl = engine.renderGrid(currentLayout, async (did, cmd) => {
        try {
          await api.sendCommand(did, cmd);
        } catch (e) {
          toast.error(e.message);
        }
      });
      contentArea.appendChild(gridEl);
    } else if (activeSubTab === 'commands') {
      const commands = selectedDevice.commands || [];
      const cmdsGrid = h('div', { className: 'commands-grid' },
        commands.map(c => {
          const cName = String(c.name || c);
          return h('button', {
            className: 'command-item-btn',
            text: cName,
            onclick: async () => {
              engine.triggerHaptic();
              try {
                await api.sendCommand(selectedDevice.id, cName);
              } catch (e) {
                toast.error(e.message);
              }
            },
          });
        })
      );
      contentArea.appendChild(cmdsGrid);
    } else if (activeSubTab === 'bt') {
      const btView = createBtInputView(selectedDevice);
      contentArea.appendChild(btView);
    }

    container.appendChild(header);
    container.appendChild(subtabs);
    container.appendChild(contentArea);
  }

  store.on('devices', () => {
    if (!selectedDevice) renderList();
  });

  renderList();
  return container;
}

export default createDevicesView;
