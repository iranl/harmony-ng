import { defineConfig } from 'vite';
import { createHash } from 'node:crypto';
import { existsSync, readFileSync } from 'node:fs';

// The Elite screen renderer sits unhashed in public/assets, which the Hub serves as immutable;
// the app imports it with this content hash so each new renderer reaches the browser
const rendererHash = createHash('sha256');
['elite_preview.js', 'elite_render.js', 'elite_render.wasm']
  .map(f => `public/assets/${f}`)
  .filter(f => existsSync(f))
  .forEach(f => rendererHash.update(readFileSync(f)));

export default defineConfig({
  root: './',
  base: './',
  define: {
    __ELITE_RENDERER_VERSION__: JSON.stringify(rendererHash.digest('hex').slice(0, 12)),
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    chunkSizeWarningLimit: 1200,
  },
  server: {
    port: 5174,
    host: true,
  }
});
