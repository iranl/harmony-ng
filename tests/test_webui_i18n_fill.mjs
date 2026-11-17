// Both web UIs fill {placeholders} in one pass: a value is put in as typed ("$&", "$1", a name
// that holds "{id}"), every occurrence of a placeholder is filled, and one without a value stays.
// Runs in Node: no browser, no Hub.
import assert from 'node:assert/strict';
import { installFakeDom, makeChecker } from './webui_fake_dom.mjs';

installFakeDom();
const legacy = await import('../android-app/src/i18n.js');
const fresh = await import('../frontend/src/i18n/index.js');
const t = makeChecker('Web UI placeholder filling');

await t.check('legacy fillPlaceholders()/tf()/tfHtml()', async () => {
  assert.equal(legacy.fillPlaceholders('{name} (ID: {id})', { name: '{id}', id: 5 }), '{id} (ID: 5)');
  assert.equal(legacy.fillPlaceholders('A {x} B {x}', { x: '$& $1 $$' }), 'A $& $1 $$ B $& $1 $$');
  assert.equal(legacy.fillPlaceholders('{a} {b}', { a: null }), ' {b}');
  legacy.setLanguage('en');
  assert.equal(legacy.tf('edit_activity_name', { name: 'Kino $&' }), 'Edit Activity: Kino $&');
  assert.equal(legacy.tfHtml('edit_activity_name', { name: '<b>x</b>' }), 'Edit Activity: &lt;b&gt;x&lt;/b&gt;');
});

await t.check('new UI t() with values', async () => {
  fresh.setLanguage('en');
  assert.equal(fresh.t('no_such_key', '{name} (ID: {id})', { name: '{id}', id: 5 }), '{id} (ID: 5)');
  assert.equal(fresh.t('no_such_key', 'A {x} B {x}', { x: '$& $1' }), 'A $& $1 B $& $1');
  assert.equal(fresh.t('no_such_key', '{a} {b}', { a: 1 }), '1 {b}');
});

t.finish();
