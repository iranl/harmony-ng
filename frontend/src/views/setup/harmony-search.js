// Harmony Manufacturer & Model Database Search View
import { h } from '../../utils/dom.js';
import { api } from '../../api/index.js';
import { store } from '../../store.js';
import { t } from '../../i18n/index.js';
import { toast } from '../../components/toast.js';
import manufacturersData from '../../harmony_manufacturers.json';

export function createView() {
  const container = h('div', { className: 'setup-subview-search', style: { display: 'flex', flexDirection: 'column', gap: '16px' } });

  const searchCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('harmony_search', 'Harmony Database Search') }),
    ]),
    h('p', { className: 'text-dim', style: { fontSize: '13px', marginBottom: '12px' }, text: 'Search Logitech Harmony cloud database of over 250,000 devices.' }),
    h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } }, [
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', text: 'Manufacturer' }),
        h('input', {
          id: 'search-mfg-input',
          type: 'text',
          className: 'form-input',
          placeholder: 'e.g. Sony, Samsung, LG, Denon, Apple...',
          list: 'mfg-datalist',
        }),
        h('datalist', { id: 'mfg-datalist' },
          (Array.isArray(manufacturersData) ? manufacturersData : Object.keys(manufacturersData)).slice(0, 200).map(m => {
            const name = typeof m === 'string' ? m : (m.name || m.label || '');
            return h('option', { value: name });
          })
        ),
      ]),
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', text: 'Model Number' }),
        h('input', {
          id: 'search-model-input',
          type: 'text',
          className: 'form-input',
          placeholder: 'e.g. OLED65C1, STR-DN1080, Apple TV 4K...',
        }),
      ]),
      h('button', {
        className: 'btn btn-primary',
        text: '🔍 Search Harmony Database',
        onclick: () => runSearch(),
      }),
    ]),
  ]);

  const resultsCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('search_results', 'Results') }),
    ]),
    h('div', { id: 'search-results-mount', className: 'text-dim', text: 'Enter manufacturer and model to search.' }),
  ]);

  async function runSearch() {
    const mfg = container.querySelector('#search-mfg-input')?.value.trim();
    const model = container.querySelector('#search-model-input')?.value.trim();
    const mount = container.querySelector('#search-results-mount');

    if (!mfg) return toast.warn('Please enter manufacturer');
    if (!mount) return;

    mount.innerHTML = '';
    mount.textContent = 'Searching database...';

    try {
      const res = await api.request(`/api/harmony/search?manufacturer=${encodeURIComponent(mfg)}&model=${encodeURIComponent(model || '')}`);
      const items = Array.isArray(res) ? res : (res?.devices || res?.results || []);

      mount.innerHTML = '';
      if (items.length === 0) {
        mount.textContent = 'No matching devices found.';
        return;
      }

      const list = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '8px' } });
      items.forEach(item => {
        const row = h('div', {
          className: 'card',
          style: { display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '10px 14px', margin: 0 },
        }, [
          h('div', {}, [
            h('div', { style: { fontWeight: 'bold' }, text: `${item.manufacturer} ${item.model}` }),
            h('div', { className: 'text-dim', style: { fontSize: '12px' }, text: `Type: ${item.deviceType || item.type || 'AV'} | Commands: ${item.commandCount || item.commands?.length || 'Available'}` }),
          ]),
          h('button', {
            className: 'btn btn-primary btn-sm',
            text: 'Import',
            onclick: async () => {
              try {
                toast.info(`Importing ${item.model}...`);
                await api.postJson('/api/harmony/import', {
                  deviceId: item.id || item.deviceId,
                  manufacturer: item.manufacturer,
                  model: item.model,
                });
                toast.success(`${item.model} imported successfully!`);
                const inv = await api.getInventory();
                store.set('devices', inv.devices || []);
              } catch (e) {
                toast.error(e.message);
              }
            },
          }),
        ]);
        list.appendChild(row);
      });

      mount.appendChild(list);
    } catch (e) {
      mount.textContent = `Search error: ${e.message}`;
    }
  }

  container.appendChild(searchCard);
  container.appendChild(resultsCard);
  return container;
}

export default createView;
