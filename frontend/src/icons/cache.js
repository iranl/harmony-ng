/**
 * Harmony Hub & Elite Remote Offline Icon Cache
 * IndexedDB-backed local storage for all 800+ official PNG icons.
 * Supports direct streaming extraction of icons.tar.gz via DecompressionStream,
 * instant in-memory Blob URLs, and automatic Hub/OTA sync.
 */

const DB_NAME = 'harmony_icons_db';
const DB_VERSION = 1;
const STORE_NAME = 'icons';

let dbInstance = null;
const cachedBlobUrls = new Map();
const cachedKeys = new Set();
let isInitialized = false;
let initPromise = null;

function openDb() {
  if (dbInstance) return Promise.resolve(dbInstance);
  return new Promise((resolve, reject) => {
    if (typeof indexedDB === 'undefined') {
      return reject(new Error('IndexedDB not supported in this environment'));
    }
    const req = indexedDB.open(DB_NAME, DB_VERSION);
    req.onupgradeneeded = (e) => {
      const db = e.target.result;
      if (!db.objectStoreNames.contains(STORE_NAME)) {
        db.createObjectStore(STORE_NAME);
      }
    };
    req.onsuccess = (e) => {
      dbInstance = e.target.result;
      resolve(dbInstance);
    };
    req.onerror = (e) => {
      reject(e.target.error || new Error('Failed to open icons IndexedDB'));
    };
  });
}

export async function initIconCache() {
  if (isInitialized) return cachedKeys.size;
  if (initPromise) return initPromise;

  initPromise = (async () => {
    try {
      const db = await openDb();
      await new Promise((resolve) => {
        const tx = db.transaction(STORE_NAME, 'readonly');
        const store = tx.objectStore(STORE_NAME);
        const req = store.getAllKeys ? store.getAllKeys() : null;
        if (req) {
          req.onsuccess = () => {
            const keys = req.result || [];
            keys.forEach(k => cachedKeys.add(String(k)));
            resolve();
          };
          req.onerror = () => resolve();
        } else {
          // Fallback for environments lacking getAllKeys
          const curReq = store.openCursor();
          curReq.onsuccess = (e) => {
            const cursor = e.target.result;
            if (cursor) {
              cachedKeys.add(String(cursor.key));
              cursor.continue();
            } else {
              resolve();
            }
          };
          curReq.onerror = () => resolve();
        }
      });
      isInitialized = true;
      return cachedKeys.size;
    } catch (e) {
      console.warn('Icon cache init failed (will use direct HTTP):', e);
      isInitialized = true;
      return 0;
    } finally {
      initPromise = null;
    }
  })();

  return initPromise;
}

export function hasCachedIcon(filename) {
  if (!filename) return false;
  const clean = filename.split('/').pop().trim();
  return cachedKeys.has(clean);
}

export function getCachedIconUrl(filename) {
  if (!filename) return '';
  const clean = filename.split('/').pop().trim();
  if (cachedBlobUrls.has(clean)) {
    return cachedBlobUrls.get(clean);
  }
  if (cachedKeys.has(clean)) {
    // Trigger background load from IDB into blobUrlMap
    loadIconBlobAsync(clean);
  }
  return '';
}

async function loadIconBlobAsync(filename) {
  if (cachedBlobUrls.has(filename)) return cachedBlobUrls.get(filename);
  try {
    const db = await openDb();
    const blob = await new Promise((resolve, reject) => {
      const tx = db.transaction(STORE_NAME, 'readonly');
      const req = tx.objectStore(STORE_NAME).get(filename);
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error);
    });
    if (blob && (blob instanceof Blob || blob instanceof ArrayBuffer)) {
      const finalBlob = blob instanceof Blob ? blob : new Blob([blob], { type: 'image/png' });
      const url = URL.createObjectURL(finalBlob);
      cachedBlobUrls.set(filename, url);
      return url;
    }
  } catch (err) {
    console.warn(`Error loading cached icon ${filename}:`, err);
  }
  return '';
}

