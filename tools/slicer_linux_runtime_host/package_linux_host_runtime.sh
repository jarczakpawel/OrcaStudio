#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PROJECT_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)"
RUNTIME_ROOT="$PROJECT_DIR/tools/slicer_linux_runtime_host/runtime/linux-x86_64"

find_host_bin() {
    local name="$1"
    local candidate=""
    for candidate in \
        "$PROJECT_DIR/build/src/Release/$name" \
        "$PROJECT_DIR/build/$name" \
        "$PROJECT_DIR/build/src/$name" \
        "$SCRIPT_DIR/.build-linux-host/$name"
    do
        if [[ -f "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    find "$PROJECT_DIR/build" "$SCRIPT_DIR/.build-linux-host" -type f -name "$name" 2>/dev/null | head -n 1
}

collect_runtime_libs() {
    local file="$1"
    [[ -n "$file" && -f "$file" ]] || return 0
    LD_LIBRARY_PATH="$RUNTIME_ROOT:${LD_LIBRARY_PATH:-}" ldd "$file" | awk '
        /=>/ && $3 ~ /^\// { print $3 }
        /^\// { print $1 }
    ' | sort -u
}

copy_if_exists_from_system() {
    local name="$1"
    local path=""
    for path in \
        "/lib64/$name" \
        "/lib/x86_64-linux-gnu/$name" \
        "/usr/lib64/$name" \
        "/usr/lib/x86_64-linux-gnu/$name"
    do
        if [[ -f "$path" ]]; then
            cp -Lf "$path" "$RUNTIME_ROOT/$name"
            return 0
        fi
    done
    return 1
}

copy_required_system_lib() {
    local name="$1"
    copy_if_exists_from_system "$name" || {
        echo "failed to copy required runtime library: $name" >&2
        exit 1
    }
}

require_x86_64_elf() {
    local path="$1"
    local label="$2"
    if ! LC_ALL=C file "$path" | grep -Eq 'ELF 64-bit.*x86-64|ELF 64-bit.*x86_64'; then
        echo "$label is not an x86_64 ELF binary: $path" >&2
        LC_ALL=C file "$path" >&2 || true
        exit 1
    fi
}

copy_runtime_libs() {
    local host_abi1="$1"
    local host_abi0="$2"
    mkdir -p "$RUNTIME_ROOT"

    mapfile -t libs < <({
        collect_runtime_libs "$host_abi1"
        collect_runtime_libs "$host_abi0"
    } | sort -u)

    local lib base
    for lib in "${libs[@]}"; do
        base="$(basename -- "$lib")"
        case "$base" in
            linux-vdso.so.*)
                continue
                ;;
        esac
        cp -Lf "$lib" "$RUNTIME_ROOT/"
    done

    local required_runtime_lib
    for required_runtime_lib in \
        ld-linux-x86-64.so.2 \
        libc.so.6 \
        libm.so.6 \
        libdl.so.2 \
        libpthread.so.0 \
        libresolv.so.2 \
        libnss_dns.so.2 \
        libnss_files.so.2 \
        libstdc++.so.6 \
        libgcc_s.so.1 \
        libz.so.1
    do
        copy_required_system_lib "$required_runtime_lib"
    done

    local optional_runtime_lib
    for optional_runtime_lib in \
        libnss_compat.so.2 \
        libanl.so.1 \
        libssl.so.3 \
        libcrypto.so.3 \
        libzstd.so.1
    do
        copy_if_exists_from_system "$optional_runtime_lib" || true
    done
}

if [[ "$(uname -m)" != "x86_64" ]]; then
    echo "this packaging script currently produces linux-x86_64 runtime only" >&2
    exit 1
fi

HOST_ABI1="$(find_host_bin slicer_linux_runtime_host_abi1 || true)"
HOST_ABI0="$(find_host_bin slicer_linux_runtime_host_abi0 || true)"

if [[ -z "$HOST_ABI1" || ! -f "$HOST_ABI1" || -z "$HOST_ABI0" || ! -f "$HOST_ABI0" ]]; then
    echo "failed to find built slicer_linux_runtime_host_abi1/abi0 under $PROJECT_DIR/build or $SCRIPT_DIR/.build-linux-host" >&2
    echo "build them first in the full Orca Linux build context, for example:" >&2
    echo "  cmake --build build --config Release --target slicer_linux_runtime_host" >&2
    exit 1
fi

rm -rf "$RUNTIME_ROOT"
mkdir -p "$RUNTIME_ROOT"

require_x86_64_elf "$HOST_ABI1" "slicer_linux_runtime_host_abi1"
require_x86_64_elf "$HOST_ABI0" "slicer_linux_runtime_host_abi0"
cp -f "$PROJECT_DIR/tools/slicer_linux_runtime/wsl/slicer_linux_runtime_host" "$RUNTIME_ROOT/slicer_linux_runtime_host"
cp -f "$HOST_ABI1" "$RUNTIME_ROOT/slicer_linux_runtime_host_abi1"
cp -f "$HOST_ABI0" "$RUNTIME_ROOT/slicer_linux_runtime_host_abi0"
chmod +x "$RUNTIME_ROOT/slicer_linux_runtime_host" "$RUNTIME_ROOT/slicer_linux_runtime_host_abi1" "$RUNTIME_ROOT/slicer_linux_runtime_host_abi0"

for extra in \
    "$PROJECT_DIR/cert/ca-certificates.crt" \
    "$PROJECT_DIR/cert/slicer_base64.cer" \
    "$PROJECT_DIR/resources/cert/ca-certificates.crt" \
    "$PROJECT_DIR/resources/cert/slicer_base64.cer"; do
    if [[ -f "$extra" ]]; then
        cp -f "$extra" "$RUNTIME_ROOT/$(basename -- "$extra")"
    fi
done

if [[ ! -f "$RUNTIME_ROOT/ca-certificates.crt" ]]; then
    for ca_bundle in \
        "/etc/ssl/certs/ca-certificates.crt" \
        "/etc/pki/tls/certs/ca-bundle.crt" \
        "/etc/ssl/ca-bundle.pem"; do
        if [[ -f "$ca_bundle" ]]; then
            cp -Lf "$ca_bundle" "$RUNTIME_ROOT/ca-certificates.crt"
            break
        fi
    done
fi

if [[ ! -f "$RUNTIME_ROOT/ca-certificates.crt" ]]; then
    echo "failed to copy required CA bundle: ca-certificates.crt" >&2
    exit 1
fi
if [[ ! -f "$RUNTIME_ROOT/slicer_base64.cer" ]]; then
    echo "failed to copy required certificate: slicer_base64.cer" >&2
    exit 1
fi

copy_runtime_libs "$HOST_ABI1" "$HOST_ABI0"

touch "$RUNTIME_ROOT/.runtime_complete"

echo "linux host runtime packaged into:"
echo "  $RUNTIME_ROOT"
find "$RUNTIME_ROOT" -maxdepth 1 -type f | sort
