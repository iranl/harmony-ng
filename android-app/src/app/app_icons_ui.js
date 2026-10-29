import { HarmonyClient } from '../api.js';
import { getSavedHubs, saveHub, deleteHub, getActiveHub, setActiveHubId } from '../storage.js';
import { RemoteEngine } from '../remote-engine.js';
import {
  getHarmonyManufacturers,
  searchHarmonyManufacturers,
  fetchHarmonyModels,
  searchHarmonyModels,
  fetchHarmonyDeviceDetail,
  fetchHarmonyCodeset,
  mapHarmonyDeviceType,
  buildDeviceFromHarmony,
  parseIrText,
  loadIrdSource,
  rankIrdEntries,
  fetchIrdFile,
  sourceLabel,
  safeImportName
} from '../irdb.js';
import { t, setLanguage, getInitialLanguage } from '../i18n.js';
import iconMetadata from '../icon-metadata.json';
import {
  initIconCache,
  hasCachedIcon,
  getCachedIconUrl,
  cacheIconFromUrl,
  syncIconsFromHub,
  syncIconsFromOta,
  getCachedIconsCount,
  clearIconCache,
  getAllCachedKeys
} from '../icon-cache.js';

export const APP_VERSION = '2.0.0';

export let stockIconsAvailable = false;
export let stockIconBaseUrl = '';
export const failedIconUrls = new Set();

export const STOCK_ICON_ALIASES = {
  'watchtv.png': 'tv.png',
  'smarttv.png': 'tv.png',
  'watchtv': 'tv.png',
  'smarttv': 'tv.png',
  'movie.png': 'BAB09C4A-E61D-4284-B1F3-84135F562217_small.png',
  'music.png': '72D0AAB4-2ED8-4644-84D2-0031BDAE356E_small.png',
  'apple.png': '0F40F257-68C8-4F24-9561-3CB3441DA395_small.png',
  'roku.png': '6EBD03B8-C815-48BE-ADBB-D36E06CAAC5E_small.png',
  'pc.png': 'computer.png',
  'receiver.png': 'heos.png',
  'light.png': 'bulb.png',
  'poweroff.png': 'power_off.png',
  'power.png': 'power_icon.png',
  'generic.png': 'default.png'
};

export function setStockIconBaseUrl(url) {
  stockIconBaseUrl = (url || '').replace(/\/+$/, '');
}

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
  const key = img.getAttribute('data-icon-key') || img.src || '';
  if (isDarkMonochromeIcon(key)) {
    img.classList.add('invert-white');
  } else {
    img.classList.remove('invert-white');
  }
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

if (typeof window !== 'undefined') {
  window.__harmonyCacheIcon = (src) => {
    // Only cache dynamically into IndexedDB in native mobile app
    if (!window.Capacitor?.isNativePlatform?.()) return;
    if (!src || typeof src !== 'string' || src.startsWith('blob:') || src.startsWith('data:')) return;
    const fn = src.split('/').pop().split('?')[0];
    if (fn && fn.toLowerCase().endsWith('.png') && !hasCachedIcon(fn)) {
      cacheIconFromUrl(fn, src).catch(() => { });
    }
  };
}

