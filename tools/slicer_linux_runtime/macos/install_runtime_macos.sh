#!/bin/bash
set -euo pipefail

export PATH="/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/bin:/bin:/usr/sbin:/sbin:${PATH:-}"

PACKAGE_DIR=""
COMPONENT_DIR=""
COMPONENT_CACHE_DIR=""
REPLACE_EXISTING=0

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
        -ReplaceExisting)
            REPLACE_EXISTING=1
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
LOCAL_LIMA_ROOT="$APP_SUPPORT_DIR/lima"
LOCAL_LIMA_BIN="$LOCAL_LIMA_ROOT/bin"
RUNTIME_DIR="${SLICER_LINUX_RUNTIME_MAC_RUNTIME_DIR:-$APP_SUPPORT_DIR/runtime}"
LOG_DIR="$APP_SUPPORT_DIR/logs"
INSTALL_VERSION="SLICER-LINUX-RUNTIME-MAC-0.16"
INSTALL_VERSION_FILE="$APP_SUPPORT_DIR/install_version.txt"
PROBE_MARKER_FILE="$APP_SUPPORT_DIR/component_probe_marker.txt"
mkdir -p "$APP_SUPPORT_DIR" "$LOCAL_LIMA_ROOT" "$RUNTIME_DIR" "$LOG_DIR"

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

