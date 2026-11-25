import { HarmonyAppHubsNetwork } from './app_hubs_network.js';
import { escapeHtml, showToast, showConfirm } from './app_icons_ui.js';
import { t, tf, tfHtml } from '../i18n.js';
import { pluginFieldHtml } from './plugin_fields.js';

export class HarmonyAppPlugins extends HarmonyAppHubsNetwork {
  constructor() {
    super();
    this.installedPlugins = [];
    this.marketplaceCatalog = [];
    this.pluginRepositories = [];
    this.activePluginTab = 'installed';
  }

  async loadPluginsView() {
    this.bindPluginEvents();
    await this.refreshPluginsList();
  }

  bindPluginEvents() {
    if (this._pluginEventsBound) return;
    this._pluginEventsBound = true;

    // Segmented tab switching: installed, marketplace, repos
    document.querySelectorAll('[data-plugin-tab]').forEach(btn => {
      btn.addEventListener('click', () => {
        const tab = btn.getAttribute('data-plugin-tab');
        this.switchPluginTab(tab);
      });
    });

    // Refresh button
    document.getElementById('btnRefreshPlugins')?.addEventListener('click', () => {
      if (this.activePluginTab === 'marketplace') {
        this.refreshMarketplace(true);
      } else if (this.activePluginTab === 'repos') {
        this.refreshRepositories();
      } else {
        this.refreshPluginsList();
      }
    });

    // Upload archive (.tar.gz) button & file input
    const fileInput = document.getElementById('pluginArchiveFileInput');
    document.getElementById('btnUploadPluginArchive')?.addEventListener('click', () => {
      fileInput?.click();
    });

    // Dev direct download from raw GitHub
    document.getElementById('btnDevDownloadPlugin')?.addEventListener('click', async () => {
      const url = prompt(t('plg_dev_prompt'));
      if (!url || !url.trim()) return;
      showToast(t('plg_dev_downloading'), 'info');
      try {
        const res = await this.client.installPlugin(url.trim(), true);
        if (res && res.error) throw new Error(res.error);
        showToast(t('plg_dev_installed'), 'success');
        await this.refreshPluginsList();
      } catch (err) {
        showToast(tf('plg_dev_install_failed', { error: err.message }), 'error');
      }
    });

    fileInput?.addEventListener('change', async (e) => {
      const file = e.target.files?.[0];
      if (!file) return;
      await this.handlePluginArchiveUpload(file);
      fileInput.value = '';
    });

    // Add repository form
    document.getElementById('btnAddRepoBtn')?.addEventListener('click', async () => {
      const input = document.getElementById('newRepoUrlInput');
      const url = input?.value.trim();
      if (!url || !url.startsWith('http')) {
        showToast(t('plg_repo_url_invalid'), 'error');
        return;
      }
      try {
        await this.client.updatePluginRepositories('add', url);
        this._marketplaceStale = true;
        showToast(t('plg_repo_added'), 'success');
        if (input) input.value = '';
        await this.refreshRepositories();
      } catch (err) {
        showToast(tf('plg_repo_add_failed', { error: err.message }), 'error');
      }
    });

    // Listen to newDeviceType change to show/hide plugin config fields
    document.getElementById('newDeviceType')?.addEventListener('change', (e) => {
      this.handleDeviceTypeChanged(e.target.value);
    });
  }

  switchPluginTab(tab) {
    this.activePluginTab = tab;
    document.querySelectorAll('[data-plugin-tab]').forEach(b => {
      b.classList.toggle('active', b.getAttribute('data-plugin-tab') === tab);
    });

    const pInstalled = document.getElementById('pluginPanelInstalled');
    const pMarketplace = document.getElementById('pluginPanelMarketplace');
    const pRepos = document.getElementById('pluginPanelRepos');

    if (pInstalled) pInstalled.style.display = (tab === 'installed') ? 'block' : 'none';
    if (pMarketplace) pMarketplace.style.display = (tab === 'marketplace') ? 'block' : 'none';
    if (pRepos) pRepos.style.display = (tab === 'repos') ? 'block' : 'none';

    if (tab === 'installed') this.refreshPluginsList();
    else if (tab === 'marketplace') this.refreshMarketplace(!!this._marketplaceStale);
    else if (tab === 'repos') this.refreshRepositories();
  }

  async refreshPluginsList() {
    const container = document.getElementById('installedPluginsList');
    const statusEl = document.getElementById('pluginsStatusBadge');
    if (!container) return;

    try {
      if (statusEl) statusEl.textContent = t('loading');
      const res = await this.client.getPlugins();
      this.installedPlugins = Array.isArray(res) ? res : (res?.plugins || []);
      this.storage = res?.storage || null;

      if (this.storage) {
        const banner = document.getElementById('pluginStorageBanner');
        const freeText = document.getElementById('pluginStorageFreeText');
        const totalText = document.getElementById('pluginStorageTotalText');
        const pctText = document.getElementById('pluginStoragePctText');
        const bar = document.getElementById('pluginStorageProgressBar');
        if (banner && freeText && totalText && pctText && bar) {
          freeText.textContent = tf('plg_storage_free', { size: this.storage.free_formatted || '0 B' });
          totalText.textContent = tf('plg_storage_total', { size: this.storage.total_formatted || '0 B' });
          const pct = this.storage.total_bytes > 0
            ? Math.round((this.storage.used_bytes / this.storage.total_bytes) * 100)
            : 0;
          pctText.textContent = tf('plg_storage_used', { pct });
          bar.style.width = `${pct}%`;
          bar.style.background = pct > 90 ? 'var(--danger)' : 'var(--primary)';
          banner.style.display = 'block';
        }
      }

      if (statusEl) {
        const runningCount = this.installedPlugins.filter(p => p.running).length;
        statusEl.textContent = tf('plg_count_status', { installed: this.installedPlugins.length, active: runningCount });
      }

      this.renderInstalledPlugins();
      this.updateDeviceStudioPluginTypes();
    } catch (err) {
      if (statusEl) statusEl.textContent = t('plg_error');
      container.innerHTML = `<div class="card error" style="padding: 12px; color: var(--danger);">${tfHtml('plg_load_failed', { error: err.message })}</div>`;
    }
  }

