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
import { HarmonyAppRemotesB25 } from './app_remotes_b25.js';

export class HarmonyAppRemotesElite extends HarmonyAppRemotesB25 {
  async loadEliteView() {
    // Check RF enabled status from backend
    try {
      const svcs = await this.client.getServices();
      const rfBanner = document.getElementById('rfDisabledBanner');
      const rfBadge = document.getElementById('rfStatusBadge');
      const rfPairBtn = document.getElementById('rfPairBtn');
      const rfUnpairBtn = document.getElementById('rfUnpairBtn');

      if (svcs && svcs.rf_enabled === false) {
        if (rfBanner) rfBanner.style.display = 'block';
        if (rfBadge) {
          rfBadge.textContent = 'RF DISABLED';
          rfBadge.className = 'badge';
          rfBadge.style.background = 'rgba(255,183,3,0.15)';
          rfBadge.style.color = 'var(--warning)';
        }
        if (rfPairBtn) rfPairBtn.disabled = true;
        if (rfUnpairBtn) rfUnpairBtn.disabled = true;
      } else {
        if (rfBanner) rfBanner.style.display = 'none';
        if (rfPairBtn) rfPairBtn.disabled = false;
        if (rfUnpairBtn) rfUnpairBtn.disabled = false;
      }
    } catch (e) {
      console.warn('Services check failed:', e);
    }

    // Query RF status to check paired slots & remote models
    let rfStatus = null;
    try {
      rfStatus = await this.client.getRfStatus();
    } catch (e) {
      console.warn('Failed to get RF status:', e);
    }

    const s1 = rfStatus?.slots?.find(s => s.slot === 1);
    const s2 = rfStatus?.slots?.find(s => s.slot === 2);
    const s1Addr = s1?.address || s1?.rf_address || '';
    const s1Active = Boolean((s1?.active || rfStatus?.paired) && s1Addr && s1Addr !== 'None' && s1Addr !== '00000000');
    const s2Addr = s2?.address || s2?.rf_address || '';
    const s2Active = Boolean(s2?.active && s2Addr && s2Addr !== 'None' && s2Addr !== '00000000');

    const slotBar = document.getElementById('rfRemoteSlotSelectorBar');
    const noRemotesMsg = document.getElementById('rfNoRemotesPairedMsg');
    const mappingContainer = document.getElementById('rfRemoteMappingContainer');

    if (!s1Active && !s2Active) {
      if (slotBar) slotBar.style.display = 'none';
      if (mappingContainer) mappingContainer.style.display = 'none';
      if (noRemotesMsg) noRemotesMsg.style.display = 'block';
      return;
    }

    if (noRemotesMsg) noRemotesMsg.style.display = 'none';
    if (mappingContainer) mappingContainer.style.display = 'block';

    // Manage active slot selection
    if (!this.selectedRemoteSlot) this.selectedRemoteSlot = 1;
    if (this.selectedRemoteSlot === 1 && !s1Active && s2Active) this.selectedRemoteSlot = 2;
    if (this.selectedRemoteSlot === 2 && !s2Active && s1Active) this.selectedRemoteSlot = 1;

    // Show slot selector if both paired
    if (slotBar) {
      if (s1Active && s2Active) {
        slotBar.style.display = 'flex';
        const tab1 = document.getElementById('rfSlotSelect1Btn');
        const tab2 = document.getElementById('rfSlotSelect2Btn');
        const lbl1 = document.getElementById('rfSlotSelect1Label');
        const lbl2 = document.getElementById('rfSlotSelect2Label');
        if (lbl1) lbl1.textContent = s1?.name || 'Slot 1: Remote 1';
        if (lbl2) lbl2.textContent = s2?.name || 'Slot 2: Remote 2';
        if (tab1) tab1.classList.toggle('active', this.selectedRemoteSlot === 1);
        if (tab2) tab2.classList.toggle('active', this.selectedRemoteSlot === 2);
      } else {
        slotBar.style.display = 'none';
      }
    }

    const curSlot = this.selectedRemoteSlot;
    const curSlotInfo = curSlot === 1 ? s1 : s2;
    const curModel = curSlotInfo?.model || 'unknown';
    const isScreenless = curModel === 'smart_control' || curModel === 'companion' || curSlotInfo?.has_screen === false;

    // Toggle skin visual: Elite vs Companion
    const eliteShell = document.getElementById('eliteRemoteShell');
    const compShell = document.getElementById('companionRemoteShell');
    if (eliteShell) eliteShell.style.display = isScreenless ? 'none' : 'flex';
    if (compShell) compShell.style.display = isScreenless ? 'flex' : 'none';

    // Hide/show screen section
    const screenSec = document.getElementById('eliteSectionScreen');
    if (screenSec) screenSec.style.display = isScreenless ? 'none' : 'block';

    // Hide/show push to remote button (Elite only)
    const pushBtn = document.getElementById('elitePushRemoteBtn');
    if (pushBtn) pushBtn.style.display = isScreenless ? 'none' : 'inline-block';

    // Copy mappings button if both slots have same remote type
    const copyBtn = document.getElementById('rfCopySlotMappingBtn');
    if (copyBtn) {
      const sameType = s1Active && s2Active && (
        s1.model === s2.model ||
        ((s1.model === 'smart_control' || s1.model === 'companion') &&
         (s2.model === 'smart_control' || s2.model === 'companion'))
      );
      if (sameType) {
        const otherSlot = curSlot === 1 ? 2 : 1;
        copyBtn.style.display = 'inline-block';
        copyBtn.textContent = `📋 Copy Mappings from Slot ${otherSlot}`;
      } else {
        copyBtn.style.display = 'none';
      }
    }

    // MQTT status & config for this slot
    try {
      const mqRes = await this.client.getMqttStatus().catch(() => null);
      if (mqRes && mqRes.config) {
        const enCb = document.getElementById('eliteRemoteMqttEnabled');
        const topIn = document.getElementById('eliteRemoteMqttTopic');
        const badge = document.getElementById('eliteMqttStatusBadge');
        const lbl = document.getElementById('eliteRemoteMqttLabel');

        const slotEv = curSlot === 1 ?
          (mqRes.config.slot1RemoteEvents !== undefined ? mqRes.config.slot1RemoteEvents : (mqRes.config.eliteRemoteEvents !== undefined ? mqRes.config.eliteRemoteEvents : true)) :
          (mqRes.config.slot2RemoteEvents !== undefined ? mqRes.config.slot2RemoteEvents : false);
        const slotTop = curSlot === 1 ?
          (mqRes.config.slot1RemoteTopic || mqRes.config.eliteRemoteTopic || '') :
          (mqRes.config.slot2RemoteTopic || '');

        if (enCb) enCb.checked = Boolean(slotEv);
        if (topIn) topIn.value = slotTop;
        if (lbl) lbl.textContent = `Mirror Slot ${curSlot} (${curSlotInfo?.name || 'Remote'}) button presses to MQTT`;
        if (badge) {
          badge.textContent = mqRes.connected ? 'CONNECTED' : (mqRes.enabled ? 'ONLINE' : 'MQTT OFF');
          badge.className = 'badge ' + (mqRes.connected ? 'ok' : 'subtle');
        }
      }
    } catch (e) {
      console.warn('MQTT status check failed:', e);
    }

    const sel = document.getElementById('eliteActivitySelect');
    const prevActVal = this.eliteSelectedActivityId || sel?.value || '-1';
    const filteredActs = (this.activitiesData.activities || []).filter(a => {
      const idStr = String(a.id);
      const name = String(a.name || a.label || '').toLowerCase().replace(/\s+/g, '');
      return idStr !== '-1' && name !== 'poweroff';
    });
    if (sel) {
      sel.innerHTML = '<option value="-1">Off / Idle (Default)</option>' +
        filteredActs.map(a => `<option value="${a.id}">${a.name || a.label}</option>`).join('');
      if (prevActVal && (prevActVal === '-1' || filteredActs.some(a => String(a.id) === String(prevActVal)))) {
        sel.value = prevActVal;
      }
      this.eliteSelectedActivityId = sel.value;
    }
    const tgtDev = document.getElementById('eliteTargetDevice');
    const presetDev = document.getElementById('elitePresetDevice');
    const tgtAct = document.getElementById('eliteTargetActivity');
    const devs = this.inventory.devices || [];
    if (tgtDev) tgtDev.innerHTML = devs.map(d => `<option value="${d.id}">${d.name || d.id}</option>`).join('');
    if (presetDev) presetDev.innerHTML = devs.map(d => `<option value="${d.id}">${d.name || d.id}</option>`).join('');
    if (tgtAct) tgtAct.innerHTML = filteredActs.map(a => `<option value="${a.id}">${a.name || a.label}</option>`).join('');
    const devSelect = document.getElementById('eliteDeviceSelect');
    if (devSelect) {
      devSelect.innerHTML = devs.map(d => `<option value="${d.id}">${d.name || d.id}</option>`).join('');
      if (this.eliteSelectedDeviceId && devs.some(d => String(d.id) === String(this.eliteSelectedDeviceId))) {
        devSelect.value = this.eliteSelectedDeviceId;
      } else if (devs.length > 0) {
        devSelect.value = devs[0].id;
        this.eliteSelectedDeviceId = devs[0].id;
      }
    }

    try {
      const m = await this.client.getEliteMapping(this.selectedRemoteSlot);
      if (m) {
        if (!m.activities) m.activities = {};
        if (!m.devices) m.devices = {};
        this.eliteMapData = m;
      } else {
        this.eliteMapData = { activities: {}, devices: {} };
      }
    } catch (e) {
      console.warn(`Failed to load slot ${this.selectedRemoteSlot} mappings:`, e);
      this.eliteMapData = { activities: {}, devices: {} };
    }

    this.selectEliteButton('Select', false);
    this.updateEliteButtonStates();
    if (!isScreenless) {
      this.renderEliteScreenButtons();
    }
  }

