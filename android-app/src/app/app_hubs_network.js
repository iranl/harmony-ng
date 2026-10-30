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
import { HarmonyAppAuthUsers } from './app_auth_users.js';

export class HarmonyAppHubsNetwork extends HarmonyAppAuthUsers {
  async showSoftwareVersions() {
    const log = document.getElementById('updateLog');
    try {
      if (log) log.textContent = 'Fetching current system & update status...';
      const [sys, stat] = await Promise.all([
        this.client.getSystemInfo().catch(() => ({})),
        this.client.getUpdateStatus().catch(() => ({}))
      ]);

      let remoteVer = stat.remote_version || '';
      try {
        const rawRes = await fetch(`https://raw.githubusercontent.com/iranl/harmony-ng/ota/VERSION?_=${Date.now()}`);
        if (rawRes.ok) {
          const v = (await rawRes.text()).trim();
          if (v) remoteVer = v.startsWith('v') ? v : `v${v}`;
        }
      } catch (_) { }

      const localVer = stat.local_version || ('v' + (sys.version || APP_VERSION));
      const targetRemote = remoteVer || stat.remote_version || '';
      const hasUpdate = Boolean(targetRemote && localVer && this.isVersionOlder(localVer, targetRemote));

      if (log) {
        log.textContent = `Hub Firmware: ${sys.firmware || stat.fw_version || '4.15.600'}\n` +
          `Hub WebUI: v${this.appVersion || APP_VERSION}\n` +
          `Hub Daemon: v${sys.version || stat.current_version || APP_VERSION}\n` +
          `Commit: ${stat.commit || sys.commit || 'dev'}\n` +
          `Build: ${stat.build_time || sys.build_time || 'Release'}\n` +
          `Local Version: ${localVer}\n` +
          `Remote Version: ${remoteVer || 'Up to date'}\n` +
          `Status: ${hasUpdate ? '⚠️ Update available' : '✓ Up to date'}`;
      }
    } catch (err) {
      if (log) log.textContent = 'Failed to fetch version info: ' + err.message;
    }
  }

  async checkSoftwareUpdate() {
    const log = document.getElementById('updateLog');
    const repoInput = document.getElementById('updateRepo')?.value.trim() || '';
    const tokenInput = document.getElementById('updateToken')?.value.trim() || '';
    try {
      if (log) log.textContent = 'Checking update repository and GitHub releases...';
      let remoteVer = '';
      try {
        const ghHeaders = { 'Accept': 'application/vnd.github.v3+json' };
        if (tokenInput) ghHeaders['Authorization'] = `token ${tokenInput}`;
        const ghRes = await fetch('https://api.github.com/repos/iranl/harmony-ng/releases/latest', { headers: ghHeaders });
        if (ghRes.ok) {
          const ghData = await ghRes.json();
          remoteVer = (ghData.tag_name || '').replace(/^v/, '');
        }
      } catch (_) {
        try {
          const rawRes = await fetch(`https://raw.githubusercontent.com/iranl/harmony-ng/ota/VERSION?_=${Date.now()}`);
          if (rawRes.ok) {
            const rawVer = await rawRes.text();
            remoteVer = rawVer.trim().replace(/^v/, '');
          }
        } catch (_) { }
      }

      const res = await this.client.checkUpdate(true, repoInput, tokenInput, remoteVer);
      const stat = await this.client.getUpdateStatus().catch(() => ({}));
      if (log) {
        const displayRemote = remoteVer ? `v${remoteVer}` : (stat.remote_version || res.remote_version || 'unknown');
        const curVer = stat.current_version || APP_VERSION;
        const targetVer = remoteVer || (stat.remote_version ? stat.remote_version.replace(/^v/, '') : (res.remote_version ? res.remote_version.replace(/^v/, '') : ''));
        const hasUpdate = Boolean(targetVer && curVer && this.isVersionOlder(curVer, targetVer));
        log.textContent = `Local: v${curVer} | Remote: ${displayRemote}\nStatus: ${hasUpdate ? `⚠️ Update available to install (${displayRemote})!` : '✓ Hub is up to date'}`;
      }
    } catch (err) {
      if (log) log.textContent = 'Update check failed: ' + err.message;
    }
  }

