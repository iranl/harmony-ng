// Icon rendering helpers with offline cache support
import { h } from '../utils/dom.js';
import { getCachedIconUrl, hasCachedIcon, initIconCache } from './cache.js';
import { api } from '../api/index.js';
import { loadRemoteRenderer } from '../remote/remote-renderer.js';

initIconCache().catch(() => {});

export function renderIcon(iconKey, options = {}) {
  const {
    className = 'app-icon',
    alt = '',
    fallbackText = '',
    size = 24,
  } = options;

  if (!iconKey) {
    if (fallbackText) {
      return h('span', { className: `${className} icon-fallback`, text: fallbackText.slice(0, 2).toUpperCase() });
    }
    return h('span', { className: `${className} icon-empty` });
  }

  // If already a data URL or blob URL or http URL
  if (iconKey.startsWith('data:') || iconKey.startsWith('blob:') || iconKey.startsWith('http')) {
    return h('img', {
      src: iconKey,
      className,
      alt,
      width: size,
      height: size,
      loading: 'lazy',
    });
  }

  // Check cached blob
  const cleanKey = iconKey.replace(/\.png$/i, '');
  const cachedUrl = getCachedIconUrl(cleanKey) || getCachedIconUrl(iconKey);

  if (cachedUrl) {
    return h('img', {
      src: cachedUrl,
      className,
      alt: alt || cleanKey,
      width: size,
      height: size,
      loading: 'lazy',
    });
  }

  // Fallback to hub endpoint or letter badge
  const img = h('img', {
    src: `/assets/icons/${encodeURIComponent(cleanKey)}.png`,
    className,
    alt: alt || cleanKey,
    width: size,
    height: size,
    loading: 'lazy',
  });

  img.onerror = () => {
    if (fallbackText) {
      const badge = h('span', { className: `${className} icon-fallback`, text: fallbackText.slice(0, 2).toUpperCase() });
      if (img.parentNode) {
        img.parentNode.replaceChild(badge, img);
      }
    } else {
      img.style.display = 'none';
    }
  };

  return img;
}

// Activity and device icons are keywords (or uploaded images) that the remote resolves to its
// own images, in its theme; the remote's renderer resolves them the same way, so the web UI
// shows the image the remote shows. Uploaded icons, which the Hub sends to the remote, load
// from the Hub.
let uploadedIcons = null;
async function remoteIconUrl(kind, item) {
  uploadedIcons = uploadedIcons || api.getCustomIcons().catch(() => []);
  const key = String(item.icon || '').split('/').pop().replace(/\.png$/i, '').toLowerCase();
  const uploaded = ((await uploadedIcons) || []).find(u => (u.type === kind && String(u.id) === String(item.id))
    || (key && [u.id, String(u.filename || '').replace(/\.png$/i, '')].some(n => String(n || '').toLowerCase() === key)));
  if (uploaded?.url) return `${api.baseUrl}${uploaded.url}`;
  const renderer = await loadRemoteRenderer();
  if (!renderer) return '';
  if (kind === 'device') {
    renderer.preview.writeRemoteConfig(renderer.module, { devices: [item] });
    return renderer.preview.deviceIconUrl(renderer.module, item.id, renderer.theme);
  }
  renderer.preview.writeRemoteConfig(renderer.module, { activities: [item] });
  return renderer.preview.activityIconUrl(renderer.module, item.id, renderer.theme);
}

function renderRemoteIcon(kind, item, className, fallbackText) {
  if (!item) return h('span', { className });
  const fallback = () => renderIcon(item.icon, { className, alt: item.name, fallbackText: item.name || fallbackText });
  const img = h('img', { className, alt: item.name || '', width: 24, height: 24 });
  remoteIconUrl(kind, item).then((url) => {
    if (url) img.src = url;
    else img.replaceWith(fallback());
  });
  return img;
}

export function renderActivityIcon(activity, className = 'activity-icon') {
  return renderRemoteIcon('activity', activity, className, 'AC');
}

export function renderDeviceIcon(device, className = 'device-icon') {
  return renderRemoteIcon('device', device, className, 'DV');
}
