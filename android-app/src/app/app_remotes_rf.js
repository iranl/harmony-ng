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
import { HarmonyAppRemotesScreen } from './app_remotes_screen.js';

/* Remote slots of the Hub (payload/source/remote_slots.h) */
const RF_SLOT_MAX = 6;

export class HarmonyAppRemotesRf extends HarmonyAppRemotesScreen {
  /* pushed: an rf_status object from the WebSocket (state watcher); fetched when absent */
  async refreshRfStatus(pushed = null) {
    try {
      const st = pushed || await this.client.getRfStatus();
      const fw = document.getElementById('rfFwVer');
      const hubAddr = document.getElementById('rfHubAddr');
      const state = document.getElementById('rfPairState');
      const badge = document.getElementById('rfStatusBadge');
      const pBtn = document.getElementById('rfPairBtn');
      const sBtn = document.getElementById('rfStopPairBtn');
      const msg = document.getElementById('rfPairMsg');

      if (st) {
        if (fw) fw.textContent = st.fw_version || t('unknown');
        const hAddr = st.hub_rf_address || st.rf_address || '';
        const hasHubAddr = Boolean(hAddr && hAddr !== 'None' && hAddr !== '00000000');
        if (hubAddr) hubAddr.textContent = hasHubAddr ? hAddr : t('val_none');
        if (state) state.textContent = st.pairing_active ? t('badge_pairing_active') : t('badge_idle');
        if (badge) {
          const isOnline = st.running !== false && st.state !== 'stopped';
          badge.className = `badge ${isOnline ? 'ok' : 'bad'}`;
          badge.textContent = isOnline ? t('badge_online') : t('badge_offline');
        }

        // A card per slot, each made from the same template: its Pair button pairs the next
        // remote into that slot
        this.syncRfSlotCards();
        for (let n = 1; n <= RF_SLOT_MAX; n++) this.renderRfSlotCard(st, n);

        if (pBtn) pBtn.style.display = st.pairing_active ? 'none' : 'inline-block';
        if (sBtn) sBtn.style.display = st.pairing_active ? 'inline-block' : 'none';
        if (msg) {
          if (st.pairing_active) {
            msg.style.display = 'block';
            msg.textContent = t('rf_pairing_active_note', 'Hub pairing active! On touchscreen remotes: Settings > Pair Remote. On Companion: press & hold pairing buttons.');
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
            if (titleEl) titleEl.textContent = `⚡ ${tf('rf_ota_in_progress_slot', { slot: st.ota.slot })}`;
            if (pctEl) pctEl.textContent = `${pct}%`;
            if (barEl) barEl.style.width = `${pct}%`;
            if (descEl) {
              const kbSent = Math.round((st.ota.sent_bytes || 0) / 1024);
              const kbTot = Math.round((st.ota.total_bytes || 0) / 1024);
              descEl.textContent = tf('rf_ota_card_progress', { sent: st.ota.sent_chunks, total: st.ota.total_chunks, kb_sent: kbSent, kb_total: kbTot });
            }
          } else {
            otaCard.style.display = 'none';
          }
        }

        // Lock / unlock conflicting buttons during OTA
        const conflictingBtnIds = ['rfUnpairBtn', 'btnPushConfigToRemote'];
        for (let n = 1; n <= RF_SLOT_MAX; n++) {
          conflictingBtnIds.push(`rfPairSlot${n}Btn`, `rfUnpairSlot${n}Btn`, `rfConfigSlot${n}Btn`, `rfPullSlot${n}Btn`);
        }
        conflictingBtnIds.forEach(id => {
          const b = document.getElementById(id);
          if (b) {
            b.disabled = isOtaBusy;
            if (isOtaBusy) b.title = tf('locked_rf_ota_active_slot', { slot: st.ota?.slot || '?' });
            else b.removeAttribute('title');
          }
        });

        for (let n = 1; n <= RF_SLOT_MAX; n++) {
          const otaBtn = document.getElementById(`rfSlot${n}UpdateBtn`);
          if (!otaBtn) continue;
          otaBtn.disabled = isOtaBusy;
          if (isOtaBusy && st.ota?.slot !== n) otaBtn.title = tf('locked_ota_in_progress_slot', { slot: st.ota?.slot });
          else otaBtn.removeAttribute('title');
        }
      }
    } catch {
      const badge = document.getElementById('rfStatusBadge');
      if (badge) {
        badge.className = 'badge bad';
        badge.textContent = t('badge_offline');
      }
    }
  }

