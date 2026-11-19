import { HarmonyClient } from '../api.js';
import { getSavedHubs, saveHub, deleteHub, getActiveHub, setActiveHubId } from '../storage.js';
import { migrateStoredPasswords } from '../app_token.js';
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

export class HarmonyAppCore {
  constructor() {
    this.isNative = Boolean(typeof window !== 'undefined' && window.Capacitor && window.Capacitor.isNativePlatform && window.Capacitor.isNativePlatform());
    this.client = new HarmonyClient(null);
    this.remoteEngine = new RemoteEngine(this.client);
    this.activeHub = null;
    this.inventory = { devices: [] };
    this.activitiesData = { activities: [], currentId: '-1', currentName: 'PowerOff' };
    this.deviceCommandsMap = {};
    this.currentTab = (typeof window !== 'undefined' && window.location.hash.replace('#', '')) || 'activity-remote';
    this.selectedDeviceId = null;
    this.selectedIrDeviceId = null;
    this.pollTimer = null;
    this.wsProgress = null;
    this.isHubOnline = false;
    this.userRole = 'admin';
    this.currentUser = 'admin';
    this.authEnabled = false;

    // Sub-states
    this.b25MapData = { remote: { name: 'Homatics B25' }, activities: {} };
    this.b25SelectedBtn = 'power';
    this.remoteMapData = { remote: { name: 'Harmony Remote' }, activities: {}, devices: {} };
    this.remoteSelectedBtn = 'Select';
    this.remoteSelectedActivityId = '-1';
    this.remoteContextMode = 'activity';
    this.remoteSelectedDeviceId = null;
    this.kbMods = { ctrl: false, shift: false, alt: false, win: false };
    this.labQueue = [];
    this.labRunning = false;
    this.autoReconnectTimer = null;
    this.consecutiveFailures = 0;
    this.isReconnecting = false;
    this.availableIcons = [];
    this.currentIconPickerTarget = null;
    this.currentIconPickerBadge = null;
    this.currentIconCategory = 'all';
    this._btCalibrateInterval = null;
    this._btCalibratePoll = null;
    this._lastSuggestedBtDelay = null;
    this._lastCalibratedDevId = null;

    this.setupWebSocket();
  }

  escapeHtml(str) {
    return escapeHtml(str);
  }

  recordSuccess() {
    this.consecutiveFailures = 0;
    if (!this.isHubOnline) {
      this.setHubStatus(true);
    }
  }

  recordFailure(err) {
    if (err?.status === 401 || (err?.message && (err.message.includes('Authentication') || err.message.includes('401')))) {
      if (!this.isHubOnline) this.setHubStatus(true);
      return;
    }
    this.consecutiveFailures++;
    // Only drop offline when 3 consecutive checks fail
    if (this.consecutiveFailures >= 3 && this.isHubOnline) {
      this.setHubStatus(false);
      const desc = document.getElementById('offlineBannerDesc');
      if (desc && this.activeHub) {
        desc.textContent = tf('hub_unreachable_detail', { host: this.activeHub.host, error: err?.message || t('connection_lost') });
      }
    }
  }

  // Tooltips of the Hub status dot, selector and pill, in the current language
  updateHubStatusTitles() {
    const online = this.isHubOnline;
    const dot = document.getElementById('activeHubStatusDot');
    if (dot) dot.title = online ? t('hub_is_online') : t('hub_is_offline');
    const name = this.activeHub ? this.activeHub.name : 'Hub';
    const statusText = tf('hub_name_status', { name: name, status: online ? t('online') : t('offline') });
    const title = this.isNative ? tf('tap_to_manage_hubs', { status: statusText }) : statusText;
    const hubBtn = document.getElementById('hubSelectorBtn');
    if (hubBtn) hubBtn.title = title;
    const pill = document.getElementById('hubStatusPill');
    if (pill) pill.title = title;
  }

