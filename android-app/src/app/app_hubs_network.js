import { HarmonyClient } from '../api.js';
import { getSavedHubs, saveHub, deleteHub, getActiveHub, setActiveHubId } from '../storage.js';
import { createAppToken, revokeAppToken, hubBaseUrl, loginFields, withLoginCheck, finishReplace } from '../app_token.js';
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
import { HarmonyAppAuthUsers } from './app_auth_users.js';

export class HarmonyAppHubsNetwork extends HarmonyAppAuthUsers {
  async showSoftwareVersions() {
    const log = document.getElementById('updateLog');
    try {
      if (log) log.textContent = t('fetching_system_update_status');
      const [sys, stat, rfStat] = await Promise.all([
        this.client.getSystemInfo().catch(() => ({})),
        this.client.getUpdateStatus().catch(() => ({})),
        this.client.getRfStatus().catch(() => ({}))
      ]);

      let remoteVer = '';
      try {
        const ghRes = await fetch('https://api.github.com/repos/iranl/harmony-ng/releases/latest', {
          headers: { 'Accept': 'application/vnd.github.v3+json' }
        });
        if (ghRes.ok) {
          const ghData = await ghRes.json();
          if (ghData.tag_name) {
            remoteVer = ghData.tag_name.startsWith('v') ? ghData.tag_name : `v${ghData.tag_name}`;
          }
        }
      } catch (_) { }

      if (!remoteVer) {
        try {
          let rawRes = await fetch(`https://raw.githubusercontent.com/iranl/harmony-ng/ota/v2/VERSION?_=${Date.now()}`);
          if (!rawRes.ok) {
            rawRes = await fetch(`https://raw.githubusercontent.com/iranl/harmony-ng/ota/VERSION_V2?_=${Date.now()}`);
          }
          if (rawRes.ok) {
            const v = (await rawRes.text()).trim();
            if (v) remoteVer = v.startsWith('v') ? v : `v${v}`;
          }
        } catch (_) { }
      }

      if (!remoteVer && stat.remote_version) {
        const clean = stat.remote_version.replace(/^v/, '').trim();
        if (clean !== '1.14.0' || (sys.version && sys.version.startsWith('1.'))) {
          remoteVer = stat.remote_version.startsWith('v') ? stat.remote_version : `v${stat.remote_version}`;
        }
      }

      const localVer = stat.local_version || ('v' + (sys.version || APP_VERSION));
      const targetRemote = remoteVer || '';
      const hasUpdate = Boolean(targetRemote && localVer && this.isVersionOlder(localVer, targetRemote));

      let remotesInfo = '';
      if (rfStat?.slots && Array.isArray(rfStat.slots)) {
        for (const s of rfStat.slots) {
          if (s.active && s.sw_version && s.sw_version !== 'unknown' && s.sw_version !== 'N/A') {
            remotesInfo += tf('ver_remote_slot', { slot: s.slot, model: s.model || t('unknown_lc'), version: s.sw_version }) + '\n';
          }
        }
      }

      if (log) {
        log.textContent = tf('ver_hub_firmware', { value: sys.firmware || stat.fw_version || '4.15.600' }) + '\n' +
          tf('ver_hub_webui', { value: this.appVersion || APP_VERSION }) + '\n' +
          tf('ver_hub_daemon', { value: sys.version || stat.current_version || APP_VERSION }) + '\n' +
          tf('ver_commit', { value: stat.commit || sys.commit || 'dev' }) + '\n' +
          tf('ver_build', { value: stat.build_time || sys.build_time || 'Release' }) + '\n' +
          remotesInfo +
          tf('ver_local_hub', { value: localVer }) + '\n' +
          tf('ver_online', { value: remoteVer || t('up_to_date') }) + '\n' +
          tf('ver_status', { value: hasUpdate ? `⚠️ ${t('update_available_short')}` : `✓ ${t('up_to_date')}` });
      }
    } catch (err) {
      if (log) log.textContent = tf('version_info_failed', { error: err.message });
    }
  }

  async checkSoftwareUpdate() {
    const log = document.getElementById('updateLog');
    const repoInput = document.getElementById('updateRepo')?.value.trim() || '';
    const tokenInput = document.getElementById('updateToken')?.value.trim() || '';
    try {
      if (log) log.textContent = t('checking_update_repo');
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
          let rawRes = await fetch(`https://raw.githubusercontent.com/iranl/harmony-ng/ota/v2/VERSION?_=${Date.now()}`);
          if (!rawRes.ok) {
            rawRes = await fetch(`https://raw.githubusercontent.com/iranl/harmony-ng/ota/VERSION_V2?_=${Date.now()}`);
          }
          if (rawRes.ok) {
            const rawVer = await rawRes.text();
            remoteVer = rawVer.trim().replace(/^v/, '');
          }
        } catch (_) { }
      }

