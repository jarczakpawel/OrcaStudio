#!/bin/bash
set -euo pipefail

export PATH="/opt/homebrew/bin:/opt/homebrew/sbin:/usr/local/bin:/usr/local/sbin:/usr/bin:/bin:/usr/sbin:/sbin:${PATH:-}"

PACKAGE_DIR=""
COMPONENT_DIR=""
COMPONENT_CACHE_DIR=""
REPLACE_EXISTING=0
RECREATE_INSTANCE=0
PRINT_GUEST_PROVISIONER=0
PRINT_GUEST_VERIFIER=0

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
            RECREATE_INSTANCE=1
            shift
            ;;
        -PrintGuestProvisioner)
            PRINT_GUEST_PROVISIONER=1
            shift
            ;;
        -PrintGuestVerifier)
            PRINT_GUEST_VERIFIER=1
            shift
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

emit_guest_provisioner() {
    cat <<'ORCASTUDIO_GUEST_PROVISIONER'
#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C

MODE="${1:-install}"
case "$MODE" in
    install|ci-check) ;;
    *) echo "unsupported guest provisioner mode: $MODE" >&2; exit 2 ;;
esac

CURRENT_PHASE="startup"
CURRENT_PACKAGE="-"
guest_provisioner_error() {
    local rc="$1"
    local line="$2"
    local command="$3"
    trap - ERR
    printf 'OrcaStudio guest provisioner failed: mode=%s arch=%s phase=%s package=%s line=%s rc=%s command=%s\n' \
        "$MODE" "${arch:-unknown}" "${CURRENT_PHASE:-unknown}" "${CURRENT_PACKAGE:--}" \
        "$line" "$rc" "$command" >&2
    exit "$rc"
}
trap 'guest_provisioner_error "$?" "$LINENO" "$BASH_COMMAND"' ERR

FORCE_SUDO="${ORCASTUDIO_PROVISIONER_FORCE_SUDO:-0}"
case "$FORCE_SUDO" in 0|1) ;; *) echo "invalid ORCASTUDIO_PROVISIONER_FORCE_SUDO value" >&2; exit 2 ;; esac
if (( EUID == 0 )) && [[ "$FORCE_SUDO" != 1 ]]; then
    SUDO=()
else
    command -v sudo >/dev/null 2>&1 || {
        echo "sudo is required to provision the Lima guest" >&2
        exit 1
    }
    SUDO=(sudo)
fi

CURRENT_PHASE="validate-tools"
command -v apt-get >/dev/null 2>&1 || {
    echo "unsupported Lima guest package manager: apt-get not found" >&2
    exit 1
}
command -v apt-cache >/dev/null 2>&1 || {
    echo "unsupported Lima guest package manager: apt-cache not found" >&2
    exit 1
}
command -v dpkg >/dev/null 2>&1 || {
    echo "unsupported Lima guest: dpkg not found" >&2
    exit 1
}

# The runtime is built against Ubuntu 24.04. Mixing suites would make the amd64
# loader and the native desktop stack incoherent, so fail instead of guessing.
CURRENT_PHASE="validate-os"
OS_RELEASE_FILE="${ORCASTUDIO_OS_RELEASE_FILE:-/etc/os-release}"
[[ -r "$OS_RELEASE_FILE" ]] || { echo "guest os-release file is missing: $OS_RELEASE_FILE" >&2; exit 1; }
. "$OS_RELEASE_FILE"
if [[ "${ID:-}" != "ubuntu" || "${VERSION_CODENAME:-}" != "noble" ]]; then
    echo "unsupported Lima guest release: ID=${ID:-unknown} VERSION_CODENAME=${VERSION_CODENAME:-unknown}; expected Ubuntu noble (24.04)" >&2
    exit 1
fi

