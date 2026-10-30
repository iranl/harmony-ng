// Standalone 5x8 Remote Grid Engine for Harmony Remote v3
import { Haptics, ImpactStyle } from '@capacitor/haptics';
import { getRemoteLayout, saveRemoteLayout, resetRemoteLayout } from '../storage.js';
import { h, escapeHtml } from '../utils/dom.js';
import { t } from '../i18n/index.js';

export class RemoteEngine {
  constructor(apiClient) {
    this.api = apiClient;
    this.editMode = false;
  }

  async triggerHaptic() {
    try {
      await Haptics.impact({ style: ImpactStyle.Light });
    } catch {
      // Browser fallback
    }
  }

  normalizeCmdName(name) {
    return String(name || '')
      .toLowerCase()
      .replace(/\+/g, 'up')
      .replace(/-/g, 'down')
      .replace(/[^a-z0-9]/g, '');
  }

  findCmdInDevice(devId, patterns, commandsMap) {
    if (!devId) return null;
    const cmds = commandsMap[devId] || [];
    if (!Array.isArray(cmds) || cmds.length === 0) return null;

    for (const pat of patterns) {
      const normPat = this.normalizeCmdName(pat);
      for (const c of cmds) {
        const rawName = String(c.name || c);
        if (this.normalizeCmdName(rawName) === normPat) {
          return rawName;
        }
      }
    }
    return null;
  }

