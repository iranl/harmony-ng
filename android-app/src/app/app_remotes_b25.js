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
import { HarmonyAppHarmonySearch } from './app_harmony_search.js';

export class HarmonyAppRemotesB25 extends HarmonyAppHarmonySearch {
  async loadRemotesView() {
    const sel = document.getElementById('b25ActivitySelect');
    if (sel) {
      sel.innerHTML = '<option value="-1">Off / Idle (Default)</option>' +
        (this.activitiesData.activities || []).map(a => `<option value="${escapeHtml(a.id)}">${escapeHtml(a.name || a.label)}</option>`).join('');
    }
    const tgtDev = document.getElementById('b25TargetDevice');
    const presetDev = document.getElementById('b25PresetBtDevice');
    const devs = this.inventory.devices || [];
    if (tgtDev) tgtDev.innerHTML = devs.map(d => `<option value="${escapeHtml(d.id)}">${escapeHtml(d.name || d.id)}</option>`).join('');
    if (presetDev) presetDev.innerHTML = devs.map(d => `<option value="${escapeHtml(d.id)}">${escapeHtml(d.name || d.id)}</option>`).join('');

    const actSel = document.getElementById('b25TargetActivity');
    if (actSel) {
      actSel.innerHTML = (this.activitiesData.activities || []).map(a => `<option value="${escapeHtml(a.id)}">${escapeHtml(a.name || a.label)}</option>`).join('');
    }

    try {
      const [mRes, stRes] = await Promise.allSettled([
        this.client.getRemoteMappings(),
        this.client.getBtRemotePairStatus()
      ]);
      if (mRes.status === 'fulfilled' && mRes.value) {
        this.b25MapData = mRes.value;
      }
      const st = stRes.status === 'fulfilled' ? stRes.value : null;
      this.updateB25StatusUI(st);

    } catch { }

    this.selectB25Button(this.b25SelectedBtn || 'power', false);
    this.updateB25ButtonStates();

    if (this._b25PollTimer) {
      clearInterval(this._b25PollTimer);
      this._b25PollTimer = null;
    }
    this._b25PollTimer = setInterval(async () => {
      const v = document.getElementById('view-remotes');
      if (!v || !v.classList.contains('active')) {
        clearInterval(this._b25PollTimer);
        this._b25PollTimer = null;
        return;
      }
      try {
        const st = await this.client.getBtRemotePairStatus();
        this.updateB25StatusUI(st);
      } catch { }
    }, 2000);
  }

  updateB25StatusUI(st) {
    const pairedName = document.getElementById('b25PairedName');
    const pairedAddr = document.getElementById('b25PairedAddr');
    const stateText = document.getElementById('b25StateText');
    const badge = document.getElementById('b25StatusBadge');
    const scanBtn = document.getElementById('b25ScanBtn');
    const discList = document.getElementById('b25DiscoveredList');

    const bdaddr = st?.bdaddr || this.b25MapData?.remote?.bdaddr || this.b25MapData?.bdaddr || '';
    const name = st?.name || this.b25MapData?.remote?.name || 'Homatics B25';
    const isConn = Boolean(st?.connected || (bdaddr && st?.running && (st?.state === 'ready' || st?.state === 'connected')));
    const isScanning = Boolean(st?.scanning);

    if (pairedName) pairedName.textContent = name;
    if (pairedAddr) pairedAddr.textContent = bdaddr || 'None';

    if (isScanning) {
      if (stateText) stateText.textContent = 'Scanning for BLE remotes...';
      if (badge) {
        badge.className = 'badge warn';
        badge.textContent = 'SCANNING';
      }
    } else {
      if (stateText) stateText.textContent = st?.state || (isConn ? 'Connected / Ready' : (bdaddr ? 'Paired / Offline' : 'Idle'));
      if (badge) {
        badge.className = `badge ${isConn ? 'ok' : (bdaddr ? 'warn' : '')}`;
        badge.textContent = isConn ? 'CONNECTED' : (bdaddr ? 'PAIRED' : 'OFFLINE');
      }
    }

    if (scanBtn) {
      if (isScanning) {
        scanBtn.textContent = 'Stop Scanning';
        scanBtn.classList.remove('btn-primary');
        scanBtn.classList.add('btn-warning');
        scanBtn.setAttribute('data-scanning', 'true');
      } else {
        scanBtn.textContent = 'Scan for BLE Remotes';
        scanBtn.classList.remove('btn-warning');
        scanBtn.classList.add('btn-primary');
        scanBtn.removeAttribute('data-scanning');
      }
    }

    if (discList && Array.isArray(st?.devices)) {
      const curVal = discList.value;
      if (st.devices.length === 0) {
        if (!isScanning && discList.options.length <= 1) {
          discList.innerHTML = '<option value="">(No remotes found)</option>';
        }
      } else {
        const opts = ['<option value="">(Select remote - ' + st.devices.length + ' found)</option>'];
        for (const d of st.devices) {
          const addr = d.bdaddr || d.addr || '';
          const dname = d.name || 'Bluetooth Remote';
          const rssi = (typeof d.rssi === 'number') ? ` (${d.rssi} dBm)` : '';
          opts.push(`<option value="${escapeHtml(addr)}">${escapeHtml(dname)} - ${escapeHtml(addr)}${escapeHtml(rssi)}</option>`);
        }
        discList.innerHTML = opts.join('');
        if (curVal) discList.value = curVal;
      }
    }
  }

