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
import { HarmonyAppLab } from './app_lab.js';

export class HarmonyAppBluetooth extends HarmonyAppLab {
  async loadBtView() {
    const badge = document.getElementById('btStatusBadge');
    const desc = document.getElementById('btHostDesc');
    const list = document.getElementById('btDevicesList');
    const hostSel = document.getElementById('btTargetHostSelect');
    const saveSel = document.getElementById('btSaveDeviceSelect');
    const pairTarget = document.getElementById('btPairTargetDev');

    const pidEl = document.getElementById('btStatusPid');
    const backendEl = document.getElementById('btStatusBackend');
    const targetNameEl = document.getElementById('btStatusTargetName');
    const targetAddrEl = document.getElementById('btStatusTargetAddr');
    const hostStateEl = document.getElementById('btStatusHostState');
    const hostDetailEl = document.getElementById('btStatusHostDetail');
    const remoteStateEl = document.getElementById('btStatusRemoteState');
    const remoteAddrEl = document.getElementById('btStatusRemoteAddr');

    if (pairTarget) {
      const devs = this.inventory.devices || [];
      pairTarget.innerHTML = '<option value="">-- Select Target Device (Required) --</option>' +
        devs.map(d => {
          const isBt = d.type === 'BT' || d.type === 'Bluetooth' || d.isBt || Number(d.transport) === 32;
          return `<option value="${d.id}">${d.name || d.id}${isBt ? ' (BT)' : ''}</option>`;
        }).join('');
    }
    if (saveSel) {
      saveSel.innerHTML = (this.inventory.devices || []).map(d => `<option value="${d.id}">${d.name || d.id}</option>`).join('');
    }

    try {
      const st = await this.client.getBtStatus();
      const isRunning = Boolean(st && (st.pid || st.running || st.backend === 'btstack'));

      if (badge) {
        badge.className = `badge ${isRunning ? 'ok' : 'bad'}`;
        badge.textContent = isRunning ? 'BTstack Running' : 'Stopped';
      }
      if (desc) desc.textContent = isRunning ? 'BTstack daemon active (UART hci0) • HID ready' : 'BTstack daemon stopped';

      if (pidEl) pidEl.textContent = isRunning ? `Running (PID ${st.pid || 'Active'})` : 'Stopped';
      if (backendEl) backendEl.textContent = st.backend ? `Backend: ${st.backend}` : 'Not Running';

      // Host connection
      const host = st.host || {};
      const isHostConn = Boolean(host.connected);

      const findAssignedDevice = (h) => {
        if (!h || !h.addr) return null;
        return (this.inventory.devices || []).find(d => {
          const a = (d.bt_host || d.BTAddress || d.btHost || d.bdaddr || '').toLowerCase();
          return a && a === h.addr.toLowerCase();
        });
      };

      const resolveHostDeviceName = (h) => {
        if (!h) return 'Bluetooth Host';
        const assigned = findAssignedDevice(h);
        if (assigned) return assigned.name || assigned.model || h.name;
        const matchDev = (this.inventory.devices || []).find(d => {
          if (h.name && (d.name === h.name || d.id === h.name)) return true;
          return false;
        });
        if (matchDev) return matchDev.name || matchDev.model || h.name;
        if (!h.name || h.name === 'Default Host' || h.name.startsWith('Host 00:')) {
          const btDev = (this.inventory.devices || []).find(d => Number(d.transport) === 32 || (d.type && d.type.toLowerCase().includes('bt')));
          if (btDev) return btDev.name || btDev.model;
        }
        return h.name || 'Bluetooth Host';
      };

      if (targetNameEl) {
        if (isHostConn) {
          targetNameEl.textContent = resolveHostDeviceName(host);
          targetNameEl.style.color = 'var(--accent)';
        } else {
          targetNameEl.textContent = 'None (Disconnected)';
          targetNameEl.style.color = 'var(--text-dim)';
        }
      }
      if (targetAddrEl) {
        targetAddrEl.textContent = isHostConn ? (host.addr || 'Connected') : (host.addr && host.addr !== '00:00:00:00:00:00' ? `Target: ${host.addr}` : 'No active connection');
      }
      if (hostStateEl) {
        hostStateEl.textContent = isHostConn ? 'CONNECTED' : (host.pairing ? 'PAIRING...' : 'DISCONNECTED');
        hostStateEl.style.color = isHostConn ? 'var(--success)' : (host.pairing ? 'var(--warning)' : 'var(--text-dim)');
      }
      const pairToggleBtn = document.getElementById('btPairToggleBtn');
      if (pairToggleBtn) {
        if (host.pairing) {
          pairToggleBtn.textContent = 'Stop Pairing Mode';
          pairToggleBtn.classList.remove('btn-primary');
          pairToggleBtn.classList.add('btn-warning');
        } else {
          pairToggleBtn.textContent = 'Start Pairing Mode';
          pairToggleBtn.classList.remove('btn-warning');
          pairToggleBtn.classList.add('btn-primary');
        }
      }
      if (hostDetailEl) hostDetailEl.textContent = isHostConn ? 'BLE HID Link Active' : 'Standby / Idle';

      // Remote connection
      const rem = st.remote || {};
      const isRemConn = Boolean(rem.connected);
      if (remoteStateEl) {
        remoteStateEl.textContent = isRemConn ? 'CONNECTED' : 'DISCONNECTED';
        remoteStateEl.style.color = isRemConn ? 'var(--success)' : 'var(--text-dim)';
      }
      if (remoteAddrEl) remoteAddrEl.textContent = rem.addr && rem.addr !== '00:00:00:00:00:00' ? rem.addr : 'Homatics B25 / BLE Remote';

      // Paired hosts list
      const hosts = st.hosts || (st.host?.addr ? [st.host] : []);
      const devs = this.inventory.devices || [];
      if (list) {
        list.innerHTML = hosts.length ? hosts.map(h => {
          const assigned = findAssignedDevice(h);
          return `
          <div class="queue-row" style="display:flex; justify-content:space-between; align-items:center; padding:8px 10px; background:var(--bg-card); border:1px solid var(--border); border-radius:var(--radius-sm); margin-bottom:6px; flex-wrap:wrap; gap:8px;">
            <div style="min-width: 180px;">
              <strong>${resolveHostDeviceName(h)}</strong>
              <div class="muted mini">${h.addr} • <span style="color:${h.connected ? 'var(--success)' : 'var(--text-dim)'}">${h.connected ? 'Connected' : 'Paired'}</span></div>
            </div>
            <div style="display:flex; gap:6px; align-items:center; flex-wrap:wrap;">
              <select class="form-control form-control-sm host-assign-sel" data-addr="${h.addr}" style="font-size:11px; max-width:180px;">
                <option value="">-- Assign Device --</option>
                ${devs.map(d => `<option value="${d.id}" ${assigned && String(assigned.id) === String(d.id) ? 'selected' : ''}>${d.name || d.id}</option>`).join('')}
              </select>
              <button type="button" class="btn btn-xs btn-secondary btn-bt-disconn" data-addr="${h.addr}">${h.connected ? 'Disconnect' : 'Connect'}</button>
            </div>
          </div>
        `;
        }).join('') : '<div class="muted mini">No paired hosts registered yet.</div>';

        list.querySelectorAll('.host-assign-sel').forEach(selEl => {
          selEl.addEventListener('change', async () => {
            const devId = selEl.value;
            const addr = selEl.getAttribute('data-addr');
            if (devId && addr) {
              try {
                await this.client.linkBtDevice(addr, devId);
                await this.refreshData();
                this.loadBtView();
              } catch (err) {
                alert(t('assign_failed', 'Assign failed: ') + err.message);
              }
            }
          });
        });

        list.querySelectorAll('.btn-bt-disconn').forEach(b => {
          b.addEventListener('click', async () => {
            await this.client.disconnectBt(b.getAttribute('data-addr'));
            this.loadBtView();
          });
        });
      }

      if (hostSel) {
        hostSel.innerHTML = '<option value="">Auto (Active Host)</option>' +
          hosts.map(d => `<option value="${escapeHtml(d.addr)}">${escapeHtml(d.name || d.addr)}</option>`).join('');
      }
    } catch (err) {
      if (desc) desc.textContent = 'Error: ' + err.message;
    }
  }