  /* The card of slot n: what the RF status says about that slot */
  renderRfSlotCard(st, n) {
      const s = (st.slots && st.slots.find(s => s.slot === n)) || { active: false, address: null, rf_address: null };
      const addr = (s && (s.address || s.rf_address)) || '';
      const active = Boolean(s.active && addr && addr !== 'None' && addr !== '00000000');
      const isNonLinux = active && (
        s.model === 'smart_control' ||
        s.model === 'companion' ||
        s.has_screen === false ||
        s.sw_version === 'N/A' ||
        !s.sw_version ||
        s.sw_version === 'unknown' ||
        (s.name && (s.name.includes('Companion') || s.name.includes('Smart')))
      );
      const isSmart = s.model === 'smart_control' || (s.name && s.name.includes('Smart'));
      const badgeEl = document.getElementById(`rfSlot${n}Badge`);
      const titleEl = document.getElementById(`rfSlot${n}Title`);
      const addrEl = document.getElementById(`rfSlot${n}Addr`);
      const battEl = document.getElementById(`rfSlot${n}Battery`);
      const battRow = document.getElementById(`rfSlot${n}BattRow`);
      const fwRow = document.getElementById(`rfSlot${n}FwRow`);
      const verEl = document.getElementById(`rfSlot${n}Ver`);
      const cfgVerEl = document.getElementById(`rfSlot${n}CfgVer`);
      const otaWrap = document.getElementById(`rfSlot${n}UpdateWrap`);
      const pairBtn = document.getElementById(`rfPairSlot${n}Btn`);
      const unpairBtn = document.getElementById(`rfUnpairSlot${n}Btn`);
      const pullBtn = document.getElementById(`rfPullSlot${n}Btn`);
      const cfgBtn = document.getElementById(`rfConfigSlot${n}Btn`);
      const refBtn = document.getElementById(`rfRefreshSlot${n}Btn`);

      if (titleEl) {
        if (!active) {
          titleEl.textContent = tf('slot_n', { slot: n });
        } else if (isSmart) {
          titleEl.textContent = tf('rf_slot_product_title', { slot: n, product: 'Harmony Smart Control' });
        } else if (isNonLinux) {
          titleEl.textContent = tf('rf_slot_product_title', { slot: n, product: 'Harmony Companion' });
        } else {
          titleEl.textContent = tf('rf_slot_product_title', { slot: n, product: 'Harmony Remote' });
        }
      }
      if (badgeEl) {
        badgeEl.className = `badge ${active ? (isNonLinux ? 'info' : 'ok') : 'subtle'}`;
        badgeEl.textContent = active ? (isSmart ? 'SMART CONTROL' : (isNonLinux ? 'COMPANION' : t('badge_paired'))) : t('badge_empty');
      }
      if (addrEl) addrEl.textContent = active ? addr : t('val_none');
      if (battRow) battRow.style.display = isNonLinux ? 'none' : 'block';
      if (fwRow) fwRow.style.display = isNonLinux ? 'none' : 'block';
      if (battEl && !isNonLinux) {
        if (s.battery && typeof s.battery.percent === 'number' && s.battery.percent > 0) {
          const chg = s.battery.charging ? ` ⚡${t('battery_charging_suffix')}` : '';
          battEl.textContent = `${s.battery.percent}% (${s.battery.voltage_mv} mV)${chg}`;
        } else {
          battEl.textContent = active ? t('waiting_for_report') : '--';
        }
      }
      if (verEl && !isNonLinux) verEl.textContent = s.sw_version || t('unknown');
      if (cfgVerEl && !isNonLinux) {
        if (!active) {
          cfgVerEl.textContent = '--';
        } else {
          const cVer = s.config_version ? `v${s.config_version}` : 'v0';
          const inSync = s.in_sync !== false;
          const bCls = inSync ? 'ok' : 'warn';
          const bTxt = escapeHtml(inSync ? t('badge_in_sync') : tf('badge_out_of_sync', { version: st.hub_config_version || 0 }));
          cfgVerEl.innerHTML = `${cVer} <span class="badge ${bCls} mini" style="font-size:10px;padding:1px 5px;margin-left:4px;">${bTxt}</span>`;
        }
      }
      if (pairBtn) pairBtn.style.display = active ? 'none' : 'inline-block';
      if (unpairBtn) unpairBtn.style.display = active ? 'inline-block' : 'none';
      if (pullBtn) pullBtn.style.display = (active && !isNonLinux) ? 'inline-block' : 'none';
      if (cfgBtn) cfgBtn.style.display = (active && !isNonLinux) ? 'inline-block' : 'none';
      if (refBtn) refBtn.style.display = (active && !isNonLinux) ? 'inline-block' : 'none';
      if (otaWrap) {
        const latestRemote = this.latestRemoteVersion || '1.3.0';
        const needsOta = !isNonLinux && active && s.sw_version && s.sw_version !== 'unknown' && this.isVersionOlder(s.sw_version, latestRemote);
        otaWrap.style.display = needsOta ? 'block' : 'none';
      }
  }

