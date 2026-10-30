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
import { HarmonyAppDevicesStudio } from './app_devices_studio.js';

export class HarmonyAppDevicesIrdb extends HarmonyAppDevicesStudio {
  async searchIrdb() {
    const q = document.getElementById('irdbSearch')?.value.trim();
    const src = document.getElementById('irdbSource')?.value || 'all';
    const status = document.getElementById('irdbStatus');
    const log = document.getElementById('irdbLog');
    const resBox = document.getElementById('irdbResults');

    if (status) status.textContent = 'Searching index...';
    if (log) log.textContent = `Searching for "${q}" in ${src}...`;

    try {
      let entries = [];
      if (src === 'all') {
        const [a, b] = await Promise.all([loadIrdSource('irdb'), loadIrdSource('flipper')]);
        entries = a.concat(b);
      } else {
        entries = await loadIrdSource(src);
      }
      const matches = rankIrdEntries(entries, q);
      if (status) status.textContent = `Found ${matches.length} files.`;

      if (resBox) {
        resBox.style.display = matches.length ? 'block' : 'none';
        resBox.innerHTML = matches.map(m => `
          <button type="button" class="btn btn-xs btn-ghost irdb-pick" data-path="${escapeHtml(m.path)}" data-source="${escapeHtml(m.source)}" style="width: 100%; text-align: left; margin-bottom: 4px;">
            <strong>[${sourceLabel(m.source)}]</strong> ${escapeHtml(m.path)}
          </button>
        `).join('');

        resBox.querySelectorAll('.irdb-pick').forEach(b => {
          b.addEventListener('click', () => {
            const p = b.getAttribute('data-path');
            const s = b.getAttribute('data-source');
            const pathInp = document.getElementById('irdbPath');
            if (pathInp) {
              pathInp.value = p;
              pathInp.dataset.source = s;
            }
            this.fetchSelectedIrdbFile();
          });
        });
      }
    } catch (err) {
      if (status) status.textContent = 'Search failed: ' + err.message;
    }
  }

  async fetchSelectedIrdbFile() {
    const pathInp = document.getElementById('irdbPath');
    const p = pathInp?.value.trim();
    const s = pathInp?.dataset.source || 'flipper';
    const status = document.getElementById('irdbStatus');

    if (!p) return alert(t("select_file_path_first"));
    try {
      if (status) status.textContent = 'Loading file...';
      const text = await fetchIrdFile(p, s);
      const rows = parseIrText(text, s, p);
      this.renderIrdbPreview(rows);
    } catch (err) {
      if (status) status.textContent = 'Failed to load file: ' + err.message;
    }
  }

  parsePastIrdb() {
    const paste = document.getElementById('irdbPaste')?.value.trim();
    if (!paste) return alert(t("paste_code_first"));
    const rows = parseIrText(paste, 'custom', '');
    this.renderIrdbPreview(rows);
  }

