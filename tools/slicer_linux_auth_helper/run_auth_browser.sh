#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only
set -Eeuo pipefail
umask 077

if [[ $# -eq 6 ]]; then
    MODE=auth
    STATE_DIR=$1
    LOGIN_URL=$2
    LOGIN_CMD_FILE=$3
    RESULT_FILE=$4
    PROFILE_DIR=$5
    VNC_PASSWORD_FILE=$6
    COMMAND_FILE=$STATE_DIR/browser-command.json
    EVENT_FILE=$STATE_DIR/browser-events.ndjson
elif [[ $# -eq 9 ]]; then
    MODE=$1
    STATE_DIR=$2
    LOGIN_URL=$3
    LOGIN_CMD_FILE=$4
    RESULT_FILE=$5
    PROFILE_DIR=$6
    VNC_PASSWORD_FILE=$7
    COMMAND_FILE=$8
    EVENT_FILE=$9
else
    echo "usage: $0 MODE STATE_DIR URL LOGIN_CMD_FILE RESULT_FILE PROFILE_DIR VNC_PASSWORD_FILE COMMAND_FILE EVENT_FILE" >&2
    exit 64
fi

[[ "$MODE" == auth || "$MODE" == browse ]] || { echo "invalid Linux browser mode: $MODE" >&2; exit 64; }
[[ -f "$VNC_PASSWORD_FILE" ]] || { echo "missing private VNC password file" >&2; exit 64; }
VNC_PASSWORD=$(cat "$VNC_PASSWORD_FILE")
rm -f "$VNC_PASSWORD_FILE"
[[ ${#VNC_PASSWORD} -ge 12 ]] || { echo "invalid private VNC password" >&2; exit 64; }
RUNTIME_DIR=${SLICER_LINUX_RUNTIME_COMPONENT_DIR:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}
BROWSER=${SLICER_LINUX_RUNTIME_AUTH_BROWSER:-}
if [[ -z "$BROWSER" ]]; then
    case "$(uname -m)" in
        aarch64|arm64)
            BROWSER="$RUNTIME_DIR/slicer_linux_auth_browser_aarch64"
            ;;
        x86_64|amd64)
            if [[ -x "$RUNTIME_DIR/slicer_linux_auth_browser_x86_64" ]]; then
                BROWSER="$RUNTIME_DIR/slicer_linux_auth_browser_x86_64"
            else
                BROWSER="$RUNTIME_DIR/slicer_linux_auth_browser"
            fi
            ;;
        *)
            echo "unsupported Linux auth browser architecture: $(uname -m)" >&2
            exit 69
            ;;
    esac
fi
unset LD_LIBRARY_PATH LD_PRELOAD
export GDK_BACKEND=x11
export LIBGL_ALWAYS_SOFTWARE=1
export WEBKIT_DISABLE_COMPOSITING_MODE=1
export WEBKIT_DISABLE_DMABUF_RENDERER=1
WINDOWS_WSL_RUNTIME=0
case "$(uname -r)" in
    *microsoft*|*Microsoft*)
        WINDOWS_WSL_RUNTIME=1
        [[ ! -f /etc/orcastudio-auth-disable-gigacage ]] || export GIGACAGE_ENABLED=0
        ;;
esac
CLIENT_VERSION=${SLICER_LINUX_RUNTIME_AUTH_CLIENT_VERSION:-0.0.0.0}
LANGUAGE=${SLICER_LINUX_RUNTIME_AUTH_LANGUAGE:-en}
THEME=${SLICER_LINUX_RUNTIME_AUTH_THEME:-light}
[[ "$CLIENT_VERSION" =~ ^[A-Za-z0-9._-]{1,32}$ ]] || CLIENT_VERSION=0.0.0.0
[[ "$LANGUAGE" =~ ^[A-Za-z0-9_-]{1,16}$ ]] || LANGUAGE=en
[[ "$THEME" == dark || "$THEME" == light ]] || THEME=light
NOVNC_PORT=${SLICER_LINUX_RUNTIME_AUTH_NOVNC_PORT:-}
VNC_PORT=${SLICER_LINUX_RUNTIME_AUTH_VNC_PORT:-}
case "$NOVNC_PORT:$VNC_PORT" in
    *[!0-9:]*|:*|*::*|*:) echo "invalid Linux browser ports" >&2; exit 64 ;;
esac
if [[ "$NOVNC_PORT" -le 0 || "$NOVNC_PORT" -gt 65535 || "$VNC_PORT" -le 0 || "$VNC_PORT" -gt 65535 || "$NOVNC_PORT" -eq "$VNC_PORT" ]]; then
    echo "invalid Linux browser port range" >&2
    exit 64
fi
LISTEN_HOST=${SLICER_LINUX_RUNTIME_AUTH_LISTEN_HOST:-127.0.0.1}

mkdir -p "$STATE_DIR" "$PROFILE_DIR"
chmod 700 "$STATE_DIR" "$PROFILE_DIR"
rm -f "$STATE_DIR/ready" "$COMMAND_FILE"
: >"$EVENT_FILE"
chmod 600 "$EVENT_FILE"

for cmd in Xvfb x11vnc websockify; do
    command -v "$cmd" >/dev/null 2>&1 || { echo "missing Linux auth dependency: $cmd" >&2; exit 69; }
done
if [[ "$WINDOWS_WSL_RUNTIME" -eq 1 ]]; then
    command -v xdpyinfo >/dev/null 2>&1 || { echo "missing Windows WSL auth dependency: xdpyinfo" >&2; exit 69; }
fi
[[ -x "$BROWSER" ]] || { echo "missing Linux auth browser: $BROWSER" >&2; exit 69; }

NOVNC_WEB=${SLICER_LINUX_RUNTIME_NOVNC_WEB:-}
if [[ -z "$NOVNC_WEB" ]]; then
    for candidate in \
        "$RUNTIME_DIR/share/novnc" \
        "${APPDIR:-}/share/novnc" \
        /usr/share/novnc \
        /usr/share/noVNC \
        /opt/novnc; do
        if [[ -f "$candidate/vnc.html" ]]; then NOVNC_WEB=$candidate; break; fi
    done
fi
[[ -n "$NOVNC_WEB" && -f "$NOVNC_WEB/vnc.html" ]] || { echo "noVNC web assets not found" >&2; exit 69; }

DISPLAY_NUMBER=${SLICER_LINUX_RUNTIME_AUTH_DISPLAY:-}
if [[ -z "$DISPLAY_NUMBER" ]]; then
    for candidate in $(seq 77 97); do
        display_in_use=0
        if [[ -S "/tmp/.X11-unix/X$candidate" || -e "/tmp/.X$candidate-lock" ]]; then
            display_in_use=1
        elif [[ "$WINDOWS_WSL_RUNTIME" -eq 1 && -r /proc/net/unix ]] &&
             grep -Fq "@/tmp/.X11-unix/X$candidate" /proc/net/unix; then
            display_in_use=1
        fi
        if [[ "$display_in_use" -eq 0 ]]; then
            DISPLAY_NUMBER=$candidate
            break
        fi
    done
fi
[[ -n "$DISPLAY_NUMBER" ]] || { echo "no free X display" >&2; exit 70; }
DISPLAY=:$DISPLAY_NUMBER
export DISPLAY

PASS_FILE="$STATE_DIR/vnc.pass"
LOG_FILE="$STATE_DIR/session.log"
PID_FILE="$STATE_DIR/session.pids"
: >"$LOG_FILE"
x11vnc -storepasswd "$VNC_PASSWORD" "$PASS_FILE" >/dev/null

children=()
cleanup() {
    local pid
    for ((i=${#children[@]}-1; i>=0; --i)); do
        pid=${children[$i]}
        kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    rm -f "$STATE_DIR/ready"
}
trap cleanup EXIT INT TERM HUP

Xvfb "$DISPLAY" -screen 0 656x840x24 -nolisten tcp -noreset >>"$LOG_FILE" 2>&1 &
XVFB_PID=$!
children+=("$XVFB_PID")
if [[ "$WINDOWS_WSL_RUNTIME" -eq 1 ]]; then
    xvfb_ready=0
    for _ in {1..200}; do
        kill -0 "$XVFB_PID" 2>/dev/null || break
        if xdpyinfo -display "$DISPLAY" >/dev/null 2>&1; then
            xvfb_ready=1
            break
        fi
        sleep 0.1
    done
    [[ "$xvfb_ready" -eq 1 ]] || { echo "Xvfb failed to accept an X11 client connection under Windows WSL" >&2; exit 70; }
else
    for _ in {1..100}; do [[ -S "/tmp/.X11-unix/X$DISPLAY_NUMBER" ]] && break; sleep 0.1; done
    [[ -S "/tmp/.X11-unix/X$DISPLAY_NUMBER" ]] || { echo "Xvfb failed" >&2; exit 70; }
fi

if command -v openbox >/dev/null 2>&1; then
    openbox >>"$LOG_FILE" 2>&1 & children+=("$!")
fi

if [[ "$WINDOWS_WSL_RUNTIME" -eq 1 ]]; then
    env \
        -u WAYLAND_DISPLAY \
        -u WAYLAND_SOCKET \
        -u DESKTOP_SESSION \
        -u XDG_CURRENT_DESKTOP \
        XDG_SESSION_TYPE=x11 \
        x11vnc -display "$DISPLAY" -rfbport "$VNC_PORT" -localhost -forever -shared -noxdamage -noshm -nowf -noscr -rfbauth "$PASS_FILE" >>"$LOG_FILE" 2>&1 &
else
    x11vnc -display "$DISPLAY" -rfbport "$VNC_PORT" -localhost -forever -shared -noxdamage -noshm -nowf -noscr -rfbauth "$PASS_FILE" >>"$LOG_FILE" 2>&1 &
fi
X11VNC_PID=$!
children+=("$X11VNC_PID")
websockify --web "$NOVNC_WEB" "$LISTEN_HOST:$NOVNC_PORT" "127.0.0.1:$VNC_PORT" >>"$LOG_FILE" 2>&1 &
WEBSOCKIFY_PID=$!
children+=("$WEBSOCKIFY_PID")
printf '%s\n' "${children[*]}" >"$PID_FILE"

process_alive() {
    local pid=$1
    kill -0 "$pid" 2>/dev/null || return 1
    [[ ! -r "/proc/$pid/stat" ]] || [[ $(awk '{print $3}' "/proc/$pid/stat") != Z ]]
}

ready=0
for _ in {1..100}; do
    process_alive "$X11VNC_PID" || break
    process_alive "$WEBSOCKIFY_PID" || break
    vnc_ready=0
    novnc_ready=0
    if exec 8<>"/dev/tcp/127.0.0.1/$VNC_PORT" 2>/dev/null; then
        exec 8>&-
        vnc_ready=1
    fi
    if exec 9<>"/dev/tcp/$LISTEN_HOST/$NOVNC_PORT" 2>/dev/null; then
        exec 9>&-
        novnc_ready=1
    fi
    if [[ "$vnc_ready" -eq 1 && "$novnc_ready" -eq 1 ]]; then
        ready=1
        break
    fi
    sleep 0.1
done
[[ "$ready" -eq 1 ]] || { echo "Linux browser transport failed" >&2; exit 70; }

browser_args=(
    --mode "$MODE"
    --login-url "$LOGIN_URL"
    --result-file "$RESULT_FILE"
    --profile-dir "$PROFILE_DIR"
    --command-file "$COMMAND_FILE"
    --event-file "$EVENT_FILE"
    --ready-file "$STATE_DIR/ready"
    --client-version "$CLIENT_VERSION"
    --language "$LANGUAGE"
    --theme "$THEME"
    --loopback-port 0
)
if [[ "$MODE" == auth && "$LOGIN_CMD_FILE" != "-" && -f "$LOGIN_CMD_FILE" ]]; then
    browser_args+=(--login-cmd-file "$LOGIN_CMD_FILE")
fi

if command -v dbus-run-session >/dev/null 2>&1; then
    dbus-run-session -- "$BROWSER" "${browser_args[@]}" >>"$LOG_FILE" 2>&1
else
    "$BROWSER" "${browser_args[@]}" >>"$LOG_FILE" 2>&1
fi
