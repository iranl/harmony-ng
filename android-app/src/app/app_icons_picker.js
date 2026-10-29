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
import { HarmonyAppCore } from './app_core.js';

const iconNameCache = new Map();

export class HarmonyAppIconPicker extends HarmonyAppCore {
  async loadAvailableIcons(force = false) {
    if (!force && this.availableIcons && this.availableIcons.length > 0) return;
    try {
      let icons = [];
      let customIcons = [];
      try {
        [icons, customIcons] = await Promise.all([
          this.client.getIcons().catch(() => []),
          this.client.getCustomIcons().catch(() => [])
        ]);
      } catch (e) {
        console.warn('Could not query icons from Hub, checking local cache:', e);
      }
      if ((!icons || icons.length === 0) && getCachedIconsCount() > 0) {
        icons = getAllCachedKeys();
      }

      const customMap = new Map();
      (customIcons || []).forEach(ci => {
        if (ci && ci.filename) {
          customMap.set(ci.filename.toLowerCase(), ci);
          const noExt = ci.filename.replace(/\.png$/i, '').toLowerCase();
          customMap.set(noExt, ci);
        }
      });
      window.__harmonyCustomIcons = new Set(customMap.keys());
      this.customIconMap = customMap;

      // Merge stock icons and custom icons, ensuring custom icons are present
      const allIconsSet = new Set(icons || []);
      (customIcons || []).forEach(ci => {
        if (ci && ci.filename) allIconsSet.add(ci.filename);
      });

      const allIconsList = Array.from(allIconsSet);
      if (allIconsList.length > 0) {
        this.availableIcons = allIconsList;
        this.iconCatalog = allIconsList.map(fn => {
          const cleanFn = String(fn).split('/').pop().trim();
          const cleanLower = cleanFn.toLowerCase();
          const meta = iconMetadata[cleanFn] || iconMetadata[`${cleanFn}.png`] || {};
          const isCust = customMap.has(cleanLower) || cleanLower.startsWith('custom_');
          const title = isCust
            ? (cleanFn.replace(/^custom_/i, '').replace(/\.png$/i, '').replace(/[-_]/g, ' '))
            : this.formatIconDisplayName(cleanFn);
          const category = isCust ? 'custom' : (meta.category || 'other');
          return {
            fn: cleanFn,
            title: title.charAt(0).toUpperCase() + title.slice(1),
            lowerTitle: title.toLowerCase(),
            lowerFn: cleanLower,
            category,
            isCustom: isCust,
            tags: ((meta.tags || []).join(' ') + (isCust ? ' custom user' : '')).toLowerCase(),
            url: getStockIconUrl(cleanFn)
          };
        }).sort((a, b) => {
          if (a.isCustom !== b.isCustom) return a.isCustom ? -1 : 1;
          return a.title.localeCompare(b.title, undefined, { sensitivity: 'base' });
        });

        this.populateIconSelects();
      }
    } catch (e) {
      console.warn('Could not load remote icons list:', e);
    }
  }

  populateIconSelects() {
    if (!this.iconCatalog || this.iconCatalog.length === 0) return;
    const selects = [
      { id: 'actEditIcon', badgeId: 'actIconPreviewBadge' },
      { id: 'editDevIcon', badgeId: 'devIconPreviewBadge' },
      { id: 'newDeviceIcon', badgeId: 'newDevIconPreviewBadge' }
    ];

    selects.forEach(({ id, badgeId }) => {
      const sel = document.getElementById(id);
      if (!sel) return;
      const currentVal = sel.value;
      const existingVals = new Set(Array.from(sel.options).map(o => o.value));

      const frag = document.createDocumentFragment();
      this.iconCatalog.forEach(item => {
        const val = item.fn;
        const valNoExt = val.replace(/\.png$/i, '');
        if (!existingVals.has(val) && !existingVals.has(valNoExt)) {
          const opt = document.createElement('option');
          opt.value = val;
          opt.textContent = item.title;
          frag.appendChild(opt);
          existingVals.add(val);
        }
      });
      sel.appendChild(frag);

      if (currentVal && existingVals.has(currentVal)) {
        sel.value = currentVal;
      }
      this.updateBadgeForSelect(sel, badgeId);
    });
  }

