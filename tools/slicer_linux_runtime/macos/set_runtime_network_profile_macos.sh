#!/bin/bash
set -euo pipefail

PROFILE="${1:-}"
case "$PROFILE" in
    default|lan-bridged) ;;
    *)
        echo "usage: $0 default|lan-bridged" >&2
        exit 2
        ;;
esac

APP_SUPPORT_DIR="$HOME/Library/Application Support/BambuStudio_OrcaSlicer/slicer-linux-runtime"
mkdir -p "$APP_SUPPORT_DIR"
printf '%s\n' "$PROFILE" > "$APP_SUPPORT_DIR/network_profile.txt"

case "$PROFILE" in
    default)
        echo "macOS Linux runtime networking set to default. Restart OrcaStudio to apply."
        ;;
    lan-bridged)
        echo "macOS Linux runtime networking set to LAN bridged. Restart OrcaStudio to apply."
        echo "If Lima/socket_vmnet bridged networking is not configured, runtime repair may fail and cloud-only default mode can be restored with: $0 default"
        ;;
esac
