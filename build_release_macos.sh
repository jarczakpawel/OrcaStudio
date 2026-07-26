#!/bin/bash

set -e
set -o pipefail
SECONDS=0

while getopts ":dpa:snt:xbc:i:1Tuh" opt; do
  case "${opt}" in
    d )
        export BUILD_TARGET="deps"
        ;;
    p )
        export PACK_DEPS="1"
        ;;
    a )
        export ARCH="$OPTARG"
        ;;
    s )
        export BUILD_TARGET="slicer"
        ;;
    n )
        export NIGHTLY_BUILD="1"
        ;;
    t )
        export OSX_DEPLOYMENT_TARGET="$OPTARG"
        ;;
    x )
        export SLICER_CMAKE_GENERATOR="Ninja Multi-Config"
        export SLICER_BUILD_TARGET="all"
        export DEPS_CMAKE_GENERATOR="Ninja"
        ;;
    b )
        export BUILD_ONLY="1"
        ;;
    c )
        export BUILD_CONFIG="$OPTARG"
        ;;
    i )
        export CMAKE_IGNORE_PREFIX_PATH="${CMAKE_IGNORE_PREFIX_PATH:+$CMAKE_IGNORE_PREFIX_PATH;}$OPTARG"
        ;;
    1 )
        export CMAKE_BUILD_PARALLEL_LEVEL=1
        ;;
    T )
        export BUILD_TESTS="1"
        ;;
    u )
        export BUILD_TARGET="universal"
        ;;
    h ) echo "Usage: ./build_release_macos.sh [-d]"
        echo "   -d: Build deps only"
        echo "   -a: Set ARCHITECTURE (arm64 or x86_64 or universal)"
        echo "   -s: Build slicer only"
        echo "   -u: Build universal app only (requires existing arm64 and x86_64 app bundles)"
        echo "   -n: Nightly build"
        echo "   -t: Specify minimum version of the target platform, default is 11.3"
        echo "   -x: Use Ninja Multi-Config CMake generator, default is Xcode"
        echo "   -b: Build without reconfiguring CMake"
        echo "   -c: Set CMake build configuration, default is Release"
        echo "   -i: Add a prefix to ignore during CMake dependency discovery (repeatable), defaults to /opt/local:/usr/local:/opt/homebrew"
        echo "   -1: Use single job for building"
        echo "   -T: Build and run tests (set ORCA_TESTS_BUILD_ONLY=1 to build without running)"
        exit 0
        ;;
    * )
        ;;
  esac
done

# Set defaults

if [ -z "$ARCH" ]; then
    ARCH="$(uname -m)"
    export ARCH
fi

if [ -z "$BUILD_CONFIG" ]; then
  export BUILD_CONFIG="Release"
fi

if [ -z "$BUILD_TARGET" ]; then
  export BUILD_TARGET="all"
fi

if [ -z "$SLICER_CMAKE_GENERATOR" ]; then
  export SLICER_CMAKE_GENERATOR="Xcode"
fi

if [ -z "$SLICER_BUILD_TARGET" ]; then
  export SLICER_BUILD_TARGET="ALL_BUILD"
fi

if [ -z "$DEPS_CMAKE_GENERATOR" ]; then
  export DEPS_CMAKE_GENERATOR="Unix Makefiles"
fi

if [ -z "$OSX_DEPLOYMENT_TARGET" ]; then
  export OSX_DEPLOYMENT_TARGET="11.3"
fi

if [ -z "$CMAKE_IGNORE_PREFIX_PATH" ]; then
  export CMAKE_IGNORE_PREFIX_PATH="/opt/local:/usr/local:/opt/homebrew"
fi

is_gnu_m4() {
  [ -n "$1" ] && [ -x "$1" ] && "$1" --gnu --version >/dev/null 2>&1
}

find_gnu_m4() {
  local _brew_prefix=""
  if command -v brew >/dev/null 2>&1; then
    _brew_prefix="$(brew --prefix m4 2>/dev/null || true)"
  fi

  for _m4 in \
    ${M4:-} \
    ${_brew_prefix:+$_brew_prefix/bin/m4} \
    ${_brew_prefix:+$_brew_prefix/bin/gm4} \
    /opt/homebrew/opt/m4/bin/m4 \
    /opt/homebrew/opt/m4/bin/gm4 \
    /usr/local/opt/m4/bin/m4 \
    /usr/local/opt/m4/bin/gm4 \
    /opt/homebrew/bin/m4 \
    /opt/homebrew/bin/gm4 \
    /usr/local/bin/m4 \
    /usr/local/bin/gm4 \
    "$(command -v m4 2>/dev/null || true)" \
    "$(command -v gm4 2>/dev/null || true)"; do
    if is_gnu_m4 "$_m4"; then
      printf '%s\n' "$_m4"
      return 0
    fi
  done

  return 1
}

