// Legacy web UI (android-app/): following a Hub software update from GET /api/update-status.
// - a failed update ends with its error, never with "updated"
// - "updated" only after a restart, with the version the Hub then runs
// - a restart with the old version, and an old Hub (no update_state) whose update ended without a
//   restart, are reported as not installed
// - a page loaded while an update runs follows it (checkBanners)
// - a 401 after the restart waits for a new login; a failed request during the install, a page
//   suspended past a limit and an unknown version before do not end it wrongly
// Runs in Node with a stand-in DOM, a stubbed Hub and a hand-driven clock: no browser, no Hub.
import assert from 'node:assert/strict';
import { installFakeDom, makeChecker, allowJsonImports } from './webui_fake_dom.mjs';

const { body } = installFakeDom();
await allowJsonImports();

const { HarmonyAppHubsNetwork } = await import('../android-app/src/app/app_hubs_network.js');
const { t, tf } = await import('../android-app/src/i18n.js');

let now = 1000000;
Date.now = () => now;
let pending = null;
// the watcher polls every 3 s; other timers (toasts) never run
globalThis.setTimeout = (fn, ms) => { if (ms === 3000) pending = fn; return 1; };
globalThis.clearTimeout = () => { pending = null; };

for (const id of ['hubUpdatingOverlay', 'hubUpdatingStatusText', 'updateLog']) {
  const el = document.createElement('div');
  el.id = id;
  body.appendChild(el);
}
const overlay = document.getElementById('hubUpdatingOverlay');
const log = document.getElementById('updateLog');

let reply = null; // what GET /api/update-status answers; null: the Hub does not answer; 401: login gone
let asked = 0;
function makeApp() {
  const app = Object.create(HarmonyAppHubsNetwork.prototype);
  app.refreshed = 0;
  app.refreshData = async () => { app.refreshed++; };
  app.client = {
    token: 'old-session',
    getUpdateStatus: async () => {
      asked++;
      if (reply === 401) throw Object.assign(new Error('auth'), { status: 401 });
      if (!reply) throw new Error('offline');
      return reply;
    },
    getRfStatus: async () => ({}),
    getSettings: async () => ({}),
  };
  return app;
}

// one poll 3 s later with this answer
async function poll(answer, secs = 3) {
  reply = answer;
  now += secs * 1000;
  const fn = pending;
  assert.ok(fn, 'a poll is scheduled');
  pending = null;
  await fn();
}

const check = makeChecker('Legacy web UI: Hub software update state');

await check.check('a failed update ends with its error, not "updated"', async () => {
  const app = makeApp();
  app.showUpdatingOverlay('x');
  app.watchHubUpdate('2.5.2');
  await poll({ current_version: '2.5.2', updating: true, update_state: 'running', update_step: '[*] Checking for unified Hub package...' });
  assert.equal(log.textContent, '[*] Checking for unified Hub package...');
  assert.equal(overlay.style.display, 'flex');
  const err = '[-] Update refused: no valid release signature from x. Nothing was changed.';
  await poll({ current_version: '2.5.2', updating: false, update_state: 'failed', update_error: err });
  assert.equal(log.textContent, tf('hub_update_failed_msg', { error: err }));
  assert.equal(overlay.style.display, 'none');
  assert.equal(app.pollTimerActive, false);
  assert.equal(pending, null, 'no further poll');
});

await check.check('installed: "updated" only after the restart, with the new version', async () => {
  const app = makeApp();
  app.watchHubUpdate('2.5.2');
  await poll({ current_version: '2.5.2', updating: true, update_state: 'running' });
  await poll({ current_version: '2.5.2', updating: true, update_state: 'restarting', update_target: '2.6.0' });
  assert.equal(document.getElementById('hubUpdatingStatusText').textContent, t('hub_update_restarting'));
  await poll(null);
  await poll(null);
  assert.equal(app.pollTimerActive, true, 'still waiting while the Hub is down');
  await poll({ current_version: '2.6.0', updating: false, update_state: 'idle' });
  assert.equal(log.textContent, tf('hub_update_complete_ver', { version: '2.6.0' }));
  assert.equal(app.refreshed, 1);
  assert.equal(app.pollTimerActive, false);
});

await check.check('a services restart faster than a poll: the new version is success', async () => {
  const app = makeApp();
  app.watchHubUpdate('2.5.2');
  await poll({ current_version: '2.5.2', updating: true, update_state: 'running' });
  await poll({ current_version: '2.6.0', updating: false, update_state: 'idle' });
  assert.equal(log.textContent, tf('hub_update_complete_ver', { version: '2.6.0' }));
});

await check.check('a restart with the old version: not installed', async () => {
  const app = makeApp();
  app.watchHubUpdate('2.5.2');
  await poll(null);
  await poll(null);
  await poll({ current_version: '2.5.2', updating: false, update_state: 'idle' });
  assert.equal(log.textContent, tf('hub_update_version_unchanged', { version: '2.5.2' }));
  assert.equal(app.refreshed, 0);
});

await check.check('old Hub: one failed request, then idle on the old version, is no restart', async () => {
  const app = makeApp();
  log.textContent = '';
  app.watchHubUpdate('2.5.2');
  await poll({ current_version: '2.5.2', updating: true });
  await poll(null);
  await poll({ current_version: '2.5.2', updating: false });
  assert.equal(log.textContent, '', 'not "restarted, still the old version"');
  for (let i = 0; i < 21; i++) await poll({ current_version: '2.5.2', updating: false });
  assert.equal(log.textContent, t('hub_update_no_restart'));
});