  formatIconDisplayName(fn) {
    if (!fn) return '';
    if (iconNameCache.has(fn)) return iconNameCache.get(fn);
    const cleanFn = String(fn).split('/').pop().trim();
    if (iconNameCache.has(cleanFn)) return iconNameCache.get(cleanFn);
    const withExt = cleanFn.endsWith('.png') ? cleanFn : `${cleanFn}.png`;
    const noExt = cleanFn.replace(/\.png$/i, '');

    let res = '';
    if (iconMetadata[withExt]?.name) res = iconMetadata[withExt].name;
    else if (iconMetadata[cleanFn]?.name) res = iconMetadata[cleanFn].name;
    else {
      const base = noExt.replace(/_(small|smallon|on|off)$/i, '');
      if (iconMetadata[`${base}.png`]?.name) {
        const baseName = iconMetadata[`${base}.png`].name;
        if (noExt.endsWith('_on') || noExt.endsWith('_smallon')) res = `${baseName} (Active)`;
        else if (noExt.endsWith('_off')) res = `${baseName} (Inactive)`;
        else res = baseName;
      } else if (iconMetadata[`${base}_small.png`]?.name) {
        const baseName = iconMetadata[`${base}_small.png`].name;
        if (noExt.endsWith('_on') || noExt.endsWith('_smallon')) res = `${baseName} (Active)`;
        else if (noExt.endsWith('_off')) res = `${baseName} (Inactive)`;
        else res = baseName;
      } else {
        const words = noExt.replace(/[-_]/g, ' ').split(/\s+/);
        res = words.map(w => w.charAt(0).toUpperCase() + w.slice(1)).join(' ');
      }
    }
    iconNameCache.set(fn, res);
    iconNameCache.set(cleanFn, res);
    return res;
  }

  updateBadgeForSelect(sel, badgeId) {
    const badge = document.getElementById(badgeId);
    if (!badge || !sel) return;
    const val = sel.value;
    const url = getStockIconUrl(val);
    if (url) {
      const shouldInvert = isDarkMonochromeIcon(val);
      badge.innerHTML = `<img src="${url}" data-icon-key="${val}" class="stock-icon-img ${shouldInvert ? 'invert-white' : ''}" loading="lazy" decoding="async" style="width: 22px; height: 22px;" onload="if(window.__harmonyAutoInvert)window.__harmonyAutoInvert(this)" onerror="this.style.display='none'" />`;
    } else {
      badge.innerHTML = `<span style="font-size: 14px; opacity: 0.5;">🖼</span>`;
    }
  }

