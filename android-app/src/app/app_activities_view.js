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
import { HarmonyAppActivitiesState } from './app_activities_state.js';

export class HarmonyAppActivitiesView extends HarmonyAppActivitiesState {
  async renderActivityRemote() {
    const cont = document.getElementById('activityRemoteContainer');
    if (!cont) return;

    if (!this.activeHub || !this.activeHub.host) {
      cont.innerHTML = `
        <div class="card" style="text-align: center; padding: 36px 16px;">
          <div style="font-size: 36px; margin-bottom: 8px;">🔍</div>
          <h3 style="font-size: 16px; margin-bottom: 8px;">No Harmony Hub Configured</h3>
          <p class="muted" style="font-size: 13px; margin-bottom: 16px;">Scan your network for Hubs or add one manually to start using the remote.</p>
          <div style="display: flex; gap: 8px; justify-content: center;">
            <button type="button" class="btn btn-sm btn-primary" onclick="window.harmonyApp && window.harmonyApp.scanNetwork(true)">Scan Network</button>
            <button type="button" class="btn btn-sm btn-secondary" onclick="window.harmonyApp && window.harmonyApp.switchTab('hubs')">Manage Hubs</button>
          </div>
        </div>
      `;
      return;
    }

    if (!this.isHubOnline) {
      cont.innerHTML = `
        <div class="card" style="text-align: center; padding: 36px 16px;">
          <div style="font-size: 36px; margin-bottom: 8px;">📡</div>
          <h3 style="font-size: 16px; margin-bottom: 8px; color: var(--danger);">Hub Connection Lost</h3>
          <p class="muted" style="font-size: 13px; margin-bottom: 16px;">Unable to reach Harmony Hub at ${escapeHtml(this.activeHub.host)}. Checking health and reconnecting...</p>
          <button type="button" class="btn btn-sm btn-primary" onclick="window.harmonyApp && window.harmonyApp.pollHubHealth(true)">Retry Connection</button>
        </div>
      `;
      return;
    }

    const curId = String(this.activitiesData.currentId || '-1');
    const curName = (this.activitiesData.currentName || '').toLowerCase().replace(/[\s_-]/g, '');
    const isOff = curId === '-1' || curId === '0' || !curId || curName === 'poweroff';

    const curAct = !isOff ? (this.activitiesData.activities || []).find(a => String(a.id) === curId) : null;

    if (isOff || !curAct) {
      const realActs = (this.activitiesData.activities || []).filter(a => {
        const idStr = String(a.id || '');
        const nameStr = (a.name || a.label || '').toLowerCase().replace(/[\s_-]/g, '');
        return idStr !== '-1' && idStr !== '-1.0' && a.id !== -1 && nameStr !== 'poweroff';
      });

      cont.innerHTML = `
        <div class="card" style="text-align: center; padding: 32px 16px;">
          <h3 style="font-size: 16px; margin-bottom: 8px;">Hub is in PowerOff State</h3>
          <p class="muted" style="font-size: 13px; margin-bottom: 16px;">Select an activity from the top banner to turn on devices and use the remote.</p>
          <div style="display: flex; gap: 8px; justify-content: center; flex-wrap: wrap;">
            ${realActs.map(a => `
              <button type="button" class="btn btn-sm btn-primary btn-start-act-idle" data-act-id="${escapeHtml(a.id)}" style="display:inline-flex; align-items:center; gap:6px;">${getActivityIconSvg(a.icon, a.type, 16)}<span>Start ${escapeHtml(a.name || a.label)}</span></button>
            `).join('')}
          </div>
        </div>
      `;
      cont.querySelectorAll('.btn-start-act-idle').forEach(b => {
        b.addEventListener('click', async () => {
          const actId = b.getAttribute('data-act-id');
          await this.startActivity(actId);
        });
      });
      return;
    }

    await this.remoteEngine.render(cont, {
      scope: 'activity',
      target: curAct,
      iconSvg: getActivityIconSvg(curAct.icon, curAct.type, 18),
      availableDevices: this.inventory.devices || [],
      commandsMap: this.deviceCommandsMap,
      onSendCommand: async (devId, cmd) => {
        await this.client.sendCommand(devId, cmd);
      }
    });
  }

