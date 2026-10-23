import { HarmonyClient } from './api.js';
import { getSavedHubs, saveHub, deleteHub, getActiveHub, setActiveHubId } from './storage.js';
import { RemoteEngine } from './remote-engine.js';
import {
  getHarmonyManufacturers,
  searchHarmonyManufacturers,
  fetchHarmonyModels,
  searchHarmonyModels,
  fetchHarmonyDeviceDetail,
  fetchHarmonyCodeset,
  mapHarmonyDeviceType,
  buildCodexDeviceFromHarmony,
  parseIrText,
  loadIrdSource,
  rankIrdEntries,
  fetchIrdFile,
  sourceLabel,
  safeImportName
} from './irdb.js';
import { t, setLanguage, getInitialLanguage } from './i18n.js';
import iconMetadata from './icon-metadata.json';

export const APP_VERSION = '1.8.2';

export let stockIconsAvailable = false;
export const failedIconUrls = new Set();
const iconNameCache = new Map();

export function escapeHtml(s) {
  if (!s) return '';
  return String(s)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

export function autoInvertDarkIcon(img) {
  if (!img) return;
  img.classList.add('invert-white');
  img.style.filter = 'brightness(0) invert(1)';
}

export function showToast(message, type = 'info', duration = 3500) {
  if (typeof document === 'undefined') return;
  let container = document.getElementById('toastContainer');
  if (!container) {
    container = document.createElement('div');
    container.id = 'toastContainer';
    container.className = 'toast-container';
    document.body.appendChild(container);
  }
  const toast = document.createElement('div');
  toast.className = `toast-item toast-${type}`;

  let iconSvg = '';
  if (type === 'success') {
    iconSvg = `<svg class="toast-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><polyline points="20 6 9 17 4 12"></polyline></svg>`;
  } else if (type === 'error') {
    iconSvg = `<svg class="toast-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><circle cx="12" cy="12" r="10"></circle><line x1="15" y1="9" x2="9" y2="15"></line><line x1="9" y1="9" x2="15" y2="15"></line></svg>`;
  } else if (type === 'warning') {
    iconSvg = `<svg class="toast-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"></path><line x1="12" y1="9" x2="12" y2="13"></line><line x1="12" y1="17" x2="12.01" y2="17"></line></svg>`;
  } else {
    iconSvg = `<svg class="toast-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5"><circle cx="12" cy="12" r="10"></circle><line x1="12" y1="16" x2="12" y2="12"></line><line x1="12" y1="8" x2="12.01" y2="8"></line></svg>`;
  }

  toast.innerHTML = `${iconSvg}<div class="toast-content">${escapeHtml(message).replace(/\n/g, '<br>')}</div>`;
  container.appendChild(toast);

  requestAnimationFrame(() => toast.classList.add('toast-show'));

  let hideTimeout = null;
  const removeToast = () => {
    if (!toast.parentNode) return;
    toast.classList.remove('toast-show');
    toast.classList.add('toast-hide');
    setTimeout(() => toast.remove(), 260);
  };

  toast.addEventListener('click', () => {
    if (hideTimeout) clearTimeout(hideTimeout);
    removeToast();
  });
  if (duration > 0) {
    hideTimeout = setTimeout(removeToast, duration);
  }
}

export function showConfirm(message, options = {}) {
  return new Promise((resolve) => {
    if (typeof document === 'undefined') {
      resolve(true);
      return;
    }
    const modal = document.getElementById('confirmModal');
    if (!modal) {
      resolve(true);
      return;
    }
    const titleEl = document.getElementById('confirmModalTitleText');
    const msgEl = document.getElementById('confirmModalMessage');
    const okBtn = document.getElementById('btnConfirmOk');
    const cancelBtn = document.getElementById('btnConfirmCancel');
    const closeBtn = document.getElementById('btnConfirmClose');
    const iconEl = document.getElementById('confirmModalIcon');

    if (titleEl) titleEl.textContent = options.title || 'Confirmation';
    if (msgEl) msgEl.textContent = message || '';
    if (okBtn) {
      okBtn.textContent = options.okText || 'Confirm';
      okBtn.className = `btn btn-sm ${options.danger !== false ? 'btn-danger' : 'btn-primary'}`;
    }
    if (cancelBtn) cancelBtn.textContent = options.cancelText || 'Cancel';
    if (iconEl) iconEl.textContent = options.icon || (options.danger !== false ? '⚠️' : '❓');

    modal.style.display = 'flex';

    let resolved = false;
    const cleanup = (val) => {
      if (resolved) return;
      resolved = true;
      modal.style.display = 'none';
      okBtn?.removeEventListener('click', onOk);
      cancelBtn?.removeEventListener('click', onCancel);
      closeBtn?.removeEventListener('click', onCancel);
      document.removeEventListener('keydown', onKey);
      resolve(val);
    };

    const onOk = (e) => {
      e?.preventDefault();
      cleanup(true);
    };
    const onCancel = (e) => {
      e?.preventDefault();
      cleanup(false);
    };
    const onKey = (e) => {
      if (e.key === 'Escape') {
        e.preventDefault();
        cleanup(false);
      } else if (e.key === 'Enter') {
        e.preventDefault();
        cleanup(true);
      }
    };

    okBtn?.addEventListener('click', onOk);
    cancelBtn?.addEventListener('click', onCancel);
    closeBtn?.addEventListener('click', onCancel);
    document.addEventListener('keydown', onKey);
    setTimeout(() => okBtn?.focus(), 50);
  });
}

if (typeof window !== 'undefined') {
  window.__harmonyFailedIcons = failedIconUrls;
  window.__harmonyAutoInvert = autoInvertDarkIcon;
  window.showToast = showToast;
  window.showConfirm = showConfirm;
  window.alert = (msg) => {
    const str = String(msg || '');
    const isErr = /fail|error|cannot|invalid|not found|please|required/i.test(str);
    const isSuccess = /success|saved|loaded|linked|complete|imported|reset/i.test(str);
    showToast(str, isErr ? 'error' : (isSuccess ? 'success' : 'info'));
  };
}

export function setStockIconsAvailable(avail) {
  stockIconsAvailable = !!avail;
}

export function getStockIconUrl(iconKey, hint = '') {
  if (!stockIconsAvailable) return '';
  if (!iconKey && !hint) return '';
  let fn = '';
  if (iconKey && typeof iconKey === 'string') {
    const clean = iconKey.split('/').pop().trim();
    if (clean) {
      fn = clean.endsWith('.png') ? clean : `${clean}.png`;
    }
  }
  if (!fn && hint && typeof hint === 'string') {
    const h = hint.toLowerCase();
    if (h.includes('tv') || h.includes('television')) fn = 'tv.png';
    else if (h.includes('shield')) fn = 'shield.png';
    else if (h.includes('ps') || h.includes('playstation')) fn = 'ps.png';
    else if (h.includes('xbox')) fn = 'xbox.png';
    else if (h.includes('sonos')) fn = 'sonos.png';
    else if (h.includes('firetv')) fn = 'firetv.png';
    else if (h.includes('heos')) fn = 'heos.png';
    else if (h.includes('switch') || h.includes('nintendo')) fn = 'switch.png';
  }
  if (!fn) return '';
  const url = `/assets/icons/${fn}`;
  if (failedIconUrls.has(url)) return '';
  return url;
}

function renderWithStockIconFallback(imgUrl, fallbackSvg, size) {
  if (!stockIconsAvailable || !imgUrl || failedIconUrls.has(imgUrl)) return fallbackSvg;
  return `<span class="stock-icon-wrap"><img src="${imgUrl}" class="stock-icon-img invert-white" loading="lazy" decoding="async" width="${size}" height="${size}" onerror="if(window.__harmonyFailedIcons)window.__harmonyFailedIcons.add('${imgUrl}');this.style.display='none';if(this.nextElementSibling)this.nextElementSibling.style.display='inline-block'" />${fallbackSvg}</span>`;
}

export function getActivityIconSvg(iconKey, actType, size = 14) {
  const fallbackSvg = getRawActivityIconSvg(iconKey, actType, size);
  return renderWithStockIconFallback(getStockIconUrl(iconKey, actType), fallbackSvg, size);
}

export function getRawActivityIconSvg(iconKey, actType, size = 14) {
  const k = (iconKey || '').toLowerCase();
  const t = (actType || '').toLowerCase();
  if (k.includes('watchtv') || k.includes('tv') || t.includes('television')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><rect x="2" y="7" width="20" height="15" rx="2" ry="2"></rect><polyline points="17 2 12 7 7 2"></polyline></svg>`;
  }
  if (k.includes('movie') || k.includes('cinema') || t.includes('movie')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><rect x="2" y="2" width="20" height="20" rx="2.18" ry="2.18"></rect><line x1="7" y1="2" x2="7" y2="22"></line><line x1="17" y1="2" x2="17" y2="22"></line><line x1="2" y1="12" x2="22" y2="12"></line><line x1="2" y1="7" x2="7" y2="7"></line><line x1="2" y1="17" x2="7" y2="17"></line><line x1="17" y1="17" x2="22" y2="17"></line><line x1="17" y1="7" x2="22" y2="7"></line></svg>`;
  }
  if (k.includes('music') || t.includes('music') || k.includes('audio') || k.includes('radio')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><path d="M9 18V5l12-2v13"></path><circle cx="6" cy="18" r="3"></circle><circle cx="18" cy="16" r="3"></circle></svg>`;
  }
  if (k.includes('game') || t.includes('game')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><line x1="6" y1="12" x2="10" y2="12"></line><line x1="8" y1="10" x2="8" y2="14"></line><line x1="15" y1="13" x2="15.01" y2="13"></line><line x1="18" y1="11" x2="18.01" y2="11"></line><rect x="2" y="6" width="20" height="12" rx="2"></rect></svg>`;
  }
  if (k.includes('shield')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"></path></svg>`;
  }
  if (k.includes('ps') || k.includes('playstation')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><polygon points="6 3 18 3 12 12 6 3"></polygon><circle cx="12" cy="17" r="4"></circle></svg>`;
  }
  if (k.includes('xbox')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><circle cx="12" cy="12" r="10"></circle><path d="m5 5 14 14"></path><path d="m19 5-14 14"></path></svg>`;
  }
  if (k.includes('switch') || k.includes('nintendo')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><rect x="5" y="4" width="6" height="16" rx="3"></rect><rect x="13" y="4" width="6" height="16" rx="3"></rect><circle cx="8" cy="8" r="1"></circle><circle cx="16" cy="14" r="1"></circle></svg>`;
  }
  if (k.includes('apple')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><path d="M12 20.94c1.88 0 3.05-.91 4.19-.91 1.15 0 2.15.91 3.81.91 1.77 0 3.32-1.07 4.38-2.61-3.66-1.88-3.07-6.95.73-8.52-1.89-2.58-4.63-2.83-5.61-2.83-2.39 0-4.32 1.48-5.46 1.48-1.2 0-3.05-1.42-5.07-1.42-2.6 0-5.02 1.54-6.38 3.91-2.73 4.75-.7 11.75 1.95 15.58 1.3 1.88 2.83 3.96 4.86 3.96z"></path></svg>`;
  }
  if (k.includes('roku') || k.includes('firetv') || k.includes('stream')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><polygon points="5 3 19 12 5 21 5 3"></polygon></svg>`;
  }
  if (k.includes('pc') || k.includes('computer')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><rect x="2" y="3" width="20" height="14" rx="2"></rect><line x1="8" y1="21" x2="16" y2="21"></line><line x1="12" y1="17" x2="12" y2="21"></line></svg>`;
  }
  if (k.includes('sonos') || k.includes('heos') || k.includes('receiver') || k.includes('denon')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><rect x="4" y="2" width="16" height="20" rx="2"></rect><circle cx="12" cy="14" r="4"></circle><line x1="12" y1="6" x2="12.01" y2="6"></line></svg>`;
  }
  if (k.includes('light') || k.includes('bulb')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><path d="M12 2v2"></path><path d="M12 20v2"></path><circle cx="12" cy="12" r="6"></circle></svg>`;
  }
  if (k.includes('power')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><path d="M18.36 6.64a9 9 0 1 1-12.73 0"></path><line x1="12" y1="2" x2="12" y2="12"></line></svg>`;
  }
  return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><polygon points="12 2 15.09 8.26 22 9.27 17 14.14 18.18 21.02 12 17.77 5.82 21.02 7 14.14 2 9.27 8.91 8.26 12 2"></polygon></svg>`;
}

export function getScreenBtnIconSvg(iconKey = '', label = '', size = 12) {
  const fallbackSvg = getRawScreenBtnIconSvg(iconKey, label, size);
  return renderWithStockIconFallback(getStockIconUrl(iconKey, label), fallbackSvg, size);
}

export function getRawScreenBtnIconSvg(iconKey = '', label = '', size = 12) {
  const k = (iconKey || '').toLowerCase();
  const l = (label || '').toLowerCase();
  const q = k || l;

  if (q.includes('power') || q.includes('pwr') || q.includes('standby')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><path d="M18.36 6.64a9 9 0 1 1-12.73 0"></path><line x1="12" y1="2" x2="12" y2="12"></line></svg>`;
  }
  if (q.includes('playpause')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><polygon points="5 3 13 8 5 13 5 3"></polygon><line x1="16" y1="3" x2="16" y2="13"></line><line x1="20" y1="3" x2="20" y2="13"></line></svg>`;
  }
  if (q.includes('play')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><polygon points="5 3 19 12 5 21 5 3"></polygon></svg>`;
  }
  if (q.includes('pause')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><rect x="6" y="4" width="4" height="16"></rect><rect x="14" y="4" width="4" height="16"></rect></svg>`;
  }
  if (q.includes('stop')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><rect x="4" y="4" width="16" height="16"></rect></svg>`;
  }
  if (q.includes('ff') || q.includes('fastforward')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><polygon points="13 19 22 12 13 5 13 19"></polygon><polygon points="2 19 11 12 2 5 2 19"></polygon></svg>`;
  }
  if (q.includes('rew') || q.includes('rewind')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><polygon points="11 19 2 12 11 5 11 19"></polygon><polygon points="22 19 13 12 22 5 22 19"></polygon></svg>`;
  }
  if (q.includes('mute')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><path d="M11 5L6 9H2v6h4l5 4V5z"></path><line x1="23" y1="9" x2="17" y2="15"></line><line x1="17" y1="9" x2="23" y2="15"></line></svg>`;
  }
  if (q.includes('vol') || q.includes('volume')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><path d="M11 5L6 9H2v6h4l5 4V5z"></path><path d="M19.07 4.93a10 10 0 0 1 0 14.14M15.54 8.46a5 5 0 0 1 0 7.07"></path></svg>`;
  }
  if (q.includes('denon') || q.includes('avr') || q.includes('receiver') || q.includes('amp')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><rect x="4" y="2" width="16" height="20" rx="2"></rect><circle cx="12" cy="14" r="4"></circle><line x1="12" y1="6" x2="12.01" y2="6"></line></svg>`;
  }
  if (q.includes('tv') || q.includes('television')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><rect x="2" y="7" width="20" height="15" rx="2"></rect><polyline points="17 2 12 7 7 2"></polyline></svg>`;
  }
  if (q.includes('shield')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"></path></svg>`;
  }
  if (q.includes('ps') || q.includes('playstation')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><polygon points="6 3 18 3 12 12 6 3"></polygon><circle cx="12" cy="17" r="4"></circle></svg>`;
  }
  if (q.includes('xbox')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><circle cx="12" cy="12" r="10"></circle><path d="m5 5 14 14"></path><path d="m19 5-14 14"></path></svg>`;
  }
  if (q.includes('home')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><path d="m3 9 9-7 9 7v11a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"></path></svg>`;
  }
  if (q.includes('menu')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><line x1="4" y1="12" x2="20" y2="12"></line><line x1="4" y1="6" x2="20" y2="6"></line><line x1="4" y1="18" x2="20" y2="18"></line></svg>`;
  }
  if (q.includes('back') || q.includes('return')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><polyline points="15 18 9 12 15 6"></polyline></svg>`;
  }
  if (q.includes('exit')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><line x1="18" y1="6" x2="6" y2="18"></line><line x1="6" y1="6" x2="18" y2="18"></line></svg>`;
  }
  if (q.includes('info')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><circle cx="12" cy="12" r="10"></circle><line x1="12" y1="16" x2="12" y2="12"></line><line x1="12" y1="8" x2="12.01" y2="8"></line></svg>`;
  }
  if (q.includes('search')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><circle cx="11" cy="11" r="8"></circle><line x1="21" y1="21" x2="16.65" y2="16.65"></line></svg>`;
  }
  if (q.includes('light') || q.includes('lamp') || q.includes('bulb')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><path d="M12 2v2"></path><path d="M12 20v2"></path><circle cx="12" cy="12" r="6"></circle></svg>`;
  }
  return '';
}

export function getDeviceIconSvg(type = '', mfg = '', model = '', iconKey = '', size = 16) {
  const fallbackSvg = getRawDeviceIconSvg(type, mfg, model, iconKey, size);
  return renderWithStockIconFallback(getStockIconUrl(iconKey, type || model || mfg), fallbackSvg, size);
}

export function getRawDeviceIconSvg(type = '', mfg = '', model = '', iconKey = '', size = 16) {
  const t = (type || '').toLowerCase();
  const m = (model || '').toLowerCase();
  const f = (mfg || '').toLowerCase();
  const k = (iconKey || '').toLowerCase();

  // TV / Display
  if (t.includes('tv') || t.includes('television') || t.includes('display') || k.includes('tv') || m.includes('oled') || m.includes('qled') || m.includes('bravia')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><rect x="2" y="7" width="20" height="15" rx="2" ry="2"></rect><polyline points="17 2 12 7 7 2"></polyline></svg>`;
  }
  // Audio / Receiver / Soundbar / AVR / Amplifier
  if (t.includes('audio') || t.includes('receiver') || t.includes('avr') || t.includes('amp') || t.includes('soundbar') || t.includes('stereo') || f.includes('denon') || f.includes('marantz') || f.includes('yamaha') || f.includes('onkyo') || f.includes('sonos')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><rect x="4" y="2" width="16" height="20" rx="2"></rect><circle cx="12" cy="14" r="4"></circle><line x1="12" y1="6" x2="12.01" y2="6"></line></svg>`;
  }
  // Media Player / Streaming Box / Apple TV / Roku / FireTV
  if (t.includes('media') || t.includes('stream') || t.includes('box') || m.includes('shield') || m.includes('apple tv') || m.includes('roku') || m.includes('fire') || m.includes('chromecast')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><polygon points="5 3 19 12 5 21 5 3"></polygon></svg>`;
  }
  // Game Console / Xbox / PlayStation / Nintendo / Switch
  if (t.includes('game') || t.includes('console') || m.includes('playstation') || m.includes('ps4') || m.includes('ps5') || m.includes('xbox') || m.includes('switch') || m.includes('nintendo')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><line x1="6" y1="12" x2="10" y2="12"></line><line x1="8" y1="10" x2="8" y2="14"></line><line x1="15" y1="13" x2="15.01" y2="13"></line><line x1="18" y1="11" x2="18.01" y2="11"></line><rect x="2" y="6" width="20" height="12" rx="2"></rect></svg>`;
  }
  // Projector
  if (t.includes('projector') || m.includes('projector')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><rect x="2" y="7" width="20" height="10" rx="2"></rect><circle cx="17" cy="12" r="3"></circle><line x1="6" y1="12" x2="6.01" y2="12"></line><line x1="10" y1="12" x2="10.01" y2="12"></line></svg>`;
  }
  // DVD / Blu-ray
  if (t.includes('bluray') || t.includes('dvd') || t.includes('disc') || m.includes('player')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><circle cx="12" cy="12" r="10"></circle><circle cx="12" cy="12" r="3"></circle></svg>`;
  }
  // Bluetooth / Keyboard / HID
  if (t.includes('keyboard') || t.includes('bt') || t.includes('bluetooth')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><rect x="2" y="4" width="20" height="16" rx="2"></rect><line x1="6" y1="8" x2="6.01" y2="8"></line><line x1="10" y1="8" x2="10.01" y2="8"></line><line x1="14" y1="8" x2="14.01" y2="8"></line><line x1="18" y1="8" x2="18.01" y2="8"></line><line x1="7" y1="16" x2="17" y2="16"></line></svg>`;
  }
  // MQTT / Smart Home / Light
  if (t.includes('mqtt') || t.includes('light') || t.includes('fan') || t.includes('switch') || t.includes('socket')) {
    return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><path d="M12 2v4M12 18v4M4.93 4.93l2.83 2.83M16.24 16.24l2.83 2.83M2 12h4M18 12h4M4.93 19.07l2.83-2.83M16.24 7.76l2.83-2.83"></path></svg>`;
  }
  // Default Generic Device
  return `<svg width="${size}" height="${size}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" style="vertical-align:middle;margin-right:4px;"><rect x="4" y="4" width="16" height="16" rx="2"></rect><rect x="9" y="9" width="6" height="6"></rect><line x1="9" y1="1" x2="9" y2="4"></line><line x1="15" y1="1" x2="15" y2="4"></line><line x1="9" y1="20" x2="9" y2="23"></line><line x1="15" y1="20" x2="15" y2="23"></line><line x1="20" y1="9" x2="23" y2="9"></line><line x1="20" y1="14" x2="23" y2="14"></line><line x1="1" y1="9" x2="4" y2="9"></line><line x1="1" y1="14" x2="4" y2="14"></line></svg>`;
}

class HarmonyApp {
  constructor() {
    this.isNative = Boolean(typeof window !== 'undefined' && window.Capacitor && window.Capacitor.isNativePlatform && window.Capacitor.isNativePlatform());
    this.client = new HarmonyClient(null);
    this.remoteEngine = new RemoteEngine(this.client);
    this.activeHub = null;
    this.inventory = { devices: [] };
    this.activitiesData = { activities: [], currentId: '-1', currentName: 'PowerOff' };
    this.deviceCommandsMap = {};
    this.currentTab = (typeof window !== 'undefined' && window.location.hash.replace('#', '')) || 'activity-remote';
    this.selectedDeviceId = null;
    this.selectedIrDeviceId = null;
    this.pollTimer = null;
    this.wsProgress = null;
    this.isHubOnline = false;

    // Sub-states
    this.b25MapData = { remote: { name: 'Homatics B25' }, activities: {} };
    this.b25SelectedBtn = 'power';
    this.eliteMapData = { remote: { name: 'Harmony Elite' }, activities: {} };
    this.eliteSelectedBtn = 'Select';
    this.eliteSelectedActivityId = '-1';
    this.kbMods = { ctrl: false, shift: false, alt: false, win: false };
    this.labQueue = [];
    this.labRunning = false;
    this.labRunId = null;
    this.autoReconnectTimer = null;
    this.consecutiveFailures = 0;
    this.isReconnecting = false;
    this.availableIcons = [];
    this.currentIconPickerTarget = null;
    this.currentIconPickerBadge = null;
    this.currentIconCategory = 'all';
    this._btCalibrateInterval = null;
    this._btCalibratePoll = null;
    this._lastSuggestedBtDelay = null;
    this._lastCalibratedDevId = null;

    this.setupWebSocket();
  }

  async loadAvailableIcons(force = false) {
    if (!force && this.availableIcons && this.availableIcons.length > 0) return;
    try {
      const icons = await this.client.getIcons();
      if (Array.isArray(icons) && icons.length > 0) {
        this.availableIcons = icons;
        this.iconCatalog = icons.map(fn => {
          const cleanFn = String(fn).split('/').pop().trim();
          const meta = iconMetadata[cleanFn] || iconMetadata[`${cleanFn}.png`] || {};
          const title = this.formatIconDisplayName(cleanFn);
          return {
            fn: cleanFn,
            title,
            lowerTitle: title.toLowerCase(),
            lowerFn: cleanFn.toLowerCase(),
            category: meta.category || 'other',
            tags: (meta.tags || []).join(' ').toLowerCase(),
            url: getStockIconUrl(cleanFn)
          };
        }).sort((a, b) => a.title.localeCompare(b.title, undefined, { sensitivity: 'base' }));

        this.populateIconSelects();
      }
    } catch (e) {
      console.warn('Could not load remote icons list:', e);
    }
  }

  populateIconSelects() {
    if (!this.iconCatalog || this.iconCatalog.length === 0) return;
    const selects = [
      { id: 'actEditIcon', badgeId: 'actIconPreviewBadge' },
      { id: 'editDevIcon', badgeId: 'devIconPreviewBadge' },
      { id: 'newDeviceIcon', badgeId: 'newDevIconPreviewBadge' }
    ];

    selects.forEach(({ id, badgeId }) => {
      const sel = document.getElementById(id);
      if (!sel) return;
      const currentVal = sel.value;
      const existingVals = new Set(Array.from(sel.options).map(o => o.value));

      const frag = document.createDocumentFragment();
      this.iconCatalog.forEach(item => {
        const val = item.fn;
        const valNoExt = val.replace(/\.png$/i, '');
        if (!existingVals.has(val) && !existingVals.has(valNoExt)) {
          const opt = document.createElement('option');
          opt.value = val;
          opt.textContent = item.title;
          frag.appendChild(opt);
          existingVals.add(val);
        }
      });
      sel.appendChild(frag);

      if (currentVal && existingVals.has(currentVal)) {
        sel.value = currentVal;
      }
      this.updateBadgeForSelect(sel, badgeId);
    });
  }

  formatIconDisplayName(fn) {
    if (!fn) return '';
    if (iconNameCache.has(fn)) return iconNameCache.get(fn);
    const cleanFn = String(fn).split('/').pop().trim();
    if (iconNameCache.has(cleanFn)) return iconNameCache.get(cleanFn);
    const withExt = cleanFn.endsWith('.png') ? cleanFn : `${cleanFn}.png`;
    const noExt = cleanFn.replace(/\.png$/i, '');

    let res = '';
    if (iconMetadata[withExt]?.name) res = iconMetadata[withExt].name;
    else if (iconMetadata[cleanFn]?.name) res = iconMetadata[cleanFn].name;
    else {
      const base = noExt.replace(/_(small|smallon|on|off)$/i, '');
      if (iconMetadata[`${base}.png`]?.name) {
        const baseName = iconMetadata[`${base}.png`].name;
        if (noExt.endsWith('_on') || noExt.endsWith('_smallon')) res = `${baseName} (Active)`;
        else if (noExt.endsWith('_off')) res = `${baseName} (Inactive)`;
        else res = baseName;
      } else if (iconMetadata[`${base}_small.png`]?.name) {
        const baseName = iconMetadata[`${base}_small.png`].name;
        if (noExt.endsWith('_on') || noExt.endsWith('_smallon')) res = `${baseName} (Active)`;
        else if (noExt.endsWith('_off')) res = `${baseName} (Inactive)`;
        else res = baseName;
      } else {
        const words = noExt.replace(/[-_]/g, ' ').split(/\s+/);
        res = words.map(w => w.charAt(0).toUpperCase() + w.slice(1)).join(' ');
      }
    }
    iconNameCache.set(fn, res);
    iconNameCache.set(cleanFn, res);
    return res;
  }

  updateBadgeForSelect(sel, badgeId) {
    const badge = document.getElementById(badgeId);
    if (!badge || !sel) return;
    const val = sel.value;
    const url = getStockIconUrl(val);
    if (url) {
      badge.innerHTML = `<img src="${url}" class="stock-icon-img invert-white" loading="lazy" decoding="async" style="width: 22px; height: 22px;" onerror="this.style.display='none'" />`;
    } else {
      badge.innerHTML = `<span style="font-size: 14px; opacity: 0.5;">🖼</span>`;
    }
  }

  openIconPicker(targetSelectIdOrCallback, previewBadgeId = null, onSelect = null) {
    if (typeof targetSelectIdOrCallback === 'function') {
      this.iconPickerCallback = targetSelectIdOrCallback;
      this.currentIconPickerTarget = null;
      this.currentIconPickerBadge = null;
      this.iconPickerCurrentVal = previewBadgeId || '';
    } else if (typeof onSelect === 'function') {
      this.currentIconPickerTarget = targetSelectIdOrCallback;
      this.currentIconPickerBadge = previewBadgeId;
      this.iconPickerCallback = onSelect;
      this.iconPickerCurrentVal = document.getElementById(targetSelectIdOrCallback)?.value || '';
    } else {
      this.currentIconPickerTarget = targetSelectIdOrCallback;
      this.currentIconPickerBadge = previewBadgeId;
      this.iconPickerCallback = null;
      this.iconPickerCurrentVal = document.getElementById(targetSelectIdOrCallback)?.value || '';
    }
    const modal = document.getElementById('iconPickerModal');
    if (!modal) return;

    const curVal = this.iconPickerCurrentVal || '';
    const subtitle = document.getElementById('iconPickerSubtitle');
    const totalIcons = (this.iconCatalog && this.iconCatalog.length) || (this.availableIcons && this.availableIcons.length) || 0;
    if (subtitle) {
      subtitle.textContent = totalIcons > 0 
        ? `${totalIcons} icons available from Hub & Remote` 
        : 'Choose an icon from remote assets';
    }
    const selNameEl = document.getElementById('iconPickerSelectedName');
    if (selNameEl) selNameEl.textContent = curVal ? `Current: ${this.formatIconDisplayName(curVal)}` : 'No icon selected';
    const sInput = document.getElementById('iconPickerSearch');
    if (sInput) sInput.value = '';
    this.currentIconCategory = 'all';

    document.querySelectorAll('.icon-cat-pill').forEach(p => {
      p.classList.toggle('btn-primary', p.getAttribute('data-cat') === 'all');
      p.classList.toggle('btn-ghost', p.getAttribute('data-cat') !== 'all');
    });

    const grid = document.getElementById('iconPickerGrid');
    if (grid) grid.scrollTop = 0;
    this.renderIconPickerGrid('');
    modal.style.display = 'flex';
  }

  renderIconPickerGrid(searchTerm = '') {
    const grid = document.getElementById('iconPickerGrid');
    if (!grid) return;

    if (!this.iconCatalog || this.iconCatalog.length === 0) {
      const iconList = this.availableIcons && this.availableIcons.length > 0 
        ? this.availableIcons 
        : Array.from(document.getElementById('actEditIcon')?.options || []).map(o => o.value);
      this.iconCatalog = iconList.map(fn => {
        const cleanFn = String(fn).split('/').pop().trim();
        const meta = iconMetadata[cleanFn] || iconMetadata[`${cleanFn}.png`] || {};
        const title = this.formatIconDisplayName(cleanFn);
        return {
          fn: cleanFn,
          title,
          lowerTitle: title.toLowerCase(),
          lowerFn: cleanFn.toLowerCase(),
          category: meta.category || 'other',
          tags: (meta.tags || []).join(' ').toLowerCase(),
          url: getStockIconUrl(cleanFn)
        };
      }).sort((a, b) => a.title.localeCompare(b.title, undefined, { sensitivity: 'base' }));
    }

    const s = (searchTerm || '').toLowerCase().trim();
    const cat = this.currentIconCategory || 'all';

    this.currentFilteredIcons = this.iconCatalog.filter(item => {
      if (cat !== 'all') {
        if (cat === 'activity' && item.category !== 'activity') return false;
        if (cat === 'device' && item.category !== 'device') return false;
        if (cat === 'home' && item.category !== 'home') return false;
        if (cat === 'commands' && item.category !== 'commands') return false;
        if (cat === 'settings' && (item.category !== 'ui' && item.category !== 'other')) return false;
      }
      if (s) {
        return item.lowerTitle.includes(s) || item.lowerFn.includes(s) || item.tags.includes(s);
      }
      return true;
    });

    if (this.currentFilteredIcons.length === 0) {
      grid.innerHTML = `<div class="muted mini" style="grid-column: 1 / -1; text-align: center; padding: 24px;">No matching icons found for "${searchTerm}"</div>`;
      return;
    }

    grid.innerHTML = '';
    this.iconRenderIndex = 0;
    this.appendNextIconBatch();

    // Attach scroll listener once
    if (!this._iconGridScrollBound) {
      this._iconGridScrollBound = true;
      grid.addEventListener('scroll', () => {
        if (grid.scrollTop + grid.clientHeight >= grid.scrollHeight - 160) {
          this.appendNextIconBatch();
        }
      }, { passive: true });
    }

    // Attach delegated click listener once
    if (!this._iconGridClickBound) {
      this._iconGridClickBound = true;
      grid.addEventListener('click', (e) => {
        const card = e.target.closest('.act-icon-card');
        if (!card) return;
        const iconVal = card.getAttribute('data-icon');
        if (!iconVal) return;

        if (typeof this.iconPickerCallback === 'function') {
          this.iconPickerCallback(iconVal);
        } else if (this.currentIconPickerTarget) {
          const targetSel = document.getElementById(this.currentIconPickerTarget);
          if (targetSel) {
            let opt = Array.from(targetSel.options).find(o => o.value === iconVal);
            if (!opt) {
              opt = document.createElement('option');
              opt.value = iconVal;
              opt.textContent = this.formatIconDisplayName(iconVal);
              targetSel.appendChild(opt);
            }
            targetSel.value = iconVal;
            targetSel.dispatchEvent(new Event('change'));
            this.updateBadgeForSelect(targetSel, this.currentIconPickerBadge);
          }
        }
        document.getElementById('iconPickerModal').style.display = 'none';
        showToast(`Selected icon "${this.formatIconDisplayName(iconVal)}"`, 'info', 1800);
      });
    }
  }

  appendNextIconBatch() {
    const grid = document.getElementById('iconPickerGrid');
    if (!grid || !this.currentFilteredIcons || this.iconRenderIndex >= this.currentFilteredIcons.length) return;

    const batch = this.currentFilteredIcons.slice(this.iconRenderIndex, this.iconRenderIndex + 48);
    this.iconRenderIndex += batch.length;

    const curVal = this.iconPickerCurrentVal || document.getElementById(this.currentIconPickerTarget)?.value || '';

    const html = batch.map(item => {
      const isSel = (item.fn === curVal || item.fn.replace(/\.png$/i, '') === curVal);
      return `
        <div class="act-icon-card ${isSel ? 'selected' : ''}" data-icon="${item.fn}">
          <div class="icon-preview">
            <img src="${item.url}" class="stock-icon-img invert-white" loading="lazy" decoding="async" style="width: 26px; height: 26px;" onerror="this.src='/favicon.png'" />
          </div>
          <div class="icon-title" title="${item.title}">${item.title}</div>
          <div class="icon-filename">${item.fn}</div>
        </div>
      `;
    }).join('');

    grid.insertAdjacentHTML('beforeend', html);
  }

  openBtDelayCalibrator(devId) {
    const dev = (this.inventory?.devices || []).find(d => String(d.id) === String(devId));
    if (!dev) return;

    const modal = document.getElementById('btDelayCalibrateModal');
    if (!modal) return;

    document.getElementById('btCalibrateDevId').value = dev.id;
    document.getElementById('btCalibrateDevName').textContent = dev.name || 'Bluetooth Device';
    document.getElementById('btCalibrateDevHost').textContent = `Host BD_ADDR: ${dev.btHost || dev.bt_host || dev.BTAddress || 'Not linked'}`;
    document.getElementById('btCalibrateCurrentStatus').textContent = 'Status: Checking...';
    document.getElementById('btCalibrateTimer').textContent = '0.0s';
    document.getElementById('btCalibrateTimer').classList.remove('bt-timer-active');
    document.getElementById('btCalibrateStateMsg').textContent = 'Ready to start timing test';
    document.getElementById('btCalibrateResultBox').style.display = 'none';

    const cmdSel = document.getElementById('btCalibratePowerCmd');
    if (cmdSel) {
      cmdSel.innerHTML = '';
      const cmds = this.deviceCommandsMap[dev.id] || dev.commands || [];
      const powerCmds = cmds.filter(c => /power|toggle|on|wake|home|menu/i.test(c.name || ''));
      const otherCmds = cmds.filter(c => !powerCmds.includes(c));
      const allCmds = [...powerCmds, ...otherCmds];

      if (allCmds.length === 0) {
        cmdSel.innerHTML = '<option value="">(No commands available - manual power)</option>';
      } else {
        allCmds.forEach(c => {
          const opt = document.createElement('option');
          opt.value = c.name;
          opt.textContent = `${c.name} (${c.protocol || 'IR'})`;
          if (/poweron|powertoggle|power/i.test(c.name) && !cmdSel.value) {
            opt.selected = true;
          }
          cmdSel.appendChild(opt);
        });
      }
    }

    this.client.getBtStatus().then(st => {
      const isConn = this.isDevBtConnected(dev, st);
      const statEl = document.getElementById('btCalibrateCurrentStatus');
      if (statEl) {
        statEl.innerHTML = isConn
          ? '<span style="color:var(--success)">● Currently Connected to Hub</span>'
          : '<span style="color:var(--text-muted)">○ Currently Disconnected</span>';
      }
    }).catch(() => {});

    modal.style.display = 'flex';
  }

  isDevBtConnected(dev, btStatus) {
    if (!btStatus) return false;
    const targetAddr = (dev.btHost || dev.bt_host || dev.BTAddress || '').toLowerCase();
    if (btStatus.hosts && Array.isArray(btStatus.hosts)) {
      const match = btStatus.hosts.find(h => targetAddr && h.addr && h.addr.toLowerCase() === targetAddr);
      if (match && match.connected) return true;
    }
    if (btStatus.host && btStatus.host.connected) {
      if (!targetAddr || (btStatus.host.addr && btStatus.host.addr.toLowerCase() === targetAddr)) {
        return true;
      }
    }
    return false;
  }

  async startBtDelayCalibration(devId) {
    const dev = (this.inventory?.devices || []).find(d => String(d.id) === String(devId));
    if (!dev) return;

    const timerEl = document.getElementById('btCalibrateTimer');
    const msgEl = document.getElementById('btCalibrateStateMsg');
    const startBtn = document.getElementById('btnBtCalibrateStart');
    const resBox = document.getElementById('btCalibrateResultBox');
    const cmdName = document.getElementById('btCalibratePowerCmd')?.value;

    if (this._btCalibrateInterval) clearInterval(this._btCalibrateInterval);
    if (this._btCalibratePoll) clearInterval(this._btCalibratePoll);

    resBox.style.display = 'none';
    timerEl.classList.add('bt-timer-active');
    startBtn.disabled = true;
    startBtn.textContent = 'Detecting...';

    if (cmdName) {
      msgEl.textContent = `Triggering power command "${cmdName}"...`;
      try {
        await this.client.sendDeviceCommand(dev.id, cmdName);
      } catch (e) {
        console.warn('Power command send error:', e);
      }
    }

    msgEl.textContent = 'Waiting for Bluetooth connection to complete...';
    const startTime = Date.now();
    timerEl.textContent = '0.0s';

    this._btCalibrateInterval = setInterval(() => {
      const elapsed = ((Date.now() - startTime) / 1000).toFixed(1);
      timerEl.textContent = `${elapsed}s`;
    }, 100);

    let attempts = 0;
    this._btCalibratePoll = setInterval(async () => {
      attempts++;
      if (attempts > 300) {
        clearInterval(this._btCalibrateInterval);
        clearInterval(this._btCalibratePoll);
        timerEl.classList.remove('bt-timer-active');
        startBtn.disabled = false;
        startBtn.textContent = 'Start Detection Test';
        msgEl.innerHTML = '<span style="color:var(--danger)">Connection timed out (60s). Check if device is powered on.</span>';
        showToast('Bluetooth detection timed out.', 'warning');
        return;
      }

      try {
        const st = await this.client.getBtStatus();
        if (this.isDevBtConnected(dev, st)) {
          const elapsedMs = Date.now() - startTime;
          const elapsedSec = (elapsedMs / 1000).toFixed(1);
          clearInterval(this._btCalibrateInterval);
          clearInterval(this._btCalibratePoll);
          timerEl.classList.remove('bt-timer-active');
          timerEl.textContent = `${elapsedSec}s`;
          startBtn.disabled = false;
          startBtn.textContent = 'Start Detection Test';

          msgEl.innerHTML = `<span style="color:var(--success)">✓ Connected in ${elapsedSec}s!</span>`;

          const suggested = Math.max(1, Math.ceil(parseFloat(elapsedSec)) + 1);

          document.getElementById('btCalibrateResultMsg').textContent = `Connected in ${elapsedSec} seconds!`;
          document.getElementById('btCalibrateResultDesc').textContent = `Suggested startup delay: ${suggested} seconds (includes 1s safety margin for HID handshake).`;
          resBox.style.display = 'block';

          this._lastSuggestedBtDelay = suggested;
          this._lastCalibratedDevId = dev.id;

          showToast(`Bluetooth connected in ${elapsedSec}s! Suggested delay: ${suggested}s.`, 'success');
        }
      } catch (err) {}
    }, 200);
  }

  async applyCalibratedBtDelay() {
    if (!this._lastCalibratedDevId || typeof this._lastSuggestedBtDelay !== 'number') return;
    const devId = this._lastCalibratedDevId;
    const delay = this._lastSuggestedBtDelay;

    const dlyInp = document.getElementById('curDevBtDelay');
    if (dlyInp) dlyInp.value = delay;

    try {
      await this.client.saveDeviceDetails({
        deviceId: devId,
        bt_reconnect_delay_sec: delay
      });
      showToast(`Saved ${delay}s Bluetooth delay override for this device!`, 'success');
      document.getElementById('btDelayCalibrateModal').style.display = 'none';
      await this.refreshData();
    } catch (e) {
      showToast(`Failed to save delay: ${e.message}`, 'error');
    }
  }

  recordSuccess() {
    this.consecutiveFailures = 0;
    if (!this.isHubOnline) {
      this.setHubStatus(true);
    }
  }

  recordFailure(err) {
    this.consecutiveFailures++;
    // Only drop offline when 3 consecutive checks fail
    if (this.consecutiveFailures >= 3 && this.isHubOnline) {
      this.setHubStatus(false);
      const desc = document.getElementById('offlineBannerDesc');
      if (desc && this.activeHub) {
        desc.textContent = `Hub unreachable (${this.activeHub.host}): ${err?.message || 'Connection lost'}`;
      }
    }
  }

  setHubStatus(online) {
    const wasOnline = this.isHubOnline;
    this.isHubOnline = Boolean(online);
    if (online) {
      this.consecutiveFailures = 0;
      if (this.activitiesData) this.activitiesData.isOffline = false;
    } else {
      if (this.activitiesData) this.activitiesData.isOffline = true;
    }
    const dot = document.getElementById('activeHubStatusDot');
    const hubBtn = document.getElementById('hubSelectorBtn');
    const banner = document.getElementById('hubOfflineBanner');
    const desc = document.getElementById('offlineBannerDesc');
    const content = document.querySelector('.app-content');

    if (dot) {
      dot.className = online ? 'hub-status-dot online' : 'hub-status-dot offline';
      dot.title = online ? 'Hub is online' : 'Hub is offline';
    }
    const name = this.activeHub ? this.activeHub.name : 'Hub';
    const statusText = `${name}: ${online ? 'Online' : 'Offline'}`;
    if (hubBtn) {
      hubBtn.title = this.isNative ? `${statusText} (Tap to manage hubs)` : statusText;
    }
    const pill = document.getElementById('hubStatusPill');
    if (pill) {
      pill.title = this.isNative ? `${statusText} (Tap to manage hubs)` : statusText;
    }
    if (banner) {
      banner.style.display = online ? 'none' : 'flex';
      if (!online && this.activeHub && desc) {
        desc.textContent = `Cannot reach hub at ${this.activeHub.host || 'specified address'}. Reconnecting automatically...`;
      }
    }
    if (content) {
      content.classList.toggle('hub-offline-disabled', !online);
    }

    if (!online) {
      this.ensureAutoReconnect();
    } else {
      this.stopAutoReconnect();
    }

    if (wasOnline !== this.isHubOnline) {
      this.updateActivityBanner();
      if (this.currentTab === 'activity-remote') this.renderActivityRemote();
      else if (this.currentTab === 'overview') this.loadDashboard();
      else if (this.currentTab === 'activities') this.loadActivitiesView();
    }
  }

  startHealthMonitoring() {
    if (this._healthTimer) clearInterval(this._healthTimer);
    this._healthTimer = setInterval(() => {
      this.pollHubHealth(false);
    }, 15000);
  }

  async pollHubHealth(force = false) {
    if (this.isScanningNetwork || this.isReconnecting) return;
    if (!this.activeHub || !this.activeHub.host) {
      if (this.isHubOnline) this.setHubStatus(false);
      return;
    }

    // 1. If WebSocket is connected, WebSocket is active and pushes events in real-time
    if (this.client.isWsConnected()) {
      this.recordSuccess();
      if (!this.isHubOnline) this.setHubStatus(true);
      return;
    }

    // 2. Fallback to HTTP /health only when WS is disconnected
    try {
      const health = await this.client.getHealth();
      if (health && (health.status === 'ok' || health.service === 'codex_daemon')) {
        const wasOffline = !this.isHubOnline;
        this.recordSuccess();

        if (wasOffline) {
          this.setHubStatus(true);
          if (!this.inventory?.devices?.length) {
            await this.refreshData();
          }
        }

        this.client.connectWebSocket();
      } else {
        throw new Error('Health check returned non-ok');
      }
    } catch (err) {
      this.recordFailure(err);
      if (force || this.consecutiveFailures >= 2) {
        if (this.isHubOnline) {
          this.setHubStatus(false);
        }
      }
    }
  }

  ensureAutoReconnect() {
    if (this.autoReconnectTimer) return;
    this.autoReconnectTimer = setInterval(() => {
      if (!this.isHubOnline && this.activeHub) {
        this.recheckConnection(false);
      }
    }, 3000);
  }

  stopAutoReconnect() {
    if (this.autoReconnectTimer) {
      clearInterval(this.autoReconnectTimer);
      this.autoReconnectTimer = null;
    }
  }

  async recheckConnection(manual = false) {
    if (this.isReconnecting) return;
    this.isReconnecting = true;

    const btn = document.getElementById('btnRetryConn');
    const label = document.getElementById('btnRetryConnLabel');
    const spinner = document.getElementById('retrySpinner');

    if (manual) {
      if (btn) btn.disabled = true;
      if (spinner) spinner.style.display = 'inline-block';
      if (label) label.textContent = 'Connecting...';
    }

    try {
      if (this.activeHub) {
        const testRes = await this.client.testConnection();
        if (testRes.ok) {
          this.recordSuccess();
          if (!this.client.isWsConnected()) {
            this.client.connectWebSocket();
          }
          await this.refreshData();
          if (manual && label) label.textContent = 'Connected!';
        } else {
          throw new Error(testRes.error || 'Connection timed out');
        }
      }
    } catch (err) {
      if (manual) {
        const desc = document.getElementById('offlineBannerDesc');
        if (desc && this.activeHub) {
          desc.textContent = `Hub unreachable (${this.activeHub.host}): ${err.message || 'Offline'}`;
        }
      }
      this.recordFailure(err);
    } finally {
      this.isReconnecting = false;
      if (manual) {
        setTimeout(() => {
          if (btn) btn.disabled = false;
          if (spinner) spinner.style.display = 'none';
          if (label) label.textContent = 'Retry Connection';
        }, 1000);
      }
    }
  }

  setupWebSocket() {
    this.client.onWsMessage = (data) => {
      if (!data) return;
      if (data.type === 'activity_progress') {
        const isFinished = (data.state === 2 || data.state === 0);
        this.activitiesData.isTransitioning = !isFinished;
        this.activitiesData.transitionTarget = String(data.activity || '');
        this.activitiesData.transitionStep = data.step || 0;
        this.activitiesData.transitionTotal = data.total || 0;
        this.activitiesData.transitionDesc = data.desc || '';
        this.wsProgress = isFinished ? null : data;

        if (isFinished) {
          if (this.actPollTimer) {
            clearTimeout(this.actPollTimer);
            this.actPollTimer = null;
          }
          const actId = (data.state === 0 || String(data.activity) === '-1') ? '-1' : String(data.activity);
          this.activitiesData.currentId = actId;
          const curAct = (this.activitiesData.activities || []).find(a => String(a.id) === actId);
          this.activitiesData.currentName = curAct ? (curAct.name || curAct.label) : (actId === '-1' ? 'PowerOff' : actId);
          this.refreshActivityState().then(() => {
            this.updateActivityBanner();
            if (this.currentTab === 'activity-remote') this.renderActivityRemote();
            if (this.currentTab === 'overview') this.loadDashboard();
            if (this.currentTab === 'activities') this.loadActivitiesView();
          });
        } else {
          this.updateActivityBanner();
        }
      } else if (data.type === 'activity_state') {
        if (this.actPollTimer) {
          clearTimeout(this.actPollTimer);
          this.actPollTimer = null;
        }
        const actId = (data.state === 0 || String(data.activity) === '-1') ? '-1' : String(data.activity);
        const changed = String(this.activitiesData.currentId) !== actId;
        this.activitiesData.currentId = actId;
        this.activitiesData.isTransitioning = (data.state === 1 || data.state === 3);
        const curAct = (this.activitiesData.activities || []).find(a => String(a.id) === actId);
        this.activitiesData.currentName = curAct ? (curAct.name || curAct.label) : (actId === '-1' ? 'PowerOff' : actId);
        this.wsProgress = null;
        this.updateActivityBanner();
        if (changed || this.currentTab === 'activity-remote') {
          this.renderActivityRemote();
        }
        if (changed && this.currentTab === 'overview') this.loadDashboard();
        if (changed && this.currentTab === 'activities') this.loadActivitiesView();
        this.refreshActivityState();
      }
    };
    this.client.onWsStatus = (online) => {
      if (online) {
        this.recordSuccess();
        this.setHubStatus(true);
        if (!this.inventory?.devices?.length) {
          this.refreshData();
        }
      } else {
        this.pollHubHealth(true);
      }
    };
  }

  async init() {
    setLanguage(getInitialLanguage());
    this.startHealthMonitoring();
    if (typeof document !== 'undefined') {
      document.addEventListener('visibilitychange', () => {
        if (!document.hidden && this.activeHub && this.activeHub.host) {
          this.pollHubHealth(true);
        }
      });
    }
    if (typeof window !== 'undefined') {
      window.addEventListener('focus', () => {
        if (this.activeHub && this.activeHub.host) {
          this.pollHubHealth(true);
        }
      });
    }
    if (this.isNative) {
      document.body.classList.add('is-native');
      const pill = document.getElementById('hubStatusPill');
      if (pill) {
        pill.setAttribute('role', 'button');
        pill.setAttribute('tabindex', '0');
        pill.title = 'Manage Hubs';
      }
    } else {
      document.body.classList.add('is-webui');
      const pill = document.getElementById('hubStatusPill');
      if (pill) {
        pill.removeAttribute('role');
        pill.removeAttribute('tabindex');
        pill.title = 'Hub Status';
      }
    }
    this.bindEvents();
    if (typeof window !== 'undefined') {
      window.addEventListener('popstate', (e) => {
        const tab = e.state?.tab || window.location.hash.replace('#', '') || 'activity-remote';
        this.switchTab(tab, false);
      });
      window.addEventListener('harmony-lang-changed', () => {
        this.updateActivityBanner();
        if (this.currentTab === 'overview') this.loadDashboard();
      });
    }
    await this.loadActiveHub();
  }

  bindEvents() {
    // Language Switchers
    const onLangChange = (e) => {
      setLanguage(e.target.value);
    };
    document.getElementById('langSelect')?.addEventListener('change', onLangChange);
    document.getElementById('drawerLangSelect')?.addEventListener('change', onLangChange);

    // Drawer Navigation
    const btnOpenDrawer = document.getElementById('btnOpenDrawer');
    const btnCloseDrawer = document.getElementById('btnCloseDrawer');
    const drawerBackdrop = document.getElementById('drawerBackdrop');
    const appDrawer = document.getElementById('appDrawer');

    const toggleDrawer = (open) => {
      if (appDrawer) appDrawer.classList.toggle('active', open);
      if (drawerBackdrop) drawerBackdrop.classList.toggle('active', open);
    };

    if (btnOpenDrawer) btnOpenDrawer.addEventListener('click', () => toggleDrawer(true));
    if (btnCloseDrawer) btnCloseDrawer.addEventListener('click', () => toggleDrawer(false));
    if (drawerBackdrop) drawerBackdrop.addEventListener('click', () => toggleDrawer(false));

    // Offline Banner Retry Button
    document.getElementById('btnRetryConn')?.addEventListener('click', () => this.recheckConnection(true));

    // Visibility / App Resume listener (Android WebView & browser background recovery)
    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'visible' && this.activeHub) {
        if (!this.isHubOnline || !this.client.isWsConnected()) {
          this.recheckConnection(false);
        }
      }
    });

    // Top Navigation Tabs
    document.querySelectorAll('.nav-tab-btn').forEach(btn => {
      btn.addEventListener('click', () => {
        const tab = btn.getAttribute('data-tab');
        if (tab) this.switchTab(tab);
      });
    });

    // Bottom Navigation Items
    document.querySelectorAll('.app-nav .nav-item').forEach(btn => {
      btn.addEventListener('click', () => {
        if (btn.id === 'navItemMore') {
          toggleDrawer(true);
          return;
        }
        const tab = btn.getAttribute('data-tab');
        if (tab) this.switchTab(tab);
      });
    });

    // Drawer Menu Items
    document.querySelectorAll('.drawer-item').forEach(btn => {
      btn.addEventListener('click', () => {
        const target = btn.getAttribute('data-view-target');
        if (target) {
          this.switchTab(target);
          toggleDrawer(false);
        }
      });
    });

    // Settings Navigation Cards & data-view-target shortcuts
    document.querySelectorAll('[data-view-target], .settings-nav-card').forEach(btn => {
      btn.addEventListener('click', (e) => {
        if (e.target.tagName === 'BUTTON' && !btn.classList.contains('btn') && !btn.classList.contains('nav-tab-btn')) return;
        const target = btn.getAttribute('data-view-target');
        if (target) {
          this.switchTab(target);
          toggleDrawer(false);
        }
      });
    });

    // Back to Settings button in sub-views
    document.querySelectorAll('.btn-back-settings').forEach(btn => {
      btn.addEventListener('click', () => this.switchTab('overview'));
    });

    // Segmented Control Navigations (Mobile UX optimization)
    // 1. Activity Editor Tabs
    const actNav = document.getElementById('actEditTabsNav');
    if (actNav) {
      actNav.querySelectorAll('.segmented-btn').forEach(btn => {
        btn.addEventListener('click', () => {
          const tab = btn.getAttribute('data-act-tab');
          actNav.querySelectorAll('.segmented-btn').forEach(b => b.classList.toggle('active', b === btn));
          const pProfile = document.getElementById('actTabPanelProfile');
          const pStart = document.getElementById('actTabPanelStart');
          const pStop = document.getElementById('actTabPanelStop');
          if (pProfile) pProfile.style.display = (tab === 'profile') ? 'block' : 'none';
          if (pStart) pStart.style.display = (tab === 'start') ? 'block' : 'none';
          if (pStop) pStop.style.display = (tab === 'stop') ? 'block' : 'none';
        });
      });
    }

    // 2. IR Studio Mobile Tabs
    const irNav = document.getElementById('irStudioMobileNav');
    if (irNav) {
      irNav.querySelectorAll('.segmented-btn').forEach(btn => {
        btn.addEventListener('click', () => {
          const tab = btn.getAttribute('data-ir-tab');
          irNav.querySelectorAll('.segmented-btn').forEach(b => b.classList.toggle('active', b === btn));
          const colLeft = document.getElementById('irStudioColLeft');
          const colRight = document.getElementById('irStudioColRight');
          const devProp = document.getElementById('devicePropertiesBox');
          const cmdsWrap = document.getElementById('curDevCmdsWrap');
          if (window.innerWidth <= 768) {
            if (tab === 'cmds') {
              if (colLeft) colLeft.style.display = 'flex';
              if (colRight) colRight.style.display = 'none';
              if (devProp) devProp.style.display = 'none';
              if (cmdsWrap) cmdsWrap.style.display = 'block';
            } else if (tab === 'add') {
              if (colLeft) colLeft.style.display = 'none';
              if (colRight) colRight.style.display = 'flex';
            } else if (tab === 'settings') {
              if (colLeft) colLeft.style.display = 'flex';
              if (colRight) colRight.style.display = 'none';
              if (devProp) devProp.style.display = 'block';
              if (cmdsWrap) cmdsWrap.style.display = 'none';
            }
          }
        });
      });
    }

    // 3. Harmony Elite Button Mapping Modal Listeners
    document.getElementById('eliteCancelBtn')?.addEventListener('click', () => {
      const m = document.getElementById('eliteBtnModal');
      if (m) m.style.display = 'none';
    });
    document.getElementById('eliteModalCloseBtn')?.addEventListener('click', () => {
      const m = document.getElementById('eliteBtnModal');
      if (m) m.style.display = 'none';
    });
    document.getElementById('eliteBtnModal')?.addEventListener('click', (e) => {
      if (e.target.id === 'eliteBtnModal') e.target.style.display = 'none';
    });
    document.getElementById('eliteScreenFrame')?.addEventListener('click', () => {
      const sec = document.getElementById('eliteSectionScreen');
      if (sec) sec.scrollIntoView({ behavior: 'smooth' });
    });

    // 4. BT Remote B25 Button Mapping Modal Listeners
    document.getElementById('b25CancelBtn')?.addEventListener('click', () => {
      const m = document.getElementById('b25BtnModal');
      if (m) m.style.display = 'none';
    });
    document.getElementById('b25ModalCloseBtn')?.addEventListener('click', () => {
      const m = document.getElementById('b25BtnModal');
      if (m) m.style.display = 'none';
    });
    document.getElementById('b25BtnModal')?.addEventListener('click', (e) => {
      if (e.target.id === 'b25BtnModal') e.target.style.display = 'none';
    });

    // Responsive window resize listener to restore desktop multi-column visibility
    window.addEventListener('resize', () => {
      if (window.innerWidth > 768) {
        ['irStudioColLeft', 'irStudioColRight', 'devicePropertiesBox', 'curDevCmdsWrap',
         'eliteColVisual', 'eliteColConfig', 'eliteSectionScreen', 'eliteSectionPresets',
         'b25ColVisual', 'b25ColPresets'].forEach(id => {
          const el = document.getElementById(id);
          if (el) el.style.display = '';
        });
      }
    });

    // Hub Switcher Button & Pill in Header (Native Android App Only)
    if (this.isNative) {
      const hubBtn = document.getElementById('hubSelectorBtn');
      if (hubBtn) {
        hubBtn.addEventListener('click', () => this.switchTab('hubs'));
      }
      const hubPill = document.getElementById('hubStatusPill') || document.querySelector('.hub-status-pill');
      if (hubPill) {
        hubPill.addEventListener('click', () => this.switchTab('hubs'));
      }
      const hubLabel = document.getElementById('activeHubLabel');
      if (hubLabel) {
        hubLabel.addEventListener('click', () => this.switchTab('hubs'));
      }
    }

    // Power Off in Banner
    const pwrBtn = document.getElementById('btnBannerPowerOff');
    if (pwrBtn) {
      pwrBtn.addEventListener('click', async () => {
        if (await showConfirm('Power off all devices in current activity?', { title: 'Power Off Activity', okText: 'Power Off' })) {
          await this.stopActivity();
        }
      });
    }

    // Add Hub Button in Hubs View
    const addHubBtn = document.getElementById('btnAddHub');
    if (addHubBtn) {
      addHubBtn.addEventListener('click', () => this.openAddHubModal());
    }

    // Stepper navigation in IR Setup
    document.querySelectorAll('.step-btn').forEach(btn => {
      btn.addEventListener('click', () => {
        const target = btn.getAttribute('data-step-target');
        document.querySelectorAll('.step-btn').forEach(b => {
          b.classList.toggle('btn-primary', b === btn);
          b.classList.toggle('btn-ghost', b !== btn);
        });
        document.querySelectorAll('.wizard-panel').forEach(p => {
          p.style.display = p.getAttribute('data-panel') === target ? 'block' : 'none';
        });
        if (target === 'manage') this.populateManageDevices();
        if (target === 'verify') this.populateVerifyDropdowns();
      });
    });

    // Activities Actions
    document.getElementById('actRefreshBtn')?.addEventListener('click', () => this.loadActivitiesView());
    document.getElementById('actNewBtn')?.addEventListener('click', () => this.openActivityEditor(null));
    document.getElementById('actEditorCloseBtn')?.addEventListener('click', () => {
      const box = document.getElementById('actEditorBox');
      if (box) box.style.display = 'none';
    });
    document.getElementById('actCancelBtn')?.addEventListener('click', () => {
      const box = document.getElementById('actEditorBox');
      if (box) box.style.display = 'none';
    });
    document.getElementById('actSaveBtn')?.addEventListener('click', () => this.saveCurrentActivity());
    document.getElementById('actDeleteBtn')?.addEventListener('click', () => this.deleteCurrentActivity());

    document.getElementById('actAddStartCmdBtn')?.addEventListener('click', () => this.addSequenceStep('actStartStepsList', 'Command'));
    document.getElementById('actAddStartDelayBtn')?.addEventListener('click', () => this.addSequenceStep('actStartStepsList', 'Delay'));
    document.getElementById('actAddStartSwitchedBtn')?.addEventListener('click', () => this.addSequenceStep('actStartStepsList', 'Switched'));
    document.getElementById('actAddStopCmdBtn')?.addEventListener('click', () => this.addSequenceStep('actStopStepsList', 'Command'));
    document.getElementById('actAddStopDelayBtn')?.addEventListener('click', () => this.addSequenceStep('actStopStepsList', 'Delay'));
    document.getElementById('actAddStopSwitchedBtn')?.addEventListener('click', () => this.addSequenceStep('actStopStepsList', 'Switched'));

    document.getElementById('actTestStartSeqBtn')?.addEventListener('click', () => this.testLiveSequence('actStartStepsList'));
    document.getElementById('actTestStopSeqBtn')?.addEventListener('click', () => this.testLiveSequence('actStopStepsList'));
    document.getElementById('previewSimulateBtn')?.addEventListener('click', () => this.simulateActivityTransition());
    document.getElementById('previewCloseBtn')?.addEventListener('click', () => {
      const box = document.getElementById('previewResultsBox');
      if (box) box.style.display = 'none';
    });

    // IR Studio Actions
    document.getElementById('btnStudioAddDevice')?.addEventListener('click', () => {
      const box = document.getElementById('studioNewDeviceBox');
      if (box) box.style.display = (box.style.display === 'none') ? 'block' : 'none';
    });
    document.getElementById('btnStudioCancelNewDevice')?.addEventListener('click', () => {
      const box = document.getElementById('studioNewDeviceBox');
      if (box) box.style.display = 'none';
    });
    document.querySelectorAll('.ir-add-tab').forEach(tab => {
      tab.addEventListener('click', () => {
        const mode = tab.getAttribute('data-add-mode');
        document.querySelectorAll('.ir-add-tab').forEach(t => {
          t.classList.toggle('btn-primary', t === tab);
          t.classList.toggle('btn-ghost', t !== tab);
        });
        const pLearn = document.getElementById('addModeLearn');
        const pDb = document.getElementById('addModeDb');
        const pPaste = document.getElementById('addModePaste');
        if (pLearn) pLearn.style.display = (mode === 'learn') ? 'block' : 'none';
        if (pDb) pDb.style.display = (mode === 'database') ? 'block' : 'none';
        if (pPaste) pPaste.style.display = (mode === 'paste') ? 'block' : 'none';
      });
    });
    document.getElementById('manageDeviceSelect')?.addEventListener('change', (e) => {
      this.selectedIrDeviceId = e.target.value;
      this.updateStudioDeviceView();
    });
    document.getElementById('curDevCmdFilter')?.addEventListener('input', () => {
      const dev = (this.inventory.devices || []).find(d => String(d.id) === String(document.getElementById('manageDeviceSelect')?.value));
      if (dev) this.renderStudioCommandsList(dev);
    });
    document.getElementById('curDevPortSelect')?.addEventListener('change', async (e) => {
      const devId = this.selectedIrDeviceId || document.getElementById('manageDeviceSelect')?.value;
      const portVal = parseInt(e.target.value, 10);
      this.selectedIrDeviceId = String(devId);
      await this.client.saveDeviceDetails({ deviceId: devId, controlPort: isNaN(portVal) ? 7 : portVal });
      await this.refreshData();
    });
    document.getElementById('btnEditCurrentDevice')?.addEventListener('click', () => {
      const box = document.getElementById('devEditConfigBox');
      if (!box) return;
      const isVisible = box.style.display !== 'none';
      box.style.display = isVisible ? 'none' : 'block';
      if (!isVisible) {
        const sel = document.getElementById('manageDeviceSelect');
        const curId = this.selectedIrDeviceId || sel?.value;
        const dev = (this.inventory.devices || []).find(d => String(d.id) === String(curId));
        if (dev) {
          const nameInp = document.getElementById('editDevName');
          const typeInp = document.getElementById('editDevType');
          const mfgInp = document.getElementById('editDevMfg');
          const mdlInp = document.getElementById('editDevModel');
          const iconInp = document.getElementById('editDevIcon');
          if (nameInp) nameInp.value = dev.name || '';
          if (typeInp) typeInp.value = dev.type || '';
          if (mfgInp) mfgInp.value = dev.manufacturer || '';
          if (mdlInp) mdlInp.value = dev.model || '';
          if (iconInp) {
            const rawIcn = (dev.icon || '').toLowerCase();
            const iconMap = {
              '1': 'tv', 'television': 'tv', 'tv': 'tv',
              '5': 'receiver', 'amplifier': 'receiver', 'avr': 'receiver', 'receiver': 'receiver',
              '10': 'pc', 'computer': 'pc', 'laptop': 'pc',
              '16': 'cable', 'satellite': 'cable', 'stb': 'cable',
              '33': 'streamer', 'media': 'streamer',
              '63': 'light', 'lights': 'light', 'bulb': 'light',
              'soundbar': 'soundbar', 'speaker': 'soundbar',
              'playstation': 'ps', 'ps': 'ps', 'ps4': 'ps', 'ps5': 'ps',
              'xbox': 'xbox', 'switch': 'switch', 'nintendo': 'switch',
              'shield': 'shield', 'apple': 'apple', 'firetv': 'firetv', 'roku': 'roku',
              'bluray': 'bluray', 'dvd': 'bluray', 'projector': 'projector',
              'sonos': 'sonos', 'heos': 'heos', 'plug': 'plug', 'blinds': 'blinds',
              'thermostat': 'thermostat', 'fan': 'fan', 'generic': 'generic'
            };
            iconInp.value = iconMap[rawIcn] || rawIcn || 'tv';
            if (!iconInp.value) iconInp.value = 'generic';
            this.updateBadgeForSelect(iconInp, 'devIconPreviewBadge');
          }
        }
      }
    });
    document.getElementById('btnSaveDevDetails')?.addEventListener('click', async () => {
      const sel = document.getElementById('manageDeviceSelect');
      const devId = this.selectedIrDeviceId || sel?.value;
      if (!devId) return;
      this.selectedIrDeviceId = String(devId);
      const name = document.getElementById('editDevName')?.value.trim();
      const type = document.getElementById('editDevType')?.value.trim();
      const mfg = document.getElementById('editDevMfg')?.value.trim();
      const model = document.getElementById('editDevModel')?.value.trim();
      const icon = document.getElementById('editDevIcon')?.value.trim();
      const idd = parseInt(document.getElementById('devInterDeviceDelay')?.value, 10);
      const ikd = parseInt(document.getElementById('devInterKeyDelay')?.value, 10);
      const hikd = parseInt(document.getElementById('devHoldInterKeyDelay')?.value, 10);
      const pod = parseInt(document.getElementById('devPowerOnDelay')?.value, 10);
      const pao = document.getElementById('devIsPowerAlwaysOn')?.checked ? 1 : 0;
      const btHost = document.getElementById('curDevBtHost')?.value.trim();
      const btAutoRecVal = parseInt(document.getElementById('curDevBtAutoReconnect')?.value, 10);
      const btAutoRec = isNaN(btAutoRecVal) ? -1 : btAutoRecVal;
      const btDur = parseInt(document.getElementById('curDevBtDuration')?.value, 10);
      const btDly = parseInt(document.getElementById('curDevBtDelay')?.value, 10);
      const btTrigVal = document.getElementById('curDevBtTriggerCmd')?.value || '';
      let btTrigDevId = '';
      let btTrigCmdName = '';
      if (btTrigVal && btTrigVal.includes(':')) {
        const parts = btTrigVal.split(':');
        btTrigDevId = parts[0];
        btTrigCmdName = parts.slice(1).join(':');
      }

      const stat = document.getElementById('devEditStatus');
      if (stat) stat.textContent = 'Saving...';
      try {
        await this.client.saveDeviceDetails({
          deviceId: devId,
          name: name || undefined,
          type: type || undefined,
          manufacturer: mfg || undefined,
          model: model || undefined,
          icon: icon || undefined,
          inter_device_delay: isNaN(idd) ? 100 : idd,
          inter_key_delay: isNaN(ikd) ? 100 : ikd,
          hold_inter_key_delay: isNaN(hikd) ? 200 : hikd,
          power_on_delay: isNaN(pod) ? 1500 : pod,
          is_power_always_on: pao,
          isPowerAlwaysOn: pao,
          bt_host: btHost || undefined,
          bt_auto_reconnect: btAutoRec,
          btAutoReconnect: btAutoRec,
          bt_reconnect_duration_sec: isNaN(btDur) ? 0 : btDur,
          btReconnectDurationSec: isNaN(btDur) ? 0 : btDur,
          bt_reconnect_delay_sec: isNaN(btDly) ? -1 : btDly,
          btReconnectDelaySec: isNaN(btDly) ? -1 : btDly,
          bt_trigger_device_id: btTrigDevId,
          bt_trigger_command_name: btTrigCmdName,
          btTriggerDeviceId: btTrigDevId,
          btTriggerCommandName: btTrigCmdName
        });
        if (stat) stat.innerHTML = '<span style="color:var(--success)">Saved!</span>';
        await this.refreshData();
        const box = document.getElementById('devEditConfigBox');
        if (box) setTimeout(() => { box.style.display = 'none'; }, 1000);
      } catch (err) {
        if (stat) stat.innerHTML = '<span style="color:var(--danger)">' + err.message + '</span>';
      }
    });

    // Bluetooth Host Pairing Controls on Device Setup
    document.getElementById('btnCurDevBtStartPair')?.addEventListener('click', async () => {
      const devId = this.selectedIrDeviceId;
      if (!devId) return;
      const helpBox = document.getElementById('curDevBtPairHelp');
      const statusMsg = document.getElementById('curDevBtPairStatusMsg');
      const startBtn = document.getElementById('btnCurDevBtStartPair');
      if (helpBox) helpBox.style.display = 'block';
      if (startBtn) startBtn.disabled = true;
      if (statusMsg) statusMsg.textContent = 'Hub is in pairing mode. Waiting for device to pair...';

      try {
        await this.client.setBtPairing(true, 'Harmony Keyboard', devId);
        // Start polling for pairing completion
        if (this._btPairPollTimer) clearInterval(this._btPairPollTimer);
        let pollAttempts = 0;
        this._btPairPollTimer = setInterval(async () => {
          pollAttempts++;
          if (pollAttempts > 35) {
            clearInterval(this._btPairPollTimer);
            if (helpBox) helpBox.style.display = 'none';
            if (startBtn) startBtn.disabled = false;
            return;
          }
          try {
            await this.refreshData();
            const curDev = (this.inventory.devices || []).find(d => String(d.id) === String(devId));
            const pairedAddr = curDev ? (curDev.btHost || curDev.bt_host || curDev.BTAddress) : '';
            if (pairedAddr) {
              clearInterval(this._btPairPollTimer);
              if (statusMsg) statusMsg.innerHTML = `<span style="color:var(--success)">✓ Successfully paired to ${pairedAddr}!</span>`;
              setTimeout(() => {
                if (helpBox) helpBox.style.display = 'none';
                if (startBtn) startBtn.disabled = false;
                this.updateStudioDeviceView();
              }, 1500);
            }
          } catch {}
        }, 2000);
      } catch (err) {
        alert('Could not start Bluetooth pairing: ' + err.message);
        if (helpBox) helpBox.style.display = 'none';
        if (startBtn) startBtn.disabled = false;
      }
    });

    document.getElementById('btnCurDevBtCancelPair')?.addEventListener('click', async () => {
      if (this._btPairPollTimer) clearInterval(this._btPairPollTimer);
      const helpBox = document.getElementById('curDevBtPairHelp');
      const startBtn = document.getElementById('btnCurDevBtStartPair');
      if (helpBox) helpBox.style.display = 'none';
      if (startBtn) startBtn.disabled = false;
      try {
        await this.client.setBtPairing(false);
      } catch {}
    });

    document.getElementById('btnCurDevBtConnect')?.addEventListener('click', async () => {
      const dev = (this.inventory.devices || []).find(d => String(d.id) === String(this.selectedIrDeviceId));
      const addr = dev ? (dev.btHost || dev.bt_host || dev.BTAddress) : '';
      if (!addr) return alert('No Bluetooth host address linked.');
      try {
        await this.client.connectBt(addr);
        alert('Connection command sent to host ' + addr);
        setTimeout(() => this.updateStudioDeviceView(), 1000);
      } catch (err) {
        alert('Connect failed: ' + err.message);
      }
    });

    document.getElementById('btnCurDevBtDisconnect')?.addEventListener('click', async () => {
      const dev = (this.inventory.devices || []).find(d => String(d.id) === String(this.selectedIrDeviceId));
      const addr = dev ? (dev.btHost || dev.bt_host || dev.BTAddress) : '';
      try {
        await this.client.disconnectBt(addr);
        alert('Disconnect command sent.');
        setTimeout(() => this.updateStudioDeviceView(), 1000);
      } catch (err) {
        alert('Disconnect failed: ' + err.message);
      }
    });

    document.getElementById('btnCurDevBtUnpair')?.addEventListener('click', async () => {
      const devId = this.selectedIrDeviceId;
      if (!devId) return;
      if (!await showConfirm('Unpair and unlink Bluetooth host from this device?', { title: 'Unpair Bluetooth Host', okText: 'Unpair' })) return;
      try {
        await this.client.linkBtDevice('', devId);
        await this.refreshData();
        this.updateStudioDeviceView();
      } catch (err) {
        alert('Unpair failed: ' + err.message);
      }
    });

    document.getElementById('btnCurDevBtLinkKnown')?.addEventListener('click', async () => {
      const devId = this.selectedIrDeviceId;
      const addr = document.getElementById('curDevBtKnownHostSelect')?.value;
      if (!devId || !addr) return alert('Please select a known Bluetooth host to link.');
      try {
        await this.client.linkBtDevice(addr, devId);
        await this.refreshData();
        this.updateStudioDeviceView();
        alert('Device linked to ' + addr);
      } catch (err) {
        alert('Link failed: ' + err.message);
      }
    });

    document.getElementById('btnOpenPowerSeqModal')?.addEventListener('click', () => {
      this.openPowerSequenceModal();
    });
    document.getElementById('btnPowerSeqClose')?.addEventListener('click', () => {
      const m = document.getElementById('powerSeqModal');
      if (m) m.style.display = 'none';
    });
    document.getElementById('btnPowerSeqCancel')?.addEventListener('click', () => {
      const m = document.getElementById('powerSeqModal');
      if (m) m.style.display = 'none';
    });
    document.getElementById('btnPowerSeqSave')?.addEventListener('click', async () => {
      await this.saveDevicePowerSequences();
    });
    document.getElementById('btnAddPowerOnCmd')?.addEventListener('click', () => {
      this.addSequenceStep('powerOnStepsList', 'Command');
    });
    document.getElementById('btnAddPowerOnDelay')?.addEventListener('click', () => {
      this.addSequenceStep('powerOnStepsList', 'Delay');
    });
    document.getElementById('btnAddPowerOffCmd')?.addEventListener('click', () => {
      this.addSequenceStep('powerOffStepsList', 'Command');
    });
    document.getElementById('btnAddPowerOffDelay')?.addEventListener('click', () => {
      this.addSequenceStep('powerOffStepsList', 'Delay');
    });
    document.getElementById('btnDevMqttConfig')?.addEventListener('click', () => {
      const box = document.getElementById('devMqttConfigBox');
      if (box) box.style.display = box.style.display === 'none' ? 'block' : 'none';
    });
    document.getElementById('devMqttEnabled')?.addEventListener('change', (e) => {
      const badge = document.getElementById('devMqttEnabledBadge');
      if (badge) {
        badge.className = e.target.checked ? 'badge badge-success mini' : 'badge badge-secondary mini';
        badge.textContent = e.target.checked ? 'Enabled' : 'Disabled';
      }
    });
    document.getElementById('btnSaveDevMqtt')?.addEventListener('click', async () => {
      const devId = this.selectedIrDeviceId || document.getElementById('manageDeviceSelect')?.value;
      if (!devId) return;
      this.selectedIrDeviceId = String(devId);
      const topic = document.getElementById('devMqttTopic')?.value.trim();
      const enabled = document.getElementById('devMqttEnabled')?.checked ? 1 : 0;
      const pulse = parseInt(document.getElementById('devMqttPulse')?.value, 10) || 1000;
      const stat = document.getElementById('devMqttStatus');
      if (stat) stat.textContent = 'Saving MQTT...';
      try {
        await this.client.saveDeviceDetails({ deviceId: devId, mqttTopic: topic, mqttEnabled: enabled, mqttPulse: pulse });
        if (stat) stat.innerHTML = '<span style="color:var(--success)">Saved!</span>';
        await this.refreshData();
      } catch (err) {
        if (stat) stat.innerHTML = '<span style="color:var(--danger)">' + err.message + '</span>';
      }
    });
    document.getElementById('btnTestDevMqtt')?.addEventListener('click', async () => {
      const devId = this.selectedIrDeviceId || document.getElementById('manageDeviceSelect')?.value;
      const topic = document.getElementById('devMqttTopic')?.value.trim();
      const pulseMs = parseInt(document.getElementById('devMqttPulse')?.value, 10) || 1000;
      const stat = document.getElementById('devMqttStatus');
      if (!topic) return alert('Enter MQTT topic first');
      if (stat) stat.textContent = 'Testing publish...';
      try {
        const dev = (this.inventory.devices || []).find(d => String(d.id) === String(devId));
        const res = await this.client.testDeviceMqtt({
          deviceId: devId,
          device: dev?.name || devId,
          command: 'PowerToggle',
          topic: topic,
          pulseMs: pulseMs
        });
        if (stat) stat.innerHTML = `<span style="color:var(--success)">Published to ${res.topic || topic}! (${res.pulseMs || pulseMs}ms pulse)</span>`;
      } catch (err) {
        if (stat) stat.innerHTML = '<span style="color:var(--danger)">' + err.message + '</span>';
      }
    });
    document.getElementById('btnCmdEditClose')?.addEventListener('click', () => {
      const modal = document.getElementById('cmdEditModal');
      if (modal) modal.style.display = 'none';
    });
    document.getElementById('btnCmdEditCancel')?.addEventListener('click', () => {
      const modal = document.getElementById('cmdEditModal');
      if (modal) modal.style.display = 'none';
    });
    document.getElementById('btnCmdEditTest')?.addEventListener('click', async () => {
      const devId = document.getElementById('editCmdDevId')?.value;
      const raw = document.getElementById('editCmdPayload')?.value.trim();
      const stat = document.getElementById('cmdEditStatus');
      if (!raw) return alert('No signal data to test');
      if (stat) stat.textContent = 'Testing signal...';
      try {
        if (raw.startsWith('0000 ') || raw.startsWith('0000')) {
          await this.client.blastPronto(raw, devId);
        } else {
          await this.client.sendCommand(devId, document.getElementById('editCmdName')?.value.trim());
        }
        if (stat) stat.innerHTML = '<span style="color:var(--success)">✓ Blasted successfully!</span>';
      } catch (err) {
        if (stat) stat.innerHTML = '<span style="color:var(--danger)">Error: ' + err.message + '</span>';
      }
    });
    document.getElementById('editCmdType')?.addEventListener('change', (e) => {
      const isIr = (e.target.value !== 'bt' && e.target.value !== 'mqtt');
      const irOpts = document.getElementById('editCmdIrOptions');
      if (irOpts) irOpts.style.display = isIr ? 'grid' : 'none';
    });
    document.getElementById('btnCmdEditSave')?.addEventListener('click', async () => {
      const devId = document.getElementById('editCmdDevId')?.value;
      const oldName = document.getElementById('editCmdOldName')?.value;
      const name = document.getElementById('editCmdName')?.value.trim();
      const modal = document.getElementById('cmdEditModal');
      const devType = modal?.dataset?.deviceType || 'IR';
      let mode = document.getElementById('editCmdType')?.value;
      if (devType === 'BT') mode = 'bt';
      else if (devType === 'MQTT') mode = 'mqtt';
      const raw = document.getElementById('editCmdPayload')?.value.trim();
      const repeats = Number(document.getElementById('editCmdRepeats')?.value) || 3;
      const port = Number(document.getElementById('editCmdPort')?.value) || 0;
      const stat = document.getElementById('cmdEditStatus');
      if (!name) return alert('Enter command name');

      if (stat) stat.textContent = 'Saving changes...';
      try {
        const payloadData = {
          deviceId: devId,
          name,
          oldName,
          raw,
          mode
        };
        if (mode !== 'bt' && mode !== 'mqtt') {
          payloadData.repeats = repeats;
          payloadData.port = port;
        }
        await this.client.saveLearnedCommand(payloadData);
        if (stat) stat.innerHTML = '<span style="color:var(--success)">✓ Saved!</span>';
        setTimeout(() => {
          const modal = document.getElementById('cmdEditModal');
          if (modal) modal.style.display = 'none';
        }, 600);
        await this.refreshData();
        this.renderIrView();
      } catch (err) {
        if (stat) stat.innerHTML = '<span style="color:var(--danger)">Error: ' + err.message + '</span>';
      }
    });
    document.getElementById('btnDeleteCurrentDevice')?.addEventListener('click', async () => {
      const devId = document.getElementById('manageDeviceSelect')?.value;
      if (!devId) return;
      if (!await showConfirm('Are you sure you want to delete this device and all its commands?', { title: 'Delete Device', okText: 'Delete' })) return;
      await this.client.deleteDevice(devId);
      await this.refreshData();
      this.renderIrView();
    });
    document.getElementById('btnTestPastedPronto')?.addEventListener('click', async () => {
      const hex = document.getElementById('irdbPaste')?.value.trim();
      const devId = document.getElementById('manageDeviceSelect')?.value;
      if (!hex) return alert('Enter Pronto Hex first');
      const stat = document.getElementById('pasteStatus');
      if (stat) stat.textContent = 'Blasting Pronto code...';
      try {
        await this.client.blastPronto(hex, devId);
        if (stat) stat.innerHTML = '<span style="color:var(--success)">✓ Blasted successfully!</span>';
      } catch (err) {
        if (stat) stat.innerHTML = '<span style="color:var(--danger)">Error: ' + err.message + '</span>';
      }
    });
    document.getElementById('btnSavePastedPronto')?.addEventListener('click', async () => {
      const hex = document.getElementById('irdbPaste')?.value.trim();
      const name = document.getElementById('pasteCmdName')?.value.trim() || 'ProntoCmd';
      const devId = document.getElementById('manageDeviceSelect')?.value;
      if (!hex) return alert('Enter Pronto Hex first');
      const stat = document.getElementById('pasteStatus');
      if (stat) stat.textContent = 'Saving...';
      try {
        await this.client.saveLearnedCommand({ deviceId: devId, name, raw: hex, mode: 'raw' });
        if (stat) stat.innerHTML = '<span style="color:var(--success)">✓ Saved to device!</span>';
        await this.refreshData();
        this.renderIrView();
      } catch (err) {
        if (stat) stat.innerHTML = '<span style="color:var(--danger)">Error: ' + err.message + '</span>';
      }
    });
    document.getElementById('btnEliteGoEnableRf')?.addEventListener('click', async () => {
      try {
        await this.client.toggleRfService(true);
        await this.loadEliteView();
      } catch (e) {
        alert('Enable RF failed: ' + e.message);
      }
    });

    // IR Wizard Actions
    document.getElementById('irdbSource')?.addEventListener('change', (e) => {
      const src = e.target.value;
      const harmonyPanel = document.getElementById('harmonySearchPanel');
      const legacyPanel = document.getElementById('legacyDbSearchPanel');
      if (harmonyPanel && legacyPanel) {
        if (src === 'harmony') {
          harmonyPanel.style.display = 'block';
          legacyPanel.style.display = 'none';
        } else {
          harmonyPanel.style.display = 'none';
          legacyPanel.style.display = 'block';
        }
      }
    });
    document.getElementById('btnCreateNewDevice')?.addEventListener('click', () => this.submitNewDeviceProfile());
    document.getElementById('irdbLoadIndex')?.addEventListener('click', () => this.searchIrdb());
    document.getElementById('irdbFetch')?.addEventListener('click', () => this.fetchSelectedIrdbFile());
    document.getElementById('irdbParsePaste')?.addEventListener('click', () => this.parsePastIrdb());
    document.getElementById('irdbSaveBtn')?.addEventListener('click', () => this.saveSelectedIrdbCommands());
    document.getElementById('wizardCapture')?.addEventListener('click', () => this.captureIrSignal());
    document.getElementById('wizardLearnTest')?.addEventListener('click', () => this.testLearnedIrSignal());
    document.getElementById('wizardSaveBtn')?.addEventListener('click', () => this.saveLearnedIrCommand());
    document.getElementById('wizardTest')?.addEventListener('click', () => this.sendVerifyTestCommand());

    // Bulk IR Lab Actions
    document.getElementById('labCandidatesBtn')?.addEventListener('click', () => this.findLabCandidates());
    document.getElementById('labScan')?.addEventListener('click', () => this.loadLabCommands());
    document.getElementById('labStored')?.addEventListener('click', () => this.useStoredLabCommands());
    document.getElementById('labClear')?.addEventListener('click', () => this.clearLabQueue());
    document.getElementById('labSelectAll')?.addEventListener('click', () => this.setLabSelections(true));
    document.getElementById('labSelectNone')?.addEventListener('click', () => this.setLabSelections(false));
    document.getElementById('labDropUnchecked')?.addEventListener('click', () => this.dropUncheckedLab());
    document.getElementById('labImport')?.addEventListener('click', () => this.importLabSelected());
    document.getElementById('labRun')?.addEventListener('click', () => this.runLabQueue());
    document.getElementById('labStop')?.addEventListener('click', () => this.stopLabQueue());

    // Remote skins fallback handler


    // Bluetooth View Actions
    document.getElementById('btPairToggleBtn')?.addEventListener('click', () => this.toggleBtPairing());
    document.getElementById('btDisconnBtn')?.addEventListener('click', () => this.client.disconnectBt());
    document.getElementById('btRefreshBtn')?.addEventListener('click', () => this.loadBtView());
    document.getElementById('btReleaseAll')?.addEventListener('click', () => {
      const tgt = (document.getElementById('btTargetHostSelect')?.value || '').trim();
      this.client.sendBtKey('release_all', 'tap', tgt);
      this.appendBtLog('Released all keys' + (tgt ? ' on ' + tgt : ''));
    });
    document.getElementById('btSendTextBtn')?.addEventListener('click', () => this.sendBtDirectText());
    document.getElementById('btClearTextBtn')?.addEventListener('click', () => {
      const el = document.getElementById('btDirectText');
      if (el) el.value = '';
    });
    document.getElementById('btRunScriptBtn')?.addEventListener('click', () => this.runBtMacroScript());
    document.getElementById('btSaveCommandBtn')?.addEventListener('click', () => this.saveBtMacroCommand());
    document.getElementById('btClearLogBtn')?.addEventListener('click', () => {
      const el = document.getElementById('btLog');
      if (el) el.textContent = 'Log cleared.';
    });
    document.getElementById('btSaveSettingsBtn')?.addEventListener('click', () => this.saveBluetoothSupervisionSettings());

    // Harmony Archive IR Database Search
    this.setupHarmonyArchiveSearch();

    // Virtual Keyboard Buttons
    document.querySelectorAll('.kb-key').forEach(btn => {
      btn.addEventListener('click', async () => {
        await this.remoteEngine.triggerHaptic();
        const mod = btn.getAttribute('data-mod');
        if (mod) {
          this.kbMods[mod] = !this.kbMods[mod];
          btn.classList.toggle('kb-on', this.kbMods[mod]);
          return;
        }
        const key = btn.getAttribute('data-key');
        if (key) {
          let code = key;
          const mods = [];
          if (this.kbMods.ctrl) mods.push('ctrl');
          if (this.kbMods.shift) mods.push('shift');
          if (this.kbMods.alt) mods.push('alt');
          if (this.kbMods.win) mods.push('win');
          if (mods.length) code = `${mods.join('+')}+${key}`;
          const tgt = (document.getElementById('btTargetHostSelect')?.value || '').trim();
          try {
            await this.client.sendBtKey(code, 'tap', tgt);
            this.appendBtLog('Key: ' + code + (tgt ? ' -> ' + tgt : ''));
          } catch (err) {
            console.warn('BT Key send error:', err);
            this.appendBtLog('Key ' + code + ' failed: ' + (err.message || err));
          }
          if (mods.length) {
            this.kbMods = { ctrl: false, shift: false, alt: false, win: false };
            document.querySelectorAll('.kb-mod').forEach(m => m.classList.remove('kb-on'));
          }
        }
      });
    });

    // Homatics B25 Buttons
    document.querySelectorAll('.b25-hotspot').forEach(btn => {
      btn.addEventListener('click', () => {
        const btnId = btn.getAttribute('data-btn');
        if (btnId) this.selectB25Button(btnId, true);
      });
    });
    document.getElementById('b25ActivitySelect')?.addEventListener('change', () => {
      this.updateB25ButtonStates();
      this.selectB25Button(this.b25SelectedBtn || 'power', false);
    });
    document.getElementById('b25ActionType')?.addEventListener('change', () => this.toggleB25ActionFields());
    document.getElementById('b25ApplyBtn')?.addEventListener('click', () => this.applyB25Mapping());
    document.getElementById('b25ClearBtn')?.addEventListener('click', () => this.clearB25Mapping());
    document.getElementById('b25SaveAllBtn')?.addEventListener('click', () => this.saveAllB25Mappings());
    document.getElementById('b25RunPresetBtn')?.addEventListener('click', () => this.autoMapB25Preset());
    document.getElementById('b25ScanBtn')?.addEventListener('click', () => this.scanB25Remote());
    document.getElementById('b25PairBtn')?.addEventListener('click', () => this.pairB25Remote());
    document.getElementById('b25RefreshBtn')?.addEventListener('click', () => this.loadRemotesView());

    // Harmony Elite Remote Buttons
    document.querySelectorAll('.elite-hotspot').forEach(btn => {
      btn.addEventListener('click', () => {
        const btnId = btn.getAttribute('data-btn');
        if (btnId) this.selectEliteButton(btnId, true);
      });
    });
    document.getElementById('eliteActivitySelect')?.addEventListener('change', (e) => {
      this.eliteSelectedActivityId = e.target.value;
      this.updateEliteButtonStates();
      this.selectEliteButton(this.eliteSelectedBtn || 'Select', false);
      this.renderEliteScreenButtons();
    });
    document.getElementById('btnEliteAddScreenBtn')?.addEventListener('click', () => this.addEliteScreenButton());
    document.getElementById('btnEliteDirectPage1')?.addEventListener('click', () => {
      this.eliteDirectPage = 1;
      const b1 = document.getElementById('btnEliteDirectPage1');
      const b2 = document.getElementById('btnEliteDirectPage2');
      if (b1) b1.className = 'btn btn-xs btn-primary';
      if (b2) b2.className = 'btn btn-xs btn-ghost';
      this.renderEliteScreenButtons();
    });
    document.getElementById('btnEliteDirectPage2')?.addEventListener('click', () => {
      this.eliteDirectPage = 2;
      const b1 = document.getElementById('btnEliteDirectPage1');
      const b2 = document.getElementById('btnEliteDirectPage2');
      if (b1) b1.className = 'btn btn-xs btn-ghost';
      if (b2) b2.className = 'btn btn-xs btn-primary';
      this.renderEliteScreenButtons();
    });
    document.getElementById('eliteActionType')?.addEventListener('change', () => this.toggleEliteActionFields());
    document.getElementById('eliteTargetDevice')?.addEventListener('change', () => this.updateEliteCommandDropdown());
    document.getElementById('eliteApplyBtn')?.addEventListener('click', () => this.applyEliteMapping());
    document.getElementById('eliteClearBtn')?.addEventListener('click', () => this.clearEliteMapping());
    document.getElementById('eliteSaveAllBtn')?.addEventListener('click', () => this.saveAllEliteMappings());
    document.getElementById('elitePushRemoteBtn')?.addEventListener('click', () => this.pushEliteConfiguration());
    document.getElementById('rfPairBtn')?.addEventListener('click', () => this.pairRfRemote(0));
    document.getElementById('rfPairSlot1Btn')?.addEventListener('click', () => this.pairRfRemote(1));
    document.getElementById('rfUnpairSlot1Btn')?.addEventListener('click', () => this.unpairRfSlot(1));
    document.getElementById('rfConfigSlot1Btn')?.addEventListener('click', () => this.openRfConfigModal(1));
    document.getElementById('rfRefreshSlot1Btn')?.addEventListener('click', () => this.refreshRfStatus());
    document.getElementById('rfPullSlot1Btn')?.addEventListener('click', () => this.pullConfigFromRemote(1));
    document.getElementById('rfPairSlot2Btn')?.addEventListener('click', () => this.pairRfRemote(2));
    document.getElementById('rfUnpairSlot2Btn')?.addEventListener('click', () => this.unpairRfSlot(2));
    document.getElementById('rfConfigSlot2Btn')?.addEventListener('click', () => this.openRfConfigModal(2));
    document.getElementById('rfRefreshSlot2Btn')?.addEventListener('click', () => this.refreshRfStatus());
    document.getElementById('rfPullSlot2Btn')?.addEventListener('click', () => this.pullConfigFromRemote(2));
    document.getElementById('rfStopPairBtn')?.addEventListener('click', () => this.stopPairRfRemote());
    document.getElementById('rfUnpairBtn')?.addEventListener('click', () => this.unpairRfRemote());
    document.getElementById('rfRefreshBtn')?.addEventListener('click', () => this.refreshRfStatus());
    document.getElementById('rfConfigModalClose')?.addEventListener('click', () => this.closeRfConfigModal());
    document.getElementById('rfConfigCancelBtn')?.addEventListener('click', () => this.closeRfConfigModal());
    document.getElementById('rfConfigSaveBtn')?.addEventListener('click', () => this.saveRfConfig());
    document.getElementById('btnHubUpdateBanner')?.addEventListener('click', () => {
      this.switchTab('system');
      document.getElementById('updateForm')?.scrollIntoView({ behavior: 'smooth' });
    });
    document.getElementById('btnPushConfigToRemote')?.addEventListener('click', () => this.pushConfigToRemote());
    document.getElementById('rfSlot1UpdateBtn')?.addEventListener('click', () => this.startEliteOta(1));
    document.getElementById('rfSlot2UpdateBtn')?.addEventListener('click', () => this.startEliteOta(2));

    // MQTT Actions
    document.getElementById('mqttSaveBtn')?.addEventListener('click', () => this.saveMqttSettings());

    // Network Actions
    document.getElementById('ethModeSelect')?.addEventListener('change', (e) => {
      const f = document.getElementById('ethStaticFields');
      if (f) f.style.display = e.target.value === 'static' ? 'block' : 'none';
    });
    document.querySelectorAll('.btn-save-eth').forEach(b => {
      b.addEventListener('click', () => this.saveEthernetConfig(b.getAttribute('data-apply')));
    });
    document.querySelectorAll('.btn-save-wifi').forEach(b => {
      b.addEventListener('click', () => this.saveWifiConfig(b.getAttribute('data-apply')));
    });
    document.getElementById('btnWifiScan')?.addEventListener('click', () => this.scanWifiNetworks());
    document.getElementById('btnWifiTest')?.addEventListener('click', () => this.testWifiConfig());
    document.addEventListener('click', (e) => {
      const dd = document.getElementById('wifiScanDropdown');
      if (dd && dd.style.display !== 'none' && !e.target.closest('#wifiScanDropdown') && !e.target.closest('#btnWifiScan')) {
        dd.style.display = 'none';
      }
    });

    // Backup & Restore Actions
    document.querySelectorAll('.btn-backup-export').forEach(b => {
      b.addEventListener('click', () => this.exportBackupItem(b.getAttribute('data-target')));
    });
    document.getElementById('importFile')?.addEventListener('change', (e) => {
      const file = e.target.files && e.target.files[0];
      if (!file) return;
      const reader = new FileReader();
      reader.onload = () => {
        const box = document.getElementById('backupPayload');
        if (box) box.value = reader.result || '';
      };
      reader.readAsText(file);
    });
    document.getElementById('btnRestoreBackup')?.addEventListener('click', () => this.restoreBackupPayload());

    // System Actions
    document.getElementById('btnSaveDebug')?.addEventListener('click', () => this.saveDebugLogging());
    document.getElementById('btnSaveAuth')?.addEventListener('click', () => this.saveWebUiAuth());
    document.getElementById('btnSaveHubName')?.addEventListener('click', async () => {
      const nameInp = document.getElementById('hubCustomNameInput');
      const newName = nameInp?.value.trim();
      const stat = document.getElementById('hubNameStatus');
      if (!newName) return;
      if (stat) stat.textContent = 'Saving...';
      try {
        await this.client.saveSettings({ hub_name: newName });
        if (stat) stat.innerHTML = '<span style="color:var(--success)">Saved!</span>';
        const label = document.getElementById('activeHubLabel');
        if (label) label.textContent = newName;
        const ovLbl = document.getElementById('ovActiveHubLabel');
        if (ovLbl) ovLbl.textContent = `Active: ${newName}`;
        if (this.activeHub) this.activeHub.name = newName;
      } catch (err) {
        if (stat) stat.innerHTML = '<span style="color:var(--danger)">' + err.message + '</span>';
      }
    });
    document.getElementById('btnSaveHubSettings')?.addEventListener('click', async () => {
      const ntp_server = document.getElementById('hubNtpServerInput')?.value.trim();
      const timezone = (document.getElementById('hubTimezoneSelect') || document.getElementById('hubTimezoneInput'))?.value.trim();
      const led_mode = document.getElementById('hubLedModeSelect')?.value;
      const stat = document.getElementById('hubSettingsStatus');
      if (stat) stat.textContent = 'Saving...';
      try {
        await this.client.saveSettings({ ntp_server, timezone, led_mode });
        if (stat) stat.innerHTML = '<span style="color:var(--success)">Saved and applied!</span>';
        setTimeout(() => { if (stat) stat.textContent = ''; }, 3000);
      } catch (err) {
        if (stat) stat.innerHTML = '<span style="color:var(--danger)">' + err.message + '</span>';
      }
    });
    document.getElementById('updateCheck')?.addEventListener('click', () => this.checkSoftwareUpdate());
    document.getElementById('updateInstall')?.addEventListener('click', () => this.installSoftwareUpdate());
    document.getElementById('updateRefresh')?.addEventListener('click', () => this.showSoftwareVersions());
    document.getElementById('btnRediscover')?.addEventListener('click', () => this.rediscoverMqtt());
    document.getElementById('btnRebootHub')?.addEventListener('click', () => this.rebootHub());
    this.setupFactoryResetModal();
    document.getElementById('btnToggleRfSvc')?.addEventListener('click', () => this.toggleRfService());
    document.getElementById('btnToggleBtSvc')?.addEventListener('click', () => this.toggleBtService());
    document.getElementById('btnRefreshLogs')?.addEventListener('click', () => this.refreshLogs());
    document.getElementById('sysLogSource')?.addEventListener('change', () => this.refreshLogs());
    document.getElementById('sysLogAutoRefresh')?.addEventListener('change', (e) => {
      if (e.target.checked) {
        this.logInterval = setInterval(() => this.refreshLogs(), 3000);
      } else if (this.logInterval) {
        clearInterval(this.logInterval);
        this.logInterval = null;
      }
    });

    // Overview Quick Settings
    document.getElementById('btnOvSaveMqtt')?.addEventListener('click', async () => {
      const host = document.getElementById('ovMqttHost')?.value.trim();
      const port = Number(document.getElementById('ovMqttPort')?.value) || 1883;
      const baseTopic = document.getElementById('ovMqttTopic')?.value.trim() || 'harmony/hub';
      const stat = document.getElementById('ovMqttSaveStatus');
      if (stat) stat.textContent = 'Saving...';
      try {
        await this.client.saveMqtt({ host, port, baseTopic, enabled: 1 });
        if (stat) stat.innerHTML = '<span style="color:var(--success)">Saved!</span>';
        setTimeout(() => this.loadDashboard(), 1000);
      } catch (err) {
        if (stat) stat.innerHTML = `<span style="color:var(--danger)">${err.message}</span>`;
      }
    });
    document.getElementById('btnOvRefreshHub')?.addEventListener('click', async () => {
      await this.refreshData();
      if (this.currentTab === 'overview') this.loadDashboard();
    });
    document.getElementById('btnOvRebootHub')?.addEventListener('click', () => this.rebootHub());
    document.getElementById('btnAddHub')?.addEventListener('click', () => this.openAddHubModal());
    document.getElementById('btnRefreshDisk')?.addEventListener('click', () => this.refreshDiskSpace());

    // Icon Browse Buttons & Live Preview Updates
    document.getElementById('btnBrowseActIcons')?.addEventListener('click', () => this.openIconPicker('actEditIcon', 'actIconPreviewBadge'));
    document.getElementById('btnBrowseDevIcons')?.addEventListener('click', () => this.openIconPicker('editDevIcon', 'devIconPreviewBadge'));
    document.getElementById('btnBrowseNewDevIcons')?.addEventListener('click', () => this.openIconPicker('newDeviceIcon', 'newDevIconPreviewBadge'));

    document.getElementById('actEditIcon')?.addEventListener('change', (e) => this.updateBadgeForSelect(e.target, 'actIconPreviewBadge'));
    document.getElementById('editDevIcon')?.addEventListener('change', (e) => this.updateBadgeForSelect(e.target, 'devIconPreviewBadge'));
    document.getElementById('newDeviceIcon')?.addEventListener('change', (e) => this.updateBadgeForSelect(e.target, 'newDevIconPreviewBadge'));

    // Icon Picker Modal
    const iconModal = document.getElementById('iconPickerModal');
    const closeIconModal = () => { if (iconModal) iconModal.style.display = 'none'; };
    document.getElementById('btnIconPickerClose')?.addEventListener('click', closeIconModal);
    document.getElementById('btnIconPickerCancel')?.addEventListener('click', closeIconModal);

    const iconSearch = document.getElementById('iconPickerSearch');
    if (iconSearch) {
      let iconSearchTimer = null;
      iconSearch.addEventListener('input', (e) => {
        clearTimeout(iconSearchTimer);
        iconSearchTimer = setTimeout(() => {
          this.renderIconPickerGrid(e.target.value);
        }, 120);
      });
    }
    document.getElementById('btnIconPickerClearSearch')?.addEventListener('click', () => {
      if (iconSearch) iconSearch.value = '';
      this.renderIconPickerGrid('');
    });

    document.querySelectorAll('.icon-cat-pill').forEach(pill => {
      pill.addEventListener('click', () => {
        const cat = pill.getAttribute('data-cat') || 'all';
        this.currentIconCategory = cat;
        document.querySelectorAll('.icon-cat-pill').forEach(p => {
          p.classList.toggle('btn-primary', p === pill);
          p.classList.toggle('btn-ghost', p !== pill);
        });
        this.renderIconPickerGrid(iconSearch ? iconSearch.value : '');
      });
    });

    // Bluetooth Auto-Detect Delay
    document.getElementById('btnAutoDetectBtDelay')?.addEventListener('click', () => {
      const devId = this.selectedIrDeviceId || this.currentIrDeviceId;
      if (devId) this.openBtDelayCalibrator(devId);
      else showToast('Please select a device first', 'warning');
    });

    const btCalibModal = document.getElementById('btDelayCalibrateModal');
    const closeBtCalib = () => {
      if (btCalibModal) btCalibModal.style.display = 'none';
      if (this._btCalibrateInterval) clearInterval(this._btCalibrateInterval);
      if (this._btCalibratePoll) clearInterval(this._btCalibratePoll);
      const startBtn = document.getElementById('btnBtCalibrateStart');
      if (startBtn) {
        startBtn.disabled = false;
        startBtn.textContent = 'Start Detection Test';
      }
      document.getElementById('btCalibrateTimer')?.classList.remove('bt-timer-active');
    };
    document.getElementById('btnBtCalibrateClose')?.addEventListener('click', closeBtCalib);
    document.getElementById('btnBtCalibrateCancel')?.addEventListener('click', closeBtCalib);

    document.getElementById('btnBtCalibrateStart')?.addEventListener('click', () => {
      const devId = document.getElementById('btCalibrateDevId')?.value;
      if (devId) this.startBtDelayCalibration(devId);
    });

    document.getElementById('btnBtCalibratePowerOff')?.addEventListener('click', async () => {
      const devId = document.getElementById('btCalibrateDevId')?.value;
      if (!devId) return;
      const cmds = this.deviceCommandsMap[devId] || [];
      const offCmd = cmds.find(c => /poweroff|powertoggle|power/i.test(c.name || ''));
      if (offCmd) {
        showToast(`Sending "${offCmd.name}" to device...`, 'info', 2000);
        try {
          await this.client.sendDeviceCommand(devId, offCmd.name);
          const stateEl = document.getElementById('btCalibrateStateMsg');
          if (stateEl) stateEl.textContent = `Power off command "${offCmd.name}" sent.`;
        } catch (e) {
          showToast(`Error sending command: ${e.message}`, 'error');
        }
      } else {
        showToast('No PowerOff/Toggle command found. Please turn off device manually.', 'info', 3000);
      }
    });

    document.getElementById('btnBtCalibrateApply')?.addEventListener('click', () => this.applyCalibratedBtDelay());
  }

  async refreshDiskSpace() {
    try {
      const disk = await this.client.getSystemDisk();
      const flVal = document.getElementById('dashFlashVal');
      const flDet = document.getElementById('dashFlashDetail');
      if (flVal) flVal.textContent = `${disk.freeMb || '2.0'} MB Free`;
      if (flDet) {
        if (disk.cacheFreeMb && disk.cacheFreeMb !== '0.0') {
          flDet.textContent = `Data: ${disk.freeMb}M/${disk.totalMb}M • Cache: ${disk.cacheFreeMb}M/${disk.cacheTotalMb}M`;
        } else {
          flDet.textContent = `${disk.usedMb || '3.0'} MB used (${disk.totalMb || '5.0'} MB total)`;
        }
      }
      const ovDisk = document.getElementById('ovDiskVal');
      if (ovDisk) {
        if (disk.cacheFreeMb && disk.cacheFreeMb !== '0.0') {
          ovDisk.textContent = `Data: ${disk.freeMb}MB free • Cache: ${disk.cacheFreeMb}MB free`;
        } else {
          ovDisk.textContent = `${disk.freeMb || '2.0'} MB free`;
        }
      }
    } catch {}
  }

  async checkHealthAndVersion() {
    try {
      const health = await this.client.getHealth();
      if (health && health.min_app_version) {
        if (this.isNative && this.isVersionOlder(APP_VERSION, health.min_app_version)) {
          this.showOutdatedAppModal(health.min_app_version, APP_VERSION);
        }
      }
    } catch (err) {
      console.warn('Health check note:', err);
    }
    await this.checkOnlineVersions();
    await this.checkBanners();
  }

  isVersionOlder(appVer, minVer) {
    const parse = (v) => String(v).replace(/^v/, '').split('.').map(n => parseInt(n, 10) || 0);
    const a = parse(appVer);
    const m = parse(minVer);
    for (let i = 0; i < Math.max(a.length, m.length); i++) {
      const av = a[i] || 0;
      const mv = m[i] || 0;
      if (av < mv) return true;
      if (av > mv) return false;
    }
    return false;
  }

  showOutdatedAppModal(minVer, curVer) {
    let modal = document.getElementById('outdatedAppModal');
    if (!modal) {
      modal = document.createElement('div');
      modal.id = 'outdatedAppModal';
      modal.style.cssText = 'position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.85);z-index:9999;display:flex;align-items:center;justify-content:center;padding:20px;';
      modal.innerHTML = `
        <div class="card" style="max-width:420px;text-align:center;border-color:var(--danger);box-shadow:0 8px 32px rgba(239,68,68,0.3);">
          <div style="font-size:36px;margin-bottom:8px;">⚠️</div>
          <h3 style="color:var(--danger);margin-bottom:8px;" data-i18n="app_outdated">App Outdated</h3>
          <p style="font-size:14px;color:var(--text-bright);margin-bottom:12px;" data-i18n="app_outdated_msg">
            The connected hub requires a newer version of the Android App. Please update your app.
          </p>
          <div class="muted mini" style="margin-bottom:16px;">
            Current: <strong>v${curVer}</strong> • Required: <strong>v${minVer}+</strong>
          </div>
          <div style="display:flex;gap:8px;justify-content:center;flex-wrap:wrap;">
            <button type="button" class="btn btn-primary" onclick="window.location.reload(true)">Reload</button>
            <button type="button" class="btn btn-secondary" onclick="if(window.caches){caches.keys().then(k=>Promise.all(k.map(n=>caches.delete(n)))).finally(()=>location.reload(true))}else{location.reload(true)}">Clear Cache</button>
            <button type="button" class="btn btn-secondary" onclick="document.getElementById('outdatedAppModal').remove()">Continue Anyway</button>
          </div>
        </div>
      `;
      document.body.appendChild(modal);
    }
  }

  async loadActiveHub() {
    if (!this.isNative) {
      const isBrowser = (typeof window !== 'undefined' && window.location && window.location.hostname);
      const host = isBrowser ? window.location.hostname : '127.0.0.1';
      const port = (isBrowser && window.location.port) ? parseInt(window.location.port, 10) : 8089;
      this.activeHub = {
        id: 'local_hub',
        name: 'Harmony Hub',
        host: host,
        port: port,
        wsPort: port
      };
    } else {
      let saved = await getActiveHub();
      if (!saved) {
        const hubs = await getSavedHubs();
        if (hubs && hubs.length > 0) saved = hubs[0];
      }

      if (saved && saved.host && saved.host !== 'localhost' && saved.host !== '127.0.0.1') {
        this.activeHub = saved;
      } else {
        // Do NOT create a default device for localhost
        this.activeHub = null;
      }
    }

    const lbl = document.getElementById('activeHubLabel');
    const ovLbl = document.getElementById('ovActiveHubLabel');

    if (!this.activeHub || !this.activeHub.host) {
      if (lbl) lbl.textContent = 'No Hub Selected';
      if (ovLbl) ovLbl.textContent = 'Active: None';
      this.client.updateHub(null);
      this.setHubStatus(false);
      this.activitiesData = { activities: [], currentId: '-1', currentName: 'No Hub', isOffline: true };
      this.inventory = { devices: [] };
      this.updateActivityBanner();
      if (this.isNative) {
        setTimeout(() => this.scanNetwork(true), 400);
      }
      return;
    }

    if (lbl) lbl.textContent = this.activeHub.name || 'Harmony Hub';
    if (ovLbl) ovLbl.textContent = `Active: ${this.activeHub.name || this.activeHub.host}`;
    this.client.updateHub(this.activeHub);
    await this.checkHealthAndVersion();
    await this.refreshData();
    this.switchTab(this.currentTab, false);
    this.startHealthMonitoring();
  }

  async refreshData() {
    try {
      const [inv, acts, stat] = await Promise.all([
        this.client.getInventory(),
        this.client.getActivities(),
        this.client.getStatus().catch(() => null)
      ]);
      this.inventory = inv || { devices: [] };
      this.activitiesData = acts || { activities: [], currentId: '-1', currentName: 'PowerOff' };

      if (stat) {
        if (typeof stat.has_icons !== 'undefined') {
          const wasAvailable = stockIconsAvailable;
          setStockIconsAvailable(stat.has_icons);
          if (stat.has_icons && (!this.availableIcons || this.availableIcons.length === 0)) {
            this.loadAvailableIcons();
          }
          if (!wasAvailable && stat.has_icons) {
            if (this.currentTab === 'activity-remote') this.renderActivityRemote();
            else if (this.currentTab === 'control') this.renderControlView();
          }
        }
        if (stat.hub_name) {
          const lbl = document.getElementById('activeHubLabel');
          if (lbl) lbl.textContent = stat.hub_name;
          const ovLbl = document.getElementById('ovActiveHubLabel');
          if (ovLbl) ovLbl.textContent = `Active: ${stat.hub_name}`;
          if (this.activeHub) this.activeHub.name = stat.hub_name;
        }
      }

      // Populate deviceCommandsMap and sort commands alphabetically
      this.deviceCommandsMap = {};
      (this.inventory.devices || []).forEach(d => {
        if (d && d.id) {
          const cmds = Array.isArray(d.commands) ? [...d.commands] : [];
          cmds.sort((a, b) => {
            const na = (typeof a === 'string' ? a : (a.name || a.label || '')).toLowerCase();
            const nb = (typeof b === 'string' ? b : (b.name || b.label || '')).toLowerCase();
            return na.localeCompare(nb);
          });
          this.deviceCommandsMap[d.id] = cmds;
          d.commands = cmds;
        }
      });

      this.recordSuccess();
    } catch (err) {
      console.warn('Failed to load hub data:', err);
      this.recordFailure(err);
      this.activitiesData = { activities: [], currentId: '-1', currentName: 'Offline', isOffline: true };
      this.inventory = { devices: [] };
    }
    this.updateActivityBanner();
    this.checkBanners();
    if (this.currentTab === 'overview') this.loadDashboard();
    else if (this.currentTab === 'activity-remote') this.renderActivityRemote();
    else if (this.currentTab === 'activities') this.loadActivitiesView();
    else if (this.currentTab === 'control') this.renderControlView();
  }

  async refreshActivityState() {
    try {
      const acts = await this.client.getActivities();
      if (acts) {
        this.activitiesData = acts;
        this.recordSuccess();
        this.updateActivityBanner();
      }
    } catch (err) {
      this.recordFailure(err);
    }
  }

  switchTab(tabId, pushHistory = true) {
    if (!tabId || (tabId === 'hubs' && !this.isNative)) tabId = 'overview';
    this.currentTab = tabId;
    if (pushHistory && typeof window !== 'undefined' && window.history) {
      if (window.location.hash !== `#${tabId}`) {
        window.history.pushState({ tab: tabId }, '', `#${tabId}`);
      }
    }

    // Toggle active view panel
    document.querySelectorAll('.view-panel').forEach(panel => {
      panel.classList.toggle('active', panel.id === `view-${tabId}`);
    });

    // Update top nav tabs active state
    document.querySelectorAll('.nav-tab-btn').forEach(btn => {
      const t = btn.getAttribute('data-tab');
      btn.classList.toggle('active', t === tabId);
    });

    // Update bottom nav active state
    document.querySelectorAll('.app-nav .nav-item').forEach(btn => {
      const t = btn.getAttribute('data-tab');
      btn.classList.toggle('active', t === tabId);
    });

    // Update drawer item active state
    document.querySelectorAll('.drawer-item').forEach(btn => {
      const t = btn.getAttribute('data-view-target');
      btn.classList.toggle('active', t === tabId);
    });

    // Load content for active tab
    if (tabId === 'overview') this.loadDashboard();
    else if (tabId === 'activity-remote') this.renderActivityRemote();
    else if (tabId === 'activities') this.loadActivitiesView();
    else if (tabId === 'control') this.renderControlView();
    else if (tabId === 'ir') this.renderIrView();
    else if (tabId === 'lab') this.initLabView();
    else if (tabId === 'bluetooth') this.loadBtView();
    else if (tabId === 'bt-keyboard') this.loadBtKeyboardView();
    else if (tabId === 'rf-remote') { this.loadEliteView(); this.refreshRfStatus(); }
    else if (tabId === 'remotes') this.loadRemotesView();
    else if (tabId === 'mqtt') this.loadMqttView();
    else if (tabId === 'network') this.loadNetworkView();
    else if (tabId === 'backup') this.loadBackupView();
    else if (tabId === 'system') this.loadSystemView();
    else if (tabId === 'hubs') this.renderHubsList();
  }

  async startActivity(id) {
    const actId = String(id);
    await this.remoteEngine.triggerHaptic();
    const acts = this.activitiesData?.activities || [];
    const aObj = acts.find(x => String(x.id) === actId);
    const actName = aObj ? (aObj.name || aObj.label) : actId;

    this.activitiesData.isTransitioning = true;
    this.activitiesData.transitionTarget = actId;
    this.activitiesData.transitionStep = 1;
    this.activitiesData.transitionTotal = 4;
    this.activitiesData.transitionDesc = `Starting ${actName}...`;
    this.updateActivityBanner();

    try {
      const res = await this.client.startActivity(actId);
      if (res && res.error && !res.ok) {
        alert(res.error);
        this.activitiesData.isTransitioning = false;
        this.updateActivityBanner();
        return;
      }
      this.pollActivityTransition(actId);
    } catch (err) {
      alert('Failed to start activity: ' + (err.message || err));
      this.activitiesData.isTransitioning = false;
      this.updateActivityBanner();
    }
  }

  async stopActivity() {
    await this.remoteEngine.triggerHaptic();
    this.activitiesData.isTransitioning = true;
    this.activitiesData.transitionTarget = '-1';
    this.activitiesData.transitionStep = 1;
    this.activitiesData.transitionTotal = 3;
    this.activitiesData.transitionDesc = 'Powering off devices...';
    this.updateActivityBanner();

    try {
      const res = await this.client.stopActivity();
      if (res && res.error && !res.ok) {
        alert(res.error);
        this.activitiesData.isTransitioning = false;
        this.updateActivityBanner();
        return;
      }
      this.pollActivityTransition('-1');
    } catch (err) {
      alert('Failed to stop activity: ' + (err.message || err));
      this.activitiesData.isTransitioning = false;
      this.updateActivityBanner();
    }
  }

  pollActivityTransition(targetId, attempts = 35) {
    if (this.actPollTimer) {
      clearTimeout(this.actPollTimer);
      this.actPollTimer = null;
    }
    const check = async () => {
      await this.refreshActivityState();
      this.updateActivityBanner();
      if (this.currentTab === 'activity-remote') {
        this.renderActivityRemote();
      }
      const isTrans = Boolean(this.activitiesData && this.activitiesData.isTransitioning);
      const cur = String((this.activitiesData && this.activitiesData.currentId) ?? '-1');
      const targetMatches = (targetId === null || targetId === undefined || cur === String(targetId));

      if (attempts > 0 && (isTrans || !targetMatches)) {
        attempts--;
        this.actPollTimer = setTimeout(check, 600);
      } else {
        this.actPollTimer = null;
        this.wsProgress = null;
        if (this.activitiesData) {
          this.activitiesData.isTransitioning = false;
        }
        this.updateActivityBanner();
        if (this.currentTab === 'activity-remote') {
          this.renderActivityRemote();
        }
      }
    };
    check();
  }

  updateActivityBanner() {
    const listEl = document.getElementById('bannerActivitiesList');
    if (!listEl) return;

    // Filter out extraneous Power Off activity (id = -1 or named PowerOff)
    const acts = (this.activitiesData.activities || []).filter(a => {
      const idStr = String(a.id || '');
      const nameStr = (a.name || a.label || '').toLowerCase().replace(/[\s_-]/g, '');
      return idStr !== '-1' && idStr !== '-1.0' && a.id !== -1 && nameStr !== 'poweroff';
    });
    const curId = String(this.activitiesData.currentId || '-1');
    const curName = (this.activitiesData.currentName || '').toLowerCase().replace(/[\s_-]/g, '');
    const isOff = curId === '-1' || curId === '0' || !curId || curName === 'poweroff';
    const isTransitioning = Boolean(this.activitiesData.isTransitioning || this.wsProgress);

    const pwrBtn = document.getElementById('btnBannerPowerOff');
    if (pwrBtn) {
      pwrBtn.classList.toggle('active', isOff);
      pwrBtn.classList.remove('btn-primary');
      pwrBtn.classList.add('btn-danger');
      pwrBtn.title = isOff ? 'Hub is currently Powered Off' : 'Power off all devices';
      pwrBtn.disabled = isTransitioning;
      if (isTransitioning) pwrBtn.style.opacity = '0.5';
      else if (isOff) pwrBtn.style.opacity = '0.75';
      else pwrBtn.style.opacity = '';
    }

    // Update Transition Progress Card
    const transCard = document.getElementById('bannerTransitionCard');
    const transName = document.getElementById('bannerTransitionName');
    const transDesc = document.getElementById('bannerTransitionDesc');
    const transBar = document.getElementById('bannerTransitionProgress');

    if (transCard) {
      if (isTransitioning) {
        transCard.style.display = 'block';
        const targetId = String(this.wsProgress?.activity || this.activitiesData?.transitionTarget || '');
        const targetAct = acts.find(a => String(a.id) === targetId);
        let titleText = 'Switching Activity...';
        if (targetId === '-1' || targetId === '0') {
          titleText = 'Powering Off...';
        } else if (targetAct) {
          titleText = `Starting ${targetAct.name || targetAct.label}...`;
        }

        const step = this.wsProgress?.step ?? this.activitiesData?.transitionStep ?? 0;
        const total = this.wsProgress?.total ?? this.activitiesData?.transitionTotal ?? 0;
        const descText = this.wsProgress?.desc || this.activitiesData?.transitionDesc || (step > 0 ? `Step ${step} of ${total}` : 'Please wait...');
        const pct = total > 0 ? Math.min(100, Math.max(10, Math.round((step / total) * 100))) : 40;

        if (transName) transName.textContent = titleText;
        if (transDesc) transDesc.textContent = descText;
        if (transBar) transBar.style.width = `${pct}%`;
      } else {
        transCard.style.display = 'none';
      }
    }

    listEl.innerHTML = acts.map(a => {
      const isActive = !isOff && String(a.id) === curId;
      const isTarget = isTransitioning && String(a.id) === String(this.activitiesData?.transitionTarget);
      return `<button type="button" class="btn btn-xs ${isActive ? 'btn-primary' : 'btn-secondary'} banner-activity-btn ${isActive ? 'active' : ''} ${isTarget ? 'btn-switching' : ''}" data-act-id="${a.id}" ${isTransitioning ? 'disabled style="opacity:0.6;"' : ''}>${getActivityIconSvg(a.icon, a.type, 13)}${escapeHtml(a.name || a.label)}</button>`;
    }).join('');

    listEl.querySelectorAll('.banner-activity-btn').forEach(btn => {
      btn.addEventListener('click', async () => {
        const id = btn.getAttribute('data-act-id');
        if (!isOff && id === curId) return;
        await this.startActivity(id);
      });
    });
  }

  /* -------------------------------------------------------------
   * 1. Activity Remote View
   * ------------------------------------------------------------- */
  async renderActivityRemote() {
    const cont = document.getElementById('activityRemoteContainer');
    if (!cont) return;

    if (!this.activeHub || !this.activeHub.host) {
      cont.innerHTML = `
        <div class="card" style="text-align: center; padding: 36px 16px;">
          <div style="font-size: 36px; margin-bottom: 8px;">🔍</div>
          <h3 style="font-size: 16px; margin-bottom: 8px;">No Harmony Hub Configured</h3>
          <p class="muted" style="font-size: 13px; margin-bottom: 16px;">Scan your network for Hubs or add one manually to start using the remote.</p>
          <div style="display: flex; gap: 8px; justify-content: center;">
            <button type="button" class="btn btn-sm btn-primary" onclick="window.harmonyApp && window.harmonyApp.scanNetwork(true)">Scan Network</button>
            <button type="button" class="btn btn-sm btn-secondary" onclick="window.harmonyApp && window.harmonyApp.switchTab('hubs')">Manage Hubs</button>
          </div>
        </div>
      `;
      return;
    }

    if (!this.isHubOnline) {
      cont.innerHTML = `
        <div class="card" style="text-align: center; padding: 36px 16px;">
          <div style="font-size: 36px; margin-bottom: 8px;">📡</div>
          <h3 style="font-size: 16px; margin-bottom: 8px; color: var(--danger);">Hub Connection Lost</h3>
          <p class="muted" style="font-size: 13px; margin-bottom: 16px;">Unable to reach Harmony Hub at ${escapeHtml(this.activeHub.host)}. Checking health and reconnecting...</p>
          <button type="button" class="btn btn-sm btn-primary" onclick="window.harmonyApp && window.harmonyApp.pollHubHealth(true)">Retry Connection</button>
        </div>
      `;
      return;
    }

    const curId = String(this.activitiesData.currentId || '-1');
    const curName = (this.activitiesData.currentName || '').toLowerCase().replace(/[\s_-]/g, '');
    const isOff = curId === '-1' || curId === '0' || !curId || curName === 'poweroff';

    const curAct = !isOff ? (this.activitiesData.activities || []).find(a => String(a.id) === curId) : null;

    if (isOff || !curAct) {
      const realActs = (this.activitiesData.activities || []).filter(a => {
        const idStr = String(a.id || '');
        const nameStr = (a.name || a.label || '').toLowerCase().replace(/[\s_-]/g, '');
        return idStr !== '-1' && idStr !== '-1.0' && a.id !== -1 && nameStr !== 'poweroff';
      });

      cont.innerHTML = `
        <div class="card" style="text-align: center; padding: 32px 16px;">
          <h3 style="font-size: 16px; margin-bottom: 8px;">Hub is in PowerOff State</h3>
          <p class="muted" style="font-size: 13px; margin-bottom: 16px;">Select an activity from the top banner to turn on devices and use the remote.</p>
          <div style="display: flex; gap: 8px; justify-content: center; flex-wrap: wrap;">
            ${realActs.map(a => `
              <button type="button" class="btn btn-sm btn-primary btn-start-act-idle" data-act-id="${a.id}" style="display:inline-flex; align-items:center; gap:6px;">${getActivityIconSvg(a.icon, a.type, 16)}<span>Start ${a.name || a.label}</span></button>
            `).join('')}
          </div>
        </div>
      `;
      cont.querySelectorAll('.btn-start-act-idle').forEach(b => {
        b.addEventListener('click', async () => {
          const actId = b.getAttribute('data-act-id');
          await this.startActivity(actId);
        });
      });
      return;
    }

    await this.remoteEngine.render(cont, {
      scope: 'activity',
      target: curAct,
      iconSvg: getActivityIconSvg(curAct.icon, curAct.type, 18),
      availableDevices: this.inventory.devices || [],
      commandsMap: this.deviceCommandsMap,
      onSendCommand: async (devId, cmd) => {
        await this.client.sendCommand(devId, cmd);
      }
    });
  }

  /* -------------------------------------------------------------
   * 2. Dashboard View
   * ------------------------------------------------------------- */
  async loadDashboard() {
    const fwEl = document.getElementById('dashFirmwareVal');
    const utEl = document.getElementById('dashUptimeVal');
    const mqBadge = document.getElementById('dashMqttBadge');
    const mqDet = document.getElementById('dashMqttDetail');
    const invVal = document.getElementById('dashInventoryVal');
    const cmdVal = document.getElementById('dashCommandsVal');
    const actName = document.getElementById('dashActiveActName');
    const actDet = document.getElementById('dashActiveActDetail');

    if (actName) actName.textContent = this.activitiesData.currentName || 'PowerOff';
    if (actDet) actDet.textContent = `ID: ${this.activitiesData.currentId || '-1'}`;

    const devs = this.inventory.devices || [];
    let totalCmds = 0;
    devs.forEach(d => { totalCmds += (d.commands || []).length; });
    if (invVal) invVal.textContent = `${devs.length} devices`;
    if (cmdVal) cmdVal.textContent = `${totalCmds} commands`;

    try {
      const mq = await this.client.getMqttStatus();
      if (mqBadge) {
        mqBadge.className = `badge ${mq.connected ? 'ok' : 'bad'}`;
        mqBadge.textContent = mq.connected ? 'Connected' : 'Disconnected';
      }
      const ovMqBadge = document.getElementById('overviewMqttBadge');
      if (ovMqBadge) {
        ovMqBadge.className = `badge ${mq.connected ? 'ok' : 'bad'}`;
        ovMqBadge.textContent = mq.connected ? 'Connected' : 'Disconnected';
      }
      if (mqDet) mqDet.textContent = mq.broker ? `${mq.broker}:${mq.port}` : (mq.enabled ? 'Bridge enabled' : 'Bridge disabled');
      const dashMqPrev = document.getElementById('dashMqttBrokerPreview');
      if (dashMqPrev) {
        dashMqPrev.textContent = mq.broker ? `${mq.broker}:${mq.port || 1883}` : (mq.enabled ? 'Enabled (no host)' : 'Disabled');
      }
      const hIn = document.getElementById('ovMqttHost');
      const pIn = document.getElementById('ovMqttPort');
      const tIn = document.getElementById('ovMqttTopic');
      if (hIn && mq.broker && !hIn.value) hIn.value = mq.broker;
      if (pIn && mq.port && !pIn.value) pIn.value = mq.port;
      if (tIn && mq.baseTopic && !tIn.value) tIn.value = mq.baseTopic;
    } catch {}

    try {
      const net = await this.client.getNetworkStatus().catch(() => null);
      if (net) {
        const ovIp = document.getElementById('ovIpVal');
        if (ovIp && net.ip) ovIp.textContent = net.ip;
        const ovDetail = document.getElementById('ovNetDetail');
        if (ovDetail) {
          ovDetail.textContent = `${net.active_interface || 'ath0'} (${net.connection_type || 'Wi-Fi'})`;
        }
      }
    } catch {}

    try {
      const status = await this.client.request('/api/status').catch(() => null);
      if (fwEl) fwEl.textContent = 'Linux 2.6.31 (Codex 4.15.600)';
      if (status && status.uptime_sec !== undefined) {
        const sec = Number(status.uptime_sec) || 0;
        const days = Math.floor(sec / 86400);
        const hours = Math.floor((sec % 86400) / 3600);
        const mins = Math.floor((sec % 3600) / 60);
        let uptStr = '';
        if (days > 0) uptStr += `${days}d `;
        if (hours > 0 || days > 0) uptStr += `${hours}h `;
        uptStr += `${mins}m`;
        if (utEl) utEl.textContent = uptStr || `${sec}s`;
      } else if (utEl) {
        utEl.textContent = 'Online';
      }
    } catch {}

    try {
      const disk = await this.client.getSystemDisk();
      const flVal = document.getElementById('dashFlashVal');
      const flDet = document.getElementById('dashFlashDetail');
      if (flVal) flVal.textContent = `${disk.freeMb || '2.0'} MB Free`;
      if (flDet) {
        if (disk.cacheFreeMb && disk.cacheFreeMb !== '0.0') {
          flDet.textContent = `Data: ${disk.freeMb}M/${disk.totalMb}M • Cache: ${disk.cacheFreeMb}M/${disk.cacheTotalMb}M`;
        } else {
          flDet.textContent = `${disk.usedMb || '3.0'} MB used (${disk.totalMb || '5.0'} MB total)`;
        }
      }
      const ovDisk = document.getElementById('ovDiskVal');
      if (ovDisk) {
        if (disk.cacheFreeMb && disk.cacheFreeMb !== '0.0') {
          ovDisk.textContent = `Data: ${disk.freeMb}MB free • Cache: ${disk.cacheFreeMb}MB free`;
        } else {
          ovDisk.textContent = `${disk.freeMb || '2.0'} MB free`;
        }
      }
    } catch {}

    let svcs = null;
    try {
      svcs = await this.client.getServices().catch(() => null);
    } catch {}

    try {
      const rfBadge = document.getElementById('dashEliteBadge');
      if (rfBadge) {
        if (svcs && svcs.rf_enabled === false) {
          rfBadge.className = 'badge bad';
          rfBadge.textContent = 'RF Disabled';
        } else {
          const rf = await this.client.getRfStatus().catch(() => null);
          if (rf) {
            rfBadge.className = `badge ${rf.paired ? 'ok' : 'subtle'}`;
            rfBadge.textContent = rf.paired ? 'RF Paired' : (rf.pairing_active ? 'RF Pairing' : 'RF Ready');
          } else {
            rfBadge.className = 'badge subtle';
            rfBadge.textContent = 'RF Ready';
          }
        }
      }
    } catch {}

    try {
      const btBadge = document.getElementById('dashBtRemoteBadge');
      const hostBadge = document.getElementById('dashBtHostBadge');
      const kbBadge = document.getElementById('dashBtKbBadge');
      if (svcs && svcs.bt_enabled === false) {
        if (btBadge) { btBadge.className = 'badge bad'; btBadge.textContent = 'BT Disabled'; }
        if (hostBadge) { hostBadge.className = 'badge bad'; hostBadge.textContent = 'BT Disabled'; }
        if (kbBadge) { kbBadge.className = 'badge bad'; kbBadge.textContent = 'BT Disabled'; }
      } else {
        const bt = await this.client.getBtRemotePairStatus().catch(() => null);
        if (btBadge && bt) {
          btBadge.className = `badge ${bt.paired ? 'ok' : 'subtle'}`;
          btBadge.textContent = bt.paired ? 'BLE Paired' : 'BLE Remote';
        }
        const btHost = await this.client.request('/api/bt-status').catch(() => null);
        if (hostBadge && btHost) {
          hostBadge.className = `badge ${btHost.connected ? 'ok' : 'subtle'}`;
          hostBadge.textContent = btHost.connected ? 'Connected' : 'HID Ready';
        }
        if (kbBadge) {
          kbBadge.className = `badge ${btHost && btHost.connected ? 'ok' : 'subtle'}`;
          kbBadge.textContent = btHost && btHost.connected ? 'Ready' : 'Virtual KB';
        }
      }
    } catch {}

    try {
      const upBadge = document.getElementById('dashUpdateBadge');
      const upDetail = document.getElementById('dashUpdateDetail');
      if (upDetail) {
        upDetail.textContent = `v${this.appVersion || APP_VERSION}`;
      }
      if (upBadge) {
        if (this.hasUpdate) {
          upBadge.className = 'badge warn';
          upBadge.textContent = 'Update Available';
        } else {
          upBadge.className = 'badge ok';
          upBadge.textContent = 'Up to date';
        }
      }
    } catch {}
  }

  /* -------------------------------------------------------------
   * 3. Activities Manager View
   * ------------------------------------------------------------- */
  async loadActivitiesView() {
    await this.refreshActivityState();
    const curNameEl = document.getElementById('actCurrentName');
    const curMetaEl = document.getElementById('actCurrentMeta');
    const badgeEl = document.getElementById('actCurrentStatusBadge');

    if (curNameEl) curNameEl.textContent = this.activitiesData.currentName || 'PowerOff';
    if (curMetaEl) curMetaEl.textContent = `Activity ID: ${this.activitiesData.currentId || '-1'}`;
    if (badgeEl) {
      badgeEl.className = `badge ${this.activitiesData.isTransitioning ? 'warn' : 'ok'}`;
      badgeEl.textContent = this.activitiesData.isTransitioning ? 'Switching...' : 'Idle';
    }

    const grid = document.getElementById('actGrid');
    if (!grid) return;

    // Include PowerOff as distinct editable activity
    const acts = [...(this.activitiesData.activities || [])];
    const hasPowerOff = acts.some(a => String(a.id) === '-1' || (a.name || '').toLowerCase() === 'poweroff');
    if (!hasPowerOff) {
      acts.unshift({
        id: '-1',
        name: 'PowerOff',
        type: 'PowerOff',
        label: 'PowerOff',
        startSequence: [],
        stopSequence: [],
        deviceIds: []
      });
    }

    if (!acts.length) {
      grid.innerHTML = '<div class="muted">No configured activities found.</div>';
      return;
    }

    grid.innerHTML = acts.map(a => {
      const devIds = a.deviceIds || [];
      const devObjs = devIds.map(did => {
        const d = (this.inventory.devices || []).find(x => String(x.id) === String(did));
        return {
          id: did,
          name: d ? (d.name || d.model || did) : did,
          iconSvg: d ? getDeviceIconSvg(d.type, d.manufacturer, d.model, d.icon || d.Icon || '', 12) : ''
        };
      });
      const startCount = (a.startSequence || []).length;
      const stopCount = (a.stopSequence || []).length;
      const isPowerOff = String(a.id) === '-1' || (a.name || '').toLowerCase() === 'poweroff';
      const isRunning = isPowerOff ?
        (String(this.activitiesData.currentId) === '-1' || !this.activitiesData.currentId) :
        (String(a.id) === String(this.activitiesData.currentId));
      const statusBadgeText = isPowerOff ? (isRunning ? 'Off' : 'Standby') : (isRunning ? 'Active' : 'Configured');
      return `
        <div class="card" style="display: flex; flex-direction: column; justify-content: space-between; gap: 10px;">
          <div>
            <div style="display: flex; justify-content: space-between; align-items: flex-start; margin-bottom: 6px;">
              <strong style="font-size: 15px; display: inline-flex; align-items: center; gap: 6px;">${getActivityIconSvg(a.icon, a.type, 18)}${a.name || a.label}</strong>
              <span class="badge ${isRunning ? 'ok' : ''}">${statusBadgeText}</span>
            </div>
            <div class="muted mini" style="margin-bottom: 8px;">${startCount} start steps • ${stopCount} stop steps • ${devIds.length} devices</div>
            <div style="display: flex; flex-wrap: wrap; gap: 4px; margin-bottom: 8px;">
              ${devObjs.map(d => `<span style="background:var(--bg-btn); font-size:11px; padding:2px 8px; border-radius:4px; border:1px solid var(--border); display:inline-flex; align-items:center; gap:4px;">${d.iconSvg}<span>${d.name}</span></span>`).join('')}
            </div>
          </div>
          <div style="display: flex; gap: 6px; margin-top: 6px;">
            <button type="button" class="btn btn-xs ${isPowerOff ? 'btn-danger' : (isRunning ? 'btn-ghost' : 'btn-primary')} btn-act-run" data-id="${a.id}" style="flex: 1;">${isRunning ? (isPowerOff ? 'Powered Off' : 'Active') : (isPowerOff ? 'Turn Off' : 'Run')}</button>
            <button type="button" class="btn btn-xs btn-secondary btn-act-edit" data-id="${a.id}" style="flex: 1;">Edit & Steps</button>
          </div>
        </div>
      `;
    }).join('');

    grid.querySelectorAll('.btn-act-run').forEach(b => {
      b.addEventListener('click', async () => {
        const id = b.getAttribute('data-id');
        if (id === '-1') {
          await this.stopCurrentActivity();
        } else {
          await this.startActivity(id);
        }
      });
    });

    grid.querySelectorAll('.btn-act-edit').forEach(b => {
      b.addEventListener('click', () => {
        const id = b.getAttribute('data-id');
        const act = acts.find(x => String(x.id) === String(id));
        this.openActivityEditor(act);
      });
    });

    // Populate transition simulator dropdowns
    const fromSel = document.getElementById('previewFromAct');
    const toSel = document.getElementById('previewToAct');
    if (fromSel && toSel) {
      const opts = acts.map(a => `<option value="${a.id}">${a.name || a.label}</option>`).join('');
      fromSel.innerHTML = `<option value="-1">PowerOff</option>${opts}`;
      toSel.innerHTML = `${opts}<option value="-1">PowerOff</option>`;
    }
  }

  openActivityEditor(act) {
    const box = document.getElementById('actEditorBox');
    if (!box) return;
    box.style.display = 'block';

    const actNav = document.getElementById('actEditTabsNav');
    if (actNav) {
      actNav.querySelectorAll('.segmented-btn').forEach((b, idx) => b.classList.toggle('active', idx === 0));
      const pProfile = document.getElementById('actTabPanelProfile');
      const pStart = document.getElementById('actTabPanelStart');
      const pStop = document.getElementById('actTabPanelStop');
      if (pProfile) pProfile.style.display = 'block';
      if (pStart) pStart.style.display = 'none';
      if (pStop) pStop.style.display = 'none';
    }

    const titleEl = document.getElementById('actEditorTitle');
    const idInput = document.getElementById('actEditId');
    const nameInput = document.getElementById('actEditName');
    const typeSelect = document.getElementById('actEditType');
    const iconInput = document.getElementById('actEditIcon');
    const orderInput = document.getElementById('actEditOrder');
    const delBtn = document.getElementById('actDeleteBtn');

    let iconVal = act ? (act.icon || 'watchtv.png') : 'watchtv.png';
    if (!iconVal.endsWith('.png')) iconVal += '.png';
    if (iconInput) {
      iconInput.value = iconVal;
      if (!iconInput.value) {
        iconInput.value = iconVal.replace('_small.png', '.png');
      }
      if (!iconInput.value) {
        iconInput.value = 'generic.png';
      }
      this.updateBadgeForSelect(iconInput, 'actIconPreviewBadge');
    }

    if (act) {
      if (titleEl) titleEl.textContent = `Edit Activity: ${act.name}`;
      if (idInput) idInput.value = act.id;
      if (nameInput) nameInput.value = act.name || '';
      if (typeSelect) typeSelect.value = act.type || 'Custom';
      if (orderInput) orderInput.value = act.order || 1;
      if (delBtn) delBtn.style.display = (String(act.id) === '-1') ? 'none' : 'block';
    } else {
      if (titleEl) titleEl.textContent = 'Create New Activity';
      if (idInput) idInput.value = '';
      if (nameInput) nameInput.value = '';
      if (typeSelect) typeSelect.value = 'Custom';
      if (orderInput) orderInput.value = 1;
      if (delBtn) delBtn.style.display = 'none';
    }

    // Participating devices in power order (like web UI)
    const chkCont = document.getElementById('actDevicesCheckboxes');
    if (chkCont) {
      const partDevIds = (act?.deviceIds || act?.devices || []).map(String);
      chkCont.innerHTML = `
        <div id="actDevOrderedList" class="queue-list" style="min-height: 40px; max-height: 180px; margin-bottom: 6px;"></div>
        <div id="actAddDevControls" style="display: flex; gap: 6px; align-items: center; margin-top: 4px;">
          <select id="actAddDevSelect" class="form-control" style="flex: 1;"></select>
          <button type="button" id="actAddDevBtn" class="btn btn-xs btn-secondary">+ Add Device</button>
        </div>
        <div class="muted mini" style="margin-top: 4px;">Start/Stop Order: Devices power ON in order (#1..#N) and power OFF in reverse.</div>
      `;

      const listWrap = document.getElementById('actDevOrderedList');
      const addControls = document.getElementById('actAddDevControls');
      const addSel = document.getElementById('actAddDevSelect');
      const addBtn = document.getElementById('actAddDevBtn');

      const refreshAddDevControls = () => {
        if (!addControls || !addSel) return;
        const currentDevIds = new Set(
          Array.from(listWrap.querySelectorAll('.act-dev-row')).map(r => String(r.dataset.devId))
        );
        const availableDevices = (this.inventory.devices || []).filter(
          d => !currentDevIds.has(String(d.id))
        );
        if (!availableDevices.length) {
          addControls.style.display = 'none';
          addSel.innerHTML = '';
        } else {
          addControls.style.display = 'flex';
          addSel.innerHTML = availableDevices.map(
            d => `<option value="${d.id}">${d.name || d.model || d.id}</option>`
          ).join('');
        }
      };

      const renderDevRow = (did) => {
        const placeholder = listWrap.querySelector('.no-devs-msg');
        if (placeholder) placeholder.remove();

        const d = (this.inventory.devices || []).find(x => String(x.id) === String(did));
        const name = d ? (d.name || d.model || d.id) : `Device ${did}`;
        const devIconSvg = d ? getDeviceIconSvg(d.type, d.manufacturer, d.model, d.icon || d.Icon || '', 16) : '';
        const row = document.createElement('div');
        row.className = 'queue-row act-dev-row';
        row.dataset.devId = String(did);
        row.style.cssText = 'display:flex;align-items:center;gap:8px;padding:6px 8px;margin-bottom:4px;';
        row.innerHTML = `
          <span class="muted mini act-dev-num" style="min-width:24px;">#</span>
          <span style="display:inline-flex;align-items:center;margin-right:2px;">${devIconSvg}</span>
          <span style="flex:1;font-weight:600;font-size:13px;">${name}</span>
          <div style="display:flex;gap:4px;">
            <button type="button" class="btn btn-xs btn-ghost btn-dev-up" title="Power on earlier">↑</button>
            <button type="button" class="btn btn-xs btn-ghost btn-dev-down" title="Power on later">↓</button>
            <button type="button" class="btn btn-xs btn-danger btn-dev-del" title="Remove device">✕</button>
          </div>
        `;
        row.querySelector('.btn-dev-up').addEventListener('click', () => {
          if (row.previousElementSibling) {
            listWrap.insertBefore(row, row.previousElementSibling);
            updateDevNums();
          }
        });
        row.querySelector('.btn-dev-down').addEventListener('click', () => {
          if (row.nextElementSibling) {
            listWrap.insertBefore(row.nextElementSibling, row);
            updateDevNums();
          }
        });
        row.querySelector('.btn-dev-del').addEventListener('click', () => {
          row.remove();
          updateDevNums();
          if (!listWrap.querySelectorAll('.act-dev-row').length) {
            listWrap.innerHTML = '<div class="muted mini no-devs-msg" style="padding: 8px;">No participating devices configured.</div>';
          }
          refreshAddDevControls();
        });
        listWrap.appendChild(row);
        updateDevNums();
      };

      const updateDevNums = () => {
        listWrap.querySelectorAll('.act-dev-row').forEach((r, idx) => {
          const num = r.querySelector('.act-dev-num');
          if (num) num.textContent = `#${idx + 1}`;
        });
      };

      if (!partDevIds.length) {
        listWrap.innerHTML = '<div class="muted mini no-devs-msg" style="padding: 8px;">No participating devices configured.</div>';
      } else {
        partDevIds.forEach(did => renderDevRow(did));
      }
      refreshAddDevControls();

      if (addBtn) {
        addBtn.addEventListener('click', () => {
          const val = addSel?.value;
          if (!val) return;
          const exists = Array.from(listWrap.querySelectorAll('.act-dev-row')).some(r => r.dataset.devId === String(val));
          if (exists) return alert('Device already added to order.');
          renderDevRow(val);
          refreshAddDevControls();
        });
      }
    }

    // Render Sequences (support startSequence/stopSequence or startSteps/stopSteps)
    let rawStart = act?.startSequence || act?.startSteps || [];
    let rawStop = act?.stopSequence || act?.stopSteps || [];
    if (typeof rawStart === 'string') {
      rawStart = rawStart.split('\n').filter(Boolean).map(l => {
        const p = l.split('|');
        if (p[0] === 'Delay') return { type: 'Delay', deviceId: '0', command: '', delay: parseInt(p[3] || p[1], 10) || 1000 };
        if (p[0] === 'Switched' || p[0] === 'ActivitySwitched' || p[2] === 'Switched') return { type: 'Switched', deviceId: '0', command: 'Switched', delay: 0 };
        return { type: (p[0] && p[0] !== 'IRCommand') ? p[0] : 'Command', deviceId: p[1] || '0', command: p[2] || '', delay: 0 };
      });
    }
    if (typeof rawStop === 'string') {
      rawStop = rawStop.split('\n').filter(Boolean).map(l => {
        const p = l.split('|');
        if (p[0] === 'Delay') return { type: 'Delay', deviceId: '0', command: '', delay: parseInt(p[3] || p[1], 10) || 1000 };
        if (p[0] === 'Switched' || p[0] === 'ActivitySwitched' || p[2] === 'Switched') return { type: 'Switched', deviceId: '0', command: 'Switched', delay: 0 };
        return { type: (p[0] && p[0] !== 'IRCommand') ? p[0] : 'Command', deviceId: p[1] || '0', command: p[2] || '', delay: 0 };
      });
    }
    this.renderSequenceList('actStartStepsList', rawStart);
    this.renderSequenceList('actStopStepsList', rawStop);
  }

  renderSequenceList(containerId, steps = []) {
    const cont = document.getElementById(containerId);
    if (!cont) return;
    cont.innerHTML = '';
    if (!steps || !steps.length) {
      cont.innerHTML = '<div class="muted mini" style="padding: 8px;">No steps configured.</div>';
      return;
    }
    steps.forEach((st, idx) => {
      let type = st.type || (st.__type === 'DelayActivityAction' ? 'Delay' : 'Command');
      if (type === 'IRCommand') type = 'Command';
      const cName = st.command || st.Command || st.CommandName || '';
      if (type === 'Switched' || type === 'ActivitySwitched' || (st.__type && st.__type.includes('Switched')) || cName === 'Switched') {
        type = 'Switched';
      }
      this.addSequenceStep(containerId, type, st, idx);
    });
  }

  addSequenceStep(containerId, type = 'Command', initialData = null, index = -1) {
    const cont = document.getElementById(containerId);
    if (!cont) return;
    const placeholder = cont.querySelector('.muted');
    if (placeholder) placeholder.remove();

    const row = document.createElement('div');
    row.className = 'queue-row';
    const initCmd = initialData?.command || initialData?.Command || initialData?.CommandName || '';
    const isSwitchedExplicit = (type === 'Switched' || type === 'ActivitySwitched' || initCmd === 'Switched' || initialData?.type === 'Switched' || initialData?.type === 'ActivitySwitched');
    const normType = isSwitchedExplicit ? 'Switched' : ((type === 'IRCommand') ? 'Command' : (type || 'Command'));
    const isDelay = (normType === 'Delay');
    const isSwitched = (normType === 'Switched');

    const devices = this.inventory.devices || [];
    const initDevId = initialData?.deviceId ? String(initialData.deviceId) : (devices[0]?.id || '');
    const initDelay = initialData?.delay !== undefined ? initialData.delay :
                      (initialData?.Delay !== undefined ? initialData.Delay :
                      (initialData?.Duration !== undefined ? initialData.Duration :
                      (initialData?.delayMs !== undefined ? initialData.delayMs : (isDelay ? 1000 : 0))));

    row.innerHTML = `
      <div style="display: flex; flex-wrap: wrap; align-items: center; gap: 6px; flex: 1;">
        <select class="form-control step-type-select" style="width: 100px; font-size: 12px; padding: 4px 6px;">
          <option value="Command" ${(!isDelay && !isSwitched) ? 'selected' : ''}>Command</option>
          <option value="Delay" ${isDelay ? 'selected' : ''}>Delay</option>
          <option value="Switched" ${isSwitched ? 'selected' : ''}>Switched</option>
        </select>
        <select class="form-control step-dev-select" style="max-width: 130px; font-size: 12px; padding: 4px 6px; display: ${(!isDelay && !isSwitched) ? 'block' : 'none'};">
          ${devices.map(d => `<option value="${d.id}" ${String(d.id) === initDevId ? 'selected' : ''}>${d.name || d.model || d.id}</option>`).join('')}
        </select>
        <span class="step-dev-icon" style="display: ${(!isDelay && !isSwitched) ? 'inline-flex' : 'none'}; align-items: center;"></span>
        <select class="form-control step-cmd-select" style="flex: 1; min-width: 120px; font-size: 12px; padding: 4px 6px; display: ${(!isDelay && !isSwitched) ? 'block' : 'none'};"></select>
        <div class="step-delay-wrap" style="display: ${isDelay ? 'inline-flex' : 'none'}; align-items: center; gap: 4px;">
          <span class="muted mini" style="font-size: 11px;">Wait:</span>
          <input type="number" class="form-control step-delay-input" min="50" max="60000" step="50" value="${initDelay > 0 ? initDelay : 1000}" style="width: 75px; font-size: 12px; padding: 4px 6px;">
          <span class="muted mini" style="font-size: 11px;">ms</span>
        </div>
        <div class="step-switched-wrap" style="display: ${isSwitched ? 'inline-flex' : 'none'}; align-items: center;">
          <span class="badge subtle mini" style="font-size: 11px; color: var(--accent);">[Activity Switched - Screen Updates Early]</span>
        </div>
      </div>
      <div style="display: flex; gap: 3px; align-items: center;">
        <button type="button" class="btn btn-xs btn-ghost btn-step-up" title="Move up">↑</button>
        <button type="button" class="btn btn-xs btn-ghost btn-step-down" title="Move down">↓</button>
        <button type="button" class="btn btn-xs btn-danger btn-step-del" title="Delete">✕</button>
      </div>
    `;

    const typeSel = row.querySelector('.step-type-select');
    const devSel = row.querySelector('.step-dev-select');
    const cmdSel = row.querySelector('.step-cmd-select');
    const devIconSpan = row.querySelector('.step-dev-icon');
    const delayWrap = row.querySelector('.step-delay-wrap');
    const delayInp = row.querySelector('.step-delay-input');
    const switchedWrap = row.querySelector('.step-switched-wrap');

    const updateCmdsForDev = () => {
      const selectedId = devSel.value;
      const curD = devices.find(x => String(x.id) === String(selectedId));
      if (devIconSpan) {
        devIconSpan.innerHTML = curD ? getDeviceIconSvg(curD.type, curD.manufacturer, curD.model, curD.icon || curD.Icon || '', 14) : '';
      }
      const cmds = this.deviceCommandsMap[selectedId] || [];
      cmdSel.innerHTML = '';
      if (!cmds.length) {
        cmdSel.innerHTML = '<option value="">(No commands)</option>';
      } else {
        cmds.forEach(c => {
          const cName = c.name || c;
          const opt = document.createElement('option');
          opt.value = cName;
          opt.textContent = cName;
          if (cName === initCmd) opt.selected = true;
          cmdSel.appendChild(opt);
        });
      }
      if (initCmd && !Array.from(cmdSel.options).some(o => o.value === initCmd)) {
        const customOpt = document.createElement('option');
        customOpt.value = initCmd;
        customOpt.textContent = initCmd;
        customOpt.selected = true;
        cmdSel.prepend(customOpt);
      }
    };

    typeSel.addEventListener('change', () => {
      const val = typeSel.value;
      const isD = (val === 'Delay');
      const isS = (val === 'Switched');
      devSel.style.display = (!isD && !isS) ? 'block' : 'none';
      if (devIconSpan) devIconSpan.style.display = (!isD && !isS) ? 'inline-flex' : 'none';
      cmdSel.style.display = (!isD && !isS) ? 'block' : 'none';
      delayWrap.style.display = isD ? 'inline-flex' : 'none';
      switchedWrap.style.display = isS ? 'inline-flex' : 'none';
      if (isD && delayInp && parseInt(delayInp.value, 10) <= 0) {
        delayInp.value = '1000';
      }
    });

    devSel.addEventListener('change', () => {
      updateCmdsForDev();
    });

    updateCmdsForDev();

    row.querySelector('.btn-step-up').addEventListener('click', () => {
      if (row.previousElementSibling) cont.insertBefore(row, row.previousElementSibling);
    });
    row.querySelector('.btn-step-down').addEventListener('click', () => {
      if (row.nextElementSibling) cont.insertBefore(row.nextElementSibling, row);
    });
    row.querySelector('.btn-step-del').addEventListener('click', () => {
      row.remove();
      if (!cont.children.length) cont.innerHTML = '<div class="muted mini" style="padding: 8px;">No steps configured.</div>';
    });

    cont.appendChild(row);
  }

  serializeSequence(containerId) {
    const cont = document.getElementById(containerId);
    if (!cont) return [];
    const steps = [];
    cont.querySelectorAll('.queue-row').forEach(row => {
      const typeSel = row.querySelector('.step-type-select');
      const devSel = row.querySelector('.step-dev-select');
      const cmdSel = row.querySelector('.step-cmd-select');
      const delayInp = row.querySelector('.step-delay-input');

      const val = typeSel ? typeSel.value : 'Command';
      if (val === 'Switched') {
        steps.push({
          type: 'Switched',
          deviceId: '0',
          command: 'Switched',
          delay: 0
        });
      } else if (val === 'Delay') {
        const delay = parseInt(delayInp?.value, 10) || 1000;
        steps.push({
          type: 'Delay',
          deviceId: '0',
          command: '',
          delay: delay > 0 ? delay : 1000
        });
      } else {
        const did = devSel?.value || '';
        const cmd = cmdSel?.value || '';
        if (did && cmd) {
          steps.push({
            type: 'Command',
            deviceId: String(did),
            command: String(cmd),
            delay: 0
          });
        }
      }
    });
    return steps;
  }

  serializeSequenceToString(containerId) {
    const steps = this.serializeSequence(containerId);
    return steps.map(s => {
      if (s.type === 'Switched') return 'Switched|0|Switched|0';
      if (s.type === 'Delay') return `Delay|||${s.delay}`;
      return `Command|${s.deviceId}|${s.command}|0`;
    }).join('\n');
  }

  async testLiveSequence(containerId) {
    const steps = this.serializeSequence(containerId);
    if (!steps.length) return alert('No steps to test.');
    try {
      await this.client.testActivitySequence(steps);
      alert('Sequence test sent to hub.');
    } catch (err) {
      alert('Test failed: ' + err.message);
    }
  }

  async saveCurrentActivity() {
    const id = document.getElementById('actEditId')?.value;
    const name = document.getElementById('actEditName')?.value.trim();
    const type = document.getElementById('actEditType')?.value;
    const order = Number(document.getElementById('actEditOrder')?.value) || 1;
    const statusEl = document.getElementById('actEditorStatus');

    if (!name) return alert('Activity name is required.');

    const listWrap = document.getElementById('actDevOrderedList');
    const deviceIds = listWrap
      ? Array.from(listWrap.querySelectorAll('.act-dev-row')).map(r => r.dataset.devId).filter(Boolean)
      : [];

    const startSeq = this.serializeSequence('actStartStepsList');
    const stopSeq = this.serializeSequence('actStopStepsList');
    const startSteps = this.serializeSequenceToString('actStartStepsList');
    const stopSteps = this.serializeSequenceToString('actStopStepsList');

    const icon = document.getElementById('actEditIcon')?.value || 'generic.png';

    const actObj = {
      id: id || '',
      name,
      type: type || 'Custom',
      icon,
      order: order,
      deviceIds,
      startSequence: startSeq,
      stopSequence: stopSeq,
      startSteps,
      stopSteps
    };

    try {
      if (statusEl) statusEl.textContent = 'Saving activity...';
      await this.client.saveActivity(actObj);
      if (statusEl) statusEl.textContent = 'Saved successfully!';
      document.getElementById('actEditorBox').style.display = 'none';
      await this.refreshData();
      await this.loadActivitiesView();
    } catch (err) {
      if (statusEl) statusEl.textContent = 'Error: ' + err.message;
    }
  }

  async deleteCurrentActivity() {
    const id = document.getElementById('actEditId')?.value;
    if (!id || String(id) === '-1') return alert('Cannot delete this activity.');
    if (!await showConfirm('Are you sure you want to delete this activity?', { title: 'Delete Activity', okText: 'Delete' })) return;
    try {
      await this.client.deleteActivity(id);
      document.getElementById('actEditorBox').style.display = 'none';
      await this.refreshData();
      await this.loadActivitiesView();
    } catch (err) {
      alert('Delete failed: ' + err.message);
    }
  }

  simulateActivityTransition() {
    const fromId = document.getElementById('previewFromAct')?.value;
    const toId = document.getElementById('previewToAct')?.value;
    const resultsBox = document.getElementById('previewResultsBox');
    const summaryEl = document.getElementById('previewSummary');
    const timelineEl = document.getElementById('previewTimeline');

    if (!resultsBox || !summaryEl || !timelineEl) return;

    const fromAct = (this.activitiesData.activities || []).find(a => String(a.id) === String(fromId));
    const toAct = (this.activitiesData.activities || []).find(a => String(a.id) === String(toId));
    const allDevs = this.inventory.devices || [];

    const getActDevIds = (act) => {
      if (!act) return [];
      const set = new Set();
      if (Array.isArray(act.deviceIds)) {
        act.deviceIds.forEach(id => set.add(String(id)));
      }
      if (Array.isArray(act.participatingDeviceIds)) {
        act.participatingDeviceIds.forEach(id => set.add(String(id)));
      }
      const seq = [
        ...(act.startSteps || act.startSequence || []),
        ...(act.stopSteps || act.stopSequence || [])
      ];
      seq.forEach(s => {
        if (s.deviceId && String(s.deviceId) !== '0') set.add(String(s.deviceId));
      });
      return Array.from(set);
    };

    const fromDevIds = getActDevIds(fromAct);
    const toDevIds = getActDevIds(toAct);

    // Departing devices: in fromAct, not in toAct, and NOT is_power_always_on
    const departingDevs = fromDevIds
      .filter(id => !toDevIds.includes(id))
      .map(id => allDevs.find(d => String(d.id) === String(id)))
      .filter(d => d && !d.is_power_always_on && !d.isPowerAlwaysOn);

    // Entering devices: in toAct, not in fromAct
    const enteringDevs = toDevIds
      .filter(id => !fromDevIds.includes(id))
      .map(id => allDevs.find(d => String(d.id) === String(id)))
      .filter(Boolean);

    const steps = [];

    const fromStop = fromAct?.stopSteps || fromAct?.stopSequence || [];
    const stopSwitchedIdx = fromStop.findIndex(s => {
      const t = (s.type || '').toLowerCase();
      const c = (s.command || s.Command || s.CommandName || '').toLowerCase();
      return t === 'switched' || t === 'activityswitched' || c === 'switched';
    });

    const departingPhases = [];
    departingDevs.forEach(dev => {
      const pOffSeq = dev.powerOffSequence || dev.power_off_sequence || dev.PowerOffActions || [];
      if (Array.isArray(pOffSeq) && pOffSeq.length > 0) {
        pOffSeq.forEach(s => departingPhases.push({ ...s, deviceId: s.deviceId || dev.id, phase: `Power Off ${dev.name || dev.id}` }));
      } else {
        const cmds = dev.commands || this.deviceCommandsMap[dev.id] || [];
        const pwrCmd = cmds.find(c => {
          const n = (c.name || c).toLowerCase();
          return n === 'poweroff' || n === 'powertoggle' || n === 'power';
        });
        if (pwrCmd) {
          departingPhases.push({
            type: 'Command',
            deviceId: dev.id,
            command: pwrCmd.name || pwrCmd,
            phase: `Power Off ${dev.name || dev.id}`
          });
        }
      }
    });

    const enteringPhases = [];
    enteringDevs.forEach(dev => {
      const pOnSeq = dev.powerOnSequence || dev.power_on_sequence || dev.PowerOnActions || [];
      if (Array.isArray(pOnSeq) && pOnSeq.length > 0) {
        pOnSeq.forEach(s => enteringPhases.push({ ...s, deviceId: s.deviceId || dev.id, phase: `Power On ${dev.name || dev.id}` }));
      } else {
        const cmds = dev.commands || this.deviceCommandsMap[dev.id] || [];
        const pwrCmd = cmds.find(c => {
          const n = (c.name || c).toLowerCase();
          return n === 'poweron' || n === 'powertoggle' || n === 'power';
        });
        if (pwrCmd) {
          enteringPhases.push({
            type: 'Command',
            deviceId: dev.id,
            command: pwrCmd.name || pwrCmd,
            phase: `Power On ${dev.name || dev.id}`
          });
        }
      }
      const pwrDelay = dev.power_on_delay !== undefined ? dev.power_on_delay : (dev.powerOnDelay || 1500);
      if (pwrDelay > 0) {
        enteringPhases.push({
          type: 'Delay',
          deviceId: dev.id,
          delay: pwrDelay,
          phase: `Warmup ${dev.name || dev.id}`
        });
      }
    });

    const toStart = toAct?.startSteps || toAct?.startSequence || [];
    const targetStartPhases = [];
    toStart.forEach(s => targetStartPhases.push({ ...s, phase: 'Enter ' + (toAct ? toAct.name : 'Target') }));

    if (stopSwitchedIdx >= 0 && toId !== '-1') {
      fromStop.slice(0, stopSwitchedIdx).forEach(s => steps.push({ ...s, phase: 'Exit ' + (fromAct ? fromAct.name : 'Current') }));
      departingPhases.forEach(s => steps.push(s));
      enteringPhases.forEach(s => steps.push(s));
      targetStartPhases.forEach(s => steps.push(s));
      fromStop.slice(stopSwitchedIdx + 1).forEach(s => steps.push({ ...s, phase: 'Exit tail ' + (fromAct ? fromAct.name : 'Current') }));
    } else {
      fromStop.forEach(s => steps.push({ ...s, phase: 'Exit ' + (fromAct ? fromAct.name : 'Current') }));
      departingPhases.forEach(s => steps.push(s));
      enteringPhases.forEach(s => steps.push(s));
      targetStartPhases.forEach(s => steps.push(s));
    }

    resultsBox.style.display = 'block';

    let totalDurationMs = 0;
    let switchedAtMs = null;
    let lastDeviceId = null;

    const renderedTimeline = steps.map((st, i) => {
      const normType = (st.type === 'IRCommand') ? 'Command' : (st.type || 'Command');
      const isDelay = normType === 'Delay';
      const isSwitched = normType === 'Switched';
      const delayVal = isDelay ? (st.delay !== undefined ? st.delay : (st.delayMs || 1000)) : 0;
      const dev = allDevs.find(d => String(d.id) === String(st.deviceId));
      const devName = dev ? (dev.name || dev.model || `Device ${st.deviceId}`) : `Device ${st.deviceId}`;

      if (isSwitched) {
        if (switchedAtMs === null) switchedAtMs = totalDurationMs;
        return `
          <div class="queue-row" style="display:flex;justify-content:space-between;align-items:center;padding:8px;margin-bottom:4px;background:rgba(34,197,94,0.1);border-left:3px solid var(--accent);">
            <span style="font-size:12px;font-weight:600;color:var(--accent);">${i + 1}. [${st.phase || 'Switchpoint'}] ★ Elite Remote UI updates to "${toAct ? toAct.name : 'Target'}" (${totalDurationMs} ms)</span>
            <span class="badge ok">Switched</span>
          </div>
        `;
      }

      if (isDelay) {
        totalDurationMs += delayVal;
        return `
          <div class="queue-row" style="display:flex;justify-content:space-between;align-items:center;padding:6px 8px;margin-bottom:4px;">
            <span style="font-size:12px;">${i + 1}. [${st.phase}] Wait ${delayVal} ms (T: ${totalDurationMs} ms)</span>
            <span class="badge warn">Delay</span>
          </div>
        `;
      }

      // Command timing: add inter-device delay if switching target device
      if (dev && lastDeviceId && String(lastDeviceId) !== String(dev.id)) {
        const idd = dev.inter_device_delay !== undefined ? dev.inter_device_delay : (dev.interDeviceDelay || 100);
        totalDurationMs += idd;
      }
      lastDeviceId = st.deviceId;

      return `
        <div class="queue-row" style="display:flex;justify-content:space-between;align-items:center;padding:6px 8px;margin-bottom:4px;">
          <span style="font-size:12px;">${i + 1}. [${st.phase}] Send ${st.command} to ${devName} (T: ${totalDurationMs} ms)</span>
          <span class="badge ok">Command</span>
        </div>
      `;
    }).join('') || '<div class="muted mini">No operations required.</div>';

    const switchNote = (switchedAtMs !== null) ? ` • UI Switched after ${switchedAtMs} ms` : '';
    summaryEl.textContent = `Transition from ${fromAct ? fromAct.name : 'PowerOff'} to ${toAct ? toAct.name : 'PowerOff'}: ${steps.length} steps • Estimated duration: ${(totalDurationMs / 1000).toFixed(1)}s${switchNote}`;
    timelineEl.innerHTML = renderedTimeline;
  }

  /* -------------------------------------------------------------
   * 4. Control View (Device Remotes)
   * ------------------------------------------------------------- */
  renderControlView() {
    const cont = document.getElementById('controlDeviceWorkspace');
    if (!cont) return;

    const devs = this.inventory.devices || [];
    if (!devs.length) {
      cont.innerHTML = '<div class="card"><div class="muted">No devices stored. Go to IR Setup to add one.</div></div>';
      return;
    }

    if (!this.selectedDeviceId || !devs.some(d => String(d.id) === String(this.selectedDeviceId))) {
      this.selectedDeviceId = devs[0].id;
    }

    const curDev = devs.find(d => String(d.id) === String(this.selectedDeviceId));
    this.remoteEngine.render(cont, {
      scope: 'device',
      target: curDev,
      iconSvg: getDeviceIconSvg(curDev.type, curDev.manufacturer, curDev.model, curDev.icon || curDev.Icon || '', 18),
      availableDevices: [curDev],
      selectableDevices: devs,
      onSelectDevice: (devId) => {
        this.selectedDeviceId = devId;
        this.renderControlView();
      },
      commandsMap: this.deviceCommandsMap,
      onSendCommand: async (devId, cmd) => {
        await this.client.sendCommand(devId, cmd);
      }
    });
  }

  /* -------------------------------------------------------------
   * 5. IR Setup View
   * ------------------------------------------------------------- */
  renderIrView(preserveDeviceId = null) {
    const devs = this.inventory.devices || [];
    const targetId = preserveDeviceId || this.selectedIrDeviceId || document.getElementById('manageDeviceSelect')?.value;
    ['wizardDevice', 'verifyDevice', 'irdbDevice', 'manageDeviceSelect', 'harmonyTargetDevSelect'].forEach(id => {
      const el = document.getElementById(id);
      if (el) {
        const curVal = targetId || el.value;
        el.innerHTML = devs.map(d => `<option value="${d.id}">${d.name || d.id}</option>`).join('');
        if (curVal && devs.some(d => String(d.id) === String(curVal))) {
          el.value = String(curVal);
        }
      }
    });
    this.updateStudioDeviceView();

    if (window.innerWidth <= 768) {
      const activeBtn = document.querySelector('#irStudioMobileNav .segmented-btn.active');
      const tab = activeBtn ? activeBtn.getAttribute('data-ir-tab') : 'cmds';
      const colLeft = document.getElementById('irStudioColLeft');
      const colRight = document.getElementById('irStudioColRight');
      const devProp = document.getElementById('devicePropertiesBox');
      const cmdsWrap = document.getElementById('curDevCmdsWrap');
      if (tab === 'cmds') {
        if (colLeft) colLeft.style.display = 'flex';
        if (colRight) colRight.style.display = 'none';
        if (devProp) devProp.style.display = 'none';
        if (cmdsWrap) cmdsWrap.style.display = 'block';
      } else if (tab === 'add') {
        if (colLeft) colLeft.style.display = 'none';
        if (colRight) colRight.style.display = 'flex';
      } else if (tab === 'settings') {
        if (colLeft) colLeft.style.display = 'flex';
        if (colRight) colRight.style.display = 'none';
        if (devProp) devProp.style.display = 'block';
        if (cmdsWrap) cmdsWrap.style.display = 'none';
      }
    }
  }

  updateStudioDeviceView() {
    const sel = document.getElementById('manageDeviceSelect');
    if (!sel) return;
    const devs = this.inventory.devices || [];
    let dev = null;
    if (this.selectedIrDeviceId && devs.some(d => String(d.id) === String(this.selectedIrDeviceId))) {
      dev = devs.find(d => String(d.id) === String(this.selectedIrDeviceId));
    } else if (sel.value && devs.some(d => String(d.id) === String(sel.value))) {
      dev = devs.find(d => String(d.id) === String(sel.value));
    } else {
      dev = devs[0];
    }
    if (!dev) {
      const nameEl = document.getElementById('curDevName');
      if (nameEl) nameEl.textContent = 'No Devices Found';
      return;
    }
    this.selectedIrDeviceId = String(dev.id);
    this.currentIrDeviceId = String(dev.id);
    if (sel.value !== String(dev.id)) sel.value = String(dev.id);

    // Sync other hidden dropdowns & target selector
    ['wizardDevice', 'verifyDevice', 'irdbDevice', 'harmonyTargetDevSelect'].forEach(id => {
      const el = document.getElementById(id);
      if (el) el.value = String(dev.id);
    });

    const nameEl = document.getElementById('curDevName');
    const detailsEl = document.getElementById('curDevDetails');
    const countEl = document.getElementById('curDevCmdCount');
    const portEl = document.getElementById('curDevPortSelect');
    const mqttTopicInput = document.getElementById('devMqttTopic');
    if (nameEl) nameEl.textContent = dev.name || 'Device ' + dev.id;
    if (detailsEl) detailsEl.textContent = `Type: ${dev.type || 'Appliance'} • Brand: ${dev.manufacturer || 'Unknown'} • Model: ${dev.model || 'Unknown'}`;
    if (countEl) countEl.textContent = (dev.commands || []).length;
    if (portEl) {
      const p = (dev.controlPort !== undefined) ? dev.controlPort : ((dev.control_port !== undefined) ? dev.control_port : 7);
      portEl.value = String(p);
    }

    const isIr = (dev.type === 'IR' || !dev.type || dev.type === 'Television' || dev.type === 'Amplifier' || dev.type === 'Media Player' || dev.type === 'HomeAppliance');
    const isBt = (dev.type === 'BT' || dev.type === 'Bluetooth' || dev.isBt || Number(dev.transport) === 32);
    const isMqtt = (dev.type === 'MQTT' || dev.mqtt_enabled);

    const blasterBox = document.getElementById('curDevBlasterBox');
    if (blasterBox) blasterBox.style.display = isIr ? 'flex' : 'none';

    const btBox = document.getElementById('curDevBtBox');
    if (btBox) {
      btBox.style.display = isBt ? 'block' : 'none';
      const btBadge = document.getElementById('curDevBtStatusBadge');
      const btHostAddrText = document.getElementById('curDevBtHostAddrText');
      const connBtn = document.getElementById('btnCurDevBtConnect');
      const disconnBtn = document.getElementById('btnCurDevBtDisconnect');
      const unpairBtn = document.getElementById('btnCurDevBtUnpair');
      const knownHostRow = document.getElementById('curDevBtKnownHostRow');
      const knownHostSel = document.getElementById('curDevBtKnownHostSelect');
      const btAutoRec = document.getElementById('curDevBtAutoReconnect');
      const btDur = document.getElementById('curDevBtDuration');
      const btDly = document.getElementById('curDevBtDelay');

      const hostAddr = dev.btHost || dev.bt_host || dev.BTAddress || dev.host_addr || '';
      const hasHost = Boolean(hostAddr && hostAddr !== '00:00:00:00:00:00');

      if (btHostAddrText) {
        btHostAddrText.textContent = hasHost ? hostAddr : 'None (Unpaired)';
      }
      if (btBadge) {
        btBadge.textContent = hasHost ? 'Paired' : 'Unpaired';
        btBadge.className = hasHost ? 'badge badge-success mini' : 'badge subtle mini';
      }
      if (connBtn) connBtn.style.display = hasHost ? 'inline-block' : 'none';
      if (disconnBtn) disconnBtn.style.display = hasHost ? 'inline-block' : 'none';
      if (unpairBtn) unpairBtn.style.display = hasHost ? 'inline-block' : 'none';

      // Query live status to see if active link is connected
      if (this.client && typeof this.client.getBtStatus === 'function') {
        this.client.getBtStatus().then(st => {
          if (!st) return;
          const hosts = st.hosts || (st.host?.addr ? [st.host] : []);
          const activeHost = st.host || {};
          if (hasHost && btBadge) {
            const isConn = (activeHost.connected && activeHost.addr && activeHost.addr.toLowerCase() === hostAddr.toLowerCase()) ||
                           hosts.some(h => h.connected && h.addr && h.addr.toLowerCase() === hostAddr.toLowerCase());
            if (isConn) {
              btBadge.textContent = 'Connected';
              btBadge.className = 'badge ok mini';
            }
          }
          if (knownHostRow && knownHostSel) {
            if (hosts.length > 0) {
              knownHostRow.style.display = 'flex';
              knownHostSel.innerHTML = '<option value="">-- Link with Known Host --</option>' +
                hosts.map(h => `<option value="${h.addr}">${h.name || h.addr} (${h.addr})</option>`).join('');
            } else {
              knownHostRow.style.display = 'none';
            }
          }
        }).catch(() => {});
      }

      if (btAutoRec) {
        let recVal = -1;
        if (dev.bt_auto_reconnect !== undefined && dev.bt_auto_reconnect !== null) {
          recVal = Number(dev.bt_auto_reconnect);
        } else if (dev.btAutoReconnect !== undefined && dev.btAutoReconnect !== null) {
          recVal = Number(dev.btAutoReconnect);
        }
        btAutoRec.value = String(recVal);
      }
      if (btDur) {
        const dVal = dev.bt_reconnect_duration_sec !== undefined ? dev.bt_reconnect_duration_sec : (dev.btReconnectDurationSec !== undefined ? dev.btReconnectDurationSec : (dev.btReconnectDuration || 0));
        btDur.value = dVal > 0 ? dVal : '';
      }
      if (btDly) {
        const dVal = dev.bt_reconnect_delay_sec !== undefined ? dev.bt_reconnect_delay_sec : (dev.btReconnectDelaySec !== undefined ? dev.btReconnectDelaySec : (dev.btReconnectDelay !== undefined ? dev.btReconnectDelay : -1));
        btDly.value = dVal >= 0 ? dVal : '';
      }
      const btTrigSel = document.getElementById('curDevBtTriggerCmd');
      if (btTrigSel) {
        let html = '<option value="">None (Activity Start Only)</option>';
        const allDevs = this.devices || [];
        allDevs.forEach(d => {
          const cmds = d.commands || [];
          if (cmds.length > 0) {
            html += `<optgroup label="${this.escapeHtml(d.name || d.id || 'Device')}">`;
            cmds.forEach(c => {
              const cName = typeof c === 'string' ? c : (c.name || c.Name || '');
              if (cName) {
                const val = `${d.id}:${cName}`;
                html += `<option value="${this.escapeHtml(val)}">${this.escapeHtml(cName)}</option>`;
              }
            });
            html += `</optgroup>`;
          }
        });
        btTrigSel.innerHTML = html;

        const currentTrigDev = dev.bt_trigger_device_id || dev.btTriggerDeviceId || '';
        const currentTrigCmd = dev.bt_trigger_command_name || dev.btTriggerCommandName || dev.bt_trigger_command || dev.btTriggerCommand || '';
        if (currentTrigDev && currentTrigCmd) {
          btTrigSel.value = `${currentTrigDev}:${currentTrigCmd}`;
        } else if (currentTrigCmd && currentTrigCmd.includes(':')) {
          btTrigSel.value = currentTrigCmd;
        } else {
          btTrigSel.value = '';
        }
      }
    }

    const setInputVal = (id, val, defVal = '') => {
      const el = document.getElementById(id);
      if (el) el.value = (val !== undefined && val !== null) ? val : defVal;
    };
    setInputVal('devInterDeviceDelay', dev.inter_device_delay !== undefined ? dev.inter_device_delay : dev.interDeviceDelay, 100);
    setInputVal('devInterKeyDelay', dev.inter_key_delay !== undefined ? dev.inter_key_delay : dev.interKeyDelay, 100);
    setInputVal('devHoldInterKeyDelay', dev.hold_inter_key_delay !== undefined ? dev.hold_inter_key_delay : dev.holdInterKeyDelay, 200);
    setInputVal('devPowerOnDelay', dev.power_on_delay !== undefined ? dev.power_on_delay : dev.powerOnDelay, 1500);
    const pwrAlwaysCheck = document.getElementById('devIsPowerAlwaysOn');
    if (pwrAlwaysCheck) pwrAlwaysCheck.checked = Boolean(dev.is_power_always_on || dev.isPowerAlwaysOn);

    const editName = document.getElementById('editDevName');
    const editType = document.getElementById('editDevType');
    const editMfg = document.getElementById('editDevMfg');
    const editMdl = document.getElementById('editDevModel');
    const editIcn = document.getElementById('editDevIcon');
    if (editName) editName.value = dev.name || '';
    if (editType) editType.value = isBt ? 'BT' : (isMqtt ? 'MQTT' : 'IR');
    if (editMfg) editMfg.value = dev.manufacturer || '';
    if (editMdl) editMdl.value = dev.model || '';
    if (editIcn) {
      const rawIcn = (dev.icon || '').toLowerCase();
      const iconMap = {
        '1': 'tv', 'television': 'tv', 'tv': 'tv',
        '5': 'receiver', 'amplifier': 'receiver', 'avr': 'receiver', 'receiver': 'receiver',
        '10': 'pc', 'computer': 'pc', 'laptop': 'pc',
        '16': 'cable', 'satellite': 'cable', 'stb': 'cable',
        '33': 'streamer', 'media': 'streamer',
        '63': 'light', 'lights': 'light', 'bulb': 'light',
        'soundbar': 'soundbar', 'speaker': 'soundbar',
        'playstation': 'ps', 'ps': 'ps', 'ps4': 'ps', 'ps5': 'ps',
        'xbox': 'xbox', 'switch': 'switch', 'nintendo': 'switch',
        'shield': 'shield', 'apple': 'apple', 'firetv': 'firetv', 'roku': 'roku',
        'bluray': 'bluray', 'dvd': 'bluray', 'projector': 'projector',
        'sonos': 'sonos', 'heos': 'heos', 'plug': 'plug', 'blinds': 'blinds',
        'thermostat': 'thermostat', 'fan': 'fan', 'generic': 'generic'
      };
      editIcn.value = iconMap[rawIcn] || rawIcn || 'tv';
      if (!editIcn.value) editIcn.value = 'generic';
      this.updateBadgeForSelect(editIcn, 'devIconPreviewBadge');
    }
    if (mqttTopicInput) {
      mqttTopicInput.value = dev.mqttTopic || dev.MQTTTopic || `{base_topic}/button/${(dev.name || 'dev').toLowerCase().replace(/\s+/g, '_')}/{command}`;
    }

    const mqttPulseInput = document.getElementById('devMqttPulse');
    if (mqttPulseInput) {
      mqttPulseInput.value = dev.mqttPulseMs || dev.mqttPulse || dev.MQTTPulseMs || dev.mqtt_pulse_ms || 1000;
    }

    const mqttEnabledCheck = document.getElementById('devMqttEnabled');
    const mqttBadge = document.getElementById('devMqttEnabledBadge');
    const isMqttEn = Boolean(dev.mqttEnabled || dev.mqtt_enabled || dev.MQTTEnabled);
    if (mqttEnabledCheck) {
      mqttEnabledCheck.checked = isMqttEn;
    }
    if (mqttBadge) {
      mqttBadge.className = isMqttEn ? 'badge badge-success mini' : 'badge badge-secondary mini';
      mqttBadge.textContent = isMqttEn ? 'Enabled' : 'Disabled';
    }

    this.renderStudioCommandsList(dev);
  }

  renderStudioCommandsList(dev) {
    const listEl = document.getElementById('curDevCmdsList');
    if (!listEl) return;
    const filter = (document.getElementById('curDevCmdFilter')?.value || '').toLowerCase().trim();
    const cmds = (dev.commands || []).filter(c => !filter || c.name.toLowerCase().includes(filter));

    if (cmds.length === 0) {
      listEl.innerHTML = `<div class="muted mini" style="padding: 12px; text-align: center;">${(dev.commands || []).length === 0 ? 'No commands saved yet. Add one on the right.' : 'No commands match filter.'}</div>`;
      return;
    }

    const isBt = (dev.type === 'BT' || dev.type === 'Bluetooth' || dev.isBt || Number(dev.transport) === 32);

    listEl.innerHTML = cmds.map(c => {
      const isLocked = isBt && (c.name === 'Connect' || c.name === 'Disconnect');
      return `
        <div style="display: flex; justify-content: space-between; align-items: center; padding: 6px 10px; background: var(--bg-card); border-radius: var(--radius-sm); margin-bottom: 4px; border: 1px solid var(--border);">
          <div style="display: flex; align-items: center; gap: 6px;">
            <strong style="font-size: 13px; color: var(--text-main);">${c.name}</strong>
            ${isLocked ? `<span class="badge subtle mini" style="font-size: 10px;">Locked</span>` : ''}
          </div>
          <div style="display: flex; gap: 6px; align-items: center;">
            <button type="button" class="btn btn-xs btn-secondary btn-studio-test" data-dev="${dev.id}" data-cmd="${c.name}" title="Send command">▶ Test</button>
            <button type="button" class="btn btn-xs btn-ghost btn-studio-edit" data-dev="${dev.id}" data-cmd="${c.name}" title="Edit command">✎ Edit</button>
            ${isLocked ? '' : `<button type="button" class="btn btn-xs btn-ghost btn-studio-del" data-dev="${dev.id}" data-cmd="${c.name}" style="color: var(--danger);" title="Delete command">✕</button>`}
          </div>
        </div>
      `;
    }).join('');

    listEl.querySelectorAll('.btn-studio-test').forEach(btn => {
      btn.addEventListener('click', async (e) => {
        e.stopPropagation();
        const dId = btn.getAttribute('data-dev');
        const cmd = btn.getAttribute('data-cmd');
        btn.textContent = '...';
        try {
          await this.client.sendCommand(dId, cmd);
          btn.textContent = '✓ Sent';
          setTimeout(() => { btn.textContent = '▶ Test'; }, 1200);
        } catch (err) {
          alert('Failed to send: ' + err.message);
          btn.textContent = '▶ Test';
        }
      });
    });

    listEl.querySelectorAll('.btn-studio-edit').forEach(btn => {
      btn.addEventListener('click', (e) => {
        e.stopPropagation();
        const cmdName = btn.getAttribute('data-cmd');
        const c = (dev.commands || []).find(x => x.name === cmdName);
        if (c) this.openCommandEditModal(dev, c);
      });
    });

    listEl.querySelectorAll('.btn-studio-del').forEach(btn => {
      btn.addEventListener('click', async (e) => {
        e.stopPropagation();
        const dId = btn.getAttribute('data-dev');
        const cmd = btn.getAttribute('data-cmd');
        if (isBt && (cmd === 'Connect' || cmd === 'Disconnect')) {
          alert('Connect and Disconnect commands cannot be deleted for Bluetooth devices.');
          return;
        }
        if (!await showConfirm(`Delete command "${cmd}" from this device?`, { title: 'Delete Command', okText: 'Delete' })) return;
        try {
          await this.client.deleteCommand(dId, cmd);
          await this.refreshData();
          this.renderIrView();
        } catch (err) {
          alert('Delete failed: ' + err.message);
        }
      });
    });
  }

  openCommandEditModal(dev, c) {
    const modal = document.getElementById('cmdEditModal');
    if (!modal) return;
    document.getElementById('editCmdDevId').value = String(dev.id);
    document.getElementById('editCmdOldName').value = c.name;
    document.getElementById('editCmdName').value = c.name;
    const rawVal = c.raw || c.keyCode || '';
    document.getElementById('editCmdPayload').value = rawVal;

    const isBt = (dev.type === 'BT' || dev.type === 'Bluetooth' || dev.isBt || Number(dev.transport) === 32);
    const isMqtt = (dev.type === 'MQTT' || dev.mqtt_enabled);
    const isIr = !isBt && !isMqtt;

    const typeWrap = document.getElementById('editCmdTypeWrap');
    const btHint = document.getElementById('editCmdBtHint');
    const irOpts = document.getElementById('editCmdIrOptions');
    const payloadWrap = document.getElementById('editCmdPayloadWrap');

    if (typeWrap) typeWrap.style.display = isIr ? 'block' : 'none';
    if (btHint) btHint.style.display = isBt ? 'block' : 'none';
    if (irOpts) irOpts.style.display = isIr ? 'grid' : 'none';
    if (payloadWrap) payloadWrap.style.display = isMqtt ? 'none' : 'block';

    let modeVal = 'raw';
    if (isBt) {
      modeVal = 'bt';
    } else if (isMqtt) {
      modeVal = 'mqtt';
    } else {
      modeVal = (rawVal.startsWith('G:') || c.type === 'keycode') ? 'keycode' : 'raw';
    }
    const typeSelect = document.getElementById('editCmdType');
    if (typeSelect && isIr) {
      typeSelect.value = modeVal;
    }

    document.getElementById('editCmdRepeats').value = c.repeats || 3;
    document.getElementById('editCmdPort').value = String(c.port || 0);

    const stat = document.getElementById('cmdEditStatus');
    if (stat) stat.textContent = '';

    modal.dataset.deviceType = isBt ? 'BT' : (isMqtt ? 'MQTT' : 'IR');
    modal.style.display = 'flex';
  }

  async submitNewDeviceProfile() {
    const name = document.getElementById('newDeviceName')?.value.trim();
    const type = document.getElementById('newDeviceType')?.value || 'IR';
    const manufacturer = document.getElementById('newDeviceManufacturer')?.value.trim();
    const model = document.getElementById('newDeviceModel')?.value.trim();
    const status = document.getElementById('profileStatus');

    if (!name || !manufacturer || !model) return alert('Name, manufacturer, and model are required.');
    try {
      if (status) status.textContent = 'Saving new device...';
      const icon = document.getElementById('newDeviceIcon')?.value || 'tv';
      const devPayload = {
        name,
        type,
        manufacturer,
        model,
        icon,
        transport: type === 'BT' ? 32 : 1
      };
      if (type === 'MQTT') {
        devPayload.mqttTopic = `{base_topic}/button/${name.toLowerCase().replace(/\\s+/g, '_')}/{command}`;
        devPayload.mqttEnabled = 1;
        devPayload.mqttPulseMs = 1000;
      }
      const res = await this.client.saveNewDevice(devPayload);
      const newDevId = res?.deviceId || res?.id;
      if (newDevId) {
        if (type === 'BT') {
          const btButtons = [
            { name: 'Power', raw: 'HID:01:0081' },
            { name: 'Select', raw: 'HID:07:0028' },
            { name: 'DirectionUp', raw: 'HID:07:0052' },
            { name: 'DirectionDown', raw: 'HID:07:0051' },
            { name: 'DirectionLeft', raw: 'HID:07:0050' },
            { name: 'DirectionRight', raw: 'HID:07:004F' },
            { name: 'Back', raw: 'HID:07:0029' },
            { name: 'Home', raw: 'HID:07:004A' },
            { name: 'Menu', raw: 'HID:07:0076' },
            { name: 'VolumeUp', raw: 'HID:0C:00E9' },
            { name: 'VolumeDown', raw: 'HID:0C:00EA' },
            { name: 'Mute', raw: 'HID:0C:00E2' },
            { name: 'Play', raw: 'HID:0C:00B0' },
            { name: 'Pause', raw: 'HID:0C:00B1' },
            { name: 'Connect', raw: 'HID:00:0000' },
            { name: 'Disconnect', raw: 'HID:00:0000' }
          ];
          for (const b of btButtons) {
            await this.client.saveLearnedCommand({ deviceId: newDevId, name: b.name, raw: b.raw, mode: 'raw' }).catch(() => {});
          }
        } else if (type === 'MQTT') {
          const mqttButtons = [
            'PowerToggle', 'VolumeUp', 'VolumeDown', 'Mute', 'Play', 'Pause',
            'DirectionUp', 'DirectionDown', 'DirectionLeft', 'DirectionRight', 'Select'
          ];
          for (const m of mqttButtons) {
            await this.client.saveLearnedCommand({ deviceId: newDevId, name: m, raw: `G:MQTT:${m}`, mode: 'raw' }).catch(() => {});
          }
        }
      }
      if (status) status.innerHTML = '<span style="color:var(--success)">✓ Device created successfully!</span>';
      await this.refreshData();
      if (newDevId) this.selectedIrDeviceId = String(newDevId);
      this.renderIrView();
    } catch (err) {
      if (status) status.innerHTML = `<span style="color:var(--danger)">Error: ${err.message}</span>`;
    }
  }

  openPowerSequenceModal() {
    const devId = this.selectedIrDeviceId || document.getElementById('manageDeviceSelect')?.value;
    const dev = (this.inventory.devices || []).find(d => String(d.id) === String(devId));
    if (!dev) return alert('Select a device first.');
    const modal = document.getElementById('powerSeqModal');
    if (!modal) return;
    const devNameEl = document.getElementById('powerSeqDevName');
    if (devNameEl) devNameEl.textContent = dev.name || dev.model || dev.id;

    const pOn = dev.powerOnSequence || dev.power_on_sequence || dev.PowerOnActions || [];
    const pOff = dev.powerOffSequence || dev.power_off_sequence || dev.PowerOffActions || [];
    this.renderSequenceList('powerOnStepsList', Array.isArray(pOn) ? pOn : []);
    this.renderSequenceList('powerOffStepsList', Array.isArray(pOff) ? pOff : []);

    const stat = document.getElementById('powerSeqStatus');
    if (stat) stat.textContent = '';
    modal.style.display = 'flex';
  }

  async saveDevicePowerSequences() {
    const devId = this.selectedIrDeviceId || document.getElementById('manageDeviceSelect')?.value;
    const dev = (this.inventory.devices || []).find(d => String(d.id) === String(devId));
    if (!dev) return;
    const stat = document.getElementById('powerSeqStatus');
    if (stat) stat.textContent = 'Saving sequences...';
    try {
      const pOn = this.serializeSequence('powerOnStepsList');
      const pOff = this.serializeSequence('powerOffStepsList');
      await this.client.saveDevicePower({
        deviceId: dev.id,
        powerOnSequence: pOn,
        powerOffSequence: pOff
      });
      dev.powerOnSequence = pOn;
      dev.powerOffSequence = pOff;
      if (stat) stat.innerHTML = '<span style="color:var(--success)">Saved!</span>';
      setTimeout(() => {
        const modal = document.getElementById('powerSeqModal');
        if (modal) modal.style.display = 'none';
      }, 1000);
      await this.refreshData();
    } catch (err) {
      if (stat) stat.innerHTML = `<span style="color:var(--danger)">Error: ${err.message}</span>`;
    }
  }

  async searchIrdb() {
    const q = document.getElementById('irdbSearch')?.value.trim();
    const src = document.getElementById('irdbSource')?.value || 'all';
    const status = document.getElementById('irdbStatus');
    const log = document.getElementById('irdbLog');
    const resBox = document.getElementById('irdbResults');

    if (status) status.textContent = 'Searching index...';
    if (log) log.textContent = `Searching for "${q}" in ${src}...`;

    try {
      let entries = [];
      if (src === 'all') {
        const [a, b] = await Promise.all([loadIrdSource('irdb'), loadIrdSource('flipper')]);
        entries = a.concat(b);
      } else {
        entries = await loadIrdSource(src);
      }
      const matches = rankIrdEntries(entries, q);
      if (status) status.textContent = `Found ${matches.length} files.`;

      if (resBox) {
        resBox.style.display = matches.length ? 'block' : 'none';
        resBox.innerHTML = matches.map(m => `
          <button type="button" class="btn btn-xs btn-ghost irdb-pick" data-path="${m.path}" data-source="${m.source}" style="width: 100%; text-align: left; margin-bottom: 4px;">
            <strong>[${sourceLabel(m.source)}]</strong> ${m.path}
          </button>
        `).join('');

        resBox.querySelectorAll('.irdb-pick').forEach(b => {
          b.addEventListener('click', () => {
            const p = b.getAttribute('data-path');
            const s = b.getAttribute('data-source');
            const pathInp = document.getElementById('irdbPath');
            if (pathInp) {
              pathInp.value = p;
              pathInp.dataset.source = s;
            }
            this.fetchSelectedIrdbFile();
          });
        });
      }
    } catch (err) {
      if (status) status.textContent = 'Search failed: ' + err.message;
    }
  }

  async fetchSelectedIrdbFile() {
    const pathInp = document.getElementById('irdbPath');
    const p = pathInp?.value.trim();
    const s = pathInp?.dataset.source || 'flipper';
    const status = document.getElementById('irdbStatus');

    if (!p) return alert('Select or type a file path first.');
    try {
      if (status) status.textContent = 'Loading file...';
      const text = await fetchIrdFile(p, s);
      const rows = parseIrText(text, s, p);
      this.renderIrdbPreview(rows);
    } catch (err) {
      if (status) status.textContent = 'Failed to load file: ' + err.message;
    }
  }

  parsePastIrdb() {
    const paste = document.getElementById('irdbPaste')?.value.trim();
    if (!paste) return alert('Paste code contents first.');
    const rows = parseIrText(paste, 'custom', '');
    this.renderIrdbPreview(rows);
  }

  renderIrdbPreview(rows = []) {
    const previewBox = document.getElementById('irdbPreview');
    const status = document.getElementById('irdbStatus');
    if (!previewBox) return;

    previewBox.style.display = 'block';
    if (!rows.length) {
      previewBox.innerHTML = '<div class="muted mini">No valid commands parsed.</div>';
      return;
    }

    const updateIrdbCount = () => {
      const checked = previewBox.querySelectorAll('.irdb-cmd-cb:checked').length;
      if (status) status.innerHTML = `Parsed ${rows.length} commands (<strong>${checked}</strong> of ${rows.length} selected).`;
    };

    previewBox.innerHTML = `
      <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:8px; padding-bottom:6px; border-bottom:1px solid var(--border);">
        <div style="display:flex; gap:6px;">
          <button type="button" id="btnIrdbSelectAll" class="btn btn-xs btn-secondary" style="padding:2px 8px; font-size:11px;">Select All</button>
          <button type="button" id="btnIrdbSelectNone" class="btn btn-xs btn-ghost" style="padding:2px 8px; font-size:11px;">Select None</button>
        </div>
      </div>
      <div id="irdbCheckboxesList">
        ${rows.map((r, i) => `
          <label style="display: flex; align-items: center; gap: 8px; font-size: 11px; margin-bottom: 4px; cursor: pointer;">
            <input type="checkbox" class="irdb-cmd-cb" data-name="${r.name}" data-keycode="${r.keycode || ''}" data-raw="${r.raw || ''}" checked>
            <span><strong>${r.name}</strong> (${r.meta})</span>
          </label>
        `).join('')}
      </div>
    `;

    document.getElementById('btnIrdbSelectAll')?.addEventListener('click', () => {
      previewBox.querySelectorAll('.irdb-cmd-cb').forEach(cb => { cb.checked = true; });
      updateIrdbCount();
    });
    document.getElementById('btnIrdbSelectNone')?.addEventListener('click', () => {
      previewBox.querySelectorAll('.irdb-cmd-cb').forEach(cb => { cb.checked = false; });
      updateIrdbCount();
    });
    previewBox.querySelectorAll('.irdb-cmd-cb').forEach(cb => {
      cb.addEventListener('change', updateIrdbCount);
    });
    updateIrdbCount();
  }

  async saveSelectedIrdbCommands() {
    const devId = document.getElementById('manageDeviceSelect')?.value || document.getElementById('irdbDevice')?.value;
    const prefix = document.getElementById('irdbPrefix')?.value || '';
    if (!devId) return alert('Select a device first.');

    const targetDev = (this.inventory.devices || []).find(d => String(d.id) === String(devId));
    const targetPort = targetDev ? ((targetDev.controlPort !== undefined) ? targetDev.controlPort : 7) : 7;

    const cmdPayloads = [];
    const lines = [];
    document.querySelectorAll('.irdb-cmd-cb:checked').forEach(cb => {
      const name = prefix + cb.getAttribute('data-name');
      const raw = cb.getAttribute('data-raw');
      const key = cb.getAttribute('data-keycode');
      if (raw) {
        lines.push(`${name}|raw|${raw}`);
        cmdPayloads.push({ name, raw, repeats: 3, port: targetPort });
      } else if (key) {
        lines.push(`${name}|${key}`);
        cmdPayloads.push({ name, raw: key, repeats: 3, port: targetPort });
      }
    });

    if (!cmdPayloads.length) return alert('No commands selected.');
    try {
      let ok = false;
      try {
        const res = await this.client.saveBatchCommands(devId, cmdPayloads);
        if (res && res.ok) ok = true;
      } catch {
        ok = false;
      }
      if (!ok) {
        await this.client.importIrdb({ deviceId: devId, payload: lines.join('\n') });
      }
      alert(`Imported ${cmdPayloads.length} commands to "${targetDev?.name || devId}".`);
      await this.refreshData();
      this.renderIrView(devId);
    } catch (err) {
      alert('Import failed: ' + err.message);
    }
  }

  async captureIrSignal() {
    const status = document.getElementById('wizardLearnStatus');
    const rawBox = document.getElementById('wizardRaw');
    const necBox = document.getElementById('wizardNec');
    const keyBox = document.getElementById('wizardKeycode');

    try {
      if (status) status.textContent = 'Listening for IR signal (press remote button)...';
      const res = await this.client.captureIr();
      if (res && res.raw) {
        if (rawBox) rawBox.value = res.raw;
        if (necBox && res.nec) necBox.value = res.nec;
        if (keyBox && res.keycode) keyBox.value = res.keycode;
        if (status) status.textContent = `Captured signal! Mode: ${res.mode || 'raw'}`;
      } else {
        if (status) status.textContent = 'No signal received or timed out.';
      }
    } catch (err) {
      if (status) status.textContent = 'Capture failed: ' + err.message;
    }
  }

  async testLearnedIrSignal() {
    const raw = document.getElementById('wizardRaw')?.value.trim();
    const nec = document.getElementById('wizardNec')?.value.trim();
    const keycode = document.getElementById('wizardKeycode')?.value.trim();
    const mode = document.getElementById('wizardMode')?.value || 'auto';
    const proto = document.getElementById('wizardProtocol')?.value || '2';
    const devId = document.getElementById('wizardDevice')?.value;
    const status = document.getElementById('wizardLearnStatus');

    try {
      if (status) status.textContent = 'Sending test signal...';
      await this.client.testLearnedIr({ deviceId: devId, raw, nec, keycode, mode, protocolId: proto });
      if (status) status.textContent = 'Test signal transmitted!';
    } catch (err) {
      if (status) status.textContent = 'Test failed: ' + err.message;
    }
  }

  async saveLearnedIrCommand() {
    const devId = document.getElementById('wizardDevice')?.value;
    const name = document.getElementById('wizardCommandName')?.value.trim();
    const raw = document.getElementById('wizardRaw')?.value.trim();
    const nec = document.getElementById('wizardNec')?.value.trim();
    const keycode = document.getElementById('wizardKeycode')?.value.trim();
    const mode = document.getElementById('wizardMode')?.value || 'auto';
    const protocol = document.getElementById('wizardProtocol')?.value || '2';
    const status = document.getElementById('wizardLearnStatus');

    if (!devId || !name) return alert('Device and command name are required.');

    try {
      if (status) status.textContent = 'Saving command...';
      await this.client.saveLearnedCommand({ deviceId: devId, name, raw, nec, keycode, mode, protocol });
      if (status) status.innerHTML = '<span style="color:var(--success)">✓ Command saved!</span>';
      await this.refreshData();
    } catch (err) {
      if (status) status.innerHTML = `<span style="color:var(--danger)">Error: ${err.message}</span>`;
    }
  }

  populateVerifyDropdowns() {
    const devSel = document.getElementById('verifyDevice');
    const cmdSel = document.getElementById('verifyCommand');
    if (!devSel || !cmdSel) return;
    const devs = this.inventory.devices || [];
    devSel.innerHTML = devs.map(d => `<option value="${d.id}">${d.name || d.id}</option>`).join('');

    const updateCmds = () => {
      const d = devs.find(x => String(x.id) === String(devSel.value));
      cmdSel.innerHTML = (d?.commands || []).map(c => `<option value="${c.name}">${c.name}</option>`).join('');
    };
    devSel.onchange = updateCmds;
    updateCmds();
  }

  async sendVerifyTestCommand() {
    const devId = document.getElementById('verifyDevice')?.value;
    const cmd = document.getElementById('verifyCommand')?.value;
    const status = document.getElementById('wizardVerifyStatus');
    if (!devId || !cmd) return alert('Select device and command first.');
    try {
      if (status) status.textContent = `Sending ${cmd}...`;
      await this.client.sendCommand(devId, cmd);
      if (status) status.textContent = `Sent ${cmd} successfully!`;
    } catch (err) {
      if (status) status.textContent = `Send failed: ${err.message}`;
    }
  }

  populateManageDevices(selectedId = null) {
    const sel = document.getElementById('manageDeviceSelect');
    const editor = document.getElementById('storedDeviceEditor');
    if (!sel || !editor) return;

    const previousId = selectedId || sel.value;
    const devs = this.inventory.devices || [];
    sel.innerHTML = devs.map(d => `<option value="${d.id}">${d.name || d.id}</option>`).join('');

    if (previousId && devs.some(d => String(d.id) === String(previousId))) {
      sel.value = String(previousId);
    }

    const render = () => {
      const dev = devs.find(d => String(d.id) === String(sel.value));
      if (!dev) { editor.innerHTML = ''; return; }
      const cp = dev.control_port > 0 ? dev.control_port : 7;

      editor.innerHTML = `
        <div class="card" style="margin-bottom: 12px;">
          <h3 style="font-size: 15px; margin-bottom: 8px;">Device Details</h3>
          <div class="row">
            <div><label>Name</label><input id="mgrDevName" class="form-control" value="${dev.name || ''}"></div>
            <div><label>Type</label><input id="mgrDevType" class="form-control" value="${dev.type || ''}"></div>
          </div>
          <div class="row" style="margin-top: 8px;">
            <div><label>Manufacturer</label><input id="mgrDevMan" class="form-control" value="${dev.manufacturer || ''}"></div>
            <div><label>Model</label><input id="mgrDevMod" class="form-control" value="${dev.model || ''}"></div>
          </div>
          <div style="margin-top: 10px;">
            <label>IR Blaster Assignment</label>
            <div style="display: flex; gap: 12px; flex-wrap: wrap;">
              <label style="display: flex; align-items: center; gap: 6px;"><input type="checkbox" id="mgrBlasterInternal" value="4" ${(cp & 4) ? 'checked' : ''}> Hub Internal</label>
              <label style="display: flex; align-items: center; gap: 6px;"><input type="checkbox" id="mgrBlasterP1" value="1" ${(cp & 1) ? 'checked' : ''}> Blaster Port 1</label>
              <label style="display: flex; align-items: center; gap: 6px;"><input type="checkbox" id="mgrBlasterP2" value="2" ${(cp & 2) ? 'checked' : ''}> Blaster Port 2</label>
            </div>
          </div>
          <div class="actions" style="margin-top: 12px; display: flex; justify-content: space-between;">
            <button type="button" id="btnMgrSaveDev" class="btn btn-sm btn-primary">Save Device Details</button>
            <button type="button" id="btnMgrDelDev" class="btn btn-sm btn-danger">Delete Device</button>
          </div>
        </div>

        <div class="card" style="margin-bottom: 12px;">
          <h3 style="font-size: 15px; margin-bottom: 8px;">Power Management</h3>
          <label style="display: flex; align-items: center; gap: 6px;">
            <input type="checkbox" id="mgrDevAlwaysOn" ${dev.is_power_always_on ? 'checked' : ''}>
            <span>Always On (leave powered between activities)</span>
          </label>
          <div style="margin-top: 8px;">
            <label>Warmup Delay (ms)</label>
            <input id="mgrDevPowerDelay" type="number" class="form-control" value="${dev.power_on_delay || 1500}">
          </div>
          <div class="actions" style="margin-top: 12px;">
            <button type="button" id="btnMgrSavePower" class="btn btn-sm btn-secondary">Save Power Settings</button>
          </div>
        </div>

        <div class="card" style="margin-bottom: 12px;">
          <h3 style="font-size: 15px; margin-bottom: 8px;">MQTT Connection</h3>
          <label style="display: flex; align-items: center; gap: 6px;">
            <input type="checkbox" id="mgrDevMqttEnabled" ${dev.mqtt_enabled ? 'checked' : ''}>
            <span>Publish button presses to MQTT</span>
          </label>
          <div class="row" style="margin-top: 8px;">
            <div>
              <label>Topic Template</label>
              <input id="mgrDevMqttTopic" class="form-control" value="${dev.mqtt_topic || '{root}/button/{device}/{command}'}">
              <div style="display:flex; gap:4px; margin-top:4px; flex-wrap:wrap;">
                <button type="button" class="btn btn-xs btn-ghost btn-insert-token" data-token="{root}">{root}</button>
                <button type="button" class="btn btn-xs btn-ghost btn-insert-token" data-token="{device}">{device}</button>
                <button type="button" class="btn btn-xs btn-ghost btn-insert-token" data-token="{command}">{command}</button>
                <button type="button" class="btn btn-xs btn-ghost btn-insert-token" data-token="{state}">{state}</button>
              </div>
            </div>
            <div><label>Pulse (ms)</label><input id="mgrDevMqttPulse" type="number" class="form-control" value="${dev.mqtt_pulse_ms || 1000}"></div>
          </div>
          <div class="actions" style="margin-top: 12px; display: flex; gap: 8px;">
            <button type="button" id="btnMgrSaveMqtt" class="btn btn-sm btn-secondary">Save MQTT</button>
            <button type="button" id="btnMgrTestMqtt" class="btn btn-sm btn-ghost">Test Pulse</button>
          </div>
        </div>

        <div class="card">
          <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px;">
            <h3 style="font-size: 15px; margin: 0;">Saved Commands (<span id="mgrCmdCount">${(dev.commands || []).length}</span>)</h3>
            <span class="muted mini">Device: ${dev.name || dev.id}</span>
          </div>
          <div style="margin-bottom: 8px;">
            <input type="text" id="mgrCmdSearch" class="form-control" placeholder="Search commands (e.g. Power, Play, Volume)...">
          </div>
          <div id="mgrCmdsList" style="display: flex; flex-direction: column; gap: 6px; max-height: 280px; overflow-y: auto;">
          </div>
        </div>
      `;

      editor.querySelectorAll('.btn-insert-token').forEach(tb => {
        tb.addEventListener('click', () => {
          const inp = editor.querySelector('#mgrDevMqttTopic');
          if (inp) {
            const token = tb.getAttribute('data-token');
            inp.value = inp.value ? `${inp.value}/${token}` : token;
          }
        });
      });

      editor.querySelector('#btnMgrSaveDev')?.addEventListener('click', async () => {
        let cpVal = 0;
        if (editor.querySelector('#mgrBlasterInternal')?.checked) cpVal |= 4;
        if (editor.querySelector('#mgrBlasterP1')?.checked) cpVal |= 1;
        if (editor.querySelector('#mgrBlasterP2')?.checked) cpVal |= 2;
        await this.client.saveDeviceDetails({
          deviceId: dev.id,
          name: editor.querySelector('#mgrDevName').value,
          type: editor.querySelector('#mgrDevType').value,
          manufacturer: editor.querySelector('#mgrDevMan').value,
          model: editor.querySelector('#mgrDevMod').value,
          controlPort: cpVal
        });
        alert('Device details saved.');
        await this.refreshData();
        this.populateManageDevices(dev.id);
      });

      editor.querySelector('#btnMgrDelDev')?.addEventListener('click', async () => {
        if (!await showConfirm(`Delete device ${dev.name}?`, { title: 'Delete Device', okText: 'Delete' })) return;
        await this.client.deleteDevice(dev.id);
        alert('Device deleted.');
        await this.refreshData();
        this.populateManageDevices();
      });

      editor.querySelector('#btnMgrSavePower')?.addEventListener('click', async () => {
        await this.client.saveDevicePower({
          deviceId: dev.id,
          isPowerAlwaysOn: editor.querySelector('#mgrDevAlwaysOn').checked ? 1 : 0,
          powerOnDelay: Number(editor.querySelector('#mgrDevPowerDelay').value) || 1500
        });
        alert('Power settings saved.');
        await this.refreshData();
        this.populateManageDevices(dev.id);
      });

      editor.querySelector('#btnMgrSaveMqtt')?.addEventListener('click', async () => {
        await this.client.saveDeviceMqtt({
          deviceId: dev.id,
          mqttEnabled: editor.querySelector('#mgrDevMqttEnabled').checked ? 1 : 0,
          mqttTopic: editor.querySelector('#mgrDevMqttTopic').value,
          mqttPulseMs: Number(editor.querySelector('#mgrDevMqttPulse').value) || 1000
        });
        alert('Device MQTT settings saved.');
        await this.refreshData();
        this.populateManageDevices(dev.id);
      });

      editor.querySelector('#btnMgrTestMqtt')?.addEventListener('click', async () => {
        const mgrTopic = editor.querySelector('#mgrDevMqttTopic')?.value?.trim();
        const mgrPulse = Number(editor.querySelector('#mgrDevMqttPulse')?.value) || 1000;
        await this.client.testDeviceMqtt({
          deviceId: dev.id,
          device: dev.name || dev.id,
          command: 'PowerToggle',
          topic: mgrTopic,
          pulseMs: mgrPulse
        });
        alert('Test MQTT pulse sent.');
      });

      const renderCmds = (filter = '') => {
        const q = filter.trim().toLowerCase();
        const allCmds = dev.commands || [];
        // Sort with friendly named commands first (letters), numbers grouped together
        const sortedCmds = [...allCmds].sort((a, b) => {
          const aIsNum = /^\d+$/.test(a.name || '');
          const bIsNum = /^\d+$/.test(b.name || '');
          if (aIsNum && !bIsNum) return 1;
          if (!aIsNum && bIsNum) return -1;
          return (a.name || '').localeCompare(b.name || '');
        });
        const filtered = q ? sortedCmds.filter(c => (c.name || '').toLowerCase().includes(q)) : sortedCmds;
        const listEl = editor.querySelector('#mgrCmdsList');
        const countEl = editor.querySelector('#mgrCmdCount');
        if (countEl) countEl.textContent = q ? `${filtered.length} / ${allCmds.length}` : allCmds.length;
        if (!listEl) return;
        if (!filtered.length) {
          listEl.innerHTML = `<div class="muted mini" style="padding: 12px; text-align: center;">${allCmds.length === 0 ? 'No commands saved for this device.' : 'No commands matching search.'}</div>`;
          return;
        }
        listEl.innerHTML = filtered.map(c => `
          <div class="queue-row" style="display: flex; justify-content: space-between; align-items: center; padding: 6px 8px; border: 1px solid var(--border); border-radius: var(--radius-sm); margin-bottom: 4px; background: rgba(255,255,255,0.02);">
            <div>
              <strong style="font-size: 13px;">${c.name}</strong>
              <div class="muted mini">Proto ${c.protocolId || c.protocol_id || 'IR'}${c.learned ? ' • Learned' : ''}${c.raw ? ' • Raw' : ''}</div>
            </div>
            <div style="display: flex; gap: 6px;">
              <button type="button" class="btn btn-xs btn-primary btn-mgr-test-cmd" data-cmd="${c.name}">Send</button>
              <button type="button" class="btn btn-xs btn-danger btn-mgr-del-cmd" data-cmd="${c.name}">Delete</button>
            </div>
          </div>
        `).join('');

        listEl.querySelectorAll('.btn-mgr-test-cmd').forEach(b => {
          b.addEventListener('click', () => this.client.sendCommand(dev.id, b.getAttribute('data-cmd')));
        });

        listEl.querySelectorAll('.btn-mgr-del-cmd').forEach(b => {
          b.addEventListener('click', async () => {
            const cmd = b.getAttribute('data-cmd');
            if (!await showConfirm(`Delete command ${cmd}?`, { title: 'Delete Command', okText: 'Delete' })) return;
            await this.client.deleteCommand(dev.id, cmd);
            await this.refreshData();
            render();
          });
        });
      };

      const searchInput = editor.querySelector('#mgrCmdSearch');
      if (searchInput) {
        searchInput.addEventListener('input', (e) => renderCmds(e.target.value));
      }
      renderCmds();
    };

    sel.onchange = render;
    render();
  }

  /* -------------------------------------------------------------
   * 6. Bulk IR Test View (Lab)
   * ------------------------------------------------------------- */
  initLabView() {
    const sel = document.getElementById('labDevice');
    if (sel) {
      sel.innerHTML = '<option value="__auto_lab__">Temporary test device</option>' +
        (this.inventory.devices || []).map(d => `<option value="${d.id}">${d.name || d.id}</option>`).join('');
    }
  }

  async findLabCandidates() {
    const q = document.getElementById('labPathFilter')?.value.trim();
    const src = document.getElementById('labSource')?.value || 'all';
    const status = document.getElementById('labStatus');
    const box = document.getElementById('labCandidates');

    if (status) status.textContent = 'Searching candidate files...';
    try {
      const entries = await loadIrdSource(src === 'all' ? 'irdb' : src);
      const matches = rankIrdEntries(entries, q);
      if (status) status.textContent = `Found ${matches.length} matching files.`;
      if (box) {
        box.style.display = matches.length ? 'block' : 'none';
        box.innerHTML = matches.slice(0, 30).map(m => `
          <div class="queue-row" style="padding: 4px 8px;">
            <span>${m.path}</span>
            <span class="badge">${m.source}</span>
          </div>
        `).join('');
      }
    } catch (err) {
      if (status) status.textContent = 'Search failed: ' + err.message;
    }
  }

  async loadLabCommands() {
    const q = document.getElementById('labPathFilter')?.value.trim();
    const src = document.getElementById('labSource')?.value || 'all';
    const cmdFilter = (document.getElementById('labCommandFilter')?.value || '').toLowerCase();
    const status = document.getElementById('labStatus');

    if (status) status.textContent = 'Loading commands from files...';
    try {
      const entries = await loadIrdSource(src === 'all' ? 'irdb' : src);
      const matches = rankIrdEntries(entries, q).slice(0, 5);
      let allRows = [];
      for (const m of matches) {
        const text = await fetchIrdFile(m.path, m.source);
        const rows = parseIrText(text, m.source, m.path);
        allRows = allRows.concat(rows);
      }
      if (cmdFilter) {
        allRows = allRows.filter(r => r.name.toLowerCase().includes(cmdFilter));
      }
      this.labQueue = allRows.slice(0, 300);
      this.renderLabQueue();
      if (status) status.textContent = `Loaded ${this.labQueue.length} commands into queue.`;
    } catch (err) {
      if (status) status.textContent = 'Failed to load: ' + err.message;
    }
  }

  useStoredLabCommands() {
    const devId = document.getElementById('labDevice')?.value;
    const dev = (this.inventory.devices || []).find(d => String(d.id) === String(devId));
    if (!dev) return alert('Select a stored device.');
    this.labQueue = (dev.commands || []).map(c => ({
      name: c.name,
      meta: 'Stored command',
      keycode: c.keycode,
      raw: c.raw,
      stored: true
    }));
    this.renderLabQueue();
  }

  renderLabQueue() {
    const box = document.getElementById('labQueue');
    const sum = document.getElementById('labSummary');
    if (sum) sum.textContent = `${this.labQueue.length} queued`;
    if (!box) return;
    if (!this.labQueue.length) {
      box.innerHTML = '<div class="muted mini" style="padding: 8px;">No commands queued.</div>';
      return;
    }
    box.innerHTML = this.labQueue.map((c, i) => `
      <div class="queue-row" style="padding: 4px 8px;">
        <label style="display: flex; align-items: center; gap: 8px; flex: 1; cursor: pointer; margin: 0;">
          <input type="checkbox" class="lab-pick" data-index="${i}" checked>
          <span><strong>${c.name}</strong> <span class="muted mini">(${c.meta})</span></span>
        </label>
        <span class="badge ${c.raw ? 'ok' : 'primary'}">${c.raw ? 'Raw' : 'Code'}</span>
      </div>
    `).join('');
  }

  setLabSelections(check) {
    document.querySelectorAll('.lab-pick').forEach(cb => { cb.checked = check; });
  }

  dropUncheckedLab() {
    const picks = new Set();
    document.querySelectorAll('.lab-pick:checked').forEach(cb => {
      picks.add(Number(cb.getAttribute('data-index')));
    });
    this.labQueue = this.labQueue.filter((_, i) => picks.has(i));
    this.renderLabQueue();
  }

  clearLabQueue() {
    this.labQueue = [];
    this.renderLabQueue();
  }

  async importLabSelected() {
    const devId = document.getElementById('labDevice')?.value;
    if (!devId || devId === '__auto_lab__') return alert('Please select a real stored device to save into.');
    const lines = [];
    document.querySelectorAll('.lab-pick:checked').forEach(cb => {
      const idx = Number(cb.getAttribute('data-index'));
      const c = this.labQueue[idx];
      if (c.raw) lines.push(`${c.name}|raw|${c.raw}`);
      else if (c.keycode) lines.push(`${c.name}|${c.keycode}`);
    });
    if (!lines.length) return alert('No commands selected.');
    try {
      await this.client.importIrdb({ deviceId: devId, payload: lines.join('\n') });
      alert(`Imported ${lines.length} commands.`);
      await this.refreshData();
    } catch (err) {
      alert('Import failed: ' + err.message);
    }
  }

  async runLabQueue() {
    if (this.labRunning) return;
    const devId = document.getElementById('labDevice')?.value;
    const delay = Number(document.getElementById('labSendDelay')?.value) || 80;
    const dry = document.getElementById('labDryRun')?.checked;
    const log = document.getElementById('labLog');
    const meter = document.getElementById('labMeter');

    const selected = [];
    document.querySelectorAll('.lab-pick:checked').forEach(cb => {
      const idx = Number(cb.getAttribute('data-index'));
      selected.push(this.labQueue[idx]);
    });

    if (!selected.length) return alert('No commands selected to run.');
    this.labRunning = true;
    this.labRunId = Date.now().toString();

    let done = 0;
    for (const cmd of selected) {
      if (!this.labRunning) break;
      done++;
      if (meter) meter.style.width = `${Math.round((done / selected.length) * 100)}%`;
      if (log) log.textContent = `[${done}/${selected.length}] Sending ${cmd.name}...`;
      if (!dry) {
        try {
          await this.client.sendCommand(devId, cmd.name);
        } catch (e) {
          if (log) log.textContent += ` (error: ${e.message})`;
        }
      }
      await new Promise(r => setTimeout(r, delay));
    }

    this.labRunning = false;
    if (log) log.textContent = 'Bulk test completed.';
  }

  stopLabQueue() {
    this.labRunning = false;
    if (this.labRunId) this.client.cancelBatchIr(this.labRunId);
  }

  /* -------------------------------------------------------------
   * 7. Bluetooth View
   * ------------------------------------------------------------- */
  async loadBtView() {
    this.loadBluetoothSupervisionSettings();
    const badge = document.getElementById('btStatusBadge');
    const desc = document.getElementById('btHostDesc');
    const list = document.getElementById('btDevicesList');
    const hostSel = document.getElementById('btTargetHostSelect');
    const saveSel = document.getElementById('btSaveDeviceSelect');
    const pairTarget = document.getElementById('btPairTargetDev');

    const pidEl = document.getElementById('btStatusPid');
    const backendEl = document.getElementById('btStatusBackend');
    const targetNameEl = document.getElementById('btStatusTargetName');
    const targetAddrEl = document.getElementById('btStatusTargetAddr');
    const hostStateEl = document.getElementById('btStatusHostState');
    const hostDetailEl = document.getElementById('btStatusHostDetail');
    const remoteStateEl = document.getElementById('btStatusRemoteState');
    const remoteAddrEl = document.getElementById('btStatusRemoteAddr');

    if (pairTarget) {
      const devs = this.inventory.devices || [];
      pairTarget.innerHTML = '<option value="">-- Select Target Device (Required) --</option>' +
        devs.map(d => {
          const isBt = d.type === 'BT' || d.type === 'Bluetooth' || d.isBt || Number(d.transport) === 32;
          return `<option value="${d.id}">${d.name || d.id}${isBt ? ' (BT)' : ''}</option>`;
        }).join('');
    }
    if (saveSel) {
      saveSel.innerHTML = (this.inventory.devices || []).map(d => `<option value="${d.id}">${d.name || d.id}</option>`).join('');
    }

    try {
      const st = await this.client.getBtStatus();
      const isRunning = Boolean(st && (st.pid || st.running || st.backend === 'btstack'));

      if (badge) {
        badge.className = `badge ${isRunning ? 'ok' : 'bad'}`;
        badge.textContent = isRunning ? 'BTstack Running' : 'Stopped';
      }
      if (desc) desc.textContent = isRunning ? 'BTstack daemon active (UART hci0) • HID ready' : 'BTstack daemon stopped';

      if (pidEl) pidEl.textContent = isRunning ? `Running (PID ${st.pid || 'Active'})` : 'Stopped';
      if (backendEl) backendEl.textContent = st.backend ? `Backend: ${st.backend}` : 'Not Running';

      // Host connection
      const host = st.host || {};
      const isHostConn = Boolean(host.connected);

      const findAssignedDevice = (h) => {
        if (!h || !h.addr) return null;
        return (this.inventory.devices || []).find(d => {
          const a = (d.bt_host || d.BTAddress || d.btHost || d.bdaddr || '').toLowerCase();
          return a && a === h.addr.toLowerCase();
        });
      };

      const resolveHostDeviceName = (h) => {
        if (!h) return 'Bluetooth Host';
        const assigned = findAssignedDevice(h);
        if (assigned) return assigned.name || assigned.model || h.name;
        const matchDev = (this.inventory.devices || []).find(d => {
          if (h.name && (d.name === h.name || d.id === h.name)) return true;
          return false;
        });
        if (matchDev) return matchDev.name || matchDev.model || h.name;
        if (!h.name || h.name === 'Default Host' || h.name.startsWith('Host 00:')) {
          const btDev = (this.inventory.devices || []).find(d => Number(d.transport) === 32 || (d.type && d.type.toLowerCase().includes('bt')));
          if (btDev) return btDev.name || btDev.model;
        }
        return h.name || 'Bluetooth Host';
      };

      if (targetNameEl) {
        if (isHostConn) {
          targetNameEl.textContent = resolveHostDeviceName(host);
          targetNameEl.style.color = 'var(--accent)';
        } else {
          targetNameEl.textContent = 'None (Disconnected)';
          targetNameEl.style.color = 'var(--text-dim)';
        }
      }
      if (targetAddrEl) {
        targetAddrEl.textContent = isHostConn ? (host.addr || 'Connected') : (host.addr && host.addr !== '00:00:00:00:00:00' ? `Target: ${host.addr}` : 'No active connection');
      }
      if (hostStateEl) {
        hostStateEl.textContent = isHostConn ? 'CONNECTED' : (host.pairing ? 'PAIRING...' : 'DISCONNECTED');
        hostStateEl.style.color = isHostConn ? 'var(--success)' : 'var(--text-dim)';
      }
      if (hostDetailEl) hostDetailEl.textContent = isHostConn ? 'BLE HID Link Active' : 'Standby / Idle';

      // Remote connection
      const rem = st.remote || {};
      const isRemConn = Boolean(rem.connected);
      if (remoteStateEl) {
        remoteStateEl.textContent = isRemConn ? 'CONNECTED' : 'DISCONNECTED';
        remoteStateEl.style.color = isRemConn ? 'var(--success)' : 'var(--text-dim)';
      }
      if (remoteAddrEl) remoteAddrEl.textContent = rem.addr && rem.addr !== '00:00:00:00:00:00' ? rem.addr : 'Homatics B25 / BLE Remote';

      // Paired hosts list
      const hosts = st.hosts || (st.host?.addr ? [st.host] : []);
      const devs = this.inventory.devices || [];
      if (list) {
        list.innerHTML = hosts.length ? hosts.map(h => {
          const assigned = findAssignedDevice(h);
          return `
          <div class="queue-row" style="display:flex; justify-content:space-between; align-items:center; padding:8px 10px; background:var(--bg-card); border:1px solid var(--border); border-radius:var(--radius-sm); margin-bottom:6px; flex-wrap:wrap; gap:8px;">
            <div style="min-width: 180px;">
              <strong>${resolveHostDeviceName(h)}</strong>
              <div class="muted mini">${h.addr} • <span style="color:${h.connected ? 'var(--success)' : 'var(--text-dim)'}">${h.connected ? 'Connected' : 'Paired'}</span></div>
            </div>
            <div style="display:flex; gap:6px; align-items:center; flex-wrap:wrap;">
              <select class="form-control form-control-sm host-assign-sel" data-addr="${h.addr}" style="font-size:11px; max-width:180px;">
                <option value="">-- Assign Device --</option>
                ${devs.map(d => `<option value="${d.id}" ${assigned && String(assigned.id) === String(d.id) ? 'selected' : ''}>${d.name || d.id}</option>`).join('')}
              </select>
              <button type="button" class="btn btn-xs btn-secondary btn-bt-disconn" data-addr="${h.addr}">${h.connected ? 'Disconnect' : 'Connect'}</button>
            </div>
          </div>
        `;
        }).join('') : '<div class="muted mini">No paired hosts registered yet.</div>';

        list.querySelectorAll('.host-assign-sel').forEach(selEl => {
          selEl.addEventListener('change', async () => {
            const devId = selEl.value;
            const addr = selEl.getAttribute('data-addr');
            if (devId && addr) {
              try {
                await this.client.linkBtDevice(addr, devId);
                await this.refreshData();
                this.loadBtView();
              } catch (err) {
                alert('Assign failed: ' + err.message);
              }
            }
          });
        });

        list.querySelectorAll('.btn-bt-disconn').forEach(b => {
          b.addEventListener('click', async () => {
            await this.client.disconnectBt(b.getAttribute('data-addr'));
            this.loadBtView();
          });
        });
      }

      if (hostSel) {
        hostSel.innerHTML = '<option value="">Auto (Active Host)</option>' +
          hosts.map(d => `<option value="${d.addr}">${d.name || d.addr}</option>`).join('');
      }
    } catch (err) {
      if (desc) desc.textContent = 'Error: ' + err.message;
    }
  }

  async toggleBtPairing() {
    const btn = document.getElementById('btPairToggleBtn');
    const name = document.getElementById('btPairName')?.value || 'Harmony Keyboard';
    const target = document.getElementById('btPairTargetDev')?.value || '';
    if (!target) {
      alert('Please select a target device to assign this Bluetooth pairing to.');
      return;
    }
    try {
      if (btn) btn.textContent = 'Entering pairing mode...';
      await this.client.setBtPairing(true, name, target);
      const devObj = (this.inventory.devices || []).find(d => String(d.id) === String(target));
      alert('Hub is now discoverable as "' + name + '" for ' + (devObj ? devObj.name : target) + '. Pair from your host device.');
      this.loadBtView();
    } catch (err) {
      alert('Pairing mode failed: ' + err.message);
    } finally {
      if (btn) btn.textContent = 'Start Pairing Mode';
    }
  }

  async sendBtDirectText() {
    const txt = document.getElementById('btDirectText')?.value;
    const stat = document.getElementById('btTextStatus');
    const target = (document.getElementById('btTargetHostSelect')?.value || '').trim();
    if (!txt) return;
    try {
      if (stat) stat.textContent = 'Sending text...';
      await this.client.sendBtText(txt, target);
      if (stat) stat.innerHTML = '<span style="color:var(--success)">Sent!</span>';
      this.appendBtLog(`Sent ${txt.length} chars text` + (target ? ' -> ' + target : ''));
    } catch (err) {
      if (stat) stat.innerHTML = `<span style="color:var(--danger)">Error: ${err.message}</span>`;
      this.appendBtLog(`Text send failed: ${err.message || err}`);
    }
  }

  appendBtLog(msg) {
    const el = document.getElementById('btLog');
    if (!el) return;
    const time = new Date().toLocaleTimeString();
    el.textContent = `[${time}] ${msg}\n` + (el.textContent === 'Ready.' ? '' : el.textContent.slice(0, 2000));
  }

  async runBtMacroScript() {
    const script = document.getElementById('btMacroScript')?.value;
    const delay = Number(document.getElementById('btScriptDelay')?.value) || 35;
    const stat = document.getElementById('btScriptStatus');
    if (!script) return;
    try {
      if (stat) stat.textContent = 'Running macro script...';
      await this.client.runBtScript(script, delay);
      if (stat) stat.innerHTML = '<span style="color:var(--success)">Script finished!</span>';
    } catch (err) {
      if (stat) stat.innerHTML = `<span style="color:var(--danger)">Failed: ${err.message}</span>`;
    }
  }

  async saveBtMacroCommand() {
    const devId = document.getElementById('btSaveDeviceSelect')?.value;
    const name = document.getElementById('btSaveCommandName')?.value.trim();
    const script = document.getElementById('btMacroScript')?.value.trim();
    const stat = document.getElementById('btSaveCommandStatus');
    if (!devId || !name || !script) return alert('Device, command name, and script content are required.');
    try {
      await this.client.saveBtCommand(devId, name, script);
      if (stat) stat.innerHTML = '<span style="color:var(--success)">Saved!</span>';
      await this.refreshData();
    } catch (err) {
      if (stat) stat.innerHTML = `<span style="color:var(--danger)">Error: ${err.message}</span>`;
    }
  }

  async loadBluetoothSupervisionSettings() {
    try {
      const s = await this.client.getSettings();
      if (!s) return;
      const toggle = document.getElementById('btAutoReconnectToggle');
      const bootToggle = document.getElementById('btBootReconnectToggle');
      const dur = document.getElementById('btReconnectDuration');
      const del = document.getElementById('btReconnectDelay');
      if (toggle) toggle.checked = !!s.bt_auto_reconnect;
      if (bootToggle) bootToggle.checked = s.bt_reconnect_on_boot !== undefined ? !!s.bt_reconnect_on_boot : true;
      if (dur && s.bt_reconnect_duration_sec !== undefined) dur.value = s.bt_reconnect_duration_sec;
      if (del && s.bt_reconnect_delay_sec !== undefined) del.value = s.bt_reconnect_delay_sec;
    } catch (e) {
      console.warn('Failed to load BT supervision settings:', e);
    }
  }

  async saveBluetoothSupervisionSettings() {
    const toggle = document.getElementById('btAutoReconnectToggle');
    const bootToggle = document.getElementById('btBootReconnectToggle');
    const dur = document.getElementById('btReconnectDuration');
    const del = document.getElementById('btReconnectDelay');
    const stat = document.getElementById('btSettingsStatus');
    if (stat) stat.textContent = 'Saving settings...';
    try {
      const payload = {
        bt_auto_reconnect: toggle ? (toggle.checked ? 1 : 0) : 1,
        bt_reconnect_on_boot: bootToggle ? (bootToggle.checked ? 1 : 0) : 1,
        bt_reconnect_duration_sec: dur ? parseInt(dur.value, 10) || 45 : 45,
        bt_reconnect_delay_sec: del ? parseInt(del.value, 10) || 5 : 5
      };
      await this.client.saveSettings(payload);
      if (stat) stat.innerHTML = '<span style="color:var(--success)">✓ Supervision settings saved</span>';
      setTimeout(() => { if (stat) stat.textContent = ''; }, 3000);
    } catch (err) {
      if (stat) stat.innerHTML = `<span style="color:var(--danger)">Error: ${err.message}</span>`;
    }
  }

  /* -------------------------------------------------------------
   * 7b. Bluetooth Keyboard & Macros View
   * ------------------------------------------------------------- */
  async loadBtKeyboardView() {
    const hostSel = document.getElementById('btTargetHostSelect');
    const saveSel = document.getElementById('btSaveDeviceSelect');
    if (saveSel) {
      saveSel.innerHTML = (this.inventory.devices || []).map(d => `<option value="${d.id}">${d.name || d.id}</option>`).join('');
    }
    try {
      const st = await this.client.getBtStatus();
      const hosts = st?.hosts || (st?.host?.addr ? [st.host] : []);
      if (hostSel) {
        hostSel.innerHTML = '<option value="">Auto (Active Host)</option>' +
          hosts.map(d => `<option value="${d.addr}">${d.name || d.addr}</option>`).join('');
      }
    } catch {}
    this.renderBtSavedCommands();
  }

  renderBtSavedCommands() {
    const list = document.getElementById('btSavedDevicesList');
    if (!list) return;
    const isMacro = c => Boolean(c.isMacro || (c.raw && (String(c.raw).startsWith('BT:SCRIPT:') || String(c.raw).startsWith('MACRO:'))));
    const devs = (this.inventory.devices || []).filter(d => (d.commands || []).some(isMacro));
    if (!devs.length) {
      list.innerHTML = '<div class="muted mini">No custom Bluetooth macro scripts saved yet.</div>';
      return;
    }
    list.innerHTML = devs.map(d => {
      const cmds = (d.commands || []).filter(isMacro);
      return `
        <div style="margin-bottom:12px; background:var(--bg-card); border:1px solid var(--border); border-radius:var(--radius-sm); padding:10px;">
          <strong style="font-size:13px; color:var(--accent);">${d.name} (${d.model || d.id})</strong>
          <div style="display:flex; flex-wrap:wrap; gap:6px; margin-top:8px;">
            ${cmds.map(c => `
              <div class="pill" style="display:flex; align-items:center; gap:6px; padding:4px 8px;">
                <span>${c.name}</span>
                <button type="button" class="btn btn-xs btn-ghost btn-del-bt-cmd" data-dev="${d.id}" data-cmd="${c.name}" style="padding:0 4px; color:var(--danger);" title="Delete macro">×</button>
              </div>
            `).join('')}
          </div>
        </div>
      `;
    }).join('');

    list.querySelectorAll('.btn-del-bt-cmd').forEach(btn => {
      btn.addEventListener('click', async () => {
        const devId = btn.getAttribute('data-dev');
        const cmdName = btn.getAttribute('data-cmd');
        if (!await showConfirm(`Delete Bluetooth macro "${cmdName}"?`, { title: 'Delete Bluetooth Macro', okText: 'Delete' })) return;
        try {
          await this.client.deleteBtCommand(devId, cmdName);
          await this.refreshData();
          this.renderBtSavedCommands();
        } catch (e) {
          alert('Delete failed: ' + e.message);
        }
      });
    });
  }

  setupHarmonyArchiveSearch() {
    const mfgInput = document.getElementById('harmonyMfgInput');
    const mfgDatalist = document.getElementById('harmonyMfgDatalist');
    const modelInput = document.getElementById('harmonyModelInput');
    const modelDatalist = document.getElementById('harmonyModelDatalist');
    const searchBtn = document.getElementById('harmonySearchBtn');
    const statusEl = document.getElementById('harmonyDbStatus');
    const specsCard = document.getElementById('harmonySpecsCard');
    const titleEl = document.getElementById('harmonyDeviceTitle');
    const catEl = document.getElementById('harmonyDeviceCategory');
    const codesetEl = document.getElementById('harmonyDeviceCodeset');
    const gridEl = document.getElementById('harmonyCommandsGrid');
    const importBtn = document.getElementById('harmonyImportBtn');
    const targetDevSel = document.getElementById('harmonyTargetDevSelect');
    const cmdCountEl = document.getElementById('harmonyCmdCount');
    const cmdSelectedCountEl = document.getElementById('harmonyCmdSelectedCount');
    const btnSelectAll = document.getElementById('btnHarmonySelectAll');
    const btnSelectNone = document.getElementById('btnHarmonySelectNone');
    const cmdFilterInput = document.getElementById('harmonyCmdFilter');
    const importBtnCountEl = document.getElementById('harmonyImportBtnCount');
    const importStatusEl = document.getElementById('harmonyImportStatus');

    if (!mfgInput || !mfgDatalist) return;

    // Populate initial manufacturers in datalist
    const mfgList = getHarmonyManufacturers();
    mfgDatalist.innerHTML = mfgList.slice(0, 200).map(m => `<option value="${m}"></option>`).join('');

    let currentModels = [];
    let currentDetail = null;
    let currentCodeset = null;

    const updateSelectedCount = () => {
      const allCbs = gridEl ? Array.from(gridEl.querySelectorAll('.harmony-cmd-cb')) : [];
      const checkedCbs = allCbs.filter(cb => cb.checked);
      const totalCount = currentCodeset?.commands?.length || 0;
      const selCount = checkedCbs.length;

      if (cmdCountEl) cmdCountEl.textContent = totalCount;
      if (cmdSelectedCountEl) cmdSelectedCountEl.textContent = selCount;
      if (importBtnCountEl) importBtnCountEl.textContent = selCount;
      if (importBtn) {
        importBtn.disabled = selCount === 0;
        importBtn.textContent = `Import Selected Commands (${selCount})`;
      }
    };

    const filterCommands = (filterText) => {
      if (!gridEl) return;
      const q = (filterText || '').toLowerCase().trim();
      const items = gridEl.querySelectorAll('.harmony-cmd-item');
      items.forEach(item => {
        const name = (item.querySelector('.cmd-chip-name')?.textContent || '').toLowerCase();
        if (!q || name.includes(q)) {
          item.style.display = 'inline-flex';
        } else {
          item.style.display = 'none';
        }
      });
    };

    cmdFilterInput?.addEventListener('input', (e) => {
      filterCommands(e.target.value);
    });

    btnSelectAll?.addEventListener('click', () => {
      if (!gridEl) return;
      const filterText = (cmdFilterInput?.value || '').toLowerCase().trim();
      gridEl.querySelectorAll('.harmony-cmd-item').forEach(item => {
        const name = (item.querySelector('.cmd-chip-name')?.textContent || '').toLowerCase();
        if (!filterText || name.includes(filterText)) {
          const cb = item.querySelector('.harmony-cmd-cb');
          if (cb) cb.checked = true;
        }
      });
      updateSelectedCount();
    });

    btnSelectNone?.addEventListener('click', () => {
      if (!gridEl) return;
      const filterText = (cmdFilterInput?.value || '').toLowerCase().trim();
      gridEl.querySelectorAll('.harmony-cmd-item').forEach(item => {
        const name = (item.querySelector('.cmd-chip-name')?.textContent || '').toLowerCase();
        if (!filterText || name.includes(filterText)) {
          const cb = item.querySelector('.harmony-cmd-cb');
          if (cb) cb.checked = false;
        }
      });
      updateSelectedCount();
    });

    targetDevSel?.addEventListener('change', () => {
      const devId = targetDevSel.value;
      if (devId) {
        this.selectedIrDeviceId = devId;
        const manageSel = document.getElementById('manageDeviceSelect');
        if (manageSel && manageSel.value !== devId) {
          manageSel.value = devId;
          this.updateStudioDeviceView();
        }
      }
    });

    mfgInput.addEventListener('input', () => {
      const q = mfgInput.value.trim();
      const matches = searchHarmonyManufacturers(q, 100);
      mfgDatalist.innerHTML = matches.map(m => `<option value="${m}"></option>`).join('');
    });

    const loadModelsForMfg = async (mfg) => {
      if (!mfg) return;
      if (statusEl) statusEl.textContent = `Loading model index for ${mfg}...`;
      try {
        currentModels = await fetchHarmonyModels(mfg);
        if (modelDatalist) {
          modelDatalist.innerHTML = currentModels.slice(0, 300).map(item => `<option value="${item.m}"></option>`).join('');
        }
        if (statusEl) statusEl.textContent = `Found ${currentModels.length} models for ${mfg}. Enter or pick model:`;
      } catch (err) {
        if (statusEl) statusEl.innerHTML = `<span style="color:var(--danger)">Failed to load models for ${mfg}: ${err.message}</span>`;
      }
    };

    mfgInput.addEventListener('change', () => {
      loadModelsForMfg(mfgInput.value.trim());
    });

    modelInput?.addEventListener('input', () => {
      if (!currentModels || !currentModels.length) return;
      const q = modelInput.value.trim();
      const filtered = searchHarmonyModels(currentModels, q, 100);
      if (modelDatalist) {
        modelDatalist.innerHTML = filtered.map(item => `<option value="${item.m}"></option>`).join('');
      }
    });

    const triggerDeviceLookup = async () => {
      const mfg = mfgInput.value.trim();
      const modelName = modelInput?.value.trim();
      if (!mfg || !modelName) {
        alert('Please specify both Manufacturer and Model.');
        return;
      }
      if (statusEl) statusEl.textContent = `Looking up specs & codeset for ${mfg} ${modelName}...`;
      if (specsCard) specsCard.style.display = 'none';

      try {
        if (!currentModels.length) {
          currentModels = await fetchHarmonyModels(mfg);
        }
        const match = currentModels.find(item => item.m.toLowerCase() === modelName.toLowerCase()) ||
                      currentModels.find(item => item.m.toLowerCase().includes(modelName.toLowerCase()));
        if (!match) {
          if (statusEl) statusEl.innerHTML = `<span style="color:var(--danger)">Model "${modelName}" not found under ${mfg}.</span>`;
          return;
        }

        const detail = await fetchHarmonyDeviceDetail(mfg, match.f);
        currentDetail = detail;

        if (!detail.codeset) {
          if (statusEl) statusEl.innerHTML = `<span style="color:var(--danger)">No IR codeset linked to this model.</span>`;
          return;
        }

        const codeset = await fetchHarmonyCodeset(detail.codeset);
        currentCodeset = codeset;

        const totalCmdCount = codeset.commands ? codeset.commands.length : 0;
        if (statusEl) statusEl.textContent = `✓ Loaded ${totalCmdCount} commands from Harmony Archive!`;
        if (specsCard) specsCard.style.display = 'block';
        if (titleEl) titleEl.textContent = `${detail.manufacturer || mfg} ${detail.model || modelName}`;
        if (catEl) catEl.textContent = mapHarmonyDeviceType(detail.deviceType);
        if (codesetEl) codesetEl.textContent = detail.codeset;

        // Sync target device selector with existing devices
        if (targetDevSel) {
          const devs = this.inventory.devices || [];
          const curDevId = this.selectedIrDeviceId || document.getElementById('manageDeviceSelect')?.value || (devs[0] && devs[0].id);
          targetDevSel.innerHTML = devs.map(d => `<option value="${d.id}">${d.name || d.id}</option>`).join('');
          if (curDevId && devs.some(d => String(d.id) === String(curDevId))) {
            targetDevSel.value = String(curDevId);
          }
        }

        if (gridEl) {
          const cmds = codeset.commands || [];
          gridEl.innerHTML = cmds.map((c, i) => `
            <label class="cmd-chip harmony-cmd-item" data-idx="${i}" style="display:inline-flex; align-items:center; gap:6px; background:var(--bg-secondary); border:1px solid var(--border); border-radius:6px; padding:4px 8px; margin:3px; font-size:12px; cursor:pointer; user-select:none;">
              <input type="checkbox" class="harmony-cmd-cb" data-idx="${i}" checked style="cursor:pointer; margin:0;">
              <span class="cmd-chip-name">${c.name}</span>
              ${c.pronto ? `<button type="button" class="btn btn-xs btn-ghost btn-test-archive-cmd" data-idx="${i}" title="Test Blast Pronto Hex" style="padding:1px 5px; font-size:10px; margin-left:2px;">▶ Test</button>` : ''}
            </label>
          `).join('');

          gridEl.querySelectorAll('.harmony-cmd-cb').forEach(cb => {
            cb.addEventListener('change', () => updateSelectedCount());
          });

          gridEl.querySelectorAll('.btn-test-archive-cmd').forEach(btn => {
            btn.addEventListener('click', async (e) => {
              e.preventDefault();
              e.stopPropagation();
              const idx = parseInt(btn.getAttribute('data-idx'), 10);
              const cmd = (codeset.commands || [])[idx];
              if (!cmd || !cmd.pronto) return;
              btn.textContent = '...';
              try {
                await this.client.sendDirectPronto(cmd.pronto, 7, 1);
                btn.textContent = '✓';
                setTimeout(() => { btn.textContent = '▶ Test'; }, 1000);
              } catch (err) {
                btn.textContent = '✗';
                alert('Test blast failed: ' + err.message);
                setTimeout(() => { btn.textContent = '▶ Test'; }, 1500);
              }
            });
          });
        }

        if (cmdFilterInput) cmdFilterInput.value = '';
        if (importStatusEl) importStatusEl.innerHTML = '';
        updateSelectedCount();
      } catch (err) {
        if (statusEl) statusEl.innerHTML = `<span style="color:var(--danger)">Error: ${err.message}</span>`;
      }
    };

    searchBtn?.addEventListener('click', triggerDeviceLookup);
    modelInput?.addEventListener('change', () => {
      if (modelInput.value.trim()) triggerDeviceLookup();
    });

    importBtn?.addEventListener('click', async () => {
      if (!currentCodeset || !currentCodeset.commands) return;
      const targetDevId = targetDevSel?.value || document.getElementById('manageDeviceSelect')?.value || this.selectedIrDeviceId;
      if (!targetDevId) return alert('Select an existing target device first.');

      const targetDev = (this.inventory.devices || []).find(d => String(d.id) === String(targetDevId));
      if (!targetDev) return alert('Target device not found in inventory.');

      const checkedCbs = Array.from(gridEl?.querySelectorAll('.harmony-cmd-cb:checked') || []);
      if (!checkedCbs.length) return alert('No commands selected to import.');

      const selectedCmds = checkedCbs.map(cb => {
        const idx = parseInt(cb.getAttribute('data-idx'), 10);
        return (currentCodeset.commands || [])[idx];
      }).filter(Boolean);

      importBtn.disabled = true;
      importBtn.textContent = `Importing ${selectedCmds.length} commands...`;
      if (importStatusEl) importStatusEl.innerHTML = `Importing ${selectedCmds.length} commands into "${targetDev.name}"...`;

      try {
        const targetPort = (targetDev.controlPort !== undefined) ? targetDev.controlPort : ((targetDev.control_port !== undefined) ? targetDev.control_port : 7);
        const cmdPayloads = selectedCmds.map(c => ({
          name: c.name,
          raw: c.pronto || c.raw || '',
          repeats: 3,
          port: targetPort
        })).filter(c => c.raw);

        let ok = false;
        try {
          const res = await this.client.saveBatchCommands(targetDevId, cmdPayloads);
          if (res && res.ok) ok = true;
        } catch {
          ok = false;
        }

        if (!ok) {
          for (const cmd of cmdPayloads) {
            await this.client.saveLearnedCommand({
              deviceId: targetDevId,
              name: cmd.name,
              raw: cmd.raw,
              repeats: cmd.repeats,
              port: cmd.port
            });
          }
        }

        if (importStatusEl) importStatusEl.innerHTML = `<span style="color:var(--success)">✓ Successfully imported ${cmdPayloads.length} commands into "${targetDev.name}"!</span>`;
        await this.refreshData();
        this.renderIrView(targetDevId);
      } catch (err) {
        alert('Failed to import commands: ' + err.message);
        if (importStatusEl) importStatusEl.innerHTML = `<span style="color:var(--danger)">Error: ${err.message}</span>`;
      } finally {
        importBtn.disabled = false;
        updateSelectedCount();
      }
    });
  }

  /* -------------------------------------------------------------
   * 8. BT Remote View (Homatics B25)
   * ------------------------------------------------------------- */
  async loadRemotesView() {
    const sel = document.getElementById('b25ActivitySelect');
    if (sel) {
      sel.innerHTML = '<option value="-1">Off / Idle (Default)</option>' +
        (this.activitiesData.activities || []).map(a => `<option value="${a.id}">${a.name || a.label}</option>`).join('');
    }
    const tgtDev = document.getElementById('b25TargetDevice');
    const presetDev = document.getElementById('b25PresetBtDevice');
    const devs = this.inventory.devices || [];
    if (tgtDev) tgtDev.innerHTML = devs.map(d => `<option value="${d.id}">${d.name || d.id}</option>`).join('');
    if (presetDev) presetDev.innerHTML = devs.map(d => `<option value="${d.id}">${d.name || d.id}</option>`).join('');

    const actSel = document.getElementById('b25TargetActivity');
    if (actSel) {
      actSel.innerHTML = (this.activitiesData.activities || []).map(a => `<option value="${a.id}">${a.name || a.label}</option>`).join('');
    }

    try {
      const [mRes, stRes] = await Promise.allSettled([
        this.client.getRemoteMappings(),
        this.client.getBtRemotePairStatus()
      ]);
      if (mRes.status === 'fulfilled' && mRes.value) {
        this.b25MapData = mRes.value;
      }
      const st = stRes.status === 'fulfilled' ? stRes.value : null;
      this.updateB25StatusUI(st);
    } catch {}

    this.selectB25Button(this.b25SelectedBtn || 'power', false);
    this.updateB25ButtonStates();
  }

  updateB25StatusUI(st) {
    const pairedName = document.getElementById('b25PairedName');
    const pairedAddr = document.getElementById('b25PairedAddr');
    const stateText = document.getElementById('b25StateText');
    const badge = document.getElementById('b25StatusBadge');

    const bdaddr = st?.bdaddr || this.b25MapData?.remote?.bdaddr || this.b25MapData?.bdaddr || '';
    const name = st?.name || this.b25MapData?.remote?.name || 'Homatics B25';
    const isConn = Boolean(st?.connected || (bdaddr && st?.running));

    if (pairedName) pairedName.textContent = name;
    if (pairedAddr) pairedAddr.textContent = bdaddr || 'None';
    if (stateText) stateText.textContent = st?.state || (isConn ? 'Connected / Ready' : (bdaddr ? 'Paired / Offline' : 'Idle'));
    if (badge) {
      badge.className = `badge ${isConn ? 'ok' : (bdaddr ? 'warn' : '')}`;
      badge.textContent = isConn ? 'CONNECTED' : (bdaddr ? 'PAIRED' : 'OFFLINE');
    }
  }

  selectB25Button(btnId, openModal = false) {
    this.b25SelectedBtn = btnId;
    document.querySelectorAll('.b25-hotspot').forEach(b => {
      b.classList.toggle('selected', b.getAttribute('data-btn') === btnId);
    });
    const lbl = document.getElementById('b25CurrentBtnName');
    if (lbl) lbl.textContent = btnId;

    const actSel = document.getElementById('b25ActivitySelect');
    const actName = actSel ? actSel.options[actSel.selectedIndex]?.text || 'Off / Idle' : 'Off / Idle';
    const modalActLbl = document.getElementById('b25ModalActivityName');
    if (modalActLbl) modalActLbl.textContent = actName;

    const actId = actSel?.value || '-1';
    const mapping = this.b25MapData?.activities?.[actId]?.[btnId];

    const typeSel = document.getElementById('b25ActionType');
    if (typeSel) {
      if (!mapping) typeSel.value = 'unmapped';
      else if (mapping.action) typeSel.value = mapping.action;
      else if (mapping.type) typeSel.value = mapping.type;
      else if (mapping.command) typeSel.value = 'device_cmd';
      this.toggleB25ActionFields();
    }

    const devId = mapping?.targetDevice || mapping?.deviceId;
    if (devId) {
      const devSel = document.getElementById('b25TargetDevice');
      if (devSel) devSel.value = devId;
    }
    const actTargetId = mapping?.targetActivity || mapping?.activityId;
    if (actTargetId) {
      const actTargetSel = document.getElementById('b25TargetActivity');
      if (actTargetSel) actTargetSel.value = actTargetId;
    }
    this.updateB25CommandDropdown(mapping?.command);

    if (openModal) {
      const m = document.getElementById('b25BtnModal');
      if (m) m.style.display = 'flex';
    }
  }

  updateB25CommandDropdown(curVal) {
    const devId = document.getElementById('b25TargetDevice')?.value;
    const cmdSel = document.getElementById('b25TargetCommand');
    if (!cmdSel) return;
    const dev = (this.inventory.devices || []).find(d => String(d.id) === String(devId));
    cmdSel.innerHTML = (dev?.commands || []).map(c => `<option value="${c.name}" ${c.name === curVal ? 'selected' : ''}>${c.name}</option>`).join('');
  }

  toggleB25ActionFields() {
    const t = document.getElementById('b25ActionType')?.value;
    const wDev = document.getElementById('b25WrapTargetDevice');
    const wCmd = document.getElementById('b25WrapTargetCommand');
    const wAct = document.getElementById('b25WrapTargetActivity');

    if (wDev) wDev.style.display = t === 'device_cmd' ? 'block' : 'none';
    if (wCmd) wCmd.style.display = t === 'device_cmd' ? 'block' : 'none';
    if (wAct) wAct.style.display = t === 'activity_start' ? 'block' : 'none';
  }

  applyB25Mapping() {
    const actId = document.getElementById('b25ActivitySelect')?.value || '-1';
    const type = document.getElementById('b25ActionType')?.value;
    if (!this.b25MapData.activities) this.b25MapData.activities = {};
    if (!this.b25MapData.activities[actId]) this.b25MapData.activities[actId] = {};

    if (type === 'unmapped') {
      delete this.b25MapData.activities[actId][this.b25SelectedBtn];
    } else if (type === 'device_cmd') {
      const devId = document.getElementById('b25TargetDevice')?.value;
      const cmd = document.getElementById('b25TargetCommand')?.value;
      this.b25MapData.activities[actId][this.b25SelectedBtn] = {
        action: 'device_cmd',
        type: 'device_cmd',
        targetDevice: devId,
        deviceId: devId,
        command: cmd
      };
    } else if (type === 'activity_start') {
      const actTarget = document.getElementById('b25TargetActivity')?.value;
      this.b25MapData.activities[actId][this.b25SelectedBtn] = {
        action: 'activity_start',
        type: 'activity_start',
        targetActivity: actTarget,
        activityId: actTarget
      };
    } else if (type === 'activity_stop') {
      this.b25MapData.activities[actId][this.b25SelectedBtn] = {
        action: 'activity_stop',
        type: 'activity_stop'
      };
    }
    this.updateB25ButtonStates();
    const modal = document.getElementById('b25BtnModal');
    if (modal) modal.style.display = 'none';
  }

  clearB25Mapping() {
    const actId = document.getElementById('b25ActivitySelect')?.value || '-1';
    if (this.b25MapData.activities?.[actId]) {
      delete this.b25MapData.activities[actId][this.b25SelectedBtn];
    }
    this.selectB25Button(this.b25SelectedBtn, false);
    this.updateB25ButtonStates();
    const modal = document.getElementById('b25BtnModal');
    if (modal) modal.style.display = 'none';
  }

  updateB25ButtonStates() {
    const actId = document.getElementById('b25ActivitySelect')?.value || '-1';
    const actMap = this.b25MapData?.activities?.[actId] || {};
    document.querySelectorAll('.b25-hotspot').forEach(b => {
      const id = b.getAttribute('data-btn');
      b.classList.toggle('mapped', Boolean(actMap[id]));
    });
  }

  async saveAllB25Mappings() {
    const btn = document.getElementById('b25SaveAllBtn');
    try {
      if (btn) btn.textContent = 'Saving...';
      await this.client.saveRemoteMappings(this.b25MapData);
      alert('All Homatics B25 button mappings saved to hub!');
    } catch (err) {
      alert('Save failed: ' + err.message);
    } finally {
      if (btn) btn.textContent = 'Save All Mappings';
    }
  }

  autoMapB25Preset() {
    const devId = document.getElementById('b25PresetBtDevice')?.value;
    const actId = document.getElementById('b25ActivitySelect')?.value || '-1';
    if (!devId) return alert('Select target device first.');
    const dev = (this.inventory.devices || []).find(d => String(d.id) === String(devId));
    if (!dev || !dev.commands) return alert('Device has no commands.');

    const norm = s => String(s || '').toLowerCase().replace(/[^a-z0-9]/g, '');
    const aliases = {
      power: ['power', 'poweroff', 'standby', 'powertoggle'],
      up: ['up', 'directionup'],
      down: ['down', 'directiondown'],
      left: ['left', 'directionleft'],
      right: ['right', 'directionright'],
      select: ['ok', 'select', 'enter'],
      back: ['back', 'return', 'exit'],
      home: ['home'],
      vol_up: ['volumeup', 'volup'],
      vol_down: ['volumedown', 'voldown'],
      mute: ['mute']
    };

    if (!this.b25MapData.activities) this.b25MapData.activities = {};
    if (!this.b25MapData.activities[actId]) this.b25MapData.activities[actId] = {};

    let mapped = 0;
    Object.entries(aliases).forEach(([btn, list]) => {
      for (const a of list) {
        const cmd = dev.commands.find(c => norm(c.name) === norm(a));
        if (cmd) {
          this.b25MapData.activities[actId][btn] = { type: 'device_cmd', deviceId: devId, command: cmd.name };
          mapped++;
          break;
        }
      }
    });

    this.updateB25ButtonStates();
    alert(`Auto-mapped ${mapped} buttons for device! Click "Save All Mappings" to persist.`);
  }

  async scanB25Remote() {
    const btn = document.getElementById('b25ScanBtn');
    const log = document.getElementById('b25PairLog');
    try {
      if (btn) btn.textContent = 'Scanning...';
      if (log) log.textContent = 'Scanning for BLE remotes (10s)...';
      await this.client.scanBtRemote();
      setTimeout(async () => {
        const st = await this.client.getBtRemotePairStatus();
        if (log) log.textContent = st.message || 'Scan completed.';
        const list = document.getElementById('b25DiscoveredList');
        if (list && st.devices) {
          list.innerHTML = st.devices.map(d => `<option value="${d.bdaddr}">${d.name || d.bdaddr}</option>`).join('');
        }
      }, 4000);
    } catch (err) {
      if (log) log.textContent = 'Scan error: ' + err.message;
    } finally {
      if (btn) btn.textContent = 'Scan for Remote';
    }
  }

  async pairB25Remote() {
    const addr = document.getElementById('b25DiscoveredList')?.value;
    const log = document.getElementById('b25PairLog');
    if (!addr) return alert('Select discovered remote first.');
    try {
      if (log) log.textContent = `Pairing with ${addr}...`;
      await this.client.pairBtRemote(addr);
      if (log) log.textContent = `Paired with ${addr}!`;
    } catch (err) {
      if (log) log.textContent = 'Pairing failed: ' + err.message;
    }
  }

  /* -------------------------------------------------------------
   * 8b. Harmony Elite RF Remote View
   * ------------------------------------------------------------- */
  async loadEliteView() {
    // Check RF enabled status from backend
    try {
      const svcs = await this.client.getServices();
      const rfBanner = document.getElementById('rfDisabledBanner');
      const rfBadge = document.getElementById('rfStatusBadge');
      const rfPairBtn = document.getElementById('rfPairBtn');
      const rfUnpairBtn = document.getElementById('rfUnpairBtn');
      const rfRefreshBtn = document.getElementById('rfRefreshBtn');

      if (svcs && svcs.rf_enabled === false) {
        if (rfBanner) rfBanner.style.display = 'block';
        if (rfBadge) {
          rfBadge.textContent = 'RF DISABLED';
          rfBadge.className = 'badge';
          rfBadge.style.background = 'rgba(255,183,3,0.15)';
          rfBadge.style.color = 'var(--warning)';
        }
        if (rfPairBtn) rfPairBtn.disabled = true;
        if (rfUnpairBtn) rfUnpairBtn.disabled = true;
      } else {
        if (rfBanner) rfBanner.style.display = 'none';
        if (rfPairBtn) rfPairBtn.disabled = false;
        if (rfUnpairBtn) rfUnpairBtn.disabled = false;
      }
    } catch (e) {
      console.warn('Services check failed:', e);
    }

    const sel = document.getElementById('eliteActivitySelect');
    const prevActVal = this.eliteSelectedActivityId || sel?.value || '-1';
    const filteredActs = (this.activitiesData.activities || []).filter(a => {
      const idStr = String(a.id);
      const name = String(a.name || a.label || '').toLowerCase().replace(/\s+/g, '');
      return idStr !== '-1' && name !== 'poweroff';
    });
    if (sel) {
      sel.innerHTML = '<option value="-1">Off / Idle (Default)</option>' +
        filteredActs.map(a => `<option value="${a.id}">${a.name || a.label}</option>`).join('');
      if (prevActVal && (prevActVal === '-1' || filteredActs.some(a => String(a.id) === String(prevActVal)))) {
        sel.value = prevActVal;
      }
      this.eliteSelectedActivityId = sel.value;
    }
    const tgtDev = document.getElementById('eliteTargetDevice');
    const presetDev = document.getElementById('elitePresetDevice');
    const tgtAct = document.getElementById('eliteTargetActivity');
    const devs = this.inventory.devices || [];
    if (tgtDev) tgtDev.innerHTML = devs.map(d => `<option value="${d.id}">${d.name || d.id}</option>`).join('');
    if (presetDev) presetDev.innerHTML = devs.map(d => `<option value="${d.id}">${d.name || d.id}</option>`).join('');
    if (tgtAct) tgtAct.innerHTML = filteredActs.map(a => `<option value="${a.id}">${a.name || a.label}</option>`).join('');

    this.eliteRemoteMode = 0;
    this.eliteDirectPage = 1;
    this.eliteDirectData = { page1: [], page2: [], buttons: {} };

    try {
      const rfCfgResp = await fetch('/api/rf/remote-config?slot=1');
      if (rfCfgResp.ok) {
        const rc = await rfCfgResp.json();
        this.eliteRemoteMode = rc.remote_mode || 0;
      }
    } catch (e) {
      console.warn('Could not read remote mode:', e);
    }

    const actCard = document.getElementById('eliteActivitySelectCard');
    const directCard = document.getElementById('eliteDirectModeCard');
    const pageTabs = document.getElementById('eliteDirectPageTabs');
    const screenTitle = document.getElementById('eliteScreenSectionTitle');
    const screenHelp = document.getElementById('eliteScreenSectionHelp');

    if (this.eliteRemoteMode === 1) {
      if (actCard) actCard.style.display = 'none';
      if (directCard) directCard.style.display = 'block';
      if (pageTabs) pageTabs.style.display = 'flex';
      if (screenTitle) screenTitle.textContent = '3. On-Screen Direct Buttons (Page 1 & 2)';
      if (screenHelp) screenHelp.textContent = 'Fixed buttons shown on the 2 pages of the Elite remote screen.';

      try {
        const dresp = await fetch('/api/rf/remote-direct-buttons?slot=1');
        if (dresp.ok) {
          const dj = await dresp.json();
          if (dj) this.eliteDirectData = dj;
        }
      } catch (e) {
        console.warn('Failed to load direct buttons:', e);
      }
    } else {
      if (actCard) actCard.style.display = 'block';
      if (directCard) directCard.style.display = 'none';
      if (pageTabs) pageTabs.style.display = 'none';
      if (screenTitle) screenTitle.textContent = '3. On-Screen "DEVICES" Touch Buttons';
      if (screenHelp) screenHelp.textContent = 'Buttons shown on the Elite touchscreen when pressing "DEVICES" during this activity (up to 48 buttons, 6 per page).';
    }

    try {
      const m = await this.client.getEliteMapping();
      if (m && m.activities) this.eliteMapData = m;
    } catch (e) {
      console.warn('Failed to load Elite mappings:', e);
    }

    this.selectEliteButton('Select', false);
    this.updateEliteButtonStates();
    this.renderEliteScreenButtons();
  }

  selectEliteButton(btnId, openModal = false) {
    this.eliteSelectedBtn = btnId;
    document.querySelectorAll('.elite-hotspot').forEach(b => {
      b.classList.toggle('selected', b.getAttribute('data-btn') === btnId);
    });
    const lbl = document.getElementById('eliteSelectedBtnLabel');
    if (lbl) lbl.textContent = btnId;

    const actSel = document.getElementById('eliteActivitySelect');
    if (actSel && this.eliteSelectedActivityId && actSel.value !== this.eliteSelectedActivityId) {
      actSel.value = this.eliteSelectedActivityId;
    }
    const actId = this.eliteSelectedActivityId || actSel?.value || '-1';
    const actName = actSel ? actSel.options[actSel.selectedIndex]?.text || 'Off / Idle' : 'Off / Idle';
    const modalActLbl = document.getElementById('eliteModalActivityName');
    if (modalActLbl) modalActLbl.textContent = actName;

    let mapping = null;
    if (this.eliteRemoteMode === 1) {
      mapping = this.eliteDirectData?.buttons?.[btnId];
    } else {
      mapping = this.eliteMapData?.activities?.[actId]?.[btnId];
    }

    const typeSel = document.getElementById('eliteActionType');
    if (typeSel) {
      if (!mapping) typeSel.value = 'none';
      else if (mapping.type === 'activity_start' || mapping.action === 'activity_start') typeSel.value = 'activity_start';
      else if (mapping.type === 'activity_stop' || mapping.action === 'activity_stop' || mapping.action === 'poweroff') typeSel.value = 'activity_stop';
      else if (mapping.type) typeSel.value = mapping.type;
      else if (mapping.action === 'device_cmd' || mapping.command) typeSel.value = 'command';
      else typeSel.value = 'none';
      this.toggleEliteActionFields();
    }

    if (mapping && (mapping.activityId || mapping.targetActivity)) {
      const actTgt = document.getElementById('eliteTargetActivity');
      if (actTgt) actTgt.value = mapping.activityId || mapping.targetActivity;
    }
    if (mapping && mapping.deviceId) {
      const devSel = document.getElementById('eliteTargetDevice');
      if (devSel) devSel.value = mapping.deviceId;
    }
    if (mapping && mapping.sequence) {
      const seqSel = document.getElementById('eliteTargetSequence');
      if (seqSel) seqSel.value = mapping.sequence;
    }
    if (mapping && mapping.script) {
      const scrField = document.getElementById('eliteTargetScript');
      if (scrField) scrField.value = mapping.script;
    }
    this.updateEliteCommandDropdown(mapping?.command);

    const stat = document.getElementById('eliteSelectedBtnStatus');
    if (stat) {
      const isMapped = Boolean(mapping);
      stat.className = `badge ${isMapped ? 'ok' : 'subtle'}`;
      stat.textContent = isMapped ? 'Mapped' : 'Unmapped';
    }

    if (openModal) {
      const m = document.getElementById('eliteBtnModal');
      if (m) m.style.display = 'flex';
    }
  }

  updateEliteCommandDropdown(curVal) {
    const devId = document.getElementById('eliteTargetDevice')?.value;
    const cmdSel = document.getElementById('eliteTargetCommand');
    if (!cmdSel) return;
    const dev = (this.inventory.devices || []).find(d => String(d.id) === String(devId));
    const cmds = (dev?.commands || []).slice().sort((a, b) => (a.name || '').localeCompare(b.name || '', undefined, { sensitivity: 'base' }));
    cmdSel.innerHTML = cmds.map(c => `<option value="${c.name}" ${c.name === curVal ? 'selected' : ''}>${c.name}</option>`).join('');
  }

  toggleEliteActionFields() {
    const t = document.getElementById('eliteActionType')?.value;
    const wCmd = document.getElementById('eliteCommandFields');
    const wAct = document.getElementById('eliteActivityFields');
    const wSeq = document.getElementById('eliteSequenceFields');
    const wScr = document.getElementById('eliteScriptFields');

    if (wCmd) wCmd.style.display = t === 'command' ? 'block' : 'none';
    if (wAct) wAct.style.display = t === 'activity_start' ? 'block' : 'none';
    if (wSeq) wSeq.style.display = t === 'sequence' ? 'block' : 'none';
    if (wScr) wScr.style.display = t === 'script' ? 'block' : 'none';
  }

  applyEliteMapping() {
    const type = document.getElementById('eliteActionType')?.value;
    let mapObj = null;

    if (type === 'command') {
      mapObj = {
        type: 'command',
        action: 'device_cmd',
        deviceId: document.getElementById('eliteTargetDevice')?.value,
        command: document.getElementById('eliteTargetCommand')?.value
      };
    } else if (type === 'activity_start') {
      mapObj = {
        type: 'activity_start',
        action: 'activity_start',
        activityId: document.getElementById('eliteTargetActivity')?.value
      };
    } else if (type === 'activity_stop') {
      mapObj = {
        type: 'activity_stop',
        action: 'activity_stop'
      };
    } else if (type === 'sequence') {
      mapObj = {
        type: 'sequence',
        action: 'sequence',
        sequence: document.getElementById('eliteTargetSequence')?.value
      };
    } else if (type === 'script') {
      mapObj = {
        type: 'script',
        action: 'script',
        script: document.getElementById('eliteTargetScript')?.value
      };
    }

    if (this.eliteRemoteMode === 1) {
      if (!this.eliteDirectData.buttons) this.eliteDirectData.buttons = {};
      if (type === 'none' || !mapObj) {
        delete this.eliteDirectData.buttons[this.eliteSelectedBtn];
      } else {
        this.eliteDirectData.buttons[this.eliteSelectedBtn] = mapObj;
      }
    } else {
      const actId = this.eliteSelectedActivityId || document.getElementById('eliteActivitySelect')?.value || '-1';
      if (!this.eliteMapData.activities) this.eliteMapData.activities = {};
      if (!this.eliteMapData.activities[actId]) this.eliteMapData.activities[actId] = {};

      if (type === 'none' || !mapObj) {
        delete this.eliteMapData.activities[actId][this.eliteSelectedBtn];
      } else {
        this.eliteMapData.activities[actId][this.eliteSelectedBtn] = mapObj;
      }
    }
    this.updateEliteButtonStates();
    const modal = document.getElementById('eliteBtnModal');
    if (modal) modal.style.display = 'none';
  }

  clearEliteMapping() {
    if (this.eliteRemoteMode === 1) {
      if (this.eliteDirectData?.buttons) {
        delete this.eliteDirectData.buttons[this.eliteSelectedBtn];
      }
    } else {
      const actId = this.eliteSelectedActivityId || document.getElementById('eliteActivitySelect')?.value || '-1';
      if (this.eliteMapData.activities?.[actId]) {
        delete this.eliteMapData.activities[actId][this.eliteSelectedBtn];
      }
    }
    this.selectEliteButton(this.eliteSelectedBtn, false);
    this.updateEliteButtonStates();
    const modal = document.getElementById('eliteBtnModal');
    if (modal) modal.style.display = 'none';
  }

  updateEliteButtonStates() {
    let actMap = {};
    if (this.eliteRemoteMode === 1) {
      actMap = this.eliteDirectData?.buttons || {};
    } else {
      const actId = this.eliteSelectedActivityId || document.getElementById('eliteActivitySelect')?.value || '-1';
      actMap = this.eliteMapData?.activities?.[actId] || {};
    }
    document.querySelectorAll('.elite-hotspot').forEach(b => {
      const id = b.getAttribute('data-btn');
      b.classList.toggle('mapped', Boolean(actMap[id]));
    });
    const stat = document.getElementById('eliteSelectedBtnStatus');
    if (stat) {
      const isMapped = Boolean(actMap[this.eliteSelectedBtn]);
      stat.className = `badge ${isMapped ? 'ok' : 'subtle'}`;
      stat.textContent = isMapped ? 'Mapped' : 'Unmapped';
    }
  }

  async saveAllEliteMappings() {
    const btn = document.getElementById('eliteSaveAllBtn');
    try {
      if (btn) btn.textContent = 'Saving...';
      if (this.eliteRemoteMode === 1) {
        const resp = await fetch('/api/rf/remote-direct-buttons?slot=1', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify(this.eliteDirectData)
        });
        if (resp.ok) {
          this.showToast('Harmony Elite direct buttons saved & pushed to remote!', 'success');
        } else {
          this.showToast('Failed to save direct buttons.', 'danger');
        }
      } else {
        await this.client.saveEliteMapping(this.eliteMapData);
        this.showToast('Harmony Elite mappings saved to Hub!', 'success');
      }
    } catch (err) {
      this.showToast('Save failed: ' + (err.message || err), 'danger');
    } finally {
      if (btn) btn.textContent = 'Save All to Hub';
    }
  }

  async pushEliteConfiguration() {
    const btn = document.getElementById('elitePushRemoteBtn');
    try {
      if (btn) btn.textContent = 'Pushing...';
      const res = await this.client.pushRfConfig();
      if (res && res.status === 'push_started') {
        this.showToast('Configuration push started over RF to Elite remote!', 'success');
      } else {
        this.showToast('Configuration push triggered successfully.', 'success');
      }
    } catch (err) {
      this.showToast('Push failed: ' + (err.message || err), 'danger');
    } finally {
      if (btn) btn.textContent = 'Push to Elite Remote';
    }
  }

  autoMapElitePreset() {
    const devId = document.getElementById('elitePresetDevice')?.value;
    const actId = this.eliteSelectedActivityId || document.getElementById('eliteActivitySelect')?.value || '-1';
    if (!devId) return alert('Select target device first.');
    const dev = (this.inventory.devices || []).find(d => String(d.id) === String(devId));
    if (!dev || !dev.commands) return alert('Device has no commands.');

    const norm = s => String(s || '').toLowerCase().replace(/[^a-z0-9]/g, '');
    const aliases = {
      PowerOffActivity: ['poweroff', 'power off', 'standby', 'power'],
      DirectionUp: ['up', 'directionup'],
      DirectionDown: ['down', 'directiondown'],
      DirectionLeft: ['left', 'directionleft'],
      DirectionRight: ['right', 'directionright'],
      Select: ['ok', 'select', 'enter'],
      Back: ['back', 'return', 'exit'],
      Exit: ['exit', 'cancel'],
      VolumeUp: ['volumeup', 'volup', 'vol up'],
      VolumeDown: ['volumedown', 'voldown', 'vol down'],
      VolumeMute: ['mute', 'volumemute'],
      ChannelUp: ['channelup', 'chup', 'pageup'],
      ChannelDown: ['channeldown', 'chdown', 'pagedown'],
      Rewind: ['rewind', 'rev'],
      Play: ['play', 'playpause'],
      FastForward: ['fastforward', 'forward', 'ffwd'],
      Pause: ['pause'],
      Stop: ['stop'],
      Record: ['record', 'rec'],
      Menu: ['menu', 'topmenu', 'homemenu'],
      Info: ['info', 'display', 'guide'],
      Guide: ['guide', 'epg']
    };

    if (!this.eliteMapData.activities) this.eliteMapData.activities = {};
    if (!this.eliteMapData.activities[actId]) this.eliteMapData.activities[actId] = {};

    let mapped = 0;
    Object.entries(aliases).forEach(([btn, list]) => {
      const cmd = dev.commands.find(c => list.some(a => norm(c.name) === norm(a)));
      if (cmd) {
        this.eliteMapData.activities[actId][btn] = {
          type: 'command',
          deviceId: devId,
          command: cmd.name
        };
        mapped++;
      }
    });

    this.updateEliteButtonStates();
    alert(`Auto-mapped ${mapped} buttons for device! Click "Save All to Hub" to persist.`);
  }

  renderEliteScreenButtons() {
    const sbtnsList = document.getElementById('eliteScreenButtonsList');
    const preview = document.getElementById('eliteScreenPreview');
    const devs = this.inventory.devices || [];

    let sbtns;
    if (this.eliteRemoteMode === 1) {
      if (!this.eliteDirectData) this.eliteDirectData = { physical: {}, page1: [], page2: [] };
      const pKey = this.eliteDirectPage === 2 ? 'page2' : 'page1';
      if (!Array.isArray(this.eliteDirectData[pKey])) {
        this.eliteDirectData[pKey] = [];
      }
      sbtns = this.eliteDirectData[pKey];
    } else {
      const actId = this.eliteSelectedActivityId || document.getElementById('eliteActivitySelect')?.value || '-1';
      if (!this.eliteMapData.activities) this.eliteMapData.activities = {};
      if (!this.eliteMapData.activities[actId]) this.eliteMapData.activities[actId] = {};
      const actObj = this.eliteMapData.activities[actId];
      if (!Array.isArray(actObj.screen_buttons)) {
        actObj.screen_buttons = [];
      }
      sbtns = actObj.screen_buttons;
    }

    // Update screen preview inside the vector remote visual
    if (preview) {
      if (sbtns.length === 0) {
        preview.innerHTML = `
          <div style="display:flex;flex-direction:column;align-items:center;justify-content:center;min-height:90px;color:#64748b;font-size:8px;text-align:center;padding:12px 4px;gap:4px;">
            <span style="font-size:12px;opacity:0.6;">📱</span>
            <span>No Screen Buttons</span>
            <span style="font-size:7px;opacity:0.7;">Add buttons below</span>
          </div>
        `;
      } else {
        preview.innerHTML = sbtns.slice(0, 6).map((b, idx) => {
          const text = b.label || 'Btn ' + (idx + 1);
          return `
            <div class="elite-screen-tile" title="${text}" style="display:flex;align-items:center;justify-content:center;gap:4px;overflow:hidden;padding:0 3px;">
              ${getScreenBtnIconSvg(b.icon, b.label, 11)}
              <span style="overflow:hidden;text-overflow:ellipsis;white-space:nowrap;">${text}</span>
            </div>
          `;
        }).join('');
      }
    }

    // Update screen buttons list editor
    if (!sbtnsList) return;
    if (sbtns.length === 0) {
      const msg = this.eliteRemoteMode === 1
        ? `No touch buttons configured for ${this.eliteDirectPage === 2 ? 'Page 2' : 'Page 1'} yet. Click "+ Add Screen Button" above.`
        : `No touch buttons configured for this activity yet. Click "+ Add Screen Button" above.`;
      sbtnsList.innerHTML = `<div class="subtle" style="font-size: 13px; padding: 8px 0;">${msg}</div>`;
      return;
    }

    sbtnsList.innerHTML = sbtns.map((btn, idx) => {
      const devOpts = devs.map(d => `<option value="${d.id}" ${String(d.id) === String(btn.deviceId) ? 'selected' : ''}>${d.name || d.id}</option>`).join('');
      const curDev = devs.find(d => String(d.id) === String(btn.deviceId)) || devs[0];
      const cmds = curDev?.commands || [];
      const cmdOpts = cmds.map(c => `<option value="${c.name}" ${c.name === btn.command ? 'selected' : ''}>${c.name}</option>`).join('');
      const iconUrl = btn.icon ? getStockIconUrl(btn.icon) : '';
      const iconDispName = btn.icon ? this.formatIconDisplayName(btn.icon) : 'Pick Icon';

      return `
        <div class="screen-btn-row" data-idx="${idx}">
          <span class="screen-btn-num">${idx + 1}</span>
          <input type="text" class="form-control screen-btn-label" maxlength="12" value="${btn.label || ''}" placeholder="Label (max 12)" style="font-size: 12px; padding: 4px 8px;">
          <button type="button" class="btn btn-xs btn-secondary btn-screen-btn-icon" title="Choose Icon" style="display: inline-flex; align-items: center; gap: 6px; padding: 4px 8px; font-size: 11px; height: 30px; text-align: left; overflow: hidden;">
            ${iconUrl ? `<img src="${iconUrl}" class="stock-icon-img invert-white" style="width: 16px; height: 16px; filter: brightness(0) invert(1); flex-shrink: 0;" onerror="this.style.display='none'" />` : '<span style="opacity: 0.6; font-size: 13px;">🖼</span>'}
            <span class="screen-btn-icon-text" style="overflow: hidden; text-overflow: ellipsis; white-space: nowrap; max-width: 95px;">${iconDispName}</span>
          </button>
          <select class="form-control screen-btn-dev" style="font-size: 12px; padding: 4px 6px;">
            ${devOpts}
          </select>
          <select class="form-control screen-btn-cmd" style="font-size: 12px; padding: 4px 6px;">
            ${cmdOpts}
          </select>
          <button type="button" class="btn btn-xs btn-ghost screen-btn-del" title="Remove" style="color: var(--danger); font-size: 14px; padding: 2px 6px;">✕</button>
        </div>
      `;
    }).join('');

    // Attach row events
    sbtnsList.querySelectorAll('.screen-btn-row').forEach(row => {
      const idx = parseInt(row.getAttribute('data-idx'), 10);
      const lblIn = row.querySelector('.screen-btn-label');
      const iconBtn = row.querySelector('.btn-screen-btn-icon');
      const devSel = row.querySelector('.screen-btn-dev');
      const cmdSel = row.querySelector('.screen-btn-cmd');
      const delBtn = row.querySelector('.screen-btn-del');

      const updatePreviewTile = () => {
        if (preview && idx < 6) {
          const tiles = preview.querySelectorAll('.elite-screen-tile');
          if (tiles[idx]) {
            const text = sbtns[idx].label || `Btn ${idx + 1}`;
            tiles[idx].innerHTML = `
              ${getScreenBtnIconSvg(sbtns[idx].icon, sbtns[idx].label, 11)}
              <span style="overflow:hidden;text-overflow:ellipsis;white-space:nowrap;">${text}</span>
            `;
            tiles[idx].setAttribute('title', text);
          }
        }
      };

      lblIn?.addEventListener('input', (e) => {
        sbtns[idx].label = e.target.value.trim();
        updatePreviewTile();
      });

      iconBtn?.addEventListener('click', () => {
        this.openIconPicker((selectedIcon) => {
          sbtns[idx].icon = selectedIcon;
          this.renderEliteScreenButtons();
        }, sbtns[idx].icon);
      });

      devSel?.addEventListener('change', (e) => {
        const newDevId = e.target.value;
        sbtns[idx].deviceId = newDevId;
        const d = devs.find(x => String(x.id) === String(newDevId));
        if (cmdSel && d) {
          cmdSel.innerHTML = (d.commands || []).map(c => `<option value="${c.name}">${c.name}</option>`).join('');
          sbtns[idx].command = d.commands?.[0]?.name || '';
        }
      });

      cmdSel?.addEventListener('change', (e) => {
        sbtns[idx].command = e.target.value;
      });

      delBtn?.addEventListener('click', () => {
        sbtns.splice(idx, 1);
        this.renderEliteScreenButtons();
      });
    });
  }

  addEliteScreenButton() {
    let sbtns;
    if (this.eliteRemoteMode === 1) {
      if (!this.eliteDirectData) this.eliteDirectData = { physical: {}, page1: [], page2: [] };
      const pKey = this.eliteDirectPage === 2 ? 'page2' : 'page1';
      if (!Array.isArray(this.eliteDirectData[pKey])) {
        this.eliteDirectData[pKey] = [];
      }
      sbtns = this.eliteDirectData[pKey];
    } else {
      const actId = this.eliteSelectedActivityId || document.getElementById('eliteActivitySelect')?.value || '-1';
      if (!this.eliteMapData.activities) this.eliteMapData.activities = {};
      if (!this.eliteMapData.activities[actId]) this.eliteMapData.activities[actId] = {};
      const actObj = this.eliteMapData.activities[actId];
      if (!Array.isArray(actObj.screen_buttons)) {
        actObj.screen_buttons = [];
      }
      sbtns = actObj.screen_buttons;
    }

    if (sbtns.length >= 48) {
      return alert('Maximum of 48 screen buttons reached.');
    }
    const devs = this.inventory.devices || [];
    const firstDev = devs[0];
    const firstCmd = firstDev?.commands?.[0]?.name || '';
    sbtns.push({
      label: `Btn ${sbtns.length + 1}`,
      icon: '',
      deviceId: firstDev ? String(firstDev.id) : '',
      command: firstCmd
    });
    this.renderEliteScreenButtons();
  }

  async refreshRfStatus() {
    try {
      const st = await this.client.getRfStatus();
      const fw = document.getElementById('rfFwVer');
      const hubAddr = document.getElementById('rfHubAddr');
      const state = document.getElementById('rfPairState');
      const badge = document.getElementById('rfStatusBadge');
      const pBtn = document.getElementById('rfPairBtn');
      const sBtn = document.getElementById('rfStopPairBtn');
      const msg = document.getElementById('rfPairMsg');

      if (st) {
        if (fw) fw.textContent = st.fw_version || 'Unknown';
        const hAddr = st.hub_rf_address || st.rf_address || '';
        const hasHubAddr = Boolean(hAddr && hAddr !== 'None' && hAddr !== '00000000');
        if (hubAddr) hubAddr.textContent = hasHubAddr ? hAddr : 'None';
        if (state) state.textContent = st.pairing_active ? 'PAIRING ACTIVE' : 'IDLE';
        if (badge) {
          const isOnline = st.running !== false && st.state !== 'stopped';
          badge.className = `badge ${isOnline ? 'ok' : 'bad'}`;
          badge.textContent = isOnline ? 'ONLINE' : 'OFFLINE';
        }

        // Slot 1: Elite
        const s1 = (st.slots && st.slots.find(s => s.slot === 1)) || { active: st.paired, address: null, rf_address: null };
        const b1 = document.getElementById('rfSlot1Badge');
        const a1 = document.getElementById('rfSlot1Addr');
        const batt1 = document.getElementById('rfSlot1Battery');
        const ver1 = document.getElementById('rfSlot1Ver');
        const cfgVer1 = document.getElementById('rfSlot1CfgVer');
        const otaWrap1 = document.getElementById('rfSlot1UpdateWrap');
        const s1Addr = (s1 && (s1.address || s1.rf_address)) || '';
        const s1Active = Boolean((s1.active || st.paired) && s1Addr && s1Addr !== 'None' && s1Addr !== '00000000');
        if (b1) {
          b1.className = `badge ${s1Active ? 'ok' : 'subtle'}`;
          b1.textContent = s1Active ? 'PAIRED' : 'EMPTY';
        }
        if (a1) a1.textContent = s1Active ? s1Addr : 'None';
        if (batt1) {
          if (s1.battery && typeof s1.battery.percent === 'number') {
            const chg = s1.battery.charging ? ' ⚡ (Charging)' : '';
            batt1.textContent = `${s1.battery.percent}% (${s1.battery.voltage_mv} mV)${chg}`;
          } else {
            batt1.textContent = s1Active ? 'Waiting for report...' : '--';
          }
        }
        if (ver1) ver1.textContent = s1.sw_version || 'Unknown';
        if (cfgVer1) {
          if (!s1Active) {
            cfgVer1.textContent = '--';
          } else {
            const cVer = s1.config_version ? `v${s1.config_version}` : 'v0';
            const inSync = s1.in_sync !== false;
            const bCls = inSync ? 'ok' : 'warn';
            const bTxt = inSync ? 'IN SYNC' : `OUT OF SYNC (Hub: v${st.hub_config_version || 0})`;
            cfgVer1.innerHTML = `${cVer} <span class="badge ${bCls} mini" style="font-size:10px;padding:1px 5px;margin-left:4px;">${bTxt}</span>`;
          }
        }
        if (otaWrap1) {
          const latestElite = this.latestEliteVersion || '1.3.0';
          const needsOta = s1Active && s1.sw_version && s1.sw_version !== 'unknown' && this.isVersionOlder(s1.sw_version, latestElite);
          otaWrap1.style.display = needsOta ? 'block' : 'none';
        }

        // Slot 2: Companion
        const s2 = (st.slots && st.slots.find(s => s.slot === 2)) || { active: false, address: null, rf_address: null };
        const b2 = document.getElementById('rfSlot2Badge');
        const a2 = document.getElementById('rfSlot2Addr');
        const batt2 = document.getElementById('rfSlot2Battery');
        const ver2 = document.getElementById('rfSlot2Ver');
        const cfgVer2 = document.getElementById('rfSlot2CfgVer');
        const otaWrap2 = document.getElementById('rfSlot2UpdateWrap');
        const s2Addr = (s2 && (s2.address || s2.rf_address)) || '';
        const s2Active = Boolean(s2.active && s2Addr && s2Addr !== 'None' && s2Addr !== '00000000');
        if (b2) {
          b2.className = `badge ${s2Active ? 'ok' : 'subtle'}`;
          b2.textContent = s2Active ? 'PAIRED' : 'EMPTY';
        }
        if (a2) a2.textContent = s2Active ? s2Addr : 'None';
        if (batt2) {
          if (s2.battery && typeof s2.battery.percent === 'number') {
            const chg = s2.battery.charging ? ' ⚡ (Charging)' : '';
            batt2.textContent = `${s2.battery.percent}% (${s2.battery.voltage_mv} mV)${chg}`;
          } else {
            batt2.textContent = s2Active ? 'Waiting for report...' : '--';
          }
        }
        if (ver2) ver2.textContent = s2.sw_version || 'Unknown';
        if (cfgVer2) {
          if (!s2Active) {
            cfgVer2.textContent = '--';
          } else {
            const cVer = s2.config_version ? `v${s2.config_version}` : 'v0';
            const inSync = s2.in_sync !== false;
            const bCls = inSync ? 'ok' : 'warn';
            const bTxt = inSync ? 'IN SYNC' : `OUT OF SYNC (Hub: v${st.hub_config_version || 0})`;
            cfgVer2.innerHTML = `${cVer} <span class="badge ${bCls} mini" style="font-size:10px;padding:1px 5px;margin-left:4px;">${bTxt}</span>`;
          }
        }
        if (otaWrap2) {
          const latestElite = this.latestEliteVersion || '1.3.0';
          const needsOta = s2Active && s2.sw_version && s2.sw_version !== 'unknown' && this.isVersionOlder(s2.sw_version, latestElite);
          otaWrap2.style.display = needsOta ? 'block' : 'none';
        }

        if (pBtn) pBtn.style.display = st.pairing_active ? 'none' : 'inline-block';
        if (sBtn) sBtn.style.display = st.pairing_active ? 'inline-block' : 'none';
        if (msg) {
          if (st.pairing_active) {
            msg.style.display = 'block';
            msg.textContent = t('rf_pairing_active_note', 'Hub pairing active! On your Elite remote, navigate to Settings and enable pairing.');
          } else {
            msg.style.display = 'none';
          }
        }
      }
    } catch {
      const badge = document.getElementById('rfStatusBadge');
      if (badge) {
        badge.className = 'badge bad';
        badge.textContent = 'OFFLINE';
      }
    }
  }

  async openRfConfigModal(slot) {
    const modal = document.getElementById('rfConfigModal');
    const title = document.getElementById('rfConfigModalTitle');
    const slotInput = document.getElementById('rfConfigSlot');
    if (!modal) return;
    if (slotInput) slotInput.value = slot;
    if (title) title.textContent = `Remote Settings (Slot ${slot})`;
    modal.style.display = 'flex';

    try {
      const resp = await fetch(`/api/rf/remote-config?slot=${slot}`);
      if (resp.ok) {
        const cfg = await resp.json();
        if (cfg.brightness) document.getElementById('rfCfgBrightness').value = cfg.brightness;
        if (cfg.timeout) document.getElementById('rfCfgTimeout').value = cfg.timeout;

        const touchVal = typeof cfg.touch !== 'undefined' ? cfg.touch : (typeof cfg.touchEnabled !== 'undefined' ? (cfg.touchEnabled ? 1 : 0) : 1);
        document.getElementById('rfCfgTouch').value = String(touchVal ? 1 : 0);

        const touchSens = cfg.touch_sensitivity || cfg.touchSensitivity || 2;
        document.getElementById('rfCfgTouchSens').value = String(touchSens);

        let remoteMode = typeof cfg.remote_mode !== 'undefined' ? cfg.remote_mode : (cfg.remoteMode || 0);
        if (cfg.no_screen || cfg.noScreen) {
          remoteMode = 2;
        }
        document.getElementById('rfCfgRemoteMode').value = String(remoteMode);

        const navKey = cfg.nav_key || cfg.navToggleKey || 139;
        document.getElementById('rfCfgNavKey').value = String(navKey);

        const navMode = typeof cfg.nav_mode !== 'undefined' ? cfg.nav_mode : (cfg.navToggleMode || 0);
        document.getElementById('rfCfgNavMode').value = String(navMode);

        const confirmAct = typeof cfg.confirm_act !== 'undefined' ? cfg.confirm_act : (cfg.confirmActivity ? 1 : 0);
        document.getElementById('rfCfgConfirmAct').value = String(confirmAct ? 1 : 0);

        const gyroVal = typeof cfg.gyro !== 'undefined' ? cfg.gyro : (typeof cfg.gyroEnabled !== 'undefined' ? (cfg.gyroEnabled ? 1 : 0) : 1);
        if (document.getElementById('rfCfgGyro')) document.getElementById('rfCfgGyro').value = String(gyroVal ? 1 : 0);

        const gyroRate = cfg.gyro_rate || cfg.gyroRate || 25;
        if (document.getElementById('rfCfgGyroRate')) document.getElementById('rfCfgGyroRate').value = String(gyroRate);

        const btnBacklight = typeof cfg.btn_backlight !== 'undefined' ? cfg.btn_backlight : (typeof cfg.btnBacklight !== 'undefined' ? cfg.btnBacklight : 1);
        if (document.getElementById('rfCfgBtnBacklight')) document.getElementById('rfCfgBtnBacklight').value = String(btnBacklight);

        const nightStart = typeof cfg.night_start !== 'undefined' ? cfg.night_start : (typeof cfg.nightStart !== 'undefined' ? cfg.nightStart : 22);
        if (document.getElementById('rfCfgNightStart')) document.getElementById('rfCfgNightStart').value = String(nightStart);

        const nightEnd = typeof cfg.night_end !== 'undefined' ? cfg.night_end : (typeof cfg.nightEnd !== 'undefined' ? cfg.nightEnd : 7);
        if (document.getElementById('rfCfgNightEnd')) document.getElementById('rfCfgNightEnd').value = String(nightEnd);

        const usbEth = typeof cfg.usbeth !== 'undefined' ? cfg.usbeth : (typeof cfg.usbEth !== 'undefined' ? (cfg.usbEth ? 1 : 0) : 0);
        if (document.getElementById('rfCfgUsbEth')) document.getElementById('rfCfgUsbEth').value = String(usbEth ? 1 : 0);
      }
    } catch (e) {
      console.warn('Could not fetch existing remote config:', e);
    }
  }

  closeRfConfigModal() {
    const modal = document.getElementById('rfConfigModal');
    if (modal) modal.style.display = 'none';
  }

  async saveRfConfig() {
    const slot = parseInt(document.getElementById('rfConfigSlot')?.value || '1', 10);
    const bright = parseInt(document.getElementById('rfCfgBrightness')?.value || '3', 10);
    const timeout = parseInt(document.getElementById('rfCfgTimeout')?.value || '120', 10);
    const touch = parseInt(document.getElementById('rfCfgTouch')?.value || '1', 10);
    const touch_sensitivity = parseInt(document.getElementById('rfCfgTouchSens')?.value || '2', 10);
    let remote_mode = parseInt(document.getElementById('rfCfgRemoteMode')?.value || '0', 10);
    const nav_key = parseInt(document.getElementById('rfCfgNavKey')?.value || '139', 10);
    const nav_mode = parseInt(document.getElementById('rfCfgNavMode')?.value || '0', 10);
    const confirm_act = parseInt(document.getElementById('rfCfgConfirmAct')?.value || '0', 10);
    const gyro = parseInt(document.getElementById('rfCfgGyro')?.value || '1', 10);
    const gyro_rate = parseInt(document.getElementById('rfCfgGyroRate')?.value || '25', 10);
    const btn_backlight = parseInt(document.getElementById('rfCfgBtnBacklight')?.value || '1', 10);
    const night_start = parseInt(document.getElementById('rfCfgNightStart')?.value || '22', 10);
    const night_end = parseInt(document.getElementById('rfCfgNightEnd')?.value || '7', 10);
    const usbeth = parseInt(document.getElementById('rfCfgUsbEth')?.value || '0', 10);

    let no_screen = 0;
    if (remote_mode === 2) {
      const confirm1 = await showConfirm(
        'WARNING: You are enabling No-Screen Mode. This disables the LCD display, touch digitizer, and backlight to conserve power on remotes with physically broken screens. Physical buttons will continue controlling the Hub. Continue?',
        { title: 'Enable No-Screen Mode', okText: 'I Understand, Continue', danger: true }
      );
      if (!confirm1) return;
      const confirm2 = await showConfirm(
        'CONFIRM BLACK SCREEN: The remote screen and backlight will turn off immediately upon receiving this configuration. Are you sure?',
        { title: 'Confirm Black Screen', okText: 'Yes, Disable Screen', danger: true }
      );
      if (!confirm2) return;
      no_screen = 1;
      remote_mode = 0;
    }

    const payload = {
      slot: slot,
      brightness: bright,
      timeout: timeout,
      touch: touch,
      touchEnabled: touch === 1,
      touch_sensitivity: touch_sensitivity,
      touchSensitivity: touch_sensitivity,
      remote_mode: remote_mode,
      remoteMode: remote_mode,
      nav_key: nav_key,
      navToggleKey: nav_key,
      nav_mode: nav_mode,
      navToggleMode: nav_mode,
      confirm_act: confirm_act,
      confirmActivity: confirm_act === 1,
      gyro: gyro,
      gyroEnabled: gyro === 1,
      gyro_rate: gyro_rate,
      gyroRate: gyro_rate,
      btn_backlight: btn_backlight,
      btnBacklight: btn_backlight,
      night_start: night_start,
      nightStart: night_start,
      night_end: night_end,
      nightEnd: night_end,
      usbeth: usbeth,
      usbEth: usbeth === 1,
      no_screen: no_screen,
      noScreen: no_screen === 1
    };

    const saveBtn = document.getElementById('rfConfigSaveBtn');
    if (saveBtn) saveBtn.textContent = 'Pushing to remote...';
    try {
      const resp = await fetch(`/api/rf/remote-config?slot=${slot}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(payload)
      });
      if (resp.ok) {
        alert(t('rf_config_saved', 'Settings saved and pushed over RF to remote!'));
        this.closeRfConfigModal();
        await this.loadEliteView();
      } else {
        alert(t('rf_config_failed', 'Failed to save settings to Hub.'));
      }
    } catch (err) {
      alert(t('rf_config_error', 'Error pushing settings: ') + err.message);
    } finally {
      if (saveBtn) saveBtn.textContent = 'Save & Push to Remote';
    }
  }

  async pullConfigFromRemote(slot) {
    const slotName = slot === 1 ? 'Slot 1' : 'Slot 2';
    if (!await showConfirm(`Pull configuration from Remote in ${slotName} to Hub? This will overwrite the Hub's saved remote configuration with what is currently running on the remote.`, { title: `Pull Config (${slotName})`, okText: 'Pull Config', danger: false })) return;
    try {
      await this.client.pullRfConfig(slot);
      alert(`Config pull requested from Remote (${slotName}). Wait a few seconds for remote to reply and check sync status.`);
      setTimeout(() => this.refreshRfStatus(), 3000);
    } catch (err) {
      alert(t('error', 'Error') + ': ' + err.message);
    }
  }

  async pairRfRemote(slot = 0) {
    try {
      await this.client.pairRfRemote(slot);
      await this.refreshRfStatus();
    } catch (err) {
      alert(t('error', 'Error') + ': ' + err.message);
    }
  }

  async stopPairRfRemote() {
    try {
      await this.client.stopPairRfRemote();
      await this.refreshRfStatus();
    } catch (err) {
      alert(t('error', 'Error') + ': ' + err.message);
    }
  }

  async unpairRfSlot(slot) {
    const slotName = slot === 1 ? 'Slot 1' : 'Slot 2';
    const msg = `Are you sure you want to unpair Remote in ${slotName}? Both Hub and remote will remove RF bonding.`;
    if (!await showConfirm(msg, { title: `Unpair ${slotName}`, okText: `Unpair ${slotName}` })) return;
    try {
      await this.client.unpairRfRemote(slot);
      await this.refreshRfStatus();
    } catch (err) {
      alert(t('error', 'Error') + ': ' + err.message);
    }
  }

  async unpairRfRemote() {
    if (!await showConfirm(t('unpair_all_confirm', 'Unpair all paired RF remotes?'), { title: 'Unpair All Remotes', okText: 'Unpair All' })) return;
    try {
      await this.client.unpairRfRemote(0);
      await this.refreshRfStatus();
    } catch (err) {
      alert(t('error', 'Error') + ': ' + err.message);
    }
  }

  async pushConfigToRemote() {
    const btn = document.getElementById('btnPushConfigToRemote');
    if (btn) {
      btn.disabled = true;
      btn.textContent = 'Pushing...';
    }
    try {
      await this.client.pushRfConfig();
      const syncBanner = document.getElementById('remoteSyncPendingBanner');
      if (syncBanner) syncBanner.style.display = 'none';
      alert(t('rf_config_saved', 'Config push initiated to paired Harmony remotes.'));
    } catch (e) {
      alert(t('error', 'Error') + ': ' + e.message);
    } finally {
      if (btn) {
        btn.disabled = false;
        btn.textContent = 'Push to Remote';
      }
    }
  }

  async startEliteOta(slot = 1) {
    if (!await showConfirm(`Start RF OTA firmware update for Remote in Slot ${slot}? Ensure remote is close to the Hub and has at least 40% battery.`, { title: 'RF OTA Update', okText: 'Start Update', danger: false })) return;
    const btn = document.getElementById(`rfSlot${slot}UpdateBtn`);
    const statusDiv = document.getElementById(`rfSlot${slot}OtaStatus`);
    if (btn) btn.disabled = true;
    if (statusDiv) {
      statusDiv.style.display = 'block';
      statusDiv.textContent = 'Initiating RF OTA transfer...';
    }
    try {
      const res = await this.client.startRfOta(slot);
      if (res && res.status === 'started') {
        const pollTimer = setInterval(async () => {
          try {
            const st = await this.client.getRfOtaStatus();
            if (!st) return;
            if (st.status === 1 || st.in_progress) {
              const pct = st.total_bytes > 0 ? Math.round((st.sent_bytes / st.total_bytes) * 100) : 0;
              if (statusDiv) statusDiv.textContent = `Sending OTA: ${pct}% (${st.sent_chunks}/${st.total_chunks} chunks, ${st.sent_bytes}/${st.total_bytes} bytes)...`;
            } else if (st.status === 2) {
              clearInterval(pollTimer);
              if (statusDiv) statusDiv.textContent = '✓ OTA Complete! Remote verifying and rebooting.';
              if (btn) btn.disabled = false;
              setTimeout(() => this.refreshRfStatus(), 10000);
            } else if (st.status === -1) {
              clearInterval(pollTimer);
              if (statusDiv) statusDiv.textContent = `❌ OTA Failed: ${st.error || 'Transfer failed'}`;
              if (btn) btn.disabled = false;
            }
          } catch {}
        }, 1500);
      } else {
        alert('Could not start OTA: ' + (res?.status || 'Unknown error'));
        if (btn) btn.disabled = false;
      }
    } catch (e) {
      alert('Failed to trigger RF OTA: ' + e.message);
      if (btn) btn.disabled = false;
    }
  }

  async checkOnlineVersions() {
    try {
      let res = await fetch('https://raw.githubusercontent.com/iranl/harmony-ng/ota/VERSION', { cache: 'no-cache' }).catch(() => null);
      if (!res || !res.ok) {
        res = await fetch('https://raw.githubusercontent.com/iranl/harmony-ng/main/VERSION', { cache: 'no-cache' }).catch(() => null);
      }
      if (res && res.ok) {
        const text = await res.text();
        const ver = text.trim();
        if (ver) {
          this.onlineVersion = ver;
          this.latestEliteVersion = ver;
        }
      }
    } catch {}
  }

  showUpdatingOverlay(statusText = 'Applying update and preparing reboot...') {
    this.isUpdating = true;
    const overlay = document.getElementById('hubUpdatingOverlay');
    if (overlay) overlay.style.display = 'flex';
    const stEl = document.getElementById('hubUpdatingStatusText');
    if (stEl) stEl.textContent = statusText;
  }

  hideUpdatingOverlay() {
    this.isUpdating = false;
    const overlay = document.getElementById('hubUpdatingOverlay');
    if (overlay) overlay.style.display = 'none';
  }

  async checkBanners() {
    try {
      // 0. Hub software update in progress
      const updateStat = await this.client.getUpdateStatus().catch(() => null);
      if (updateStat?.updating) {
        this.showUpdatingOverlay('Hub software update is running in background...');
      } else if (this.isUpdating && !this.pollTimerActive) {
        this.hideUpdatingOverlay();
      }

      // 1. Hub software update available
      const hubBanner = document.getElementById('hubUpdateBanner');
      if (hubBanner) {
        const localVer = updateStat?.current_version || updateStat?.version || '';
        const hasOnlineUpdate = Boolean(this.onlineVersion && localVer && this.isVersionOlder(localVer, this.onlineVersion));
        const isUpdateAvail = Boolean(updateStat?.update_available || hasOnlineUpdate);
        if (isUpdateAvail && !updateStat?.updating) {
          hubBanner.style.display = 'flex';
          const title = document.getElementById('hubUpdateTitle');
          const desc = document.getElementById('hubUpdateDesc');
          const targetVer = this.onlineVersion || updateStat?.remote_version || 'New';
          if (title) title.textContent = `Hub Software Update Available (v${targetVer.replace(/^v/, '')})`;
          if (desc) desc.textContent = `Installed: v${localVer.replace(/^v/, '') || 'current'} • Tap to view and install.`;
        } else {
          hubBanner.style.display = 'none';
        }
      }

      // 2. RF remote sync pending
      const rfStat = await this.client.getRfStatus().catch(() => null);
      const syncBanner = document.getElementById('remoteSyncPendingBanner');
      if (syncBanner) {
        if (rfStat && rfStat.sync_pending) {
          syncBanner.style.display = 'flex';
          const title = document.getElementById('remoteSyncTitle');
          const desc = document.getElementById('remoteSyncDesc');
          const slotLabel = rfStat.sync_pending_slot ? `Remote Slot ${rfStat.sync_pending_slot}` : 'Paired Remotes';
          if (title) title.textContent = `Remote Settings Pending Push (${slotLabel})`;
          if (desc) {
            const diffInfo = rfStat.sync_pending_diff ? `Pending changes: ${rfStat.sync_pending_diff}` : 'Hub configuration changed. Push settings to synchronize remote.';
            desc.textContent = diffInfo;
          }
        } else {
          syncBanner.style.display = 'none';
        }
      }
    } catch {}
  }

  /* -------------------------------------------------------------
   * 9. MQTT View
   * ------------------------------------------------------------- */
  async loadMqttView() {
    const badge = document.getElementById('mqttConnBadge');
    try {
      const st = await this.client.getMqttStatus();
      if (badge) {
        badge.className = `badge ${st.connected ? 'ok' : 'bad'}`;
        badge.textContent = st.connected ? 'Connected' : 'Disconnected';
      }
      if (st.config) {
        document.getElementById('mqttEnabled').checked = Boolean(st.config.enabled);
        document.getElementById('mqttHost').value = st.config.host || '';
        document.getElementById('mqttPort').value = st.config.port || 1883;
        document.getElementById('mqttUsername').value = st.config.username || '';
        document.getElementById('mqttBaseTopic').value = st.config.baseTopic || 'harmony/hub';
        document.getElementById('mqttDiscoveryPrefix').value = st.config.discoveryPrefix || 'homeassistant';
        document.getElementById('mqttClientId').value = st.config.clientId || 'harmony_hub';
        document.getElementById('mqttName').value = st.config.name || 'Harmony Hub';
        document.getElementById('mqttKeepAlive').value = st.config.keepAlive || 60;
        document.getElementById('mqttHaDiscovery').checked = Boolean(st.config.haDiscovery);
        document.getElementById('mqttBtEvents').checked = Boolean(st.config.btRemoteEvents);
      }
    } catch (err) {
      if (badge) badge.textContent = 'Offline';
    }
  }

  async saveMqttSettings() {
    const status = document.getElementById('mqttSaveStatus');
    const data = {
      enabled: document.getElementById('mqttEnabled')?.checked ? 1 : 0,
      host: document.getElementById('mqttHost')?.value.trim(),
      port: Number(document.getElementById('mqttPort')?.value) || 1883,
      username: document.getElementById('mqttUsername')?.value.trim(),
      password: document.getElementById('mqttPassword')?.value,
      keep_password: document.getElementById('mqttKeepPass')?.checked ? 1 : 0,
      baseTopic: document.getElementById('mqttBaseTopic')?.value.trim(),
      discoveryPrefix: document.getElementById('mqttDiscoveryPrefix')?.value.trim(),
      clientId: document.getElementById('mqttClientId')?.value.trim(),
      name: document.getElementById('mqttName')?.value.trim(),
      keepAlive: Number(document.getElementById('mqttKeepAlive')?.value) || 60,
      haDiscovery: document.getElementById('mqttHaDiscovery')?.checked ? 1 : 0,
      btRemoteEvents: document.getElementById('mqttBtEvents')?.checked ? 1 : 0
    };

    try {
      if (status) status.textContent = 'Saving MQTT settings...';
      await this.client.saveMqtt(data);
      if (status) status.innerHTML = '<span style="color:var(--success)">Saved!</span>';
    } catch (err) {
      if (status) status.innerHTML = `<span style="color:var(--danger)">Error: ${err.message}</span>`;
    }
  }

  /* -------------------------------------------------------------
   * 10. Network View (Wi-Fi & Ethernet)
   * ------------------------------------------------------------- */
  async loadNetworkView() {
    try {
      const [st, sys] = await Promise.all([
        this.client.getNetworkStatus().catch(() => ({})),
        this.client.getSystemInfo().catch(() => ({}))
      ]);

      // Active Connection & Interface
      document.getElementById('netConnType').textContent = st.connection_type || 'Unknown';
      const activeIfEl = document.getElementById('netActiveIf');
      if (activeIfEl) {
        if (st.eth_present && st.wifi_connected) {
          const isEthActive = (st.active_interface === st.eth_interface) || (st.mode === 'ethernet');
          if (isEthActive) {
            activeIfEl.textContent = `${st.active_interface} (Ethernet Active + Wi-Fi Standby)`;
          } else {
            activeIfEl.textContent = (st.eth_carrier)
              ? `${st.active_interface} (Wi-Fi Active + Ethernet Linking)`
              : `${st.active_interface} (Wi-Fi Active + Ethernet Standby)`;
          }
        } else {
          activeIfEl.textContent = st.active_interface || 'none';
        }
      }

      // IP Address
      const ipEl = document.getElementById('netIp');
      if (ipEl) {
        if (st.eth_ip && st.wifi_ip && st.eth_ip === st.wifi_ip) {
          ipEl.textContent = `${st.ip} (Unified Static IP across Eth & Wi-Fi)`;
        } else if (st.eth_ip && st.wifi_ip) {
          ipEl.textContent = `${st.ip} (Eth: ${st.eth_ip} | Wi-Fi: ${st.wifi_ip})`;
        } else {
          ipEl.textContent = st.ip || 'None';
        }
      }
      document.getElementById('netMask').textContent = st.netmask || '-';
      document.getElementById('netGw').textContent = st.gateway || '-';
      document.getElementById('netMac').textContent = st.mac || '-';

      // USB Controller Mode
      const usbEl = document.getElementById('netUsbMode');
      if (usbEl) {
        usbEl.innerHTML = st.usb_host_mode
          ? '<span style="color:var(--success); font-weight:600;">Host Mode (High-Speed 480Mbps active)</span>'
          : '<span style="color:var(--text-muted);">Gadget Mode (USB Console/PC)</span>';
      }

      // Ethernet Adapter Status
      const ethEl = document.getElementById('netEthStatus');
      if (ethEl) {
        if (st.eth_present) {
          const ifStr = st.eth_interface ? ` [${st.eth_interface}]` : '';
          if (st.eth_carrier) {
            const ipStr = st.eth_ip ? ` (IP: ${st.eth_ip})` : ' (Acquiring IP...)';
            ethEl.innerHTML = `<span style="color:var(--success); font-weight:600;">Connected (100Base-TX Link Up)${ifStr}</span>${ipStr}`;
          } else {
            ethEl.innerHTML = `<span style="color:var(--warning);">Adapter Detected${ifStr} (Cable Unplugged)</span>`;
          }
        } else {
          ethEl.innerHTML = '<span style="color:var(--text-muted);">Not Detected</span>';
        }
      }

      // Wi-Fi Status
      const wifiEl = document.getElementById('netWifiStatus');
      if (wifiEl) {
        if (st.wifi_connected) {
          const ssidStr = st.wifi_ssid ? ` [${st.wifi_ssid}]` : '';
          const ipStr = st.wifi_ip ? ` (IP: ${st.wifi_ip})` : '';
          wifiEl.innerHTML = `<span style="color:var(--success); font-weight:600;">Connected${ssidStr}</span>${ipStr}`;
        } else {
          wifiEl.innerHTML = '<span style="color:var(--danger);">Disconnected</span>';
        }
      }

      // Pre-fill Ethernet & IP configuration form
      if (st.config) {
        const modeSel = document.getElementById('ethModeSelect');
        if (modeSel && st.config.eth_mode) {
          modeSel.value = st.config.eth_mode;
          const staticFields = document.getElementById('ethStaticFields');
          if (staticFields) staticFields.style.display = (st.config.eth_mode === 'static') ? 'block' : 'none';
        }
        if (st.config.eth_ip && document.getElementById('ethIp')) document.getElementById('ethIp').value = st.config.eth_ip;
        if (st.config.eth_netmask && document.getElementById('ethNetmask')) document.getElementById('ethNetmask').value = st.config.eth_netmask;
        if (st.config.eth_gateway && document.getElementById('ethGateway')) document.getElementById('ethGateway').value = st.config.eth_gateway;
        if (st.config.eth_dns && document.getElementById('ethDns')) document.getElementById('ethDns').value = st.config.eth_dns;
      }

      // Ensure IP assignment mode select toggles static fields
      const modeSel = document.getElementById('ethModeSelect');
      if (modeSel && !modeSel._hasChangeAttached) {
        modeSel._hasChangeAttached = true;
        modeSel.addEventListener('change', () => {
          const sf = document.getElementById('ethStaticFields');
          if (sf) sf.style.display = (modeSel.value === 'static') ? 'block' : 'none';
        });
      }

      const ifcPre = document.getElementById('netIfconfigPre');
      if (ifcPre) {
        ifcPre.textContent = st.ifconfig ? st.ifconfig.trim() : 'No ifconfig output available.';
      }
      const netPre = document.getElementById('sysNetInfoPre');
      if (netPre && sys && sys.network_details) {
        netPre.textContent = sys.network_details.trim();
      }
    } catch (err) {
      console.warn('Network status error:', err);
    }
  }

  async saveEthernetConfig(applyMode = 'save') {
    const status = document.getElementById('ethSaveStatus');
    const data = {
      apply: applyMode,
      eth_enabled: 1,
      eth_fallback_wifi: 1,
      eth_mode: document.getElementById('ethModeSelect')?.value || 'dhcp',
      eth_ip: document.getElementById('ethIp')?.value.trim(),
      eth_netmask: document.getElementById('ethNetmask')?.value.trim(),
      eth_gateway: document.getElementById('ethGateway')?.value.trim(),
      eth_dns: document.getElementById('ethDns')?.value.trim()
    };
    try {
      if (status) status.textContent = 'Saving IP settings...';
      await this.client.saveEthernet(data);
      if (status) status.innerHTML = '<span style="color:var(--success)">Saved!</span>';
    } catch (err) {
      if (status) status.innerHTML = `<span style="color:var(--danger)">Error: ${err.message}</span>`;
    }
  }

  escapeHtml(str) {
    if (!str) return '';
    return String(str)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;');
  }

  async scanWifiNetworks() {
    const btn = document.getElementById('btnWifiScan');
    const btnText = document.getElementById('btnWifiScanText');
    const dropdown = document.getElementById('wifiScanDropdown');
    const status = document.getElementById('wifiSaveStatus');
    if (!dropdown) return;

    const originalText = btnText ? btnText.textContent : 'Scan Networks';
    if (btn) btn.disabled = true;
    if (btnText) btnText.textContent = 'Scanning...';
    if (status) status.innerHTML = '<span class="subtle">Scanning 2.4GHz channels...</span>';

    try {
      const res = await this.client.scanWifi();
      const networks = (res && res.networks) ? res.networks : [];
      if (networks.length === 0) {
        dropdown.innerHTML = '<div style="padding:12px; font-size:12px; color:var(--text-dim); text-align:center;">No networks found</div>';
        dropdown.style.display = 'block';
        if (status) status.textContent = '';
        return;
      }

      let html = '';
      networks.forEach(net => {
        const rawSsid = net.ssid || '';
        const safeSsid = this.escapeHtml(rawSsid);
        const sig = Number(net.signal) || -80;
        let bars = '●○○○';
        if (sig >= -55) bars = '●●●●';
        else if (sig >= -68) bars = '●●●○';
        else if (sig >= -78) bars = '●●○○';

        const secBadge = this.escapeHtml(net.security || 'WPA2');
        const isOpen = secBadge.toLowerCase() === 'open';

        html += `
          <div class="wifi-scan-item" data-ssid="${safeSsid}" data-open="${isOpen ? 1 : 0}">
            <div class="wifi-scan-info">
              <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" style="flex-shrink:0; color:var(--accent);">
                <path d="M5 12.55a11 11 0 0 1 14.08 0M1.42 9a16 16 0 0 1 21.16 0M8.53 16.11a6 6 0 0 1 6.95 0M12 20h.01"/>
              </svg>
              <span class="wifi-scan-ssid">${safeSsid}</span>
            </div>
            <div class="wifi-scan-meta">
              <span class="wifi-scan-badge">${secBadge}</span>
              <span class="wifi-scan-signal" title="${sig} dBm (Ch ${net.channel || '?'})">${bars}</span>
            </div>
          </div>
        `;
      });

      dropdown.innerHTML = html;
      dropdown.style.display = 'block';
      if (status) status.textContent = '';

      dropdown.querySelectorAll('.wifi-scan-item').forEach(item => {
        item.addEventListener('click', () => {
          const ssid = item.getAttribute('data-ssid');
          const isOpen = item.getAttribute('data-open') === '1';
          const input = document.getElementById('wifiSsidInput');
          if (input) {
            input.value = ssid;
            input.dispatchEvent(new Event('input', { bubbles: true }));
          }
          const openChk = document.getElementById('wifiOpen');
          if (openChk) openChk.checked = isOpen;

          const passInput = document.getElementById('wifiPskInput');
          if (passInput) {
            if (isOpen) {
              passInput.value = '';
              passInput.disabled = true;
            } else {
              passInput.disabled = false;
              passInput.focus();
            }
          }
          dropdown.style.display = 'none';
        });
      });
    } catch (err) {
      if (status) status.innerHTML = `<span style="color:var(--danger)">Scan error: ${err.message}</span>`;
    } finally {
      if (btn) btn.disabled = false;
      if (btnText) btnText.textContent = originalText;
    }
  }

  async testWifiConfig() {
    const status = document.getElementById('wifiSaveStatus');
    const ssid = document.getElementById('wifiSsidInput')?.value.trim();
    if (!ssid) {
      if (status) status.innerHTML = '<span style="color:var(--danger)">Please enter an SSID first</span>';
      return;
    }

    const data = {
      ssid: ssid,
      password: document.getElementById('wifiPskInput')?.value,
      keep_password: document.getElementById('wifiKeepPass')?.checked ? 1 : 0,
      hidden: document.getElementById('wifiHidden')?.checked ? 1 : 0,
      open: document.getElementById('wifiOpen')?.checked ? 1 : 0
    };

    const testBtn = document.getElementById('btnWifiTest');
    if (testBtn) testBtn.disabled = true;

    try {
      if (status) status.innerHTML = '<span class="subtle">Testing Wi-Fi connection with AP (10-15s)...</span>';
      const res = await this.client.testWifi(data);
      if (res && res.ok) {
        if (status) status.innerHTML = '<span style="color:var(--success); font-weight:600;">Connection test passed!</span>';
      } else {
        const errMsg = (res && res.error) ? res.error : 'Connection test failed';
        if (status) status.innerHTML = `<span style="color:var(--danger); font-weight:600;">${errMsg}</span>`;
      }
    } catch (err) {
      if (status) status.innerHTML = `<span style="color:var(--danger); font-weight:600;">Test failed: ${err.message}</span>`;
    } finally {
      if (testBtn) testBtn.disabled = false;
    }
  }

  async saveWifiConfig(applyMode = 'save') {
    const status = document.getElementById('wifiSaveStatus');
    const ssid = document.getElementById('wifiSsidInput')?.value.trim();
    if (!ssid) {
      if (status) status.innerHTML = '<span style="color:var(--danger)">Please enter an SSID</span>';
      return;
    }

    const data = {
      apply: applyMode,
      ssid: ssid,
      password: document.getElementById('wifiPskInput')?.value,
      keep_password: document.getElementById('wifiKeepPass')?.checked ? 1 : 0,
      hidden: document.getElementById('wifiHidden')?.checked ? 1 : 0,
      open: document.getElementById('wifiOpen')?.checked ? 1 : 0
    };

    const isApplying = (applyMode === 'reconfigure' || applyMode === 'reboot' || applyMode === 'apply');

    try {
      if (status) {
        status.innerHTML = isApplying
          ? '<span class="subtle">Testing connection before switching...</span>'
          : '<span class="subtle">Saving Wi-Fi...</span>';
      }
      const res = await this.client.saveWifi(data);
      if (res && res.ok === false) {
        throw new Error(res.error || 'Failed to save Wi-Fi');
      }
      if (status) {
        status.innerHTML = isApplying
          ? '<span style="color:var(--success); font-weight:600;">Wi-Fi verified & applied!</span>'
          : '<span style="color:var(--success); font-weight:600;">Wi-Fi settings saved!</span>';
      }
    } catch (err) {
      if (status) {
        status.innerHTML = `<span style="color:var(--danger); font-weight:600;">${err.message}</span>`;
      }
    }
  }

  /* -------------------------------------------------------------
   * 11. Backup & Restore View
   * ------------------------------------------------------------- */
  loadBackupView() {}

  async exportBackupItem(target) {
    try {
      const url = this.client.baseUrl ? `${this.client.baseUrl}/export/${target}` : `/export/${target}`;
      const res = await fetch(url, { headers: this.client.getHeaders() });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const blob = await res.blob();
      const a = document.createElement('a');
      const filename = target === 'bundle' ? 'harmony_backup_bundle.json' :
                       (target === 'network' || target === 'wifi' || target === 'auth' || target === 'hub-id' || target === 'hub_id') ?
                       `${target}.conf` : `${target}.json`;
      a.href = URL.createObjectURL(blob);
      a.download = filename;
      document.body.appendChild(a);
      a.click();
      setTimeout(() => {
        URL.revokeObjectURL(a.href);
        a.remove();
      }, 1000);
    } catch (err) {
      alert(`Export failed: ${err.message}`);
    }
  }

  async restoreBackupPayload() {
    const target = document.getElementById('backupTarget')?.value;
    const payload = document.getElementById('backupPayload')?.value.trim();
    const restoreNet = !!document.getElementById('restoreNetworkCheckbox')?.checked;
    const status = document.getElementById('backupImportStatus');
    if (!payload) return alert('Enter backup payload to restore.');
    const confirmMsg = restoreNet ?
      `Restore ${target} from payload INCLUDING network/Wi-Fi settings?\nWARNING: If network configuration differs, Hub may become unreachable.` :
      `Restore ${target} from payload? (Active network & Wi-Fi settings will be safely preserved)`;
    if (!await showConfirm(confirmMsg, { title: 'Restore Backup', okText: 'Restore' })) return;
    try {
      if (status) status.textContent = 'Restoring...';
      await this.client.restoreBackup(target, payload, restoreNet);
      if (status) status.innerHTML = '<span style="color:var(--success)">Restored!</span>';
      alert('Restore complete. Reboot hub if necessary.');
      await this.refreshData();
    } catch (err) {
      if (status) status.innerHTML = `<span style="color:var(--danger)">Restore failed: ${err.message}</span>`;
    }
  }

  /* -------------------------------------------------------------
   * 12. System Management View
   * ------------------------------------------------------------- */
  async loadSystemView() {
    this.client.getSettings().then(st => {
      if (st) {
        if (st.hub_name) {
          const inp = document.getElementById('hubCustomNameInput');
          if (inp) inp.value = st.hub_name;
          const label = document.getElementById('activeHubLabel');
          if (label) label.textContent = st.hub_name;
        }
        const ntpInp = document.getElementById('hubNtpServerInput');
        if (ntpInp && st.ntp_server) ntpInp.value = st.ntp_server;
        const tzEl = document.getElementById('hubTimezoneSelect') || document.getElementById('hubTimezoneInput');
        if (tzEl && st.timezone) {
          tzEl.value = st.timezone;
          if (!tzEl.value && st.timezone) {
            // Custom timezone not in preset list
            const opt = document.createElement('option');
            opt.value = st.timezone;
            opt.textContent = `Custom (${st.timezone})`;
            opt.selected = true;
            tzEl.appendChild(opt);
            tzEl.value = st.timezone;
          }
        }
        const ledSel = document.getElementById('hubLedModeSelect');
        if (ledSel && st.led_mode) ledSel.value = st.led_mode;
      }
    }).catch(() => {});

    await Promise.allSettled([
      this.refreshServiceStatuses(),
      this.refreshSystemInfo(),
      this.refreshLogs()
    ]);
  }

  async refreshServiceStatuses() {
    const rfBadge = document.getElementById('svcRfBadge');
    const btBadge = document.getElementById('svcBtBadge');
    const rfBtn = document.getElementById('btnToggleRfSvc');
    const btBtn = document.getElementById('btnToggleBtSvc');

    try {
      const svcs = await this.client.getServices();
      this.servicesState = svcs;

      if (rfBadge && rfBtn) {
        if (svcs.rf_enabled) {
          rfBadge.className = svcs.rf_running ? 'badge badge-success' : 'badge badge-warning';
          rfBadge.textContent = svcs.rf_running ? 'Running' : 'Enabled (Idle)';
          rfBtn.textContent = 'Disable RF';
          rfBtn.className = 'btn btn-sm btn-danger';
        } else {
          rfBadge.className = 'badge badge-danger';
          rfBadge.textContent = 'Disabled';
          rfBtn.textContent = 'Enable RF';
          rfBtn.className = 'btn btn-sm btn-primary';
        }
      }

      if (btBadge && btBtn) {
        if (svcs.bt_enabled) {
          btBadge.className = svcs.bt_running ? 'badge badge-success' : 'badge badge-warning';
          btBadge.textContent = svcs.bt_running ? 'Running' : 'Enabled (Idle)';
          btBtn.textContent = 'Disable Bluetooth';
          btBtn.className = 'btn btn-sm btn-danger';
        } else {
          btBadge.className = 'badge badge-danger';
          btBadge.textContent = 'Disabled';
          btBtn.textContent = 'Enable Bluetooth';
          btBtn.className = 'btn btn-sm btn-primary';
        }
      }
    } catch (err) {
      if (rfBadge) { rfBadge.className = 'badge badge-danger'; rfBadge.textContent = 'Offline'; }
      if (btBadge) { btBadge.className = 'badge badge-danger'; btBadge.textContent = 'Offline'; }
    }
  }

  async toggleRfService() {
    const isEnabled = this.servicesState ? this.servicesState.rf_enabled : false;
    const action = isEnabled ? 'disable' : 'enable';
    if (!await showConfirm(`${action === 'disable' ? 'Disable' : 'Enable'} Harmony Elite RF receiver service?`, { title: `${action === 'disable' ? 'Disable' : 'Enable'} RF Service`, okText: action === 'disable' ? 'Disable' : 'Enable' })) return;

    try {
      await this.client.toggleRfService(!isEnabled);
      await this.refreshServiceStatuses();
    } catch (err) {
      alert('Failed to toggle RF: ' + err.message);
    }
  }

  async toggleBtService() {
    const isEnabled = this.servicesState ? this.servicesState.bt_enabled : false;
    const action = isEnabled ? 'disable' : 'enable';
    if (!await showConfirm(`${action === 'disable' ? 'Disable' : 'Enable'} Bluetooth service?`, { title: `${action === 'disable' ? 'Disable' : 'Enable'} Bluetooth Service`, okText: action === 'disable' ? 'Disable' : 'Enable' })) return;

    try {
      await this.client.toggleBtService(!isEnabled);
      await this.refreshServiceStatuses();
    } catch (err) {
      alert('Failed to toggle Bluetooth: ' + err.message);
    }
  }

  async refreshLogs() {
    const logsEl = document.getElementById('sysLogsPre');
    const source = document.getElementById('sysLogSource')?.value || 'daemon';
    try {
      const res = await this.client.getLogs(source);
      if (logsEl) {
        logsEl.textContent = (res && (res.log || res.logs)) ? (res.log || res.logs) : (typeof res === 'string' ? res : 'No log entries available.');
        logsEl.scrollTop = logsEl.scrollHeight;
      }
    } catch (err) {
      if (logsEl) logsEl.textContent = 'Failed to load logs: ' + err.message;
    }
  }

  async refreshSystemInfo() {
    const infoEl = document.getElementById('sysInfoPre');
    const netEl = document.getElementById('sysNetInfoPre');

    try {
      const sys = await this.client.getSystemInfo();
      if (sys && sys.ok) {
        if (infoEl) {
          let text = `Kernel:        ${sys.kernel || 'Linux'}\n`;
          text += `Firmware:      ${sys.firmware || '4.15.600'}\n`;
          text += `Uptime:        ${sys.uptime || 'Unknown'}\n`;
          text += `Load Average:  ${sys.loadavg || 'N/A'}\n`;
          text += `Memory Total:  ${sys.mem_total_kb ? Math.round(sys.mem_total_kb / 1024) + ' MB (' + sys.mem_total_kb + ' kB)' : 'N/A'}\n`;
          text += `Memory Free:   ${sys.mem_free_kb ? Math.round(sys.mem_free_kb / 1024) + ' MB (' + sys.mem_free_kb + ' kB)' : 'N/A'}\n`;
          text += `Memory Avail:  ${sys.mem_available_kb ? Math.round(sys.mem_available_kb / 1024) + ' MB (' + sys.mem_available_kb + ' kB)' : 'N/A'}\n`;
          infoEl.textContent = text;
        }
        if (netEl && sys.network_details) {
          netEl.textContent = sys.network_details;
        }
      }
    } catch (err) {
      if (infoEl) infoEl.textContent = 'Failed to load system info: ' + err.message;
    }
  }

  async saveDebugLogging() {
    const status = document.getElementById('sysDebugStatus');
    const on = document.getElementById('sysDebugCheck')?.checked ? 1 : 0;
    try {
      await this.client.saveSystemAction('debug_logging', { debugLogging: on });
      if (status) status.innerHTML = '<span style="color:var(--success)">Saved!</span>';
    } catch (err) {
      if (status) status.innerHTML = `<span style="color:var(--danger)">Error: ${err.message}</span>`;
    }
  }

  async saveWebUiAuth() {
    const status = document.getElementById('authSaveStatus');
    const on = document.getElementById('authEnabledCheck')?.checked ? 1 : 0;
    const u = document.getElementById('authUsername')?.value.trim();
    const p = document.getElementById('authPassword')?.value;
    try {
      await this.client.saveSystemAction('auth', { authEnabled: on, authUsername: u, authPassword: p });
      if (status) status.innerHTML = '<span style="color:var(--success)">Sign-in setting saved!</span>';
    } catch (err) {
      if (status) status.innerHTML = `<span style="color:var(--danger)">Error: ${err.message}</span>`;
    }
  }

  async showSoftwareVersions() {
    const log = document.getElementById('updateLog');
    try {
      if (log) log.textContent = 'Fetching current system & update status...';
      const [sys, stat] = await Promise.all([
        this.client.getSystemInfo().catch(() => ({})),
        this.client.getUpdateStatus().catch(() => ({}))
      ]);

      let remoteVer = stat.remote_version || '';
      try {
        const rawRes = await fetch(`https://raw.githubusercontent.com/iranl/harmony-ng/ota/VERSION?_=${Date.now()}`);
        if (rawRes.ok) {
          const v = (await rawRes.text()).trim();
          if (v) remoteVer = v.startsWith('v') ? v : `v${v}`;
        }
      } catch (_) {}

      const localVer = stat.local_version || ('v' + (sys.version || APP_VERSION));
      const hasUpdate = stat.update_available || (remoteVer && !localVer.includes(remoteVer.replace(/^v/, '')));

      if (log) {
        log.textContent = `Hub Firmware: ${sys.firmware || stat.fw_version || '4.15.600'}\n` +
          `Codex Software: v${this.appVersion || APP_VERSION}\n` +
          `Codex Daemon: v${sys.version || stat.current_version || APP_VERSION}\n` +
          `Commit: ${stat.commit || sys.commit || 'dev'}\n` +
          `Build: ${stat.build_time || sys.build_time || 'Release'}\n` +
          `Local Version: ${localVer}\n` +
          `Remote Version: ${remoteVer || 'Up to date'}\n` +
          `Status: ${hasUpdate ? '⚠️ Update available' : '✓ Up to date'}`;
      }
    } catch (err) {
      if (log) log.textContent = 'Failed to fetch version info: ' + err.message;
    }
  }

  async checkSoftwareUpdate() {
    const log = document.getElementById('updateLog');
    const repoInput = document.getElementById('updateRepo')?.value.trim() || '';
    const tokenInput = document.getElementById('updateToken')?.value.trim() || '';
    try {
      if (log) log.textContent = 'Checking update repository and GitHub releases...';
      let remoteVer = '';
      try {
        const ghHeaders = { 'Accept': 'application/vnd.github.v3+json' };
        if (tokenInput) ghHeaders['Authorization'] = `token ${tokenInput}`;
        const ghRes = await fetch('https://api.github.com/repos/iranl/harmony-ng/releases/latest', { headers: ghHeaders });
        if (ghRes.ok) {
          const ghData = await ghRes.json();
          remoteVer = (ghData.tag_name || '').replace(/^v/, '');
        }
      } catch (_) {
        try {
          const rawRes = await fetch(`https://raw.githubusercontent.com/iranl/harmony-ng/ota/VERSION?_=${Date.now()}`);
          if (rawRes.ok) {
            const rawVer = await rawRes.text();
            remoteVer = rawVer.trim().replace(/^v/, '');
          }
        } catch (_) {}
      }

      const res = await this.client.checkUpdate(true, repoInput, tokenInput, remoteVer);
      const stat = await this.client.getUpdateStatus().catch(() => ({}));
      if (log) {
        const displayRemote = remoteVer ? `v${remoteVer}` : (stat.remote_version || res.remote_version || 'unknown');
        const curVer = stat.current_version || APP_VERSION;
        const hasUpdate = (stat.update_available || res.has_update || (remoteVer && remoteVer !== curVer));
        log.textContent = `Local: v${curVer} | Remote: ${displayRemote}\nStatus: ${hasUpdate ? `⚠️ Update available to install (${displayRemote})!` : '✓ Hub is up to date'}`;
      }
    } catch (err) {
      if (log) log.textContent = 'Update check failed: ' + err.message;
    }
  }

  async installSoftwareUpdate() {
    const repo = document.getElementById('updateRepo')?.value.trim();
    const token = document.getElementById('updateToken')?.value.trim();
    const log = document.getElementById('updateLog');
    if (!await showConfirm(t('update_confirm', 'Download and apply update? Hub services will restart.'), { title: 'Software Update', okText: 'Update' })) return;
    try {
      this.showUpdatingOverlay('Hub is downloading and applying software update...');
      if (log) log.textContent = t('update_applying', 'Applying software update...\nHub is downloading package and will reboot automatically.');

      await this.client.applyUpdate(token, repo);

      const statusEl = document.getElementById('hubUpdatingStatusText');
      if (statusEl) statusEl.textContent = 'Hub is updated and is rebooting...';
      if (log) log.textContent = 'Hub is updated and is rebooting. Please wait for reconnection...';

      this.pollTimerActive = true;
      let attempts = 0;
      let sawDrop = false;

      const pollTimer = setInterval(async () => {
        attempts++;
        if (attempts > 60) {
          clearInterval(pollTimer);
          this.pollTimerActive = false;
          this.hideUpdatingOverlay();
          if (log) log.textContent += '\nUpdate wait timed out. Please refresh the page.';
          return;
        }

        try {
          const stat = await this.client.getUpdateStatus();
          if (!stat || stat.updating) {
            sawDrop = true;
            if (statusEl) statusEl.textContent = 'Hub is rebooting...';
            return;
          }
          if (sawDrop || attempts >= 5) {
            clearInterval(pollTimer);
            this.pollTimerActive = false;
            this.hideUpdatingOverlay();
            if (log) log.textContent = `Hub is updated and reboot complete!\nVersion: v${stat.current_version || stat.version || ''}`;
            showToast('Hub is updated and reboot complete!', 'success', 5000);
            await this.refreshData();
          }
        } catch (_) {
          sawDrop = true;
          if (statusEl) statusEl.textContent = 'Hub is rebooting... Reconnecting...';
        }
      }, 3000);
    } catch (err) {
      this.pollTimerActive = false;
      this.hideUpdatingOverlay();
      if (log) log.textContent = t('update_failed', 'Install failed: ') + err.message;
      showToast('Install failed: ' + err.message, 'error');
    }
  }

  async rediscoverMqtt() {
    const status = document.getElementById('sysActionStatus');
    try {
      await this.client.saveSystemAction('rediscover');
      if (status) status.innerHTML = '<span style="color:var(--success)">Discovery triggered!</span>';
    } catch (err) {
      if (status) status.innerHTML = `<span style="color:var(--danger)">Error: ${err.message}</span>`;
    }
  }

  async rebootHub() {
    if (this.isUpdating) {
      showToast('Manual reboot blocked: software update is currently in progress.', 'warning');
      return;
    }
    if (!await showConfirm(t('reboot_confirm', 'Are you sure you want to reboot the Harmony Hub?'), { title: 'Reboot Hub', okText: 'Reboot' })) return;
    try {
      await this.client.rebootHub();
      alert(t('reboot_sent', 'Reboot command sent to hub. Reconnecting in 30 seconds...'));
    } catch (err) {
      alert(t('reboot_failed', 'Reboot failed: ') + err.message);
    }
  }

  setupFactoryResetModal() {
    const modal = document.getElementById('factoryResetModal');
    const btnOpen = document.getElementById('btnFactoryResetHub');
    const step1 = document.getElementById('resetModalStep1');
    const step2 = document.getElementById('resetModalStep2');
    const progress = document.getElementById('resetModalProgress');
    const btnCancel1 = document.getElementById('btnResetCancel1');
    const btnCancel2 = document.getElementById('btnResetCancel2');
    const btnProceed1 = document.getElementById('btnResetProceed1');
    const btnExecute = document.getElementById('btnResetExecute');
    const inputConfirm = document.getElementById('resetConfirmInput');

    if (!modal || !btnOpen) return;

    const closeModal = () => {
      modal.style.display = 'none';
      if (inputConfirm) inputConfirm.value = '';
    };

    btnOpen.addEventListener('click', () => {
      step1.style.display = 'block';
      step2.style.display = 'none';
      progress.style.display = 'none';
      if (btnExecute) btnExecute.disabled = true;
      if (inputConfirm) inputConfirm.value = '';
      modal.style.display = 'flex';
    });

    btnCancel1?.addEventListener('click', closeModal);
    btnCancel2?.addEventListener('click', closeModal);

    btnProceed1?.addEventListener('click', () => {
      step1.style.display = 'none';
      step2.style.display = 'block';
      if (inputConfirm) {
        inputConfirm.value = '';
        setTimeout(() => inputConfirm.focus(), 100);
      }
    });

    inputConfirm?.addEventListener('input', () => {
      if (btnExecute) {
        btnExecute.disabled = (inputConfirm.value.trim().toUpperCase() !== 'RESET');
      }
    });

    btnExecute?.addEventListener('click', async () => {
      if (inputConfirm.value.trim().toUpperCase() !== 'RESET') return;
      step2.style.display = 'none';
      progress.style.display = 'block';
      try {
        await this.client.saveSystemAction('factory_reset', { confirm: 'RESET' });
      } catch (err) {
        // Hub reboots quickly and connection drops
      }
    });
  }

  /* -------------------------------------------------------------
   * 13. Saved Hubs View (Multi-Hub Manager)
   * ------------------------------------------------------------- */
  async renderHubsList() {
    const list = document.getElementById('hubsList');
    if (!list) return;

    document.getElementById('btnScanHubs')?.addEventListener('click', () => this.scanNetwork(false));
    document.getElementById('btnStopScan')?.addEventListener('click', () => this.stopNetworkScan());

    const hubs = await getSavedHubs();
    if (!hubs.length) {
      list.innerHTML = '<div class="muted" style="padding: 16px;">No saved hubs yet. Click "+ Add Hub".</div>';
      return;
    }

    list.innerHTML = hubs.map(h => {
      const isCur = this.activeHub && this.activeHub.id === h.id;
      return `
        <div class="card" style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px;">
          <div>
            <div style="display: flex; align-items: center; gap: 8px;">
              <strong>${h.name}</strong>
              ${isCur ? '<span class="badge ok">Active</span>' : ''}
            </div>
            <div class="muted mini">${h.host}:${h.port || 8080}</div>
          </div>
          <div style="display: flex; gap: 6px;">
            ${!isCur ? `<button type="button" class="btn btn-xs btn-primary btn-select-hub" data-id="${h.id}">Connect</button>` : ''}
            <button type="button" class="btn btn-xs btn-secondary btn-edit-hub" data-id="${h.id}">Edit</button>
            <button type="button" class="btn btn-xs btn-danger btn-delete-hub" data-id="${h.id}">✕</button>
          </div>
        </div>
      `;
    }).join('');

    list.querySelectorAll('.btn-select-hub').forEach(b => {
      b.addEventListener('click', async () => {
        await setActiveHubId(b.getAttribute('data-id'));
        await this.loadActiveHub();
        this.renderHubsList();
      });
    });

    list.querySelectorAll('.btn-edit-hub').forEach(b => {
      b.addEventListener('click', () => {
        const h = hubs.find(x => x.id === b.getAttribute('data-id'));
        if (h) this.openEditHubModal(h);
      });
    });

    list.querySelectorAll('.btn-delete-hub').forEach(b => {
      b.addEventListener('click', async () => {
        const id = b.getAttribute('data-id');
        if (await showConfirm('Delete this hub profile?', { title: 'Delete Hub Profile', okText: 'Delete' })) {
          await deleteHub(id);
          await this.loadActiveHub();
          this.renderHubsList();
        }
      });
    });
  }

  openAddHubModal() {
    const defaultHost = (typeof window !== 'undefined' && window.location && window.location.hostname && window.location.hostname !== 'localhost' && window.location.hostname !== '127.0.0.1') ? window.location.hostname : '';
    this.openEditHubModal({ id: 'hub_' + Date.now(), name: 'Harmony Hub', host: defaultHost, port: 8089, wsPort: 8089 }, false);
  }

  async scanNetwork(autoSelect = false) {
    if (this.isScanningNetwork) return;
    this.isScanningNetwork = true;

    const box = document.getElementById('hubScanStatusBox');
    const title = document.getElementById('hubScanTitle');
    const sub = document.getElementById('hubScanSub');
    const btnScan = document.getElementById('btnScanHubs');
    const btnLabel = document.getElementById('scanHubsLabel');
    const btnIcon = document.getElementById('scanHubsIcon');

    if (box) box.style.display = 'block';
    if (btnScan) btnScan.disabled = true;
    if (btnLabel) btnLabel.textContent = 'Scanning...';
    if (btnIcon) btnIcon.textContent = '⏳';

    this._cancelScan = false;

    const subnets = new Set();

    // 1. Current host from browser URL / WebView hostname
    if (typeof window !== 'undefined' && window.location && window.location.hostname) {
      const host = window.location.hostname;
      if (/^\d+\.\d+\.\d+\.\d+$/.test(host) && !host.startsWith('127.')) {
        const parts = host.split('.');
        subnets.add(`${parts[0]}.${parts[1]}.${parts[2]}`);
      }
    }

    // 2. Active or previously configured hubs
    if (this.activeHub && this.activeHub.host && /^\d+\.\d+\.\d+\.\d+$/.test(this.activeHub.host)) {
      if (!this.activeHub.host.startsWith('127.')) {
        const parts = this.activeHub.host.split('.');
        subnets.add(`${parts[0]}.${parts[1]}.${parts[2]}`);
      }
    }
    const saved = await getSavedHubs();
    for (const h of saved) {
      if (h.host && /^\d+\.\d+\.\d+\.\d+$/.test(h.host) && !h.host.startsWith('127.')) {
        const parts = h.host.split('.');
        subnets.add(`${parts[0]}.${parts[1]}.${parts[2]}`);
      }
    }

    // 3. Local interface IPs detected via WebRTC
    try {
      const localIps = await this.detectLocalIps();
      for (const ip of localIps) {
        if (/^\d+\.\d+\.\d+\.\d+$/.test(ip) && !ip.startsWith('127.')) {
          const parts = ip.split('.');
          subnets.add(`${parts[0]}.${parts[1]}.${parts[2]}`);
        }
      }
    } catch {}

    // 4. Fallback only if no network subnet could be detected (e.g. localhost file:// with no saved hubs)
    if (subnets.size === 0) {
      subnets.add('192.168.1');
    }

    let foundCount = 0;
    const subnetList = Array.from(subnets);

    for (const subnet of subnetList) {
      if (this._cancelScan) break;
      if (title) title.textContent = `Scanning ${subnet}.1 – ${subnet}.254 on port 8089...`;
      if (sub) sub.textContent = `Checking /health endpoint (${foundCount} found so far)`;

      const ips = [];
      for (let i = 1; i <= 254; i++) {
        ips.push(`${subnet}.${i}`);
      }

      const concurrency = 35;
      for (let i = 0; i < ips.length; i += concurrency) {
        if (this._cancelScan) break;
        const batch = ips.slice(i, i + concurrency);
        await Promise.all(batch.map(async (ip) => {
          if (this._cancelScan) return;
          const res = await HarmonyClient.checkHubHealth(ip, 8089, 1400);
          if (res.ok) {
            const hubName = await HarmonyClient.fetchHubName(ip, 8089, 1800);
            const hubId = 'hub_' + ip.replace(/\./g, '_');
            const foundHub = {
              id: hubId,
              name: hubName,
              host: ip,
              port: 8089,
              wsPort: 8089
            };
            await saveHub(foundHub);
            foundCount++;

            showToast(`Found Harmony Hub: ${hubName} (${ip})`, 'success', 3000);
            await this.renderHubsList();

            if (autoSelect || !this.activeHub || !this.activeHub.host) {
              await setActiveHubId(hubId);
              await this.loadActiveHub();
            }
          }
        }));
      }

      if (foundCount > 0 && subnetList.indexOf(subnet) === 0) {
        break;
      }
    }

    this.isScanningNetwork = false;
    if (box) box.style.display = 'none';
    if (btnScan) btnScan.disabled = false;
    if (btnLabel) btnLabel.textContent = 'Scan Network';
    if (btnIcon) btnIcon.textContent = '🔍';

    if (foundCount > 0) {
      showToast(`Network scan complete! Added ${foundCount} Harmony Hub(s).`, 'success');
      await this.renderHubsList();
    } else if (!this._cancelScan) {
      showToast('Scan complete. No Harmony Hubs found on port 8089. You can add one manually.', 'warning');
      if (!this.activeHub || !this.activeHub.host) {
        this.switchTab('hubs');
      }
    }
  }

  stopNetworkScan() {
    this._cancelScan = true;
    const box = document.getElementById('hubScanStatusBox');
    if (box) box.style.display = 'none';
    const btnScan = document.getElementById('btnScanHubs');
    if (btnScan) btnScan.disabled = false;
    const btnLabel = document.getElementById('scanHubsLabel');
    if (btnLabel) btnLabel.textContent = 'Scan Network';
    const btnIcon = document.getElementById('scanHubsIcon');
    if (btnIcon) btnIcon.textContent = '🔍';
  }

  async detectLocalIps() {
    const ips = [];
    if (typeof RTCPeerConnection === 'undefined') return ips;
    try {
      const pc = new RTCPeerConnection({ iceServers: [] });
      pc.createDataChannel('');
      const offer = await pc.createOffer();
      await pc.setLocalDescription(offer);

      await new Promise((resolve) => {
        const timer = setTimeout(() => {
          try { pc.close(); } catch {}
          resolve();
        }, 1200);

        pc.onicecandidate = (e) => {
          if (!e || !e.candidate || !e.candidate.candidate) return;
          const m = e.candidate.candidate.match(/\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b/);
          if (m && !m[0].startsWith('127.')) {
            ips.push(m[0]);
          }
        };

        pc.onicegatheringstatechange = () => {
          if (pc.iceGatheringState === 'complete') {
            clearTimeout(timer);
            try { pc.close(); } catch {}
            resolve();
          }
        };
      });
    } catch {}
    return ips;
  }

  openEditHubModal(h, isEdit = true) {
    if (!h) h = {};
    if (!h.id) h.id = 'hub_' + Date.now();
    let modal = document.getElementById('hubModal');
    if (!modal) {
      modal = document.createElement('div');
      modal.id = 'hubModal';
      modal.style.cssText = 'position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.7);z-index:2000;display:flex;align-items:center;justify-content:center;padding:16px;';
      document.body.appendChild(modal);
    }

    modal.innerHTML = `
      <div class="card" style="width:100%;max-width:380px;">
        <h3 style="font-size:16px;margin-bottom:12px;">${isEdit ? 'Edit Hub Profile' : 'Add New Harmony Hub'}</h3>
        <label>Hub Name</label><input id="hubFormName" class="form-control" value="${h.name || ''}">
        <label>IP Address / Host</label><input id="hubFormHost" class="form-control" value="${h.host || ''}">
        <label>HTTP Port</label><input id="hubFormPort" type="number" class="form-control" value="${h.port || 8089}">
        <label>Username (Optional)</label><input id="hubFormUser" class="form-control" value="${h.username || ''}">
        <label>Password (Optional)</label><input id="hubFormPass" type="password" class="form-control" value="${h.password || ''}">
        <div id="hubTestStatus" class="subtle" style="margin-top:8px;"></div>
        <div class="actions" style="margin-top:14px;display:flex;justify-content:space-between;">
          <div style="display:flex;gap:6px;">
            <button type="button" id="btnHubSave" class="btn btn-sm btn-primary">Save Hub</button>
            <button type="button" id="btnHubTest" class="btn btn-sm btn-secondary">Test</button>
          </div>
          <button type="button" id="btnHubCancel" class="btn btn-sm btn-ghost">Cancel</button>
        </div>
      </div>
    `;

    modal.style.display = 'flex';
    const close = () => { modal.style.display = 'none'; };
    modal.querySelector('#btnHubCancel').onclick = close;

    modal.querySelector('#btnHubTest').onclick = async () => {
      const stat = modal.querySelector('#hubTestStatus');
      stat.textContent = 'Testing connection...';
      const c = new HarmonyClient({
        host: modal.querySelector('#hubFormHost').value.trim(),
        port: Number(modal.querySelector('#hubFormPort').value) || 8089,
        username: modal.querySelector('#hubFormUser').value.trim(),
        password: modal.querySelector('#hubFormPass').value.trim()
      });
      const res = await c.testConnection();
      if (res.ok) stat.innerHTML = '<span style="color:var(--success)">✓ Connected!</span>';
      else stat.innerHTML = `<span style="color:var(--danger)">✗ ${res.error}</span>`;
    };

    modal.querySelector('#btnHubSave').onclick = async () => {
      h.name = modal.querySelector('#hubFormName').value.trim() || 'Harmony Hub';
      h.host = modal.querySelector('#hubFormHost').value.trim() || '127.0.0.1';
      h.port = Number(modal.querySelector('#hubFormPort').value) || 8089;
      h.username = modal.querySelector('#hubFormUser').value.trim();
      h.password = modal.querySelector('#hubFormPass').value.trim();
      await saveHub(h);
      await setActiveHubId(h.id);
      await this.loadActiveHub();
      close();
      this.renderHubsList();
    };
  }
}

document.addEventListener('DOMContentLoaded', () => {
  const app = new HarmonyApp();
  window.harmonyApp = app;
  app.init();
});
