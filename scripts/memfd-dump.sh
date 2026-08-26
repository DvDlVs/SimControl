#!/usr/bin/env bash
# Dump shared-memory wine-mappings of a running sim (RaceRoom, AMS2, AC...)
# so the raw bytes can be inspected and field offsets confirmed. Same user
# runs the game and this script — no root needed.
#
# Usage:
#   ./memfd-dump.sh                 list candidate processes + mappings
#   ./memfd-dump.sh PID FD [OUT]    copy /proc/PID/FD to OUT (default stdout)
#
# Example:
#   ./memfd-dump.sh                # find game pid + fd number
#   ./memfd-dump.sh 12345 47 dump.bin
#   xxd dump.bin | head -40        # inspect the raw bytes

set -u

dump() {
    local pid="$1" fd="$2" out="${3:--}"
    if [ ! -r "/proc/$pid/fd/$fd" ]; then
        echo "memfd-dump: cannot read /proc/$pid/fd/$fd" >&2
        return 1
    fi
    if [ "$out" = "-" ]; then
        cat "/proc/$pid/fd/$fd"
    else
        cp "/proc/$pid/fd/$fd" "$out" && echo "memfd-dump: wrote $out ($(stat -c%s "$out") bytes)"
    fi
}

if [ $# -ge 2 ]; then
    dump "$1" "$2" "${3:--}"
    exit $?
fi

printf '%-8s %-22s %s\n' PID GAME MAPPINGS
for p in /proc/[0-9]*; do
    pid="${p#/proc/}"
    cmd=$(tr '\0' ' ' <"$p/cmdline" 2>/dev/null)
    case "$cmd" in
        *[Rr][Rr][Rr][Ee]*|*AMS2AVX*|*pCARS2AVX*|*AMS.exe*|*[Rr][Ff]actor2.exe*) ;;
        *) continue ;;
    esac
    game=$(printf '%s' "$cmd" | cut -c1-20)
    while IFS= read -r l; do
        fd="${l%/wine-mapping*}"; fd="${fd##*/}"
        sz=$(stat -c%s "$p/fd/$fd" 2>/dev/null) || continue
        printf '%-8s %-22s fd=%-4s %s bytes\n' "$pid" "$game" "$fd" "$sz"
    done < <(find "$p/fd" -maxdepth 1 -type l -lname '*wine-mapping*' -printf '%p\n' 2>/dev/null)
done
