#!/bin/sh
# /cache/kernel6/modprobe.sh: modprobe for the Linux 6.18 Hub system, installed as /sbin/modprobe
# and as the kernel's on-demand loader (/proc/sys/kernel/modprobe). The BusyBox on the Hubs has
# the small modprobe, which loads by trial and retry and does not follow a symlinked directory.
# This one uses modules.dep (dependencies, deepest first) and modules.alias (the kernel asks for
# aliases such as tty-ldisc-15 or net-pf-31).
#
#   modprobe [-q] [-r] [--] <module or alias> [param=value ...]
D=${MODPROBE_DIR:-/lib/modules/$(uname -r)}  # MODPROBE_DIR: tests
q=0
r=0
while [ $# -gt 0 ]; do
    case "$1" in
        -q) q=1 ;;
        -r) r=1 ;;
        -qr|-rq) q=1; r=1 ;;
        --) shift; break ;;
        -*) ;;
        *) break ;;
    esac
    shift
done
name=$1
[ -n "$name" ] || exit 1
shift
say() { [ $q = 1 ] || echo "modprobe: $*" >&2; }

# module file for a name (dashes and underscores are the same). One awk pass: the loop with tr
# and basename per line took 2.3 s for a name not there, and the kernel asks for those on
# every socket() of a family it lacks (net-pf-10, IPv6): each Wi-Fi key wpa_supplicant set
# waited for it, 20 s from association to the connection.
modpath() {
    awk -v n="$1" 'BEGIN { gsub("-", "_", n) }
        { f = $1; sub(":.*", "", f); sub(".*/", "", f); sub("\\.ko$", "", f); gsub("-", "_", f)
          if (f == n) { print; found = 1; exit } }
        END { exit !found }' "$D/modules.dep"
}

line=$(modpath "$name")
if [ -z "$line" ]; then
    # alias: modules.alias lines are "alias <glob> <module>". Only the lines starting with the
    # name's leading word (net, tty, usb, ...) go through the glob match: all ~700 took 0.5 s.
    pre=${name%%[!A-Za-z0-9_]*}
    mod=$(grep "^alias $pre" "$D/modules.alias" | while read -r a pat m; do
        case "$name" in $pat) echo "$m"; break ;; esac
    done)
    [ -n "$mod" ] && line=$(modpath "$mod")
fi
[ -n "$line" ] || { say "module '$name' not found"; exit 1; }

if [ $r = 1 ]; then
    rmmod "$(basename "${line%%:*}" .ko)"
    exit $?
fi
# the module last, its needs first (modules.dep lists the deepest last)
deps=$(echo ${line#*:} | tr ' ' '\n' | awk '{a[NR]=$0} END {for (i=NR; i>0; i--) print a[i]}')
for m in $deps "${line%%:*}"; do
    n=$(basename "$m" .ko | tr - _)
    grep -q "^$n " /proc/modules && continue
    if [ "$m" = "${line%%:*}" ]; then
        insmod "$D/$m" "$@"
    else
        insmod "$D/$m"
    fi || { say "insmod $m failed"; exit 1; }
done
exit 0
