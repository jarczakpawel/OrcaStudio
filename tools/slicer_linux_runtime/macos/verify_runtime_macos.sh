#!/bin/bash
set -euo pipefail

export PATH="/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/bin:/bin:/usr/sbin:/sbin:${PATH:-}"

PACKAGE_DIR=""
COMPONENT_DIR=""
COMPONENT_CACHE_DIR=""
ALLOW_MISSING_COMPONENT=0
SKIP_PROBE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        -PackageDir)
            PACKAGE_DIR="${2:-}"
            shift 2
            ;;
        -ComponentDir)
            COMPONENT_DIR="${2:-}"
            shift 2
            ;;
        -ComponentCacheDir)
            COMPONENT_CACHE_DIR="${2:-}"
            shift 2
            ;;
        -AllowMissingComponent)
            ALLOW_MISSING_COMPONENT=1
            shift
            ;;
        -SkipProbe)
            SKIP_PROBE=1
            shift
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

if [[ -z "$COMPONENT_DIR" ]]; then
    COMPONENT_DIR="$PACKAGE_DIR"
fi
if [[ -z "$COMPONENT_DIR" ]]; then
    echo "ComponentDir is required" >&2
    exit 2
fi


normalize_component_cache_dir() {
    local dir="${1:-}"
    if [[ -z "$dir" ]]; then
        printf '\n'
        return 0
    fi
    if [[ -d "$dir/plugins" && ! -f "$dir/libbambu_networking.so" && ! -f "$dir/libBambuSource.so" ]]; then
        printf '%s\n' "$dir/plugins"
    else
        printf '%s\n' "$dir"
    fi
}

COMPONENT_CACHE_DIR=$(normalize_component_cache_dir "$COMPONENT_CACHE_DIR")

APP_SUPPORT_DIR="${SLICER_LINUX_RUNTIME_MAC_APP_SUPPORT_DIR:-$HOME/Library/Application Support/BambuStudio_OrcaSlicer/slicer-linux-runtime}"
RUNTIME_DIR="${SLICER_LINUX_RUNTIME_MAC_RUNTIME_DIR:-$APP_SUPPORT_DIR/runtime}"
LOG_DIR="$APP_SUPPORT_DIR/logs"
INSTALL_VERSION="SLICER-LINUX-RUNTIME-MAC-0.34-BAMBU-NETWORK-ROSETTA-SPLITLOCK-V21"
INSTALL_VERSION_FILE="$APP_SUPPORT_DIR/install_version.txt"
PROBE_MARKER_FILE="$APP_SUPPORT_DIR/component_probe_marker.txt"
mkdir -p "$APP_SUPPORT_DIR" "$LOG_DIR"

shell_quote() {
    local value="$1"
    printf "'"
    printf '%s' "$value" | sed "s/'/'\\\\''/g"
    printf "'"
}

trim_file() {
    local path="$1"
    if [[ ! -f "$path" ]]; then
        return 1
    fi
    LC_ALL=C awk 'NR == 1 { gsub(/\r/, ""); sub(/^[[:space:]]+/, ""); sub(/[[:space:]]+$/, ""); print }' "$path"
}

validate_ca_bundle() {
    local path="$1"
    local label="${2:-$path}"
    local size certs
    if [[ ! -f "$path" ]]; then
        echo "missing CA certificate bundle: $label" >&2
        return 1
    fi
    size=$(wc -c < "$path" | tr -d '[:space:]')
    certs=$(grep -c -- '-----BEGIN CERTIFICATE-----' "$path" 2>/dev/null || true)
    if [[ -z "$size" || "$size" -lt 65536 || "$certs" -lt 50 ]]; then
        echo "invalid CA certificate bundle: $label (bytes=${size:-0}, certificates=${certs:-0})" >&2
        return 1
    fi
}

