// The Elite remote's own drawing code (tools/elite_render, built into public/assets), with the
// Hub's stock assets loaded. Resolves to { preview, module }, or null in builds without it.
import { api } from '../api/index.js';

let renderer = null;

export function loadEliteRenderer() {
  if (!renderer) {
    renderer = (async () => {
      // Versioned, because the Hub serves /assets/ as immutable
      const preview = await import(/* @vite-ignore */ new URL(`assets/elite_preview.js?v=${__ELITE_RENDERER_VERSION__}`, document.baseURI).href);
      return { preview, module: await preview.loadHubRenderer(api.baseUrl) };
    })().catch((e) => {
      console.warn('Elite renderer: not in this build', e);
      return null;
    });
  }
  return renderer;
}
