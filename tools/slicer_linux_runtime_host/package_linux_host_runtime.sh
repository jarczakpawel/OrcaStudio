#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PROJECT_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)"
RUNTIME_ROOT="$PROJECT_DIR/tools/slicer_linux_runtime_host/runtime/linux-x86_64"
INCLUDE_ROSETTA_SPLITLOCK_COMPAT="${SLICER_LINUX_RUNTIME_INCLUDE_ROSETTA_SPLITLOCK_COMPAT:-0}"

case "$INCLUDE_ROSETTA_SPLITLOCK_COMPAT" in
    0|1) ;;
    *)
        echo "SLICER_LINUX_RUNTIME_INCLUDE_ROSETTA_SPLITLOCK_COMPAT must be 0 or 1" >&2
        exit 2
        ;;
esac

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
    find "$PROJECT_DIR/build" "$SCRIPT_DIR/.build-linux-host" -type f -name "$name" -print -quit 2>/dev/null
}

find_rosetta_splitlock_compiler() {
    local requested="${SLICER_LINUX_RUNTIME_ROSETTA_SPLITLOCK_CC:-${CC:-}}"
    local candidate=""
    if [[ -n "$requested" ]]; then
        command -v "$requested" 2>/dev/null || {
            echo "requested Rosetta split-lock compiler not found: $requested" >&2
            return 1
        }
        return 0
    fi
    for candidate in gcc-14 clang cc; do
        if command -v "$candidate" >/dev/null 2>&1; then
            command -v "$candidate"
            return 0
        fi
    done
    echo "no compatible compiler found for Rosetta split-lock shim" >&2
    return 1
}

