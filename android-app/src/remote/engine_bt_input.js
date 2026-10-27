import { RemoteEngineBase } from './engine_base.js';
import { t } from '../i18n.js';

export class RemoteEngineBtInput extends RemoteEngineBase {
  renderKeyboardView() {
    const view = document.createElement('div');
    view.className = 'keyboard-panel';

    const btDev = this.getBluetoothDevice();
    const devName = btDev ? (btDev.name || btDev.label || 'Bluetooth Device') : 'Bluetooth';
    const devId = btDev ? btDev.id : null;

    view.innerHTML = `
      <div class="keyboard-header" style="display:flex; justify-content:space-between; align-items:center; margin-bottom:12px; flex-wrap:wrap; gap:8px;">
        <div class="keyboard-device-badge">
          <span class="bt-dot"></span>
          <span>Target: <strong>${devName}</strong></span>
        </div>
        <div style="display:flex; align-items:center; gap:6px;">
          <div class="subtabs-group" style="display:inline-flex; border:1px solid var(--border); border-radius:var(--radius-sm); overflow:hidden;">
            <button type="button" class="btn btn-xs btn-primary" style="border-radius:0;">${t('keyboard_tab')}</button>
            <button type="button" class="btn btn-xs btn-ghost" id="kbSwitchToMouseBtn" style="border-radius:0;">${t('mouse_tab')}</button>
          </div>
          <button type="button" class="btn btn-xs btn-danger" id="kbReleaseAllBtn" title="${t('release_held_keys_title')}">${t('release_all')}</button>
        </div>
      </div>

      <div class="keyboard-card" style="margin-bottom:14px;">
        <form class="keyboard-form" id="kbForm">
          <div class="keyboard-input-wrap">
            <input type="text" id="kbTextInput" class="form-control keyboard-input" placeholder="${t('placeholder_kb_text')}" autocomplete="off" autocorrect="off" autocapitalize="off" spellcheck="false">
            <button type="submit" class="btn btn-primary keyboard-send-btn" id="kbSendBtn">${t('send')}</button>
          </div>
          <div class="keyboard-hint mini muted" style="margin-top:4px;">${t('kb_send_hint')}</div>
          <div class="keyboard-feedback" id="kbFeedback" style="display: none; margin-top:6px;"></div>
        </form>
      </div>

      <div style="display:grid; grid-template-columns: repeat(auto-fit, minmax(260px, 1fr)); gap:12px; margin-bottom:14px;">
        <div class="keyboard-nav-section card" style="padding:10px; margin:0;">
          <div class="keyboard-section-title" style="font-size:12px; font-weight:600; margin-bottom:8px;">Navigation</div>
          <div class="keyboard-dpad-grid" style="display:grid; grid-template-columns:repeat(3, 1fr); gap:6px; max-width:210px; margin:0 auto;">
            <div class="btn-spacer"></div>
            <button type="button" class="remote-btn kb-nav-btn" data-nav="up" data-code="up">▲</button>
            <div class="btn-spacer"></div>
            <button type="button" class="remote-btn kb-nav-btn" data-nav="left" data-code="left">◀</button>
            <button type="button" class="remote-btn btn-primary kb-nav-btn" data-nav="ok" data-code="enter">OK</button>
            <button type="button" class="remote-btn kb-nav-btn" data-nav="right" data-code="right">▶</button>
            <div class="btn-spacer"></div>
            <button type="button" class="remote-btn kb-nav-btn" data-nav="down" data-code="down">▼</button>
            <div class="btn-spacer"></div>
          </div>
          <div style="display:flex; justify-content:center; gap:8px; margin-top:8px;">
            <button type="button" class="remote-btn kb-btn-action" data-code="escape" data-fallback="Back,Return,Exit" style="padding:4px 10px; font-size:11px;">Back</button>
            <button type="button" class="remote-btn kb-btn-action" data-code="home" data-fallback="Home" style="padding:4px 10px; font-size:11px;">Home</button>
            <button type="button" class="remote-btn kb-btn-action" data-code="menu" data-fallback="Menu" style="padding:4px 10px; font-size:11px;">Menu</button>
          </div>
        </div>

        <div class="keyboard-quick-section card" style="padding:10px; margin:0;">
          <div class="keyboard-section-title" style="font-size:12px; font-weight:600; margin-bottom:8px;">Media & Actions</div>
          <div style="display:grid; grid-template-columns:repeat(3, 1fr); gap:6px;">
            <button type="button" class="remote-btn kb-btn-action" data-code="play_pause" data-fallback="Play,Pause,Play/Pause"><span>⏯</span><span class="mini">Play/Pause</span></button>
            <button type="button" class="remote-btn kb-btn-action" data-code="mute" data-fallback="Mute"><span>🔇</span><span class="mini">Mute</span></button>
            <button type="button" class="remote-btn kb-btn-action" data-code="enter" data-fallback="Select,OK,Enter"><span>⏎</span><span class="mini">Enter</span></button>
            <button type="button" class="remote-btn kb-btn-action" data-code="vol_down" data-fallback="VolumeDown"><span>🔉</span><span class="mini">Vol -</span></button>
            <button type="button" class="remote-btn kb-btn-action" data-code="vol_up" data-fallback="VolumeUp"><span>🔊</span><span class="mini">Vol +</span></button>
            <button type="button" class="remote-btn kb-btn-action" data-code="backspace" data-fallback="Back,Return,Clear"><span>⌫</span><span class="mini">Backspace</span></button>
            <button type="button" class="remote-btn kb-btn-action" data-code="prev_track" data-fallback="PrevTrack,Previous"><span>⏮</span><span class="mini">Prev</span></button>
            <button type="button" class="remote-btn kb-btn-action" data-code="next_track" data-fallback="NextTrack,Next"><span>⏭</span><span class="mini">Next</span></button>
            <button type="button" class="remote-btn kb-btn-action" data-code="space" data-fallback="Space"><span>␣</span><span class="mini">Space</span></button>
          </div>
        </div>
      </div>

      <div class="card" style="padding:10px; margin-bottom:12px;">
        <div class="keyboard-section-title" style="font-size:12px; font-weight:600; margin-bottom:8px;">Virtual Keyboard</div>
        <div class="kb-panel" style="overflow-x:auto; padding-bottom:6px; -webkit-overflow-scrolling:touch;">
          <div class="kb-row">
            <button type="button" class="kb-key kb-125" data-code="escape">Esc</button>
            <button type="button" class="kb-key" data-code="1">1</button>
            <button type="button" class="kb-key" data-code="2">2</button>
            <button type="button" class="kb-key" data-code="3">3</button>
            <button type="button" class="kb-key" data-code="4">4</button>
            <button type="button" class="kb-key" data-code="5">5</button>
            <button type="button" class="kb-key" data-code="6">6</button>
            <button type="button" class="kb-key" data-code="7">7</button>
            <button type="button" class="kb-key" data-code="8">8</button>
            <button type="button" class="kb-key" data-code="9">9</button>
            <button type="button" class="kb-key" data-code="0">0</button>
            <button type="button" class="kb-key" data-code="minus">-</button>
            <button type="button" class="kb-key" data-code="equal">=</button>
            <button type="button" class="kb-key kb-15" data-code="backspace">⌫</button>
          </div>
          <div class="kb-row">
            <button type="button" class="kb-key kb-125" data-code="tab">Tab</button>
            <button type="button" class="kb-key" data-code="q">Q</button>
            <button type="button" class="kb-key" data-code="w">W</button>
            <button type="button" class="kb-key" data-code="e">E</button>
            <button type="button" class="kb-key" data-code="r">R</button>
            <button type="button" class="kb-key" data-code="t">T</button>
            <button type="button" class="kb-key" data-code="y">Y</button>
            <button type="button" class="kb-key" data-code="u">U</button>
            <button type="button" class="kb-key" data-code="i">I</button>
            <button type="button" class="kb-key" data-code="o">O</button>
            <button type="button" class="kb-key" data-code="p">P</button>
            <button type="button" class="kb-key" data-code="bracket_left">[</button>
            <button type="button" class="kb-key" data-code="bracket_right">]</button>
            <button type="button" class="kb-key" data-code="backslash">\\</button>
          </div>
          <div class="kb-row">
            <button type="button" class="kb-key kb-15" data-code="caps_lock">Caps</button>
            <button type="button" class="kb-key" data-code="a">A</button>
            <button type="button" class="kb-key" data-code="s">S</button>
            <button type="button" class="kb-key" data-code="d">D</button>
            <button type="button" class="kb-key" data-code="f">F</button>
            <button type="button" class="kb-key" data-code="g">G</button>
            <button type="button" class="kb-key" data-code="h">H</button>
            <button type="button" class="kb-key" data-code="j">J</button>
            <button type="button" class="kb-key" data-code="k">K</button>
            <button type="button" class="kb-key" data-code="l">L</button>
            <button type="button" class="kb-key" data-code="semicolon">;</button>
            <button type="button" class="kb-key" data-code="quote">'</button>
            <button type="button" class="kb-key kb-15" data-code="enter">⏎</button>
          </div>
          <div class="kb-row">
            <button type="button" class="kb-key kb-15" data-code="shift_left">Shift</button>
            <button type="button" class="kb-key" data-code="z">Z</button>
            <button type="button" class="kb-key" data-code="x">X</button>
            <button type="button" class="kb-key" data-code="c">C</button>
            <button type="button" class="kb-key" data-code="v">V</button>
            <button type="button" class="kb-key" data-code="b">B</button>
            <button type="button" class="kb-key" data-code="n">N</button>
            <button type="button" class="kb-key" data-code="m">M</button>
            <button type="button" class="kb-key" data-code="comma">,</button>
            <button type="button" class="kb-key" data-code="dot">.</button>
            <button type="button" class="kb-key" data-code="slash">/</button>
            <button type="button" class="kb-key kb-15" data-code="delete">Del</button>
          </div>
          <div class="kb-row">
            <button type="button" class="kb-key kb-125" data-code="home">Home</button>
            <button type="button" class="kb-key kb-sp" data-code="space" style="flex-grow:1;">Space</button>
            <button type="button" class="kb-key kb-125" data-code="end">End</button>
          </div>
        </div>
      </div>
    `;

    const form = view.querySelector('#kbForm');
    const input = view.querySelector('#kbTextInput');
    const sendBtn = view.querySelector('#kbSendBtn');
    const feedback = view.querySelector('#kbFeedback');
    const releaseAllBtn = view.querySelector('#kbReleaseAllBtn');

    const showFeedback = (msg, isErr = false) => {
      feedback.style.display = 'block';
      feedback.className = `keyboard-feedback ${isErr ? 'text-danger' : 'text-success'}`;
      feedback.textContent = msg;
      setTimeout(() => {
        if (feedback) feedback.style.display = 'none';
      }, 3000);
    };

    const sendKey = async (code, fallbackCsv = '') => {
      await this.triggerHaptic();
      let sent = false;
      try {
        if (this.client && typeof this.client.sendBtKey === 'function') {
          await this.client.sendBtKey(code, 'tap');
          sent = true;
        }
      } catch {
        sent = false;
      }
      if (!sent && devId && fallbackCsv && this.onSendCallback) {
        const patterns = fallbackCsv.split(',').map(s => s.trim());
        const cmd = this.findCmdInDevice(devId, patterns);
        if (cmd) {
          this.onSendCallback(devId, cmd);
        }
      }
    };

    const switchToMouseBtn = view.querySelector('#kbSwitchToMouseBtn');
    if (switchToMouseBtn) {
      switchToMouseBtn.addEventListener('click', () => {
        this.currentSubTab = 'mouse';
        this.updateDOM();
      });
    }

    if (releaseAllBtn) {
      releaseAllBtn.addEventListener('click', async () => {
        await this.triggerHaptic();
        if (this.client && typeof this.client.sendBtKey === 'function') {
          await this.client.sendBtKey('release_all', 'tap');
          showFeedback('Released all held keys');
        }
      });
    }

    const sendText = async (text) => {
      if (!text) return;
      await this.triggerHaptic();
      sendBtn.disabled = true;
      try {
        if (this.client && typeof this.client.sendBtText === 'function') {
          const res = await this.client.sendBtText(text);
          if (res && res.error) {
            showFeedback(`Error: ${res.error}`, true);
          } else {
            showFeedback(`✓ Sent "${text}"`);
            input.value = '';
          }
        }
      } catch (err) {
        showFeedback(`Failed: ${err.message || err}`, true);
      } finally {
        sendBtn.disabled = false;
        input.focus();
      }
    };

    form.addEventListener('submit', (e) => {
      e.preventDefault();
      sendText(input.value);
    });

    view.querySelectorAll('.kb-btn-action, .kb-nav-btn, .kb-key').forEach(btn => {
      btn.addEventListener('click', async () => {
        const code = btn.getAttribute('data-code');
        const fallback = btn.getAttribute('data-fallback') || '';
        btn.classList.add('active-press');
        setTimeout(() => btn.classList.remove('active-press'), 150);
        if (code) {
          await sendKey(code, fallback);
        }
      });
    });

    return view;
  }