if [ "$BUILD_TARGET" = "all" ] || [ "$BUILD_TARGET" = "deps" ]; then
  if ! is_gnu_m4 "${M4:-}"; then
    if _found_m4="$(find_gnu_m4)"; then
      export M4="$_found_m4"
    else
      echo "ERROR: GNU m4 not found. Install Homebrew m4 and ensure M4 points to GNU m4."
      exit 1
    fi
  fi

  export PATH="$(dirname "$M4"):$PATH"
  echo " - M4: $M4"
fi

CMAKE_VERSION_OUTPUT=$(cmake --version)
CMAKE_VERSION=$(awk 'NR == 1 { line=$0; sub(/^[^0-9]*/, "", line); sub(/[^0-9].*$/, "", line); print line }' <<< "$CMAKE_VERSION_OUTPUT")
if [ "$CMAKE_VERSION" -ge 4 ] 2>/dev/null; then
  export CMAKE_POLICY_VERSION_MINIMUM=3.5
  export CMAKE_POLICY_COMPAT="-DCMAKE_POLICY_VERSION_MINIMUM=3.5"
  echo "Detected CMake 4.x, adding compatibility flag (env + cmake arg)"
else
  export CMAKE_POLICY_COMPAT=""
fi

echo "Build params:"
echo " - ARCH: $ARCH"
echo " - BUILD_CONFIG: $BUILD_CONFIG"
echo " - BUILD_TARGET: $BUILD_TARGET"
echo " - CMAKE_GENERATOR: $SLICER_CMAKE_GENERATOR for Slicer, $DEPS_CMAKE_GENERATOR for deps"
echo " - OSX_DEPLOYMENT_TARGET: $OSX_DEPLOYMENT_TARGET"
echo " - CMAKE_IGNORE_PREFIX_PATH: $CMAKE_IGNORE_PREFIX_PATH"
echo

# if which -s brew; then
# 	brew --prefix libiconv
# 	brew --prefix zstd
# 	export LIBRARY_PATH=$LIBRARY_PATH:$(brew --prefix zstd)/lib/
# elif which -s port; then
# 	port install libiconv
# 	port install zstd
# 	export LIBRARY_PATH=$LIBRARY_PATH:/opt/local/lib
# else
# 	echo "Need either brew or macports to successfully build deps"
# 	exit 1
# fi

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_BUILD_DIR="$PROJECT_DIR/build/$ARCH"
DEPS_DIR="$PROJECT_DIR/deps"
APP_BUNDLE_NAME="OrcaStudio.app"
PACKAGE_OUTPUT_DIR_NAME="OrcaStudio"
LEGACY_APP_BUNDLE_NAME="BambuStudio.app"
SECONDARY_LEGACY_APP_BUNDLE_NAME="OrcaSlicer.app"

# For Multi-config generators like Ninja and Xcode
export BUILD_DIR_CONFIG_SUBDIR="/$BUILD_CONFIG"

function build_deps() {
    # iterate over two architectures: x86_64 and arm64
    for _ARCH in x86_64 arm64; do
        # if ARCH is universal or equal to _ARCH
        if [ "$ARCH" == "universal" ] || [ "$ARCH" == "$_ARCH" ]; then

            PROJECT_BUILD_DIR="$PROJECT_DIR/build/$_ARCH"
            DEPS_BUILD_DIR="$DEPS_DIR/build/$_ARCH"
            DEPS="$DEPS_BUILD_DIR/OrcaSlicer_dep"

            echo "Building deps..."
            (
                set -x
                mkdir -p "$DEPS"
                cd "$DEPS_BUILD_DIR"
                if [ "1." != "$BUILD_ONLY". ]; then
                    cmake "${DEPS_DIR}" \
                        -G "${DEPS_CMAKE_GENERATOR}" \
                        -DCMAKE_BUILD_TYPE="$BUILD_CONFIG" \
                        -DCMAKE_OSX_ARCHITECTURES:STRING="${_ARCH}" \
                        -DCMAKE_OSX_DEPLOYMENT_TARGET="${OSX_DEPLOYMENT_TARGET}" \
                        -DCMAKE_IGNORE_PREFIX_PATH="${CMAKE_IGNORE_PREFIX_PATH}" \
                        ${CMAKE_POLICY_COMPAT}
                fi
                cmake --build . --config "$BUILD_CONFIG" --target deps
            )
        fi
    done
}

function pack_deps() {
    echo "Packing deps..."
    (
        set -x
        cd "$DEPS_DIR"
        tar -zcvf "OrcaSlicer_dep_mac_${ARCH}_$(date +"%Y%m%d").tar.gz" "build"
    )
}