build_rosetta_splitlock_shim() {
    local output="$SCRIPT_DIR/.build-linux-host/liborcastudio_rosetta_splitlock_compat.so"
    local source="$SCRIPT_DIR/rosetta_splitlock_compat.c"
    local compiler=""
    compiler="$(find_rosetta_splitlock_compiler)"
    mkdir -p "$(dirname "$output")"
    echo "Building Rosetta split-lock shim with: $compiler" >&2
    "$compiler" -std=c11 -O2 -fPIC -shared \
        -Wall -Wextra -Werror -fvisibility=hidden \
        -Wl,-z,relro,-z,now -Wl,--no-undefined \
        -o "$output" "$source"
    printf '%s\n' "$output"
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


validate_ca_bundle() {
    local path="$1"
    [[ -f "$path" ]] || return 1
    [[ $(wc -c <"$path") -ge 65536 ]] || return 1
    [[ $(grep -c -- '-----BEGIN CERTIFICATE-----' "$path" 2>/dev/null || true) -ge 50 ]] || return 1
    openssl crl2pkcs7 -nocrl -certfile "$path" 2>/dev/null |
        openssl pkcs7 -print_certs -noout >/dev/null 2>&1
}

validate_vendor_certificate_file() {
    local path="$1"
    [[ -s "$path" ]] || return 1
    [[ $(grep -c -- '-----BEGIN CERTIFICATE-----' "$path" 2>/dev/null || true) -ge 1 ]] || return 1
    openssl crl2pkcs7 -nocrl -certfile "$path" 2>/dev/null |
        openssl pkcs7 -print_certs -noout >/dev/null 2>&1
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

require_aarch64_elf() {
    local path="$1"
    local label="$2"
    if ! LC_ALL=C file "$path" | grep -Eq 'ELF 64-bit.*(ARM aarch64|aarch64)'; then
        echo "$label is not an aarch64 ELF binary: $path" >&2
        LC_ALL=C file "$path" >&2 || true
        exit 1
    fi
}

find_system_lib() {
    local name="$1"
    local path=""
    for path in \
        "/lib64/$name" \
        "/lib/x86_64-linux-gnu/$name" \
        "/usr/lib64/$name" \
        "/usr/lib/x86_64-linux-gnu/$name" \
        "/usr/lib/x86_64-linux-gnu/pulseaudio/$name"
    do
        if [[ -f "$path" ]]; then
            printf '%s\n' "$path"
            return 0
        fi
    done
    return 1
}

copy_x86_library_closure() {
    local required="$1"
    shift
    local seed name path dep
    declare -A copied=()

    for name in "$@"; do
        path=$(find_system_lib "$name" || true)
        if [[ -z "$path" ]]; then
            if [[ "$required" == "required" ]]; then
                echo "failed to locate required x86_64 runtime library: $name" >&2
                exit 1
            fi
            continue
        fi

        while IFS= read -r dep; do
            [[ -n "$dep" && -f "$dep" ]] || continue
            seed=$(basename -- "$dep")
            case "$seed" in
                linux-vdso.so.*) continue ;;
            esac
            if [[ -z "${copied[$seed]:-}" ]]; then
                # Core loader/libc/curl libraries were selected earlier from the
                # host build closure. Optional Agora support may only fill a
                # missing SONAME, never replace that coherent runtime set.
                if [[ ! -f "$RUNTIME_ROOT/$seed" ]]; then
                    cp -Lf "$dep" "$RUNTIME_ROOT/$seed"
                fi
                copied[$seed]=1
            fi
        done < <({
            printf '%s\n' "$path"
            LD_LIBRARY_PATH="$RUNTIME_ROOT:${LD_LIBRARY_PATH:-}" ldd "$path" 2>/dev/null | awk '
                /=>/ && $3 ~ /^\// { print $3 }
                /^\// { print $1 }
            '
        } | sort -u)
    done
}

copy_x86_plugin_support_libs() {
    # The real Bambu package contains an x86_64 Agora camera library. In a
    # native ARM64 Lima guest Rosetta translates that code, but the guest's ARM
    # libraries cannot satisfy its x86_64 DT_NEEDED or optional audio dlopen()
    # requests. Bundle a closed x86_64 userspace set next to the plug-in.
    copy_x86_library_closure required \
        libXfixes.so.3 \
        libX11.so.6 \
        libXext.so.6 \
        libdrm.so.2 \
        libXdamage.so.1 \
        libxcb.so.1 \
        libXau.so.6 \
        libXdmcp.so.6 \
        librt.so.1

    copy_x86_library_closure optional \
        libasound.so.2 \
        libpulse.so.0
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
AUTH_BROWSER="$(find_host_bin slicer_linux_auth_browser || true)"
AUTH_BROWSER_AARCH64="${SLICER_LINUX_AUTH_BROWSER_AARCH64:-}"
ROSETTA_SPLITLOCK_SHIM=""
if [[ "$INCLUDE_ROSETTA_SPLITLOCK_COMPAT" == "1" ]]; then
    ROSETTA_SPLITLOCK_SHIM="$(build_rosetta_splitlock_shim)"
fi

if [[ -z "$HOST_ABI1" || ! -f "$HOST_ABI1" || -z "$HOST_ABI0" || ! -f "$HOST_ABI0" || -z "$AUTH_BROWSER" || ! -f "$AUTH_BROWSER" ]]; then
    echo "failed to find built slicer_linux_runtime_host_abi1/abi0 under $PROJECT_DIR/build or $SCRIPT_DIR/.build-linux-host" >&2
    echo "build them first in the full Orca Linux build context, for example:" >&2
    echo "  cmake --build build --config Release --target slicer_linux_runtime_host" >&2
    exit 1
fi

rm -rf "$RUNTIME_ROOT"
mkdir -p "$RUNTIME_ROOT"

require_x86_64_elf "$HOST_ABI1" "slicer_linux_runtime_host_abi1"
require_x86_64_elf "$HOST_ABI0" "slicer_linux_runtime_host_abi0"
require_x86_64_elf "$AUTH_BROWSER" "slicer_linux_auth_browser"
if [[ -n "$ROSETTA_SPLITLOCK_SHIM" ]]; then
    require_x86_64_elf "$ROSETTA_SPLITLOCK_SHIM" "liborcastudio_rosetta_splitlock_compat.so"
fi
"$AUTH_BROWSER" --probe >/dev/null
if ! command -v xvfb-run >/dev/null 2>&1; then
    echo "xvfb-run is required to validate the Linux authentication browser" >&2
    exit 1
fi
xvfb-run -a "$AUTH_BROWSER" --self-test >/dev/null

if [[ -n "$AUTH_BROWSER_AARCH64" ]]; then
    [[ -f "$AUTH_BROWSER_AARCH64" ]] || {
        echo "SLICER_LINUX_AUTH_BROWSER_AARCH64 does not point to a file: $AUTH_BROWSER_AARCH64" >&2
        exit 1
    }
    require_aarch64_elf "$AUTH_BROWSER_AARCH64" "slicer_linux_auth_browser_aarch64"
fi

cp -f "$PROJECT_DIR/tools/slicer_linux_runtime/wsl/slicer_linux_runtime_host" "$RUNTIME_ROOT/slicer_linux_runtime_host"
cp -f "$HOST_ABI1" "$RUNTIME_ROOT/slicer_linux_runtime_host_abi1"
cp -f "$HOST_ABI0" "$RUNTIME_ROOT/slicer_linux_runtime_host_abi0"
if [[ -n "$ROSETTA_SPLITLOCK_SHIM" ]]; then
    cp -f "$ROSETTA_SPLITLOCK_SHIM" "$RUNTIME_ROOT/liborcastudio_rosetta_splitlock_compat.so"
fi
# Keep the legacy name for Windows/WSL and older launchers, and add an explicit
# architecture name for deterministic selection inside Lima.
cp -f "$AUTH_BROWSER" "$RUNTIME_ROOT/slicer_linux_auth_browser"
cp -f "$AUTH_BROWSER" "$RUNTIME_ROOT/slicer_linux_auth_browser_x86_64"
if [[ -n "$AUTH_BROWSER_AARCH64" ]]; then
    cp -f "$AUTH_BROWSER_AARCH64" "$RUNTIME_ROOT/slicer_linux_auth_browser_aarch64"
fi
cp -f "$PROJECT_DIR/tools/slicer_linux_auth_helper/run_auth_browser.sh" "$RUNTIME_ROOT/run_auth_browser.sh"
chmod +x "$RUNTIME_ROOT/slicer_linux_runtime_host"     "$RUNTIME_ROOT/slicer_linux_runtime_host_abi1"     "$RUNTIME_ROOT/slicer_linux_runtime_host_abi0"     "$RUNTIME_ROOT/slicer_linux_auth_browser"     "$RUNTIME_ROOT/slicer_linux_auth_browser_x86_64"     "$RUNTIME_ROOT/run_auth_browser.sh"
if [[ -f "$RUNTIME_ROOT/slicer_linux_auth_browser_aarch64" ]]; then
    chmod +x "$RUNTIME_ROOT/slicer_linux_auth_browser_aarch64"
fi
if [[ -f "$RUNTIME_ROOT/liborcastudio_rosetta_splitlock_compat.so" ]]; then
    chmod 755 "$RUNTIME_ROOT/liborcastudio_rosetta_splitlock_compat.so"
fi

# Always package the current runner trust store. The dependency curl build has
# no compiled-in CA path, so the runtime must carry a deterministic bundle.
for ca_bundle in \
    "/etc/ssl/certs/ca-certificates.crt" \
    "/etc/pki/tls/certs/ca-bundle.crt" \
    "/etc/ssl/ca-bundle.pem"; do
    if [[ -f "$ca_bundle" ]]; then
        cp -Lf "$ca_bundle" "$RUNTIME_ROOT/ca-certificates.crt"
        break
    fi
done
if [[ ! -f "$RUNTIME_ROOT/ca-certificates.crt" ]]; then
    for ca_bundle in \
        "$PROJECT_DIR/cert/ca-certificates.crt" \
        "$PROJECT_DIR/resources/cert/ca-certificates.crt"; do
        if [[ -f "$ca_bundle" ]]; then
            cp -f "$ca_bundle" "$RUNTIME_ROOT/ca-certificates.crt"
            break
        fi
    done
fi

for vendor_cert in \
    "$PROJECT_DIR/cert/slicer_base64.cer" \
    "$PROJECT_DIR/resources/cert/slicer_base64.cer"; do
    if [[ -f "$vendor_cert" ]]; then
        cp -f "$vendor_cert" "$RUNTIME_ROOT/slicer_base64.cer"
        break
    fi
done

if ! validate_ca_bundle "$RUNTIME_ROOT/ca-certificates.crt"; then
    echo "failed to package a valid CA bundle: ca-certificates.crt" >&2
    exit 1
fi
if ! validate_vendor_certificate_file "$RUNTIME_ROOT/slicer_base64.cer"; then
    echo "failed to package a parseable vendor certificate file: slicer_base64.cer" >&2
    exit 1
fi

copy_runtime_libs "$HOST_ABI1" "$HOST_ABI0"
copy_x86_plugin_support_libs

# Exercise the coherent guest-system loader path first. Apple Rosetta is a
# binfmt handler and requires a normal amd64 loader/library hierarchy inside the
# guest. Windows WSL also owns a complete x86_64 userspace. The packaged private
# loader remains a compatibility fallback, but it is no longer the primary path.
for packaged_host in slicer_linux_runtime_host_abi1 slicer_linux_runtime_host_abi0; do
    SLICER_LINUX_RUNTIME_COMPONENT_DIR="$RUNTIME_ROOT" \
    SLICER_LINUX_RUNTIME_CA_BUNDLE="$RUNTIME_ROOT/ca-certificates.crt" \
    LD_LIBRARY_PATH="/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu:$RUNTIME_ROOT" \
        "$RUNTIME_ROOT/$packaged_host" --probe-runtime
done

if [[ -f "$RUNTIME_ROOT/liborcastudio_rosetta_splitlock_compat.so" ]]; then
    SLICER_LINUX_RUNTIME_ROSETTA_SPLITLOCK_COMPAT=1 \
    LD_PRELOAD="$RUNTIME_ROOT/liborcastudio_rosetta_splitlock_compat.so" \
    SLICER_LINUX_RUNTIME_COMPONENT_DIR="$RUNTIME_ROOT" \
    SLICER_LINUX_RUNTIME_CA_BUNDLE="$RUNTIME_ROOT/ca-certificates.crt" \
    LD_LIBRARY_PATH="/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu:$RUNTIME_ROOT" \
        "$RUNTIME_ROOT/slicer_linux_runtime_host_abi1" --probe-runtime
fi

# Also keep the isolated private-loader fallback buildable for older WSL images.
SLICER_LINUX_RUNTIME_COMPONENT_DIR="$RUNTIME_ROOT" \
SLICER_LINUX_RUNTIME_CA_BUNDLE="$RUNTIME_ROOT/ca-certificates.crt" \
"$RUNTIME_ROOT/ld-linux-x86-64.so.2" \
    --library-path "$RUNTIME_ROOT" \
    "$RUNTIME_ROOT/slicer_linux_runtime_host_abi1" --probe-runtime

# Validate the production dispatcher in system-loader mode with ABI0 disabled.
# A stale ABI cache may never bypass the current component probe.
sh -n "$RUNTIME_ROOT/slicer_linux_runtime_host"
rm -f "$RUNTIME_ROOT/.selected_host_abi"
selected_host="$(
    SLICER_LINUX_RUNTIME_ALLOW_COMPONENTLESS=1 \
    SLICER_LINUX_RUNTIME_PREFER_SYSTEM_LOADER=1 \
    SLICER_LINUX_RUNTIME_DISABLE_ABI0_FALLBACK=1 \
    "$RUNTIME_ROOT/slicer_linux_runtime_host" --print-bin
)"
case "$selected_host" in
    "$RUNTIME_ROOT/slicer_linux_runtime_host_abi1") ;;
    *)
        echo "runtime dispatcher selected an unexpected host: $selected_host" >&2
        exit 1
        ;;
