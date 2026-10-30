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
import { HarmonyAppIconPicker } from './app_icons_picker.js';

export class HarmonyAppEventsNav extends HarmonyAppIconPicker {
  bindNavigationAndDeviceEvents() {
    // Language Switchers
    const onLangChange = (e) => {
      setLanguage(e.target.value);
    };
    document.getElementById('langSelect')?.addEventListener('change', onLangChange);
    document.getElementById('drawerLangSelect')?.addEventListener('change', onLangChange);

    // Drawer Navigation
    const btnOpenDrawer = document.getElementById('btnOpenDrawer');
    const btnCloseDrawer = document.getElementById('btnCloseDrawer');
    const drawerBackdrop = document.getElementById('drawerBackdrop');
    const appDrawer = document.getElementById('appDrawer');

    const toggleDrawer = (open) => {
      if (appDrawer) appDrawer.classList.toggle('active', open);
      if (drawerBackdrop) drawerBackdrop.classList.toggle('active', open);
    };

    if (btnOpenDrawer) btnOpenDrawer.addEventListener('click', () => toggleDrawer(true));
    if (btnCloseDrawer) btnCloseDrawer.addEventListener('click', () => toggleDrawer(false));
    if (drawerBackdrop) drawerBackdrop.addEventListener('click', () => toggleDrawer(false));

    // Offline Banner Retry Button
    document.getElementById('btnRetryConn')?.addEventListener('click', () => this.recheckConnection(true));

    // Visibility / App Resume listener (Android WebView & browser background recovery)
    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'visible' && this.activeHub) {
        if (!this.isHubOnline || !this.client.isWsConnected()) {
          this.recheckConnection(false);
        }
      }
    });

    // Top Navigation Tabs
    document.querySelectorAll('.nav-tab-btn').forEach(btn => {
      btn.addEventListener('click', () => {
        const tab = btn.getAttribute('data-tab');
        if (tab) this.switchTab(tab);
      });
    });

    // Bottom Navigation Items
    document.querySelectorAll('.app-nav .nav-item').forEach(btn => {
      btn.addEventListener('click', () => {
        if (btn.id === 'navItemMore') {
          toggleDrawer(true);
          return;
        }
        const tab = btn.getAttribute('data-tab');
        if (tab) this.switchTab(tab);
      });
    });

    // Drawer Menu Items
    document.querySelectorAll('.drawer-item').forEach(btn => {
      btn.addEventListener('click', () => {
        const target = btn.getAttribute('data-view-target');
        if (target) {
          this.switchTab(target);
          toggleDrawer(false);
        }
      });
    });

    // Settings Navigation Cards & data-view-target shortcuts
    document.querySelectorAll('[data-view-target], .settings-nav-card').forEach(btn => {
      btn.addEventListener('click', (e) => {
        if (e.target.tagName === 'BUTTON' && !btn.classList.contains('btn') && !btn.classList.contains('nav-tab-btn')) return;
        const target = btn.getAttribute('data-view-target');
        if (target) {
          this.switchTab(target);
          toggleDrawer(false);
        }
      });
    });

    // Back to Settings button in sub-views
    document.querySelectorAll('.btn-back-settings').forEach(btn => {
      btn.addEventListener('click', () => this.switchTab('overview'));
    });

    // Segmented Control Navigations (Mobile UX optimization)
    // 1. Activity Editor Tabs
    const actNav = document.getElementById('actEditTabsNav');
    if (actNav) {
      actNav.querySelectorAll('.segmented-btn').forEach(btn => {
        btn.addEventListener('click', () => {
          const tab = btn.getAttribute('data-act-tab');
          actNav.querySelectorAll('.segmented-btn').forEach(b => b.classList.toggle('active', b === btn));
          const pProfile = document.getElementById('actTabPanelProfile');
          const pStart = document.getElementById('actTabPanelStart');
          const pStop = document.getElementById('actTabPanelStop');
          if (pProfile) pProfile.style.display = (tab === 'profile') ? 'block' : 'none';
          if (pStart) pStart.style.display = (tab === 'start') ? 'block' : 'none';
          if (pStop) pStop.style.display = (tab === 'stop') ? 'block' : 'none';
        });
      });
    }

    // 2. IR Studio Mobile Tabs
    const irNav = document.getElementById('irStudioMobileNav');
    if (irNav) {
      irNav.querySelectorAll('.segmented-btn').forEach(btn => {
        btn.addEventListener('click', () => {
          const tab = btn.getAttribute('data-ir-tab');
          irNav.querySelectorAll('.segmented-btn').forEach(b => b.classList.toggle('active', b === btn));
          const colLeft = document.getElementById('irStudioColLeft');
          const colRight = document.getElementById('irStudioColRight');
          const devProp = document.getElementById('devicePropertiesBox');
          const cmdsWrap = document.getElementById('curDevCmdsWrap');
          if (window.innerWidth <= 768) {
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
        });
      });
    }

    // 3. Harmony Elite Button Mapping Modal Listeners
    document.getElementById('eliteCancelBtn')?.addEventListener('click', () => {
      const m = document.getElementById('eliteBtnModal');
      if (m) m.style.display = 'none';
    });
    document.getElementById('eliteModalCloseBtn')?.addEventListener('click', () => {
      const m = document.getElementById('eliteBtnModal');
      if (m) m.style.display = 'none';
    });
    document.getElementById('eliteBtnModal')?.addEventListener('click', (e) => {
      if (e.target.id === 'eliteBtnModal') e.target.style.display = 'none';
    });
    document.getElementById('eliteScreenFrame')?.addEventListener('click', () => {
      const sec = document.getElementById('eliteSectionScreen');
      if (sec) sec.scrollIntoView({ behavior: 'smooth' });
    });

    // 4. BT Remote B25 Button Mapping Modal Listeners
    document.getElementById('b25CancelBtn')?.addEventListener('click', () => {
      const m = document.getElementById('b25BtnModal');
      if (m) m.style.display = 'none';
    });
    document.getElementById('b25ModalCloseBtn')?.addEventListener('click', () => {
      const m = document.getElementById('b25BtnModal');
      if (m) m.style.display = 'none';
    });
    document.getElementById('b25BtnModal')?.addEventListener('click', (e) => {
      if (e.target.id === 'b25BtnModal') e.target.style.display = 'none';
    });

    // Responsive window resize listener to restore desktop multi-column visibility
    window.addEventListener('resize', () => {
      if (window.innerWidth > 768) {
        ['irStudioColLeft', 'irStudioColRight', 'devicePropertiesBox', 'curDevCmdsWrap',
          'eliteColVisual', 'eliteColConfig', 'eliteSectionScreen', 'eliteSectionPresets',
          'b25ColVisual', 'b25ColPresets'].forEach(id => {
            const el = document.getElementById(id);
            if (el) el.style.display = '';
          });
      }
    });

    // Hub Switcher Button & Pill in Header (Native Android App Only)
    if (this.isNative) {
      const hubBtn = document.getElementById('hubSelectorBtn');
      if (hubBtn) {
        hubBtn.addEventListener('click', () => this.switchTab('hubs'));
      }
      const hubPill = document.getElementById('hubStatusPill') || document.querySelector('.hub-status-pill');
      if (hubPill) {
        hubPill.addEventListener('click', () => this.switchTab('hubs'));
      }
      const hubLabel = document.getElementById('activeHubLabel');
      if (hubLabel) {
        hubLabel.addEventListener('click', () => this.switchTab('hubs'));
      }
    }

    // Power Off in Banner
    const pwrBtn = document.getElementById('btnBannerPowerOff');
    if (pwrBtn) {
      pwrBtn.addEventListener('click', async () => {
        if (await showConfirm('Power off all devices in current activity?', { title: 'Power Off Activity', okText: 'Power Off' })) {
          await this.stopActivity();
        }
      });
    }

    // Add Hub Button in Hubs View
    const addHubBtn = document.getElementById('btnAddHub');
    if (addHubBtn) {
      addHubBtn.addEventListener('click', () => this.openAddHubModal());
    }

    // Stepper navigation in IR Setup
    document.querySelectorAll('.step-btn').forEach(btn => {
      btn.addEventListener('click', () => {
        const target = btn.getAttribute('data-step-target');
        document.querySelectorAll('.step-btn').forEach(b => {
          b.classList.toggle('btn-primary', b === btn);
          b.classList.toggle('btn-ghost', b !== btn);
        });
        document.querySelectorAll('.wizard-panel').forEach(p => {
          p.style.display = p.getAttribute('data-panel') === target ? 'block' : 'none';
        });
        if (target === 'manage') this.populateManageDevices();
        if (target === 'verify') this.populateVerifyDropdowns();
      });
    });

    // Activities Actions
    document.getElementById('actRefreshBtn')?.addEventListener('click', () => this.loadActivitiesView());
    document.getElementById('actNewBtn')?.addEventListener('click', () => this.openActivityEditor(null));
    document.getElementById('actEditorCloseBtn')?.addEventListener('click', () => {
      const box = document.getElementById('actEditorBox');
      if (box) box.style.display = 'none';
    });
    document.getElementById('actCancelBtn')?.addEventListener('click', () => {
      const box = document.getElementById('actEditorBox');
      if (box) box.style.display = 'none';
    });
    document.getElementById('actSaveBtn')?.addEventListener('click', () => this.saveCurrentActivity());
    document.getElementById('actDeleteBtn')?.addEventListener('click', () => this.deleteCurrentActivity());

    document.getElementById('actAddStartCmdBtn')?.addEventListener('click', () => this.addSequenceStep('actStartStepsList', 'Command'));
    document.getElementById('actAddStartDelayBtn')?.addEventListener('click', () => this.addSequenceStep('actStartStepsList', 'Delay'));
    document.getElementById('actAddStartSwitchedBtn')?.addEventListener('click', () => this.addSequenceStep('actStartStepsList', 'Switched'));
    document.getElementById('actAddStopCmdBtn')?.addEventListener('click', () => this.addSequenceStep('actStopStepsList', 'Command'));
    document.getElementById('actAddStopDelayBtn')?.addEventListener('click', () => this.addSequenceStep('actStopStepsList', 'Delay'));
    document.getElementById('actAddStopSwitchedBtn')?.addEventListener('click', () => this.addSequenceStep('actStopStepsList', 'Switched'));

    document.getElementById('actTestStartSeqBtn')?.addEventListener('click', () => this.testLiveSequence('actStartStepsList'));
    document.getElementById('actTestStopSeqBtn')?.addEventListener('click', () => this.testLiveSequence('actStopStepsList'));
    document.getElementById('previewSimulateBtn')?.addEventListener('click', () => this.simulateActivityTransition());
    document.getElementById('previewCloseBtn')?.addEventListener('click', () => {
      const box = document.getElementById('previewResultsBox');
      if (box) box.style.display = 'none';
    });

    // IR Studio Actions
    document.getElementById('btnStudioAddDevice')?.addEventListener('click', () => {
      const box = document.getElementById('studioNewDeviceBox');
      if (box) box.style.display = (box.style.display === 'none') ? 'block' : 'none';
    });
    document.getElementById('btnStudioCancelNewDevice')?.addEventListener('click', () => {
      const box = document.getElementById('studioNewDeviceBox');
      if (box) box.style.display = 'none';
    });
    document.querySelectorAll('.ir-add-tab').forEach(tab => {
      tab.addEventListener('click', () => {
        const mode = tab.getAttribute('data-add-mode');
        document.querySelectorAll('.ir-add-tab').forEach(t => {
          t.classList.toggle('btn-primary', t === tab);
          t.classList.toggle('btn-ghost', t !== tab);
        });
        const pLearn = document.getElementById('addModeLearn');
        const pDb = document.getElementById('addModeDb');
        const pPaste = document.getElementById('addModePaste');
        if (pLearn) pLearn.style.display = (mode === 'learn') ? 'block' : 'none';
        if (pDb) pDb.style.display = (mode === 'database') ? 'block' : 'none';
        if (pPaste) pPaste.style.display = (mode === 'paste') ? 'block' : 'none';
      });
    });
    document.querySelectorAll('.bt-add-tab').forEach(tab => {
      tab.addEventListener('click', () => {
        const mode = tab.getAttribute('data-bt-mode');
        document.querySelectorAll('.bt-add-tab').forEach(t => {
          t.classList.toggle('btn-primary', t === tab);
          t.classList.toggle('btn-ghost', t !== tab);
        });
        const pPresets = document.getElementById('addModeBtPresets');
        const pCustom = document.getElementById('addModeBtCustom');
        if (pPresets) pPresets.style.display = (mode === 'presets') ? 'block' : 'none';
        if (pCustom) pCustom.style.display = (mode === 'custom') ? 'block' : 'none';
      });
    });
    document.getElementById('btPresetsFilter')?.addEventListener('input', () => {
      const dev = (this.inventory.devices || []).find(d => String(d.id) === String(document.getElementById('manageDeviceSelect')?.value));
      if (dev) this.renderBtPresetsList(dev);
    });
    document.getElementById('manageDeviceSelect')?.addEventListener('change', (e) => {
      this.selectedIrDeviceId = e.target.value;
      this.updateStudioDeviceView();
    });
    document.getElementById('curDevCmdFilter')?.addEventListener('input', () => {
      const dev = (this.inventory.devices || []).find(d => String(d.id) === String(document.getElementById('manageDeviceSelect')?.value));
      if (dev) this.renderStudioCommandsList(dev);
    });
    document.getElementById('curDevPortSelect')?.addEventListener('change', async (e) => {
      const devId = this.selectedIrDeviceId || document.getElementById('manageDeviceSelect')?.value;
      const portVal = parseInt(e.target.value, 10);
      this.selectedIrDeviceId = String(devId);
      await this.client.saveDeviceDetails({ deviceId: devId, controlPort: isNaN(portVal) ? 7 : portVal });
      await this.refreshData();
    });
    document.getElementById('btnEditCurrentDevice')?.addEventListener('click', () => {
      const box = document.getElementById('devEditConfigBox');
      if (!box) return;
      const isVisible = box.style.display !== 'none';
      box.style.display = isVisible ? 'none' : 'block';
      if (!isVisible) {
        const sel = document.getElementById('manageDeviceSelect');
        const curId = this.selectedIrDeviceId || sel?.value;
        const dev = (this.inventory.devices || []).find(d => String(d.id) === String(curId));
        if (dev) {
          const nameInp = document.getElementById('editDevName');
          const typeInp = document.getElementById('editDevType');
          const mfgInp = document.getElementById('editDevMfg');
          const mdlInp = document.getElementById('editDevModel');
          const iconInp = document.getElementById('editDevIcon');
          if (nameInp) nameInp.value = dev.name || '';
          if (typeInp) typeInp.value = dev.type || '';
          if (mfgInp) mfgInp.value = dev.manufacturer || '';
          if (mdlInp) mdlInp.value = dev.model || '';
          if (iconInp) {
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
            iconInp.value = iconMap[rawIcn] || rawIcn || 'tv';
            if (!iconInp.value) iconInp.value = 'generic';
            this.updateBadgeForSelect(iconInp, 'devIconPreviewBadge');
          }
        }
      }
    });
    document.getElementById('btnSaveDevDetails')?.addEventListener('click', async () => {
      const sel = document.getElementById('manageDeviceSelect');
      const devId = this.selectedIrDeviceId || sel?.value;
      if (!devId) return;
      this.selectedIrDeviceId = String(devId);
      const name = document.getElementById('editDevName')?.value.trim();
      const type = document.getElementById('editDevType')?.value.trim();
      const mfg = document.getElementById('editDevMfg')?.value.trim();
      const model = document.getElementById('editDevModel')?.value.trim();
      const icon = document.getElementById('editDevIcon')?.value.trim();
      const idd = parseInt(document.getElementById('devInterDeviceDelay')?.value, 10);
      const ikd = parseInt(document.getElementById('devInterKeyDelay')?.value, 10);
      const hikd = parseInt(document.getElementById('devHoldInterKeyDelay')?.value, 10);
      const pod = parseInt(document.getElementById('devPowerOnDelay')?.value, 10);
      const pao = document.getElementById('devIsPowerAlwaysOn')?.checked ? 1 : 0;
      const btHost = document.getElementById('curDevBtHost')?.value.trim();
      const btAutoRecVal = parseInt(document.getElementById('curDevBtAutoReconnect')?.value, 10);
      const btAutoRec = isNaN(btAutoRecVal) ? 1 : (btAutoRecVal === 0 ? 0 : 1);
      const btDur = parseInt(document.getElementById('curDevBtDuration')?.value, 10);
      const btDly = parseInt(document.getElementById('curDevBtDelay')?.value, 10);
      const btTrigVal = document.getElementById('curDevBtTriggerCmd')?.value || '';
      let btTrigDevId = '';
      let btTrigCmdName = '';
      if (btTrigVal && btTrigVal.includes(':')) {
        const parts = btTrigVal.split(':');
        btTrigDevId = parts[0];
        btTrigCmdName = parts.slice(1).join(':');
      }

      const stat = document.getElementById('devEditStatus');
      if (stat) stat.textContent = 'Saving...';
      try {
        await this.client.saveDeviceDetails({
          deviceId: devId,
          name: name || undefined,
          type: type || undefined,
          manufacturer: mfg || undefined,
          model: model || undefined,
          icon: icon || undefined,
          interDeviceDelay: isNaN(idd) ? 100 : idd,
          interKeyDelay: isNaN(ikd) ? 100 : ikd,
          holdInterKeyDelay: isNaN(hikd) ? 200 : hikd,
          powerOnDelay: isNaN(pod) ? 1500 : pod,
          isPowerAlwaysOn: pao,
          bdaddr: btHost || undefined,
          btAutoReconnect: btAutoRec,
          btReconnectDurationSec: isNaN(btDur) ? 0 : btDur,
          btReconnectDelaySec: isNaN(btDly) ? -1 : btDly,
          btTriggerDeviceId: btTrigDevId,
          btTriggerCommandName: btTrigCmdName
        });
        if (stat) stat.innerHTML = '<span style="color:var(--success)">Saved!</span>';
        await this.refreshData();
        const box = document.getElementById('devEditConfigBox');
        if (box) setTimeout(() => { box.style.display = 'none'; }, 1000);
      } catch (err) {
        if (stat) stat.innerHTML = '<span style="color:var(--danger)">' + err.message + '</span>';
      }
    });

    // Bluetooth Host Pairing Controls on Device Setup
    document.getElementById('btnCurDevBtStartPair')?.addEventListener('click', async () => {
      const devId = this.selectedIrDeviceId;
      if (!devId) return;
      const helpBox = document.getElementById('curDevBtPairHelp');
      const statusMsg = document.getElementById('curDevBtPairStatusMsg');
      const startBtn = document.getElementById('btnCurDevBtStartPair');
      if (helpBox) helpBox.style.display = 'block';
      if (startBtn) startBtn.disabled = true;
      if (statusMsg) statusMsg.textContent = 'Hub is in pairing mode. Waiting for device to pair...';

      try {
        const initialBtSt = await this.client.getBtStatus().catch(() => null);
        const initialHosts = new Set(((initialBtSt?.hosts) || []).map(h => (h.addr || '').toLowerCase()));
        await this.client.setBtPairing(true, 'Harmony Keyboard', devId);

        // Start polling for pairing completion
        if (this._btPairPollTimer) clearInterval(this._btPairPollTimer);
        let pollAttempts = 0;
        this._btPairPollTimer = setInterval(async () => {
          pollAttempts++;
          if (pollAttempts > 45) {
            clearInterval(this._btPairPollTimer);
            this._btPairPollTimer = null;
            if (helpBox) helpBox.style.display = 'none';
            if (startBtn) startBtn.disabled = false;
            return;
          }
          try {
            const btSt = await this.client.getBtStatus().catch(() => null);
            let newlyPairedAddr = '';

            // 1. Check if an active host is connected
            if (btSt?.host?.connected && btSt?.host?.addr && btSt.host.addr !== '00:00:00:00:00:00') {
              newlyPairedAddr = btSt.host.addr;
            } else if (btSt?.hosts && btSt.hosts.length > 0) {
              // 2. Check if any host is connected or newly added
              const connectedHost = btSt.hosts.find(h => h.connected && h.addr && h.addr !== '00:00:00:00:00:00');
              const newHost = btSt.hosts.find(h => h.addr && !initialHosts.has(h.addr.toLowerCase()) && h.addr !== '00:00:00:00:00:00');
              if (connectedHost) newlyPairedAddr = connectedHost.addr;
              else if (newHost) newlyPairedAddr = newHost.addr;
            }

            if (newlyPairedAddr) {
              clearInterval(this._btPairPollTimer);
              this._btPairPollTimer = null;
              // Automatically link device to newly paired host
              await this.client.linkBtDevice(newlyPairedAddr, devId);
              await this.refreshData();
              if (statusMsg) {
                statusMsg.innerHTML = `<span style="color:var(--success)">✓ Successfully paired and linked to ${newlyPairedAddr}!</span>`;
              }
              setTimeout(() => {
                if (helpBox) helpBox.style.display = 'none';
                if (startBtn) startBtn.disabled = false;
                this.updateStudioDeviceView();
              }, 1200);
              return;
            }

            // Fallback: check if inventory was already updated directly
            await this.refreshData();
            const curDev = (this.inventory.devices || []).find(d => String(d.id) === String(devId));
            const pairedAddr = curDev ? (curDev.btHost || curDev.bt_host || curDev.BTAddress) : '';
            if (pairedAddr) {
              clearInterval(this._btPairPollTimer);
              this._btPairPollTimer = null;
              if (statusMsg) statusMsg.innerHTML = `<span style="color:var(--success)">✓ Successfully paired to ${pairedAddr}!</span>`;
              setTimeout(() => {
                if (helpBox) helpBox.style.display = 'none';
                if (startBtn) startBtn.disabled = false;
                this.updateStudioDeviceView();
              }, 1200);
            }
          } catch { }
        }, 2000);
      } catch (err) {
        alert(t('could_not_start_bt_pairing', 'Could not start Bluetooth pairing: ') + err.message);
        if (helpBox) helpBox.style.display = 'none';
        if (startBtn) startBtn.disabled = false;
      }
    });

    document.getElementById('btnCurDevBtCancelPair')?.addEventListener('click', async () => {
      if (this._btPairPollTimer) {
        clearInterval(this._btPairPollTimer);
        this._btPairPollTimer = null;
      }
      const helpBox = document.getElementById('curDevBtPairHelp');
      const startBtn = document.getElementById('btnCurDevBtStartPair');
      if (helpBox) helpBox.style.display = 'none';
      if (startBtn) startBtn.disabled = false;
      try {
        await this.client.setBtPairing(false);
      } catch { }
    });

    document.getElementById('btnCurDevBtConnect')?.addEventListener('click', async () => {
      const dev = (this.inventory.devices || []).find(d => String(d.id) === String(this.selectedIrDeviceId));
      const addr = dev ? (dev.btHost || dev.bt_host || dev.BTAddress) : '';
      if (!addr) return alert(t("no_bt_host_linked"));
      try {
        await this.client.connectBt(addr);
        alert(t('connection_cmd_sent', 'Connection command sent to host ') + addr);
        setTimeout(() => this.updateStudioDeviceView(), 1000);
      } catch (err) {
        alert(t('connect_failed', 'Connect failed: ') + err.message);
      }
    });

    document.getElementById('btnCurDevBtDisconnect')?.addEventListener('click', async () => {
      const dev = (this.inventory.devices || []).find(d => String(d.id) === String(this.selectedIrDeviceId));
      const addr = dev ? (dev.btHost || dev.bt_host || dev.BTAddress) : '';
      try {
        await this.client.disconnectBt(addr);
        alert(t("disconnect_cmd_sent"));
        setTimeout(() => this.updateStudioDeviceView(), 1000);
      } catch (err) {
        alert(t('disconnect_failed', 'Disconnect failed: ') + err.message);
      }
    });

    document.getElementById('btnCurDevBtUnpair')?.addEventListener('click', async () => {
      const devId = this.selectedIrDeviceId;
      if (!devId) return;
      if (!await showConfirm('Unpair and unlink Bluetooth host from this device?', { title: 'Unpair Bluetooth Host', okText: 'Unpair' })) return;
      try {
        await this.client.linkBtDevice('', devId);
        await this.refreshData();
        this.updateStudioDeviceView();
      } catch (err) {
        alert(t('unpair_failed', 'Unpair failed: ') + err.message);
      }
    });

    document.getElementById('btnCurDevBtLinkKnown')?.addEventListener('click', async () => {
      const devId = this.selectedIrDeviceId;
      const addr = document.getElementById('curDevBtKnownHostSelect')?.value;
      if (!devId || !addr) return alert(t("select_known_bt_host"));
      try {
        await this.client.linkBtDevice(addr, devId);
        await this.refreshData();
        this.updateStudioDeviceView();
        alert(t('device_linked_to', 'Device linked to ') + addr);
      } catch (err) {
        alert(t('link_failed', 'Link failed: ') + err.message);
      }
    });

    document.getElementById('btnOpenPowerSeqModal')?.addEventListener('click', () => {
      this.openPowerSequenceModal();
    });
    document.getElementById('btnPowerSeqClose')?.addEventListener('click', () => {
      const m = document.getElementById('powerSeqModal');
      if (m) m.style.display = 'none';
    });
    document.getElementById('btnPowerSeqCancel')?.addEventListener('click', () => {
      const m = document.getElementById('powerSeqModal');
      if (m) m.style.display = 'none';
    });
    document.getElementById('btnPowerSeqSave')?.addEventListener('click', async () => {
      await this.saveDevicePowerSequences();
    });
    document.getElementById('btnAddPowerOnCmd')?.addEventListener('click', () => {
      this.addSequenceStep('powerOnStepsList', 'Command');
    });
    document.getElementById('btnAddPowerOnDelay')?.addEventListener('click', () => {
      this.addSequenceStep('powerOnStepsList', 'Delay');
    });
    document.getElementById('btnAddPowerOffCmd')?.addEventListener('click', () => {
      this.addSequenceStep('powerOffStepsList', 'Command');
    });
    document.getElementById('btnAddPowerOffDelay')?.addEventListener('click', () => {
      this.addSequenceStep('powerOffStepsList', 'Delay');
    });
    document.getElementById('btnDevMqttConfig')?.addEventListener('click', () => {
      const box = document.getElementById('devMqttConfigBox');
      if (box) box.style.display = box.style.display === 'none' ? 'block' : 'none';
    });
    document.getElementById('devMqttEnabled')?.addEventListener('change', (e) => {
      const badge = document.getElementById('devMqttEnabledBadge');
      if (badge) {
        badge.className = e.target.checked ? 'badge badge-success mini' : 'badge badge-secondary mini';
        badge.textContent = e.target.checked ? 'Enabled' : 'Disabled';
      }
    });
    document.getElementById('btnSaveDevMqtt')?.addEventListener('click', async () => {
      const devId = this.selectedIrDeviceId || document.getElementById('manageDeviceSelect')?.value;
      if (!devId) return;
      this.selectedIrDeviceId = String(devId);
      const topic = document.getElementById('devMqttTopic')?.value.trim();
      const enabled = document.getElementById('devMqttEnabled')?.checked ? 1 : 0;
      const pulse = parseInt(document.getElementById('devMqttPulse')?.value, 10) || 1000;
      const stat = document.getElementById('devMqttStatus');
      if (stat) stat.textContent = 'Saving MQTT...';
      try {
        await this.client.saveDeviceDetails({ deviceId: devId, mqttTopic: topic, mqttEnabled: enabled, mqttPulse: pulse });
        if (stat) stat.innerHTML = '<span style="color:var(--success)">Saved!</span>';
        await this.refreshData();
      } catch (err) {
        if (stat) stat.innerHTML = '<span style="color:var(--danger)">' + err.message + '</span>';
      }
    });
    document.getElementById('btnTestDevMqtt')?.addEventListener('click', async () => {
      const devId = this.selectedIrDeviceId || document.getElementById('manageDeviceSelect')?.value;
      const topic = document.getElementById('devMqttTopic')?.value.trim();
      const pulseMs = parseInt(document.getElementById('devMqttPulse')?.value, 10) || 1000;
      const stat = document.getElementById('devMqttStatus');
      if (!topic) return alert(t("enter_mqtt_topic"));
      if (stat) stat.textContent = 'Testing publish...';
      try {
        const dev = (this.inventory.devices || []).find(d => String(d.id) === String(devId));
        const res = await this.client.testDeviceMqtt({
          deviceId: devId,
          device: dev?.name || devId,
          command: 'PowerToggle',
          topic: topic,
          pulseMs: pulseMs
        });
        if (stat) stat.innerHTML = `<span style="color:var(--success)">Published to ${res.topic || topic}! (${res.pulseMs || pulseMs}ms pulse)</span>`;
      } catch (err) {
        if (stat) stat.innerHTML = '<span style="color:var(--danger)">' + err.message + '</span>';
      }
    });
    document.getElementById('btnCmdEditClose')?.addEventListener('click', () => {
      const modal = document.getElementById('cmdEditModal');
      if (modal) modal.style.display = 'none';
    });
    document.getElementById('btnCmdEditCancel')?.addEventListener('click', () => {
      const modal = document.getElementById('cmdEditModal');
      if (modal) modal.style.display = 'none';
    });
    document.getElementById('btnCmdEditTest')?.addEventListener('click', async () => {
      const devId = document.getElementById('editCmdDevId')?.value;
      const raw = document.getElementById('editCmdPayload')?.value.trim();
      const stat = document.getElementById('cmdEditStatus');
      if (!raw) return alert(t("no_signal_test"));
      if (stat) stat.textContent = 'Testing signal...';
      try {
        if (raw.startsWith('0000 ') || raw.startsWith('0000')) {
          await this.client.blastPronto(raw, devId);
        } else {
          await this.client.sendCommand(devId, document.getElementById('editCmdName')?.value.trim());
        }
        if (stat) stat.innerHTML = '<span style="color:var(--success)">✓ Blasted successfully!</span>';
      } catch (err) {
        if (stat) stat.innerHTML = '<span style="color:var(--danger)">Error: ' + err.message + '</span>';
      }
    });
    document.getElementById('editCmdType')?.addEventListener('change', (e) => {
      const isIr = (e.target.value !== 'bt' && e.target.value !== 'mqtt');
      const irOpts = document.getElementById('editCmdIrOptions');
      if (irOpts) irOpts.style.display = isIr ? 'grid' : 'none';
    });
    document.getElementById('btnCmdEditSave')?.addEventListener('click', async () => {
      const devId = document.getElementById('editCmdDevId')?.value;
      const oldName = document.getElementById('editCmdOldName')?.value;
      const name = document.getElementById('editCmdName')?.value.trim();
      const modal = document.getElementById('cmdEditModal');
      const devType = modal?.dataset?.deviceType || 'IR';
      let mode = document.getElementById('editCmdType')?.value;
      if (devType === 'BT') mode = 'bt';
      else if (devType === 'MQTT') mode = 'mqtt';
      const raw = document.getElementById('editCmdPayload')?.value.trim();
      const repeats = Number(document.getElementById('editCmdRepeats')?.value) || 3;
      const port = Number(document.getElementById('editCmdPort')?.value) || 0;
      const stat = document.getElementById('cmdEditStatus');
      if (!name) return alert(t("enter_cmd_name"));

      if (stat) stat.textContent = 'Saving changes...';
      try {
        const payloadData = {
          deviceId: devId,
          name,
          oldName,
          raw,
          mode
        };
        if (mode !== 'bt' && mode !== 'mqtt') {
          payloadData.repeats = repeats;
          payloadData.port = port;
        }
        await this.client.saveLearnedCommand(payloadData);
        if (stat) stat.innerHTML = '<span style="color:var(--success)">✓ Saved!</span>';
        setTimeout(() => {
          const modal = document.getElementById('cmdEditModal');
          if (modal) modal.style.display = 'none';
        }, 600);
        await this.refreshData();
        this.renderIrView();
      } catch (err) {
        if (stat) stat.innerHTML = '<span style="color:var(--danger)">Error: ' + err.message + '</span>';
      }
    });
    document.getElementById('btnDeleteCurrentDevice')?.addEventListener('click', async () => {
      const devId = document.getElementById('manageDeviceSelect')?.value;
      if (!devId) return;
      if (!await showConfirm('Are you sure you want to delete this device and all its commands?', { title: 'Delete Device', okText: 'Delete' })) return;
      await this.client.deleteDevice(devId);
      await this.refreshData();
      this.renderIrView();
    });
  }
}
