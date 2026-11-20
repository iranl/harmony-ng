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
  let storageInfo = null;
  let loadError = '';
  let marketplaceStale = false;

  // A failed action says so; without it the click did nothing visible
  async function runAction(fn, failKey, failText) {
    try {
      await fn();
    } catch (err) {
      toast.error(t(failKey, failText, { error: err.message }));
    }
    renderContent();
  }

  // Only http(s) links from a catalog: a javascript: homepage ran in the page
  function safeHttpUrl(u) {
    try {
      const url = new URL(String(u));
      return (url.protocol === 'https:' || url.protocol === 'http:') ? url.href : '';
    } catch {
      return '';
    }
  }

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
      { id: 'installed', label: t('plg_tab_installed', 'Installed Plugins') },
      { id: 'marketplace', label: t('plg_tab_marketplace', 'Community Marketplace') },
      { id: 'repositories', label: t('plg_tab_repositories', 'Plugin Repositories') }
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

  async function loadData(forceReload = false) {
    loadError = '';
    try {
      const plugRes = await api.getPlugins();
      installedPlugins = Array.isArray(plugRes?.plugins) ? plugRes.plugins : [];
      storageInfo = plugRes?.storage || null;
    } catch (e) {
      installedPlugins = [];
      storageInfo = null;
      loadError = e.message;
    }

    if (activeTab === 'marketplace') {
      try {
        const markRes = await api.getPluginMarketplace(forceReload || marketplaceStale);
        marketplaceStale = false;
        const list = Array.isArray(markRes?.marketplace) ? markRes.marketplace : [];
        // a null or malformed entry of a repository must not break the whole list
        marketplacePlugins = list.filter(p => p && typeof p === 'object' && p.id);
      } catch (e) {
        marketplacePlugins = [];
        loadError = e.message;
      }
    }

    if (activeTab === 'repositories') {
      try {
        const repoRes = await api.getPluginRepositories();
        repositories = Array.isArray(repoRes?.repositories) ? repoRes.repositories : [];
      } catch (e) {
        repositories = [];
        loadError = e.message;
      }
    }
  }

  async function renderContent(forceReload = false) {
    contentContainer.innerHTML = '';
    contentContainer.appendChild(h('div', { className: 'text-dim', text: t('loading', 'Loading...') }));

    await loadData(forceReload);
    contentContainer.innerHTML = '';
    if (loadError) {
      contentContainer.appendChild(h('div', { className: 'card', style: { padding: '12px', color: 'var(--danger)' }, text: t('plg_load_failed', 'Failed to load plugins: {error}', { error: loadError }) }));
      return;
    }

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
          h('h3', { className: 'card-title', text: t('plg_installed_title', 'Installed Hardware Plugins') }),
          h('div', { className: 'text-dim', style: { fontSize: '12px' }, text: t('plg_installed_desc', 'Installed dynamic device drivers & daemons') }),
        ]),
        h('div', { style: { display: 'flex', gap: '8px', flexWrap: 'wrap' } }, [
          h('button', {
            className: 'btn btn-secondary btn-sm',
            text: t('plg_dev_download_btn', '📥 Dev Download (GitHub Raw)'),
            onclick: () => openDevDownloadModal(),
          }),
          h('button', {
            className: 'btn btn-primary btn-sm',
            text: t('plg_upload_btn', '+ Upload Plugin (.tar.gz)'),
            onclick: () => openUploadModal(),
          }),
        ]),
      ]),
    ]);

    if (storageInfo) {
      const pct = storageInfo.total_bytes > 0
        ? Math.round((storageInfo.used_bytes / storageInfo.total_bytes) * 100)
        : 0;
      card.appendChild(h('div', {
        style: {
          margin: '10px 16px 14px',
          padding: '10px 14px',
          background: 'var(--bg-card)',
          border: '1px solid var(--border)',
          borderRadius: '8px',
          display: 'flex',
          flexDirection: 'column',
          gap: '6px'
        }
      }, [
        h('div', { style: { display: 'flex', justifyContent: 'space-between', fontSize: '12px' } }, [
          h('span', { text: t('plg_storage_line', '💾 Hub Storage (/data): {free} free of {total}', { free: storageInfo.free_formatted || '0 B', total: storageInfo.total_formatted || '0 B' }) }),
          h('span', { className: 'text-dim', text: t('plg_storage_used', '{pct}% used', { pct }) })
        ]),
        h('div', { style: { width: '100%', height: '6px', background: 'var(--border)', borderRadius: '3px', overflow: 'hidden' } }, [
          h('div', { style: { width: `${pct}%`, height: '100%', background: pct > 90 ? 'var(--danger)' : 'var(--primary)' } })
        ])
      ]));
    }

    const list = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } });

    if (installedPlugins.length === 0) {
      list.appendChild(h('div', { className: 'text-dim', style: { padding: '12px' }, text: t('plg_none_installed', 'No plugins installed yet. Explore the Community Marketplace to add device integrations.') }));
    } else {
      installedPlugins.forEach(p => {
        const isRunning = p.running;
        const isEnabled = p.enabled !== false;

        const badgeClass = !isEnabled ? 'badge-secondary' : (isRunning ? 'badge-success' : 'badge-danger');
        const badgeText = !isEnabled ? t('plg_status_disabled', 'Disabled') : (isRunning ? t('plg_status_running', 'Running (PID {pid})', { pid: p.pid }) : t('plg_status_stopped', 'Stopped'));

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
              h('span', { className: 'badge badge-secondary', style: { fontSize: '11px' }, text: `💾 ${p.size_formatted || '0 B'}` }),
            ]),
            h('div', { className: 'text-dim', style: { fontSize: '13px' }, text: p.description || t('plg_no_description', 'No description provided') }),
            h('div', { className: 'text-dim', style: { fontSize: '11px' }, text: t('plg_id_socket', 'ID: {id} | Socket: /tmp/plugins/{id}.sock', { id: p.id }) }),
          ]),
          h('div', { style: { display: 'flex', alignItems: 'center', gap: '8px' } }, [
            h('button', {
              className: `btn btn-sm ${isEnabled ? 'btn-secondary' : 'btn-primary'}`,
              text: isEnabled ? t('plg_disable', 'Disable') : t('plg_enable', 'Enable'),
              onclick: () => runAction(async () => {
                await api.pluginAction(p.id, isEnabled ? 'disable' : 'enable');
                toast.info(isEnabled ? t('plg_toast_disabled', 'Plugin {id} disabled', { id: p.id }) : t('plg_toast_enabled', 'Plugin {id} enabled', { id: p.id }));
              }, 'plg_action_failed', 'Action failed: {error}')
            }),
            isEnabled && h('button', {
              className: 'btn btn-secondary btn-sm',
              text: t('plg_restart', 'Restart'),
              onclick: () => runAction(async () => {
                await api.pluginAction(p.id, 'restart');
                toast.success(t('plg_restarted', 'Restarted {name}', { name: p.name || p.id }));
              }, 'plg_restart_failed', 'Restart failed: {error}')
            }),
            h('button', {
              className: 'btn btn-danger btn-sm',
              text: t('plg_uninstall', 'Uninstall'),
              onclick: async () => {
                const ok = await confirm.ask({
                  title: t('plg_uninstall_title', 'Uninstall Plugin'),
                  message: t('plg_uninstall_confirm', "Are you sure you want to uninstall '{name}'?", { name: p.name || p.id }),
                  confirmText: t('plg_uninstall', 'Uninstall'),
                  danger: true
                });
                if (ok) {
                  try {
                    const res = await api.uninstallPlugin(p.id);
                    if (res && res.error) {
                      toast.error(t('plg_cannot_uninstall', 'Cannot uninstall: {error}', { error: res.error }));
                    } else {
                      toast.success(t('plg_uninstalled', 'Uninstalled {name}', { name: p.name || p.id }));
                      renderContent();
                    }
                  } catch (err) {
                    toast.error(t('plg_cannot_uninstall', 'Cannot uninstall: {error}', { error: err.message }));
                  }
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
          h('h3', { className: 'card-title', text: t('plg_market_title', 'Community Plugin Marketplace') }),
          h('div', { className: 'text-dim', style: { fontSize: '12px' }, text: t('plg_market_desc', 'Curated integrations from official & custom repositories') }),
        ]),
        h('button', {
          className: 'btn btn-secondary btn-sm',
          text: t('plg_refresh_catalog', '🔄 Refresh Catalog'),
          onclick: () => renderContent(true),
        }),
      ]),
    ]);

    const grid = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } });

    if (marketplacePlugins.length === 0) {
      grid.appendChild(h('div', { className: 'text-dim', style: { padding: '12px' }, text: t('plg_market_empty', 'No plugins found in configured repositories.') }));
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
            safeHttpUrl(p.homepage) && h('a', {
              href: safeHttpUrl(p.homepage),
              target: '_blank',
              rel: 'noopener noreferrer',
              style: { fontSize: '11px', color: 'var(--accent)', textDecoration: 'none' },
              text: t('plg_view_docs', 'View Documentation / Source →')
            }),
          ]),
          h('div', {}, [
            installed && !hasUpdate && h('span', { className: 'badge badge-success', style: { padding: '6px 12px' }, text: t('plg_installed_badge', 'Installed') }),
            hasUpdate && h('button', {
              className: 'btn btn-accent btn-sm',
              text: t('plg_update_btn', 'Update (v{version})', { version: p.version }),
              onclick: () => installFromRepo(p)
            }),
            !installed && h('button', {
              className: 'btn btn-primary btn-sm',
              text: t('plg_install', 'Install'),
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
          h('h3', { className: 'card-title', text: t('plg_repos_title', 'Configured Repositories') }),
          h('div', { className: 'text-dim', style: { fontSize: '12px' }, text: t('plg_repos_desc', 'Sources used to discover and update community plugins') }),
        ]),
        h('button', {
          className: 'btn btn-primary btn-sm',
          text: t('plg_add_repo_btn', '+ Add Repository'),
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
          text: t('plg_remove', 'Remove'),
          onclick: () => runAction(async () => {
            await api.updatePluginRepositories('remove', repoUrl);
            marketplaceStale = true;
            toast.info(t('plg_repo_removed', 'Repository removed'));
          }, 'plg_repo_remove_failed', 'Failed to remove repository: {error}')
        })
      ]);
      list.appendChild(item);
    });

    card.appendChild(list);
    contentContainer.appendChild(card);
  }

  async function installFromRepo(plugin) {
    if (!plugin.download_url) {
      toast.error(t('plg_no_download_url', 'Plugin manifest has no download_url'));
      return;
    }
    toast.info(t('plg_installing', 'Installing {name}...', { name: plugin.name }));
    try {
      // 1. Primary: Hub direct download
      const res = await api.installPlugin(plugin.download_url);
      if (res && res.ok) {
        toast.success(t('plg_install_ok', 'Successfully installed {name}!', { name: plugin.name }));
        renderContent();
        return;
      }
      throw new Error(res?.error || 'Direct download failed');
    } catch (directErr) {
      // 2. Browser download, only when the Hub could not download (502) and with the published
      // checksum, which the Hub checks: a refusal (checksum, authentication, flash space) or a
      // timeout (the Hub may still be installing) is never retried from here
      if (directErr.status !== 502 || !plugin.sha256) {
        toast.error(t('plg_install_failed_generic', 'Failed to install plugin: {error}', { error: directErr.message }));
        return;
      }
      try {
        toast.info(t('plg_proxy_try', 'Direct download failed, attempting browser proxy download...'));
        const resp = await fetch(plugin.download_url);
        if (!resp.ok) throw new Error(`HTTP error ${resp.status}`);
        const blob = await resp.blob();
        const reader = new FileReader();
        reader.onloadend = async () => {
          try {
            const base64 = reader.result.split(',')[1];
            const uploadRes = await api.installPlugin(base64, false, plugin.sha256);
            if (uploadRes && uploadRes.ok) {
              toast.success(t('plg_install_ok_proxy', 'Successfully installed {name} via browser proxy!', { name: plugin.name }));
              renderContent();
            } else {
              toast.error(t('plg_install_failed', 'Install failed: {error}', { error: uploadRes?.error || t('plg_unknown_error', 'Unknown error') }));
            }
          } catch (upErr) {
            toast.error(t('plg_proxy_upload_failed', 'Proxy upload failed: {error}', { error: upErr.message }));
          }
        };
        reader.readAsDataURL(blob);
      } catch (proxyErr) {
        toast.error(t('plg_install_failed_generic', 'Failed to install plugin: {error}', { error: proxyErr.message }));
      }
    }
  }

  function openDevDownloadModal() {
    const urlInput = h('input', {
      type: 'text',
      className: 'form-input',
      placeholder: 'https://raw.githubusercontent.com/user/repo/main/plugins/my-plugin/manifest.json'  // i18n-ok: example URL
    });
    const content = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } }, [
      h('div', { className: 'text-dim', text: t('plg_dev_intro', 'Enter a direct link to a raw GitHub manifest.json file or folder. All files listed in manifest will be downloaded directly to the Hub without packaging.') }),
      h('label', { className: 'form-label', text: t('plg_dev_url_label', 'Raw GitHub Manifest URL') }),
      urlInput,
    ]);

    createModal({
      title: t('plg_dev_title', 'Download Dev Plugin from GitHub'),
      content,
      actions: [
        { label: t('cancel', 'Cancel') },
        {
          label: t('plg_download_install', 'Download & Install'),
          variant: 'primary',
          onClick: async () => {
            const url = urlInput.value.trim();
            if (!url) {
              toast.warn(t('plg_enter_url', 'Please enter a valid URL'));
              return false;
            }
            toast.info(t('plg_dev_downloading', 'Downloading plugin files directly from repository...'));
            try {
              const res = await api.installPlugin(url, true);
              if (res && res.ok) {
                toast.success(t('plg_dev_installed', 'Dev plugin installed successfully!'));
                renderContent();
                return true;
              } else {
                toast.error(t('plg_install_error', 'Install error: {error}', { error: res?.error || res?.message || t('plg_failed', 'Failed') }));
                return false;
              }
            } catch (err) {
              toast.error(err.message);
              return false;
            }
          }
        }
      ]
    });
  }

  function openUploadModal() {
    const fileInput = h('input', { type: 'file', accept: '.tar.gz,.tgz', className: 'form-input' });
    const content = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } }, [
      h('div', { className: 'text-dim', text: t('plg_upload_intro', 'Select a plugin archive (.tar.gz) from your computer to install directly on the Hub.') }),
      fileInput,
    ]);

    createModal({
      title: t('plg_upload_title', 'Upload Plugin Package'),
      content,
      actions: [
        { label: t('cancel', 'Cancel') },
        {
          label: t('plg_upload_install', 'Upload & Install'),
          variant: 'primary',
          onClick: async () => {
            const file = fileInput.files?.[0];
            if (!file) {
              toast.warn(t('plg_select_archive', 'Please select a .tar.gz file'));
              return false;
            }
            // the Hub refuses an archive over 1 MB (and a request over about 1.5 MB before reading it)
            if (file.size > 1024 * 1024) {
              toast.error(t('plg_upload_too_large', 'The plugin archive is larger than 1 MB, the most the Hub accepts'));
              return false;
            }
            toast.info(t('plg_uploading', 'Uploading {name}...', { name: file.name }));
            const reader = new FileReader();
            reader.onloadend = async () => {
              try {
                const b64 = reader.result.split(',')[1];
                // a local archive is a dev install: there is no published checksum for it
                const res = await api.installPlugin(b64, true);
                if (res && res.ok) {
                  toast.success(t('plg_uploaded', 'Plugin uploaded and installed!'));
                  renderContent();
                } else {
                  toast.error(t('plg_install_error', 'Install error: {error}', { error: res?.error || t('plg_failed', 'Failed') }));
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
      placeholder: 'https://raw.githubusercontent.com/user/repo/main/repository.json'  // i18n-ok: example URL
    });
    const content = h('div', { style: { display: 'flex', flexDirection: 'column', gap: '10px' } }, [
      h('label', { className: 'form-label', text: t('plg_repo_url_label', 'Repository JSON URL') }),
      urlInput,
    ]);

    createModal({
      title: t('plg_add_repo_title', 'Add Plugin Repository'),
      content,
      actions: [
        { label: t('cancel', 'Cancel') },
        {
          label: t('plg_add', 'Add'),
          variant: 'primary',
          onClick: async () => {
            const url = urlInput.value.trim();
            if (!url) return false;
            try {
              await api.updatePluginRepositories('add', url);
            } catch (err) {
              toast.error(t('plg_repo_add_failed', 'Failed to add repository: {error}', { error: err.message }));
              return false;
            }
            marketplaceStale = true;
            toast.success(t('plg_repo_added', 'Repository added'));
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