  async switchRemoteSlot(slot) {
    if (slot !== 1 && slot !== 2) return;
    this.selectedRemoteSlot = slot;
    await this.loadEliteView();
  }

  async copySlotMappings() {
    const curSlot = this.selectedRemoteSlot || 1;
    const otherSlot = curSlot === 1 ? 2 : 1;
    const confirmed = confirm(`Copy all activity and device button mappings from Slot ${otherSlot} to Slot ${curSlot}? Any existing mappings on Slot ${curSlot} will be overwritten.`);
    if (!confirmed) return;

    try {
      await this.client.copyRemoteMapping(otherSlot, curSlot);
      this.showToast(`Mappings copied from Slot ${otherSlot} to Slot ${curSlot}!`, 'success');
      await this.loadEliteView();
    } catch (err) {
      this.showToast(`Copy failed: ${err.message || err}`, 'danger');
    }
  }

  selectEliteButton(btnId, openModal = false) {
    this.eliteSelectedBtn = btnId;
    document.querySelectorAll('.elite-hotspot').forEach(b => {
      b.classList.toggle('selected', b.getAttribute('data-btn') === btnId);
    });
    const lbl = document.getElementById('eliteSelectedBtnLabel');
    if (lbl) lbl.textContent = btnId;

    const actSel = document.getElementById('eliteActivitySelect');
    if (actSel && this.eliteSelectedActivityId && actSel.value !== this.eliteSelectedActivityId) {
      actSel.value = this.eliteSelectedActivityId;
    }
    const actId = this.eliteSelectedActivityId || actSel?.value || '-1';
    const actName = actSel ? actSel.options[actSel.selectedIndex]?.text || 'Off / Idle' : 'Off / Idle';
    const modalActLbl = document.getElementById('eliteModalActivityName');
    if (modalActLbl) modalActLbl.textContent = actName;

    let mapping = null;
    if (this.eliteContextMode === 'device') {
      const devSel = document.getElementById('eliteDeviceSelect');
      const devId = this.eliteSelectedDeviceId || devSel?.value || (this.inventory.devices?.[0]?.id);
      const devName = devSel ? devSel.options[devSel.selectedIndex]?.text || devId : devId;
      if (modalActLbl) modalActLbl.textContent = `Device: ${devName}`;
      mapping = this.eliteMapData?.devices?.[devId]?.[btnId];
    } else {
      if (modalActLbl) modalActLbl.textContent = actName;
      mapping = this.eliteMapData?.activities?.[actId]?.[btnId];
    }

    const typeSel = document.getElementById('eliteActionType');
    const devSel = document.getElementById('eliteTargetDevice');
    if (typeSel) {
      if (this.eliteContextMode === 'device') {
        Array.from(typeSel.options).forEach(opt => {
          opt.disabled = (opt.value !== 'command' && opt.value !== 'none');
          opt.style.display = (opt.value !== 'command' && opt.value !== 'none') ? 'none' : '';
        });
        if (!mapping || typeSel.value === 'none') typeSel.value = 'none';
        else typeSel.value = 'command';
      } else {
        Array.from(typeSel.options).forEach(opt => {
          opt.disabled = false;
          opt.style.display = '';
        });
        if (!mapping) typeSel.value = 'none';
        else if (mapping.type === 'activity_start' || mapping.action === 'activity_start') typeSel.value = 'activity_start';
        else if (mapping.type === 'activity_stop' || mapping.action === 'activity_stop' || mapping.action === 'poweroff') typeSel.value = 'activity_stop';
        else if (mapping.type) typeSel.value = mapping.type;
        else if (mapping.action === 'device_cmd' || mapping.command) typeSel.value = 'command';
        else typeSel.value = 'none';
      }
      this.toggleEliteActionFields();
    }

    if (devSel) {
      if (this.eliteContextMode === 'device') {
        const curDev = this.eliteSelectedDeviceId || document.getElementById('eliteDeviceSelect')?.value;
        if (curDev) devSel.value = curDev;
        devSel.disabled = true;
      } else {
        devSel.disabled = false;
        if (mapping && mapping.deviceId) {
          devSel.value = mapping.deviceId;
        }
      }
    }

    if (mapping && (mapping.activityId || mapping.targetActivity)) {
      const actTgt = document.getElementById('eliteTargetActivity');
      if (actTgt) actTgt.value = mapping.activityId || mapping.targetActivity;
    }
    if (mapping && mapping.sequence) {
      const seqSel = document.getElementById('eliteTargetSequence');
      if (seqSel) seqSel.value = mapping.sequence;
    }
    if (mapping && mapping.script) {
      const scrField = document.getElementById('eliteTargetScript');
      if (scrField) scrField.value = mapping.script;
    }
    this.updateEliteCommandDropdown(mapping?.command);

    const stat = document.getElementById('eliteSelectedBtnStatus');
    if (stat) {
      const isMapped = Boolean(mapping);
      stat.className = `badge ${isMapped ? 'ok' : 'subtle'}`;
      stat.textContent = isMapped ? 'Mapped' : 'Unmapped';
    }

    if (openModal) {
      const m = document.getElementById('eliteBtnModal');
      if (m) m.style.display = 'flex';
    }
  }

