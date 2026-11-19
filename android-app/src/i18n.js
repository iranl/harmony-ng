// Multi-language (i18n) support for Harmony Hub WebUI & Android App
// Languages: English (en), German (de), French (fr), Dutch (nl), Spanish (es), Italian (it)

import en from './i18n/en.js';
import de from './i18n/de.js';
import fr from './i18n/fr.js';
import nl from './i18n/nl.js';
import es from './i18n/es.js';
import it from './i18n/it.js';
import { escapeHtml } from './html_escape.js';

export const SUPPORTED_LANGUAGES = [
  { code: 'en', name: 'English', label: 'EN' },
  { code: 'de', name: 'Deutsch', label: 'DE' },
  { code: 'fr', name: 'Français', label: 'FR' },
  { code: 'nl', name: 'Nederlands', label: 'NL' },
  { code: 'es', name: 'Español', label: 'ES' },
  { code: 'it', name: 'Italiano', label: 'IT' },
];

export const TRANSLATIONS = {
  en,
  de,
  fr,
  nl,
  es,
  it,
};

let currentLang = 'en';

export function getInitialLanguage() {
  try {
    const saved = localStorage.getItem('harmony_lang');
    if (saved && TRANSLATIONS[saved]) return saved;
  } catch (e) {
    // Ignore storage errors
  }

  // Detect browser/device language
  if (typeof navigator !== 'undefined' && navigator.language) {
    const code = navigator.language.substring(0, 2).toLowerCase();
    if (TRANSLATIONS[code]) return code;
  }

  return 'en';
}

export function getCurrentLanguage() {
  return currentLang;
}

export function t(key, fallback = '') {
  const dict = TRANSLATIONS[currentLang] || TRANSLATIONS['en'];
  if (dict && dict[key]) return dict[key];
  if (TRANSLATIONS['en'] && TRANSLATIONS['en'][key]) return TRANSLATIONS['en'][key];
  return fallback || key;
}

// t() with values: each {name} in the text gets vars.name, in one pass, so a value that itself
// holds "{id}" or "$&" stays as typed. A {name} without a value is left as it is.
export function tf(key, vars = {}, fallback = '') {
  return fillPlaceholders(t(key, fallback), vars);
}

export function fillPlaceholders(text, vars = {}) {
  return String(text).replace(/\{([A-Za-z0-9_]+)\}/g, (whole, name) => {
    if (!Object.prototype.hasOwnProperty.call(vars, name)) return whole;
    const value = vars[name];
    return value === null || value === undefined ? '' : String(value);
  });
}

// tf() for innerHTML: the values are escaped (names, Hub error texts), the translated text is not
// (it holds no markup).
export function tfHtml(key, vars = {}, fallback = '') {
  const safe = {};
  for (const [name, value] of Object.entries(vars)) safe[name] = escapeHtml(value);
  return tf(key, safe, fallback);
}

export function setLanguage(lang) {
  if (!TRANSLATIONS[lang]) lang = 'en';
  currentLang = lang;
  try {
    localStorage.setItem('harmony_lang', lang);
  } catch (e) {
    // Ignore storage errors
  }

  if (typeof document !== 'undefined') {
    document.documentElement.lang = lang;

    // Update text content for data-i18n elements
    document.querySelectorAll('[data-i18n]').forEach(el => {
      const key = el.getAttribute('data-i18n');
      if (key) {
        const val = t(key);
        if (val) el.textContent = val;
      }
    });

    // Update placeholders
    document.querySelectorAll('[data-i18n-placeholder]').forEach(el => {
      const key = el.getAttribute('data-i18n-placeholder');
      if (key) {
        const val = t(key);
        if (val) el.setAttribute('placeholder', val);
      }
    });

    // Update titles / tooltips
    document.querySelectorAll('[data-i18n-title]').forEach(el => {
      const key = el.getAttribute('data-i18n-title');
      if (key) {
        const val = t(key);
        if (val) el.setAttribute('title', val);
      }
    });

    // Update aria-labels (screen readers)
    document.querySelectorAll('[data-i18n-aria-label]').forEach(el => {
      const key = el.getAttribute('data-i18n-aria-label');
      if (key) {
        const val = t(key);
        if (val) el.setAttribute('aria-label', val);
      }
    });

    // Sync dropdowns
    document.querySelectorAll('#langSelect, .lang-select').forEach(sel => {
      if (sel && sel.value !== lang) {
        sel.value = lang;
      }
    });

    // Dispatch event for components that need dynamic rerender
    window.dispatchEvent(new CustomEvent('harmony-lang-changed', { detail: { lang } }));
  }
}
