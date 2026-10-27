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
import { HarmonyAppActivitiesView } from './app_activities_view.js';

export class HarmonyAppActivitiesEditor extends HarmonyAppActivitiesView {
  openActivityEditor(act) {
    if (this.userRole === 'user') return;
    const box = document.getElementById('actEditorBox');
    if (!box) return;
    box.style.display = 'block';

    const actNav = document.getElementById('actEditTabsNav');
    if (actNav) {
      actNav.querySelectorAll('.segmented-btn').forEach((b, idx) => b.classList.toggle('active', idx === 0));
      const pProfile = document.getElementById('actTabPanelProfile');
      const pStart = document.getElementById('actTabPanelStart');
      const pStop = document.getElementById('actTabPanelStop');
      if (pProfile) pProfile.style.display = 'block';
      if (pStart) pStart.style.display = 'none';
      if (pStop) pStop.style.display = 'none';
    }

    const titleEl = document.getElementById('actEditorTitle');
    const idInput = document.getElementById('actEditId');
    const nameInput = document.getElementById('actEditName');
    const typeSelect = document.getElementById('actEditType');
    const iconInput = document.getElementById('actEditIcon');
    const orderInput = document.getElementById('actEditOrder');
    const delBtn = document.getElementById('actDeleteBtn');

    let iconVal = act ? (act.icon || 'watchtv.png') : 'watchtv.png';
    if (!iconVal.endsWith('.png')) iconVal += '.png';
    if (iconInput) {
      iconInput.value = iconVal;
      if (!iconInput.value) {
        iconInput.value = iconVal.replace('_small.png', '.png');
      }
      if (!iconInput.value) {
        iconInput.value = 'generic.png';
      }
      this.updateBadgeForSelect(iconInput, 'actIconPreviewBadge');
    }

    if (act) {
      if (titleEl) titleEl.textContent = `Edit Activity: ${act.name}`;
      if (idInput) idInput.value = act.id;
      if (nameInput) nameInput.value = act.name || '';
      if (typeSelect) typeSelect.value = act.type || 'Custom';
      if (orderInput) orderInput.value = act.order || 1;
      if (delBtn) delBtn.style.display = (String(act.id) === '-1') ? 'none' : 'block';
    } else {
      if (titleEl) titleEl.textContent = 'Create New Activity';
      if (idInput) idInput.value = '';
      if (nameInput) nameInput.value = '';
      if (typeSelect) typeSelect.value = 'Custom';
      if (orderInput) orderInput.value = 1;
      if (delBtn) delBtn.style.display = 'none';
    }

    // Participating devices in power order (like web UI)
    const chkCont = document.getElementById('actDevicesCheckboxes');
    if (chkCont) {
      const partDevIds = (act?.deviceIds || act?.devices || []).map(String);
      chkCont.innerHTML = `
        <div id="actDevOrderedList" class="queue-list" style="min-height: 40px; max-height: 180px; margin-bottom: 6px;"></div>
        <div id="actAddDevControls" style="display: flex; gap: 6px; align-items: center; margin-top: 4px;">
          <select id="actAddDevSelect" class="form-control" style="flex: 1;"></select>
          <button type="button" id="actAddDevBtn" class="btn btn-xs btn-secondary">+ Add Device</button>
        </div>
        <div class="muted mini" style="margin-top: 4px;">Start/Stop Order: Devices power ON in order (#1..#N) and power OFF in reverse.</div>
      `;

      const listWrap = document.getElementById('actDevOrderedList');
      const addControls = document.getElementById('actAddDevControls');
      const addSel = document.getElementById('actAddDevSelect');
      const addBtn = document.getElementById('actAddDevBtn');

      const refreshAddDevControls = () => {
        if (!addControls || !addSel) return;
        const currentDevIds = new Set(
          Array.from(listWrap.querySelectorAll('.act-dev-row')).map(r => String(r.dataset.devId))
        );
        const availableDevices = (this.inventory.devices || []).filter(
          d => !currentDevIds.has(String(d.id))
        );
        if (!availableDevices.length) {
          addControls.style.display = 'none';
          addSel.innerHTML = '';
        } else {
          addControls.style.display = 'flex';
          addSel.innerHTML = availableDevices.map(
            d => `<option value="${d.id}">${d.name || d.model || d.id}</option>`
          ).join('');
        }
      };

      const renderDevRow = (did) => {
        const placeholder = listWrap.querySelector('.no-devs-msg');
        if (placeholder) placeholder.remove();

        const d = (this.inventory.devices || []).find(x => String(x.id) === String(did));
        const name = d ? (d.name || d.model || d.id) : `Device ${did}`;
        const devIconSvg = d ? getDeviceIconSvg(d.type, d.manufacturer, d.model, d.icon || d.Icon || '', 16) : '';
        const row = document.createElement('div');
        row.className = 'queue-row act-dev-row';
        row.dataset.devId = String(did);
        row.style.cssText = 'display:flex;align-items:center;gap:8px;padding:6px 8px;margin-bottom:4px;';
        row.innerHTML = `
          <span class="muted mini act-dev-num" style="min-width:24px;">#</span>
          <span style="display:inline-flex;align-items:center;margin-right:2px;">${devIconSvg}</span>
          <span style="flex:1;font-weight:600;font-size:13px;">${name}</span>
          <div style="display:flex;gap:4px;">
            <button type="button" class="btn btn-xs btn-ghost btn-dev-up" title="Power on earlier">↑</button>
            <button type="button" class="btn btn-xs btn-ghost btn-dev-down" title="Power on later">↓</button>
            <button type="button" class="btn btn-xs btn-danger btn-dev-del" title="Remove device">✕</button>
          </div>
        `;
        row.querySelector('.btn-dev-up').addEventListener('click', () => {
          if (row.previousElementSibling) {
            listWrap.insertBefore(row, row.previousElementSibling);
            updateDevNums();
          }
        });
        row.querySelector('.btn-dev-down').addEventListener('click', () => {
          if (row.nextElementSibling) {
            listWrap.insertBefore(row.nextElementSibling, row);
            updateDevNums();
          }
        });
        row.querySelector('.btn-dev-del').addEventListener('click', () => {
          row.remove();
          updateDevNums();
          if (!listWrap.querySelectorAll('.act-dev-row').length) {
            listWrap.innerHTML = '<div class="muted mini no-devs-msg" style="padding: 8px;">No participating devices configured.</div>';
          }
          refreshAddDevControls();
        });
        listWrap.appendChild(row);
        updateDevNums();
      };

      const updateDevNums = () => {
        listWrap.querySelectorAll('.act-dev-row').forEach((r, idx) => {
          const num = r.querySelector('.act-dev-num');
          if (num) num.textContent = `#${idx + 1}`;
        });
      };

      if (!partDevIds.length) {
        listWrap.innerHTML = '<div class="muted mini no-devs-msg" style="padding: 8px;">No participating devices configured.</div>';
      } else {
        partDevIds.forEach(did => renderDevRow(did));
      }
      refreshAddDevControls();

      if (addBtn) {
        addBtn.addEventListener('click', () => {
          const val = addSel?.value;
          if (!val) return;
          const exists = Array.from(listWrap.querySelectorAll('.act-dev-row')).some(r => r.dataset.devId === String(val));
          if (exists) return alert(t("dev_already_in_order"));
          renderDevRow(val);
          refreshAddDevControls();
        });
      }
    }

    // Render Sequences (support startSequence/stopSequence or startSteps/stopSteps)
    let rawStart = act?.startSequence || act?.startSteps || [];
    let rawStop = act?.stopSequence || act?.stopSteps || [];
    if (typeof rawStart === 'string') {
      rawStart = rawStart.split('\n').filter(Boolean).map(l => {
        const p = l.split('|');
        if (p[0] === 'Delay') return { type: 'Delay', deviceId: '0', command: '', delay: parseInt(p[3] || p[1], 10) || 1000 };
        if (p[0] === 'Switched' || p[0] === 'ActivitySwitched' || p[2] === 'Switched') return { type: 'Switched', deviceId: '0', command: 'Switched', delay: 0 };
        return { type: (p[0] && p[0] !== 'IRCommand') ? p[0] : 'Command', deviceId: p[1] || '0', command: p[2] || '', delay: 0 };
      });
    }
    if (typeof rawStop === 'string') {
      rawStop = rawStop.split('\n').filter(Boolean).map(l => {
        const p = l.split('|');
        if (p[0] === 'Delay') return { type: 'Delay', deviceId: '0', command: '', delay: parseInt(p[3] || p[1], 10) || 1000 };
        if (p[0] === 'Switched' || p[0] === 'ActivitySwitched' || p[2] === 'Switched') return { type: 'Switched', deviceId: '0', command: 'Switched', delay: 0 };
        return { type: (p[0] && p[0] !== 'IRCommand') ? p[0] : 'Command', deviceId: p[1] || '0', command: p[2] || '', delay: 0 };
      });
    }
    this.renderSequenceList('actStartStepsList', rawStart);
    this.renderSequenceList('actStopStepsList', rawStop);
  }