find_system_limactl() {
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

find_brew() {
    if command -v brew >/dev/null 2>&1; then
        command -v brew
        return 0
    fi
    for candidate in /opt/homebrew/bin/brew /usr/local/bin/brew; do
        if [[ -x "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

find_qemu_system_x86_64() {
    local candidate prefix brew_bin
    if command -v qemu-system-x86_64 >/dev/null 2>&1; then
        command -v qemu-system-x86_64
        return 0
    fi
    for candidate in \
        /opt/homebrew/bin/qemu-system-x86_64 \
        /usr/local/bin/qemu-system-x86_64 \
        /opt/homebrew/opt/qemu/bin/qemu-system-x86_64 \
        /usr/local/opt/qemu/bin/qemu-system-x86_64
    do
        if [[ -x "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    brew_bin=$(find_brew || true)
    if [[ -n "$brew_bin" ]]; then
        for prefix in "$($brew_bin --prefix qemu 2>/dev/null || true)" "$($brew_bin --prefix 2>/dev/null || true)"; do
            [[ -n "$prefix" ]] || continue
            for candidate in "$prefix/bin/qemu-system-x86_64" "$prefix/opt/qemu/bin/qemu-system-x86_64"; do
                if [[ -x "$candidate" ]]; then
                    printf '%s\n' "$candidate"
                    return 0
                fi
            done
        done
    fi
    return 1
}

find_limactl() {
    if [[ -n "${SLICER_LINUX_RUNTIME_LIMACTL:-}" && -x "${SLICER_LINUX_RUNTIME_LIMACTL}" ]]; then
        printf '%s\n' "$SLICER_LINUX_RUNTIME_LIMACTL"
        return 0
    fi
    if [[ -x "$LOCAL_LIMA_BIN/limactl" ]]; then
        printf '%s\n' "$LOCAL_LIMA_BIN/limactl"
        return 0
    fi
    find_system_limactl
}

limactl_version_text() {
    local limactl_bin="$1"
    "$limactl_bin" --version 2>/dev/null || true
}

limactl_version_matches() {
    local limactl_bin="$1"
    local version="$2"
    local wanted="${version#v}"
    limactl_version_text "$limactl_bin" | grep -Eq "(^|[^0-9])${wanted//./\.}([^0-9]|$)"
}

limactl_supports_required_mode() {
    local limactl_bin="$1"
    local help
    help=$("$limactl_bin" start --help 2>&1 || true)
    printf '%s\n' "$help" | grep -q -- '--vm-type' || return 1
    printf '%s\n' "$help" | grep -q -- '--arch' || return 1
    printf '%s\n' "$help" | grep -q -- '--containerd' || return 1
    printf '%s\n' "$help" | grep -q -- '--mount-type' || return 1
    if [[ "${LIMA_MODE:-}" == "vz-aarch64-rosetta" ]]; then
        printf '%s\n' "$help" | grep -q -- '--rosetta' || return 1
    fi
}

resolve_lima_version_from_redirect() {
    local effective_url=""
    effective_url=$(curl -fsSL -o /dev/null -w '%{url_effective}' https://github.com/lima-vm/lima/releases/latest || true)
    case "$effective_url" in
        */tag/*)
            printf '%s\n' "${effective_url##*/}"
            return 0
            ;;
    esac
    return 1
}

resolve_lima_version() {
    if [[ -n "${SLICER_LINUX_RUNTIME_LIMA_VERSION:-}" ]]; then
        printf '%s\n' "$SLICER_LINUX_RUNTIME_LIMA_VERSION"
        return 0
    fi

    if [[ "${SLICER_LINUX_RUNTIME_LIMA_USE_LATEST:-}" == "1" ]]; then
        local version=""
        version=$(curl -fsSL https://api.github.com/repos/lima-vm/lima/releases/latest | awk -F'"' '/"tag_name"[[:space:]]*:/ { print $4; exit }' || true)
        if [[ -n "$version" ]]; then
            printf '%s\n' "$version"
            return 0
        fi
        resolve_lima_version_from_redirect
        return $?
    fi

    printf '%s\n' "v2.1.2"
}

expected_lima_sha256() {
    local archive="$1"
    case "$archive" in
        lima-2.1.2-Darwin-arm64.tar.gz) printf '%s\n' '7081d03d01511f20c4a3b38d8120428ef1c66e4b21ec9b54017bc65da60b031f' ;;
        lima-2.1.2-Darwin-x86_64.tar.gz) printf '%s\n' '3dc5218c7b0cc14126fb6e3ae6f174f026660e4e2cdffcb34b16e5a2f415eb45' ;;
        lima-additional-guestagents-2.1.2-Darwin-arm64.tar.gz) printf '%s\n' '66032828bdae79221ea7e515e340145059c592210f81954110e33ed91edf9652' ;;
        lima-additional-guestagents-2.1.2-Darwin-x86_64.tar.gz) printf '%s\n' '0936fd4523b994b9da857a0f458f286fcccc887b2afe79b2af34f15e3b9e0296' ;;
        *) return 1 ;;
    esac
}

verify_archive_sha256() {
    local path="$1"
    local archive expected actual
    archive=$(basename -- "$path")
    expected=$(expected_lima_sha256 "$archive" || true)
    if [[ -z "$expected" ]]; then
        return 0
    fi
    actual=$(shasum -a 256 "$path" | awk '{print $1}')
    if [[ "$actual" != "$expected" ]]; then
        echo "sha256 mismatch for $archive" >&2
        echo "expected: $expected" >&2
        echo "actual:   $actual" >&2
        return 1
    fi
}

install_lima_binary_locally() {
    local version
    version=$(resolve_lima_version)
    if [[ -z "$version" ]]; then
        echo "failed to resolve latest Lima version from GitHub API" >&2
        return 1
    fi

    local host_arch
    host_arch=$(uname -m)
    case "$host_arch" in
        arm64|aarch64)
            host_arch=arm64
            ;;
        x86_64|amd64)
            host_arch=x86_64
            ;;
        *)
            echo "unsupported macOS architecture for Lima: $host_arch" >&2
            return 1
            ;;
    esac

    local version_no_v="${version#v}"
    if [[ -x "$LOCAL_LIMA_BIN/limactl" ]] && limactl_version_matches "$LOCAL_LIMA_BIN/limactl" "$version"; then
        if [[ "${LIMA_MODE:-}" != "qemu-x86_64" || "$(uname -m)" != "arm64" || -f "$LOCAL_LIMA_ROOT/share/lima/lima-guestagent.Linux-x86_64.gz" ]]; then
            return 0
        fi
    fi

    local base_url="https://github.com/lima-vm/lima/releases/download/${version}"
    local main_archive="lima-${version_no_v}-Darwin-${host_arch}.tar.gz"
    local guest_archive="lima-additional-guestagents-${version_no_v}-Darwin-${host_arch}.tar.gz"
    local tmpdir rc
    tmpdir=$(mktemp -d)
    rc=0

    curl -fL --retry 3 --retry-delay 2 "$base_url/$main_archive" -o "$tmpdir/$main_archive" || rc=$?
    if [[ "$rc" -eq 0 ]]; then
        verify_archive_sha256 "$tmpdir/$main_archive" || rc=$?
    fi
    if [[ "$rc" -eq 0 ]]; then
        rm -rf "$LOCAL_LIMA_ROOT/bin" "$LOCAL_LIMA_ROOT/share"
        mkdir -p "$LOCAL_LIMA_ROOT"
        tar -xzf "$tmpdir/$main_archive" -C "$LOCAL_LIMA_ROOT" || rc=$?
    fi
    if [[ "$rc" -eq 0 ]]; then
        if curl -fL --retry 3 --retry-delay 2 "$base_url/$guest_archive" -o "$tmpdir/$guest_archive"; then
            verify_archive_sha256 "$tmpdir/$guest_archive" || rc=$?
            if [[ "$rc" -eq 0 ]]; then
                tar -xzf "$tmpdir/$guest_archive" -C "$LOCAL_LIMA_ROOT" || rc=$?
            fi
        elif [[ "${LIMA_MODE:-}" == "qemu-x86_64" && "$(uname -m)" == "arm64" ]]; then
            rc=1
        fi
    fi

    rm -rf "$tmpdir"
    [[ "$rc" -eq 0 && -x "$LOCAL_LIMA_BIN/limactl" ]] || return 1
    limactl_supports_required_mode "$LOCAL_LIMA_BIN/limactl"
}

