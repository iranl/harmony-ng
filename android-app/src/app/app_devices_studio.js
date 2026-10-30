import { HarmonyClient } from '../api.js';
import { getSavedHubs, saveHub, deleteHub, getActiveHub, setActiveHubId } from '../storage.js';
import { RemoteEngine } from '../remote-engine.js';
import {
  getHarmonyManufacturers,
  searchHarmonyManufacturers,
  fetchHarmonyModels,
  searchHarmonyModels,
  fetchHarmonyDeviceDetail,
  fetchHarmonyCodeset,
  mapHarmonyDeviceType,
  buildDeviceFromHarmony,
  parseIrText,
  loadIrdSource,
  rankIrdEntries,
  fetchIrdFile,
  sourceLabel,
  safeImportName
} from '../irdb.js';
import { t, setLanguage, getInitialLanguage } from '../i18n.js';
import iconMetadata from '../icon-metadata.json';
import {
  initIconCache,
  hasCachedIcon,
  getCachedIconUrl,
  cacheIconFromUrl,
  syncIconsFromHub,
  syncIconsFromOta,
  getCachedIconsCount,
  clearIconCache,
  getAllCachedKeys
} from '../icon-cache.js';
import {
  APP_VERSION,
  stockIconsAvailable,
  stockIconBaseUrl,
  failedIconUrls,
  setStockIconBaseUrl,
  setStockIconsAvailable,
  escapeHtml,
  autoInvertDarkIcon,
  showToast,
  showConfirm,
  isCustomIcon,
  isDarkMonochromeIcon,
  getStockIconUrl,
  getActivityIconSvg,
  getRawActivityIconSvg,
  getScreenBtnIconSvg,
  getRawScreenBtnIconSvg,
  getDeviceIconSvg,
  getRawDeviceIconSvg
} from './app_icons_ui.js';
import { HarmonyAppActivitiesSim } from './app_activities_sim.js';

export const BT_STANDARD_KEY_PRESETS = [
  {
    category: 'Navigation & Core',
    keys: [
      { name: 'Select', code: 'HID:07:0028', desc: 'Enter / OK' },
      { name: 'DirectionUp', code: 'HID:07:0052', desc: 'Up Arrow' },
      { name: 'DirectionDown', code: 'HID:07:0051', desc: 'Down Arrow' },
      { name: 'DirectionLeft', code: 'HID:07:0050', desc: 'Left Arrow' },
      { name: 'DirectionRight', code: 'HID:07:004F', desc: 'Right Arrow' },
      { name: 'Back', code: 'HID:0C:0224', desc: 'Android / Apple Return' },
      { name: 'Home', code: 'HID:0C:0223', desc: 'AC Home Screen' },
      { name: 'Menu', code: 'HID:0C:0040', desc: 'Context / App Menu' },
      { name: 'Escape', code: 'HID:07:0029', desc: 'Cancel / Exit' },
      { name: 'PowerToggle', code: 'HID:0C:0030', desc: 'Power On / Standby' },
      { name: 'Search', code: 'HID:0C:0221', desc: 'Voice Search / Assistant' },
      { name: 'Tab', code: 'HID:07:002B', desc: 'Tab Key' },
      { name: 'Space', code: 'HID:07:002C', desc: 'Spacebar' },
      { name: 'Backspace', code: 'HID:07:002A', desc: 'Backspace' }
    ]
  },
  {
    category: 'Media & Playback',
    keys: [
      { name: 'Play', code: 'HID:0C:00B0', desc: 'Play' },
      { name: 'Pause', code: 'HID:0C:00B1', desc: 'Pause' },
      { name: 'PlayPause', code: 'HID:0C:00CD', desc: 'Play / Pause Toggle' },
      { name: 'Stop', code: 'HID:0C:00B7', desc: 'Stop Playback' },
      { name: 'NextTrack', code: 'HID:0C:00B5', desc: 'Skip Forward' },
      { name: 'PreviousTrack', code: 'HID:0C:00B6', desc: 'Skip Backward' },
      { name: 'FastForward', code: 'HID:0C:00B3', desc: 'Fast Forward' },
      { name: 'Rewind', code: 'HID:0C:00B4', desc: 'Scan Backward' }
    ]
  },
  {
    category: 'Audio & Volume',
    keys: [
      { name: 'VolumeUp', code: 'HID:0C:00E9', desc: 'Volume +' },
      { name: 'VolumeDown', code: 'HID:0C:00EA', desc: 'Volume -' },
      { name: 'Mute', code: 'HID:0C:00E2', desc: 'Mute Audio' }
    ]
  },
  {
    category: 'TV & Guide',
    keys: [
      { name: 'ChannelUp', code: 'HID:0C:009C', desc: 'Channel Up' },
      { name: 'ChannelDown', code: 'HID:0C:009D', desc: 'Channel Down' },
      { name: 'Info', code: 'HID:0C:008D', desc: 'Program Guide / Info' },
      { name: 'PageUp', code: 'HID:07:004B', desc: 'Page Up' },
      { name: 'PageDown', code: 'HID:07:004E', desc: 'Page Down' },
      { name: 'ContextMenu', code: 'HID:07:0065', desc: 'Application Context Menu' }
    ]
  },
  {
    category: 'Numeric Keypad',
    keys: [
      { name: 'Number0', code: 'HID:07:0027', desc: '0' },
      { name: 'Number1', code: 'HID:07:001E', desc: '1' },
      { name: 'Number2', code: 'HID:07:001F', desc: '2' },
      { name: 'Number3', code: 'HID:07:0020', desc: '3' },
      { name: 'Number4', code: 'HID:07:0021', desc: '4' },
      { name: 'Number5', code: 'HID:07:0022', desc: '5' },
      { name: 'Number6', code: 'HID:07:0023', desc: '6' },
      { name: 'Number7', code: 'HID:07:0024', desc: '7' },
      { name: 'Number8', code: 'HID:07:0025', desc: '8' },
      { name: 'Number9', code: 'HID:07:0026', desc: '9' }
    ]
  }
];