  updateEliteCommandDropdown(curVal) {
    const devId = document.getElementById('eliteTargetDevice')?.value;
    const cmdSel = document.getElementById('eliteTargetCommand');
    if (!cmdSel) return;
    const dev = (this.inventory.devices || []).find(d => String(d.id) === String(devId));
    const cmds = (dev?.commands || []).slice().sort((a, b) => (a.name || '').localeCompare(b.name || '', undefined, { sensitivity: 'base' }));
    cmdSel.innerHTML = cmds.map(c => `<option value="${c.name}" ${c.name === curVal ? 'selected' : ''}>${c.name}</option>`).join('');
  }

  toggleEliteActionFields() {
    const t = document.getElementById('eliteActionType')?.value;
    const wCmd = document.getElementById('eliteCommandFields');
    const wAct = document.getElementById('eliteActivityFields');
    const wSeq = document.getElementById('eliteSequenceFields');
    const wScr = document.getElementById('eliteScriptFields');

    if (wCmd) wCmd.style.display = t === 'command' ? 'block' : 'none';
    if (wAct) wAct.style.display = t === 'activity_start' ? 'block' : 'none';
    if (wSeq) wSeq.style.display = t === 'sequence' ? 'block' : 'none';
    if (wScr) wScr.style.display = t === 'script' ? 'block' : 'none';
  }