  /* The cards of slots 1 to RF_SLOT_MAX, all made from #rfSlotCardTpl: every slot has one,
   * paired or not */
  syncRfSlotCards() {
    const grid = document.getElementById('rfSlotsGrid');
    const tpl = document.getElementById('rfSlotCardTpl');
    if (!grid || !tpl) return;
    for (let n = 1; n <= RF_SLOT_MAX; n++) {
      if (document.getElementById(`rfSlotCard${n}`)) continue;
      const c = tpl.content.firstElementChild.cloneNode(true);
      c.id = `rfSlotCard${n}`;
      c.querySelectorAll('[id]').forEach(el => { el.id = el.id.replace('SlotN', `Slot${n}`); });
      // template content is not in the document when the page is translated
      c.querySelectorAll('[data-i18n]').forEach(el => { el.textContent = t(el.getAttribute('data-i18n')); });
      const title = c.querySelector(`#rfSlot${n}Title`);
      if (title) title.textContent = tf('slot_n', { slot: n });
      const pair = c.querySelector(`#rfPairSlot${n}Btn`);
      const unpair = c.querySelector(`#rfUnpairSlot${n}Btn`);
      if (pair) pair.textContent = tf('pair_slot_n', { slot: n });
      if (unpair) unpair.textContent = tf('unpair_slot_n', { slot: n });
      pair?.addEventListener('click', () => this.pairRfRemote(n));
      unpair?.addEventListener('click', () => this.unpairRfSlot(n));
      c.querySelector(`#rfConfigSlot${n}Btn`)?.addEventListener('click', () => this.openRfConfigModal(n));
      c.querySelector(`#rfRefreshSlot${n}Btn`)?.addEventListener('click', () => this.refreshRfStatus());
      c.querySelector(`#rfPullSlot${n}Btn`)?.addEventListener('click', () => this.pullConfigFromRemote(n));
      c.querySelector(`#rfSlot${n}UpdateBtn`)?.addEventListener('click', () => this.startRemoteOta(n));
      grid.appendChild(c);
    }
  }

