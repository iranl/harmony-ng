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
import { HarmonyAppRemotesElite } from './app_remotes_elite.js';

export class HarmonyAppRemotesRf extends HarmonyAppRemotesElite {
  async refreshRfStatus() {
    try {
      const st = await this.client.getRfStatus();
      const fw = document.getElementById('rfFwVer');
      const hubAddr = document.getElementById('rfHubAddr');
      const state = document.getElementById('rfPairState');
      const badge = document.getElementById('rfStatusBadge');
      const pBtn = document.getElementById('rfPairBtn');
      const sBtn = document.getElementById('rfStopPairBtn');
      const msg = document.getElementById('rfPairMsg');

      if (st) {
        if (fw) fw.textContent = st.fw_version || 'Unknown';
        const hAddr = st.hub_rf_address || st.rf_address || '';
        const hasHubAddr = Boolean(hAddr && hAddr !== 'None' && hAddr !== '00000000');
        if (hubAddr) hubAddr.textContent = hasHubAddr ? hAddr : 'None';
        if (state) state.textContent = st.pairing_active ? 'PAIRING ACTIVE' : 'IDLE';
        if (badge) {
          const isOnline = st.running !== false && st.state !== 'stopped';
          badge.className = `badge ${isOnline ? 'ok' : 'bad'}`;
          badge.textContent = isOnline ? 'ONLINE' : 'OFFLINE';
        }

        // Slot 1
        const s1 = (st.slots && st.slots.find(s => s.slot === 1)) || { active: st.paired, address: null, rf_address: null };
        const isComp1 = s1.model === 'companion' || (s1.name && s1.name.includes('Companion'));
        const b1 = document.getElementById('rfSlot1Badge');
        const a1 = document.getElementById('rfSlot1Addr');
        const batt1 = document.getElementById('rfSlot1Battery');
        const ver1 = document.getElementById('rfSlot1Ver');
        const cfgVer1 = document.getElementById('rfSlot1CfgVer');
        const otaWrap1 = document.getElementById('rfSlot1UpdateWrap');
        const pull1 = document.getElementById('rfPullSlot1Btn');
        const cfgBtn1 = document.getElementById('rfConfigSlot1Btn');
        const s1Addr = (s1 && (s1.address || s1.rf_address)) || '';
        const s1Active = Boolean((s1.active || st.paired) && s1Addr && s1Addr !== 'None' && s1Addr !== '00000000');
        if (b1) {
          b1.className = `badge ${s1Active ? (isComp1 ? 'info' : 'ok') : 'subtle'}`;
          b1.textContent = s1Active ? (isComp1 ? 'COMPANION' : 'PAIRED') : 'EMPTY';
        }
        if (a1) a1.textContent = s1Active ? s1Addr : 'None';
        if (batt1) {
          if (s1.battery && typeof s1.battery.percent === 'number' && s1.battery.percent > 0) {
            const chg = s1.battery.charging ? ' ⚡ (Charging)' : '';
            batt1.textContent = `${s1.battery.percent}% (${s1.battery.voltage_mv} mV)${chg}`;
          } else {
            batt1.textContent = s1Active ? (isComp1 ? 'Coin Cell (CR2032)' : 'Waiting for report...') : '--';
          }
        }
        if (ver1) ver1.textContent = isComp1 ? 'N/A (Companion)' : (s1.sw_version || 'Unknown');
        if (cfgVer1) {
          if (!s1Active) {
            cfgVer1.textContent = '--';
          } else if (isComp1) {
            cfgVer1.innerHTML = `<span class="badge ok mini" style="font-size:10px;padding:1px 5px;">IN SYNC (DIRECT RF)</span>`;
          } else {
            const cVer = s1.config_version ? `v${s1.config_version}` : 'v0';
            const inSync = s1.in_sync !== false;
            const bCls = inSync ? 'ok' : 'warn';
            const bTxt = inSync ? 'IN SYNC' : `OUT OF SYNC (Hub: v${st.hub_config_version || 0})`;
            cfgVer1.innerHTML = `${cVer} <span class="badge ${bCls} mini" style="font-size:10px;padding:1px 5px;margin-left:4px;">${bTxt}</span>`;
          }
        }
        if (pull1) pull1.style.display = isComp1 ? 'none' : 'inline-block';
        if (cfgBtn1) cfgBtn1.title = isComp1 ? 'Companion remote has no screen settings' : '';
        if (otaWrap1) {
          const latestElite = this.latestEliteVersion || '1.3.0';
          const needsOta = !isComp1 && s1Active && s1.sw_version && s1.sw_version !== 'unknown' && this.isVersionOlder(s1.sw_version, latestElite);
          otaWrap1.style.display = needsOta ? 'block' : 'none';
        }

        // Slot 2
        const s2 = (st.slots && st.slots.find(s => s.slot === 2)) || { active: false, address: null, rf_address: null };
        const isComp2 = s2.model === 'companion' || (s2.name && s2.name.includes('Companion'));
        const b2 = document.getElementById('rfSlot2Badge');
        const a2 = document.getElementById('rfSlot2Addr');
        const batt2 = document.getElementById('rfSlot2Battery');
        const ver2 = document.getElementById('rfSlot2Ver');
        const cfgVer2 = document.getElementById('rfSlot2CfgVer');
        const otaWrap2 = document.getElementById('rfSlot2UpdateWrap');
        const pull2 = document.getElementById('rfPullSlot2Btn');
        const cfgBtn2 = document.getElementById('rfConfigSlot2Btn');
        const s2Addr = (s2 && (s2.address || s2.rf_address)) || '';
        const s2Active = Boolean(s2.active && s2Addr && s2Addr !== 'None' && s2Addr !== '00000000');
        if (b2) {
          b2.className = `badge ${s2Active ? (isComp2 ? 'info' : 'ok') : 'subtle'}`;
          b2.textContent = s2Active ? (isComp2 ? 'COMPANION' : 'PAIRED') : 'EMPTY';
        }
        if (a2) a2.textContent = s2Active ? s2Addr : 'None';
        if (batt2) {
          if (s2.battery && typeof s2.battery.percent === 'number' && s2.battery.percent > 0) {
            const chg = s2.battery.charging ? ' ⚡ (Charging)' : '';
            batt2.textContent = `${s2.battery.percent}% (${s2.battery.voltage_mv} mV)${chg}`;
          } else {
            batt2.textContent = s2Active ? (isComp2 ? 'Coin Cell (CR2032)' : 'Waiting for report...') : '--';
          }
        }
        if (ver2) ver2.textContent = isComp2 ? 'N/A (Companion)' : (s2.sw_version || 'Unknown');
        if (cfgVer2) {
          if (!s2Active) {
            cfgVer2.textContent = '--';
          } else if (isComp2) {
            cfgVer2.innerHTML = `<span class="badge ok mini" style="font-size:10px;padding:1px 5px;">IN SYNC (DIRECT RF)</span>`;
          } else {
            const cVer = s2.config_version ? `v${s2.config_version}` : 'v0';
            const inSync = s2.in_sync !== false;
            const bCls = inSync ? 'ok' : 'warn';
            const bTxt = inSync ? 'IN SYNC' : `OUT OF SYNC (Hub: v${st.hub_config_version || 0})`;
            cfgVer2.innerHTML = `${cVer} <span class="badge ${bCls} mini" style="font-size:10px;padding:1px 5px;margin-left:4px;">${bTxt}</span>`;
          }
        }
        if (pull2) pull2.style.display = isComp2 ? 'none' : 'inline-block';
        if (cfgBtn2) cfgBtn2.title = isComp2 ? 'Companion remote has no screen settings' : '';
        if (otaWrap2) {
          const latestElite = this.latestEliteVersion || '1.3.0';
          const needsOta = !isComp2 && s2Active && s2.sw_version && s2.sw_version !== 'unknown' && this.isVersionOlder(s2.sw_version, latestElite);
          otaWrap2.style.display = needsOta ? 'block' : 'none';
        }

        if (pBtn) pBtn.style.display = st.pairing_active ? 'none' : 'inline-block';
        if (sBtn) sBtn.style.display = st.pairing_active ? 'inline-block' : 'none';
        if (msg) {
          if (st.pairing_active) {
            msg.style.display = 'block';
            msg.textContent = t('rf_pairing_active_note', 'Hub pairing active! On Elite remotes: Settings > Pair Remote. On Companion: press & hold pairing buttons.');
          } else {
            msg.style.display = 'none';
          }
        }

        // OTA Card Banner & RF Action Locking
        const otaCard = document.getElementById('rfOtaCardBanner');
        const isOtaBusy = Boolean(st.ota && st.ota.in_progress);
        if (otaCard) {
          if (isOtaBusy) {
            otaCard.style.display = 'block';
            const pct = st.ota.total_chunks > 0 ? Math.round((st.ota.sent_chunks / st.ota.total_chunks) * 100) : 0;
            const titleEl = document.getElementById('rfOtaCardTitle');
            const pctEl = document.getElementById('rfOtaCardPct');
            const barEl = document.getElementById('rfOtaCardBar');
            const descEl = document.getElementById('rfOtaCardDesc');
            if (titleEl) titleEl.textContent = `⚡ RF OTA Update in Progress (Slot ${st.ota.slot})`;
            if (pctEl) pctEl.textContent = `${pct}%`;
            if (barEl) barEl.style.width = `${pct}%`;
            if (descEl) {
              const kbSent = Math.round((st.ota.sent_bytes || 0) / 1024);
              const kbTot = Math.round((st.ota.total_bytes || 0) / 1024);
              descEl.textContent = `Chunk ${st.ota.sent_chunks}/${st.ota.total_chunks} (${kbSent} KB / ${kbTot} KB). Pairing and settings changes locked.`;
            }
          } else {
            otaCard.style.display = 'none';
          }
        }

        // Lock / unlock conflicting buttons during OTA
        const conflictingBtnIds = [
          'rfPairSlot1Btn', 'rfPairSlot2Btn', 'rfUnpairSlot1Btn', 'rfUnpairSlot2Btn',
          'rfUnpairBtn', 'rfConfigSlot1Btn', 'rfConfigSlot2Btn', 'rfPullSlot1Btn',
          'rfPullSlot2Btn', 'btnPushConfigToRemote'
        ];
        conflictingBtnIds.forEach(id => {
          const b = document.getElementById(id);
          if (b) {
            b.disabled = isOtaBusy;
            if (isOtaBusy) b.title = `Locked: RF OTA firmware update active on Slot ${st.ota?.slot || 'remote'}`;
            else b.removeAttribute('title');
          }
        });

        const ota1Btn = document.getElementById('rfSlot1UpdateBtn');
        const ota2Btn = document.getElementById('rfSlot2UpdateBtn');
        if (ota1Btn) {
          ota1Btn.disabled = isOtaBusy;
          if (isOtaBusy && st.ota?.slot !== 1) ota1Btn.title = `Locked: OTA in progress on Slot ${st.ota?.slot}`;
          else ota1Btn.removeAttribute('title');
        }
        if (ota2Btn) {
          ota2Btn.disabled = isOtaBusy;
          if (isOtaBusy && st.ota?.slot !== 2) ota2Btn.title = `Locked: OTA in progress on Slot ${st.ota?.slot}`;
          else ota2Btn.removeAttribute('title');
        }
      }
    } catch {
      const badge = document.getElementById('rfStatusBadge');
      if (badge) {
        badge.className = 'badge bad';
        badge.textContent = 'OFFLINE';
      }
    }
  }

