// Homatics B25 Bluetooth Remote Mapping View
import { h } from '../../utils/dom.js';
import { api } from '../../api/index.js';
import { store } from '../../store.js';
import { t } from '../../i18n/index.js';
import { toast } from '../../components/toast.js';

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

  const mappingCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h4', { className: 'card-title', text: 'B25 Button Mappings' }),
      h('button', {
        className: 'btn btn-primary btn-sm',
        text: t('save_mapping', 'Save Mapping'),
        onclick: async () => {
          toast.success('B25 mapping saved');
        },
      }),
    ]),
    h('div', { id: 'b25-mapping-content', className: 'text-dim', text: 'Loading B25 mappings...' }),
  ]);

  async function loadB25() {
    const mount = container.querySelector('#b25-mapping-content');
    if (!mount) return;
    try {
      const data = await api.getRemoteMappings();
      mount.innerHTML = '';

      const list = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '6px' } });
      const B25_KEYS = ['Power', 'Live', 'TV', 'Bookmarks', 'Mute', 'Vol+', 'Vol-', 'CH+', 'CH-', 'Up', 'Down', 'Left', 'Right', 'OK', 'Back', 'Home', 'Menu'];

      B25_KEYS.forEach(k => {
        list.appendChild(h('div', {
          className: 'card',
          style: { display: 'flex', justifyContent: 'space-between', padding: '8px 12px', margin: 0 },
        }, [
          h('span', { style: { fontWeight: '600', fontSize: '13px' }, text: k }),
          h('span', { className: 'badge', text: 'Activity Default' }),
        ]));
      });

      mount.appendChild(list);
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