  async installSoftwareUpdate() {
    const repo = document.getElementById('updateRepo')?.value.trim();
    const token = document.getElementById('updateToken')?.value.trim();
    const log = document.getElementById('updateLog');
    if (!await showConfirm(t('update_confirm', 'Download and apply update? Hub services will restart.'), { title: 'Software Update', okText: 'Update' })) return;
    try {
      this.showUpdatingOverlay('Hub is downloading and applying software update...');
      if (log) log.textContent = t('update_applying', 'Applying software update...\nHub is downloading package and will reboot automatically.');

      await this.client.applyUpdate(token, repo);

      const statusEl = document.getElementById('hubUpdatingStatusText');
      if (statusEl) statusEl.textContent = 'Hub is updated and is rebooting...';
      if (log) log.textContent = 'Hub is updated and is rebooting. Please wait for reconnection...';

      this.pollTimerActive = true;
      let attempts = 0;
      let sawDrop = false;

      const pollTimer = setInterval(async () => {
        attempts++;
        if (attempts > 60) {
          clearInterval(pollTimer);
          this.pollTimerActive = false;
          this.hideUpdatingOverlay();
          if (log) log.textContent += '\nUpdate wait timed out. Please refresh the page.';
          return;
        }

        try {
          const stat = await this.client.getUpdateStatus();
          if (!stat || stat.updating) {
            sawDrop = true;
            if (statusEl) statusEl.textContent = 'Hub is rebooting...';
            return;
          }
          if (sawDrop || attempts >= 5) {
            clearInterval(pollTimer);
            this.pollTimerActive = false;
            this.hideUpdatingOverlay();
            if (log) log.textContent = `Hub is updated and reboot complete!\nVersion: v${stat.current_version || stat.version || ''}`;
            showToast('Hub is updated and reboot complete!', 'success', 5000);
            await this.refreshData();
          }
        } catch (_) {
          sawDrop = true;
          if (statusEl) statusEl.textContent = 'Hub is rebooting... Reconnecting...';
        }
      }, 3000);
    } catch (err) {
      this.pollTimerActive = false;
      this.hideUpdatingOverlay();
      if (log) log.textContent = t('update_failed', 'Install failed: ') + err.message;
      showToast('Install failed: ' + err.message, 'error');
    }
  }

  async rediscoverMqtt() {
    const status = document.getElementById('sysActionStatus');
    try {
      await this.client.saveSystemAction('rediscover');
      if (status) status.innerHTML = '<span style="color:var(--success)">Discovery triggered!</span>';
    } catch (err) {
      if (status) status.innerHTML = `<span style="color:var(--danger)">Error: ${escapeHtml(err.message)}</span>`;
    }
  }

  async rebootHub() {
    if (this.isUpdating) {
      showToast(t("reboot_blocked_ota"), 'warning');
      return;
    }
    if (!await showConfirm(t('reboot_confirm', 'Are you sure you want to reboot the Harmony Hub?'), { title: 'Reboot Hub', okText: 'Reboot' })) return;
    try {
      await this.client.rebootHub();
      alert(t('reboot_sent', 'Reboot command sent to hub. Reconnecting in 30 seconds...'));
    } catch (err) {
      alert(t('reboot_failed', 'Reboot failed: ') + err.message);
    }
  }

  setupFactoryResetModal() {
    const modal = document.getElementById('factoryResetModal');
    const btnOpen = document.getElementById('btnFactoryResetHub');
    const step1 = document.getElementById('resetModalStep1');
    const step2 = document.getElementById('resetModalStep2');
    const progress = document.getElementById('resetModalProgress');
    const btnCancel1 = document.getElementById('btnResetCancel1');
    const btnCancel2 = document.getElementById('btnResetCancel2');
    const btnProceed1 = document.getElementById('btnResetProceed1');
    const btnExecute = document.getElementById('btnResetExecute');
    const inputConfirm = document.getElementById('resetConfirmInput');

    if (!modal || !btnOpen) return;

    const closeModal = () => {
      modal.style.display = 'none';
      if (inputConfirm) inputConfirm.value = '';
    };

    btnOpen.addEventListener('click', () => {
      step1.style.display = 'block';
      step2.style.display = 'none';
      progress.style.display = 'none';
      if (btnExecute) btnExecute.disabled = true;
      if (inputConfirm) inputConfirm.value = '';
      modal.style.display = 'flex';
    });

    btnCancel1?.addEventListener('click', closeModal);
    btnCancel2?.addEventListener('click', closeModal);

    btnProceed1?.addEventListener('click', () => {
      step1.style.display = 'none';
      step2.style.display = 'block';
      if (inputConfirm) {
        inputConfirm.value = '';
        setTimeout(() => inputConfirm.focus(), 100);
      }
    });

    inputConfirm?.addEventListener('input', () => {
      if (btnExecute) {
        btnExecute.disabled = (inputConfirm.value.trim().toUpperCase() !== 'RESET');
      }
    });

    btnExecute?.addEventListener('click', async () => {
      if (inputConfirm.value.trim().toUpperCase() !== 'RESET') return;
      step2.style.display = 'none';
      progress.style.display = 'block';
      try {
        await this.client.saveSystemAction('factory_reset', { confirm: 'RESET' });
      } catch (err) {
        // Hub reboots quickly and connection drops
      }
    });
  }