KEYRING="${ORCASTUDIO_UBUNTU_KEYRING:-/usr/share/keyrings/ubuntu-archive-keyring.gpg}"
case "$KEYRING" in
    /*) ;;
    *) echo "Ubuntu archive keyring path must be absolute: $KEYRING" >&2; exit 1 ;;
esac
case "$KEYRING" in *$'\n'*|*$'\r'*) echo "Ubuntu archive keyring path contains a newline" >&2; exit 1 ;; esac
[[ -r "$KEYRING" ]] || {
    echo "Ubuntu archive keyring is missing: $KEYRING" >&2
    exit 1
}

has_foreign_architecture() {
    local wanted="$1"
    local foreign_architectures=""
    foreign_architectures=$(dpkg --print-foreign-architectures)
    grep -Fxq "$wanted" <<< "$foreign_architectures"
}

CURRENT_PHASE="configure-architecture"
arch=$(dpkg --print-architecture)
case "$arch" in
    arm64)
        if ! has_foreign_architecture amd64; then
            "${SUDO[@]}" dpkg --add-architecture amd64
        fi
        has_foreign_architecture amd64 || {
            echo "failed to enable the amd64 foreign architecture" >&2
            exit 1
        }
        ;;
    amd64) ;;
    *) echo "unsupported Lima guest architecture: $arch" >&2; exit 1 ;;
esac

disable_rosetta_aot_cache() {
    [[ "$arch" == arm64 ]] || return 0
    [[ -x /mnt/lima-rosetta/rosetta ]] || return 0

    CURRENT_PHASE="disable-rosetta-aot-cache"
    local marker_tmp dropin_tmp
    marker_tmp=$(mktemp)
    dropin_tmp=$(mktemp)
    printf '%s\n' 'disabled-for-protected-vendor-elf' > "$marker_tmp"
    printf '%s\n' '[Unit]' 'ConditionPathExists=!/etc/orcastudio-rosetta-aot-disabled' > "$dropin_tmp"
    "${SUDO[@]}" install -m 0644 "$marker_tmp" /etc/orcastudio-rosetta-aot-disabled

    if command -v systemctl >/dev/null 2>&1 && systemctl cat rosettad.service >/dev/null 2>&1; then
        "${SUDO[@]}" install -d -m 0755 /etc/systemd/system/rosettad.service.d
        "${SUDO[@]}" install -m 0644 "$dropin_tmp" /etc/systemd/system/rosettad.service.d/10-orcastudio-disable-aot.conf
        "${SUDO[@]}" systemctl daemon-reload
        "${SUDO[@]}" systemctl stop rosettad.service
        if systemctl is-active --quiet rosettad.service 2>/dev/null; then
            echo "rosettad remained active after stop" >&2
            rm -f "$marker_tmp" "$dropin_tmp"
            exit 1
        fi
    fi
    rm -f "$marker_tmp" "$dropin_tmp"

    "${SUDO[@]}" rm -f \
        /run/rosettad/rosetta.sock \
        /var/cache/rosettad/uds/rosetta.sock
    if [[ -d /var/cache/rosettad ]]; then
        "${SUDO[@]}" find /var/cache/rosettad -xdev -mindepth 1 -delete
    fi
    [[ ! -S /run/rosettad/rosetta.sock ]]
    [[ ! -S /var/cache/rosettad/uds/rosetta.sock ]]
}

SOURCE_FILE="${ORCASTUDIO_APT_SOURCE_FILE:-/etc/apt/sources.list.d/orcastudio-runtime.sources}"
SOURCE_PARTS="${ORCASTUDIO_APT_SOURCE_PARTS:-/etc/apt/orcastudio-runtime-sourceparts}"
APT_LISTS_DIR="${ORCASTUDIO_APT_LISTS_DIR:-/var/lib/apt/lists/orcastudio-runtime-v11}"
for managed_path in "$SOURCE_FILE" "$SOURCE_PARTS" "$APT_LISTS_DIR"; do
    case "$managed_path" in
        /*) ;;
        *) echo "managed APT path must be absolute: $managed_path" >&2; exit 1 ;;
    esac
    case "$managed_path" in
        /) echo "managed APT path must not be the filesystem root" >&2; exit 1 ;;
        *$'\n'*|*$'\r'*) echo "managed APT path contains a newline" >&2; exit 1 ;;
    esac
done
source_tmp=$(mktemp)
download_dir=""
trap 'rm -f "$source_tmp"; [[ -z "$download_dir" ]] || rm -rf "$download_dir"' EXIT

CURRENT_PHASE="write-apt-sources"
if [[ "$arch" == arm64 ]]; then
    cat > "$source_tmp" <<EOF_ARM_SOURCES
Types: deb
URIs: https://ports.ubuntu.com/ubuntu-ports
Suites: noble noble-updates noble-security
Components: main restricted universe multiverse
Architectures: arm64
Signed-By: $KEYRING

Types: deb
URIs: https://archive.ubuntu.com/ubuntu
Suites: noble noble-updates
Components: main restricted universe multiverse
Architectures: amd64
Signed-By: $KEYRING

Types: deb
URIs: https://security.ubuntu.com/ubuntu
Suites: noble-security
Components: main restricted universe multiverse
Architectures: amd64
Signed-By: $KEYRING
EOF_ARM_SOURCES
else
    cat > "$source_tmp" <<EOF_AMD_SOURCES
Types: deb
URIs: https://archive.ubuntu.com/ubuntu
Suites: noble noble-updates
Components: main restricted universe multiverse
Architectures: amd64
Signed-By: $KEYRING

Types: deb
URIs: https://security.ubuntu.com/ubuntu
Suites: noble-security
Components: main restricted universe multiverse
Architectures: amd64
Signed-By: $KEYRING
EOF_AMD_SOURCES
fi

"${SUDO[@]}" install -d -m 0755 "$SOURCE_PARTS"
"${SUDO[@]}" find "$SOURCE_PARTS" -mindepth 1 -maxdepth 1 -type f -delete
"${SUDO[@]}" install -m 0644 "$source_tmp" "$SOURCE_FILE"

# Package candidates must be computed only from the managed source map. A
# dedicated lists directory prevents stale indexes from the guest image or a
# previous failed setup from influencing apt-cache policy.
"${SUDO[@]}" install -d -m 0755 "$APT_LISTS_DIR"
"${SUDO[@]}" find "$APT_LISTS_DIR" -mindepth 1 -delete
# Let apt-get create lists/partial itself so it receives the correct _apt
# ownership and permissions for sandboxed downloads.

if [[ "$MODE" == install ]]; then
    # This is a dedicated runtime VM. Once amd64 is enabled, unqualified
    # ports.ubuntu.com entries make every future plain apt-get update request
    # request non-existent amd64 indices. Preserve the originals as backups and
    # make the architecture-qualified managed source the only active source.
    for existing in /etc/apt/sources.list /etc/apt/sources.list.d/*.list /etc/apt/sources.list.d/*.sources; do
        [[ -e "$existing" ]] || continue
        [[ "$existing" == "$SOURCE_FILE" ]] && continue
        case "$existing" in *.orcastudio-disabled) continue ;; esac
        "${SUDO[@]}" mv -f "$existing" "$existing.orcastudio-disabled"
    done
fi

APT_OPTS=(
    -o "Dir::Etc::sourcelist=$SOURCE_FILE"
    -o "Dir::Etc::sourceparts=$SOURCE_PARTS"
    -o APT::Get::List-Cleanup=1
    -o Dpkg::Use-Pty=0
    -o APT::Color=0
    -o Acquire::Retries=3
    -o Acquire::http::Timeout=30
    -o Acquire::https::Timeout=30
    -o Acquire::Languages=none
    -o Acquire::AllowInsecureRepositories=false
    -o Acquire::AllowDowngradeToInsecureRepositories=false
)
APT_OPTS+=( -o "Dir::State::lists=$APT_LISTS_DIR" )

run_apt() {
    local attempt rc=1
    for attempt in 1 2 3; do
        if "${SUDO[@]}" env DEBIAN_FRONTEND=noninteractive apt-get "${APT_OPTS[@]}" "$@"; then
            return 0
        else
            rc=$?
        fi
        if (( attempt < 3 )); then
            echo "APT command failed (attempt $attempt/3), retrying" >&2
            sleep $((attempt * 3))
        fi
    done
    return "$rc"
}

KERNEL_RELEASE=$(uname -r)
KERNEL_MODULE_DIR="/lib/modules/$KERNEL_RELEASE"
KERNEL_EXTRA_PACKAGE="linux-modules-extra-$KERNEL_RELEASE"

NATIVE_PACKAGES=(
    alsa-utils
    ca-certificates
    dbus-x11
    fonts-dejavu-core
    fonts-liberation
    kmod
    libgtk-3-0t64
    libwebkit2gtk-4.1-0
    novnc
    websockify
    x11vnc
    xvfb
)
if ! find "$KERNEL_MODULE_DIR/kernel/sound/drivers" -maxdepth 1 -type f \
        \( -name 'snd-dummy.ko*' -o -name 'snd-aloop.ko*' \) -print -quit 2>/dev/null | grep -q .; then
    NATIVE_PACKAGES+=("$KERNEL_EXTRA_PACKAGE")
fi
AMD64_PACKAGES=(
    libc6:amd64
    libstdc++6:amd64
    libgcc-s1:amd64
    zlib1g:amd64
    libx11-6:amd64
    libxdamage1:amd64
    libxext6:amd64
    libxfixes3:amd64
    libdrm2:amd64
    libxcb1:amd64
    libxau6:amd64
    libxdmcp6:amd64
    libasound2t64:amd64
    libpulse0:amd64
    libssl3t64:amd64
)

CURRENT_PHASE="apt-update"
CURRENT_PACKAGE="-"
run_apt update

apt_policy_candidate() {
    local package="$1"
    local policy_output=""
    local line=""
    local candidate=""

    # Do not use `apt-cache ... | awk '... exit'` here. With pipefail enabled,
    # an early-exiting reader can close the pipe while apt-cache is still
    # writing and turn a valid lookup into SIGPIPE/141.
    policy_output=$(apt-cache "${APT_OPTS[@]}" policy "$package")
    while IFS= read -r line; do
        if [[ "$line" =~ ^[[:space:]]*Candidate:[[:space:]]*(.*)$ ]]; then
            candidate="${BASH_REMATCH[1]}"
        fi
    done <<< "$policy_output"
    candidate="${candidate#"${candidate%%[![:space:]]*}"}"
    candidate="${candidate%"${candidate##*[![:space:]]}"}"
    printf '%s\n' "$candidate"
}

# Verify package metadata before changing the guest. This catches wrong mirror,
# suite, architecture, or renamed package failures with the exact package name.
CHECK_PACKAGES=("${NATIVE_PACKAGES[@]}")
if [[ "$arch" == arm64 ]]; then
    CHECK_PACKAGES+=("${AMD64_PACKAGES[@]}")
fi
CURRENT_PHASE="apt-candidate"
for pkg in "${CHECK_PACKAGES[@]}"; do
    CURRENT_PACKAGE="$pkg"
    candidate=$(apt_policy_candidate "$pkg")
    if [[ -z "$candidate" || "$candidate" == "(none)" ]]; then
        echo "no install candidate from managed Ubuntu sources: $pkg" >&2
        exit 1
    fi
done
CURRENT_PACKAGE="-"

INSTALL_PACKAGES=("${NATIVE_PACKAGES[@]}")
if [[ "$arch" == arm64 ]]; then
    INSTALL_PACKAGES+=("${AMD64_PACKAGES[@]}")
fi

if [[ "$MODE" == ci-check ]]; then
    CURRENT_PHASE="apt-simulate"
    # Resolve the entire transaction and download representative foreign
    # packages. This catches wrong repositories, missing architectures, renamed
    # packages and dependency conflicts before a DMG is produced.
    run_apt --simulate install -y --no-install-recommends "${INSTALL_PACKAGES[@]}"
    if [[ "$arch" == arm64 ]]; then
        CURRENT_PHASE="apt-download"
        download_dir=$(mktemp -d)
        (
            cd "$download_dir"
            apt-get "${APT_OPTS[@]}" download libc6:amd64 libstdc++6:amd64 libssl3t64:amd64
            test -n "$(find . -maxdepth 1 -type f -name '*.deb' -print -quit)"
        )
    fi
    echo "OrcaStudio Ubuntu noble multiarch source integration check OK ($arch)"
    exit 0
fi

CURRENT_PHASE="apt-install"
run_apt install -y --no-install-recommends "${INSTALL_PACKAGES[@]}"

CURRENT_PHASE="configure-virtual-audio"

# Agora/WebRTC enumerates kernel ALSA devices even when the application only
# uses camera video. Do not expose macOS CoreAudio to the VM. Create a local
# virtual full-duplex ALSA card instead. Explicitly reload the module because a
# previous failed installation may have left it loaded without an enabled card.
audio_card_visible() {
    local card_id="$1"
    local playback_devices=""
    local capture_devices=""
    [[ -r /proc/asound/cards ]] || return 1
    grep -Fq "[$card_id" /proc/asound/cards || return 1
    playback_devices=$("${SUDO[@]}" aplay -l 2>/dev/null) || return 1
    capture_devices=$("${SUDO[@]}" arecord -l 2>/dev/null) || return 1
    grep -Fq "$card_id" <<< "$playback_devices" || return 1
    grep -Fq "$card_id" <<< "$capture_devices" || return 1
}

try_dummy_audio() {
    modinfo snd_dummy >/dev/null 2>&1 || return 1
    "${SUDO[@]}" modprobe -r snd_dummy 2>/dev/null || true
    "${SUDO[@]}" modprobe snd_dummy enable=1 id=BambuDummy pcm_devs=1 pcm_substreams=8 fake_buffer=1 || return 1
    sleep 1
    audio_card_visible BambuDummy
}

try_loopback_audio() {
    modinfo snd_aloop >/dev/null 2>&1 || return 1
    "${SUDO[@]}" modprobe -r snd_aloop 2>/dev/null || true
    "${SUDO[@]}" modprobe snd_aloop enable=1 id=BambuLoopback pcm_substreams=8 || return 1
    sleep 1
    audio_card_visible BambuLoopback
}

"${SUDO[@]}" depmod -a "$KERNEL_RELEASE"
"${SUDO[@]}" rm -f \
    /etc/modprobe.d/orcastudio-bambu-dummy-audio.conf \
    /etc/modules-load.d/orcastudio-bambu-dummy-audio.conf \
    /etc/modprobe.d/orcastudio-bambu-virtual-audio.conf \
    /etc/modules-load.d/orcastudio-bambu-virtual-audio.conf

AUDIO_MODULE=""
AUDIO_CARD=""
AUDIO_PLAYBACK_DEVICE=""
AUDIO_CAPTURE_DEVICE=""
if try_dummy_audio; then
    AUDIO_MODULE="snd_dummy"
    AUDIO_CARD="BambuDummy"
    AUDIO_PLAYBACK_DEVICE="plughw:BambuDummy,0"
    AUDIO_CAPTURE_DEVICE="plughw:BambuDummy,0"
elif try_loopback_audio; then
    AUDIO_MODULE="snd_aloop"
    AUDIO_CARD="BambuLoopback"
    # snd_aloop connects playback device 0 to capture device 1.
    AUDIO_PLAYBACK_DEVICE="plughw:BambuLoopback,0,0"
    AUDIO_CAPTURE_DEVICE="plughw:BambuLoopback,1,0"
else
    echo "could not create a virtual ALSA playback/capture card with snd_dummy or snd_aloop" >&2
    cat /proc/asound/cards 2>/dev/null >&2 || true
    "${SUDO[@]}" dmesg 2>/dev/null | tail -80 >&2 || true
    exit 1
fi

modprobe_tmp=$(mktemp)
modules_load_tmp=$(mktemp)
asound_tmp=$(mktemp)
case "$AUDIO_MODULE" in
    snd_dummy)
        printf '%s\n' 'options snd_dummy enable=1 id=BambuDummy pcm_devs=1 pcm_substreams=8 fake_buffer=1' > "$modprobe_tmp"
        ;;
    snd_aloop)
        printf '%s\n' 'options snd_aloop enable=1 id=BambuLoopback pcm_substreams=8' > "$modprobe_tmp"
        ;;
esac
printf '%s\n' "$AUDIO_MODULE" > "$modules_load_tmp"
cat > "$asound_tmp" <<EOF_ASOUND
pcm.!default {
    type asym
    playback.pcm "$AUDIO_PLAYBACK_DEVICE"
    capture.pcm "$AUDIO_CAPTURE_DEVICE"
}
ctl.!default {
    type hw
    card $AUDIO_CARD
}
EOF_ASOUND
"${SUDO[@]}" install -d -m 0755 /etc/modprobe.d /etc/modules-load.d
"${SUDO[@]}" install -m 0644 "$modprobe_tmp" /etc/modprobe.d/orcastudio-bambu-virtual-audio.conf
"${SUDO[@]}" install -m 0644 "$modules_load_tmp" /etc/modules-load.d/orcastudio-bambu-virtual-audio.conf
"${SUDO[@]}" install -m 0644 "$asound_tmp" /etc/asound.conf
printf '%s\n' "$AUDIO_MODULE" | "${SUDO[@]}" tee /etc/orcastudio-bambu-audio-module >/dev/null
printf '%s\n' "$AUDIO_CARD" | "${SUDO[@]}" tee /etc/orcastudio-bambu-audio-card >/dev/null

runtime_user=$(id -un)
if [[ "$runtime_user" != root ]]; then
    "${SUDO[@]}" usermod -aG audio "$runtime_user"
fi

# The group added above is visible only after a new login session. The runtime
# starts immediately after provisioning, so make the isolated VM sound devices
# directly accessible now and persist the mode through udev for future boots.
audio_udev_tmp=$(mktemp)
printf '%s\n' 'SUBSYSTEM=="sound", MODE:="0666"' > "$audio_udev_tmp"
"${SUDO[@]}" install -d -m 0755 /etc/udev/rules.d
"${SUDO[@]}" install -m 0644 "$audio_udev_tmp" /etc/udev/rules.d/99-orcastudio-bambu-audio.rules
if command -v udevadm >/dev/null 2>&1; then
    "${SUDO[@]}" udevadm control --reload-rules || true
    "${SUDO[@]}" udevadm trigger --subsystem-match=sound || true
fi
if [[ -d /dev/snd ]]; then
    "${SUDO[@]}" find /dev/snd -maxdepth 1 -type c -exec chmod 0666 {} +
fi

audio_card_visible "$AUDIO_CARD"
audio_test_dir=$(mktemp -d)
dd if=/dev/zero of="$audio_test_dir/silence.raw" bs=192000 count=3 status=none
"${SUDO[@]}" timeout 8 aplay -q -D default -t raw -f S16_LE -r 48000 -c 2 "$audio_test_dir/silence.raw" &
audio_play_pid=$!
sleep 0.25
"${SUDO[@]}" timeout 8 arecord -q -D default -d 1 -t raw -f S16_LE -r 48000 -c 2 "$audio_test_dir/capture.raw"
wait "$audio_play_pid"
[[ -s "$audio_test_dir/capture.raw" ]]
rm -rf "$audio_test_dir"

CURRENT_PHASE="verify-native-runtime"
command -v Xvfb >/dev/null
command -v xvfb-run >/dev/null
command -v x11vnc >/dev/null
command -v websockify >/dev/null
command -v dbus-run-session >/dev/null
[[ -f /usr/share/novnc/vnc.html ]]

if [[ "$arch" == arm64 ]]; then
    CURRENT_PHASE="verify-amd64-runtime"
    has_foreign_architecture amd64
    [[ "$(dpkg-query -W -f='${Architecture}' libc6:amd64)" == amd64 ]]
    [[ "$(dpkg-query -W -f='${Architecture}' libstdc++6:amd64)" == amd64 ]]
    [[ "$(dpkg-query -W -f='${Architecture}' libssl3t64:amd64)" == amd64 ]]
    [[ -x /lib64/ld-linux-x86-64.so.2 || -x /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 ]]
    [[ -f /usr/lib/x86_64-linux-gnu/libstdc++.so.6 ]]
    [[ -f /lib/x86_64-linux-gnu/libc.so.6 ]]

    # Lima --rosetta is expected to register a binfmt_misc handler. Check the
    # active handler rather than merely trusting the VM creation flag.
    [[ -r /proc/sys/fs/binfmt_misc/status ]]
    grep -Fxq enabled /proc/sys/fs/binfmt_misc/status
    rosetta_entry=""
    for entry in /proc/sys/fs/binfmt_misc/*; do
        [[ -f "$entry" ]] || continue
        case "$(basename "$entry")" in status|register) continue ;; esac
        if grep -qi rosetta "$entry"; then
            rosetta_entry="$entry"
            break
        fi
    done
    [[ -n "$rosetta_entry" ]] || {
        echo "Rosetta binfmt_misc handler is not registered in the Lima guest" >&2
        exit 1
    }
    grep -q '^enabled' "$rosetta_entry"

    # Run the system amd64 loader from the guest ext4 filesystem. This validates
    # the actual kernel -> binfmt -> Rosetta path before the vendor plug-in is
    # copied or loaded.
    amd64_loader=/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2
    [[ -x "$amd64_loader" ]] || amd64_loader=/lib64/ld-linux-x86-64.so.2
    "$amd64_loader" --help >/dev/null
fi

disable_rosetta_aot_cache

CURRENT_PHASE="write-marker"
printf '%s\n' 'ubuntu-24.04-bambu-network-amd64-rosetta-splitlock-v16' | "${SUDO[@]}" tee /etc/orcastudio-linux-auth-runtime >/dev/null
CURRENT_PHASE="apt-clean"
CURRENT_PACKAGE="-"
run_apt clean
ORCASTUDIO_GUEST_PROVISIONER

}

emit_guest_runtime_verifier() {
    cat <<'ORCASTUDIO_GUEST_RUNTIME_VERIFIER'
#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C

CURRENT_CHECK="startup"
runtime_verifier_error() {
    local rc="$1"
    local line="$2"
    local command="$3"
    trap - ERR
    printf 'OrcaStudio guest runtime verification failed: arch=%s check=%s line=%s rc=%s command=%s\n' \
        "${arch:-unknown}" "${CURRENT_CHECK:-unknown}" "$line" "$rc" "$command" >&2
    exit "$rc"
}
trap 'runtime_verifier_error "$?" "$LINENO" "$BASH_COMMAND"' ERR

EXPECTED_MARKER="${ORCASTUDIO_EXPECTED_RUNTIME_MARKER:-ubuntu-24.04-bambu-network-amd64-rosetta-splitlock-v16}"
MARKER_FILE="${ORCASTUDIO_RUNTIME_MARKER_FILE:-/etc/orcastudio-linux-auth-runtime}"
OS_RELEASE_FILE="${ORCASTUDIO_OS_RELEASE_FILE:-/etc/os-release}"
SOURCE_FILE="${ORCASTUDIO_APT_SOURCE_FILE:-/etc/apt/sources.list.d/orcastudio-runtime.sources}"
NOVNC_HTML="${ORCASTUDIO_NOVNC_HTML:-/usr/share/novnc/vnc.html}"
BINFMT_DIR="${ORCASTUDIO_BINFMT_DIR:-/proc/sys/fs/binfmt_misc}"
X86_LIB_DIR="${ORCASTUDIO_X86_LIB_DIR:-/lib/x86_64-linux-gnu}"
X86_USR_LIB_DIR="${ORCASTUDIO_X86_USR_LIB_DIR:-/usr/lib/x86_64-linux-gnu}"
X86_LOADER_FALLBACK="${ORCASTUDIO_X86_LOADER_FALLBACK:-/lib64/ld-linux-x86-64.so.2}"

require_command() {
    local name="$1"
    command -v "$name" >/dev/null 2>&1 || {
        echo "missing required guest command: $name" >&2
        return 1
    }
}

require_regular_file() {
    local path="$1"
    local label="$2"
    [[ -f "$path" ]] || {
        echo "missing required guest file: $label ($path)" >&2
        return 1
    }
}

require_readable_file() {
    local path="$1"
    local label="$2"
    [[ -r "$path" ]] || {
        echo "missing or unreadable guest file: $label ($path)" >&2
        return 1
    }
}

require_executable_file() {
    local path="$1"
    local label="$2"
    [[ -x "$path" ]] || {
        echo "missing or non-executable guest file: $label ($path)" >&2
        return 1
    }
}

foreign_architecture_enabled() {
    local wanted="$1"
    local line
    local architectures=""
    architectures=$(dpkg --print-foreign-architectures)
    while IFS= read -r line; do
        [[ "$line" == "$wanted" ]] && return 0
    done <<< "$architectures"
    return 1
}

require_package_architecture() {
    local package="$1"
    local expected="$2"
    local actual=""
    CURRENT_CHECK="package-architecture:$package"
    actual=$(dpkg-query -W -f='${Architecture}' "$package")
    if [[ "$actual" != "$expected" ]]; then
        echo "wrong installed package architecture: package=$package expected=$expected actual=${actual:-missing}" >&2
        return 1
    fi
}

source_declares_architecture() {
    local wanted="$1"
    grep -Fq "Architectures: $wanted" "$SOURCE_FILE"
}

CURRENT_CHECK="marker"
require_readable_file "$MARKER_FILE" "runtime marker"
marker_value=$(cat "$MARKER_FILE")
[[ "$marker_value" == "$EXPECTED_MARKER" ]] || {
    echo "runtime marker mismatch: expected=$EXPECTED_MARKER actual=${marker_value:-missing}" >&2
    exit 1
}

CURRENT_CHECK="os-release"
require_readable_file "$OS_RELEASE_FILE" "os-release"
. "$OS_RELEASE_FILE"
[[ "${ID:-}" == ubuntu && "${VERSION_CODENAME:-}" == noble ]] || {
    echo "unsupported guest release: ID=${ID:-unknown} VERSION_CODENAME=${VERSION_CODENAME:-unknown}" >&2
    exit 1
}

CURRENT_CHECK="native-commands"
for command_name in Xvfb xvfb-run x11vnc websockify dbus-run-session dpkg dpkg-query aplay arecord id; do
    require_command "$command_name"
done
require_regular_file "$NOVNC_HTML" "noVNC entry point"
require_readable_file "$SOURCE_FILE" "managed APT source map"

CURRENT_CHECK="virtual-audio"
AUDIO_MODULE_FILE="${ORCASTUDIO_AUDIO_MODULE_FILE:-/etc/orcastudio-bambu-audio-module}"
AUDIO_CARD_FILE="${ORCASTUDIO_AUDIO_CARD_FILE:-/etc/orcastudio-bambu-audio-card}"
require_readable_file "$AUDIO_MODULE_FILE" "virtual audio module marker"
require_readable_file "$AUDIO_CARD_FILE" "virtual audio card marker"
audio_module=$(cat "$AUDIO_MODULE_FILE")
audio_card=$(cat "$AUDIO_CARD_FILE")
case "$audio_module:$audio_card" in
    snd_dummy:BambuDummy|snd_aloop:BambuLoopback) ;;
    *) echo "invalid virtual audio selection: module=$audio_module card=$audio_card" >&2; exit 1 ;;
esac
[[ -d "/sys/module/$audio_module" ]] || {
    echo "virtual audio module is not loaded: $audio_module" >&2
    exit 1
}
grep -Fq "[$audio_card" /proc/asound/cards
aplay -l | grep -Fq "$audio_card"
arecord -l | grep -Fq "$audio_card"
audio_test_dir=$(mktemp -d)
trap 'rm -rf "$audio_test_dir"' EXIT
dd if=/dev/zero of="$audio_test_dir/silence.raw" bs=192000 count=3 status=none
timeout 8 aplay -q -D default -t raw -f S16_LE -r 48000 -c 2 "$audio_test_dir/silence.raw" &
audio_play_pid=$!
sleep 0.25
timeout 8 arecord -q -D default -d 1 -t raw -f S16_LE -r 48000 -c 2 "$audio_test_dir/capture.raw"
wait "$audio_play_pid"
[[ -s "$audio_test_dir/capture.raw" ]]

CURRENT_CHECK="guest-architecture"
arch=$(dpkg --print-architecture)
case "$arch" in
    arm64)
        CURRENT_CHECK="foreign-architecture"
        foreign_architecture_enabled amd64 || {
            echo "amd64 foreign architecture is not enabled" >&2
            exit 1
        }

        require_package_architecture libc6:amd64 amd64
        require_package_architecture libstdc++6:amd64 amd64
        require_package_architecture libssl3t64:amd64 amd64

        CURRENT_CHECK="source-map"
        source_declares_architecture arm64
        source_declares_architecture amd64

        CURRENT_CHECK="amd64-files"
        require_regular_file "$X86_USR_LIB_DIR/libstdc++.so.6" "amd64 libstdc++"
        require_regular_file "$X86_LIB_DIR/libc.so.6" "amd64 libc"

        CURRENT_CHECK="rosetta-binfmt"
        require_readable_file "$BINFMT_DIR/status" "binfmt_misc status"
        [[ "$(cat "$BINFMT_DIR/status")" == enabled ]] || {
            echo "binfmt_misc is not enabled" >&2
            exit 1
        }
        rosetta_entry=""
        for entry in "$BINFMT_DIR"/*; do
            [[ -f "$entry" ]] || continue
            case "${entry##*/}" in status|register) continue ;; esac
            entry_content=$(cat "$entry")
            entry_content_lower=${entry_content,,}
            if [[ "$entry_content_lower" == *rosetta* ]]; then
                [[ "$entry_content" == enabled$'\n'* || "$entry_content" == enabled ]] || {
                    echo "Rosetta binfmt handler exists but is disabled: $entry" >&2
                    exit 1
                }
                rosetta_entry="$entry"
                break
            fi
        done
        [[ -n "$rosetta_entry" ]] || {
            echo "Rosetta binfmt_misc handler is not registered" >&2
            exit 1
        }

        CURRENT_CHECK="rosetta-aot-disabled"
        require_regular_file /etc/orcastudio-rosetta-aot-disabled "Rosetta AOT disabled marker"
        if command -v systemctl >/dev/null 2>&1 && systemctl cat rosettad.service >/dev/null 2>&1; then
            require_regular_file /etc/systemd/system/rosettad.service.d/10-orcastudio-disable-aot.conf "Rosetta AOT systemd drop-in"
            grep -Fxq 'ConditionPathExists=!/etc/orcastudio-rosetta-aot-disabled' /etc/systemd/system/rosettad.service.d/10-orcastudio-disable-aot.conf
            ! systemctl is-active --quiet rosettad.service
        fi
        [[ ! -S /run/rosettad/rosetta.sock ]]
        [[ ! -S /var/cache/rosettad/uds/rosetta.sock ]]

        CURRENT_CHECK="amd64-loader"
        amd64_loader="$X86_LIB_DIR/ld-linux-x86-64.so.2"
        [[ -x "$amd64_loader" ]] || amd64_loader="$X86_LOADER_FALLBACK"
        require_executable_file "$amd64_loader" "amd64 system loader"
        "$amd64_loader" --help >/dev/null
        ;;
    amd64)
        CURRENT_CHECK="source-map"
        source_declares_architecture amd64
        CURRENT_CHECK="amd64-loader"
        amd64_loader="$X86_LIB_DIR/ld-linux-x86-64.so.2"
        [[ -x "$amd64_loader" ]] || amd64_loader="$X86_LOADER_FALLBACK"
        require_executable_file "$amd64_loader" "amd64 system loader"
        ;;
    *)
        echo "unsupported Lima guest architecture: $arch" >&2
        exit 1
        ;;