await check.check('old Hub (no update_state): the end of "updating" without a restart is no success', async () => {
  const app = makeApp();
  log.textContent = '';
  app.watchHubUpdate('2.5.2');
  await poll({ current_version: '2.5.2', updating: true });
  for (let i = 0; i < 20; i++) await poll({ current_version: '2.5.2', updating: false });
  assert.equal(app.pollTimerActive, true, 'waits up to 60 s for a restart');
  assert.equal(log.textContent, '');
  await poll({ current_version: '2.5.2', updating: false }, 5);
  assert.equal(log.textContent, t('hub_update_no_restart'));
  assert.equal(app.pollTimerActive, false);
});

await check.check('old Hub (no update_state): restart and newer version is success', async () => {
  const app = makeApp();
  app.watchHubUpdate('2.5.2');
  await poll({ current_version: '2.5.2', updating: true });
  await poll({ current_version: '2.5.2', updating: false });
  await poll(null);
  await poll({ current_version: '2.6.0', updating: false });
  assert.equal(log.textContent, tf('hub_update_complete_ver', { version: '2.6.0' }));
});

await check.check('a restart that never ends times out after 5 minutes', async () => {
  const app = makeApp();
  app.watchHubUpdate('2.5.2');
  await poll(null);
  await poll(null, 299);
  assert.equal(app.pollTimerActive, true);
  await poll(null, 5);
  assert.equal(app.pollTimerActive, true, 'one more try past the limit');
  await poll(null);
  assert.equal(log.textContent, t('update_wait_timed_out'));
  assert.equal(overlay.style.display, 'none');
});

await check.check('a page loaded during an update follows it; no answer keeps the overlay', async () => {
  const app = makeApp();
  reply = { current_version: '2.5.2', updating: true, update_state: 'running' };
  await app.checkBanners();
  assert.equal(app.pollTimerActive, true);
  assert.equal(overlay.style.display, 'flex');
  reply = null;
  await app.checkBanners();
  assert.equal(overlay.style.display, 'flex', 'the Hub restarting is no reason to close the overlay');
  await poll({ current_version: '2.6.0', updating: false, update_state: 'idle' });
  assert.equal(log.textContent, tf('hub_update_complete_ver', { version: '2.6.0' }));
});

await check.check('login lost with the restart: one 401, then wait for the new login', async () => {
  const app = makeApp();
  app.watchHubUpdate('2.5.2');
  await poll({ current_version: '2.5.2', updating: true, update_state: 'running' });
  await poll(401);
  assert.equal(log.textContent, t('hub_update_login_again'));
  assert.equal(overlay.style.display, 'none', 'the login dialog stays usable');
  const n = asked;
  for (let i = 0; i < 200; i++) await poll(401); // 10 minutes without a login
  assert.equal(asked, n, 'no request (and no new 401) until the login');
  assert.equal(app.pollTimerActive, true);
  app.client.token = 'new-session';
  await poll({ current_version: '2.6.0', updating: false, update_state: 'idle' });
  assert.equal(log.textContent, tf('hub_update_complete_ver', { version: '2.6.0' }));
});

await check.check('a failed request during the install does not start the restart limit', async () => {
  const app = makeApp();
  app.watchHubUpdate('2.5.2');
  await poll({ current_version: '2.5.2', updating: true, update_state: 'running' });
  await poll(null);
  for (let i = 0; i < 120; i++) await poll({ current_version: '2.5.2', updating: true, update_state: 'running' });
  assert.equal(app.pollTimerActive, true, 'still running after 6 minutes');
  await poll({ current_version: '2.6.0', updating: false, update_state: 'idle' });
  assert.equal(log.textContent, tf('hub_update_complete_ver', { version: '2.6.0' }));
});

await check.check('a page suspended past the limit asks the Hub once before it gives up', async () => {
  const app = makeApp();
  app.watchHubUpdate('2.5.2');
  await poll({ current_version: '2.5.2', updating: true, update_state: 'restarting', update_target: '2.6.0' });
  await poll({ current_version: '2.6.0', updating: false, update_state: 'idle' }, 360);
  assert.equal(log.textContent, tf('hub_update_complete_ver', { version: '2.6.0' }));
});

await check.check('after a suspend, a failed first request gets one more try', async () => {
  const app = makeApp();
  app.watchHubUpdate('2.5.2');
  await poll({ current_version: '2.5.2', updating: true, update_state: 'restarting', update_target: '2.6.0' });
  await poll(null, 360);
  assert.equal(app.pollTimerActive, true, 'network not up yet: not given up');
  await poll({ current_version: '2.6.0', updating: false, update_state: 'idle' });
  assert.equal(log.textContent, tf('hub_update_complete_ver', { version: '2.6.0' }));
});

await check.check('the version before unknown: the first answer is the base', async () => {
  const app = makeApp();
  app.watchHubUpdate('');
  await poll({ current_version: '2.5.2', updating: true, update_state: 'running' });
  await poll(null);
  await poll(null);
  await poll({ current_version: '2.5.2', updating: false, update_state: 'idle' });
  assert.equal(log.textContent, tf('hub_update_version_unchanged', { version: '2.5.2' }));
  app.watchHubUpdate('');
  await poll(null);
  await poll(null);
  await poll({ current_version: '2.5.2', updating: false }); // old Hub, nothing known
  assert.equal(log.textContent, tf('hub_update_ended_ver', { version: '2.5.2' }), 'no success with the old version');
});

await check.check('a failure without an error line still says it failed', async () => {
  const app = makeApp();
  app.watchHubUpdate('2.5.2');
  await poll({ current_version: '2.5.2', updating: false, update_state: 'failed' });
  assert.equal(log.textContent, tf('hub_update_failed_msg', { error: t('unknown_lc') }));
});

check.finish();