  findBestCmd(candidateDevices, patterns, commandsMap) {
    if (!candidateDevices || !Array.isArray(candidateDevices)) return null;
    for (const dev of candidateDevices) {
      if (!dev) continue;
      const devId = typeof dev === 'object' ? dev.id : dev;
      const cmd = this.findCmdInDevice(devId, patterns, commandsMap);
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

  generateDefaultLayout(scope, target, availableDevices = [], commandsMap = {}) {
    const grid = new Array(40).fill(null);

    if (!availableDevices || availableDevices.length === 0) {
      return { scope, targetId: target?.id, targetName: target?.name, grid };
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
        variant,
      };
      assignedKeys.add(`${devId}:${command}`);
      return true;
    };

    const placeCmd = (row, col, candidates, patterns, label, variant = '') => {
      const match = this.findBestCmd(candidates, patterns, commandsMap);
      if (!match) return false;
      if (assignedKeys.has(`${match.deviceId}:${match.command}`)) return false;
      return setSlot(row, col, match.deviceId, match.command, label, variant);
    };

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
      power: ['powertoggle', 'power', 'poweron', 'pwr', 'standby', 'systempower'],
      powerOff: ['poweroff', 'systempoweroff', 'off'],
      input: ['inputnext', 'input', 'source', 'inputsource', 'inputhdmi1', 'hdmi'],
      home: ['home', 'smart', 'hub', 'mainmenu', 'topmenu'],
      menu: ['menu', 'settings', 'setup', 'options', 'tools'],
      exit: ['exit', 'clear', 'cancel', 'close'],
      back: ['back', 'return', 'exit', 'escape', 'esc', 'goback'],
      info: ['info', 'display', 'status', 'osd'],
      guide: ['guide', 'epg', 'livetv', 'tvguide'],
      up: ['directionup', 'cursorup', 'arrowup', 'navup', 'up'],
      down: ['directiondown', 'cursordown', 'arrowdown', 'navdown', 'down'],
      left: ['directionleft', 'cursorleft', 'arrowleft', 'navleft', 'left'],
      right: ['directionright', 'cursorright', 'arrowright', 'navright', 'right'],
      select: ['select', 'ok', 'enter', 'confirm', 'center'],
      volUp: ['volumeup', 'volup', 'volumeplus', 'volplus', 'volume+'],
      volDown: ['volumedown', 'voldown', 'volumeminus', 'volminus', 'volume-'],
      mute: ['mute', 'volumemute', 'mutetoggle'],
      chUp: ['channelup', 'chup', 'channelplus', 'pageup'],
      chDown: ['channeldown', 'chdown', 'channelminus', 'pagedown'],
      prevCh: ['prevchannel', 'previouschannel', 'lastchannel', 'prev', 'recall'],
      rewind: ['rewind', 'rev', 'scanrev', 'skipback', 'previoustrack'],
      play: ['play', 'playpause'],
      pause: ['pause', 'playpause'],
      stop: ['stop'],
      fastForward: ['fastforward', 'ff', 'forward', 'scanfwd', 'skipforward'],
      replay: ['replay', 'jumpback', 'instantreplay'],
      skipFwd: ['skipforward', 'nexttrack', 'jumpforward'],
      record: ['record', 'rec'],
      sub: ['subtitle', 'subtitles', 'cc'],
      audio: ['audio', 'soundtrack', 'audiotrack', 'sound'],
      popup: ['popupmenu', 'popup', 'options', 'quickmenu'],
      dvr: ['dvr', 'list', 'recordedtv'],
      red: ['red', 'colorred'],
      green: ['green', 'colorgreen'],
      yellow: ['yellow', 'coloryellow'],
      blue: ['blue', 'colorblue']
    };

    // Row 0: Power, Input, Home, Guide, Info
    placeCmd(0, 0, uniqueDisplay, PATTERNS.power, 'PWR', 'danger');
    placeCmd(0, 1, uniqueDisplay, PATTERNS.input, 'INPUT');
    placeCmd(0, 2, uniqueNav, PATTERNS.home, 'HOME');
    placeCmd(0, 3, uniqueDisplay, PATTERNS.guide, 'GUIDE');
    if (!placeCmd(0, 4, uniqueDisplay, PATTERNS.powerOff, 'OFF', 'danger')) {
      placeCmd(0, 4, uniqueDisplay, PATTERNS.info, 'INFO');
    }

    // Row 1: Menu, Back, Up, Exit, Info
    placeCmd(1, 0, uniqueNav, PATTERNS.menu, 'MENU');
    placeCmd(1, 1, uniqueNav, PATTERNS.back, 'BACK');
    placeCmd(1, 2, uniqueNav, PATTERNS.up, '▲');
    placeCmd(1, 3, uniqueNav, PATTERNS.exit, 'EXIT');
    if (!grid[4]) placeCmd(1, 4, uniqueDisplay, PATTERNS.info, 'INFO');

    // Row 2: Vol+, Left, OK, Right, Ch+
    placeCmd(2, 0, uniqueAudio, PATTERNS.volUp, 'VOL +');
    placeCmd(2, 1, uniqueNav, PATTERNS.left, '◀');
    placeCmd(2, 2, uniqueNav, PATTERNS.select, 'OK', 'primary');
    placeCmd(2, 3, uniqueNav, PATTERNS.right, '▶');
    placeCmd(2, 4, uniqueDisplay, PATTERNS.chUp, 'CH +');

    // Row 3: Vol-, Replay, Down, Skip, Ch-
    placeCmd(3, 0, uniqueAudio, PATTERNS.volDown, 'VOL -');
    placeCmd(3, 1, uniqueNav, PATTERNS.replay, 'REPLAY');
    placeCmd(3, 2, uniqueNav, PATTERNS.down, '▼');
    placeCmd(3, 3, uniqueNav, PATTERNS.skipFwd, 'SKIP');
    placeCmd(3, 4, uniqueDisplay, PATTERNS.chDown, 'CH -');

    // Row 4: Mute, Audio, Opt, Sub, Prev
    placeCmd(4, 0, uniqueAudio, PATTERNS.mute, 'MUTE', 'warning');
    placeCmd(4, 1, uniqueAudio, PATTERNS.audio, 'AUDIO');
    placeCmd(4, 2, uniqueNav, PATTERNS.popup, 'OPT');
    placeCmd(4, 3, uniqueNav, PATTERNS.sub, 'SUB');
    placeCmd(4, 4, uniqueDisplay, PATTERNS.prevCh, 'PREV');

    // Row 5: Rew, Play, Pause, Stop, FF
    placeCmd(5, 0, uniqueNav, PATTERNS.rewind, 'REW');
    placeCmd(5, 1, uniqueNav, PATTERNS.play, 'PLAY', 'accent');
    placeCmd(5, 2, uniqueNav, PATTERNS.pause, 'PAUSE');
    placeCmd(5, 3, uniqueNav, PATTERNS.stop, 'STOP');
    placeCmd(5, 4, uniqueNav, PATTERNS.fastForward, 'FF');

    // Row 6: Rec, DVR
    placeCmd(6, 0, uniqueNav, PATTERNS.record, 'REC', 'danger');
    placeCmd(6, 4, uniqueNav, PATTERNS.dvr, 'DVR');

    // Row 7: Red, Green, Yellow, Blue
    placeCmd(7, 0, uniqueDisplay, PATTERNS.red, 'RED', 'danger');
    placeCmd(7, 1, uniqueDisplay, PATTERNS.green, 'GREEN', 'accent');
    placeCmd(7, 2, uniqueDisplay, PATTERNS.yellow, 'YELLOW', 'warning');
    placeCmd(7, 3, uniqueDisplay, PATTERNS.blue, 'BLUE', 'primary');

    // Fallback fill for device mode
    if (scope === 'device' && target) {
      const devCmds = commandsMap[target.id] || [];
      const unassigned = devCmds.filter(c => {
        const rawName = String(c.name || c);
        return !assignedKeys.has(`${target.id}:${rawName}`) && !/^(number|digit)?[0-9]$/i.test(rawName);
      });
      const fillSlots = [31, 32, 33, 39, 21, 22, 23, 16, 18];
      let uIdx = 0;
      for (const slotIdx of fillSlots) {
        if (!grid[slotIdx] && uIdx < unassigned.length) {
          const c = unassigned[uIdx++];
          const rawName = String(c.name || c);
          setSlot(Math.floor(slotIdx / 5), slotIdx % 5, target.id, rawName, rawName);
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
    if (!target || !target.id) return null;
    let saved = await getRemoteLayout(scope, target.id);
    if (saved && Array.isArray(saved.grid) && saved.grid.length === 40) {
      return saved;
    }
    const defaultLayout = this.generateDefaultLayout(scope, target, availableDevices, commandsMap);
    await saveRemoteLayout(scope, target.id, defaultLayout);
    return defaultLayout;
  }

  renderGrid(layout, onCommandClick, onSlotEdit) {
    const gridEl = h('div', { className: 'remote-grid' });
    const grid = layout?.grid || new Array(40).fill(null);

    grid.forEach((btn, idx) => {
      if (!btn) {
        if (this.editMode) {
          const emptyBtn = h('button', {
            className: 'remote-btn remote-btn-empty edit-slot',
            text: '+',
            onclick: () => onSlotEdit && onSlotEdit(idx, null),
          });
          gridEl.appendChild(emptyBtn);
        } else {
          gridEl.appendChild(h('div', { className: 'remote-slot-empty' }));
        }
        return;
      }

      const variantClass = btn.variant ? `remote-btn-${btn.variant}` : '';
      const buttonEl = h('button', {
        className: `remote-btn ${variantClass} ${this.editMode ? 'in-edit-mode' : ''}`,
        onclick: async (e) => {
          e.preventDefault();
          this.triggerHaptic();
          if (this.editMode) {
            if (onSlotEdit) onSlotEdit(idx, btn);
          } else {
            if (onCommandClick) onCommandClick(btn.deviceId, btn.command);
          }
        },
      }, [
        h('span', { className: 'btn-label', text: btn.label || btn.command }),
      ]);

      gridEl.appendChild(buttonEl);
    });

    return gridEl;
  }
}

export default RemoteEngine;
