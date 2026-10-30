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
import { HarmonyAppDevicesIrdb } from './app_devices_irdb.js';

export class HarmonyAppLab extends HarmonyAppDevicesIrdb {
  initLabView() {
    const sel = document.getElementById('labDevice');
    if (sel) {
      sel.innerHTML = '<option value="__auto_lab__">Temporary test device</option>' +
        (this.inventory.devices || []).map(d => `<option value="${d.id}">${d.name || d.id}</option>`).join('');
    }
  }

  async findLabCandidates() {
    const q = document.getElementById('labPathFilter')?.value.trim();
    const src = document.getElementById('labSource')?.value || 'all';
    const status = document.getElementById('labStatus');
    const box = document.getElementById('labCandidates');

    if (status) status.textContent = 'Searching candidate files...';
    try {
      const entries = await loadIrdSource(src === 'all' ? 'irdb' : src);
      const matches = rankIrdEntries(entries, q);
      if (status) status.textContent = `Found ${matches.length} matching files.`;
      if (box) {
        box.style.display = matches.length ? 'block' : 'none';
        box.innerHTML = matches.slice(0, 30).map(m => `
          <div class="queue-row" style="padding: 4px 8px;">
            <span>${m.path}</span>
            <span class="badge">${m.source}</span>
          </div>
        `).join('');
      }
    } catch (err) {
      if (status) status.textContent = 'Search failed: ' + err.message;
    }
  }

  async loadLabCommands() {
    const q = document.getElementById('labPathFilter')?.value.trim();
    const src = document.getElementById('labSource')?.value || 'all';
    const cmdFilter = (document.getElementById('labCommandFilter')?.value || '').toLowerCase();
    const status = document.getElementById('labStatus');

    if (status) status.textContent = 'Loading commands from files...';
    try {
      const entries = await loadIrdSource(src === 'all' ? 'irdb' : src);
      const matches = rankIrdEntries(entries, q).slice(0, 5);
      let allRows = [];
      for (const m of matches) {
        const text = await fetchIrdFile(m.path, m.source);
        const rows = parseIrText(text, m.source, m.path);
        allRows = allRows.concat(rows);
      }
      if (cmdFilter) {
        allRows = allRows.filter(r => r.name.toLowerCase().includes(cmdFilter));
      }
      this.labQueue = allRows.slice(0, 300);
      this.renderLabQueue();
      if (status) status.textContent = `Loaded ${this.labQueue.length} commands into queue.`;
    } catch (err) {
      if (status) status.textContent = 'Failed to load: ' + err.message;
    }
  }

  useStoredLabCommands() {
    const devId = document.getElementById('labDevice')?.value;
    const dev = (this.inventory.devices || []).find(d => String(d.id) === String(devId));
    if (!dev) return alert(t("select_stored_device"));
    this.labQueue = (dev.commands || []).map(c => ({
      name: c.name,
      meta: 'Stored command',
      keycode: c.keycode,
      raw: c.raw,
      stored: true
    }));
    this.renderLabQueue();
  }

  renderLabQueue() {
    const box = document.getElementById('labQueue');
    const sum = document.getElementById('labSummary');
    if (sum) sum.textContent = `${this.labQueue.length} queued`;
    if (!box) return;
    if (!this.labQueue.length) {
      box.innerHTML = '<div class="muted mini" style="padding: 8px;">No commands queued.</div>';
      return;
    }
    box.innerHTML = this.labQueue.map((c, i) => `
      <div class="queue-row" style="padding: 4px 8px;">
        <label style="display: flex; align-items: center; gap: 8px; flex: 1; cursor: pointer; margin: 0;">
          <input type="checkbox" class="lab-pick" data-index="${i}" checked>
          <span><strong>${c.name}</strong> <span class="muted mini">(${c.meta})</span></span>
        </label>
        <span class="badge ${c.raw ? 'ok' : 'primary'}">${c.raw ? 'Raw' : 'Code'}</span>
      </div>
    `).join('');
  }

  setLabSelections(check) {
    document.querySelectorAll('.lab-pick').forEach(cb => { cb.checked = check; });
  }

  dropUncheckedLab() {
    const picks = new Set();
    document.querySelectorAll('.lab-pick:checked').forEach(cb => {
      picks.add(Number(cb.getAttribute('data-index')));
    });
    this.labQueue = this.labQueue.filter((_, i) => picks.has(i));
    this.renderLabQueue();
  }

  clearLabQueue() {
    this.labQueue = [];
    this.renderLabQueue();
  }

  async importLabSelected() {
    const devId = document.getElementById('labDevice')?.value;
    if (!devId || devId === '__auto_lab__') return alert(t("select_real_stored_device"));
    const lines = [];
    document.querySelectorAll('.lab-pick:checked').forEach(cb => {
      const idx = Number(cb.getAttribute('data-index'));
      const c = this.labQueue[idx];
      if (c.raw) lines.push(`${c.name}|raw|${c.raw}`);
      else if (c.keycode) lines.push(`${c.name}|${c.keycode}`);
    });
    if (!lines.length) return alert(t('no_cmds_selected', 'No commands selected.'));
    try {
      await this.client.importIrdb({ deviceId: devId, payload: lines.join('\n') });
      alert(t("imported_n_cmds").replace("{count}", lines.length));
      await this.refreshData();
    } catch (err) {
      alert(t('import_failed', 'Import failed: ') + err.message);
    }
  }

  async runLabQueue() {
    if (this.labRunning) return;
    const devId = document.getElementById('labDevice')?.value;
    const delay = Number(document.getElementById('labSendDelay')?.value) || 80;
    const dry = document.getElementById('labDryRun')?.checked;
    const log = document.getElementById('labLog');
    const meter = document.getElementById('labMeter');

    const selected = [];
    document.querySelectorAll('.lab-pick:checked').forEach(cb => {
      const idx = Number(cb.getAttribute('data-index'));
      selected.push(this.labQueue[idx]);
    });

    if (!selected.length) return alert(t("no_cmds_to_run"));
    this.labRunning = true;
    this.labRunId = Date.now().toString();

    let done = 0;
    for (const cmd of selected) {
      if (!this.labRunning) break;
      done++;
      if (meter) meter.style.width = `${Math.round((done / selected.length) * 100)}%`;
      if (log) log.textContent = `[${done}/${selected.length}] Sending ${cmd.name}...`;
      if (!dry) {
        try {
          await this.client.sendCommand(devId, cmd.name);
        } catch (e) {
          if (log) log.textContent += ` (error: ${e.message})`;
        }
      }
      await new Promise(r => setTimeout(r, delay));
    }

    this.labRunning = false;
    if (log) log.textContent = 'Bulk test completed.';
  }

  stopLabQueue() {
    this.labRunning = false;
    if (this.labRunId) this.client.cancelBatchIr(this.labRunId);
  }

  /* -------------------------------------------------------------
   * 7. Bluetooth View
   * ------------------------------------------------------------- */
}
