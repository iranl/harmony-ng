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
import { HarmonyAppEventsActions } from './app_events_actions.js';

export class HarmonyAppActivitiesState extends HarmonyAppEventsActions {
  async refreshDiskSpace() {
    try {
      const disk = await this.client.getSystemDisk();
      const flVal = document.getElementById('dashFlashVal');
      const flDet = document.getElementById('dashFlashDetail');
      const hasCache = (disk.cacheTotalMb && disk.cacheTotalMb !== '0.0') || (disk.cacheFreeMb && disk.cacheFreeMb !== '0.0');
      if (flVal) {
        flVal.textContent = `${(hasCache && disk.totalCombinedFreeMb) ? disk.totalCombinedFreeMb : (disk.freeMb || '2.0')} MB Free`;
      }
      if (flDet) {
        if (hasCache) {
          flDet.textContent = `Data: ${disk.freeMb}M/${disk.totalMb}M • Cache: ${disk.cacheFreeMb}M/${disk.cacheTotalMb}M`;
        } else {
          flDet.textContent = `${disk.usedMb || '3.0'} MB used (${disk.totalMb || '5.0'} MB total)`;
        }
      }
      const ovDisk = document.getElementById('ovDiskVal');
      if (ovDisk) {
        if (hasCache) {
          ovDisk.textContent = `Data: ${disk.freeMb}MB free • Cache: ${disk.cacheFreeMb}MB free`;
        } else {
          ovDisk.textContent = `${disk.freeMb || '2.0'} MB free`;
        }
      }
    } catch { }
  }

  async checkHealthAndVersion() {
    try {
      const health = await this.client.getHealth();
      if (health && health.min_app_version) {
        if (this.isNative && this.isVersionOlder(APP_VERSION, health.min_app_version)) {
          this.showOutdatedAppModal(health.min_app_version, APP_VERSION);
        }
      }
    } catch (err) {
      console.warn('Health check note:', err);
    }
    await this.checkOnlineVersions();
    await this.checkBanners();
  }

  isVersionOlder(appVer, minVer) {
    const parse = (v) => String(v).replace(/^v/, '').split('.').map(n => parseInt(n, 10) || 0);
    const a = parse(appVer);
    const m = parse(minVer);
    for (let i = 0; i < Math.max(a.length, m.length); i++) {
      const av = a[i] || 0;
      const mv = m[i] || 0;
      if (av < mv) return true;
      if (av > mv) return false;
    }
    return false;
  }

  showOutdatedAppModal(minVer, curVer) {
    let modal = document.getElementById('outdatedAppModal');
    if (!modal) {
      modal = document.createElement('div');
      modal.id = 'outdatedAppModal';
      modal.style.cssText = 'position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.85);z-index:9999;display:flex;align-items:center;justify-content:center;padding:20px;';
      modal.innerHTML = `
        <div class="card" style="max-width:420px;text-align:center;border-color:var(--danger);box-shadow:0 8px 32px rgba(239,68,68,0.3);">
          <div style="font-size:36px;margin-bottom:8px;">⚠️</div>
          <h3 style="color:var(--danger);margin-bottom:8px;" data-i18n="app_outdated">App Outdated</h3>
          <p style="font-size:14px;color:var(--text-bright);margin-bottom:12px;" data-i18n="app_outdated_msg">
            The connected hub requires a newer version of the Android App. Please update your app.
          </p>
          <div class="muted mini" style="margin-bottom:16px;">
            Current: <strong>v${curVer}</strong> • Required: <strong>v${minVer}+</strong>
          </div>
          <div style="display:flex;gap:8px;justify-content:center;flex-wrap:wrap;">
            <button type="button" class="btn btn-primary" onclick="window.location.reload(true)">Reload</button>
            <button type="button" class="btn btn-secondary" onclick="if(window.caches){caches.keys().then(k=>Promise.all(k.map(n=>caches.delete(n)))).finally(()=>location.reload(true))}else{location.reload(true)}">Clear Cache</button>
            <button type="button" class="btn btn-secondary" onclick="document.getElementById('outdatedAppModal').remove()">Continue Anyway</button>
          </div>
        </div>
      `;
      document.body.appendChild(modal);
    }
  }

