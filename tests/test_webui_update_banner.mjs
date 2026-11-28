// The "Update Hub" button of the legacy web UI's update banner must start the update, as
// Install Update on the System page does, and not only open that page.
// Runs in Node with a stand-in DOM and a stubbed Hub: no browser, no Hub.
import assert from 'node:assert/strict';
import { installFakeDom, makeChecker, tick, allowJsonImports } from './webui_fake_dom.mjs';

const { body } = installFakeDom();
await allowJsonImports();
globalThis.location.hash = '';

const posts = [];
globalThis.fetch = async (url, opts = {}) => {
  const path = new URL(String(url), 'http://192.168.1.20/').pathname;
  if ((opts.method || 'GET') === 'POST') posts.push({ path, body: new URLSearchParams(opts.body) });
  return new Response('{}', { status: 200, headers: { 'Content-Type': 'application/json' } });
};

const check = makeChecker('Web UI update banner button');
const { HarmonyAppEventsActions } = await import('../android-app/src/app/app_events_actions.js');

function setup() {
  for (const n of [...body.walk()]) if (n.id && n.parentNode) n.parentNode.removeChild(n);
  const btn = document.createElement('button');
  btn.id = 'btnHubUpdateBanner';
  body.appendChild(btn);
  const form = document.createElement('form');
  form.id = 'updateForm';
  let scrolled = 0;
  form.scrollIntoView = () => { scrolled++; };
  body.appendChild(form);
  const calls = { install: 0, tabs: [] };
  const own = Object.create(HarmonyAppEventsActions.prototype);
  own.installSoftwareUpdate = async () => { calls.install++; };
  own.bannerReleaseUrl = '';
  own.switchTab = (tab) => { calls.tabs.push(tab); };
  // The binder also sets up other panels through helpers of other mixins: those are no-ops here.
  const app = new Proxy(own, { get: (t, k) => (k in t ? t[k] : () => {}) });
  own.bindIrAndSettingsEvents.call(app);
  btn.app = own;
  return { btn, calls, scrolled: () => scrolled };
}

await check.check('banner button starts the install', async () => {
  const { btn, calls, scrolled } = setup();
  await btn.fire('click');
  await tick(5);
  assert.equal(calls.install, 1, 'installSoftwareUpdate called once');
  assert.deepEqual(calls.tabs, [], 'no tab switch');
  assert.equal(scrolled(), 0, 'no scroll to the update card');
});

await check.check('a release for the bundle only opens its page instead', async () => {
  const { btn, calls } = setup();
  const opened = [];
  const prevOpen = globalThis.window.open;
  globalThis.window.open = (url) => { opened.push(url); };
  try {
    btn.app.bannerReleaseUrl = 'https://github.com/iranl/harmony-ng/releases/tag/v4.0.0';
    await btn.fire('click');
    await tick(5);
  } finally {
    globalThis.window.open = prevOpen;
  }
  assert.equal(calls.install, 0, 'no install');
  assert.deepEqual(opened, ['https://github.com/iranl/harmony-ng/releases/tag/v4.0.0'], 'release page opened');
});

const { HarmonyAppHubsNetwork } = await import('../android-app/src/app/app_hubs_network.js');

await check.check('online version: beta takes the newer branch, VERSION_V4, page never from release.json', async () => {
  const app = Object.create(HarmonyAppHubsNetwork.prototype);
  const files = {};
  const prevFetch = globalThis.fetch;
  globalThis.fetch = async (url) => {
    const path = new URL(String(url)).pathname.replace('/iranl/harmony-ng/', '');
    return path in files ? new Response(files[path], { status: 200 }) : new Response('', { status: 404 });
  };
  try {
    files['ota/v2/VERSION'] = '3.5.1\n';
    files['ota/VERSION_V4'] = '4.0.0\n';
    app.updateChannel = 'beta';
    // no ota-beta branch: the stable release
    assert.equal(await app.fetchOnlineHubVersion(), '3.5.1');
    assert.equal(app.onlineNextMajor, '4.0.0');
    files['ota-beta/v2/VERSION'] = '3.5.0\n';
    assert.equal(await app.fetchOnlineHubVersion(), '3.5.1', 'a full release newer than the pre-release');
    files['ota-beta/v2/VERSION'] = '3.6.0\n';
    files['ota-beta/v2/release.json'] = JSON.stringify({ version: '3.6.0', ota_installable: false, release_url: 'https://example.org/x.zip' });
    assert.equal(await app.fetchOnlineHubVersion(), '3.6.0');
    assert.equal(app.bundleOnlyUrl('3.5.1', '3.6.0'), 'https://github.com/iranl/harmony-ng/releases/tag/v3.6.0');
    assert.equal(app.bundleOnlyUrl('3.5.1', '4.0.0'), 'https://github.com/iranl/harmony-ng/releases/tag/v4.0.0');
    files['ota-beta/v2/VERSION'] = '4.0.0\n';
    assert.equal(await app.fetchOnlineHubVersion(), '3.5.1', 'a v4 pre-release is not followed');
    app.updateChannel = 'stable';
    assert.equal(await app.fetchOnlineHubVersion(), '3.5.1');
    assert.equal(app.bundleOnlyUrl('3.5.0', '3.5.1'), '', 'same major, no notice: installs online');
  } finally {
    globalThis.fetch = prevFetch;
  }
});

await check.check('a mirror installs instead of opening the release page', async () => {
  setup();
  const repo = document.createElement('input');
  repo.id = 'updateRepo';
  repo.value = 'https://mirror.example/hng';
  body.appendChild(repo);
  const opened = [];
  const prevOpen = globalThis.window.open;
  globalThis.window.open = (url) => { opened.push(url); };
  try {
    const app = Object.create(HarmonyAppHubsNetwork.prototype);
    app.bannerReleaseUrl = 'https://github.com/iranl/harmony-ng/releases/tag/v4.0.0';
    app.client = { getUpdateStatus: async () => ({}), applyUpdate: async () => ({}) };
    await Promise.race([app.installSoftwareUpdate().catch(() => {}), tick(5)]);
  } finally {
    globalThis.window.open = prevOpen;
  }
  assert.deepEqual(opened, [], 'no release page for a mirror');
});

check.finish();