export class HarmonyAppDevicesStudio extends HarmonyAppActivitiesSim {
  renderControlView() {
    const cont = document.getElementById('controlDeviceWorkspace');
    if (!cont) return;

    const devs = this.inventory.devices || [];
    if (!devs.length) {
      cont.innerHTML = '<div class="card"><div class="muted">No devices stored. Go to IR Setup to add one.</div></div>';
      return;
    }

    if (!this.selectedDeviceId || !devs.some(d => String(d.id) === String(this.selectedDeviceId))) {
      this.selectedDeviceId = devs[0].id;
    }

    const curDev = devs.find(d => String(d.id) === String(this.selectedDeviceId));
    this.remoteEngine.render(cont, {
      scope: 'device',
      target: curDev,
      iconSvg: getDeviceIconSvg(curDev.type, curDev.manufacturer, curDev.model, curDev.icon || curDev.Icon || '', 18),
      availableDevices: [curDev],
      selectableDevices: devs,
      onSelectDevice: (devId) => {
        this.selectedDeviceId = devId;
        this.renderControlView();
      },
      commandsMap: this.deviceCommandsMap,
      onSendCommand: async (devId, cmd) => {
        await this.client.sendCommand(devId, cmd);
      }
    });
  }

  /* -------------------------------------------------------------
   * 5. IR Setup View
   * ------------------------------------------------------------- */
  renderIrView(preserveDeviceId = null) {
    const devs = this.inventory.devices || [];
    const targetId = preserveDeviceId || this.selectedIrDeviceId || document.getElementById('manageDeviceSelect')?.value;
    ['wizardDevice', 'verifyDevice', 'irdbDevice', 'manageDeviceSelect', 'harmonyTargetDevSelect'].forEach(id => {
      const el = document.getElementById(id);
      if (el) {
        const curVal = targetId || el.value;
        el.innerHTML = devs.map(d => `<option value="${escapeHtml(d.id)}">${escapeHtml(d.name || d.id)}</option>`).join('');
        if (curVal && devs.some(d => String(d.id) === String(curVal))) {
          el.value = String(curVal);
        }
      }
    });
    this.updateStudioDeviceView();

    if (window.innerWidth <= 768) {
      const activeBtn = document.querySelector('#irStudioMobileNav .segmented-btn.active');
      const tab = activeBtn ? activeBtn.getAttribute('data-ir-tab') : 'cmds';
      const colLeft = document.getElementById('irStudioColLeft');
      const colRight = document.getElementById('irStudioColRight');
      const devProp = document.getElementById('devicePropertiesBox');
      const cmdsWrap = document.getElementById('curDevCmdsWrap');
      if (tab === 'cmds') {
        if (colLeft) colLeft.style.display = 'flex';
        if (colRight) colRight.style.display = 'none';
        if (devProp) devProp.style.display = 'none';
        if (cmdsWrap) cmdsWrap.style.display = 'block';
      } else if (tab === 'add') {
        if (colLeft) colLeft.style.display = 'none';
        if (colRight) colRight.style.display = 'flex';
      } else if (tab === 'settings') {
        if (colLeft) colLeft.style.display = 'flex';
        if (colRight) colRight.style.display = 'none';
        if (devProp) devProp.style.display = 'block';
        if (cmdsWrap) cmdsWrap.style.display = 'none';
      }
    }
  }