  applyEliteMapping() {
    const type = document.getElementById('eliteActionType')?.value;
    let mapObj = null;

    if (type === 'command') {
      mapObj = {
        type: 'command',
        action: 'device_cmd',
        deviceId: document.getElementById('eliteTargetDevice')?.value,
        command: document.getElementById('eliteTargetCommand')?.value
      };
    } else if (type === 'activity_start') {
      mapObj = {
        type: 'activity_start',
        action: 'activity_start',
        activityId: document.getElementById('eliteTargetActivity')?.value
      };
    } else if (type === 'activity_stop') {
      mapObj = {
        type: 'activity_stop',
        action: 'activity_stop'
      };
    } else if (type === 'sequence') {
      mapObj = {
        type: 'sequence',
        action: 'sequence',
        sequence: document.getElementById('eliteTargetSequence')?.value
      };
    } else if (type === 'script') {
      mapObj = {
        type: 'script',
        action: 'script',
        script: document.getElementById('eliteTargetScript')?.value
      };
    }

    if (this.eliteContextMode === 'device') {
      const devId = this.eliteSelectedDeviceId || document.getElementById('eliteDeviceSelect')?.value;
      if (devId) {
        if (!this.eliteMapData.devices) this.eliteMapData.devices = {};
        if (!this.eliteMapData.devices[devId]) this.eliteMapData.devices[devId] = {};
        if (type === 'none' || !mapObj) {
          delete this.eliteMapData.devices[devId][this.eliteSelectedBtn];
        } else {
          this.eliteMapData.devices[devId][this.eliteSelectedBtn] = mapObj;
        }
      }
    } else {
      const actId = this.eliteSelectedActivityId || document.getElementById('eliteActivitySelect')?.value || '-1';
      if (!this.eliteMapData.activities) this.eliteMapData.activities = {};
      if (!this.eliteMapData.activities[actId]) this.eliteMapData.activities[actId] = {};

      if (type === 'none' || !mapObj) {
        delete this.eliteMapData.activities[actId][this.eliteSelectedBtn];
      } else {
        this.eliteMapData.activities[actId][this.eliteSelectedBtn] = mapObj;
      }
    }
    this.updateEliteButtonStates();
    const modal = document.getElementById('eliteBtnModal');
    if (modal) modal.style.display = 'none';
  }

