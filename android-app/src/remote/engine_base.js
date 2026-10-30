import { Haptics, ImpactStyle } from '@capacitor/haptics';
import { getRemoteLayout, saveRemoteLayout, resetRemoteLayout } from '../storage.js';
import { t } from '../i18n.js';

export class RemoteEngineBase {
  constructor(client) {
    this.client = client;
    this.editMode = false;
    this.currentSubTab = 'remote'; // 'remote' | 'commands' | 'keypad'
    this.currentScope = null; // 'activity' | 'device'
    this.currentTarget = null;
    this.availableDevices = [];
    this.deviceCommandsMap = {};
    this.activeLayout = null;
    this.onSendCallback = null;
    this.container = null;
    this.cmdSearchFilter = '';
    this.selectedKeypadDeviceId = null;
    this.selectableDevices = [];
    this.onSelectDeviceCallback = null;
  }

  async triggerHaptic() {
    try {
      await Haptics.impact({ style: ImpactStyle.Light });
    } catch {
      // Browser fallback - ignore
    }
  }

  getNumberPatterns() {
    return [
      '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
      'number0', 'number1', 'number2', 'number3', 'number4', 'number5', 'number6', 'number7', 'number8', 'number9'
    ];
  }

  getRelevantScopeDevices() {
    if (this.currentScope === 'device') {
      return this.currentTarget ? [this.currentTarget] : [];
    }
    if (this.currentTarget && this.currentScope === 'activity') {
      const act = this.currentTarget;
      const set = new Set();
      if (Array.isArray(act.deviceIds)) {
        act.deviceIds.forEach(id => set.add(String(id)));
      }
      const seq = [
        ...(act.startSequence || []),
        ...(act.stopSequence || [])
      ];
      seq.forEach(s => {
        const did = s.deviceId;
        if (did && String(did) !== '0') set.add(String(did));
      });
      if (set.size > 0) {
        return (this.availableDevices || []).filter(d => set.has(String(d.id)));
      }
    }
    return this.availableDevices || [];
  }

  isDeviceBluetooth(dev) {
    if (!dev) return false;
    if (Number(dev.transport) === 32 || dev.isBt || dev.type === 'BT' || dev.type === 'Bluetooth') return true;
    const name = `${dev.name || ''} ${dev.manufacturer || ''} ${dev.model || ''}`.toLowerCase();
    if (name.includes('shield') || name.includes('fire tv') || name.includes('firetv') || name.includes('apple tv') || name.includes('appletv')) {
      return true;
    }
    const cmds = this.deviceCommandsMap[dev.id] || [];
    return cmds.some(c => Number(c.protocolId) === 999997 || (c.keyCode && (String(c.keyCode).startsWith('HID:') || String(c.keyCode).startsWith('G:HID'))) || (c.keycode && (String(c.keycode).startsWith('HID:') || String(c.keycode).startsWith('G:HID'))));
  }

  getDevicesWithNumbers() {
    const patterns = this.getNumberPatterns();
    const devs = this.getRelevantScopeDevices();
    return devs.filter(dev => {
      const cmds = this.deviceCommandsMap[dev.id] || [];
      return cmds.some(c => {
        const name = String(c.name || c).toLowerCase().replace(/[\s_\-]/g, '');
        return patterns.includes(name) || /^(num|number|digit)?[0-9]$/i.test(name);
      });
    });
  }

  hasNumberCommands() {
    return this.getDevicesWithNumbers().length > 0;
  }

  hasBluetoothConnection() {
    const devs = this.getRelevantScopeDevices();
    return devs.some(dev => this.isDeviceBluetooth(dev));
  }

  getBluetoothDevice() {
    const devs = this.getRelevantScopeDevices();
    return devs.find(dev => this.isDeviceBluetooth(dev)) || (this.isDeviceBluetooth(this.currentTarget) ? this.currentTarget : null);
  }

  normalizeCmdName(name) {
    return String(name || '')
      .toLowerCase()
      .replace(/\+/g, 'up')
      .replace(/-/g, 'down')
      .replace(/[^a-z0-9]/g, '');
  }