  /* -------------------------------------------------------------
   * 13. Saved Hubs View (Multi-Hub Manager)
   * ------------------------------------------------------------- */
  async renderHubsList() {
    const list = document.getElementById('hubsList');
    if (!list) return;

    document.getElementById('btnScanHubs')?.addEventListener('click', () => this.scanNetwork(false));
    document.getElementById('btnStopScan')?.addEventListener('click', () => this.stopNetworkScan());

    const hubs = await getSavedHubs();
    if (!hubs.length) {
      list.innerHTML = '<div class="muted" style="padding: 16px;">No saved hubs yet. Click "+ Add Hub".</div>';
      return;
    }

    list.innerHTML = hubs.map(h => {
      const isCur = this.activeHub && this.activeHub.id === h.id;
      return `
        <div class="card" style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px;">
          <div>
            <div style="display: flex; align-items: center; gap: 8px;">
              <strong>${escapeHtml(h.name)}</strong>
              ${isCur ? '<span class="badge ok">Active</span>' : ''}
            </div>
            <div class="muted mini">${escapeHtml(h.host)}:${escapeHtml(h.port || 8080)}</div>
          </div>
          <div style="display: flex; gap: 6px;">
            ${!isCur ? `<button type="button" class="btn btn-xs btn-primary btn-select-hub" data-id="${h.id}">Connect</button>` : ''}
            <button type="button" class="btn btn-xs btn-secondary btn-edit-hub" data-id="${h.id}">Edit</button>
            <button type="button" class="btn btn-xs btn-danger btn-delete-hub" data-id="${h.id}">✕</button>
          </div>
        </div>
      `;
    }).join('');

    list.querySelectorAll('.btn-select-hub').forEach(b => {
      b.addEventListener('click', async () => {
        await setActiveHubId(b.getAttribute('data-id'));
        await this.loadActiveHub();
        this.renderHubsList();
      });
    });

    list.querySelectorAll('.btn-edit-hub').forEach(b => {
      b.addEventListener('click', () => {
        const h = hubs.find(x => x.id === b.getAttribute('data-id'));
        if (h) this.openEditHubModal(h);
      });
    });

    list.querySelectorAll('.btn-delete-hub').forEach(b => {
      b.addEventListener('click', async () => {
        const id = b.getAttribute('data-id');
        if (await showConfirm('Delete this hub profile?', { title: 'Delete Hub Profile', okText: 'Delete' })) {
          await deleteHub(id);
          await this.loadActiveHub();
          this.renderHubsList();
        }
      });
    });
  }

  openAddHubModal() {
    const defaultHost = (typeof window !== 'undefined' && window.location && window.location.hostname && window.location.hostname !== 'localhost' && window.location.hostname !== '127.0.0.1') ? window.location.hostname : '';
    this.openEditHubModal({ id: 'hub_' + Date.now(), name: 'Harmony Hub', host: defaultHost, port: 8089, wsPort: 8089 }, false);
  }