macos_major() {
    sw_vers -productVersion | awk -F. '{print $1}'
}

portable_x86_loader_available() {
    [[ -f "$RUNTIME_DIR/ld-linux-x86-64.so.2" && -f "$RUNTIME_DIR/libc.so.6" ]]
}

select_lima_mode() {
    local major host_arch
    major=$(macos_major)
    host_arch=$(uname -m)
    LIMA_MODE=""
    LIMA_CREATE_ARGS=(start "--name=${INSTANCE}" --tty=false --mount-writable --containerd=none)

    if [[ "$host_arch" == "arm64" ]]; then
        if [[ "$major" -ge 13 && "${SLICER_LINUX_RUNTIME_MAC_USE_ROSETTA:-}" == "1" && portable_x86_loader_available ]]; then
            LIMA_MODE="vz-aarch64-rosetta"
            LIMA_CREATE_ARGS+=(--vm-type=vz --arch=aarch64 --mount-type=virtiofs --rosetta)
        else
            LIMA_MODE="qemu-x86_64"
            LIMA_CREATE_ARGS+=(--vm-type=qemu --arch=x86_64 --mount-type=9p)
        fi
    else
        if [[ "$major" -ge 13 ]]; then
            LIMA_MODE="vz-x86_64"
            LIMA_CREATE_ARGS+=(--vm-type=vz --arch=x86_64 --mount-type=virtiofs)
        else
            LIMA_MODE="qemu-x86_64"
            LIMA_CREATE_ARGS+=(--vm-type=qemu --arch=x86_64 --mount-type=9p)
        fi
    fi
    if [[ -d /var/folders ]]; then
        LIMA_CREATE_ARGS+=(--mount=/var/folders:w)
    fi
}

ensure_qemu_if_needed() {
    case "$LIMA_MODE" in
        qemu-*) ;;
        *) return 0 ;;
    esac

    local qemu_bin brew_bin
    qemu_bin=$(find_qemu_system_x86_64 || true)
    if [[ -n "$qemu_bin" ]]; then
        export PATH="$(dirname "$qemu_bin"):$PATH"
        return 0
    fi

    brew_bin=$(find_brew || true)
    if [[ -n "$brew_bin" ]]; then
        "$brew_bin" install qemu || true
        qemu_bin=$(find_qemu_system_x86_64 || true)
        if [[ -n "$qemu_bin" ]]; then
            export PATH="$(dirname "$qemu_bin"):$PATH"
            return 0
        fi
    fi

    echo "qemu-system-x86_64 not found; install QEMU with: brew install qemu" >&2
    return 1
}

qemu_x86_guestagent_available() {
    local path prefix
    for path in \
        "$LOCAL_LIMA_ROOT/share/lima/lima-guestagent.Linux-x86_64.gz" \
        /opt/homebrew/opt/lima/share/lima/lima-guestagent.Linux-x86_64.gz \
        /usr/local/opt/lima/share/lima/lima-guestagent.Linux-x86_64.gz \
        /opt/homebrew/opt/lima-additional-guestagents/share/lima/lima-guestagent.Linux-x86_64.gz \
        /usr/local/opt/lima-additional-guestagents/share/lima/lima-guestagent.Linux-x86_64.gz
    do
        if [[ -f "$path" ]]; then
            return 0
        fi
    done

    if command -v brew >/dev/null 2>&1; then
        for prefix in "$(brew --prefix lima 2>/dev/null || true)" "$(brew --prefix lima-additional-guestagents 2>/dev/null || true)"; do
            if [[ -n "$prefix" && -f "$prefix/share/lima/lima-guestagent.Linux-x86_64.gz" ]]; then
                return 0
            fi
        done
    fi

    return 1
}

