// Devices Studio View (IR Learn, Pronto, Command Manager, Device Timing)
import { h } from '../../utils/dom.js';
import { store } from '../../store.js';
import { api } from '../../api/index.js';
import { t } from '../../i18n/index.js';
import { toast } from '../../components/toast.js';
import { confirm } from '../../components/confirm.js';
import { renderDeviceIcon } from '../../icons/render.js';
import { createModal } from '../../components/modal.js';

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
      devGrid.appendChild(h('div', { className: 'text-dim', text: 'No devices found.' }));
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
              h('div', { className: 'text-dim', style: { fontSize: '12px' }, text: `${dev.manufacturer || ''} ${dev.model || ''} (${dev.commands?.length || 0} commands)` }),
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
      h('h3', { className: 'card-title', text: `${dev.name} Studio` }),
    ]);

    // IR Capture / Learn Card
    const learnCard = h('div', { className: 'card' }, [
      h('div', { className: 'card-header' }, [
        h('h4', { className: 'card-title', text: t('ir_learn_title', 'IR Capture (Learning Mode)') }),
      ]),
      h('p', { className: 'text-dim', style: { fontSize: '12px', marginBottom: '10px' }, text: 'Point your original remote at the Harmony Hub front sensor and press Capture.' }),
      h('button', {
        className: 'btn btn-primary',
        text: '🎯 Start IR Capture',
        onclick: async () => {
          try {
            toast.info('Listening for IR signal on Hub... press button on remote');
            const res = await api.captureIr();
            if (res && res.ok) {
              promptSaveCapturedIr(res);
            } else {
              toast.error(res?.error || 'Capture timed out without signal');
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
      placeholder: 'Command Name (e.g. HDMI 1)',
    });

    const prontoCard = h('div', { className: 'card' }, [
      h('div', { className: 'card-header' }, [
        h('h4', { className: 'card-title', text: t('pronto_hex', 'Pronto Hex Direct') }),
      ]),
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', text: 'Command Name' }),
        prontoNameInput,
      ]),
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', text: 'Pronto Hex Code' }),
        prontoInput,
      ]),
      h('div', { style: { display: 'flex', gap: '8px', marginTop: '8px' } }, [
        h('button', {
          className: 'btn btn-secondary',
          text: '▶ Test Blast',
          onclick: async () => {
            const hex = prontoInput.value.trim();
            if (!hex) return toast.warn('Enter Pronto hex code');
            try {
              toast.info('Blasting Pronto code...');
              await api.sendDirectPronto(hex, dev.controlPort || 7);
              toast.success('Blasted Pronto code');
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
            if (!name || !hex) return toast.warn('Enter both name and Pronto hex');
            try {
              await api.saveLearnedCommand({
                deviceId: dev.id,
                command: name,
                pronto: hex,
                port: dev.controlPort || 7,
              });
              toast.success(`Command ${name} saved!`);
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
                toast.info(`Sent ${cName}`);
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
                title: 'Delete Command',
                message: `Delete ${cName} from ${dev.name}?`,
                danger: true,
              });
              if (ok) {
                try {
                  await api.deleteCommand(dev.id, cName);
                  toast.success('Command deleted');
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
      h('p', { className: 'text-dim', style: { fontSize: '12px', marginBottom: '10px' }, text: 'Removing this device will remove it from all activities and remotes.' }),
      h('button', {
        className: 'btn btn-danger',
        text: t('delete_device', 'Delete Device Permanently'),
        onclick: async () => {
          const ok = await confirm.ask({
            title: t('delete_device', 'Delete Device'),
            message: `Are you sure you want to delete ${dev.name}?`,
            danger: true,
          });
          if (ok) {
            try {
              await api.deleteDevice(dev.id);
              toast.success('Device deleted');
              await refreshInventory();
              selectedDevice = null;
              renderDeviceSelector();
            } catch (e) {
              toast.error(e.message);
            }
          }
        },
      }),
    ]);

    container.appendChild(header);
    container.appendChild(learnCard);
    container.appendChild(prontoCard);
    container.appendChild(cmdsCard);
    container.appendChild(dangerCard);
  }

  function promptSaveCapturedIr(result) {
    const nameInput = h('input', { type: 'text', className: 'form-input', placeholder: 'e.g. Power, Volume Up' });
    const content = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '8px' } }, [
      h('span', { className: 'badge badge-success', text: 'Signal captured successfully!' }),
      h('label', { className: 'form-label', text: 'Command Name' }),
      nameInput,
    ]);

    createModal({
      title: 'Save Captured IR Command',
      content,
      actions: [
        { label: 'Cancel' },
        {
          label: 'Test Blast',
          onClick: async () => {
            if (result.pronto) {
              await api.sendDirectPronto(result.pronto, selectedDevice?.controlPort || 7);
              toast.info('Blasted captured signal');
            }
            return false;
          },
        },
        {
          label: 'Save',
          variant: 'primary',
          onClick: async () => {
            const name = nameInput.value.trim();
            if (!name) {
              toast.warn('Enter command name');
              return false;
            }
            await api.saveLearnedCommand({
              deviceId: selectedDevice.id,
              command: name,
              pronto: result.pronto || '',
              raw: result.raw || '',
              port: selectedDevice.controlPort || 7,
            });
            toast.success(`Saved command ${name}`);
            refreshInventory();
            return true;
          },
        },
      ],
    });
  }

  function openAddDeviceModal() {
    const nameInput = h('input', { type: 'text', className: 'form-input', placeholder: 'Samsung TV' });
    const mfgInput = h('input', { type: 'text', className: 'form-input', placeholder: 'Samsung' });
    const modelInput = h('input', { type: 'text', className: 'form-input', placeholder: 'QN90A' });
    const portSelect = h('select', { className: 'form-select' }, [
      h('option', { value: 7, text: 'All IR Ports (7)' }),
      h('option', { value: 1, text: 'Port 1 (Internal)' }),
      h('option', { value: 2, text: 'Port 2 (Blaster 1)' }),
      h('option', { value: 3, text: 'Port 3 (Blaster 2)' }),
    ]);

    const content = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } }, [
      h('label', { className: 'form-label', text: 'Device Name' }),
      nameInput,
      h('label', { className: 'form-label', text: 'Manufacturer' }),
      mfgInput,
      h('label', { className: 'form-label', text: 'Model' }),
      modelInput,
      h('label', { className: 'form-label', text: 'IR Control Port' }),
      portSelect,
    ]);

    createModal({
      title: t('add_device', 'Add New Device'),
      content,
      actions: [
        { label: 'Cancel' },
        {
          label: 'Create',
          variant: 'primary',
          onClick: async () => {
            const name = nameInput.value.trim();
            if (!name) {
              toast.warn('Device name is required');
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
              toast.success(`Device ${name} added!`);
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
