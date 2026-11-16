// Checks a web UI's translations: every language file has the English keys, and every key the
// code asks for (t('key'), data-i18n*="key") exists in every language. A key missing from the
// files makes t() fall back to the English text at the call, in every language.
// Usage: node scripts/i18n-check.js [app dir]   (default: this frontend; ../android-app works too)
// Exit code 1 when a used key is missing.
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
const enKeys = Object.keys(en);
console.log(`Base English keys: ${enKeys.length}`);

for (const lang of LANGS.slice(1)) {
  const dict = dicts[lang];
  const missing = enKeys.filter(k => !(k in dict));
  const extra = Object.keys(dict).filter(k => !(k in en));
  console.log(`[${lang.toUpperCase()}] Total: ${Object.keys(dict).length}, Missing: ${missing.length}, Extra: ${extra.length}`);
  if (missing.length > 0) {
    console.warn(`  Missing in ${lang}: ${missing.slice(0, 10).join(', ')}${missing.length > 10 ? '...' : ''}`);
  }
}

// Keys the code asks for
const used = new Map();
const KEY_RES = [
  /\bt\(\s*['"`]([A-Za-z0-9_.]+)['"`]/g,
  /data-i18n(?:-[a-z]+)?="([A-Za-z0-9_.]+)"/g,
];
function scan(file) {
  const text = readFileSync(file, 'utf8');
  for (const re of KEY_RES) {
    for (const m of text.matchAll(re)) {
      if (!used.has(m[1])) used.set(m[1], relative(appDir, file));
    }
  }
}
function walk(dir) {
  for (const name of readdirSync(dir)) {
    const p = join(dir, name);
    if (statSync(p).isDirectory()) {
      if (p !== langDir) walk(p);
    } else if (/\.(js|html)$/.test(name)) {
      scan(p);
    }
  }
}
walk(join(appDir, 'src'));
if (existsSync(join(appDir, 'index.html'))) scan(join(appDir, 'index.html'));

let failed = false;
for (const lang of LANGS) {
  const missing = [...used].filter(([k]) => !(k in dicts[lang]));
  if (missing.length) {
    failed = true;
    console.error(`[${lang.toUpperCase()}] ${missing.length} key(s) used in the code but missing:`);
    for (const [k, file] of missing.slice(0, 20)) console.error(`  ${k} (${file})`);
  }
}
console.log(`Keys used in the code: ${used.size}`);
if (failed) {
  process.exit(1);
}
console.log('Every key used in the code exists in all languages.');