  findCmdInDevice(devId, patterns) {
    if (!devId) return null;
    const cmds = this.deviceCommandsMap[devId] || [];
    if (!Array.isArray(cmds) || cmds.length === 0) return null;

    for (const pat of patterns) {
      const normPat = this.normalizeCmdName(pat);
      for (const c of cmds) {
        const rawName = String(c.name || c);
        const normName = this.normalizeCmdName(rawName);
        if (normName === normPat) {
          return rawName;
        }
      }
    }
    return null;
  }

  findCmd(devIdOrList, patterns) {
    if (!devIdOrList) return null;
    if (Array.isArray(devIdOrList)) {
      for (const dev of devIdOrList) {
        const id = typeof dev === 'object' ? dev.id : dev;
        const found = this.findCmdInDevice(id, patterns);
        if (found) return found;
      }
      return null;
    }
    const id = typeof devIdOrList === 'object' ? devIdOrList.id : devIdOrList;
    return this.findCmdInDevice(id, patterns);
  }

  findBestCmd(candidateDevices, patterns) {
    if (!candidateDevices || !Array.isArray(candidateDevices)) return null;
    for (const dev of candidateDevices) {
      if (!dev) continue;
      const devId = typeof dev === 'object' ? dev.id : dev;
      const cmd = this.findCmdInDevice(devId, patterns);
      if (cmd) {
        return { deviceId: devId, command: cmd };
      }
    }
    return null;
  }

  classifyDevices(devices) {
    const isDisplay = (d) => {
      const t = String(d.type || '').toLowerCase();
      const n = String(d.name || '').toLowerCase();
      const m = String(d.model || '').toLowerCase();
      return t.includes('tv') || t.includes('television') || t.includes('projector') || t.includes('display') ||
             n.includes('tv') || n.includes('television') || n.includes('projector') || n.includes('samsung') || n.includes('lg') || n.includes('sony') ||
             m.includes('tv') || m.includes('oled');
    };

    const isAudio = (d) => {
      const t = String(d.type || '').toLowerCase();
      const n = String(d.name || '').toLowerCase();
      const m = String(d.model || '').toLowerCase();
      return t.includes('receiver') || t.includes('stereo') || t.includes('soundbar') || t.includes('audio') || t.includes('amplifier') || t.includes('avr') ||
             n.includes('denon') || n.includes('receiver') || n.includes('soundbar') || n.includes('yamaha') || n.includes('sonos') || n.includes('marantz') || n.includes('onkyo') || n.includes('bose') || n.includes('avr') ||
             m.includes('avr');
    };

    const isMediaSource = (d) => {
      const t = String(d.type || '').toLowerCase();
      const n = String(d.name || '').toLowerCase();
      const m = String(d.model || '').toLowerCase();
      return t.includes('streaming') || t.includes('game') || t.includes('cable') || t.includes('satellite') || t.includes('settop') || t.includes('media') || t.includes('dvd') || t.includes('bluray') || t.includes('cd') || t.includes('tuner') ||
             n.includes('apple') || n.includes('roku') || n.includes('shield') || n.includes('fire') || n.includes('chromecast') || n.includes('kodi') || n.includes('playstation') || n.includes('xbox') || n.includes('nintendo') || n.includes('tivo') || n.includes('bluray') ||
             m.includes('apple') || m.includes('roku');
    };

    const displayDevs = devices.filter(isDisplay);
    const audioDevs = devices.filter(isAudio);
    const mediaDevs = devices.filter(isMediaSource);

    return { displayDevs, audioDevs, mediaDevs };
  }

  isButtonDeviceValid(btn) {
    if (!btn || !btn.deviceId) return false;
    if (this.currentScope === 'device') {
      return String(btn.deviceId) === String(this.currentTarget?.id);
    }
    if (this.currentScope === 'activity') {
      return this.availableDevices.some(d => String(d.id) === String(btn.deviceId));
    }
    return true;
  }

  isButtonCommandValid(btn) {
    if (!btn || !btn.deviceId || !btn.command) return false;
    const cmds = this.deviceCommandsMap[btn.deviceId];
    if (Array.isArray(cmds) && cmds.length > 0) {
      const targetNorm = this.normalizeCmdName(btn.command);
      return cmds.some(c => this.normalizeCmdName(c.name || c) === targetNorm);
    }
    return false;
  }

