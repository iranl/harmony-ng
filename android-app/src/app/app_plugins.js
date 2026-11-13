import { HarmonyAppHubsNetwork } from './app_hubs_network.js';
import { escapeHtml, showToast, showConfirm } from './app_icons_ui.js';
import { t } from '../i18n.js';

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
        showToast('Please enter a valid HTTP/HTTPS repository JSON URL', 'error');
        return;
      }
      try {
        await this.client.updatePluginRepositories('add', url);
        showToast('Repository added successfully', 'success');
        if (input) input.value = '';
        await this.refreshRepositories();
      } catch (err) {
        showToast(`Failed to add repository: ${err.message}`, 'error');
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
    else if (tab === 'marketplace') this.refreshMarketplace();
    else if (tab === 'repos') this.refreshRepositories();
  }

  async refreshPluginsList() {
    const container = document.getElementById('installedPluginsList');
    const statusEl = document.getElementById('pluginsStatusBadge');
    if (!container) return;

    try {
      if (statusEl) statusEl.textContent = 'Loading...';
      const res = await this.client.getPlugins();
      this.installedPlugins = Array.isArray(res) ? res : (res?.plugins || []);

      if (statusEl) {
        const runningCount = this.installedPlugins.filter(p => p.running).length;
        statusEl.textContent = `${this.installedPlugins.length} installed • ${runningCount} active`;
      }

      this.renderInstalledPlugins();
      this.updateDeviceStudioPluginTypes();
    } catch (err) {
      if (statusEl) statusEl.textContent = 'Error';
      container.innerHTML = `<div class="card error" style="padding: 12px; color: var(--danger);">Failed to load plugins: ${escapeHtml(err.message)}</div>`;
    }
  }

  renderInstalledPlugins() {
    const container = document.getElementById('installedPluginsList');
    if (!container) return;

    if (!this.installedPlugins || this.installedPlugins.length === 0) {
      container.innerHTML = `
        <div class="card" style="text-align: center; padding: 24px; color: var(--text-muted);">
          <div style="font-size: 24px; margin-bottom: 8px;">🔌</div>
          <div style="font-weight: 600; margin-bottom: 4px;">No Plugins Installed</div>
          <div class="mini">Explore community drivers in the Marketplace or upload a custom .tar.gz package.</div>
          <button type="button" class="btn btn-sm btn-primary" style="margin-top: 14px;" onclick="window.harmonyApp && window.harmonyApp.switchPluginTab('marketplace')">Browse Marketplace</button>
        </div>
      `;
      return;
    }

    container.innerHTML = this.installedPlugins.map(p => {
      const isRunning = !!p.running;
      const isEnabled = p.enabled !== false;
      const badgeColor = isRunning ? 'var(--success)' : (isEnabled ? 'var(--warning)' : 'var(--text-muted)');
      const badgeText = isRunning ? `Running (PID ${p.pid || '?'})` : (isEnabled ? 'Stopped' : 'Disabled');

      return `
        <div class="card" style="margin-bottom: 12px; padding: 14px;">
          <div style="display: flex; justify-content: space-between; align-items: flex-start; gap: 10px; margin-bottom: 8px;">
            <div>
              <div style="display: flex; align-items: center; gap: 8px;">
                <strong style="font-size: 15px; color: var(--text-main);">${escapeHtml(p.name || p.id)}</strong>
                <span class="badge" style="font-size: 11px; border: 1px solid ${badgeColor}; color: ${badgeColor}; background: transparent;">${escapeHtml(badgeText)}</span>
                <span class="muted mini">v${escapeHtml(p.version || '1.0.0')}</span>
              </div>
              <div class="muted mini" style="margin-top: 4px;">${escapeHtml(p.description || 'Community driver plugin')}</div>
            </div>
            <label style="display: flex; align-items: center; gap: 6px; cursor: pointer; font-size: 13px;">
              <input type="checkbox" class="plugin-enable-toggle" data-plugin-id="${escapeHtml(p.id)}" ${isEnabled ? 'checked' : ''}>
              <span>Enabled</span>
            </label>
          </div>

          ${p.device_definition ? `
            <div style="font-size: 12px; background: var(--bg-card); border: 1px solid var(--border); border-radius: 6px; padding: 6px 10px; margin: 8px 0;">
              <span class="muted">Device Support:</span> <strong>${escapeHtml(p.device_definition.display_name || p.device_definition.device_type)}</strong>
              ${p.device_definition.config_fields ? `<span class="muted mini" style="margin-left: 6px;">(${p.device_definition.config_fields.map(f => f.label || f.key).join(', ')})</span>` : ''}
            </div>
          ` : ''}

          <div style="display: flex; justify-content: space-between; align-items: center; margin-top: 10px; border-top: 1px solid var(--border); padding-top: 8px;">
            <div style="display: flex; gap: 6px;">
              <button type="button" class="btn btn-xs btn-outline btn-plugin-restart" data-plugin-id="${escapeHtml(p.id)}" ${!isEnabled ? 'disabled' : ''}>Restart</button>
              ${isRunning
                ? `<button type="button" class="btn btn-xs btn-outline btn-plugin-stop" data-plugin-id="${escapeHtml(p.id)}">Stop</button>`
                : `<button type="button" class="btn btn-xs btn-outline btn-plugin-start" data-plugin-id="${escapeHtml(p.id)}" ${!isEnabled ? 'disabled' : ''}>Start</button>`
              }
              ${p.device_definition ? `
                <button type="button" class="btn btn-xs btn-primary btn-plugin-add-dev" data-plugin-id="${escapeHtml(p.id)}" ${!isEnabled ? 'disabled' : ''}>+ Add Device</button>
              ` : ''}
            </div>
            <button type="button" class="btn btn-xs btn-ghost btn-plugin-uninstall" data-plugin-id="${escapeHtml(p.id)}" style="color: var(--danger);">Uninstall</button>
          </div>
        </div>
      `;
    }).join('');

    // Bind action buttons
    container.querySelectorAll('.plugin-enable-toggle').forEach(input => {
      input.addEventListener('change', async (e) => {
        const id = input.getAttribute('data-plugin-id');
        const enable = input.checked;
        try {
          await this.client.pluginAction(id, enable ? 'enable' : 'disable');
          showToast(`Plugin ${id} ${enable ? 'enabled' : 'disabled'}`, 'success');
          await this.refreshPluginsList();
        } catch (err) {
          showToast(`Action failed: ${err.message}`, 'error');
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
          showToast(`Plugin ${id} restarted`, 'success');
          await this.refreshPluginsList();
        } catch (err) {
          showToast(`Restart failed: ${err.message}`, 'error');
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
          showToast(`Plugin ${id} started`, 'success');
          await this.refreshPluginsList();
        } catch (err) {
          showToast(`Start failed: ${err.message}`, 'error');
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
          showToast(`Plugin ${id} stopped`, 'success');
          await this.refreshPluginsList();
        } catch (err) {
          showToast(`Stop failed: ${err.message}`, 'error');
        } finally {
          btn.disabled = false;
        }
      });
    });

    container.querySelectorAll('.btn-plugin-uninstall').forEach(btn => {
      btn.addEventListener('click', async () => {
        const id = btn.getAttribute('data-plugin-id');
        const ok = await showConfirm(`Are you sure you want to uninstall plugin '${id}'? This will stop its daemon and remove all files.`);
        if (!ok) return;
        btn.disabled = true;
        try {
          await this.client.uninstallPlugin(id);
          showToast(`Plugin ${id} uninstalled`, 'success');
          await this.refreshPluginsList();
        } catch (err) {
          showToast(`Uninstall failed: ${err.message}`, 'error');
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
  }

  async refreshMarketplace(forceReload = false) {
    const container = document.getElementById('marketplaceCatalogList');
    if (!container) return;

    try {
      container.innerHTML = '<div class="muted mini" style="padding: 16px; text-align: center;">Fetching marketplace catalog from repositories...</div>';
      const res = await this.client.getPluginMarketplace();
      this.marketplaceCatalog = Array.isArray(res) ? res : (res?.marketplace || res?.plugins || []);

      if (!this.marketplaceCatalog || this.marketplaceCatalog.length === 0) {
        try {
          const directRes = await fetch('https://raw.githubusercontent.com/iranl/harmony-ng-plugins/main/repository.json');
          if (directRes.ok) {
            const data = await directRes.json();
            if (data && Array.isArray(data.plugins)) {
              this.marketplaceCatalog = data.plugins;
            }
          }
        } catch (_) {
          /* ignore fallback error */
        }
      }

      this.renderMarketplaceCatalog();
    } catch (err) {
      container.innerHTML = `<div class="card error" style="padding: 12px; color: var(--danger);">Failed to load marketplace catalog: ${escapeHtml(err.message)}</div>`;
    }
  }

  renderMarketplaceCatalog() {
    const container = document.getElementById('marketplaceCatalogList');
    if (!container) return;

    if (!this.marketplaceCatalog || this.marketplaceCatalog.length === 0) {
      container.innerHTML = `
        <div class="card" style="text-align: center; padding: 24px; color: var(--text-muted);">
          <div>No plugins found in configured repositories.</div>
          <div class="mini" style="margin-top: 4px;">Check your repository URLs in the Repositories tab.</div>
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
                ${item.author ? `<span class="muted mini">by ${escapeHtml(item.author)}</span>` : ''}
              </div>
              <div class="muted mini" style="margin-top: 4px;">${escapeHtml(item.description || 'Community driver')}</div>
            </div>

            <div>
              ${isUpToDate
                ? `<span class="badge" style="color: var(--success); border: 1px solid var(--success); background: transparent;">Installed ✓</span>`
                : (isUpdateAvailable
                  ? `<button type="button" class="btn btn-xs btn-primary btn-install-plugin" data-url="${escapeHtml(item.download_url)}" data-id="${escapeHtml(item.id)}">Update to v${escapeHtml(item.version)}</button>`
                  : `<button type="button" class="btn btn-xs btn-primary btn-install-plugin" data-url="${escapeHtml(item.download_url)}" data-id="${escapeHtml(item.id)}">Install</button>`
                )
              }
            </div>
          </div>

          ${item.device_definition ? `
            <div style="margin-top: 8px; font-size: 12px; color: var(--text-muted);">
              Supports: <strong>${escapeHtml(item.device_definition.display_name || item.device_definition.device_type)}</strong>
            </div>
          ` : ''}
        </div>
      `;
    }).join('');

    container.querySelectorAll('.btn-install-plugin').forEach(btn => {
      btn.addEventListener('click', async () => {
        const url = btn.getAttribute('data-url');
        const id = btn.getAttribute('data-id');
        btn.disabled = true;
        btn.textContent = 'Installing...';
        await this.installPluginWithFallback(url, id);
        btn.disabled = false;
        btn.textContent = 'Install';
      });
    });
  }

  async installPluginWithFallback(downloadUrl, pluginId) {
    try {
      showToast(`Downloading and installing plugin '${pluginId}'...`, 'info');
      // Step 1: Direct Hub install via URL
      const res = await this.client.installPlugin(downloadUrl);
      if (res && res.error) throw new Error(res.error);
      showToast(`Plugin '${pluginId}' installed successfully!`, 'success');
      await this.refreshPluginsList();
      this.switchPluginTab('installed');
    } catch (err) {
      console.warn('Hub direct download failed, attempting client-side browser proxy fallback:', err);
      try {
        showToast('Direct download on hub failed, using browser download fallback...', 'info');
        // Step 2: Fetch via browser client and upload as base64 archive
        const response = await fetch(downloadUrl);
        if (!response.ok) throw new Error(`Browser download HTTP ${response.status}`);
        const blob = await response.blob();
        const base64 = await this.blobToBase64(blob);
        const res = await this.client.installPlugin(base64);
        if (res && res.error) throw new Error(res.error);
        showToast(`Plugin '${pluginId}' installed successfully via fallback!`, 'success');
        await this.refreshPluginsList();
        this.switchPluginTab('installed');
      } catch (fallbackErr) {
        showToast(`Installation failed: ${fallbackErr.message}`, 'error');
      }
    }
  }

  async handlePluginArchiveUpload(file) {
    if (!file.name.endsWith('.tar.gz') && !file.name.endsWith('.tgz')) {
      showToast('Please upload a .tar.gz or .tgz plugin archive', 'error');
      return;
    }
    showToast(`Uploading and installing '${file.name}'...`, 'info');
    try {
      const base64 = await this.fileToBase64(file);
      const res = await this.client.installPlugin(base64);
      if (res && res.error) throw new Error(res.error);
      showToast(`Plugin archive installed successfully!`, 'success');
      await this.refreshPluginsList();
      this.switchPluginTab('installed');
    } catch (err) {
      showToast(`Archive upload failed: ${err.message}`, 'error');
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
      container.innerHTML = `<div class="card error" style="padding: 12px; color: var(--danger);">Failed to load repositories: ${escapeHtml(err.message)}</div>`;
    }
  }

  renderRepositories() {
    const container = document.getElementById('pluginReposList');
    if (!container) return;

    if (!this.pluginRepositories || this.pluginRepositories.length === 0) {
      container.innerHTML = `<div class="card" style="padding: 16px; color: var(--text-muted); text-align: center;">No repositories configured.</div>`;
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
          showToast('Repository removed', 'success');
          await this.refreshRepositories();
        } catch (err) {
          showToast(`Failed to remove repository: ${err.message}`, 'error');
        }
      });
    });
  }

  // --- Device Studio Integration ---
  updateDeviceStudioPluginTypes() {
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
        optgroup.label = 'Community Plugins';
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
        opt.textContent = `${p.device_definition.display_name || p.name} (Plugin)`;
        optgroup.appendChild(opt);
      });
    });
  }

  openAddDeviceForPlugin(pluginId) {
    this.switchTab('ir-studio');
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

    if (!selectedType.startsWith('plugin:')) {
      wrap.style.display = 'none';
      wrap.innerHTML = '';
      return;
    }

    const pluginId = selectedType.substring(7);
    const plug = (this.installedPlugins || []).find(p => p.id === pluginId);
    if (!plug || !plug.device_definition || !plug.device_definition.config_fields) {
      wrap.style.display = 'none';
      wrap.innerHTML = '';
      return;
    }

    const fields = plug.device_definition.config_fields;
    wrap.style.display = 'block';
    wrap.innerHTML = `
      <div class="card" style="padding: 12px; background: var(--bg-card); border: 1px solid var(--border);">
        <strong style="font-size: 13px; color: var(--accent); margin-bottom: 8px; display: block;">Plugin Configuration: ${escapeHtml(plug.device_definition.display_name || plug.name)}</strong>
        <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 10px;">
          ${fields.map(f => `
            <div>
              <label for="plug_cfg_${escapeHtml(f.key)}" style="font-size: 12px; margin-bottom: 2px;">${escapeHtml(f.label || f.key)}</label>
              <input id="plug_cfg_${escapeHtml(f.key)}" class="form-control" type="${f.type === 'number' ? 'number' : (f.type === 'password' ? 'password' : 'text')}"
                     value="${f.default !== undefined ? escapeHtml(String(f.default)) : ''}" placeholder="${escapeHtml(f.placeholder || '')}">
            </div>
          `).join('')}
        </div>
      </div>
    `;
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

    if (!selectedType.startsWith('plugin:')) {
      wrap.style.display = 'none';
      wrap.innerHTML = '';
      return;
    }

    const pluginId = selectedType.substring(7);
    if ((!this.installedPlugins || this.installedPlugins.length === 0) && this.client && typeof this.client.getPlugins === 'function') {
      this.client.getPlugins().then(res => {
        this.installedPlugins = Array.isArray(res) ? res : (res?.plugins || []);
        this.updateDeviceStudioPluginTypes();
        this.handleEditDeviceTypeChanged(selectedType, prefillConfig);
      }).catch(() => {});
    }

    const plug = (this.installedPlugins || []).find(p => p.id === pluginId);
    let fields = plug?.device_definition?.config_fields;

    if (!fields && prefillConfig) {
      const cfgObj = prefillConfig.config || prefillConfig;
      const knownKeys = Object.keys(cfgObj).filter(k => !['id', 'name', 'type', 'config', 'commands', 'icon', 'manufacturer', 'model', 'interDeviceDelay', 'interKeyDelay', 'holdInterKeyDelay', 'powerOnDelay', 'isPowerAlwaysOn'].includes(k));
      if (knownKeys.length > 0) {
        fields = knownKeys.map(k => ({ key: k, label: k.replace(/_/g, ' ').toUpperCase(), type: typeof cfgObj[k] === 'number' ? 'number' : 'string' }));
      }
    }
    if (!fields && pluginId === 'roku-ecp') {
      fields = [
        { key: 'ip_address', label: 'IP Address', type: 'string', placeholder: '192.168.1.50' },
        { key: 'port', label: 'Port', type: 'number', default: 8060 }
      ];
    } else if (!fields && pluginId === 'android-tv') {
      fields = [
        { key: 'host', label: 'IP Address', type: 'string', placeholder: '192.168.1.60' },
        { key: 'port', label: 'Port', type: 'number', default: 6467 }
      ];
    } else if (!fields && pluginId === 'ps5-wake') {
      fields = [
        { key: 'host_mac', label: 'PS5 Bluetooth MAC Address', type: 'string', placeholder: 'AA:BB:CC:DD:EE:FF' }
      ];
    }

    if (!fields || fields.length === 0) {
      wrap.style.display = 'none';
      wrap.innerHTML = '';
      return;
    }

    const displayName = plug?.device_definition?.display_name || plug?.name || pluginId;
    wrap.style.display = 'block';
    wrap.innerHTML = `
      <div class="card" style="padding: 10px; background: var(--bg-card); border: 1px solid var(--border); margin-top: 6px;">
        <strong style="font-size: 12px; color: var(--accent); margin-bottom: 6px; display: block;">Plugin Settings: ${escapeHtml(displayName)}</strong>
        <div style="display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 8px;">
          ${fields.map(f => {
            let curVal = '';
            if (prefillConfig && prefillConfig[f.key] !== undefined) {
              curVal = prefillConfig[f.key];
            } else if (prefillConfig && prefillConfig.config && prefillConfig.config[f.key] !== undefined) {
              curVal = prefillConfig.config[f.key];
            } else if (f.default !== undefined) {
              curVal = f.default;
            }
            return `
            <div>
              <label for="edit_plug_cfg_${escapeHtml(f.key)}" style="font-size: 11px; margin-bottom: 2px;">${escapeHtml(f.label || f.key)}</label>
              <input id="edit_plug_cfg_${escapeHtml(f.key)}" class="form-control form-control-sm"
                     type="${f.type === 'number' ? 'number' : (f.type === 'password' ? 'password' : 'text')}"
                     value="${escapeHtml(String(curVal))}" placeholder="${escapeHtml(f.placeholder || '')}">
            </div>
            `;
          }).join('')}
        </div>
      </div>
    `;
  }
}