  renderSequenceList(containerId, steps = []) {
    const cont = document.getElementById(containerId);
    if (!cont) return;
    cont.innerHTML = '';
    if (!steps || !steps.length) {
      cont.innerHTML = '<div class="muted mini" style="padding: 8px;">No steps configured.</div>';
      return;
    }
    steps.forEach((st, idx) => {
      let type = st.type || (st.__type === 'DelayActivityAction' ? 'Delay' : 'Command');
      if (type === 'IRCommand') type = 'Command';
      const cName = st.command || st.Command || st.CommandName || '';
      if (type === 'Switched' || type === 'ActivitySwitched' || (st.__type && st.__type.includes('Switched')) || cName === 'Switched') {
        type = 'Switched';
      }
      this.addSequenceStep(containerId, type, st, idx);
    });
  }

  addSequenceStep(containerId, type = 'Command', initialData = null, index = -1) {
    const cont = document.getElementById(containerId);
    if (!cont) return;
    const placeholder = cont.querySelector('.muted');
    if (placeholder) placeholder.remove();

    const row = document.createElement('div');
    row.className = 'queue-row';
    const initCmd = initialData?.command || initialData?.Command || initialData?.CommandName || '';
    const isSwitchedExplicit = (type === 'Switched' || type === 'ActivitySwitched' || initCmd === 'Switched' || initialData?.type === 'Switched' || initialData?.type === 'ActivitySwitched');
    const normType = isSwitchedExplicit ? 'Switched' : ((type === 'IRCommand') ? 'Command' : (type || 'Command'));
    const isDelay = (normType === 'Delay');
    const isSwitched = (normType === 'Switched');

    const devices = this.inventory.devices || [];
    const initDevId = initialData?.deviceId ? String(initialData.deviceId) : (devices[0]?.id || '');
    const initDelay = initialData?.delay !== undefined ? initialData.delay :
                      (initialData?.Delay !== undefined ? initialData.Delay :
                      (initialData?.Duration !== undefined ? initialData.Duration :
                      (initialData?.delayMs !== undefined ? initialData.delayMs : (isDelay ? 1000 : 0))));

    row.innerHTML = `
      <div style="display: flex; flex-wrap: wrap; align-items: center; gap: 6px; flex: 1;">
        <select class="form-control step-type-select" style="width: 100px; font-size: 12px; padding: 4px 6px;">
          <option value="Command" ${(!isDelay && !isSwitched) ? 'selected' : ''}>Command</option>
          <option value="Delay" ${isDelay ? 'selected' : ''}>Delay</option>
          <option value="Switched" ${isSwitched ? 'selected' : ''}>Switched</option>
        </select>
        <select class="form-control step-dev-select" style="max-width: 130px; font-size: 12px; padding: 4px 6px; display: ${(!isDelay && !isSwitched) ? 'block' : 'none'};">
          ${devices.map(d => `<option value="${d.id}" ${String(d.id) === initDevId ? 'selected' : ''}>${d.name || d.model || d.id}</option>`).join('')}
        </select>
        <span class="step-dev-icon" style="display: ${(!isDelay && !isSwitched) ? 'inline-flex' : 'none'}; align-items: center;"></span>
        <select class="form-control step-cmd-select" style="flex: 1; min-width: 120px; font-size: 12px; padding: 4px 6px; display: ${(!isDelay && !isSwitched) ? 'block' : 'none'};"></select>
        <div class="step-delay-wrap" style="display: ${isDelay ? 'inline-flex' : 'none'}; align-items: center; gap: 4px;">
          <span class="muted mini" style="font-size: 11px;">Wait:</span>
          <input type="number" class="form-control step-delay-input" min="50" max="60000" step="50" value="${initDelay > 0 ? initDelay : 1000}" style="width: 75px; font-size: 12px; padding: 4px 6px;">
          <span class="muted mini" style="font-size: 11px;">ms</span>
        </div>
        <div class="step-switched-wrap" style="display: ${isSwitched ? 'inline-flex' : 'none'}; align-items: center;">
          <span class="badge subtle mini" style="font-size: 11px; color: var(--accent);">[Activity Switched - Screen Updates Early]</span>
        </div>
      </div>
      <div style="display: flex; gap: 3px; align-items: center;">
        <button type="button" class="btn btn-xs btn-ghost btn-step-up" title="Move up">↑</button>
        <button type="button" class="btn btn-xs btn-ghost btn-step-down" title="Move down">↓</button>
        <button type="button" class="btn btn-xs btn-danger btn-step-del" title="Delete">✕</button>
      </div>
    `;

    const typeSel = row.querySelector('.step-type-select');
    const devSel = row.querySelector('.step-dev-select');
    const cmdSel = row.querySelector('.step-cmd-select');
    const devIconSpan = row.querySelector('.step-dev-icon');
    const delayWrap = row.querySelector('.step-delay-wrap');
    const delayInp = row.querySelector('.step-delay-input');
    const switchedWrap = row.querySelector('.step-switched-wrap');

    const updateCmdsForDev = () => {
      const selectedId = devSel.value;
      const curD = devices.find(x => String(x.id) === String(selectedId));
      if (devIconSpan) {
        devIconSpan.innerHTML = curD ? getDeviceIconSvg(curD.type, curD.manufacturer, curD.model, curD.icon || curD.Icon || '', 14) : '';
      }
      const cmds = this.deviceCommandsMap[selectedId] || [];
      cmdSel.innerHTML = '';
      if (!cmds.length) {
        cmdSel.innerHTML = '<option value="">(No commands)</option>';
      } else {
        cmds.forEach(c => {
          const cName = c.name || c;
          const opt = document.createElement('option');
          opt.value = cName;
          opt.textContent = cName;
          if (cName === initCmd) opt.selected = true;
          cmdSel.appendChild(opt);
        });
      }
      if (initCmd && !Array.from(cmdSel.options).some(o => o.value === initCmd)) {
        const customOpt = document.createElement('option');
        customOpt.value = initCmd;
        customOpt.textContent = initCmd;
        customOpt.selected = true;
        cmdSel.prepend(customOpt);
      }
    };

    typeSel.addEventListener('change', () => {
      const val = typeSel.value;
      const isD = (val === 'Delay');
      const isS = (val === 'Switched');
      devSel.style.display = (!isD && !isS) ? 'block' : 'none';
      if (devIconSpan) devIconSpan.style.display = (!isD && !isS) ? 'inline-flex' : 'none';
      cmdSel.style.display = (!isD && !isS) ? 'block' : 'none';
      delayWrap.style.display = isD ? 'inline-flex' : 'none';
      switchedWrap.style.display = isS ? 'inline-flex' : 'none';
      if (isD && delayInp && parseInt(delayInp.value, 10) <= 0) {
        delayInp.value = '1000';
      }
    });

    devSel.addEventListener('change', () => {
      updateCmdsForDev();
    });

    updateCmdsForDev();

    row.querySelector('.btn-step-up').addEventListener('click', () => {
      if (row.previousElementSibling) cont.insertBefore(row, row.previousElementSibling);
    });
    row.querySelector('.btn-step-down').addEventListener('click', () => {
      if (row.nextElementSibling) cont.insertBefore(row.nextElementSibling, row);
    });
    row.querySelector('.btn-step-del').addEventListener('click', () => {
      row.remove();
      if (!cont.children.length) cont.innerHTML = '<div class="muted mini" style="padding: 8px;">No steps configured.</div>';
    });

    cont.appendChild(row);
  }