  async toggleBtPairing() {
    const btn = document.getElementById('btPairToggleBtn');
    const name = document.getElementById('btPairName')?.value || 'Harmony Keyboard';
    const target = document.getElementById('btPairTargetDev')?.value || '';

    const isPairing = btn && btn.textContent.toLowerCase().includes('stop');
    if (isPairing) {
      try {
        if (btn) btn.textContent = 'Stopping pairing...';
        await this.client.setBtPairing(false);
        showToast(t("bt_pairing_stopped"), 'info');
      } catch (err) {
        alert(t('failed_stop_pairing', 'Failed to stop pairing: ') + err.message);
      } finally {
        await this.loadBtView();
      }
      return;
    }

    if (!target) {
      alert(t("select_target_bt_pairing"));
      return;
    }
    try {
      if (btn) btn.textContent = 'Entering pairing mode...';
      await this.client.setBtPairing(true, name, target);
      const devObj = (this.inventory.devices || []).find(d => String(d.id) === String(target));
      showToast('Hub is now discoverable as "' + name + '" for ' + (devObj ? devObj.name : target) + '. Pair from your host device.', 'success');
    } catch (err) {
      alert(t('pairing_mode_failed', 'Pairing mode failed: ') + err.message);
    } finally {
      await this.loadBtView();
    }
  }