  setHubStatus(online) {
    const wasOnline = this.isHubOnline;
    this.isHubOnline = Boolean(online);
    if (online) {
      this.consecutiveFailures = 0;
      if (this.activitiesData) this.activitiesData.isOffline = false;
    } else {
      if (this.activitiesData) this.activitiesData.isOffline = true;
    }
    const dot = document.getElementById('activeHubStatusDot');
    const banner = document.getElementById('hubOfflineBanner');
    const desc = document.getElementById('offlineBannerDesc');
    const content = document.querySelector('.app-content');

    if (dot) dot.className = online ? 'hub-status-dot online' : 'hub-status-dot offline';
    this.updateHubStatusTitles();
    if (banner) {
      banner.style.display = online ? 'none' : 'flex';
      if (!online && this.activeHub && desc) {
        desc.textContent = tf('cannot_reach_hub_at', { host: this.activeHub.host || t('specified_address') });
      }
    }
    if (content) {
      content.classList.toggle('hub-offline-disabled', !online);
    }

    if (!online) {
      this.ensureAutoReconnect();
    } else {
      this.stopAutoReconnect();
    }

    if (wasOnline !== this.isHubOnline) {
      this.updateActivityBanner();
      if (this.currentTab === 'activity-remote') this.renderActivityRemote();
      else if (this.currentTab === 'overview') this.loadDashboard();
      else if (this.currentTab === 'activities') this.loadActivitiesView();
    }
  }

  startHealthMonitoring() {
    if (this._healthTimer) clearInterval(this._healthTimer);
    this._healthTimer = setInterval(() => {
      this.pollHubHealth(false);
    }, 15000);
  }

  async pollHubHealth(force = false) {
    if (this.isScanningNetwork || this.isReconnecting) return;
    if (!this.activeHub || !this.activeHub.host) {
      if (this.isHubOnline) this.setHubStatus(false);
      return;
    }

    // 1. If WebSocket is connected, WebSocket is active and pushes events in real-time
    if (this.client.isWsConnected()) {
      this.recordSuccess();
      if (!this.isHubOnline) this.setHubStatus(true);
      return;
    }

    // 2. Fallback to HTTP /health only when WS is disconnected
    try {
      const health = await this.client.getHealth();
      if (health && (health.status === 'ok' || health.service === 'hub_daemon')) {
        const wasOffline = !this.isHubOnline;
        this.recordSuccess();

        if (wasOffline) {
          this.setHubStatus(true);
          if (!this.inventory?.devices?.length) {
            await this.refreshData();
          }
        }

        this.client.connectWebSocket();
      } else {
        throw new Error('Health check returned non-ok');
      }
    } catch (err) {
      this.recordFailure(err);
      if (force || this.consecutiveFailures >= 2) {
        if (this.isHubOnline) {
          this.setHubStatus(false);
        }
      }
    }
  }

  ensureAutoReconnect() {
    if (this.autoReconnectTimer) return;
    this.autoReconnectTimer = setInterval(() => {
      if (!this.isHubOnline && this.activeHub) {
        this.recheckConnection(false);
      }
    }, 3000);
  }

  stopAutoReconnect() {
    if (this.autoReconnectTimer) {
      clearInterval(this.autoReconnectTimer);
      this.autoReconnectTimer = null;
    }
  }

  async recheckConnection(manual = false) {
    if (this.isReconnecting) return;
    this.isReconnecting = true;

    const btn = document.getElementById('btnRetryConn');
    const label = document.getElementById('btnRetryConnLabel');
    const spinner = document.getElementById('retrySpinner');

    if (manual) {
      if (btn) btn.disabled = true;
      if (spinner) spinner.style.display = 'inline-block';
      if (label) label.textContent = t('connecting_ellipsis');
    }

    try {
      if (this.activeHub) {
        const testRes = await this.client.testConnection();
        if (testRes.ok) {
          this.recordSuccess();
          if (!this.client.isWsConnected()) {
            this.client.connectWebSocket();
          }
          await this.refreshData();
          if (manual && label) label.textContent = t('connected_excl');
        } else {
          const testErr = new Error(testRes.error || t('connection_timed_out'));
          testErr.status = testRes.status;
          throw testErr;
        }
      }
    } catch (err) {
      if (manual) {
        const desc = document.getElementById('offlineBannerDesc');
        if (desc && this.activeHub) {
          desc.textContent = tf('hub_unreachable_detail', { host: this.activeHub.host, error: err.message || t('offline') });
        }
      }
      this.recordFailure(err);
    } finally {
      this.isReconnecting = false;
      if (manual) {
        setTimeout(() => {
          if (btn) btn.disabled = false;
          if (spinner) spinner.style.display = 'none';
          if (label) label.textContent = t('retry_connection');
        }, 1000);
      }
    }
  }