  isButtonValid(btn) {
    return this.isButtonDeviceValid(btn) && this.isButtonCommandValid(btn);
  }

  sanitizeGrid(grid) {
    if (!Array.isArray(grid)) return { sanitized: new Array(40).fill(null), changed: true };
    let changed = false;
    const sanitized = grid.map(btn => {
      if (!btn) return null;
      if (!this.isButtonValid(btn)) {
        changed = true;
        return null;
      }
      return btn;
    });
    return { sanitized, changed };
  }

  generateDefaultLayout(scope, target, availableDevices, commandsMap) {
    // 5 columns x 8 rows = 40 slots (indices 0..39)
    const grid = new Array(40).fill(null);

    if (!availableDevices || availableDevices.length === 0) {
      return {
        scope,
        targetId: target?.id,
        targetName: target?.name || target?.label,
        grid
      };
    }

    const assignedKeys = new Set();

    const setSlot = (row, col, devId, command, label, variant = '') => {
      if (!devId || !command) return false;
      const idx = row * 5 + col;
      if (idx < 0 || idx >= 40) return false;
      grid[idx] = {
        id: `btn_${row}_${col}`,
        deviceId: devId,
        command,
        label: label || command,
        variant
      };
      assignedKeys.add(`${devId}:${command}`);
      return true;
    };

    const placeCmd = (row, col, candidates, patterns, label, variant = '', allowDuplicate = false) => {
      const match = this.findBestCmd(candidates, patterns);
      if (!match) return false;
      if (!allowDuplicate && assignedKeys.has(`${match.deviceId}:${match.command}`)) {
        return false;
      }
      return setSlot(row, col, match.deviceId, match.command, label, variant);
    };

    // Candidate devices resolution
    let uniqueNav = [];
    let uniqueAudio = [];
    let uniqueDisplay = [];

    if (scope === 'device') {
      uniqueNav = [target];
      uniqueAudio = [target];
      uniqueDisplay = [target];
    } else {
      const { displayDevs, audioDevs, mediaDevs } = this.classifyDevices(availableDevices);
      const navOrder = [...mediaDevs, ...displayDevs, ...audioDevs, ...availableDevices];
      uniqueNav = navOrder.filter((d, i, arr) => arr.findIndex(x => String(x.id) === String(d.id)) === i);

      const audioOrder = [...audioDevs, ...displayDevs, ...availableDevices];
      uniqueAudio = audioOrder.filter((d, i, arr) => arr.findIndex(x => String(x.id) === String(d.id)) === i);

      const displayOrder = [...displayDevs, ...audioDevs, ...mediaDevs, ...availableDevices];
      uniqueDisplay = displayOrder.filter((d, i, arr) => arr.findIndex(x => String(x.id) === String(d.id)) === i);
    }

    const PATTERNS = {
      power: ['powertoggle', 'power', 'poweron', 'pwr', 'standby', 'standbytoggle', 'systempower'],
      powerOff: ['poweroff', 'systempoweroff', 'off'],
      input: ['inputnext', 'input', 'source', 'inputsource', 'inputhdmi1', 'hdmi'],
      home: ['home', 'smart', 'hub', 'mainmenu', 'topmenu', 'rootmenu'],
      menu: ['menu', 'settings', 'setup', 'options', 'tools', 'actionmenu', 'preferences'],
      exit: ['exit', 'clear', 'cancel', 'close'],
      back: ['back', 'return', 'exit', 'escape', 'esc', 'goback', 'backspace'],
      info: ['info', 'display', 'status', 'osd', 'displayinfo', 'information'],
      guide: ['guide', 'epg', 'programguide', 'livetv', 'tvguide'],

      up: ['directionup', 'cursorup', 'arrowup', 'navup', 'up', 'uparrow', 'cursor_up'],
      down: ['directiondown', 'cursordown', 'arrowdown', 'navdown', 'down', 'downarrow', 'cursor_down'],
      left: ['directionleft', 'cursorleft', 'arrowleft', 'navleft', 'left', 'leftarrow', 'cursor_left'],
      right: ['directionright', 'cursorright', 'arrowright', 'navright', 'right', 'rightarrow', 'cursor_right'],
      select: ['select', 'ok', 'enter', 'confirm', 'center', 'selectenter', 'cursorenter'],

      volUp: ['volumeup', 'volup', 'volumeplus', 'volplus', 'volume+'],
      volDown: ['volumedown', 'voldown', 'volumeminus', 'volminus', 'volume-'],
      mute: ['mute', 'volumemute', 'mutetoggle', 'soundmute'],

      chUp: ['channelup', 'chup', 'channelplus', 'chplus', 'pageup', 'pgup', 'nextchannel'],
      chDown: ['channeldown', 'chdown', 'channelminus', 'chminus', 'pagedown', 'pgdown', 'previouschannel', 'prevchannel'],
      prevCh: ['prevchannel', 'previouschannel', 'lastchannel', 'last', 'prev', 'recall', 'jump'],

      rewind: ['rewind', 'rev', 'scanrev', 'skipback', 'previoustrack', 'skipbackward'],
      play: ['play', 'playpause'],
      pause: ['pause', 'playpause'],
      stop: ['stop'],
      fastForward: ['fastforward', 'ff', 'forward', 'ffwd', 'scanfwd', 'skipforward', 'nexttrack'],
      replay: ['replay', 'jumpback', 'instantreplay', 'quickskip', 'skipback'],
      skipFwd: ['skipforward', 'nexttrack', 'jumpforward', 'advance', 'skip'],
      record: ['record', 'rec'],

      sub: ['subtitle', 'subtitles', 'cc', 'closedcaption'],
      audio: ['audio', 'soundtrack', 'audiotrack', 'sound', 'surround', 'soundmode'],
      popup: ['popupmenu', 'popup', 'options', 'tools', 'quickmenu'],
      dvr: ['dvr', 'list', 'recordedtv', 'recordings'],

      red: ['red', 'colorred', 'buttonred'],
      green: ['green', 'colorgreen', 'buttongreen'],
      yellow: ['yellow', 'coloryellow', 'buttonyellow'],
      blue: ['blue', 'colorblue', 'buttonblue']
    };

    // Row 0: Top Controls - Power in corner, Input, Home, Guide, Info/PowerOff
    placeCmd(0, 0, uniqueDisplay, PATTERNS.power, 'PWR', 'danger');
    placeCmd(0, 1, uniqueDisplay, PATTERNS.input, 'INPUT');
    placeCmd(0, 2, uniqueNav, PATTERNS.home, 'HOME');
    placeCmd(0, 3, uniqueDisplay, PATTERNS.guide, 'GUIDE');
    if (!placeCmd(0, 4, uniqueDisplay, PATTERNS.powerOff, 'OFF', 'danger')) {
      placeCmd(0, 4, uniqueDisplay, PATTERNS.info, 'INFO');
    }

    // Row 1: Menus, Back, D-Pad Up (▲), Exit, Info/Guide
    placeCmd(1, 0, uniqueNav, PATTERNS.menu, 'MENU');
    placeCmd(1, 1, uniqueNav, PATTERNS.back, 'BACK');
    placeCmd(1, 2, uniqueNav, PATTERNS.up, '▲');
    placeCmd(1, 3, uniqueNav, PATTERNS.exit, 'EXIT');
    if (!grid[4]) {
      placeCmd(1, 4, uniqueDisplay, PATTERNS.info, 'INFO');
    }

    // Row 2: Vol+, D-Pad Left (◀), D-Pad OK (OK), D-Pad Right (▶), Ch+
    placeCmd(2, 0, uniqueAudio, PATTERNS.volUp, 'VOL +');
    placeCmd(2, 1, uniqueNav, PATTERNS.left, '◀');
    placeCmd(2, 2, uniqueNav, PATTERNS.select, 'OK', 'primary');
    placeCmd(2, 3, uniqueNav, PATTERNS.right, '▶');
    placeCmd(2, 4, uniqueDisplay, PATTERNS.chUp, 'CH +');

    // Row 3: Vol-, Replay, D-Pad Down (▼), Skip Fwd, Ch-
    placeCmd(3, 0, uniqueAudio, PATTERNS.volDown, 'VOL -');
    placeCmd(3, 1, uniqueNav, PATTERNS.replay, 'REPLAY');
    placeCmd(3, 2, uniqueNav, PATTERNS.down, '▼');
    placeCmd(3, 3, uniqueNav, PATTERNS.skipFwd, 'SKIP');
    placeCmd(3, 4, uniqueDisplay, PATTERNS.chDown, 'CH -');

    // Row 4: Mute, Audio, PopUp/Options, Subtitle, Prev Channel
    placeCmd(4, 0, uniqueAudio, PATTERNS.mute, 'MUTE', 'warning');
    placeCmd(4, 1, uniqueAudio, PATTERNS.audio, 'AUDIO');
    placeCmd(4, 2, uniqueNav, PATTERNS.popup, 'OPT');
    placeCmd(4, 3, uniqueNav, PATTERNS.sub, 'SUB');
    placeCmd(4, 4, uniqueDisplay, PATTERNS.prevCh, 'PREV');

    // Row 5: Media Controls: Rewind, Play, Pause, Stop, Fast Forward
    placeCmd(5, 0, uniqueNav, PATTERNS.rewind, 'REW');
    placeCmd(5, 1, uniqueNav, PATTERNS.play, 'PLAY', 'accent');
    placeCmd(5, 2, uniqueNav, PATTERNS.pause, 'PAUSE');
    placeCmd(5, 3, uniqueNav, PATTERNS.stop, 'STOP');
    placeCmd(5, 4, uniqueNav, PATTERNS.fastForward, 'FF');

    // Row 6: Record, Secondary Media / DVR
    placeCmd(6, 0, uniqueNav, PATTERNS.record, 'REC', 'danger');
    placeCmd(6, 4, uniqueNav, PATTERNS.dvr, 'DVR');

    // Row 7: Color buttons
    placeCmd(7, 0, uniqueDisplay, PATTERNS.red, 'RED', 'danger');
    placeCmd(7, 1, uniqueDisplay, PATTERNS.green, 'GREEN', 'accent');
    placeCmd(7, 2, uniqueDisplay, PATTERNS.yellow, 'YELLOW', 'warning');
    placeCmd(7, 3, uniqueDisplay, PATTERNS.blue, 'BLUE', 'primary');

    // Fallback pass: For devices with remaining unassigned commands, fill empty slots
    if (scope === 'device' && target) {
      const devCmds = this.deviceCommandsMap[target.id] || [];
      const unassigned = devCmds.filter(c => {
        const rawName = String(c.name || c);
        return !assignedKeys.has(`${target.id}:${rawName}`) &&
               !/^(number|digit)?[0-9]$/i.test(rawName);
      });

      const fillSlots = [31, 32, 33, 39, 21, 22, 23, 16, 18];
      let uIdx = 0;
      for (const slotIdx of fillSlots) {
        if (!grid[slotIdx] && uIdx < unassigned.length) {
          const c = unassigned[uIdx++];
          const rawName = String(c.name || c);
          const r = Math.floor(slotIdx / 5);
          const col = slotIdx % 5;
          setSlot(r, col, target.id, rawName, rawName);
        }
      }
    }

    return {
      scope,
      targetId: target.id,
      targetName: target.name || target.label,
      grid
    };
  }