  openIconPicker(targetSelectIdOrCallback, previewBadgeId = null, onSelect = null, context = null) {
    if (typeof targetSelectIdOrCallback === 'function') {
      this.iconPickerCallback = targetSelectIdOrCallback;
      this.currentIconPickerTarget = null;
      this.currentIconPickerBadge = null;
      this.iconPickerCurrentVal = previewBadgeId || '';
      this.iconPickerContext = context || null;
    } else if (typeof onSelect === 'function') {
      this.currentIconPickerTarget = targetSelectIdOrCallback;
      this.currentIconPickerBadge = previewBadgeId;
      this.iconPickerCallback = onSelect;
      this.iconPickerCurrentVal = document.getElementById(targetSelectIdOrCallback)?.value || '';
      this.iconPickerContext = context || null;
    } else {
      this.currentIconPickerTarget = targetSelectIdOrCallback;
      this.currentIconPickerBadge = previewBadgeId;
      this.iconPickerCallback = null;
      this.iconPickerCurrentVal = document.getElementById(targetSelectIdOrCallback)?.value || '';
      this.iconPickerContext = context || null;
    }
    const modal = document.getElementById('iconPickerModal');
    if (!modal) return;

    // Reset upload preview row
    const prevRow = document.getElementById('customIconPreviewRow');
    if (prevRow) prevRow.style.display = 'none';
    const urlInp = document.getElementById('customIconUrlInput');
    if (urlInp) urlInp.value = '';
    const fileInp = document.getElementById('customIconFileInput');
    if (fileInp) fileInp.value = '';
    const nameInp = document.getElementById('customIconNameInput');
    if (nameInp) nameInp.value = '';
    const nameStatus = document.getElementById('customIconNameStatus');
    if (nameStatus) nameStatus.innerHTML = '';
    const invertCheck = document.getElementById('customIconInvertCheck');
    if (invertCheck) invertCheck.checked = false;

    const curVal = this.iconPickerCurrentVal || '';
    const subtitle = document.getElementById('iconPickerSubtitle');
    const totalIcons = (this.iconCatalog && this.iconCatalog.length) || (this.availableIcons && this.availableIcons.length) || 0;
    if (subtitle) {
      subtitle.textContent = totalIcons > 0
        ? `${totalIcons} icons available from Hub & Remote`
        : 'Choose an icon from remote assets';
    }
    const selNameEl = document.getElementById('iconPickerSelectedName');
    if (selNameEl) selNameEl.textContent = curVal ? `Current: ${this.formatIconDisplayName(curVal)}` : 'No icon selected';
    const sInput = document.getElementById('iconPickerSearch');
    if (sInput) sInput.value = '';
    this.currentIconCategory = 'all';

    document.querySelectorAll('.icon-cat-pill').forEach(p => {
      p.classList.toggle('btn-primary', p.getAttribute('data-cat') === 'all');
      p.classList.toggle('btn-ghost', p.getAttribute('data-cat') !== 'all');
    });

    const grid = document.getElementById('iconPickerGrid');
    if (grid) grid.scrollTop = 0;
    this.renderIconPickerGrid('');
    modal.style.display = 'flex';
  }

