/**
 * Harmony Hub Web & Android App - Modular Index
 */
export * from './app/app_icons_ui.js';
import { HarmonyAppHubsNetwork } from './app/app_hubs_network.js';

export class HarmonyApp extends HarmonyAppHubsNetwork {}

document.addEventListener('DOMContentLoaded', () => {
  const app = new HarmonyApp();
  window.harmonyApp = app;
  app.init();
});
