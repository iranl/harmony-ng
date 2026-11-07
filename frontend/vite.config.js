import { defineConfig } from 'vite';
import { createHash } from 'node:crypto';
import { existsSync, readdirSync, readFileSync } from 'node:fs';

// The remote screen renderer and its themes sit unhashed in public/assets, which the Hub serves as immutable;
// the app imports it with this content hash so each new renderer reaches the browser
const rendererHash = createHash('sha256');
const themeFiles = existsSync('public/assets/themes') ? readdirSync('public/assets/themes').sort().map(f => `themes/${f}`) : [];
['remote_preview.js', 'remote_render.js', 'remote_render.wasm', ...themeFiles]
  .map(f => `public/assets/${f}`)
  .filter(f => existsSync(f))
  .forEach(f => rendererHash.update(readFileSync(f)));

export default defineConfig({
  root: './',
  base: './',
  define: {
    __REMOTE_RENDERER_VERSION__: JSON.stringify(rendererHash.digest('hex').slice(0, 12)),
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
