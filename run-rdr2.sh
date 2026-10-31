#!/usr/bin/env bash
# Launches Red Dead Redemption 2 through Beer Loader.
#
#   ./run-rdr2.sh            start the game (log only)
#   ./run-rdr2.sh -v         also show the loader output live
#
# The log of each attempt goes to ~/rdr2-logs/, and ~/rdr2.log points at the latest one.
# Two known start-up failures are retried automatically (up to RDR2_RETRIES times):
#   - a crash before the game reaches its renderer (rare start-up race)
#   - a hang inside the protection code before the game window appears
# Ctrl+C stops the game and everything it started.
#
# Environment: RDR2_DIR (game folder), RDR2_RETRIES (default 3), any BEER_* diagnostics.

set -u

LOADER="$(cd -- "$(dirname -- "$0")" && pwd)/loader"
GAME_DIR="${RDR2_DIR:-/mnt/wwn-0x5000c500c67d4454-part1/Games/RDR2}"
RETRIES="${RDR2_RETRIES:-3}"
LOG_DIR="$HOME/rdr2-logs"
HANG_SECONDS=120          # silence before the game window appears that counts as a hang
VERBOSE=0
[[ "${1:-}" == "-v" ]] && VERBOSE=1

[[ -x "$LOADER" ]] || { echo "loader not built: run make in $(dirname "$LOADER")" >&2; exit 1; }
[[ -f "$GAME_DIR/Launcher.exe" ]] || { echo "Launcher.exe not found in $GAME_DIR (set RDR2_DIR)" >&2; exit 1; }
mkdir -p "$LOG_DIR"

loader_pids() { pgrep -f "^$LOADER " || true; }

stop_game() {
    local pids
    pids=$(loader_pids)
    [[ -n "$pids" ]] && kill -TERM $pids 2>/dev/null && sleep 2
    pids=$(loader_pids)
    [[ -n "$pids" ]] && kill -KILL $pids 2>/dev/null
    rm -f "/run/user/$(id -u)/beer/"*.proc "/run/user/$(id -u)/beer/"*.sock 2>/dev/null
}

trap 'echo; echo "Stopping RDR2..."; stop_game; exit 130' INT TERM

if [[ -n "$(loader_pids)" ]]; then
    echo "Beer Loader is already running; stopping it first."
    stop_game
fi

cd "$GAME_DIR" || exit 1

for attempt in $(seq 1 $((RETRIES + 1))); do
    log="$LOG_DIR/rdr2-$(date +%Y%m%d-%H%M%S).log"
    ln -sf "$log" "$HOME/rdr2.log"
    echo "Starting RDR2 (attempt $attempt), log: $log"

    "$LOADER" --audio=fmod Launcher.exe > "$log" 2>&1 &
    pid=$!
    viewer=""
    if (( VERBOSE )); then tail -n +1 -f "$log" & viewer=$!; fi

    outcome=exited
    last_size=-1; quiet=0; window=0
    while kill -0 "$pid" 2>/dev/null || [[ -n "$(loader_pids)" ]]; do
        sleep 5
        if (( !window )) && grep -aq "CreateWindowExW" "$log"; then
            window=1
            echo "Game window created."
        fi
        if grep -aq "^\[CRASH\]" "$log"; then outcome=crash; break; fi
        if (( !window )); then
            size=$(stat -c %s "$log")
            if (( size == last_size )); then quiet=$((quiet + 5)); else quiet=0; last_size=$size; fi
            if (( quiet >= HANG_SECONDS )); then outcome=hang; break; fi
        fi
    done

    [[ -n "$viewer" ]] && kill "$viewer" 2>/dev/null
    case "$outcome" in
        exited)
            wait "$pid" 2>/dev/null
            echo "RDR2 exited."
            stop_game
            exit 0 ;;
        crash)
            echo "RDR2 crashed:"
            grep -a -m1 "^\[CRASH\] SIG" "$log"
            stop_game ;;
        hang)
            echo "RDR2 hung during start-up (no output for ${HANG_SECONDS}s)."
            stop_game ;;
    esac
    # A crash after the renderer is up is not the known start-up race: keep the log and stop.
    if [[ "$outcome" == crash ]] && grep -aq "NvLL_VK_Initialize" "$log"; then
        echo "The crash happened after start-up; not retrying. Log: $log"
        exit 1
    fi
    (( attempt <= RETRIES )) && echo "Retrying..."
done

echo "RDR2 failed to start after $((RETRIES + 1)) attempts. Latest log: $HOME/rdr2.log"
exit 1
