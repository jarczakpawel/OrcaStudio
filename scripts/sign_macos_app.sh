#!/bin/bash
set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 3 ]; then
    echo "Usage: $0 /path/OrcaStudio.app [identity|-] [entitlements.plist]" >&2
    exit 2
fi

app="$1"
identity="${2:--}"
entitlements="${3:-}"

if [ ! -d "$app" ]; then
    echo "ERROR: macOS app bundle not found: $app" >&2
    exit 1
fi

info_plist="$app/Contents/Info.plist"
if [ ! -f "$info_plist" ]; then
    echo "ERROR: Info.plist not found in app bundle: $info_plist" >&2
    exit 1
fi

main_executable="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$info_plist" 2>/dev/null || true)"
if [ -z "$main_executable" ]; then
    echo "ERROR: CFBundleExecutable is missing in: $info_plist" >&2
    exit 1
fi

main_binary="$app/Contents/MacOS/$main_executable"
if [ ! -f "$main_binary" ]; then
    echo "ERROR: bundle executable not found: $main_binary" >&2
    exit 1
fi

if [ "$identity" = "-" ]; then
    leaf_args=(--force --verbose --timestamp=none --sign -)
    app_args=(--force --verbose --timestamp=none --sign -)
else
    leaf_args=(--force --verbose --options runtime --timestamp --sign "$identity")
    app_args=(--force --verbose --options runtime --timestamp --sign "$identity")
fi

rm -rf "$app/Contents/_CodeSignature"

while IFS= read -r -d '' candidate; do
    [ "$candidate" = "$main_binary" ] && continue
    if file -b "$candidate" | grep -q 'Mach-O'; then
        codesign "${leaf_args[@]}" "$candidate"
    fi
done < <(find "$app/Contents" -type f -print0)

if [ -n "$entitlements" ]; then
    if [ ! -f "$entitlements" ]; then
        echo "ERROR: entitlements file not found: $entitlements" >&2
        exit 1
    fi
    app_args+=(--entitlements "$entitlements")
fi

codesign "${app_args[@]}" "$app"
codesign --verify --deep --strict --verbose=4 "$app"