const STOCK_DARK_ICONS = new Set([
  "013a19aa-dfb1-40e8-80a3-773c7a0ae986_smallon.png", "01d34356-613c-45f3-935d-4bf763ae4f4b_smallon.png",
  "053fa80a-458f-4ea9-85e5-b2f7db1c7052_smallon.png", "085a8031-090d-4052-bb5f-c7809dce8665_smallon.png",
  "0d75b8c1-3ee5-40c2-995e-34c8adedaaa4_smallon.png", "0df1488a-eedb-40ec-8bc7-b87e16e13dc2_smallon.png",
  "0e588723-18da-4df7-978f-0306f601fe28_smallon.png", "0f40f257-68c8-4f24-9561-3cb3441da395_smallon.png",
  "16.png", "1861d98d-b831-4dd7-96dd-58fd0ea1b045_smallon.png", "1978f30f-e27f-4126-b622-fc230936ae2f_smallon.png",
  "204.png", "204f0071-4b64-4577-bda9-e726d6480abf_smallon.png", "21aa7d88-e238-49b3-9933-276232b7dc6c_smallon.png",
  "257c953f-b575-4f4d-8429-f8eaa1de8241_off.png", "257c953f-b575-4f4d-8429-f8eaa1de8241_smallon.png",
  "27bbf517-b5ea-46ce-8155-08f6d48e0bce_smallon.png", "29281b8d-fc8d-4c13-89c4-60705b8429f9_smallon.png",
  "31179e2f-bd17-48e9-95c0-22ceb36c6188_smallon.png", "311d40b5-6cf9-4da2-8820-e17012b852a1_smallon.png",
  "349826b4-fa41-4af4-b032-d5280b1363ac_off.png", "349826b4-fa41-4af4-b032-d5280b1363ac_smallon.png",
  "363ce48c-b589-48fa-99d9-6ae5056c1e9c_smallon.png", "36d512d1-9f0b-4084-89a1-63b6947e9587_smallon.png",
  "3aebfa09-de9a-46f5-86fb-ac5e585295fb_smallon.png", "3bf99e7b-9904-4afa-8d3d-7337cd8e9bfd_smallon.png",
  "3d057f31-87ad-4ad5-bcfd-453403a21d29_smallon.png", "3d423017-ba9e-4132-a885-69222cfe4842_smallon.png",
  "3fb5eead-419d-472f-8232-3bbbed1f4874_smallon.png", "41bf3aa6-da1d-4115-a225-e99f475d7dc7_smallon.png",
  "46700828-34b6-42b9-9d6a-968bc681172a_smallon.png", "468668ab-2587-4662-9e89-cf66bd70f314_smallon.png",
  "4a703d51-717d-4bd3-8457-3442879a7dcc_smallon.png", "4ae30392-221f-4718-9e17-ace55f27cd3d_off.png",
  "4ae30392-221f-4718-9e17-ace55f27cd3d_smallon.png", "4d002443-a954-47c3-98c5-f24e53b1e628_smallon.png",
  "50ea2de1-d490-4c34-bda9-f93b5b4342fc_smallon.png", "5180221e-8f0c-459e-a61c-6d6e02bb2a0c_smallon.png",
  "5248477c-d901-49e0-a8a6-24f212443817_smallon.png", "53469038-7e1c-4241-b9bd-bae95cc0509c_smallon.png",
  "58df2164-056e-4965-a911-cae9621e0885_smallon.png", "5bbc2bc3-551a-493b-b374-3ec77eb5b1eb_smallon.png",
  "5be0f80a-d144-40b0-a467-b67a7633282a_smallon.png", "5f5e0c9d-1d5a-4f6f-9dd4-243afccd8285_smallon.png",
  "6231922f-ab2e-4ae1-a111-41b09b6f3e08_off.png", "6231922f-ab2e-4ae1-a111-41b09b6f3e08_smallon.png",
  "63677e44-c3ac-46cb-873d-e948bb64a727_smallon.png", "64657363-41a2-4ebd-a5fd-ba7cf83de4d3_smallon.png",
  "68aa6991-a06f-49fa-9be6-fe882e03d979_off.png", "68aa6991-a06f-49fa-9be6-fe882e03d979_smallon.png",
  "6d621a24-72d6-4ccd-8ed8-1114b52952ae_smallon.png", "6e24f18e-ea94-4a87-a4ae-1f2faf1a1099_smallon.png",
  "6ebd03b8-c815-48be-adbb-d36e06caac5e_smallon.png", "718b691b-df39-4787-989d-eab9d39d595e_smallon.png",
  "72d0aab4-2ed8-4644-84d2-0031bdae356e_smallon.png", "7a38712d-0bb0-4d3c-b9af-d54ff86cc9cc_smallon.png",
  "7b7ff0c6-cc71-498d-8b1e-c029cbb07cf3_smallon.png", "818ad711-eac5-42e8-80dc-d5ed9f1046f4_smallon.png",
  "818d06a7-7821-4380-8a62-719a9d6b9c9b_smallon.png", "819abab3-94ad-4798-8b71-bd98c829eb89_smallon.png",
  "835b9806-a8f5-4395-9ae7-a1086d75f5d3_smallon.png", "840ad7c6-5295-465f-a6ae-9bc9154b6549_off.png",
  "840ad7c6-5295-465f-a6ae-9bc9154b6549_smallon.png", "86d46e83-04ea-460a-b3dc-70a0246c7794_smallon.png",
  "897101b2-567b-11e7-907b-a6006ad3dba0_smallon.png", "8a4cc6cd-16e8-40e4-8c91-2917076f967d_off.png",
  "8a4cc6cd-16e8-40e4-8c91-2917076f967d_smallon.png", "8b271ada-d2ed-477b-bff2-d2450245c3b0_smallon.png",
  "8c07fd25-a715-4a64-a4f9-46b0b7a0c1ef_smallon.png", "8d8f352a-131e-11e2-8581-b78bf89f9455.png",
  "93ce3aba-5fed-4374-adc1-d5cd36dedb36_smallon.png", "9532487a-4fbf-4bef-add3-3cde6ee8db16_off.png",
  "9532487a-4fbf-4bef-add3-3cde6ee8db16_smallon.png", "970c1e6e-9e37-48f1-baf0-eabddb9e989b_smallon.png",
  "974d2e15-d103-463a-8386-36b9ebd4037f_smallon.png", "9b0478b4-cb51-4cc8-b58e-682033dfc8ef_smallon.png",
  "9c0b36c2-f503-41fa-b957-4a0bcc7e4d8f_smallon.png", "9ccf9a96-b984-4aeb-9227-9b565ff74e06_off.png",
  "9ccf9a96-b984-4aeb-9227-9b565ff74e06_smallon.png", "9cec604e-b876-4953-88d2-7cd6283d3275_smallon.png",
  "9d24eb35-468b-4148-a6a2-bb764aa9609c_smallon.png", "9d3ea5d0-9e27-4488-922e-f706c7fa1a8c_smallon.png",
  "9d6b957c-8a14-4c92-adee-dd0c8fbb2366_off.png", "9d6b957c-8a14-4c92-adee-dd0c8fbb2366_smallon.png",
  "a1164c5f-1d7a-4b7f-bbec-e9f5acf303b7_smallon.png", "a12ff2be-5c15-4e5d-8e51-1f328716ef3d_smallon.png",
  "a181d535-d966-4178-91f9-57278be8d7ea_off.png", "a181d535-d966-4178-91f9-57278be8d7ea_smallon.png",
  "a3a7fe08-e4dd-4b05-b0a9-945cd717a09b_off.png", "a3a7fe08-e4dd-4b05-b0a9-945cd717a09b_smallon.png",
  "a54ab6f8-8fbe-4a90-a43e-2c9993961efd_smallon.png", "a648ac27-8b04-4cad-a2b2-82a83bcdb741_smallon.png",
  "ab95c212-7b27-4118-8cd3-0e595ea061d5_smallon.png", "ac921110-8f9c-40a3-b51c-06f3b1e18c92_off.png",
  "ac921110-8f9c-40a3-b51c-06f3b1e18c92_smallon.png", "activitygreyline.png", "addphoto_small.png",
  "arrow_off.png", "b25a8282-30ce-4ea3-9f92-3e25474b076b_smallon.png", "b4241984-3855-46c7-a940-3604743297bd_off.png",
  "b4241984-3855-46c7-a940-3604743297bd_smallon.png", "bab09c4a-e61d-4284-b1f3-84135f562217_smallon.png",
  "background.png", "bf4c747e-c437-423e-9801-535e0ea55848_smallon.png", "bg_check_box.png",
  "black_line.png", "blinds_off.png", "bri.png", "bulb_off.png", "buttonbg.png",
  "c45b3641-9ce3-43d8-907f-f87838a86c84_smallon.png", "c4d124b8-f851-4200-86c0-b38494fe6167_smallon.png",
  "c62c7998-d6bf-4c2d-9e6c-47440e28a113_smallon.png", "c83a52e3-5615-4a0b-987e-58c3d85ffccd_off.png",
  "c83a52e3-5615-4a0b-987e-58c3d85ffccd_smallon.png", "cb589261-82db-4113-bc1b-c09ec6e17b2c_smallon.png",
  "cd78e011-e7fd-42ea-b6d8-fafca64b9c09_smallon.png", "ceda3f57-1d34-48f0-852d-ec9c425b2733_smallon.png",
  "cedd3f0f-932a-4135-8f2d-1670550f20ee_smallon.png", "cfa819f4-db13-4f04-985d-d59087108d62_smallon.png",
  "chevron.png", "covering_off.png", "d1d5e928-d0ad-488a-a43e-56fd6d17209b_smallon.png",
  "d22a587c-e45c-4b4a-9396-19396eb8e32b_smallon.png", "d4f91d44-f634-41fb-a9be-fc8f63abced2_smallon.png",
  "d636fd4e-016c-447f-ba4a-25e8943e23f4_smallon.png", "d7f21109-8f71-4edb-990a-bcab435efb92_off.png",
  "d7f21109-8f71-4edb-990a-bcab435efb92_smallon.png", "dec07396-b858-47e8-ae50-4ab346af1d98_smallon.png",
  "default.png", "device_group2.png", "device_group3.png", "device_group4.png",
  "devicelock_lock.png", "dff3a4b7-b2a5-41f6-8347-77cac20a7dd0_smallon.png", "doorlock_lock.png",
  "e816d949-6f22-4fa2-a278-17fabe2cfcc9_smallon.png", "e942edfa-826d-4b06-9e8f-073f8895e69c_smallon.png",
  "eac61c29-abb4-4545-9f69-85010b4ccad5_off.png", "eac61c29-abb4-4545-9f69-85010b4ccad5_smallon.png",
  "eb6113e5-431d-4642-9348-f1fd23207b69_smallon.png", "ec6648b8-49ee-4457-95e4-f31b242d989a_off.png",
  "ec6648b8-49ee-4457-95e4-f31b242d989a_smallon.png", "edcd349e-0bb5-4320-a1ae-d409bcfba325_smallon.png",
  "efd06b84-dce8-4bf7-af0c-afac990862b5_smallon.png", "f30d50e9-96a2-4a44-92d5-9a42431f83da_off.png",
  "f30d50e9-96a2-4a44-92d5-9a42431f83da_smallon.png", "fa224ad1-a6b0-4fb3-84a1-9ee204c0dcd9_smallon.png",
  "fad05710-2621-4042-9dd4-b3466665a584_off.png", "fad05710-2621-4042-9dd4-b3466665a584_smallon.png",
  "fcf344ce-4029-406c-8e11-582d7a7bfb08_smallon.png", "fdc89d40-dbbf-469b-9fc9-b24a6651fe6a_smallon.png",
  "firetv.png", "gesture_bg.png", "gesture_cue.png", "heos.png", "hopsdevice_blaster.png",
  "hopsdevice_group2.png", "hopsdevice_group3.png", "hopsdevice_group4.png", "hopsgesture_box.png",
  "hopsmenu.png", "hopsmenu_sleeptimer.png", "hopspower_icon.png", "lamp_off.png",
  "lock_lock.png", "menu-bg.png", "minus_normal.png", "myharmonyblocking.png",
  "myharmonydismiss.png", "pc_wifi_hops.png", "pc_wifi_remote.png", "plug_off.png",
  "pluggroup_off.png", "plus_normal.png", "power_off.png", "presence_off.png",
  "ps.png", "scene-default_jrf.png", "sensor_off.png", "separator.png",
  "shield.png", "sonos.png", "text_btn_hover.png", "thermostat_off.png",
  "toolbarbg.png", "toolbarbgwithblackline.png", "toolbarblackline.png", "waterheater_off.png",
  "white_line.png", "wii_bg.png", "xbox.png"
]);

