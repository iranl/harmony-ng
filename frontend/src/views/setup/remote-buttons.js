// Harmony remote key bindings: a drawing of the remote whose keys are hotspots, with the
// remote's screen in it, and an editor for what each key does per activity and per device.
// The Hub resolves a key press against remote_slotN_map.json (rf_keymap.c
// dispatch_remote_button); resolveBinding() below follows the same order.
import { h } from '../../utils/dom.js';
import { api } from '../../api/index.js';
import { store } from '../../store.js';
import { toast } from '../../components/toast.js';
import { confirm } from '../../components/confirm.js';
import { loadRemoteRenderer, setRendererTheme } from '../../remote/remote-renderer.js';

const SVG_NS = 'http://www.w3.org/2000/svg';

// Drawing units: traced at 4x from a 160x515 front photo of the remote
const VIEW = { x: 30, y: 0, w: 580, h: 1900 };
const SCREEN = { x: 118, y: 192, w: 398, h: 532 };

const pad = (x, y, w, hgt, r = 12) => ({ rect: [x, y, w, hgt, r] });
const poly = (...pts) => ({ poly: pts });

// The keys the remote sends to the Hub, by the names map_button_code() gives them.
// Ha1..Ha4 (KEY_PROG1..4) run left to right, then top to bottom: lights left, plugs right.
const KEYS = [
  { id: 'PowerOffActivity', label: 'Off', ...pad(125, 88, 92, 30, 15), text: 'Off', size: 18 },
  { id: 'Rewind', label: 'Rewind', ...pad(97, 950, 145, 50), glyph: 'rew' },
  { id: 'Play', label: 'Play', ...pad(276, 948, 80, 66, 10), glyph: 'play' },
  { id: 'FastForward', label: 'Fast forward', ...pad(392, 950, 143, 50), glyph: 'ff' },
  { id: 'Record', label: 'Record', ...pad(97, 1032, 145, 50), glyph: 'rec' },
  { id: 'Pause', label: 'Pause', ...pad(276, 1018, 80, 66, 10), glyph: 'pause' },
  { id: 'Stop', label: 'Stop', ...pad(392, 1032, 143, 50), glyph: 'stop' },
  { id: 'Exit', label: 'Exit', ...poly([97, 1112], [270, 1112], [234, 1165], [97, 1165]), text: 'Exit', at: [168, 1139] },
  { id: 'Menu', label: 'Menu', ...poly([535, 1112], [362, 1112], [398, 1165], [535, 1165]), text: 'Menu', at: [464, 1139] },
  { id: 'VolumeUp', label: 'Volume up', ...poly([95, 1195], [205, 1195], [158, 1298], [95, 1298]), text: 'Vol +', at: [134, 1250] },
  { id: 'VolumeDown', label: 'Volume down', ...poly([95, 1306], [158, 1306], [205, 1410], [95, 1410]), text: 'Vol −', at: [134, 1356] },
  { id: 'ChannelUp', label: 'Channel / page up', ...poly([537, 1195], [427, 1195], [474, 1298], [537, 1298]), text: 'Ch +', at: [498, 1250] },
  { id: 'ChannelDown', label: 'Channel / page down', ...poly([537, 1306], [474, 1306], [427, 1410], [537, 1410]), text: 'Ch −', at: [498, 1356] },
  { id: 'DirectionUp', label: 'Up', ...poly([316, 1300], [252, 1232], [316, 1165], [381, 1232]), glyph: 'up', at: [316, 1200] },
  { id: 'DirectionRight', label: 'Right', ...poly([316, 1300], [381, 1232], [446, 1300], [381, 1367]), glyph: 'right', at: [412, 1300] },
  { id: 'DirectionDown', label: 'Down', ...poly([316, 1300], [381, 1367], [316, 1435], [252, 1367]), glyph: 'down', at: [316, 1400] },
  { id: 'DirectionLeft', label: 'Left', ...poly([316, 1300], [252, 1367], [188, 1300], [252, 1232]), glyph: 'left', at: [220, 1300] },
  { id: 'Select', label: 'OK', circle: [316, 1300, 56], text: 'OK', size: 26 },
  { id: 'VolumeMute', label: 'Mute', ...poly([97, 1438], [234, 1438], [270, 1490], [97, 1490]), glyph: 'mute', at: [168, 1464] },
  { id: 'Back', label: 'Back', ...poly([535, 1438], [398, 1438], [362, 1490], [535, 1490]), glyph: 'back', at: [464, 1464] },
  { id: 'Dvr', label: 'DVR', ...pad(97, 1532, 141, 53, 10), text: 'DVR' },
  { id: 'Guide', label: 'Guide', ...pad(247, 1532, 140, 53, 10), text: 'Guide' },
  { id: 'Info', label: 'Info', ...pad(396, 1532, 139, 53, 10), text: 'Info' },
  { id: 'Red', label: 'Red', ...pad(97, 1618, 86, 44), bar: '#ef4444' },
  { id: 'Green', label: 'Green', ...pad(215, 1618, 85, 44), bar: '#22c55e' },
  { id: 'Yellow', label: 'Yellow', ...pad(332, 1618, 86, 44), bar: '#facc15' },
  { id: 'Blue', label: 'Blue', ...pad(450, 1618, 85, 44), bar: '#3b82f6' },
  { id: 'Ha1', label: 'Light 1', ...pad(97, 1690, 145, 52), glyph: 'bulb' },
  { id: 'RockerUp', label: 'Dimmer +', ...pad(276, 1690, 80, 66, 10), text: '+', size: 30 },
  { id: 'Ha2', label: 'Plug 1', ...pad(392, 1690, 143, 52), glyph: 'plug' },
  { id: 'Ha3', label: 'Light 2', ...pad(97, 1772, 145, 52), glyph: 'bulb' },
  { id: 'RockerDown', label: 'Dimmer −', ...pad(276, 1760, 80, 66, 10), text: '−', size: 30 },
  { id: 'Ha4', label: 'Plug 2', ...pad(392, 1772, 143, 52), glyph: 'plug' },
];
const KEY_BY_ID = Object.fromEntries(KEYS.map(k => [k.id, k]));

