#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
OUTPUT_TAR="${1:-$SCRIPT_DIR/windows-wsl2-rootfs.tar}"
PRIMARY_IMAGE="${SLICER_LINUX_RUNTIME_WSL_ROOTFS_IMAGE:-ubuntu:24.04}"
FORCE_REBUILD="${SLICER_LINUX_RUNTIME_WSL_ROOTFS_FORCE:-0}"

if ! command -v docker >/dev/null 2>&1; then
    echo "docker not found. Install Docker or provide a prebuilt windows-wsl2-rootfs.tar." >&2
    exit 1
fi

mkdir -p "$(dirname -- "$OUTPUT_TAR")"

if [[ "$FORCE_REBUILD" != "1" && -s "$OUTPUT_TAR" ]]; then
    if tar -tf "$OUTPUT_TAR" >/dev/null 2>&1; then
        echo "Using existing WSL rootfs:"
        echo "  $OUTPUT_TAR"
        exit 0
    fi
    echo "Existing WSL rootfs is not a valid tar, rebuilding: $OUTPUT_TAR" >&2
    rm -f "$OUTPUT_TAR"
fi

run_with_retries() {
    local label="$1"
    shift

    local attempt
    for attempt in 1 2 3 4 5; do
        echo "$label attempt $attempt/5"
        if "$@"; then
            return 0
        fi
        if [[ "$attempt" -lt 5 ]]; then
            sleep $((attempt * 15))
        fi
    done

    echo "$label failed after retries" >&2
    return 1
}

IMAGES=("$PRIMARY_IMAGE")
if [[ -z "${SLICER_LINUX_RUNTIME_WSL_ROOTFS_IMAGE:-}" ]]; then
    IMAGES+=("public.ecr.aws/docker/library/ubuntu:24.04")
    IMAGES+=("mcr.microsoft.com/devcontainers/base:ubuntu-24.04")
fi

CONTAINER_NAME="bambu-studio-wsl-rootfs-$(date +%s)-$$"
cleanup() {
    docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
}
trap cleanup EXIT

for image in "${IMAGES[@]}"; do
    echo "Preparing WSL rootfs from image: $image"

    if ! run_with_retries "docker pull $image" docker pull --platform linux/amd64 "$image"; then
        echo "Unable to pull $image, trying next image if available" >&2
        continue
    fi

    docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true

    if ! run_with_retries "docker create $image" docker create --platform linux/amd64 --name "$CONTAINER_NAME" "$image" /bin/sh -lc 'exit 0' >/dev/null; then
        echo "Unable to create container from $image, trying next image if available" >&2
        continue
    fi

    rm -f "$OUTPUT_TAR"
    if ! run_with_retries "docker export $image" docker export "$CONTAINER_NAME" -o "$OUTPUT_TAR"; then
        echo "Unable to export container from $image, trying next image if available" >&2
        rm -f "$OUTPUT_TAR"
        continue
    fi

    if [[ ! -s "$OUTPUT_TAR" ]]; then
        echo "failed to create rootfs tar: $OUTPUT_TAR" >&2
        rm -f "$OUTPUT_TAR"
        continue
    fi

    if ! tar -tf "$OUTPUT_TAR" >/dev/null 2>&1; then
        echo "created rootfs tar is invalid: $OUTPUT_TAR" >&2
        rm -f "$OUTPUT_TAR"
        continue
    fi

    echo "WSL rootfs created:"
    echo "  $OUTPUT_TAR"
    echo "  image: $image"
    exit 0
done

echo "failed to create WSL rootfs from all configured images" >&2
exit 1