  updateStudioDeviceView() {
    const sel = document.getElementById('manageDeviceSelect');
    if (!sel) return;
    const devs = this.inventory.devices || [];
    let dev = null;
    if (this.selectedIrDeviceId && devs.some(d => String(d.id) === String(this.selectedIrDeviceId))) {
      dev = devs.find(d => String(d.id) === String(this.selectedIrDeviceId));
    } else if (sel.value && devs.some(d => String(d.id) === String(sel.value))) {
      dev = devs.find(d => String(d.id) === String(sel.value));
    } else {
      dev = devs[0];
    }
    if (!dev) {
      const nameEl = document.getElementById('curDevName');
      if (nameEl) nameEl.textContent = 'No Devices Found';
      return;
    }
    this.selectedIrDeviceId = String(dev.id);
    this.currentIrDeviceId = String(dev.id);
    if (sel.value !== String(dev.id)) sel.value = String(dev.id);

    // Sync other hidden dropdowns & target selector
    ['wizardDevice', 'verifyDevice', 'irdbDevice', 'harmonyTargetDevSelect'].forEach(id => {
      const el = document.getElementById(id);
      if (el) el.value = String(dev.id);
    });

    const nameEl = document.getElementById('curDevName');
    const detailsEl = document.getElementById('curDevDetails');
    const countEl = document.getElementById('curDevCmdCount');
    const portEl = document.getElementById('curDevPortSelect');
    const mqttTopicInput = document.getElementById('devMqttTopic');
    if (nameEl) nameEl.textContent = dev.name || 'Device ' + dev.id;
    if (detailsEl) detailsEl.textContent = `Type: ${dev.type || 'Appliance'} • Brand: ${dev.manufacturer || 'Unknown'} • Model: ${dev.model || 'Unknown'}`;
    if (countEl) countEl.textContent = (dev.commands || []).length;
    if (portEl) {
      const p = (dev.controlPort !== undefined) ? dev.controlPort : ((dev.control_port !== undefined) ? dev.control_port : 7);
      portEl.value = String(p);
    }

    const isIr = (dev.type === 'IR' || !dev.type || dev.type === 'Television' || dev.type === 'Amplifier' || dev.type === 'Media Player' || dev.type === 'HomeAppliance');
    const isBt = (dev.type === 'BT' || dev.type === 'Bluetooth' || dev.isBt || Number(dev.transport) === 32);
    const isMqtt = (dev.type === 'MQTT' || dev.mqtt_enabled);

    const blasterBox = document.getElementById('curDevBlasterBox');
    if (blasterBox) blasterBox.style.display = isIr ? 'flex' : 'none';

    const btBox = document.getElementById('curDevBtBox');
    if (btBox) {
      btBox.style.display = isBt ? 'block' : 'none';
      const btBadge = document.getElementById('curDevBtStatusBadge');
      const btHostAddrText = document.getElementById('curDevBtHostAddrText');
      const connBtn = document.getElementById('btnCurDevBtConnect');
      const disconnBtn = document.getElementById('btnCurDevBtDisconnect');
      const unpairBtn = document.getElementById('btnCurDevBtUnpair');
      const knownHostRow = document.getElementById('curDevBtKnownHostRow');
      const knownHostSel = document.getElementById('curDevBtKnownHostSelect');
      const btAutoRec = document.getElementById('curDevBtAutoReconnect');
      const btDur = document.getElementById('curDevBtDuration');
      const btDly = document.getElementById('curDevBtDelay');

      const hostAddr = dev.btHost || dev.bt_host || dev.BTAddress || dev.host_addr || '';
      const hasHost = Boolean(hostAddr && hostAddr !== '00:00:00:00:00:00');

      if (btHostAddrText) {
        btHostAddrText.textContent = hasHost ? hostAddr : 'None (Unpaired)';
      }
      if (btBadge) {
        btBadge.textContent = hasHost ? 'Paired' : 'Unpaired';
        btBadge.className = hasHost ? 'badge badge-success mini' : 'badge subtle mini';
      }
      if (connBtn) connBtn.style.display = hasHost ? 'inline-block' : 'none';
      if (disconnBtn) disconnBtn.style.display = hasHost ? 'inline-block' : 'none';
      if (unpairBtn) unpairBtn.style.display = hasHost ? 'inline-block' : 'none';

      // Query live status to see if active link is connected
      if (this.client && typeof this.client.getBtStatus === 'function') {
        this.client.getBtStatus().then(st => {
          if (!st) return;
          const hosts = st.hosts || (st.host?.addr ? [st.host] : []);
          const activeHost = st.host || {};
          if (hasHost && btBadge) {
            const isConn = (activeHost.connected && activeHost.addr && activeHost.addr.toLowerCase() === hostAddr.toLowerCase()) ||
              hosts.some(h => h.connected && h.addr && h.addr.toLowerCase() === hostAddr.toLowerCase());
            btBadge.textContent = isConn ? 'Connected' : 'Paired';
            btBadge.className = isConn ? 'badge ok mini' : 'badge badge-secondary mini';
            if (connBtn) {
              connBtn.disabled = isConn;
              connBtn.textContent = isConn ? 'Connected' : 'Connect';
            }
            if (disconnBtn) {
              disconnBtn.disabled = !isConn;
            }
          }
          if (knownHostRow && knownHostSel) {
            if (hosts.length > 0) {
              knownHostRow.style.display = 'flex';
              knownHostSel.innerHTML = '<option value="">-- Link with Known Host --</option>' +
                hosts.map(h => `<option value="${escapeHtml(h.addr)}">${escapeHtml(h.name || h.addr)} (${escapeHtml(h.addr)})</option>`).join('');
            } else {
              knownHostRow.style.display = 'none';
            }
          }
        }).catch(() => { });
      }

      if (btAutoRec) {
        let recVal = 1;
        if (dev.bt_auto_reconnect !== undefined && dev.bt_auto_reconnect !== null) {
          recVal = Number(dev.bt_auto_reconnect) === 0 ? 0 : 1;
        } else if (dev.btAutoReconnect !== undefined && dev.btAutoReconnect !== null) {
          recVal = Number(dev.btAutoReconnect) === 0 ? 0 : 1;
        }
        btAutoRec.value = String(recVal);
      }
      if (btDur) {
        const dVal = dev.bt_reconnect_duration_sec !== undefined ? dev.bt_reconnect_duration_sec : (dev.btReconnectDurationSec !== undefined ? dev.btReconnectDurationSec : (dev.btReconnectDuration || 0));
        btDur.value = dVal > 0 ? dVal : '';
      }
      if (btDly) {
        const dVal = dev.bt_reconnect_delay_sec !== undefined ? dev.bt_reconnect_delay_sec : (dev.btReconnectDelaySec !== undefined ? dev.btReconnectDelaySec : (dev.btReconnectDelay !== undefined ? dev.btReconnectDelay : -1));
        btDly.value = dVal >= 0 ? dVal : '';
      }
      const btTrigSel = document.getElementById('curDevBtTriggerCmd');
      if (btTrigSel) {
        let html = '<option value="">None (Fallback to Button Press)</option>';
        const allDevs = this.devices || [];
        allDevs.forEach(d => {
          const cmds = d.commands || [];
          if (cmds.length > 0) {
            html += `<optgroup label="${this.escapeHtml(d.name || d.id || 'Device')}">`;
            cmds.forEach(c => {
              const cName = typeof c === 'string' ? c : (c.name || c.Name || '');
              if (cName) {
                const val = `${d.id}:${cName}`;
                html += `<option value="${this.escapeHtml(val)}">${this.escapeHtml(cName)}</option>`;
              }
            });
            html += `</optgroup>`;
          }
        });
        btTrigSel.innerHTML = html;

        const currentTrigDev = dev.bt_trigger_device_id || dev.btTriggerDeviceId || '';
        const currentTrigCmd = dev.bt_trigger_command_name || dev.btTriggerCommandName || dev.bt_trigger_command || dev.btTriggerCommand || '';
        if (currentTrigDev && currentTrigCmd) {
          btTrigSel.value = `${currentTrigDev}:${currentTrigCmd}`;
        } else if (currentTrigCmd && currentTrigCmd.includes(':')) {
          btTrigSel.value = currentTrigCmd;
        } else {
          btTrigSel.value = '';
        }
      }
    }

    const setInputVal = (id, val, defVal = '') => {
      const el = document.getElementById(id);
      if (el) el.value = (val !== undefined && val !== null) ? val : defVal;
    };
    setInputVal('devInterDeviceDelay', dev.inter_device_delay !== undefined ? dev.inter_device_delay : dev.interDeviceDelay, 100);
    setInputVal('devInterKeyDelay', dev.inter_key_delay !== undefined ? dev.inter_key_delay : dev.interKeyDelay, 100);
    setInputVal('devHoldInterKeyDelay', dev.hold_inter_key_delay !== undefined ? dev.hold_inter_key_delay : dev.holdInterKeyDelay, 200);
    setInputVal('devPowerOnDelay', dev.power_on_delay !== undefined ? dev.power_on_delay : dev.powerOnDelay, 1500);
    const pwrAlwaysCheck = document.getElementById('devIsPowerAlwaysOn');
    if (pwrAlwaysCheck) pwrAlwaysCheck.checked = Boolean(dev.is_power_always_on || dev.isPowerAlwaysOn);

    const editName = document.getElementById('editDevName');
    const editType = document.getElementById('editDevType');
    const editMfg = document.getElementById('editDevMfg');
    const editMdl = document.getElementById('editDevModel');
    const editIcn = document.getElementById('editDevIcon');
    if (editName) editName.value = dev.name || '';
    if (editType) editType.value = isBt ? 'BT' : (isMqtt ? 'MQTT' : 'IR');
    if (editMfg) editMfg.value = dev.manufacturer || '';
    if (editMdl) editMdl.value = dev.model || '';
    if (editIcn) {
      const rawIcn = (dev.icon || '').toLowerCase();
      const iconMap = {
        '1': 'tv', 'television': 'tv', 'tv': 'tv',
        '5': 'receiver', 'amplifier': 'receiver', 'avr': 'receiver', 'receiver': 'receiver',
        '10': 'pc', 'computer': 'pc', 'laptop': 'pc',
        '16': 'cable', 'satellite': 'cable', 'stb': 'cable',
        '33': 'streamer', 'media': 'streamer',
        '63': 'light', 'lights': 'light', 'bulb': 'light',
        'soundbar': 'soundbar', 'speaker': 'soundbar',
        'playstation': 'ps', 'ps': 'ps', 'ps4': 'ps', 'ps5': 'ps',
        'xbox': 'xbox', 'switch': 'switch', 'nintendo': 'switch',
        'shield': 'shield', 'apple': 'apple', 'firetv': 'firetv', 'roku': 'roku',
        'bluray': 'bluray', 'dvd': 'bluray', 'projector': 'projector',
        'sonos': 'sonos', 'heos': 'heos', 'plug': 'plug', 'blinds': 'blinds',
        'thermostat': 'thermostat', 'fan': 'fan', 'generic': 'generic'
      };
      editIcn.value = iconMap[rawIcn] || rawIcn || 'tv';
      if (!editIcn.value) editIcn.value = 'generic';
      this.updateBadgeForSelect(editIcn, 'devIconPreviewBadge');
    }
    if (mqttTopicInput) {
      mqttTopicInput.value = dev.mqttTopic || dev.MQTTTopic || `{base_topic}/button/${(dev.name || 'dev').toLowerCase().replace(/\s+/g, '_')}/{command}`;
    }

    const mqttPulseInput = document.getElementById('devMqttPulse');
    if (mqttPulseInput) {
      mqttPulseInput.value = dev.mqttPulseMs || dev.mqttPulse || dev.MQTTPulseMs || dev.mqtt_pulse_ms || 1000;
    }

    const mqttEnabledCheck = document.getElementById('devMqttEnabled');
    const mqttBadge = document.getElementById('devMqttEnabledBadge');
    const isMqttEn = Boolean(dev.mqttEnabled || dev.mqtt_enabled || dev.MQTTEnabled);
    if (mqttEnabledCheck) {
      mqttEnabledCheck.checked = isMqttEn;
    }
    if (mqttBadge) {
      mqttBadge.className = isMqttEn ? 'badge badge-success mini' : 'badge badge-secondary mini';
      mqttBadge.textContent = isMqttEn ? 'Enabled' : 'Disabled';
    }

    this.renderStudioCommandsList(dev);
    this.updateStudioAddPanels(dev);
  }

