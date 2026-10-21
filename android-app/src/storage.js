import { Preferences } from '@capacitor/preferences';

const HUBS_KEY = 'harmony_saved_hubs';
const ACTIVE_HUB_KEY = 'harmony_active_hub_id';
const LAYOUT_PREFIX = 'harmony_layout_';

async function prefGet(key) {
  try {
    const { value } = await Preferences.get({ key });
    return value ? JSON.parse(value) : null;
  } catch {
    const raw = localStorage.getItem(key);
    return raw ? JSON.parse(raw) : null;
  }
}

async function prefSet(key, value) {
  const str = JSON.stringify(value);
  try {
    await Preferences.set({ key, value: str });
  } catch {
    localStorage.setItem(key, str);
  }
}

async function prefRemove(key) {
  try {
    await Preferences.remove({ key });
  } catch {
    localStorage.removeItem(key);
  }
}

export async function getSavedHubs() {
  const hubs = await prefGet(HUBS_KEY);
  return Array.isArray(hubs) ? hubs : [];
}

export async function saveHub(hub) {
  const hubs = await getSavedHubs();
  const index = hubs.findIndex(h => h.id === hub.id);
  if (index >= 0) {
    hubs[index] = { ...hubs[index], ...hub };
  } else {
    hubs.push(hub);
  }
  await prefSet(HUBS_KEY, hubs);
  return hubs;
}

export async function deleteHub(hubId) {
  let hubs = await getSavedHubs();
  hubs = hubs.filter(h => h.id !== hubId);
  await prefSet(HUBS_KEY, hubs);
  const activeId = await getActiveHubId();
  if (activeId === hubId) {
    const nextActive = hubs[0] ? hubs[0].id : null;
    await setActiveHubId(nextActive);
  }
  return hubs;
}

export async function getActiveHubId() {
  const id = await prefGet(ACTIVE_HUB_KEY);
  if (id) return id;
  const hubs = await getSavedHubs();
  return hubs[0] ? hubs[0].id : null;
}

export async function setActiveHubId(hubId) {
  await prefSet(ACTIVE_HUB_KEY, hubId);
}

export async function getActiveHub() {
  const id = await getActiveHubId();
  if (!id) return null;
  const hubs = await getSavedHubs();
  return hubs.find(h => h.id === id) || hubs[0] || null;
}

export async function getRemoteLayout(scope, targetId) {
  const key = `${LAYOUT_PREFIX}${scope}_${targetId}`;
  return prefGet(key);
}

export async function saveRemoteLayout(scope, targetId, layout) {
  const key = `${LAYOUT_PREFIX}${scope}_${targetId}`;
  await prefSet(key, layout);
}

export async function resetRemoteLayout(scope, targetId) {
  const key = `${LAYOUT_PREFIX}${scope}_${targetId}`;
  await prefRemove(key);
}
