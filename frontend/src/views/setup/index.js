// Setup Shell & Master-Detail Navigation for Harmony Remote v3
import { h } from '../../utils/dom.js';
import { store } from '../../store.js';
import { router } from '../../router.js';
import { t } from '../../i18n/index.js';

export const SETUP_ITEMS = [
  { id: 'activities', icon: '⚡', label: 'Activities Setup', desc: 'Profiles, startup/shutdown sequences & icons', module: () => import('./activities.js') },
  { id: 'devices-studio', icon: '📺', label: 'Devices Studio', desc: 'IR capture, Pronto hex, commands & timing', module: () => import('./devices-studio.js') },
  { id: 'harmony-search', icon: '🔍', label: 'Harmony DB Search', desc: 'Search official database for devices & codes', module: () => import('./harmony-search.js') },
  { id: 'ir-lab', icon: '🔬', label: 'IR Lab', desc: 'Bulk test command codes & verify response', module: () => import('./ir-lab.js') },
  { id: 'elite-remote', icon: '📡', label: 'Harmony (RF) Remote', desc: 'Physical RF pairing, touchscreen & button mapping', module: () => import('./elite-remote.js') },
  { id: 'b25-remote', icon: '📱', label: 'BT Remote (B25)', desc: 'Bluetooth physical remote button mapping', module: () => import('./b25-remote.js') },
  { id: 'bt-hosts', icon: '📶', label: 'Bluetooth Hosts', desc: 'Pairing, host supervision & device links', module: () => import('./bt-hosts.js') },
  { id: 'mqtt', icon: '🌐', label: 'MQTT & Home Assistant', desc: 'Broker connection, auto-discovery & event mirroring', module: () => import('./mqtt.js') },
  { id: 'network', icon: '📶', label: 'Network (WiFi / Eth)', desc: 'Wi-Fi scanner, Ethernet configuration & IP', module: () => import('./network.js') },
  { id: 'backup', icon: '💾', label: 'Backup & Restore', desc: 'Export / import full hub configurations', module: () => import('./backup.js') },
  { id: 'system', icon: '⚙️', label: 'System Admin', desc: 'Dashboard, disk space, services, logs & update', module: () => import('./system.js') },
  { id: 'users', icon: '👤', label: 'Users & Auth', desc: 'User accounts, passwords & API tokens', module: () => import('./users.js') },
  { id: 'hubs', icon: '🏠', label: 'Hub Management', desc: 'Discover and switch between multiple Hubs', module: () => import('./hubs.js') },
];

export function createSetupView(options = {}) {
  const container = h('div', { className: 'setup-container', style: { height: '100%' } });
  const sidebar = h('div', { className: 'setup-sidebar' });
  const contentPane = h('div', { className: 'setup-content-pane' });

  let currentSubView = store.get('activeSetupView') || 'menu';

  function renderSidebar() {
    sidebar.innerHTML = '';

    const listHeader = h('div', { style: { padding: '16px 14px 8px' } }, [
      h('h2', { className: 'card-title', text: t('tab_settings', 'Setup & Configuration') }),
      h('div', { className: 'text-dim', style: { fontSize: '12px' }, text: 'Manage hub hardware, remotes and integrations' }),
    ]);
    sidebar.appendChild(listHeader);

    const menuList = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '6px', padding: '8px 12px' } });

    SETUP_ITEMS.forEach(item => {
      const isSelected = item.id === currentSubView;
      const card = h('div', {
        className: `card setup-menu-card ${isSelected ? 'active' : ''}`,
        style: {
          cursor: 'pointer',
          padding: '12px 14px',
          margin: 0,
          background: isSelected ? 'var(--bg-card-hover)' : 'var(--bg-card)',
          borderColor: isSelected ? 'var(--accent)' : 'var(--border)',
        },
        onclick: () => {
          router.navigate(`/setup/${item.id}`);
        },
      }, [
        h('div', { style: { display: 'flex', alignItems: 'center', gap: '12px' } }, [
          h('span', { style: { fontSize: '20px' }, text: item.icon }),
          h('div', {}, [
            h('div', { style: { fontWeight: '700', fontSize: '14px', color: isSelected ? 'var(--accent)' : 'var(--text-main)' }, text: item.label }),
            h('div', { className: 'text-dim', style: { fontSize: '11px', marginTop: '2px' }, text: item.desc }),
          ]),
        ]),
      ]);

      menuList.appendChild(card);
    });

    sidebar.appendChild(menuList);
  }

  async function renderContent() {
    contentPane.innerHTML = '';
    const isMobile = window.innerWidth < 768;

    if (currentSubView === 'menu' || !currentSubView) {
      if (isMobile) {
        // Show menu in sidebar, content pane empty
        sidebar.style.display = 'block';
        contentPane.style.display = 'none';
        return;
      }
      // On desktop, default to 'system'
      currentSubView = 'system';
    }

    if (isMobile) {
      sidebar.style.display = 'none';
      contentPane.style.display = 'block';
    } else {
      sidebar.style.display = 'block';
      contentPane.style.display = 'block';
    }

    const item = SETUP_ITEMS.find(i => i.id === currentSubView);
    if (!item) {
      contentPane.appendChild(h('div', { className: 'card text-dim', text: 'Select a section from the setup menu.' }));
      return;
    }

    // Top Header with Back button on mobile
    const subHeader = h('div', {
      style: { display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: '16px', borderBottom: '1px solid var(--border)', paddingBottom: '10px' },
    }, [
      h('div', { style: { display: 'flex', alignItems: 'center', gap: '10px' } }, [
        isMobile ? h('button', {
          className: 'btn btn-secondary btn-sm',
          text: '‹ Back',
          onclick: () => router.navigate('/setup/menu'),
        }) : null,
        h('h3', { className: 'card-title', text: `${item.icon} ${item.label}` }),
      ]),
    ]);

    contentPane.appendChild(subHeader);

    // Mount sub-view
    const loadingPill = h('div', { className: 'text-dim', text: 'Loading view...' });
    contentPane.appendChild(loadingPill);

    try {
      const mod = await item.module();
      contentPane.removeChild(loadingPill);
      const viewEl = mod.default ? mod.default() : mod.createView();
      contentPane.appendChild(viewEl);
    } catch (err) {
      loadingPill.textContent = `Error loading view: ${err.message}`;
    }
  }

  window.addEventListener('harmony-route-changed', (e) => {
    if (e.detail.tab === 'setup') {
      currentSubView = e.detail.subView || (window.innerWidth < 768 ? 'menu' : 'system');
      renderSidebar();
      renderContent();
    }
  });

  window.addEventListener('resize', () => {
    renderContent();
  });

  renderSidebar();
  renderContent();

  container.appendChild(sidebar);
  container.appendChild(contentPane);
  return container;
}

export default createSetupView;
