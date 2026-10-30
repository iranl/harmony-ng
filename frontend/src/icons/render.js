// Icon rendering helpers with offline cache support
import { h } from '../utils/dom.js';
import { getCachedIconUrl, hasCachedIcon, initIconCache } from './cache.js';

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
    src: `/api/icons/${encodeURIComponent(cleanKey)}.png`,
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

export function renderActivityIcon(activity, className = 'activity-icon') {
  if (!activity) return h('span', { className });
  return renderIcon(activity.icon, {
    className,
    alt: activity.name,
    fallbackText: activity.name || 'AC',
  });
}

export function renderDeviceIcon(device, className = 'device-icon') {
  if (!device) return h('span', { className });
  return renderIcon(device.icon, {
    className,
    alt: device.name,
    fallbackText: device.name || 'DV',
  });
}
