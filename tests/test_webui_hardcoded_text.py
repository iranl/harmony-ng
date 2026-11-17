#!/usr/bin/env python3
"""Both web UIs: no English text hard-coded where the UI shows it.

Every text the UIs show goes through t('key') so it follows the chosen language. This test scans
the places where text reaches the screen and fails on a string that still holds words:

- JS, both UIs: the value given to `text:`, `title:`, `message:`, `label:`, `placeholder:`,
  `html:`, `'aria-label':`, `okText:`/`confirmText:`/`cancelText:`, `.textContent`, `.innerText`,
  `.innerHTML`, `.outerHTML`, `.title`, `.placeholder`, `.label`, setAttribute('title' |
  'placeholder' | 'aria-label'), insertAdjacentHTML(), createTextNode(), append(), new Option(),
  toast.*(), showToast(), alert(), confirm(), prompt(), showConfirm(), a variable named *html*
  (`html += ...`), strings in the child list of the new UI's h(), and every template literal that
  holds markup, wherever it goes (rows built in map(), returned from a function). The whole
  expression is read: both sides of `? :`, the right side of `||`, `+` pieces, template text and
  the strings inside its ${...}. Strings given to t()/tf()/tfHtml() (key and fallback), look-ups
  and selectors, values of data fields (className, style, type, id, ...) and comparison operands
  are not output.
- legacy index.html: every text node needs data-i18n on its element (an element with child
  elements and text needs the text wrapped in a span), and placeholder / title / aria-label
  need data-i18n-placeholder / data-i18n-title / data-i18n-aria-label. Code samples in
  <code>/<pre>/<kbd>, <script> and <style> are skipped, and so is everything under the standard
  translate="no" attribute (names: brands, languages, time zones, file names, key caps).

Not language, so allowed as they are: the names, protocols, codes and units in ALLOWED, the key
caps and badges of a drawn remote in KEY_CAPS (whole texts only), CODE_LIKE tokens (units, two-
or three-letter abbreviations, codes with an underscore or digit, MAC addresses, host names),
texts in lower case only (CSS values, class names, topic paths), and a string on a line that
carries an `i18n-ok` comment (`// i18n-ok: why` in JS, `<!-- i18n-ok: why -->` in HTML) or, in
an HTML template line, a translate="no" attribute. The marker covers that one line only.
Runs on the host: no browser, no Hub.
"""
import os
import re
import sys
from html.parser import HTMLParser

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))

# Names, protocols, codes and units: allowed alone and inside a text
ALLOWED = {
    'Harmony Hub', 'Harmony Remote', 'Harmony Keyboard', 'Harmony Elite', 'Harmony Ultimate',
    'Home Assistant', 'MQTT', 'Wi-Fi', 'Ethernet', 'Bluetooth', 'BTstack', 'IR', 'RF', 'BLE', 'HID',
    'IR Lab', 'Pronto', 'LIRC', 'B25', 'Homatics B25', 'OK', 'ID', 'IP', 'MAC', 'SSID', 'URL',
    'API', 'PIN', 'USB', 'OTA', 'JSON', 'CSV', 'ms', 'mV', 'dBm', 'admin', 'root', 'http', 'Eth',
    'Linux', 'dx', 'dy', 'hub_daemon', 'hub_btstack', 'hub_rf', 'script.harmony_send_command',
    'PowerOff', 'PowerOn', 'PowerToggle', 'VolumeUp', 'VolumeDown', 'ChannelUp', 'ChannelDown',
    'LightOn', 'LightOff', 'MB', 'KB', 'GB', 'SMART CONTROL', 'COMPANION', 'BT',
}

# Not language wherever they appear: units, short abbreviations (AP, STA, UTC, NEC), codes with
# an underscore or digit (KEY_ENTER, HDMI2), MAC addresses, host names (pool.ntp.org)
COMMON_CAPS = r'(?!(?:OFF|ON|NEW|ADD|ALL|YES|NO|OUT|AND|THE|FOR|NOT|SET|RUN|GET)\b)'
CODE_LIKE = re.compile(
    r'\b(?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}\b'
    r'|\b[\w-]+(?:\.[\w-]+)+\b'
    r'|\b\d*\s*(?:kB|KB|MB|GB|Hz|kHz|MHz|GHz|ms|dBm|mV|kbps|Mbps)\b'
    r'|\b' + COMMON_CAPS + r'[A-Z]{2,3}\b'
    r'|\b[A-Z][A-Z0-9]*(?:_[A-Z0-9]+)+\b|\b[A-Z]+[0-9]+\b'
    r'|\bHDMI\b')

