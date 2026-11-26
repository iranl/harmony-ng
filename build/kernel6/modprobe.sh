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

# module file for a name (dashes and underscores are the same)
modpath() {
    n=$(echo "$1" | tr - _)
    while read -r l; do
        f=${l%%:*}
        b=$(basename "$f" .ko | tr - _)
        [ "$b" = "$n" ] && { echo "$l"; return 0; }
    done < "$D/modules.dep"
    return 1
}

line=$(modpath "$name")
if [ -z "$line" ]; then
    # alias: modules.alias lines are "alias <glob> <module>"
    while read -r a pat mod; do
        [ "$a" = alias ] || continue
        case "$name" in $pat) line=$(modpath "$mod") && break ;; esac
    done < "$D/modules.alias"
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