  renderIrdbPreview(rows = []) {
    const previewBox = document.getElementById('irdbPreview');
    const status = document.getElementById('irdbStatus');
    if (!previewBox) return;

    previewBox.style.display = 'block';
    if (!rows.length) {
      previewBox.innerHTML = '<div class="muted mini">No valid commands parsed.</div>';
      return;
    }

    const updateIrdbCount = () => {
      const checked = previewBox.querySelectorAll('.irdb-cmd-cb:checked').length;
      if (status) status.innerHTML = `Parsed ${rows.length} commands (<strong>${checked}</strong> of ${rows.length} selected).`;
    };

    previewBox.innerHTML = `
      <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:8px; padding-bottom:6px; border-bottom:1px solid var(--border);">
        <div style="display:flex; gap:6px;">
          <button type="button" id="btnIrdbSelectAll" class="btn btn-xs btn-secondary" style="padding:2px 8px; font-size:11px;">Select All</button>
          <button type="button" id="btnIrdbSelectNone" class="btn btn-xs btn-ghost" style="padding:2px 8px; font-size:11px;">Select None</button>
        </div>
      </div>
      <div id="irdbCheckboxesList">
        ${rows.map((r, i) => `
          <label style="display: flex; align-items: center; gap: 8px; font-size: 11px; margin-bottom: 4px; cursor: pointer;">
            <input type="checkbox" class="irdb-cmd-cb" data-name="${r.name}" data-keycode="${r.keycode || ''}" data-raw="${r.raw || ''}" checked>
            <span><strong>${r.name}</strong> (${r.meta})</span>
          </label>
        `).join('')}
      </div>
    `;

    document.getElementById('btnIrdbSelectAll')?.addEventListener('click', () => {
      previewBox.querySelectorAll('.irdb-cmd-cb').forEach(cb => { cb.checked = true; });
      updateIrdbCount();
    });
    document.getElementById('btnIrdbSelectNone')?.addEventListener('click', () => {
      previewBox.querySelectorAll('.irdb-cmd-cb').forEach(cb => { cb.checked = false; });
      updateIrdbCount();
    });
    previewBox.querySelectorAll('.irdb-cmd-cb').forEach(cb => {
      cb.addEventListener('change', updateIrdbCount);
    });
    updateIrdbCount();
  }

  async saveSelectedIrdbCommands() {
    const devId = document.getElementById('manageDeviceSelect')?.value || document.getElementById('irdbDevice')?.value;
    const prefix = document.getElementById('irdbPrefix')?.value || '';
    if (!devId) return alert(t('select_device_first_dot', 'Select a device first.'));

    const targetDev = (this.inventory.devices || []).find(d => String(d.id) === String(devId));
    const targetPort = targetDev ? ((targetDev.controlPort !== undefined) ? targetDev.controlPort : 7) : 7;

    const cmdPayloads = [];
    const lines = [];
    document.querySelectorAll('.irdb-cmd-cb:checked').forEach(cb => {
      const name = prefix + cb.getAttribute('data-name');
      const raw = cb.getAttribute('data-raw');
      const key = cb.getAttribute('data-keycode');
      if (raw) {
        lines.push(`${name}|raw|${raw}`);
        cmdPayloads.push({ name, raw, repeats: 3, port: targetPort });
      } else if (key) {
        lines.push(`${name}|${key}`);
        cmdPayloads.push({ name, raw: key, repeats: 3, port: targetPort });
      }
    });

    if (!cmdPayloads.length) return alert(t("no_cmds_selected"));
    try {
      let ok = false;
      try {
        const res = await this.client.saveBatchCommands(devId, cmdPayloads);
        if (res && res.ok) ok = true;
      } catch {
        ok = false;
      }
      if (!ok) {
        await this.client.importIrdb({ deviceId: devId, payload: lines.join('\n') });
      }
      alert(t("imported_cmds_to").replace("{count}", cmdPayloads.length).replace("{device}", targetDev?.name || devId));
      await this.refreshData();
      this.renderIrView(devId);
    } catch (err) {
      alert(t('import_failed', 'Import failed: ') + err.message);
    }
  }

  async captureIrSignal() {
    const status = document.getElementById('wizardLearnStatus');
    const rawBox = document.getElementById('wizardRaw');
    const necBox = document.getElementById('wizardNec');
    const keyBox = document.getElementById('wizardKeycode');

    try {
      if (status) status.textContent = 'Listening for IR signal (press remote button)...';
      const res = await this.client.captureIr();
      if (res && res.raw) {
        if (rawBox) rawBox.value = res.raw;
        if (necBox && res.nec) necBox.value = res.nec;
        if (keyBox && res.keycode) keyBox.value = res.keycode;
        if (status) status.textContent = `Captured signal! Mode: ${res.mode || 'raw'}`;
      } else {
        if (status) status.textContent = 'No signal received or timed out.';
      }
    } catch (err) {
      if (status) status.textContent = 'Capture failed: ' + err.message;
    }
  }