  async loadLayout(scope, target, availableDevices, commandsMap) {
    // 1. Check local storage override first
    let saved = await getRemoteLayout(scope, target.id);
    const hasAnyButtons = saved && Array.isArray(saved.grid) && saved.grid.some(b => b && b.command);

    // 2. If no local override, check Hub default layout
    if (!hasAnyButtons && this.client && typeof this.client.getUiRemoteLayout === 'function') {
      try {
        const hubData = await this.client.getUiRemoteLayout();
        const key = `${scope}_${target.id}`;
        const hubLayout = hubData?.layouts?.[key];
        if (hubLayout && Array.isArray(hubLayout.grid) && hubLayout.grid.some(b => b && b.command)) {
          saved = hubLayout;
        }
      } catch (e) {
        // Hub layout fetch failed or offline, continue to fallback
      }
    }

    if (!saved || !Array.isArray(saved.grid) || saved.grid.length !== 40 || !saved.grid.some(b => b && b.command)) {
      saved = this.generateDefaultLayout(scope, target, availableDevices, commandsMap);
      await saveRemoteLayout(scope, target.id, saved);
    } else {
      const { sanitized, changed } = this.sanitizeGrid(saved.grid);
      if (changed) {
        const stillHasButtons = sanitized.some(b => b && b.command);
        if (!stillHasButtons) {
          saved = this.generateDefaultLayout(scope, target, availableDevices, commandsMap);
        } else {
          saved.grid = sanitized;
        }
        await saveRemoteLayout(scope, target.id, saved);
      }
    }
    return saved;
  }