ensure_additional_guestagents_if_needed() {
    if [[ "$LIMA_MODE" != "qemu-x86_64" || "$(uname -m)" != "arm64" ]]; then
        return 0
    fi

    if qemu_x86_guestagent_available; then
        return 0
    fi

    install_lima_binary_locally || true
    if qemu_x86_guestagent_available; then
        return 0
    fi

    if command -v brew >/dev/null 2>&1; then
        brew install lima-additional-guestagents || true
    fi

    if qemu_x86_guestagent_available; then
        return 0
    fi

    echo "lima additional x86_64 guestagent not available for qemu-x86_64 fallback" >&2
    return 1
}

select_qemu_mode() {
    LIMA_MODE="qemu-x86_64"
    LIMA_CREATE_ARGS=(start "--name=${INSTANCE}" --tty=false --mount-writable --containerd=none --vm-type=qemu --arch=x86_64 --mount-type=9p)
    if [[ -d /var/folders ]]; then
        LIMA_CREATE_ARGS+=(--mount=/var/folders:w)
    fi
}

ensure_lima_installed() {
    if [[ -n "${SLICER_LINUX_RUNTIME_LIMACTL:-}" ]]; then
        LIMACTL=$(find_limactl || true)
        [[ -n "$LIMACTL" ]] || return 1
        limactl_supports_required_mode "$LIMACTL"
        return $?
    fi

    local desired_version
    desired_version=$(resolve_lima_version)

    if [[ -x "$LOCAL_LIMA_BIN/limactl" ]] && limactl_version_matches "$LOCAL_LIMA_BIN/limactl" "$desired_version" && limactl_supports_required_mode "$LOCAL_LIMA_BIN/limactl"; then
        if [[ "$LIMA_MODE" != "qemu-x86_64" || "$(uname -m)" != "arm64" || -f "$LOCAL_LIMA_ROOT/share/lima/lima-guestagent.Linux-x86_64.gz" ]]; then
            LIMACTL="$LOCAL_LIMA_BIN/limactl"
            return 0
        fi
    fi

    install_lima_binary_locally || true
    if [[ -x "$LOCAL_LIMA_BIN/limactl" ]] && limactl_supports_required_mode "$LOCAL_LIMA_BIN/limactl"; then
        if [[ "$LIMA_MODE" != "qemu-x86_64" || "$(uname -m)" != "arm64" || -f "$LOCAL_LIMA_ROOT/share/lima/lima-guestagent.Linux-x86_64.gz" ]]; then
            LIMACTL="$LOCAL_LIMA_BIN/limactl"
            return 0
        fi
    fi

    if command -v brew >/dev/null 2>&1; then
        brew install lima || true
    fi

    LIMACTL=$(find_system_limactl || true)
    if [[ -n "$LIMACTL" ]] && limactl_supports_required_mode "$LIMACTL"; then
        return 0
    fi

    echo "compatible limactl not found and local Lima install failed" >&2
    return 1
}

maybe_install_rosetta() {
    if [[ "$LIMA_MODE" != "vz-aarch64-rosetta" ]]; then
        return 0
    fi
    /usr/sbin/softwareupdate --install-rosetta --agree-to-license >/dev/null 2>&1 || true
}

check_optional_pair() {
    local dir="$1"
    if [[ -z "$dir" || ! -d "$dir" ]]; then
        return 0
    fi
    if { [[ -f "$dir/libbambu_networking.so" ]] && [[ ! -f "$dir/libBambuSource.so" ]]; } || { [[ ! -f "$dir/libbambu_networking.so" ]] && [[ -f "$dir/libBambuSource.so" ]]; }; then
        echo "partial optional linux component package in $dir: libbambu_networking.so and libBambuSource.so must exist together" >&2
        exit 1
    fi
}

