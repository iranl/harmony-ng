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
import { HarmonyAppEventsNav } from './app_events_nav.js';

export class HarmonyAppEventsActions extends HarmonyAppEventsNav {
  bindIrAndSettingsEvents() {
    document.getElementById('btnTestPastedPronto')?.addEventListener('click', async () => {
      const hex = document.getElementById('irdbPaste')?.value.trim();
      const devId = document.getElementById('manageDeviceSelect')?.value;
      if (!hex) return alert(t("enter_pronto_first"));
      const stat = document.getElementById('pasteStatus');
      if (stat) stat.textContent = 'Blasting Pronto code...';
      try {
        await this.client.blastPronto(hex, devId);
        if (stat) stat.innerHTML = '<span style="color:var(--success)">✓ Blasted successfully!</span>';
      } catch (err) {
        if (stat) stat.innerHTML = '<span style="color:var(--danger)">Error: ' + err.message + '</span>';
      }
    });
    document.getElementById('btnSavePastedPronto')?.addEventListener('click', async () => {
      const hex = document.getElementById('irdbPaste')?.value.trim();
      const name = document.getElementById('pasteCmdName')?.value.trim() || 'ProntoCmd';
      const devId = document.getElementById('manageDeviceSelect')?.value;
      if (!hex) return alert('Enter Pronto Hex first');
      const stat = document.getElementById('pasteStatus');
      if (stat) stat.textContent = 'Saving...';
      try {
        await this.client.saveLearnedCommand({ deviceId: devId, name, raw: hex, mode: 'raw' });
        if (stat) stat.innerHTML = '<span style="color:var(--success)">✓ Saved to device!</span>';
        await this.refreshData();
        this.renderIrView();
      } catch (err) {
        if (stat) stat.innerHTML = '<span style="color:var(--danger)">Error: ' + err.message + '</span>';
      }
    });
    document.getElementById('btnEliteGoEnableRf')?.addEventListener('click', async () => {
      try {
        await this.client.toggleRfService(true);
        await this.loadEliteView();
      } catch (e) {
        alert('Enable RF failed: ' + e.message);
      }
    });

    // IR Wizard Actions
    document.getElementById('irdbSource')?.addEventListener('change', (e) => {
      const src = e.target.value;
      const harmonyPanel = document.getElementById('harmonySearchPanel');
      const legacyPanel = document.getElementById('legacyDbSearchPanel');
      if (harmonyPanel && legacyPanel) {
        if (src === 'harmony') {
          harmonyPanel.style.display = 'block';
          legacyPanel.style.display = 'none';
        } else {
          harmonyPanel.style.display = 'none';
          legacyPanel.style.display = 'block';
        }
      }
    });
    document.getElementById('btnCreateNewDevice')?.addEventListener('click', () => this.submitNewDeviceProfile());
    document.getElementById('irdbLoadIndex')?.addEventListener('click', () => this.searchIrdb());
    document.getElementById('irdbFetch')?.addEventListener('click', () => this.fetchSelectedIrdbFile());
    document.getElementById('irdbParsePaste')?.addEventListener('click', () => this.parsePastIrdb());
    document.getElementById('irdbSaveBtn')?.addEventListener('click', () => this.saveSelectedIrdbCommands());
    document.getElementById('wizardCapture')?.addEventListener('click', () => this.captureIrSignal());
    document.getElementById('wizardLearnTest')?.addEventListener('click', () => this.testLearnedIrSignal());
    document.getElementById('wizardSaveBtn')?.addEventListener('click', () => this.saveLearnedIrCommand());
    document.getElementById('wizardTest')?.addEventListener('click', () => this.sendVerifyTestCommand());

    // Bulk IR Lab Actions
    document.getElementById('labCandidatesBtn')?.addEventListener('click', () => this.findLabCandidates());
    document.getElementById('labScan')?.addEventListener('click', () => this.loadLabCommands());
    document.getElementById('labStored')?.addEventListener('click', () => this.useStoredLabCommands());
    document.getElementById('labClear')?.addEventListener('click', () => this.clearLabQueue());
    document.getElementById('labSelectAll')?.addEventListener('click', () => this.setLabSelections(true));
    document.getElementById('labSelectNone')?.addEventListener('click', () => this.setLabSelections(false));
    document.getElementById('labDropUnchecked')?.addEventListener('click', () => this.dropUncheckedLab());
    document.getElementById('labImport')?.addEventListener('click', () => this.importLabSelected());
    document.getElementById('labRun')?.addEventListener('click', () => this.runLabQueue());
    document.getElementById('labStop')?.addEventListener('click', () => this.stopLabQueue());

    // Remote skins fallback handler


    // Bluetooth View Actions
    document.getElementById('btPairToggleBtn')?.addEventListener('click', () => this.toggleBtPairing());
    document.getElementById('btDisconnBtn')?.addEventListener('click', () => this.client.disconnectBt());
    document.getElementById('btRefreshBtn')?.addEventListener('click', () => this.loadBtView());
    document.getElementById('btReleaseAll')?.addEventListener('click', () => {
      const tgt = (document.getElementById('btTargetHostSelect')?.value || '').trim();
      this.client.sendBtKey('release_all', 'tap', tgt);
      this.appendBtLog('Released all keys' + (tgt ? ' on ' + tgt : ''));
    });
    document.getElementById('btSendTextBtn')?.addEventListener('click', () => this.sendBtDirectText());
    document.getElementById('btClearTextBtn')?.addEventListener('click', () => {
      const el = document.getElementById('btDirectText');
      if (el) el.value = '';
    });
    document.getElementById('btRunScriptBtn')?.addEventListener('click', () => this.runBtMacroScript());
    document.getElementById('btSaveCommandBtn')?.addEventListener('click', () => this.saveBtMacroCommand());
    document.getElementById('btClearLogBtn')?.addEventListener('click', () => {
      const el = document.getElementById('btLog');
      if (el) el.textContent = 'Log cleared.';
    });

    // Bluetooth Virtual Mouse & Touchpad
    this.setupBtTouchpad();

    // Harmony Archive IR Database Search
    this.setupHarmonyArchiveSearch();

    // Virtual Keyboard Buttons
    document.querySelectorAll('.kb-key').forEach(btn => {
      btn.addEventListener('click', async () => {
        await this.remoteEngine.triggerHaptic();
        const mod = btn.getAttribute('data-mod');
        if (mod) {
          this.kbMods[mod] = !this.kbMods[mod];
          btn.classList.toggle('kb-on', this.kbMods[mod]);
          return;
        }
        const key = btn.getAttribute('data-key');
        if (key) {
          let code = key;
          const mods = [];
          if (this.kbMods.ctrl) mods.push('ctrl');
          if (this.kbMods.shift) mods.push('shift');
          if (this.kbMods.alt) mods.push('alt');
          if (this.kbMods.win) mods.push('win');
          if (mods.length) code = `${mods.join('+')}+${key}`;
          const tgt = (document.getElementById('btTargetHostSelect')?.value || '').trim();
          try {
            await this.client.sendBtKey(code, 'tap', tgt);
            this.appendBtLog('Key: ' + code + (tgt ? ' -> ' + tgt : ''));
          } catch (err) {
            console.warn('BT Key send error:', err);
            this.appendBtLog('Key ' + code + ' failed: ' + (err.message || err));
          }
          if (mods.length) {
            this.kbMods = { ctrl: false, shift: false, alt: false, win: false };
            document.querySelectorAll('.kb-mod').forEach(m => m.classList.remove('kb-on'));
          }
        }
      });
    });

    // Homatics B25 Buttons
    document.querySelectorAll('.b25-hotspot').forEach(btn => {
      btn.addEventListener('click', () => {
        const btnId = btn.getAttribute('data-btn');
        if (btnId) this.selectB25Button(btnId, true);
      });
    });
    document.getElementById('b25ActivitySelect')?.addEventListener('change', () => {
      this.updateB25ButtonStates();
      this.selectB25Button(this.b25SelectedBtn || 'power', false);
    });
    document.getElementById('b25ActionType')?.addEventListener('change', () => this.toggleB25ActionFields());
    document.getElementById('b25ApplyBtn')?.addEventListener('click', () => this.applyB25Mapping());
    document.getElementById('b25ClearBtn')?.addEventListener('click', () => this.clearB25Mapping());
    document.getElementById('b25SaveAllBtn')?.addEventListener('click', () => this.saveAllB25Mappings());
    document.getElementById('b25RunPresetBtn')?.addEventListener('click', () => this.autoMapB25Preset());
    document.getElementById('b25ScanBtn')?.addEventListener('click', () => this.scanB25Remote());
    document.getElementById('b25PairBtn')?.addEventListener('click', () => this.pairB25Remote());
    document.getElementById('b25RefreshBtn')?.addEventListener('click', () => this.loadRemotesView());
    document.getElementById('btMqttSaveBtn')?.addEventListener('click', () => this.saveBtRemoteMqttSettings());

    // Harmony Elite Remote Buttons
    document.querySelectorAll('.elite-hotspot').forEach(btn => {
      btn.addEventListener('click', () => {
        const btnId = btn.getAttribute('data-btn');
        if (btnId) this.selectEliteButton(btnId, true);
      });
    });
    document.getElementById('eliteActivitySelect')?.addEventListener('change', (e) => {
      this.eliteSelectedActivityId = e.target.value;
      this.updateEliteButtonStates();
      this.selectEliteButton(this.eliteSelectedBtn || 'Select', false);
      this.renderEliteScreenButtons();
    });
    document.getElementById('eliteContextModeSelect')?.addEventListener('change', (e) => {
      this.eliteContextMode = e.target.value;
      const isDev = this.eliteContextMode === 'device';
      const aW = document.getElementById('eliteActivitySelectWrap');
      const dW = document.getElementById('eliteDeviceSelectWrap');
      const sc = document.getElementById('eliteScreenButtonsCard');
      if (aW) aW.style.display = isDev ? 'none' : 'block';
      if (dW) dW.style.display = isDev ? 'block' : 'none';
      if (sc && this.eliteRemoteMode !== 1) sc.style.display = isDev ? 'none' : 'block';
      this.updateEliteButtonStates();
      this.selectEliteButton(this.eliteSelectedBtn || 'Select', false);
    });
    document.getElementById('eliteDeviceSelect')?.addEventListener('change', (e) => {
      this.eliteSelectedDeviceId = e.target.value;
      this.updateEliteButtonStates();
      this.selectEliteButton(this.eliteSelectedBtn || 'Select', false);
    });
    document.getElementById('btnEliteAddScreenBtn')?.addEventListener('click', () => this.addEliteScreenButton());
    document.getElementById('btnEliteDirectPage1')?.addEventListener('click', () => {
      this.eliteDirectPage = 1;
      const b1 = document.getElementById('btnEliteDirectPage1');
      const b2 = document.getElementById('btnEliteDirectPage2');
      if (b1) b1.className = 'btn btn-xs btn-primary';
      if (b2) b2.className = 'btn btn-xs btn-ghost';
      this.renderEliteScreenButtons();
    });
    document.getElementById('btnEliteDirectPage2')?.addEventListener('click', () => {
      this.eliteDirectPage = 2;
      const b1 = document.getElementById('btnEliteDirectPage1');
      const b2 = document.getElementById('btnEliteDirectPage2');
      if (b1) b1.className = 'btn btn-xs btn-ghost';
      if (b2) b2.className = 'btn btn-xs btn-primary';
      this.renderEliteScreenButtons();
    });
    document.getElementById('eliteActionType')?.addEventListener('change', () => this.toggleEliteActionFields());
    document.getElementById('eliteTargetDevice')?.addEventListener('change', () => this.updateEliteCommandDropdown());
    document.getElementById('eliteApplyBtn')?.addEventListener('click', () => this.applyEliteMapping());
    document.getElementById('eliteClearBtn')?.addEventListener('click', () => this.clearEliteMapping());
    document.getElementById('eliteSaveAllBtn')?.addEventListener('click', () => this.saveAllEliteMappings());
    document.getElementById('elitePushRemoteBtn')?.addEventListener('click', () => this.pushEliteConfiguration());
    document.getElementById('rfPairBtn')?.addEventListener('click', () => this.pairRfRemote(0));
    document.getElementById('rfPairSlot1Btn')?.addEventListener('click', () => this.pairRfRemote(1));
    document.getElementById('rfUnpairSlot1Btn')?.addEventListener('click', () => this.unpairRfSlot(1));
    document.getElementById('rfConfigSlot1Btn')?.addEventListener('click', () => this.openRfConfigModal(1));
    document.getElementById('rfRefreshSlot1Btn')?.addEventListener('click', () => this.refreshRfStatus());
    document.getElementById('rfPullSlot1Btn')?.addEventListener('click', () => this.pullConfigFromRemote(1));
    document.getElementById('rfPairSlot2Btn')?.addEventListener('click', () => this.pairRfRemote(2));
    document.getElementById('rfUnpairSlot2Btn')?.addEventListener('click', () => this.unpairRfSlot(2));
    document.getElementById('rfConfigSlot2Btn')?.addEventListener('click', () => this.openRfConfigModal(2));
    document.getElementById('rfRefreshSlot2Btn')?.addEventListener('click', () => this.refreshRfStatus());
    document.getElementById('rfPullSlot2Btn')?.addEventListener('click', () => this.pullConfigFromRemote(2));
    document.getElementById('rfStopPairBtn')?.addEventListener('click', () => this.stopPairRfRemote());
    document.getElementById('rfUnpairBtn')?.addEventListener('click', () => this.unpairRfRemote());
    document.getElementById('rfRefreshBtn')?.addEventListener('click', () => this.refreshRfStatus());
    document.getElementById('rfConfigModalClose')?.addEventListener('click', () => this.closeRfConfigModal());
    document.getElementById('rfConfigCancelBtn')?.addEventListener('click', () => this.closeRfConfigModal());
    document.getElementById('rfConfigSaveBtn')?.addEventListener('click', () => this.saveRfConfig());
    document.getElementById('btnHubUpdateBanner')?.addEventListener('click', () => {
      this.switchTab('system');
      document.getElementById('updateForm')?.scrollIntoView({ behavior: 'smooth' });
    });
    document.getElementById('btnPushConfigToRemote')?.addEventListener('click', () => this.pushConfigToRemote());
    document.getElementById('rfSlot1UpdateBtn')?.addEventListener('click', () => this.startEliteOta(1));
    document.getElementById('rfSlot2UpdateBtn')?.addEventListener('click', () => this.startEliteOta(2));
    document.getElementById('eliteMqttSaveBtn')?.addEventListener('click', () => this.saveEliteRemoteMqttSettings());

    // MQTT Actions
    document.getElementById('mqttSaveBtn')?.addEventListener('click', () => this.saveMqttSettings());

    // Network Actions
    document.getElementById('ethModeSelect')?.addEventListener('change', (e) => {
      const f = document.getElementById('ethStaticFields');
      if (f) f.style.display = e.target.value === 'static' ? 'block' : 'none';
    });
    document.querySelectorAll('.btn-save-eth').forEach(b => {
      b.addEventListener('click', () => this.saveEthernetConfig(b.getAttribute('data-apply')));
    });
    document.querySelectorAll('.btn-save-wifi').forEach(b => {
      b.addEventListener('click', () => this.saveWifiConfig(b.getAttribute('data-apply')));
    });
    document.getElementById('btnWifiScan')?.addEventListener('click', () => this.scanWifiNetworks());
    document.getElementById('btnWifiTest')?.addEventListener('click', () => this.testWifiConfig());
    document.addEventListener('click', (e) => {
      const dd = document.getElementById('wifiScanDropdown');
      if (dd && dd.style.display !== 'none' && !e.target.closest('#wifiScanDropdown') && !e.target.closest('#btnWifiScan')) {
        dd.style.display = 'none';
      }
    });

    // Backup & Restore Actions
    document.querySelectorAll('.btn-backup-export').forEach(b => {
      b.addEventListener('click', () => this.exportBackupItem(b.getAttribute('data-target')));
    });
    document.getElementById('importFile')?.addEventListener('change', (e) => {
      const file = e.target.files && e.target.files[0];
      if (!file) return;
      const reader = new FileReader();
      reader.onload = () => {
        const box = document.getElementById('backupPayload');
        if (box) box.value = reader.result || '';
      };
      reader.readAsText(file);
    });
    document.getElementById('btnRestoreBackup')?.addEventListener('click', () => this.restoreBackupPayload());

    // System Actions
    document.getElementById('btnSaveDebug')?.addEventListener('click', () => this.saveDebugLogging());
    document.getElementById('btnSaveAuth')?.addEventListener('click', () => this.saveWebUiAuth());
    document.getElementById('btnAddNonAdminUser')?.addEventListener('click', () => this.addNonAdminUser());
    document.getElementById('btnSaveHubName')?.addEventListener('click', async () => {
      const nameInp = document.getElementById('hubCustomNameInput');
      const newName = nameInp?.value.trim();
      const stat = document.getElementById('hubNameStatus');
      if (!newName) return;
      if (stat) stat.textContent = 'Saving...';
      try {
        await this.client.saveSettings({ hub_name: newName });
        if (stat) stat.innerHTML = '<span style="color:var(--success)">Saved!</span>';
        const label = document.getElementById('activeHubLabel');
        if (label) label.textContent = newName;
        const ovLbl = document.getElementById('ovActiveHubLabel');
        if (ovLbl) ovLbl.textContent = `Active: ${newName}`;
        if (this.activeHub) this.activeHub.name = newName;
      } catch (err) {
        if (stat) stat.innerHTML = '<span style="color:var(--danger)">' + err.message + '</span>';
      }
    });
    document.getElementById('btnSaveHubSettings')?.addEventListener('click', async () => {
      const ntp_server = document.getElementById('hubNtpServerInput')?.value.trim();
      const timezone = (document.getElementById('hubTimezoneSelect') || document.getElementById('hubTimezoneInput'))?.value.trim();
      const led_mode = document.getElementById('hubLedModeSelect')?.value;
      const stat = document.getElementById('hubSettingsStatus');
      if (stat) stat.textContent = 'Saving...';
      try {
        await this.client.saveSettings({ ntp_server, timezone, led_mode });
        if (stat) stat.innerHTML = '<span style="color:var(--success)">Saved and applied!</span>';
        setTimeout(() => { if (stat) stat.textContent = ''; }, 3000);
      } catch (err) {
        if (stat) stat.innerHTML = '<span style="color:var(--danger)">' + err.message + '</span>';
      }
    });
    document.getElementById('updateCheck')?.addEventListener('click', () => this.checkSoftwareUpdate());
    document.getElementById('updateInstall')?.addEventListener('click', () => this.installSoftwareUpdate());
    document.getElementById('updateRefresh')?.addEventListener('click', () => this.showSoftwareVersions());
    if (this.isNative) {
      document.getElementById('btnSyncIconsHub')?.addEventListener('click', () => this.syncIconPack('hub'));
      document.getElementById('btnSyncIconsOta')?.addEventListener('click', () => this.syncIconPack('ota'));
      document.getElementById('btnClearIconCache')?.addEventListener('click', () => this.clearLocalIconCache());
    }
    document.getElementById('btnRediscover')?.addEventListener('click', () => this.rediscoverMqtt());
    document.getElementById('btnRebootHub')?.addEventListener('click', () => this.rebootHub());
    this.setupFactoryResetModal();
    this.initAuthUI();
    document.getElementById('btnToggleRfSvc')?.addEventListener('click', () => this.toggleRfService());
    document.getElementById('btnToggleBtSvc')?.addEventListener('click', () => this.toggleBtService());
    document.getElementById('btnRefreshLogs')?.addEventListener('click', () => this.refreshLogs());
    document.getElementById('sysLogSource')?.addEventListener('change', () => this.refreshLogs());
    document.getElementById('sysLogAutoRefresh')?.addEventListener('change', (e) => {
      if (e.target.checked) {
        this.logInterval = setInterval(() => this.refreshLogs(), 3000);
      } else if (this.logInterval) {
        clearInterval(this.logInterval);
        this.logInterval = null;
      }
    });

    // Overview Quick Settings
    document.getElementById('btnOvSaveMqtt')?.addEventListener('click', async () => {
      const host = document.getElementById('ovMqttHost')?.value.trim();
      const port = Number(document.getElementById('ovMqttPort')?.value) || 1883;
      const baseTopic = document.getElementById('ovMqttTopic')?.value.trim() || 'harmony/hub';
      const stat = document.getElementById('ovMqttSaveStatus');
      if (stat) stat.textContent = 'Saving...';
      try {
        await this.client.saveMqtt({ host, port, baseTopic, enabled: 1 });
        if (stat) stat.innerHTML = '<span style="color:var(--success)">Saved!</span>';
        setTimeout(() => this.loadDashboard(), 1000);
      } catch (err) {
        if (stat) stat.innerHTML = `<span style="color:var(--danger)">${err.message}</span>`;
      }
    });
    document.getElementById('btnOvRefreshHub')?.addEventListener('click', async () => {
      await this.refreshData();
      if (this.currentTab === 'overview') this.loadDashboard();
    });
    document.getElementById('btnOvRebootHub')?.addEventListener('click', () => this.rebootHub());
    document.getElementById('btnAddHub')?.addEventListener('click', () => this.openAddHubModal());
    document.getElementById('btnRefreshDisk')?.addEventListener('click', () => this.refreshDiskSpace());

    // Icon Browse Buttons & Live Preview Updates
    document.getElementById('btnBrowseActIcons')?.addEventListener('click', () => this.openIconPicker('actEditIcon', 'actIconPreviewBadge'));
    document.getElementById('btnBrowseDevIcons')?.addEventListener('click', () => this.openIconPicker('editDevIcon', 'devIconPreviewBadge'));
    document.getElementById('btnBrowseNewDevIcons')?.addEventListener('click', () => this.openIconPicker('newDeviceIcon', 'newDevIconPreviewBadge'));

    document.getElementById('actEditIcon')?.addEventListener('change', (e) => this.updateBadgeForSelect(e.target, 'actIconPreviewBadge'));
    document.getElementById('editDevIcon')?.addEventListener('change', (e) => this.updateBadgeForSelect(e.target, 'devIconPreviewBadge'));
    document.getElementById('newDeviceIcon')?.addEventListener('change', (e) => this.updateBadgeForSelect(e.target, 'newDevIconPreviewBadge'));

    // Icon Picker Modal
    const iconModal = document.getElementById('iconPickerModal');
    const closeIconModal = () => { if (iconModal) iconModal.style.display = 'none'; };
    document.getElementById('btnIconPickerClose')?.addEventListener('click', closeIconModal);
    document.getElementById('btnIconPickerCancel')?.addEventListener('click', closeIconModal);

    const iconSearch = document.getElementById('iconPickerSearch');
    if (iconSearch) {
      let iconSearchTimer = null;
      iconSearch.addEventListener('input', (e) => {
        clearTimeout(iconSearchTimer);
        iconSearchTimer = setTimeout(() => {
          this.renderIconPickerGrid(e.target.value);
        }, 120);
      });
    }
    document.getElementById('btnIconPickerClearSearch')?.addEventListener('click', () => {
      if (iconSearch) iconSearch.value = '';
      this.renderIconPickerGrid('');
    });

    document.querySelectorAll('.icon-cat-pill').forEach(pill => {
      pill.addEventListener('click', () => {
        const cat = pill.getAttribute('data-cat') || 'all';
        this.currentIconCategory = cat;
        document.querySelectorAll('.icon-cat-pill').forEach(p => {
          p.classList.toggle('btn-primary', p === pill);
          p.classList.toggle('btn-ghost', p !== pill);
        });
        this.renderIconPickerGrid(iconSearch ? iconSearch.value : '');
      });
    });

    // Custom Icon Upload & Canvas Resize (40x40 PNG client-side to prevent Hub RAM OOM)
    let rawIconImage = null;

    const checkCustomIconNameStatus = () => {
      const nameInput = document.getElementById('customIconNameInput');
      const statusEl = document.getElementById('customIconNameStatus');
      if (!nameInput || !statusEl) return { exists: false, cleanName: '' };
      const raw = nameInput.value.trim();
      if (!raw) {
        statusEl.innerHTML = '';
        return { exists: false, cleanName: '' };
      }
      const cleanName = raw.toLowerCase().replace(/[^a-z0-9_-]/g, '_');
      const key1 = cleanName;
      const key2 = `${cleanName}.png`;
      const key3 = `custom_${cleanName}`;
      const key4 = `custom_${cleanName}.png`;

      const existsInCustom = this.customIconMap && (
        this.customIconMap.has(key1) || this.customIconMap.has(key2) ||
        this.customIconMap.has(key3) || this.customIconMap.has(key4)
      );
      const existsInAll = (this.availableIcons || []).some(fn => {
        const c = String(fn).split('/').pop().trim().toLowerCase();
        return c === key1 || c === key2 || c === key3 || c === key4;
      });

      const exists = existsInCustom || existsInAll;
      if (exists) {
        statusEl.innerHTML = `<span class="badge warn" style="font-size: 10px; color: #f59e0b;">⚠️ Exists (will overwrite)</span>`;
      } else {
        statusEl.innerHTML = `<span class="badge ok" style="font-size: 10px; color: #10b981;">✓ Name available</span>`;
      }
      return { exists, cleanName };
    };

    const analyzeIsDarkSilhouette = (ctx, w, h) => {
      try {
        const imgData = ctx.getImageData(0, 0, w, h);
        const data = imgData.data;
        let totalOpaque = 0;
        let lowSatCount = 0;
        let totalLuminance = 0;
        for (let i = 0; i < data.length; i += 4) {
          const a = data[i + 3];
          if (a > 30) {
            totalOpaque++;
            const r = data[i], g = data[i + 1], b = data[i + 2];
            const max = Math.max(r, g, b);
            const min = Math.min(r, g, b);
            const sat = max === 0 ? 0 : (max - min) / max;
            if (sat < 0.25) lowSatCount++;
            const lum = 0.299 * r + 0.587 * g + 0.114 * b;
            totalLuminance += lum;
          }
        }
        if (totalOpaque < 10) return false;
        const avgLum = totalLuminance / totalOpaque;
        const isMonochrome = (lowSatCount / totalOpaque) > 0.85;
        return isMonochrome && avgLum < 110;
      } catch (e) {
        return false;
      }
    };

    const redrawCustomCanvas = () => {
      const canvas = document.getElementById('customIconCanvas');
      const invertCheck = document.getElementById('customIconInvertCheck');
      const previewSz = document.getElementById('customIconPreviewSize');
      if (!canvas || !rawIconImage) return;

      const ctx = canvas.getContext('2d');
      ctx.clearRect(0, 0, 40, 40);

      const nw = rawIconImage.naturalWidth || rawIconImage.width || 40;
      const nh = rawIconImage.naturalHeight || rawIconImage.height || 40;
      const scale = Math.min(40 / nw, 40 / nh);
      const w = Math.max(1, Math.round(nw * scale));
      const h = Math.max(1, Math.round(nh * scale));
      const x = Math.round((40 - w) / 2);
      const y = Math.round((40 - h) / 2);

      ctx.drawImage(rawIconImage, x, y, w, h);

      if (invertCheck && invertCheck.checked) {
        const imgData = ctx.getImageData(0, 0, 40, 40);
        const d = imgData.data;
        for (let i = 0; i < d.length; i += 4) {
          if (d[i + 3] > 15) {
            d[i] = 255;
            d[i + 1] = 255;
            d[i + 2] = 255;
          }
        }
        ctx.putImageData(imgData, 0, 0);
      }

      const dataUrl = canvas.toDataURL('image/png');
      const approxBytes = Math.round((dataUrl.length * 3) / 4);
      if (previewSz) {
        const mode = invertCheck && invertCheck.checked ? 'White silhouette' : 'Original colors';
        previewSz.textContent = `${w}×${h} px • ${approxBytes} bytes • ${mode}`;
      }
    };

    const processImageToCanvas = (img, labelText) => {
      rawIconImage = img;
      const canvas = document.getElementById('customIconCanvas');
      const previewRow = document.getElementById('customIconPreviewRow');
      const previewLbl = document.getElementById('customIconPreviewLabel');
      const invertCheck = document.getElementById('customIconInvertCheck');
      const nameInput = document.getElementById('customIconNameInput');
      if (!canvas) return;
      canvas.width = 40;
      canvas.height = 40;
      const ctx = canvas.getContext('2d');
      ctx.clearRect(0, 0, 40, 40);

      const nw = img.naturalWidth || img.width || 40;
      const nh = img.naturalHeight || img.height || 40;
      const scale = Math.min(40 / nw, 40 / nh);
      const w = Math.max(1, Math.round(nw * scale));
      const h = Math.max(1, Math.round(nh * scale));
      const x = Math.round((40 - w) / 2);
      const y = Math.round((40 - h) / 2);
      ctx.drawImage(img, x, y, w, h);

      // Auto-detect if image is dark monochrome silhouette
      const isDarkSil = analyzeIsDarkSilhouette(ctx, 40, 40);
      if (invertCheck) {
        invertCheck.checked = isDarkSil;
      }

      // Populate icon name input if empty or from labelText
      if (nameInput) {
        let suggestedName = (labelText || '')
          .replace(/\.[^/.]+$/, '')
          .replace(/^custom_/i, '')
          .replace(/[^a-zA-Z0-9_-]/g, '_')
          .toLowerCase();
        if (!suggestedName && this.iconPickerCurrentVal) {
          suggestedName = this.iconPickerCurrentVal.replace(/\.[^/.]+$/, '').replace(/^custom_/i, '');
        }
        nameInput.value = suggestedName || 'my_icon';
        checkCustomIconNameStatus();
      }

      if (previewRow) previewRow.style.display = 'flex';
      if (previewLbl) previewLbl.textContent = labelText || 'Custom Icon';

      redrawCustomCanvas();
    };

    document.getElementById('customIconInvertCheck')?.addEventListener('change', () => {
      redrawCustomCanvas();
    });

    document.getElementById('customIconNameInput')?.addEventListener('input', () => {
      checkCustomIconNameStatus();
    });

    const loadImgFromBlob = (blob, label) => {
      const img = new Image();
      img.onload = () => {
        URL.revokeObjectURL(img.src);
        processImageToCanvas(img, label);
      };
      img.onerror = () => showToast(t("fail_decode_img"), 'error');
      img.src = URL.createObjectURL(blob);
    };

    const customFileInput = document.getElementById('customIconFileInput');
    customFileInput?.addEventListener('change', (e) => {
      const file = e.target.files && e.target.files[0];
      if (!file) return;
      loadImgFromBlob(file, file.name);
    });

    const btnFetchUrl = document.getElementById('btnFetchCustomIconUrl');
    btnFetchUrl?.addEventListener('click', async () => {
      const urlInput = document.getElementById('customIconUrlInput');
      const url = urlInput?.value.trim();
      if (!url) {
        showToast(t("enter_img_url"), 'warning');
        return;
      }
      btnFetchUrl.disabled = true;
      btnFetchUrl.textContent = 'Fetching...';
      try {
        let blob = null;
        try {
          const res = await fetch(url, { mode: 'cors' });
          if (res.ok) blob = await res.blob();
        } catch (corsErr) {
          console.log('Direct fetch CORS error, trying hub proxy...', corsErr);
        }
        if (!blob) {
          const proxyUrl = `${this.client.baseUrl}/api/custom-icons/proxy-fetch?url=${encodeURIComponent(url)}`;
          const pRes = await fetch(proxyUrl);
          if (!pRes.ok) throw new Error(`HTTP ${pRes.status} from proxy`);
          blob = await pRes.blob();
        }
        const fn = url.split('/').pop().split('?')[0] || 'remote_icon.png';
        loadImgFromBlob(blob, fn);
        showToast('Image loaded and resized to 40×40', 'success');
      } catch (err) {
        showToast(t("could_not_fetch_img") + err.message, 'error');
      } finally {
        btnFetchUrl.disabled = false;
        btnFetchUrl.textContent = 'Fetch';
      }
    });

    const btnSaveIcon = document.getElementById('btnSaveCustomIcon');
    btnSaveIcon?.addEventListener('click', async () => {
      const canvas = document.getElementById('customIconCanvas');
      if (!canvas) return;

      const { exists, cleanName } = checkCustomIconNameStatus();
      let iconName = cleanName;
      if (!iconName) {
        iconName = 'icon_' + Date.now();
      }

      if (exists) {
        const ok = confirm(t("icon_already_exists").replace("{name}", iconName));
        if (!ok) return;
      }

      // Ensure canvas has latest pixel rendering (with silhouette or original colors)
      redrawCustomCanvas();
      const dataUrl = canvas.toDataURL('image/png');

      btnSaveIcon.disabled = true;
      btnSaveIcon.textContent = 'Uploading...';
      try {
        let type = 'custom';
        let id = iconName;
        if (this.currentIconPickerTarget === 'actEditIcon') {
          type = 'activity';
          id = document.getElementById('actEditId')?.value || iconName;
        } else if (this.currentIconPickerTarget === 'editDevIcon') {
          type = 'device';
          id = this.selectedIrDeviceId || this.currentIrDeviceId || iconName;
        }

        const uploadRes = await this.client.uploadCustomIcon(type, id, dataUrl, iconName);
        const iconFn = uploadRes.filename || `custom_${iconName}.png`;
        showToast(`Custom icon "${iconName}" saved & queued for remote push!`, 'success');

        if (typeof this.iconPickerCallback === 'function') {
          await this.iconPickerCallback(iconFn);
        } else if (this.currentIconPickerTarget) {
          const targetSel = document.getElementById(this.currentIconPickerTarget);
          if (targetSel) {
            let opt = Array.from(targetSel.options).find(o => o.value === iconFn);
            if (!opt) {
              opt = document.createElement('option');
              opt.value = iconFn;
              opt.textContent = this.formatIconDisplayName(iconFn);
              targetSel.appendChild(opt);
            }
            targetSel.value = iconFn;
            targetSel.dispatchEvent(new Event('change'));
            this.updateBadgeForSelect(targetSel, this.currentIconPickerBadge);
          }
        }

        const prevRow = document.getElementById('customIconPreviewRow');
        if (prevRow) prevRow.style.display = 'none';
        const urlInp = document.getElementById('customIconUrlInput');
        if (urlInp) urlInp.value = '';
        if (customFileInput) customFileInput.value = '';
        rawIconImage = null;

        await this.loadAvailableIcons(true);
        this.renderIconPickerGrid(document.getElementById('iconPickerSearch')?.value || '');
      } catch (err) {
        showToast(t("fail_upload_icon") + err.message, 'error');
      } finally {
        btnSaveIcon.disabled = false;
        btnSaveIcon.textContent = 'Upload & Push to Remote';
      }
    });

    // Bluetooth Auto-Detect Delay
    document.getElementById('btnAutoDetectBtDelay')?.addEventListener('click', () => {
      const devId = this.selectedIrDeviceId || this.currentIrDeviceId;
      if (devId) this.openBtDelayCalibrator(devId);
      else showToast(t("select_device_first"), 'warning');
    });

    const btCalibModal = document.getElementById('btDelayCalibrateModal');
    const closeBtCalib = () => {
      if (btCalibModal) btCalibModal.style.display = 'none';
      if (this._btCalibrateInterval) clearInterval(this._btCalibrateInterval);
      if (this._btCalibratePoll) clearInterval(this._btCalibratePoll);
      const startBtn = document.getElementById('btnBtCalibrateStart');
      if (startBtn) {
        startBtn.disabled = false;
        startBtn.textContent = 'Start Detection Test';
      }
      document.getElementById('btCalibrateTimer')?.classList.remove('bt-timer-active');
    };
    document.getElementById('btnBtCalibrateClose')?.addEventListener('click', closeBtCalib);
    document.getElementById('btnBtCalibrateCancel')?.addEventListener('click', closeBtCalib);

    document.getElementById('btnBtCalibrateStart')?.addEventListener('click', () => {
      const devId = document.getElementById('btCalibrateDevId')?.value;
      if (devId) this.startBtDelayCalibration(devId);
    });

    document.getElementById('btnBtCalibratePowerOff')?.addEventListener('click', async () => {
      const devId = document.getElementById('btCalibrateDevId')?.value;
      if (!devId) return;
      const cmds = this.deviceCommandsMap[devId] || [];
      const offCmd = cmds.find(c => /poweroff|powertoggle|power/i.test(c.name || ''));
      if (offCmd) {
        showToast(`Sending "${offCmd.name}" to device...`, 'info', 2000);
        try {
          await this.client.sendDeviceCommand(devId, offCmd.name);
          const stateEl = document.getElementById('btCalibrateStateMsg');
          if (stateEl) stateEl.textContent = `Power off command "${offCmd.name}" sent.`;
        } catch (e) {
          showToast(t("error_sending_cmd") + e.message, 'error');
        }
      } else {
        showToast('No PowerOff/Toggle command found. Please turn off device manually.', 'info', 3000);
      }
    });

    document.getElementById('btnBtCalibrateApply')?.addEventListener('click', () => this.applyCalibratedBtDelay());
  }

  bindEvents() {
    this.bindNavigationAndDeviceEvents();
    this.bindIrAndSettingsEvents();
  }
}
