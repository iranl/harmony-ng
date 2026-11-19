// frontend/scripts/i18n-check.js fails on: a translation that loses, adds or repeats a
// {placeholder}; a key missing in one language file; a key asked for through tf(), tfHtml(), ui.t() or
// t(cond ? 'a' : 'b') that does not exist; a call whose values do not match the {placeholders} of the
// English text; a machine-translated.json entry naming an unknown key.
// Works on a copy of the new UI's language files in a temporary folder: no browser, no Hub.
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { cpSync, mkdtempSync, readFileSync, rmSync, writeFileSync, mkdirSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { makeChecker } from './webui_fake_dom.mjs';

const root = fileURLToPath(new URL('..', import.meta.url));
const checker = join(root, 'frontend/scripts/i18n-check.js');
const t = makeChecker('i18n checker');

function makeApp() {
  const dir = mkdtempSync(join(tmpdir(), 'hng-i18n-'));
  mkdirSync(join(dir, 'src'));
  cpSync(join(root, 'frontend/src/i18n'), join(dir, 'src/i18n'), { recursive: true });
  return dir;
}
function run(dir) {
  try {
    return { code: 0, out: execFileSync(process.execPath, [checker, dir], { encoding: 'utf8', stdio: 'pipe' }) };
  } catch (e) {
    return { code: e.status, out: `${e.stdout}${e.stderr}` };
  }
}

await t.check('the copied language files pass', async () => {
  const dir = makeApp();
  try {
    const r = run(dir);
    assert.equal(r.code, 0, r.out);
    assert.match(r.out, /\[DE\] Machine translations to check: \d+/);
  } finally { rmSync(dir, { recursive: true, force: true }); }
});

await t.check('a German text without the English {placeholder} fails', async () => {
  const dir = makeApp();
  try {
    const en = (await import(join(dir, 'src/i18n/en.js'))).default;
    const key = Object.keys(en).find(k => /\{[a-z_]+\}/.test(en[k]));
    const de = join(dir, 'src/i18n/de.js');
    const lines = readFileSync(de, 'utf8').split('\n').map(l => (l.trimStart().startsWith(`"${key}":`) ? `    "${key}": "kaputt",` : l));
    writeFileSync(de, lines.join('\n'));
    const r = run(dir);
    assert.equal(r.code, 1, r.out);
    assert.match(r.out, new RegExp(`\\[DE\\] 1 text\\(s\\) with other placeholders than English: ${key}`));
  } finally { rmSync(dir, { recursive: true, force: true }); }
});

await t.check('a German text with a {placeholder} twice fails (counts are compared)', async () => {
  const dir = makeApp();
  try {
    const en = (await import(join(dir, 'src/i18n/en.js'))).default;
    const key = Object.keys(en).find(k => /^[^{]*\{([a-z_]+)\}[^{]*$/.test(en[k]));
    const ph = en[key].match(/\{[a-z_]+\}/)[0];
    const de = join(dir, 'src/i18n/de.js');
    const lines = readFileSync(de, 'utf8').split('\n').map(l => (l.trimStart().startsWith(`"${key}":`) ? `    "${key}": "${ph} (${ph})",` : l));
    writeFileSync(de, lines.join('\n'));
    const r = run(dir);
    assert.equal(r.code, 1, r.out);
    assert.match(r.out, new RegExp(`other placeholders than English: ${key}`));
  } finally { rmSync(dir, { recursive: true, force: true }); }
});

await t.check('a key missing in one language file fails, even if the code does not use it', async () => {
  const dir = makeApp();
  try {
    const nl = join(dir, 'src/i18n/nl.js');
    const lines = readFileSync(nl, 'utf8').split('\n');
    const i = lines.findIndex(l => /^\s*"[a-z0-9_]+"\s*:/.test(l));
    const key = lines[i].match(/"([a-z0-9_]+)"/)[1];
    lines.splice(i, 1);
    writeFileSync(nl, lines.join('\n'));
    const r = run(dir);
    assert.equal(r.code, 1, r.out);
    assert.match(r.out, new RegExp(`Missing in nl: ${key}`));
  } finally { rmSync(dir, { recursive: true, force: true }); }
});

await t.check('tf(), tfHtml(), t(cond ? a : b) and ui.t() keys are checked', async () => {
  const dir = makeApp();
  try {
    writeFileSync(join(dir, 'src/probe.js'), "tf('no_such_key_a', {}); tfHtml('no_such_key_b'); t(on ? 'no_such_key_c' : 'no_such_key_d'); ui.t('no_such_key_e', 'E'); x.t('not_a_key_f');\n");
    const r = run(dir);
    assert.equal(r.code, 1, r.out);
    for (const k of ['a', 'b', 'c', 'd', 'e']) assert.match(r.out, new RegExp(`no_such_key_${k}`));
    assert.doesNotMatch(r.out, /not_a_key_f/, 'obj.t() other than ui.t() counted as a translation');
  } finally { rmSync(dir, { recursive: true, force: true }); }
});

await t.check('a call whose values do not match the {placeholders} of the English text fails', async () => {
  const dir = makeApp();
  try {
    const en = (await import(join(dir, 'src/i18n/en.js'))).default;
    const plain = Object.keys(en).find(k => !/\{/.test(en[k]));
    const one = Object.keys(en).find(k => /^[^{]*\{([a-z_]+)\}[^{]*$/.test(en[k]));
    const ph = en[one].match(/\{([a-z_]+)\}/)[1];
    // a value the text has no {name} for (never shown), a {name} without a value (shown as is),
    // and a call that fills the text right (not reported)
    writeFileSync(join(dir, 'src/probe.js'), [
      `tf('${plain}', { name: x });`,
      `t('${one}', 'fallback', {\n  other: y,\n});`,
      `t('${one}', { ${ph}: z(a, b) });`,
      `tfHtml('${one}', { ...vars });`,
      '',
    ].join('\n'));
    const r = run(dir);
    assert.equal(r.code, 1, r.out);
    assert.match(r.out, new RegExp(`probe.js:1: ${plain} gets \\{name\\}, the English text has no such placeholder`));
    assert.match(r.out, new RegExp(`probe.js:2: ${one} gets \\{other\\}`));
    assert.match(r.out, new RegExp(`probe.js:2: ${one} gives no value for \\{${ph}\\}`));
    assert.doesNotMatch(r.out, /probe\.js:[34]:/, 'a right call or a spread was reported');
  } finally { rmSync(dir, { recursive: true, force: true }); }
});

await t.check('machine-translated.json naming an unknown key fails', async () => {
  const dir = makeApp();
  try {
    const p = join(dir, 'src/i18n/machine-translated.json');
    const mt = JSON.parse(readFileSync(p, 'utf8'));
    mt.fr = [...(mt.fr || []), 'no_such_key_here'];
    writeFileSync(p, JSON.stringify(mt));
    const r = run(dir);
    assert.equal(r.code, 1, r.out);
    assert.match(r.out, /\[FR\] machine-translated.json lists unknown key\(s\): no_such_key_here/);
  } finally { rmSync(dir, { recursive: true, force: true }); }
});

t.finish();
