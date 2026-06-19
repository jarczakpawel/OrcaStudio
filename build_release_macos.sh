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
        echo "   -T: Build and run tests"
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

CMAKE_VERSION=$(cmake --version | head -1 | sed 's/[^0-9]*\([0-9]*\).*/\1/')
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
APP_BUNDLE_NAME="BambuStudio.app"
LEGACY_APP_BUNDLE_NAME="OrcaSlicer.app"

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
                    -DCMAKE_IGNORE_PREFIX_PATH="${CMAKE_IGNORE_PREFIX_PATH}" \
                    ${CMAKE_POLICY_COMPAT}
            fi
            cmake --build . --config "$BUILD_CONFIG" --target "$SLICER_BUILD_TARGET"
        )

        if [ "1." == "$BUILD_TESTS". ]; then
            echo "Running tests for $_ARCH..."
            (
                set -x
                cd "$PROJECT_BUILD_DIR"
                ctest --build-config "$BUILD_CONFIG" --output-on-failure
            )
        fi

        echo "Verify localization with gettext..."
        (
            cd "$PROJECT_DIR"
            ./scripts/run_gettext.sh
        )

        echo "Fix macOS app package..."
        (
            cd "$PROJECT_BUILD_DIR"
            mkdir -p OrcaSlicer
            cd OrcaSlicer
            built_app="../src$BUILD_DIR_CONFIG_SUBDIR/$APP_BUNDLE_NAME"
            if [ ! -d "$built_app" ]; then
                built_app="../src$BUILD_DIR_CONFIG_SUBDIR/$LEGACY_APP_BUNDLE_NAME"
            fi
            if [ ! -d "$built_app" ]; then
                echo "Built app bundle not found: $APP_BUNDLE_NAME or $LEGACY_APP_BUNDLE_NAME"
                exit 1
            fi
            rm -rf ./$APP_BUNDLE_NAME ./$LEGACY_APP_BUNDLE_NAME
            cp -pR "$built_app" ./$APP_BUNDLE_NAME
            resources_path=$(readlink ./$APP_BUNDLE_NAME/Contents/Resources)
            rm ./$APP_BUNDLE_NAME/Contents/Resources
            cp -R "$resources_path" ./$APP_BUNDLE_NAME/Contents/Resources

            runtime_dst="./$APP_BUNDLE_NAME/Contents/MacOS/plugins"
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
                "$runtime_dst/ld-linux-x86-64.so.2"

            find ./$APP_BUNDLE_NAME/ -name '.DS_Store' -delete
            
            # Copy OrcaSlicer_profile_validator.app if it exists
            if [ -f "../src$BUILD_DIR_CONFIG_SUBDIR/OrcaSlicer_profile_validator.app/Contents/MacOS/OrcaSlicer_profile_validator" ]; then
                echo "Copying OrcaSlicer_profile_validator.app..."
                rm -rf ./OrcaSlicer_profile_validator.app
                cp -pR "../src$BUILD_DIR_CONFIG_SUBDIR/OrcaSlicer_profile_validator.app" ./OrcaSlicer_profile_validator.app
                # delete .DS_Store file
                find ./OrcaSlicer_profile_validator.app/ -name '.DS_Store' -delete
            fi
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
    ARM64_APP="$PROJECT_DIR/build/arm64/OrcaSlicer/$APP_BUNDLE_NAME"
    X86_64_APP="$PROJECT_DIR/build/x86_64/OrcaSlicer/$APP_BUNDLE_NAME"

    mkdir -p "$PROJECT_BUILD_DIR/OrcaSlicer"
    UNIVERSAL_APP="$PROJECT_BUILD_DIR/OrcaSlicer/$APP_BUNDLE_NAME"
    rm -rf "$UNIVERSAL_APP"
    cp -R "$ARM64_APP" "$UNIVERSAL_APP"

    echo "Creating universal binaries for $APP_BUNDLE_NAME..."
    lipo_dir "$UNIVERSAL_APP" "$X86_64_APP"
    echo "Universal $APP_BUNDLE_NAME created at $UNIVERSAL_APP"

    # Create universal binary for profile validator if it exists
    ARM64_VALIDATOR="$PROJECT_DIR/build/arm64/OrcaSlicer/OrcaSlicer_profile_validator.app"
    X86_64_VALIDATOR="$PROJECT_DIR/build/x86_64/OrcaSlicer/OrcaSlicer_profile_validator.app"
    if [ -d "$ARM64_VALIDATOR" ] && [ -d "$X86_64_VALIDATOR" ]; then
        echo "Creating universal binaries for OrcaSlicer_profile_validator.app..."
        UNIVERSAL_VALIDATOR_APP="$PROJECT_BUILD_DIR/OrcaSlicer/OrcaSlicer_profile_validator.app"
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
