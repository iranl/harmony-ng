// The remote's own drawing code (tools/remote_render, built into public/assets), with the
// Hub's stock assets loaded. Resolves to { preview, module, theme }, or null in builds without
// it. theme is the remote's theme as the Hub last set it (slot 1), loaded into the module.
import { api } from '../api/index.js';

let renderer = null;

export function loadRemoteRenderer() {
  if (!renderer) {
    renderer = (async () => {
      // Versioned, because the Hub serves /assets/ as immutable
      const preview = await import(/* @vite-ignore */ new URL(`assets/remote_preview.js?v=${__REMOTE_RENDERER_VERSION__}`, document.baseURI).href);
      const r = { preview, module: await preview.loadHubRenderer(api.baseUrl), theme: 'classic' };
      const cfg = await api.request('/api/rf/remote-config?slot=1').catch(() => null);
      await setRendererTheme(r, cfg?.theme);
      return r;
    })().catch((e) => {
      console.warn('Remote renderer: not in this build', e);
      return null;
    });
  }
  return renderer;
}

// Draws with a theme from now on (Classic when the build doesn't have it)
export async function setRendererTheme(r, name) {
  r.theme = name && await r.preview.loadTheme(r.module, name) ? name : 'classic';
  return r.theme;
}