# codesign cannot seal the runtime's dotted directories (include/python3.12,
# lib/python3.12) anywhere under Contents/MacOS -- it mistakes any dotted
# directory there for a nested bundle and fails with "bundle format
# unrecognized" -- so packaged apps ship the runtime under Contents/Resources
# with a compatibility symlink that keeps every Contents/MacOS/python path and
# the @executable_path/python/lib rpath resolving unchanged.
function relocate_python_runtime() {
    local app="$1"
    local pydir="$app/Contents/MacOS/python"
    if [ -d "$pydir" ] && [ ! -L "$pydir" ]; then
        rm -rf "$app/Contents/Resources/python"
        mv "$pydir" "$app/Contents/Resources/python"
        ln -s ../Resources/python "$pydir"
    fi
}

# --- Bundled Python runtime verification --------------------------------------
# Relocation is handled at the source: deps/python3/python3.cmake stamps
# libpython with an @rpath id and src/CMakeLists.txt gives the app a matching
# rpath. This gate catches regressions that would otherwise only surface as
# launch failures on end users' machines (the absolute deps path still exists
# on the build host, so a plain run can pass while relocation is broken --
# hence the otool checks). The x86_64 leg runs under Rosetta on arm64 hosts.
function verify_python_runtime() {
    local app="$1"
    local pydir="$app/Contents/MacOS/python"
    [ -d "$pydir" ] || return 0  # app doesn't bundle Python (e.g. profile validator)
    if [ ! -L "$pydir" ]; then
        echo "ERROR: Contents/MacOS/python must be a symlink into Contents/Resources" >&2
        echo "       (see relocate_python_runtime in this script)" >&2
        exit 1
    fi
    # Version-agnostic interpreter name so a CPython version bump cannot
    # silently skip the gate; if the dir exists the interpreter must too.
    local pybin="$pydir/bin/python3"
    if [ ! -x "$pybin" ]; then
        echo "ERROR: bundled python/ present but no interpreter at $pybin" >&2
        exit 1
    fi
    echo "  Verifying bundled Python runtime in $(basename "$app")..."
    local bad
    bad=$(otool -arch all -L "$pybin" "$app/Contents/MacOS/OrcaStudio" | grep "libpython" | grep -v "@rpath/" || true)
    if [ -n "$bad" ]; then
        echo "ERROR: a bundled binary references libpython by absolute path (relocation regression):" >&2
        echo "$bad" >&2
        exit 1
    fi
    # otool -L shows load commands only; assert the consumer rpath separately.
    # Its loss is masked on the build host by CMake's absolute build-tree rpath.
    if ! otool -arch all -l "$app/Contents/MacOS/OrcaStudio" | grep -q "path @executable_path/python/lib "; then
        echo "ERROR: OrcaStudio lacks the @executable_path/python/lib rpath (relocation regression)" >&2
        exit 1
    fi
    if ! "$pybin" -c "import ssl"; then
        echo "ERROR: bundled Python failed to start (libpython relocation broken," >&2
        echo "       or missing Rosetta 2 for the x86_64 leg?)" >&2
        exit 1
    fi
}