export function isCustomIcon(iconKey) {
  if (!iconKey || typeof iconKey !== 'string') return false;
  const clean = iconKey.split('/').pop().trim().toLowerCase();
  if (clean.startsWith('custom_')) return true;
  if (typeof window !== 'undefined' && window.__harmonyCustomIcons && window.__harmonyCustomIcons.has(clean)) return true;
  return false;
}

export function isDarkMonochromeIcon(iconKey) {
  if (!iconKey || typeof iconKey !== 'string') return false;
  if (isCustomIcon(iconKey)) return false;
  const clean = iconKey.split('/').pop().trim().toLowerCase();
  const withExt = clean.endsWith('.png') ? clean : `${clean}.png`;
  if (STOCK_DARK_ICONS.has(withExt) || clean.endsWith('_off.png') || clean.endsWith('_smallon.png')) {
    return true;
  }
  return false;
}

export function getStockIconUrl(iconKey, hint = '') {
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

  const lowerFn = fn.toLowerCase();
  if (STOCK_ICON_ALIASES[lowerFn]) {
    fn = STOCK_ICON_ALIASES[lowerFn];
  }

  // 1. Check local IndexedDB in-memory Blob URL cache first
  const cachedUrl = getCachedIconUrl(fn);
  if (cachedUrl) return cachedUrl;

  // 2. Custom icon check
  const isCustom = isCustomIcon(fn);
  if (!stockIconsAvailable && !hasCachedIcon(fn) && !isCustom) return '';

  const base = stockIconBaseUrl || '';
  const url = base ? `${base}/assets/icons/${fn}` : `/assets/icons/${fn}`;
  if (failedIconUrls.has(url)) return '';
  return url;
}

