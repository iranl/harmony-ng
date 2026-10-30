// Unified Harmony Client API barrel
import { HarmonyClientCore } from './client.js';
import { createActivitiesApi } from './activities.js';
import { createDevicesApi } from './devices.js';
import { createBluetoothApi } from './bluetooth.js';
import { createRemotesApi } from './remotes.js';
import { createSystemApi } from './system.js';

export class HarmonyClient extends HarmonyClientCore {
  constructor(hub = null) {
    super(hub);
    Object.assign(this, createActivitiesApi(this));
    Object.assign(this, createDevicesApi(this));
    Object.assign(this, createBluetoothApi(this));
    Object.assign(this, createRemotesApi(this));
    Object.assign(this, createSystemApi(this));
  }
}

export const api = new HarmonyClient();
export default api;