  renderInstalledPlugins() {
    const container = document.getElementById('installedPluginsList');
    if (!container) return;

    if (!this.installedPlugins || this.installedPlugins.length === 0) {
      container.innerHTML = `
        <div class="card" style="text-align: center; padding: 24px; color: var(--text-muted);">
          <div style="font-size: 24px; margin-bottom: 8px;">🔌</div>
          <div style="font-weight: 600; margin-bottom: 4px;">${t('plg_none_title')}</div>
          <div class="mini">${t('plg_none_hint')}</div>
          <button type="button" class="btn btn-sm btn-primary" style="margin-top: 14px;" onclick="window.harmonyApp && window.harmonyApp.switchPluginTab('marketplace')">${t('plg_browse_marketplace')}</button>
        </div>
      `;
      return;
    }

    container.innerHTML = this.installedPlugins.map(p => {
      const isRunning = !!p.running;
      const isEnabled = p.enabled !== false;
      const badgeColor = isRunning ? 'var(--success)' : (isEnabled ? 'var(--warning)' : 'var(--text-muted)');
      const badgeStyle = `font-size: 11px; border: 1px solid ${badgeColor}; color: ${badgeColor}; background: transparent;`;  // i18n-ok: CSS
      const badgeText = isRunning ? tf('plg_status_running', { pid: p.pid || '?' }) : (isEnabled ? t('plg_status_stopped') : t('plg_status_disabled'));

      return `
        <div class="card" style="margin-bottom: 12px; padding: 14px;">
          <div style="display: flex; justify-content: space-between; align-items: flex-start; gap: 10px; margin-bottom: 8px;">
            <div>
              <div style="display: flex; align-items: center; gap: 8px;">
                <strong style="font-size: 15px; color: var(--text-main);">${escapeHtml(p.name || p.id)}</strong>
                <span class="badge" style="${badgeStyle}">${escapeHtml(badgeText)}</span>
                <span class="muted mini">v${escapeHtml(p.version || '1.0.0')}</span>
                <span class="badge" style="font-size: 11px; border: 1px solid var(--border); color: var(--text-muted); background: transparent;">💾 ${escapeHtml(p.size_formatted || '0 B')}</span>
              </div>
              <div class="muted mini" style="margin-top: 4px;">${escapeHtml(p.description || t('plg_default_desc'))}</div>
            </div>
            <label style="display: flex; align-items: center; gap: 6px; cursor: pointer; font-size: 13px;">
              <input type="checkbox" class="plugin-enable-toggle" data-plugin-id="${escapeHtml(p.id)}" ${isEnabled ? 'checked' : ''}>
              <span>${t('plg_enabled')}</span>
            </label>
          </div>

          ${p.device_definition ? `
            <div style="font-size: 12px; background: var(--bg-card); border: 1px solid var(--border); border-radius: 6px; padding: 6px 10px; margin: 8px 0;">
              <span class="muted">${t('plg_device_support')}</span> <strong>${escapeHtml(p.device_definition.display_name || p.device_definition.device_type)}</strong>
              ${Array.isArray(p.device_definition.config_fields) ? `<span class="muted mini" style="margin-left: 6px;">(${escapeHtml(p.device_definition.config_fields.map(f => f?.label || f?.key || '').join(', '))})</span>` : ''}
            </div>
          ` : ''}

          <div style="display: flex; justify-content: space-between; align-items: center; margin-top: 10px; border-top: 1px solid var(--border); padding-top: 8px;">
            <div style="display: flex; gap: 6px;">
              <button type="button" class="btn btn-xs btn-outline btn-plugin-restart" data-plugin-id="${escapeHtml(p.id)}" ${!isEnabled ? 'disabled' : ''}>${t('plg_restart')}</button>
              ${isRunning
                ? `<button type="button" class="btn btn-xs btn-outline btn-plugin-stop" data-plugin-id="${escapeHtml(p.id)}">${t('plg_stop')}</button>`
                : `<button type="button" class="btn btn-xs btn-outline btn-plugin-start" data-plugin-id="${escapeHtml(p.id)}" ${!isEnabled ? 'disabled' : ''}>${t('plg_start')}</button>`
              }
              <button type="button" class="btn btn-xs btn-outline btn-plugin-pull-dev" data-plugin-id="${escapeHtml(p.id)}" title="${t('plg_pull_title')}">${t('plg_pull_btn')}</button>
              ${p.device_definition ? `
                <button type="button" class="btn btn-xs btn-primary btn-plugin-add-dev" data-plugin-id="${escapeHtml(p.id)}" ${!isEnabled ? 'disabled' : ''}>${t('plg_add_device')}</button>
              ` : ''}
              ${(p.manifest?.pairing?.supported || p.pairing?.supported) ? `
                <button type="button" class="btn btn-xs btn-outline btn-plugin-pair-card" data-plugin-id="${escapeHtml(p.id)}" ${!isEnabled ? 'disabled' : ''}>🔗 ${escapeHtml(p.manifest?.pairing?.button_label || p.pairing?.button_label || t('plg_pair_with_tv'))}</button>
              ` : ''}
            </div>
            <button type="button" class="btn btn-xs btn-ghost btn-plugin-uninstall" data-plugin-id="${escapeHtml(p.id)}" style="color: var(--danger);">${t('plg_uninstall')}</button>
          </div>
        </div>
      `;
    }).join('');

    // Bind action buttons
    container.querySelectorAll('.btn-plugin-pull-dev').forEach(btn => {
      btn.addEventListener('click', async () => {
        const id = btn.getAttribute('data-plugin-id');
        btn.disabled = true;
        btn.textContent = t('plg_pulling');
        showToast(tf('plg_pulling_files', { id }), 'info');
        try {
          const res = await this.client.pullPlugin(id);
          if (res && res.error) throw new Error(res.error);
          showToast(tf('plg_pulled', { id }), 'success');
          await this.refreshPluginsList();
        } catch (err) {
          showToast(tf('plg_pull_failed', { error: err.message }), 'error');
        } finally {
          btn.disabled = false;
          btn.textContent = t('plg_pull_btn');
        }
      });
    });

    container.querySelectorAll('.plugin-enable-toggle').forEach(input => {
      input.addEventListener('change', async (e) => {
        const id = input.getAttribute('data-plugin-id');
        const enable = input.checked;
        try {
          await this.client.pluginAction(id, enable ? 'enable' : 'disable');
          showToast(enable ? tf('plg_toast_enabled', { id }) : tf('plg_toast_disabled', { id }), 'success');
          await this.refreshPluginsList();
        } catch (err) {
          showToast(tf('plg_action_failed', { error: err.message }), 'error');
          input.checked = !enable;
        }
      });
    });

    container.querySelectorAll('.btn-plugin-restart').forEach(btn => {
      btn.addEventListener('click', async () => {
        const id = btn.getAttribute('data-plugin-id');
        btn.disabled = true;
        try {
          await this.client.pluginAction(id, 'restart');
          showToast(tf('plg_toast_restarted', { id }), 'success');
          await this.refreshPluginsList();
        } catch (err) {
          showToast(tf('plg_restart_failed', { error: err.message }), 'error');
        } finally {
          btn.disabled = false;
        }
      });
    });

    container.querySelectorAll('.btn-plugin-start').forEach(btn => {
      btn.addEventListener('click', async () => {
        const id = btn.getAttribute('data-plugin-id');
        btn.disabled = true;
        try {
          await this.client.pluginAction(id, 'start');
          showToast(tf('plg_toast_started', { id }), 'success');
          await this.refreshPluginsList();
        } catch (err) {
          showToast(tf('plg_start_failed', { error: err.message }), 'error');
        } finally {
          btn.disabled = false;
        }
      });
    });

    container.querySelectorAll('.btn-plugin-stop').forEach(btn => {
      btn.addEventListener('click', async () => {
        const id = btn.getAttribute('data-plugin-id');
        btn.disabled = true;
        try {
          await this.client.pluginAction(id, 'stop');
          showToast(tf('plg_toast_stopped', { id }), 'success');
          await this.refreshPluginsList();
        } catch (err) {
          showToast(tf('plg_stop_failed', { error: err.message }), 'error');
        } finally {
          btn.disabled = false;
        }
      });
    });

    container.querySelectorAll('.btn-plugin-uninstall').forEach(btn => {
      btn.addEventListener('click', async () => {
        const id = btn.getAttribute('data-plugin-id');
        const ok = await showConfirm(tf('plg_uninstall_confirm_full', { id }));
        if (!ok) return;
        btn.disabled = true;
        try {
          await this.client.uninstallPlugin(id);
          showToast(tf('plg_toast_uninstalled', { id }), 'success');
          await this.refreshPluginsList();
        } catch (err) {
          showToast(tf('plg_uninstall_failed', { error: err.message }), 'error');
        } finally {
          btn.disabled = false;
        }
      });
    });

    container.querySelectorAll('.btn-plugin-add-dev').forEach(btn => {
      btn.addEventListener('click', () => {
        const id = btn.getAttribute('data-plugin-id');
        this.openAddDeviceForPlugin(id);
      });
    });

    container.querySelectorAll('.btn-plugin-pair-card').forEach(btn => {
      btn.addEventListener('click', () => {
        const id = btn.getAttribute('data-plugin-id');
        this.openPluginPairingModal(id);
      });
    });
  }

