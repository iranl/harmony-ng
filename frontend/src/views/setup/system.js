// System Administration View (Dashboard, Disk, Logs, Services, Reboot, Updates)
import { h } from '../../utils/dom.js';
import { api } from '../../api/index.js';
import { t } from '../../i18n/index.js';
import { toast } from '../../components/toast.js';
import { confirm } from '../../components/confirm.js';

export function createView() {
  const container = h('div', { className: 'setup-subview-system', style: { display: 'flex', flexDirection: 'column', gap: '16px' } });

  // System Dashboard Card
  const statsCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('system_dashboard', 'System Dashboard') }),
      h('button', {
        className: 'btn btn-secondary btn-sm',
        text: t('refresh', 'Refresh'),
        onclick: () => loadSystemInfo(),
      }),
    ]),
    h('div', { id: 'sys-stats-content', className: 'text-dim', text: 'Loading stats...' }),
  ]);

  // Services Card
  const servicesCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('services', 'Hardware Services') }),
    ]),
    h('div', { id: 'sys-services-content', style: { display: 'flex', flexDirection: 'column', gap: '10px' } }),
  ]);

  // Maintenance & Reboot Card
  const actionsCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('system_actions', 'Maintenance') }),
    ]),
    h('div', { style: { display: 'flex', gap: '10px', flexWrap: 'wrap' } }, [
      h('button', {
        className: 'btn btn-secondary',
        text: t('check_update', 'Check for Updates'),
        onclick: async () => {
          try {
            toast.info(t('checking_update', 'Checking for update...'));
            const st = await api.checkUpdate(true);
            toast.success(st?.message || 'Update check finished');
          } catch (e) {
            toast.error(e.message);
          }
        },
      }),
      h('button', {
        className: 'btn btn-danger',
        text: `⚠️ ${t('reboot_hub', 'Reboot Hub')}`,
        onclick: async () => {
          const ok = await confirm.ask({
            title: t('reboot_hub', 'Reboot Hub'),
            message: t('confirm_reboot', 'Are you sure you want to reboot the Harmony Hub? All connections will be temporarily lost.'),
            danger: true,
          });
          if (ok) {
            try {
              await api.rebootHub();
              toast.warn(t('hub_rebooting', 'Hub is rebooting...'));
            } catch (e) {
              toast.error(e.message);
            }
          }
        },
      }),
    ]),
  ]);

  // Logs Card
  const logsCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('system_logs', 'Hub Logs') }),
      h('div', { style: { display: 'flex', gap: '8px' } }, [
        h('select', {
          id: 'log-source-select',
          className: 'form-select',
          style: { padding: '4px 8px', fontSize: '12px' },
          onchange: (e) => loadLogs(e.target.value),
        }, [
          h('option', { value: 'daemon', text: 'hub_daemon' }),
          h('option', { value: 'rf', text: 'codex_rf' }),
          h('option', { value: 'bt', text: 'codex_btstack' }),
          h('option', { value: 'mqtt', text: 'codex_mqtt' }),
          h('option', { value: 'dmesg', text: 'dmesg (kernel)' }),
        ]),
        h('button', {
          className: 'btn btn-secondary btn-sm',
          text: t('refresh', 'Refresh'),
          onclick: () => {
            const sel = container.querySelector('#log-source-select');
            loadLogs(sel ? sel.value : 'daemon');
          },
        }),
      ]),
    ]),
    h('pre', {
      id: 'sys-logs-content',
      style: {
        background: 'rgba(0,0,0,0.5)',
        padding: '10px',
        borderRadius: 'var(--radius-sm)',
        fontSize: '11px',
        fontFamily: 'monospace',
        maxHeight: '260px',
        overflowY: 'auto',
        whiteSpace: 'pre-wrap',
        color: 'var(--text-muted)',
      },
      text: 'Loading logs...',
    }),
  ]);

  async function loadSystemInfo() {
    const statsMount = container.querySelector('#sys-stats-content');
    if (!statsMount) return;

    try {
      const [disk, info, status] = await Promise.all([
        api.getSystemDisk().catch(() => null),
        api.getSystemInfo().catch(() => null),
        api.getStatus().catch(() => null),
      ]);

      statsMount.innerHTML = '';
      const grid = h('div', { style: { display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(180px, 1fr))', gap: '12px' } }, [
        h('div', { className: 'card', style: { margin: 0, padding: '10px' } }, [
          h('div', { className: 'card-sub', text: 'Firmware Version' }),
          h('div', { style: { fontSize: '15px', fontWeight: 'bold', marginTop: '4px' }, text: info?.version || status?.version || 'Custom' }),
        ]),
        h('div', { className: 'card', style: { margin: 0, padding: '10px' } }, [
          h('div', { className: 'card-sub', text: 'Flash Free Space' }),
          h('div', { style: { fontSize: '15px', fontWeight: 'bold', marginTop: '4px' }, text: disk ? `${disk.totalCombinedFreeMb} MB (${disk.totalCombinedMb} MB)` : 'Unknown' }),
        ]),
        h('div', { className: 'card', style: { margin: 0, padding: '10px' } }, [
          h('div', { className: 'card-sub', text: 'Uptime' }),
          h('div', { style: { fontSize: '15px', fontWeight: 'bold', marginTop: '4px' }, text: info?.uptime || status?.uptime || 'Active' }),
        ]),
      ]);

      statsMount.appendChild(grid);
    } catch (e) {
      statsMount.textContent = `Error: ${e.message}`;
    }
  }

  async function loadServices() {
    const srvMount = container.querySelector('#sys-services-content');
    if (!srvMount) return;

    try {
      const srv = await api.getServices().catch(() => ({ rf: true, bt: true }));
      srvMount.innerHTML = '';

      const createSrvRow = (name, key, toggleFn, enabled) => {
        return h('div', {
          style: { display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '8px 12px', background: 'rgba(0,0,0,0.2)', borderRadius: 'var(--radius-sm)' },
        }, [
          h('span', { style: { fontWeight: '600', fontSize: '13px' }, text: name }),
          h('button', {
            className: `btn btn-sm ${enabled ? 'btn-primary' : 'btn-secondary'}`,
            text: enabled ? 'Enabled' : 'Disabled',
            onclick: async () => {
              try {
                await toggleFn(!enabled);
                toast.success(`${name} toggled`);
                loadServices();
              } catch (e) {
                toast.error(e.message);
              }
            },
          }),
        ]);
      };

      srvMount.appendChild(createSrvRow('Harmony RF Transceiver (CC2544)', 'rf', (en) => api.toggleRfService(en), srv.rf !== false));
      srvMount.appendChild(createSrvRow('Bluetooth Stack (BT HID/UART)', 'bt', (en) => api.toggleBtService(en), srv.bt !== false));
    } catch (e) {
      srvMount.textContent = `Error loading services: ${e.message}`;
    }
  }

  async function loadLogs(source = 'daemon') {
    const logsMount = container.querySelector('#sys-logs-content');
    if (!logsMount) return;
    try {
      logsMount.textContent = 'Loading logs...';
      const logData = await api.getLogs(source);
      logsMount.textContent = typeof logData === 'string' ? logData : (logData?.logs || logData?.output || JSON.stringify(logData, null, 2));
      logsMount.scrollTop = logsMount.scrollHeight;
    } catch (e) {
      logsMount.textContent = `Error fetching logs: ${e.message}`;
    }
  }

  container.appendChild(statsCard);
  container.appendChild(servicesCard);
  container.appendChild(actionsCard);
  container.appendChild(logsCard);

  loadSystemInfo();
  loadServices();
  loadLogs('daemon');

  return container;
}

export default createView;