  serializeSequence(containerId) {
    const cont = document.getElementById(containerId);
    if (!cont) return [];
    const steps = [];
    cont.querySelectorAll('.queue-row').forEach(row => {
      const typeSel = row.querySelector('.step-type-select');
      const devSel = row.querySelector('.step-dev-select');
      const cmdSel = row.querySelector('.step-cmd-select');
      const delayInp = row.querySelector('.step-delay-input');

      const val = typeSel ? typeSel.value : 'Command';
      if (val === 'Switched') {
        steps.push({
          type: 'Switched',
          deviceId: '0',
          command: 'Switched',
          delay: 0
        });
      } else if (val === 'Delay') {
        const delay = parseInt(delayInp?.value, 10) || 1000;
        steps.push({
          type: 'Delay',
          deviceId: '0',
          command: '',
          delay: delay > 0 ? delay : 1000
        });
      } else {
        const did = devSel?.value || '';
        const cmd = cmdSel?.value || '';
        if (did && cmd) {
          steps.push({
            type: 'Command',
            deviceId: String(did),
            command: String(cmd),
            delay: 0
          });
        }
      }
    });
    return steps;
  }

  serializeSequenceToString(containerId) {
    const steps = this.serializeSequence(containerId);
    return steps.map(s => {
      if (s.type === 'Switched') return 'Switched|0|Switched|0';
      if (s.type === 'Delay') return `Delay|||${s.delay}`;
      return `Command|${s.deviceId}|${s.command}|0`;
    }).join('\n');
  }