  setupWebSocket() {
    this.client.onWsMessage = (data) => {
      if (!data) return;
      if (data.type === 'activity_progress') {
        const isFinished = (data.state === 2 || data.state === 0 || data.state === 'running' || data.state === 'idle' || data.stateCode === 2 || data.stateCode === 0);
        const actId = String(data.activityId || data.activity || '');
        const step = data.currentStep ?? data.step ?? 0;
        const total = data.totalSteps ?? data.total ?? 0;
        const desc = data.stepDesc || data.desc || '';

        this.activitiesData.isTransitioning = !isFinished;
        this.activitiesData.transitionTarget = actId;
        this.activitiesData.transitionStep = step;
        this.activitiesData.transitionTotal = total;
        this.activitiesData.transitionDesc = desc;
        this.wsProgress = isFinished ? null : {
          ...data,
          activity: actId,
          activityId: actId,
          step,
          currentStep: step,
          total,
          totalSteps: total,
          desc,
          stepDesc: desc
        };

        if (isFinished) {
          if (this.actPollTimer) {
            clearTimeout(this.actPollTimer);
            this.actPollTimer = null;
          }
          const finalActId = (data.state === 0 || data.state === 'idle' || data.stateCode === 0 || actId === '-1') ? '-1' : actId;
          this.activitiesData.currentId = finalActId;
          const curAct = (this.activitiesData.activities || []).find(a => String(a.id) === finalActId);
          this.activitiesData.currentName = curAct ? (curAct.name || curAct.label) : (finalActId === '-1' ? 'PowerOff' : finalActId);
          this.refreshActivityState().then(() => {
            this.updateActivityBanner();
            if (this.currentTab === 'activity-remote') this.renderActivityRemote();
            if (this.currentTab === 'overview') this.loadDashboard();
            if (this.currentTab === 'activities') this.loadActivitiesView();
          });
        } else {
          this.updateActivityBanner();
        }
      } else if (data.type === 'activity_state') {
        if (this.actPollTimer) {
          clearTimeout(this.actPollTimer);
          this.actPollTimer = null;
        }
        const actId = (data.state === 0 || String(data.activity) === '-1') ? '-1' : String(data.activity);
        const changed = String(this.activitiesData.currentId) !== actId;
        this.activitiesData.currentId = actId;
        this.activitiesData.isTransitioning = (data.state === 1 || data.state === 3);
        const curAct = (this.activitiesData.activities || []).find(a => String(a.id) === actId);
        this.activitiesData.currentName = curAct ? (curAct.name || curAct.label) : (actId === '-1' ? 'PowerOff' : actId);
        this.wsProgress = null;
        this.updateActivityBanner();
        /* Only on its own tab: the activity remote and the Control page share one RemoteEngine,
         * so rendering it from another tab took the engine away from the Control page.
         * Switching to the activity remote tab renders it anyway. */
        if (this.currentTab === 'activity-remote') {
          this.renderActivityRemote();
        }
        if (changed && this.currentTab === 'overview') this.loadDashboard();
        if (changed && this.currentTab === 'activities') this.loadActivitiesView();
        this.refreshActivityState();
      } else if (data.type === 'bt_status' || data.type === 'rf_status' || data.type === 'config_changed' ||
                 data.type === 'mqtt_status') {
        this.onLiveState(data);
      }
    };
    this.client.onWsStatus = (online) => {
      if (online) {
        this.recordSuccess();
        this.setHubStatus(true);
        if (!this.inventory?.devices?.length) {
          this.refreshData();
        }
      } else {
        this.pollHubHealth(true);
      }
    };
  }