  renderIconPickerGrid(searchTerm = '') {
    const grid = document.getElementById('iconPickerGrid');
    if (!grid) return;

    if (!this.iconCatalog || this.iconCatalog.length === 0) {
      const iconList = this.availableIcons && this.availableIcons.length > 0
        ? this.availableIcons
        : Array.from(document.getElementById('actEditIcon')?.options || []).map(o => o.value);
      this.iconCatalog = iconList.map(fn => {
        const cleanFn = String(fn).split('/').pop().trim();
        const meta = iconMetadata[cleanFn] || iconMetadata[`${cleanFn}.png`] || {};
        const isCust = isCustomIcon(cleanFn);
        const title = isCust
          ? (cleanFn.replace(/^custom_/i, '').replace(/\.png$/i, '').replace(/[-_]/g, ' '))
          : this.formatIconDisplayName(cleanFn);
        const category = isCust ? 'custom' : (meta.category || (cleanFn.startsWith('button_') ? 'commands' : 'other'));
        return {
          fn: cleanFn,
          title: title.charAt(0).toUpperCase() + title.slice(1),
          lowerTitle: title.toLowerCase(),
          lowerFn: cleanFn.toLowerCase(),
          category,
          isCustom: isCust,
          tags: ((meta.tags || []).join(' ') + (isCust ? ' custom user' : '')).toLowerCase(),
          url: getStockIconUrl(cleanFn)
        };
      }).sort((a, b) => {
        if (a.isCustom !== b.isCustom) return a.isCustom ? -1 : 1;
        return a.title.localeCompare(b.title, undefined, { sensitivity: 'base' });
      });
    }

    const s = (searchTerm || '').toLowerCase().trim();
    const cat = this.currentIconCategory || 'all';

    this.currentFilteredIcons = this.iconCatalog.filter(item => {
      if (cat !== 'all') {
        if (cat === 'custom') {
          if (!item.isCustom) return false;
        } else {
          if (item.isCustom) return false;
          if (cat === 'activity' && item.category !== 'activity' && !item.fn.startsWith('activity_')) return false;
          if (cat === 'device' && item.category !== 'device' && !item.fn.startsWith('device_')) return false;
          if (cat === 'home' && item.category !== 'home') return false;
          if (cat === 'commands' && item.category !== 'commands' && !item.fn.startsWith('button_')) return false;
          if (cat === 'settings' && (item.category !== 'ui' && item.category !== 'other')) return false;
        }
      }
      if (s) {
        return item.lowerTitle.includes(s) || item.lowerFn.includes(s) || item.tags.includes(s);
      }
      return true;
    });

    if (this.currentFilteredIcons.length === 0) {
      grid.innerHTML = `<div class="muted mini" style="grid-column: 1 / -1; text-align: center; padding: 24px;">No matching icons found for "${searchTerm}"</div>`;
      return;
    }

    grid.innerHTML = '';
    this.iconRenderIndex = 0;
    this.appendNextIconBatch();

    // Attach scroll listener once
    if (!this._iconGridScrollBound) {
      this._iconGridScrollBound = true;
      grid.addEventListener('scroll', () => {
        if (grid.scrollTop + grid.clientHeight >= grid.scrollHeight - 160) {
          this.appendNextIconBatch();
        }
      }, { passive: true });
    }

    // Attach delegated click listener once
    if (!this._iconGridClickBound) {
      this._iconGridClickBound = true;
      grid.addEventListener('click', async (e) => {
        const delBtn = e.target.closest('.btn-delete-custom-icon');
        if (delBtn) {
          e.stopPropagation();
          const fnToDelete = delBtn.getAttribute('data-fn');
          if (confirm(t("del_custom_icon_confirm").replace("{name}", fnToDelete))) {
            try {
              await this.client.deleteCustomIcon(fnToDelete);
              showToast(t("del_custom_icon_success").replace("{name}", fnToDelete), 'info');
              await this.loadAvailableIcons(true);
              this.renderIconPickerGrid(document.getElementById('iconPickerSearch')?.value || '');
            } catch (err) {
              showToast(t("del_custom_icon_failed") + err.message, 'error');
            }
          }
          return;
        }

        const card = e.target.closest('.act-icon-card');
        if (!card) return;
        const iconVal = card.getAttribute('data-icon');
        if (!iconVal) return;

        if (typeof this.iconPickerCallback === 'function') {
          this.iconPickerCallback(iconVal);
        } else if (this.currentIconPickerTarget) {
          const targetSel = document.getElementById(this.currentIconPickerTarget);
          if (targetSel) {
            let opt = Array.from(targetSel.options).find(o => o.value === iconVal);
            if (!opt) {
              opt = document.createElement('option');
              opt.value = iconVal;
              opt.textContent = this.formatIconDisplayName(iconVal);
              targetSel.appendChild(opt);
            }
            targetSel.value = iconVal;
            targetSel.dispatchEvent(new Event('change'));
            this.updateBadgeForSelect(targetSel, this.currentIconPickerBadge);
          }
        }
        document.getElementById('iconPickerModal').style.display = 'none';
        showToast(`Selected icon "${this.formatIconDisplayName(iconVal)}"`, 'info', 1800);
      });
    }
  }

  appendNextIconBatch() {
    const grid = document.getElementById('iconPickerGrid');
    if (!grid || !this.currentFilteredIcons || this.iconRenderIndex >= this.currentFilteredIcons.length) return;

    const batch = this.currentFilteredIcons.slice(this.iconRenderIndex, this.iconRenderIndex + 48);
    this.iconRenderIndex += batch.length;

    const curVal = this.iconPickerCurrentVal || document.getElementById(this.currentIconPickerTarget)?.value || '';

    const html = batch.map(item => {
      const isSel = (item.fn === curVal || item.fn.replace(/\.png$/i, '') === curVal);
      const isCustom = item.isCustom;
      const shouldInvert = isDarkMonochromeIcon(item.fn);
      return `
        <div class="act-icon-card ${isSel ? 'selected' : ''}" data-icon="${item.fn}" style="position: relative;">
          ${isCustom ? `<button type="button" class="btn-delete-custom-icon" data-fn="${item.fn}" title="Delete custom icon" style="position: absolute; top: 3px; right: 3px; background: rgba(220,38,38,0.85); color: #fff; border: none; border-radius: 50%; width: 18px; height: 18px; font-size: 11px; line-height: 1; cursor: pointer; display: flex; align-items: center; justify-content: center; z-index: 2;">&times;</button>` : ''}
          <div class="icon-preview">
            <img src="${item.url}" data-icon-key="${item.fn}" class="stock-icon-img ${shouldInvert ? 'invert-white' : ''}" loading="lazy" decoding="async" style="width: 26px; height: 26px;" onload="if(window.__harmonyAutoInvert)window.__harmonyAutoInvert(this)" onerror="this.src='/favicon.png'" />
          </div>
          <div class="icon-title" title="${item.title}">${item.title}</div>
          <div class="icon-filename">${item.fn}</div>
        </div>
      `;
    }).join('');

    grid.insertAdjacentHTML('beforeend', html);
  }

