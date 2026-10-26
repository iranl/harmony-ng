import { defineConfig } from 'vite';

export default defineConfig({
  root: './',
  base: './',
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    chunkSizeWarningLimit: 1000,
  },
  server: {
    port: 5173,
    host: true,
  }
});