  renderStudioCommandsList(dev) {
    const listEl = document.getElementById('curDevCmdsList');
    if (!listEl) return;
    const filter = (document.getElementById('curDevCmdFilter')?.value || '').toLowerCase().trim();
    const cmds = (dev.commands || []).filter(c => !filter || c.name.toLowerCase().includes(filter));

    if (cmds.length === 0) {
      listEl.innerHTML = `<div class="muted mini" style="padding: 12px; text-align: center;">${(dev.commands || []).length === 0 ? 'No commands saved yet. Add one on the right.' : 'No commands match filter.'}</div>`;
      return;
    }

    const isBt = (dev.type === 'BT' || dev.type === 'Bluetooth' || dev.isBt || Number(dev.transport) === 32);

    listEl.innerHTML = cmds.map(c => {
      const isLocked = isBt && (c.name === 'Connect' || c.name === 'Disconnect');
      return `
        <div style="display: flex; justify-content: space-between; align-items: center; padding: 6px 10px; background: var(--bg-card); border-radius: var(--radius-sm); margin-bottom: 4px; border: 1px solid var(--border);">
          <div style="display: flex; align-items: center; gap: 6px;">
            <strong style="font-size: 13px; color: var(--text-main);">${escapeHtml(c.name)}</strong>
            ${isLocked ? `<span class="badge subtle mini" style="font-size: 10px;">Locked</span>` : ''}
          </div>
          <div style="display: flex; gap: 6px; align-items: center;">
            <button type="button" class="btn btn-xs btn-secondary btn-studio-test" data-dev="${escapeHtml(dev.id)}" data-cmd="${escapeHtml(c.name)}" title="Send command">▶ Test</button>
            <button type="button" class="btn btn-xs btn-ghost btn-studio-edit" data-dev="${escapeHtml(dev.id)}" data-cmd="${escapeHtml(c.name)}" title="Edit command">✎ Edit</button>
            ${isLocked ? '' : `<button type="button" class="btn btn-xs btn-ghost btn-studio-del" data-dev="${escapeHtml(dev.id)}" data-cmd="${escapeHtml(c.name)}" style="color: var(--danger);" title="Delete command">✕</button>`}
          </div>
        </div>
      `;
    }).join('');

    listEl.querySelectorAll('.btn-studio-test').forEach(btn => {
      btn.addEventListener('click', async (e) => {
        e.stopPropagation();
        const dId = btn.getAttribute('data-dev');
        const cmd = btn.getAttribute('data-cmd');
        btn.textContent = '...';
        try {
          await this.client.sendCommand(dId, cmd);
          btn.textContent = '✓ Sent';
          setTimeout(() => { btn.textContent = '▶ Test'; }, 1200);
        } catch (err) {
          alert(t('failed_to_send', 'Failed to send: ') + err.message);
          btn.textContent = '▶ Test';
        }
      });
    });

    listEl.querySelectorAll('.btn-studio-edit').forEach(btn => {
      btn.addEventListener('click', (e) => {
        e.stopPropagation();
        const cmdName = btn.getAttribute('data-cmd');
        const c = (dev.commands || []).find(x => x.name === cmdName);
        if (c) this.openCommandEditModal(dev, c);
      });
    });

    listEl.querySelectorAll('.btn-studio-del').forEach(btn => {
      btn.addEventListener('click', async (e) => {
        e.stopPropagation();
        const dId = btn.getAttribute('data-dev');
        const cmd = btn.getAttribute('data-cmd');
        if (isBt && (cmd === 'Connect' || cmd === 'Disconnect')) {
          alert(t("bt_cmds_cannot_delete"));
          return;
        }
        if (!await showConfirm(`Delete command "${cmd}" from this device?`, { title: 'Delete Command', okText: 'Delete' })) return;
        try {
          await this.client.deleteCommand(dId, cmd);
          await this.refreshData();
          this.renderIrView();
        } catch (err) {
          alert(t('delete_failed', 'Delete failed: ') + err.message);
        }
      });
    });
  }