  clearEliteMapping() {
    if (this.eliteContextMode === 'device') {
      const devId = this.eliteSelectedDeviceId || document.getElementById('eliteDeviceSelect')?.value;
      if (devId && this.eliteMapData.devices?.[devId]) {
        delete this.eliteMapData.devices[devId][this.eliteSelectedBtn];
      }
    } else {
      const actId = this.eliteSelectedActivityId || document.getElementById('eliteActivitySelect')?.value || '-1';
      if (this.eliteMapData.activities?.[actId]) {
        delete this.eliteMapData.activities[actId][this.eliteSelectedBtn];
      }
    }
    this.selectEliteButton(this.eliteSelectedBtn, false);
    this.updateEliteButtonStates();
    const modal = document.getElementById('eliteBtnModal');
    if (modal) modal.style.display = 'none';
  }

  updateEliteButtonStates() {
    let actMap = {};
    if (this.eliteContextMode === 'device') {
      const devId = this.eliteSelectedDeviceId || document.getElementById('eliteDeviceSelect')?.value;
      actMap = this.eliteMapData?.devices?.[devId] || {};
    } else {
      const actId = this.eliteSelectedActivityId || document.getElementById('eliteActivitySelect')?.value || '-1';
      actMap = this.eliteMapData?.activities?.[actId] || {};
    }
    document.querySelectorAll('.elite-hotspot').forEach(b => {
      const id = b.getAttribute('data-btn');
      b.classList.toggle('mapped', Boolean(actMap[id]));
    });
    const stat = document.getElementById('eliteSelectedBtnStatus');
    if (stat) {
      const isMapped = Boolean(actMap[this.eliteSelectedBtn]);
      stat.className = `badge ${isMapped ? 'ok' : 'subtle'}`;
      stat.textContent = isMapped ? 'Mapped' : 'Unmapped';
    }
  }

  async saveAllEliteMappings() {
    const btn = document.getElementById('eliteSaveAllBtn');
    const slot = this.selectedRemoteSlot || 1;
    try {
      if (btn) btn.textContent = 'Saving...';
      await this.client.saveEliteMapping(this.eliteMapData, slot);
      this.showToast(`Slot ${slot} mappings saved to Hub!`, 'success');
    } catch (err) {
      this.showToast('Save failed: ' + (err.message || err), 'danger');
    } finally {
      if (btn) btn.textContent = 'Save All to Hub';
    }
  }

  async pushEliteConfiguration() {
    const btn = document.getElementById('elitePushRemoteBtn');
    try {
      if (btn) btn.textContent = 'Pushing...';
      const res = await this.client.pushRfConfig();
      if (res && res.status === 'push_started') {
        this.showToast('Configuration push started over RF to Elite remote!', 'success');
      } else {
        this.showToast('Configuration push triggered successfully.', 'success');
      }
    } catch (err) {
      this.showToast('Push failed: ' + (err.message || err), 'danger');
    } finally {
      if (btn) btn.textContent = 'Push to Elite Remote';
    }
  }

  autoMapElitePreset() {
    const devId = document.getElementById('elitePresetDevice')?.value;
    const actId = this.eliteSelectedActivityId || document.getElementById('eliteActivitySelect')?.value || '-1';
    if (!devId) return alert('Select target device first.');
    const dev = (this.inventory.devices || []).find(d => String(d.id) === String(devId));
    if (!dev || !dev.commands) return alert('Device has no commands.');

    const norm = s => String(s || '').toLowerCase().replace(/[^a-z0-9]/g, '');
    const aliases = {
      PowerOffActivity: ['poweroff', 'power off', 'standby', 'power'],
      DirectionUp: ['up', 'directionup'],
      DirectionDown: ['down', 'directiondown'],
      DirectionLeft: ['left', 'directionleft'],
      DirectionRight: ['right', 'directionright'],
      Select: ['ok', 'select', 'enter'],
      Back: ['back', 'return', 'exit'],
      Exit: ['exit', 'cancel'],
      VolumeUp: ['volumeup', 'volup', 'vol up'],
      VolumeDown: ['volumedown', 'voldown', 'vol down'],
      VolumeMute: ['mute', 'volumemute'],
      ChannelUp: ['channelup', 'chup', 'pageup'],
      ChannelDown: ['channeldown', 'chdown', 'pagedown'],
      Rewind: ['rewind', 'rev'],
      Play: ['play', 'playpause'],
      FastForward: ['fastforward', 'forward', 'ffwd'],
      Pause: ['pause'],
      Stop: ['stop'],
      Record: ['record', 'rec'],
      Menu: ['menu', 'topmenu', 'homemenu'],
      Info: ['info', 'display', 'guide'],
      Guide: ['guide', 'epg']
    };

    if (!this.eliteMapData.activities) this.eliteMapData.activities = {};
    if (!this.eliteMapData.activities[actId]) this.eliteMapData.activities[actId] = {};

    let mapped = 0;
    Object.entries(aliases).forEach(([btn, list]) => {
      const cmd = dev.commands.find(c => list.some(a => norm(c.name) === norm(a)));
      if (cmd) {
        this.eliteMapData.activities[actId][btn] = {
          type: 'command',
          deviceId: devId,
          command: cmd.name
        };
        mapped++;
      }
    });

    this.updateEliteButtonStates();
    alert(t("auto_mapped_elite").replace("{count}", mapped));
  }

