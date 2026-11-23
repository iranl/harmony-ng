/**
 * Harmony Hub Web & Android App - Modular Index
 */
export * from './app/app_icons_ui.js';
import { HarmonyAppPlugins } from './app/app_plugins.js';

export class HarmonyApp extends HarmonyAppPlugins {}

document.addEventListener('DOMContentLoaded', () => {
  const app = new HarmonyApp();
  window.harmonyApp = app;
  app.init();
});