esac

CURRENT_CHECK="complete"
printf 'OrcaStudio guest runtime verification OK (%s)\n' "$arch"
ORCASTUDIO_GUEST_RUNTIME_VERIFIER
}

if [[ "$PRINT_GUEST_PROVISIONER" -eq 1 ]]; then
    emit_guest_provisioner
    exit 0
fi
if [[ "$PRINT_GUEST_VERIFIER" -eq 1 ]]; then
    emit_guest_runtime_verifier
    exit 0
fi

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
LOCAL_LIMA_ROOT="$APP_SUPPORT_DIR/lima"
LOCAL_LIMA_BIN="$LOCAL_LIMA_ROOT/bin"
RUNTIME_DIR="${SLICER_LINUX_RUNTIME_MAC_RUNTIME_DIR:-$APP_SUPPORT_DIR/runtime}"
LOG_DIR="$APP_SUPPORT_DIR/logs"
INSTALL_VERSION="SLICER-LINUX-RUNTIME-MAC-0.34-BAMBU-NETWORK-ROSETTA-SPLITLOCK-V21"
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

validate_runtime_manifest() {
    local dir="$1"
    if [[ ! -f "$dir/runtime-files.sha256" ]]; then
        echo "missing runtime manifest: $dir/runtime-files.sha256" >&2
        return 1
    fi
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
    local version_text=""
    version_text=$(limactl_version_text "$limactl_bin")
    grep -Eq "(^|[^0-9])${wanted//./\.}([^0-9]|$)" <<< "$version_text"
}