find_limactl() {
    if [[ -n "${SLICER_LINUX_RUNTIME_LIMACTL:-}" && -x "${SLICER_LINUX_RUNTIME_LIMACTL}" ]]; then
        printf '%s\n' "$SLICER_LINUX_RUNTIME_LIMACTL"
        return 0
    fi
    local local_bin="$APP_SUPPORT_DIR/lima/bin/limactl"
    if [[ -x "$local_bin" ]]; then
        printf '%s\n' "$local_bin"
        return 0
    fi
    if command -v limactl >/dev/null 2>&1; then
        command -v limactl
        return 0
    fi
    for candidate in /opt/homebrew/bin/limactl /usr/local/bin/limactl; do
        if [[ -x "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

require_file() {
    local path="$1"
    local label="$2"
    if [[ ! -f "$path" ]]; then
        echo "missing required file: $label" >&2
        exit 1
    fi
}

validate_runtime_manifest() {
    local dir="$1"
    require_file "$dir/runtime-files.sha256" "$dir/runtime-files.sha256"
    if grep -Eq '^[0-9a-fA-F]{64}  \.' "$dir/runtime-files.sha256"; then
        echo "runtime manifest contains transient hidden state" >&2
        return 1
    fi
    if grep -Eq '^[0-9a-fA-F]{64}  runtime-files\.sha256$' "$dir/runtime-files.sha256"; then
        echo "runtime manifest must not contain itself" >&2
        return 1
    fi
    if ! awk '
        !/^[0-9a-fA-F]{64}  [^\/]+$/ { exit 1 }
        { count++ }
        END { if (count < 10) exit 1 }
    ' "$dir/runtime-files.sha256"; then
        echo "invalid runtime manifest: $dir/runtime-files.sha256" >&2
        return 1
    fi
    (cd "$dir" && shasum -a 256 -c runtime-files.sha256 >/dev/null)
}

compare_file() {
    local src="$1"
    local dst="$2"
    local label="$3"
    if [[ ! -f "$src" || ! -f "$dst" ]]; then
        echo "runtime payload file missing: $label" >&2
        exit 1
    fi
    if ! cmp -s "$src" "$dst"; then
        echo "runtime payload out of date: $label" >&2
        exit 1
    fi
}

atomic_copy_file() {
    local src="$1"
    local dst="$2"
    local dir base tmp
    dir=$(dirname -- "$dst")
    base=$(basename -- "$dst")
    mkdir -p "$dir"
    tmp=$(mktemp "$dir/.${base}.tmp.XXXXXX")
    if ! cp -f "$src" "$tmp"; then
        rm -f "$tmp"
        return 1
    fi
    mv -f "$tmp" "$dst"
}

compare_copied_payload() {
    local path base
    for path in "$COMPONENT_DIR"/*; do
        [[ -f "$path" ]] || continue
        base=$(basename -- "$path")
        case "$base" in
            slicer_linux_runtime_host|slicer_linux_runtime_host_abi1|slicer_linux_runtime_host_abi0|slicer_linux_auth_browser|slicer_linux_auth_browser_x86_64|slicer_linux_auth_browser_aarch64|run_auth_browser.sh|libbambu_networking.so|libBambuSource.so|linux_component_manifest.json|runtime-files.sha256|ca-certificates.crt|slicer_base64.cer|ld-linux-x86-64.so.2|lib*.so|lib*.so.*|*.so|*.so.*)
                compare_file "$path" "$RUNTIME_DIR/$base" "$base"
                ;;
        esac
    done
}

sync_payload_files_from_dir() {
    local src_dir="$1"
    local path base
    if [[ -z "$src_dir" || ! -d "$src_dir" ]]; then
        return 0
    fi
    for path in "$src_dir"/*; do
        [[ -f "$path" ]] || continue
        base=$(basename -- "$path")
        case "$base" in
            slicer_linux_runtime_host|slicer_linux_runtime_host_abi1|slicer_linux_runtime_host_abi0|slicer_linux_auth_browser|slicer_linux_auth_browser_x86_64|slicer_linux_auth_browser_aarch64|run_auth_browser.sh|libbambu_networking.so|libBambuSource.so|linux_component_manifest.json|runtime-files.sha256|ca-certificates.crt|slicer_base64.cer|ld-linux-x86-64.so.2|lib*.so|lib*.so.*|*.so|*.so.*)
                if [[ ! -f "$RUNTIME_DIR/$base" ]] || ! cmp -s "$path" "$RUNTIME_DIR/$base"; then
                    atomic_copy_file "$path" "$RUNTIME_DIR/$base"
                fi
                ;;
        esac
    done
}

sync_runtime_payload() {
    mkdir -p "$RUNTIME_DIR"
    if { [[ ! -f "$COMPONENT_DIR/libbambu_networking.so" || ! -f "$COMPONENT_DIR/libBambuSource.so" ]]; } && { [[ -z "$COMPONENT_CACHE_DIR" || ! -f "$COMPONENT_CACHE_DIR/libbambu_networking.so" || ! -f "$COMPONENT_CACHE_DIR/libBambuSource.so" ]]; }; then
        rm -f "$RUNTIME_DIR/libbambu_networking.so" "$RUNTIME_DIR/libBambuSource.so" "$RUNTIME_DIR/linux_component_manifest.json" "$PROBE_MARKER_FILE"
    fi
    sync_payload_files_from_dir "$COMPONENT_DIR"
    if [[ -n "$COMPONENT_CACHE_DIR" && "$COMPONENT_CACHE_DIR" != "$COMPONENT_DIR" ]]; then
        sync_payload_files_from_dir "$COMPONENT_CACHE_DIR"
    fi
    chmod 755 "$RUNTIME_DIR/slicer_linux_runtime_host" \
        "$RUNTIME_DIR/slicer_linux_runtime_host_abi1" \
        "$RUNTIME_DIR/slicer_linux_runtime_host_abi0" \
        "$RUNTIME_DIR/slicer_linux_auth_browser" \
        "$RUNTIME_DIR/slicer_linux_auth_browser_x86_64" \
        "$RUNTIME_DIR/slicer_linux_auth_browser_aarch64" \
        "$RUNTIME_DIR/run_auth_browser.sh" 2>/dev/null || true
    [[ ! -f "$RUNTIME_DIR/ld-linux-x86-64.so.2" ]] || chmod 755 "$RUNTIME_DIR/ld-linux-x86-64.so.2"
    chmod 755 "$RUNTIME_DIR"/*.so "$RUNTIME_DIR"/*.so.* 2>/dev/null || true
}


guest_auth_browser_path() {
    local guest_arch
    local guest_arch_output=""
    guest_arch_output=$("$LIMACTL" shell "$INSTANCE" -- uname -m)
    guest_arch=$(LC_ALL=C awk 'NR == 1 { gsub(/\r/, ""); print }' <<< "$guest_arch_output")
    case "$guest_arch" in
        aarch64|arm64) printf '%s\n' "$RUNTIME_DIR/slicer_linux_auth_browser_aarch64" ;;
        x86_64|amd64) printf '%s\n' "$RUNTIME_DIR/slicer_linux_auth_browser_x86_64" ;;
        *) echo "unsupported Lima guest architecture: ${guest_arch:-unknown}" >&2; return 1 ;;
    esac
}

probe_linux_payload() {
    local wrapper="$COMPONENT_DIR/slicer-linux-runtime-host-wrapper"
    local host="$RUNTIME_DIR/slicer_linux_runtime_host"
    if [[ ! -x "$wrapper" ]]; then
        echo "macOS runtime bridge wrapper is missing: $wrapper" >&2
        return 1
    fi

    local out=""
    local rc=0
    set +e
    out=$(printf x | \
        SLICER_LINUX_RUNTIME_REQUIRE_COMPATIBLE_HOST=1 \
        SLICER_LINUX_RUNTIME_MAC_APP_SUPPORT_DIR="$APP_SUPPORT_DIR" \
        SLICER_LINUX_RUNTIME_MAC_RUNTIME_DIR="$RUNTIME_DIR" \
        SLICER_LINUX_RUNTIME_MAC_LIMA_INSTANCE="$INSTANCE" \
            "$wrapper" "$host" "$RUNTIME_DIR" "$COMPONENT_DIR" --probe-stdio-roundtrip)
    rc=$?
    set -e
    if [[ "$rc" -ne 0 ]]; then
        echo "runtime plug-in compatibility/stdio transaction failed: rc=$rc" >&2
        return 1
    fi
    if [[ "$out" != "SLICER_RUNTIME_STDIO_OK" ]]; then
        echo "runtime plug-in load/stdio transaction failed: ${out:-<empty>}" >&2
        return 1
    fi
}

component_probe_marker_value() {
    [[ -f "$RUNTIME_DIR/libbambu_networking.so" && -f "$RUNTIME_DIR/libBambuSource.so" ]] || return 1
    local mode
    mode=$(trim_file "$APP_SUPPORT_DIR/lima_mode.txt" || true)
    {
        printf 'mode=%s\n' "$mode"
        shasum -a 256 "$RUNTIME_DIR/libbambu_networking.so" "$RUNTIME_DIR/libBambuSource.so"
        if [[ -f "$RUNTIME_DIR/linux_component_manifest.json" ]]; then
            shasum -a 256 "$RUNTIME_DIR/linux_component_manifest.json"
        fi
    } | shasum -a 256 | awk '{print $1}'
}

if [[ ! -f "$INSTALL_VERSION_FILE" || "$(trim_file "$INSTALL_VERSION_FILE" || true)" != "$INSTALL_VERSION" ]]; then
    echo "runtime version marker out of date; reinstall required" >&2
    exit 1
fi

require_file "$COMPONENT_DIR/install_runtime_macos.sh" "install_runtime_macos.sh"
require_file "$COMPONENT_DIR/verify_runtime_macos.sh" "verify_runtime_macos.sh"
require_file "$COMPONENT_DIR/slicer_linux_runtime_lima_instance.txt" "slicer_linux_runtime_lima_instance.txt"
require_file "$COMPONENT_DIR/slicer-linux-runtime-host-wrapper" "slicer-linux-runtime-host-wrapper"
require_file "$COMPONENT_DIR/libslicer_linux_runtime.dylib" "libslicer_linux_runtime.dylib"
require_file "$COMPONENT_DIR/slicer_linux_runtime_host" "slicer_linux_runtime_host"
require_file "$COMPONENT_DIR/slicer_linux_runtime_host_abi1" "slicer_linux_runtime_host_abi1"
require_file "$COMPONENT_DIR/slicer_linux_runtime_host_abi0" "slicer_linux_runtime_host_abi0"
require_file "$COMPONENT_DIR/liborcastudio_rosetta_splitlock_compat.so" "liborcastudio_rosetta_splitlock_compat.so"
require_file "$COMPONENT_DIR/slicer_linux_auth_browser" "slicer_linux_auth_browser"
require_file "$COMPONENT_DIR/slicer_linux_auth_browser_x86_64" "slicer_linux_auth_browser_x86_64"
require_file "$COMPONENT_DIR/slicer_linux_auth_browser_aarch64" "slicer_linux_auth_browser_aarch64"
require_file "$COMPONENT_DIR/run_auth_browser.sh" "run_auth_browser.sh"
require_file "$COMPONENT_DIR/ca-certificates.crt" "ca-certificates.crt"
validate_ca_bundle "$COMPONENT_DIR/ca-certificates.crt" "ca-certificates.crt"
validate_runtime_manifest "$COMPONENT_DIR"
require_file "$COMPONENT_DIR/slicer_base64.cer" "slicer_base64.cer"
require_file "$COMPONENT_DIR/ld-linux-x86-64.so.2" "ld-linux-x86-64.so.2"
require_file "$COMPONENT_DIR/libc.so.6" "libc.so.6"
require_file "$COMPONENT_DIR/libm.so.6" "libm.so.6"
require_file "$COMPONENT_DIR/libresolv.so.2" "libresolv.so.2"
require_file "$COMPONENT_DIR/libnss_dns.so.2" "libnss_dns.so.2"
require_file "$COMPONENT_DIR/libnss_files.so.2" "libnss_files.so.2"
require_file "$COMPONENT_DIR/libstdc++.so.6" "libstdc++.so.6"
require_file "$COMPONENT_DIR/libgcc_s.so.1" "libgcc_s.so.1"
require_file "$COMPONENT_DIR/libz.so.1" "libz.so.1"

sync_runtime_payload

COMPONENT_AVAILABLE=1
if [[ ! -f "$RUNTIME_DIR/libbambu_networking.so" && ! -f "$RUNTIME_DIR/libBambuSource.so" ]]; then
    if [[ "$ALLOW_MISSING_COMPONENT" -eq 1 ]]; then
        COMPONENT_AVAILABLE=0
    else
        echo "optional linux component not downloaded: libbambu_networking.so/libBambuSource.so" >&2
        exit 1
    fi
elif [[ ! -f "$RUNTIME_DIR/libbambu_networking.so" || ! -f "$RUNTIME_DIR/libBambuSource.so" ]]; then
    echo "partial optional linux component package: libbambu_networking.so and libBambuSource.so must exist together" >&2
    exit 1
fi

if [[ "$COMPONENT_AVAILABLE" -eq 1 ]]; then
    require_file "$RUNTIME_DIR/libbambu_networking.so" "runtime/libbambu_networking.so"
    require_file "$RUNTIME_DIR/libBambuSource.so" "runtime/libBambuSource.so"
fi

require_file "$RUNTIME_DIR/slicer_linux_runtime_host" "runtime/slicer_linux_runtime_host"
require_file "$RUNTIME_DIR/slicer_linux_runtime_host_abi1" "runtime/slicer_linux_runtime_host_abi1"
require_file "$RUNTIME_DIR/slicer_linux_runtime_host_abi0" "runtime/slicer_linux_runtime_host_abi0"
require_file "$RUNTIME_DIR/liborcastudio_rosetta_splitlock_compat.so" "runtime/liborcastudio_rosetta_splitlock_compat.so"
require_file "$RUNTIME_DIR/slicer_linux_auth_browser" "runtime/slicer_linux_auth_browser"
require_file "$RUNTIME_DIR/slicer_linux_auth_browser_x86_64" "runtime/slicer_linux_auth_browser_x86_64"
require_file "$RUNTIME_DIR/slicer_linux_auth_browser_aarch64" "runtime/slicer_linux_auth_browser_aarch64"
require_file "$RUNTIME_DIR/run_auth_browser.sh" "runtime/run_auth_browser.sh"
require_file "$RUNTIME_DIR/ca-certificates.crt" "runtime/ca-certificates.crt"
validate_ca_bundle "$RUNTIME_DIR/ca-certificates.crt" "runtime/ca-certificates.crt"
validate_runtime_manifest "$RUNTIME_DIR"
require_file "$RUNTIME_DIR/slicer_base64.cer" "runtime/slicer_base64.cer"
require_file "$RUNTIME_DIR/ld-linux-x86-64.so.2" "runtime/ld-linux-x86-64.so.2"
require_file "$RUNTIME_DIR/libc.so.6" "runtime/libc.so.6"
require_file "$RUNTIME_DIR/libm.so.6" "runtime/libm.so.6"
require_file "$RUNTIME_DIR/libresolv.so.2" "runtime/libresolv.so.2"
require_file "$RUNTIME_DIR/libnss_dns.so.2" "runtime/libnss_dns.so.2"
require_file "$RUNTIME_DIR/libnss_files.so.2" "runtime/libnss_files.so.2"
require_file "$RUNTIME_DIR/libstdc++.so.6" "runtime/libstdc++.so.6"
require_file "$RUNTIME_DIR/libgcc_s.so.1" "runtime/libgcc_s.so.1"
require_file "$RUNTIME_DIR/libz.so.1" "runtime/libz.so.1"

compare_copied_payload

LIMACTL=$(find_limactl || true)
if [[ -z "$LIMACTL" ]]; then
    echo "limactl not found" >&2
    exit 1
fi

INSTANCE="${SLICER_LINUX_RUNTIME_MAC_LIMA_INSTANCE:-}"
if [[ -z "$INSTANCE" ]]; then
    INSTANCE=$(trim_file "$COMPONENT_DIR/slicer_linux_runtime_lima_instance.txt" || true)
fi
if [[ -z "$INSTANCE" ]]; then
    echo "Lima instance name is not configured" >&2
    exit 1
fi

if ! "$LIMACTL" shell --workdir=/ "$INSTANCE" -- /usr/bin/env true >/dev/null 2>&1; then
    echo "Lima instance '$INSTANCE' is not ready" >&2
    exit 1
fi

dependency_check=$("$COMPONENT_DIR/install_runtime_macos.sh" -PrintGuestVerifier)
if [[ "$dependency_check" != '#!/usr/bin/env bash'* ]]; then
    echo "install_runtime_macos.sh did not emit a valid guest verifier" >&2
    exit 1
fi
if ! "$LIMACTL" shell --workdir=/ "$INSTANCE" -- /bin/bash -s -- <<< "$dependency_check"; then
    echo "Linux authentication or amd64 system-loader dependencies are missing in Lima" >&2
    exit 1
fi

if [[ "$SKIP_PROBE" -eq 0 ]]; then
    auth_browser=$(guest_auth_browser_path)
    auth_probe="unset LD_LIBRARY_PATH LD_PRELOAD; $(shell_quote "$auth_browser") --probe"
    "$LIMACTL" shell --workdir=/ "$INSTANCE" -- /bin/sh -lc "$auth_probe" >> "$LOG_DIR/auth-browser-probe.log" 2>&1
    auth_self_test="unset LD_LIBRARY_PATH LD_PRELOAD; xvfb-run -a $(shell_quote "$auth_browser") --self-test"
    "$LIMACTL" shell --workdir=/ "$INSTANCE" -- /bin/sh -lc "$auth_self_test" >> "$LOG_DIR/auth-browser-self-test.log" 2>&1
fi

if [[ "$COMPONENT_AVAILABLE" -eq 1 ]]; then
    marker=$(component_probe_marker_value || true)
    current_marker=$(trim_file "$PROBE_MARKER_FILE" || true)
    if [[ "$SKIP_PROBE" -eq 0 || -z "$marker" || "$current_marker" != "$marker" ]]; then
        if ! probe_linux_payload >> "$LOG_DIR/verify-probe.log" 2>&1; then
            echo "macOS Lima runtime probe failed" >&2
            echo "log: $LOG_DIR/verify-probe.log" >&2
            exit 1
        fi
        if [[ -n "$marker" ]]; then
            printf '%s\n' "$marker" > "$PROBE_MARKER_FILE"
        fi
    fi
elif [[ "$COMPONENT_AVAILABLE" -eq 0 ]]; then
    rm -f "$PROBE_MARKER_FILE"
    echo "optional linux component not present; Lima runtime verified without plugin probe"
fi

printf 'runtime ok\n'