  async loadActiveHub() {
    if (!this.isNative) {
      const isBrowser = (typeof window !== 'undefined' && window.location && window.location.hostname);
      const host = isBrowser ? window.location.hostname : '127.0.0.1';
      const port = (isBrowser && window.location.port) ? parseInt(window.location.port, 10) : 8089;
      this.activeHub = {
        id: 'local_hub',
        name: 'Harmony Hub',
        host: host,
        port: port,
        wsPort: port
      };
    } else {
      let saved = await getActiveHub();
      if (!saved) {
        const hubs = await getSavedHubs();
        if (hubs && hubs.length > 0) saved = hubs[0];
      }

      if (saved && saved.host && saved.host !== 'localhost' && saved.host !== '127.0.0.1') {
        this.activeHub = saved;
      } else {
        // Do NOT create a default device for localhost
        this.activeHub = null;
      }
    }

    const lbl = document.getElementById('activeHubLabel');
    const ovLbl = document.getElementById('ovActiveHubLabel');

    if (!this.activeHub || !this.activeHub.host) {
      if (lbl) lbl.textContent = 'No Hub Selected';
      if (ovLbl) ovLbl.textContent = 'Active: None';
      this.client.updateHub(null);
      setStockIconBaseUrl('');
      this.setHubStatus(false);
      this.activitiesData = { activities: [], currentId: '-1', currentName: 'No Hub', isOffline: true };
      this.inventory = { devices: [] };
      this.updateActivityBanner();
      if (this.isNative) {
        setTimeout(() => this.scanNetwork(true), 400);
      }
      return;
    }

    if (lbl) lbl.textContent = this.activeHub.name || 'Harmony Hub';
    if (ovLbl) ovLbl.textContent = `Active: ${this.activeHub.name || this.activeHub.host}`;
    this.client.updateHub(this.activeHub);
    setStockIconBaseUrl(this.client.baseUrl);
    await this.checkHealthAndVersion();
    await this.refreshData();
    this.switchTab(this.currentTab, false);
    this.startHealthMonitoring();
  }

  async refreshData() {
    try {
      const [inv, acts, stat] = await Promise.all([
        this.client.getInventory(),
        this.client.getActivities(),
        this.client.getStatus().catch(() => null)
      ]);
      this.inventory = inv || { devices: [] };
      this.activitiesData = acts || { activities: [], currentId: '-1', currentName: 'PowerOff' };

      const hasIcons = Boolean(stat?.has_icons || getCachedIconsCount() > 0);
      const wasAvailable = stockIconsAvailable;
      setStockIconsAvailable(hasIcons);
      if (hasIcons && (!this.availableIcons || this.availableIcons.length === 0)) {
        this.loadAvailableIcons();
      }
      if (!wasAvailable && hasIcons) {
        if (this.currentTab === 'activity-remote') this.renderActivityRemote();
        else if (this.currentTab === 'control') this.renderControlView();
      }
      this.updateIconCacheUI();

      if (stat) {
        if (stat.role) this.userRole = stat.role;
        if (stat.user) this.currentUser = stat.user;
        if (typeof stat.auth_enabled !== 'undefined') this.authEnabled = Boolean(stat.auth_enabled);
        this.applyRoleRestrictions();
        if (stat.hub_name) {
          const lbl = document.getElementById('activeHubLabel');
          if (lbl) lbl.textContent = stat.hub_name;
          const ovLbl = document.getElementById('ovActiveHubLabel');
          if (ovLbl) ovLbl.textContent = `Active: ${stat.hub_name}`;
          if (this.activeHub) this.activeHub.name = stat.hub_name;
        }
      }

      // Populate deviceCommandsMap and sort commands alphabetically
      this.deviceCommandsMap = {};
      (this.inventory.devices || []).forEach(d => {
        if (d && d.id) {
          const cmds = Array.isArray(d.commands) ? [...d.commands] : [];
          cmds.sort((a, b) => {
            const na = (typeof a === 'string' ? a : (a.name || a.label || '')).toLowerCase();
            const nb = (typeof b === 'string' ? b : (b.name || b.label || '')).toLowerCase();
            return na.localeCompare(nb);
          });
          this.deviceCommandsMap[d.id] = cmds;
          d.commands = cmds;
        }
      });

      this.recordSuccess();
    } catch (err) {
      console.warn('Failed to load hub data:', err);
      const isAuthErr = err?.message && (err.message.includes('Authentication') || err.message.includes('401'));
      this.recordFailure(err);
      this.activitiesData = { activities: [], currentId: '-1', currentName: isAuthErr ? 'Sign In Required' : 'Offline', isOffline: !isAuthErr };
      this.inventory = { devices: [] };
    }
    this.updateActivityBanner();
    this.checkBanners();
    if (this.currentTab === 'overview') this.loadDashboard();
    else if (this.currentTab === 'activity-remote') this.renderActivityRemote();
    else if (this.currentTab === 'activities') this.loadActivitiesView();
    else if (this.currentTab === 'control') this.renderControlView();
  }

  async refreshActivityState() {
    try {
      const acts = await this.client.getActivities();
      if (acts) {
        this.activitiesData = acts;
        this.recordSuccess();
        this.updateActivityBanner();
      }
    } catch (err) {
      this.recordFailure(err);
    }
  }