  async refreshMarketplace(forceReload = false) {
    const container = document.getElementById('marketplaceCatalogList');
    if (!container) return;

    try {
      container.innerHTML = '<div class="muted mini" style="padding: 16px; text-align: center;">' + t('plg_market_fetching') + '</div>';
      const res = await this.client.getPluginMarketplace(forceReload);
      if (forceReload) this._marketplaceStale = false;
      const list = Array.isArray(res) ? res : (res?.marketplace || res?.plugins || []);
      // a null or malformed entry of a repository must not break the whole list
      this.marketplaceCatalog = (Array.isArray(list) ? list : []).filter(item => item && typeof item === 'object' && item.id);
      this.renderMarketplaceCatalog();
    } catch (err) {
      container.innerHTML = `<div class="card error" style="padding: 12px; color: var(--danger);">${tfHtml('plg_market_load_failed', { error: err.message })}</div>`;
    }
  }

  renderMarketplaceCatalog() {
    const container = document.getElementById('marketplaceCatalogList');
    if (!container) return;

    if (!this.marketplaceCatalog || this.marketplaceCatalog.length === 0) {
      container.innerHTML = `
        <div class="card" style="text-align: center; padding: 24px; color: var(--text-muted);">
          <div>${t('plg_market_empty')}</div>
          <div class="mini" style="margin-top: 4px;">${t('plg_market_check_repos')}</div>
        </div>
      `;
      return;
    }

    const installedMap = new Map((this.installedPlugins || []).map(p => [p.id, p]));

    container.innerHTML = this.marketplaceCatalog.map(item => {
      const installed = installedMap.get(item.id);
      const isUpToDate = installed && installed.version === item.version;
      const isUpdateAvailable = installed && installed.version !== item.version;

      return `
        <div class="card" style="margin-bottom: 12px; padding: 14px;">
          <div style="display: flex; justify-content: space-between; align-items: flex-start; gap: 10px;">
            <div>
              <div style="display: flex; align-items: center; gap: 8px;">
                <strong style="font-size: 15px; color: var(--text-main);">${escapeHtml(item.name || item.id)}</strong>
                <span class="muted mini">v${escapeHtml(item.version || '1.0.0')}</span>
                ${item.author ? `<span class="muted mini">${tfHtml('plg_by_author', { author: item.author })}</span>` : ''}
              </div>
              <div class="muted mini" style="margin-top: 4px;">${escapeHtml(item.description || t('plg_default_desc_short'))}</div>
            </div>

            <div>
              ${isUpToDate
                ? `<span class="badge" style="color: var(--success); border: 1px solid var(--success); background: transparent;">${t('plg_installed_check')}</span>`
                : (isUpdateAvailable
                  ? `<button type="button" class="btn btn-xs btn-primary btn-install-plugin" data-url="${escapeHtml(item.download_url || '')}" data-sha="${escapeHtml(item.sha256 || '')}" data-id="${escapeHtml(item.id)}">${tfHtml('plg_update_to', { version: item.version })}</button>`
                  : `<button type="button" class="btn btn-xs btn-primary btn-install-plugin" data-url="${escapeHtml(item.download_url || '')}" data-sha="${escapeHtml(item.sha256 || '')}" data-id="${escapeHtml(item.id)}">${t('plg_install')}</button>`
                )
              }
            </div>
          </div>

          ${item.device_definition ? `
            <div style="margin-top: 8px; font-size: 12px; color: var(--text-muted);">
              ${t('plg_supports')} <strong>${escapeHtml(item.device_definition.display_name || item.device_definition.device_type)}</strong>
            </div>
          ` : ''}
        </div>
      `;
    }).join('');

    container.querySelectorAll('.btn-install-plugin').forEach(btn => {
      btn.addEventListener('click', async () => {
        const url = btn.getAttribute('data-url');
        const id = btn.getAttribute('data-id');
        const sha = btn.getAttribute('data-sha') || '';
        btn.disabled = true;
        btn.textContent = t('plg_installing_btn');
        await this.installPluginWithFallback(url, id, sha);
        btn.disabled = false;
        btn.textContent = t('plg_install');
      });
    });
  }

