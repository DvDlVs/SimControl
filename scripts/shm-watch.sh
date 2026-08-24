#!/usr/bin/env bash
# Vigia wine-mappings grandes e novos arquivos em /dev/shm por N segundos.
DUR="${1:-60}"
declare -A seen
for ((i=0; i<DUR; i++)); do
    for l in /proc/[0-9]*/fd/*; do
        t=$(readlink "$l" 2>/dev/null) || continue
        case "$t" in
            *wine-mapping*) ;;
            *) continue ;;
        esac
        sz=$(stat -Lc%s "$l" 2>/dev/null) || continue
        if [ "$sz" -gt 300 ]; then
            pid=$(echo "$l" | cut -d/ -f3)
            fd=$(basename "$l")
            key="$pid/$fd"
            [ -n "${seen[$key]}" ] && continue
            seen[$key]=1
            comm=$(cat "/proc/$pid/comm" 2>/dev/null)
            echo "[watch ${i}s] NOVO: pid=$pid($comm) fd=$fd size=$sz"
        fi
    done
    f=$(find /dev/shm -maxdepth 1 -newermt '-3 seconds' ! -name 'u1000*' ! -name 'pulse*' ! -name 'simcontrol*' 2>/dev/null)
    [ -n "$f" ] && echo "[watch ${i}s] /dev/shm novo/modificado: $f"
    sleep 1
done
echo "watch fim"
