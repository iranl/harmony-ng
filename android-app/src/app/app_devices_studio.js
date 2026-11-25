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
import { t, setLanguage, getInitialLanguage, tf } from '../i18n.js';
import { pluginFieldIsBoolean, configFlagOn, pluginDefaultCommand, irAltBadgeHtml } from './plugin_fields.js';
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

// category and desc are the English texts; categoryKey and descKey translate them when shown
export const BT_STANDARD_KEY_PRESETS = [
  {
    category: 'Navigation & Core',
    categoryKey: 'btkey_cat_navigation',
    keys: [
      { name: 'Select', code: 'HID:07:0028', desc: 'Enter / OK', descKey: 'btkey_desc_enter_ok' },
      { name: 'DirectionUp', code: 'HID:07:0052', desc: 'Up Arrow', descKey: 'btkey_desc_up' },
      { name: 'DirectionDown', code: 'HID:07:0051', desc: 'Down Arrow', descKey: 'btkey_desc_down' },
      { name: 'DirectionLeft', code: 'HID:07:0050', desc: 'Left Arrow', descKey: 'btkey_desc_left' },
      { name: 'DirectionRight', code: 'HID:07:004F', desc: 'Right Arrow', descKey: 'btkey_desc_right' },
      { name: 'Back', code: 'HID:0C:0224', desc: 'Android / Apple Return', descKey: 'btkey_desc_back' },
      { name: 'Home', code: 'HID:0C:0223', desc: 'AC Home Screen', descKey: 'btkey_desc_home' },
      { name: 'Menu', code: 'HID:0C:0040', desc: 'Context / App Menu', descKey: 'btkey_desc_menu' },
      { name: 'Escape', code: 'HID:07:0029', desc: 'Cancel / Exit', descKey: 'btkey_desc_escape' },
      { name: 'PowerToggle', code: 'HID:0C:0030', desc: 'Power On / Standby', descKey: 'btkey_desc_power' },
      { name: 'Search', code: 'HID:0C:0221', desc: 'Voice Search / Assistant', descKey: 'btkey_desc_search' },
      { name: 'Tab', code: 'HID:07:002B', desc: 'Tab Key', descKey: 'btkey_desc_tab' },
      { name: 'Space', code: 'HID:07:002C', desc: 'Spacebar', descKey: 'btkey_desc_space' },
      { name: 'Backspace', code: 'HID:07:002A', desc: 'Backspace', descKey: 'btkey_desc_backspace' }
    ]
  },
  {
    category: 'Media & Playback',
    categoryKey: 'btkey_cat_media',
    keys: [
      { name: 'Play', code: 'HID:0C:00B0', desc: 'Play', descKey: 'play' },
      { name: 'Pause', code: 'HID:0C:00B1', desc: 'Pause', descKey: 'pause' },
      { name: 'PlayPause', code: 'HID:0C:00CD', desc: 'Play / Pause Toggle', descKey: 'btkey_desc_playpause' },
      { name: 'Stop', code: 'HID:0C:00B7', desc: 'Stop Playback', descKey: 'btkey_desc_stop' },
      { name: 'NextTrack', code: 'HID:0C:00B5', desc: 'Skip Forward', descKey: 'btkey_desc_next' },
      { name: 'PreviousTrack', code: 'HID:0C:00B6', desc: 'Skip Backward', descKey: 'btkey_desc_prev' },
      { name: 'FastForward', code: 'HID:0C:00B3', desc: 'Fast Forward', descKey: 'btkey_desc_ff' },
      { name: 'Rewind', code: 'HID:0C:00B4', desc: 'Scan Backward', descKey: 'btkey_desc_rewind' }
    ]
  },
  {
    category: 'Audio & Volume',
    categoryKey: 'btkey_cat_audio',
    keys: [
      { name: 'VolumeUp', code: 'HID:0C:00E9', desc: 'Volume +', descKey: 'btkey_desc_vol_up' },
      { name: 'VolumeDown', code: 'HID:0C:00EA', desc: 'Volume -', descKey: 'btkey_desc_vol_down' },
      { name: 'Mute', code: 'HID:0C:00E2', desc: 'Mute Audio', descKey: 'btkey_desc_mute' }
    ]
  },
  {
    category: 'TV & Guide',
    categoryKey: 'btkey_cat_tv_guide',
    keys: [
      { name: 'ChannelUp', code: 'HID:0C:009C', desc: 'Channel Up', descKey: 'btn_channel_up' },
      { name: 'ChannelDown', code: 'HID:0C:009D', desc: 'Channel Down', descKey: 'btn_channel_down' },
      { name: 'Info', code: 'HID:0C:008D', desc: 'Program Guide / Info', descKey: 'btkey_desc_info' },
      { name: 'PageUp', code: 'HID:07:004B', desc: 'Page Up', descKey: 'btn_page_up' },
      { name: 'PageDown', code: 'HID:07:004E', desc: 'Page Down', descKey: 'btn_page_down' },
      { name: 'ContextMenu', code: 'HID:07:0065', desc: 'Application Context Menu', descKey: 'btkey_desc_context_menu' }
    ]
  },
  {
    category: 'Numeric Keypad',
    categoryKey: 'btkey_cat_numeric',
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

    const allDevs = this.inventory.devices || [];
    if (!allDevs.length) {
      cont.innerHTML = `<div class="card"><div class="muted">${t('studio_no_devices_stored')}</div></div>`;
      return;
    }
    const devs = allDevs.filter(d => !d.hideInDevicesTab);
    if (!devs.length) {
      cont.innerHTML = `<div class="card"><div class="muted">${t('devices_all_hidden')}</div></div>`;
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
      onSendCommand: async (devId, cmd, action) => {
        await this.client.sendCommand(devId, cmd, action, { live: true });
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

  getPluginForType(type) {
    if (!type) return null;
    const sType = String(type).trim();
    if (sType.startsWith('plugin:')) {
      const pid = sType.substring(7);
      return (this.installedPlugins || []).find(p => p.id === pid) || { id: pid };
    }
    return (this.installedPlugins || []).find(p => p.id === sType || (p.device_definition?.device_type && p.device_definition.device_type.toLowerCase() === sType.toLowerCase()) || (p.device_definition?.display_name && p.device_definition.display_name.toLowerCase() === sType.toLowerCase()));
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
      if (nameEl) nameEl.textContent = t('no_devices_found');
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
    if (nameEl) nameEl.textContent = dev.name || tf('device_n', { id: dev.id });
    const pluginDef = this.getPluginForType(dev.type) || (dev.plugin ? (this.installedPlugins || []).find(p => p.id === dev.plugin) : null);
    const isPlugin = !!(pluginDef || dev.plugin || (dev.type && dev.type.startsWith('plugin:')));
    const pluginId = dev.plugin || pluginDef?.id || (dev.type?.startsWith('plugin:') ? dev.type.substring(7) : '');
    if (detailsEl) {
      if (isPlugin) {
        // a password field of the plugin is never shown
        const secretKeys = new Set((Array.isArray(pluginDef?.device_definition?.config_fields) ? pluginDef.device_definition.config_fields : [])
          .filter(f => f && f.type === 'password').map(f => f.key));
        const cfgSummary = dev.config && typeof dev.config === 'object'
          ? Object.entries(dev.config).map(([k, v]) => `${k}: ${secretKeys.has(k) ? '••••' : v}`).join(', ') : '';
        detailsEl.textContent = tf('studio_plugin_details_line', { plugin: pluginId, config: cfgSummary ? ' • (' + cfgSummary + ')' : '', name: dev.name || dev.id });
      } else {
        detailsEl.textContent = tf('studio_device_details_line', { type: dev.type || t('device_type_appliance'), brand: dev.manufacturer || t('unknown'), model: dev.model || t('unknown') });
      }
    }
    if (countEl) countEl.textContent = (dev.commands || []).length;
    if (portEl) {
      const p = (dev.controlPort !== undefined) ? dev.controlPort : 7;
      portEl.value = String(p);
    }

    const isBt = !isPlugin && (dev.type === 'BT' || dev.type === 'Bluetooth' || dev.isBt);
    const isMqtt = !isPlugin && !isBt && (dev.type === 'MQTT' || dev.mqttEnabled);
    /* IR: not a plugin, Bluetooth, MQTT, HTTP or TCP device, as in the Edit box and the Hub's
     * sanitizer (a type list hid the blaster row of an IR device of another type on a redraw) */
    const isIr = !isPlugin && !isBt && !isMqtt && !this.isNetDevice(dev);

    const blasterBox = document.getElementById('curDevBlasterBox');
    if (blasterBox) blasterBox.style.display = isIr ? 'flex' : 'none';

    const devEditBtBox = document.getElementById('devEditBtReconnectBox');
    if (devEditBtBox) devEditBtBox.style.display = isBt ? 'block' : 'none';

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

      const hostAddr = dev.bdaddr || dev.btHost || '';
      const hasHost = Boolean(hostAddr && hostAddr !== '00:00:00:00:00:00');

      if (btHostAddrText) {
        btHostAddrText.textContent = hasHost ? hostAddr : t('none_unpaired');
      }
      if (btBadge) {
        btBadge.textContent = hasHost ? t('host_paired') : t('unpaired_state');
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
            btBadge.textContent = isConn ? t('connected') : t('host_paired');
            btBadge.className = isConn ? 'badge ok mini' : 'badge badge-secondary mini';
            if (connBtn) {
              connBtn.disabled = isConn;
              connBtn.textContent = isConn ? t('connected') : t('connect');
            }
            if (disconnBtn) {
              disconnBtn.disabled = !isConn;
            }
          }
          if (knownHostRow && knownHostSel) {
            if (hosts.length > 0) {
              knownHostRow.style.display = 'flex';
              knownHostSel.innerHTML = `<option value="">${t('link_known_host_opt')}</option>` +
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
        const dVal = dev.btReconnectDurationSec !== undefined ? dev.btReconnectDurationSec : 0;
        btDur.value = dVal > 0 ? dVal : '';
      }
      if (btDly) {
        const dVal = dev.btReconnectDelaySec !== undefined ? dev.btReconnectDelaySec : -1;
        btDly.value = dVal >= 0 ? dVal : '';
      }
      const btTrigSel = document.getElementById('curDevBtTriggerCmd');
      if (btTrigSel) {
        let html = `<option value="">${t('opt_none_fallback')}</option>`;
        const allDevs = this.inventory?.devices || [];
        allDevs.forEach(d => {
          const cmds = (this.deviceCommandsMap && this.deviceCommandsMap[d.id]) || d.commands || [];
          if (cmds.length > 0) {
            let typeLabel = '';
            if (d.type === 'MQTT' || d.mqttEnabled) typeLabel = 'MQTT';
            else if (d.type === 'BT' || d.type === 'Bluetooth' || d.isBt) typeLabel = 'Bluetooth';
            else if (d.type) typeLabel = d.type;
            else typeLabel = 'IR';
            html += `<optgroup label="${escapeHtml(d.name || d.id || t('device_default'))} (${typeLabel})">`;
            cmds.forEach(c => {
              const cName = typeof c === 'string' ? c : (c.name || '');
              if (cName) {
                const val = `${d.id}:${cName}`;
                html += `<option value="${escapeHtml(val)}">${escapeHtml(cName)}</option>`;
              }
            });
            html += `</optgroup>`;
          }
        });
        btTrigSel.innerHTML = html;

        const currentTrigDev = dev.btTriggerDeviceId || '';
        const currentTrigCmd = dev.btTriggerCommandName || '';
        if (currentTrigDev && currentTrigCmd) {
          btTrigSel.value = `${currentTrigDev}:${currentTrigCmd}`;
        } else if (currentTrigCmd && currentTrigCmd.includes(':')) {
          btTrigSel.value = currentTrigCmd;
        } else if (currentTrigCmd) {
          const optMatch = btTrigSel.querySelector(`option[value="${dev.id}:${currentTrigCmd}"]`) ||
                           btTrigSel.querySelector(`option[value$=":${currentTrigCmd}"]`);
          if (optMatch) {
            btTrigSel.value = optMatch.value;
          } else {
            btTrigSel.value = '';
          }
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
    // empty when the device has none: the Hub then uses 3 (the placeholder)
    setInputVal('devPressMinRepeats', dev.pressMinRepeats, '');
    const pwrAlwaysCheck = document.getElementById('devIsPowerAlwaysOn');
    if (pwrAlwaysCheck) pwrAlwaysCheck.checked = Boolean(dev.is_power_always_on || dev.isPowerAlwaysOn);
    const hideCheck = document.getElementById('devHideOnRemote');
    if (hideCheck) hideCheck.checked = Boolean(dev.hideOnRemote);
    const hideTabCheck = document.getElementById('devHideInDevicesTab');
    if (hideTabCheck) hideTabCheck.checked = Boolean(dev.hideInDevicesTab);

    const editName = document.getElementById('editDevName');
    const editType = document.getElementById('editDevType');
    const editMfg = document.getElementById('editDevMfg');
    const editMdl = document.getElementById('editDevModel');
    const editIcn = document.getElementById('editDevIcon');
    if (editName) editName.value = dev.name || '';
    if (editType) {
      editType.disabled = true;
      editType.title = t('plg_type_locked');
      if (isPlugin) {
        const plugVal = (dev.type && dev.type.startsWith('plugin:')) ? dev.type : (pluginId ? `plugin:${pluginId}` : dev.type);
        let opt = editType.querySelector(`option[value="${plugVal}"]`) || editType.querySelector(`option[value="${dev.type}"]`);
        if (!opt) {
          let optgroup = document.getElementById('editDevTypePluginGroup');
          if (!optgroup) {
            optgroup = document.createElement('optgroup');
            optgroup.id = 'editDevTypePluginGroup';
            optgroup.label = t('plg_community_plugins');
            editType.appendChild(optgroup);
          }
          opt = document.createElement('option');
          opt.value = plugVal;
          opt.textContent = tf('plg_type_option', { name: pluginDef?.device_definition?.display_name || pluginDef?.name || pluginId });
          optgroup.appendChild(opt);
        }
        editType.value = opt.value;
      } else if (isBt) {
        editType.value = 'BT';
      } else if (isMqtt) {
        editType.value = 'MQTT';
      } else {
        editType.value = dev.type || 'IR';
      }
    }
    const devEditTimingsBox = document.getElementById('devEditTimingsBox');
    if (devEditTimingsBox) devEditTimingsBox.style.display = isIr ? 'block' : 'none';
    if (typeof this.handleEditDeviceTypeChanged === 'function') {
      this.handleEditDeviceTypeChanged(editType ? editType.value : (dev.type || 'IR'), dev.config || dev);
    }
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
      mqttTopicInput.value = dev.mqttTopic || `{base_topic}/button/${(dev.name || 'dev').toLowerCase().replace(/\s+/g, '_')}/{command}`;
    }

    const mqttPulseInput = document.getElementById('devMqttPulse');
    if (mqttPulseInput) {
      mqttPulseInput.value = dev.mqttPulseMs || 1000;
    }

    const mqttEnabledCheck = document.getElementById('devMqttEnabled');
    const mqttBadge = document.getElementById('devMqttEnabledBadge');
    const isMqttEn = Boolean(dev.mqttEnabled);
    if (mqttEnabledCheck) {
      mqttEnabledCheck.checked = isMqttEn;
    }
    if (mqttBadge) {
      mqttBadge.className = isMqttEn ? 'badge badge-success mini' : 'badge badge-secondary mini';
      mqttBadge.textContent = isMqttEn ? t('enabled_lbl') : t('disabled');
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
      listEl.innerHTML = `<div class="muted mini" style="padding: 12px; text-align: center;">${(dev.commands || []).length === 0 ? t('no_cmds_saved_add_right') : t('no_cmds_match_filter')}</div>`;
      return;
    }

    const isBt = (dev.type === 'BT' || dev.type === 'Bluetooth' || dev.isBt);

    listEl.innerHTML = cmds.map(c => {
      const isLocked = isBt && (c.name === 'Connect' || c.name === 'Disconnect');
      return `
        <div style="display: flex; justify-content: space-between; align-items: center; padding: 6px 10px; background: var(--bg-card); border-radius: var(--radius-sm); margin-bottom: 4px; border: 1px solid var(--border);">
          <div style="display: flex; align-items: center; gap: 6px;">
            <strong style="font-size: 13px; color: var(--text-main);">${escapeHtml(c.name)}</strong>
            ${isLocked ? `<span class="badge subtle mini" style="font-size: 10px;">${t('badge_locked')}</span>` : ''}
            ${irAltBadgeHtml(c, dev.config)}
          </div>
          <div style="display: flex; gap: 6px; align-items: center;">
            <button type="button" class="btn btn-xs btn-secondary btn-studio-test" data-dev="${escapeHtml(dev.id)}" data-cmd="${escapeHtml(c.name)}" title="${escapeHtml(t('send_command_title'))}">▶ ${t('test')}</button>
            <button type="button" class="btn btn-xs btn-ghost btn-studio-edit" data-dev="${escapeHtml(dev.id)}" data-cmd="${escapeHtml(c.name)}" title="${escapeHtml(t('edit_command_title'))}">✎ ${t('edit')}</button>
            ${isLocked ? '' : `<button type="button" class="btn btn-xs btn-ghost btn-studio-del" data-dev="${escapeHtml(dev.id)}" data-cmd="${escapeHtml(c.name)}" style="color: var(--danger);" title="${escapeHtml(t('delete_command_title'))}">✕</button>`}
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
          btn.textContent = `✓ ${t('sent_short')}`;
          setTimeout(() => { btn.textContent = `▶ ${t('test')}`; }, 1200);
        } catch (err) {
          alert(t('failed_to_send', 'Failed to send: ') + err.message);
          btn.textContent = `▶ ${t('test')}`;
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
        if (!await showConfirm(tf('delete_cmd_from_dev_confirm', { cmd: cmd }), { title: t('delete_cmd', 'Delete Command'), okText: t('delete', 'Delete') })) return;
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
    if (this.isNetDevice(dev)) {
      this.fillNetCommand(dev, c);
      return;
    }
    const modal = document.getElementById('cmdEditModal');
    if (!modal) return;
    document.getElementById('editCmdDevId').value = String(dev.id);
    document.getElementById('editCmdOldName').value = c.name;
    document.getElementById('editCmdName').value = c.name;
    const rawVal = c.raw || c.keyCode || '';
    document.getElementById('editCmdPayload').value = rawVal;

    const pluginDef = this.getPluginForType(dev.type) || (dev.plugin ? (this.installedPlugins || []).find(p => p.id === dev.plugin) : null);
    const isPlugin = !!(pluginDef || dev.plugin || (dev.type && dev.type.startsWith('plugin:')) || (rawVal && rawVal.startsWith('PLUGIN:')));
    const isBt = (dev.type === 'BT' || dev.type === 'Bluetooth' || dev.isBt);
    const isMqtt = (dev.type === 'MQTT' || dev.mqttEnabled);
    const isIr = !isPlugin && !isBt && !isMqtt;

    const typeWrap = document.getElementById('editCmdTypeWrap');
    const btHint = document.getElementById('editCmdBtHint');
    const irOpts = document.getElementById('editCmdIrOptions');
    const payloadWrap = document.getElementById('editCmdPayloadWrap');
    const payloadLabel = payloadWrap?.querySelector('label');

    if (typeWrap) typeWrap.style.display = isIr ? 'block' : 'none';
    if (btHint) btHint.style.display = isBt ? 'block' : 'none';
    if (irOpts) irOpts.style.display = isIr ? 'grid' : 'none';
    if (payloadWrap) payloadWrap.style.display = isMqtt ? 'none' : 'block';

    const payloadInp = document.getElementById('editCmdPayload');
    if (isPlugin) {
      if (payloadLabel) payloadLabel.textContent = t('plg_keycode_label');
      if (payloadInp) payloadInp.placeholder = tf('plg_keycode_placeholder', { id: pluginDef?.id || dev.plugin || 'plugin' });
    } else if (isBt) {
      if (payloadLabel) payloadLabel.textContent = t('cmd_bt_hid_code_label');
      if (payloadInp) payloadInp.placeholder = t('cmd_bt_hid_code_placeholder');
    } else {
      if (payloadLabel) payloadLabel.textContent = t('command_data_signal');
      if (payloadInp) payloadInp.placeholder = t('ph_cmd_payload_formats');
    }

    let modeVal = 'raw';
    if (isPlugin) {
      modeVal = 'plugin';
    } else if (isBt) {
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

    modal.dataset.deviceType = isPlugin ? 'PLUGIN' : (isBt ? 'BT' : (isMqtt ? 'MQTT' : 'IR'));
    modal.style.display = 'flex';
  }

  async submitNewDeviceProfile() {
    const name = document.getElementById('newDeviceName')?.value.trim();
    const type = document.getElementById('newDeviceType')?.value || 'IR';
    const manufacturer = document.getElementById('newDeviceManufacturer')?.value.trim();
    const model = document.getElementById('newDeviceModel')?.value.trim();
    const status = document.getElementById('profileStatus');

    const pluginDef = this.getPluginForType(type);
    const isPlugin = !!pluginDef;
    const pluginId = pluginDef?.id || (type.startsWith('plugin:') ? type.substring(7) : null);
    let effectiveMfg = manufacturer;
    let effectiveModel = model;
    if (isPlugin) {
      if (!effectiveMfg) effectiveMfg = pluginDef?.author || pluginDef?.name || 'Community';
      if (!effectiveModel) effectiveModel = pluginDef?.device_definition?.device_type || pluginId;
    }

    if (!name || (!isPlugin && (!effectiveMfg || !effectiveModel))) return alert(t("name_mfr_model_required"));
    try {
      if (status) status.textContent = t('saving_new_device');
      const icon = document.getElementById('newDeviceIcon')?.value || 'tv';
      const existingIds = new Set((this.inventory.devices || []).map(d => String(d.id)));
      let cand = 1;
      while (existingIds.has(String(cand))) {
        cand++;
      }
      const generatedDevId = String(cand);
      const devPayload = {
        deviceId: generatedDevId,
        id: generatedDevId,
        name,
        type,
        manufacturer: effectiveMfg || 'Generic',
        model: effectiveModel || 'Generic',
        icon
      };

      if (isPlugin) {
        devPayload.type = type;
        devPayload.plugin = pluginId;
        const cfg = {};
        if (pluginDef?.device_definition?.config_fields) {
          for (const f of pluginDef.device_definition.config_fields) {
            const inp = document.getElementById(`plug_cfg_${f.key}`);
            if (pluginFieldIsBoolean(f)) {
              cfg[f.key] = inp ? !!inp.checked : configFlagOn(f.default);
              continue;
            }
            const val = inp ? inp.value : '';
            cfg[f.key] = (f.type === 'number') ? Number(val || f.default || 0) : (val !== undefined && val !== null && val !== '' ? val : (f.default !== undefined ? f.default : ''));
          }
        }
        devPayload.config = cfg;

        const defaultCmds = pluginDef?.device_definition?.default_commands;
        if (defaultCmds && defaultCmds.length > 0) {
          devPayload.commands = defaultCmds.map(c => pluginDefaultCommand(c, pluginId));
        }
      }
      if (type === 'MQTT') {
        devPayload.mqttTopic = `{base_topic}/button/${name.toLowerCase().replace(/\s+/g, '_')}/{command}`;
        devPayload.mqttEnabled = 1;
        devPayload.mqttPulseMs = 1000;
      }
      const res = await this.client.saveNewDevice(devPayload);
      if (res && res.ok === false) {
        throw new Error(res.error || res.message || t('failed_save_device_hub'));
      }
      const newDevId = res?.deviceId || res?.id || generatedDevId;
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
      if (status) status.innerHTML = `<span style="color:var(--success)">✓ ${t('device_created_ok')}</span>`;
      const nameInput = document.getElementById('newDeviceName');
      const mfgInput = document.getElementById('newDeviceManufacturer');
      const modelInput = document.getElementById('newDeviceModel');
      if (nameInput) nameInput.value = '';
      if (mfgInput) mfgInput.value = '';
      if (modelInput) modelInput.value = '';
      const box = document.getElementById('studioNewDeviceBox');
      if (box) {
        setTimeout(() => {
          box.style.display = 'none';
          if (status) status.textContent = '';
        }, 1500);
      }
      await this.refreshData();
      if (newDevId) this.selectedIrDeviceId = String(newDevId);
      this.renderIrView(newDevId);
    } catch (err) {
      if (status) status.innerHTML = `<span style="color:var(--danger)">${escapeHtml(tf('error_msg', { error: err.message }))}</span>`;
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

    this.powerSeqOwnerId = String(dev.id); // the step rows show and save this device (addSequenceStep)
    const pOn = dev.powerOnSequence || dev.power_on_sequence || dev.PowerOnActions || [];
    const pOff = dev.powerOffSequence || dev.power_off_sequence || dev.PowerOffActions || [];
    this.renderSequenceList('powerOnStepsList', Array.isArray(pOn) ? pOn : []);
    this.renderSequenceList('powerOffStepsList', Array.isArray(pOff) ? pOff : []);

    const stat = document.getElementById('powerSeqStatus');
    if (stat) stat.textContent = '';
    modal.style.display = 'flex';
  }

  closePowerSequenceModal() {
    const modal = document.getElementById('powerSeqModal');
    if (modal) modal.style.display = 'none';
    this.powerSeqOwnerId = null; // only the power step lists read it; cleared so nothing else can
  }

  async saveDevicePowerSequences() {
    const devId = this.selectedIrDeviceId || document.getElementById('manageDeviceSelect')?.value;
    const dev = (this.inventory.devices || []).find(d => String(d.id) === String(devId));
    if (!dev) return;
    const stat = document.getElementById('powerSeqStatus');
    if (stat) stat.textContent = t('saving_sequences');
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
      if (stat) stat.innerHTML = `<span style="color:var(--success)">${t('saved_status')}</span>`;
      setTimeout(() => this.closePowerSequenceModal(), 1000);
      await this.refreshData();
    } catch (err) {
      if (stat) stat.innerHTML = `<span style="color:var(--danger)">${escapeHtml(tf('error_msg', { error: err.message }))}</span>`;
    }
  }

  updateStudioAddPanels(dev) {
    if (!dev) return;
    const isNet = this.isNetDevice(dev);
    const netTabsRow = document.getElementById('netAddTabsRow');
    const pNet = document.getElementById('addModeNet');
    if (netTabsRow) netTabsRow.style.display = isNet ? 'flex' : 'none';
    if (pNet) pNet.style.display = isNet ? 'block' : 'none';
    if (isNet) {
      for (const id of ['irAddTabsRow', 'btAddTabsRow', 'mqttAddTabsRow', 'pluginAddTabsRow', 'addModeLearn', 'addModeDb',
        'addModePaste', 'addModeBtPresets', 'addModeBtCustom', 'addModeMqtt', 'addModePluginPresets', 'addModePluginCustom']) {
        const el = document.getElementById(id);
        if (el) el.style.display = 'none';
      }
      this.renderNetPanel(dev);
      return;
    }
    const isPlugin = !!(dev.plugin || (dev.type && dev.type.startsWith('plugin:')));
    const isBt = !isPlugin && (dev.type === 'BT' || dev.type === 'Bluetooth' || dev.isBt);
    const isMqtt = !isPlugin && !isBt && (dev.type === 'MQTT' || dev.mqttEnabled);
    const isIr = !isPlugin && !isBt && !isMqtt;

    const irTabsRow = document.getElementById('irAddTabsRow');
    const btTabsRow = document.getElementById('btAddTabsRow');
    const mqttTabsRow = document.getElementById('mqttAddTabsRow');
    const pluginTabsRow = document.getElementById('pluginAddTabsRow');

    if (irTabsRow) irTabsRow.style.display = isIr ? 'flex' : 'none';
    if (btTabsRow) btTabsRow.style.display = isBt ? 'flex' : 'none';
    if (mqttTabsRow) mqttTabsRow.style.display = isMqtt ? 'flex' : 'none';
    if (pluginTabsRow) pluginTabsRow.style.display = isPlugin ? 'flex' : 'none';

    const pLearn = document.getElementById('addModeLearn');
    const pDb = document.getElementById('addModeDb');
    const pPaste = document.getElementById('addModePaste');
    const pBtPresets = document.getElementById('addModeBtPresets');
    const pBtCustom = document.getElementById('addModeBtCustom');
    const pMqtt = document.getElementById('addModeMqtt');
    const pPlugPresets = document.getElementById('addModePluginPresets');
    const pPlugCustom = document.getElementById('addModePluginCustom');

    if (isPlugin) {
      if (pLearn) pLearn.style.display = 'none';
      if (pDb) pDb.style.display = 'none';
      if (pPaste) pPaste.style.display = 'none';
      if (pBtPresets) pBtPresets.style.display = 'none';
      if (pBtCustom) pBtCustom.style.display = 'none';
      if (pMqtt) pMqtt.style.display = 'none';

      const activePlugTab = document.querySelector('.plug-add-tab.btn-primary')?.getAttribute('data-plug-mode') || 'presets';
      if (pPlugPresets) pPlugPresets.style.display = (activePlugTab === 'presets') ? 'block' : 'none';
      if (pPlugCustom) pPlugCustom.style.display = (activePlugTab === 'custom') ? 'block' : 'none';

      this.renderPluginPresetsList(dev);
    } else if (isBt) {
      if (pLearn) pLearn.style.display = 'none';
      if (pDb) pDb.style.display = 'none';
      if (pPaste) pPaste.style.display = 'none';
      if (pMqtt) pMqtt.style.display = 'none';
      if (pPlugPresets) pPlugPresets.style.display = 'none';
      if (pPlugCustom) pPlugCustom.style.display = 'none';

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
      if (pPlugPresets) pPlugPresets.style.display = 'none';
      if (pPlugCustom) pPlugCustom.style.display = 'none';
      if (pMqtt) pMqtt.style.display = 'block';

      this.updateMqttTopicPreview(dev);
    } else if (isIr) {
      if (pBtPresets) pBtPresets.style.display = 'none';
      if (pBtCustom) pBtCustom.style.display = 'none';
      if (pMqtt) pMqtt.style.display = 'none';
      if (pPlugPresets) pPlugPresets.style.display = 'none';
      if (pPlugCustom) pPlugCustom.style.display = 'none';

      const activeTab = document.querySelector('.ir-add-tab.btn-primary')?.getAttribute('data-add-mode') || 'learn';
      if (pLearn) pLearn.style.display = (activeTab === 'learn') ? 'block' : 'none';
      if (pDb) pDb.style.display = (activeTab === 'database') ? 'block' : 'none';
      if (pPaste) pPaste.style.display = (activeTab === 'paste') ? 'block' : 'none';
    }
  }

  async renderPluginPresetsList(dev) {
    const cont = document.getElementById('plugPresetsContainer');
    if (!cont) return;
    const filter = (document.getElementById('plugPresetsFilter')?.value || '').toLowerCase().trim();
    const pluginId = dev.plugin || (dev.type && dev.type.startsWith('plugin:') ? dev.type.substring(7) : '');

    if ((!this.installedPlugins || this.installedPlugins.length === 0) && this.client && typeof this.client.getPlugins === 'function') {
      try {
        const res = await this.client.getPlugins();
        this.installedPlugins = Array.isArray(res) ? res : (res?.plugins || []);
      } catch (_) {}
    }

    const plug = (this.installedPlugins || []).find(p => p.id === pluginId);
    const defCmds = plug?.device_definition?.default_commands || plug?.manifest?.device_definition?.default_commands || [];

    const existingCmds = dev.commands || [];
    const existingNames = new Set(existingCmds.map(c => (typeof c === 'string' ? c : (c.name || '')).toLowerCase()));

    const filtered = defCmds.filter(c => !filter || c.name.toLowerCase().includes(filter));
    if (filtered.length === 0) {
      cont.innerHTML = `<div class="muted mini" style="padding: 12px; text-align: center;">${defCmds.length === 0 ? t('plg_no_default_cmds') : t('plg_no_cmds_match')}</div>`;
      return;
    }

    cont.innerHTML = filtered.map(c => {
      const isAdded = existingNames.has(c.name.toLowerCase());
      return `
        <div style="display: flex; justify-content: space-between; align-items: center; padding: 6px 10px; background: var(--bg-card); border-radius: var(--radius-sm); border: 1px solid var(--border);">
          <div>
            <strong style="font-size: 12px; color: var(--text-main);">${escapeHtml(c.name)}</strong>
            <span class="subtle mini" style="margin-left: 6px; font-family: monospace; font-size: 10px;">${escapeHtml(c.keyCode || '')}</span>
            ${irAltBadgeHtml(c, dev.config)}
          </div>
          <div>
            ${isAdded ? `<span class="badge ok mini">${t('plg_added')}</span>` : `<button type="button" class="btn btn-xs btn-primary btn-plug-preset-add" data-name="${escapeHtml(c.name)}" data-code="${escapeHtml(c.keyCode || '')}">${t('plg_add_one')}</button>`}
          </div>
        </div>
      `;
    }).join('');

    cont.querySelectorAll('.btn-plug-preset-add').forEach(btn => {
      btn.addEventListener('click', async () => {
        const name = btn.getAttribute('data-name');
        const keyCode = btn.getAttribute('data-code');
        btn.textContent = '...';
        btn.disabled = true;
        try {
          await this.client.saveLearnedCommand({
            deviceId: dev.id,
            name,
            raw: keyCode,
            mode: 'plugin'
          });
          await this.refreshData();
          const updatedDev = (this.inventory.devices || []).find(d => String(d.id) === String(dev.id));
          if (updatedDev) this.updateStudioDeviceView(updatedDev);
        } catch (err) {
          alert(t('failed_to_save', 'Failed to save: ') + err.message);
          btn.textContent = t('plg_add_one');
          btn.disabled = false;
        }
      });
    });
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
               (k.descKey ? t(k.descKey) : '').toLowerCase().includes(filter) ||
               k.code.toLowerCase().includes(filter);
      });

      if (filteredKeys.length === 0) return;

      html += `
        <div style="background: var(--bg-dark); border: 1px solid var(--border); border-radius: var(--radius-sm); padding: 8px 10px;">
          <div style="font-size: 11px; font-weight: 600; color: var(--text-dim); margin-bottom: 6px; text-transform: uppercase; letter-spacing: 0.5px;">${escapeHtml(group.categoryKey ? t(group.categoryKey) : group.category)}</div>
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
              <span class="muted mini" style="font-size: 11px; text-overflow: ellipsis; overflow: hidden; white-space: nowrap;">${escapeHtml(k.descKey ? t(k.descKey) : k.desc)}</span>
            </div>
            <div style="display: flex; align-items: center; gap: 4px; flex-shrink: 0;">
              <button type="button" class="btn btn-xs btn-ghost btn-test-bt-preset" data-code="${escapeHtml(k.code)}" title="${escapeHtml(t('test_key_to_host_title'))}">▶</button>
              ${isAdded ?
                `<span class="badge subtle mini" style="font-size: 10px;">${t('badge_added')}</span>` :
                `<button type="button" class="btn btn-xs btn-primary btn-add-bt-preset" data-name="${escapeHtml(k.name)}" data-code="${escapeHtml(k.code)}">+ ${t('add')}</button>`
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
      cont.innerHTML = `<div class="muted mini" style="padding: 16px; text-align: center;">${t('no_matching_bt_keys')}</div>`;
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
          btn.textContent = `+ ${t('add')}`;
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

  /* ---- HTTP(S) and TCP devices (payload/source/net_device.h) ---- */

  isNetDevice(dev) {
    return !!dev && !dev.plugin && (dev.type === 'HTTP' || dev.type === 'TCP');
  }

  netSelectedDevice() {
    const devs = this.inventory?.devices || [];
    return devs.find(d => String(d.id) === String(this.selectedDeviceId)) || null;
  }

  netStatus(id, ok, text) {
    const el = document.getElementById(id);
    if (!el) return;
    if (!text) el.innerHTML = '';
    else if (ok) el.innerHTML = `<span style="color:var(--success)">${escapeHtml(text)}</span>`;
    else el.innerHTML = `<span style="color:var(--danger)">${escapeHtml(text)}</span>`;
  }

  renderNetPanel(dev) {
    const isHttp = dev.type === 'HTTP';
    const set = (id, v) => { const el = document.getElementById(id); if (el) el.value = v; };
    if (this._netPanelDevId !== String(dev.id)) {
      // filled once per device: a refresh does not overwrite what is being typed
      this._netPanelDevId = String(dev.id);
      set('netHost', dev.netHost || '');
      set('netPort', dev.netPort > 0 ? String(dev.netPort) : '');
      set('netTimeoutMs', String(dev.netTimeoutMs || 3000));
      const tls = document.getElementById('netTls');
      if (tls) tls.checked = !!dev.netTls;
      set('netTlsVerify', dev.netTlsVerify || 'verify');
      set('netHeaders', (dev.httpHeaders || []).map(h => `${h.name}: ${h.value || ''}`).join('\n'));
      set('netAuthType', dev.httpAuthType || 'none');
      set('netAuthUser', dev.httpUser || '');
      set('netAuthPass', '');
      set('netAuthToken', '');
      set('netCmdName', '');
      set('netCmdMethod', 'GET');
      set('netCmdPath', '');
      set('netCmdBody', '');
      set('netCmdContentType', '');
      set('netCmdPayload', '');
      set('netCmdFormat', 'text');
      this.netStatus('netConnStatus', true, '');
      this.netStatus('netCmdStatus', true, '');
    }
    const pass = document.getElementById('netAuthPass');
    if (pass) pass.placeholder = dev.hasHttpPassword ? t('keep_current_pwd_lbl') : '';
    const token = document.getElementById('netAuthToken');
    if (token) token.placeholder = dev.hasHttpToken ? t('net_keep_current_token') : '';
    const show = (id, on) => { const el = document.getElementById(id); if (el) el.style.display = on ? '' : 'none'; };
    show('netHttpOnly', isHttp);
    show('netCmdHttp', isHttp);
    show('netCmdTcp', !isHttp);
    this.updateNetPanelVisibility();
    this.bindNetPanelEvents();
  }

  updateNetPanelVisibility() {
    const show = (id, on) => { const el = document.getElementById(id); if (el) el.style.display = on ? '' : 'none'; };
    const tls = !!document.getElementById('netTls')?.checked;
    const auth = document.getElementById('netAuthType')?.value || 'none';
    show('netTlsVerifyWrap', tls);
    show('btnNetForgetPin', tls && document.getElementById('netTlsVerify')?.value === 'pin');
    show('netAuthUserWrap', auth === 'basic');
    show('netAuthPassWrap', auth === 'basic');
    show('netAuthTokenWrap', auth === 'bearer');
  }

  bindNetPanelEvents() {
    if (this._netPanelBound) return;
    this._netPanelBound = true;
    for (const id of ['netTls', 'netTlsVerify', 'netAuthType']) {
      document.getElementById(id)?.addEventListener('change', () => this.updateNetPanelVisibility());
    }
    document.getElementById('btnNetSaveConn')?.addEventListener('click', () => this.saveNetConnection(false));
    document.getElementById('btnNetTestConn')?.addEventListener('click', () => this.saveNetConnection(true));
    document.getElementById('btnNetForgetPin')?.addEventListener('click', () => this.forgetNetPin());
    document.getElementById('btnNetSaveCmd')?.addEventListener('click', () => this.saveNetCommand(false));
    document.getElementById('btnNetTestCmd')?.addEventListener('click', () => this.saveNetCommand(true));
  }

  /* "Name: value" lines; a line without a name and a colon is an error */
  parseNetHeaders(text) {
    const out = [];
    for (const line of String(text || '').split('\n')) {
      const l = line.trim();
      if (!l) continue;
      const i = l.indexOf(':');
      if (i <= 0) throw new Error(tf('net_bad_header_line', { line: l }));
      out.push({ name: l.slice(0, i).trim(), value: l.slice(i + 1).trim() });
    }
    return out;
  }

  netTestText(r) {
    if (!r) return t('net_test_failed');
    const parts = [];
    if (r.status) parts.push(`HTTP ${r.status}`);
    if (r.error) parts.push(r.error);
    if (r.pinned) parts.push(t('net_key_pinned'));
    if (r.tlsFingerprint) parts.push(tf('net_key_fingerprint', { fp: r.tlsFingerprint.slice(0, 16) }));
    if (r.response) parts.push(r.response.slice(0, 120));
    return (r.ok ? '✓ ' : '') + (parts.join(' · ') || (r.ok ? t('net_test_ok') : t('net_test_failed')));
  }

  async saveNetConnection(andTest) {
    const dev = this.netSelectedDevice();
    if (!dev) return;
    const val = id => (document.getElementById(id)?.value || '').trim();
    try {
      const payload = {
        id: String(dev.id),
        type: dev.type,
        netHost: val('netHost'),
        netPort: Number(val('netPort') || 0),
        netTls: !!document.getElementById('netTls')?.checked,
        netTlsVerify: val('netTlsVerify') || 'verify',
        netTimeoutMs: Number(val('netTimeoutMs') || 3000)
      };
      if (dev.type === 'HTTP') {
        payload.httpHeaders = this.parseNetHeaders(document.getElementById('netHeaders')?.value);
        payload.httpAuthType = val('netAuthType') || 'none';
        payload.httpUser = val('netAuthUser');
        // empty: the Hub keeps the stored secret (GET never returns it)
        if (val('netAuthPass')) payload.httpPassword = document.getElementById('netAuthPass').value;
        if (val('netAuthToken')) payload.httpToken = document.getElementById('netAuthToken').value;
      }
      this.netStatus('netConnStatus', true, t('net_saving'));
      const res = await this.client.saveDeviceNet(payload);
      if (res && res.ok === false) throw new Error(res.error || t('failed_save_device_hub'));
      this._netPanelDevId = null; // refill from the saved device
      await this.refreshData();
      if (andTest) {
        this.netStatus('netConnStatus', true, t('net_testing'));
        const r = await this.client.testDeviceNet(dev.id);
        this.netStatus('netConnStatus', !!r?.ok, this.netTestText(r));
      } else {
        this.netStatus('netConnStatus', true, '✓ ' + t('net_conn_saved'));
      }
    } catch (err) {
      this.netStatus('netConnStatus', false, tf('error_msg', { error: err.message }));
    }
  }

  async forgetNetPin() {
    const dev = this.netSelectedDevice();
    if (!dev) return;
    try {
      const r = await this.client.forgetDeviceNetPin(dev.id);
      this.netStatus('netConnStatus', true, r?.removed ? t('net_pin_forgotten') : t('net_no_pin'));
    } catch (err) {
      this.netStatus('netConnStatus', false, tf('error_msg', { error: err.message }));
    }
  }

  fillNetCommand(dev, c) {
    const set = (id, v) => { const el = document.getElementById(id); if (el) el.value = v; };
    set('netCmdName', c.name || '');
    set('netCmdMethod', c.httpMethod || 'GET');
    set('netCmdPath', c.httpPath || '');
    set('netCmdBody', c.httpBody || '');
    set('netCmdContentType', c.httpContentType || '');
    set('netCmdPayload', c.tcpPayload || '');
    set('netCmdFormat', c.tcpFormat || 'text');
    this.netStatus('netCmdStatus', true, '');
    document.getElementById('netCmdName')?.focus();
  }

  async saveNetCommand(andTest) {
    const dev = this.netSelectedDevice();
    if (!dev) return;
    const val = id => document.getElementById(id)?.value || '';
    const name = val('netCmdName').trim();
    if (!name) {
      this.netStatus('netCmdStatus', false, t('net_cmd_name_required'));
      return;
    }
    const data = { deviceId: String(dev.id), name, raw: '' };
    if (dev.type === 'HTTP') {
      data.httpMethod = val('netCmdMethod') || 'GET';
      data.httpPath = val('netCmdPath').trim() || '/';
      data.httpBody = val('netCmdBody');
      data.httpContentType = val('netCmdContentType').trim();
    } else {
      data.tcpPayload = val('netCmdPayload');
      data.tcpFormat = val('netCmdFormat') || 'text';
    }
    try {
      const res = await this.client.saveLearnedCommand(data);
      if (res && res.ok === false) throw new Error(res.error || t('failed_save_device_hub'));
      await this.refreshData();
      if (andTest) {
        this.netStatus('netCmdStatus', true, t('net_testing'));
        const r = await this.client.testDeviceNet(dev.id, name);
        this.netStatus('netCmdStatus', !!r?.ok, this.netTestText(r));
      } else {
        this.netStatus('netCmdStatus', true, '✓ ' + t('net_cmd_saved'));
      }
    } catch (err) {
      this.netStatus('netCmdStatus', false, tf('error_msg', { error: err.message }));
    }
  }

}
