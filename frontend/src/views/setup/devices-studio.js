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

    // IR Capture / Learn Card
    const learnCard = h('div', { className: 'card' }, [
      h('div', { className: 'card-header' }, [
        h('h4', { className: 'card-title', text: t('ir_learn_title', 'IR Capture (Learning Mode)') }),
      ]),
      h('p', { className: 'text-dim', style: { fontSize: '12px', marginBottom: '10px' }, text: t('ir_learn_hint', 'Point your original remote at the Harmony Hub front sensor and press Capture.') }),
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
        h('span', { style: { fontWeight: '600', fontSize: '13px' }, text: cName }),
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
    container.appendChild(learnCard);
    container.appendChild(prontoCard);
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

  function openAddDeviceModal() {
    const nameInput = h('input', { type: 'text', className: 'form-input', placeholder: 'Samsung TV' });  // i18n-ok: example value
    const mfgInput = h('input', { type: 'text', className: 'form-input', placeholder: 'Samsung' });  // i18n-ok: example value
    const modelInput = h('input', { type: 'text', className: 'form-input', placeholder: 'QN90A' });  // i18n-ok: example value
    const portSelect = h('select', { className: 'form-select' }, [
      h('option', { value: 7, text: t('ir_port_all', 'All IR Ports (7)') }),
      h('option', { value: 1, text: t('ir_port_internal', 'Port 1 (Internal)') }),
      h('option', { value: 2, text: t('ir_port_blaster1', 'Port 2 (Blaster 1)') }),
      h('option', { value: 3, text: t('ir_port_blaster2', 'Port 3 (Blaster 2)') }),
    ]);

    const content = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } }, [
      h('label', { className: 'form-label', text: t('device_name', 'Device Name') }),
      nameInput,
      h('label', { className: 'form-label', text: t('manufacturer', 'Manufacturer') }),
      mfgInput,
      h('label', { className: 'form-label', text: t('model', 'Model') }),
      modelInput,
      h('label', { className: 'form-label', text: t('ir_control_port', 'IR Control Port') }),
      portSelect,
    ]);

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
              await api.saveNewDevice({
                name,
                manufacturer: mfgInput.value.trim(),
                model: modelInput.value.trim(),
                controlPort: Number(portSelect.value) || 7,
                type: 'IR',
              });
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