limactl_supports_required_mode() {
    local limactl_bin="$1"
    local help
    help=$("$limactl_bin" start --help 2>&1 || true)
    grep -q -- '--vm-type' <<< "$help" || return 1
    grep -q -- '--arch' <<< "$help" || return 1
    grep -q -- '--containerd' <<< "$help" || return 1
    grep -q -- '--mount-type' <<< "$help" || return 1
    grep -q -- '--disk' <<< "$help" || return 1
    grep -q -- '--set' <<< "$help" || return 1
    if [[ "${LIMA_MODE:-}" == "vz-rosetta-aarch64" ]]; then
        grep -q -- '--rosetta' <<< "$help" || return 1
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
        local release_json=""
        release_json=$(curl -fsSL https://api.github.com/repos/lima-vm/lima/releases/latest || true)
        version=$(awk -F'"' '/"tag_name"[[:space:]]*:/ && !found { value=$4; found=1 } END { if (found) print value }' <<< "$release_json")
        if [[ -n "$version" ]]; then
            printf '%s\n' "$version"
            return 0
        fi
        resolve_lima_version_from_redirect
        return $?
    fi

    printf '%s\n' "v2.1.4"
}

expected_lima_sha256() {
    local archive="$1"
    case "$archive" in
        lima-2.1.4-Darwin-arm64.tar.gz) printf '%s\n' '14c5b283f1c5eb4078e5a300b8d241f69197a3e41326dfc685a69c9455917acf' ;;
        lima-2.1.4-Darwin-x86_64.tar.gz) printf '%s\n' '57f319f2d2b3e781cdceaaf58efc0cae92e31ccc774fee988df438c3a22feb45' ;;
        lima-additional-guestagents-2.1.4-Darwin-arm64.tar.gz) printf '%s\n' '475c9dbcda16ebbae239c57758c487d5c255aa13d9e6db437dae964d03f9414c' ;;
        lima-additional-guestagents-2.1.4-Darwin-x86_64.tar.gz) printf '%s\n' 'c36676c01551a4057bcd3586989fe068f9b408c6be3ae6e86cd8e95cc4032781' ;;
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
    if [[ "$rc" -eq 0 && "${LIMA_MODE:-}" == "qemu-x86_64" && "$(uname -m)" == "arm64" ]]; then
        if curl -fL --retry 3 --retry-delay 2 "$base_url/$guest_archive" -o "$tmpdir/$guest_archive"; then
            verify_archive_sha256 "$tmpdir/$guest_archive" || rc=$?
            if [[ "$rc" -eq 0 ]]; then
                tar -xzf "$tmpdir/$guest_archive" -C "$LOCAL_LIMA_ROOT" || rc=$?
            fi
        else
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