  renderEliteScreenButtons() {
    const sbtnsList = document.getElementById('eliteScreenButtonsList');
    const preview = document.getElementById('eliteScreenPreview');
    const devs = this.inventory.devices || [];
    const isDevScope = (this.eliteContextMode === 'device');

    const titleEl = document.getElementById('eliteScreenSectionTitle');
    const helpEl = document.getElementById('eliteScreenSectionHelp');
    if (titleEl) {
      titleEl.textContent = isDevScope ? 'Touch Screen Buttons (Device Commands)' : 'On-Screen "DEVICES" Touch Buttons';
    }
    if (helpEl) {
      helpEl.textContent = isDevScope
        ? 'Buttons shown on the Elite touchscreen when browsing this device (commands can be reordered, renamed, or deleted).'
        : 'Buttons shown on the Elite touchscreen when pressing "DEVICES" during this activity (up to 48 buttons, 6 per page).';
    }

    let sbtns = [];
    let curDev = null;

    if (isDevScope) {
      const devSel = document.getElementById('eliteDeviceSelect');
      const devId = this.eliteSelectedDeviceId || devSel?.value || (devs[0]?.id);
      if (!this.eliteMapData.devices) this.eliteMapData.devices = {};
      if (!this.eliteMapData.devices[devId]) this.eliteMapData.devices[devId] = {};
      const devObj = this.eliteMapData.devices[devId];
      curDev = devs.find(d => String(d.id) === String(devId)) || devs[0];

      // Auto-populate with all device commands by default if not populated yet
      if (!Array.isArray(devObj.screen_buttons)) {
        if (curDev && Array.isArray(curDev.commands) && curDev.commands.length > 0) {
          devObj.screen_buttons = curDev.commands.map(c => ({
            label: (c.name || '').slice(0, 12),
            deviceId: String(devId),
            command: c.name,
            icon: ''
          }));
        } else {
          devObj.screen_buttons = [];
        }
      }
      sbtns = devObj.screen_buttons;
    } else {
      const actId = this.eliteSelectedActivityId || document.getElementById('eliteActivitySelect')?.value || '-1';
      if (!this.eliteMapData.activities) this.eliteMapData.activities = {};
      if (!this.eliteMapData.activities[actId]) this.eliteMapData.activities[actId] = {};
      const actObj = this.eliteMapData.activities[actId];
      if (!Array.isArray(actObj.screen_buttons)) {
        actObj.screen_buttons = [];
      }
      sbtns = actObj.screen_buttons;
    }

    // Update screen preview inside the vector remote visual
    if (preview) {
      if (sbtns.length === 0) {
        preview.innerHTML = `
          <div style="display:flex;flex-direction:column;align-items:center;justify-content:center;min-height:90px;color:#64748b;font-size:8px;text-align:center;padding:12px 4px;gap:4px;">
            <span style="font-size:12px;opacity:0.6;">📱</span>
            <span>No Screen Buttons</span>
            <span style="font-size:7px;opacity:0.7;">Add buttons below</span>
          </div>
        `;
      } else {
        preview.innerHTML = sbtns.slice(0, 6).map((b, idx) => {
          const text = b.label || 'Btn ' + (idx + 1);
          return `
            <div class="elite-screen-tile" title="${text}" style="display:flex;align-items:center;justify-content:center;gap:4px;overflow:hidden;padding:0 3px;">
              ${getScreenBtnIconSvg(b.icon, b.label, 11)}
              <span style="overflow:hidden;text-overflow:ellipsis;white-space:nowrap;">${text}</span>
            </div>
          `;
        }).join('');
      }
    }

    // Update screen buttons list editor
    if (!sbtnsList) return;
    if (sbtns.length === 0) {
      const msg = isDevScope
        ? `No touch buttons configured for this device yet. Click "+ Add Screen Button" above.`
        : `No touch buttons configured for this activity yet. Click "+ Add Screen Button" above.`;
      sbtnsList.innerHTML = `<div class="subtle" style="font-size: 13px; padding: 8px 0;">${msg}</div>`;
      return;
    }

    sbtnsList.innerHTML = sbtns.map((btn, idx) => {
      const devOpts = isDevScope
        ? `<option value="${curDev?.id}">${curDev?.name || curDev?.id}</option>`
        : devs.map(d => `<option value="${d.id}" ${String(d.id) === String(btn.deviceId) ? 'selected' : ''}>${d.name || d.id}</option>`).join('');
      const targetDev = isDevScope ? curDev : (devs.find(d => String(d.id) === String(btn.deviceId)) || devs[0]);
      const cmds = targetDev?.commands || [];
      const cmdOpts = cmds.map(c => `<option value="${c.name}" ${c.name === btn.command ? 'selected' : ''}>${c.name}</option>`).join('');
      const iconUrl = btn.icon ? getStockIconUrl(btn.icon) : '';
      const iconDispName = btn.icon ? this.formatIconDisplayName(btn.icon) : 'Pick Icon';
      const shouldInvert = isDarkMonochromeIcon(btn.icon);

      return `
        <div class="screen-btn-row" data-idx="${idx}">
          <span class="screen-btn-num">${idx + 1}</span>
          <div style="display: flex; gap: 2px;">
            <button type="button" class="btn btn-xs btn-ghost screen-btn-up" title="Move Up" ${idx === 0 ? 'disabled style="opacity:0.3;cursor:default;"' : ''}>▲</button>
            <button type="button" class="btn btn-xs btn-ghost screen-btn-down" title="Move Down" ${idx === sbtns.length - 1 ? 'disabled style="opacity:0.3;cursor:default;"' : ''}>▼</button>
          </div>
          <input type="text" class="form-control screen-btn-label" maxlength="12" value="${btn.label || ''}" placeholder="Label (max 12)" style="font-size: 12px; padding: 4px 8px;">
          <button type="button" class="btn btn-xs btn-secondary btn-screen-btn-icon" title="Choose Icon" style="display: inline-flex; align-items: center; gap: 6px; padding: 4px 8px; font-size: 11px; height: 30px; text-align: left; overflow: hidden;">
            ${iconUrl ? `<img src="${iconUrl}" data-icon-key="${btn.icon}" class="stock-icon-img ${shouldInvert ? 'invert-white' : ''}" style="width: 16px; height: 16px; flex-shrink: 0;" onload="if(window.__harmonyAutoInvert)window.__harmonyAutoInvert(this)" onerror="this.style.display='none'" />` : '<span style="opacity: 0.6; font-size: 13px;">🖼</span>'}
            <span class="screen-btn-icon-text" style="overflow: hidden; text-overflow: ellipsis; white-space: nowrap; max-width: 95px;">${iconDispName}</span>
          </button>
          <select class="form-control screen-btn-dev" style="font-size: 12px; padding: 4px 6px; ${isDevScope ? 'display:none;' : ''}" ${isDevScope ? 'disabled' : ''}>
            ${devOpts}
          </select>
          <select class="form-control screen-btn-cmd" style="font-size: 12px; padding: 4px 6px;">
            ${cmdOpts}
          </select>
          <button type="button" class="btn btn-xs btn-ghost screen-btn-del" title="Remove" style="color: var(--danger); font-size: 14px; padding: 2px 6px;">✕</button>
        </div>
      `;
    }).join('');

    // Attach row events
    sbtnsList.querySelectorAll('.screen-btn-row').forEach(row => {
      const idx = parseInt(row.getAttribute('data-idx'), 10);
      const lblIn = row.querySelector('.screen-btn-label');
      const iconBtn = row.querySelector('.btn-screen-btn-icon');
      const devSel = row.querySelector('.screen-btn-dev');
      const cmdSel = row.querySelector('.screen-btn-cmd');
      const delBtn = row.querySelector('.screen-btn-del');
      const upBtn = row.querySelector('.screen-btn-up');
      const downBtn = row.querySelector('.screen-btn-down');

      const updatePreviewTile = () => {
        if (preview && idx < 6) {
          const tiles = preview.querySelectorAll('.elite-screen-tile');
          if (tiles[idx]) {
            const text = sbtns[idx].label || `Btn ${idx + 1}`;
            tiles[idx].innerHTML = `
              ${getScreenBtnIconSvg(sbtns[idx].icon, sbtns[idx].label, 11)}
              <span style="overflow:hidden;text-overflow:ellipsis;white-space:nowrap;">${text}</span>
            `;
            tiles[idx].setAttribute('title', text);
          }
        }
      };

      lblIn?.addEventListener('input', (e) => {
        sbtns[idx].label = e.target.value.trim();
        updatePreviewTile();
      });

      iconBtn?.addEventListener('click', () => {
        this.openIconPicker((selectedIcon) => {
          sbtns[idx].icon = selectedIcon;
          this.renderEliteScreenButtons();
        }, sbtns[idx].icon);
      });

      upBtn?.addEventListener('click', () => {
        if (idx > 0) {
          const item = sbtns.splice(idx, 1)[0];
          sbtns.splice(idx - 1, 0, item);
          this.renderEliteScreenButtons();
        }
      });

      downBtn?.addEventListener('click', () => {
        if (idx < sbtns.length - 1) {
          const item = sbtns.splice(idx, 1)[0];
          sbtns.splice(idx + 1, 0, item);
          this.renderEliteScreenButtons();
        }
      });

      devSel?.addEventListener('change', (e) => {
        const newDevId = e.target.value;
        sbtns[idx].deviceId = newDevId;
        const d = devs.find(x => String(x.id) === String(newDevId));
        if (cmdSel && d) {
          cmdSel.innerHTML = (d.commands || []).map(c => `<option value="${c.name}">${c.name}</option>`).join('');
          sbtns[idx].command = d.commands?.[0]?.name || '';
        }
      });

      cmdSel?.addEventListener('change', (e) => {
        sbtns[idx].command = e.target.value;
      });

      delBtn?.addEventListener('click', () => {
        sbtns.splice(idx, 1);
        this.renderEliteScreenButtons();
      });
    });
  }

