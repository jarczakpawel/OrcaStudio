#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
OUTPUT_TAR="${1:-$SCRIPT_DIR/windows-wsl2-rootfs.tar}"
PRIMARY_IMAGE="${SLICER_LINUX_RUNTIME_WSL_ROOTFS_IMAGE:-ubuntu:24.04}"
FORCE_REBUILD="${SLICER_LINUX_RUNTIME_WSL_ROOTFS_FORCE:-0}"
ROOTFS_MARKER="ubuntu-24.04-linux-auth-v3"

for required_command in docker tar python3; do
    if ! command -v "$required_command" >/dev/null 2>&1; then
        echo "$required_command not found. It is required to build and validate windows-wsl2-rootfs.tar." >&2
        exit 1
    fi
done

mkdir -p "$(dirname -- "$OUTPUT_TAR")"

PREPARE_SCRIPT="$SCRIPT_DIR/prepare_windows_wsl_rootfs.sh"
VALIDATOR_SCRIPT="$SCRIPT_DIR/validate_windows_wsl_rootfs.py"
for required_file in "$PREPARE_SCRIPT" "$VALIDATOR_SCRIPT"; do
    if [[ ! -f "$required_file" ]]; then
        echo "required WSL rootfs support file is missing: $required_file" >&2
        exit 1
    fi
done

validate_rootfs_tar() {
    python3 "$SCRIPT_DIR/validate_windows_wsl_rootfs.py" "$1" "$ROOTFS_MARKER"
}

if [[ "$FORCE_REBUILD" != "1" && -s "$OUTPUT_TAR" ]]; then
    if validate_rootfs_tar "$OUTPUT_TAR"; then
        echo "Using existing WSL rootfs:"
        echo "  $OUTPUT_TAR"
        exit 0
    fi
    echo "Existing WSL rootfs is stale or invalid, rebuilding: $OUTPUT_TAR" >&2
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

    if ! run_with_retries "docker create $image" docker create --platform linux/amd64 --user 0:0 --name "$CONTAINER_NAME" "$image" /bin/sh -lc 'trap : TERM INT; sleep infinity & wait' >/dev/null; then
        echo "Unable to create container from $image, trying next image if available" >&2
        continue
    fi

    if ! run_with_retries "docker start $image" docker start "$CONTAINER_NAME" >/dev/null; then
        echo "Unable to start container from $image, trying next image if available" >&2
        docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
        continue
    fi
    if ! run_with_retries "copy WSL rootfs preparation script" \
        docker cp "$PREPARE_SCRIPT" "${CONTAINER_NAME}:/tmp/orcastudio-prepare-rootfs.sh"; then
        echo "Unable to copy the WSL rootfs preparation script into $image, trying next image if available" >&2
        docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
        continue
    fi

    if ! run_with_retries "prepare Linux auth runtime dependencies" docker exec \
        -e DEBIAN_FRONTEND=noninteractive "$CONTAINER_NAME" /bin/sh -lc \
        'sed -i "s/\r$//" /tmp/orcastudio-prepare-rootfs.sh; exec /bin/sh /tmp/orcastudio-prepare-rootfs.sh "$1"' \
        sh "$ROOTFS_MARKER"; then
        echo "Unable to prepare runtime dependencies in $image, trying next image if available" >&2
        docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
        continue
    fi
    if ! run_with_retries "docker stop $image" docker stop "$CONTAINER_NAME" >/dev/null; then
        echo "Unable to stop prepared container from $image, trying next image if available" >&2
        docker rm -f "$CONTAINER_NAME" >/dev/null 2>&1 || true
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

    if ! validate_rootfs_tar "$OUTPUT_TAR"; then
        echo "created rootfs tar is invalid or missing Linux auth runtime marker/dependencies: $OUTPUT_TAR" >&2
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