select_lima_mode() {
    local major host_arch
    major=$(macos_major)
    host_arch=$(uname -m)
    LIMA_MODE=""
    LIMA_CREATE_ARGS=(start "--name=${INSTANCE}" --tty=false --mount-writable --containerd=none --disk=20 --set '.audio.device = "none"')

    if [[ "$host_arch" == "arm64" && "$major" -ge 13 ]]; then
        # Native ARM VM through Virtualization.framework. Rosetta translates only
        # the x86_64 Linux userspace binaries, which is substantially faster than
        # emulating an entire x86_64 VM with QEMU.
        LIMA_MODE="vz-rosetta-aarch64"
        LIMA_CREATE_ARGS+=(--vm-type=vz --arch=aarch64 --mount-type=virtiofs --rosetta)
    elif [[ "$host_arch" == "x86_64" && "$major" -ge 13 ]]; then
        LIMA_MODE="vz-x86_64"
        LIMA_CREATE_ARGS+=(--vm-type=vz --arch=x86_64 --mount-type=virtiofs)
    else
        LIMA_MODE="qemu-x86_64"
        LIMA_CREATE_ARGS+=(--vm-type=qemu --arch=x86_64 --mount-type=9p)
    fi
    if [[ -d /var/folders ]]; then
        LIMA_CREATE_ARGS+=(--mount=/var/folders:w)
    fi
}