  async openRfConfigModal(slot) {
    const modal = document.getElementById('rfConfigModal');
    const title = document.getElementById('rfConfigModalTitle');
    const slotInput = document.getElementById('rfConfigSlot');
    if (!modal) return;
    if (slotInput) slotInput.value = slot;
    if (title) title.textContent = tf('remote_settings_slot_n', { slot: slot });

    /* Load first, show the dialog only with the remote's own values: a failed load used to leave
     * the fields as they were (silently), and saving them pushed those values to the remote.
     * client.request() sends the login token, asks for a login on 401 and reports the Hub's error. */
    try {
      const cfg = await this.client.request(`/api/rf/remote-config?slot=${slot}`);
      if (!cfg || typeof cfg !== 'object') throw new Error(t('no_settings_in_reply'));
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

      const layoutVal = ['list', 'grid', 'carousel'].includes(cfg?.layout) ? cfg.layout : 'theme';
      if (document.getElementById('rfCfgHomeLayout')) document.getElementById('rfCfgHomeLayout').value = layoutVal;

      const physNavVal = Boolean(cfg?.physicalNavEnabled || cfg?.physical_nav);
      if (document.getElementById('rfCfgPhysicalNav')) {
        document.getElementById('rfCfgPhysicalNav').value = physNavVal ? '1' : '0';
        const updateModalLayoutOptions = () => {
          const pNav = document.getElementById('rfCfgPhysicalNav')?.value === '1';
          const lSelect = document.getElementById('rfCfgHomeLayout');
          if (lSelect) {
            if (pNav) { lSelect.value = 'list'; lSelect.disabled = true; }
            else { lSelect.disabled = false; }
          }
        };
        document.getElementById('rfCfgPhysicalNav').onchange = updateModalLayoutOptions;
        updateModalLayoutOptions();
      }

      const confirmAct = typeof cfg.confirm_act !== 'undefined' ? cfg.confirm_act : (typeof cfg.confirmActivity !== 'undefined' ? (cfg.confirmActivity ? 1 : 0) : 1);
      document.getElementById('rfCfgConfirmAct').value = String(confirmAct ? 1 : 0);

      const gyroVal = typeof cfg.gyro !== 'undefined' ? cfg.gyro : (typeof cfg.gyroEnabled !== 'undefined' ? (cfg.gyroEnabled ? 1 : 0) : 1);
      if (document.getElementById('rfCfgGyro')) document.getElementById('rfCfgGyro').value = String(gyroVal ? 1 : 0);

      const btnBacklight = typeof cfg.btn_backlight !== 'undefined' ? cfg.btn_backlight : (typeof cfg.btnBacklight !== 'undefined' ? cfg.btnBacklight : 2);
      if (document.getElementById('rfCfgBtnBacklight')) document.getElementById('rfCfgBtnBacklight').value = String(btnBacklight);

      const nightStart = typeof cfg.night_start !== 'undefined' ? cfg.night_start : (typeof cfg.nightStart !== 'undefined' ? cfg.nightStart : 22);
      const nightEnd = typeof cfg.night_end !== 'undefined' ? cfg.night_end : (typeof cfg.nightEnd !== 'undefined' ? cfg.nightEnd : 7);
      /* the night window in whole hours (what the remote and the RF settings frame carry) */
      [['rfCfgNightStart', nightStart], ['rfCfgNightEnd', nightEnd]].forEach(([id, hour]) => {
        const el = document.getElementById(id);
        if (!el) return;
        if (!el.options.length) {
          el.innerHTML = Array.from({ length: 24 }, (_, h) => `<option value="${h}">${String(h).padStart(2, '0')}:00</option>`).join('');
        }
        const h = parseInt(hour, 10);
        el.value = String(h >= 0 && h <= 23 ? h : 0);
      });

      const powerDebug = typeof cfg.power_debug !== 'undefined' ? cfg.power_debug : (typeof cfg.powerDebug !== 'undefined' ? (cfg.powerDebug ? 1 : 0) : 0);
      if (document.getElementById('rfCfgPowerDebug')) document.getElementById('rfCfgPowerDebug').value = String(powerDebug ? 1 : 0);
    } catch (e) {
      console.warn('Could not fetch existing remote config:', e);
      alert(t('rf_config_load_failed', 'Could not load the remote settings from the Hub (nothing was changed): ') + e.message);
      return;
    }
    modal.style.display = 'flex';
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
    const physical_nav = parseInt(document.getElementById('rfCfgPhysicalNav')?.value || '0', 10);
    const layout = physical_nav === 1 ? 'list' : (document.getElementById('rfCfgHomeLayout')?.value || 'theme');
    const nav_key = parseInt(document.getElementById('rfCfgNavKey')?.value || '139', 10);
    const nav_mode = parseInt(document.getElementById('rfCfgNavMode')?.value || '1', 10);
    const confirm_act = parseInt(document.getElementById('rfCfgConfirmAct')?.value || '1', 10);
    const gyro = parseInt(document.getElementById('rfCfgGyro')?.value || '1', 10);
    const btn_backlight = parseInt(document.getElementById('rfCfgBtnBacklight')?.value || '2', 10);
    const night_start = parseInt(document.getElementById('rfCfgNightStart')?.value || '22', 10);
    const night_end = parseInt(document.getElementById('rfCfgNightEnd')?.value || '7', 10);
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
      layout: layout,
      physical_nav: physical_nav,
      physicalNavEnabled: physical_nav === 1,
      screen_nav_mode: physical_nav === 1 ? 1 : 0,
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
      btn_backlight: btn_backlight,
      btnBacklight: btn_backlight,
      night_start: night_start,
      nightStart: night_start,
      night_end: night_end,
      nightEnd: night_end,
      usbeth: 0,
      usbEth: false,
      power_debug: power_debug,
      powerDebug: power_debug === 1,
      no_screen: no_screen,
      noScreen: no_screen === 1,
      language: language,
      language_id: language_id
    };

