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
      mount.appendChild(h('div', { className: 'text-dim', text: t('no_saved_hubs', 'No custom hubs saved. Using current connection.') }));
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
            h('span', { style: { fontWeight: 'bold' }, text: hub.name || t('hub_with_host', 'Hub ({host})', { host: hub.host }) }),
            isActive ? h('span', { className: 'badge badge-accent', text: t('status_active', 'Active') }) : null,
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
              toast.success(t('switched_to_hub', 'Switched to {name}', { name: hub.name || hub.host }));
              renderHubs();
            },
          }) : null,
          h('button', {
            className: 'btn btn-danger btn-sm',
            text: '×',
            onclick: async () => {
              const ok = await confirm.ask({
                title: t('delete_hub', 'Delete Hub'),
                message: t('remove_hub_confirm', 'Remove {name} from saved hubs list?', { name: hub.name || hub.host }),
                danger: true,
              });
              if (ok) {
                await deleteHub(hub.id);
                toast.info(t('hub_removed', 'Hub removed'));
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
      placeholder: t('hub_host_placeholder', 'e.g. 192.168.1.1 or harmony-hub.local'),
    });

    const portInput = h('input', {
      type: 'number',
      className: 'form-input',
      value: 8089,
    });

    const nameInput = h('input', {
      type: 'text',
      className: 'form-input',
      placeholder: t('hub_name_placeholder', 'Living Room Hub'),
    });

    const content = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } }, [
      h('label', { className: 'form-label', text: t('hub_ip', 'Hub IP / Hostname') }),
      hostInput,
      h('label', { className: 'form-label', text: t('port', 'Port') }),
      portInput,
      h('label', { className: 'form-label', text: t('friendly_name', 'Friendly Name') }),
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
              toast.warn(t('enter_host_or_ip', 'Please enter a host or IP'));
              return false;
            }

            toast.info(t('testing_hub_connection', 'Testing connection to Hub...'));
            const health = await HarmonyClientCore.checkHubHealth(host, port, 3000);
            if (!health.ok) {
              const proceed = await confirm.ask({
                title: t('hub_unreachable', 'Hub Unreachable'),
                message: t('hub_unreachable_save', 'Could not reach Hub at {url}. Save anyway?', { url: `http://${host}:${port}/health` }),
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
            toast.success(t('hub_saved', 'Hub {name} saved!', { name }));
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
