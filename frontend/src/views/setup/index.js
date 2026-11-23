// Setup Shell & Master-Detail Navigation for Harmony Remote v3
import { h } from '../../utils/dom.js';
import { store } from '../../store.js';
import { router } from '../../router.js';
import { t } from '../../i18n/index.js';

export const SETUP_ITEMS = [
  { id: 'activities', icon: '⚡', label: () => t('activities_setup', 'Activities Setup'), desc: () => t('setup_desc_activities', 'Profiles, startup/shutdown sequences & icons'), module: () => import('./activities.js') },
  { id: 'devices-studio', icon: '📺', label: () => t('devices_studio', 'Devices Studio'), desc: () => t('setup_desc_devices_studio', 'IR capture, Pronto hex, commands & timing'), module: () => import('./devices-studio.js') },
  { id: 'harmony-search', icon: '🔍', label: () => t('setup_harmony_search', 'Harmony DB Search'), desc: () => t('setup_desc_harmony_search', 'Search official database for devices & codes'), module: () => import('./harmony-search.js') },
  { id: 'ir-lab', icon: '🔬', label: () => t('setup_ir_lab', 'IR Lab'), desc: () => t('setup_desc_ir_lab', 'Bulk test command codes & verify response'), module: () => import('./ir-lab.js') },
  { id: 'rf-remote', icon: '📡', label: () => t('harmony_remote', 'Harmony (RF) Remote'), desc: () => t('setup_desc_rf_remote', 'Physical RF pairing, touchscreen & button mapping'), module: () => import('./remote.js') },
  { id: 'b25-remote', icon: '📱', label: () => t('setup_b25_remote', 'BT Remote (B25)'), desc: () => t('setup_desc_b25_remote', 'Bluetooth physical remote button mapping'), module: () => import('./b25-remote.js') },
  { id: 'bt-hosts', icon: '📶', label: () => t('setup_bt_hosts', 'Bluetooth Hosts'), desc: () => t('setup_desc_bt_hosts', 'Pairing, host supervision & device links'), module: () => import('./bt-hosts.js') },
  { id: 'mqtt', icon: '🌐', label: () => t('mqtt_config', 'MQTT & Home Assistant'), desc: () => t('setup_desc_mqtt', 'Broker connection, auto-discovery & event mirroring'), module: () => import('./mqtt.js') },
  { id: 'network', icon: '📶', label: () => t('setup_network', 'Network (WiFi / Eth)'), desc: () => t('setup_desc_network', 'Wi-Fi scanner, Ethernet configuration & IP'), module: () => import('./network.js') },
  { id: 'backup', icon: '💾', label: () => t('backup_restore', 'Backup & Restore'), desc: () => t('setup_desc_backup', 'Export / import full hub configurations'), module: () => import('./backup.js') },
  { id: 'system', icon: '⚙️', label: () => t('setup_system_admin', 'System Admin'), desc: () => t('setup_desc_system', 'Dashboard, disk space, services, logs & update'), module: () => import('./system.js') },
  { id: 'plugins', icon: '🧩', label: () => t('setup_plugins', 'Plugins & Marketplace'), desc: () => t('setup_desc_plugins', 'Install and manage hardware device integrations'), module: () => import('./plugins.js') },
  { id: 'users', icon: '👤', label: () => t('setup_users_auth', 'Users & Auth'), desc: () => t('setup_desc_users', 'User accounts, passwords & API tokens'), module: () => import('./users.js') },
  { id: 'hubs', icon: '🏠', label: () => t('multi_hub', 'Hub Management'), desc: () => t('setup_desc_hubs', 'Discover and switch between multiple Hubs'), module: () => import('./hubs.js') },
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
      h('div', { className: 'text-dim', style: { fontSize: '12px' }, text: t('setup_sidebar_desc', 'Manage hub hardware, remotes and integrations') }),
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
            h('div', { style: { fontWeight: '700', fontSize: '14px', color: isSelected ? 'var(--accent)' : 'var(--text-main)' }, text: item.label() }),
            h('div', { className: 'text-dim', style: { fontSize: '11px', marginTop: '2px' }, text: item.desc() }),
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
      contentPane.appendChild(h('div', { className: 'card text-dim', text: t('select_setup_section', 'Select a section from the setup menu.') }));
      return;
    }

    // Top Header with Back button on mobile
    const subHeader = h('div', {
      style: { display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: '16px', borderBottom: '1px solid var(--border)', paddingBottom: '10px' },
    }, [
      h('div', { style: { display: 'flex', alignItems: 'center', gap: '10px' } }, [
        isMobile ? h('button', {
          className: 'btn btn-secondary btn-sm',
          text: `‹ ${t('back', 'Back')}`,
          onclick: () => router.navigate('/setup/menu'),
        }) : null,
        h('h3', { className: 'card-title', text: `${item.icon} ${item.label()}` }),
      ]),
    ]);

    contentPane.appendChild(subHeader);

    // Mount sub-view
    const loadingPill = h('div', { className: 'text-dim', text: t('loading_view', 'Loading view...') });
    contentPane.appendChild(loadingPill);

    try {
      const mod = await item.module();
      contentPane.removeChild(loadingPill);
      const viewEl = mod.default ? mod.default() : mod.createView();
      contentPane.appendChild(viewEl);
    } catch (err) {
      loadingPill.textContent = t('error_loading_view', 'Error loading view: {error}', { error: err.message });
    }
  }

  window.addEventListener('harmony-route-changed', (e) => {
    if (e.detail.tab === 'setup') {
      currentSubView = e.detail.subView || (window.innerWidth < 768 ? 'menu' : 'system');
      renderSidebar();
      renderContent();
    }
  });

  // Rebuild only when the layout switches between mobile and desktop, so a resize keeps a
  // subview's unsaved edits
  let wasMobile = window.innerWidth < 768;
  window.addEventListener('resize', () => {
    if ((window.innerWidth < 768) === wasMobile) return;
    wasMobile = !wasMobile;
    renderContent();
  });

  renderSidebar();
  renderContent();

  container.appendChild(sidebar);
  container.appendChild(contentPane);
  return container;
}

export default createSetupView;