  async saveCurrentLayoutToHub() {
    if (!this.activeLayout || !this.currentTarget?.id || !this.client) return;
    const targetName = this.currentTarget?.name || this.currentTarget?.label || this.currentTarget?.model || 'Remote';
    try {
      const key = `${this.currentScope}_${this.currentTarget.id}`;
      let hubData = null;
      try {
        hubData = await this.client.getUiRemoteLayout();
      } catch {
        hubData = { ok: true, layouts: {} };
      }
      if (!hubData || typeof hubData !== 'object') hubData = { ok: true, layouts: {} };
      if (!hubData.layouts || typeof hubData.layouts !== 'object') hubData.layouts = {};

      hubData.layouts[key] = this.activeLayout;
      await this.client.saveUiRemoteLayout(hubData);
      await saveRemoteLayout(this.currentScope, this.currentTarget.id, this.activeLayout);
      await this.triggerHaptic();
      alert(t("layout_saved_hub"));
    } catch (err) {
      alert(t("failed_save_layout") + err.message);
    }
  }

  async restoreLayoutFromHub() {
    if (!this.currentTarget?.id || !this.client) return;
    const targetName = this.currentTarget?.name || this.currentTarget?.label || this.currentTarget?.model || 'Remote';
    const ok = window.showConfirm
      ? await window.showConfirm(`Restore layout for "${targetName}" from Hub default?\n\nThis clears any local override on this device and reloads the Hub's saved layout.`, { title: 'Restore Layout', okText: 'Restore' })
      : true;
    if (!ok) return;

    try {
      const key = `${this.currentScope}_${this.currentTarget.id}`;
      const hubData = await this.client.getUiRemoteLayout();
      const hubLayout = hubData?.layouts?.[key];

      if (hubLayout && Array.isArray(hubLayout.grid) && hubLayout.grid.some(b => b && b.command)) {
        await resetRemoteLayout(this.currentScope, this.currentTarget.id);
        const { sanitized } = this.sanitizeGrid(hubLayout.grid);
        this.activeLayout = { grid: sanitized };
        await saveRemoteLayout(this.currentScope, this.currentTarget.id, this.activeLayout);
        await this.triggerHaptic();
        this.updateDOM();
        alert(t("loaded_hub_layout"));
      } else {
        alert(t("no_saved_layout_hub"));
        await this.resetCurrentLayout(true);
      }
    } catch (err) {
      alert(t("save_failed") + err.message);
    }
  }

