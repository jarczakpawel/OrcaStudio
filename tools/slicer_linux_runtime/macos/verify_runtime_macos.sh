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

APP_SUPPORT_DIR="$HOME/Library/Application Support/BambuStudio_OrcaSlicer/slicer-linux-runtime"
RUNTIME_DIR="${SLICER_LINUX_RUNTIME_MAC_RUNTIME_DIR:-$APP_SUPPORT_DIR/runtime}"
LOG_DIR="$APP_SUPPORT_DIR/logs"
INSTALL_VERSION="SLICER-LINUX-RUNTIME-MAC-0.16"
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
    LC_ALL=C tr -d '\r' < "$path" | head -n 1 | sed 's/^[[:space:]]*//;s/[[:space:]]*$//'
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

compare_copied_payload() {
    local path base
    for path in "$COMPONENT_DIR"/*; do
        [[ -f "$path" ]] || continue
        base=$(basename -- "$path")
        case "$base" in
            slicer_linux_runtime_host|slicer_linux_runtime_host_abi1|slicer_linux_runtime_host_abi0|libbambu_networking.so|libBambuSource.so|linux_component_manifest.json|ca-certificates.crt|slicer_base64.cer|ld-linux-x86-64.so.2|lib*.so|lib*.so.*|*.so|*.so.*)
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
            slicer_linux_runtime_host|slicer_linux_runtime_host_abi1|slicer_linux_runtime_host_abi0|libbambu_networking.so|libBambuSource.so|linux_component_manifest.json|ca-certificates.crt|slicer_base64.cer|ld-linux-x86-64.so.2|lib*.so|lib*.so.*|*.so|*.so.*)
                if [[ ! -f "$RUNTIME_DIR/$base" ]] || ! cmp -s "$path" "$RUNTIME_DIR/$base"; then
                    cp -f "$path" "$RUNTIME_DIR/$base"
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
    chmod 755 "$RUNTIME_DIR/slicer_linux_runtime_host" "$RUNTIME_DIR/slicer_linux_runtime_host_abi1" "$RUNTIME_DIR/slicer_linux_runtime_host_abi0" 2>/dev/null || true
    [[ ! -f "$RUNTIME_DIR/ld-linux-x86-64.so.2" ]] || chmod 755 "$RUNTIME_DIR/ld-linux-x86-64.so.2"
    chmod 755 "$RUNTIME_DIR"/*.so "$RUNTIME_DIR"/*.so.* 2>/dev/null || true
}

runtime_host_env_prefix() {
    printf '%s' "export SLICER_LINUX_RUNTIME_COMPONENT_DIR=$(shell_quote "$RUNTIME_DIR"); "
    printf '%s' "export SLICER_LINUX_RUNTIME_COMPONENT_SO=$(shell_quote "$RUNTIME_DIR/libbambu_networking.so"); "
    printf '%s' "export SLICER_LINUX_RUNTIME_SOURCE_SO=$(shell_quote "$RUNTIME_DIR/libBambuSource.so"); "
    printf '%s' "export SLICER_LINUX_RUNTIME_MEDIA_SO=$(shell_quote "$RUNTIME_DIR/liblive555.so"); "
    printf '%s' "export SLICER_LINUX_RUNTIME_PROBE_LOG_DIR=$(shell_quote "$LOG_DIR"); "
    printf '%s' "export SLICER_LINUX_RUNTIME_COUNTRY_CODE=PL; "
    printf '%s' "unset LD_LIBRARY_PATH; "
    printf '%s' "if [ -f $(shell_quote "$RUNTIME_DIR/ca-certificates.crt") ]; then export SSL_CERT_FILE=$(shell_quote "$RUNTIME_DIR/ca-certificates.crt"); export CURL_CA_BUNDLE=$(shell_quote "$RUNTIME_DIR/ca-certificates.crt"); fi; "
}

probe_linux_payload() {
    local cmd
    cmd="$(runtime_host_env_prefix) exec /bin/sh $(shell_quote "$RUNTIME_DIR/slicer_linux_runtime_host") --probe-load"
    "$LIMACTL" shell "$INSTANCE" -- /bin/sh -lc "$cmd"

    local out
    cmd="$(runtime_host_env_prefix) exec /bin/sh $(shell_quote "$RUNTIME_DIR/slicer_linux_runtime_host") --probe-stdio-roundtrip"
    out=$(printf x | "$LIMACTL" shell "$INSTANCE" -- /bin/sh -lc "$cmd")
    if [[ "$out" != "SLICER_RUNTIME_STDIO_OK" ]]; then
        echo "runtime stdio roundtrip probe failed: ${out:-<empty>}" >&2
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
require_file "$COMPONENT_DIR/ca-certificates.crt" "ca-certificates.crt"
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
require_file "$RUNTIME_DIR/ca-certificates.crt" "runtime/ca-certificates.crt"
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

if ! "$LIMACTL" shell "$INSTANCE" -- /usr/bin/env true >/dev/null 2>&1; then
    echo "Lima instance '$INSTANCE' is not ready" >&2
    exit 1
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