function renderWithStockIconFallback(imgUrl, fallbackSvg, size, originalIconKey = '') {
  if (!imgUrl || failedIconUrls.has(imgUrl)) return fallbackSvg;
  const keyToCheck = originalIconKey || imgUrl;
  const shouldInvert = isDarkMonochromeIcon(keyToCheck);
  return `<span class="stock-icon-wrap"><img src="${imgUrl}" data-icon-key="${keyToCheck}" class="stock-icon-img ${shouldInvert ? 'invert-white' : ''}" loading="lazy" decoding="async" width="${size}" height="${size}" onload="if(window.__harmonyCacheIcon)window.__harmonyCacheIcon(this.src);if(window.__harmonyAutoInvert)window.__harmonyAutoInvert(this)" onerror="if(window.__harmonyFailedIcons)window.__harmonyFailedIcons.add('${imgUrl}');this.style.display='none';if(this.nextElementSibling)this.nextElementSibling.style.display='inline-block'" />${fallbackSvg}</span>`;
}

// Activity icons are keywords; the Hub sends them to the Elite, which draws its stock icon
// for each keyword (resolve_activity_icon in elite/elite_config.c). These are the same stock images,
// so the web UI shows what the remote shows.
const ACTIVITY_ICON_PREVIEWS = {
  watchtv: '3BF99E7B-9904-4AFA-8D3D-7337CD8E9BFD_small.png',
  tv: '3BF99E7B-9904-4AFA-8D3D-7337CD8E9BFD_small.png',
  smarttv: '3BF99E7B-9904-4AFA-8D3D-7337CD8E9BFD_small.png',
  movie: 'BAB09C4A-E61D-4284-B1F3-84135F562217_small.png',
  music: '72D0AAB4-2ED8-4644-84D2-0031BDAE356E_small.png',
  game: '9D24EB35-468B-4148-A6A2-BB764AA9609C_small.png',
  switch: '9D24EB35-468B-4148-A6A2-BB764AA9609C_small.png',
  ps: 'ps.png',
  xbox: 'xbox.png',
  shield: 'shield.png',
  apple: '0F40F257-68C8-4F24-9561-3CB3441DA395_small.png',
  roku: '6EBD03B8-C815-48BE-ADBB-D36E06CAAC5E_small.png',
  firetv: 'firetv.png',
  pc: '468668AB-2587-4662-9E89-CF66BD70F314_small.png',
  sonos: 'sonos.png',
  receiver: '53469038-7E1C-4241-B9BD-BAE95CC0509C_small.png',
  light: '53469038-7E1C-4241-B9BD-BAE95CC0509C_small.png',
  power: '116.png',
  poweroff: '116.png',
  generic: '53469038-7E1C-4241-B9BD-BAE95CC0509C_small.png'
};

export function activityIconPreview(iconKey) {
  const k = String(iconKey || '').split('/').pop().trim().toLowerCase().replace(/\.png$/, '');
  return ACTIVITY_ICON_PREVIEWS[k] || '';
}

export function getActivityIconSvg(iconKey, actType, size = 14) {
  const fallbackSvg = getRawActivityIconSvg(iconKey, actType, size);
  const previewKey = activityIconPreview(iconKey) || iconKey;
  return renderWithStockIconFallback(getStockIconUrl(previewKey, actType), fallbackSvg, size, previewKey || actType);
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

