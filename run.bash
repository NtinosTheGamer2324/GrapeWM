#!/usr/bin/env bash
# ============================================================
#  grape-launch.sh — Auto-launch GRAPE WM in WSL2
#  Tries (in order):
#    1. Xephyr with custom TMPDIR
#    2. Xephyr on localhost TCP
#    3. Host VcXsrv / Xming (Windows X server)
# ============================================================

set -e

GRAPE_BIN="./grape-wm"
SCREEN_SIZE="1024x768"
DISPLAY_NUM=2
XEPHYR_PID=""
GRAPE_PID=""

# ---- colours ------------------------------------------------
RED='\033[0;31m'
GRN='\033[0;32m'
YEL='\033[1;33m'
CYN='\033[0;36m'
RST='\033[0m'

log()  { echo -e "${CYN}[GRAPE]${RST} $*"; }
ok()   { echo -e "${GRN}[  OK ]${RST} $*"; }
warn() { echo -e "${YEL}[WARN ]${RST} $*"; }
err()  { echo -e "${RED}[FAIL ]${RST} $*"; }

# ---- cleanup on exit ----------------------------------------
cleanup() {
    log "Cleaning up..."
    [ -n "$GRAPE_PID" ] && kill "$GRAPE_PID" 2>/dev/null && ok "Stopped GRAPE (pid $GRAPE_PID)"
    [ -n "$XEPHYR_PID" ] && kill "$XEPHYR_PID" 2>/dev/null && ok "Stopped Xephyr (pid $XEPHYR_PID)"
    rm -f /tmp/grape.pid
}
trap cleanup EXIT INT TERM

# ---- sanity checks ------------------------------------------
if [ ! -f "$GRAPE_BIN" ]; then
    err "Cannot find $GRAPE_BIN — run this script from your project root."
    exit 1
fi

if ! command -v xterm &>/dev/null; then
    warn "xterm not found — installing..."
    sudo apt-get install -y xterm
fi

# ---- helper: wait for display to accept connections ---------
wait_for_display() {
    local disp="$1"
    local tries=20
    log "Waiting for display $disp..."
    for ((i=0; i<tries; i++)); do
        if DISPLAY="$disp" xdpyinfo &>/dev/null 2>&1; then
            ok "Display $disp is ready."
            return 0
        fi
        sleep 0.3
    done
    return 1
}

# ---- helper: launch GRAPE + xterm on a given display --------
launch_grape() {
    local disp="$1"
    export DISPLAY="$disp"

    log "Starting GRAPE on display $disp..."
    "$GRAPE_BIN" &
    GRAPE_PID=$!
    sleep 0.5

    if ! kill -0 "$GRAPE_PID" 2>/dev/null; then
        err "GRAPE exited immediately on display $disp."
        GRAPE_PID=""
        return 1
    fi
    ok "GRAPE running (pid $GRAPE_PID)"

    log "Launching xterm..."
    DISPLAY="$disp" xterm &
    ok "xterm launched on $disp"
    return 0
}

# ==============================================================
# METHOD 1 — Xephyr with custom TMPDIR
# ==============================================================
try_xephyr_tmpdir() {
    log "Method 1: Xephyr with custom TMPDIR..."

    if ! command -v Xephyr &>/dev/null; then
        warn "Xephyr not installed — skipping. (sudo apt install xserver-xephyr)"
        return 1
    fi

    local xtmp="$HOME/tmp/.X11-unix"
    mkdir -p "$xtmp"

    TMPDIR="$HOME/tmp" Xephyr -ac -screen "$SCREEN_SIZE" ":$DISPLAY_NUM" &>/tmp/xephyr.log &
    XEPHYR_PID=$!
    sleep 1

    if ! kill -0 "$XEPHYR_PID" 2>/dev/null; then
        warn "Xephyr (TMPDIR method) failed to start."
        XEPHYR_PID=""
        return 1
    fi

    local disp="localhost:${DISPLAY_NUM}.0"
    if wait_for_display "$disp"; then
        launch_grape "$disp" && return 0
    fi

    warn "Xephyr started but display not reachable via $disp."
    kill "$XEPHYR_PID" 2>/dev/null
    XEPHYR_PID=""
    return 1
}

# ==============================================================
# METHOD 2 — Xephyr plain TCP fallback
# ==============================================================
try_xephyr_tcp() {
    log "Method 2: Xephyr plain TCP..."

    if ! command -v Xephyr &>/dev/null; then
        warn "Xephyr not installed — skipping."
        return 1
    fi

    Xephyr -ac -screen "$SCREEN_SIZE" ":$DISPLAY_NUM" &>/tmp/xephyr.log &
    XEPHYR_PID=$!
    sleep 1

    if ! kill -0 "$XEPHYR_PID" 2>/dev/null; then
        warn "Xephyr (TCP method) failed to start."
        XEPHYR_PID=""
        return 1
    fi

    for disp in "localhost:${DISPLAY_NUM}.0" ":${DISPLAY_NUM}"; do
        if wait_for_display "$disp"; then
            launch_grape "$disp" && return 0
        fi
    done

    warn "Xephyr running but no display reachable."
    kill "$XEPHYR_PID" 2>/dev/null
    XEPHYR_PID=""
    return 1
}

# ==============================================================
# METHOD 3 — Host Windows X server (VcXsrv / Xming)
# ==============================================================
try_host_xserver() {
    log "Method 3: Host Windows X server (VcXsrv/Xming)..."

    # Resolve the Windows host IP from WSL2
    local host_ip
    host_ip=$(grep nameserver /etc/resolv.conf 2>/dev/null | awk '{print $2}' | head -1)

    if [ -z "$host_ip" ]; then
        err "Could not determine Windows host IP from /etc/resolv.conf."
        return 1
    fi
    ok "Windows host IP: $host_ip"

    local disp="${host_ip}:0"

    if ! wait_for_display "$disp"; then
        err "No X server found at $disp."
        echo ""
        echo -e "${YEL}  ► Please install and start VcXsrv on Windows:${RST}"
        echo "    https://sourceforge.net/projects/vcxsrv/"
        echo "    Launch with: Multiple windows, Display 0,"
        echo "    disable access control (tick 'Disable access control')"
        echo ""
        return 1
    fi

    ok "Found X server at $disp"
    launch_grape "$disp" && return 0
    return 1
}

# ==============================================================
# Main — try methods in order
# ==============================================================
echo ""
echo -e "${CYN}╔══════════════════════════════════════╗${RST}"
echo -e "${CYN}║     GRAPE Window Manager Launcher    ║${RST}"
echo -e "${CYN}╚══════════════════════════════════════╝${RST}"
echo ""

# Kill any stale Xephyr on our display number
pkill -f "Xephyr.*:${DISPLAY_NUM}" 2>/dev/null && sleep 0.5

try_xephyr_tmpdir  && { log "Running via Method 1. Press Ctrl+C to stop."; wait; exit 0; }
try_xephyr_tcp     && { log "Running via Method 2. Press Ctrl+C to stop."; wait; exit 0; }
try_host_xserver   && { log "Running via Method 3. Press Ctrl+C to stop."; wait; exit 0; }

echo ""
err "All methods failed. Xephyr log (if any):"
cat /tmp/xephyr.log 2>/dev/null || true
echo ""
err "Manual steps to try:"
echo "  1. sudo apt install xserver-xephyr xterm"
echo "  2. Install VcXsrv: https://sourceforge.net/projects/vcxsrv/"
echo "  3. Run VcXsrv with 'Disable access control' checked"
exit 1