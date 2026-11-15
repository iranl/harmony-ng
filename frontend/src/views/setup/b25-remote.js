// Homatics B25 Bluetooth Remote Mapping View
import { h } from '../../utils/dom.js';
import { api } from '../../api/index.js';
import { store } from '../../store.js';
import { t } from '../../i18n/index.js';
import { toast } from '../../components/toast.js';

// The B25's keys by the names bt_remote_map.json uses (the legacy UI's B25 picture)
const B25_KEYS = [
  ['power', 'Power'], ['input', 'Input'], ['settings', 'Settings'], ['assistant', 'Google Assistant'],
  ['up', 'Up'], ['down', 'Down'], ['left', 'Left'], ['right', 'Right'], ['select', 'OK'],
  ['back', 'Back'], ['home', 'Home'], ['live_tv', 'Live TV'], ['guide', 'Guide'], ['info', 'Info'],
  ['vol_up', 'Volume Up'], ['vol_down', 'Volume Down'], ['mute', 'Mute'],
  ['ch_up', 'Channel Up'], ['ch_down', 'Channel Down'], ['watchlist', 'Watchlist'],
  ['red', 'Red'], ['green', 'Green'], ['yellow', 'Yellow'], ['blue', 'Blue'],
  ['youtube', 'YouTube'], ['netflix', 'Netflix'], ['prime_video', 'Prime Video'], ['google_play', 'Google Play'],
  ['1', '1'], ['2', '2'], ['3', '3'], ['4', '4'], ['5', '5'], ['6', '6'], ['7', '7'], ['8', '8'], ['9', '9'], ['0', '0'],
];

export function createView() {
  const container = h('div', { className: 'setup-subview-b25', style: { display: 'flex', flexDirection: 'column', gap: '16px' } });

  const scanCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('b25_remote', 'Homatics B25 Bluetooth Remote') }),
      h('button', {
        className: 'btn btn-primary btn-sm',
        text: '🔍 Scan & Pair B25',
        onclick: async () => {
          try {
            toast.info('Scanning for B25 remote...');
            const res = await api.scanBtRemote();
            toast.success(res?.message || 'B25 remote scanned');
          } catch (e) {
            toast.error(e.message);
          }
        },
      }),
    ]),
    h('p', { className: 'text-dim', style: { fontSize: '12px' }, text: 'Pair and map buttons for physical Homatics B25 Bluetooth BLE remote.' }),
  ]);

  // What each key does: read-only view of bt_remote_map.json (the legacy UI edits it). The Hub
  // looks a key up in the running activity first, then in "-1" (bt_remote.c), and so does this list.
  const activitySelect = h('select', { className: 'form-select', 'aria-label': 'Activity', onchange: () => render() });
  const mappingCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h4', { className: 'card-title', text: 'B25 Button Mappings' }),
      activitySelect,
    ]),
    h('div', { id: 'b25-mapping-content', className: 'text-dim', text: 'Loading B25 mappings...' }),
  ]);

  let map = null;

  function describe(b) {
    if (!b || typeof b !== 'object') return null;
    const action = b.action || b.type || (b.command ? 'device_cmd' : '');
    const devId = b.deviceId || b.targetDevice;
    const dev = (store.get('devices') || []).find(d => String(d.id) === String(devId));
    const actId = b.activityId || b.targetActivity || b.deviceId;
    const act = (store.get('activities.list') || []).find(a => String(a.id) === String(actId));
    switch (action) {
      case 'device_cmd': case 'command': return `${dev?.name || `Device ${devId}`} › ${b.command}`;
      case 'passthrough_bt': return `Bluetooth key › ${b.command}`;
      case 'activity_start': case 'change_activity': return `Start ${act?.name || `Activity ${actId}`}`;
      case 'activity_stop': case 'poweroff': return 'Turn everything off';
      default: return action ? `Not supported (${action})` : null;
    }
  }

  function render() {
    const mount = container.querySelector('#b25-mapping-content');
    if (!mount || !map) return;
    const acts = map.activities && typeof map.activities === 'object' ? map.activities : map;
    const actId = activitySelect.value || '-1';
    const own = actId !== '-1' ? acts[actId] || {} : {};
    const def = acts['-1'] || acts.default || {};
    const list = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '6px' } });
    B25_KEYS.forEach(([id, label]) => {
      const here = describe(own[id]);
      const inherited = here ? null : describe(def[id]);
      const text = here || (inherited ? `${inherited} (default)` : 'Not mapped');
      list.appendChild(h('div', {
        className: 'card',
        style: { display: 'flex', justifyContent: 'space-between', padding: '8px 12px', margin: 0 },
        dataset: { key: id },
      }, [
        h('span', { style: { fontWeight: '600', fontSize: '13px' }, text: label }),
        h('span', { className: `badge${here || inherited ? ' badge-accent' : ''}`, text }),
      ]));
    });
    mount.replaceChildren(list);
  }

  async function loadB25() {
    const mount = container.querySelector('#b25-mapping-content');
    if (!mount) return;
    try {
      const data = await api.getRemoteMappings();
      map = data && typeof data === 'object' ? data : {};
      const remote = map.remote?.name || 'Homatics B25';
      const acts = (store.get('activities.list') || []).filter(a => String(a.id) !== '-1');
      activitySelect.replaceChildren(
        h('option', { value: '-1', text: 'Off / Idle (Default)' }),
        ...acts.map(a => h('option', { value: String(a.id), text: a.name })),
      );
      const running = String(store.get('activities.currentId') || '-1');
      activitySelect.value = acts.some(a => String(a.id) === running) ? running : '-1';
      mappingCard.querySelector('.card-title').textContent = `${remote} Button Mappings`;
      render();
    } catch (e) {
      mount.textContent = `Error: ${e.message}`;
    }
  }

  container.appendChild(scanCard);
  container.appendChild(mappingCard);
  loadB25();

  return container;
}

export default createView;