# Key caps, badges and app tiles of a drawn remote or keyboard: allowed only as the whole text
KEY_CAPS = {
    'AR', 'BT', 'KB', 'RC', 'HB', 'Esc', 'Ins', 'Del', 'Tab', 'End', 'Caps', 'Ctrl', 'Win', 'Alt',
    'Space', 'PgUp', 'PgDn', 'Enter ↵', '⇧ Shift', '⌫ Back', '◀◀ Rew', 'Fwd ▶▶', 'IN', 'GUIDE',
    'INFO', 'TV', 'YouTube', 'NETFLIX', 'prime', 'Google Play', 'PWR', 'VOL +', 'VOL -', 'CH +',
    'CH -', 'Shift', 'Enter', 'Backspace', 'Fn', 'Vol -', 'Vol +', 'Vol-', 'Vol+',
}

SKIP_DIRS = {'i18n', 'node_modules'}


def words(text):
    """The visible words of a text, or None when it holds no language. A piece of HTML built
    from parts may start inside a tag or end in one: those tag parts are dropped too. A single
    lower-case word (a CSS value, class or attribute name) is not language."""
    head = text.split('>', 1)[0]
    if '>' in text and '<' not in head and re.search(r'["=]', head):
        text = text.split('>', 1)[1]  # starts inside a tag: `" class="x">Text`
    elif '<' not in text and '>' not in text and re.search(r'=\s*"|^\s*"', text):
        return None  # only attribute parts: `" data-key="`
    text = re.sub(r'<[^>]*$', ' ', text)
    text = re.sub(r'''<(?:[^>"']|"[^"]*"|'[^']*')*>''', ' ', text)
    text = re.sub(r'&[a-z]+;|&#\d+;', ' ', text)
    text = re.sub(r'\{[A-Za-z_]+\}', ' ', text)  # {tokens} shown as they are (topic templates)
    text = ' '.join(text.split())
    bare = re.sub(r'^[^\w(]+\s*', '', text)  # a key cap after its symbol: "⌫ Backspace"
    if not text or text in ALLOWED or text in KEY_CAPS or bare in KEY_CAPS:
        return None
    if re.fullmatch(r'[a-z][a-z0-9_ -]*', text) or ('/' in text and re.fullmatch(r'[a-z0-9_./ -]+', text)):
        return None  # CSS values, class or attribute names, or a topic path (all lower case)
    if re.search(r'var\(--|rgba?\(|\d(px|em|rem|%)\b', text) or re.fullmatch(r'([a-z-]+\s*:\s*[^;]+;\s*)+', text):
        return None  # CSS
    rest = re.sub(CODE_LIKE, ' ', text)
    for name in sorted(ALLOWED, key=len, reverse=True):
        rest = re.sub(r'(?<![A-Za-z])' + re.escape(name) + r'(?![A-Za-z])', ' ', rest)
    if not re.search(r'[A-Za-z]{2,}', rest):
        return None
    return text


# ── JS: a small tokenizer that knows strings, template literals and brackets ──

def skip_string(src, i):
    q, i = src[i], i + 1
    while i < len(src):
        if src[i] == '\\':
            i += 2
            continue
        if src[i] == q or (src[i] == '\n' and q != '`'):
            return i + 1
        if q == '`' and src.startswith('${', i):
            i = skip_brackets(src, i + 2, 1)
            continue
        i += 1
    return i


def skip_brackets(src, i, depth=1):
    while i < len(src):
        c = src[i]
        if c in '\'"`':
            i = skip_string(src, i)
            continue
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return i


CONTINUES = re.compile(r'\s*(\?|:|\+|\|\||&&|\.(?!\.)|\()')
ENDS_WITH_OP = re.compile(r'(\?|:|\+|\|\||&&|=|\(|,)\s*$')