  addEliteScreenButton() {
    let sbtns = null;
    let targetDevId = '';
    let firstCmd = '';

    if (this.eliteContextMode === 'device') {
      const devId = this.eliteSelectedDeviceId || document.getElementById('eliteDeviceSelect')?.value || (this.inventory.devices?.[0]?.id);
      if (!this.eliteMapData.devices) this.eliteMapData.devices = {};
      if (!this.eliteMapData.devices[devId]) this.eliteMapData.devices[devId] = {};
      const devObj = this.eliteMapData.devices[devId];
      if (!Array.isArray(devObj.screen_buttons)) devObj.screen_buttons = [];
      sbtns = devObj.screen_buttons;
      targetDevId = String(devId);
      const dev = (this.inventory.devices || []).find(d => String(d.id) === String(devId));
      firstCmd = dev?.commands?.[0]?.name || '';
    } else {
      const actId = this.eliteSelectedActivityId || document.getElementById('eliteActivitySelect')?.value || '-1';
      if (!this.eliteMapData.activities) this.eliteMapData.activities = {};
      if (!this.eliteMapData.activities[actId]) this.eliteMapData.activities[actId] = {};
      const actObj = this.eliteMapData.activities[actId];
      if (!Array.isArray(actObj.screen_buttons)) actObj.screen_buttons = [];
      sbtns = actObj.screen_buttons;
      const devs = this.inventory.devices || [];
      const firstDev = devs[0];
      targetDevId = firstDev ? String(firstDev.id) : '';
      firstCmd = firstDev?.commands?.[0]?.name || '';
    }

    if (sbtns.length >= 48) {
      return alert(t("max_screen_buttons"));
    }
    sbtns.push({
      label: `Btn ${sbtns.length + 1}`,
      icon: '',
      deviceId: targetDevId,
      command: firstCmd
    });
    this.renderEliteScreenButtons();
  }

  async saveEliteRemoteMqttSettings() {
    const status = document.getElementById('eliteMqttSaveStatus');
    const enabled = document.getElementById('eliteRemoteMqttEnabled')?.checked ? 1 : 0;
    const topic = document.getElementById('eliteRemoteMqttTopic')?.value.trim() || '';
    const slot = this.selectedRemoteSlot || 1;
    try {
      if (status) status.textContent = 'Saving...';
      const payload = {};
      if (slot === 1) {
        payload.slot1RemoteEvents = enabled;
        payload.slot1RemoteTopic = topic;
        payload.eliteRemoteEvents = enabled;
        payload.eliteRemoteTopic = topic;
      } else {
        payload.slot2RemoteEvents = enabled;
        payload.slot2RemoteTopic = topic;
      }
      await this.client.saveMqtt(payload);
      if (status) status.innerHTML = '<span style="color:var(--success)">Saved!</span>';
      setTimeout(() => { if (status) status.textContent = ''; }, 3000);
    } catch (err) {
      if (status) status.innerHTML = `<span style="color:var(--danger)">Error: ${err.message}</span>`;
    }
  }

}