  openCommandEditModal(dev, c) {
    const modal = document.getElementById('cmdEditModal');
    if (!modal) return;
    document.getElementById('editCmdDevId').value = String(dev.id);
    document.getElementById('editCmdOldName').value = c.name;
    document.getElementById('editCmdName').value = c.name;
    const rawVal = c.raw || c.keyCode || '';
    document.getElementById('editCmdPayload').value = rawVal;

    const isBt = (dev.type === 'BT' || dev.type === 'Bluetooth' || dev.isBt || Number(dev.transport) === 32);
    const isMqtt = (dev.type === 'MQTT' || dev.mqtt_enabled);
    const isIr = !isBt && !isMqtt;

    const typeWrap = document.getElementById('editCmdTypeWrap');
    const btHint = document.getElementById('editCmdBtHint');
    const irOpts = document.getElementById('editCmdIrOptions');
    const payloadWrap = document.getElementById('editCmdPayloadWrap');

    if (typeWrap) typeWrap.style.display = isIr ? 'block' : 'none';
    if (btHint) btHint.style.display = isBt ? 'block' : 'none';
    if (irOpts) irOpts.style.display = isIr ? 'grid' : 'none';
    if (payloadWrap) payloadWrap.style.display = isMqtt ? 'none' : 'block';

    let modeVal = 'raw';
    if (isBt) {
      modeVal = 'bt';
    } else if (isMqtt) {
      modeVal = 'mqtt';
    } else {
      modeVal = (rawVal.startsWith('G:') || c.type === 'keycode') ? 'keycode' : 'raw';
    }
    const typeSelect = document.getElementById('editCmdType');
    if (typeSelect && isIr) {
      typeSelect.value = modeVal;
    }

    document.getElementById('editCmdRepeats').value = c.repeats || 3;
    document.getElementById('editCmdPort').value = String(c.port || 0);

    const stat = document.getElementById('cmdEditStatus');
    if (stat) stat.textContent = '';

    modal.dataset.deviceType = isBt ? 'BT' : (isMqtt ? 'MQTT' : 'IR');
    modal.style.display = 'flex';
  }