def expression_end(src, i):
    """End of the expression that starts at i (a value after `x =`, `key:` or a call's first argument)."""
    depth, start = 0, i
    while i < len(src):
        c = src[i]
        if c in '\'"`':
            i = skip_string(src, i)
            continue
        if c in '([{':
            depth += 1
        elif c in ')]}':
            if depth == 0:
                return i
            depth -= 1
        elif depth == 0 and c in ',;':
            return i
        elif depth == 0 and c == '\n':
            if not CONTINUES.match(src, i + 1) and not ENDS_WITH_OP.search(src[start:i]):
                return i
        i += 1
    return i


# Calls whose string arguments are not shown (keys, selectors, look-ups). Callbacks of map(),
# filter(), find(), join() ... are read: rows are built there.
NOT_OUTPUT_CALLS = {'t', 'tf', 'tfHtml', 'getElementById', 'querySelector', 'querySelectorAll',
                    'includes', 'startsWith', 'endsWith', 'indexOf', 'split', 'replace', 'replaceAll',
                    'match', 'test', 'getAttribute', 'setAttribute', 'classList', 'add',
                    'remove', 'toggle', 'contains', 'request', 'fetch', 'get', 'set', 'has',
                    'delete', 'log', 'warn', 'error', 'padStart', 'padEnd', 'toFixed',
                    'toLocaleString', 'RegExp', 'Error', 'addEventListener', 'dispatchEvent',
                    'postJson', 'postForm', 'sendCommand', 'sendWsCommand', 'createElement'}
COMPARE = re.compile(r'(===|!==|==|!=|\bin|\bcase)\s*$')
# an object field whose value is shown; other fields (className, style, id, type, ...) are not
OUTPUT_KEYS = {'text', 'title', 'message', 'label', 'placeholder', 'html', 'okText', 'confirmText',
               'cancelText', 'aria-label', 'alt', 'hint', 'tooltip', 'desc', 'description', 'error'}
FIELD_BEFORE = re.compile(r'''[{,]\s*(?:([A-Za-z_$][\w$]*)|'([\w-]+)'|"([\w-]+)")\s*:\s*$''')
COMPARE_AFTER = re.compile(r'\s*(===|!==|==|!=)')


def output_strings(src, a, b):
    """(position, text) of every string in src[a:b] that is shown: not a call argument of a
    look-up or helper, not a comparison operand. Template literal text is split into its pieces."""
    found = []
    i = a
    calls = []  # stack: True when the bracket belongs to a non-output call
    while i < b:
        c = src[i]
        if c in '\'"':
            j = skip_string(src, i)
            field = FIELD_BEFORE.search(src[a:i])
            data_field = field and next(g for g in field.groups() if g) not in OUTPUT_KEYS
            if not any(calls) and not data_field and not COMPARE.search(src[a:i]) and not COMPARE_AFTER.match(src, j):
                found.append((i + 1, src[i + 1:j - 1]))
            i = j
            continue
        if c == '`':
            j = skip_string(src, i)
            field = FIELD_BEFORE.search(src[a:i])
            data_field = field and next(g for g in field.groups() if g) not in OUTPUT_KEYS
            if not any(calls) and not data_field:
                found.extend(template_pieces(src, i, j))
            i = j
            continue
        if c in '([{':
            name = re.search(r'([A-Za-z_$][\w$]*)\s*$', src[a:i]) if c == '(' else None
            calls.append(bool(name and name.group(1) in NOT_OUTPUT_CALLS))
        elif c in ')]}':
            if calls:
                calls.pop()
        i += 1
    return found


def strip_dollar(text):
    """A template literal's text without its ${...} parts."""
    out, k = [], 0
    while k < len(text):
        if text.startswith('${', k):
            k = skip_brackets(text, k + 2, 1)
            out.append(' ')
            continue
        out.append(text[k])
        k += 1
    return ''.join(out)


