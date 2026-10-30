// Backup & Restore Configuration View
import { h } from '../../utils/dom.js';
import { api } from '../../api/index.js';
import { t } from '../../i18n/index.js';
import { toast } from '../../components/toast.js';
import { confirm } from '../../components/confirm.js';

export function createView() {
  const container = h('div', { className: 'setup-subview-backup', style: { display: 'flex', flexDirection: 'column', gap: '16px' } });

  const exportCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('backup_export', 'Export Backup') }),
    ]),
    h('p', { className: 'text-dim', style: { fontSize: '13px', marginBottom: '12px' }, text: 'Download a backup archive of your activities, devices, and remote mappings.' }),
    h('div', { style: { display: 'flex', gap: '8px', flexWrap: 'wrap' } }, [
      h('button', {
        className: 'btn btn-primary',
        text: '💾 Download Full Backup (JSON)',
        onclick: () => downloadExport('all'),
      }),
      h('button', {
        className: 'btn btn-secondary',
        text: 'Export Activities',
        onclick: () => downloadExport('activities'),
      }),
      h('button', {
        className: 'btn btn-secondary',
        text: 'Export Devices',
        onclick: () => downloadExport('devices'),
      }),
      h('button', {
        className: 'btn btn-secondary',
        text: 'Export Remotes',
        onclick: () => downloadExport('remotes'),
      }),
    ]),
  ]);

  const importTextarea = h('textarea', {
    className: 'form-textarea',
    rows: 6,
    placeholder: 'Paste exported JSON backup content here...',
  });

  const fileInput = h('input', {
    type: 'file',
    accept: '.json,application/json',
    style: { display: 'none' },
    onchange: (e) => {
      const file = e.target.files?.[0];
      if (!file) return;
      const reader = new FileReader();
      reader.onload = (evt) => {
        importTextarea.value = evt.target.result;
        toast.info('File loaded into text area');
      };
      reader.readAsText(file);
    },
  });

  const importCard = h('div', { className: 'card' }, [
    h('div', { className: 'card-header' }, [
      h('h3', { className: 'card-title', text: t('backup_import', 'Restore Backup') }),
      h('button', {
        className: 'btn btn-secondary btn-sm',
        text: 'Choose File...',
        onclick: () => fileInput.click(),
      }),
    ]),
    fileInput,
    h('div', { className: 'form-group' }, [
      importTextarea,
    ]),
    h('button', {
      className: 'btn btn-primary',
      text: '📥 Restore Configuration to Hub',
      onclick: async () => {
        const text = importTextarea.value.trim();
        if (!text) return toast.warn('Please paste JSON or choose a backup file');
        const ok = await confirm.ask({
          title: 'Restore Configuration',
          message: 'Restoring will overwrite current activities and devices. Continue?',
          danger: true,
        });
        if (ok) {
          try {
            toast.info('Restoring backup to Hub...');
            await api.restoreBackup('all', text, false);
            toast.success('Backup restored successfully!');
          } catch (e) {
            toast.error(e.message);
          }
        }
      },
    }),
  ]);

  async function downloadExport(target) {
    try {
      toast.info(`Generating ${target} export...`);
      const data = await api.fetchExportText(target);
      const str = typeof data === 'string' ? data : JSON.stringify(data, null, 2);
      const blob = new Blob([str], { type: 'application/json' });
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = `harmony_backup_${target}_${new Date().toISOString().slice(0, 10)}.json`;
      a.click();
      URL.revokeObjectURL(url);
      toast.success('Backup downloaded');
    } catch (e) {
      toast.error(e.message);
    }
  }

  container.appendChild(exportCard);
  container.appendChild(importCard);
  return container;
}

export default createView;
