// Primary Remote Control View for Harmony Remote v3
import { h } from '../utils/dom.js';
import { store } from '../store.js';
import { api } from '../api/index.js';
import { t } from '../i18n/index.js';
import { toast } from '../components/toast.js';
import { RemoteEngine } from '../remote/engine.js';
import { createBtInputView } from '../remote/bt-input.js';
import { renderActivityIcon } from '../icons/render.js';

export function createRemoteView(options = {}) {
  const container = h('div', { className: 'remote-view-container' });
  const engine = new RemoteEngine(api);

  let activeSubTab = 'remote'; // 'remote' | 'commands' | 'keypad' | 'bt'
  let currentLayout = null;
  let cmdFilter = '';

  // Render Transition Banner if activity in progress
  const transitionBannerMount = h('div');

  function updateTransitionBanner() {
    transitionBannerMount.innerHTML = '';
    const isTrans = store.get('activities.transitioning');
    if (isTrans) {
      const banner = h('div', { className: 'transition-banner' }, [
        h('span', { text: t('activity_switching', 'Switching activity...') }),
        h('button', {
          className: 'btn btn-sm btn-danger',
          text: t('cancel', 'Cancel'),
          onclick: async () => {
            try {
              await api.cancelActivity();
              toast.info(t('activity_cancelled', 'Activity transition cancelled'));
            } catch (e) {
              toast.error(e.message);
            }
          },
        }),
      ]);
      transitionBannerMount.appendChild(banner);
    }
  }

  // Activity Selector Bar
  const activityBar = h('div', { className: 'activity-selector-bar' });

  function renderActivityBar() {
    activityBar.innerHTML = '';
    const activities = store.get('activities.list') || [];
    const currentId = store.get('activities.currentId') || '-1';

    // PowerOff Chip
    const powerOffActive = currentId === '-1';
    const powerOffChip = h('button', {
      className: `activity-chip poweroff-chip ${powerOffActive ? 'active' : ''}`,
      onclick: async () => {
        engine.triggerHaptic();
        try {
          await api.stopActivity();
        } catch (e) {
          toast.error(e.message);
        }
      },
    }, [
      h('span', { text: '⏻' }),
      h('span', { text: t('power_off', 'Power Off') }),
    ]);
    activityBar.appendChild(powerOffChip);

    activities.forEach(act => {
      const isActive = String(act.id) === String(currentId);
      const iconEl = renderActivityIcon(act, 'activity-icon');
      const chip = h('button', {
        className: `activity-chip ${isActive ? 'active' : ''}`,
        onclick: async () => {
          engine.triggerHaptic();
          try {
            await api.startActivity(act.id);
          } catch (e) {
            toast.error(e.message);
          }
        },
      }, [
        iconEl,
        h('span', { text: act.name }),
      ]);
      activityBar.appendChild(chip);
    });
  }

  // Sub-tabs bar
  const subtabsBar = h('div', { className: 'remote-subtabs' });

  function renderSubtabsBar() {
    subtabsBar.innerHTML = '';

    const currentId = store.get('activities.currentId') || '-1';
    const activities = store.get('activities.list') || [];
    const currentAct = activities.find(a => String(a.id) === String(currentId));
    const devices = store.get('devices') || [];
    const scopeDevices = currentAct
      ? devices.filter(d => (currentAct.deviceIds || []).includes(String(d.id)))
      : devices;

    const hasBt = scopeDevices.some(d => d.type === 'BT' || d.isBt);

    const tabsGroup = h('div', { className: 'subtabs-group' }, [
      h('button', {
        className: `subtab-btn ${activeSubTab === 'remote' ? 'active' : ''}`,
        text: t('tab_remote', 'Remote'),
        onclick: () => switchSubTab('remote'),
      }),
      h('button', {
        className: `subtab-btn ${activeSubTab === 'commands' ? 'active' : ''}`,
        text: t('tab_commands', 'Commands'),
        onclick: () => switchSubTab('commands'),
      }),
      h('button', {
        className: `subtab-btn ${activeSubTab === 'keypad' ? 'active' : ''}`,
        text: t('tab_keypad', 'Keypad'),
        onclick: () => switchSubTab('keypad'),
      }),
      hasBt ? h('button', {
        className: `subtab-btn ${activeSubTab === 'bt' ? 'active' : ''}`,
        text: 'BT Input',
        onclick: () => switchSubTab('bt'),
      }) : null,
    ]);

    const editBtn = h('button', {
      className: `btn btn-sm ${engine.editMode ? 'btn-primary' : 'btn-secondary'}`,
      text: engine.editMode ? t('done', 'Done') : t('edit_grid', 'Edit Grid'),
      onclick: () => {
        engine.editMode = !engine.editMode;
        renderSubtabsBar();
        renderMainArea();
      },
    });

    subtabsBar.appendChild(tabsGroup);
    subtabsBar.appendChild(editBtn);
  }

  // Content Area
  const mainArea = h('div', { className: 'remote-main-area' });

  async function loadAndRenderGrid() {
    mainArea.innerHTML = '';
    const currentId = store.get('activities.currentId') || '-1';
    const activities = store.get('activities.list') || [];
    const currentAct = activities.find(a => String(a.id) === String(currentId));
    const devices = store.get('devices') || [];
    const commandsMap = store.get('commandsMap') || {};

    const target = currentAct || { id: '-1', name: 'PowerOff' };
    const availableDevices = currentAct
      ? devices.filter(d => (currentAct.deviceIds || []).includes(String(d.id)))
      : devices;

    currentLayout = await engine.loadLayout('activity', target, availableDevices, commandsMap);

    const gridEl = engine.renderGrid(
      currentLayout,
      async (deviceId, command) => {
        try {
          await api.sendCommand(deviceId, command);
        } catch (e) {
          toast.error(e.message);
        }
      },
      (slotIdx, btn) => {
        promptEditSlot(slotIdx, btn, availableDevices);
      }
    );

    mainArea.appendChild(gridEl);
  }

  function promptEditSlot(slotIdx, btn, availableDevices) {
    const commandsMap = store.get('commandsMap') || {};
    // Let user select device and command
    const devSelect = h('select', { className: 'form-select' });
    availableDevices.forEach(d => {
      const opt = h('option', { value: d.id, text: d.name });
      if (btn && String(btn.deviceId) === String(d.id)) opt.selected = true;
      devSelect.appendChild(opt);
    });

    const cmdSelect = h('select', { className: 'form-select' });
    const updateCmds = () => {
      cmdSelect.innerHTML = '';
      const did = devSelect.value;
      const cmds = commandsMap[did] || [];
      cmds.forEach(c => {
        const cName = String(c.name || c);
        const opt = h('option', { value: cName, text: cName });
        if (btn && btn.command === cName) opt.selected = true;
        cmdSelect.appendChild(opt);
      });
    };
    devSelect.onchange = updateCmds;
    updateCmds();

    const labelInput = h('input', {
      type: 'text',
      className: 'form-input',
      value: btn ? (btn.label || btn.command) : '',
      placeholder: 'Button label',
    });

    const modalBody = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } }, [
      h('label', { className: 'form-label', text: 'Device' }),
      devSelect,
      h('label', { className: 'form-label', text: 'Command' }),
      cmdSelect,
      h('label', { className: 'form-label', text: 'Label' }),
      labelInput,
    ]);

    const modalBackdrop = h('div', { className: 'modal-backdrop modal-visible' }, [
      h('div', { className: 'modal-dialog' }, [
        h('div', { className: 'modal-header' }, [
          h('h3', { className: 'modal-title', text: t('edit_slot', 'Edit Slot') }),
          h('button', { className: 'modal-close', text: '×', onclick: () => modalBackdrop.remove() }),
        ]),
        h('div', { className: 'modal-body' }, [modalBody]),
        h('div', { className: 'modal-actions' }, [
          btn ? h('button', {
            className: 'btn btn-danger',
            text: t('clear', 'Clear'),
            onclick: async () => {
              currentLayout.grid[slotIdx] = null;
              await engine.saveRemoteLayout('activity', currentLayout.targetId, currentLayout);
              modalBackdrop.remove();
              renderMainArea();
            },
          }) : null,
          h('button', {
            className: 'btn btn-primary',
            text: t('save', 'Save'),
            onclick: async () => {
              currentLayout.grid[slotIdx] = {
                id: `btn_${Math.floor(slotIdx / 5)}_${slotIdx % 5}`,
                deviceId: devSelect.value,
                command: cmdSelect.value,
                label: labelInput.value || cmdSelect.value,
              };
              await engine.saveRemoteLayout('activity', currentLayout.targetId, currentLayout);
              modalBackdrop.remove();
              renderMainArea();
            },
          }),
        ]),
      ]),
    ]);

    document.body.appendChild(modalBackdrop);
  }

  function renderCommandsTab() {
    mainArea.innerHTML = '';
    const currentId = store.get('activities.currentId') || '-1';
    const activities = store.get('activities.list') || [];
    const currentAct = activities.find(a => String(a.id) === String(currentId));
    const devices = store.get('devices') || [];
    const commandsMap = store.get('commandsMap') || {};

    const scopeDevices = currentAct
      ? devices.filter(d => (currentAct.deviceIds || []).includes(String(d.id)))
      : devices;

    const searchInput = h('input', {
      type: 'text',
      className: 'form-input',
      placeholder: t('search_commands', 'Search commands...'),
      value: cmdFilter,
      oninput: (e) => {
        cmdFilter = e.target.value.toLowerCase();
        renderFilteredCommands();
      },
    });

    const listContainer = h('div', { className: 'commands-list-container' });

    function renderFilteredCommands() {
      listContainer.innerHTML = '';
      scopeDevices.forEach(dev => {
        const cmds = commandsMap[dev.id] || [];
        const filtered = cmds.filter(c => {
          const name = String(c.name || c).toLowerCase();
          return !cmdFilter || name.includes(cmdFilter);
        });

        if (filtered.length === 0) return;

        const devCard = h('div', { className: 'card' }, [
          h('div', { className: 'card-title', text: dev.name, style: { marginBottom: '8px' } }),
          h('div', { className: 'commands-grid' },
            filtered.map(c => {
              const cName = String(c.name || c);
              return h('button', {
                className: 'command-item-btn',
                text: cName,
                onclick: async () => {
                  engine.triggerHaptic();
                  try {
                    await api.sendCommand(dev.id, cName);
                  } catch (e) {
                    toast.error(e.message);
                  }
                },
              });
            })
          ),
        ]);
        listContainer.appendChild(devCard);
      });
    }

    mainArea.appendChild(searchInput);
    mainArea.appendChild(listContainer);
    renderFilteredCommands();
  }

  function renderKeypadTab() {
    mainArea.innerHTML = '';
    const devices = store.get('devices') || [];
    const currentId = store.get('activities.currentId') || '-1';
    const activities = store.get('activities.list') || [];
    const currentAct = activities.find(a => String(a.id) === String(currentId));

    const scopeDevices = currentAct
      ? devices.filter(d => (currentAct.deviceIds || []).includes(String(d.id)))
      : devices;

    const numpadButtons = ['1', '2', '3', '4', '5', '6', '7', '8', '9', 'Clear', '0', 'Enter'];

    const keypadGrid = h('div', { className: 'keypad-grid' },
      numpadButtons.map(btnText => {
        return h('button', {
          className: 'keypad-btn',
          text: btnText,
          onclick: async () => {
            engine.triggerHaptic();
            // Send to best device
            const pat = btnText.toLowerCase();
            for (const d of scopeDevices) {
              try {
                await api.sendCommand(d.id, pat);
                break;
              } catch {}
            }
          },
        });
      })
    );

    mainArea.appendChild(keypadGrid);
  }

  function renderBtTab() {
    mainArea.innerHTML = '';
    const devices = store.get('devices') || [];
    const btDev = devices.find(d => d.type === 'BT' || d.isBt);
    if (!btDev) {
      mainArea.appendChild(h('div', { className: 'card', text: 'No Bluetooth device connected in this activity.' }));
      return;
    }
    const btView = createBtInputView(btDev);
    mainArea.appendChild(btView);
  }

  function switchSubTab(tab) {
    activeSubTab = tab;
    renderSubtabsBar();
    renderMainArea();
  }

  function renderMainArea() {
    if (activeSubTab === 'remote') {
      loadAndRenderGrid();
    } else if (activeSubTab === 'commands') {
      renderCommandsTab();
    } else if (activeSubTab === 'keypad') {
      renderKeypadTab();
    } else if (activeSubTab === 'bt') {
      renderBtTab();
    }
  }

  // Reactive subscriptions
  store.on('activities.list', () => {
    renderActivityBar();
    renderSubtabsBar();
  });
  store.on('activities.currentId', () => {
    renderActivityBar();
    renderSubtabsBar();
    if (activeSubTab === 'remote') loadAndRenderGrid();
  });
  store.on('activities.transitioning', updateTransitionBanner);

  // Initial render
  container.appendChild(transitionBannerMount);
  container.appendChild(activityBar);
  container.appendChild(subtabsBar);
  container.appendChild(mainArea);

  renderActivityBar();
  renderSubtabsBar();
  renderMainArea();

  return container;
}

export default createRemoteView;
