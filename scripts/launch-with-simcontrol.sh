#!/usr/bin/env bash
# Steam "Launch Options" wrapper: starts simcontrol BEFORE the game launches
# (so it already sees the virtual wheel from the start) and kills simcontrol
# when the game closes.
#
# Usage — game Properties on Steam -> Launch Options:
#   /full/path/to/simcontrol-linux/scripts/launch-with-simcontrol.sh %command%
#
# If a simcontrol is already running (e.g. started by simcontrol_config.py),
# this script does not start another one — it just lets the game launch.

set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/simcontrol"
CONF="$ROOT/simcontrol.conf"
IPC_SHM="/dev/shm/simcontrol_ipc"
LOG="${XDG_STATE_HOME:-$HOME/.local/state}/simcontrol/simcontrol.log"
mkdir -p "$(dirname "$LOG")"

if [ ! -x "$BIN" ]; then
    echo "launch-with-simcontrol: $BIN missing or not executable (run 'make' in $ROOT)" >&2
    exec "$@"
fi

STARTED_HERE=0
SC_PID=""

if pgrep -x "$(basename "$BIN")" >/dev/null 2>&1; then
    echo "launch-with-simcontrol: simcontrol already running, not starting another." >>"$LOG"
else
    "$BIN" -c "$CONF" >>"$LOG" 2>&1 &
    SC_PID=$!
    STARTED_HERE=1
    # Wait for the daemon to open the physical pad + create the virtual wheel
    # (/dev/shm/simcontrol_ipc only appears after that) before launching the game.
    for _ in $(seq 1 50); do
        [ -e "$IPC_SHM" ] && break
        sleep 0.1
    done
fi

UI_PID=""
UI_PY="$ROOT/ui/simcontrol_config.py"
if [ -f "$UI_PY" ] && command -v python3 >/dev/null 2>&1; then
    python3 "$UI_PY" >>"$LOG" 2>&1 &
    UI_PID=$!
fi

cleanup() {
    if [ -n "$UI_PID" ]; then kill "$UI_PID" 2>/dev/null; fi

    if [ "$STARTED_HERE" = 1 ] && [ -n "$SC_PID" ]; then
        kill "$SC_PID" 2>/dev/null
    fi
}
trap cleanup EXIT INT TERM

"$@"