  async installPluginWithFallback(downloadUrl, pluginId, sha256 = '') {
    try {
      showToast(tf('plg_downloading_installing', { id: pluginId }), 'info');
      // Step 1: Direct Hub install via URL
      const res = await this.client.installPlugin(downloadUrl);
      if (res && res.error) throw new Error(res.error);
      showToast(tf('plg_installed_ok', { id: pluginId }), 'success');
      await this.refreshPluginsList();
      this.switchPluginTab('installed');
    } catch (err) {
      // Only when the Hub could not download (502), and only with the published checksum: a
      // refusal (checksum, authentication, flash space) or a timeout (the Hub may still be
      // installing) is never retried from the browser
      if (err.status !== 502 || !sha256) {
        showToast(tf('plg_installation_failed', { error: err.message }), 'error');
        return;
      }
      console.warn('Hub direct download failed, attempting client-side browser proxy fallback:', err);
      try {
        showToast(t('plg_browser_fallback'), 'info');
        // Step 2: Fetch via browser client and upload as base64 archive; the Hub checks the SHA-256
        const response = await fetch(downloadUrl);
        if (!response.ok) throw new Error(`Browser download HTTP ${response.status}`);
        const blob = await response.blob();
        const base64 = await this.blobToBase64(blob);
        const res = await this.client.installPlugin(base64, false, sha256);
        if (res && res.error) throw new Error(res.error);
        showToast(tf('plg_installed_ok_fallback', { id: pluginId }), 'success');
        await this.refreshPluginsList();
        this.switchPluginTab('installed');
      } catch (fallbackErr) {
        showToast(tf('plg_installation_failed', { error: fallbackErr.message }), 'error');
      }
    }
  }

  async handlePluginArchiveUpload(file) {
    if (!file.name.endsWith('.tar.gz') && !file.name.endsWith('.tgz')) {
      showToast(t('plg_upload_wrong_type'), 'error');
      return;
    }
    // the Hub refuses an archive over 1 MB (and a request over about 1.5 MB before reading it)
    if (file.size > 1024 * 1024) {
      showToast(t('plg_upload_too_large'), 'error');
      return;
    }
    showToast(tf('plg_uploading_installing', { name: file.name }), 'info');
    try {
      const base64 = await this.fileToBase64(file);
      // a local archive is a dev install: there is no published checksum for it
      const res = await this.client.installPlugin(base64, true);
      if (res && res.error) throw new Error(res.error);
      showToast(t('plg_archive_installed'), 'success');
      await this.refreshPluginsList();
      this.switchPluginTab('installed');
    } catch (err) {
      showToast(tf('plg_archive_failed', { error: err.message }), 'error');
    }
  }