  async sendBtDirectText() {
    const txt = document.getElementById('btDirectText')?.value;
    const stat = document.getElementById('btTextStatus');
    const target = (document.getElementById('btTargetHostSelect')?.value || '').trim();
    if (!txt) return;
    try {
      if (stat) stat.textContent = 'Sending text...';
      await this.client.sendBtText(txt, target);
      if (stat) stat.innerHTML = '<span style="color:var(--success)">Sent!</span>';
      this.appendBtLog(`Sent ${txt.length} chars text` + (target ? ' -> ' + target : ''));
    } catch (err) {
      if (stat) stat.innerHTML = `<span style="color:var(--danger)">Error: ${err.message}</span>`;
      this.appendBtLog(`Text send failed: ${err.message || err}`);
    }
  }

  appendBtLog(msg) {
    const el = document.getElementById('btLog');
    if (!el) return;
    const time = new Date().toLocaleTimeString();
    el.textContent = `[${time}] ${msg}\n` + (el.textContent === 'Ready.' ? '' : el.textContent.slice(0, 2000));
  }

  async runBtMacroScript() {
    const script = document.getElementById('btMacroScript')?.value;
    const delay = Number(document.getElementById('btScriptDelay')?.value) || 35;
    const stat = document.getElementById('btScriptStatus');
    if (!script) return;
    try {
      if (stat) stat.textContent = 'Running macro script...';
      await this.client.runBtScript(script, delay);
      if (stat) stat.innerHTML = '<span style="color:var(--success)">Script finished!</span>';
    } catch (err) {
      if (stat) stat.innerHTML = `<span style="color:var(--danger)">Failed: ${err.message}</span>`;
    }
  }

  async saveBtMacroCommand() {
    const devId = document.getElementById('btSaveDeviceSelect')?.value;
    const name = document.getElementById('btSaveCommandName')?.value.trim();
    const script = document.getElementById('btMacroScript')?.value.trim();
    const stat = document.getElementById('btSaveCommandStatus');
    if (!devId || !name || !script) return alert(t("dev_cmd_script_required"));
    try {
      await this.client.saveBtCommand(devId, name, script);
      if (stat) stat.innerHTML = '<span style="color:var(--success)">Saved!</span>';
      await this.refreshData();
    } catch (err) {
      if (stat) stat.innerHTML = `<span style="color:var(--danger)">Error: ${err.message}</span>`;
    }
  }