  async submitNewDeviceProfile() {
    const name = document.getElementById('newDeviceName')?.value.trim();
    const type = document.getElementById('newDeviceType')?.value || 'IR';
    const manufacturer = document.getElementById('newDeviceManufacturer')?.value.trim();
    const model = document.getElementById('newDeviceModel')?.value.trim();
    const status = document.getElementById('profileStatus');

    if (!name || !manufacturer || !model) return alert(t("name_mfr_model_required"));
    try {
      if (status) status.textContent = 'Saving new device...';
      const icon = document.getElementById('newDeviceIcon')?.value || 'tv';
      const devPayload = {
        name,
        type,
        manufacturer,
        model,
        icon,
        transport: type === 'BT' ? 32 : 1
      };
      if (type === 'MQTT') {
        devPayload.mqttTopic = `{base_topic}/button/${name.toLowerCase().replace(/\\s+/g, '_')}/{command}`;
        devPayload.mqttEnabled = 1;
        devPayload.mqttPulseMs = 1000;
      }
      const res = await this.client.saveNewDevice(devPayload);
      const newDevId = res?.deviceId || res?.id;
      if (newDevId) {
        if (type === 'BT') {
          const btButtons = [
            { name: 'Power', raw: 'HID:01:0081' },
            { name: 'Select', raw: 'HID:07:0028' },
            { name: 'DirectionUp', raw: 'HID:07:0052' },
            { name: 'DirectionDown', raw: 'HID:07:0051' },
            { name: 'DirectionLeft', raw: 'HID:07:0050' },
            { name: 'DirectionRight', raw: 'HID:07:004F' },
            { name: 'Back', raw: 'HID:07:0029' },
            { name: 'Home', raw: 'HID:07:004A' },
            { name: 'Menu', raw: 'HID:07:0076' },
            { name: 'VolumeUp', raw: 'HID:0C:00E9' },
            { name: 'VolumeDown', raw: 'HID:0C:00EA' },
            { name: 'Mute', raw: 'HID:0C:00E2' },
            { name: 'Play', raw: 'HID:0C:00B0' },
            { name: 'Pause', raw: 'HID:0C:00B1' },
            { name: 'Connect', raw: 'HID:00:0000' },
            { name: 'Disconnect', raw: 'HID:00:0000' }
          ];
          for (const b of btButtons) {
            await this.client.saveLearnedCommand({ deviceId: newDevId, name: b.name, raw: b.raw, mode: 'raw' }).catch(() => { });
          }
        } else if (type === 'MQTT') {
          const mqttButtons = [
            'PowerToggle', 'VolumeUp', 'VolumeDown', 'Mute', 'Play', 'Pause',
            'DirectionUp', 'DirectionDown', 'DirectionLeft', 'DirectionRight', 'Select'
          ];
          for (const m of mqttButtons) {
            await this.client.saveLearnedCommand({ deviceId: newDevId, name: m, raw: `G:MQTT:${m}`, mode: 'raw' }).catch(() => { });
          }
        }
      }
      if (status) status.innerHTML = '<span style="color:var(--success)">✓ Device created successfully!</span>';
      await this.refreshData();
      if (newDevId) this.selectedIrDeviceId = String(newDevId);
      this.renderIrView();
    } catch (err) {
      if (status) status.innerHTML = `<span style="color:var(--danger)">Error: ${escapeHtml(err.message)}</span>`;
    }
  }

