// Multi-language (i18n) module for Harmony Remote v3
import en from './en.js';
import de from './de.js';
import fr from './fr.js';
import nl from './nl.js';
import es from './es.js';
import it from './it.js';

export const SUPPORTED_LANGUAGES = [
  { code: 'en', name: 'English', label: 'EN' },
  { code: 'de', name: 'Deutsch', label: 'DE' },
  { code: 'fr', name: 'Français', label: 'FR' },
  { code: 'nl', name: 'Nederlands', label: 'NL' },
  { code: 'es', name: 'Español', label: 'ES' },
  { code: 'it', name: 'Italiano', label: 'IT' },
];

export const TRANSLATIONS = { en, de, fr, nl, es, it };

let currentLang = 'en';

export function getInitialLanguage() {
  try {
    const saved = localStorage.getItem('harmony_lang');
    if (saved && TRANSLATIONS[saved]) return saved;
  } catch {
    // Ignore storage errors
  }

  if (typeof navigator !== 'undefined' && navigator.language) {
    const code = navigator.language.substring(0, 2).toLowerCase();
    if (TRANSLATIONS[code]) return code;
  }

  return 'en';
}

export function getCurrentLanguage() {
  return currentLang;
}

export function t(key, fallbackOrParams = '', params = null) {
  let fallback = '';
  let vars = null;

  if (typeof fallbackOrParams === 'object' && fallbackOrParams !== null) {
    vars = fallbackOrParams;
    fallback = key;
  } else {
    fallback = fallbackOrParams || key;
    if (typeof params === 'object' && params !== null) {
      vars = params;
    }
  }

  const dict = TRANSLATIONS[currentLang] || TRANSLATIONS.en;
  let text = dict[key] || TRANSLATIONS.en[key] || fallback;

  if (vars && typeof text === 'string') {
    for (const [k, v] of Object.entries(vars)) {
      text = text.replace(new RegExp(`\\{${k}\\}`, 'g'), String(v));
    }
  }

  return text;
}

export function setLanguage(lang) {
  if (!TRANSLATIONS[lang]) lang = 'en';
  currentLang = lang;
  try {
    localStorage.setItem('harmony_lang', lang);
  } catch {
    // Ignore storage errors
  }

  if (typeof document !== 'undefined') {
    document.documentElement.lang = lang;

    document.querySelectorAll('[data-i18n]').forEach((el) => {
      const key = el.getAttribute('data-i18n');
      if (key) {
        const val = t(key);
        if (val) el.textContent = val;
      }
    });

    document.querySelectorAll('[data-i18n-placeholder]').forEach((el) => {
      const key = el.getAttribute('data-i18n-placeholder');
      if (key) {
        const val = t(key);
        if (val) el.setAttribute('placeholder', val);
      }
    });

    document.querySelectorAll('[data-i18n-title]').forEach((el) => {
      const key = el.getAttribute('data-i18n-title');
      if (key) {
        const val = t(key);
        if (val) el.setAttribute('title', val);
      }
    });

    document.querySelectorAll('.lang-select').forEach((sel) => {
      if (sel && sel.value !== lang) {
        sel.value = lang;
      }
    });

    window.dispatchEvent(new CustomEvent('harmony-lang-changed', { detail: { lang } }));
  }
}
