#!/usr/bin/env python3
"""
Both web UIs build HTML strings for innerHTML. A device, command, activity, button, host or
token name put into such a string without escaping becomes markup (stored XSS). This scan
fails when a template interpolation of a name-like value sits in an HTML line and is not
wrapped in escapeHtml(). It is a guard against new sites, not a full HTML parser: when a
value is known to be safe (a number, a fixed string), name it differently or wrap it anyway.

A translated text is safe as it is (t('key')), but a value put into it is not: t('key') with
.replace(...), the legacy tf('key', { name }) and the new UI t('key', 'text', { name }) are checked
like any other expression. tfHtml() escapes its values.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIRS = [os.path.join(ROOT, "android-app", "src"), os.path.join(ROOT, "frontend", "src")]
NAMEISH = re.compile(r"\b(name|names|label|model|manufacturer|title|path|alias|desc|description|command|cmd|text|username|devName|devLabel|addr|host|message)\b")
SAFE = re.compile(r"^\s*(escapeHtml|this\.escapeHtml|tfHtml|encodeURIComponent|Number|parseInt|String\(\s*\w+\s*\)\.length)\s*\(")
# a translated text without values: t('key') or t('key', 'fallback')
PLAIN_T = re.compile(r"""^\s*(t|tr)\(\s*(['"])(?:(?!\2).)*\2\s*(,\s*(['"])(?:(?!\4).)*\4\s*)?\)\s*$""")
# values that are numbers: a count, a length, a size
NUMERIC = re.compile(r"^\s*(?:[\w$.]+\.(?:length|size)|Number\(.*\)|\d+)\s*$")


def skip_string(text, i):
    q, i = text[i], i + 1
    while i < len(text):
        if text[i] == "\\":
            i += 2
            continue
        if q == "`" and text.startswith("${", i):
            i = skip_expr(text, i + 2)
            continue
        if text[i] == q:
            return i + 1
        i += 1
    return i


def skip_expr(text, i):
    """Index after the } that closes the ${ expression starting at i."""
    depth = 1
    while i < len(text):
        c = text[i]
        if c in "'\"`":
            i = skip_string(text, i)
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return i


def split_top(text, sep=","):
    parts, depth, cur, i = [], 0, "", 0
    while i < len(text):
        c = text[i]
        if c in "'\"`":
            j = skip_string(text, i)
            cur += text[i:j]
            i = j
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        if c == sep and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += c
        i += 1
    parts.append(cur)
    return [x.strip() for x in parts if x.strip()]


VALUES_CALL = re.compile(r"^\s*(tf|t)\((.*)\)\s*$", re.S)


def unsafe(expr):
    """True when a name-like value reaches the HTML unescaped."""
    if not NAMEISH.search(expr) or SAFE.match(expr) or PLAIN_T.match(expr) or NUMERIC.match(expr):
        return False
    m = VALUES_CALL.match(expr)
    if m and not re.search(r"\)\s*\.", expr):
        # tf('key', { a: x, b: y }) / t('key', 'text', { a: x }): only the values are output
        args = split_top(m.group(2))
        objs = [a for a in args if a.startswith("{") and a.endswith("}")]
        if objs and all(re.match(r"""^(['"]).*\1$""", a, re.S) or a in objs for a in args):
            for entry in split_top(objs[-1][1:-1]):
                key, _, value = entry.partition(":")
                value = value or key  # shorthand { name }
                if unsafe(value.strip()):
                    return True
            return False
    # a conditional that only picks fixed strings, e.g. ${x.name ? 'a' : 'b'}: names in the
    # condition are not output
    if "?" in expr:
        branches = re.sub(r"'[^']*'", "", expr.rsplit("?", 1)[1])
        if not NAMEISH.search(branches):
            return False
    return True


def interpolations(line):
    """Every ${...} of a line, nested ones too, each without its own nested templates."""
    out = []
    i = line.find("${")
    while i != -1:
        end = skip_expr(line, i + 2)
        expr = line[i + 2:end - 1]
        out.append(re.sub(r"`(?:[^`\\]|\\.)*`", "``", expr))
        i = line.find("${", i + 2)
    return out


def scan():
    hits = []
    for base in DIRS:
        for dirpath, dirnames, files in os.walk(base):
            dirnames[:] = [d for d in dirnames if d not in ("node_modules", "dist")]
            for fn in files:
                if not fn.endswith(".js"):
                    continue
                path = os.path.join(dirpath, fn)
                with open(path, encoding="utf-8", errors="replace") as f:
                    for no, line in enumerate(f, 1):
                        if "<" not in line and '="' not in line:
                            continue
                        if "console." in line:
                            continue
                        for expr in interpolations(line):
                            if not unsafe(expr):
                                continue
                            hits.append("%s:%d: ${%s}" % (os.path.relpath(path, ROOT), no, expr.strip()[:80]))
    return hits


def main():
    hits = scan()
    if hits:
        print("[-] unescaped name interpolations in HTML templates:")
        for h in hits:
            print("    " + h)
        sys.exit(1)
    print("[+] ui escape scan: no unescaped name interpolation in HTML templates")


if __name__ == "__main__":
    main()
