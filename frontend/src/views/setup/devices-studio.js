// Devices Studio View (IR Learn, Pronto, Command Manager, Device Timing)
import { h } from '../../utils/dom.js';
import { store } from '../../store.js';
import { api } from '../../api/index.js';
import { t } from '../../i18n/index.js';
import { toast } from '../../components/toast.js';
import { confirm } from '../../components/confirm.js';
import { renderDeviceIcon } from '../../icons/render.js';
import { createModal } from '../../components/modal.js';
import { openIconPicker } from '../../components/icon-picker.js';
import { showDeviceInUse } from '../../components/hub-decisions.js';
import { pluginPairingRow } from './plugin-pairing.js';
import { pluginFieldInput, pluginFieldRow, pluginInputValue, irAltBadge } from './plugin-fields.js';

export function createView() {
  const container = h('div', { className: 'setup-subview-devices', style: { display: 'flex', flexDirection: 'column', gap: '16px' } });

  let selectedDevice = null;

  function renderDeviceSelector() {
    container.innerHTML = '';
    const devices = store.get('devices') || [];

    const header = h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('devices_studio', 'Devices Studio') }),
      h('button', {
        className: 'btn btn-primary btn-sm',
        text: `+ ${t('add_device', 'Add Device')}`,
        onclick: () => openAddDeviceModal(),
      }),
    ]);

    const devGrid = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '8px' } });

    if (devices.length === 0) {
      devGrid.appendChild(h('div', { className: 'text-dim', text: t('no_devices_found', 'No devices found.') }));
    } else {
      devices.forEach(dev => {
        const iconEl = renderDeviceIcon(dev, 'device-icon');
        const card = h('div', {
          className: 'card',
          style: { display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '12px', margin: 0, cursor: 'pointer' },
          onclick: () => {
            selectedDevice = dev;
            renderStudio();
          },
        }, [
          h('div', { style: { display: 'flex', alignItems: 'center', gap: '12px' } }, [
            iconEl,
            h('div', {}, [
              h('div', { style: { fontWeight: 'bold' }, text: dev.name }),
              h('div', { className: 'text-dim', style: { fontSize: '12px' }, text: t('device_cmd_count_line', '{manufacturer} {model} ({count} commands)', { manufacturer: dev.manufacturer || '', model: dev.model || '', count: dev.commands?.length || 0 }).trim() }),
            ]),
          ]),
          h('button', { className: 'btn btn-secondary btn-sm', text: t('manage', 'Manage') }),
        ]);
        devGrid.appendChild(card);
      });
    }

    container.appendChild(header);
    container.appendChild(devGrid);
  }

  function renderStudio() {
    container.innerHTML = '';
    const dev = selectedDevice;

    const backBtn = h('button', {
      className: 'btn btn-secondary btn-sm',
      text: `‹ ${t('back_to_devices', 'Back')}`,
      onclick: () => {
        selectedDevice = null;
        renderDeviceSelector();
      },
    });

    const header = h('div', { className: 'card-header' }, [
      backBtn,
      h('h3', { className: 'card-title', text: t('device_studio_title', '{name} Studio', { name: dev.name }) }),
    ]);

    // The icon the remote draws for the device; saving it queues a config push to the remote
    const iconCard = h('div', { className: 'card' }, [
      h('label', { className: 'form-label', text: t('icon', 'Icon') }),
      h('div', { style: { display: 'flex', alignItems: 'center', gap: '10px', marginTop: '6px' } }, [
        renderDeviceIcon(dev, 'device-icon'),
        h('button', {
          className: 'btn btn-secondary btn-sm',
          text: t('change_icon', 'Change Icon...'),
          onclick: () => {
            openIconPicker(dev.icon, async (icon) => {
              try {
                await api.saveDeviceDetails({ id: dev.id, icon });
                await refreshInventory();
              } catch (e) {
                toast.error(e.message);
              }
            }, { kind: 'device' });
          },
        }),
      ]),
    ]);

    // Where the device is listed: the remote's device pages (config push) and this UI's Devices tab
    const visibilityToggle = (key, label) => h('label', { style: { display: 'flex', alignItems: 'center', gap: '8px', cursor: 'pointer', marginTop: '6px' } }, [
      h('input', {
        type: 'checkbox',
        checked: Boolean(dev[key]),
        onchange: async (e) => {
          try {
            await api.saveDeviceDetails({ id: dev.id, [key]: e.target.checked });
            await refreshInventory();
          } catch (err) {
            e.target.checked = !e.target.checked;
            toast.error(err.message);
          }
        },
      }),
      h('span', { style: { fontSize: '13px' }, text: label }),
    ]);
    const visibilityCard = h('div', { className: 'card' }, [
      h('label', { className: 'form-label', text: t('device_visibility', 'Visibility') }),
      visibilityToggle('hideOnRemote', t('hide_on_remote', 'Hide on remote (device lists only; activity buttons and keys still work)')),
      visibilityToggle('hideInDevicesTab', t('hide_in_devices_tab', 'Hide in Devices tab (web UI only; the remote is not changed)')),
    ]);

    const isPlugin = Boolean(dev?.plugin || (dev?.type && (dev.type.startsWith('plugin:') || dev.type === 'plugin')));
    const pluginId = dev?.plugin || (dev?.type && dev.type.startsWith('plugin:') ? dev.type.substring(7) : '');

    // Plugin Configuration Card (for plugin devices)
    let pluginCard = null;
    if (isPlugin && pluginId) {
      let cfgObj = dev.config || {};
      if (typeof cfgObj === 'string') {
        try { cfgObj = JSON.parse(cfgObj); } catch {}
      }

      const pluginInputs = {};
      const fieldsContainer = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '8px', marginTop: '10px' } });

      const savePluginBtn = h('button', {
        className: 'btn btn-primary btn-sm',
        style: { alignSelf: 'flex-start', marginTop: '8px' },
        text: t('plg_save_settings', 'Save Plugin Settings'),
        onclick: async () => {
          try {
            const updatedConfig = {};
            for (const [key, inp] of Object.entries(pluginInputs)) {
              updatedConfig[key] = pluginInputValue(inp);
            }
            savePluginBtn.disabled = true;
            savePluginBtn.textContent = t('plg_saving', 'Saving...');
            await api.saveDeviceDetails({
              id: dev.id,
              deviceId: dev.id,
              type: `plugin:${pluginId}`,
              plugin: pluginId,
              config: updatedConfig,
            });
            toast.success(t('plg_settings_saved', 'Plugin settings saved!'));
            await refreshInventory();
            dev.config = updatedConfig;
          } catch (e) {
            toast.error(e.message);
          } finally {
            savePluginBtn.disabled = false;
            savePluginBtn.textContent = t('plg_save_settings', 'Save Plugin Settings');
          }
        }
      });

      pluginCard = h('div', { className: 'card' }, [
        h('div', { className: 'card-header' }, [
          h('h4', { className: 'card-title', text: t('plg_config_title', '🧩 Plugin Configuration: {id}', { id: pluginId }) }),
        ]),
        fieldsContainer,
        savePluginBtn,
      ]);

      // Fetch plugin manifest definition to render proper config field inputs
      api.getPlugins().then(res => {
        const plugins = res?.plugins || [];
        const plug = plugins.find(p => p.id === pluginId);
        let fields = Array.isArray(plug?.device_definition?.config_fields)
          ? plug.device_definition.config_fields.filter(f => f && f.key) : null;
        if (!fields && cfgObj && typeof cfgObj === 'object') {
          fields = Object.keys(cfgObj).map(k => ({
            key: k,
            label: k.replace(/_/g, ' ').toUpperCase(),
            type: typeof cfgObj[k] === 'number' ? 'number' : (typeof cfgObj[k] === 'boolean' ? 'boolean' : 'string')
          }));
        }
        if (fields && fields.length > 0) {
          fieldsContainer.innerHTML = '';
          fields.forEach(f => {
            const curVal = cfgObj[f.key] !== undefined ? cfgObj[f.key] : (f.default !== undefined ? f.default : '');
            const inp = pluginFieldInput(f, curVal);
            pluginInputs[f.key] = inp;
            fieldsContainer.appendChild(pluginFieldRow(f, inp));
          });
          const pairRow = pluginPairingRow(plug, pluginInputs);
          if (pairRow) fieldsContainer.appendChild(pairRow);
        } else {
          fieldsContainer.replaceChildren(h('span', { className: 'text-dim', style: { fontSize: '12px' }, text: t('plg_no_settings', 'No configurable settings for this plugin.') }));
        }
      }).catch(() => {
        fieldsContainer.replaceChildren(h('span', { className: 'text-dim', style: { fontSize: '12px' }, text: t('plg_definition_failed', 'Could not load plugin definition.') }));
      });
    }

    // IR Capture / Learn Card
    const learnCard = h('div', { className: 'card' }, [
      h('div', { className: 'card-header' }, [
        h('h4', { className: 'card-title', text: t('ir_learn_title', 'IR Capture (Learning Mode)') }),
      ]),
      h('p', { className: 'text-dim', style: { fontSize: '12px', marginBottom: '10px' }, text: t('ir_learn_hint', 'Point your original remote at the Harmony Hub front sensor, 5 to 8 cm away, press Capture, then tap the key once.') }),
      h('button', {
        className: 'btn btn-primary',
        text: `🎯 ${t('start_ir_capture', 'Start IR Capture')}`,
        onclick: async () => {
          try {
            toast.info(t('ir_listening_press_button', 'Listening for IR signal on Hub... press button on remote'));
            const res = await api.captureIr();
            if (res && res.ok) {
              promptSaveCapturedIr(res);
            } else {
              toast.error(res?.error || t('ir_capture_timed_out', 'Capture timed out without signal'));
            }
          } catch (e) {
            toast.error(e.message);
          }
        },
      }),
    ]);

    // Pronto Hex Test / Add
    const prontoInput = h('textarea', {
      className: 'form-textarea',
      placeholder: '0000 006d 0022 0002 ...',
      rows: 2,
    });

    const prontoNameInput = h('input', {
      type: 'text',
      className: 'form-input',
      placeholder: t('command_name_placeholder', 'Command Name (e.g. HDMI 1)'),
    });

    const prontoCard = h('div', { className: 'card' }, [
      h('div', { className: 'card-header' }, [
        h('h4', { className: 'card-title', text: t('pronto_hex', 'Pronto Hex Direct') }),
      ]),
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', text: t('command_name', 'Command Name') }),
        prontoNameInput,
      ]),
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', text: t('pronto_hex_code', 'Pronto Hex Code') }),
        prontoInput,
      ]),
      h('div', { style: { display: 'flex', gap: '8px', marginTop: '8px' } }, [
        h('button', {
          className: 'btn btn-secondary',
          text: `▶ ${t('test_blast', 'Test Blast')}`,
          onclick: async () => {
            const hex = prontoInput.value.trim();
            if (!hex) return toast.warn(t('enter_pronto_hex_code', 'Enter Pronto hex code'));
            try {
              toast.info(t('blasting_pronto', 'Blasting Pronto code...'));
              await api.sendDirectPronto(hex, dev.controlPort || 7);
              toast.success(t('blasted_pronto', 'Blasted Pronto code'));
            } catch (e) {
              toast.error(e.message);
            }
          },
        }),
        h('button', {
          className: 'btn btn-primary',
          text: t('save_command', 'Save to Device'),
          onclick: async () => {
            const name = prontoNameInput.value.trim();
            const hex = prontoInput.value.trim();
            if (!name || !hex) return toast.warn(t('enter_name_and_pronto', 'Enter both name and Pronto hex'));
            try {
              /* /ir/command reads name and raw; the IR encoder plays a raw value starting with "0000 " as Pronto */
              const res = await api.saveLearnedCommand({ deviceId: dev.id, name, raw: hex });
              if (!res || res.ok === false) return toast.error(t('could_not_save_command', 'Could not save command {name}', { name }));
              toast.success(t('command_saved_excl', 'Command {name} saved!', { name }));
              refreshInventory();
            } catch (e) {
              toast.error(e.message);
            }
          },
        }),
      ]),
    ]);

    // Device Commands List & Delete
    const cmds = dev.commands || [];
    const cmdsList = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '6px' } });

    cmds.forEach(c => {
      const cName = String(c.name || c);
      const row = h('div', {
        className: 'card',
        style: { display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '8px 12px', margin: 0 },
      }, [
        h('span', { style: { fontWeight: '600', fontSize: '13px' } }, [cName, irAltBadge(c, dev.config)]),
        h('div', { style: { display: 'flex', gap: '6px' } }, [
          h('button', {
            className: 'btn btn-secondary btn-sm',
            text: '▶',
            onclick: async () => {
              try {
                await api.sendCommand(dev.id, cName);
                toast.info(t('sent_command', 'Sent {name}', { name: cName }));
              } catch (e) {
                toast.error(e.message);
              }
            },
          }),
          h('button', {
            className: 'btn btn-danger btn-sm',
            text: '×',
            onclick: async () => {
              const ok = await confirm.ask({
                title: t('delete_command', 'Delete Command'),
                message: t('delete_command_from_device', 'Delete {command} from {device}?', { command: cName, device: dev.name }),
                danger: true,
              });
              if (ok) {
                try {
                  await api.deleteCommand(dev.id, cName);
                  toast.success(t('command_deleted', 'Command deleted'));
                  refreshInventory();
                } catch (e) {
                  toast.error(e.message);
                }
              }
            },
          }),
        ]),
      ]);
      cmdsList.appendChild(row);
    });

    const cmdsCard = h('div', { className: 'card' }, [
      h('div', { className: 'card-header' }, [
        h('h4', { className: 'card-title', text: `${t('commands', 'Commands')} (${cmds.length})` }),
      ]),
      cmdsList,
    ]);

    // Danger Zone
    const dangerCard = h('div', { className: 'card', style: { borderColor: 'rgba(244, 63, 94, 0.4)' } }, [
      h('div', { className: 'card-header' }, [
        h('h4', { className: 'card-title', style: { color: 'var(--danger)' }, text: t('danger_zone', 'Delete Device') }),
      ]),
      h('p', { className: 'text-dim', style: { fontSize: '12px', marginBottom: '10px' }, text: t('device_delete_in_use_note', 'A device that activities or remote keys still use cannot be deleted: the Hub lists them so you can change them first.') }),
      h('button', {
        className: 'btn btn-danger',
        text: t('delete_device', 'Delete Device Permanently'),
        onclick: async () => {
          const ok = await confirm.ask({
            title: t('delete_device', 'Delete Device'),
            message: t('confirm_delete_device_name', 'Are you sure you want to delete {name}?', { name: dev.name }),
            danger: true,
          });
          if (ok) {
            try {
              await api.deleteDevice(dev.id);
              toast.success(t('device_deleted_toast', 'Device deleted'));
              await refreshInventory();
              selectedDevice = null;
              renderDeviceSelector();
            } catch (e) {
              if (!showDeviceInUse(e)) toast.error(e.message);
            }
          }
        },
      }),
    ]);

    container.appendChild(header);
    container.appendChild(iconCard);
    container.appendChild(visibilityCard);
    if (pluginCard) {
      container.appendChild(pluginCard);
    } else {
      container.appendChild(learnCard);
      container.appendChild(prontoCard);
    }
    container.appendChild(cmdsCard);
    container.appendChild(dangerCard);
  }

  function promptSaveCapturedIr(result) {
    const nameInput = h('input', { type: 'text', className: 'form-input', placeholder: t('ir_name_placeholder', 'e.g. Power, Volume Up') });
    const content = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '8px' } }, [
      h('span', { className: 'badge badge-success', text: t('signal_captured', 'Signal captured successfully!') }),
      h('label', { className: 'form-label', text: t('command_name', 'Command Name') }),
      nameInput,
    ]);

    createModal({
      title: t('save_captured_ir', 'Save Captured IR Command'),
      content,
      actions: [
        { label: t('cancel', 'Cancel') },
        {
          label: t('test_blast', 'Test Blast'),
          onClick: async () => {
            try {
              /* the Hub returns a raw timing string (no pronto); send it through the device's IR ports */
              const res = result.pronto
                ? await api.sendDirectPronto(result.pronto, selectedDevice?.controlPort || 7)
                : await api.testLearnedIr({ deviceId: selectedDevice?.id, keycode: result.keycode || result.raw || '' });
              if (res && res.ok === false) toast.error(res.error || t('ir_transmit_failed', 'IR transmit failed'));
              else toast.info(t('blasted_captured_signal', 'Blasted captured signal'));
            } catch (e) {
              toast.error(e.message);
            }
            return false;
          },
        },
        {
          label: t('save', 'Save'),
          variant: 'primary',
          onClick: async () => {
            const name = nameInput.value.trim();
            if (!name) {
              toast.warn(t('enter_cmd_name', 'Enter command name'));
              return false;
            }
            /* /ir/command reads name and raw (the legacy UI sends the same fields) */
            const res = await api.saveLearnedCommand({
              deviceId: selectedDevice.id,
              name,
              raw: result.keycode || result.raw || '',
            });
            if (!res || res.ok === false) {
              toast.error(t('could_not_save_command', 'Could not save command {name}', { name }));
              return false;
            }
            toast.success(t('saved_command_name', 'Saved command {name}', { name }));
            refreshInventory();
            return true;
          },
        },
      ],
    });
  }

  async function openAddDeviceModal() {
    let plugins = [];
    try {
      const pRes = await api.getPlugins();
      plugins = (pRes?.plugins || []).filter(p => p.device_definition);
    } catch {}

    const nameInput = h('input', { type: 'text', className: 'form-input', placeholder: 'Samsung TV' });  // i18n-ok: example value
    const mfgInput = h('input', { type: 'text', className: 'form-input', placeholder: 'Samsung' });  // i18n-ok: example value
    const modelInput = h('input', { type: 'text', className: 'form-input', placeholder: 'QN90A' });  // i18n-ok: example value

    const typeSelect = h('select', { className: 'form-select' }, [
      h('option', { value: 'IR', text: t('device_type_ir', 'Infrared (IR)') }),
      ...plugins.map(p => h('option', {
        value: `plugin:${p.id}`,
        text: t('device_type_plugin_option', '🧩 {name} (Plugin)', { name: p.device_definition.display_name || p.name })
      }))
    ]);

    const portSelect = h('select', { className: 'form-select' }, [
      h('option', { value: 7, text: t('ir_port_all', 'All IR Ports (7)') }),
      h('option', { value: 1, text: t('ir_port_internal', 'Port 1 (Internal)') }),
      h('option', { value: 2, text: t('ir_port_blaster1', 'Port 2 (Blaster 1)') }),
      h('option', { value: 3, text: t('ir_port_blaster2', 'Port 3 (Blaster 2)') }),
    ]);

    const portGroup = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '4px' } }, [
      h('label', { className: 'form-label', text: t('ir_control_port', 'IR Control Port') }),
      portSelect,
    ]);

    const dynamicConfigContainer = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } });
    const dynamicFieldInputs = {};

    function updateTypeVisibility() {
      const selectedType = typeSelect.value;
      if (selectedType === 'IR') {
        portGroup.style.display = 'flex';
        dynamicConfigContainer.style.display = 'none';
        dynamicConfigContainer.innerHTML = '';
      } else {
        portGroup.style.display = 'none';
        dynamicConfigContainer.style.display = 'flex';
        dynamicConfigContainer.innerHTML = '';
        const plugId = selectedType.replace('plugin:', '');
        const plug = plugins.find(p => p.id === plugId);
        if (plug?.device_definition) {
          mfgInput.value = plug.device_definition.display_name || plug.name;
          modelInput.value = 'Plugin';
          if (Array.isArray(plug.device_definition.config_fields)) {
            plug.device_definition.config_fields.filter(f => f && f.key).forEach(f => {
              const inp = pluginFieldInput(f, f.default, f.placeholder || (f.default !== undefined ? String(f.default) : ''));
              dynamicFieldInputs[f.key] = inp;
              dynamicConfigContainer.appendChild(pluginFieldRow(f, inp));
            });
          }
          const pairRow = pluginPairingRow(plug, dynamicFieldInputs);
          if (pairRow) dynamicConfigContainer.appendChild(pairRow);
        }
      }
    }

    typeSelect.onchange = updateTypeVisibility;

    const content = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } }, [
      h('label', { className: 'form-label', text: t('device_type', 'Device Type') }),
      typeSelect,
      h('label', { className: 'form-label', text: t('device_name', 'Device Name') }),
      nameInput,
      h('label', { className: 'form-label', text: t('manufacturer', 'Manufacturer') }),
      mfgInput,
      h('label', { className: 'form-label', text: t('model', 'Model') }),
      modelInput,
      portGroup,
      dynamicConfigContainer,
    ]);

    updateTypeVisibility();

    createModal({
      title: t('add_device', 'Add New Device'),
      content,
      actions: [
        { label: t('cancel', 'Cancel') },
        {
          label: t('create', 'Create'),
          variant: 'primary',
          onClick: async () => {
            const name = nameInput.value.trim();
            if (!name) {
              toast.warn(t('device_name_required', 'Device name is required'));
              return false;
            }
            try {
              const selectedType = typeSelect.value;
              if (selectedType === 'IR') {
                await api.saveNewDevice({
                  name,
                  manufacturer: mfgInput.value.trim(),
                  model: modelInput.value.trim(),
                  controlPort: Number(portSelect.value) || 7,
                  type: 'IR',
                });
              } else {
                const plugId = selectedType.replace('plugin:', '');
                const plug = plugins.find(p => p.id === plugId);
                const configVals = {};
                if (Array.isArray(plug?.device_definition?.config_fields)) {
                  plug.device_definition.config_fields.filter(f => f && f.key).forEach(f => {
                    const inp = dynamicFieldInputs[f.key];
                    if (inp) {
                      configVals[f.key] = pluginInputValue(inp);
                    }
                  });
                }
                const defaultCmds = plug?.device_definition?.default_commands || [];
                await api.saveNewDevice({
                  name,
                  manufacturer: mfgInput.value.trim(),
                  model: modelInput.value.trim(),
                  type: selectedType,
                  plugin: plugId,
                  config: configVals,
                  commands: defaultCmds,
                });
              }
              toast.success(t('device_added_excl', 'Device {name} added!', { name }));
              await refreshInventory();
              renderDeviceSelector();
              return true;
            } catch (e) {
              toast.error(e.message);
              return false;
            }
          },
        },
      ],
    });
  }

  async function refreshInventory() {
    try {
      const inv = await api.getInventory();
      store.set('devices', inv.devices || []);
      if (selectedDevice) {
        selectedDevice = inv.devices?.find(d => String(d.id) === String(selectedDevice.id)) || selectedDevice;
        renderStudio();
      }
    } catch {}
  }

  renderDeviceSelector();
  return container;
}

export default createView;