  switchTab(tabId, pushHistory = true) {
    if (this.userRole === 'user' && !['activities', 'control', 'activity-remote', 'hubs'].includes(tabId)) {
      tabId = 'activities';
    }
    if (!tabId || (tabId === 'hubs' && !this.isNative)) tabId = 'overview';
    this.currentTab = tabId;
    if (pushHistory && typeof window !== 'undefined' && window.history) {
      if (window.location.hash !== `#${tabId}`) {
        window.history.pushState({ tab: tabId }, '', `#${tabId}`);
      }
    }

    // Toggle active view panel
    document.querySelectorAll('.view-panel').forEach(panel => {
      panel.classList.toggle('active', panel.id === `view-${tabId}`);
    });

    // Update top nav tabs active state
    document.querySelectorAll('.nav-tab-btn').forEach(btn => {
      const t = btn.getAttribute('data-tab');
      btn.classList.toggle('active', t === tabId);
    });

    // Update bottom nav active state
    document.querySelectorAll('.app-nav .nav-item').forEach(btn => {
      const t = btn.getAttribute('data-tab');
      btn.classList.toggle('active', t === tabId);
    });

    // Update drawer item active state
    document.querySelectorAll('.drawer-item').forEach(btn => {
      const t = btn.getAttribute('data-view-target');
      btn.classList.toggle('active', t === tabId);
    });

    // Load content for active tab
    if (tabId === 'overview') this.loadDashboard();
    else if (tabId === 'activity-remote') this.renderActivityRemote();
    else if (tabId === 'activities') this.loadActivitiesView();
    else if (tabId === 'control') this.renderControlView();
    else if (tabId === 'ir') this.renderIrView();
    else if (tabId === 'lab') this.initLabView();
    else if (tabId === 'bluetooth') this.loadBtView();
    else if (tabId === 'bt-keyboard') this.loadBtKeyboardView();
    else if (tabId === 'rf-remote') { this.loadEliteView(); this.refreshRfStatus(); }
    else if (tabId === 'remotes') this.loadRemotesView();
    else if (tabId === 'mqtt') this.loadMqttView();
    else if (tabId === 'network') this.loadNetworkView();
    else if (tabId === 'backup') this.loadBackupView();
    else if (tabId === 'system') this.loadSystemView();
    else if (tabId === 'hubs') this.renderHubsList();
  }

  async startActivity(id) {
    const actId = String(id);
    await this.remoteEngine.triggerHaptic();
    const acts = this.activitiesData?.activities || [];
    const aObj = acts.find(x => String(x.id) === actId);
    const actName = aObj ? (aObj.name || aObj.label) : actId;

    this.activitiesData.isTransitioning = true;
    this.activitiesData.transitionTarget = actId;
    this.activitiesData.transitionStep = 1;
    this.activitiesData.transitionTotal = 4;
    this.activitiesData.transitionDesc = `Starting ${actName}...`;
    this.updateActivityBanner();

    try {
      const res = await this.client.startActivity(actId);
      if (res && res.error && !res.ok) {
        alert(res.error);
        this.activitiesData.isTransitioning = false;
        this.updateActivityBanner();
        return;
      }
      this.pollActivityTransition(actId);
    } catch (err) {
      alert('Failed to start activity: ' + (err.message || err));
      this.activitiesData.isTransitioning = false;
      this.updateActivityBanner();
    }
  }

  async stopActivity() {
    await this.remoteEngine.triggerHaptic();
    this.activitiesData.isTransitioning = true;
    this.activitiesData.transitionTarget = '-1';
    this.activitiesData.transitionStep = 1;
    this.activitiesData.transitionTotal = 3;
    this.activitiesData.transitionDesc = 'Powering off devices...';
    this.updateActivityBanner();

    try {
      const res = await this.client.stopActivity();
      if (res && res.error && !res.ok) {
        alert(res.error);
        this.activitiesData.isTransitioning = false;
        this.updateActivityBanner();
        return;
      }
      this.pollActivityTransition('-1');
    } catch (err) {
      alert('Failed to stop activity: ' + (err.message || err));
      this.activitiesData.isTransitioning = false;
      this.updateActivityBanner();
    }
  }

