// Icon Picker Overlay Component
import { h } from '../utils/dom.js';
import { createModal } from './modal.js';
import { renderIcon } from '../icons/render.js';
import iconMeta from '../icons/metadata.json';
import { t } from '../i18n/index.js';

export function openIconPicker(currentIcon = '', onSelect) {
  let filterText = '';
  const searchInput = h('input', {
    type: 'text',
    className: 'form-input',
    placeholder: t('search_icons', 'Search icons by name...'),
    style: { marginBottom: '12px' },
  });

  const gridContainer = h('div', {
    style: {
      display: 'grid',
      gridTemplateColumns: 'repeat(auto-fill, minmax(54px, 1fr))',
      gap: '8px',
      maxHeight: '360px',
      overflowY: 'auto',
      padding: '4px',
    },
  });

  const allIcons = Array.isArray(iconMeta) ? iconMeta : Object.keys(iconMeta);

  function renderGrid() {
    gridContainer.innerHTML = '';
    const filtered = allIcons.filter(ic => {
      const name = typeof ic === 'string' ? ic : (ic.name || ic.key || '');
      return !filterText || name.toLowerCase().includes(filterText);
    });

    filtered.slice(0, 150).forEach(ic => {
      const iconKey = typeof ic === 'string' ? ic : (ic.key || ic.name);
      const isSelected = iconKey === currentIcon;

      const btn = h('button', {
        className: 'btn btn-secondary',
        style: {
          padding: '8px',
          height: '54px',
          display: 'flex',
          alignItems: 'center',
          justifyContent: 'center',
          borderColor: isSelected ? 'var(--accent)' : 'var(--border)',
          background: isSelected ? 'rgba(56, 189, 248, 0.2)' : 'var(--bg-card)',
        },
        onclick: () => {
          if (onSelect) onSelect(iconKey);
          modal.close();
        },
      }, [
        renderIcon(iconKey, { size: 32, alt: iconKey }),
      ]);

      gridContainer.appendChild(btn);
    });
  }

  searchInput.oninput = (e) => {
    filterText = e.target.value.toLowerCase().trim();
    renderGrid();
  };

  const body = h('div', {}, [searchInput, gridContainer]);
  renderGrid();

  const modal = createModal({
    title: t('select_icon', 'Select Icon'),
    content: body,
    actions: [
      {
        label: t('remove_icon', 'Remove Icon'),
        variant: 'danger',
        onClick: () => {
          if (onSelect) onSelect('');
        },
      },
      {
        label: t('cancel', 'Cancel'),
      },
    ],
  });

  return modal;
}

export default openIconPicker;
