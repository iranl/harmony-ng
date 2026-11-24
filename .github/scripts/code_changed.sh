#!/bin/sh
# Does a change need the builds and the firmware tests? Reads the changed paths, one per line,
# on stdin (run from the repository root) and prints "true" or "false".
# "false" only when every changed file is a Markdown file that no test names: such files are
# prose only (the roadmap, guides). A Markdown file a test reads (API.md, the RF protocol, ...)
# still runs everything, and so does any file under tests/ or .github/, and an empty list
# (nothing to compare: run, to be safe).
run=false
count=0
while IFS= read -r path; do
    [ -n "$path" ] || continue
    count=$((count + 1))
    case "$path" in
        tests/*|.github/*)
            echo "tests needed: $path" >&2
            run=true
            ;;
        *.md)
            name=${path##*/}
            if grep -rqF -- "$name" tests; then
                echo "tests needed: $path (named in tests/)" >&2
                run=true
            else
                echo "prose only: $path" >&2
            fi
            ;;
        *)
            echo "tests needed: $path" >&2
            run=true
            ;;
    esac
done
[ "$count" -gt 0 ] || { echo "no changed files listed: running everything" >&2; run=true; }
echo "$run"