copy_payload_files_from_dir() {
    local src_dir="$1"
    local dst_dir="$2"
    local path base
    if [[ -z "$src_dir" || ! -d "$src_dir" ]]; then
        return 0
    fi
    for path in "$src_dir"/*; do
        [[ -f "$path" ]] || continue
        base=$(basename -- "$path")
        case "$base" in
            slicer_linux_runtime_host|slicer_linux_runtime_host_abi1|slicer_linux_runtime_host_abi0|libbambu_networking.so|libBambuSource.so|linux_component_manifest.json|ca-certificates.crt|slicer_base64.cer|ld-linux-x86-64.so.2|lib*.so|lib*.so.*|*.so|*.so.*)
                cp -f "$path" "$dst_dir/$base"
                ;;
        esac
    done
}

copy_runtime_payload() {
    local src_dir="$1"
    local dst_dir="$2"
    local cache_dir="${3:-}"
    local file
    local required_files=(
        slicer_linux_runtime_host
        slicer_linux_runtime_host_abi1
        slicer_linux_runtime_host_abi0
        ca-certificates.crt
        slicer_base64.cer
        ld-linux-x86-64.so.2
        libc.so.6
        libm.so.6
        libresolv.so.2
        libnss_dns.so.2
        libnss_files.so.2
        libstdc++.so.6
        libgcc_s.so.1
        libz.so.1
    )

    for file in "${required_files[@]}"; do
        if [[ ! -f "$src_dir/$file" ]]; then
            echo "missing required runtime payload file: $file" >&2
            exit 1
        fi
    done

    check_optional_pair "$src_dir"
    check_optional_pair "$cache_dir"

    mkdir -p "$dst_dir"
    if { [[ ! -f "$src_dir/libbambu_networking.so" || ! -f "$src_dir/libBambuSource.so" ]]; } && { [[ -z "$cache_dir" || ! -f "$cache_dir/libbambu_networking.so" || ! -f "$cache_dir/libBambuSource.so" ]]; }; then
        rm -f "$dst_dir/libbambu_networking.so" "$dst_dir/libBambuSource.so" "$dst_dir/linux_component_manifest.json" "$PROBE_MARKER_FILE"
    fi
    copy_payload_files_from_dir "$src_dir" "$dst_dir"
    if [[ -n "$cache_dir" && "$cache_dir" != "$src_dir" ]]; then
        copy_payload_files_from_dir "$cache_dir" "$dst_dir"
    fi

    if { [[ -f "$dst_dir/libbambu_networking.so" ]] && [[ ! -f "$dst_dir/libBambuSource.so" ]]; } || { [[ ! -f "$dst_dir/libbambu_networking.so" ]] && [[ -f "$dst_dir/libBambuSource.so" ]]; }; then
        echo "partial optional linux component package in runtime: libbambu_networking.so and libBambuSource.so must exist together" >&2
        exit 1
    fi

    chmod 755 "$dst_dir/slicer_linux_runtime_host" "$dst_dir/slicer_linux_runtime_host_abi1" "$dst_dir/slicer_linux_runtime_host_abi0"
    [[ ! -f "$dst_dir/ld-linux-x86-64.so.2" ]] || chmod 755 "$dst_dir/ld-linux-x86-64.so.2"
    chmod 755 "$dst_dir"/*.so "$dst_dir"/*.so.* 2>/dev/null || true
    rm -f "$dst_dir/.selected_host_abi"
}


lima_instance_exists() {
    "$LIMACTL" list --format '{{.Name}}' 2>/dev/null | grep -Fxq "$INSTANCE"
}

delete_lima_instance_if_exists() {
    "$LIMACTL" stop "$INSTANCE" >/dev/null 2>&1 || true
    "$LIMACTL" delete -f "$INSTANCE" >/dev/null 2>&1 || true
}

start_lima_instance() {
    if [[ "$REPLACE_EXISTING" -eq 1 ]]; then
        delete_lima_instance_if_exists
    fi

    if "$LIMACTL" shell "$INSTANCE" -- /usr/bin/env true >/dev/null 2>&1; then
        return 0
    fi

    if lima_instance_exists; then
        "$LIMACTL" start "$INSTANCE" >/dev/null 2>&1 || true
        if ! "$LIMACTL" shell "$INSTANCE" -- /usr/bin/env true >/dev/null 2>&1; then
            echo "existing Lima instance is not usable - recreating: $INSTANCE"
            delete_lima_instance_if_exists
        else
            return 0
        fi
    fi

    if ! "$LIMACTL" "${LIMA_CREATE_ARGS[@]}" template:default; then
        "$LIMACTL" "${LIMA_CREATE_ARGS[@]}" template://default || return 1
    fi

    "$LIMACTL" shell "$INSTANCE" -- /usr/bin/env true >/dev/null 2>&1
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

linux_component_package_available() {
    [[ -f "$RUNTIME_DIR/libbambu_networking.so" && -f "$RUNTIME_DIR/libBambuSource.so" ]]
}

component_probe_marker_value() {
    linux_component_package_available || return 1
    local mode
    mode="${LIMA_MODE:-$(trim_file "$APP_SUPPORT_DIR/lima_mode.txt" || true)}"
    {
        printf 'mode=%s\n' "$mode"
        shasum -a 256 "$RUNTIME_DIR/libbambu_networking.so" "$RUNTIME_DIR/libBambuSource.so"
        if [[ -f "$RUNTIME_DIR/linux_component_manifest.json" ]]; then
            shasum -a 256 "$RUNTIME_DIR/linux_component_manifest.json"
        fi
    } | shasum -a 256 | awk '{print $1}'
}

INSTANCE="${SLICER_LINUX_RUNTIME_MAC_LIMA_INSTANCE:-}"
if [[ -z "$INSTANCE" ]]; then
    INSTANCE=$(trim_file "$COMPONENT_DIR/slicer_linux_runtime_lima_instance.txt" || true)
fi
if [[ -z "$INSTANCE" ]]; then
    INSTANCE="slicer-linux-runtime"
fi

try_current_lima_mode() {
    echo "Trying Lima mode: $LIMA_MODE" >> "$LOG_DIR/install-probe.log"
    if ! start_lima_instance >> "$LOG_DIR/install-probe.log" 2>&1; then
        echo "Lima start failed for mode: $LIMA_MODE" >> "$LOG_DIR/install-probe.log"
        return 1
    fi

    case "$LIMA_MODE" in
        vz-*)
            "$LIMACTL" start-at-login "$INSTANCE" --enabled >/dev/null 2>&1 || true
            ;;
    esac

    printf '%s\n' "$LIMA_MODE" > "$APP_SUPPORT_DIR/lima_mode.txt"
    if linux_component_package_available; then
        probe_linux_payload >> "$LOG_DIR/install-probe.log" 2>&1
        local marker
        marker=$(component_probe_marker_value || true)
        if [[ -n "$marker" ]]; then
            printf '%s\n' "$marker" > "$PROBE_MARKER_FILE"
        fi
    else
        rm -f "$PROBE_MARKER_FILE"
        echo "optional linux component not present; Lima runtime start verified without plugin probe" >> "$LOG_DIR/install-probe.log"
    fi
}

try_qemu_fallback() {
    echo "Retrying with qemu-x86_64 fallback" >> "$LOG_DIR/install-probe.log"
    delete_lima_instance_if_exists >> "$LOG_DIR/install-probe.log" 2>&1 || true
    select_qemu_mode
    REPLACE_EXISTING=0
    ensure_qemu_if_needed
    ensure_lima_installed
    ensure_additional_guestagents_if_needed
    try_current_lima_mode
}

if [[ ! -f "$INSTALL_VERSION_FILE" || "$(trim_file "$INSTALL_VERSION_FILE" || true)" != "$INSTALL_VERSION" ]]; then
    REPLACE_EXISTING=1
fi
if [[ "$REPLACE_EXISTING" -eq 1 ]]; then
    rm -rf "$RUNTIME_DIR"
fi
copy_runtime_payload "$COMPONENT_DIR" "$RUNTIME_DIR" "$COMPONENT_CACHE_DIR"
select_lima_mode
if [[ -f "$APP_SUPPORT_DIR/lima_mode.txt" && "$(trim_file "$APP_SUPPORT_DIR/lima_mode.txt" || true)" != "$LIMA_MODE" ]]; then
    REPLACE_EXISTING=1
fi
ensure_qemu_if_needed
ensure_lima_installed
ensure_additional_guestagents_if_needed
maybe_install_rosetta

if ! try_current_lima_mode; then
    case "$LIMA_MODE" in
        vz-*)
            if [[ -z "${SLICER_LINUX_RUNTIME_MAC_DISABLE_QEMU_FALLBACK:-}" ]]; then
                if ! try_qemu_fallback; then
                    echo "macOS Lima runtime probe failed; see $LOG_DIR/install-probe.log" >&2
                    exit 1
                fi
            else
                echo "macOS Lima runtime probe failed; see $LOG_DIR/install-probe.log" >&2
                exit 1
            fi
            ;;
        *)
            echo "macOS Lima runtime probe failed; see $LOG_DIR/install-probe.log" >&2
            exit 1
            ;;
    esac
fi

printf '%s\n' "$INSTALL_VERSION" > "$INSTALL_VERSION_FILE"
printf 'runtime installed\n'
