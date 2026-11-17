// Harmony Manufacturer & Model Database Search View
// The Hub has no search route: like the legacy UI, the browser reads the public Logitech Harmony
// IR archive (utils/irdb.js) and saves the chosen codes into a device with /api/commands-batch.
import { h } from '../../utils/dom.js';
import { api } from '../../api/index.js';
import { store } from '../../store.js';
import { t } from '../../i18n/index.js';
import { toast } from '../../components/toast.js';
import {
  getHarmonyManufacturers,
  fetchHarmonyModels,
  searchHarmonyModels,
  fetchHarmonyDeviceDetail,
  fetchHarmonyCodeset,
  mapHarmonyDeviceType,
} from '../../utils/irdb.js';

// The commands of an archive codeset as /api/commands-batch takes them (codes without a Pronto
// form are left out, as in the legacy UI)
export function codesetToBatch(codeset, port = 7) {
  return (codeset?.commands || [])
    .filter(c => c && c.name && (c.pronto || c.raw))
    .map(c => ({ name: String(c.name), raw: c.pronto || c.raw, repeats: 3, port }));
}

export function createView() {
  const container = h('div', { className: 'setup-subview-search', style: { display: 'flex', flexDirection: 'column', gap: '16px' } });

  const mfgInput = h('input', {
    id: 'search-mfg-input',
    type: 'text',
    className: 'form-input',
    placeholder: t('harmony_search_mfg_placeholder', 'e.g. Sony, Samsung, LG, Denon, Apple...'),
    list: 'mfg-datalist',
  });
  const modelInput = h('input', {
    id: 'search-model-input',
    type: 'text',
    className: 'form-input',
    placeholder: t('harmony_search_model_placeholder', 'e.g. OLED65C1, STR-DN1080, Apple TV 4K...'),
  });

  const searchCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('harmony_search', 'Harmony Database Search') }),
    ]),
    h('p', { className: 'text-dim', style: { fontSize: '13px', marginBottom: '12px' }, text: t('harmony_search_desc', 'Search the archived Logitech Harmony database of over 250,000 devices, then import a model\'s IR codes into one of your devices.') }),
    h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } }, [
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', text: t('manufacturer', 'Manufacturer') }),
        mfgInput,
        h('datalist', { id: 'mfg-datalist' },
          getHarmonyManufacturers().slice(0, 200).map(name => h('option', { value: name }))
        ),
      ]),
      h('div', { className: 'form-group' }, [
        h('label', { className: 'form-label', text: t('model_number', 'Model Number') }),
        modelInput,
      ]),
      h('button', {
        className: 'btn btn-primary',
        text: `🔍 ${t('harmony_search_btn', 'Search Harmony Database')}`,
        onclick: () => runSearch(),
      }),
    ]),
  ]);

  const resultsMount = h('div', { id: 'search-results-mount', className: 'text-dim', text: t('harmony_search_hint', 'Enter manufacturer and model to search.') });
  const resultsCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('search_results', 'Results') }),
    ]),
    resultsMount,
  ]);

  async function runSearch() {
    const mfg = mfgInput.value.trim();
    const model = modelInput.value.trim();
    if (!mfg) return toast.warn(t('enter_manufacturer', 'Please enter manufacturer'));

    resultsMount.textContent = t('searching_database', 'Searching database...');
    try {
      const models = await fetchHarmonyModels(mfg);
      const items = searchHarmonyModels(models, model, 50).filter(it => it && it.f);
      resultsMount.textContent = '';
      if (items.length === 0) {
        resultsMount.textContent = t('no_matching_devices', 'No matching devices found.');
        return;
      }
      const list = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '8px' } });
      items.forEach(item => list.appendChild(resultRow(mfg, item)));
      resultsMount.appendChild(list);
    } catch (e) {
      resultsMount.textContent = t('search_error_msg', 'Search error: {error}', { error: e.message });
    }
  }

  function resultRow(mfg, item) {
    const detailMount = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '8px' } });
    return h('div', {
      className: 'card',
      style: { display: 'flex', flexDirection: 'column', gap: '8px', padding: '10px 14px', margin: 0 },
    }, [
      h('div', { style: { display: 'flex', alignItems: 'center', justifyContent: 'space-between' } }, [
        h('div', { style: { fontWeight: 'bold' }, text: `${mfg} ${item.m}` }),
        h('button', {
          className: 'btn btn-secondary btn-sm',
          text: t('load_codes', 'Load Codes'),
          onclick: () => loadCodes(mfg, item, detailMount),
        }),
      ]),
      detailMount,
    ]);
  }

  async function loadCodes(mfg, item, mount) {
    mount.textContent = t('loading_codes', 'Loading codes...');
    try {
      const detail = await fetchHarmonyDeviceDetail(mfg, item.f);
      if (!detail.codeset) {
        mount.textContent = t('no_ir_codeset', 'No IR codeset linked to this model.');
        return;
      }
      const codeset = await fetchHarmonyCodeset(detail.codeset);
      const devices = (store.get('devices') || []).filter(d => d.type !== 'BT' && d.type !== 'MQTT');
      const target = h('select', { className: 'form-select', 'aria-label': t('import_into', 'Import into') },
        devices.map(d => h('option', { value: String(d.id), text: d.name || d.id })));
      const total = (codeset.commands || []).length;
      mount.replaceChildren(
        h('div', { className: 'text-dim', style: { fontSize: '12px' }, text: t('codeset_type_commands', 'Type: {type} | Commands: {count}', { type: mapHarmonyDeviceType(detail.deviceType), count: total }) }),
        devices.length ? h('div', { style: { display: 'flex', gap: '8px', alignItems: 'center' } }, [
          target,
          h('button', {
            className: 'btn btn-primary btn-sm',
            text: t('import_btn', 'Import'),
            onclick: (e) => importCodes(codeset, target.value, e.target),
          }),
        ]) : h('div', { className: 'text-dim', text: t('add_ir_device_first', 'Add an IR device first, then import the codes into it.') }),
      );
    } catch (e) {
      mount.textContent = t('error_msg', 'Error: {error}', { error: e.message });
    }
  }

  async function importCodes(codeset, deviceId, btn) {
    const dev = (store.get('devices') || []).find(d => String(d.id) === String(deviceId));
    if (!dev) return toast.warn(t('choose_import_device', 'Choose a device to import into'));
    const port = dev.controlPort ?? dev.control_port ?? 7;
    const batch = codesetToBatch(codeset, port);
    if (!batch.length) return toast.warn(t('codeset_no_sendable', 'This codeset has no codes the Hub can send'));
    if (btn) btn.disabled = true;
    try {
      const res = await api.saveBatchCommands(deviceId, batch);
      if (res && res.ok === false) throw new Error(res.error || t('import_failed', 'Import failed'));
      toast.success(t('imported_commands_into', 'Imported {count} commands into {device}', { count: batch.length, device: dev.name || deviceId }));
      const inv = await api.getInventory();
      store.set('devices', inv.devices || []);
    } catch (e) {
      toast.error(e.message);
    } finally {
      if (btn) btn.disabled = false;
    }
  }

  container.appendChild(searchCard);
  container.appendChild(resultsCard);
  return container;
}

export default createView;
