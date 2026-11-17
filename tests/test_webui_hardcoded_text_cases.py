#!/usr/bin/env python3
"""The hard-coded text check (test_webui_hardcoded_text.py) and the escape scan
(test_ui_escape_scan.py) find each form of the problem they are meant to find, and leave the
correct forms alone. Each case is a small piece of UI code, checked on its own.
Runs on the host: no browser, no Hub.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_webui_hardcoded_text as hc  # noqa: E402
import test_ui_escape_scan as esc  # noqa: E402


def new_ui(code):
    return hc.js_text_hits(code, hc.NEW_UI, True, 'case.js')


def legacy(code):
    return hc.js_text_hits(code, hc.LEGACY, False, 'case.js')


class HardCodedText(unittest.TestCase):
    def found(self, hits, text):
        self.assertTrue(any(text in h for h in hits), f'{text!r} not found in {hits}')

    def test_new_ui_forms(self):
        self.found(new_ui("modal({ label: 'Throw it all away' });"), 'Throw it all away')
        self.found(new_ui("h('input', { placeholder: 'Type the network name here' });"), 'Type the network name')
        self.found(new_ui("h('button', { 'aria-label': 'Close the dialog now' });"), 'Close the dialog')
        self.found(new_ui("h('div', { html: 'Some words here' });"), 'Some words here')
        self.found(new_ui("h('div', {}, ['Plain child text']);"), 'Plain child text')
        hits = new_ui("h('b', { text: cond ? 'Secret pass phrase' : 'Other words' });")
        self.found(hits, 'Secret pass phrase')
        self.found(hits, 'Other words')

    def test_legacy_forms(self):
        hits = legacy("el.textContent = online ? 'Hub is online now' : 'Hub is offline now';")
        self.found(hits, 'Hub is online now')
        self.found(hits, 'Hub is offline now')
        self.found(legacy("el.title = 'Some title words';"), 'Some title words')
        self.found(legacy("el.setAttribute('title', 'Words in a tooltip');"), 'Words in a tooltip')
        self.found(legacy("el.textContent = name || 'No name given';"), 'No name given')
        self.found(legacy("el.textContent = 'Saved ' + count + ' items';"), 'Saved')
        self.found(legacy("el.innerHTML = `<b>${on ? 'Enabled and saved' : 'Disabled'}</b>`;"), 'Enabled and saved')
        self.found(legacy("showToast(`Imported ${n} devices`);"), 'Imported')
        self.found(legacy("el.innerHTML = `\n  <div>\n    Plain words in a template\n  </div>`;"), 'Plain words in a template')

    def test_row_templates_and_other_sinks(self):
        self.found(legacy("list.innerHTML = items.map(a => `<option value=\"${a.id}\">Activity called ${a.name}</option>`).join('');"), 'Activity called')
        self.found(legacy("function row(n) {\n  return `<div>Step number here ${n}</div>`;\n}"), 'Step number here')
        self.found(legacy("let html = '';\nhtml += `<div class=\"x\">Token details here</div>`;"), 'Token details here')
        self.found(legacy("html += '<b>Plain row words</b>';"), 'Plain row words')
        self.found(legacy("el.insertAdjacentHTML('beforeend', 'Inserted words here');"), 'Inserted words here')
        self.found(legacy("el.innerText = 'Inner text words';"), 'Inner text words')
        self.found(legacy("sel.add(new Option('Option words here', 'v'));"), 'Option words here')
        self.found(legacy('el.setAttribute("placeholder", "Double quoted words");'), 'Double quoted words')

    def test_codes_units_and_data_fields_pass(self):
        ok = [
            "el.textContent = `${n} kB, 2.4 GHz, AP`;",
            "el.placeholder = 'AA:BB:CC:DD:EE:FF';",
            "el.placeholder = 'pool.ntp.org';",
            "el.textContent = 'KEY_ENTER';",
            "el.textContent = `HDMI ${n} (UTC)`;",
            "el.innerHTML = `<div class=\"${on ? 'btn btn-primary' : 'btn'}\" style=\"display:none;\">${t('k')}</div>`;",
            "return { type: 'Command', className: 'queue-row act-dev-row' };",
        ]
        for code in ok:
            self.assertEqual(legacy(code), [], code)
        self.found(legacy("el.textContent = 'Hub is OFF now';"), 'Hub is OFF now')

    def test_marker_covers_one_line(self):
        code = "// i18n-ok: sample\nel.innerHTML = `\n  <span>Allowed here?</span>`;"
        self.found(legacy(code), 'Allowed here?')
        self.assertEqual(legacy("el.textContent = 'Kept as it is';  // i18n-ok: sample"), [])
        code = "el.innerHTML = `\n  <span translate=\"no\">Home</span>\n  <span>Home page words</span>`;"
        hits = legacy(code)
        self.assertEqual(len(hits), 1, hits)
        self.found(hits, 'Home page words')

    def test_correct_forms_pass(self):
        ok = [
            "el.textContent = t('saved');",
            "el.textContent = tf('saved_n', { count: n });",
            "el.className = 'big words here';",
            "el.innerHTML = `<div class=\"a b\" title=\"${t('x')}\">${t('y')}</div>`;",
            "el.textContent = log.textContent === 'Ready now.' ? t('a') : t('b');",
            "el.textContent = t(cond ? 'key_a' : 'key_b');",
            "el.innerHTML = '<span class=\"' + (on ? 'active' : 'idle') + '\">' + t('x') + '</span>';",
            "el.textContent = `${n} MB`;",
        ]
        for code in ok:
            self.assertEqual(legacy(code), [], code)
        self.assertEqual(new_ui("h('span', { text: t('k', 'Text') }, [h('b')]);"), [])

    def test_html_forms(self):
        self.found(hc.html_text_hits('<label><input type="checkbox"> Enable the new thing</label>'), 'Enable the new thing')
        self.found(hc.html_text_hits('<select><option value="a">Some option</option></select>'), 'Some option')
        self.found(hc.html_text_hits('<input placeholder="Type here please">'), 'Type here please')
        self.found(hc.html_text_hits('<button aria-label="Close this">x</button>'), 'Close this')
        self.found(hc.html_text_hits('<span title="Tooltip words">-</span>'), 'Tooltip words')
        ok = [
            '<span data-i18n="k">Text in English</span>',
            '<select translate="no"><option>Sonos</option><option>Europe/Rome</option></select>',
            '<div>Format: <code>KEY name</code></div>'.replace('Format:', '<span data-i18n="f">Format:</span>'),
            '<input placeholder="Type here" data-i18n-placeholder="k">',
            '<button aria-label="Close" data-i18n-aria-label="close">x</button>',
            '<button>Esc</button>',
        ]
        for page in ok:
            self.assertEqual(hc.html_text_hits(page), [], page)


class EscapeScan(unittest.TestCase):
    def test_values_in_translated_text_are_checked(self):
        self.assertTrue(esc.unsafe("t('edit_activity_name').replace('{name}', act.name)"))
        self.assertTrue(esc.unsafe("tf('edit_activity_name', { name: act.name })"))
        self.assertTrue(esc.unsafe("t('k', 'Edit {name}', { name: act.name })"))
        self.assertTrue(esc.unsafe("tf('error_msg', { error: err.message })"))
        self.assertTrue(esc.unsafe("err.message"))

    def test_safe_forms_numbers_and_quotes(self):
        self.assertFalse(esc.unsafe("tf('k', { count: names.length })"))
        self.assertFalse(esc.unsafe("t('key', \"Couldn't send command\")"))

    def test_safe_forms(self):
        self.assertFalse(esc.unsafe("t('edit_activity')"))
        self.assertFalse(esc.unsafe("tfHtml('edit_activity_name', { name: act.name })"))
        self.assertFalse(esc.unsafe("tf('edit_activity_name', { name: escapeHtml(act.name) })"))
        self.assertFalse(esc.unsafe("escapeHtml(tf('edit_activity_name', { name: act.name }))"))
        self.assertFalse(esc.unsafe("tf('count_n', { count: list.length })"))

    def test_nested_interpolations_are_seen(self):
        exprs = esc.interpolations("x.innerHTML = `<b>${tf('k', { name: d.name })}</b>`;")
        self.assertTrue(any(esc.unsafe(e) for e in exprs), exprs)


if __name__ == '__main__':
    unittest.main(verbosity=1)
