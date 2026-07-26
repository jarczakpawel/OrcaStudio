#!/usr/bin/env bash

set -euo pipefail

fail() {
    echo "Linux bundle input preflight failed: $*" >&2
    exit 1
}

for command_name in \
    Xvfb \
    bwrap \
    dbus-daemon \
    dbus-run-session \
    file \
    glib-compile-schemas \
    objdump \
    pkg-config \
    python3 \
    websockify \
    xdpyinfo \
    x11vnc \
    xdg-dbus-proxy \
    xkbcomp; do
    command -v "$command_name" >/dev/null 2>&1 || fail "missing command: $command_name"
done

for module in \
    gdk-pixbuf-2.0 \
    gio-2.0 \
    gstreamer-1.0 \
    gtk+-3.0 \
    webkit2gtk-4.1; do
    pkg-config --exists "$module" || fail "missing pkg-config module: $module"
done

webkit_api=4.1
webkit_libdir="$(pkg-config --variable=libdir webkit2gtk-$webkit_api)"
webkit_libexecdir="$(pkg-config --variable=libexecdir webkit2gtk-$webkit_api 2>/dev/null || true)"
webkit_process_dir=""
for candidate in \
    "$webkit_libdir/webkit2gtk-$webkit_api" \
    "$webkit_libexecdir/webkit2gtk-$webkit_api" \
    /usr/lib/*/webkit2gtk-$webkit_api \
    /usr/lib64/webkit2gtk-$webkit_api; do
    if [[ -d "$candidate" ]]; then
        webkit_process_dir="$candidate"
        break
    fi
done
[[ -n "$webkit_process_dir" ]] || fail "WebKitGTK process directory not found"
for process_name in WebKitNetworkProcess WebKitWebProcess; do
    [[ -x "$webkit_process_dir/$process_name" ]] || fail "missing WebKitGTK process: $webkit_process_dir/$process_name"
done

novnc_dir=""
for candidate in /usr/share/novnc /usr/share/noVNC; do
    if [[ -f "$candidate/vnc.html" ]]; then
        novnc_dir="$candidate"
        break
    fi
done
[[ -n "$novnc_dir" ]] || fail "noVNC vnc.html not found"
[[ -d /usr/share/X11/xkb ]] || fail "XKB data directory not found"

python3 - <<'PYTHON'
import importlib.util

required = ("numpy", "websockify")
missing = [name for name in required if importlib.util.find_spec(name) is None]
if missing:
    raise SystemExit("missing Python modules: " + ", ".join(missing))
PYTHON

smoke_dir="$(mktemp -d)"
cleanup() {
    if [[ -n "${xvfb_pid:-}" ]]; then
        kill "$xvfb_pid" 2>/dev/null || true
        wait "$xvfb_pid" 2>/dev/null || true
    fi
    rm -rf "$smoke_dir"
}
trap cleanup EXIT

Xvfb \
    -displayfd 3 \
    -screen 0 64x64x24 \
    -nolisten tcp \
    -ac \
    -noreset \
    3>"$smoke_dir/display" \
    >"$smoke_dir/Xvfb.log" 2>&1 &
xvfb_pid=$!

ready=0
display_number=""
for _ in $(seq 1 200); do
    kill -0 "$xvfb_pid" 2>/dev/null || break
    if [[ -s "$smoke_dir/display" ]]; then
        display_number="$(tr -d '[:space:]' < "$smoke_dir/display")"
        if [[ "$display_number" =~ ^[0-9]+$ ]] && \
            DISPLAY=":$display_number" xdpyinfo >/dev/null 2>&1; then
            ready=1
            break
        fi
    fi
    sleep 0.05
done
if [[ "$ready" -ne 1 ]]; then
    cat "$smoke_dir/Xvfb.log" >&2 || true
    fail "host Xvfb cannot accept X11 client connections"
fi

echo "Linux bundle input preflight passed"