  async openRfConfigModal(slot) {
    const modal = document.getElementById('rfConfigModal');
    const title = document.getElementById('rfConfigModalTitle');
    const slotInput = document.getElementById('rfConfigSlot');
    if (!modal) return;
    if (slotInput) slotInput.value = slot;
    if (title) title.textContent = `Remote Settings (Slot ${slot})`;
    modal.style.display = 'flex';

    try {
      const resp = await fetch(`/api/rf/remote-config?slot=${slot}`);
      if (resp.ok) {
        const cfg = await resp.json();
        let langCode = cfg.language || 'en';
        if (typeof cfg.language_id !== 'undefined') {
          const map = ['en', 'de', 'fr', 'nl', 'es', 'it'];
          if (cfg.language_id >= 0 && cfg.language_id < map.length) langCode = map[cfg.language_id];
        }
        if (document.getElementById('rfCfgLanguage')) {
          document.getElementById('rfCfgLanguage').value = langCode;
        }
        if (cfg.brightness) document.getElementById('rfCfgBrightness').value = cfg.brightness;
        if (cfg.timeout) document.getElementById('rfCfgTimeout').value = cfg.timeout;

        const touchVal = typeof cfg.touch !== 'undefined' ? cfg.touch : (typeof cfg.touchEnabled !== 'undefined' ? (cfg.touchEnabled ? 1 : 0) : 1);
        document.getElementById('rfCfgTouch').value = String(touchVal ? 1 : 0);

        const capButtonsVal = typeof cfg.cap_buttons !== 'undefined' ? cfg.cap_buttons : (typeof cfg.capButtonsEnabled !== 'undefined' ? (cfg.capButtonsEnabled ? 1 : 0) : 1);
        if (document.getElementById('rfCfgCapButtons')) document.getElementById('rfCfgCapButtons').value = String(capButtonsVal ? 1 : 0);

        const touchSens = cfg.touch_sensitivity || cfg.touchSensitivity || 2;
        document.getElementById('rfCfgTouchSens').value = String(touchSens);

        const noScreenVal = cfg.no_screen || cfg.noScreen ? 1 : 0;
        if (document.getElementById('rfCfgNoScreen')) {
          document.getElementById('rfCfgNoScreen').value = String(noScreenVal);
        }

        const navKey = cfg.nav_key || cfg.navToggleKey || 139;
        document.getElementById('rfCfgNavKey').value = String(navKey);

        const navMode = typeof cfg.nav_mode !== 'undefined' ? cfg.nav_mode : (typeof cfg.navToggleMode !== 'undefined' ? cfg.navToggleMode : 1);
        document.getElementById('rfCfgNavMode').value = String(navMode);

        const confirmAct = typeof cfg.confirm_act !== 'undefined' ? cfg.confirm_act : (typeof cfg.confirmActivity !== 'undefined' ? (cfg.confirmActivity ? 1 : 0) : 1);
        document.getElementById('rfCfgConfirmAct').value = String(confirmAct ? 1 : 0);

        const gyroVal = typeof cfg.gyro !== 'undefined' ? cfg.gyro : (typeof cfg.gyroEnabled !== 'undefined' ? (cfg.gyroEnabled ? 1 : 0) : 1);
        if (document.getElementById('rfCfgGyro')) document.getElementById('rfCfgGyro').value = String(gyroVal ? 1 : 0);

        const gyroRate = cfg.gyro_rate || cfg.gyroRate || 25;
        if (document.getElementById('rfCfgGyroRate')) document.getElementById('rfCfgGyroRate').value = String(gyroRate);

        const btnBacklight = typeof cfg.btn_backlight !== 'undefined' ? cfg.btn_backlight : (typeof cfg.btnBacklight !== 'undefined' ? cfg.btnBacklight : 2);
        if (document.getElementById('rfCfgBtnBacklight')) document.getElementById('rfCfgBtnBacklight').value = String(btnBacklight);

        const nightStart = typeof cfg.night_start !== 'undefined' ? cfg.night_start : (typeof cfg.nightStart !== 'undefined' ? cfg.nightStart : 22);
        const nightEnd = typeof cfg.night_end !== 'undefined' ? cfg.night_end : (typeof cfg.nightEnd !== 'undefined' ? cfg.nightEnd : 7);
        if (document.getElementById('rfCfgNightSchedule')) {
          const schedVal = `${nightStart}-${nightEnd}`;
          const schedSelect = document.getElementById('rfCfgNightSchedule');
          if (schedSelect.querySelector(`option[value="${schedVal}"]`)) {
            schedSelect.value = schedVal;
          } else {
            schedSelect.value = '22-7';
          }
        }

        const usbEth = typeof cfg.usbeth !== 'undefined' ? cfg.usbeth : (typeof cfg.usbEth !== 'undefined' ? (cfg.usbEth ? 1 : 0) : 0);
        if (document.getElementById('rfCfgUsbEth')) document.getElementById('rfCfgUsbEth').value = String(usbEth ? 1 : 0);

        const powerDebug = typeof cfg.power_debug !== 'undefined' ? cfg.power_debug : (typeof cfg.powerDebug !== 'undefined' ? (cfg.powerDebug ? 1 : 0) : 0);
        if (document.getElementById('rfCfgPowerDebug')) document.getElementById('rfCfgPowerDebug').value = String(powerDebug ? 1 : 0);
      }
    } catch (e) {
      console.warn('Could not fetch existing remote config:', e);
    }
  }