ensure_host_rosetta_if_needed() {
    [[ "$LIMA_MODE" == "vz-rosetta-aarch64" ]] || return 0
    if /usr/bin/arch -x86_64 /usr/bin/true >/dev/null 2>&1; then
        return 0
    fi
    echo "Installing Apple Rosetta required by the Linux x86_64 plug-in runtime"
    /usr/sbin/softwareupdate --install-rosetta --agree-to-license
    /usr/bin/arch -x86_64 /usr/bin/true >/dev/null 2>&1 || {
        echo "Rosetta installation did not become available" >&2
        return 1
    }
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
    LIMA_CREATE_ARGS=(start "--name=${INSTANCE}" --tty=false --mount-writable --containerd=none --disk=20 --set '.audio.device = "none"' --vm-type=qemu --arch=x86_64 --mount-type=9p)
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
            slicer_linux_runtime_host|slicer_linux_runtime_host_abi1|slicer_linux_runtime_host_abi0|slicer_linux_auth_browser|slicer_linux_auth_browser_x86_64|slicer_linux_auth_browser_aarch64|run_auth_browser.sh|libbambu_networking.so|libBambuSource.so|linux_component_manifest.json|runtime-files.sha256|ca-certificates.crt|slicer_base64.cer|ld-linux-x86-64.so.2|lib*.so|lib*.so.*|*.so|*.so.*)
                atomic_copy_file "$path" "$dst_dir/$base"
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
        liborcastudio_rosetta_splitlock_compat.so
        slicer_linux_auth_browser
        slicer_linux_auth_browser_x86_64
        slicer_linux_auth_browser_aarch64
        run_auth_browser.sh
        runtime-files.sha256
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
    validate_ca_bundle "$src_dir/ca-certificates.crt" "package/ca-certificates.crt"
    validate_runtime_manifest "$src_dir"

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

    validate_ca_bundle "$dst_dir/ca-certificates.crt" "runtime/ca-certificates.crt"
    validate_runtime_manifest "$dst_dir"
    chmod 755 "$dst_dir/slicer_linux_runtime_host" \
        "$dst_dir/slicer_linux_runtime_host_abi1" \
        "$dst_dir/slicer_linux_runtime_host_abi0" \
        "$dst_dir/slicer_linux_auth_browser" \
        "$dst_dir/slicer_linux_auth_browser_x86_64" \
        "$dst_dir/slicer_linux_auth_browser_aarch64" \
        "$dst_dir/run_auth_browser.sh"
    [[ ! -f "$dst_dir/ld-linux-x86-64.so.2" ]] || chmod 755 "$dst_dir/ld-linux-x86-64.so.2"
    chmod 755 "$dst_dir"/*.so "$dst_dir"/*.so.* 2>/dev/null || true
    rm -f "$dst_dir/.selected_host_abi"
}


lima_instance_exists() {
    local instance_names=""
    instance_names=$("$LIMACTL" list --format '{{.Name}}' 2>/dev/null)
    grep -Fxq "$INSTANCE" <<< "$instance_names"
}

lima_shell() {
    "$LIMACTL" shell --workdir=/ "$INSTANCE" -- "$@"
}

delete_lima_instance_if_exists() {
    "$LIMACTL" stop "$INSTANCE" >/dev/null 2>&1 || true
    "$LIMACTL" delete -f "$INSTANCE" >/dev/null 2>&1 || true
}

start_lima_instance() {
    if [[ "$RECREATE_INSTANCE" -eq 1 ]]; then
        delete_lima_instance_if_exists
        RECREATE_INSTANCE=0
    fi

    if lima_shell /usr/bin/env true >/dev/null 2>&1; then
        return 0
    fi

    if lima_instance_exists; then
        "$LIMACTL" start "$INSTANCE" >/dev/null 2>&1 || true
        if ! lima_shell /usr/bin/env true >/dev/null 2>&1; then
            echo "existing Lima instance is not usable - recreating: $INSTANCE"
            delete_lima_instance_if_exists
        else
            return 0
        fi
    fi

    if ! "$LIMACTL" "${LIMA_CREATE_ARGS[@]}" template:ubuntu-24.04; then
        "$LIMACTL" "${LIMA_CREATE_ARGS[@]}" template://ubuntu-24.04 || return 1
    fi

    lima_shell /usr/bin/env true >/dev/null 2>&1
}


ensure_linux_auth_dependencies() {
    local verifier_script
    local provisioner_script

    verifier_script=$(emit_guest_runtime_verifier)
    if lima_shell /bin/bash -s -- <<< "$verifier_script"; then
        return 0
    fi

    # Run exactly the provisioner exercised by CI. Supplying the complete script
    # through a here-string avoids producer-side SIGPIPE and nested shell quoting.
    provisioner_script=$(emit_guest_provisioner)
    if ! lima_shell /bin/bash -s -- install <<< "$provisioner_script"; then
        echo "Ubuntu noble multiarch guest provisioning failed" >&2
        return 1
    fi

    lima_shell /bin/bash -s -- <<< "$verifier_script"
}

guest_auth_browser_path() {
    local guest_arch
    local guest_arch_output=""
    guest_arch_output=$(lima_shell uname -m)
    guest_arch=$(LC_ALL=C awk 'NR == 1 { gsub(/\r/, ""); print }' <<< "$guest_arch_output")
    case "$guest_arch" in
        aarch64|arm64)
            printf '%s\n' "$RUNTIME_DIR/slicer_linux_auth_browser_aarch64"
            ;;
        x86_64|amd64)
            printf '%s\n' "$RUNTIME_DIR/slicer_linux_auth_browser_x86_64"
            ;;
        *)
            echo "unsupported Lima guest architecture: ${guest_arch:-unknown}" >&2
            return 1
            ;;
    esac
}

probe_linux_payload() {
    local wrapper="$COMPONENT_DIR/slicer-linux-runtime-host-wrapper"
    local host="$RUNTIME_DIR/slicer_linux_runtime_host"
    if [[ ! -x "$wrapper" ]]; then
        echo "macOS runtime bridge wrapper is missing: $wrapper" >&2
        return 1
    fi

    local out
    local -a probe_env=(
        "SLICER_LINUX_RUNTIME_REQUIRE_COMPATIBLE_HOST=1"
        "SLICER_LINUX_RUNTIME_MAC_APP_SUPPORT_DIR=$APP_SUPPORT_DIR"
        "SLICER_LINUX_RUNTIME_MAC_RUNTIME_DIR=$RUNTIME_DIR"
        "SLICER_LINUX_RUNTIME_MAC_LIMA_INSTANCE=$INSTANCE"
    )
    if ! out=$(printf x | env "${probe_env[@]}" \
            "$wrapper" "$host" "$RUNTIME_DIR" "$COMPONENT_DIR" --probe-stdio-roundtrip); then
        echo "runtime plug-in compatibility/stdio transaction failed" >&2
        return 1
    fi
    if [[ "$out" != "SLICER_RUNTIME_STDIO_OK" ]]; then
        echo "runtime plug-in load/stdio transaction failed: ${out:-<empty>}" >&2
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

    if ! ensure_linux_auth_dependencies >> "$LOG_DIR/install-probe.log" 2>&1; then
        echo "Linux authentication runtime dependency installation failed" >> "$LOG_DIR/install-probe.log"
        return 1
    fi

    local auth_browser auth_probe auth_self_test
    auth_browser=$(guest_auth_browser_path) || return 1
    if ! lima_shell test -x "$auth_browser"; then
        echo "authentication browser missing for Lima guest: $auth_browser" >> "$LOG_DIR/install-probe.log"
        return 1
    fi

    auth_probe="unset LD_LIBRARY_PATH LD_PRELOAD; $(shell_quote "$auth_browser") --probe"
    if ! lima_shell /bin/sh -lc "$auth_probe" >> "$LOG_DIR/install-probe.log" 2>&1; then
        echo "authentication browser probe failed: $auth_browser" >> "$LOG_DIR/install-probe.log"
        return 1
    fi

    auth_self_test="unset LD_LIBRARY_PATH LD_PRELOAD; xvfb-run -a $(shell_quote "$auth_browser") --self-test"
    if ! lima_shell /bin/sh -lc "$auth_self_test" >> "$LOG_DIR/install-probe.log" 2>&1; then
        echo "authentication browser self-test failed: $auth_browser" >> "$LOG_DIR/install-probe.log"
        return 1
    fi

    printf '%s\n' "$LIMA_MODE" > "$APP_SUPPORT_DIR/lima_mode.txt"
    if linux_component_package_available; then
        if ! probe_linux_payload >> "$LOG_DIR/install-probe.log" 2>&1; then
            echo "Linux component compatibility/stdio probe failed" >> "$LOG_DIR/install-probe.log"
            return 1
        fi
        local marker
        if ! marker=$(component_probe_marker_value); then
            echo "failed to calculate Linux component probe marker" >> "$LOG_DIR/install-probe.log"
            return 1
        fi
        printf '%s\n' "$marker" > "$PROBE_MARKER_FILE"
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
    ensure_qemu_if_needed || return 1
    ensure_lima_installed || return 1
    ensure_additional_guestagents_if_needed || return 1
    try_current_lima_mode
}

if [[ ! -f "$INSTALL_VERSION_FILE" || "$(trim_file "$INSTALL_VERSION_FILE" || true)" != "$INSTALL_VERSION" ]]; then
    REPLACE_EXISTING=1
fi
if [[ "$REPLACE_EXISTING" -eq 1 ]]; then
    rm -rf "$RUNTIME_DIR"
fi
copy_runtime_payload "$COMPONENT_DIR" "$RUNTIME_DIR" "$COMPONENT_CACHE_DIR"
printf '%s\n' 'abi_source=downloaded-plugin' >> "$LOG_DIR/install-probe.log"
select_lima_mode
ensure_host_rosetta_if_needed
if [[ -f "$APP_SUPPORT_DIR/lima_mode.txt" && "$(trim_file "$APP_SUPPORT_DIR/lima_mode.txt" || true)" != "$LIMA_MODE" ]]; then
    RECREATE_INSTANCE=1
fi
ensure_qemu_if_needed
ensure_lima_installed
ensure_additional_guestagents_if_needed

if ! try_current_lima_mode; then
    case "$LIMA_MODE" in
        vz-rosetta-aarch64)
            # Do not silently fall back to full x86_64 system emulation on Apple
            # Silicon. That path is extremely slow and was the cause of the
            # multi-minute apparent application hang. Keep it available only as
            # an explicit diagnostic compatibility override.
            if [[ "${SLICER_LINUX_RUNTIME_MAC_ENABLE_QEMU_FALLBACK:-}" == "1" ]]; then
                if ! try_qemu_fallback; then
                    echo "macOS Lima VZ/Rosetta runtime failed, and the explicitly enabled QEMU fallback also failed; see $LOG_DIR/install-probe.log" >&2
                    exit 1
                fi
            else
                echo "macOS Lima VZ/Rosetta runtime failed; see $LOG_DIR/install-probe.log" >&2
                echo "Rosetta may need to be installed or authorized on this Mac. QEMU fallback is disabled by default because full x86_64 emulation is extremely slow." >&2
                exit 1
            fi
            ;;
        vz-x86_64)
            # On Intel Macs the fallback keeps the native x86_64 architecture,
            # so it does not have the severe foreign-architecture penalty.
            if ! try_qemu_fallback; then
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