  pollActivityTransition(targetId, attempts = 35) {
    if (this.actPollTimer) {
      clearTimeout(this.actPollTimer);
      this.actPollTimer = null;
    }
    const check = async () => {
      await this.refreshActivityState();
      this.updateActivityBanner();
      if (this.currentTab === 'activity-remote') {
        this.renderActivityRemote();
      }
      const isTrans = Boolean(this.activitiesData && this.activitiesData.isTransitioning);
      const cur = String((this.activitiesData && this.activitiesData.currentId) ?? '-1');
      const targetMatches = (targetId === null || targetId === undefined || cur === String(targetId));

      if (attempts > 0 && (isTrans || !targetMatches)) {
        attempts--;
        this.actPollTimer = setTimeout(check, 600);
      } else {
        this.actPollTimer = null;
        this.wsProgress = null;
        if (this.activitiesData) {
          this.activitiesData.isTransitioning = false;
        }
        this.updateActivityBanner();
        if (this.currentTab === 'activity-remote') {
          this.renderActivityRemote();
        }
      }
    };
    check();
  }

  updateActivityBanner() {
    const listEl = document.getElementById('bannerActivitiesList');
    if (!listEl) return;

    // Filter out extraneous Power Off activity (id = -1 or named PowerOff)
    const acts = (this.activitiesData.activities || []).filter(a => {
      const idStr = String(a.id || '');
      const nameStr = (a.name || a.label || '').toLowerCase().replace(/[\s_-]/g, '');
      return idStr !== '-1' && idStr !== '-1.0' && a.id !== -1 && nameStr !== 'poweroff';
    });
    const curId = String(this.activitiesData.currentId || '-1');
    const curName = (this.activitiesData.currentName || '').toLowerCase().replace(/[\s_-]/g, '');
    const isOff = curId === '-1' || curId === '0' || !curId || curName === 'poweroff';
    const isTransitioning = Boolean(this.activitiesData.isTransitioning || this.wsProgress);

    const pwrBtn = document.getElementById('btnBannerPowerOff');
    if (pwrBtn) {
      pwrBtn.classList.toggle('active', isOff);
      pwrBtn.classList.remove('btn-primary');
      pwrBtn.classList.add('btn-danger');
      pwrBtn.title = isOff ? 'Hub is currently Powered Off' : 'Power off all devices';
      pwrBtn.disabled = isTransitioning;
      if (isTransitioning) pwrBtn.style.opacity = '0.5';
      else if (isOff) pwrBtn.style.opacity = '0.75';
      else pwrBtn.style.opacity = '';
    }

    // Update Transition Progress Card
    const transCard = document.getElementById('bannerTransitionCard');
    const transName = document.getElementById('bannerTransitionName');
    const transDesc = document.getElementById('bannerTransitionDesc');
    const transBar = document.getElementById('bannerTransitionProgress');

    if (transCard) {
      if (isTransitioning) {
        transCard.style.display = 'block';
        const targetId = String(this.wsProgress?.activityId || this.wsProgress?.activity || this.activitiesData?.transitionTarget || '');
        const targetAct = acts.find(a => String(a.id) === targetId);
        let titleText = 'Switching Activity...';
        if (targetId === '-1' || targetId === '0') {
          titleText = 'Powering Off...';
        } else if (targetAct) {
          titleText = `Starting ${targetAct.name || targetAct.label}...`;
        }

        const step = this.wsProgress?.currentStep ?? this.wsProgress?.step ?? this.activitiesData?.transitionStep ?? 0;
        const total = this.wsProgress?.totalSteps ?? this.wsProgress?.total ?? this.activitiesData?.transitionTotal ?? 0;
        const descText = this.wsProgress?.stepDesc || this.wsProgress?.desc || this.activitiesData?.transitionDesc || (step > 0 ? `Step ${step} of ${total}` : 'Please wait...');
        const pct = total > 0 ? Math.min(100, Math.max(10, Math.round((step / total) * 100))) : 40;

        if (transName) transName.textContent = titleText;
        if (transDesc) transDesc.textContent = descText;
        if (transBar) transBar.style.width = `${pct}%`;
      } else {
        transCard.style.display = 'none';
      }
    }

    listEl.innerHTML = acts.map(a => {
      const isActive = !isOff && String(a.id) === curId;
      const isTarget = isTransitioning && String(a.id) === String(this.activitiesData?.transitionTarget);
      return `<button type="button" class="btn btn-xs ${isActive ? 'btn-primary' : 'btn-secondary'} banner-activity-btn ${isActive ? 'active' : ''} ${isTarget ? 'btn-switching' : ''}" data-act-id="${a.id}" ${isTransitioning ? 'disabled style="opacity:0.6;"' : ''}>${getActivityIconSvg(a.icon, a.type, 13)}${escapeHtml(a.name || a.label)}</button>`;
    }).join('');

    listEl.querySelectorAll('.banner-activity-btn').forEach(btn => {
      btn.addEventListener('click', async () => {
        const id = btn.getAttribute('data-act-id');
        if (!isOff && id === curId) return;
        await this.startActivity(id);
      });
    });
  }

  /* -------------------------------------------------------------
   * 1. Activity Remote View
   * ------------------------------------------------------------- */
}