  /* -------------------------------------------------------------
   * 7b. Bluetooth Keyboard & Macros View
   * ------------------------------------------------------------- */
  async loadBtKeyboardView() {
    const hostSel = document.getElementById('btTargetHostSelect');
    const saveSel = document.getElementById('btSaveDeviceSelect');
    if (saveSel) {
      saveSel.innerHTML = (this.inventory.devices || []).map(d => `<option value="${d.id}">${d.name || d.id}</option>`).join('');
    }
    try {
      const st = await this.client.getBtStatus();
      const hosts = st?.hosts || (st?.host?.addr ? [st.host] : []);
      if (hostSel) {
        hostSel.innerHTML = '<option value="">Auto (Active Host)</option>' +
          hosts.map(d => `<option value="${d.addr}">${d.name || d.addr}</option>`).join('');
      }
    } catch { }
    this.renderBtSavedCommands();
  }

  renderBtSavedCommands() {
    const list = document.getElementById('btSavedDevicesList');
    if (!list) return;
    const isMacro = c => Boolean(c.isMacro || (c.raw && (String(c.raw).startsWith('BT:SCRIPT:') || String(c.raw).startsWith('MACRO:'))));
    const devs = (this.inventory.devices || []).filter(d => (d.commands || []).some(isMacro));
    if (!devs.length) {
      list.innerHTML = '<div class="muted mini">No custom Bluetooth macro scripts saved yet.</div>';
      return;
    }
    list.innerHTML = devs.map(d => {
      const cmds = (d.commands || []).filter(isMacro);
      return `
        <div style="margin-bottom:12px; background:var(--bg-card); border:1px solid var(--border); border-radius:var(--radius-sm); padding:10px;">
          <strong style="font-size:13px; color:var(--accent);">${d.name} (${d.model || d.id})</strong>
          <div style="display:flex; flex-wrap:wrap; gap:6px; margin-top:8px;">
            ${cmds.map(c => `
              <div class="pill" style="display:flex; align-items:center; gap:6px; padding:4px 8px;">
                <span>${c.name}</span>
                <button type="button" class="btn btn-xs btn-ghost btn-del-bt-cmd" data-dev="${d.id}" data-cmd="${c.name}" style="padding:0 4px; color:var(--danger);" title="Delete macro">×</button>
              </div>
            `).join('')}
          </div>
        </div>
      `;
    }).join('');

    list.querySelectorAll('.btn-del-bt-cmd').forEach(btn => {
      btn.addEventListener('click', async () => {
        const devId = btn.getAttribute('data-dev');
        const cmdName = btn.getAttribute('data-cmd');
        if (!await showConfirm(`Delete Bluetooth macro "${cmdName}"?`, { title: 'Delete Bluetooth Macro', okText: 'Delete' })) return;
        try {
          await this.client.deleteBtCommand(devId, cmdName);
          await this.refreshData();
          this.renderBtSavedCommands();
        } catch (e) {
          alert(t('delete_failed', 'Delete failed: ') + e.message);
        }
      });
    });
  }