  renderMouseView() {
    const view = document.createElement('div');
    view.className = 'keyboard-panel';

    const btDev = this.getBluetoothDevice();
    const devName = btDev ? (btDev.name || btDev.label || 'Bluetooth Device') : 'Bluetooth';
    const devId = btDev ? btDev.id : null;
    const btTarget = btDev ? (btDev.btMac || btDev.mac || btDev.addr || btDev.bt_mac || '') : '';

    view.innerHTML = `
      <div class="keyboard-header" style="display:flex; justify-content:space-between; align-items:center; margin-bottom:12px; flex-wrap:wrap; gap:8px;">
        <div class="keyboard-device-badge">
          <span class="bt-dot"></span>
          <span>Target: <strong>${devName}</strong></span>
        </div>
        <div style="display:flex; align-items:center; gap:6px;">
          <div class="subtabs-group" style="display:inline-flex; border:1px solid var(--border); border-radius:var(--radius-sm); overflow:hidden;">
            <button type="button" class="btn btn-xs btn-ghost" id="mouseSwitchToKbBtn" style="border-radius:0;">${t('keyboard_tab')}</button>
            <button type="button" class="btn btn-xs btn-primary" style="border-radius:0;">${t('mouse_tab')}</button>
          </div>
          <select id="remoteMouseSpeed" class="form-control form-control-sm" style="font-size:11px; padding:2px 6px; width:72px;">
            <option value="3.0">3.0x</option>
            <option value="4.0">4.0x</option>
            <option value="5.0">5.0x</option>
            <option value="6.0" selected>6.0x</option>
            <option value="8.0">8.0x</option>
            <option value="10.0">10.0x</option>
            <option value="12.0">12.0x</option>
            <option value="15.0">15.0x</option>
            <option value="20.0">20.0x</option>
          </select>
          <button type="button" class="btn btn-xs btn-danger" id="mouseReleaseAllBtn" title="${t('release_held_mouse_title')}">${t('release')}</button>
        </div>
      </div>

      <div class="bt-touchpad-card" style="margin-bottom:14px;">
        <div class="bt-touchpad-surface" id="remoteTouchpad" style="height:260px;">
          <div class="bt-touchpad-hint">${t('touchpad_hint')}<br><span style="font-size:11px; opacity:0.75;">${t('touchpad_subhint')}</span></div>
          <div class="bt-touchpad-scroll-zone">
            <span>▲</span>
            <span>${t('scroll_zone')}</span>
            <span>▼</span>
          </div>
          <div class="bt-touchpad-ripple" id="remoteTouchpadRipple"></div>
        </div>

        <div class="bt-mouse-btn-bar">
          <button type="button" class="bt-mouse-btn btn-primary" id="remoteBtnMouseLeft" data-btn="left">${t('left_click')}</button>
          <button type="button" class="bt-mouse-btn" id="remoteBtnMouseMiddle" data-btn="middle" style="font-size:11px;">${t('middle_click')}</button>
          <button type="button" class="bt-mouse-btn" id="remoteBtnMouseRight" data-btn="right">${t('right_click')}</button>
        </div>

        <div style="display:flex; justify-content:space-between; align-items:center; padding:0 4px;">
          <div style="display:flex; gap:6px;">
            <button type="button" class="btn btn-xs btn-secondary" id="remoteBtnWheelUp" style="padding:4px 10px;">${t('scroll_up')}</button>
            <button type="button" class="btn btn-xs btn-secondary" id="remoteBtnWheelDown" style="padding:4px 10px;">${t('scroll_down')}</button>
          </div>
          <div class="mini subtle" id="remoteMouseCoords">dx: 0, dy: 0</div>
        </div>
      </div>

      <div class="card" style="padding:10px; margin-bottom:12px;">
        <div class="keyboard-section-title" style="font-size:12px; font-weight:600; margin-bottom:8px;">Quick Shortcuts</div>
        <div style="display:grid; grid-template-columns:repeat(5, 1fr); gap:6px;">
          <button type="button" class="remote-btn kb-btn-action" data-code="escape" data-fallback="Back,Return,Exit" style="padding:8px 4px; font-size:11px;">Back</button>
          <button type="button" class="remote-btn kb-btn-action" data-code="home" data-fallback="Home" style="padding:8px 4px; font-size:11px;">Home</button>
          <button type="button" class="remote-btn kb-btn-action" data-code="play_pause" data-fallback="Play,Pause,Play/Pause" style="padding:8px 4px; font-size:11px;">⏯ Play</button>
          <button type="button" class="remote-btn kb-btn-action" data-code="vol_down" data-fallback="VolumeDown" style="padding:8px 4px; font-size:11px;">🔉 Vol-</button>
          <button type="button" class="remote-btn kb-btn-action" data-code="vol_up" data-fallback="VolumeUp" style="padding:8px 4px; font-size:11px;">🔊 Vol+</button>
        </div>
      </div>
    `;

    const switchToKbBtn = view.querySelector('#mouseSwitchToKbBtn');
    if (switchToKbBtn) {
      switchToKbBtn.addEventListener('click', () => {
        this.currentSubTab = 'keyboard';
        this.updateDOM();
      });
    }

    const touchpad = view.querySelector('#remoteTouchpad');
    const ripple = view.querySelector('#remoteTouchpadRipple');
    const speedSel = view.querySelector('#remoteMouseSpeed');
    const coordsEl = view.querySelector('#remoteMouseCoords');
    const releaseBtn = view.querySelector('#mouseReleaseAllBtn');
    const btnLeft = view.querySelector('#remoteBtnMouseLeft');
    const btnMiddle = view.querySelector('#remoteBtnMouseMiddle');
    const btnRight = view.querySelector('#remoteBtnMouseRight');
    const btnWheelUp = view.querySelector('#remoteBtnWheelUp');
    const btnWheelDown = view.querySelector('#remoteBtnWheelDown');

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

    const sendMouse = (payload) => {
      if (this.client && typeof this.client.sendBtMouse === 'function') {
        return this.client.sendBtMouse({ ...payload, target: btTarget });
      }
    };

    if (releaseBtn) {
      releaseBtn.addEventListener('click', async () => {
        await this.triggerHaptic();
        sendMouse({ action: 'up', button: 'left' });
        sendMouse({ action: 'up', button: 'middle' });
        sendMouse({ action: 'up', button: 'right' });
        if (coordsEl) coordsEl.textContent = t('state_released');
      });
    }

    if (btnLeft) {
      btnLeft.addEventListener('click', async () => {
        await this.triggerHaptic();
        sendMouse({ action: 'click', button: 'left' });
      });
    }

    if (btnMiddle) {
      btnMiddle.addEventListener('click', async () => {
        await this.triggerHaptic();
        sendMouse({ action: 'click', button: 'middle' });
      });
    }

    if (btnRight) {
      btnRight.addEventListener('click', async () => {
        await this.triggerHaptic();
        sendMouse({ action: 'click', button: 'right' });
      });
    }

    if (btnWheelUp) {
      btnWheelUp.addEventListener('click', async () => {
        await this.triggerHaptic();
        sendMouse({ wheel: 1 });
      });
    }

    if (btnWheelDown) {
      btnWheelDown.addEventListener('click', async () => {
        await this.triggerHaptic();
        sendMouse({ wheel: -1 });
      });
    }

    if (touchpad) {
      let touchStartX = 0, touchStartY = 0;
      let lastX = 0, lastY = 0;
      let touchStartTime = 0;
      let totalDist = 0;
      let isScrollZone = false;
      let scrollAcc = 0;
      let isMouseDown = false;

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

        if (isScrollZone || e.touches.length >= 2) {
          scrollAcc += rawDy;
          if (Math.abs(scrollAcc) >= 14) {
            const wheel = scrollAcc < 0 ? 1 : -1;
            sendMouse({ wheel });
            scrollAcc = 0;
          }
        } else {
          const speed = getSpeed();
          const dx = Math.max(-127, Math.min(127, Math.round(rawDx * speed)));
          const dy = Math.max(-127, Math.min(127, Math.round(rawDy * speed)));
          if (dx !== 0 || dy !== 0) {
            sendMouse({ dx, dy });
            if (coordsEl) coordsEl.textContent = `dx: ${dx}, dy: ${dy}`;
          }
        }
      }, { passive: false });