def template_pieces(src, i, j):
    """Text pieces of the template literal src[i:j] plus the shown strings of its ${...}."""
    out, k, piece = [], i + 1, i + 1
    end = j - 1
    while k < end:
        if src[k] == '\\':
            k += 2
            continue
        if src.startswith('${', k):
            out.append((piece, src[piece:k]))
            close = skip_brackets(src, k + 2, 1)
            out.extend(output_strings(src, k + 2, close - 1))
            k = close
            piece = k
            continue
        k += 1
    out.append((piece, src[piece:end]))
    return out


NEW_UI = [
    r'(?<![\w.$])(?:text|title|message|label|placeholder|html|okText|confirmText|cancelText)\s*:',
    r"['\"]aria-label['\"]\s*:",
    r'\btoast\.(?:success|error|warn|info|show)\(',
    r'\.(?:textContent|innerText|innerHTML|outerHTML|title|placeholder|label)\s*\+?=(?!=)',
    r"\.setAttribute\(\s*['\"](?:title|placeholder|aria-label)['\"]\s*,",
    r'\.insertAdjacentHTML\(\s*[\'"][\w-]+[\'"]\s*,',
    r'\b(?:createTextNode|append|prepend)\(',
    r'\bnew Option\(',
    r'(?<![\w.$])(?:alert|confirm|prompt)\(',
    # a variable that collects HTML: html += '<div>...'
    r'(?<![\w.$])\w*(?:[hH]tml|HTML)\w*\s*\+?=(?!=)',
]
LEGACY = NEW_UI + [r'\bshowToast\(', r'\bshowConfirm\(']


def blank_comments(src):
    """Comments become spaces (line numbers kept); `// i18n-ok` lines are remembered."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c in '\'"`':
            j = skip_string(src, i)
            out.append(src[i:j])
            i = j
        elif src.startswith('//', i):
            j = src.find('\n', i)
            j = n if j == -1 else j
            out.append(' ' * (j - i))
            i = j
        elif src.startswith('/*', i):
            j = src.find('*/', i + 2)
            j = n if j == -1 else j + 2
            out.append(re.sub(r'[^\n]', ' ', src[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def h_children(src):
    """Strings given as children of the new UI's h(tag, attrs, [children])."""
    hits = []
    for m in re.finditer(r'(?<![\w.$])h\(', src):
        close = skip_brackets(src, m.end(), 1)
        args, depth, k, starts = [], 0, m.end(), [m.end()]
        while k < close - 1:
            c = src[k]
            if c in '\'"`':
                k = skip_string(src, k)
                continue
            if c in '([{':
                depth += 1
            elif c in ')]}':
                depth -= 1
            elif c == ',' and depth == 0:
                starts.append(k + 1)
            k += 1
        if len(starts) >= 3:
            third = starts[2]
            text = src[third:close - 1].lstrip()
            off = third + (len(src[third:close - 1]) - len(text))
            if text.startswith('['):
                # only the direct string elements of the list
                k, depth = off + 1, 0
                end = skip_brackets(src, off + 1, 1) - 1
                while k < end:
                    c = src[k]
                    if c in '\'"' and depth == 0:
                        j = skip_string(src, k)
                        hits.append((k + 1, src[k + 1:j - 1]))
                        k = j
                        continue
                    if c in '\'"`':
                        k = skip_string(src, k)
                        continue
                    if c in '([{':
                        depth += 1
                    elif c in ')]}':
                        depth -= 1
                    k += 1
            elif text[:1] in '\'"':
                j = skip_string(src, off)
                hits.append((off + 1, src[off + 1:j - 1]))
    return hits


def js_hits(path, patterns, with_h):
    return js_text_hits(open(path, encoding='utf8').read(), patterns, with_h, os.path.relpath(path, ROOT))