  async scanNetwork(autoSelect = false) {
    if (this.isScanningNetwork) return;
    this.isScanningNetwork = true;

    const box = document.getElementById('hubScanStatusBox');
    const title = document.getElementById('hubScanTitle');
    const sub = document.getElementById('hubScanSub');
    const btnScan = document.getElementById('btnScanHubs');
    const btnLabel = document.getElementById('scanHubsLabel');
    const btnIcon = document.getElementById('scanHubsIcon');

    if (box) box.style.display = 'block';
    if (btnScan) btnScan.disabled = true;
    if (btnLabel) btnLabel.textContent = 'Scanning...';
    if (btnIcon) btnIcon.textContent = '⏳';

    this._cancelScan = false;

    const subnets = new Set();

    // 1. Current host from browser URL / WebView hostname
    if (typeof window !== 'undefined' && window.location && window.location.hostname) {
      const host = window.location.hostname;
      if (/^\d+\.\d+\.\d+\.\d+$/.test(host) && !host.startsWith('127.')) {
        const parts = host.split('.');
        subnets.add(`${parts[0]}.${parts[1]}.${parts[2]}`);
      }
    }

    // 2. Active or previously configured hubs
    if (this.activeHub && this.activeHub.host && /^\d+\.\d+\.\d+\.\d+$/.test(this.activeHub.host)) {
      if (!this.activeHub.host.startsWith('127.')) {
        const parts = this.activeHub.host.split('.');
        subnets.add(`${parts[0]}.${parts[1]}.${parts[2]}`);
      }
    }
    const saved = await getSavedHubs();
    for (const h of saved) {
      if (h.host && /^\d+\.\d+\.\d+\.\d+$/.test(h.host) && !h.host.startsWith('127.')) {
        const parts = h.host.split('.');
        subnets.add(`${parts[0]}.${parts[1]}.${parts[2]}`);
      }
    }

    // 3. Local interface IPs detected via WebRTC
    try {
      const localIps = await this.detectLocalIps();
      for (const ip of localIps) {
        if (/^\d+\.\d+\.\d+\.\d+$/.test(ip) && !ip.startsWith('127.')) {
          const parts = ip.split('.');
          subnets.add(`${parts[0]}.${parts[1]}.${parts[2]}`);
        }
      }
    } catch { }

    // 4. Fallback only if no network subnet could be detected (e.g. localhost file:// with no saved hubs)
    if (subnets.size === 0) {
      subnets.add('192.168.1');
    }

    let foundCount = 0;
    const subnetList = Array.from(subnets);

    for (const subnet of subnetList) {
      if (this._cancelScan) break;
      if (title) title.textContent = `Scanning ${subnet}.1 – ${subnet}.254 on port 8089...`;
      if (sub) sub.textContent = `Checking /health endpoint (${foundCount} found so far)`;

      const ips = [];
      for (let i = 1; i <= 254; i++) {
        ips.push(`${subnet}.${i}`);
      }

      const concurrency = 35;
      for (let i = 0; i < ips.length; i += concurrency) {
        if (this._cancelScan) break;
        const batch = ips.slice(i, i + concurrency);
        await Promise.all(batch.map(async (ip) => {
          if (this._cancelScan) return;
          const res = await HarmonyClient.checkHubHealth(ip, 8089, 1400);
          if (res.ok) {
            const hubName = await HarmonyClient.fetchHubName(ip, 8089, 1800);
            const hubId = 'hub_' + ip.replace(/\./g, '_');
            const foundHub = {
              id: hubId,
              name: hubName,
              host: ip,
              port: 8089,
              wsPort: 8089
            };
            await saveHub(foundHub);
            foundCount++;

            showToast(`Found Harmony Hub: ${hubName} (${ip})`, 'success', 3000);
            await this.renderHubsList();

            if (autoSelect || !this.activeHub || !this.activeHub.host) {
              await setActiveHubId(hubId);
              await this.loadActiveHub();
            }
          }
        }));
      }

      if (foundCount > 0 && subnetList.indexOf(subnet) === 0) {
        break;
      }
    }

    this.isScanningNetwork = false;
    if (box) box.style.display = 'none';
    if (btnScan) btnScan.disabled = false;
    if (btnLabel) btnLabel.textContent = 'Scan Network';
    if (btnIcon) btnIcon.textContent = '🔍';

    if (foundCount > 0) {
      showToast(`Network scan complete! Added ${foundCount} Harmony Hub(s).`, 'success');
      await this.renderHubsList();
    } else if (!this._cancelScan) {
      showToast(t("scan_complete_none"), 'warning');
      if (!this.activeHub || !this.activeHub.host) {
        this.switchTab('hubs');
      }
    }
  }

  stopNetworkScan() {
    this._cancelScan = true;
    const box = document.getElementById('hubScanStatusBox');
    if (box) box.style.display = 'none';
    const btnScan = document.getElementById('btnScanHubs');
    if (btnScan) btnScan.disabled = false;
    const btnLabel = document.getElementById('scanHubsLabel');
    if (btnLabel) btnLabel.textContent = 'Scan Network';
    const btnIcon = document.getElementById('scanHubsIcon');
    if (btnIcon) btnIcon.textContent = '🔍';
  }