  openBtDelayCalibrator(devId) {
    const dev = (this.inventory?.devices || []).find(d => String(d.id) === String(devId));
    if (!dev) return;

    const modal = document.getElementById('btDelayCalibrateModal');
    if (!modal) return;

    document.getElementById('btCalibrateDevId').value = dev.id;
    document.getElementById('btCalibrateDevName').textContent = dev.name || 'Bluetooth Device';
    document.getElementById('btCalibrateDevHost').textContent = `Host BD_ADDR: ${dev.btHost || dev.bt_host || dev.BTAddress || 'Not linked'}`;
    document.getElementById('btCalibrateCurrentStatus').textContent = 'Status: Checking...';
    document.getElementById('btCalibrateTimer').textContent = '0.0s';
    document.getElementById('btCalibrateTimer').classList.remove('bt-timer-active');
    document.getElementById('btCalibrateStateMsg').textContent = 'Ready to start timing test';
    document.getElementById('btCalibrateResultBox').style.display = 'none';

    const cmdSel = document.getElementById('btCalibratePowerCmd');
    if (cmdSel) {
      cmdSel.innerHTML = '';
      const cmds = this.deviceCommandsMap[dev.id] || dev.commands || [];
      const powerCmds = cmds.filter(c => /power|toggle|on|wake|home|menu/i.test(c.name || ''));
      const otherCmds = cmds.filter(c => !powerCmds.includes(c));
      const allCmds = [...powerCmds, ...otherCmds];

      if (allCmds.length === 0) {
        cmdSel.innerHTML = '<option value="">(No commands available - manual power)</option>';
      } else {
        allCmds.forEach(c => {
          const opt = document.createElement('option');
          opt.value = c.name;
          opt.textContent = `${c.name} (${c.protocol || 'IR'})`;
          if (/poweron|powertoggle|power/i.test(c.name) && !cmdSel.value) {
            opt.selected = true;
          }
          cmdSel.appendChild(opt);
        });
      }
    }

    this.client.getBtStatus().then(st => {
      const isConn = this.isDevBtConnected(dev, st);
      const statEl = document.getElementById('btCalibrateCurrentStatus');
      if (statEl) {
        statEl.innerHTML = isConn
          ? '<span style="color:var(--success)">● Currently Connected to Hub</span>'
          : '<span style="color:var(--text-muted)">○ Currently Disconnected</span>';
      }
    }).catch(() => { });

    modal.style.display = 'flex';
  }

  isDevBtConnected(dev, btStatus) {
    if (!btStatus) return false;
    const targetAddr = (dev.btHost || dev.bt_host || dev.BTAddress || '').toLowerCase();
    if (btStatus.hosts && Array.isArray(btStatus.hosts)) {
      const match = btStatus.hosts.find(h => targetAddr && h.addr && h.addr.toLowerCase() === targetAddr);
      if (match && match.connected) return true;
    }
    if (btStatus.host && btStatus.host.connected) {
      if (!targetAddr || (btStatus.host.addr && btStatus.host.addr.toLowerCase() === targetAddr)) {
        return true;
      }
    }
    return false;
  }