  fileToBase64(file) {
    return new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.onload = () => {
        const result = reader.result;
        const b64 = result.includes(',') ? result.split(',')[1] : result;
        resolve(b64);
      };
      reader.onerror = reject;
      reader.readAsDataURL(file);
    });
  }

  blobToBase64(blob) {
    return new Promise((resolve, reject) => {
      const reader = new FileReader();
      reader.onload = () => {
        const result = reader.result;
        const b64 = result.includes(',') ? result.split(',')[1] : result;
        resolve(b64);
      };
      reader.onerror = reject;
      reader.readAsDataURL(blob);
    });
  }

  async refreshRepositories() {
    const container = document.getElementById('pluginReposList');
    if (!container) return;

    try {
      const res = await this.client.getPluginRepositories();
      this.pluginRepositories = Array.isArray(res) ? res : (res?.repositories || []);
      this.renderRepositories();
    } catch (err) {
      container.innerHTML = `<div class="card error" style="padding: 12px; color: var(--danger);">${tfHtml('plg_repos_load_failed', { error: err.message })}</div>`;
    }
  }

  renderRepositories() {
    const container = document.getElementById('pluginReposList');
    if (!container) return;

    if (!this.pluginRepositories || this.pluginRepositories.length === 0) {
      container.innerHTML = `<div class="card" style="padding: 16px; color: var(--text-muted); text-align: center;">${t('plg_no_repos')}</div>`;
      return;
    }

    container.innerHTML = this.pluginRepositories.map(url => {
      return `
        <div class="card" style="margin-bottom: 8px; padding: 10px 14px; display: flex; justify-content: space-between; align-items: center; gap: 10px;">
          <span style="font-size: 13px; font-family: monospace; word-break: break-all;">${escapeHtml(url)}</span>
          <button type="button" class="btn btn-xs btn-ghost btn-remove-repo" data-url="${escapeHtml(url)}" style="color: var(--danger);">✕</button>
        </div>
      `;
    }).join('');

    container.querySelectorAll('.btn-remove-repo').forEach(btn => {
      btn.addEventListener('click', async () => {
        const url = btn.getAttribute('data-url');
        try {
          await this.client.updatePluginRepositories('remove', url);
          this._marketplaceStale = true;
          showToast(t('plg_repo_removed'), 'success');
          await this.refreshRepositories();
        } catch (err) {
          showToast(tf('plg_repo_remove_failed', { error: err.message }), 'error');
        }
      });
    });
  }

  // --- Device Studio Integration ---
  // The plugin list is fetched once when nothing loaded it yet. Fetching again whenever the list
  // was empty looped for ever on a Hub with no plugin (about 200 requests per second).
  fetchInstalledPluginsOnce(then) {
    if (this._pluginsFetched || (this.installedPlugins && this.installedPlugins.length > 0)) return;
    if (!this.client || typeof this.client.getPlugins !== 'function') return;
    this._pluginsFetched = true;
    this.client.getPlugins().then(res => {
      this.installedPlugins = Array.isArray(res) ? res : (res?.plugins || []);
      if (this.installedPlugins.length > 0) then();
    }).catch(() => {});
  }

  updateDeviceStudioPluginTypes() {
    this.fetchInstalledPluginsOnce(() => this.updateDeviceStudioPluginTypes());

    ['newDeviceType', 'editDevType'].forEach(selectId => {
      const select = document.getElementById(selectId);
      if (!select) return;

      if (!select._deviceTypeListenerAttached) {
        select._deviceTypeListenerAttached = true;
        select.addEventListener('change', (e) => {
          if (selectId === 'newDeviceType') {
            this.handleDeviceTypeChanged(e.target.value);
          } else {
            this.handleEditDeviceTypeChanged(e.target.value);
          }
        });
      }

      let optgroup = document.getElementById(`${selectId}PluginGroup`);
      if (!optgroup) {
        optgroup = document.createElement('optgroup');
        optgroup.id = `${selectId}PluginGroup`;
        optgroup.label = t('plg_community_plugins');
        select.appendChild(optgroup);
      }
      optgroup.innerHTML = '';

      const pluginsWithDev = (this.installedPlugins || []).filter(p => p.device_definition);
      if (pluginsWithDev.length === 0) {
        optgroup.style.display = 'none';
        return;
      }

      optgroup.style.display = '';
      pluginsWithDev.forEach(p => {
        const opt = document.createElement('option');
        opt.value = `plugin:${p.id}`;
        opt.textContent = tf('plg_type_option', { name: p.device_definition.display_name || p.name });
        optgroup.appendChild(opt);
      });
    });
  }

  openAddDeviceForPlugin(pluginId) {
    this.switchTab('ir');
    const box = document.getElementById('studioNewDeviceBox');
    if (box) box.style.display = 'block';
    this.updateDeviceStudioPluginTypes();
    const select = document.getElementById('newDeviceType');
    if (select) {
      select.value = `plugin:${pluginId}`;
      this.handleDeviceTypeChanged(`plugin:${pluginId}`);
    }
    const nameInput = document.getElementById('newDeviceName');
    const mfgInput = document.getElementById('newDeviceManufacturer');
    const modelInput = document.getElementById('newDeviceModel');
    const plug = (this.installedPlugins || []).find(p => p.id === pluginId);
    if (plug) {
      if (nameInput) nameInput.value = plug.device_definition?.display_name || plug.name;
      if (mfgInput && !mfgInput.value) mfgInput.value = plug.author || 'Community';
      if (modelInput && !modelInput.value) modelInput.value = plug.device_definition?.device_type || plug.id;
    }
    nameInput?.focus();
  }

  handleDeviceTypeChanged(selectedType) {
    let wrap = document.getElementById('newDevicePluginConfigWrap');
    if (!wrap) {
      const row = document.createElement('div');
      row.id = 'newDevicePluginConfigWrap';
      row.style.marginTop = '10px';
      row.style.display = 'none';
      const anchor = document.getElementById('newDeviceType')?.closest('.row');
      if (anchor && anchor.parentNode) {
        anchor.parentNode.insertBefore(row, anchor.nextSibling);
      }
      wrap = row;
    }

    let plug = this.getPluginForType(selectedType);
    if (!plug || !plug.id) {
      wrap.style.display = 'none';
      wrap.innerHTML = '';
      return;
    }

    this.fetchInstalledPluginsOnce(() => this.handleDeviceTypeChanged(selectedType));

    const fields = Array.isArray(plug.device_definition?.config_fields)
      ? plug.device_definition.config_fields.filter(f => f && f.key) : null;
    if (!fields || fields.length === 0) {
      wrap.style.display = 'none';
      wrap.innerHTML = '';
      return;
    }

    const displayName = plug.device_definition?.display_name || plug.name || plug.id;
    const pairingDef = plug.manifest?.pairing || plug.pairing;
    const hasPairing = !!pairingDef?.supported;

    wrap.style.display = 'block';
    wrap.innerHTML = `
      <div class="card" style="padding: 12px; background: var(--bg-card); border: 1px solid var(--border);">
        <strong style="font-size: 13px; color: var(--accent); margin-bottom: 8px; display: block;">${tfHtml('plg_config_title_short', { name: displayName })}</strong>
        <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 10px;">
          ${fields.map(f => pluginFieldHtml(f, 'plug_cfg_', f.default)).join('')}
        </div>
        ${hasPairing ? `
          <div style="margin-top: 10px; padding: 10px; background: rgba(59, 130, 246, 0.08); border: 1px solid rgba(59, 130, 246, 0.25); border-radius: 6px; display: flex; justify-content: space-between; align-items: center; gap: 8px;">
            <div>
              <div style="font-weight: 600; font-size: 12px; color: var(--text-main);">${escapeHtml(pairingDef.title || t('plg_device_pairing'))}</div>
              <div class="muted mini" id="new_plug_pair_status">${escapeHtml(pairingDef.description || tf('plg_pairing_required', { name: displayName }))}</div>
            </div>
            <button type="button" class="btn btn-xs btn-primary btn-trigger-plug-pair" data-plugin-id="${escapeHtml(plug.id)}" data-input-id="plug_cfg_host">
              🔗 ${escapeHtml(pairingDef.button_label || t('plg_pair_device'))}
            </button>
          </div>
        ` : ''}
      </div>
    `;

    wrap.querySelectorAll('.btn-trigger-plug-pair').forEach(btn => {
      btn.addEventListener('click', () => {
        const pId = btn.getAttribute('data-plugin-id');
        const inpId = btn.getAttribute('data-input-id');
        if (pairingDef?.type === 'listen' || pairingDef?.type === 'confirm') {
          const editing = inpId.startsWith('edit_');
          const host = pairingDef.type === 'confirm' ? (document.getElementById(inpId)?.value || '').trim() : '';
          this.runListenPairing(pId, editing ? 'edit_plug_cfg_' : 'plug_cfg_',
            editing ? 'edit_plug_pair_status' : 'new_plug_pair_status', btn, pairingDef.timeout_s,
            pairingDef.type === 'confirm', host);
          return;
        }
        const hostVal = document.getElementById(inpId)?.value || '';
        this.openPluginPairingModal(pId, hostVal);
      });
    });
  }

  handleEditDeviceTypeChanged(selectedType, prefillConfig = null) {
    let wrap = document.getElementById('editDevPluginConfigWrap');
    if (!wrap) {
      const row = document.createElement('div');
      row.id = 'editDevPluginConfigWrap';
      row.style.marginTop = '8px';
      row.style.display = 'none';
      const anchor = document.getElementById('editDevType')?.closest('.row') || document.getElementById('editDevType')?.parentNode?.parentNode;
      if (anchor && anchor.parentNode) {
        anchor.parentNode.insertBefore(row, anchor.nextSibling);
      }
      wrap = row;
    }

    let plug = this.getPluginForType(selectedType);
    if (!plug && prefillConfig?.plugin) {
      plug = (this.installedPlugins || []).find(p => p.id === prefillConfig.plugin) || { id: prefillConfig.plugin };
    }

    if (!plug || !plug.id) {
      wrap.style.display = 'none';
      wrap.innerHTML = '';
      return;
    }

    this.fetchInstalledPluginsOnce(() => {
      this.updateDeviceStudioPluginTypes();
      this.handleEditDeviceTypeChanged(selectedType, prefillConfig);
    });

    let cfgObj = prefillConfig;
    if (typeof cfgObj === 'string') {
      try { cfgObj = JSON.parse(cfgObj); } catch {}
    } else if (cfgObj && typeof cfgObj.config === 'string') {
      try { cfgObj = JSON.parse(cfgObj.config); } catch { cfgObj = cfgObj.config; }
    } else if (cfgObj && typeof cfgObj.config === 'object') {
      cfgObj = cfgObj.config;
    }

    let fields = Array.isArray(plug.device_definition?.config_fields)
      ? plug.device_definition.config_fields.filter(f => f && f.key) : null;
    if (!fields && cfgObj && typeof cfgObj === 'object') {
      const knownKeys = Object.keys(cfgObj).filter(k => !['id', 'name', 'type', 'config', 'commands', 'icon', 'manufacturer', 'model', 'interDeviceDelay', 'interKeyDelay', 'holdInterKeyDelay', 'powerOnDelay', 'pressMinRepeats', 'isPowerAlwaysOn'].includes(k));
      if (knownKeys.length > 0) {
        fields = knownKeys.map(k => ({ key: k, label: k.replace(/_/g, ' ').toUpperCase(), type: typeof cfgObj[k] === 'number' ? 'number' : (typeof cfgObj[k] === 'boolean' ? 'boolean' : 'string') }));
      }
    }

    if (!fields || fields.length === 0) {
      wrap.style.display = 'none';
      wrap.innerHTML = '';
      return;
    }

    const displayName = plug.device_definition?.display_name || plug.name || plug.id;
    const pairingDef = plug.manifest?.pairing || plug.pairing;
    const hasPairing = !!pairingDef?.supported;

    wrap.style.display = 'block';
    wrap.innerHTML = `
      <div class="card" style="padding: 10px; background: var(--bg-card); border: 1px solid var(--border); margin-top: 6px;">
        <strong style="font-size: 12px; color: var(--accent); margin-bottom: 6px; display: block;">${tfHtml('plg_settings_title', { name: displayName })}</strong>
        <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 8px;">
          ${fields.map(f => {
            let curVal = '';
            if (cfgObj && typeof cfgObj === 'object' && cfgObj[f.key] !== undefined && cfgObj[f.key] !== null) {
              curVal = cfgObj[f.key];
            } else if (f.default !== undefined) {
              curVal = f.default;
            }
            return pluginFieldHtml(f, 'edit_plug_cfg_', curVal, true);
          }).join('')}
        </div>
        ${hasPairing ? `
          <div style="margin-top: 10px; padding: 10px; background: rgba(59, 130, 246, 0.08); border: 1px solid rgba(59, 130, 246, 0.25); border-radius: 6px; display: flex; justify-content: space-between; align-items: center; gap: 8px;">
            <div>
              <div style="font-weight: 600; font-size: 12px; color: var(--text-main);">${escapeHtml(pairingDef.title || t('plg_device_pairing'))}</div>
              <div class="muted mini" id="edit_plug_pair_status">${escapeHtml(pairingDef.description || tf('plg_pairing_required', { name: displayName }))}</div>
            </div>
            <button type="button" class="btn btn-xs btn-primary btn-trigger-plug-pair" data-plugin-id="${escapeHtml(plug.id)}" data-input-id="edit_plug_cfg_host">
              🔗 ${escapeHtml(pairingDef.button_label || t('plg_pair_device'))}
            </button>
          </div>
        ` : ''}
      </div>
    `;

    wrap.querySelectorAll('.btn-trigger-plug-pair').forEach(btn => {
      btn.addEventListener('click', () => {
        const pId = btn.getAttribute('data-plugin-id');
        const inpId = btn.getAttribute('data-input-id');
        if (pairingDef?.type === 'listen' || pairingDef?.type === 'confirm') {
          const editing = inpId.startsWith('edit_');
          const host = pairingDef.type === 'confirm' ? (document.getElementById(inpId)?.value || '').trim() : '';
          this.runListenPairing(pId, editing ? 'edit_plug_cfg_' : 'plug_cfg_',
            editing ? 'edit_plug_pair_status' : 'new_plug_pair_status', btn, pairingDef.timeout_s,
            pairingDef.type === 'confirm', host);
          return;
        }
        const hostVal = document.getElementById(inpId)?.value || '';
        this.openPluginPairingModal(pId, hostVal);
      });
    });
  }

  // A "listen" pairing: the plugin hears its device itself (no address to give). A "confirm"
  // pairing: the user accepts a prompt on the device (a TV) at the host given. The Hub answers at
  // once; the status is polled until the plugin reports the end, and the values it returns fill
  // the device fields (fieldPrefix + key). The user saves the device as usual.
  async runListenPairing(pluginId, fieldPrefix, statusId, btn, timeoutS, confirm = false, host = '') {
    const show = (txt) => {
      const el = document.getElementById(statusId);
      if (el) el.textContent = txt;
    };
    if (confirm && !host) {
      show(t('plg_pair_host_required'));
      return;
    }
    const ask = async (action) => {
      try {
        return await this.client.pairPlugin(pluginId, action, host);
      } catch (err) {
        return { ok: false, stage: 'error', message: err.message };
      }
    };
    if (btn) btn.disabled = true;
    try {
      let res = await ask('start');
      const end = Date.now() + ((Number(res?.timeout_s) || Number(timeoutS) || 30) + 2) * 1000;
      for (;;) {
        if (res?.ok === false || res?.stage === 'error' || res?.error) {
          const error = res?.message || res?.error || '';
          show(confirm ? tf('plg_pairing_failed', { error }) : tf('plg_listen_failed', { error }));
          return;
        }
        if (res?.stage === 'done') {
          Object.entries(res.config || {}).forEach(([key, val]) => {
            const inp = document.getElementById(fieldPrefix + key);
            if (inp) inp.value = String(val);
          });
          show(res.message || (confirm ? t('plg_paired_toast') : t('plg_listen_done')));
          return;
        }
        const left = Math.round((end - Date.now()) / 1000);
        if (left <= 0) {
          show(confirm ? tf('plg_pairing_failed', { error: t('plg_confirm_timeout') }) : tf('plg_listen_failed', { error: '' }));
          return;
        }
        show(res?.message ? `${res.message} (${left} s)`
          : (confirm ? tf('plg_confirm_running', { s: left }) : tf('plg_listen_running', { s: left })));
        await new Promise(r => setTimeout(r, 1000));
        res = await ask('status');
      }
    } finally {
      if (btn) btn.disabled = false;
    }
  }

  openPluginPairingModal(pluginId, initialHost = '') {
    const plug = (this.installedPlugins || []).find(p => p.id === pluginId);
    const dispName = plug?.device_definition?.display_name || plug?.name || pluginId;
    const pairingDef = plug?.manifest?.pairing || plug?.pairing || {};
    const titleText = pairingDef.title || tf('plg_pair_name', { name: dispName });
    const descText = pairingDef.instructions || t('plg_pair_instructions');
    const hostLabelText = pairingDef.host_label || t('plg_pair_host_label');
    const promptText = pairingDef.prompt || t('plg_pair_prompt');
    const pinLen = Number(pairingDef.pin_length) || 6;
    const btnLabelText = pairingDef.button_label || t('plg_pair_device');

    let modal = document.getElementById('pluginPairModal');
    if (!modal) {
      modal = document.createElement('div');
      modal.id = 'pluginPairModal';
      modal.className = 'modal-overlay';
      modal.style.cssText = 'display: none; position: fixed; inset: 0; background: rgba(0,0,0,0.65); z-index: 9999; align-items: center; justify-content: center; backdrop-filter: blur(4px);';
      modal.innerHTML = `
        <div class="card" style="width: 90%; max-width: 440px; padding: 20px; background: var(--bg-card); border: 1px solid var(--border); box-shadow: 0 10px 25px rgba(0,0,0,0.5); border-radius: 12px;">
          <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 14px;">
            <h3 style="margin: 0; font-size: 16px; font-weight: 600; display: flex; align-items: center; gap: 8px;">
              <span>🔗</span> <span id="pairModalTitle">${t('plg_device_pairing')}</span>
            </h3>
            <button type="button" class="btn btn-xs btn-ghost" id="pairModalCloseBtn" style="font-size: 16px;">✕</button>
          </div>

          <div id="pairStep1">
            <p class="muted mini" id="pairModalInstructions" style="margin-bottom: 12px;"></p>
            <div style="margin-bottom: 14px;">
              <label for="pairModalHost" id="pairModalHostLabel" style="font-size: 12px; font-weight: 600; display: block; margin-bottom: 4px;"></label>
              <input id="pairModalHost" class="form-control" type="text" placeholder="192.168.1.50" />
            </div>
            <div id="pairStep1Error" style="display: none; color: var(--danger); font-size: 12px; margin-bottom: 10px;"></div>
            <div style="display: flex; justify-content: flex-end; gap: 8px;">
              <button type="button" class="btn btn-sm btn-outline" id="pairModalCancelBtn">${t('cancel')}</button>
              <button type="button" class="btn btn-sm btn-primary" id="pairModalStartBtn">${t('plg_start_pairing')}</button>
            </div>
          </div>

          <div id="pairStep2" style="display: none;">
            <div style="text-align: center; margin-bottom: 16px;">
              <div style="font-size: 28px; margin-bottom: 6px;">🔐</div>
              <div style="font-weight: 600; font-size: 14px; margin-bottom: 4px;">${t('plg_verification_code')}</div>
              <div class="muted mini" id="pairModalPrompt"></div>
            </div>
            <div style="margin-bottom: 18px; text-align: center;">
              <input id="pairModalPin" class="form-control" type="text"
                     style="text-align: center; font-size: 24px; font-family: monospace; letter-spacing: 6px; text-transform: uppercase; max-width: 220px; margin: 0 auto;"
                     autocomplete="off" />
            </div>
            <div id="pairStep2Error" style="display: none; color: var(--danger); font-size: 12px; text-align: center; margin-bottom: 10px;"></div>
            <div style="display: flex; justify-content: flex-end; gap: 8px;">
              <button type="button" class="btn btn-sm btn-outline" id="pairModalBackBtn">${t('plg_back')}</button>
              <button type="button" class="btn btn-sm btn-primary" id="pairModalConfirmBtn">${t('plg_verify_code')}</button>
            </div>
          </div>

          <div id="pairStep3" style="display: none; text-align: center; padding: 10px 0;">
            <div style="font-size: 32px; color: var(--success); margin-bottom: 8px;">✓</div>
            <div style="font-weight: 600; font-size: 15px; margin-bottom: 6px;">${t('plg_paired_title')}</div>
            <div class="muted mini" id="pairSuccessMsg" style="margin-bottom: 16px;">${t('plg_paired_desc')}</div>
            <button type="button" class="btn btn-sm btn-primary" id="pairModalDoneBtn" style="min-width: 100px;">${t('plg_done')}</button>
          </div>
        </div>
      `;
      document.body.appendChild(modal);

      const closeModal = () => { modal.style.display = 'none'; };
      modal.querySelector('#pairModalCloseBtn').addEventListener('click', closeModal);
      modal.querySelector('#pairModalCancelBtn').addEventListener('click', closeModal);
      modal.querySelector('#pairModalDoneBtn').addEventListener('click', closeModal);
      modal.querySelector('#pairModalBackBtn').addEventListener('click', () => {
        modal.querySelector('#pairStep2').style.display = 'none';
        modal.querySelector('#pairStep1').style.display = 'block';
      });

      modal.querySelector('#pairModalStartBtn').addEventListener('click', async () => {
        const host = modal.querySelector('#pairModalHost').value.trim();
        const errEl = modal.querySelector('#pairStep1Error');
        errEl.style.display = 'none';
        if (!host) {
          errEl.textContent = t('plg_pair_host_required');
          errEl.style.display = 'block';
          modal.querySelector('#pairModalHost').focus();
          return;
        }

        const startBtn = modal.querySelector('#pairModalStartBtn');
        startBtn.disabled = true;
        startBtn.textContent = t('plg_connecting');
        try {
          const res = await this.client.pairPlugin(modal._currentPluginId, 'start', host);
          if (res && res.error) throw new Error(res.error);
          modal.querySelector('#pairStep1').style.display = 'none';
          modal.querySelector('#pairStep2').style.display = 'block';
          modal.querySelector('#pairModalPin').value = '';
          modal.querySelector('#pairStep2Error').style.display = 'none';
          modal.querySelector('#pairModalPin').focus();
        } catch (err) {
          errEl.textContent = tf('plg_pairing_failed', { error: err.message });
          errEl.style.display = 'block';
        } finally {
          startBtn.disabled = false;
          startBtn.textContent = t('plg_start_pairing');
        }
      });

      modal.querySelector('#pairModalConfirmBtn').addEventListener('click', async () => {
        const host = modal.querySelector('#pairModalHost').value.trim();
        const pin = modal.querySelector('#pairModalPin').value.trim().toUpperCase();
        const errEl = modal.querySelector('#pairStep2Error');
        errEl.style.display = 'none';

        const expLen = modal._expectedPinLength || 6;
        if (pin.length !== expLen) {
          errEl.textContent = tf('plg_pin_length', { n: expLen });
          errEl.style.display = 'block';
          modal.querySelector('#pairModalPin').focus();
          return;
        }

        const confirmBtn = modal.querySelector('#pairModalConfirmBtn');
        confirmBtn.disabled = true;
        confirmBtn.textContent = t('plg_verifying');
        try {
          const res = await this.client.pairPlugin(modal._currentPluginId, 'pin', host, pin);
          if (res && res.error) throw new Error(res.error);
          modal.querySelector('#pairStep2').style.display = 'none';
          modal.querySelector('#pairStep3').style.display = 'block';
          showToast(t('plg_paired_toast'), 'success');

          ['edit_plug_pair_status', 'new_plug_pair_status'].forEach(id => {
            const el = document.getElementById(id);
            if (el) el.innerHTML = `<span style="color: var(--success); font-weight: 600;">${tfHtml('plg_paired_with', { host })}</span>`;
          });
        } catch (err) {
          errEl.textContent = tf('plg_verify_failed', { error: err.message });
          errEl.style.display = 'block';
        } finally {
          confirmBtn.disabled = false;
          confirmBtn.textContent = t('plg_verify_code');
        }
      });

      modal.querySelector('#pairModalPin').addEventListener('keydown', (e) => {
        if (e.key === 'Enter') {
          modal.querySelector('#pairModalConfirmBtn').click();
        }
      });
      modal.querySelector('#pairModalHost').addEventListener('keydown', (e) => {
        if (e.key === 'Enter') {
          modal.querySelector('#pairModalStartBtn').click();
        }
      });
    }

    modal._currentPluginId = pluginId;
    modal._expectedPinLength = pinLen;
    modal.querySelector('#pairModalTitle').textContent = titleText;
    modal.querySelector('#pairModalInstructions').textContent = descText;
    modal.querySelector('#pairModalHostLabel').textContent = hostLabelText;
    modal.querySelector('#pairModalPrompt').textContent = promptText;
    modal.querySelector('#pairModalStartBtn').textContent = btnLabelText;
    const pinInput = modal.querySelector('#pairModalPin');
    pinInput.maxLength = pinLen;
    pinInput.placeholder = 'X'.repeat(pinLen);

    let effHost = initialHost;
    if (!effHost) {
      effHost = document.getElementById('edit_plug_cfg_host')?.value ||
                document.getElementById('plug_cfg_host')?.value || '';
    }
    modal.querySelector('#pairModalHost').value = effHost;
    modal.querySelector('#pairStep1').style.display = 'block';
    modal.querySelector('#pairStep2').style.display = 'none';
    modal.querySelector('#pairStep3').style.display = 'none';
    modal.querySelector('#pairStep1Error').style.display = 'none';
    modal.style.display = 'flex';
    if (effHost) {
      modal.querySelector('#pairModalStartBtn').focus();
    } else {
      modal.querySelector('#pairModalHost').focus();
    }
  }
}
