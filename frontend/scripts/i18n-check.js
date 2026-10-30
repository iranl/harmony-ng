import en from '../src/i18n/en.js';
import de from '../src/i18n/de.js';
import fr from '../src/i18n/fr.js';
import nl from '../src/i18n/nl.js';
import es from '../src/i18n/es.js';
import it from '../src/i18n/it.js';

const languages = { de, fr, nl, es, it };
const enKeys = Object.keys(en);
console.log(`Base English keys: ${enKeys.length}`);

let hasMissing = false;
for (const [lang, dict] of Object.entries(languages)) {
  const missing = enKeys.filter(k => !(k in dict));
  const extra = Object.keys(dict).filter(k => !(k in en));
  console.log(`[${lang.toUpperCase()}] Total: ${Object.keys(dict).length}, Missing: ${missing.length}, Extra: ${extra.length}`);
  if (missing.length > 0) {
    console.warn(`  Missing in ${lang}: ${missing.slice(0, 10).join(', ')}${missing.length > 10 ? '...' : ''}`);
    hasMissing = true;
  }
}

if (hasMissing) {
  console.warn('Note: Gaps detected in secondary languages; fallback to English active.');
} else {
  console.log('All languages 100% complete.');
}
