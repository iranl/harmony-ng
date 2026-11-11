// Plugins & Marketplace Setup View for Harmony Remote v3
import { h } from '../../utils/dom.js';
import { api } from '../../api/index.js';
import { t } from '../../i18n/index.js';
import { toast } from '../../components/toast.js';
import { confirm } from '../../components/confirm.js';
import { createModal } from '../../components/modal.js';

export function createView() {
  const container = h('div', { className: 'setup-subview-plugins', style: { display: 'flex', flexDirection: 'column', gap: '16px' } });

  let activeTab = 'installed'; // 'installed' | 'marketplace' | 'repositories'
  let installedPlugins = [];
  let marketplacePlugins = [];
  let repositories = [];

  const tabContainer = h('div', {
    style: {
      display: 'flex',
      gap: '8px',
      borderBottom: '1px solid var(--border)',
      paddingBottom: '12px'
    }
  });

  const contentContainer = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '16px' } });

  function renderTabs() {
    tabContainer.innerHTML = '';
    const tabs = [
      { id: 'installed', label: 'Installed Plugins' },
      { id: 'marketplace', label: 'Community Marketplace' },
      { id: 'repositories', label: 'Plugin Repositories' }
    ];

    tabs.forEach(tab => {
      const isActive = tab.id === activeTab;
      const btn = h('button', {
        className: `btn btn-sm ${isActive ? 'btn-primary' : 'btn-secondary'}`,
        text: tab.label,
        onclick: () => {
          activeTab = tab.id;
          renderTabs();
          renderContent();
        }
      });
      tabContainer.appendChild(btn);
    });
  }

  async function loadData() {
    try {
      const plugRes = await api.getPlugins();
      installedPlugins = plugRes?.plugins || [];
    } catch (e) {
      installedPlugins = [];
    }

    if (activeTab === 'marketplace') {
      try {
        const markRes = await api.getPluginMarketplace();
        marketplacePlugins = markRes?.marketplace || [];
      } catch (e) {
        marketplacePlugins = [];
      }
    }

    if (activeTab === 'repositories') {
      try {
        const repoRes = await api.getPluginRepositories();
        repositories = repoRes?.repositories || [];
      } catch (e) {
        repositories = [];
      }
    }
  }

  async function renderContent() {
    contentContainer.innerHTML = '';
    contentContainer.appendChild(h('div', { className: 'text-dim', text: 'Loading...' }));

    await loadData();
    contentContainer.innerHTML = '';

    if (activeTab === 'installed') {
      renderInstalledView();
    } else if (activeTab === 'marketplace') {
      renderMarketplaceView();
    } else if (activeTab === 'repositories') {
      renderRepositoriesView();
    }
  }

  function renderInstalledView() {
    const card = h('div', { className: 'card' }, [
      h('div', { className: 'card-header' }, [
        h('div', {}, [
          h('h3', { className: 'card-title', text: 'Installed Hardware Plugins' }),
          h('div', { className: 'text-dim', style: { fontSize: '12px' }, text: 'Installed dynamic device drivers & daemons' }),
        ]),
        h('button', {
          className: 'btn btn-primary btn-sm',
          text: '+ Upload Plugin (.tar.gz)',
          onclick: () => openUploadModal(),
        }),
      ]),
    ]);

    const list = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } });

    if (installedPlugins.length === 0) {
      list.appendChild(h('div', { className: 'text-dim', style: { padding: '12px' }, text: 'No plugins installed yet. Explore the Community Marketplace to add device integrations.' }));
    } else {
      installedPlugins.forEach(p => {
        const isRunning = p.running;
        const isEnabled = p.enabled !== false;

        const badgeClass = !isEnabled ? 'badge-secondary' : (isRunning ? 'badge-success' : 'badge-danger');
        const badgeText = !isEnabled ? 'Disabled' : (isRunning ? `Running (PID ${p.pid})` : 'Stopped');

        const item = h('div', {
          className: 'card',
          style: {
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'space-between',
            padding: '14px',
            margin: 0,
            background: 'var(--bg-surface)'
          }
        }, [
          h('div', { style: { display: 'flex', flexDirection: 'column', gap: '4px' } }, [
            h('div', { style: { display: 'flex', alignItems: 'center', gap: '8px' } }, [
              h('span', { style: { fontWeight: 'bold', fontSize: '15px' }, text: p.name || p.id }),
              h('span', { className: 'text-dim', style: { fontSize: '12px' }, text: `v${p.version || '1.0'}` }),
              h('span', { className: `badge ${badgeClass}`, text: badgeText }),
            ]),
            h('div', { className: 'text-dim', style: { fontSize: '13px' }, text: p.description || 'No description provided' }),
            h('div', { className: 'text-dim', style: { fontSize: '11px' }, text: `ID: ${p.id} | Socket: /tmp/plugins/${p.id}.sock` }),
          ]),
          h('div', { style: { display: 'flex', alignItems: 'center', gap: '8px' } }, [
            h('button', {
              className: `btn btn-sm ${isEnabled ? 'btn-secondary' : 'btn-primary'}`,
              text: isEnabled ? 'Disable' : 'Enable',
              onclick: async () => {
                await api.pluginAction(p.id, isEnabled ? 'disable' : 'enable');
                toast.info(`Plugin ${p.id} ${isEnabled ? 'disabled' : 'enabled'}`);
                renderContent();
              }
            }),
            isEnabled && h('button', {
              className: 'btn btn-secondary btn-sm',
              text: 'Restart',
              onclick: async () => {
                await api.pluginAction(p.id, 'restart');
                toast.success(`Restarted ${p.name || p.id}`);
                renderContent();
              }
            }),
            h('button', {
              className: 'btn btn-danger btn-sm',
              text: 'Uninstall',
              onclick: async () => {
                const ok = await confirm({
                  title: 'Uninstall Plugin',
                  message: `Are you sure you want to uninstall '${p.name || p.id}'?`,
                  confirmText: 'Uninstall',
                  danger: true
                });
                if (ok) {
                  await api.uninstallPlugin(p.id);
                  toast.success(`Uninstalled ${p.name || p.id}`);
                  renderContent();
                }
              }
            }),
          ]),
        ]);
        list.appendChild(item);
      });
    }

    card.appendChild(list);
    contentContainer.appendChild(card);
  }

  function renderMarketplaceView() {
    const card = h('div', { className: 'card' }, [
      h('div', { className: 'card-header' }, [
        h('div', {}, [
          h('h3', { className: 'card-title', text: 'Community Plugin Marketplace' }),
          h('div', { className: 'text-dim', style: { fontSize: '12px' }, text: 'Curated integrations from official & custom repositories' }),
        ]),
        h('button', {
          className: 'btn btn-secondary btn-sm',
          text: '🔄 Refresh Catalog',
          onclick: () => renderContent(),
        }),
      ]),
    ]);

    const grid = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } });

    if (marketplacePlugins.length === 0) {
      grid.appendChild(h('div', { className: 'text-dim', style: { padding: '12px' }, text: 'No plugins found in configured repositories.' }));
    } else {
      marketplacePlugins.forEach(p => {
        const installed = installedPlugins.find(ip => ip.id === p.id);
        const hasUpdate = installed && installed.version && p.version && (installed.version !== p.version);

        const cardItem = h('div', {
          className: 'card',
          style: {
            display: 'flex',
            alignItems: 'center',
            justifyContent: 'space-between',
            padding: '14px',
            margin: 0,
            background: 'var(--bg-surface)'
          }
        }, [
          h('div', { style: { display: 'flex', flexDirection: 'column', gap: '4px' } }, [
            h('div', { style: { display: 'flex', alignItems: 'center', gap: '8px' } }, [
              h('span', { style: { fontWeight: 'bold', fontSize: '15px' }, text: p.name || p.id }),
              h('span', { className: 'text-dim', style: { fontSize: '12px' }, text: `v${p.version || '1.0'}` }),
              p.author && h('span', { className: 'badge badge-secondary', text: p.author }),
            ]),
            h('div', { className: 'text-dim', style: { fontSize: '13px' }, text: p.description || '' }),
            p.homepage && h('a', {
              href: p.homepage,
              target: '_blank',
              style: { fontSize: '11px', color: 'var(--accent)', textDecoration: 'none' },
              text: 'View Documentation / Source →'
            }),
          ]),
          h('div', {}, [
            installed && !hasUpdate && h('span', { className: 'badge badge-success', style: { padding: '6px 12px' }, text: 'Installed' }),
            hasUpdate && h('button', {
              className: 'btn btn-accent btn-sm',
              text: `Update (v${p.version})`,
              onclick: () => installFromRepo(p)
            }),
            !installed && h('button', {
              className: 'btn btn-primary btn-sm',
              text: 'Install',
              onclick: () => installFromRepo(p)
            })
          ])
        ]);
        grid.appendChild(cardItem);
      });
    }

    card.appendChild(grid);
    contentContainer.appendChild(card);
  }

  function renderRepositoriesView() {
    const card = h('div', { className: 'card' }, [
      h('div', { className: 'card-header' }, [
        h('div', {}, [
          h('h3', { className: 'card-title', text: 'Configured Repositories' }),
          h('div', { className: 'text-dim', style: { fontSize: '12px' }, text: 'Sources used to discover and update community plugins' }),
        ]),
        h('button', {
          className: 'btn btn-primary btn-sm',
          text: '+ Add Repository',
          onclick: () => openAddRepoModal(),
        }),
      ]),
    ]);

    const list = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '8px' } });

    repositories.forEach(repoUrl => {
      const item = h('div', {
        className: 'card',
        style: {
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'space-between',
          padding: '12px',
          margin: 0,
          background: 'var(--bg-surface)'
        }
      }, [
        h('div', { style: { wordBreak: 'break-all', fontFamily: 'monospace', fontSize: '13px' }, text: repoUrl }),
        h('button', {
          className: 'btn btn-danger btn-sm',
          text: 'Remove',
          onclick: async () => {
            await api.updatePluginRepositories('remove', repoUrl);
            toast.info('Repository removed');
            renderContent();
          }
        })
      ]);
      list.appendChild(item);
    });

    card.appendChild(list);
    contentContainer.appendChild(card);
  }

  async function installFromRepo(plugin) {
    if (!plugin.download_url) {
      toast.error('Plugin manifest has no download_url');
      return;
    }
    toast.info(`Installing ${plugin.name}...`);
    try {
      // 1. Primary: Hub direct download
      const res = await api.installPlugin(plugin.download_url);
      if (res && res.ok) {
        toast.success(`Successfully installed ${plugin.name}!`);
        renderContent();
        return;
      }
      throw new Error(res?.error || 'Direct download failed');
    } catch (directErr) {
      // 2. Client-side browser download fallback (air-gapped hubs / TLS cert issues)
      try {
        toast.info('Direct download failed, attempting browser proxy download...');
        const resp = await fetch(plugin.download_url);
        if (!resp.ok) throw new Error(`HTTP error ${resp.status}`);
        const blob = await resp.blob();
        const reader = new FileReader();
        reader.onloadend = async () => {
          try {
            const base64 = reader.result.split(',')[1];
            const uploadRes = await api.installPlugin(base64);
            if (uploadRes && uploadRes.ok) {
              toast.success(`Successfully installed ${plugin.name} via browser proxy!`);
              renderContent();
            } else {
              toast.error(`Install failed: ${uploadRes?.error || 'Unknown error'}`);
            }
          } catch (upErr) {
            toast.error(`Proxy upload failed: ${upErr.message}`);
          }
        };
        reader.readAsDataURL(blob);
      } catch (proxyErr) {
        toast.error(`Failed to install plugin: ${proxyErr.message}`);
      }
    }
  }

  function openUploadModal() {
    const fileInput = h('input', { type: 'file', accept: '.tar.gz,.tgz', className: 'form-input' });
    const content = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } }, [
      h('div', { className: 'text-dim', text: 'Select a plugin archive (.tar.gz) from your computer to install directly on the Hub.' }),
      fileInput,
    ]);

    createModal({
      title: 'Upload Plugin Package',
      content,
      actions: [
        { label: 'Cancel' },
        {
          label: 'Upload & Install',
          variant: 'primary',
          onClick: async () => {
            const file = fileInput.files?.[0];
            if (!file) {
              toast.warn('Please select a .tar.gz file');
              return false;
            }
            toast.info(`Uploading ${file.name}...`);
            const reader = new FileReader();
            reader.onloadend = async () => {
              try {
                const b64 = reader.result.split(',')[1];
                const res = await api.installPlugin(b64);
                if (res && res.ok) {
                  toast.success('Plugin uploaded and installed!');
                  renderContent();
                } else {
                  toast.error(`Install error: ${res?.error || 'Failed'}`);
                }
              } catch (e) {
                toast.error(e.message);
              }
            };
            reader.readAsDataURL(file);
            return true;
          }
        }
      ]
    });
  }

  function openAddRepoModal() {
    const urlInput = h('input', {
      type: 'text',
      className: 'form-input',
      placeholder: 'https://raw.githubusercontent.com/user/repo/main/repository.json'
    });
    const content = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } }, [
      h('label', { className: 'form-label', text: 'Repository JSON URL' }),
      urlInput,
    ]);

    createModal({
      title: 'Add Plugin Repository',
      content,
      actions: [
        { label: 'Cancel' },
        {
          label: 'Add',
          variant: 'primary',
          onClick: async () => {
            const url = urlInput.value.trim();
            if (!url) return false;
            await api.updatePluginRepositories('add', url);
            toast.success('Repository added');
            renderContent();
            return true;
          }
        }
      ]
    });
  }

  container.appendChild(tabContainer);
  container.appendChild(contentContainer);

  renderTabs();
  renderContent();

  return container;
}

export default createView;
