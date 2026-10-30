// Elite screen preview: draws the remote's screens with the firmware's own renderer, built
// to WebAssembly by tools/elite_render/build.sh --wasm public/assets. When the module is
// missing (a build without Emscripten), loadEliteScreen() rejects and the page keeps its
// HTML mock-up.

let loading = null;

// Loads the renderer and the stock assets from the Hub (once per page)
export function loadEliteScreen(hubBaseUrl) {
  if (!loading) {
    loading = (async () => {
      const api = await import(/* @vite-ignore */ new URL('assets/elite_preview.js', document.baseURI).href);
      const module = await api.loadRenderer();
      try {
        const tar = await api.fetchTar(`${hubBaseUrl || ''}/api/icons/archive`);
        api.writeTar(module, tar, api.STOCK_ASSET_DIR, 1); // drop the leading "icons/"
      } catch (e) {
        console.warn('Elite preview: no stock assets from the Hub', e);
      }
      return { api, module };
    })();
  }
  return loading;
}

// The activity ID as the remote stores it: the RF frame keeps 3 characters of short IDs
// and 8 of longer ones
export function remoteActivityId(id) {
  const s = String(id);
  return s.slice(0, s.length <= 2 ? 3 : 8);
}

// Writes /data/elite_rf_config.json the way the remote stores the Hub's config push
// (payload/source/rf_config_push.c, decoded by elite_rf_sync.c), including the field
// lengths the RF frames cut names to. Custom icons (which the Hub numbers from 0x8001 and
// streams to the remote) aren't known here, so those entries show their stock fallback.
export function writeRemoteConfig(screen, { activities = [], devices = [], eliteMap = {} }) {
  const { api, module } = screen;
  // name: as the Hub reads it; stored: as the remote keeps it after the frame cut it
  const iconFields = (icon, name, type, stored) => {
    const iconId = api.stockIconId(module, icon, name);
    return { icon: api.iconKeyword(module, icon, name, type, stored), ...(iconId ? { icon_id: iconId } : {}) };
  };
  const acts = activities.filter((a) => String(a.id) !== '-1' && (a.name || a.Name)).slice(0, 16).map((a) => {
    const name = String(a.name || a.Name);
    const stored = name.slice(0, String(a.id).length <= 2 ? 17 : 12); // short IDs leave 17 characters
    return { id: remoteActivityId(a.id), name: stored, ...iconFields(a.icon || a.Icon, name, a.type, stored) };
  });
  const devs = devices.filter((d) => d.id != null && (d.name || d.Name)).slice(0, 16).map((d) => {
    const name = String(d.name || d.Name);
    const stored = name.slice(0, 16);
    const type = d.type || d.Type || d.deviceType || d.DeviceTypeDisplayName;
    return {
      id: String(d.id).slice(0, 4),
      name: stored,
      ...iconFields(d.icon || d.Icon, name, type, stored),
      commands: (d.commands || []).slice(0, 255).map((c) => String(typeof c === 'string' ? c : c.name || '').slice(0, 22)),
    };
  });

  const buttons = [];
  let hasPowerOff = false;
  for (const [ctx, entry] of Object.entries(eliteMap.activities || {})) {
    if (!entry || typeof entry !== 'object') continue;
    const screenButtons = Array.isArray(entry.screen_buttons) ? entry.screen_buttons : [];
    if (screenButtons.length > 0) {
      let i = 0;
      for (const b of screenButtons) {
        if (buttons.length >= 48) break;
        if (!b || typeof b !== 'object') continue;
        buttons.push({ ctx: ctx.slice(0, 9), label: String(b.label || 'Button').slice(0, 12), ctype: 1, atype: 1, bidx: i++ });
      }
      continue;
    }
    let i = 0;
    for (const [name, b] of Object.entries(entry)) {
      if (buttons.length >= 48) break;
      if (name === 'screen_buttons' || !b || typeof b !== 'object') continue;
      if (/^(pwroff|poweroffactivity)$/i.test(name)) hasPowerOff = true;
      let atype = 1;
      let target = ctx;
      if (b.action === 'activity_start') { atype = 2; target = String(b.activityId || ctx); }
      if (b.action === 'activity_stop') { atype = 3; target = '-1'; }
      buttons.push({ ctx: target.slice(0, 9), label: name.slice(0, 12), ctype: 0, atype, bidx: i++ });
    }
  }
  if (!hasPowerOff && buttons.length < 48) buttons.push({ ctx: '-1', label: 'PwrOff', ctype: 0, atype: 3, bidx: 0 });

  const config = { version: 0, slot: 1, activities: acts, devices: devs, buttons, steps: [] };
  api.writeFile(module, '/data/elite_rf_config.json', new TextEncoder().encode(JSON.stringify(config)));
  return config;
}

// state: { view: 'activities'|'commands'|'devices', active, select }
export function renderEliteScreen(screen, canvas, state) {
  const now = new Date();
  screen.api.render(screen.module, canvas, {
    battery: 100, minutes: now.getHours() * 60 + now.getMinutes(), ...state,
  });
}