  /* -------------------------------------------------------------
   * 2. Dashboard View
   * ------------------------------------------------------------- */
  async loadDashboard() {
    const fwEl = document.getElementById('dashFirmwareVal');
    const utEl = document.getElementById('dashUptimeVal');
    const mqBadge = document.getElementById('dashMqttBadge');
    const mqDet = document.getElementById('dashMqttDetail');
    const invVal = document.getElementById('dashInventoryVal');
    const cmdVal = document.getElementById('dashCommandsVal');
    const actName = document.getElementById('dashActiveActName');
    const actDet = document.getElementById('dashActiveActDetail');

    if (actName) actName.textContent = this.activitiesData.currentName || 'PowerOff';
    if (actDet) actDet.textContent = `ID: ${this.activitiesData.currentId || '-1'}`;

    const devs = this.inventory.devices || [];
    let totalCmds = 0;
    devs.forEach(d => { totalCmds += (d.commands || []).length; });
    if (invVal) invVal.textContent = `${devs.length} devices`;
    if (cmdVal) cmdVal.textContent = `${totalCmds} commands`;

    try {
      const mq = await this.client.getMqttStatus();
      if (mqBadge) {
        mqBadge.className = `badge ${mq.connected ? 'ok' : 'bad'}`;
        mqBadge.textContent = mq.connected ? 'Connected' : 'Disconnected';
      }
      const ovMqBadge = document.getElementById('overviewMqttBadge');
      if (ovMqBadge) {
        ovMqBadge.className = `badge ${mq.connected ? 'ok' : 'bad'}`;
        ovMqBadge.textContent = mq.connected ? 'Connected' : 'Disconnected';
      }
      if (mqDet) mqDet.textContent = mq.broker ? `${mq.broker}:${mq.port}` : (mq.enabled ? 'Bridge enabled' : 'Bridge disabled');
      const dashMqPrev = document.getElementById('dashMqttBrokerPreview');
      if (dashMqPrev) {
        dashMqPrev.textContent = mq.broker ? `${mq.broker}:${mq.port || 1883}` : (mq.enabled ? 'Enabled (no host)' : 'Disabled');
      }
      const hIn = document.getElementById('ovMqttHost');
      const pIn = document.getElementById('ovMqttPort');
      const tIn = document.getElementById('ovMqttTopic');
      if (hIn && mq.broker && !hIn.value) hIn.value = mq.broker;
      if (pIn && mq.port && !pIn.value) pIn.value = mq.port;
      if (tIn && mq.baseTopic && !tIn.value) tIn.value = mq.baseTopic;
    } catch { }

    try {
      const net = await this.client.getNetworkStatus().catch(() => null);
      if (net) {
        const ovIp = document.getElementById('ovIpVal');
        if (ovIp && net.ip) ovIp.textContent = net.ip;
        const ovDetail = document.getElementById('ovNetDetail');
        if (ovDetail) {
          ovDetail.textContent = `${net.active_interface || 'ath0'} (${net.connection_type || 'Wi-Fi'})`;
        }
      }
    } catch { }

    try {
      const status = await this.client.request('/api/status').catch(() => null);
      if (fwEl) fwEl.textContent = 'Linux 2.6.31 (4.15.600)';
      if (status && status.uptime_sec !== undefined) {
        const sec = Number(status.uptime_sec) || 0;
        const days = Math.floor(sec / 86400);
        const hours = Math.floor((sec % 86400) / 3600);
        const mins = Math.floor((sec % 3600) / 60);
        let uptStr = '';
        if (days > 0) uptStr += `${days}d `;
        if (hours > 0 || days > 0) uptStr += `${hours}h `;
        uptStr += `${mins}m`;
        if (utEl) utEl.textContent = uptStr || `${sec}s`;
      } else if (utEl) {
        utEl.textContent = 'Online';
      }
    } catch { }

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

    let svcs = null;
    try {
      svcs = await this.client.getServices().catch(() => null);
    } catch { }

    try {
      const rfBadge = document.getElementById('dashEliteBadge');
      if (rfBadge) {
        if (svcs && svcs.rf_enabled === false) {
          rfBadge.className = 'badge bad';
          rfBadge.textContent = 'RF Disabled';
        } else {
          const rf = await this.client.getRfStatus().catch(() => null);
          if (rf) {
            rfBadge.className = `badge ${rf.paired ? 'ok' : 'subtle'}`;
            rfBadge.textContent = rf.paired ? 'RF Paired' : (rf.pairing_active ? 'RF Pairing' : 'RF Ready');
          } else {
            rfBadge.className = 'badge subtle';
            rfBadge.textContent = 'RF Ready';
          }
        }
      }
    } catch { }

    try {
      const btBadge = document.getElementById('dashBtRemoteBadge');
      const hostBadge = document.getElementById('dashBtHostBadge');
      const kbBadge = document.getElementById('dashBtKbBadge');
      if (svcs && svcs.bt_enabled === false) {
        if (btBadge) { btBadge.className = 'badge bad'; btBadge.textContent = 'BT Disabled'; }
        if (hostBadge) { hostBadge.className = 'badge bad'; hostBadge.textContent = 'BT Disabled'; }
        if (kbBadge) { kbBadge.className = 'badge bad'; kbBadge.textContent = 'BT Disabled'; }
      } else {
        const bt = await this.client.getBtRemotePairStatus().catch(() => null);
        if (btBadge && bt) {
          btBadge.className = `badge ${bt.paired ? 'ok' : 'subtle'}`;
          btBadge.textContent = bt.paired ? 'BLE Paired' : 'BLE Remote';
        }
        const btHost = await this.client.request('/api/bt-status').catch(() => null);
        if (hostBadge && btHost) {
          hostBadge.className = `badge ${btHost.connected ? 'ok' : 'subtle'}`;
          hostBadge.textContent = btHost.connected ? 'Connected' : 'HID Ready';
        }
        if (kbBadge) {
          kbBadge.className = `badge ${btHost && btHost.connected ? 'ok' : 'subtle'}`;
          kbBadge.textContent = btHost && btHost.connected ? 'Ready' : 'Virtual KB';
        }
      }
    } catch { }

    try {
      const upBadge = document.getElementById('dashUpdateBadge');
      const upDetail = document.getElementById('dashUpdateDetail');
      if (upDetail) {
        upDetail.textContent = `v${this.appVersion || APP_VERSION}`;
      }
      if (upBadge) {
        if (this.hasUpdate) {
          upBadge.className = 'badge warn';
          upBadge.textContent = 'Update Available';
        } else {
          upBadge.className = 'badge ok';
          upBadge.textContent = 'Up to date';
        }
      }
    } catch { }
  }