function build_slicer() {
    # iterate over two architectures: x86_64 and arm64
    for _ARCH in x86_64 arm64; do
        # if ARCH is universal or equal to _ARCH
        if [ "$ARCH" == "universal" ] || [ "$ARCH" == "$_ARCH" ]; then

            PROJECT_BUILD_DIR="$PROJECT_DIR/build/$_ARCH"
            DEPS_BUILD_DIR="$DEPS_DIR/build/$_ARCH"
            DEPS="$DEPS_BUILD_DIR/OrcaSlicer_dep"

            echo "Building slicer for $_ARCH..."
            (
                set -x
            mkdir -p "$PROJECT_BUILD_DIR"
            cd "$PROJECT_BUILD_DIR"
            if [ "1." != "$BUILD_ONLY". ]; then
                cmake "${PROJECT_DIR}" \
                    -G "${SLICER_CMAKE_GENERATOR}" \
                    -DORCA_TOOLS=ON \
                    -DBBL_RELEASE_TO_PUBLIC=1 \
                    -DBBL_INTERNAL_TESTING=0 \
                    ${ORCA_UPDATER_SIG_KEY:+-DORCA_UPDATER_SIG_KEY="$ORCA_UPDATER_SIG_KEY"} \
                    ${BUILD_TESTS:+-DBUILD_TESTS=ON} \
                    -DCMAKE_BUILD_TYPE="$BUILD_CONFIG" \
                    -DCMAKE_OSX_ARCHITECTURES="${_ARCH}" \
                    -DCMAKE_OSX_DEPLOYMENT_TARGET="${OSX_DEPLOYMENT_TARGET}" \
                    -DCMAKE_PREFIX_PATH="$DEPS/usr/local" \
                    -DCMAKE_IGNORE_PREFIX_PATH="${CMAKE_IGNORE_PREFIX_PATH}" \
                    ${CMAKE_POLICY_COMPAT}
            fi
            cmake --build . --config "$BUILD_CONFIG" --target "$SLICER_BUILD_TARGET"
        )

        # -T also runs the tests; ORCA_TESTS_BUILD_ONLY=1 builds them without
        # running, so CI can build here and run them in a dedicated job.
        if [ "1." == "$BUILD_TESTS". ] && [ "1." != "$ORCA_TESTS_BUILD_ONLY". ]; then
            "$PROJECT_DIR/scripts/run_unit_tests.sh" "build/$_ARCH/tests" "$BUILD_CONFIG"
        fi

        echo "Verify localization with gettext..."
        (
            cd "$PROJECT_DIR"
            ./scripts/run_gettext.sh
        )

        echo "Fix macOS app package..."
        (
            cd "$PROJECT_BUILD_DIR"
            mkdir -p "$PACKAGE_OUTPUT_DIR_NAME"
            cd "$PACKAGE_OUTPUT_DIR_NAME"
            built_app="../src$BUILD_DIR_CONFIG_SUBDIR/$APP_BUNDLE_NAME"
            if [ ! -d "$built_app" ]; then
                built_app="../src$BUILD_DIR_CONFIG_SUBDIR/$LEGACY_APP_BUNDLE_NAME"
            fi
            if [ ! -d "$built_app" ]; then
                built_app="../src$BUILD_DIR_CONFIG_SUBDIR/$SECONDARY_LEGACY_APP_BUNDLE_NAME"
            fi
            if [ ! -d "$built_app" ]; then
                echo "Built app bundle not found: $APP_BUNDLE_NAME, $LEGACY_APP_BUNDLE_NAME or $SECONDARY_LEGACY_APP_BUNDLE_NAME"
                exit 1
            fi
            rm -rf ./$APP_BUNDLE_NAME ./$LEGACY_APP_BUNDLE_NAME ./$SECONDARY_LEGACY_APP_BUNDLE_NAME
            cp -pR "$built_app" ./$APP_BUNDLE_NAME
            resources_path=$(readlink ./$APP_BUNDLE_NAME/Contents/Resources)
            rm ./$APP_BUNDLE_NAME/Contents/Resources
            cp -R "$resources_path" ./$APP_BUNDLE_NAME/Contents/Resources
            relocate_python_runtime "./$APP_BUNDLE_NAME"

            runtime_dst="./$APP_BUNDLE_NAME/Contents/Resources/plugins"
            runtime_compat_link="./$APP_BUNDLE_NAME/Contents/MacOS/plugins"
            rm -rf "$runtime_dst" "$runtime_compat_link"
            mkdir -p "$runtime_dst"
            ln -s ../Resources/plugins "$runtime_compat_link"
            runtime_marker=""
            runtime_candidates=(
                "../src/slic3r/Utils/SlicerLinuxRuntime$BUILD_DIR_CONFIG_SUBDIR/libslicer_linux_runtime.dylib"
                "../src/slic3r/Utils/SlicerLinuxRuntime/$BUILD_CONFIG/libslicer_linux_runtime.dylib"
                "../src$BUILD_DIR_CONFIG_SUBDIR/libslicer_linux_runtime.dylib"
            )
            for candidate in "${runtime_candidates[@]}"; do
                if [ -f "$candidate" ]; then
                    runtime_marker="$candidate"
                    break
                fi
            done
            if [ -z "$runtime_marker" ]; then
                runtime_marker=$(find ../src -path "*/SlicerLinuxRuntime/*/libslicer_linux_runtime.dylib" -type f -print -quit)
            fi
            if [ -z "$runtime_marker" ]; then
                runtime_marker=$(find ../src -name "libslicer_linux_runtime.dylib" -type f -print -quit)
            fi
            if [ -z "$runtime_marker" ]; then
                echo "Missing macOS Linux runtime library: libslicer_linux_runtime.dylib"
                find ../src -maxdepth 8 -name "libslicer_linux_runtime.dylib" -print || true
                exit 1
            fi
            runtime_src=$(cd "$(dirname "$runtime_marker")" && pwd)
            echo "macOS Linux runtime package source: $runtime_src"
            runtime_required=(
                "libslicer_linux_runtime.dylib"
                "slicer-linux-runtime-host-wrapper"
                "install_runtime_macos.sh"
                "verify_runtime_macos.sh"
                "slicer_linux_runtime_lima_instance.txt"
                "slicer_linux_runtime_host"
                "slicer_linux_runtime_host_abi1"
                "slicer_linux_runtime_host_abi0"
                "slicer_linux_auth_browser"
                "slicer_linux_auth_browser_x86_64"
                "slicer_linux_auth_browser_aarch64"
                "run_auth_browser.sh"
                "runtime-files.sha256"
                "ca-certificates.crt"
                "slicer_base64.cer"
                "ld-linux-x86-64.so.2"
                "libc.so.6"
                "libm.so.6"
                "libresolv.so.2"
                "libnss_dns.so.2"
                "libnss_files.so.2"
                "libstdc++.so.6"
                "libgcc_s.so.1"
                "libz.so.1"
            )
            mkdir -p "$runtime_dst"
            rm -f \
                "$runtime_dst/libbambu_networking.so" \
                "$runtime_dst/libBambuSource.so" \
                "$runtime_dst/liblive555.so" \
                "$runtime_dst/libagora_rtc_sdk.so" \
                "$runtime_dst/libagora-fdkaac.so" \
                "$runtime_dst/linux_component_manifest.json" \
                "$runtime_dst/network_plugins.json"
            for runtime_file in "${runtime_required[@]}"; do
                if [ ! -f "$runtime_src/$runtime_file" ]; then
                    echo "Missing macOS Linux runtime file: $runtime_src/$runtime_file"
                    exit 1
                fi
                cp -f "$runtime_src/$runtime_file" "$runtime_dst/$runtime_file"
            done
            find "$runtime_src" -maxdepth 1 -type f \( -name '*.so' -o -name '*.so.*' \) \
                ! -name 'libbambu_networking.so' \
                ! -name 'libBambuSource.so' \
                ! -name 'liblive555.so' \
                ! -name 'libagora_rtc_sdk.so' \
                ! -name 'libagora-fdkaac.so' \
                -exec cp -f {} "$runtime_dst/" \;

            cp -f "$PROJECT_DIR/tools/slicer_linux_runtime_host/slicer-linux-runtime-host-wrapper" "$runtime_dst/slicer-linux-runtime-host-wrapper"
            cp -f "$PROJECT_DIR/tools/slicer_linux_runtime/macos/install_runtime_macos.sh" "$runtime_dst/install_runtime_macos.sh"
            cp -f "$PROJECT_DIR/tools/slicer_linux_runtime/macos/verify_runtime_macos.sh" "$runtime_dst/verify_runtime_macos.sh"
            cp -f "$PROJECT_DIR/tools/slicer_linux_runtime/macos/slicer_linux_runtime_lima_instance.txt" "$runtime_dst/slicer_linux_runtime_lima_instance.txt"

            chmod 755 \
                "$runtime_dst/slicer-linux-runtime-host-wrapper" \
                "$runtime_dst/install_runtime_macos.sh" \
                "$runtime_dst/verify_runtime_macos.sh" \
                "$runtime_dst/slicer_linux_runtime_host" \
                "$runtime_dst/slicer_linux_runtime_host_abi1" \
                "$runtime_dst/slicer_linux_runtime_host_abi0" \
                "$runtime_dst/slicer_linux_auth_browser" \
                "$runtime_dst/slicer_linux_auth_browser_x86_64" \
                "$runtime_dst/slicer_linux_auth_browser_aarch64" \
                "$runtime_dst/run_auth_browser.sh" \
                "$runtime_dst/ld-linux-x86-64.so.2"

            resources_cert_dst="./$APP_BUNDLE_NAME/Contents/Resources/cert"
            mkdir -p "$resources_cert_dst"
            if [ -f "$runtime_src/slicer_base64.cer" ]; then
                cp -f "$runtime_src/slicer_base64.cer" "$resources_cert_dst/slicer_base64.cer"
            elif [ -f "$PROJECT_DIR/resources/cert/slicer_base64.cer" ]; then
                cp -f "$PROJECT_DIR/resources/cert/slicer_base64.cer" "$resources_cert_dst/slicer_base64.cer"
            else
                echo "Missing slicer_base64.cer for Contents/Resources/cert"
                exit 1
            fi
            if [ -f "$runtime_src/ca-certificates.crt" ]; then
                cp -f "$runtime_src/ca-certificates.crt" "$resources_cert_dst/ca-certificates.crt"
            elif [ -f "$PROJECT_DIR/resources/cert/ca-certificates.crt" ]; then
                cp -f "$PROJECT_DIR/resources/cert/ca-certificates.crt" "$resources_cert_dst/ca-certificates.crt"
            fi
            echo "macOS cert files after packaging:"
            ls -la "$resources_cert_dst" || true
            ls -la "$runtime_dst"/slicer_base64.cer "$runtime_dst"/ca-certificates.crt 2>/dev/null || true

            frameworks_dst="./$APP_BUNDLE_NAME/Contents/Frameworks"
            mkdir -p "$frameworks_dst"
            deps_lib_dirs=("$DEPS/usr/local/lib" "$DEPS/lib")
            for ffmpeg_pattern in libavcodec*.dylib libavutil*.dylib libswscale*.dylib libswresample*.dylib; do
                found_ffmpeg=0
                for deps_lib_dir in "${deps_lib_dirs[@]}"; do
                    [ -d "$deps_lib_dir" ] || continue
                    for ffmpeg_lib in "$deps_lib_dir"/$ffmpeg_pattern; do
                        if [ -f "$ffmpeg_lib" ]; then
                            cp -f "$ffmpeg_lib" "$frameworks_dst/$(basename "$ffmpeg_lib")"
                            found_ffmpeg=1
                        fi
                    done
                done
                if [ "$found_ffmpeg" -eq 0 ] && [ "$ffmpeg_pattern" != "libswresample*.dylib" ]; then
                    echo "Missing macOS FFmpeg runtime file matching ${deps_lib_dirs[*]} / $ffmpeg_pattern"
                    exit 1
                fi
            done

            is_macho_file() {
                local target="$1"
                [ -f "$target" ] || return 1
                file "$target" | grep -q "Mach-O"
            }

            list_macho_rpaths() {
                local target="$1"
                otool -l "$target" | awk '
                    $1 == "cmd" && $2 == "LC_RPATH" { in_rpath = 1; next }
                    in_rpath && $1 == "path" {
                        line = $0
                        sub(/^[[:space:]]*path /, "", line)
                        sub(/ \(offset [0-9]+\)$/, "", line)
                        print line
                        in_rpath = 0
                    }
                '
            }

            add_rpath_if_missing() {
                local target="$1"
                local rpath="$2"
                is_macho_file "$target" || return 0
                list_macho_rpaths "$target" | grep -Fx "$rpath" >/dev/null 2>&1 && return 0
                install_name_tool -add_rpath "$rpath" "$target"
            }

            remove_nonportable_rpaths() {
                local target="$1"
                local rpath
                is_macho_file "$target" || return 0
                while IFS= read -r rpath; do
                    [ -n "$rpath" ] || continue
                    case "$rpath" in
                        @*) continue ;;
                    esac
                    install_name_tool -delete_rpath "$rpath" "$target"
                done < <(list_macho_rpaths "$target" | awk '!seen[$0]++')
            }

            is_system_macos_dependency() {
                local dep="$1"
                case "$dep" in
                    @rpath/*|@loader_path/*|@executable_path/*|/usr/lib/*|/System/Library/*)
                        return 0
                        ;;
                esac
                return 1
            }

            fix_bundle_ref_if_present() {
                local target="$1"
                local dep="$2"
                local dep_base
                dep_base="$(basename "$dep")"
                if [ -f "$frameworks_dst/$dep_base" ]; then
                    install_name_tool -change "$dep" "@rpath/$dep_base" "$target" 2>/dev/null || true
                fi
            }

            is_macho_rewrite_exempt() {
                local target="$1"
                case "$target" in
                    */Contents/MacOS/tools/uv/uv|*/Contents/Resources/tools/uv/uv)
                        return 0
                        ;;
                esac
                return 1
            }

            normalize_macho_file() {
                local target="$1"
                is_macho_file "$target" || return 0

                # The pinned uv release binary is already self-contained and has
                # no Mach-O header padding for additional LC_RPATH commands.
                # Leave its load commands intact; later gates still inspect its
                # dependencies/rpaths and sign it as a nested executable.
                if is_macho_rewrite_exempt "$target"; then
                    return 0
                fi

                case "$(basename "$target")" in
                    *.dylib)
                        install_name_tool -id "@rpath/$(basename "$target")" "$target" 2>/dev/null || true
                        ;;
                esac

                remove_nonportable_rpaths "$target"
                add_rpath_if_missing "$target" "@executable_path/../Frameworks"
                add_rpath_if_missing "$target" "@loader_path"
                add_rpath_if_missing "$target" "@loader_path/../Frameworks"
                add_rpath_if_missing "$target" "@loader_path/../../Frameworks"

                while IFS= read -r dep; do
                    [ -n "$dep" ] || continue
                    fix_bundle_ref_if_present "$target" "$dep"
                done < <(otool -L "$target" | awk 'NR > 1 {print $1}')
            }

            bundle_transitive_framework_deps() {
                local changed=1
                local pass=0
                while [ "$changed" -eq 1 ]; do
                    changed=0
                    pass=$((pass + 1))
                    echo "Bundling transitive macOS dylib dependencies, pass $pass"

                    while IFS= read -r -d '' macho_file; do
                        is_macho_file "$macho_file" || continue
                        if is_macho_rewrite_exempt "$macho_file"; then
                            echo "Preserving prebuilt Mach-O load commands: $macho_file"
                            continue
                        fi
                        normalize_macho_file "$macho_file"

                        while IFS= read -r dep; do
                            [ -n "$dep" ] || continue
                            is_system_macos_dependency "$dep" && continue

                            dep_base="$(basename "$dep")"
                            dep_dst="$frameworks_dst/$dep_base"

                            if [ -f "$dep" ]; then
                                if [ ! -f "$dep_dst" ]; then
                                    echo "Bundling macOS dependency: $dep -> $dep_dst"
                                    cp -f "$dep" "$dep_dst"
                                    chmod u+w "$dep_dst" 2>/dev/null || true
                                    changed=1
                                fi
                                install_name_tool -change "$dep" "@rpath/$dep_base" "$macho_file" 2>/dev/null || true
                            elif [ -f "$dep_dst" ]; then
                                install_name_tool -change "$dep" "@rpath/$dep_base" "$macho_file" 2>/dev/null || true
                            else
                                echo "WARNING: unresolved non-system dependency for $macho_file: $dep"
                            fi
                        done < <(otool -L "$macho_file" | awk 'NR > 1 {print $1}')
                    done < <(find "./$APP_BUNDLE_NAME" -type f -print0)

                    if [ "$pass" -gt 30 ]; then
                        echo "ERROR: too many dependency bundling passes"
                        exit 1
                    fi
                done

                while IFS= read -r -d '' macho_file; do
                    is_macho_rewrite_exempt "$macho_file" && continue
                    normalize_macho_file "$macho_file"
                done < <(find "./$APP_BUNDLE_NAME" -type f -print0)
            }

            bundle_transitive_framework_deps

            echo "Verifying that packaged Mach-O files do not reference build-host libraries..."
            unresolved_macos_deps=0
            while IFS= read -r -d '' candidate; do
                is_macho_file "$candidate" || continue
                while IFS= read -r dep; do
                    [ -n "$dep" ] || continue
                    case "$dep" in
                        @rpath/*|@loader_path/*|@executable_path/*|/usr/lib/*|/System/Library/*)
                            ;;
                        *)
                            echo "ERROR: unresolved external macOS dependency for $candidate: $dep" >&2
                            unresolved_macos_deps=1
                            ;;
                    esac
                done < <(otool -L "$candidate" | awk 'NR > 1 {print $1}')
                while IFS= read -r rpath; do
                    [ -n "$rpath" ] || continue
                    case "$rpath" in
                        @*) ;;
                        *)
                            echo "ERROR: non-portable macOS LC_RPATH for $candidate: $rpath" >&2
                            unresolved_macos_deps=1
                            ;;
                    esac
                done < <(list_macho_rpaths "$candidate")
            done < <(find "./$APP_BUNDLE_NAME" -type f -print0)
            if [ "$unresolved_macos_deps" -ne 0 ]; then
                exit 1
            fi

            echo "macOS media/runtime dylib references after packaging:"
            while IFS= read -r -d '' candidate; do
                if is_macho_file "$candidate" && otool -L "$candidate" | grep -E 'libavcodec|libavutil|libswscale|libswresample|libX11|libxcb|libXau|libXdmcp' >/dev/null 2>&1; then
                    echo "-- $candidate"
                    otool -L "$candidate" | grep -E 'libavcodec|libavutil|libswscale|libswresample|libX11|libxcb|libXau|libXdmcp'
                fi
            done < <(find "./$APP_BUNDLE_NAME" -type f -print0)

            find ./$APP_BUNDLE_NAME/ -name '.DS_Store' -delete
            verify_python_runtime "./$APP_BUNDLE_NAME"
            
            # Copy OrcaSlicer_profile_validator.app if it exists
            if [ -f "../src$BUILD_DIR_CONFIG_SUBDIR/OrcaSlicer_profile_validator.app/Contents/MacOS/OrcaSlicer_profile_validator" ]; then
                echo "Copying OrcaSlicer_profile_validator.app..."
                rm -rf ./OrcaSlicer_profile_validator.app
                cp -pR "../src$BUILD_DIR_CONFIG_SUBDIR/OrcaSlicer_profile_validator.app" ./OrcaSlicer_profile_validator.app
                # delete .DS_Store file
                find ./OrcaSlicer_profile_validator.app/ -name '.DS_Store' -delete
                verify_python_runtime ./OrcaSlicer_profile_validator.app
            fi

            sign_identity="${MACOS_CODESIGN_IDENTITY:-${CERTIFICATE_ID:-}}"
            if [ -n "$sign_identity" ]; then
                echo "Signing macOS applications with identity: $sign_identity"
            else
                sign_identity="-"
                echo "Signing macOS applications with ad-hoc identity"
            fi

            if [ -d ./OrcaSlicer_profile_validator.app ]; then
                "$PROJECT_DIR/scripts/sign_macos_app.sh" \
                    ./OrcaSlicer_profile_validator.app \
                    "$sign_identity"
            fi
            "$PROJECT_DIR/scripts/sign_macos_app.sh" \
                ./$APP_BUNDLE_NAME \
                "$sign_identity" \
                "$PROJECT_DIR/scripts/disable_validation.entitlements"
            "$PROJECT_DIR/scripts/verify_macos_app_launch.sh" ./$APP_BUNDLE_NAME
        )

        # extract version
        # export ver=$(grep '^#define SoftFever_VERSION' ../src/libslic3r/libslic3r_version.h | cut -d ' ' -f3)
        # ver="_V${ver//\"}"
        # echo $PWD
        # if [ "1." != "$NIGHTLY_BUILD". ];
        # then
        #     ver=${ver}_dev
        # fi

        # zip -FSr OrcaSlicer${ver}_Mac_${_ARCH}.zip OrcaSlicer.app

    fi
    done
}

function lipo_dir() {
    local universal_dir="$1"
    local x86_64_dir="$2"

    # Find all Mach-O files in the universal (arm64-based) copy and lipo them
    while IFS= read -r -d '' f; do
        local rel="${f#"$universal_dir"/}"
        local x86="$x86_64_dir/$rel"
        if [ -f "$x86" ]; then
            echo "  lipo: $rel"
            lipo -create "$f" "$x86" -output "$f.tmp"
            mv "$f.tmp" "$f"
        else
            echo "  warning: no x86_64 counterpart for $rel, keeping arm64 only"
        fi
    done < <(find "$universal_dir" -type f -print0 | while IFS= read -r -d '' candidate; do
        if file "$candidate" | grep -q "Mach-O"; then
            printf '%s\0' "$candidate"
        fi
    done)
}

function build_universal() {
    echo "Building universal binary..."

    PROJECT_BUILD_DIR="$PROJECT_DIR/build/$ARCH"
    ARM64_APP="$PROJECT_DIR/build/arm64/$PACKAGE_OUTPUT_DIR_NAME/$APP_BUNDLE_NAME"
    X86_64_APP="$PROJECT_DIR/build/x86_64/$PACKAGE_OUTPUT_DIR_NAME/$APP_BUNDLE_NAME"

    mkdir -p "$PROJECT_BUILD_DIR/$PACKAGE_OUTPUT_DIR_NAME"
    UNIVERSAL_APP="$PROJECT_BUILD_DIR/$PACKAGE_OUTPUT_DIR_NAME/$APP_BUNDLE_NAME"
    rm -rf "$UNIVERSAL_APP"
    cp -R "$ARM64_APP" "$UNIVERSAL_APP"

    echo "Creating universal binaries for $APP_BUNDLE_NAME..."
    lipo_dir "$UNIVERSAL_APP" "$X86_64_APP"
    echo "Universal $APP_BUNDLE_NAME created at $UNIVERSAL_APP"
    verify_python_runtime "$UNIVERSAL_APP"

    # Create universal binary for profile validator if it exists
    ARM64_VALIDATOR="$PROJECT_DIR/build/arm64/$PACKAGE_OUTPUT_DIR_NAME/OrcaSlicer_profile_validator.app"
    X86_64_VALIDATOR="$PROJECT_DIR/build/x86_64/$PACKAGE_OUTPUT_DIR_NAME/OrcaSlicer_profile_validator.app"
    if [ -d "$ARM64_VALIDATOR" ] && [ -d "$X86_64_VALIDATOR" ]; then
        echo "Creating universal binaries for OrcaSlicer_profile_validator.app..."
        UNIVERSAL_VALIDATOR_APP="$PROJECT_BUILD_DIR/$PACKAGE_OUTPUT_DIR_NAME/OrcaSlicer_profile_validator.app"
        rm -rf "$UNIVERSAL_VALIDATOR_APP"
        cp -R "$ARM64_VALIDATOR" "$UNIVERSAL_VALIDATOR_APP"
        lipo_dir "$UNIVERSAL_VALIDATOR_APP" "$X86_64_VALIDATOR"
        echo "Universal OrcaSlicer_profile_validator.app created at $UNIVERSAL_VALIDATOR_APP"
    fi
}

case "${BUILD_TARGET}" in
    all)
        build_deps
        build_slicer
        ;;
    deps)
        build_deps
        ;;
    slicer)
        build_slicer
        ;;
    universal)
        build_universal
        ;;
    *)
        echo "Unknown target: $BUILD_TARGET. Available targets: deps, slicer, universal, all."
        exit 1
        ;;
esac

if [ "$ARCH" = "universal" ] && { [ "$BUILD_TARGET" = "all" ] || [ "$BUILD_TARGET" = "slicer" ]; }; then
    build_universal
fi

if [ "1." == "$PACK_DEPS". ]; then
    pack_deps
fi

elapsed=$SECONDS
printf "\nBuild completed in %dh %dm %ds\n" $((elapsed/3600)) $((elapsed%3600/60)) $((elapsed%60))
