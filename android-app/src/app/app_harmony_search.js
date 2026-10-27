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
import { HarmonyAppBluetooth } from './app_bluetooth.js';

export class HarmonyAppHarmonySearch extends HarmonyAppBluetooth {
  setupHarmonyArchiveSearch() {
    const mfgInput = document.getElementById('harmonyMfgInput');
    const mfgDatalist = document.getElementById('harmonyMfgDatalist');
    const modelInput = document.getElementById('harmonyModelInput');
    const modelDatalist = document.getElementById('harmonyModelDatalist');
    const searchBtn = document.getElementById('harmonySearchBtn');
    const statusEl = document.getElementById('harmonyDbStatus');
    const specsCard = document.getElementById('harmonySpecsCard');
    const titleEl = document.getElementById('harmonyDeviceTitle');
    const catEl = document.getElementById('harmonyDeviceCategory');
    const codesetEl = document.getElementById('harmonyDeviceCodeset');
    const gridEl = document.getElementById('harmonyCommandsGrid');
    const importBtn = document.getElementById('harmonyImportBtn');
    const targetDevSel = document.getElementById('harmonyTargetDevSelect');
    const cmdCountEl = document.getElementById('harmonyCmdCount');
    const cmdSelectedCountEl = document.getElementById('harmonyCmdSelectedCount');
    const btnSelectAll = document.getElementById('btnHarmonySelectAll');
    const btnSelectNone = document.getElementById('btnHarmonySelectNone');
    const cmdFilterInput = document.getElementById('harmonyCmdFilter');
    const importBtnCountEl = document.getElementById('harmonyImportBtnCount');
    const importStatusEl = document.getElementById('harmonyImportStatus');

    if (!mfgInput || !mfgDatalist) return;

    // Populate initial manufacturers in datalist
    const mfgList = getHarmonyManufacturers();
    mfgDatalist.innerHTML = mfgList.slice(0, 200).map(m => `<option value="${m}"></option>`).join('');

    let currentModels = [];
    let currentDetail = null;
    let currentCodeset = null;

    const updateSelectedCount = () => {
      const allCbs = gridEl ? Array.from(gridEl.querySelectorAll('.harmony-cmd-cb')) : [];
      const checkedCbs = allCbs.filter(cb => cb.checked);
      const totalCount = currentCodeset?.commands?.length || 0;
      const selCount = checkedCbs.length;

      if (cmdCountEl) cmdCountEl.textContent = totalCount;
      if (cmdSelectedCountEl) cmdSelectedCountEl.textContent = selCount;
      if (importBtnCountEl) importBtnCountEl.textContent = selCount;
      if (importBtn) {
        importBtn.disabled = selCount === 0;
        importBtn.textContent = `Import Selected Commands (${selCount})`;
      }
    };

    const filterCommands = (filterText) => {
      if (!gridEl) return;
      const q = (filterText || '').toLowerCase().trim();
      const items = gridEl.querySelectorAll('.harmony-cmd-item');
      items.forEach(item => {
        const name = (item.querySelector('.cmd-chip-name')?.textContent || '').toLowerCase();
        if (!q || name.includes(q)) {
          item.style.display = 'inline-flex';
        } else {
          item.style.display = 'none';
        }
      });
    };

    cmdFilterInput?.addEventListener('input', (e) => {
      filterCommands(e.target.value);
    });

    btnSelectAll?.addEventListener('click', () => {
      if (!gridEl) return;
      const filterText = (cmdFilterInput?.value || '').toLowerCase().trim();
      gridEl.querySelectorAll('.harmony-cmd-item').forEach(item => {
        const name = (item.querySelector('.cmd-chip-name')?.textContent || '').toLowerCase();
        if (!filterText || name.includes(filterText)) {
          const cb = item.querySelector('.harmony-cmd-cb');
          if (cb) cb.checked = true;
        }
      });
      updateSelectedCount();
    });

    btnSelectNone?.addEventListener('click', () => {
      if (!gridEl) return;
      const filterText = (cmdFilterInput?.value || '').toLowerCase().trim();
      gridEl.querySelectorAll('.harmony-cmd-item').forEach(item => {
        const name = (item.querySelector('.cmd-chip-name')?.textContent || '').toLowerCase();
        if (!filterText || name.includes(filterText)) {
          const cb = item.querySelector('.harmony-cmd-cb');
          if (cb) cb.checked = false;
        }
      });
      updateSelectedCount();
    });

    targetDevSel?.addEventListener('change', () => {
      const devId = targetDevSel.value;
      if (devId) {
        this.selectedIrDeviceId = devId;
        const manageSel = document.getElementById('manageDeviceSelect');
        if (manageSel && manageSel.value !== devId) {
          manageSel.value = devId;
          this.updateStudioDeviceView();
        }
      }
    });

    mfgInput.addEventListener('input', () => {
      const q = mfgInput.value.trim();
      const matches = searchHarmonyManufacturers(q, 100);
      mfgDatalist.innerHTML = matches.map(m => `<option value="${m}"></option>`).join('');
    });

    const loadModelsForMfg = async (mfg) => {
      if (!mfg) return;
      if (statusEl) statusEl.textContent = `Loading model index for ${mfg}...`;
      try {
        currentModels = await fetchHarmonyModels(mfg);
        if (modelDatalist) {
          modelDatalist.innerHTML = currentModels.slice(0, 300).map(item => `<option value="${item.m}"></option>`).join('');
        }
        if (statusEl) statusEl.textContent = `Found ${currentModels.length} models for ${mfg}. Enter or pick model:`;
      } catch (err) {
        if (statusEl) statusEl.innerHTML = `<span style="color:var(--danger)">Failed to load models for ${mfg}: ${err.message}</span>`;
      }
    };

    mfgInput.addEventListener('change', () => {
      loadModelsForMfg(mfgInput.value.trim());
    });

    modelInput?.addEventListener('input', () => {
      if (!currentModels || !currentModels.length) return;
      const q = modelInput.value.trim();
      const filtered = searchHarmonyModels(currentModels, q, 100);
      if (modelDatalist) {
        modelDatalist.innerHTML = filtered.map(item => `<option value="${item.m}"></option>`).join('');
      }
    });

    const triggerDeviceLookup = async () => {
      const mfg = mfgInput.value.trim();
      const modelName = modelInput?.value.trim();
      if (!mfg || !modelName) {
        alert(t("specify_mfg_model"));
        return;
      }
      if (statusEl) statusEl.textContent = `Looking up specs & codeset for ${mfg} ${modelName}...`;
      if (specsCard) specsCard.style.display = 'none';

      try {
        if (!currentModels.length) {
          currentModels = await fetchHarmonyModels(mfg);
        }
        const match = currentModels.find(item => item.m.toLowerCase() === modelName.toLowerCase()) ||
          currentModels.find(item => item.m.toLowerCase().includes(modelName.toLowerCase()));
        if (!match) {
          if (statusEl) statusEl.innerHTML = `<span style="color:var(--danger)">Model "${modelName}" not found under ${mfg}.</span>`;
          return;
        }

        const detail = await fetchHarmonyDeviceDetail(mfg, match.f);
        currentDetail = detail;

        if (!detail.codeset) {
          if (statusEl) statusEl.innerHTML = `<span style="color:var(--danger)">No IR codeset linked to this model.</span>`;
          return;
        }

        const codeset = await fetchHarmonyCodeset(detail.codeset);
        currentCodeset = codeset;

        const totalCmdCount = codeset.commands ? codeset.commands.length : 0;
        if (statusEl) statusEl.textContent = `✓ Loaded ${totalCmdCount} commands from Harmony Archive!`;
        if (specsCard) specsCard.style.display = 'block';
        if (titleEl) titleEl.textContent = `${detail.manufacturer || mfg} ${detail.model || modelName}`;
        if (catEl) catEl.textContent = mapHarmonyDeviceType(detail.deviceType);
        if (codesetEl) codesetEl.textContent = detail.codeset;

        // Sync target device selector with existing devices
        if (targetDevSel) {
          const devs = this.inventory.devices || [];
          const curDevId = this.selectedIrDeviceId || document.getElementById('manageDeviceSelect')?.value || (devs[0] && devs[0].id);
          targetDevSel.innerHTML = devs.map(d => `<option value="${d.id}">${d.name || d.id}</option>`).join('');
          if (curDevId && devs.some(d => String(d.id) === String(curDevId))) {
            targetDevSel.value = String(curDevId);
          }
        }

        if (gridEl) {
          const cmds = codeset.commands || [];
          gridEl.innerHTML = cmds.map((c, i) => `
            <label class="cmd-chip harmony-cmd-item" data-idx="${i}" style="display:inline-flex; align-items:center; gap:6px; background:var(--bg-secondary); border:1px solid var(--border); border-radius:6px; padding:4px 8px; margin:3px; font-size:12px; cursor:pointer; user-select:none;">
              <input type="checkbox" class="harmony-cmd-cb" data-idx="${i}" checked style="cursor:pointer; margin:0;">
              <span class="cmd-chip-name">${c.name}</span>
              ${c.pronto ? `<button type="button" class="btn btn-xs btn-ghost btn-test-archive-cmd" data-idx="${i}" title="Test Blast Pronto Hex" style="padding:1px 5px; font-size:10px; margin-left:2px;">▶ Test</button>` : ''}
            </label>
          `).join('');

          gridEl.querySelectorAll('.harmony-cmd-cb').forEach(cb => {
            cb.addEventListener('change', () => updateSelectedCount());
          });

          gridEl.querySelectorAll('.btn-test-archive-cmd').forEach(btn => {
            btn.addEventListener('click', async (e) => {
              e.preventDefault();
              e.stopPropagation();
              const idx = parseInt(btn.getAttribute('data-idx'), 10);
              const cmd = (codeset.commands || [])[idx];
              if (!cmd || !cmd.pronto) return;
              btn.textContent = '...';
              try {
                await this.client.sendDirectPronto(cmd.pronto, 7, 1);
                btn.textContent = '✓';
                setTimeout(() => { btn.textContent = '▶ Test'; }, 1000);
              } catch (err) {
                btn.textContent = '✗';
                alert('Test blast failed: ' + err.message);
                setTimeout(() => { btn.textContent = '▶ Test'; }, 1500);
              }
            });
          });
        }

        if (cmdFilterInput) cmdFilterInput.value = '';
        if (importStatusEl) importStatusEl.innerHTML = '';
        updateSelectedCount();
      } catch (err) {
        if (statusEl) statusEl.innerHTML = `<span style="color:var(--danger)">Error: ${err.message}</span>`;
      }
    };

    searchBtn?.addEventListener('click', triggerDeviceLookup);
    modelInput?.addEventListener('change', () => {
      if (modelInput.value.trim()) triggerDeviceLookup();
    });

    importBtn?.addEventListener('click', async () => {
      if (!currentCodeset || !currentCodeset.commands) return;
      const targetDevId = targetDevSel?.value || document.getElementById('manageDeviceSelect')?.value || this.selectedIrDeviceId;
      if (!targetDevId) return alert(t("select_existing_target"));

      const targetDev = (this.inventory.devices || []).find(d => String(d.id) === String(targetDevId));
      if (!targetDev) return alert(t("target_dev_not_found"));

      const checkedCbs = Array.from(gridEl?.querySelectorAll('.harmony-cmd-cb:checked') || []);
      if (!checkedCbs.length) return alert(t("no_cmds_to_import"));

      const selectedCmds = checkedCbs.map(cb => {
        const idx = parseInt(cb.getAttribute('data-idx'), 10);
        return (currentCodeset.commands || [])[idx];
      }).filter(Boolean);

      importBtn.disabled = true;
      importBtn.textContent = `Importing ${selectedCmds.length} commands...`;
      if (importStatusEl) importStatusEl.innerHTML = `Importing ${selectedCmds.length} commands into "${targetDev.name}"...`;

      try {
        const targetPort = (targetDev.controlPort !== undefined) ? targetDev.controlPort : ((targetDev.control_port !== undefined) ? targetDev.control_port : 7);
        const cmdPayloads = selectedCmds.map(c => ({
          name: c.name,
          raw: c.pronto || c.raw || '',
          repeats: 3,
          port: targetPort
        })).filter(c => c.raw);

        let ok = false;
        try {
          const res = await this.client.saveBatchCommands(targetDevId, cmdPayloads);
          if (res && res.ok) ok = true;
        } catch {
          ok = false;
        }

        if (!ok) {
          for (const cmd of cmdPayloads) {
            await this.client.saveLearnedCommand({
              deviceId: targetDevId,
              name: cmd.name,
              raw: cmd.raw,
              repeats: cmd.repeats,
              port: cmd.port
            });
          }
        }

        if (importStatusEl) importStatusEl.innerHTML = `<span style="color:var(--success)">✓ Successfully imported ${cmdPayloads.length} commands into "${targetDev.name}"!</span>`;
        await this.refreshData();
        this.renderIrView(targetDevId);
      } catch (err) {
        alert('Failed to import commands: ' + err.message);
        if (importStatusEl) importStatusEl.innerHTML = `<span style="color:var(--danger)">Error: ${err.message}</span>`;
      } finally {
        importBtn.disabled = false;
        updateSelectedCount();
      }
    });
  }

  /* -------------------------------------------------------------
   * 8. BT Remote View (Homatics B25)
   * ------------------------------------------------------------- */
}