  async testLearnedIrSignal() {
    const raw = document.getElementById('wizardRaw')?.value.trim();
    const nec = document.getElementById('wizardNec')?.value.trim();
    const keycode = document.getElementById('wizardKeycode')?.value.trim();
    const mode = document.getElementById('wizardMode')?.value || 'auto';
    const proto = document.getElementById('wizardProtocol')?.value || '2';
    const devId = document.getElementById('wizardDevice')?.value;
    const status = document.getElementById('wizardLearnStatus');

    try {
      if (status) status.textContent = 'Sending test signal...';
      await this.client.testLearnedIr({ deviceId: devId, raw, nec, keycode, mode, protocolId: proto });
      if (status) status.textContent = 'Test signal transmitted!';
    } catch (err) {
      if (status) status.textContent = 'Test failed: ' + err.message;
    }
  }

  async saveLearnedIrCommand() {
    const devId = document.getElementById('wizardDevice')?.value;
    const name = document.getElementById('wizardCommandName')?.value.trim();
    const raw = document.getElementById('wizardRaw')?.value.trim();
    const nec = document.getElementById('wizardNec')?.value.trim();
    const keycode = document.getElementById('wizardKeycode')?.value.trim();
    const mode = document.getElementById('wizardMode')?.value || 'auto';
    const protocol = document.getElementById('wizardProtocol')?.value || '2';
    const status = document.getElementById('wizardLearnStatus');

    if (!devId || !name) return alert(t("dev_cmd_name_required"));

    try {
      if (status) status.textContent = 'Saving command...';
      await this.client.saveLearnedCommand({ deviceId: devId, name, raw, nec, keycode, mode, protocol });
      if (status) status.innerHTML = '<span style="color:var(--success)">✓ Command saved!</span>';
      await this.refreshData();
    } catch (err) {
      if (status) status.innerHTML = `<span style="color:var(--danger)">Error: ${escapeHtml(err.message)}</span>`;
    }
  }

  populateVerifyDropdowns() {
    const devSel = document.getElementById('verifyDevice');
    const cmdSel = document.getElementById('verifyCommand');
    if (!devSel || !cmdSel) return;
    const devs = this.inventory.devices || [];
    devSel.innerHTML = devs.map(d => `<option value="${escapeHtml(d.id)}">${escapeHtml(d.name || d.id)}</option>`).join('');

    const updateCmds = () => {
      const d = devs.find(x => String(x.id) === String(devSel.value));
      cmdSel.innerHTML = (d?.commands || []).map(c => `<option value="${escapeHtml(c.name)}">${escapeHtml(c.name)}</option>`).join('');
    };
    devSel.onchange = updateCmds;
    updateCmds();
  }

  async sendVerifyTestCommand() {
    const devId = document.getElementById('verifyDevice')?.value;
    const cmd = document.getElementById('verifyCommand')?.value;
    const status = document.getElementById('wizardVerifyStatus');
    if (!devId || !cmd) return alert(t("select_dev_cmd_first"));
    try {
      if (status) status.textContent = `Sending ${cmd}...`;
      await this.client.sendCommand(devId, cmd);
      if (status) status.textContent = `Sent ${cmd} successfully!`;
    } catch (err) {
      if (status) status.textContent = `Send failed: ${err.message}`;
    }
  }