  openPowerSequenceModal() {
    const devId = this.selectedIrDeviceId || document.getElementById('manageDeviceSelect')?.value;
    const dev = (this.inventory.devices || []).find(d => String(d.id) === String(devId));
    if (!dev) return alert(t("select_device_first_dot"));
    const modal = document.getElementById('powerSeqModal');
    if (!modal) return;
    const devNameEl = document.getElementById('powerSeqDevName');
    if (devNameEl) devNameEl.textContent = dev.name || dev.model || dev.id;

    const pOn = dev.powerOnSequence || dev.power_on_sequence || dev.PowerOnActions || [];
    const pOff = dev.powerOffSequence || dev.power_off_sequence || dev.PowerOffActions || [];
    this.renderSequenceList('powerOnStepsList', Array.isArray(pOn) ? pOn : []);
    this.renderSequenceList('powerOffStepsList', Array.isArray(pOff) ? pOff : []);

    const stat = document.getElementById('powerSeqStatus');
    if (stat) stat.textContent = '';
    modal.style.display = 'flex';
  }

  async saveDevicePowerSequences() {
    const devId = this.selectedIrDeviceId || document.getElementById('manageDeviceSelect')?.value;
    const dev = (this.inventory.devices || []).find(d => String(d.id) === String(devId));
    if (!dev) return;
    const stat = document.getElementById('powerSeqStatus');
    if (stat) stat.textContent = 'Saving sequences...';
    try {
      const pOn = this.serializeSequence('powerOnStepsList');
      const pOff = this.serializeSequence('powerOffStepsList');
      await this.client.saveDevicePower({
        deviceId: dev.id,
        powerOnSequence: pOn,
        powerOffSequence: pOff
      });
      dev.powerOnSequence = pOn;
      dev.powerOffSequence = pOff;
      if (stat) stat.innerHTML = '<span style="color:var(--success)">Saved!</span>';
      setTimeout(() => {
        const modal = document.getElementById('powerSeqModal');
        if (modal) modal.style.display = 'none';
      }, 1000);
      await this.refreshData();
    } catch (err) {
      if (stat) stat.innerHTML = `<span style="color:var(--danger)">Error: ${escapeHtml(err.message)}</span>`;
    }
  }

  updateStudioAddPanels(dev) {
    if (!dev) return;
    const isBt = (dev.type === 'BT' || dev.type === 'Bluetooth' || dev.isBt || Number(dev.transport) === 32);
    const isMqtt = (dev.type === 'MQTT' || dev.mqtt_enabled);
    const isIr = !isBt && !isMqtt;

    const irTabsRow = document.getElementById('irAddTabsRow');
    const btTabsRow = document.getElementById('btAddTabsRow');
    const mqttTabsRow = document.getElementById('mqttAddTabsRow');

    if (irTabsRow) irTabsRow.style.display = isIr ? 'flex' : 'none';
    if (btTabsRow) btTabsRow.style.display = isBt ? 'flex' : 'none';
    if (mqttTabsRow) mqttTabsRow.style.display = isMqtt ? 'flex' : 'none';

    const pLearn = document.getElementById('addModeLearn');
    const pDb = document.getElementById('addModeDb');
    const pPaste = document.getElementById('addModePaste');
    const pBtPresets = document.getElementById('addModeBtPresets');
    const pBtCustom = document.getElementById('addModeBtCustom');
    const pMqtt = document.getElementById('addModeMqtt');

    if (isIr) {
      if (pBtPresets) pBtPresets.style.display = 'none';
      if (pBtCustom) pBtCustom.style.display = 'none';
      if (pMqtt) pMqtt.style.display = 'none';

      const activeTab = document.querySelector('.ir-add-tab.btn-primary')?.getAttribute('data-add-mode') || 'learn';
      if (pLearn) pLearn.style.display = (activeTab === 'learn') ? 'block' : 'none';
      if (pDb) pDb.style.display = (activeTab === 'database') ? 'block' : 'none';
      if (pPaste) pPaste.style.display = (activeTab === 'paste') ? 'block' : 'none';
    } else if (isBt) {
      if (pLearn) pLearn.style.display = 'none';
      if (pDb) pDb.style.display = 'none';
      if (pPaste) pPaste.style.display = 'none';
      if (pMqtt) pMqtt.style.display = 'none';

      const activeBtTab = document.querySelector('.bt-add-tab.btn-primary')?.getAttribute('data-bt-mode') || 'presets';
      if (pBtPresets) pBtPresets.style.display = (activeBtTab === 'presets') ? 'block' : 'none';
      if (pBtCustom) pBtCustom.style.display = (activeBtTab === 'custom') ? 'block' : 'none';

      this.renderBtPresetsList(dev);
    } else if (isMqtt) {
      if (pLearn) pLearn.style.display = 'none';
      if (pDb) pDb.style.display = 'none';
      if (pPaste) pPaste.style.display = 'none';
      if (pBtPresets) pBtPresets.style.display = 'none';
      if (pBtCustom) pBtCustom.style.display = 'none';
      if (pMqtt) pMqtt.style.display = 'block';

      this.updateMqttTopicPreview(dev);
    }
  }