  async resetCurrentLayout(skipConfirm = false) {
    const targetName = this.currentTarget?.name || this.currentTarget?.label || (this.currentScope === 'activity' ? 'Activity' : 'Device');
    const msg = `Reset remote for "${targetName}" to default layout based on available buttons?\n\nThis will organize available buttons into D-Pad, Volume, Power in the corner, Back/Home, and Media controls.`;
    if (!skipConfirm) {
      const ok = window.showConfirm
        ? await window.showConfirm(msg, { title: 'Reset Remote Layout', okText: 'Reset' })
        : true;
      if (!ok) return;
    }
    await resetRemoteLayout(this.currentScope, this.currentTarget.id);
    this.activeLayout = this.generateDefaultLayout(
      this.currentScope,
      this.currentTarget,
      this.availableDevices,
      this.deviceCommandsMap
    );
    await saveRemoteLayout(this.currentScope, this.currentTarget.id, this.activeLayout);
    await this.triggerHaptic();
    this.updateDOM();
  }

  async render(container, { scope, target, availableDevices, commandsMap, onSendCommand, iconSvg, selectableDevices, onSelectDevice }) {
    if (this.currentTarget?.id !== target?.id || this.currentScope !== scope) {
      this.selectedKeypadDeviceId = null;
    }

    this.currentScope = scope;
    this.currentTarget = target;
    this.targetIconSvg = iconSvg || '';
    this.availableDevices = availableDevices || [];
    this.selectableDevices = selectableDevices || [];
    this.onSelectDeviceCallback = onSelectDevice || null;
    this.deviceCommandsMap = commandsMap || {};
    this.onSendCallback = onSendCommand;
    this.container = container;

    // Reset keypad tab if numbers not available
    if (this.currentSubTab === 'keypad' && !this.hasNumberCommands()) {
      this.currentSubTab = 'remote';
    }

    // Reset keyboard / mouse tab if bluetooth not available
    if ((this.currentSubTab === 'keyboard' || this.currentSubTab === 'mouse') && !this.hasBluetoothConnection()) {
      this.currentSubTab = 'remote';
    }

    this.activeLayout = await this.loadLayout(scope, target, this.availableDevices, this.deviceCommandsMap);
    this.updateDOM();
  }