  /* -------------------------------------------------------------
   * 3. Activities Manager View
   * ------------------------------------------------------------- */
  async loadActivitiesView() {
    await this.refreshActivityState();
    const curNameEl = document.getElementById('actCurrentName');
    const curMetaEl = document.getElementById('actCurrentMeta');
    const badgeEl = document.getElementById('actCurrentStatusBadge');

    if (curNameEl) curNameEl.textContent = this.activitiesData.currentName || 'PowerOff';
    if (curMetaEl) curMetaEl.textContent = `Activity ID: ${this.activitiesData.currentId || '-1'}`;
    if (badgeEl) {
      badgeEl.className = `badge ${this.activitiesData.isTransitioning ? 'warn' : 'ok'}`;
      badgeEl.textContent = this.activitiesData.isTransitioning ? 'Switching...' : 'Idle';
    }

    const grid = document.getElementById('actGrid');
    if (!grid) return;

    // Include PowerOff as distinct editable activity
    const acts = [...(this.activitiesData.activities || [])];
    const hasPowerOff = acts.some(a => String(a.id) === '-1' || (a.name || '').toLowerCase() === 'poweroff');
    if (!hasPowerOff) {
      acts.unshift({
        id: '-1',
        name: 'PowerOff',
        type: 'PowerOff',
        label: 'PowerOff',
        startSequence: [],
        stopSequence: [],
        deviceIds: []
      });
    }

    if (!acts.length) {
      grid.innerHTML = '<div class="muted">No configured activities found.</div>';
      return;
    }

    grid.innerHTML = acts.map(a => {
      const devIds = a.deviceIds || [];
      const devObjs = devIds.map(did => {
        const d = (this.inventory.devices || []).find(x => String(x.id) === String(did));
        return {
          id: did,
          name: d ? (d.name || d.model || did) : did,
          iconSvg: d ? getDeviceIconSvg(d.type, d.manufacturer, d.model, d.icon || d.Icon || '', 12) : ''
        };
      });
      const startCount = (a.startSequence || []).length;
      const stopCount = (a.stopSequence || []).length;
      const isPowerOff = String(a.id) === '-1' || (a.name || '').toLowerCase() === 'poweroff';
      const isRunning = isPowerOff ?
        (String(this.activitiesData.currentId) === '-1' || !this.activitiesData.currentId) :
        (String(a.id) === String(this.activitiesData.currentId));
      const statusBadgeText = isPowerOff ? (isRunning ? 'Off' : 'Standby') : (isRunning ? 'Active' : 'Configured');
      return `
        <div class="card" style="display: flex; flex-direction: column; justify-content: space-between; gap: 10px;">
          <div>
            <div style="display: flex; justify-content: space-between; align-items: flex-start; margin-bottom: 6px;">
              <strong style="font-size: 15px; display: inline-flex; align-items: center; gap: 6px;">${getActivityIconSvg(a.icon, a.type, 18)}${escapeHtml(a.name || a.label)}</strong>
              <span class="badge ${isRunning ? 'ok' : ''}">${statusBadgeText}</span>
            </div>
            <div class="muted mini" style="margin-bottom: 8px;">${startCount} start steps • ${stopCount} stop steps • ${devIds.length} devices</div>
            <div style="display: flex; flex-wrap: wrap; gap: 4px; margin-bottom: 8px;">
              ${devObjs.map(d => `<span style="background:var(--bg-btn); font-size:11px; padding:2px 8px; border-radius:4px; border:1px solid var(--border); display:inline-flex; align-items:center; gap:4px;">${d.iconSvg}<span>${escapeHtml(d.name)}</span></span>`).join('')}
            </div>
          </div>
          <div style="display: flex; gap: 6px; margin-top: 6px;">
            <button type="button" class="btn btn-xs ${isPowerOff ? 'btn-danger' : (isRunning ? 'btn-ghost' : 'btn-primary')} btn-act-run" data-id="${a.id}" style="flex: 1;">${isRunning ? (isPowerOff ? 'Powered Off' : 'Active') : (isPowerOff ? 'Turn Off' : 'Run')}</button>
            <button type="button" class="btn btn-xs btn-secondary btn-act-edit" data-id="${a.id}" style="flex: 1;">Edit & Steps</button>
            <button type="button" class="btn btn-xs btn-secondary btn-act-icon" data-id="${a.id}" title="Change Icon" style="padding: 0 8px;">🎨</button>
          </div>
        </div>
      `;
    }).join('');

    grid.querySelectorAll('.btn-act-run').forEach(b => {
      b.addEventListener('click', async () => {
        const id = b.getAttribute('data-id');
        if (id === '-1') {
          await this.stopCurrentActivity();
        } else {
          await this.startActivity(id);
        }
      });
    });

    grid.querySelectorAll('.btn-act-edit').forEach(b => {
      b.addEventListener('click', () => {
        const id = b.getAttribute('data-id');
        const act = acts.find(x => String(x.id) === String(id));
        this.openActivityEditor(act);
      });
    });

    grid.querySelectorAll('.btn-act-icon').forEach(b => {
      b.addEventListener('click', () => {
        const id = b.getAttribute('data-id');
        const act = acts.find(x => String(x.id) === String(id));
        if (!act) return;
        this.openIconPicker(async (selectedIcon) => {
          act.icon = selectedIcon;
          try {
            await this.client.updateActivity({ ...act, icon: selectedIcon });
            showToast(`Updated icon for "${act.name}"`, 'success');
            await this.refreshData();
            await this.loadActivitiesView();
          } catch (e) {
            showToast(t("fail_update_act") + e.message, 'error');
          }
        }, null, null, { type: 'activity', id: act.id });
      });
    });

    // Populate transition simulator dropdowns
    const fromSel = document.getElementById('previewFromAct');
    const toSel = document.getElementById('previewToAct');
    if (fromSel && toSel) {
      const opts = acts.map(a => `<option value="${escapeHtml(a.id)}">${escapeHtml(a.name || a.label)}</option>`).join('');
      fromSel.innerHTML = `<option value="-1">PowerOff</option>${opts}`;
      toSel.innerHTML = `${opts}<option value="-1">PowerOff</option>`;
    }

    this.applyRoleRestrictions();
  }

}
