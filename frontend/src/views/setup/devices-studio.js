// Devices Studio View (IR Learn, Pronto, BLE Broadcast, Mantra Fans, Command Manager, Device Timing)
import { h } from '../../utils/dom.js';
import { store } from '../../store.js';
import { api } from '../../api/index.js';
import { t } from '../../i18n/index.js';
import { toast } from '../../components/toast.js';
import { confirm } from '../../components/confirm.js';
import { renderDeviceIcon } from '../../icons/render.js';
import { createModal } from '../../components/modal.js';
import { openIconPicker } from '../../components/icon-picker.js';
import { BLE_ADV_PREFIX, toAdvHex } from '../../utils/ble-adv.js';
import { mantraActionsFor, mantraDefaultCommands, mantraKeyCode, normalizeMantraId } from '../../utils/mantra.js';

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

    // BLE Broadcast Add: a captured advertisement the Hub rebroadcasts when the command runs
    const bleNameInput = h('input', {
      type: 'text',
      className: 'form-input',
      placeholder: 'Command Name (e.g. LightOn)',
    });

    const blePayloadInput = h('textarea', {
      className: 'form-textarea',
      placeholder: 'uuids=AABBCCDD,11223344,... or 0201060303AAFE...',
      rows: 2,
    });

    const bleCard = h('div', { className: 'card' }, [
      h('div', { className: 'card-header' }, [
        h('h4', { className: 'card-title', text: t('ble_broadcast_title', 'BLE Broadcast Command') }),
      ]),
      h('p', { className: 'text-dim', style: { fontSize: '12px', marginBottom: '10px' }, text: t('ble_broadcast_desc', 'Paste a captured advertisement: a uuids= line from tools/ble_capture/ble_scan, or raw advertising data as hex.') }),
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', text: 'Command Name' }),
        bleNameInput,
      ]),
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', text: t('ble_payload', 'Advertisement') }),
        blePayloadInput,
      ]),
      h('div', { style: { display: 'flex', gap: '8px', marginTop: '8px' } }, [
        h('button', {
          className: 'btn btn-primary',
          text: t('save_command', 'Save to Device'),
          onclick: async () => {
            const name = bleNameInput.value.trim();
            if (!name) return toast.warn('Enter command name');
            const hex = toAdvHex(blePayloadInput.value);
            if (!hex) return toast.warn(t('ble_payload_invalid', 'Not a valid advertisement (at most 31 bytes of AD structures)'));
            try {
              await api.saveCommand({ deviceId: dev.id, name, raw: BLE_ADV_PREFIX + hex });
              toast.success(`Command ${name} saved!`);
              refreshInventory();
            } catch (e) {
              toast.error(e.message);
            }
          },
        }),
      ]),
    ]);

    // Mantra fan: the id the fan obeys, set by pairing the fan with the Hub or learned from the mAntra app
    const mantraIdInput = h('input', { type: 'text', className: 'form-input', value: dev.mantraId || '', placeholder: 'e.g. 1A2B', maxLength: 6 });
    const mantraVersionSelect = h('select', { className: 'form-select' }, [
      h('option', { value: 0, text: t('mantra_v0', 'Fan and light (most models)') }),
      h('option', { value: 1, text: t('mantra_v1', 'Light only (v1)') }),
    ]);
    mantraVersionSelect.value = String(dev.mantraVersion || 0);
    const learnResult = h('div', { style: { display: 'flex', flexWrap: 'wrap', gap: '6px', marginTop: '8px' } });

    async function saveMantraSettings() {
      const id = normalizeMantraId(mantraIdInput.value);
      if (!id) {
        toast.warn(t('mantra_id_invalid', 'Fan id must be 1-4 hex digits'));
        return false;
      }
      await api.saveDeviceDetails({ id: dev.id, mantraId: id, mantraVersion: Number(mantraVersionSelect.value) || 0 });
      mantraIdInput.value = id;
      return true;
    }

    // Saves a fan id the Hub found, from pairing or listening
    async function adoptFanId(id, version) {
      mantraIdInput.value = id;
      mantraVersionSelect.value = String(version || 0);
      if (await saveMantraSettings()) {
        toast.success(`Fan id ${id} saved`);
        refreshInventory();
      }
    }

    const pairResult = h('div', { className: 'text-dim', style: { fontSize: '12px', marginTop: '8px' } });

    async function pairFan(btn) {
      btn.disabled = true;
      pairResult.textContent = t('mantra_pairing', 'Searching for the fan…');
      try {
        const res = await api.mantraPair(30);
        if (res?.ok === false) throw new Error(res.error || 'Pairing failed');
        const deadline = Date.now() + 36000;
        let st = null;
        while (Date.now() < deadline) {
          await new Promise(r => setTimeout(r, 1500));
          st = await api.mantraLearnStatus();
          if (st?.paired || st?.pairing === false) break;
        }
        if (st?.paired) {
          pairResult.textContent = t('mantra_paired', 'The fan answered; the light should blink 3 times.');
          await adoptFanId(st.paired, st.pairedVersion);
        } else {
          await api.mantraLearnStop().catch(() => {});
          pairResult.textContent = t('mantra_pair_none', 'No fan answered. Switch the fan off for 5 seconds and on again, then press Pair within 30 seconds.');
        }
      } catch (e) {
        pairResult.textContent = '';
        toast.error(e.message);
      } finally {
        btn.disabled = false;
      }
    }

    async function listenForApp(btn) {
      btn.disabled = true;
      learnResult.innerHTML = '';
      learnResult.appendChild(h('span', { className: 'text-dim', text: t('mantra_listening', 'Listening… use any control in the mAntra app near the Hub.') }));
      try {
        await api.mantraLearnStart(30);
        const deadline = Date.now() + 32000;
        let found = [];
        while (Date.now() < deadline) {
          await new Promise(r => setTimeout(r, 1500));
          const st = await api.mantraLearnStatus();
          found = st?.found || [];
          if (found.length || st?.listening === false) break;
        }
        await api.mantraLearnStop().catch(() => {});
        learnResult.innerHTML = '';
        if (!found.length) {
          learnResult.appendChild(h('span', { className: 'text-dim', text: t('mantra_none_heard', 'No Mantra app broadcast heard. Move the phone closer to the Hub and try again.') }));
        }
        found.forEach(f => {
          learnResult.appendChild(h('button', {
            className: 'btn btn-secondary btn-sm',
            text: `${t('mantra_use_id', 'Use')} ${f.id} (${f.rssi} dBm)`,
            onclick: async () => {
              try {
                await adoptFanId(f.id, f.version);
              } catch (e) {
                toast.error(e.message);
              }
            },
          }));
        });
      } catch (e) {
        learnResult.innerHTML = '';
        toast.error(e.message);
      } finally {
        btn.disabled = false;
      }
    }

    const mantraCard = h('div', { className: 'card' }, [
      h('div', { className: 'card-header' }, [
        h('h4', { className: 'card-title', text: t('mantra_fan', 'Mantra Fan') }),
      ]),
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', text: t('mantra_id', 'Fan Id') }),
        mantraIdInput,
      ]),
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', text: t('mantra_variant', 'Mantra Model') }),
        mantraVersionSelect,
      ]),
      h('button', {
        className: 'btn btn-secondary btn-sm',
        text: t('save', 'Save'),
        onclick: async () => {
          try {
            if (await saveMantraSettings()) {
              toast.success('Fan settings saved');
              refreshInventory();
            }
          } catch (e) {
            toast.error(e.message);
          }
        },
      }),
      h('h5', { style: { margin: '16px 0 4px' }, text: t('mantra_pair_title', 'Pair the fan with the Hub') }),
      h('p', { className: 'text-dim', style: { fontSize: '12px', margin: '0 0 8px' }, text: t('mantra_pair_desc', 'Switch the fan off at the wall for 5 seconds and back on, then press Pair within 30 seconds. The fan answers with its id, which is saved above, and the light blinks 3 times. The mAntra app keeps working.') }),
      h('button', {
        className: 'btn btn-primary btn-sm',
        text: t('mantra_pair', 'Pair'),
        onclick: (e) => pairFan(e.currentTarget),
      }),
      pairResult,
      h('h5', { style: { margin: '16px 0 4px' }, text: t('mantra_learn_title', 'Or learn the id from the mAntra app') }),
      h('p', { className: 'text-dim', style: { fontSize: '12px', margin: '0 0 8px' }, text: t('mantra_learn_desc', 'Press Listen, then use any control in the mAntra lighting app near the Hub.') }),
      h('button', {
        className: 'btn btn-secondary btn-sm',
        text: t('mantra_listen', 'Listen'),
        onclick: (e) => listenForApp(e.currentTarget),
      }),
      learnResult,
    ]);

    // Mantra command builder: the Hub encodes the action with the fan id when the command runs
    const mantraNameInput = h('input', { type: 'text', className: 'form-input', placeholder: 'Command Name (e.g. FanSpeed20)' });
    const mantraActionSelect = h('select', { className: 'form-select' },
      mantraActionsFor(dev.mantraVersion).map(a => h('option', { value: a.action, text: a.label })));
    const mantraValueInput = h('input', { type: 'number', className: 'form-input' });
    const mantraValueGroup = h('div', { className: 'form-group' }, [
      h('label', { className: 'form-label', text: t('value', 'Value') }),
      mantraValueInput,
    ]);
    const syncValueField = () => {
      const def = mantraActionsFor(dev.mantraVersion).find(a => a.action === mantraActionSelect.value);
      mantraValueGroup.style.display = def?.value ? 'block' : 'none';
      if (def?.value) {
        mantraValueInput.min = def.value[0];
        mantraValueInput.max = def.value[1];
        mantraValueInput.placeholder = def.value[2];
      }
    };
    mantraActionSelect.onchange = syncValueField;
    syncValueField();

    const mantraCmdCard = h('div', { className: 'card' }, [
      h('div', { className: 'card-header' }, [
        h('h4', { className: 'card-title', text: t('mantra_add_command', 'Add Fan Command') }),
      ]),
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', text: 'Command Name' }),
        mantraNameInput,
      ]),
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', text: t('action', 'Action') }),
        mantraActionSelect,
      ]),
      mantraValueGroup,
      h('button', {
        className: 'btn btn-primary',
        text: t('save_command', 'Save to Device'),
        onclick: async () => {
          const name = mantraNameInput.value.trim();
          if (!name) return toast.warn('Enter command name');
          const keyCode = mantraKeyCode(mantraActionSelect.value, mantraValueInput.value);
          if (!keyCode) return toast.warn(t('mantra_value_invalid', 'Enter a value in the range shown'));
          try {
            await api.saveCommand({ deviceId: dev.id, name, raw: keyCode });
            toast.success(`Command ${name} saved!`);
            refreshInventory();
          } catch (e) {
            toast.error(e.message);
          }
        },
      }),
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
    container.appendChild(iconCard);
    if (dev.type === 'MANTRA') {
      container.appendChild(mantraCard);
      container.appendChild(mantraCmdCard);
    } else if (dev.type === 'BLE') {
      container.appendChild(bleCard);
    } else {
      container.appendChild(learnCard);
      container.appendChild(prontoCard);
    }
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
    const typeSelect = h('select', { className: 'form-select' }, [
      h('option', { value: 'IR', text: t('device_type_ir', 'Infrared (IR)') }),
      h('option', { value: 'MANTRA', text: t('device_type_mantra', 'Mantra fan or light (Bluetooth)') }),
      h('option', { value: 'BLE', text: t('device_type_ble', 'Other Bluetooth broadcast device (advanced)') }),
    ]);
    const typeHints = {
      IR: t('device_type_ir_hint', 'Controlled by an infrared remote. Learn its codes with the Hub or enter Pronto codes.'),
      MANTRA: t('device_type_mantra_hint', 'For fans and lights that use the mAntra lighting app. The Hub creates the commands and pairs with the fan, so there is nothing to capture.'),
      BLE: t('device_type_ble_hint', "For other devices whose app sends Bluetooth broadcasts. Capture each command from the app with tools/ble_capture on a Mac and paste it in; the Hub replays it."),
    };
    const typeHint = h('p', { className: 'text-dim', style: { fontSize: '12px', margin: 0 }, text: typeHints.IR });
    const versionSelect = h('select', { className: 'form-select' }, [
      h('option', { value: 0, text: t('mantra_v0', 'Fan and light (most models)') }),
      h('option', { value: 1, text: t('mantra_v1', 'Light only (v1)') }),
    ]);

    const portGroup = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } }, [
      h('label', { className: 'form-label', text: 'IR Control Port' }),
      portSelect,
    ]);
    const mantraGroup = h('div', { style: { display: 'none', flexDirection: 'column', gap: '10px' } }, [
      h('label', { className: 'form-label', text: t('mantra_variant', 'Mantra Model') }),
      versionSelect,
      h('p', { className: 'text-dim', style: { fontSize: '12px', margin: 0 }, text: t('mantra_add_hint', 'After creating the device, pair the fan with the Hub to get its id.') }),
    ]);

    typeSelect.onchange = () => {
      typeHint.textContent = typeHints[typeSelect.value];
      portGroup.style.display = typeSelect.value === 'IR' ? 'flex' : 'none';
      mantraGroup.style.display = typeSelect.value === 'MANTRA' ? 'flex' : 'none';
      if (typeSelect.value === 'MANTRA' && !mfgInput.value.trim()) mfgInput.value = 'Mantra';
    };

    const content = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } }, [
      h('label', { className: 'form-label', text: 'Device Name' }),
      nameInput,
      h('label', { className: 'form-label', text: t('device_type', 'Device Type') }),
      typeSelect,
      typeHint,
      h('label', { className: 'form-label', text: 'Manufacturer' }),
      mfgInput,
      h('label', { className: 'form-label', text: 'Model' }),
      modelInput,
      portGroup,
      mantraGroup,
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
            const type = typeSelect.value;
            const version = Number(versionSelect.value) || 0;
            try {
              const res = await api.saveNewDevice({
                name,
                manufacturer: mfgInput.value.trim(),
                model: modelInput.value.trim(),
                controlPort: type === 'IR' ? (Number(portSelect.value) || 7) : 0,
                type,
                // The fan id comes from pairing, on the device's page
                ...(type === 'MANTRA' ? { mantraVersion: version } : {}),
              });
              if (type === 'MANTRA' && res?.deviceId) {
                await api.saveBatchCommands(res.deviceId, mantraDefaultCommands(version));
              }
              toast.success(`Device ${name} added!`);
              await refreshInventory();
              const created = (store.get('devices') || []).find(d => String(d.id) === String(res?.deviceId));
              if (type === 'MANTRA' && created) {
                selectedDevice = created;
                renderStudio();
              } else {
                renderDeviceSelector();
              }
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