  async detectLocalIps() {
    const ips = [];
    if (typeof RTCPeerConnection === 'undefined') return ips;
    try {
      const pc = new RTCPeerConnection({ iceServers: [] });
      pc.createDataChannel('');
      const offer = await pc.createOffer();
      await pc.setLocalDescription(offer);

      await new Promise((resolve) => {
        const timer = setTimeout(() => {
          try { pc.close(); } catch { }
          resolve();
        }, 1200);

        pc.onicecandidate = (e) => {
          if (!e || !e.candidate || !e.candidate.candidate) return;
          const m = e.candidate.candidate.match(/\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b/);
          if (m && !m[0].startsWith('127.')) {
            ips.push(m[0]);
          }
        };

        pc.onicegatheringstatechange = () => {
          if (pc.iceGatheringState === 'complete') {
            clearTimeout(timer);
            try { pc.close(); } catch { }
            resolve();
          }
        };
      });
    } catch { }
    return ips;
  }

  openEditHubModal(h, isEdit = true) {
    if (!h) h = {};
    if (!h.id) h.id = 'hub_' + Date.now();
    let modal = document.getElementById('hubModal');
    if (!modal) {
      modal = document.createElement('div');
      modal.id = 'hubModal';
      modal.style.cssText = 'position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.7);z-index:2000;display:flex;align-items:center;justify-content:center;padding:16px;';
      document.body.appendChild(modal);
    }

    modal.innerHTML = `
      <div class="card" style="width:100%;max-width:380px;">
        <h3 style="font-size:16px;margin-bottom:12px;">${isEdit ? 'Edit Hub Profile' : 'Add New Harmony Hub'}</h3>
        <label>Hub Name</label><input id="hubFormName" class="form-control" value="${h.name || ''}">
        <label>IP Address / Host</label><input id="hubFormHost" class="form-control" value="${h.host || ''}">
        <label>HTTP Port</label><input id="hubFormPort" type="number" class="form-control" value="${h.port || 8089}">
        <label>Username (Optional)</label><input id="hubFormUser" class="form-control" value="${h.username || ''}">
        <label>Password (Optional)</label><input id="hubFormPass" type="password" class="form-control" value="${h.password || ''}">
        <div id="hubTestStatus" class="subtle" style="margin-top:8px;"></div>
        <div class="actions" style="margin-top:14px;display:flex;justify-content:space-between;">
          <div style="display:flex;gap:6px;">
            <button type="button" id="btnHubSave" class="btn btn-sm btn-primary">Save Hub</button>
            <button type="button" id="btnHubTest" class="btn btn-sm btn-secondary">Test</button>
          </div>
          <button type="button" id="btnHubCancel" class="btn btn-sm btn-ghost">Cancel</button>
        </div>
      </div>
    `;

    modal.style.display = 'flex';
    const close = () => { modal.style.display = 'none'; };
    modal.querySelector('#btnHubCancel').onclick = close;

    modal.querySelector('#btnHubTest').onclick = async () => {
      const stat = modal.querySelector('#hubTestStatus');
      stat.textContent = 'Testing connection...';
      const c = new HarmonyClient({
        host: modal.querySelector('#hubFormHost').value.trim(),
        port: Number(modal.querySelector('#hubFormPort').value) || 8089,
        username: modal.querySelector('#hubFormUser').value.trim(),
        password: modal.querySelector('#hubFormPass').value.trim()
      });
      const res = await c.testConnection();
      if (res.ok) stat.innerHTML = '<span style="color:var(--success)">✓ Connected!</span>';
      else stat.innerHTML = `<span style="color:var(--danger)">✗ ${escapeHtml(res.error)}</span>`;
    };

    modal.querySelector('#btnHubSave').onclick = async () => {
      h.name = modal.querySelector('#hubFormName').value.trim() || 'Harmony Hub';
      h.host = modal.querySelector('#hubFormHost').value.trim() || '127.0.0.1';
      h.port = Number(modal.querySelector('#hubFormPort').value) || 8089;
      h.username = modal.querySelector('#hubFormUser').value.trim();
      h.password = modal.querySelector('#hubFormPass').value.trim();
      await saveHub(h);
      await setActiveHubId(h.id);
      await this.loadActiveHub();
      close();
      this.renderHubsList();
    };
  }
}