export async function storeIconBlob(filename, blobOrBuffer) {
  if (!filename || !blobOrBuffer) return '';
  const clean = filename.split('/').pop().trim();
  const blob = blobOrBuffer instanceof Blob ? blobOrBuffer : new Blob([blobOrBuffer], { type: 'image/png' });

  try {
    const db = await openDb();
    await new Promise((resolve, reject) => {
      const tx = db.transaction(STORE_NAME, 'readwrite');
      const store = tx.objectStore(STORE_NAME);
      const req = store.put(blob, clean);
      req.onsuccess = () => resolve();
      req.onerror = () => reject(req.error);
    });
  } catch (err) {
    console.warn(`Failed to store icon ${clean} in IndexedDB:`, err);
  }

  cachedKeys.add(clean);
  if (cachedBlobUrls.has(clean)) {
    try { URL.revokeObjectURL(cachedBlobUrls.get(clean)); } catch {}
  }
  const url = URL.createObjectURL(blob);
  cachedBlobUrls.set(clean, url);
  return url;
}

export async function cacheIconFromUrl(filename, url) {
  if (!filename || !url) return '';
  const clean = filename.split('/').pop().trim();
  if (cachedBlobUrls.has(clean)) return cachedBlobUrls.get(clean);
  try {
    const res = await fetch(url);
    if (!res.ok) return '';
    const blob = await res.blob();
    return await storeIconBlob(clean, blob);
  } catch (e) {
    return '';
  }
}

/**
 * Extract icons from gzip-compressed TAR archive (icons.tar.gz)
 * and persist them directly into IndexedDB.
 */
export async function extractAndCacheTarGz(arrayBuffer, onProgress = null) {
  if (!arrayBuffer || arrayBuffer.byteLength < 512) {
    throw new Error('Invalid or empty archive buffer');
  }

  let tarBytes;
  if (typeof DecompressionStream !== 'undefined') {
    const ds = new DecompressionStream('gzip');
    const writer = ds.writable.getWriter();
    writer.write(arrayBuffer);
    writer.close();

    const reader = ds.readable.getReader();
    const chunks = [];
    while (true) {
      const { done, value } = await reader.read();
      if (done) break;
      chunks.push(value);
    }
    const totalLen = chunks.reduce((acc, c) => acc + c.length, 0);
    tarBytes = new Uint8Array(totalLen);
    let off = 0;
    for (const c of chunks) {
      tarBytes.set(c, off);
      off += c.length;
    }
  } else {
    throw new Error('Browser does not support native DecompressionStream');
  }

  const db = await openDb();
  let ptr = 0;
  let savedCount = 0;
  const entriesToSave = [];

  while (ptr + 512 <= tarBytes.length) {
    const header = tarBytes.subarray(ptr, ptr + 512);
    let isZero = true;
    for (let i = 0; i < 512; i++) {
      if (header[i] !== 0) { isZero = false; break; }
    }
    if (isZero) break;

    let name = '';
    for (let i = 0; i < 100; i++) {
      if (header[i] === 0) break;
      name += String.fromCharCode(header[i]);
    }
    let sizeStr = '';
    for (let i = 124; i < 136; i++) {
      if (header[i] === 0 || header[i] === 32) break;
      sizeStr += String.fromCharCode(header[i]);
    }
    const size = parseInt(sizeStr.trim(), 8) || 0;
    const type = String.fromCharCode(header[156]);
    ptr += 512;

    if ((type === '0' || type === '\0') && size > 0) {
      const cleanFn = name.split('/').pop().trim();
      if (cleanFn.toLowerCase().endsWith('.png')) {
        const fileData = tarBytes.slice(ptr, ptr + size);
        entriesToSave.push({ fn: cleanFn, data: fileData });
      }
    }
    ptr += Math.ceil(size / 512) * 512;
  }

  const total = entriesToSave.length;
  const BATCH_SIZE = 100;

  for (let b = 0; b < entriesToSave.length; b += BATCH_SIZE) {
    const batch = entriesToSave.slice(b, b + BATCH_SIZE);
    await new Promise((resolve, reject) => {
      const tx = db.transaction(STORE_NAME, 'readwrite');
      const store = tx.objectStore(STORE_NAME);
      batch.forEach(({ fn, data }) => {
        const blob = new Blob([data], { type: 'image/png' });
        store.put(blob, fn);
        cachedKeys.add(fn);
        const url = URL.createObjectURL(blob);
        cachedBlobUrls.set(fn, url);
      });
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
    });
    savedCount += batch.length;
    if (onProgress) {
      onProgress({
        current: savedCount,
        total,
        percent: Math.min(100, Math.round((savedCount / total) * 100)),
        filename: batch[batch.length - 1].fn
      });
    }
  }

  isInitialized = true;
  return { count: savedCount, total };
}

