#!/usr/bin/env python3
"""The Hub's C code uses no wait that is built on futex.

The Hub's kernel (Logitech 2.6.31, MIPS) has no futex system call: FUTEX_WAIT returns ENOSYS
at once, and uClibc-ng ignores the error and tries again. So every wait built on futex never
sleeps on the Hub and spins at full CPU for as long as it waits: pthread_cond_wait,
pthread_cond_timedwait, sem_wait, sem_timedwait, pthread_join, pthread_barrier_wait. Wait on a
pipe (or socket) with poll() or select() instead, or sleep in short steps with usleep() where
the wait is rare and short; a thread that must be waited for sets a flag as its last action
and is detached once the flag is set (see payload/source/ir_i2s.c).

Scans payload/source/*.c|*.h and payload/source/hub_btstack/*.c|*.h (the Hub binaries), not
payload/source/remote/ (the remote's kernel has futex) and not vendored code. Comments and
strings are ignored.
"""
import os
import re
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(SCRIPT_DIR)
SRC = os.path.join(REPO, "payload", "source")
DIRS = [SRC, os.path.join(SRC, "hub_btstack")]
VENDORED = {"cJSON.c", "cJSON.h", "tweetnacl.c", "tweetnacl.h"}
WAITS = ["pthread_cond_wait", "pthread_cond_timedwait", "pthread_cond_clockwait",
         "sem_wait", "sem_timedwait", "sem_clockwait",
         "pthread_join", "pthread_timedjoin_np", "pthread_clockjoin_np",
         "pthread_barrier_wait"]
CALL = re.compile(r"\b(" + "|".join(WAITS) + r")\s*\(")
# block comments, line comments, string and character literals (kept: their newlines)
TOKENS = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)


def code_only(text):
    return TOKENS.sub(lambda m: "\n" * m.group(0).count("\n") if m.group(0)[0] == "/" else '""', text)


def scan():
    hits = []
    files = 0
    for d in DIRS:
        for name in sorted(os.listdir(d)):
            if not name.endswith((".c", ".h")) or name in VENDORED:
                continue
            path = os.path.join(d, name)
            with open(path, encoding="utf-8", errors="replace") as f:
                code = code_only(f.read())
            files += 1
            for lineno, line in enumerate(code.split("\n"), 1):
                for m in CALL.finditer(line):
                    hits.append("%s:%d: %s()" % (os.path.relpath(path, REPO), lineno, m.group(1)))
    return files, hits


def self_check():
    """The scanner finds a call in code and ignores names in comments and strings"""
    sample = ('/* pthread_join(t) in a comment */\n'
              'const char *s = "sem_wait(x)";\n'
              '// pthread_cond_wait(&c, &m);\n'
              'int f(void) { return pthread_join (t, NULL); }\n')
    found = [m.group(1) for line in code_only(sample).split("\n") for m in CALL.finditer(line)]
    return found == ["pthread_join"]


def main():
    if not self_check():
        print("[-] test_hub_no_futex_waits: the scanner itself is broken")
        return 1
    files, hits = scan()
    if hits:
        print("[-] test_hub_no_futex_waits: futex-based waits in Hub code (%d files scanned):" % files)
        for h in hits:
            print("    " + h)
        print("    The Hub's kernel has no futex system call: these waits never sleep there and spin at")
        print("    full CPU. Wait on a pipe with poll()/select(), or sleep in short steps with usleep();")
        print("    see the docstring of tests/test_hub_no_futex_waits.py.")
        return 1
    print("[+] test_hub_no_futex_waits: no futex-based wait in %d Hub source files" % files)
    return 0


if __name__ == "__main__":
    sys.exit(main())
