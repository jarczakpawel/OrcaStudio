#!/bin/sh
set -eu

ROOTFS_MARKER="${1:-ubuntu-24.04-linux-auth-v3}"

fail() {
    echo "WSL rootfs preparation failed: $*" >&2
    exit 1
}

require_command() {
    command_name="$1"
    command_path="$(command -v "$command_name" 2>/dev/null || true)"
    [ -n "$command_path" ] || fail "missing command after package installation: $command_name"
    printf '%s\n' "$command_path"
}

export DEBIAN_FRONTEND=noninteractive

echo "Installing Windows WSL Linux-auth runtime dependencies..."
apt-get update
apt-get install -y --no-install-recommends \
    ca-certificates \
    dbus-x11 \
    fonts-dejavu-core \
    fonts-liberation \
    fonts-noto-color-emoji \
    epiphany-browser \
    libegl1 \
    libgl1-mesa-dri \
    libgtk-3-0t64 \
    libwebkit2gtk-4.1-0 \
    mesa-vulkan-drivers \
    novnc \
    openbox \
    shared-mime-info \
    websockify \
    x11-utils \
    x11-xkb-utils \
    x11vnc \
    xauth \
    xdg-utils \
    xfonts-base \
    xkb-data \
    xvfb

xvfb_path="$(require_command Xvfb)"
xvfb_run_path="$(require_command xvfb-run)"
xdpyinfo_path="$(require_command xdpyinfo)"
xkbcomp_path="$(require_command xkbcomp)"
x11vnc_path="$(require_command x11vnc)"
websockify_path="$(require_command websockify)"
python_path="$(require_command python3)"
timeout_path="$(require_command timeout)"
browser_path="$(command -v epiphany 2>/dev/null || command -v epiphany-browser 2>/dev/null || true)"
[ -n "$browser_path" ] || fail "missing Epiphany browser after package installation"

novnc_path=""
for candidate in /usr/share/novnc/vnc.html /usr/share/noVNC/vnc.html; do
    if [ -f "$candidate" ]; then
        novnc_path="$candidate"
        break
    fi
done
if [ -z "$novnc_path" ]; then
    echo "Installed novnc package paths:" >&2
    dpkg-query -L novnc 2>/dev/null | grep -E '/(novnc|noVNC)/|/vnc(_lite)?\.html$' >&2 || true
    fail "noVNC vnc.html was not installed"
fi
if [ "$novnc_path" != "/usr/share/novnc/vnc.html" ]; then
    mkdir -p /usr/share/novnc
    ln -sfn "$novnc_path" /usr/share/novnc/vnc.html
    novnc_path="/usr/share/novnc/vnc.html"
fi
[ -f "$novnc_path" ] || fail "noVNC compatibility path is not readable: $novnc_path"

ca_path="/etc/ssl/certs/ca-certificates.crt"
[ -s "$ca_path" ] || fail "CA certificate bundle is missing or empty: $ca_path"
certificate_count="$(grep -c '^-----BEGIN CERTIFICATE-----' "$ca_path" 2>/dev/null || true)"
case "$certificate_count" in
    ''|*[!0-9]*) fail "could not count certificates in $ca_path" ;;
esac
[ "$certificate_count" -ge 50 ] || fail "CA certificate bundle is incomplete: certificates=$certificate_count"

xkb_data_path="/usr/share/X11/xkb/rules/evdev"
[ -f "$xkb_data_path" ] || fail "XKB data is missing: $xkb_data_path"

if ! "$python_path" -c 'import websockify' >/dev/null; then
    fail "Python cannot import websockify"
fi

# The runtime launcher waits for a real /tmp/.X11-unix socket and then connects
# x11vnc to it. Ensure the directory has the standard X11 permissions before
# validating both direct Xvfb startup and the xvfb-run wrapper.
mkdir -p /tmp/.X11-unix
chmod 1777 /tmp/.X11-unix
rm -f /tmp/.X*-lock /tmp/.X11-unix/X* 2>/dev/null || true

smoke_dir="$(mktemp -d)"
xvfb_pid=""
cleanup() {
    if [ -n "$xvfb_pid" ]; then
        kill "$xvfb_pid" 2>/dev/null || true
        wait "$xvfb_pid" 2>/dev/null || true
        xvfb_pid=""
    fi
    rm -rf "$smoke_dir"
}
trap cleanup 0 1 2 15

"$xvfb_path" \
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
i=0
while [ "$i" -lt 200 ]; do
    kill -0 "$xvfb_pid" 2>/dev/null || break
    if [ -s "$smoke_dir/display" ]; then
        display_number="$(tr -d '[:space:]' < "$smoke_dir/display")"
        case "$display_number" in
            ''|*[!0-9]*) ;;
            *)
                if DISPLAY=":$display_number" "$xdpyinfo_path" >/dev/null 2>&1; then
                    ready=1
                    break
                fi
                ;;
        esac
    fi
    i=$((i + 1))
    sleep 0.05
done

if [ "$ready" -ne 1 ]; then
    cat "$smoke_dir/Xvfb.log" >&2 || true
    fail "Xvfb did not accept an X11 client connection"
fi
if [ ! -S "/tmp/.X11-unix/X$display_number" ]; then
    cat "$smoke_dir/Xvfb.log" >&2 || true
    fail "Xvfb did not create the Unix socket required by the runtime: /tmp/.X11-unix/X$display_number"
fi

kill "$xvfb_pid" 2>/dev/null || true
wait "$xvfb_pid" 2>/dev/null || true
xvfb_pid=""
rm -f /tmp/.X"$display_number"-lock /tmp/.X11-unix/X"$display_number" 2>/dev/null || true

if ! "$timeout_path" 20 "$xvfb_run_path" -a -e /dev/stderr "$xdpyinfo_path" >/dev/null; then
    fail "xvfb-run could not start Xvfb and connect xdpyinfo"
fi

printf '%s\n' "$ROOTFS_MARKER" >/etc/orcastudio-linux-auth-runtime
cat >/etc/orcastudio-linux-auth-runtime.manifest <<EOF_MANIFEST
schema=1
marker=$ROOTFS_MARKER
ca=$ca_path
xvfb=$xvfb_path
xvfb_run=$xvfb_run_path
xdpyinfo=$xdpyinfo_path
xkbcomp=$xkbcomp_path
xkb_data=$xkb_data_path
x11vnc=$x11vnc_path
websockify=$websockify_path
python3=$python_path
browser=$browser_path
novnc=$novnc_path
EOF_MANIFEST

[ "$(cat /etc/orcastudio-linux-auth-runtime)" = "$ROOTFS_MARKER" ] || fail "runtime marker round-trip failed"
[ -s /etc/orcastudio-linux-auth-runtime.manifest ] || fail "runtime manifest was not created"

apt-get clean
rm -rf /var/lib/apt/lists/*

echo "Windows WSL Linux-auth rootfs preparation OK"
