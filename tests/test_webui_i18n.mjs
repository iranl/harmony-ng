// Both web UIs: every translation key the code asks for exists in all six language files (else
// t() shows the English fallback in every language). Runs frontend/scripts/i18n-check.js for each
// UI, then spot-checks a few new UI texts that used to stay English.
// Runs in Node: no browser, no Hub.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { installFakeDom, makeChecker } from './webui_fake_dom.mjs';

installFakeDom();
const root = fileURLToPath(new URL('..', import.meta.url));
const t = makeChecker('Web UI translation keys');

for (const app of ['frontend', 'android-app']) {
  await t.check(`${app}: every key used in the code is translated`, async () => {
    try {
      execFileSync(process.execPath, [`${root}frontend/scripts/i18n-check.js`, `${root}${app}`], { encoding: 'utf8', stdio: 'pipe' });
    } catch (e) {
      throw new Error((e.stderr || e.stdout || e.message).trim().split('\n').slice(0, 6).join(' | '));
    }
  });
}

await t.check('new UI: confirm dialog, tabs and status read in the chosen language', async () => {
  const i18n = await import('../frontend/src/i18n/index.js');
  i18n.setLanguage('de');
  assert.equal(i18n.t('confirm_btn', 'Confirm'), 'Bestätigen');
  assert.equal(i18n.t('tab_devices', 'Devices'), 'Geräte');
  assert.equal(i18n.t('hub_status_online', 'Online'), 'Online');
  i18n.setLanguage('it');
  assert.equal(i18n.t('cancel_btn', 'Cancel'), 'Annulla');
  i18n.setLanguage('en');
});

t.finish();