      touchpad.addEventListener('touchend', async () => {
        touchpad.classList.remove('touching');
        if (ripple) ripple.style.display = 'none';
        const duration = Date.now() - touchStartTime;
        if (duration < 250 && totalDist < 10) {
          await this.triggerHaptic();
          sendMouse({ action: 'click', button: 'left' });
          if (coordsEl) coordsEl.textContent = t('state_tap_click_left');
        }
      });

      touchpad.addEventListener('touchcancel', () => {
        touchpad.classList.remove('touching');
        if (ripple) ripple.style.display = 'none';
      });

      // Desktop Mouse / Pointer support
      touchpad.addEventListener('mousedown', (e) => {
        if (e.button !== 0) return;
        isMouseDown = true;
        const rect = touchpad.getBoundingClientRect();
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
        const rect = touchpad.getBoundingClientRect();
        const rawDx = e.clientX - lastX;
        const rawDy = e.clientY - lastY;
        lastX = e.clientX;
        lastY = e.clientY;
        totalDist += Math.hypot(rawDx, rawDy);

        if (ripple) {
          ripple.style.left = `${Math.max(0, Math.min(rect.width, e.clientX - rect.left))}px`;
          ripple.style.top = `${Math.max(0, Math.min(rect.height, e.clientY - rect.top))}px`;
        }

        if (isScrollZone) {
          scrollAcc += rawDy;
          if (Math.abs(scrollAcc) >= 14) {
            const wheel = scrollAcc < 0 ? 1 : -1;
            sendMouse({ wheel });
            scrollAcc = 0;
          }
        } else {
          const speed = getSpeed();
          const dx = Math.max(-127, Math.min(127, Math.round(rawDx * speed)));
          const dy = Math.max(-127, Math.min(127, Math.round(rawDy * speed)));
          if (dx !== 0 || dy !== 0) {
            sendMouse({ dx, dy });
            if (coordsEl) coordsEl.textContent = `dx: ${dx}, dy: ${dy}`;
          }
        }
      });

