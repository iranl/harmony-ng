// Multi-Hub Management View
import { h } from '../../utils/dom.js';
import { getSavedHubs, saveHub, deleteHub, getActiveHubId, setActiveHubId } from '../../storage.js';
import { api } from '../../api/index.js';
import { HarmonyClientCore } from '../../api/client.js';
import { t } from '../../i18n/index.js';
import { toast } from '../../components/toast.js';
import { confirm } from '../../components/confirm.js';
import { createModal } from '../../components/modal.js';
import { store } from '../../store.js';

export function createView() {
  const container = h('div', { className: 'setup-subview-hubs', style: { display: 'flex', flexDirection: 'column', gap: '16px' } });

  const listCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('saved_hubs', 'Saved Hubs') }),
      h('button', {
        className: 'btn btn-primary btn-sm',
        text: `+ ${t('add_hub', 'Add Hub')}`,
        onclick: () => openAddHubModal(),
      }),
    ]),
    h('div', { id: 'hubs-list-mount', style: { display: 'flex', flexDirection: 'column', gap: '8px' } }),
  ]);

  async function renderHubs() {
    const mount = container.querySelector('#hubs-list-mount');
    if (!mount) return;

    const [hubs, activeId] = await Promise.all([
      getSavedHubs(),
      getActiveHubId(),
    ]);

    mount.innerHTML = '';

    if (hubs.length === 0) {
      mount.appendChild(h('div', { className: 'text-dim', text: 'No custom hubs saved. Using current connection.' }));
      return;
    }

    hubs.forEach(hub => {
      const isActive = hub.id === activeId;
      const row = h('div', {
        className: 'card',
        style: {
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'space-between',
          padding: '12px',
          margin: 0,
          borderColor: isActive ? 'var(--accent)' : 'var(--border)',
        },
      }, [
        h('div', {}, [
          h('div', { style: { display: 'flex', alignItems: 'center', gap: '8px' } }, [
            h('span', { style: { fontWeight: 'bold' }, text: hub.name || `Hub (${hub.host})` }),
            isActive ? h('span', { className: 'badge badge-accent', text: 'Active' }) : null,
          ]),
          h('div', { className: 'text-dim', style: { fontSize: '12px' }, text: `http://${hub.host}:${hub.port || 8089}` }),
        ]),
        h('div', { style: { display: 'flex', gap: '8px' } }, [
          !isActive ? h('button', {
            className: 'btn btn-secondary btn-sm',
            text: t('switch_hub', 'Connect'),
            onclick: async () => {
              await setActiveHubId(hub.id);
              api.updateHub(hub);
              store.set('hub', hub);
              toast.success(`Switched to ${hub.name || hub.host}`);
              renderHubs();
            },
          }) : null,
          h('button', {
            className: 'btn btn-danger btn-sm',
            text: '×',
            onclick: async () => {
              const ok = await confirm.ask({
                title: t('delete_hub', 'Delete Hub'),
                message: `Remove ${hub.name || hub.host} from saved hubs list?`,
                danger: true,
              });
              if (ok) {
                await deleteHub(hub.id);
                toast.info('Hub removed');
                renderHubs();
              }
            },
          }),
        ]),
      ]);

      mount.appendChild(row);
    });
  }

  function openAddHubModal() {
    const hostInput = h('input', {
      type: 'text',
      className: 'form-input',
      placeholder: 'e.g. 10.234.234.98 or harmony-hub.local',
    });

    const portInput = h('input', {
      type: 'number',
      className: 'form-input',
      value: 8089,
    });

    const nameInput = h('input', {
      type: 'text',
      className: 'form-input',
      placeholder: 'Living Room Hub',
    });

    const content = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } }, [
      h('label', { className: 'form-label', text: 'Hub IP / Hostname' }),
      hostInput,
      h('label', { className: 'form-label', text: 'Port' }),
      portInput,
      h('label', { className: 'form-label', text: 'Friendly Name' }),
      nameInput,
    ]);

    createModal({
      title: t('add_hub', 'Add Harmony Hub'),
      content,
      actions: [
        {
          label: t('cancel', 'Cancel'),
        },
        {
          label: t('connect_save', 'Test & Save'),
          variant: 'primary',
          onClick: async () => {
            const host = hostInput.value.trim();
            const port = Number(portInput.value) || 8089;
            const name = nameInput.value.trim() || `Harmony Hub (${host})`;

            if (!host) {
              toast.warn('Please enter a host or IP');
              return false;
            }

            toast.info('Testing connection to Hub...');
            const health = await HarmonyClientCore.checkHubHealth(host, port, 3000);
            if (!health.ok) {
              const proceed = await confirm.ask({
                title: 'Hub Unreachable',
                message: `Could not reach Hub at http://${host}:${port}/health. Save anyway?`,
              });
              if (!proceed) return false;
            }

            const newHub = {
              id: `hub_${Date.now()}`,
              host,
              port,
              name,
            };

            await saveHub(newHub);
            toast.success(`Hub ${name} saved!`);
            renderHubs();
            return true;
          },
        },
      ],
    });
  }

  container.appendChild(listCard);
  renderHubs();
  return container;
}

export default createView;
