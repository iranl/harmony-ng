// Checks a web UI's translations: every language file has the English keys, and every key the
// code asks for (t('key'), data-i18n*="key") exists in every language. A key missing from the
// files makes t() fall back to the English text at the call, in every language.
// It also checks that every translation has the same {placeholders} as the English text, and reads
// src/i18n/machine-translated.json: the keys whose texts were translated by machine and not yet
// checked by a person, per language (each listed key must exist; the count is printed).
// Usage: node scripts/i18n-check.js [app dir]   (default: this frontend; ../android-app works too)
// Exit code 1 when a used key is missing, a placeholder differs, or the list names an unknown key.
import { readFileSync, readdirSync, statSync, existsSync } from 'node:fs';
import { dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const appDir = resolve(process.argv[2] || join(dirname(fileURLToPath(import.meta.url)), '..'));
const langDir = join(appDir, 'src', 'i18n');
const LANGS = ['en', 'de', 'fr', 'nl', 'es', 'it'];

const dicts = {};
for (const lang of LANGS) {
  dicts[lang] = (await import(pathToFileURL(join(langDir, `${lang}.js`)).href)).default;
}
const en = dicts.en;
let missingInFiles = false;
const enKeys = Object.keys(en);
console.log(`Base English keys: ${enKeys.length}`);

for (const lang of LANGS.slice(1)) {
  const dict = dicts[lang];
  const missing = enKeys.filter(k => !(k in dict));
  const extra = Object.keys(dict).filter(k => !(k in en));
  console.log(`[${lang.toUpperCase()}] Total: ${Object.keys(dict).length}, Missing: ${missing.length}, Extra: ${extra.length}`);
  if (missing.length > 0) {
    console.error(`  Missing in ${lang}: ${missing.slice(0, 10).join(', ')}${missing.length > 10 ? '...' : ''}`);
    missingInFiles = true;
  }
}

// Keys the code asks for: t('key') (also ui.t('key')), tf('key'), tfHtml('key'), both keys of t(cond ? 'a' : 'b'),
// table fields named ...Key: 'key', and data-i18n*="key". A call whose key is computed in
// another way cannot be checked: those are listed.
const used = new Map();
const dynamic = [];
const CALL = /\b(?:t|tf|tfHtml)\(\s*/g;
const KEY_RES = [
  /data-i18n(?:-[a-z-]+)?="([A-Za-z0-9_.]+)"/g,
  /\b[a-z]+Key\s*:\s*['"]([a-z0-9_]+)['"]/g,
];
function scan(file) {
  const text = readFileSync(file, 'utf8');
  const where = relative(appDir, file);
  const add = (key) => { if (!used.has(key)) used.set(key, where); };
  for (const re of KEY_RES) {
    for (const m of text.matchAll(re)) add(m[1]);
  }
  if (!file.endsWith('.js')) return;
  for (const m of text.matchAll(CALL)) {
    const prev = text[m.index - 1] || '';
    // obj.t(...), sort(...), ... are not translations; ui.t(...) is: a helper passed in an object
    const viaUi = prev === '.' && /(^|[^\w.$])ui\.$/.test(text.slice(Math.max(0, m.index - 4), m.index));
    if (/[\w.$]/.test(prev) && !viaUi) continue;
    const rest = text.slice(m.index + m[0].length);
    const lit = rest.match(/^(['"`])([A-Za-z0-9_.]+)\1/);
    if (lit) { add(lit[2]); continue; }
    const tern = rest.match(/^[^,()`]*?\?\s*(['"])([a-z0-9_]+)\1\s*:\s*(['"])([a-z0-9_]+)\3/);
    if (tern) { add(tern[2]); add(tern[4]); continue; }
    if (/^\)/.test(rest) || /function\s+$/.test(text.slice(Math.max(0, m.index - 12), m.index))) continue;
    const line = text.slice(0, m.index).split('\n').length;
    dynamic.push(`${where}:${line}: ${rest.split('\n')[0].slice(0, 60)}`);
  }
}
function walk(dir) {
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) {
      if (p !== langDir) walk(p);
    } else if (/\.(js|html)$/.test(name) && name !== 'i18n.js') { // i18n.js: the legacy t() itself
      scan(p);
    }
  }
}
walk(join(appDir, 'src'));
if (existsSync(join(appDir, 'index.html'))) scan(join(appDir, 'index.html'));

let failed = false;

// Placeholders: a translation must keep every {name} of the English text as many times, and add none.
const placeholders = (text) => (String(text).match(/\{[A-Za-z0-9_]+\}/g) || []).sort().join(' ');
for (const lang of LANGS.slice(1)) {
  const bad = enKeys.filter(k => k in dicts[lang] && placeholders(dicts[lang][k]) !== placeholders(en[k]));
  if (bad.length) {
    failed = true;
    console.error(`[${lang.toUpperCase()}] ${bad.length} text(s) with other placeholders than English: ${bad.slice(0, 10).join(', ')}`);
  }
}

// Machine translations still to be checked by a person
const mtPath = join(langDir, 'machine-translated.json');
if (existsSync(mtPath)) {
  const mt = JSON.parse(readFileSync(mtPath, 'utf8'));
  for (const lang of LANGS.slice(1)) {
    const keys = mt[lang] || [];
    const unknown = keys.filter(k => !(k in dicts[lang]) || !(k in en));
    if (unknown.length) {
      failed = true;
      console.error(`[${lang.toUpperCase()}] machine-translated.json lists unknown key(s): ${unknown.slice(0, 10).join(', ')}`);
    }
    console.log(`[${lang.toUpperCase()}] Machine translations to check: ${keys.length}`);
  }
}

for (const lang of LANGS) {
  const missing = [...used].filter(([k]) => !(k in dicts[lang]));
  if (missing.length) {
    failed = true;
    console.error(`[${lang.toUpperCase()}] ${missing.length} key(s) used in the code but missing:`);
    for (const [k, file] of missing.slice(0, 20)) console.error(`  ${k} (${file})`);
  }
}
console.log(`Keys used in the code: ${used.size}`);
if (dynamic.length) {
  console.log(`Calls with a computed key (not checked here): ${dynamic.length}`);
  for (const d of dynamic) console.log(`  ${d}`);
}
if (missingInFiles) failed = true;
if (failed) {
  process.exit(1);
}
console.log('Every key used in the code exists in all languages.');
