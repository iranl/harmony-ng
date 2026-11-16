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
import { HarmonyAppDevicesIrdb } from './app_devices_irdb.js';

export class HarmonyAppLab extends HarmonyAppDevicesIrdb {
  initLabView() {
    const sel = document.getElementById('labDevice');
    if (sel) {
      sel.innerHTML = `<option value="__auto_lab__">${t('lab_temp_test_device')}</option>` +
        (this.inventory.devices || []).map(d => `<option value="${escapeHtml(d.id)}">${escapeHtml(d.name || d.id)}</option>`).join('');
    }
  }

  async findLabCandidates() {
    const q = document.getElementById('labPathFilter')?.value.trim();
    const src = document.getElementById('labSource')?.value || 'all';
    const status = document.getElementById('labStatus');
    const box = document.getElementById('labCandidates');

    if (status) status.textContent = t('lab_searching_files');
    try {
      const entries = await loadIrdSource(src === 'all' ? 'irdb' : src);
      const matches = rankIrdEntries(entries, q);
      if (status) status.textContent = tf('lab_found_files', { count: matches.length });
      if (box) {
        box.style.display = matches.length ? 'block' : 'none';
        box.innerHTML = matches.slice(0, 30).map(m => `
          <div class="queue-row" style="padding: 4px 8px;">
            <span>${escapeHtml(m.path)}</span>
            <span class="badge">${escapeHtml(m.source)}</span>
          </div>
        `).join('');
      }
    } catch (err) {
      if (status) status.textContent = tf('search_failed_error', { error: err.message });
    }
  }

  async loadLabCommands() {
    const q = document.getElementById('labPathFilter')?.value.trim();
    const src = document.getElementById('labSource')?.value || 'all';
    const cmdFilter = (document.getElementById('labCommandFilter')?.value || '').toLowerCase();
    const status = document.getElementById('labStatus');

    if (status) status.textContent = t('lab_loading_commands');
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
      if (status) status.textContent = tf('lab_loaded_commands', { count: this.labQueue.length });
    } catch (err) {
      if (status) status.textContent = tf('failed_to_load_error', { error: err.message });
    }
  }

  useStoredLabCommands() {
    const devId = document.getElementById('labDevice')?.value;
    const dev = (this.inventory.devices || []).find(d => String(d.id) === String(devId));
    if (!dev) return alert(t("select_stored_device"));
    this.labQueue = (dev.commands || []).map(c => ({
      name: c.name,
      meta: t('lab_stored_command'),
      keycode: c.keycode,
      raw: c.raw,
      stored: true
    }));
    this.renderLabQueue();
  }

  renderLabQueue() {
    const box = document.getElementById('labQueue');
    const sum = document.getElementById('labSummary');
    if (sum) sum.textContent = tf('lab_queued_count', { count: this.labQueue.length });
    if (!box) return;
    if (!this.labQueue.length) {
      box.innerHTML = `<div class="muted mini" style="padding: 8px;">${t('lab_no_commands_queued')}</div>`;
      return;
    }
    box.innerHTML = this.labQueue.map((c, i) => `
      <div class="queue-row" style="padding: 4px 8px;">
        <label style="display: flex; align-items: center; gap: 8px; flex: 1; cursor: pointer; margin: 0;">
          <input type="checkbox" class="lab-pick" data-index="${i}" checked>
          <span><strong>${escapeHtml(c.name)}</strong> <span class="muted mini">(${escapeHtml(c.meta)})</span></span>
        </label>
        <span class="badge ${c.raw ? 'ok' : 'primary'}">${c.raw ? t('lab_badge_raw') : t('lab_badge_code')}</span>
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
    // Saved like the IR database tab does (/api/commands-batch): the Hub has no IR Lab import route
    const dev = (this.inventory?.devices || []).find(d => String(d.id) === String(devId));
    const port = dev?.controlPort ?? dev?.control_port ?? 7;
    const cmds = [];
    document.querySelectorAll('.lab-pick:checked').forEach(cb => {
      const idx = Number(cb.getAttribute('data-index'));
      const c = this.labQueue[idx];
      const code = c?.raw || c?.keycode;
      if (c && code) cmds.push({ name: c.name, raw: code, repeats: 3, port });
    });
    if (!cmds.length) return alert(t('no_cmds_selected', 'No commands selected.'));
    try {
      const res = await this.client.saveBatchCommands(devId, cmds);
      if (res && res.ok === false) throw new Error(res.error || t('import_failed_short'));
      alert(tf("imported_n_cmds", { count: cmds.length }));
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

    let done = 0;
    for (const cmd of selected) {
      if (!this.labRunning) break;
      done++;
      if (meter) meter.style.width = `${Math.round((done / selected.length) * 100)}%`;
      if (log) log.textContent = tf('lab_sending_progress', { done: done, total: selected.length, name: cmd.name });
      if (!dry) {
        try {
          await this.client.sendCommand(devId, cmd.name);
        } catch (e) {
          if (log) log.textContent += tf('lab_send_error_suffix', { error: e.message });
        }
      }
      await new Promise(r => setTimeout(r, delay));
    }

    this.labRunning = false;
    if (log) log.textContent = t('lab_bulk_test_done');
  }

  stopLabQueue() {
    // The run is a loop in this page (runLabQueue): clearing the flag stops it after the
    // command in flight
    this.labRunning = false;
  }

  /* -------------------------------------------------------------
   * 7. Bluetooth View
   * ------------------------------------------------------------- */
}
