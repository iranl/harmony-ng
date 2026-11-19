import { HarmonyClient } from '../api.js';
import { getSavedHubs, saveHub, deleteHub, getActiveHub, setActiveHubId } from '../storage.js';
import { RemoteEngine } from '../remote-engine.js';
import { forgetBtHostFlow } from './bt_forget.js';
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
import { t, setLanguage, getInitialLanguage, tf, tfHtml } from '../i18n.js';
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
  /* pushed: a bt_status object from the WebSocket (state watcher); fetched when absent */
  async loadBtView(pushed = null) {
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
      pairTarget.innerHTML = `<option value="">${t('select_target_device_required')}</option>` +
        devs.map(d => {
          const isBt = d.type === 'BT' || d.type === 'Bluetooth' || d.isBt;
          return `<option value="${escapeHtml(d.id)}">${escapeHtml(d.name || d.id)}${isBt ? ' (BT)' : ''}</option>`;
        }).join('');
    }
    if (saveSel) {
      saveSel.innerHTML = (this.inventory.devices || []).map(d => `<option value="${escapeHtml(d.id)}">${escapeHtml(d.name || d.id)}</option>`).join('');
    }

    try {
      const st = pushed || await this.client.getBtStatus();
      const isRunning = Boolean(st && (st.pid || st.running || st.backend === 'btstack'));

      if (badge) {
        badge.className = `badge ${isRunning ? 'ok' : 'bad'}`;
        badge.textContent = isRunning ? t('btstack_running') : t('stopped_status');
      }
      if (desc) desc.textContent = isRunning ? t('btstack_daemon_active') : t('btstack_daemon_stopped');

      if (pidEl) pidEl.textContent = isRunning ? tf('running_pid', { pid: st.pid || t('active_lc') }) : t('stopped_status');
      if (backendEl) backendEl.textContent = st.backend ? tf('backend_value', { name: st.backend }) : t('not_running');

      // Host connection
      const host = st.host || {};
      const isHostConn = Boolean(host.connected);

      const findAssignedDevice = (h) => {
        if (!h || !h.addr) return null;
        return (this.inventory.devices || []).find(d => {
          const a = (d.bdaddr || d.btHost || '').toLowerCase();
          return a && a === h.addr.toLowerCase();
        });
      };

      const resolveHostDeviceName = (h) => {
        if (!h) return t('bluetooth_host_default');
        const assigned = findAssignedDevice(h);
        if (assigned) return assigned.name || assigned.model || h.name;
        const matchDev = (this.inventory.devices || []).find(d => {
          if (h.name && (d.name === h.name || d.id === h.name)) return true;
          return false;
        });
        if (matchDev) return matchDev.name || matchDev.model || h.name;
        if (!h.name || h.name === 'Default Host' || h.name.startsWith('Host 00:')) {
          const btDev = (this.inventory.devices || []).find(d => d.isBt || (d.type && d.type.toLowerCase().includes('bt')));
          if (btDev) return btDev.name || btDev.model;
        }
        return h.name || t('bluetooth_host_default');
      };

      if (targetNameEl) {
        if (isHostConn) {
          targetNameEl.textContent = resolveHostDeviceName(host);
          targetNameEl.style.color = 'var(--accent)';
        } else {
          targetNameEl.textContent = t('none_disconnected');
          targetNameEl.style.color = 'var(--text-dim)';
        }
      }
      if (targetAddrEl) {
        targetAddrEl.textContent = isHostConn ? (host.addr || t('connected')) : (host.addr && host.addr !== '00:00:00:00:00:00' ? tf('target_addr', { addr: host.addr }) : t('no_active_connection'));
      }
      if (hostStateEl) {
        hostStateEl.textContent = isHostConn ? t('badge_connected') : (host.pairing ? t('pairing_caps') : t('disconnected_caps'));
        hostStateEl.style.color = isHostConn ? 'var(--success)' : (host.pairing ? 'var(--warning)' : 'var(--text-dim)');
      }
      const pairToggleBtn = document.getElementById('btPairToggleBtn');
      if (pairToggleBtn) {
        if (host.pairing) {
          pairToggleBtn.textContent = t('stop_pairing_mode');
          pairToggleBtn.classList.remove('btn-primary');
          pairToggleBtn.classList.add('btn-warning');
        } else {
          pairToggleBtn.textContent = t('start_pairing_mode');
          pairToggleBtn.classList.remove('btn-warning');
          pairToggleBtn.classList.add('btn-primary');
        }
      }
      if (hostDetailEl) hostDetailEl.textContent = isHostConn ? t('ble_hid_link_active') : t('standby_idle');

      // Remote connection
      const rem = st.remote || {};
      const isRemConn = Boolean(rem.connected);
      if (remoteStateEl) {
        remoteStateEl.textContent = isRemConn ? t('badge_connected') : t('disconnected_caps');
        remoteStateEl.style.color = isRemConn ? 'var(--success)' : 'var(--text-dim)';
      }
      if (remoteAddrEl) remoteAddrEl.textContent = rem.addr && rem.addr !== '00:00:00:00:00:00' ? rem.addr : t('b25_ble_remote_label');

      // Paired hosts list
      const hosts = st.hosts || (st.host?.addr ? [st.host] : []);
      const devs = this.inventory.devices || [];
      if (list) {
        list.innerHTML = hosts.length ? hosts.map(h => {
          const assigned = findAssignedDevice(h);
          return `
          <div class="queue-row" style="display:flex; justify-content:space-between; align-items:center; padding:8px 10px; background:var(--bg-card); border:1px solid var(--border); border-radius:var(--radius-sm); margin-bottom:6px; flex-wrap:wrap; gap:8px;">
            <div style="min-width: 180px;">
              <strong>${escapeHtml(resolveHostDeviceName(h))}</strong>
              <div class="muted mini">${escapeHtml(h.addr)} • <span style="color:${h.connected ? 'var(--success)' : (h.paired === false ? 'var(--warning)' : 'var(--text-dim)')}">${h.connected ? t('connected') : (h.paired === false ? t('host_not_paired_no_key') : t('host_paired'))}</span></div>
            </div>
            <div style="display:flex; gap:6px; align-items:center; flex-wrap:wrap;">
              <button type="button" class="btn btn-xs btn-danger btn-bt-forget" data-addr="${escapeHtml(h.addr)}" data-name="${escapeHtml(resolveHostDeviceName(h))}">${escapeHtml(t('bt_forget_host', 'Forget'))}</button>
              <select class="form-control form-control-sm host-assign-sel" data-addr="${escapeHtml(h.addr)}" style="font-size:11px; max-width:180px;">
                <option value="">${t('assign_device_option')}</option>
                ${devs.map(d => `<option value="${escapeHtml(d.id)}" ${assigned && String(assigned.id) === String(d.id) ? 'selected' : ''}>${escapeHtml(d.name || d.id)}</option>`).join('')}
              </select>
              <button type="button" class="btn btn-xs btn-secondary btn-bt-disconn" data-addr="${escapeHtml(h.addr)}" data-connected="${h.connected ? '1' : ''}">${h.connected ? t('disconnect') : t('connect')}</button>
            </div>
          </div>
        `;
        }).join('') : `<div class="muted mini">${t('no_paired_hosts_yet')}</div>`;

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
            /* The button reads Connect or Disconnect; it used to disconnect in both cases */
            const addr = b.getAttribute('data-addr');
            if (b.getAttribute('data-connected')) {
              await this.client.disconnectBt(addr);
            } else {
              await this.client.connectBt(addr);
            }
            this.loadBtView();
          });
        });

        list.querySelectorAll('.btn-bt-forget').forEach(b => {
          b.addEventListener('click', async () => {
            const addr = b.getAttribute('data-addr');
            const name = b.getAttribute('data-name') || addr;
            if (await forgetBtHostFlow({ client: this.client, addr, name, t, showConfirm, showToast })) await this.refreshData();
            this.loadBtView();
          });
        });
      }

      if (hostSel) {
        hostSel.innerHTML = `<option value="">${t('auto_active_host')}</option>` +
          hosts.map(d => `<option value="${escapeHtml(d.addr)}">${escapeHtml(d.name || d.addr)}</option>`).join('');
      }
    } catch (err) {
      if (desc) desc.textContent = tf('error_msg', { error: err.message });
    }
  }

  async toggleBtPairing() {
    const btn = document.getElementById('btPairToggleBtn');
    const name = document.getElementById('btPairName')?.value || 'Harmony Keyboard';
    const target = document.getElementById('btPairTargetDev')?.value || '';

    const isPairing = btn && btn.classList.contains('btn-warning'); /* set while pairing; the label is translated */
    if (isPairing) {
      try {
        if (btn) btn.textContent = t('stopping_pairing');
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
      if (btn) btn.textContent = t('entering_pairing_mode');
      await this.client.setBtPairing(true, name, target);
      const devObj = (this.inventory.devices || []).find(d => String(d.id) === String(target));
      showToast(tf('hub_now_discoverable', { name: name, target: devObj ? devObj.name : target }), 'success');
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
      if (stat) stat.textContent = t('sending_text');
      await this.client.sendBtText(txt, target);
      if (stat) stat.innerHTML = `<span style="color:var(--success)">${t('sent_status')}</span>`;
      this.appendBtLog(target ? tf('bt_log_text_sent_to', { count: txt.length, target: target }) : tf('bt_log_text_sent', { count: txt.length }));
    } catch (err) {
      if (stat) stat.innerHTML = `<span style="color:var(--danger)">${tfHtml('error_msg', { error: err.message })}</span>`;
      this.appendBtLog(tf('bt_log_text_failed', { error: err.message || err }));
    }
  }

  appendBtLog(msg) {
    const el = document.getElementById('btLog');
    if (!el) return;
    const time = new Date().toLocaleTimeString();
    el.textContent = `[${time}] ${msg}\n` + ((el.textContent === 'Ready.' || el.textContent === t('ready_status')) ? '' : el.textContent.slice(0, 2000));
  }

  async runBtMacroScript() {
    const script = document.getElementById('btMacroScript')?.value;
    const delay = Number(document.getElementById('btScriptDelay')?.value) || 35;
    const stat = document.getElementById('btScriptStatus');
    if (!script) return;
    try {
      if (stat) stat.textContent = t('running_macro_script');
      await this.client.runBtScript(script, delay);
      if (stat) stat.innerHTML = `<span style="color:var(--success)">${t('script_finished')}</span>`;
    } catch (err) {
      if (stat) stat.innerHTML = `<span style="color:var(--danger)">${tfHtml('failed_msg', { error: err.message })}</span>`;
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
      if (stat) stat.innerHTML = `<span style="color:var(--success)">${t('saved_status')}</span>`;
      await this.refreshData();
    } catch (err) {
      if (stat) stat.innerHTML = `<span style="color:var(--danger)">${tfHtml('error_msg', { error: err.message })}</span>`;
    }
  }

  /* -------------------------------------------------------------
   * 7b. Bluetooth Keyboard & Macros View
   * ------------------------------------------------------------- */
  async loadBtKeyboardView() {
    const hostSel = document.getElementById('btTargetHostSelect');
    const saveSel = document.getElementById('btSaveDeviceSelect');
    if (saveSel) {
      saveSel.innerHTML = (this.inventory.devices || []).map(d => `<option value="${escapeHtml(d.id)}">${escapeHtml(d.name || d.id)}</option>`).join('');
    }
    try {
      const st = await this.client.getBtStatus();
      const hosts = st?.hosts || (st?.host?.addr ? [st.host] : []);
      if (hostSel) {
        hostSel.innerHTML = `<option value="">${t('auto_active_host')}</option>` +
          hosts.map(d => `<option value="${escapeHtml(d.addr)}">${escapeHtml(d.name || d.addr)}</option>`).join('');
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
      list.innerHTML = `<div class="muted mini">${t('no_bt_macros')}</div>`;
      return;
    }
    list.innerHTML = devs.map(d => {
      const cmds = (d.commands || []).filter(isMacro);
      return `
        <div style="margin-bottom:12px; background:var(--bg-card); border:1px solid var(--border); border-radius:var(--radius-sm); padding:10px;">
          <strong style="font-size:13px; color:var(--accent);">${escapeHtml(d.name)} (${escapeHtml(d.model || d.id)})</strong>
          <div style="display:flex; flex-wrap:wrap; gap:6px; margin-top:8px;">
            ${cmds.map(c => `
              <div class="pill" style="display:flex; align-items:center; gap:6px; padding:4px 8px;">
                <span>${escapeHtml(c.name)}</span>
                <button type="button" class="btn btn-xs btn-ghost btn-del-bt-cmd" data-dev="${escapeHtml(d.id)}" data-cmd="${escapeHtml(c.name)}" style="padding:0 4px; color:var(--danger);" title="${t('delete_macro')}">×</button>
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
        if (!await showConfirm(tf('delete_bt_macro_confirm', { name: cmdName }), { title: t('delete_bt_macro'), okText: t('delete') })) return;
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