// The two capacitive keys under the screen stay on the remote
const NAV_KEYS = [
  { id: 'activities', text: 'ACTIVITIES', rect: [110, 848, 142, 45, 4] },
  { id: 'devices', text: 'DEVICES', rect: [378, 848, 142, 45, 4] },
];

function svg(tag, attrs = {}, children = []) {
  const el = document.createElementNS(SVG_NS, tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === 'text') el.textContent = v;
    else el.setAttribute(k, String(v));
  }
  children.forEach(c => c && el.appendChild(c));
  return el;
}

function keyCenter(k) {
  if (k.at) return k.at;
  if (k.circle) return [k.circle[0], k.circle[1]];
  const [x, y, w, hgt] = k.rect;
  return [x + w / 2, y + hgt / 2];
}

function keyShape(k) {
  if (k.circle) return svg('circle', { cx: k.circle[0], cy: k.circle[1], r: k.circle[2] });
  if (k.poly) return svg('polygon', { points: k.poly.map(p => p.join(',')).join(' '), 'stroke-linejoin': 'round' });
  const [x, y, w, hgt, r] = k.rect;
  return svg('rect', { x, y, width: w, height: hgt, rx: r });
}

// Key legends drawn as shapes, so they look the same in every browser
function glyph(name, cx, cy) {
  const g = svg('g', { class: 'eb-glyph', transform: `translate(${cx} ${cy})` });
  const tri = (dx, s, dir) => svg('polygon', { points: dir > 0 ? `${dx - s},${-s} ${dx + s},0 ${dx - s},${s}` : `${dx + s},${-s} ${dx - s},0 ${dx + s},${s}` });
  const add = (...els) => els.forEach(e => g.appendChild(e));
  switch (name) {
    case 'play': add(tri(2, 11, 1)); break;
    case 'pause': add(svg('rect', { x: -9, y: -11, width: 6, height: 22 }), svg('rect', { x: 3, y: -11, width: 6, height: 22 })); break;
    case 'stop': add(svg('rect', { x: -10, y: -10, width: 20, height: 20, rx: 2 })); break;
    case 'rec': add(svg('circle', { r: 11, class: 'eb-glyph-rec' })); break;
    case 'rew': add(svg('rect', { x: -24, y: -10, width: 4, height: 20 }), tri(-8, 10, -1), tri(10, 10, -1)); break;
    case 'ff': add(tri(-10, 10, 1), tri(8, 10, 1), svg('rect', { x: 20, y: -10, width: 4, height: 20 })); break;
    case 'up': add(svg('polygon', { points: '-12,7 12,7 0,-8' })); break;
    case 'down': add(svg('polygon', { points: '-12,-7 12,-7 0,8' })); break;
    case 'left': add(svg('polygon', { points: '7,-12 7,12 -8,0' })); break;
    case 'right': add(svg('polygon', { points: '-7,-12 -7,12 8,0' })); break;
    case 'mute':
      add(svg('polygon', { points: '-16,-6 -8,-6 2,-15 2,15 -8,6 -16,6' }),
        svg('line', { x1: 8, y1: -9, x2: 22, y2: 9, class: 'eb-glyph-line' }),
        svg('line', { x1: 22, y1: -9, x2: 8, y2: 9, class: 'eb-glyph-line' }));
      break;
    case 'back':
      add(svg('path', { d: 'M -14 -2 H 10 A 9 9 0 0 1 10 16 H -2', class: 'eb-glyph-line' }),
        svg('polygon', { points: '-20,-2 -10,-11 -10,7' }));
      break;
    case 'bulb':
      add(svg('circle', { cy: -6, r: 11, class: 'eb-glyph-line' }),
        svg('rect', { x: -6, y: 7, width: 12, height: 9, rx: 2 }));
      break;
    case 'plug':
      add(svg('rect', { x: -14, y: -14, width: 28, height: 28, rx: 8, class: 'eb-glyph-line' }),
        svg('rect', { x: -7, y: -7, width: 4, height: 9 }),
        svg('rect', { x: 3, y: -7, width: 4, height: 9 }));
      break;
    default: break;
  }
  return g;
}