/**
 * Synchronize full icon pack from connected Hub.
 * Attempts archive download (/api/icons/archive) first, with fallback to batch icon downloads.
 */
export async function syncIconsFromHub(baseUrl, client, onProgress = null) {
  const rootUrl = (baseUrl || '').replace(/\/+$/, '');
  let archiveUrl = `${rootUrl}/api/icons/archive`;

  try {
    if (onProgress) onProgress({ status: 'connecting', percent: 5, message: 'Connecting to Hub icon archive...' });
    const res = await fetch(archiveUrl);
    if (res.ok) {
      if (onProgress) onProgress({ status: 'downloading', percent: 30, message: 'Downloading icons bundle...' });
      const buf = await res.arrayBuffer();
      if (onProgress) onProgress({ status: 'extracting', percent: 60, message: 'Unpacking official icons...' });
      return await extractAndCacheTarGz(buf, (p) => {
        if (onProgress) onProgress({ status: 'storing', percent: 60 + Math.round(p.percent * 0.4), ...p });
      });
    }
  } catch (err) {
    console.warn('Archive download failed, attempting itemized sync:', err);
  }

  // Fallback: Individual downloads via /api/icons
  if (!client || typeof client.getIcons !== 'function') {
    throw new Error('Could not download icon archive and client.getIcons is unavailable');
  }

  if (onProgress) onProgress({ status: 'listing', percent: 10, message: 'Fetching icon list from Hub...' });
  const iconList = await client.getIcons();
  if (!Array.isArray(iconList) || iconList.length === 0) {
    throw new Error('Hub reports no icons available');
  }

  let done = 0;
  const total = iconList.length;
  const CONCURRENCY = 8;

  for (let i = 0; i < iconList.length; i += CONCURRENCY) {
    const chunk = iconList.slice(i, i + CONCURRENCY);
    await Promise.all(chunk.map(async (fn) => {
      const clean = String(fn).split('/').pop().trim();
      const url = `${rootUrl}/assets/icons/${clean}`;
      await cacheIconFromUrl(clean, url);
      done++;
      if (onProgress) {
        onProgress({
          status: 'downloading_itemized',
          current: done,
          total,
          percent: Math.round((done / total) * 100),
          filename: clean
        });
      }
    }));
  }

  return { count: done, total };
}

/**
 * Fallback: Download official icon archive directly from GitHub OTA mirror.
 */
export async function syncIconsFromOta(onProgress = null) {
  const otaUrl = `https://raw.githubusercontent.com/iranl/harmony-ng/ota/icons.tar.gz?_=${Date.now()}`;
  if (onProgress) onProgress({ status: 'downloading', percent: 20, message: 'Downloading icons from OTA release...' });
  const res = await fetch(otaUrl);
  if (!res.ok) {
    throw new Error(`OTA mirror returned HTTP ${res.status}`);
  }
  const buf = await res.arrayBuffer();
  if (onProgress) onProgress({ status: 'extracting', percent: 60, message: 'Unpacking official icons...' });
  return await extractAndCacheTarGz(buf, (p) => {
    if (onProgress) onProgress({ status: 'storing', percent: 60 + Math.round(p.percent * 0.4), ...p });
  });
}

export function getCachedIconsCount() {
  return cachedKeys.size;
}

export function getAllCachedKeys() {
  return Array.from(cachedKeys);
}

export async function clearIconCache() {
  cachedBlobUrls.forEach((url) => {
    try { URL.revokeObjectURL(url); } catch {}
  });
  cachedBlobUrls.clear();
  cachedKeys.clear();

  try {
    const db = await openDb();
    await new Promise((resolve, reject) => {
      const tx = db.transaction(STORE_NAME, 'readwrite');
      const store = tx.objectStore(STORE_NAME);
      const req = store.clear();
      req.onsuccess = () => resolve();
      req.onerror = () => reject(req.error);
    });
  } catch (e) {
    console.warn('Error clearing icon cache:', e);
  }
}
