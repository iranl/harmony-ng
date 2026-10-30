// Harmony Remote App v3 Entry Point
import { store } from './store.js';
import { api } from './api/index.js';
import { router } from './router.js';
import { connection } from './utils/connection.js';
import { getActiveHub } from './storage.js';
import { getInitialLanguage, setLanguage } from './i18n/index.js';
import { createStatusPill } from './components/status-pill.js';
import { initIconCache } from './icons/cache.js';
import { createRemoteView } from './views/remote.js';
import { createDevicesView } from './views/devices.js';
import { createSetupView } from './views/setup/index.js';

async function initApp() {
  // 1. Language init
  const initialLang = getInitialLanguage();
  setLanguage(initialLang);

  const langSelect = document.getElementById('header-lang-select');
  if (langSelect) {
    langSelect.value = initialLang;
    langSelect.addEventListener('change', (e) => {
      setLanguage(e.target.value);
    });
  }

  // 2. Active Hub configuration
  const hub = await getActiveHub();
  if (hub) {
    api.updateHub(hub);
    store.set('hub', hub);
    const hubNameEl = document.getElementById('header-hub-name');
    if (hubNameEl) hubNameEl.textContent = hub.name || `Hub (${hub.host})`;
  }

  // 3. Status Pill
  const pillMount = document.getElementById('header-status-pill-mount');
  if (pillMount) {
    pillMount.appendChild(createStatusPill());
  }

  // 4. View Mount & Tab Routing
  const mainMount = document.getElementById('app-main');
  router.setMountPoint(mainMount);

  let remoteViewEl = null;
  let devicesViewEl = null;
  let setupViewEl = null;

  function renderActiveTab(tab) {
    // Update tab bar buttons
    document.querySelectorAll('.tab-btn').forEach(btn => {
      if (btn.dataset.tab === tab) {
        btn.classList.add('active');
      } else {
        btn.classList.remove('active');
      }
    });

    // Mount view
    mainMount.innerHTML = '';
    if (tab === 'devices') {
      if (!devicesViewEl) devicesViewEl = createDevicesView();
      mainMount.appendChild(devicesViewEl);
    } else if (tab === 'setup') {
      if (!setupViewEl) setupViewEl = createSetupView();
      mainMount.appendChild(setupViewEl);
    } else {
      if (!remoteViewEl) remoteViewEl = createRemoteView();
      mainMount.appendChild(remoteViewEl);
    }
  }

  // Tab navigation clicks
  document.querySelectorAll('.tab-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      const targetTab = btn.dataset.tab;
      if (targetTab === 'setup') {
        router.navigate('/setup/menu');
      } else {
        router.navigate(`/${targetTab}`);
      }
    });
  });

  window.addEventListener('harmony-route-changed', (e) => {
    renderActiveTab(e.detail.tab);
  });

  // 5. Start router
  router.init();

  // 6. Start background services & warm caches
  initIconCache().catch(() => {});
  connection.start();

  // Initial data load
  try {
    const [inv, acts] = await Promise.all([
      api.getInventory().catch(() => ({ devices: [] })),
      api.getActivities().catch(() => ({ activities: [], currentId: '-1' })),
    ]);

    const cmdsMap = {};
    (inv.devices || []).forEach(d => {
      cmdsMap[d.id] = d.commands || [];
    });

    store.update({
      devices: inv.devices || [],
      commandsMap: cmdsMap,
      'activities.list': acts.activities || [],
      'activities.currentId': acts.currentId || '-1',
      'activities.currentName': acts.currentName || 'PowerOff',
    });
  } catch (err) {
    console.warn('Initial data load error:', err);
  }
}

document.addEventListener('DOMContentLoaded', initApp);