  closeRfConfigModal() {
    const modal = document.getElementById('rfConfigModal');
    if (modal) modal.style.display = 'none';
  }

  async saveRfConfig() {
    const slot = parseInt(document.getElementById('rfConfigSlot')?.value || '1', 10);
    const bright = parseInt(document.getElementById('rfCfgBrightness')?.value || '2', 10);
    const timeout = parseInt(document.getElementById('rfCfgTimeout')?.value || '10', 10);
    const touch = parseInt(document.getElementById('rfCfgTouch')?.value || '1', 10);
    const cap_buttons = parseInt(document.getElementById('rfCfgCapButtons')?.value || '1', 10);
    const touch_sensitivity = parseInt(document.getElementById('rfCfgTouchSens')?.value || '2', 10);
    const nav_key = parseInt(document.getElementById('rfCfgNavKey')?.value || '139', 10);
    const nav_mode = parseInt(document.getElementById('rfCfgNavMode')?.value || '1', 10);
    const confirm_act = parseInt(document.getElementById('rfCfgConfirmAct')?.value || '1', 10);
    const gyro = parseInt(document.getElementById('rfCfgGyro')?.value || '1', 10);
    const gyro_rate = parseInt(document.getElementById('rfCfgGyroRate')?.value || '25', 10);
    const btn_backlight = parseInt(document.getElementById('rfCfgBtnBacklight')?.value || '2', 10);
    const schedParts = (document.getElementById('rfCfgNightSchedule')?.value || '22-7').split('-');
    const night_start = parseInt(schedParts[0] || '22', 10);
    const night_end = parseInt(schedParts[1] || '7', 10);
    const usbeth = parseInt(document.getElementById('rfCfgUsbEth')?.value || '0', 10);
    const power_debug = parseInt(document.getElementById('rfCfgPowerDebug')?.value || '0', 10);
    const language = document.getElementById('rfCfgLanguage')?.value || 'en';
    const langMap = ['en', 'de', 'fr', 'nl', 'es', 'it'];
    const language_id = langMap.indexOf(language) >= 0 ? langMap.indexOf(language) : 0;

    let no_screen = parseInt(document.getElementById('rfCfgNoScreen')?.value || '0', 10);
    if (no_screen === 1) {
      const confirm1 = await showConfirm(
        t('confirm_no_screen_1'),
        { title: t('title_enable_no_screen'), okText: t('btn_understand_continue'), danger: true }
      );
      if (!confirm1) return;
      const confirm2 = await showConfirm(
        t('confirm_no_screen_2'),
        { title: t('title_confirm_black_screen'), okText: t('btn_yes_disable_screen'), danger: true }
      );
      if (!confirm2) return;
    }

    const payload = {
      slot: slot,
      brightness: bright,
      timeout: timeout,
      touch: touch,
      touchEnabled: touch === 1,
      cap_buttons: cap_buttons,
      capButtonsEnabled: cap_buttons === 1,
      touch_sensitivity: touch_sensitivity,
      touchSensitivity: touch_sensitivity,
      remote_mode: 0,
      remoteMode: 0,
      nav_key: nav_key,
      navToggleKey: nav_key,
      nav_mode: nav_mode,
      navToggleMode: nav_mode,
      confirm_act: confirm_act,
      confirmActivity: confirm_act === 1,
      gyro: gyro,
      gyroEnabled: gyro === 1,
      gyro_rate: gyro_rate,
      gyroRate: gyro_rate,
      btn_backlight: btn_backlight,
      btnBacklight: btn_backlight,
      night_start: night_start,
      nightStart: night_start,
      night_end: night_end,
      nightEnd: night_end,
      usbeth: usbeth,
      usbEth: usbeth === 1,
      power_debug: power_debug,
      powerDebug: power_debug === 1,
      no_screen: no_screen,
      noScreen: no_screen === 1,
      language: language,
      language_id: language_id
    };

    const saveBtn = document.getElementById('rfConfigSaveBtn');
    if (saveBtn) saveBtn.textContent = 'Pushing to remote...';
    try {
      const resp = await fetch(`/api/rf/remote-config?slot=${slot}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      });
      if (resp.ok) {
        alert(t('rf_config_saved', 'Settings saved and pushed over RF to remote!'));
        this.closeRfConfigModal();
        await this.loadEliteView();
      } else {
        alert(t('rf_config_failed', 'Failed to save settings to Hub.'));
      }
    } catch (err) {
      alert(t('rf_config_error', 'Error pushing settings: ') + err.message);
    } finally {
      if (saveBtn) saveBtn.textContent = 'Save & Push to Remote';
    }
  }

  async pullConfigFromRemote(slot) {
    const slotName = slot === 1 ? 'Slot 1' : 'Slot 2';
    if (!await showConfirm(`Pull configuration from Remote in ${slotName} to Hub? This will overwrite the Hub's saved remote configuration with what is currently running on the remote.`, { title: `Pull Config (${slotName})`, okText: 'Pull Config', danger: false })) return;
    try {
      await this.client.pullRfConfig(slot);
      alert(t("config_pull_requested").replace("{slot}", slotName));
      setTimeout(() => this.refreshRfStatus(), 3000);
    } catch (err) {
      alert(t('error', 'Error') + ': ' + err.message);
    }
  }

  async pairRfRemote(slot = 0) {
    try {
      await this.client.pairRfRemote(slot);
      await this.refreshRfStatus();
    } catch (err) {
      alert(t('error', 'Error') + ': ' + err.message);
    }
  }

  async stopPairRfRemote() {
    try {
      await this.client.stopPairRfRemote();
      await this.refreshRfStatus();
    } catch (err) {
      alert(t('error', 'Error') + ': ' + err.message);
    }
  }

  async unpairRfSlot(slot) {
    const slotName = slot === 1 ? 'Slot 1' : 'Slot 2';
    const msg = `Are you sure you want to unpair Remote in ${slotName}? Both Hub and remote will remove RF bonding.`;
    if (!await showConfirm(msg, { title: `Unpair ${slotName}`, okText: `Unpair ${slotName}` })) return;
    try {
      await this.client.unpairRfRemote(slot);
      await this.refreshRfStatus();
    } catch (err) {
      alert(t('error', 'Error') + ': ' + err.message);
    }
  }

  async unpairRfRemote() {
    if (!await showConfirm(t('unpair_all_confirm', 'Unpair all paired RF remotes?'), { title: 'Unpair All Remotes', okText: 'Unpair All' })) return;
    try {
      await this.client.unpairRfRemote(0);
      await this.refreshRfStatus();
    } catch (err) {
      alert(t('error', 'Error') + ': ' + err.message);
    }
  }

  async pushConfigToRemote() {
    const btn = document.getElementById('btnPushConfigToRemote');
    if (btn) {
      btn.disabled = true;
      btn.textContent = 'Pushing...';
    }
    try {
      await this.client.pushRfConfig();
      const syncBanner = document.getElementById('remoteSyncPendingBanner');
      if (syncBanner) syncBanner.style.display = 'none';
      alert(t('rf_config_saved', 'Config push initiated to paired Harmony remotes.'));
    } catch (e) {
      alert(t('error', 'Error') + ': ' + e.message);
    } finally {
      if (btn) {
        btn.disabled = false;
        btn.textContent = 'Push to Remote';
      }
    }
  }

  async startEliteOta(slot = 1) {
    if (!await showConfirm(`Start RF OTA firmware update for Remote in Slot ${slot}? Ensure remote is close to the Hub and has at least 40% battery.`, { title: 'RF OTA Update', okText: 'Start Update', danger: false })) return;
    const btn = document.getElementById(`rfSlot${slot}UpdateBtn`);
    const otherBtn = document.getElementById(`rfSlot${slot === 1 ? 2 : 1}UpdateBtn`);
    const statusDiv = document.getElementById(`rfSlot${slot}OtaStatus`);
    if (btn) btn.disabled = true;
    if (otherBtn) otherBtn.disabled = true;
    if (statusDiv) {
      statusDiv.style.display = 'block';
      statusDiv.textContent = 'Initiating RF OTA transfer...';
    }
    try {
      const res = await this.client.startRfOta(slot);
      if (res && res.status === 'started') {
        let startedTransmitting = false;
        this.refreshRfStatus().catch(() => { });
        const pollTimer = setInterval(async () => {
          try {
            const st = await this.client.getRfOtaStatus();
            if (!st) return;
            if (st.status === 1 || st.in_progress) {
              startedTransmitting = true;
              const pct = st.total_bytes > 0 ? Math.round((st.sent_bytes / st.total_bytes) * 100) : 0;
              if (statusDiv) statusDiv.textContent = `Sending OTA: ${pct}% (${st.sent_chunks}/${st.total_chunks} chunks, ${st.sent_bytes}/${st.total_bytes} bytes)...`;
              this.refreshRfStatus().catch(() => { });
            } else if (st.status === 2) {
              clearInterval(pollTimer);
              if (statusDiv) statusDiv.textContent = '✓ OTA Complete! Remote verifying and rebooting.';
              if (btn) btn.disabled = false;
              if (otherBtn) otherBtn.disabled = false;
              setTimeout(() => this.refreshRfStatus(), 10000);
            } else if (st.status === -1) {
              clearInterval(pollTimer);
              if (statusDiv) statusDiv.textContent = `❌ OTA Failed: ${st.error || 'Transfer failed'}`;
              if (btn) btn.disabled = false;
              if (otherBtn) otherBtn.disabled = false;
              this.refreshRfStatus().catch(() => { });
            } else if (st.status === 0 && !st.in_progress && startedTransmitting) {
              clearInterval(pollTimer);
              if (statusDiv) statusDiv.textContent = '❌ OTA Aborted: RF daemon restarted or connection lost';
              if (btn) btn.disabled = false;
              if (otherBtn) otherBtn.disabled = false;
              this.refreshRfStatus().catch(() => { });
            }
          } catch { }
        }, 1500);
      } else {
        alert(t("could_not_start_ota") + (res?.error || res?.status || 'Unknown error'));
        if (btn) btn.disabled = false;
        if (otherBtn) otherBtn.disabled = false;
      }
    } catch (e) {
      alert(t('could_not_start_ota', 'Failed to trigger RF OTA: ') + e.message);
      if (btn) btn.disabled = false;
      if (otherBtn) otherBtn.disabled = false;
    }
  }

  async checkOnlineVersions() {
    try {
      let res = await fetch('https://raw.githubusercontent.com/iranl/harmony-ng/ota/VERSION', { cache: 'no-cache' }).catch(() => null);
      if (!res || !res.ok) {
        res = await fetch('https://raw.githubusercontent.com/iranl/harmony-ng/main/VERSION', { cache: 'no-cache' }).catch(() => null);
      }
      if (res && res.ok) {
        const text = await res.text();
        const ver = text.trim();
        if (ver) {
          this.onlineVersion = ver;
          this.latestEliteVersion = ver;
        }
      }
    } catch { }
  }

  showUpdatingOverlay(statusText = 'Applying update and preparing reboot...') {
    this.isUpdating = true;
    const overlay = document.getElementById('hubUpdatingOverlay');
    if (overlay) overlay.style.display = 'flex';
    const stEl = document.getElementById('hubUpdatingStatusText');
    if (stEl) stEl.textContent = statusText;
  }

  hideUpdatingOverlay() {
    this.isUpdating = false;
    const overlay = document.getElementById('hubUpdatingOverlay');
    if (overlay) overlay.style.display = 'none';
  }

  async checkBanners() {
    try {
      // 0. Hub software update in progress
      const updateStat = await this.client.getUpdateStatus().catch(() => null);
      if (updateStat?.updating) {
        this.showUpdatingOverlay('Hub software update is running in background...');
      } else if (this.isUpdating && !this.pollTimerActive) {
        this.hideUpdatingOverlay();
      }

      // 1. Hub software update available
      const hubBanner = document.getElementById('hubUpdateBanner');
      if (hubBanner) {
        const localVer = updateStat?.current_version || updateStat?.version || '';
        const targetRemote = this.onlineVersion || updateStat?.remote_version || '';
        const hasOnlineUpdate = Boolean(targetRemote && localVer && this.isVersionOlder(localVer, targetRemote));
        const isUpdateAvail = Boolean(hasOnlineUpdate || (updateStat?.update_available && (!targetRemote || (localVer && this.isVersionOlder(localVer, targetRemote)))));
        if (isUpdateAvail && !updateStat?.updating) {
          hubBanner.style.display = 'flex';
          const title = document.getElementById('hubUpdateTitle');
          const desc = document.getElementById('hubUpdateDesc');
          const targetVer = targetRemote || 'New';
          if (title) title.textContent = `Hub Software Update Available (v${targetVer.replace(/^v/, '')})`;
          if (desc) desc.textContent = `Installed: v${localVer.replace(/^v/, '') || 'current'} • Tap to view and install.`;
        } else {
          hubBanner.style.display = 'none';
        }
      }

      // 2. RF remote sync pending
      const rfStat = await this.client.getRfStatus().catch(() => null);
      const syncBanner = document.getElementById('remoteSyncPendingBanner');
      if (syncBanner) {
        if (rfStat && rfStat.sync_pending) {
          syncBanner.style.display = 'flex';
          const title = document.getElementById('remoteSyncTitle');
          const desc = document.getElementById('remoteSyncDesc');
          let slotLabel = 'All Paired Remotes';
          if (rfStat.sync_pending_slot === 1) slotLabel = 'Remote Slot 1';
          else if (rfStat.sync_pending_slot === 2) slotLabel = 'Remote Slot 2';
          if (title) title.textContent = `Remote Settings Pending Push (${slotLabel})`;
          if (desc) {
            const diffInfo = rfStat.sync_pending_diff ? `Pending changes: ${rfStat.sync_pending_diff}` : 'Hub configuration changed. Push settings to synchronize remotes.';
            desc.textContent = diffInfo;
          }
        } else {
          syncBanner.style.display = 'none';
        }
      }

      // 3. RF OTA update in progress top banner
      const otaBanner = document.getElementById('rfOtaTopBanner');
      if (otaBanner) {
        if (rfStat?.ota?.in_progress) {
          otaBanner.style.display = 'flex';
          const title = document.getElementById('rfOtaTopTitle');
          const desc = document.getElementById('rfOtaTopDesc');
          const badge = document.getElementById('rfOtaTopPctBadge');
          const bar = document.getElementById('rfOtaTopProgressBar');
          const pct = rfStat.ota.total_chunks > 0 ? Math.round((rfStat.ota.sent_chunks / rfStat.ota.total_chunks) * 100) : 0;
          if (title) title.textContent = `Remote Slot ${rfStat.ota.slot} Firmware OTA in Progress`;
          if (badge) badge.textContent = `${pct}%`;
          if (bar) bar.style.width = `${pct}%`;
          if (desc) {
            const kbSent = Math.round((rfStat.ota.sent_bytes || 0) / 1024);
            const kbTot = Math.round((rfStat.ota.total_bytes || 0) / 1024);
            desc.textContent = `Transmitting chunk ${rfStat.ota.sent_chunks} of ${rfStat.ota.total_chunks} (${kbSent} KB / ${kbTot} KB). Other RF changes locked.`;
          }
        } else {
          otaBanner.style.display = 'none';
        }
      }
    } catch { }
  }

  /* -------------------------------------------------------------
   * 9. MQTT View
   * ------------------------------------------------------------- */
}
