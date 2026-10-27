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
      const rfRefreshBtn = document.getElementById('rfRefreshBtn');

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

    try {
      const mqRes = await this.client.getMqttStatus().catch(() => null);
      if (mqRes && mqRes.config) {
        const enCb = document.getElementById('eliteRemoteMqttEnabled');
        const topIn = document.getElementById('eliteRemoteMqttTopic');
        const badge = document.getElementById('eliteMqttStatusBadge');
        if (enCb) enCb.checked = Boolean(mqRes.config.eliteRemoteEvents !== undefined ? mqRes.config.eliteRemoteEvents : true);
        if (topIn) topIn.value = mqRes.config.eliteRemoteTopic || '';
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

    this.eliteRemoteMode = 0;
    this.eliteDirectPage = 1;
    this.eliteDirectData = { page1: [], page2: [], buttons: {} };

    try {
      const rfCfgResp = await fetch('/api/rf/remote-config?slot=1');
      if (rfCfgResp.ok) {
        const rc = await rfCfgResp.json();
        this.eliteRemoteMode = rc.remote_mode || 0;
      }
    } catch (e) {
      console.warn('Could not read remote mode:', e);
    }

    const actCard = document.getElementById('eliteActivitySelectCard');
    const directCard = document.getElementById('eliteDirectModeCard');
    const pageTabs = document.getElementById('eliteDirectPageTabs');
    const screenTitle = document.getElementById('eliteScreenSectionTitle');
    const screenHelp = document.getElementById('eliteScreenSectionHelp');

    if (this.eliteRemoteMode === 1) {
      if (actCard) actCard.style.display = 'none';
      if (directCard) directCard.style.display = 'block';
      if (pageTabs) pageTabs.style.display = 'flex';
      if (screenTitle) screenTitle.textContent = '3. On-Screen Direct Buttons (Page 1 & 2)';
      if (screenHelp) screenHelp.textContent = 'Fixed buttons shown on the 2 pages of the Elite remote screen.';

      try {
        const dresp = await fetch('/api/rf/remote-direct-buttons?slot=1');
        if (dresp.ok) {
          const dj = await dresp.json();
          if (dj) this.eliteDirectData = dj;
        }
      } catch (e) {
        console.warn('Failed to load direct buttons:', e);
      }
    } else {
      if (actCard) actCard.style.display = 'block';
      if (directCard) directCard.style.display = 'none';
      if (pageTabs) pageTabs.style.display = 'none';
      if (screenTitle) screenTitle.textContent = '3. On-Screen "DEVICES" Touch Buttons';
      if (screenHelp) screenHelp.textContent = 'Buttons shown on the Elite touchscreen when pressing "DEVICES" during this activity (up to 48 buttons, 6 per page).';
    }

    try {
      const m = await this.client.getEliteMapping();
      if (m && m.activities) this.eliteMapData = m;
    } catch (e) {
      console.warn('Failed to load Elite mappings:', e);
    }

    this.selectEliteButton('Select', false);
    this.updateEliteButtonStates();
    this.renderEliteScreenButtons();
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
    if (this.eliteRemoteMode === 1) {
      mapping = this.eliteDirectData?.buttons?.[btnId];
    } else {
      mapping = this.eliteMapData?.activities?.[actId]?.[btnId];
    }

    const typeSel = document.getElementById('eliteActionType');
    if (typeSel) {
      if (!mapping) typeSel.value = 'none';
      else if (mapping.type === 'activity_start' || mapping.action === 'activity_start') typeSel.value = 'activity_start';
      else if (mapping.type === 'activity_stop' || mapping.action === 'activity_stop' || mapping.action === 'poweroff') typeSel.value = 'activity_stop';
      else if (mapping.type) typeSel.value = mapping.type;
      else if (mapping.action === 'device_cmd' || mapping.command) typeSel.value = 'command';
      else typeSel.value = 'none';
      this.toggleEliteActionFields();
    }

    if (mapping && (mapping.activityId || mapping.targetActivity)) {
      const actTgt = document.getElementById('eliteTargetActivity');
      if (actTgt) actTgt.value = mapping.activityId || mapping.targetActivity;
    }
    if (mapping && mapping.deviceId) {
      const devSel = document.getElementById('eliteTargetDevice');
      if (devSel) devSel.value = mapping.deviceId;
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

    if (this.eliteRemoteMode === 1) {
      if (!this.eliteDirectData.buttons) this.eliteDirectData.buttons = {};
      if (type === 'none' || !mapObj) {
        delete this.eliteDirectData.buttons[this.eliteSelectedBtn];
      } else {
        this.eliteDirectData.buttons[this.eliteSelectedBtn] = mapObj;
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
    if (this.eliteRemoteMode === 1) {
      if (this.eliteDirectData?.buttons) {
        delete this.eliteDirectData.buttons[this.eliteSelectedBtn];
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
    if (this.eliteRemoteMode === 1) {
      actMap = this.eliteDirectData?.buttons || {};
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
    try {
      if (btn) btn.textContent = 'Saving...';
      if (this.eliteRemoteMode === 1) {
        const resp = await fetch('/api/rf/remote-direct-buttons?slot=1', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(this.eliteDirectData)
        });
        if (resp.ok) {
          this.showToast('Harmony Elite direct buttons saved & pushed to remote!', 'success');
        } else {
          this.showToast('Failed to save direct buttons.', 'danger');
        }
      } else {
        await this.client.saveEliteMapping(this.eliteMapData);
        this.showToast('Harmony Elite mappings saved to Hub!', 'success');
      }
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

    let sbtns;
    if (this.eliteRemoteMode === 1) {
      if (!this.eliteDirectData) this.eliteDirectData = { physical: {}, page1: [], page2: [] };
      const pKey = this.eliteDirectPage === 2 ? 'page2' : 'page1';
      if (!Array.isArray(this.eliteDirectData[pKey])) {
        this.eliteDirectData[pKey] = [];
      }
      sbtns = this.eliteDirectData[pKey];
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
      const msg = this.eliteRemoteMode === 1
        ? `No touch buttons configured for ${this.eliteDirectPage === 2 ? 'Page 2' : 'Page 1'} yet. Click "+ Add Screen Button" above.`
        : `No touch buttons configured for this activity yet. Click "+ Add Screen Button" above.`;
      sbtnsList.innerHTML = `<div class="subtle" style="font-size: 13px; padding: 8px 0;">${msg}</div>`;
      return;
    }

    sbtnsList.innerHTML = sbtns.map((btn, idx) => {
      const devOpts = devs.map(d => `<option value="${d.id}" ${String(d.id) === String(btn.deviceId) ? 'selected' : ''}>${d.name || d.id}</option>`).join('');
      const curDev = devs.find(d => String(d.id) === String(btn.deviceId)) || devs[0];
      const cmds = curDev?.commands || [];
      const cmdOpts = cmds.map(c => `<option value="${c.name}" ${c.name === btn.command ? 'selected' : ''}>${c.name}</option>`).join('');
      const iconUrl = btn.icon ? getStockIconUrl(btn.icon) : '';
      const iconDispName = btn.icon ? this.formatIconDisplayName(btn.icon) : 'Pick Icon';
      const shouldInvert = isDarkMonochromeIcon(btn.icon);

      return `
        <div class="screen-btn-row" data-idx="${idx}">
          <span class="screen-btn-num">${idx + 1}</span>
          <input type="text" class="form-control screen-btn-label" maxlength="12" value="${btn.label || ''}" placeholder="Label (max 12)" style="font-size: 12px; padding: 4px 8px;">
          <button type="button" class="btn btn-xs btn-secondary btn-screen-btn-icon" title="Choose Icon" style="display: inline-flex; align-items: center; gap: 6px; padding: 4px 8px; font-size: 11px; height: 30px; text-align: left; overflow: hidden;">
            ${iconUrl ? `<img src="${iconUrl}" data-icon-key="${btn.icon}" class="stock-icon-img ${shouldInvert ? 'invert-white' : ''}" style="width: 16px; height: 16px; flex-shrink: 0;" onload="if(window.__harmonyAutoInvert)window.__harmonyAutoInvert(this)" onerror="this.style.display='none'" />` : '<span style="opacity: 0.6; font-size: 13px;">🖼</span>'}
            <span class="screen-btn-icon-text" style="overflow: hidden; text-overflow: ellipsis; white-space: nowrap; max-width: 95px;">${iconDispName}</span>
          </button>
          <select class="form-control screen-btn-dev" style="font-size: 12px; padding: 4px 6px;">
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
    let sbtns;
    if (this.eliteRemoteMode === 1) {
      if (!this.eliteDirectData) this.eliteDirectData = { physical: {}, page1: [], page2: [] };
      const pKey = this.eliteDirectPage === 2 ? 'page2' : 'page1';
      if (!Array.isArray(this.eliteDirectData[pKey])) {
        this.eliteDirectData[pKey] = [];
      }
      sbtns = this.eliteDirectData[pKey];
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

    if (sbtns.length >= 48) {
      return alert(t("max_screen_buttons"));
    }
    const devs = this.inventory.devices || [];
    const firstDev = devs[0];
    const firstCmd = firstDev?.commands?.[0]?.name || '';
    sbtns.push({
      label: `Btn ${sbtns.length + 1}`,
      icon: '',
      deviceId: firstDev ? String(firstDev.id) : '',
      command: firstCmd
    });
    this.renderEliteScreenButtons();
  }

  async saveEliteRemoteMqttSettings() {
    const status = document.getElementById('eliteMqttSaveStatus');
    const enabled = document.getElementById('eliteRemoteMqttEnabled')?.checked ? 1 : 0;
    const topic = document.getElementById('eliteRemoteMqttTopic')?.value.trim() || '';
    try {
      if (status) status.textContent = 'Saving...';
      await this.client.saveMqtt({ eliteRemoteEvents: enabled, eliteRemoteTopic: topic });
      if (status) status.innerHTML = '<span style="color:var(--success)">Saved!</span>';
      setTimeout(() => { if (status) status.textContent = ''; }, 3000);
    } catch (err) {
      if (status) status.innerHTML = `<span style="color:var(--danger)">Error: ${err.message}</span>`;
    }
  }

}