  renderBtPresetsList(dev) {
    const cont = document.getElementById('btPresetsContainer');
    if (!cont) return;
    const filter = (document.getElementById('btPresetsFilter')?.value || '').toLowerCase().trim();
    const existingCmds = dev.commands || [];
    const existingMap = new Set(existingCmds.map(c => (typeof c === 'string' ? c : (c.name || '')).toLowerCase()));
    const existingCodes = new Set(existingCmds.map(c => (typeof c === 'string' ? '' : (c.raw || c.keyCode || '')).toUpperCase()));

    let html = '';
    let totalRendered = 0;

    BT_STANDARD_KEY_PRESETS.forEach(group => {
      const filteredKeys = group.keys.filter(k => {
        if (!filter) return true;
        return k.name.toLowerCase().includes(filter) ||
               k.desc.toLowerCase().includes(filter) ||
               k.code.toLowerCase().includes(filter);
      });

      if (filteredKeys.length === 0) return;

      html += `
        <div style="background: var(--bg-dark); border: 1px solid var(--border); border-radius: var(--radius-sm); padding: 8px 10px;">
          <div style="font-size: 11px; font-weight: 600; color: var(--text-dim); margin-bottom: 6px; text-transform: uppercase; letter-spacing: 0.5px;">${escapeHtml(group.category)}</div>
          <div style="display: flex; flex-direction: column; gap: 4px;">
      `;

      filteredKeys.forEach(k => {
        totalRendered++;
        const isAdded = existingMap.has(k.name.toLowerCase()) || existingCodes.has(k.code.toUpperCase());
        html += `
          <div style="display: flex; justify-content: space-between; align-items: center; padding: 4px 8px; background: var(--bg-card); border-radius: var(--radius-sm); border: 1px solid var(--border-subtle);">
            <div style="display: flex; align-items: baseline; gap: 8px; min-width: 0;">
              <strong style="font-size: 12px; color: var(--text-main); white-space: nowrap;">${escapeHtml(k.name)}</strong>
              <span class="muted mini" style="font-size: 10px; font-family: monospace;">${escapeHtml(k.code)}</span>
              <span class="muted mini" style="font-size: 11px; text-overflow: ellipsis; overflow: hidden; white-space: nowrap;">${escapeHtml(k.desc)}</span>
            </div>
            <div style="display: flex; align-items: center; gap: 4px; flex-shrink: 0;">
              <button type="button" class="btn btn-xs btn-ghost btn-test-bt-preset" data-code="${escapeHtml(k.code)}" title="Test sending key to host">▶</button>
              ${isAdded ?
                `<span class="badge subtle mini" style="font-size: 10px;">Added</span>` :
                `<button type="button" class="btn btn-xs btn-primary btn-add-bt-preset" data-name="${escapeHtml(k.name)}" data-code="${escapeHtml(k.code)}">+ Add</button>`
              }
            </div>
          </div>
        `;
      });

      html += `
          </div>
        </div>
      `;
    });

    if (totalRendered === 0) {
      cont.innerHTML = `<div class="muted mini" style="padding: 16px; text-align: center;">No Bluetooth keys match filter.</div>`;
      return;
    }

    cont.innerHTML = html;

    cont.querySelectorAll('.btn-test-bt-preset').forEach(btn => {
      btn.addEventListener('click', async (e) => {
        e.stopPropagation();
        const code = btn.getAttribute('data-code');
        btn.textContent = '...';
        try {
          await this.client.sendBtKey(code, 'tap', dev.bdaddr || dev.btHost || '');
          btn.textContent = '✓';
          setTimeout(() => { btn.textContent = '▶'; }, 1000);
        } catch (err) {
          alert(t('test_failed', 'Test failed: ') + err.message);
          btn.textContent = '▶';
        }
      });
    });

    cont.querySelectorAll('.btn-add-bt-preset').forEach(btn => {
      btn.addEventListener('click', async (e) => {
        e.stopPropagation();
        const name = btn.getAttribute('data-name');
        const code = btn.getAttribute('data-code');
        btn.disabled = true;
        btn.textContent = '...';
        try {
          await this.client.saveLearnedCommand({ deviceId: dev.id, name, raw: code, mode: 'raw' });
          await this.refreshData();
          this.renderIrView();
        } catch (err) {
          alert(t('add_failed', 'Add failed: ') + err.message);
          btn.disabled = false;
          btn.textContent = '+ Add';
        }
      });
    });
  }

  updateMqttTopicPreview(dev) {
    const previewEl = document.getElementById('mqttCmdTopicPreview');
    if (!previewEl) return;
    const cmdName = document.getElementById('mqttNewCmdName')?.value.trim() || '<command>';
    const template = dev.mqttTopic || '{base_topic}/button/{device}/{command}';
    const devSlug = (dev.name || 'device').toLowerCase().replace(/\s+/g, '_');
    const preview = template
      .replace('{base_topic}', 'harmony')
      .replace('{device}', devSlug)
      .replace('{device_id}', dev.id)
      .replace('{command}', cmdName);
    previewEl.textContent = preview;
  }

}