      const res = await this.client.checkUpdate(true, repoInput, tokenInput, remoteVer);
      const stat = await this.client.getUpdateStatus().catch(() => ({}));
      if (log) {
        const displayRemote = remoteVer ? `v${remoteVer}` : (stat.remote_version || res.remote_version || t('unknown_lc'));
        const curVer = stat.current_version || APP_VERSION;
        const targetVer = remoteVer || (stat.remote_version ? stat.remote_version.replace(/^v/, '') : (res.remote_version ? res.remote_version.replace(/^v/, '') : ''));
        const hasUpdate = Boolean(targetVer && curVer && this.isVersionOlder(curVer, targetVer));
        log.textContent = tf('update_check_result', { local: curVer, remote: displayRemote }) + '\n' +
          tf('ver_status', { value: hasUpdate ? `⚠️ ${tf('update_available_install', { version: displayRemote })}` : `✓ ${t('hub_is_up_to_date')}` });
      }
    } catch (err) {
      if (log) log.textContent = tf('update_check_failed_msg', { error: err.message });
    }
  }

  async installSoftwareUpdate() {
    const repo = document.getElementById('updateRepo')?.value.trim();
    const token = document.getElementById('updateToken')?.value.trim();
    const log = document.getElementById('updateLog');
    if (!await showConfirm(t('update_confirm', 'Download and apply update? Hub services will restart.'), { title: t('software_update'), okText: t('update_btn') })) return;
    try {
      this.showUpdatingOverlay(t('hub_downloading_update'));
      if (log) log.textContent = t('update_applying', 'Applying software update...\nHub is downloading package and will reboot automatically.');

      await this.client.applyUpdate(token, repo);

      const statusEl = document.getElementById('hubUpdatingStatusText');
      if (statusEl) statusEl.textContent = t('hub_updated_rebooting');
      if (log) log.textContent = t('hub_updated_rebooting_wait');

      this.pollTimerActive = true;
      let attempts = 0;
      let sawDrop = false;

      const pollTimer = setInterval(async () => {
        attempts++;
        if (attempts > 60) {
          clearInterval(pollTimer);
          this.pollTimerActive = false;
          this.hideUpdatingOverlay();
          if (log) log.textContent += '\n' + t('update_wait_timed_out');
          return;
        }

        try {
          const stat = await this.client.getUpdateStatus();
          if (!stat || stat.updating) {
            sawDrop = true;
            if (statusEl) statusEl.textContent = t('hub_is_rebooting');
            return;
          }
          if (sawDrop || attempts >= 5) {
            clearInterval(pollTimer);
            this.pollTimerActive = false;
            this.hideUpdatingOverlay();
            if (log) log.textContent = tf('hub_updated_reboot_complete_ver', { version: stat.current_version || stat.version || '' });
            showToast(t('hub_updating_status', 'Hub is updated and reboot complete!'), 'success', 5000);
            await this.refreshData();
          }
        } catch (_) {
          sawDrop = true;
          if (statusEl) statusEl.textContent = t('hub_rebooting_reconnecting');
        }
      }, 3000);
    } catch (err) {
      this.pollTimerActive = false;
      this.hideUpdatingOverlay();
      if (log) log.textContent = t('update_failed', 'Install failed: ') + err.message;
      showToast(t('update_failed', 'Install failed: ') + err.message, 'error');
    }
  }

  async rediscoverMqtt() {
    const status = document.getElementById('sysActionStatus');
    try {
      await this.client.saveSystemAction('rediscover');
      if (status) status.innerHTML = `<span style="color:var(--success)">${t('discovery_triggered')}</span>`;
    } catch (err) {
      if (status) status.innerHTML = `<span style="color:var(--danger)">${escapeHtml(tf('error_msg', { error: err.message }))}</span>`;
    }
  }

  async rebootHub() {
    if (this.isUpdating) {
      showToast(t("reboot_blocked_ota"), 'warning');
      return;
    }
    if (!await showConfirm(t('reboot_confirm', 'Are you sure you want to reboot the Harmony Hub?'), { title: t('reboot_hub'), okText: t('reboot') })) return;
    try {
      await this.client.rebootHub();
      alert(t('reboot_sent', 'Reboot command sent to hub. Reconnecting in 30 seconds...'));
    } catch (err) {
      alert(t('reboot_failed', 'Reboot failed: ') + err.message);
    }
  }

  async cleanStockResources() {
    const disk = await this.client.getSystemDisk().catch(() => null);
    const kb = disk?.stock_resources_kb ?? 0;
    const msg = tf('clean_stock_resources_confirm', { kb });
    if (!await showConfirm(msg, {
      title: t('clean_stock_resources_title', 'Empty Stock Resources'),
      okText: t('empty_resources', 'Empty Resources'),
      danger: true
    })) return;

    try {
      const res = await this.client.cleanStockResources();
      showToast(tf('freed_stock_resources_toast', { kb: res?.freed_kb ?? kb }), 'success');
      await this.refreshStockResources();
      if (typeof this.refreshDiskSpace === 'function') await this.refreshDiskSpace();
    } catch (err) {
      showToast(t('clean_resources_failed', 'Failed to empty stock resources: ') + err.message, 'error');
    }
  }

  async refreshStockResources() {
    const badge = document.getElementById('stockResourcesBadge');
    const detail = document.getElementById('stockResourcesDetail');
    const btn = document.getElementById('btnCleanStockResources');
    if (!badge && !btn) return;

    try {
      const disk = await this.client.getSystemDisk();
      const kb = disk?.stock_resources_kb ?? 0;
      const canEmpty = disk?.can_empty_stock_resources ?? kb > 0;
      if (badge) {
        badge.textContent = `${kb} kB`;
        badge.className = `badge ${kb > 0 ? 'warn' : 'ok'}`;
      }
      if (detail) {
        detail.textContent = kb === 0 ? t('stock_resources_empty')
          : canEmpty ? tf('stock_resources_used', { kb }) : t('stock_resources_not_imported');
      }
      if (btn) {
        btn.disabled = !canEmpty;
        btn.textContent = kb > 0 ? tf('empty_stock_resources_btn', { kb }) : t('stock_resources_cleaned');
      }
    } catch {
      if (badge) badge.textContent = t('unknown');
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
      list.innerHTML = `<div class="muted" style="padding: 16px;">${escapeHtml(t('no_saved_hubs_yet'))}</div>`;
      return;
    }

    list.innerHTML = hubs.map(h => {
      const isCur = this.activeHub && this.activeHub.id === h.id;
      return `
        <div class="card" style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px;">
          <div>
            <div style="display: flex; align-items: center; gap: 8px;">
              <strong>${escapeHtml(h.name)}</strong>
              ${isCur ? `<span class="badge ok">${t('active_lc')}</span>` : ''}
            </div>
            <div class="muted mini">${escapeHtml(h.host)}:${escapeHtml(h.port || 8080)}</div>
          </div>
          <div style="display: flex; gap: 6px;">
            ${!isCur ? `<button type="button" class="btn btn-xs btn-primary btn-select-hub" data-id="${h.id}">${t('connect')}</button>` : ''}
            <button type="button" class="btn btn-xs btn-secondary btn-edit-hub" data-id="${h.id}">${t('edit')}</button>
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
        if (await showConfirm(t('delete_hub_profile_confirm', 'Delete this hub profile?'), { title: t('delete_hub_profile', 'Delete Hub Profile'), okText: t('delete', 'Delete') })) {
          const gone = hubs.find(x => x.id === id);
          if (gone && gone.token && !(await revokeAppToken(hubBaseUrl(gone), gone))) {
            showToast(t('app_token_not_deleted', 'The previous login could not be deleted on the Hub: an admin can delete it in the token list.'), 'warning', 6000);
          }
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
    if (btnLabel) btnLabel.textContent = t('scanning_ellipsis');
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
      if (title) title.textContent = tf('scanning_subnet_port', { subnet: subnet });
      if (sub) sub.textContent = tf('checking_health_found', { count: foundCount });

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

            showToast(tf('found_hub', { name: hubName, ip: ip }, 'Found Harmony Hub: {name} ({ip})'), 'success', 3000);
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
    if (btnLabel) btnLabel.textContent = t('scan_network');
    if (btnIcon) btnIcon.textContent = '🔍';

    if (foundCount > 0) {
      showToast(tf('scan_complete_added', { count: foundCount }, 'Network scan complete! Added {count} Harmony Hub(s).'), 'success');
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
    if (btnLabel) btnLabel.textContent = t('scan_network');
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
        <h3 style="font-size:16px;margin-bottom:12px;">${isEdit ? t('edit_hub_profile') : t('add_new_harmony_hub')}</h3>
        <label>${t('hub_name', 'Hub Name')}</label><input id="hubFormName" class="form-control" value="${escapeHtml(h.name || '')}">
        <label>${t('ip_address_host_lbl', 'IP Address / Host')}</label><input id="hubFormHost" class="form-control" value="${escapeHtml(h.host || '')}">
        <label>${t('http_port_lbl', 'HTTP Port')}</label><input id="hubFormPort" type="number" class="form-control" value="${escapeHtml(h.port || 8089)}">
        <label>${t('user_optional_lbl', 'Username (Optional)')}</label><input id="hubFormUser" class="form-control" value="${escapeHtml(h.username || '')}">
        <label>${t('pwd_optional_lbl', 'Password (Optional)')}</label><input id="hubFormPass" type="password" class="form-control" autocomplete="current-password" placeholder="${h.token ? escapeHtml(t('hub_login_kept_placeholder', 'Logged in: leave empty to keep')) : ''}">
        <div class="muted mini" style="margin-top:4px;">${t('hub_password_not_saved', 'The password is not saved: the app keeps an access token instead.')}</div>
        <div id="hubTestStatus" class="subtle" style="margin-top:8px;"></div>
        <div class="actions" style="margin-top:14px;display:flex;justify-content:space-between;">
          <div style="display:flex;gap:6px;">
            <button type="button" id="btnHubSave" class="btn btn-sm btn-primary">${t('save_hub_btn')}</button>
            <button type="button" id="btnHubTest" class="btn btn-sm btn-secondary">${t('test')}</button>
          </div>
          <button type="button" id="btnHubCancel" class="btn btn-sm btn-ghost">${t('cancel')}</button>
        </div>
      </div>
    `;

    modal.style.display = 'flex';
    const close = () => { modal.style.display = 'none'; };
    modal.querySelector('#btnHubCancel').onclick = close;

    modal.querySelector('#btnHubTest').onclick = async () => {
      const stat = modal.querySelector('#hubTestStatus');
      stat.textContent = t('testing_connection');
      const c = new HarmonyClient({
        host: modal.querySelector('#hubFormHost').value.trim(),
        port: Number(modal.querySelector('#hubFormPort').value) || 8089,
        username: modal.querySelector('#hubFormUser').value.trim(),
        password: modal.querySelector('#hubFormPass').value.trim()
      });
      // the app sends no password with requests: a typed one is checked with one login that is
      // ended at once (nothing is kept)
      c.testConnection = withLoginCheck(c, modal.querySelector('#hubFormUser').value.trim(), modal.querySelector('#hubFormPass').value);
      const res = await c.testConnection();
      if (res.ok) stat.innerHTML = `<span style="color:var(--success)">✓ ${t('connected_excl')}</span>`;
      else stat.innerHTML = `<span style="color:var(--danger)">✗ ${escapeHtml(res.error)}</span>`;
    };

    modal.querySelector('#btnHubSave').onclick = async () => {
      const stat = modal.querySelector('#hubTestStatus');
      const next = {
        ...h,
        name: modal.querySelector('#hubFormName').value.trim() || 'Harmony Hub',
        host: modal.querySelector('#hubFormHost').value.trim() || '127.0.0.1',
        port: Number(modal.querySelector('#hubFormPort').value) || 8089,
        username: modal.querySelector('#hubFormUser').value.trim()
      };
      delete next.password;
      // The login as stored now, in the profile this save replaces (the same id, or the same
      // address, as saveHub() matches): the dialog's copy can be older than a login made since
      const stored = await getSavedHubs();
      const prev = stored.find(x => x.id === next.id || x.host === next.host) || null;
      next.token = prev && prev.token ? prev.token : null;
      next.tokenId = prev && prev.tokenId ? prev.tokenId : null;
      const prevUser = prev ? (prev.username || '') : '';
      let oldKept = false;
      const password = modal.querySelector('#hubFormPass').value;
      if (password) {
        // The password is used once, to create the app's access token; only the token is saved
        stat.textContent = t('hub_logging_in', 'Logging in...');
        let r;
        const replace = prev && prev.token ? { baseUrl: hubBaseUrl(prev), hub: prev } : null;
        try {
          r = await createAppToken(hubBaseUrl(next), next.username, password, { replace });
        } catch (err) {
          // the old token is gone on the Hub already: do not keep it in the profile
          if (err.oldRevoked && prev) await saveHub({ ...prev, token: null, tokenId: null });
          stat.innerHTML = `<span style="color:var(--danger)">✗ ${escapeHtml(err.message)}</span>`;
          return;
        }
        oldKept = await finishReplace(r, replace);
        Object.assign(next, { username: r.username || next.username }, loginFields(r));
      } else if (prev && prev.token) {
        if (next.username !== prevUser) {
          // the token is the other account's login
          stat.innerHTML = `<span style="color:var(--danger)">✗ ${escapeHtml(t('hub_password_for_user', 'Type the password to log in as the new user.'))}</span>`;
          return;
        }
        if (next.host !== prev.host || next.port !== (prev.port || 8089)) {
          // a token belongs to one Hub
          oldKept = !(await revokeAppToken(hubBaseUrl(prev), prev));
          Object.assign(next, loginFields(null));
        }
      }
      if (oldKept) showToast(t('app_token_not_deleted', 'The previous login could not be deleted on the Hub: an admin can delete it in the token list.'), 'warning', 6000);
      Object.keys(h).forEach(k => delete h[k]);
      Object.assign(h, next);
      await saveHub(h);
      await setActiveHubId(h.id);
      await this.loadActiveHub();
      close();
      this.renderHubsList();
    };
  }
}