  toggleEditMode() {
    this.editMode = !this.editMode;
    this.updateDOM();
  }

  updateDOM() {
    if (!this.container || !this.activeLayout) return;
    this.container.innerHTML = '';

    const root = document.createElement('div');
    root.className = 'remote-canvas';

    // Target Info Header with Icon & Optional Device Dropdown
    if (this.targetIconSvg || (this.selectableDevices && this.selectableDevices.length > 0)) {
      const infoHeader = document.createElement('div');
      infoHeader.className = 'remote-target-info-header';
      infoHeader.style.cssText = 'display:flex; align-items:center; justify-content:space-between; margin-bottom:8px; padding:6px 12px; background:var(--bg-card); border:1px solid var(--border); border-radius:var(--radius-sm);';
      
      if (this.currentScope === 'device' && this.selectableDevices && this.selectableDevices.length > 1) {
        infoHeader.innerHTML = `
          <div style="display:flex; align-items:center; gap:8px; flex:1; min-width:0;">
            <span style="display:inline-flex; align-items:center; color:var(--accent); flex-shrink:0;">${this.targetIconSvg}</span>
            <select class="remote-device-select form-control" style="font-size:14px; font-weight:700; background:transparent; border:none; color:var(--text-main); cursor:pointer; padding:2px 6px; flex:1; min-width:0; box-shadow:none;">
              ${this.selectableDevices.map(d => `
                <option value="${d.id}" ${String(d.id) === String(this.currentTarget?.id) ? 'selected' : ''} style="background:var(--bg-card); color:var(--text-main); font-weight:normal;">
                  ${d.name || d.model || d.id}
                </option>
              `).join('')}
            </select>
          </div>
          <span class="badge subtle" style="font-size:10px; margin-left:8px; flex-shrink:0;">${this.currentTarget?.type || 'Device'}</span>
        `;
        const sel = infoHeader.querySelector('.remote-device-select');
        if (sel && this.onSelectDeviceCallback) {
          sel.addEventListener('change', (e) => {
            this.onSelectDeviceCallback(e.target.value);
          });
        }
      } else {
        infoHeader.innerHTML = `
          <div style="display:flex; align-items:center; gap:8px;">
            <span style="display:inline-flex; align-items:center; color:var(--accent);">${this.targetIconSvg}</span>
            <strong style="font-size:14px;">${this.currentTarget?.name || this.currentTarget?.label || this.currentTarget?.model || 'Remote'}</strong>
          </div>
          <span class="badge subtle" style="font-size:10px;">${this.currentScope === 'activity' ? 'Activity' : (this.currentTarget?.type || 'Device')}</span>
        `;
      }
      root.appendChild(infoHeader);
    }

    const hasNumbers = this.hasNumberCommands();
    const hasBt = this.hasBluetoothConnection();

    // Sub-Tabs Header Bar
    const subtabsBar = document.createElement('div');
    subtabsBar.className = 'remote-subtabs-bar';
    subtabsBar.innerHTML = `
      <div class="remote-subtabs-group">
        <button type="button" class="subtab-btn ${this.currentSubTab === 'remote' ? 'active' : ''}" data-subtab="remote">
          Remote
        </button>
        <button type="button" class="subtab-btn ${this.currentSubTab === 'commands' ? 'active' : ''}" data-subtab="commands" title="All Commands">
          Commands
        </button>
        ${hasNumbers ? `
        <button type="button" class="subtab-btn ${this.currentSubTab === 'keypad' ? 'active' : ''}" data-subtab="keypad">
          Keypad
        </button>
        ` : ''}
        ${hasBt ? `
        <button type="button" class="subtab-btn ${this.currentSubTab === 'keyboard' ? 'active' : ''}" data-subtab="keyboard">
          Keyboard
        </button>
        <button type="button" class="subtab-btn ${this.currentSubTab === 'mouse' ? 'active' : ''}" data-subtab="mouse">
          Mouse
        </button>
        ` : ''}
      </div>
      <div class="remote-subtabs-actions">
        ${this.currentSubTab === 'remote' ? `
          ${this.editMode ? `
          <button type="button" class="subtab-reset-btn" id="btnSaveToHub" title="${t('save_layout_hub_title')}">
            <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"></path>
              <polyline points="17 21 17 13 7 13 7 21"></polyline>
              <polyline points="7 3 7 8 15 8"></polyline>
            </svg>
            <span>${t('save_to_hub')}</span>
          </button>
          <button type="button" class="subtab-reset-btn" id="btnRestoreFromHub" title="${t('restore_layout_hub_title')}">
            <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"></path>
              <polyline points="7 10 12 15 17 10"></polyline>
              <line x1="12" y1="15" x2="12" y2="3"></line>
            </svg>
            <span>${t('hub_default')}</span>
          </button>
          <button type="button" class="subtab-reset-btn" id="btnResetLayout" title="${t('reset_layout_title')}">
            <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8"></path>
              <path d="M3 3v5h5"></path>
            </svg>
            <span>${t('reset_layout')}</span>
          </button>
          ` : ''}
          <button type="button" class="subtab-pencil-btn ${this.editMode ? 'active' : ''}" id="btnToggleEdit" title="${this.editMode ? t('finish_editing') : t('customize_remote_buttons')}">
            <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
              <path d="M12 20h9"></path>
              <path d="M16.5 3.5a2.121 2.121 0 0 1 3 3L7 19l-4 1 1-4L16.5 3.5z"></path>
            </svg>
          </button>
        ` : ''}
      </div>
    `;
    root.appendChild(subtabsBar);

    // Bind sub-tabs events
    subtabsBar.querySelectorAll('.subtab-btn').forEach(btn => {
      btn.addEventListener('click', () => {
        this.currentSubTab = btn.getAttribute('data-subtab');
        this.updateDOM();
      });
    });

    const activeTabEl = subtabsBar.querySelector('.subtab-btn.active');
    if (activeTabEl && typeof activeTabEl.scrollIntoView === 'function') {
      activeTabEl.scrollIntoView({ behavior: 'smooth', block: 'nearest', inline: 'nearest' });
    }

    const editBtn = subtabsBar.querySelector('#btnToggleEdit');
    if (editBtn) {
      editBtn.addEventListener('click', () => this.toggleEditMode());
    }

    const saveHubBtn = subtabsBar.querySelector('#btnSaveToHub');
    if (saveHubBtn) {
      const isRestricted = (window.harmonyApp && window.harmonyApp.userRole === 'user');
      if (isRestricted) {
        saveHubBtn.style.display = 'none';
      } else {
        saveHubBtn.addEventListener('click', () => this.saveCurrentLayoutToHub());
      }
    }

    const restoreHubBtn = subtabsBar.querySelector('#btnRestoreFromHub');
    if (restoreHubBtn) {
      restoreHubBtn.addEventListener('click', () => this.restoreLayoutFromHub());
    }

    const resetBtn = subtabsBar.querySelector('#btnResetLayout');
    if (resetBtn) {
      resetBtn.addEventListener('click', () => this.resetCurrentLayout());
    }

    // View Content based on active subtab
    if (this.currentSubTab === 'remote') {
      root.appendChild(this.renderRemoteGrid());
    } else if (this.currentSubTab === 'commands') {
      root.appendChild(this.renderAllCommandsView());
    } else if (this.currentSubTab === 'keypad') {
      root.appendChild(this.renderKeypadView());
    } else if (this.currentSubTab === 'keyboard') {
      root.appendChild(this.renderKeyboardView());
    } else if (this.currentSubTab === 'mouse') {
      root.appendChild(this.renderMouseView());
    }

    this.container.appendChild(root);
  }

}
