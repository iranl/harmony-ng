// New web UI (frontend/): the setup views read in the chosen language. Builds each view once in
// English and once in German (the Hub answers nothing: fetch fails) and compares the headings,
// labels and buttons: a text that stays the same in German is hard-coded English, unless it is a
// name or code. Also checks that no text shows "undefined" or an unfilled {placeholder}.
// Runs in Node with a stand-in DOM: no browser, no Hub.
import assert from 'node:assert/strict';
import { installFakeDom, makeChecker, tick, allowJsonImports } from './webui_fake_dom.mjs';

installFakeDom();
await allowJsonImports();
globalThis.fetch = async () => { throw new Error('no hub in this test'); };

const i18n = await import('../frontend/src/i18n/index.js');
const t = makeChecker('New web UI views in the chosen language');

const VIEWS = ['activities', 'b25-remote', 'backup', 'bt-hosts', 'bt-keyboard', 'devices-studio',
  'harmony-search', 'hubs', 'ir-lab', 'mqtt', 'network', 'remote', 'remote-buttons', 'system', 'users'];
// Same text in English and German on purpose: names, codes, and words German shares
const SAME_OK = new Set(['MQTT', 'Home Assistant', 'Wi-Fi', 'Bluetooth', 'IR', 'RF', 'OK', 'Hub', 'Port',
  'Name', 'Status', 'Token', 'Tokens', 'Slot', 'Backup', 'Host', 'Hosts', 'Info', 'Menu', 'Guide', 'Exit',
  'Off', 'DVR', 'Online', 'Offline', 'Server', 'Broker', 'Pronto', 'Logs', 'Code', 'Topic', 'Admin', 'User',
  'Username', 'Model', 'Format', 'Timeout', 'Play', 'Pause', 'Stop', 'Mute', 'Bluetooth HID',
  'Vol +', 'Vol −', 'Ch +', 'Ch −', 'ACTIVITIES', 'DEVICES', 'Home', 'Interface']);

// A text that is only names, codes and numbers may read the same in both languages
function sameOk(text) {
  if (SAME_OK.has(text)) return true;
  let rest = text;
  for (const w of [...SAME_OK].sort((a, b) => b.length - a.length)) rest = rest.split(w).join(' ');
  return !/[A-Za-z]{2,}/.test(rest);
}

function visibleTexts(root) {
  const out = [];
  for (const n of root.walk()) {
    const cls = n._classes();
    if (['h2', 'h3', 'h4', 'label', 'button'].includes(n.tagName) || cls.has('card-title') || cls.has('form-label')) {
      const text = n.textContent.replace(/[\p{Extended_Pictographic}️]/gu, '').trim();
      if (/[A-Za-z]{2,}/.test(text)) out.push(text);
    }
  }
  return out;
}

const realError = console.error, realWarn = console.warn;

async function render(name, lang) {
  i18n.setLanguage(lang);
  // the views log their failed Hub calls; that is expected here
  console.error = console.warn = () => {};
  const mod = await import(`../frontend/src/views/setup/${name}.js`);
  const made = (mod.createView || mod.createButtonEditor || mod.default)();
  const view = made.element || made;
  await tick(5);
  console.error = realError; console.warn = realWarn;
  return view;
}

for (const name of VIEWS) {
  await t.check(`${name}: headings, labels and buttons are German in German`, async () => {
    const en = visibleTexts(await render(name, 'en'));
    const deView = await render(name, 'de');
    const de = visibleTexts(deView);
    const all = deView.textContent;
    assert.ok(!/\bundefined\b/.test(all), 'a text shows "undefined"');
    assert.ok(!/\{[a-z_]+\}/.test(all), 'a text shows an unfilled {placeholder}');
    const stayed = de.filter(x => en.includes(x) && !sameOk(x));
    assert.deepEqual(stayed, [], `still English: ${stayed.slice(0, 8).join(' | ')}`);
  });
}

i18n.setLanguage('en');
t.finish();