      window.addEventListener('mouseup', async () => {
        if (!isMouseDown) return;
        isMouseDown = false;
        touchpad.classList.remove('touching');
        if (ripple) ripple.style.display = 'none';
        const duration = Date.now() - touchStartTime;
        if (duration < 250 && totalDist < 6) {
          await this.triggerHaptic();
          sendMouse({ action: 'click', button: 'left' });
          if (coordsEl) coordsEl.textContent = t('state_click_left');
        }
      });

      touchpad.addEventListener('wheel', (e) => {
        e.preventDefault();
        const wheel = e.deltaY < 0 ? 1 : -1;
        sendMouse({ wheel });
      }, { passive: false });
    }

    // Quick shortcuts event delegation
    view.querySelectorAll('.kb-btn-action').forEach(btn => {
      btn.addEventListener('click', async () => {
        const code = btn.getAttribute('data-code');
        const fallback = btn.getAttribute('data-fallback') || '';
        btn.classList.add('active-press');
        setTimeout(() => btn.classList.remove('active-press'), 150);
        await this.triggerHaptic();
        let sent = false;
        try {
          if (this.client && typeof this.client.sendBtKey === 'function') {
            await this.client.sendBtKey(code, 'tap');
            sent = true;
          }
        } catch {
          sent = false;
        }
        if (!sent && devId && fallback && this.onSendCallback) {
          const patterns = fallback.split(',').map(s => s.trim());
          const cmd = this.findCmdInDevice(devId, patterns);
          if (cmd) {
            this.onSendCallback(devId, cmd);
          }
        }
      });
    });

    return view;
  }
}