def js_text_hits(raw, patterns, with_h, name):
    lines = raw.split('\n')
    src = blank_comments(raw)
    found = {}
    for pat in patterns:
        for m in re.finditer(pat, src):
            start = m.end()
            end = expression_end(src, start)
            for pos, text in output_strings(src, start, end):
                found[pos] = text
    if with_h:
        for pos, text in h_children(src):
            found[pos] = text
    # every template literal that holds markup, wherever it goes (rows built in map(), returned
    # from a function, collected in a variable)
    i = 0
    while i < len(src):
        c = src[i]
        if c in '\'"':
            i = skip_string(src, i)
            continue
        if c == '`':
            j = skip_string(src, i)
            if re.search(r'<[a-zA-Z/]', strip_dollar(src[i + 1:j - 1])):
                for pos, text in template_pieces(src, i, j):
                    found.setdefault(pos, text)
            i = j
            continue
        i += 1
    hits = []
    for pos, text in sorted(found.items()):
        # a template piece can span lines: check each line on its own
        line_no = src.count('\n', 0, pos) + 1
        for k, part in enumerate(text.split('\n')):
            w = words(part)
            n = line_no + k
            if w and 'i18n-ok' not in lines[n - 1] and 'translate="no"' not in lines[n - 1]:
                hits.append(f'{name}:{n}: {w[:80]!r}')
    return hits


# ── legacy index.html ──

class PageScan(HTMLParser):
    SKIP = {'script', 'style', 'code', 'pre', 'kbd'}
    VOID = {'area', 'base', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'source',
            'track', 'wbr'}
    ATTRS = {'placeholder': 'data-i18n-placeholder', 'title': 'data-i18n-title',
             'aria-label': 'data-i18n-aria-label'}

    def __init__(self, lines):
        super().__init__(convert_charrefs=True)
        self.lines = lines
        self.stack = []  # (tag, attrs dict)
        self.hits = []

    def ok_line(self, n):
        return 'i18n-ok' in self.lines[n - 1]

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        line = self.getpos()[0]
        no = a.get('translate') == 'no' or any(x.get('translate') == 'no' for _, x in self.stack)
        for attr, key in self.ATTRS.items():
            if no:
                break
            if a.get(attr) and key not in a:
                w = words(a[attr])
                if w and not self.ok_line(line):
                    self.hits.append(f'android-app/index.html:{line}: {attr}={w[:70]!r}')
        if tag not in self.VOID:
            self.stack.append((tag, a))

    def handle_startendtag(self, tag, attrs):
        self.handle_starttag(tag, attrs)
        if tag not in self.VOID and self.stack:
            self.stack.pop()

    def handle_endtag(self, tag):
        for k in range(len(self.stack) - 1, -1, -1):
            if self.stack[k][0] == tag:
                del self.stack[k:]
                break

    def handle_data(self, data):
        if any(t in self.SKIP or x.get('translate') == 'no' for t, x in self.stack) or not self.stack:
            return
        tag, a = self.stack[-1]
        if tag == 'title' or 'data-i18n' in a:
            return
        start = self.getpos()[0]
        for k, part in enumerate(data.split('\n')):
            w = words(part)
            if w and ' '.join(data.split()) not in KEY_CAPS and not self.ok_line(start + k):
                self.hits.append(f'android-app/index.html:{start + k}: {w[:80]!r}')


def html_hits(path):
    return html_text_hits(open(path, encoding='utf8').read())


def html_text_hits(raw):
    scan = PageScan(raw.split('\n'))
    scan.feed(raw)
    return scan.hits


def walk(base, sub):
    for d in sub:
        top = os.path.join(base, d)
        if os.path.isfile(top):
            yield top
            continue
        for dirpath, dirnames, files in os.walk(top):
            dirnames[:] = [x for x in dirnames if x not in SKIP_DIRS]
            for f in sorted(files):
                if f.endswith('.js'):
                    yield os.path.join(dirpath, f)


def scan_all():
    hits = []
    fe = os.path.join(ROOT, 'frontend', 'src')
    for p in walk(fe, ['views', 'components', 'remote', 'main.js']):
        hits += js_hits(p, NEW_UI, True)
    la = os.path.join(ROOT, 'android-app', 'src')
    for p in walk(la, ['app', 'remote']):
        hits += js_hits(p, LEGACY, False)
    hits += html_hits(os.path.join(ROOT, 'android-app', 'index.html'))
    return hits


def main():
    hits = scan_all()
    print('Web UI hard-coded text check')
    if hits:
        for h in hits:
            print('  ' + h)
        print(f'FAIL: {len(hits)} English text(s) not behind a translation key')
        return 1
    print('PASS: every checked text goes through a translation key')
    return 0


if __name__ == '__main__':
    sys.exit(main())