  setupBtTouchpad() {
    const tabKb = document.getElementById('tabBtKeyboard');
    const tabMouse = document.getElementById('tabBtMouse');
    const kbCont = document.getElementById('btKeyboardContainer');
    const mouseCont = document.getElementById('btMouseContainer');
    const touchpad = document.getElementById('btTouchpad');
    const ripple = document.getElementById('btTouchpadRipple');
    const speedSel = document.getElementById('btMouseSpeedSelect');
    const coordsEl = document.getElementById('btMouseCoords');

    if (tabKb && tabMouse && kbCont && mouseCont) {
      tabKb.addEventListener('click', () => {
        tabKb.className = 'btn btn-xs btn-primary';
        tabMouse.className = 'btn btn-xs btn-ghost';
        kbCont.style.display = 'block';
        mouseCont.style.display = 'none';
      });
      tabMouse.addEventListener('click', () => {
        tabMouse.className = 'btn btn-xs btn-primary';
        tabKb.className = 'btn btn-xs btn-ghost';
        kbCont.style.display = 'none';
        mouseCont.style.display = 'flex';
      });
    }

    if (!touchpad) return;

    let touchStartX = 0, touchStartY = 0;
    let lastX = 0, lastY = 0;
    let touchStartTime = 0;
    let totalDist = 0;
    let isScrollZone = false;
    let scrollAcc = 0;
    let isMouseDown = false;

    if (speedSel) {
      const savedSpeed = localStorage.getItem('bt_mouse_speed') || '6.0';
      if (speedSel.querySelector(`option[value="${savedSpeed}"]`)) {
        speedSel.value = savedSpeed;
      }
      speedSel.addEventListener('change', () => {
        localStorage.setItem('bt_mouse_speed', speedSel.value);
      });
    }

    const getSpeed = () => Number(speedSel?.value) || 6.0;
    const getTarget = () => (document.getElementById('btTargetHostSelect')?.value || '').trim();

    // Touch events for mobile / Android / Capacitor
    touchpad.addEventListener('touchstart', (e) => {
      if (!e.touches.length) return;
      const t = e.touches[0];
      const rect = touchpad.getBoundingClientRect();
      touchStartX = t.clientX;
      touchStartY = t.clientY;
      lastX = t.clientX;
      lastY = t.clientY;
      touchStartTime = Date.now();
      totalDist = 0;
      scrollAcc = 0;

      isScrollZone = (t.clientX >= rect.right - 48);

      touchpad.classList.add('touching');
      if (ripple) {
        ripple.style.left = `${t.clientX - rect.left}px`;
        ripple.style.top = `${t.clientY - rect.top}px`;
        ripple.style.display = 'block';
      }
    }, { passive: true });

    touchpad.addEventListener('touchmove', (e) => {
      if (!e.touches.length) return;
      e.preventDefault();
      const t = e.touches[0];
      const rect = touchpad.getBoundingClientRect();
      const rawDx = t.clientX - lastX;
      const rawDy = t.clientY - lastY;
      lastX = t.clientX;
      lastY = t.clientY;
      totalDist += Math.hypot(rawDx, rawDy);

      if (ripple) {
        ripple.style.left = `${t.clientX - rect.left}px`;
        ripple.style.top = `${t.clientY - rect.top}px`;
      }

      const tgt = getTarget();
      if (isScrollZone || e.touches.length >= 2) {
        scrollAcc += rawDy;
        if (Math.abs(scrollAcc) >= 14) {
          const wheel = scrollAcc < 0 ? 1 : -1;
          this.client.sendBtMouse({ wheel, target: tgt });
          scrollAcc = 0;
        }
      } else {
        const speed = getSpeed();
        const dx = Math.max(-127, Math.min(127, Math.round(rawDx * speed)));
        const dy = Math.max(-127, Math.min(127, Math.round(rawDy * speed)));
        if (dx !== 0 || dy !== 0) {
          this.client.sendBtMouse({ dx, dy, target: tgt });
          if (coordsEl) coordsEl.textContent = `dx: ${dx}, dy: ${dy}`;
        }
      }
    }, { passive: false });

    touchpad.addEventListener('touchend', () => {
      touchpad.classList.remove('touching');
      if (ripple) ripple.style.display = 'none';

      const duration = Date.now() - touchStartTime;
      if (duration < 280 && totalDist < 8) {
        const tgt = getTarget();
        this.remoteEngine.triggerHaptic();
        this.client.sendBtMouse({ action: 'click', button: 'left', target: tgt });
      }
    });

    touchpad.addEventListener('touchcancel', () => {
      touchpad.classList.remove('touching');
      if (ripple) ripple.style.display = 'none';
    });

    // Mouse drag events for desktop
    touchpad.addEventListener('mousedown', (e) => {
      if (e.button !== 0) return;
      isMouseDown = true;
      const rect = touchpad.getBoundingClientRect();
      touchStartX = e.clientX;
      touchStartY = e.clientY;
      lastX = e.clientX;
      lastY = e.clientY;
      touchStartTime = Date.now();
      totalDist = 0;
      scrollAcc = 0;
      isScrollZone = (e.clientX >= rect.right - 48);

      touchpad.classList.add('touching');
      if (ripple) {
        ripple.style.left = `${e.clientX - rect.left}px`;
        ripple.style.top = `${e.clientY - rect.top}px`;
        ripple.style.display = 'block';
      }
    });

    window.addEventListener('mousemove', (e) => {
      if (!isMouseDown) return;
      const rawDx = e.clientX - lastX;
      const rawDy = e.clientY - lastY;
      lastX = e.clientX;
      lastY = e.clientY;
      totalDist += Math.hypot(rawDx, rawDy);

      const rect = touchpad.getBoundingClientRect();
      if (ripple) {
        ripple.style.left = `${Math.max(0, Math.min(rect.width, e.clientX - rect.left))}px`;
        ripple.style.top = `${Math.max(0, Math.min(rect.height, e.clientY - rect.top))}px`;
      }

      const tgt = getTarget();
      if (isScrollZone) {
        scrollAcc += rawDy;
        if (Math.abs(scrollAcc) >= 16) {
          const wheel = scrollAcc < 0 ? 1 : -1;
          this.client.sendBtMouse({ wheel, target: tgt });
          scrollAcc = 0;
        }
      } else {
        const speed = getSpeed();
        const dx = Math.max(-127, Math.min(127, Math.round(rawDx * speed)));
        const dy = Math.max(-127, Math.min(127, Math.round(rawDy * speed)));
        if (dx !== 0 || dy !== 0) {
          this.client.sendBtMouse({ dx, dy, target: tgt });
          if (coordsEl) coordsEl.textContent = `dx: ${dx}, dy: ${dy}`;
        }
      }
    });

    window.addEventListener('mouseup', (e) => {
      if (!isMouseDown) return;
      isMouseDown = false;
      touchpad.classList.remove('touching');
      if (ripple) ripple.style.display = 'none';

      const duration = Date.now() - touchStartTime;
      if (duration < 280 && totalDist < 8 && e.target === touchpad) {
        const tgt = getTarget();
        this.client.sendBtMouse({ action: 'click', button: 'left', target: tgt });
      }
    });

    touchpad.addEventListener('wheel', (e) => {
      e.preventDefault();
      const tgt = getTarget();
      const wheel = e.deltaY < 0 ? 1 : -1;
      this.client.sendBtMouse({ wheel, target: tgt });
    }, { passive: false });

    // Click & scroll button handlers
    document.getElementById('btnMouseLeft')?.addEventListener('click', () => {
      this.remoteEngine.triggerHaptic();
      this.client.sendBtMouse({ action: 'click', button: 'left', target: getTarget() });
    });
    document.getElementById('btnMouseRight')?.addEventListener('click', () => {
      this.remoteEngine.triggerHaptic();
      this.client.sendBtMouse({ action: 'click', button: 'right', target: getTarget() });
    });
    document.getElementById('btnMouseMiddle')?.addEventListener('click', () => {
      this.remoteEngine.triggerHaptic();
      this.client.sendBtMouse({ action: 'click', button: 'middle', target: getTarget() });
    });
    document.getElementById('btnMouseWheelUp')?.addEventListener('click', () => {
      this.client.sendBtMouse({ wheel: 1, target: getTarget() });
    });
    document.getElementById('btnMouseWheelDown')?.addEventListener('click', () => {
      this.client.sendBtMouse({ wheel: -1, target: getTarget() });
    });
  }

}