  selectB25Button(btnId, openModal = false) {
    this.b25SelectedBtn = btnId;
    document.querySelectorAll('.b25-hotspot').forEach(b => {
      b.classList.toggle('selected', b.getAttribute('data-btn') === btnId);
    });
    const lbl = document.getElementById('b25CurrentBtnName');
    if (lbl) lbl.textContent = btnId;

    const actSel = document.getElementById('b25ActivitySelect');
    const actName = actSel ? actSel.options[actSel.selectedIndex]?.text || 'Off / Idle' : 'Off / Idle';
    const modalActLbl = document.getElementById('b25ModalActivityName');
    if (modalActLbl) modalActLbl.textContent = actName;

    const actId = actSel?.value || '-1';
    const mapping = this.b25MapData?.activities?.[actId]?.[btnId];

    const typeSel = document.getElementById('b25ActionType');
    if (typeSel) {
      if (!mapping) typeSel.value = 'unmapped';
      else if (mapping.action) typeSel.value = mapping.action;
      else if (mapping.type) typeSel.value = mapping.type;
      else if (mapping.command) typeSel.value = 'device_cmd';
      this.toggleB25ActionFields();
    }

    const devId = mapping?.deviceId || mapping?.targetDevice;
    if (devId) {
      const devSel = document.getElementById('b25TargetDevice');
      if (devSel) devSel.value = devId;
    }
    const actTargetId = mapping?.activityId || mapping?.targetActivity;
    if (actTargetId) {
      const actTargetSel = document.getElementById('b25TargetActivity');
      if (actTargetSel) actTargetSel.value = actTargetId;
    }
    this.updateB25CommandDropdown(mapping?.command);

    if (openModal) {
      const m = document.getElementById('b25BtnModal');
      if (m) m.style.display = 'flex';
    }
  }

  updateB25CommandDropdown(curVal) {
    const devId = document.getElementById('b25TargetDevice')?.value;
    const cmdSel = document.getElementById('b25TargetCommand');
    if (!cmdSel) return;
    const dev = (this.inventory.devices || []).find(d => String(d.id) === String(devId));
    cmdSel.innerHTML = (dev?.commands || []).map(c => `<option value="${escapeHtml(c.name)}" ${c.name === curVal ? 'selected' : ''}>${escapeHtml(c.name)}</option>`).join('');
  }

  toggleB25ActionFields() {
    const t = document.getElementById('b25ActionType')?.value;
    const wDev = document.getElementById('b25WrapTargetDevice');
    const wCmd = document.getElementById('b25WrapTargetCommand');
    const wAct = document.getElementById('b25WrapTargetActivity');

    if (wDev) wDev.style.display = t === 'device_cmd' ? 'block' : 'none';
    if (wCmd) wCmd.style.display = t === 'device_cmd' ? 'block' : 'none';
    if (wAct) wAct.style.display = t === 'activity_start' ? 'block' : 'none';
  }

  applyB25Mapping() {
    const actId = document.getElementById('b25ActivitySelect')?.value || '-1';
    const type = document.getElementById('b25ActionType')?.value;
    if (!this.b25MapData.activities) this.b25MapData.activities = {};
    if (!this.b25MapData.activities[actId]) this.b25MapData.activities[actId] = {};

    if (type === 'unmapped') {
      delete this.b25MapData.activities[actId][this.b25SelectedBtn];
    } else if (type === 'device_cmd') {
      const devId = document.getElementById('b25TargetDevice')?.value;
      const cmd = document.getElementById('b25TargetCommand')?.value;
      this.b25MapData.activities[actId][this.b25SelectedBtn] = {
        action: 'device_cmd',
        type: 'device_cmd',
        deviceId: devId,
        targetDevice: devId,
        command: cmd
      };
    } else if (type === 'activity_start') {
      const actTarget = document.getElementById('b25TargetActivity')?.value;
      this.b25MapData.activities[actId][this.b25SelectedBtn] = {
        action: 'activity_start',
        type: 'activity_start',
        activityId: actTarget,
        targetActivity: actTarget
      };
    } else if (type === 'activity_stop') {
      this.b25MapData.activities[actId][this.b25SelectedBtn] = {
        action: 'activity_stop',
        type: 'activity_stop'
      };
    }
    this.updateB25ButtonStates();
    const modal = document.getElementById('b25BtnModal');
    if (modal) modal.style.display = 'none';
  }

