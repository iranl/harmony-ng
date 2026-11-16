#!/usr/bin/env python3
"""
Both web UIs build HTML strings for innerHTML. A device, command, activity, button, host or
token name put into such a string without escaping becomes markup (stored XSS). This scan
fails when a template interpolation of a name-like value sits in an HTML line and is not
wrapped in escapeHtml(). It is a guard against new sites, not a full HTML parser: when a
value is known to be safe (a number, a fixed string), name it differently or wrap it anyway.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIRS = [os.path.join(ROOT, "android-app", "src"), os.path.join(ROOT, "frontend", "src")]
NAMEISH = re.compile(r"\b(name|names|label|model|manufacturer|title|path|alias|desc|description|command|cmd|text|username|devName|devLabel|addr|host)\b")
INTERP = re.compile(r"\$\{([^{}`]*)\}")  # innermost interpolations only
SAFE = re.compile(r"^\s*(escapeHtml|this\.escapeHtml|t|tr|encodeURIComponent|Number|parseInt|String\(\s*\w+\s*\)\.length)\s*\(")


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
                        for m in INTERP.finditer(line):
                            expr = m.group(1)
                            if not NAMEISH.search(expr) or SAFE.match(expr):
                                continue
                            # a conditional that only picks fixed strings, e.g. ${x.name ? 'a' : 'b'}:
                            # names in the condition are not output
                            if "?" in expr:
                                branches = re.sub(r"'[^']*'", "", expr.rsplit("?", 1)[1])
                                if not NAMEISH.search(branches):
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
