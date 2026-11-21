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
  own.switchTab = (tab) => { calls.tabs.push(tab); };
  // The binder also sets up other panels through helpers of other mixins: those are no-ops here.
  const app = new Proxy(own, { get: (t, k) => (k in t ? t[k] : () => {}) });
  own.bindIrAndSettingsEvents.call(app);
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

check.finish();