  async startBtDelayCalibration(devId) {
    const dev = (this.inventory?.devices || []).find(d => String(d.id) === String(devId));
    if (!dev) return;

    const timerEl = document.getElementById('btCalibrateTimer');
    const msgEl = document.getElementById('btCalibrateStateMsg');
    const startBtn = document.getElementById('btnBtCalibrateStart');
    const resBox = document.getElementById('btCalibrateResultBox');
    const cmdName = document.getElementById('btCalibratePowerCmd')?.value;

    if (this._btCalibrateInterval) clearInterval(this._btCalibrateInterval);
    if (this._btCalibratePoll) clearInterval(this._btCalibratePoll);

    resBox.style.display = 'none';
    timerEl.classList.add('bt-timer-active');
    startBtn.disabled = true;
    startBtn.textContent = 'Detecting...';

    if (cmdName) {
      msgEl.textContent = `Triggering power command "${cmdName}"...`;
      try {
        await this.client.sendDeviceCommand(dev.id, cmdName);
      } catch (e) {
        console.warn('Power command send error:', e);
      }
    }

    msgEl.textContent = 'Waiting for Bluetooth connection to complete...';
    const startTime = Date.now();
    timerEl.textContent = '0.0s';

    this._btCalibrateInterval = setInterval(() => {
      const elapsed = ((Date.now() - startTime) / 1000).toFixed(1);
      timerEl.textContent = `${elapsed}s`;
    }, 100);

    let attempts = 0;
    this._btCalibratePoll = setInterval(async () => {
      attempts++;
      if (attempts > 300) {
        clearInterval(this._btCalibrateInterval);
        clearInterval(this._btCalibratePoll);
        timerEl.classList.remove('bt-timer-active');
        startBtn.disabled = false;
        startBtn.textContent = 'Start Detection Test';
        msgEl.innerHTML = '<span style="color:var(--danger)">Connection timed out (60s). Check if device is powered on.</span>';
        showToast(t("bt_detect_timeout"), 'warning');
        return;
      }

      try {
        const st = await this.client.getBtStatus();
        if (this.isDevBtConnected(dev, st)) {
          const elapsedMs = Date.now() - startTime;
          const elapsedSec = (elapsedMs / 1000).toFixed(1);
          clearInterval(this._btCalibrateInterval);
          clearInterval(this._btCalibratePoll);
          timerEl.classList.remove('bt-timer-active');
          timerEl.textContent = `${elapsedSec}s`;
          startBtn.disabled = false;
          startBtn.textContent = 'Start Detection Test';

          msgEl.innerHTML = `<span style="color:var(--success)">✓ Connected in ${elapsedSec}s!</span>`;

          const suggested = Math.max(1, Math.ceil(parseFloat(elapsedSec)) + 1);

          document.getElementById('btCalibrateResultMsg').textContent = `Connected in ${elapsedSec} seconds!`;
          document.getElementById('btCalibrateResultDesc').textContent = `Suggested startup delay: ${suggested} seconds (includes 1s safety margin for HID handshake).`;
          resBox.style.display = 'block';

          this._lastSuggestedBtDelay = suggested;
          this._lastCalibratedDevId = dev.id;

          showToast(`Bluetooth connected in ${elapsedSec}s! Suggested delay: ${suggested}s.`, 'success');
        }
      } catch (err) { }
    }, 200);
  }

  async applyCalibratedBtDelay() {
    if (!this._lastCalibratedDevId || typeof this._lastSuggestedBtDelay !== 'number') return;
    const devId = this._lastCalibratedDevId;
    const delay = this._lastSuggestedBtDelay;

    const dlyInp = document.getElementById('curDevBtDelay');
    if (dlyInp) dlyInp.value = delay;

    try {
      await this.client.saveDeviceDetails({
        deviceId: devId,
        btReconnectDelaySec: delay
      });
      showToast(`Saved ${delay}s Bluetooth delay override for this device!`, 'success');
      document.getElementById('btDelayCalibrateModal').style.display = 'none';
      await this.refreshData();
    } catch (e) {
      showToast(t("bt_delay_fail") + e.message, 'error');
    }
  }

}