esac
test -x "$selected_host"

# Exercise the actual WSL bootstrap control flow in componentless probe mode.
# This verifies argument/port parsing, runtime staging, dispatcher selection and
# the final probe result instead of asserting stale literal strings in PowerShell.
WSL_BOOTSTRAP="$PROJECT_DIR/tools/slicer_linux_runtime/wsl/slicer_linux_runtime_wsl_run_host.sh"
sh -n "$WSL_BOOTSTRAP"
bootstrap_probe_home="$(mktemp -d)"
bootstrap_probe_output="$(
    HOME="$bootstrap_probe_home" \
    SLICER_LINUX_RUNTIME_ALLOW_COMPONENTLESS=1 \
    sh "$WSL_BOOTSTRAP" --probe "$RUNTIME_ROOT" "" 5901 5901 5902
)"
rm -rf "$bootstrap_probe_home"
case "$bootstrap_probe_output" in
    probe_ok*) ;;
    *)
        echo "WSL bootstrap probe returned an unexpected result: $bootstrap_probe_output" >&2
        exit 1
        ;;
esac

# The dispatcher probes above intentionally exercise ABI selection and therefore
# create .selected_host_abi. That file is mutable machine-local state, not package
# content. Shipping it (or merely listing it in runtime-files.sha256) makes the
# macOS package inconsistent because later bundle stages copy the manifest but
# correctly omit hidden cache files. It is also unsafe on Windows because a build
# machine's ABI choice must never be pre-seeded on an end-user system.
rm -f "$RUNTIME_ROOT/.selected_host_abi"

