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
    this.eliteMapData = { remote: { name: 'Harmony Remote' }, activities: {}, devices: {} };
    this.eliteSelectedBtn = 'Select';
    this.eliteSelectedActivityId = '-1';
    this.eliteContextMode = 'activity';
    this.eliteSelectedDeviceId = null;
    this.kbMods = { ctrl: false, shift: false, alt: false, win: false };
    this.labQueue = [];
    this.labRunning = false;
    this.labRunId = null;
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
    if (err?.message && (err.message.includes('Authentication') || err.message.includes('401'))) {
      if (!this.isHubOnline) this.setHubStatus(true);
      return;
    }
    this.consecutiveFailures++;
    // Only drop offline when 3 consecutive checks fail
    if (this.consecutiveFailures >= 3 && this.isHubOnline) {
      this.setHubStatus(false);
      const desc = document.getElementById('offlineBannerDesc');
      if (desc && this.activeHub) {
        desc.textContent = `Hub unreachable (${this.activeHub.host}): ${err?.message || 'Connection lost'}`;
      }
    }
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
    const hubBtn = document.getElementById('hubSelectorBtn');
    const banner = document.getElementById('hubOfflineBanner');
    const desc = document.getElementById('offlineBannerDesc');
    const content = document.querySelector('.app-content');

    if (dot) {
      dot.className = online ? 'hub-status-dot online' : 'hub-status-dot offline';
      dot.title = online ? 'Hub is online' : 'Hub is offline';
    }
    const name = this.activeHub ? this.activeHub.name : 'Hub';
    const statusText = `${name}: ${online ? 'Online' : 'Offline'}`;
    if (hubBtn) {
      hubBtn.title = this.isNative ? `${statusText} (Tap to manage hubs)` : statusText;
    }
    const pill = document.getElementById('hubStatusPill');
    if (pill) {
      pill.title = this.isNative ? `${statusText} (Tap to manage hubs)` : statusText;
    }
    if (banner) {
      banner.style.display = online ? 'none' : 'flex';
      if (!online && this.activeHub && desc) {
        desc.textContent = `Cannot reach hub at ${this.activeHub.host || 'specified address'}. Reconnecting automatically...`;
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
      if (label) label.textContent = 'Connecting...';
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
          if (manual && label) label.textContent = 'Connected!';
        } else {
          throw new Error(testRes.error || 'Connection timed out');
        }
      }
    } catch (err) {
      if (manual) {
        const desc = document.getElementById('offlineBannerDesc');
        if (desc && this.activeHub) {
          desc.textContent = `Hub unreachable (${this.activeHub.host}): ${err.message || 'Offline'}`;
        }
      }
      this.recordFailure(err);
    } finally {
      this.isReconnecting = false;
      if (manual) {
        setTimeout(() => {
          if (btn) btn.disabled = false;
          if (spinner) spinner.style.display = 'none';
          if (label) label.textContent = 'Retry Connection';
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
        if (changed || this.currentTab === 'activity-remote') {
          this.renderActivityRemote();
        }
        if (changed && this.currentTab === 'overview') this.loadDashboard();
        if (changed && this.currentTab === 'activities') this.loadActivitiesView();
        this.refreshActivityState();
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
        pill.title = 'Manage Hubs';
      }
    } else {
      document.body.classList.add('is-webui');
      const pill = document.getElementById('hubStatusPill');
      if (pill) {
        pill.removeAttribute('role');
        pill.removeAttribute('tabindex');
        pill.title = 'Hub Status';
      }
    }
    this.bindEvents();
    if (typeof window !== 'undefined') {
      window.addEventListener('popstate', (e) => {
        const tab = e.state?.tab || window.location.hash.replace('#', '') || 'activity-remote';
        this.switchTab(tab, false);
      });
      window.addEventListener('harmony-lang-changed', () => {
        this.updateActivityBanner();
        if (this.currentTab === 'overview') this.loadDashboard();
      });
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
    }
    await this.loadActiveHub();
    this.updateIconCacheUI();
  }

}
