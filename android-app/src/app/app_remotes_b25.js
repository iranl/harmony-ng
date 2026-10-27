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
        (this.activitiesData.activities || []).map(a => `<option value="${a.id}">${a.name || a.label}</option>`).join('');
    }
    const tgtDev = document.getElementById('b25TargetDevice');
    const presetDev = document.getElementById('b25PresetBtDevice');
    const devs = this.inventory.devices || [];
    if (tgtDev) tgtDev.innerHTML = devs.map(d => `<option value="${d.id}">${d.name || d.id}</option>`).join('');
    if (presetDev) presetDev.innerHTML = devs.map(d => `<option value="${d.id}">${d.name || d.id}</option>`).join('');

    const actSel = document.getElementById('b25TargetActivity');
    if (actSel) {
      actSel.innerHTML = (this.activitiesData.activities || []).map(a => `<option value="${a.id}">${a.name || a.label}</option>`).join('');
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
    } catch {}

    this.selectB25Button(this.b25SelectedBtn || 'power', false);
    this.updateB25ButtonStates();
  }

  updateB25StatusUI(st) {
    const pairedName = document.getElementById('b25PairedName');
    const pairedAddr = document.getElementById('b25PairedAddr');
    const stateText = document.getElementById('b25StateText');
    const badge = document.getElementById('b25StatusBadge');

    const bdaddr = st?.bdaddr || this.b25MapData?.remote?.bdaddr || this.b25MapData?.bdaddr || '';
    const name = st?.name || this.b25MapData?.remote?.name || 'Homatics B25';
    const isConn = Boolean(st?.connected || (bdaddr && st?.running));

    if (pairedName) pairedName.textContent = name;
    if (pairedAddr) pairedAddr.textContent = bdaddr || 'None';
    if (stateText) stateText.textContent = st?.state || (isConn ? 'Connected / Ready' : (bdaddr ? 'Paired / Offline' : 'Idle'));
    if (badge) {
      badge.className = `badge ${isConn ? 'ok' : (bdaddr ? 'warn' : '')}`;
      badge.textContent = isConn ? 'CONNECTED' : (bdaddr ? 'PAIRED' : 'OFFLINE');
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

    const devId = mapping?.targetDevice || mapping?.deviceId;
    if (devId) {
      const devSel = document.getElementById('b25TargetDevice');
      if (devSel) devSel.value = devId;
    }
    const actTargetId = mapping?.targetActivity || mapping?.activityId;
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
    cmdSel.innerHTML = (dev?.commands || []).map(c => `<option value="${c.name}" ${c.name === curVal ? 'selected' : ''}>${c.name}</option>`).join('');
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
        targetDevice: devId,
        deviceId: devId,
        command: cmd
      };
    } else if (type === 'activity_start') {
      const actTarget = document.getElementById('b25TargetActivity')?.value;
      this.b25MapData.activities[actId][this.b25SelectedBtn] = {
        action: 'activity_start',
        type: 'activity_start',
        targetActivity: actTarget,
        activityId: actTarget
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
      alert('Save failed: ' + err.message);
    } finally {
      if (btn) btn.textContent = 'Save All Mappings';
    }
  }

  autoMapB25Preset() {
    const devId = document.getElementById('b25PresetBtDevice')?.value;
    const actId = document.getElementById('b25ActivitySelect')?.value || '-1';
    if (!devId) return alert('Select target device first.');
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
    try {
      if (btn) btn.textContent = 'Scanning...';
      if (log) log.textContent = 'Scanning for BLE remotes (10s)...';
      await this.client.scanBtRemote();
      setTimeout(async () => {
        const st = await this.client.getBtRemotePairStatus();
        if (log) log.textContent = st.message || 'Scan completed.';
        const list = document.getElementById('b25DiscoveredList');
        if (list && st.devices) {
          list.innerHTML = st.devices.map(d => `<option value="${d.bdaddr}">${d.name || d.bdaddr}</option>`).join('');
        }
      }, 4000);
    } catch (err) {
      if (log) log.textContent = 'Scan error: ' + err.message;
    } finally {
      if (btn) btn.textContent = 'Scan for Remote';
    }
  }

  async pairB25Remote() {
    const addr = document.getElementById('b25DiscoveredList')?.value;
    const log = document.getElementById('b25PairLog');
    if (!addr) return alert(t("select_discovered_remote"));
    try {
      if (log) log.textContent = `Pairing with ${addr}...`;
      await this.client.pairBtRemote(addr);
      if (log) log.textContent = `Paired with ${addr}!`;
    } catch (err) {
      if (log) log.textContent = 'Pairing failed: ' + err.message;
    }
  }

  /* -------------------------------------------------------------
   * 8b. Harmony Elite RF Remote View
   * ------------------------------------------------------------- */
}