unexpected_hidden="$(find "$RUNTIME_ROOT" -maxdepth 1 -type f -name '.*' -print -quit)"
if [[ -n "$unexpected_hidden" ]]; then
    echo "refusing to package transient hidden runtime file: $unexpected_hidden" >&2
    exit 1
fi

(
    cd "$RUNTIME_ROOT"
    find . -maxdepth 1 -type f \
        ! -name '.*' \
        ! -name 'runtime-files.sha256' \
        -printf '%P\0' |
        sort -z | xargs -0 -r sha256sum > runtime-files.sha256

    if [[ ! -s runtime-files.sha256 ]]; then
        echo "runtime manifest is empty" >&2
        exit 1
    fi
    if grep -Eq '^[0-9a-fA-F]{64}  \.' runtime-files.sha256; then
        echo "runtime manifest contains a hidden/transient file" >&2
        exit 1
    fi
    if grep -Eq '^[0-9a-fA-F]{64}  runtime-files\.sha256$' runtime-files.sha256; then
        echo "runtime manifest must not contain itself" >&2
        exit 1
    fi

    sha256sum -c runtime-files.sha256 >/dev/null
)

test ! -e "$RUNTIME_ROOT/.selected_host_abi"
touch "$RUNTIME_ROOT/.runtime_complete"

echo "linux host runtime packaged into:"
echo "  $RUNTIME_ROOT"
find "$RUNTIME_ROOT" -maxdepth 1 -type f | sort
