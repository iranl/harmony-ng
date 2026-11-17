# Translating the web UIs

Both web UIs show their texts in English, German, French, Dutch, Spanish and Italian. This page is for
translators and for contributors who add or change a text.

| UI | Language files | Look-up functions |
|---|---|---|
| Legacy UI (the one the Hub serves, also the Android app) | `android-app/src/i18n/<lang>.js` | `t()`, `tf()`, `tfHtml()` in `android-app/src/i18n.js` |
| New UI | `frontend/src/i18n/<lang>.js` | `t()` in `frontend/src/i18n/index.js` |

`<lang>` is `en`, `de`, `fr`, `nl`, `es` or `it`. Each file is a flat object of `"key": "text"`.
English (`en.js`) is the reference: every key in it must exist in the five other files.

## How keys work

Texts the UI shows go through a key instead of being written into the code. A few names, codes
and samples stay as they are on purpose (see "Texts that stay as they are").

- New UI: `t('scan_failed_error', 'Scan failed: {error}', { error: e.message })`. The second
  argument is the English text used only when the key is missing; the third fills the
  `{placeholders}`.
- Legacy UI: `t('error_msg')` for a plain text, `tf('error_msg', { error: err.message })` with
  values, and `tfHtml(...)` when the result goes
  into `innerHTML`: it escapes the values, so a device name or a Hub error text cannot become
  markup. `tests/test_ui_escape_scan.py` fails on a name or error text put into HTML through
  `tf()` or `t().replace()` without escaping.
- Both UIs fill all placeholders in one pass: a value goes in as typed, also when it holds
  `$&` or another `{placeholder}`.
- Static HTML (legacy `index.html`): `data-i18n="key"` sets the element's text,
  `data-i18n-placeholder`, `data-i18n-title` and `data-i18n-aria-label` set those attributes.
  They are applied again when the language changes. Never put `data-i18n` on an element that the
  code fills later (it would put the static text back) or on an element with child elements (it
  would remove them): wrap the text in a `<span data-i18n="...">`.
- A language change redraws the open page of both UIs, so the texts the code builds follow too.
  In the legacy UI a page with a settings form (MQTT, network, backup, system, hubs) or an
  unsaved key map (the two remote editors) is not reloaded, so nothing typed or changed is lost:
  only its status parts follow at once, the rest when the page is opened again.

Rules for texts:

- Keys are lower case with underscores, named after what the text is (`confirm_reboot`,
  `wifi_password`). Reuse an existing key when the English text is the same and means the same.
- One key per sentence. Values go in as `{name}` placeholders, never by gluing translated pieces
  together, so each language can put them where its grammar wants them. A translation must keep
  every placeholder of the English text, as many times, and add none.
- No HTML in texts; markup and emoji stay in the code around the call.
- Not translated: brand and product names (Harmony, Logitech, Home Assistant), protocol and
  technical names (MQTT, IR, RF, Bluetooth, Wi-Fi, Pronto), button and command codes (`PowerOff`,
  `VolumeUp`), key legends printed on a remote ("DEVICES", "Menu" on the Harmony remote), units
  (MB, KB, ms), code samples, and names that are stored on the Hub (a default token or activity
  name).
- German uses "Sie", French "vous", Dutch "u", Spanish "usted", Italian the informal imperative
  ("Seleziona"). Use the terms the file already uses for the same thing (device, activity,
  remote, slot).

## Texts that stay as they are

The test `tests/test_webui_hardcoded_text.py` fails when English text is written into the UI code
where it reaches the screen. When a text must stay as it is:

- JS: end the line with `// i18n-ok: <why>` (for example a key legend printed on the remote). The
  marker covers that one line only, so a code sample goes into a named constant instead of a
  `text:` value.
- HTML (`index.html` or a template in the code): put `translate="no"` on the element, the standard
  HTML attribute for content that is not translated: names in a list (brands, languages, time
  zones, file names), key caps, sample values.
- Names, protocols, codes and units that may appear in any text are listed in `ALLOWED` in that
  test; key caps and badges of a drawn remote or keyboard (allowed only as the whole text) in
  `KEY_CAPS`. Units (kB, GHz), two- or three-letter abbreviations (AP, UTC), codes with an
  underscore or a digit (KEY_ENTER), MAC addresses, host names and texts in lower case only
  (CSS values, class names) need no marker.
- `tests/test_frontend_views_translated.mjs` builds the new UI's setup pages in English and in
  German and fails on a heading, label or button that reads the same in both; a word that is the
  same in German ("Name", "Status", "Port") goes into its `SAME_OK` list.
- `tests/test_webui_rows_translated.mjs` draws pages of both UIs from sample Hub data (users,
  tokens, Bluetooth hosts, the lists of a refused device delete) in a second language and fails
  when an English text of a translated key is left on the page, or when a name holding `$&` or
  `{name}` is not shown as typed.

## Machine translations still to check

Many texts were first translated by machine. Each UI keeps the list of these keys in
`src/i18n/machine-translated.json`, one list per language:

```json
{
  "about": "...",
  "de": ["scan_failed_error", "..."],
  "fr": ["..."]
}
```

To check a language: open the list for it, check (and correct) each text in `<lang>.js`, then
remove the checked keys from that language's list. A key still in the list means "machine
translated, not yet checked by a person". When you add a new text and translate it yourself
without being a fluent speaker, add its key to the lists of the languages you did not check.

## Checking

```bash
node frontend/scripts/i18n-check.js frontend      # new UI
node frontend/scripts/i18n-check.js android-app   # legacy UI
```

The checker fails (exit code 1) when a key is missing in one of the language files, when a key
the code uses (`t()`, `tf()`, `tfHtml()`, `ui.t()` for a translate function passed in an options
object, both keys of `t(cond ? 'a' : 'b')`, table fields named `...Key`, `data-i18n*`) does not exist, when a translation has other placeholders than the English
text, when a call's values do not match the placeholders of the English text (a value the text has
no `{name}` for, or a `{name}` the call gives no value; checked for calls that pass an object
literal, e.g. `tf('key', { name })`), or when `machine-translated.json` names a key that does not
exist. It prints how many
machine translations each language still has, and lists the calls whose key is computed in some
other way (it cannot check those). The test suite runs it for both UIs (`tests/suite.d/86-i18n.sh`)
together with the tests named above.

## Adding a language

1. Copy `en.js` to `<code>.js` in both `i18n` folders and translate the texts.
2. Import it and add it to `SUPPORTED_LANGUAGES` and `TRANSLATIONS` in
   `android-app/src/i18n.js` and `frontend/src/i18n/index.js`.
3. Add an `<option>` for it to every language select (`class="lang-select"`): two in
   `android-app/index.html`, one in `frontend/index.html`.
4. Add the language code to `LANGS` in `frontend/scripts/i18n-check.js`.
5. If the texts were translated by machine, add a list for the language to both
   `machine-translated.json` files.