function keyLegend(k) {
  const [cx, cy] = keyCenter(k);
  if (k.bar) {
    return svg('rect', { x: cx - 26, y: cy - 6, width: 52, height: 12, rx: 6, fill: 'none', stroke: k.bar, 'stroke-width': 4 });
  }
  if (k.glyph) return glyph(k.glyph, cx, cy);
  return svg('text', { x: cx, y: cy, class: 'eb-label', 'font-size': k.size || 20, text: k.text });
}

// The Hub falls back to these when the running activity doesn't bind a key
function defaultBindings(acts) {
  return acts['-1'] ?? acts.default ?? acts['53591842'] ?? Object.values(acts)[0];
}

function actionOf(b) {
  const a = b.action || b.type || '';
  if (a === 'device_cmd' || a === 'command' || (!a && b.command && (b.deviceId || b.targetDevice))) return 'command';
  if (a === 'activity_start' || a === 'change_activity') return 'activity';
  if (a === 'activity_stop' || a === 'poweroff') return 'poweroff';
  return a === 'none' ? 'none' : `unsupported:${a}`;
}

export function createButtonEditor() {
  let slot = 1;
  let map = { activities: {}, devices: {} };
  let devices = [];
  let ctx = 'a:-1';
  let selected = 'VolumeUp';
  let dirty = false;
  let screenView = null; // a screen picked with the ACTIVITIES / DEVICES keys; null follows the context
  let listTop = null;     // the screen's list scrolled this many remote pixels; null opens it on its selection
  let homeSelect = 0;     // an activity on the home page shown
  let shown = { view: '', listTop: 0, homePageSize: 5 };

  const activities = () => (store.get('activities.list') || []).filter(a => String(a.id) !== '-1');
  const deviceById = id => devices.find(d => String(d.id) === String(id));
  const deviceName = id => deviceById(id)?.name || `Device ${id}`;
  const activityName = id => activities().find(a => String(a.id) === String(id))?.name || `Activity ${id}`;
  const ctxKind = () => ctx.slice(0, 1);
  const ctxId = () => ctx.slice(2);
  const ctxName = () => (ctxKind() === 'd' ? deviceName(ctxId()) : ctxId() === '-1' ? 'No activity' : activityName(ctxId()));

  function describe(b) {
    switch (actionOf(b)) {
      case 'command': return `${deviceName(b.deviceId || b.targetDevice)} › ${b.command}`;
      case 'activity': return `Start ${activityName(b.activityId || b.targetActivity || b.deviceId)}`;
      case 'poweroff': return 'Turn everything off';
      case 'none': return 'Nothing';
      default: return `Not supported by the Hub (${b.action || b.type})`;
    }
  }

  // What a key press does in the current context, and where that comes from
  function resolveBinding(keyId) {
    if (keyId === 'PowerOffActivity') return { source: 'fixed', text: 'Turn everything off' };
    const acts = map.activities || {};
    if (ctxKind() === 'd') {
      const own = map.devices?.[ctxId()]?.[keyId];
      if (own?.command) return { source: 'here', binding: own, text: `${deviceName(ctxId())} › ${own.command}` };
      const same = sameNameCommand(ctxId(), keyId);
      if (same) return { source: 'name', text: `${deviceName(ctxId())} › ${same}` };
      return { source: 'none', text: 'Nothing' };
    }
    const own = acts[ctxId()]?.[keyId];
    if (own && ctxId() !== '-1') return { source: 'here', binding: own, text: describe(own) };
    const def = defaultBindings(acts)?.[keyId];
    if (def) return { source: ctxId() === '-1' && acts['-1'] ? 'here' : 'default', binding: def, text: describe(def) };
    return { source: 'none', text: 'Nothing' };
  }

  // The device's command named like the key, which the Hub sends when nothing is bound
  function sameNameCommand(devId, keyId) {
    const cmd = (deviceById(devId)?.commands || []).find(c => (c.name || c).toLowerCase() === keyId.toLowerCase());
    return cmd ? (cmd.name || cmd) : null;
  }

  function setBinding(keyId, binding) {
    const kind = ctxKind() === 'd' ? 'devices' : 'activities';
    map[kind] = map[kind] || {};
    const entry = (map[kind][ctxId()] = map[kind][ctxId()] || {});
    if (binding) entry[keyId] = binding;
    else delete entry[keyId];
    dirty = true;
    refresh();
  }

  // ── Picture ──
  const hotspots = {};
  const picture = svg('svg', {
    viewBox: `${VIEW.x} ${VIEW.y} ${VIEW.w} ${VIEW.h}`,
    class: 'eb-svg',
    role: 'group',
    'aria-label': 'Harmony remote keys',
  }, [
    svg('rect', { x: 40, y: 10, width: 560, height: 1880, rx: 130, class: 'eb-body' }),
    svg('rect', { x: SCREEN.x - 4, y: SCREEN.y - 4, width: SCREEN.w + 8, height: SCREEN.h + 8, rx: 6, class: 'eb-screen' }),
  ]);
  KEYS.forEach((k) => {
    const g = svg('g', { class: 'eb-key', tabindex: 0, role: 'button', 'data-key': k.id }, [keyShape(k), keyLegend(k)]);
    const title = svg('title');
    g.appendChild(title);
    g.addEventListener('click', () => select(k.id));
    g.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); select(k.id); }
    });
    hotspots[k.id] = { g, title };
    picture.appendChild(g);
  });
  NAV_KEYS.forEach((k) => {
    const [x, y, w, hgt, r] = k.rect;
    const g = svg('g', { class: 'eb-nav', tabindex: 0, role: 'button' }, [
      svg('rect', { x, y, width: w, height: hgt, rx: r }),
      svg('text', { x: x + w / 2, y: y + hgt / 2, class: 'eb-label', 'font-size': 17, text: k.text }),
      svg('title', { text: `Shows the remote's ${k.text.toLowerCase()} screen; this key stays on the remote` }),
    ]);
    g.addEventListener('click', () => { screenView = k.id; showScreen(); });
    picture.appendChild(g);
  });

  const screenCanvas = h('canvas', {
    width: 240,
    height: 320,
    className: 'eb-canvas',
    style: {
      left: `${((SCREEN.x - VIEW.x) / VIEW.w) * 100}%`,
      top: `${((SCREEN.y - VIEW.y) / VIEW.h) * 100}%`,
      width: `${(SCREEN.w / VIEW.w) * 100}%`,
      height: `${(SCREEN.h / VIEW.h) * 100}%`,
    },
  });
  const pictureWrap = h('div', { className: 'eb-picture', style: { aspectRatio: `${VIEW.w} / ${VIEW.h}` } }, [picture, screenCanvas]);

  // The screen works like the remote's: tapping an activity or a device shows its screen and
  // edits its bindings; the wheel scrolls a list or turns the home page
  const screenPoint = (e) => {
    const r = screenCanvas.getBoundingClientRect();
    return [(e.clientX - r.left) * 240 / r.width, (e.clientY - r.top) * 320 / r.height];
  };
  function openHit(hit) {
    let next = null;
    if (hit?.activity) {
      const a = activities().find(act => loadedPreview?.remoteActivityId(act.id) === hit.activity);
      if (a) next = `a:${a.id}`;
    } else if (hit && hit.device >= 0 && devices[hit.device]) {
      next = `d:${devices[hit.device].id}`;
    }
    if (!next) return;
    ctx = next;
    ctxSelect.value = ctx;
    screenView = null;
    refresh();
    showScreen();
  }
  screenCanvas.addEventListener('click', (e) => {
    const [x, y] = screenPoint(e);
    drawScreen(({ preview, module }) => openHit(preview.hitTest(module, x, y)));
  });
  screenCanvas.addEventListener('mousemove', async (e) => {
    const renderer = await loadRemoteRenderer();
    if (!renderer) return;
    const [x, y] = screenPoint(e);
    const hit = renderer.preview.hitTest(renderer.module, x, y);
    if (hit === undefined) { drawScreen(); return; } // the icon thumbnails used the module since
    screenCanvas.style.cursor = hit ? 'pointer' : '';
  });
  let wheelTurn = 0;
  screenCanvas.addEventListener('wheel', (e) => {
    const step = e.deltaMode === 1 ? 16 : e.deltaMode === 2 ? 280 : 1; // lines, pages, pixels
    const dy = e.deltaY * step * 320 / screenCanvas.getBoundingClientRect().height;
    if (shown.view === 'activities') {
      const count = activities().length, size = shown.homePageSize;
      wheelTurn += dy;
      if (Math.abs(wheelTurn) < 40) { e.preventDefault(); return; }
      const page = Math.floor(homeSelect / size) + Math.sign(wheelTurn);
      wheelTurn = 0;
      if (page < 0 || page * size >= count) return;
      e.preventDefault();
      homeSelect = page * size;
      drawScreen();
    } else if (['commands', 'devices', 'device'].includes(shown.view)) {
      e.preventDefault();
      listTop = Math.max(0, shown.listTop + dy);
      drawScreen();
    }
  }, { passive: false });

  // ── Header: context, save ──
  const ctxSelect = h('select', {
    className: 'form-select',
    'aria-label': 'Bindings for',
    onchange: () => { ctx = ctxSelect.value; screenView = null; refresh(); showScreen(); },
  });
  const dirtyBadge = h('span', { className: 'badge badge-accent', text: 'Unsaved changes', style: { display: 'none' } });
  const revertBtn = h('button', {
    className: 'btn btn-secondary btn-sm',
    text: 'Revert',
    onclick: () => load(slot),
  });
  const otherSlot = () => (slot === 1 ? 2 : 1);
  const copyBtn = h('button', {
    className: 'btn btn-secondary btn-sm',
    onclick: async () => {
      const from = otherSlot();
      const ok = await confirm.ask({
        title: 'Copy Remote Mappings',
        message: `Copy all button mappings from Slot ${from} to Slot ${slot}? Any existing mappings on Slot ${slot} will be overwritten.`,
        danger: true,
      });
      if (!ok) return;
      try {
        await api.copyRFMapping(from, slot);
        toast.success(`Copied mappings from Slot ${from} to Slot ${slot}`);
        load(slot);
      } catch (e) {
        toast.error(e.message);
      }
    },
  });
  const saveBtn = h('button', {
    className: 'btn btn-primary btn-sm',
    text: 'Save to Hub',
    onclick: async () => {
      try {
        await api.saveRFMapping(map, slot);
        dirty = false;
        refresh();
        toast.success('Saved. The Hub uses the new bindings from the next key press.');
      } catch (e) {
        toast.error(e.message);
      }
    },
  });
  const title = h('h4', { className: 'card-title', text: 'Buttons' });

  // ── Theme and home layout: the remote's look, which the Hub sets in its settings push ──
  const themeSelect = h('select', {
    className: 'form-select',
    'aria-label': 'Remote theme',
    onchange: () => setTheme(themeSelect.value),
  });
  const layoutSelect = h('select', {
    className: 'form-select',
    'aria-label': 'Home layout',
    onchange: () => sendRemoteSetting({ layout: layoutSelect.value }, 'Home layout').then(() => showScreen()),
  }, [
    h('option', { value: 'theme', text: "The theme's" }),
    h('option', { value: 'list', text: 'List' }),
    h('option', { value: 'grid', text: 'Grid' }),
    h('option', { value: 'carousel', text: 'Carousel' }),
  ]);
  const remoteConfigPath = () => `/api/rf/remote-config?slot=${slot}`;
  async function loadTheme() {
    const renderer = await loadRemoteRenderer();
    if (!renderer) return;
    const [list, cfg] = await Promise.all([renderer.preview.themeList(), api.request(remoteConfigPath()).catch(() => null)]);
    themeSelect.replaceChildren(...list.map(th => h('option', { value: th.id, text: th.name })));
    themeSelect.value = list.some(th => th.id === cfg?.theme) ? cfg.theme : 'classic';
    layoutSelect.value = ['list', 'grid', 'carousel'].includes(cfg?.layout) ? cfg.layout : 'theme';
    await setRendererTheme(renderer, themeSelect.value);
    drawScreen();
  }
  async function sendRemoteSetting(change, label) {
    try {
      // The Hub rebuilds the remote's settings from the whole object
      const cfg = await api.request(remoteConfigPath());
      await api.request(remoteConfigPath(), { method: 'POST', body: JSON.stringify({ ...cfg, ...change }) });
      toast.success(`${label} sent to the remote.`);
    } catch (e) {
      toast.error(e.message);
    }
  }
  async function setTheme(name) {
    await sendRemoteSetting({ theme: name }, 'Theme');
    const renderer = await loadRemoteRenderer();
    if (renderer) await setRendererTheme(renderer, name);
    drawScreen();
  }

  // ── Inspector: the selected key ──
  const keyPanel = h('div', { className: 'eb-key-panel' });

  function ctxOptions() {
    const opt = (value, text) => h('option', { value, text });
    return [
      h('optgroup', { label: 'Activities' }, [
        ...activities().map(a => opt(`a:${a.id}`, a.name)),
        opt('a:-1', 'No activity (defaults for all activities)'),
      ]),
      h('optgroup', { label: 'Devices (on the remote\'s Devices screen)' }, devices.map(d => opt(`d:${d.id}`, d.name))),
    ];
  }

  function sourceBadge(source) {
    const [cls, text] = {
      here: ['badge-success', ctxKind() === 'd' ? 'This device' : ctxId() === '-1' ? 'Default' : 'This activity'],
      default: ['badge-accent', 'Inherited'],
      name: ['badge-accent', 'Same name'],
      fixed: ['', 'Fixed'],
      none: ['', 'Unbound'],
    }[source];
    return h('span', { className: `badge ${cls}`, text });
  }

  function commandOptions(dev, current) {
    return sortedCommands(dev).map(c => h('option', { value: c, text: c, selected: c === current }));
  }

  function sortedCommands(dev) {
    return (dev?.commands || []).map(c => c.name || c).sort((a, b) => a.localeCompare(b, undefined, { sensitivity: 'base' }));
  }

  // A first device command for a key: the current binding's, else the first device in the
  // activity (then any device) with a command named like the key, else the device this
  // activity's other keys use most
  function suggestCommand(keyId, binding) {
    if (binding?.command) return { deviceId: String(binding.deviceId || binding.targetDevice), command: binding.command };
    const act = activities().find(a => String(a.id) === ctxId());
    const order = [...(act?.deviceIds || []).map(String), ...devices.map(d => String(d.id))];
    const uses = {};
    Object.values(map.activities?.[ctxId()] || {}).forEach((b) => {
      if (b?.deviceId) uses[b.deviceId] = (uses[b.deviceId] || 0) + 1;
    });
    const mostUsed = Object.keys(uses).sort((a, b) => uses[b] - uses[a])[0];
    const devId = order.find(id => sameNameCommand(id, keyId)) || mostUsed || order[0] || '';
    return { deviceId: devId, command: sameNameCommand(devId, keyId) || sortedCommands(deviceById(devId))[0] || '' };
  }

  // The binding the current activity or device sets for a key itself
  function ownBinding(keyId) {
    return ctxKind() === 'd' ? map.devices?.[ctxId()]?.[keyId] : map.activities?.[ctxId()]?.[keyId];
  }

  // What a key falls back to once its own binding is removed
  function inheritedText(keyId) {
    if (ctxKind() === 'd') {
      const same = sameNameCommand(ctxId(), keyId);
      return same ? `${deviceName(ctxId())} › ${same}` : null;
    }
    if (ctxId() === '-1') return null;
    const def = defaultBindings(map.activities || {})?.[keyId];
    return def ? describe(def) : null;
  }

  function renderKeyPanel() {
    const k = KEY_BY_ID[selected];
    const r = resolveBinding(selected);
    const rows = [
      h('div', { className: 'eb-key-head' }, [h('strong', { text: k.label }), h('code', { text: k.id }), sourceBadge(r.source)]),
      h('p', { className: `eb-key-now${r.source === 'default' || r.source === 'name' ? ' eb-inherited' : ''}`, text: r.text }),
    ];
    if (r.source === 'default') {
      rows.push(h('p', { className: 'text-dim eb-hint', text: 'Inherited from the No activity bindings. Set a binding to override it here.' }));
    } else if (r.source === 'name') {
      rows.push(h('p', { className: 'text-dim eb-hint', text: 'Inherited: the Hub sends the device\'s command of the same name. Set a binding to override it.' }));
    }
    if (r.source === 'fixed') {
      rows.push(h('p', { className: 'text-dim eb-hint', text: 'The Hub always turns everything off for this key.' }));
      keyPanel.replaceChildren(...rows);
      return;
    }

    const isDevice = ctxKind() === 'd';
    const isDefaults = !isDevice && ctxId() === '-1';
    const own = isDevice ? map.devices?.[ctxId()]?.[selected] : map.activities?.[ctxId()]?.[selected];
    const mode = own ? (isDevice ? 'command' : actionOf(own)) : (isDefaults ? 'none' : 'inherit');

    const fallback = isDevice ? sameNameCommand(ctxId(), selected) : (isDefaults ? null : defaultBindings(map.activities || {})?.[selected]);
    const modes = isDevice
      ? [['inherit', `Use the command of the same name${fallback ? ` (${fallback})` : ' (none)'}`], ['command', 'Send a command']]
      : [
        ...(isDefaults ? [] : [['inherit', `Use the default${fallback ? ` (${describe(fallback)})` : ' (nothing)'}`]]),
        ['command', 'Send a device command'],
        ['activity', 'Start an activity'],
        ['poweroff', 'Turn everything off'],
        ['none', isDefaults ? 'Nothing' : 'Nothing (ignore the default)'],
      ];
    if (mode.startsWith('unsupported:')) modes.push([mode, describe(own)]);

    const modeSelect = h('select', {
      className: 'form-select',
      'aria-label': `When ${k.label} is pressed`,
      onchange: () => {
        const m = modeSelect.value;
        if (m === 'inherit' || (m === 'none' && isDefaults)) setBinding(selected, null);
        else if (m === 'command') {
          const s = isDevice
            ? { deviceId: ctxId(), command: sameNameCommand(ctxId(), selected) || sortedCommands(deviceById(ctxId()))[0] || '' }
            : suggestCommand(selected, own);
          setBinding(selected, { type: 'command', action: 'device_cmd', deviceId: s.deviceId, command: s.command });
        } else if (m === 'activity') {
          setBinding(selected, { type: 'activity_start', action: 'activity_start', activityId: String(activities()[0]?.id || '') });
        } else if (m === 'poweroff') setBinding(selected, { type: 'activity_stop', action: 'activity_stop' });
        else if (m === 'none') setBinding(selected, { type: 'none', action: 'none' });
      },
    }, modes.map(([v, text]) => h('option', { value: v, text, selected: v === mode })));

    rows.push(h('label', { className: 'eb-field' }, [h('span', { text: `When pressed${isDevice ? ` on ${ctxName()}` : ` in ${ctxName()}`}` }), modeSelect]));

    if (mode === 'command') {
      const devId = isDevice ? ctxId() : String(own.deviceId || own.targetDevice || '');
      const update = (patch) => setBinding(selected, { type: 'command', action: 'device_cmd', deviceId: devId, command: own.command, ...patch });
      if (!isDevice) {
        const act = activities().find(a => String(a.id) === ctxId());
        const inAct = new Set((act?.deviceIds || []).map(String));
        const devOpt = d => h('option', { value: String(d.id), text: d.name, selected: String(d.id) === devId });
        const devSelect = h('select', {
          className: 'form-select',
          'aria-label': 'Device',
          onchange: () => {
            const dev = deviceById(devSelect.value);
            const keep = dev?.commands?.some(c => (c.name || c) === own.command) ? own.command : null;
            update({ deviceId: devSelect.value, command: keep || sameNameCommand(devSelect.value, selected) || sortedCommands(dev)[0] || '' });
          },
        }, inAct.size
          ? [h('optgroup', { label: `In ${ctxName()}` }, devices.filter(d => inAct.has(String(d.id))).map(devOpt)),
            h('optgroup', { label: 'Other devices' }, devices.filter(d => !inAct.has(String(d.id))).map(devOpt))]
          : devices.map(devOpt));
        rows.push(h('label', { className: 'eb-field' }, [h('span', { text: 'Device' }), devSelect]));
      }
      const cmdSelect = h('select', {
        className: 'form-select',
        'aria-label': 'Command',
        onchange: () => update({ command: cmdSelect.value }),
      }, commandOptions(deviceById(devId), own.command));
      rows.push(h('label', { className: 'eb-field' }, [h('span', { text: 'Command' }), cmdSelect]));
    } else if (mode === 'activity') {
      const actSelect = h('select', {
        className: 'form-select',
        'aria-label': 'Activity',
        onchange: () => setBinding(selected, { type: 'activity_start', action: 'activity_start', activityId: actSelect.value }),
      }, activities().map(a => h('option', { value: String(a.id), text: a.name, selected: String(a.id) === String(own.activityId || own.targetActivity) })));
      rows.push(h('label', { className: 'eb-field' }, [h('span', { text: 'Activity' }), actSelect]));
    }
    if (own) {
      const after = inheritedText(selected);
      rows.push(h('div', { className: 'eb-remove-row' }, [
        h('button', { className: 'btn btn-secondary btn-sm', text: 'Remove binding', onclick: () => setBinding(selected, null) }),
        h('span', { className: 'text-dim', text: after ? `Then inherits ${after}` : 'Then does nothing' }),
      ]));
    }
    keyPanel.replaceChildren(...rows);
  }

  function refresh() {
    title.textContent = `Buttons (Slot ${slot})`;
    copyBtn.textContent = `📋 Copy from Slot ${otherSlot()}`;
    dirtyBadge.style.display = dirty ? '' : 'none';
    KEYS.forEach((k) => {
      const r = resolveBinding(k.id);
      const { g, title: t } = hotspots[k.id];
      g.setAttribute('class', `eb-key eb-${r.source}${k.id === selected ? ' eb-selected' : ''}`);
      g.setAttribute('aria-label', `${k.label}: ${r.text}`);
      t.textContent = `${k.label}: ${r.text}`;
    });
    renderKeyPanel();
  }

  function select(keyId) {
    selected = keyId;
    refresh();
  }

  // ── Screen: the remote's own drawing code, from the config the Hub pushes to it ──
  // A new screen: its list opens on its selection and home on its first page
  function showScreen() {
    listTop = null;
    homeSelect = 0;
    drawScreen();
  }

  // then(renderer) runs straight after the render, while the module still holds the screen
  let loadedPreview = null;
  async function drawScreen(then) {
    const renderer = await loadRemoteRenderer();
    if (!renderer) return;
    loadedPreview = renderer.preview;
    const { preview, module } = renderer;
    const pushed = devices.slice(0, 16); // the push sends the first 16
    preview.writeRemoteConfig(module, { activities: activities(), devices: pushed, remoteMap: map });

    const running = String(store.get('activities.currentId') || '-1');
    const state = { view: 'activities', select: homeSelect, active: running === '-1' ? '' : preview.remoteActivityId(running) };
    if (screenView === 'devices' || (!screenView && ctxKind() === 'd')) {
      const idx = pushed.findIndex(d => String(d.id) === ctxId());
      if (!screenView && idx >= 0) { state.view = 'device'; state.select = idx; } else state.view = 'devices';
    } else if (!screenView && ctxId() !== '-1') {
      state.view = 'commands';
      state.active = preview.remoteActivityId(ctxId());
    }
    const now = new Date();
    const out = preview.render(module, screenCanvas, {
      ...state, listTop: listTop ?? undefined, theme: renderer.theme, layout: layoutSelect.value,
      battery: 100, minutes: now.getHours() * 60 + now.getMinutes(),
    });
    shown = { view: state.view, ...out };
    if (then) then(renderer);
  }

  async function load(newSlot) {
    slot = newSlot;
    const [m, devs] = await Promise.all([
      api.getRFMapping(slot).catch(() => null),
      api.request('/api/devices').catch(() => null),
    ]);
    // The Hub's push and its device-browser matching read devices.json (/api/devices)
    devices = devs?.devices || [];
    map = m && typeof m === 'object' && !Array.isArray(m) ? m : {};
    delete map.ok; delete map.mappings; // the Hub's reply when it has no map yet
    map.activities = map.activities || {};
    map.devices = map.devices || {};
    dirty = false;
    ctxSelect.replaceChildren(...ctxOptions());
    const running = String(store.get('activities.currentId') || '-1');
    const wanted = ctx !== 'a:-1' || running === '-1' ? ctx : `a:${running}`;
    ctx = [...ctxSelect.querySelectorAll('option')].some(o => o.value === wanted) ? wanted : 'a:-1';
    ctxSelect.value = ctx;
    refresh();
    showScreen();
    loadTheme();
  }

  const element = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      title,
      h('div', { style: { display: 'flex', gap: '8px', alignItems: 'center', flexWrap: 'wrap' } }, [dirtyBadge, copyBtn, revertBtn, saveBtn]),
    ]),
    h('label', { className: 'eb-field eb-ctx' }, [h('span', { text: 'Bindings for' }), ctxSelect]),
    h('div', { className: 'eb-layout' }, [
      h('div', { className: 'eb-picture-col' }, [
        pictureWrap,
        h('div', { className: 'eb-legend' }, [
          h('span', {}, [h('span', { className: 'eb-swatch eb-here' }), 'Set here']),
          h('span', {}, [h('span', { className: 'eb-swatch eb-default' }), 'Inherited']),
          h('span', {}, [h('span', { className: 'eb-swatch eb-none' }), 'Unbound']),
        ]),
        h('label', { className: 'eb-field eb-theme' }, [h('span', { text: 'Remote theme' }), themeSelect]),
        h('label', { className: 'eb-field eb-theme' }, [h('span', { text: 'Home layout' }), layoutSelect]),
      ]),
      h('div', { className: 'eb-panel-col' }, [keyPanel]),
    ]),
  ]);

  // Redraw when the Hub's activities change; views aren't unmounted, so each listener
  // removes itself once the page has gone
  ['activities.list', 'activities.currentId'].forEach((path) => {
    const off = store.on(path, () => {
      if (!element.isConnected) { off(); return; }
      if (path === 'activities.list') {
        ctxSelect.replaceChildren(...ctxOptions());
        ctxSelect.value = ctx;
      }
      refresh();
      drawScreen();
    });
  });

  return { element, load };
}