  populateManageDevices(selectedId = null) {
    const sel = document.getElementById('manageDeviceSelect');
    const editor = document.getElementById('storedDeviceEditor');
    if (!sel || !editor) return;

    const previousId = selectedId || sel.value;
    const devs = this.inventory.devices || [];
    sel.innerHTML = devs.map(d => `<option value="${escapeHtml(d.id)}">${escapeHtml(d.name || d.id)}</option>`).join('');

    if (previousId && devs.some(d => String(d.id) === String(previousId))) {
      sel.value = String(previousId);
    }

    const render = () => {
      const dev = devs.find(d => String(d.id) === String(sel.value));
      if (!dev) { editor.innerHTML = ''; return; }
      const cp = dev.control_port > 0 ? dev.control_port : 7;

      editor.innerHTML = `
        <div class="card" style="margin-bottom: 12px;">
          <h3 style="font-size: 15px; margin-bottom: 8px;">Device Details</h3>
          <div class="row">
            <div><label>Name</label><input id="mgrDevName" class="form-control" value="${dev.name || ''}"></div>
            <div><label>Type</label><input id="mgrDevType" class="form-control" value="${dev.type || ''}"></div>
          </div>
          <div class="row" style="margin-top: 8px;">
            <div><label>Manufacturer</label><input id="mgrDevMan" class="form-control" value="${dev.manufacturer || ''}"></div>
            <div><label>Model</label><input id="mgrDevMod" class="form-control" value="${dev.model || ''}"></div>
          </div>
          <div style="margin-top: 10px;">
            <label>IR Blaster Assignment</label>
            <div style="display: flex; gap: 12px; flex-wrap: wrap;">
              <label style="display: flex; align-items: center; gap: 6px;"><input type="checkbox" id="mgrBlasterInternal" value="4" ${(cp & 4) ? 'checked' : ''}> Hub Internal</label>
              <label style="display: flex; align-items: center; gap: 6px;"><input type="checkbox" id="mgrBlasterP1" value="1" ${(cp & 1) ? 'checked' : ''}> Blaster Port 1</label>
              <label style="display: flex; align-items: center; gap: 6px;"><input type="checkbox" id="mgrBlasterP2" value="2" ${(cp & 2) ? 'checked' : ''}> Blaster Port 2</label>
            </div>
          </div>
          <div class="actions" style="margin-top: 12px; display: flex; justify-content: space-between;">
            <button type="button" id="btnMgrSaveDev" class="btn btn-sm btn-primary">Save Device Details</button>
            <button type="button" id="btnMgrDelDev" class="btn btn-sm btn-danger">Delete Device</button>
          </div>
        </div>

        <div class="card" style="margin-bottom: 12px;">
          <h3 style="font-size: 15px; margin-bottom: 8px;">Power Management</h3>
          <label style="display: flex; align-items: center; gap: 6px;">
            <input type="checkbox" id="mgrDevAlwaysOn" ${dev.is_power_always_on ? 'checked' : ''}>
            <span>Always On (leave powered between activities)</span>
          </label>
          <div style="margin-top: 8px;">
            <label>Warmup Delay (ms)</label>
            <input id="mgrDevPowerDelay" type="number" class="form-control" value="${dev.power_on_delay || 1500}">
          </div>
          <div class="actions" style="margin-top: 12px;">
            <button type="button" id="btnMgrSavePower" class="btn btn-sm btn-secondary">Save Power Settings</button>
          </div>
        </div>

        <div class="card" style="margin-bottom: 12px;">
          <h3 style="font-size: 15px; margin-bottom: 8px;">MQTT Connection</h3>
          <label style="display: flex; align-items: center; gap: 6px;">
            <input type="checkbox" id="mgrDevMqttEnabled" ${dev.mqtt_enabled ? 'checked' : ''}>
            <span>Publish button presses to MQTT</span>
          </label>
          <div class="row" style="margin-top: 8px;">
            <div>
              <label>Topic Template</label>
              <input id="mgrDevMqttTopic" class="form-control" value="${dev.mqtt_topic || '{root}/button/{device}/{command}'}">
              <div style="display:flex; gap:4px; margin-top:4px; flex-wrap:wrap;">
                <button type="button" class="btn btn-xs btn-ghost btn-insert-token" data-token="{root}">{root}</button>
                <button type="button" class="btn btn-xs btn-ghost btn-insert-token" data-token="{device}">{device}</button>
                <button type="button" class="btn btn-xs btn-ghost btn-insert-token" data-token="{command}">{command}</button>
                <button type="button" class="btn btn-xs btn-ghost btn-insert-token" data-token="{state}">{state}</button>
              </div>
            </div>
            <div><label>Pulse (ms)</label><input id="mgrDevMqttPulse" type="number" class="form-control" value="${dev.mqtt_pulse_ms || 1000}"></div>
          </div>
          <div class="actions" style="margin-top: 12px; display: flex; gap: 8px;">
            <button type="button" id="btnMgrSaveMqtt" class="btn btn-sm btn-secondary">Save MQTT</button>
            <button type="button" id="btnMgrTestMqtt" class="btn btn-sm btn-ghost">Test Pulse</button>
          </div>
        </div>

        <div class="card">
          <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px;">
            <h3 style="font-size: 15px; margin: 0;">Saved Commands (<span id="mgrCmdCount">${(dev.commands || []).length}</span>)</h3>
            <span class="muted mini">Device: ${dev.name || dev.id}</span>
          </div>
          <div style="margin-bottom: 8px;">
            <input type="text" id="mgrCmdSearch" class="form-control" placeholder="Search commands (e.g. Power, Play, Volume)...">
          </div>
          <div id="mgrCmdsList" style="display: flex; flex-direction: column; gap: 6px; max-height: 280px; overflow-y: auto;">
          </div>
        </div>
      `;

      editor.querySelectorAll('.btn-insert-token').forEach(tb => {
        tb.addEventListener('click', () => {
          const inp = editor.querySelector('#mgrDevMqttTopic');
          if (inp) {
            const token = tb.getAttribute('data-token');
            inp.value = inp.value ? `${inp.value}/${token}` : token;
          }
        });
      });

      editor.querySelector('#btnMgrSaveDev')?.addEventListener('click', async () => {
        let cpVal = 0;
        if (editor.querySelector('#mgrBlasterInternal')?.checked) cpVal |= 4;
        if (editor.querySelector('#mgrBlasterP1')?.checked) cpVal |= 1;
        if (editor.querySelector('#mgrBlasterP2')?.checked) cpVal |= 2;
        await this.client.saveDeviceDetails({
          deviceId: dev.id,
          name: editor.querySelector('#mgrDevName').value,
          type: editor.querySelector('#mgrDevType').value,
          manufacturer: editor.querySelector('#mgrDevMan').value,
          model: editor.querySelector('#mgrDevMod').value,
          controlPort: cpVal
        });
        alert(t("device_details_saved"));
        await this.refreshData();
        this.populateManageDevices(dev.id);
      });

      editor.querySelector('#btnMgrDelDev')?.addEventListener('click', async () => {
        if (!await showConfirm(`Delete device ${dev.name}?`, { title: 'Delete Device', okText: 'Delete' })) return;
        await this.client.deleteDevice(dev.id);
        alert(t("device_deleted"));
        await this.refreshData();
        this.populateManageDevices();
      });

      editor.querySelector('#btnMgrSavePower')?.addEventListener('click', async () => {
        await this.client.saveDevicePower({
          deviceId: dev.id,
          isPowerAlwaysOn: editor.querySelector('#mgrDevAlwaysOn').checked ? 1 : 0,
          powerOnDelay: Number(editor.querySelector('#mgrDevPowerDelay').value) || 1500
        });
        alert(t("power_saved"));
        await this.refreshData();
        this.populateManageDevices(dev.id);
      });

      editor.querySelector('#btnMgrSaveMqtt')?.addEventListener('click', async () => {
        await this.client.saveDeviceMqtt({
          deviceId: dev.id,
          mqttEnabled: editor.querySelector('#mgrDevMqttEnabled').checked ? 1 : 0,
          mqttTopic: editor.querySelector('#mgrDevMqttTopic').value,
          mqttPulseMs: Number(editor.querySelector('#mgrDevMqttPulse').value) || 1000
        });
        alert(t("mqtt_saved"));
        await this.refreshData();
        this.populateManageDevices(dev.id);
      });

      editor.querySelector('#btnMgrTestMqtt')?.addEventListener('click', async () => {
        const mgrTopic = editor.querySelector('#mgrDevMqttTopic')?.value?.trim();
        const mgrPulse = Number(editor.querySelector('#mgrDevMqttPulse')?.value) || 1000;
        await this.client.testDeviceMqtt({
          deviceId: dev.id,
          device: dev.name || dev.id,
          command: 'PowerToggle',
          topic: mgrTopic,
          pulseMs: mgrPulse
        });
        alert(t("test_mqtt_pulse_sent"));
      });

      const renderCmds = (filter = '') => {
        const q = filter.trim().toLowerCase();
        const allCmds = dev.commands || [];
        // Sort with friendly named commands first (letters), numbers grouped together
        const sortedCmds = [...allCmds].sort((a, b) => {
          const aIsNum = /^\d+$/.test(a.name || '');
          const bIsNum = /^\d+$/.test(b.name || '');
          if (aIsNum && !bIsNum) return 1;
          if (!aIsNum && bIsNum) return -1;
          return (a.name || '').localeCompare(b.name || '');
        });
        const filtered = q ? sortedCmds.filter(c => (c.name || '').toLowerCase().includes(q)) : sortedCmds;
        const listEl = editor.querySelector('#mgrCmdsList');
        const countEl = editor.querySelector('#mgrCmdCount');
        if (countEl) countEl.textContent = q ? `${filtered.length} / ${allCmds.length}` : allCmds.length;
        if (!listEl) return;
        if (!filtered.length) {
          listEl.innerHTML = `<div class="muted mini" style="padding: 12px; text-align: center;">${allCmds.length === 0 ? 'No commands saved for this device.' : 'No commands matching search.'}</div>`;
          return;
        }
        listEl.innerHTML = filtered.map(c => `
          <div class="queue-row" style="display: flex; justify-content: space-between; align-items: center; padding: 6px 8px; border: 1px solid var(--border); border-radius: var(--radius-sm); margin-bottom: 4px; background: rgba(255,255,255,0.02);">
            <div>
              <strong style="font-size: 13px;">${escapeHtml(c.name)}</strong>
              <div class="muted mini">Proto ${escapeHtml(c.protocolId || c.protocol_id || 'IR')}${c.learned ? ' • Learned' : ''}${c.raw ? ' • Raw' : ''}</div>
            </div>
            <div style="display: flex; gap: 6px;">
              <button type="button" class="btn btn-xs btn-primary btn-mgr-test-cmd" data-cmd="${escapeHtml(c.name)}">Send</button>
              <button type="button" class="btn btn-xs btn-danger btn-mgr-del-cmd" data-cmd="${escapeHtml(c.name)}">Delete</button>
            </div>
          </div>
        `).join('');

        listEl.querySelectorAll('.btn-mgr-test-cmd').forEach(b => {
          b.addEventListener('click', () => this.client.sendCommand(dev.id, b.getAttribute('data-cmd')));
        });

        listEl.querySelectorAll('.btn-mgr-del-cmd').forEach(b => {
          b.addEventListener('click', async () => {
            const cmd = b.getAttribute('data-cmd');
            if (!await showConfirm(`Delete command ${cmd}?`, { title: 'Delete Command', okText: 'Delete' })) return;
            await this.client.deleteCommand(dev.id, cmd);
            await this.refreshData();
            render();
          });
        });
      };

      const searchInput = editor.querySelector('#mgrCmdSearch');
      if (searchInput) {
        searchInput.addEventListener('input', (e) => renderCmds(e.target.value));
      }
      renderCmds();
    };

    sel.onchange = render;
    render();
  }

  /* -------------------------------------------------------------
   * 6. Bulk IR Test View (Lab)
   * ------------------------------------------------------------- */
}