  clearB25Mapping() {
    const actId = document.getElementById('b25ActivitySelect')?.value || '-1';
    if (this.b25MapData.activities?.[actId]) {
      delete this.b25MapData.activities[actId][this.b25SelectedBtn];
    }
    this.selectB25Button(this.b25SelectedBtn, false);
    this.updateB25ButtonStates();
    const modal = document.getElementById('b25BtnModal');
    if (modal) modal.style.display = 'none';
  }

  updateB25ButtonStates() {
    const actId = document.getElementById('b25ActivitySelect')?.value || '-1';
    const actMap = this.b25MapData?.activities?.[actId] || {};
    document.querySelectorAll('.b25-hotspot').forEach(b => {
      const id = b.getAttribute('data-btn');
      b.classList.toggle('mapped', Boolean(actMap[id]));
    });
  }

  async saveAllB25Mappings() {
    const btn = document.getElementById('b25SaveAllBtn');
    try {
      if (btn) btn.textContent = 'Saving...';
      await this.client.saveRemoteMappings(this.b25MapData);
      alert(t("b25_saved_hub"));
    } catch (err) {
      alert(t('save_failed', 'Save failed: ') + err.message);
    } finally {
      if (btn) btn.textContent = 'Save All Mappings';
    }
  }

  autoMapB25Preset() {
    const devId = document.getElementById('b25PresetBtDevice')?.value;
    const actId = document.getElementById('b25ActivitySelect')?.value || '-1';
    if (!devId) return alert(t('select_target_dev_first', 'Select target device first.'));
    const dev = (this.inventory.devices || []).find(d => String(d.id) === String(devId));
    if (!dev || !dev.commands) return alert(t("dev_has_no_cmds"));

    const norm = s => String(s || '').toLowerCase().replace(/[^a-z0-9]/g, '');
    const aliases = {
      power: ['power', 'poweroff', 'standby', 'powertoggle'],
      up: ['up', 'directionup'],
      down: ['down', 'directiondown'],
      left: ['left', 'directionleft'],
      right: ['right', 'directionright'],
      select: ['ok', 'select', 'enter'],
      back: ['back', 'return', 'exit'],
      home: ['home'],
      vol_up: ['volumeup', 'volup'],
      vol_down: ['volumedown', 'voldown'],
      mute: ['mute']
    };

    if (!this.b25MapData.activities) this.b25MapData.activities = {};
    if (!this.b25MapData.activities[actId]) this.b25MapData.activities[actId] = {};

    let mapped = 0;
    Object.entries(aliases).forEach(([btn, list]) => {
      for (const a of list) {
        const cmd = dev.commands.find(c => norm(c.name) === norm(a));
        if (cmd) {
          this.b25MapData.activities[actId][btn] = { type: 'device_cmd', deviceId: devId, command: cmd.name };
          mapped++;
          break;
        }
      }
    });

    this.updateB25ButtonStates();
    alert(t("auto_mapped_b25").replace("{count}", mapped));
  }

  async scanB25Remote() {
    const btn = document.getElementById('b25ScanBtn');
    const log = document.getElementById('b25PairLog');
    const isScanning = btn && btn.getAttribute('data-scanning') === 'true';

    try {
      if (isScanning) {
        if (btn) btn.textContent = 'Stopping...';
        if (log) log.textContent = 'Stopping scan...';
        await this.client.stopScanBtRemote();
      } else {
        if (btn) {
          btn.textContent = 'Starting scan...';
          btn.classList.remove('btn-primary');
          btn.classList.add('btn-warning');
          btn.setAttribute('data-scanning', 'true');
        }
        if (log) log.textContent = 'Scanning for BLE remotes (put remote in pairing mode)...';
        await this.client.scanBtRemote();
      }
      const st = await this.client.getBtRemotePairStatus().catch(() => null);
      if (st) this.updateB25StatusUI(st);
    } catch (err) {
      if (log) log.textContent = (isScanning ? 'Stop scan error: ' : 'Scan error: ') + err.message;
      const st = await this.client.getBtRemotePairStatus().catch(() => null);
      if (st) this.updateB25StatusUI(st);
    }
  }

  async pairB25Remote() {
    const addr = document.getElementById('b25DiscoveredList')?.value;
    const log = document.getElementById('b25PairLog');
    const btn = document.getElementById('b25PairBtn');
    if (!addr) return alert(t("select_discovered_remote", "Select a discovered remote first."));
    try {
      if (btn) btn.disabled = true;
      if (log) log.textContent = `Pairing with ${addr}... Put remote in pairing mode.`;
      await this.client.pairBtRemote(addr);
      if (log) log.textContent = `Pairing initiated for ${addr}! Connecting...`;
      setTimeout(async () => {
        const st = await this.client.getBtRemotePairStatus().catch(() => null);
        if (st) this.updateB25StatusUI(st);
      }, 1500);
    } catch (err) {
      if (log) log.textContent = 'Pairing failed: ' + err.message;
    } finally {
      if (btn) btn.disabled = false;
    }
  }

}