  /* True while a form field inside that view has focus: a live redraw would reset what the
   * user is typing or choosing */
  isEditingIn(viewId) {
    const el = document.activeElement;
    const view = document.getElementById(viewId);
    return Boolean(el && view && view.contains(el) && /^(INPUT|SELECT|TEXTAREA)$/.test(el.tagName));
  }

  /* Coalesce bursts of pushed changes into one refresh per key */
  scheduleLiveRefresh(key, fn, delayMs = 800) {
    this.liveTimers = this.liveTimers || {};
    clearTimeout(this.liveTimers[key]);
    this.liveTimers[key] = setTimeout(() => {
      this.liveTimers[key] = null;
      fn();
    }, delayMs);
  }

  /* bt_status, rf_status and config_changed are pushed by hub_daemon's state watcher when that
   * state changes on the Hub; redraw only what is on screen */
  onLiveState(data) {
    if (data.type === 'bt_status') {
      if (this.currentTab === 'bluetooth' && !this.isEditingIn('view-bluetooth')) this.loadBtView(data);
      if (this.currentTab === 'overview') this.scheduleLiveRefresh('dashboard', () => this.loadDashboard());
    } else if (data.type === 'rf_status') {
      if (this.currentTab === 'rf-remote') this.refreshRfStatus(data);
      this.scheduleLiveRefresh('banners', () => this.checkBanners());
      if (this.currentTab === 'overview') this.scheduleLiveRefresh('dashboard', () => this.loadDashboard());
    } else if (data.type === 'config_changed') {
      this.scheduleLiveRefresh('config', () => this.refreshData());
    } else if (data.type === 'mqtt_status') {
      /* only the badge: reloading the whole view would reset a form being edited */
      if (this.currentTab === 'mqtt') this.setMqttBadge(data);
    }
  }

  // A language change: the static data-i18n texts are already switched; the open view built some
  // texts in JS, so redrawForLanguage() draws what can be drawn again without losing input.
  watchLanguage() {
    window.addEventListener('harmony-lang-changed', () => this.onLanguageChanged());
  }

  onLanguageChanged() {
    this.updateHubStatusTitles();
    this.updateActivityBanner();
    this.redrawForLanguage();
  }

  async init() {
    setLanguage(getInitialLanguage());
    this.startHealthMonitoring();
    if (typeof document !== 'undefined') {
      document.addEventListener('visibilitychange', () => {
        if (!document.hidden && this.activeHub && this.activeHub.host) {
          this.pollHubHealth(true);
        }
      });
    }
    if (typeof window !== 'undefined') {
      window.addEventListener('focus', () => {
        if (this.activeHub && this.activeHub.host) {
          this.pollHubHealth(true);
        }
      });
    }
    if (this.isNative) {
      document.body.classList.add('is-native');
      const pill = document.getElementById('hubStatusPill');
      if (pill) {
        pill.setAttribute('role', 'button');
        pill.setAttribute('tabindex', '0');
        pill.title = t('manage_hubs_btn');
      }
    } else {
      document.body.classList.add('is-webui');
      const pill = document.getElementById('hubStatusPill');
      if (pill) {
        pill.removeAttribute('role');
        pill.removeAttribute('tabindex');
        pill.title = t('hub_status_title');
      }
    }
    this.bindEvents();
    if (typeof window !== 'undefined') {
      window.addEventListener('popstate', (e) => {
        const tab = e.state?.tab || window.location.hash.replace('#', '') || 'activity-remote';
        this.switchTab(tab, false);
      });
      this.watchLanguage();
    }

    if (this.isNative) {
      try {
        const cachedCount = await initIconCache();
        if (cachedCount > 0) {
          setStockIconsAvailable(true);
        }
      } catch (e) {
        console.warn('Failed to initialize icon cache:', e);
      }
      // Profiles from older versions hold the Hub password: keep an access token instead
      try {
        await migrateStoredPasswords();
      } catch (e) {
        console.warn('Failed to update saved Hub logins:', e);
      }
    }
    await this.loadActiveHub();
    this.updateIconCacheUI();
  }

}
