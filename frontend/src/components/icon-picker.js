// Activity and Device Icon Picker Overlay Component
import { h } from '../utils/dom.js';
import { createModal } from './modal.js';
import { renderActivityIcon, renderDeviceIcon } from '../icons/render.js';
import { api } from '../api/index.js';
import { t } from '../i18n/index.js';
import iconMeta from '../icons/metadata.json';

// The remote draws three kinds of activity and device icon:
// - keywords (rf_activity_icon_keyword in payload/source/rf_proto.h), which a theme can redraw;
// - the remote's stock images, sent as a 16-bit index (payload/source/remote_stock_icons.c);
// - icons uploaded to the Hub, which the Hub streams to the remote.
const KEYWORDS = [
  ['watchtv', 'Watch TV'], ['movie', 'Watch Movie / Stream'], ['music', 'Listen to Music'],
  ['game', 'Play Game'], ['ps', 'PlayStation'], ['xbox', 'Xbox'], ['switch', 'Nintendo Switch'],
  ['shield', 'NVIDIA Shield'], ['apple', 'Apple TV'], ['roku', 'Roku'], ['firetv', 'Fire TV'],
  ['pc', 'PC / Computer'], ['smarttv', 'Smart TV'], ['sonos', 'Sonos'],
  ['receiver', 'AV Receiver / Amplifier'], ['light', 'Smart Lights'], ['power', 'Power Off'],
  ['projector', 'Projector'], ['bluray', 'Blu-ray / DVD'], ['satellite', 'Satellite'], ['soundbar', 'Soundbar'],
  ['hifi', 'Hi-Fi / Vinyl'], ['netflix', 'Netflix'], ['night', 'Night'], ['fireplace', 'Fireplace'],
  ['sunrise', 'Morning'], ['fan', 'Fan'],
];

// The stock activity images at the size the activity list draws
const STOCK = Object.entries(iconMeta)
  .filter(([file, meta]) => meta.category === 'activity' && file.endsWith('_small.png'))
  .map(([file, meta]) => ({ key: file.replace(/\.png$/, ''), label: meta.name, tags: meta.tags || [] }));

const iconKey = (icon) => String(icon || '').split('/').pop().replace(/\.png$/i, '').toLowerCase();

// kind: 'activity' or 'device', which the previews draw the icon as
export function openIconPicker(currentIcon = '', onSelect, { kind = 'activity' } = {}) {
  let filterText = '';
  let uploaded = [];
  const current = iconKey(currentIcon);
  const searchInput = h('input', {
    type: 'text',
    className: 'form-input',
    placeholder: t('search_icons', 'Search icons by name...'),
    style: { marginBottom: '12px' },
  });

  const gridContainer = h('div', {
    style: {
      display: 'grid',
      gridTemplateColumns: 'repeat(auto-fill, minmax(88px, 1fr))',
      gap: '8px',
      maxHeight: '360px',
      overflowY: 'auto',
      padding: '4px',
    },
  });

  function renderGrid() {
    gridContainer.innerHTML = '';
    // The remote resolves a keyword or stock icon before the name, so the label doesn't affect the image
    const renderRemote = kind === 'device' ? renderDeviceIcon : renderActivityIcon;
    const remoteIcon = ({ key, label }) => () => renderRemote({ id: `icon-picker-${key}`, name: label, icon: key }, 'app-icon');
    const keywordGroup = kind === 'device' ? t('icons_keywords_theme', 'Theme icons') : t('icons_keywords', 'Activity types');
    const items = [
      ...KEYWORDS.map(([key, label]) => ({ group: keywordGroup, key, label, preview: remoteIcon({ key, label }) })),
      ...uploaded.map(u => ({
        group: t('icons_uploaded', 'Uploaded'),
        key: iconKey(u.filename),
        label: iconKey(u.filename),
        preview: () => h('img', { src: `${api.baseUrl}${u.url}`, className: 'app-icon', alt: '' }),
      })),
      ...STOCK.map(st => ({ group: t('icons_stock', 'Remote images'), ...st, preview: remoteIcon(st) })),
    ];

    let group = null;
    items.filter(it => !filterText || [it.label, it.key, ...(it.tags || [])].some(w => w.toLowerCase().includes(filterText)))
      .forEach(it => {
        if (it.group !== group) {
          group = it.group;
          gridContainer.appendChild(h('div', { className: 'form-label', text: group, style: { gridColumn: '1 / -1', margin: '4px 0 0' } }));
        }
        const isSelected = iconKey(it.key) === current;
        const preview = it.preview();
        preview.width = 32;
        preview.height = 32;

        gridContainer.appendChild(h('button', {
          className: 'btn btn-secondary',
          title: it.label,
          style: {
            padding: '8px 4px',
            display: 'flex',
            flexDirection: 'column',
            gap: '4px',
            alignItems: 'center',
            justifyContent: 'center',
            borderColor: isSelected ? 'var(--accent)' : 'var(--border)',
            background: isSelected ? 'rgba(56, 189, 248, 0.2)' : 'var(--bg-card)',
          },
          onclick: () => {
            if (onSelect) onSelect(it.key);
            modal.close();
          },
        }, [
          preview,
          // Some keywords share an image on the remote, so the label tells them apart
          h('span', { text: it.label, style: { fontSize: '11px', lineHeight: '1.2', textAlign: 'center', overflowWrap: 'anywhere' } }),
        ]));
      });
  }

  searchInput.oninput = (e) => {
    filterText = e.target.value.toLowerCase().trim();
    renderGrid();
  };

  const body = h('div', {}, [searchInput, gridContainer]);
  renderGrid();
  api.getCustomIcons().then((list) => {
    uploaded = Array.isArray(list) ? list : [];
    if (uploaded.length) renderGrid();
  }).catch(() => {});

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