    const saveBtn = document.getElementById('rfConfigSaveBtn');
    if (saveBtn) {
      saveBtn.textContent = t('pushing_to_remote');
      saveBtn.disabled = true; /* one RF push at a time */
    }
    try {
      /* client.request(): login token, login prompt on 401, the Hub's own error text on failure.
       * A failed save becomes { ok: false }, so the catch below only sees a failed view refresh. */
      const resp = await this.client.request(`/api/rf/remote-config?slot=${slot}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload),
        timeout: 20000
      }).then(() => ({ ok: true }), (error) => ({ ok: false, error }));
      if (resp.ok) {
        alert(t('rf_config_saved', 'Settings saved and pushed over RF to remote!'));
        this.closeRfConfigModal();
        await this.loadRemoteView();
      } else {
        alert(t('rf_config_failed', 'Failed to save settings to Hub.') + ' ' + resp.error.message);
      }
    } catch (err) {
      /* saved and pushed; only the refresh of the remote view failed */
      console.warn('Remote view refresh after saving failed:', err);
    } finally {
      if (saveBtn) {
        saveBtn.textContent = t('save_push_remote');
        saveBtn.disabled = false;
      }
    }
  }

  async pullConfigFromRemote(slot) {
    const slotName = tf('slot_n', { slot });
    if (!await showConfirm(tf('pull_config_confirm', { slot: slotName }), { title: tf('pull_config_slot_title', { slot: slotName }), okText: t('pull_config', 'Pull Config'), danger: false })) return;
    try {
      await this.client.pullRfConfig(slot);
      alert(tf("config_pull_requested", { slot: slotName }));
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
    const slotName = tf('slot_n', { slot });
    const msg = tf('unpair_slot_confirm', { slot: slotName });
    if (!await showConfirm(msg, { title: tf('unpair_slot_title', { slot: slotName }), okText: tf('unpair_slot_title', { slot: slotName }) })) return;
    try {
      await this.client.unpairRfRemote(slot);
      await this.refreshRfStatus();
    } catch (err) {
      alert(t('error', 'Error') + ': ' + err.message);
    }
  }

  async unpairRfRemote() {
    if (!await showConfirm(t('unpair_all_confirm', 'Unpair all paired RF remotes?'), { title: t('unpair_all_remotes_btn'), okText: t('unpair_all_ok') })) return;
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
      btn.textContent = t('pushing_ellipsis');
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
        btn.textContent = t('push_to_remote');
      }
    }
  }

  async startRemoteOta(slot = 1) {
    if (!await showConfirm(tf('rf_ota_update_confirm', { slot: slot }), { title: t('rf_ota_title', 'RF OTA Update'), okText: t('btn_start_update', 'Start Update'), danger: false })) return;
    const btn = document.getElementById(`rfSlot${slot}UpdateBtn`);
    /* the Update buttons of the other slots: one OTA at a time */
    const otherBtns = [];
    for (let n = 1; n <= RF_SLOT_MAX; n++) {
      const b = n !== slot && document.getElementById(`rfSlot${n}UpdateBtn`);
      if (b) otherBtns.push(b);
    }
    const statusDiv = document.getElementById(`rfSlot${slot}OtaStatus`);
    if (btn) btn.disabled = true;
    otherBtns.forEach(b => { b.disabled = true; });
    if (statusDiv) {
      statusDiv.style.display = 'block';
      statusDiv.textContent = t('rf_ota_initiating');
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
              if (statusDiv) statusDiv.textContent = tf('rf_ota_sending_progress', { pct: pct, sent_chunks: st.sent_chunks, total_chunks: st.total_chunks, sent_bytes: st.sent_bytes, total_bytes: st.total_bytes });
              this.refreshRfStatus().catch(() => { });
            } else if (st.status === 2) {
              clearInterval(pollTimer);
              if (statusDiv) statusDiv.textContent = `✓ ${t('rf_ota_complete')}`;
              if (btn) btn.disabled = false;
              otherBtns.forEach(b => { b.disabled = false; });
              setTimeout(() => this.refreshRfStatus(), 10000);
            } else if (st.status === -1) {
              clearInterval(pollTimer);
              if (statusDiv) statusDiv.textContent = `❌ ${tf('rf_ota_failed_error', { error: st.error || t('transfer_failed') })}`;
              if (btn) btn.disabled = false;
              otherBtns.forEach(b => { b.disabled = false; });
              this.refreshRfStatus().catch(() => { });
            } else if (st.status === 0 && !st.in_progress && startedTransmitting) {
              clearInterval(pollTimer);
              if (statusDiv) statusDiv.textContent = `❌ ${t('rf_ota_aborted')}`;
              if (btn) btn.disabled = false;
              otherBtns.forEach(b => { b.disabled = false; });
              this.refreshRfStatus().catch(() => { });
            }
          } catch { }
        }, 1500);
      } else {
        alert(t("could_not_start_ota") + (res?.error || res?.status || t('unknown_error')));
        if (btn) btn.disabled = false;
        otherBtns.forEach(b => { b.disabled = false; });
      }
    } catch (e) {
      alert(t('could_not_start_ota', 'Failed to trigger RF OTA: ') + e.message);
      if (btn) btn.disabled = false;
      otherBtns.forEach(b => { b.disabled = false; });
    }
  }

  /* The Hub version update.sh would install, without "v", or '' when unknown: the ota branch
   * VERSION first (its signed manifest is what gets installed), GitHub releases only when the
   * branch cannot be read. */
  async fetchOnlineHubVersion(token = '') {
    const base = 'https://raw.githubusercontent.com/iranl/harmony-ng/ota';
    for (const file of ['v2/VERSION', 'VERSION_V2']) {
      try {
        const res = await fetch(`${base}/${file}?_=${Date.now()}`, { cache: 'no-cache' });
        const ver = res.ok ? (await res.text()).trim().replace(/^v/, '') : '';
        if (/^\d+(\.\d+)*$/.test(ver)) return ver;
      } catch (_) { }
    }
    try {
      const headers = { 'Accept': 'application/vnd.github.v3+json' };
      if (token) headers['Authorization'] = `token ${token}`;
      const res = await fetch('https://api.github.com/repos/iranl/harmony-ng/releases/latest', { headers, cache: 'no-cache' });
      if (res.ok) return String((await res.json()).tag_name || '').trim().replace(/^v/, '');
    } catch (_) { }
    return '';
  }

  async checkOnlineVersions() {
    const ver = await this.fetchOnlineHubVersion();
    if (ver) {
      this.onlineVersion = ver;
      this.latestRemoteVersion = ver;
    }
  }

  showUpdatingOverlay(statusText = t('applying_update_reboot')) {
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
      // An update started before this page loaded (or in another tab): follow it to its end.
      // No answer is no information (the Hub may be restarting): the overlay stays.
      if (updateStat?.updating) {
        if (!this.pollTimerActive) {
          this.showUpdatingOverlay(t('hub_update_running_bg'));
          this.watchHubUpdate(updateStat.current_version || updateStat.version || '');
        }
      } else if (updateStat && this.isUpdating && !this.pollTimerActive) {
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
          const targetVer = targetRemote || t('hub_update_new');
          if (title) title.textContent = tf('hub_update_available_ver', { version: targetVer.replace(/^v/, '') });
          if (desc) desc.textContent = tf('hub_update_installed_tap', { version: localVer.replace(/^v/, '') || t('current_version_word') });
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
          let slotLabel = t('all_paired_remotes');
          const ps = rfStat.sync_pending_slot;
          if (Number.isInteger(ps) && ps >= 1 && ps <= RF_SLOT_MAX) slotLabel = tf('remote_slot_n', { slot: ps });
          if (title) title.textContent = tf('remote_settings_pending_push', { target: slotLabel });
          if (desc) {
            const diffInfo = rfStat.sync_pending_diff ? tf('pending_changes_list', { changes: rfStat.sync_pending_diff }) : t('hub_config_changed_push');
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
          if (title) title.textContent = tf('remote_slot_ota_in_progress', { slot: rfStat.ota.slot });
          if (badge) badge.textContent = `${pct}%`;
          if (bar) bar.style.width = `${pct}%`;
          if (desc) {
            const kbSent = Math.round((rfStat.ota.sent_bytes || 0) / 1024);
            const kbTot = Math.round((rfStat.ota.total_bytes || 0) / 1024);
            desc.textContent = tf('rf_ota_top_progress', { sent: rfStat.ota.sent_chunks, total: rfStat.ota.total_chunks, kb_sent: kbSent, kb_total: kbTot });
          }
        } else {
          otaBanner.style.display = 'none';
        }
      }

      // 4. Dry-run simulation active banner
      const dryRunBanner = document.getElementById('hubDryRunBanner');
      if (dryRunBanner) {
        const settings = await this.client.getSettings().catch(() => null);
        if (settings && (settings.dry_run === true || settings.dry_run === 1)) {
          dryRunBanner.style.display = 'flex';
        } else {
          dryRunBanner.style.display = 'none';
        }
      }
    } catch { }
  }
}