  async testLiveSequence(containerId) {
    const steps = this.serializeSequence(containerId);
    if (!steps.length) return alert(t("no_steps_to_test"));
    try {
      await this.client.testActivitySequence(steps);
      alert(t("seq_test_sent"));
    } catch (err) {
      alert('Test failed: ' + err.message);
    }
  }

  async saveCurrentActivity() {
    const id = document.getElementById('actEditId')?.value;
    const name = document.getElementById('actEditName')?.value.trim();
    const type = document.getElementById('actEditType')?.value;
    const order = Number(document.getElementById('actEditOrder')?.value) || 1;
    const statusEl = document.getElementById('actEditorStatus');

    if (!name) return alert(t("act_name_required"));

    const listWrap = document.getElementById('actDevOrderedList');
    const deviceIds = listWrap
      ? Array.from(listWrap.querySelectorAll('.act-dev-row')).map(r => r.dataset.devId).filter(Boolean)
      : [];

    const startSeq = this.serializeSequence('actStartStepsList');
    const stopSeq = this.serializeSequence('actStopStepsList');
    const startSteps = this.serializeSequenceToString('actStartStepsList');
    const stopSteps = this.serializeSequenceToString('actStopStepsList');

    const icon = document.getElementById('actEditIcon')?.value || 'generic.png';

    const actObj = {
      id: id || '',
      name,
      type: type || 'Custom',
      icon,
      order: order,
      deviceIds,
      startSequence: startSeq,
      stopSequence: stopSeq,
      startSteps,
      stopSteps
    };

    try {
      if (statusEl) statusEl.textContent = 'Saving activity...';
      await this.client.saveActivity(actObj);
      if (statusEl) statusEl.textContent = 'Saved successfully!';
      document.getElementById('actEditorBox').style.display = 'none';
      await this.refreshData();
      await this.loadActivitiesView();
    } catch (err) {
      if (statusEl) statusEl.textContent = 'Error: ' + err.message;
    }
  }

  async deleteCurrentActivity() {
    const id = document.getElementById('actEditId')?.value;
    if (!id || String(id) === '-1') return alert(t("cannot_delete_act"));
    if (!await showConfirm('Are you sure you want to delete this activity?', { title: 'Delete Activity', okText: 'Delete' })) return;
    try {
      await this.client.deleteActivity(id);
      document.getElementById('actEditorBox').style.display = 'none';
      await this.refreshData();
      await this.loadActivitiesView();
    } catch (err) {
      alert('Delete failed: ' + err.message);
    }
  }

}
