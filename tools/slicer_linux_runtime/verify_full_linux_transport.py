#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
errors: list[str] = []


def fail(message: str) -> None:
    errors.append(message)


def read(rel: str) -> str:
    path = ROOT / rel
    if not path.is_file():
        errors.append(f"missing required file: {rel}")
        return ""
    return path.read_text(encoding="utf-8", errors="replace")


def require(rel: str, *needles: str) -> None:
    text = read(rel)
    for needle in needles:
        if needle not in text:
            errors.append(f"{rel}: missing invariant: {needle}")


def forbid(rel: str, *needles: str) -> None:
    text = read(rel)
    for needle in needles:
        if needle in text:
            errors.append(f"{rel}: forbidden stale invariant: {needle}")


def require_order(rel: str, *needles: str) -> None:
    text = read(rel)
    position = -1
    for needle in needles:
        next_position = text.find(needle, position + 1)
        if next_position < 0:
            errors.append(f"{rel}: missing ordered invariant: {needle}")
            return
        if next_position <= position:
            errors.append(f"{rel}: invalid invariant order at: {needle}")
            return
        position = next_position


def require_ca_bundle(rel: str) -> None:
    path = ROOT / rel
    if not path.is_file():
        errors.append(f"missing required CA bundle: {rel}")
        return
    data = path.read_bytes()
    certs = data.count(b"-----BEGIN CERTIFICATE-----")
    if len(data) < 65536 or certs < 50:
        errors.append(f"{rel}: invalid CA bundle: bytes={len(data)}, certificates={certs}")


require(
    "tools/slicer_linux_auth_helper/LinuxAuthBrowser.cpp",
    "SPDX-License-Identifier: AGPL-3.0-only",
    "webkit2/webkit2.h",
    "start_callback_server(&app)",
    "launch_external_linux_browser",
    'for (const char* candidate : {"epiphany", "epiphany-browser"})',
    'result["platform"] = "linux"',
    '"callback_server", callback_self_test_ok',
    'callback-self-test',
    "webkit_settings_set_enable_webgl(settings, FALSE)",
    'webkit_settings_get_user_agent(settings)',
    'webkit_settings_set_user_agent(settings, user_agent.c_str())',
    '"BBL-Slicer/v"',
    '"BBL-Language/"',
    '"Mozilla/5.0 (X11; Linux " + linux_machine()',
    'g_signal_connect(app.web_view, "draw", G_CALLBACK(on_draw), &app)',
    '{"kind", "render_ready"}',
    '{"kind", "browser_identity"}',
)
require(
    "tools/slicer_linux_auth_helper/run_auth_browser.sh",
    "Xvfb",
    "x11vnc",
    "websockify",
    "dbus-run-session",
    "SLICER_LINUX_RUNTIME_AUTH_NOVNC_PORT",
    "SLICER_LINUX_RUNTIME_AUTH_VNC_PORT",
    "SLICER_LINUX_RUNTIME_AUTH_CLIENT_VERSION",
    "SLICER_LINUX_RUNTIME_AUTH_LANGUAGE",
    "SLICER_LINUX_RUNTIME_AUTH_THEME",
    '--client-version "$CLIENT_VERSION"',
    '--language "$LANGUAGE"',
    '--theme "$THEME"',
    "WEBKIT_DISABLE_COMPOSITING_MODE=1",
    "WEBKIT_DISABLE_DMABUF_RENDERER=1",
    "LIBGL_ALWAYS_SOFTWARE=1",
    "656x840x24",
    "-noshm -nowf -noscr",
    "WINDOWS_WSL_RUNTIME=0",
    "WINDOWS_WSL_RUNTIME=1",
    'grep -Fq "@/tmp/.X11-unix/X$candidate" /proc/net/unix',
    'if xdpyinfo -display "$DISPLAY" >/dev/null 2>&1; then',
    'Xvfb failed to accept an X11 client connection under Windows WSL',
    "-u WAYLAND_DISPLAY",
    "-u WAYLAND_SOCKET",
    "-u DESKTOP_SESSION",
    "-u XDG_CURRENT_DESKTOP",
    "XDG_SESSION_TYPE=x11",
)
require_order(
    "tools/slicer_linux_auth_helper/run_auth_browser.sh",
    'WINDOWS_WSL_RUNTIME=0',
    '*microsoft*|*Microsoft*)',
    'WINDOWS_WSL_RUNTIME=1',
    'if [[ "$WINDOWS_WSL_RUNTIME" -eq 1 ]]; then',
    'if xdpyinfo -display "$DISPLAY" >/dev/null 2>&1; then',
    'else',
    '[[ -S "/tmp/.X11-unix/X$DISPLAY_NUMBER" ]] || { echo "Xvfb failed"',
    'if [[ "$WINDOWS_WSL_RUNTIME" -eq 1 ]]; then',
    '-u WAYLAND_DISPLAY',
    'XDG_SESSION_TYPE=x11',
    'else',
    'x11vnc -display "$DISPLAY"',
)
require(
    "tools/slicer_linux_runtime/test_windows_wsl_auth_runner.py",
    "Windows WSL auth runner behavioral test OK",
    '6.6.87.2-microsoft-standard-WSL2',
    'WAYLAND_DISPLAY": None',
    'XDG_SESSION_TYPE": "x11"',
    'browser_evidence.read_text(encoding="utf-8") != "wayland-0"',
)
require(
    "src/dev-utils/platform/unix/build_linux_image.sh.in",
    'if [ "${ORCA_BUNDLE_DESKTOP_STACK:-0}" != "0" ]; then',
    "bundled WebKitGTK desktop stack is disabled",
    "target_missing_runtime_library()",
    "missing host WebKitGTK 4.1 runtime libraries",
    "libwebkit2gtk-4.1.so.0",
    "libjavascriptcoregtk-4.1.so.0",
    "locale charmap",
    "FALLBACK_LOCALE=",
)
forbid(
    "src/dev-utils/platform/unix/build_linux_image.sh.in",
    "/tmp/.orca-webkit",
    "patch_webkit_helper_reference",
    "patch_webkit_directory_reference",
    "orca-webkit-helper-paths-relocated",
    "WEBKIT_EXEC_PATH",
    "WebKitNetworkProcess",
    "python-site-packages",
    "Xvfb.real",
)
require(
    "scripts/appimage_lib_policy.sh",
    "libgtk-*.so*",
    "libglib-2.0.so*",
    "libwebkit2gtk-*.so*",
    "libjavascriptcoregtk-*.so*",
)
require(
    "scripts/check_appimage_libs.sh",
    "appimage_is_host_library",
    "AppImage dependency audit passed",
)
forbid(
    "scripts/check_appimage_libs.sh",
    "orca-webkit-helper-paths-relocated",
    "smoke_bundled_xvfb",
    "smoke_bundled_python",
    "PYTHON_WEBKIT_PATH_AUDIT",
)
require(
    ".github/workflows/build_linux_appimage.yml",
    "ORCA_BUNDLE_DESKTOP_STACK: '0'",
    "Install AppImage build and smoke-test inputs",
    "libwebkit2gtk-4.1-dev",
    "ORCA_BUNDLE_DESKTOP_STACK=0",
    "test ! -e ./build/package/share/orca-bundled-desktop-runtime",
    "missing host WebKitGTK 4.1 runtime libraries",
    "locale charmap",
    "check_appimage_libs.sh out/squashfs-root",
    "AppImage startup smoke test passed",
    "Unable to spawn a new child process",
    "LANG=C.UTF-8",
)
forbid(
    ".github/workflows/build_linux_appimage.yml",
    "ORCA_BUNDLE_DESKTOP_STACK: '1'",
    "check_linux_bundle_inputs.sh",
    "package_linux_host_runtime.sh",
    "cmake --build build --config Release --target slicer_linux_runtime_host",
)
require(
    ".github/workflows/build_linux_portable.yml",
    "ORCA_BUNDLE_DESKTOP_STACK: '0'",
    "Install Linux portable build and smoke-test inputs",
    "libwebkit2gtk-4.1-dev",
    "ORCA_BUNDLE_DESKTOP_STACK=0",
    "test ! -e build/package/share/orca-bundled-desktop-runtime",
    "missing host WebKitGTK 4.1 runtime libraries",
    "locale charmap",
    "Portable startup smoke test passed",
    "Unable to spawn a new child process",
    "find_provider_package()",
    "declare -A bundled_provider_packages=()",
    "ldconfig -p",
    "dpkg-query -S",
    "basename_pattern=",
    "awk -F: 'NR == 1 { print $1 }' || true",
    'provider="$(find_provider_package "$lookup_path")"',
    'exclude_args+=("-x${provider}")',
    '"${exclude_args[@]}"',
    "dpkg-shlibdeps produced an empty dependency set",
    "dpkg-shlibdeps left an unresolved Debian substvar",
    'dpkg-deb -f "$deb_path" Depends',
    "built .deb still contains an unresolved Debian substvar",
)
require_order(
    ".github/workflows/build_linux_portable.yml",
    "find_provider_package()",
    "declare -A bundled_provider_packages=()",
    'provider="$(find_provider_package "$lookup_path")"',
    'exclude_args+=("-x${provider}")',
    'out="$(dpkg-shlibdeps --ignore-missing-info -O',
    '"${exclude_args[@]}"',
    'depends="$(printf "%s\\n" "$out"',
    'if [[ "$depends" == *\'${\'* ]]',
    'fakeroot dpkg-deb --build pkgroot "$deb_path"',
    'built_depends="$(dpkg-deb -f "$deb_path" Depends)"',
)
forbid(
    ".github/workflows/build_linux_portable.yml",
    "ORCA_BUNDLE_DESKTOP_STACK: '1'",
    "check_linux_bundle_inputs.sh",
    "package_linux_host_runtime.sh",
    "sha256sum -c runtime-files.sha256",
    "--probe-runtime",
    "build AppImage (best-effort)",
    "WARN: no AppImage produced",
)
require(
    "tools/slicer_linux_runtime_host/LinuxRuntimeHost.cpp",
    'if (method == "auth.start")',
    '{"diagnostic", diagnostic}',
    '{"client_version", client_version}',
    '{"language", language}',
    '{"theme", theme}',
    'SLICER_LINUX_RUNTIME_AUTH_CLIENT_VERSION',
    'if (!request && auth_running)',
    '{"suppressed_during_auth", true}',
    'if (request) {',
    'if (method == "browser.start")',
    'if (method == "http.start")',
    'if (method == "http.status")',
    'if (method == "http.cancel")',
    'if (method == "http.release")',
    'curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https")',
    'curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR',
    'curl_mime_init(curl)',
    'HostHttpJobState',
    'clear_auth_profile();',
    '"browser_engine", "webkitgtk"',
    '"platform", "linux"',
    '"linux_release", linux_release()',
    '"linux_os_version", linux_os_version()',
)
require(
    "src/slic3r/GUI/GUI_App.cpp",
    "select_automatic_ui_language",
    "match_available_ui_translation",
    "GetAvailableTranslations(SLIC3R_APP_KEY)",
    "GetBestTranslation(SLIC3R_APP_KEY, wxLANGUAGE_ENGLISH_US)",
    "Automatically selected UI language:",
    "Using user-selected UI language from OrcaSlicer.conf:",
    "without overwriting the preference",
    "new_locale->Init(wxLANGUAGE_DEFAULT, wxLOCALE_DONT_LOAD_DEFAULT)",
    'new_locale->Init("C", "C", "C", false)',
    "wxTranslations::Get()->SetLanguage(dictionary_language)",
    "Failed to load UI catalog",
)
require_order(
    "src/slic3r/GUI/GUI_App.cpp",
    'app_config->get("language")',
    "select_automatic_ui_language",
    "new_locale->Init(wxLANGUAGE_DEFAULT, wxLOCALE_DONT_LOAD_DEFAULT)",
    "wxTranslations::Get()->SetLanguage(dictionary_language)",
    "m_wxLocale->AddCatalog(SLIC3R_APP_KEY)",
)
load_language_source = read("src/slic3r/GUI/GUI_App.cpp")
load_language_start = load_language_source.index("bool GUI_App::load_language")
load_language_end = load_language_source.index("\nTab* GUI_App::get_tab", load_language_start)
load_language_body = load_language_source[load_language_start:load_language_end]
if 'app_config->set("language"' in load_language_body:
    fail("load_language() must not persist an automatically detected UI language")

require(
    "src/slic3r/Utils/BBLCloudServiceAgent.cpp",
    "SlicerLinuxRuntime::linux_component_package_os_type()",
    'extra_headers.emplace("X-BBL-OS-Version", os_version)',
    'value("linux_os_version", std::string())',
)
require(
    "src/slic3r/Utils/Http.cpp",
    "is_bambu_linux_runtime_url",
    "http_perform_linux_runtime();",
    "RuntimeMultipartPart",
    "runtime_multipart_parts",
    "multipart_parts.dump()",
    "std::atomic<bool> cancel",
    "force_native_transport",
    "via_native_transport",
    "set_bambu_extra_headers",
    'host_matches_domain(host, "bambulab.com")',
    'host_matches_domain(host, "makerworld.com")',
)
forbid("src/slic3r/Utils/Http.cpp", "multipart Bambu request is not supported")
require(
    "src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeLauncher_win.cpp",
    "allocate_loopback_port()",
    "std::to_string(novnc_port), std::to_string(novnc_port), std::to_string(vnc_port)",
)
require(
    "src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeLauncher_mac.mm",
    "allocate_loopback_port()",
    '{"SLICER_LINUX_RUNTIME_AUTH_HOST_NOVNC_PORT", std::to_string(novnc_port)}',
    '{"SLICER_LINUX_RUNTIME_AUTH_NOVNC_PORT", std::to_string(novnc_port)}',
)
require(
    "tools/slicer_linux_runtime_host/package_linux_host_runtime.sh",
    'sh -n "$RUNTIME_ROOT/slicer_linux_runtime_host"',
    'selected_host="$(',
    '"$RUNTIME_ROOT/slicer_linux_runtime_host" --print-bin',
    'sh -n "$WSL_BOOTSTRAP"',
    "SLICER_LINUX_RUNTIME_ALLOW_COMPONENTLESS=1",
    "probe_ok*",
)
require(
    "tools/slicer_linux_runtime_host/slicer-linux-runtime-host-wrapper",
    '-L "127.0.0.1:$HOST_NOVNC_PORT:127.0.0.1:$GUEST_NOVNC_PORT"',
    "SLICER_LINUX_RUNTIME_REQUIRE_LINUX_GUEST=1",
)
require(
    "src/slic3r/GUI/ModelMall.cpp",
    "wxEVT_WEBVIEW_NAVIGATING",
    "wxEVT_WEBVIEW_NEWWINDOW",
    "m_linux_viewer_port",
    "wxEVT_WEBVIEW_NAVIGATING",
)
require(
    "tools/slicer_linux_runtime/rootfs/build_windows_wsl_rootfs.sh",
    'ROOTFS_MARKER="ubuntu-24.04-linux-auth-v3"',
    "prepare_windows_wsl_rootfs.sh",
    "validate_windows_wsl_rootfs.py",
    "docker cp",
    "prepare Linux auth runtime dependencies",
    "--user 0:0",
)
require(
    "tools/slicer_linux_runtime/rootfs/prepare_windows_wsl_rootfs.sh",
    "epiphany-browser",
    "orcastudio-linux-auth-runtime.manifest",
    "x11-utils",
    "x11-xkb-utils",
    "xfonts-base",
    "xkb-data",
    "import websockify",
    "Xvfb did not create the Unix socket required by the runtime",
    "xvfb-run could not start Xvfb and connect xdpyinfo",
    "-e /dev/stderr",
    "xkb_data=",
)
require(
    "tools/slicer_linux_runtime/rootfs/validate_windows_wsl_rootfs.py",
    "MANIFEST_MEMBER",
    "REQUIRED_MANIFEST_KEYS",
    "runtime manifest marker mismatch",
    "manifest paths missing from archive",
    "CA certificate bundle is incomplete",
    '"xdpyinfo"',
    '"xkbcomp"',
    '"xkb_data"',
)
require(
    "tools/slicer_linux_runtime/rootfs/build_windows_wsl_rootfs.ps1",
    "function Test-RootfsTar",
    "function ConvertTo-NormalizedRootfsTarEntry",
    "orcastudio-linux-auth-runtime.manifest",
    "requiredPathKeys",
    "prepare_windows_wsl_rootfs.sh",
    "docker cp",
    "prepare Linux auth runtime dependencies",
    "Invoke-WithRetry",
    "--user 0:0",
    "failed to create WSL rootfs from all configured images",
)
require(
    "tools/slicer_linux_runtime/wsl/verify_runtime.ps1",
    "Assert-RuntimeManifest",
    "runtime-files.sha256",
    "function Assert-PosixScript",
    "Linux runtime dispatcher script",
    "Linux authentication launcher script",
    "'slicer_linux_runtime_host_abi1'",
    "'slicer_linux_runtime_host_abi0'",
)
forbid(
    "tools/slicer_linux_runtime/wsl/verify_runtime.ps1",
    "@('slicer_linux_runtime_host', 'slicer_linux_runtime_host_abi1', 'slicer_linux_runtime_host_abi0', 'slicer_linux_auth_browser')",
)
require(
    "tools/slicer_linux_runtime/macos/install_runtime_macos.sh",
    'INSTALL_VERSION="SLICER-LINUX-RUNTIME-MAC-0.34-BAMBU-NETWORK-ROSETTA-SPLITLOCK-V21"',
    "ubuntu-24.04-bambu-network-amd64-rosetta-splitlock-v16",
    "ConditionPathExists=!/etc/orcastudio-rosetta-aot-disabled",
)
require(
    "tools/slicer_linux_runtime/macos/verify_runtime_macos.sh",
    'INSTALL_VERSION="SLICER-LINUX-RUNTIME-MAC-0.34-BAMBU-NETWORK-ROSETTA-SPLITLOCK-V21"',
    "-PrintGuestVerifier",
    "component_probe_marker.txt",
)
require(
    ".github/workflows/build_macos_bridge.yml",
    "ubuntu-24.04-arm",
    "slicer_linux_auth_browser_aarch64",
    "SLICER_LINUX_AUTH_BROWSER_AARCH64",
    "openssl",
    "package_linux_host_runtime.sh",
    "sha256sum -c runtime-files.sha256",
    "runs-on: macos-15",
    "Build macOS x86_64 (cross-compiled on Apple Silicon)",
    "Verify macOS x86_64 cross-compilation host",
    'host_arch="$(uname -m)"',
    '[[ "$host_arch" != "arm64" ]]',
    "Cross-compiling macOS x86_64 on Apple Silicon host",
    "./build_release_macos.sh -dx -1 -a x86_64 -t 11.3",
    "./build_release_macos.sh -s -n -x -1 -a x86_64 -t 11.3",
    "Verify macOS x86_64 binary architecture",
    'architectures="$(lipo -archs "$binary")"',
    "expected an x86_64-only macOS executable",
)
require(
    ".github/workflows/build_macos_bridge.yml",
    "verify_full_linux_transport.py",
    "test_runtime_dispatcher.py",
    "test_macos_runtime_wrapper.py",
    "test_rosetta_splitlock_compat.py",
    "SLICER_LINUX_RUNTIME_INCLUDE_ROSETTA_SPLITLOCK_COMPAT=1",
    "SLICER_LINUX_RUNTIME_ROSETTA_SPLITLOCK_CC=gcc-14",
)
require(
    ".github/workflows/build_windows_bridge.yml",
    "verify_full_linux_transport.py",
    "test_runtime_dispatcher.py",
)
forbid(
    ".github/workflows/build_windows_bridge.yml",
    "test_macos_runtime_wrapper.py",
    "test_rosetta_splitlock_compat.py",
    "SLICER_LINUX_RUNTIME_INCLUDE_ROSETTA_SPLITLOCK_COMPAT=1",
)
forbid(
    ".github/workflows/build_macos_bridge.yml",
    "runs-on: macos-15-intel",
    "build_macos_x86_64_deps:",
    "fail-on-cache-miss: true",
    "macos-15-intel-x86_64",
)
require_order(
    ".github/workflows/build_macos_bridge.yml",
    "build_macos_x86_64:",
    "runs-on: macos-15",
    "Verify macOS x86_64 cross-compilation host",
    "./build_release_macos.sh -dx -1 -a x86_64 -t 11.3",
    "./build_release_macos.sh -s -n -x -1 -a x86_64 -t 11.3",
    "Verify macOS x86_64 binary architecture",
)


require(
    "src/CMakeLists.txt",
    "_default_linux_runtime_dir",
    "file(GLOB _default_linux_runtime_files",
    "tools/slicer_linux_runtime_host/runtime/linux-x86_64",
)
require(
    "build_release_macos.sh",
    '"slicer_linux_auth_browser"',
    '"run_auth_browser.sh"',
    '"runtime-files.sha256"',
)

require(
    "build_release_vs.bat",
    "runtime-files.sha256",
    "Missing Linux runtime manifest",
)

require_ca_bundle("resources/cert/ca-certificates.crt")
require(
    "CMakeLists.txt",
    "target_link_libraries(libcurl INTERFACE CURL::libcurl)",
    "target_link_libraries(libcurl INTERFACE ZLIB::ZLIB)",
    "OpenSSL::SSL",
    "OpenSSL::Crypto",
    "Threads::Threads",
)
require_order(
    "CMakeLists.txt",
    "target_link_libraries(libcurl INTERFACE CURL::libcurl)",
    "target_link_libraries(libcurl INTERFACE ZLIB::ZLIB)",
    "OpenSSL::SSL",
    "OpenSSL::Crypto",
)
forbid(
    "CMakeLists.txt",
    "COMMAND msgmerge -N -o ${po_file} ${po_file} \"${BBL_L18N_DIR}/OrcaSlicer.pot\"\n        DEPENDS ${po_file}",
    "#COMMAND msgfmt ARGS --check-compatibility -o ${mo_file} ${po_file}\n        DEPENDS ${po_file}",
)
forbid(
    "src/CMakeLists.txt",
    "add_custom_command(TARGET OrcaSlicer POST_BUILD\n            WORKING_DIRECTORY \"$<TARGET_FILE_DIR:OrcaSlicer>\"",
)
require_order(
    "tools/slicer_linux_runtime_host/CMakeLists.txt",
    "CURL::libcurl",
    "ZLIB::ZLIB",
    "OpenSSL::SSL",
    "OpenSSL::Crypto",
    "Threads::Threads",
)
forbid(
    "src/slic3r/Utils/SlicerLinuxRuntime/CMakeLists.txt",
    "OpenSSL::Crypto CURL::libcurl",
)
require_order(
    "src/slic3r/Utils/SlicerLinuxRuntime/CMakeLists.txt",
    "if (WIN32)",
    "OpenSSL::Crypto",
    "ws2_32",
    "Crypt32",
    "elseif (APPLE)",
)
require(
    "src/slic3r/Utils/SlicerLinuxRuntime/CMakeLists.txt",
    "boost_headeronly\n            libcurl",
    "get_target_property(SLICER_LINUX_RUNTIME_LIBSLIC3R_BINARY_DIR libslic3r BINARY_DIR)",
    "${SLICER_LINUX_RUNTIME_LIBSLIC3R_BINARY_DIR}",
    "runtime-files.sha256",
)
require(
    "tools/slicer_linux_runtime_host/main.cpp",
    "--probe-runtime",
    "const std::string pem{std::istreambuf_iterator<char>{ca}, std::istreambuf_iterator<char>{}};",
    "CURL_VERSION_SSL",
    "CURL_VERSION_LIBZ",
    "CURLOPT_CAINFO",
)
forbid(
    "tools/slicer_linux_runtime_host/main.cpp",
    "const std::string pem(std::istreambuf_iterator<char>(ca), std::istreambuf_iterator<char>());",
)
require(
    "tools/slicer_linux_runtime_host/LinuxRuntimeHost.cpp",
    "SLICER_LINUX_RUNTIME_CA_BUNDLE",
    "CURLOPT_CAINFO",
)
require(
    "src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeLauncher_linux.cpp",
    "SLICER_LINUX_RUNTIME_CA_BUNDLE",
    "ca-certificates.crt",
)
require(
    "tools/slicer_linux_runtime/wsl/slicer_linux_runtime_host",
    "SLICER_LINUX_RUNTIME_CA_BUNDLE",
    "SSL_CERT_FILE",
    "CURL_CA_BUNDLE",
)
require(
    "tools/slicer_linux_runtime_host/package_linux_host_runtime.sh",
    "ca-certificates.crt",
    "BEGIN CERTIFICATE",
    "openssl crl2pkcs7",
    "--probe-runtime",
    "runtime-files.sha256",
)
require(
    ".github/workflows/build_linux_portable.yml",
    r'exec "/opt/orcastudio/${APP_BIN}" "\$@"',
    "libwebkit2gtk-4.1-dev",
    "dpkg-shlibdeps",
)
require(
    "tools/slicer_linux_runtime/wsl/verify_runtime.ps1",
    "Assert-RuntimeManifest",
    "runtime-files.sha256",
)
require(
    "tools/slicer_linux_runtime/macos/install_runtime_macos.sh",
    "validate_ca_bundle",
    "validate_runtime_manifest",
    "runtime-files.sha256",
    "slicer-linux-runtime-host-wrapper",
    "SLICER_LINUX_RUNTIME_REQUIRE_COMPATIBLE_HOST=1",
    "--probe-stdio-roundtrip",
)
require(
    "tools/slicer_linux_runtime/macos/verify_runtime_macos.sh",
    "validate_ca_bundle",
    "slicer-linux-runtime-host-wrapper",
    "SLICER_LINUX_RUNTIME_REQUIRE_COMPATIBLE_HOST=1",
    "--probe-stdio-roundtrip",
    "runtime plug-in compatibility/stdio transaction failed: rc=$rc",
)
require(
    "tools/slicer_linux_runtime_host/slicer-linux-runtime-host-wrapper",
    "SLICER_LINUX_RUNTIME_CA_BUNDLE",
    "valid CA certificate bundle not found in Linux guest",
    "ssh_guest_control()",
    "ssh_guest_stream()",
    '"$SSH_BIN" -F "$SSH_CONFIG" -T -n',
)
forbid(
    "tools/slicer_linux_runtime_host/slicer-linux-runtime-host-wrapper",
    "ssh_guest() {",
)
require(
    "tools/slicer_linux_runtime/wsl/slicer_linux_runtime_host",
    "liborcastudio_rosetta_splitlock_compat.so",
    "SLICER_LINUX_RUNTIME_ROSETTA_SPLITLOCK_COMPAT=1",
    "LD_PRELOAD",
    "rosetta_translation_active",
)
require(
    "tools/slicer_linux_runtime_host/rosetta_splitlock_compat.c",
    "orcastudio_emulate_splitlock",
    "OP_XADD",
    "OP_CMPXCHG_PAIR",
    "SA_SIGINFO",
)
require(
    "tools/slicer_linux_runtime_host/package_linux_host_runtime.sh",
    "build_rosetta_splitlock_shim",
    "liborcastudio_rosetta_splitlock_compat.so",
    "SLICER_LINUX_RUNTIME_INCLUDE_ROSETTA_SPLITLOCK_COMPAT",
    "SLICER_LINUX_RUNTIME_ROSETTA_SPLITLOCK_CC",
)
require(
    "tools/slicer_linux_runtime/test_rosetta_splitlock_compat.py",
    "Rosetta split-lock compatibility verification OK",
    "test_rosetta_splitlock_concurrency",
    "xchg-old=31 xchg-new=23",
)
require(
    "tools/slicer_linux_runtime_host/test_rosetta_splitlock_compat.c",
    "test_unprefixed_xchg32",
    "OP_XCHG",
)
require(
    "tools/slicer_linux_runtime_host/test_rosetta_splitlock_signal.c",
    "xchgl",
    "xchg-old=%u xchg-new=%u",
)
require(
    "tools/slicer_linux_runtime_host/test_rosetta_splitlock_concurrency.c",
    "THREAD_COUNT",
    "lock addl",
)

require(
    "tools/slicer_linux_runtime/wsl/slicer_linux_runtime_host",
    "AOT header specified too many segments",
    "/var/cache/rosettad",
    "--probe-stdio-roundtrip",
    "Rosetta uncached translation probe succeeded",
    "systemctl stop rosettad.service",
    "disable_rosetta_aot_cache",
    "ConditionPathExists=!/etc/orcastudio-rosetta-aot-disabled",
)
require(
    "tools/slicer_linux_runtime/test_runtime_dispatcher.py",
    "test_aot_cache_recovery",
    "test_plugin_sigbus_retries_with_uncached_rosetta",
    "test_bare_host_sigbus_is_not_misclassified_as_plugin_failure",
    "test_non_aot_failure_does_not_clear_cache",
)
require(
    "tools/slicer_linux_runtime/test_macos_runtime_wrapper.py",
    "test_control_ssh_does_not_consume_runtime_stdin",
    "test_payload_is_content_addressed_and_path_independent",
    "linux-runtime-payloads",
)
require(
    "tools/slicer_linux_runtime_host/main.cpp",
    "net.set_cert_file",
    "slicer_base64.cer",
    "net.destroy_agent",
    'unsetenv("LD_PRELOAD")',
)
require(
    "tools/slicer_linux_runtime_host/slicer-linux-runtime-host-wrapper",
    "linux-runtime-payloads",
    "orcastudio-linux-guest-payload-v3-content-addressed",
    "PAYLOAD_MARKER",
    "disable_guest_rosetta_aot_cache",
    "systemctl stop rosettad.service",
    "ConditionPathExists=!/etc/orcastudio-rosetta-aot-disabled",
)
require(
    "tools/slicer_linux_runtime/wsl/install_runtime.ps1",
    "ca-certificates.crt",
    "runtime-files.sha256",
)
require(
    "tools/slicer_linux_runtime/wsl/install_runtime.cmd",
    "install_runtime.ps1",
    "pwsh",
    "powershell",
)
require(
    "src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeForwarderExports.cpp",
    "slicer_linux_runtime_forwarder_shutdown",
    "http.start",
    "http.status",
    "http.cancel",
    "http.release",
    "begin_destroy_tunnel_handle",
    "join_forwarder_workers",
)
forbid(
    "src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeForwarderExports.cpp",
    "poll_ft_messages).detach",
    "poll_ft_result).detach",
)
require(
    "src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeForwarderState.hpp",
    "RuntimeTunnelLease acquire_tunnel_handle",
    "RuntimeTunnel* begin_destroy_tunnel_handle",
)
require(
    "src/slic3r/Utils/BBLNetworkPlugin.cpp",
    "slicer_linux_runtime_forwarder_shutdown",
    "g_network_module_lifetime_mutex",
    "g_network_module_call_depth",
    "ModuleCallGuard::ModuleCallGuard",
    "static BBLNetworkPlugin* plugin = new BBLNetworkPlugin();",
    "destroy_agent_unlocked();",
    "if (!UnloadFTModule())",
)
forbid(
    "src/slic3r/Utils/BBLNetworkPlugin.cpp",
    "BBLNetworkPlugin* BBLNetworkPlugin::s_instance",
    "std::once_flag",
)
require(
    "src/slic3r/Utils/BBLNetworkPlugin.hpp",
    "class ModuleCallGuard",
    "static ModuleCallGuard lock_module_for_call();",
    "create_agent_unlocked",
    "destroy_agent_unlocked",
)
forbid("src/slic3r/Utils/BBLNetworkPlugin.hpp", "static BBLNetworkPlugin* s_instance")
for rel in (
    "src/slic3r/Utils/BBLCloudServiceAgent.cpp",
    "src/slic3r/Utils/BBLPrinterAgent.cpp",
    "src/slic3r/GUI/Printer/PrinterFileSystem.cpp",
):
    require(rel, "lock_module_for_call()")
require(
    "src/slic3r/GUI/Printer/PrinterFileSystem.cpp",
    "real_last_error_message",
    "real_last_error_module",
    "locked_last_error_message.c_str()",
)
require(
    "tools/slicer_linux_auth_helper/LinuxAuthBrowser.cpp",
    '"auth-reply.json"',
    '"kind", "ticket"',
    '"?result="',
)
forbid(
    "tools/slicer_linux_auth_helper/LinuxAuthBrowser.cpp",
    '"kind", "code"',
    '"kind", "access_token"',
)
require(
    "src/slic3r/Utils/BBLCloudServiceAgent.cpp",
    "Http::set_bambu_extra_headers(extra_headers)",
)
for rel in ("src/slic3r/GUI/SelectMachine.cpp", "src/slic3r/GUI/SendToPrinter.cpp"):
    require(
        rel,
        "BMCU_AUTO_RETRY_MAX_ATTEMPTS = 3",
        "BMCU_AUTO_RETRY_COUNTDOWN_SECONDS = 3",
        "The printer did not become ready after the BMCU error. You can send the print again;",
        "OrcaStudio will retry without blocking the application.",
        "The printer rejected the print after %d automatic BMCU retries.",
        "You can send it again without restarting OrcaStudio.",
        "Enable_Send_Button(true)",
    )
    forbid(
        rel,
        "The print is still being rejected by the printer because of the BMCU error after %d retry attempts",
    )


require(
    "src/slic3r/Utils/BBLNetworkPlugin.hpp",
    "std::string dev_model, std::string sec_link, std::string timezone",
)
require(
    "src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeForwarderExports.cpp",
    "std::string dev_model, std::string sec_link",
    '{"dev_model", dev_model}',
)
require(
    "tools/slicer_linux_runtime_host/LinuxRuntimeHost.cpp",
    'params.value("dev_model", std::string())',
)
require(
    "src/slic3r/GUI/Jobs/BindJob.cpp",
    "m_dev_model, m_sec_link, timezone",
)
require(
    "src/slic3r/Utils/BBLPrinterAgent.cpp",
    "m_last_print_request.retry_count >= 3",
    '<< "/3, dev_id=" << params.dev_id',
)
require(
    "tools/slicer_linux_runtime/wsl/install_runtime.ps1",
    "Resolve-WslExecutable",
    "Windows Subsystem for Linux (wsl.exe) was not found.",
    "Repair-CaBundleFromRootFs",
)
require(
    "tools/slicer_linux_runtime/wsl/verify_runtime.ps1",
    "Resolve-WslExecutable",
    "Windows Subsystem for Linux (wsl.exe) was not found.",
    "Repair-CaBundleFromRootFs",
)
require(
    "src/slic3r/Utils/bambu_networking.hpp",
    '#define BAMBU_NETWORK_AGENT_VERSION "02.08.01"',
    '#define BAMBU_NETWORK_ERR_AMS_SYNC_FAILED               -32',
)
require(
    "version.inc",
    'set(SLIC3R_VERSION "02.08.01.55")',
)

for rel in (
    "tools/slicer_linux_runtime_host/slicer-linux-runtime-host-wrapper",
    "tools/slicer_linux_runtime/macos/install_runtime_macos.sh",
    "tools/slicer_linux_runtime/macos/verify_runtime_macos.sh",
):
    forbid(rel, "SLICER_LINUX_RUNTIME_HOST_OS")

require(
    "tools/slicer_linux_runtime/macos/install_runtime_macos.sh",
    'LIMA_MODE="vz-rosetta-aarch64"',
    "--vm-type=vz --arch=aarch64 --mount-type=virtiofs --rosetta",
    "--disk=20",
)
require(
    "tools/slicer_linux_runtime_host/slicer-linux-runtime-host-wrapper",
    "list --format '{{.SSHConfigFile}}'",
    'exec "$SSH_BIN" -F "$SSH_CONFIG" -T',
)
forbid(
    "tools/slicer_linux_runtime_host/slicer-linux-runtime-host-wrapper",
    "show-ssh",
    "--tty=false shell",
)
require(
    "src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeConfig.cpp",
    "#elif defined(__WXMAC__) || defined(__APPLE__)\n    return true;",
    'return "linux";',
)
require(
    "src/slic3r/Utils/PresetUpdater.cpp",
    'SlicerLinuxRuntime::should_select_linux_component_package("plugins")',
    'cache_folder.string() + "/libbambu_networking.so"',
    'cache_folder.string() + "/libBambuSource.so"',
)
require(
    "src/slic3r/GUI/GUI_App.cpp",
    "BBLNetworkPlugin::instance().destroy_agent();",
)
require(
    "src/slic3r/Utils/BBLPrinterAgent.cpp",
    "m_last_print_request.update_fn = nullptr;",
    "m_last_print_request.cancel_fn = nullptr;",
    "m_last_print_request.wait_fn = nullptr;",
)
require(
    "src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeRpcClient.cpp",
    "std::chrono::seconds(2)",
    "proc->child.wait();",
)
require(
    "src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeLauncher_win.cpp",
    "return {};",
    "if (wsl.empty() || !std::filesystem::exists(std::filesystem::u8path(wsl)))",
)

for rel in (
    "tools/slicer_linux_runtime_host/main.cpp",
    "tools/slicer_linux_runtime/macos/install_runtime_macos.sh",
    "tools/slicer_linux_runtime/macos/verify_runtime_macos.sh",
    "tools/slicer_linux_runtime/wsl/slicer_linux_runtime_host",
):
    forbid(rel, "SLICER_LINUX_RUNTIME_COUNTRY_CODE=PL", 'std::string("PL")', ':-PL}')

for rel in (
    "src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeLauncher_win.cpp",
    "src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeLauncher_mac.mm",
    "tools/slicer_linux_runtime_host/LinuxRuntimeHost.cpp",
    "tools/slicer_linux_runtime_host/slicer-linux-runtime-host-wrapper",
    "tools/slicer_linux_runtime/wsl/slicer_linux_runtime_wsl_run_host.sh",
):
    forbid(rel, "39457", "39458", "5901")

# The host-side Bambu login must never navigate directly to a remote Bambu URL.
require(
    "src/slic3r/GUI/WebUserLoginDialog.cpp",
    "get_linux_auth_start()",
    "get_linux_auth_start_v2()",
    "SlicerLinuxRuntime::use_linux_runtime()",
    "SLIC3R_VERSION",
    "wxGetApp().dark_mode()",
    'const wxString initial_url = m_linux_auth ? "about:blank" : TargetUrl',
    "sizer->Add(m_browser, 1, wxEXPAND)",
    "m_browser->LoadURL(TargetUrl)",
    'BOOST_LOG_TRIVIAL(info) << "linux_auth_start: " << diagnostic',
    "constexpr int loopback_wait_attempts = 200;",
    "attempt < loopback_wait_attempts",
)
require(
    "src/slic3r/GUI/GUI_App.cpp",
    "login_dlg->run();",
)
require(
    "src/slic3r/Utils/BBLNetworkPlugin.hpp",
    "func_linux_auth_start_v2",
    "get_linux_auth_start_v2()",
)
require(
    "src/slic3r/Utils/BBLNetworkPlugin.cpp",
    '"slicer_linux_runtime_auth_start_v2"',
)
require(
    "src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeForwarderExports.cpp",
    "slicer_linux_runtime_auth_start_v2",
    '{"client_version", std::move(client_version)}',
    '{"language", std::move(language)}',
    '{"theme", dark_mode ? "dark" : "light"}',
)

require(
    "scripts/generate_linux_payload_manifest.py",
    'DEFAULT_ABI_VERSION = "02.08.01"',
    'MANIFEST_NAME = "linux_component_manifest.json"',
    '{"schema": 1, "files": files}',
)
forbid(
    "scripts/generate_linux_payload_manifest.py",
    "02.05.02.58",
    "linux_payload_manifest.json",
)
require(
    "src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeConfig.cpp",
    'root.value("schema", 0) != 1',
    '"liblive555.so"',
    '"libagora_rtc_sdk.so"',
    '"libagora-fdkaac.so"',
    'manifest filename duplicated',
    'missing from manifest',
)
require(
    "src/slic3r/GUI/GUI_App.cpp",
    "safe_plugin_archive_path",
    "copy_plugin_contents(component_folder, backup_staging_folder, backup_folder",
    "restore_plugin_backup(component_folder, backup_folder",
    "remove_or_retire_plugin_entry",
    "known_linux_component_payload_names",
    "is_known_linux_component_payload_name",
    ".size_limit(4ULL * 1024ULL * 1024ULL)",
    ".size_limit(512ULL * 1024ULL * 1024ULL)",
)
require(
    "src/slic3r/GUI/GUI_App.cpp",
    'header("X-BBL-OS-Type", os_type)',
    ".via_native_transport()",
    "boost::optional<Semver> selected_version",
    "selected_version = *parsed_version",
    "package_version = version",
    "downloaded_version",
    "app_config ? app_config->get_network_plugin_version()",
    "using transient Linux component series",
    "The Bambu network component is optional",
)
forbid(
    "src/slic3r/GUI/GUI_App.cpp",
    "BBLNetworkPlugin::linux_runtime_http_get(\n            url",
    "BBLNetworkPlugin::linux_runtime_http_get(\n            download_url",
    "app_config->set(SETTING_NETWORK_PLUGIN_VERSION, get_latest_network_version())",
    "set_network_plugin_version(get_latest_network_version())",
    "prepare_windows_slicer_linux_runtime(component_folder, component_cache_dir)",
    "prepare_macos_slicer_linux_runtime(component_folder, component_cache_dir)",
)
require_order(
    "src/slic3r/GUI/GUI_App.cpp",
    "const std::error_code rename_error = rename_file(tmp_path.string(), target_file_path.string())",
    "*downloaded_version = package_version",
)
require(
    "src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeConfig.cpp",
    'file_name == "runtime-files.sha256"',
)
require(
    "src/slic3r/GUI/WebGuideDialog.cpp",
    'InstallNetplugin = requested && !network_plugin_ready',
    'set_bool("installed_networking", requested && network_plugin_ready)',
    'A failed or cancelled attempt must not trigger work on startup',
)
forbid(
    "src/slic3r/GUI/WebGuideDialog.cpp",
    'set_bool("installed_networking", requested);',
    "offer a retry",
)
require(
    "src/slic3r/GUI/Preferences.cpp",
    'if (param == "installed_networking")',
    'app_config->set_bool(param, false)',
    'ShowDownNetPluginDlg()',
)
require(
    "src/slic3r/GUI/Jobs/UpgradeNetworkJob.cpp",
    "downloaded_version",
    "download_plugin(",
    "install_plugin(",
    "install_requested_network_runtime",
    "network_plugin_runtime_install",
    'set_bool("installed_networking", true)',
    "set_network_plugin_version(version)",
    "boost::process",
)
require_order(
    "src/slic3r/GUI/Jobs/UpgradeNetworkJob.cpp",
    "download_plugin(name, package_name",
    "install_plugin(",
    "install_requested_network_runtime(cancel_fn, runtime_error)",
    "commit_network_plugin_configuration(app_config, downloaded_version)",
)
for rel in (
    "src/slic3r/GUI/WebDownPluginDlg.cpp",
    "src/slic3r/GUI/WebGuideDialog.cpp",
):
    require(rel, "m_downloaded_plugin_version", "download_plugin(", "install_plugin(")

require(
    "src/slic3r/Utils/PresetUpdater.cpp",
    "unsafe archive path",
    "relative.lexically_normal()",
)
require(
    "tools/slicer_linux_runtime/wsl/install_runtime.ps1",
    "& $FilePath @ArgumentList",
    "'-InstallDir', $InstallDir",
)
forbid(
    "tools/slicer_linux_runtime/wsl/install_runtime.ps1",
    "Start-Process -FilePath $FilePath -ArgumentList",
    "'-SkipProbe'",
)
require(
    "tools/slicer_linux_runtime/wsl/verify_runtime.ps1",
    "& $FilePath @ArgumentList",
    "function Assert-PosixScript",
    "contains CR/CRLF line endings",
    "Assert-PosixScript (Join-Path $PackageDir 'slicer_linux_runtime_host')",
    "Assert-FileMagic (Join-Path $PackageDir 'slicer_linux_runtime.dll') ([byte[]](0x4D, 0x5A))",
    "foreach ($name in @('slicer_linux_runtime_host_abi1', 'slicer_linux_runtime_host_abi0', 'slicer_linux_auth_browser'))",
    "([byte[]](0x7F, 0x45, 0x4C, 0x46))",
)
forbid(
    "tools/slicer_linux_runtime/wsl/verify_runtime.ps1",
    "Start-Process -FilePath $FilePath -ArgumentList",
)

# Native PowerShell argument marshalling corrupts nested `sh -lc` positional
# parameters on both Windows PowerShell 5.1 and PowerShell 7.  Authentication
# validation must pass the Linux executable and every argument directly to
# wsl.exe instead of relying on `$1` inside a shell command string.
require(
    "tools/slicer_linux_runtime/wsl/verify_runtime.ps1",
    "function Invoke-WslRootCapture",
    "[string[]]$wslArgs = @('-d', $Name, '--user', 'root', '--')",
    "Assert-WslRootCommand $wsl $DistroName 'WSL Linux authentication browser prerequisite verification failed' @($authBrowserWsl, '--probe')",
    "[string[]]$authSelfTestCommand = @(",
    "'env',",
    "'xvfb-run', '-a', '-e', '/dev/stderr',",
    "$authBrowserWsl, '--self-test'",
    "Invoke-WslRootCapture $wsl $DistroName @('touch', '/etc/orcastudio-auth-disable-gigacage')",
)
forbid(
    "tools/slicer_linux_runtime/wsl/verify_runtime.ps1",
    "$authPrerequisiteScript",
    "$authSelfTestScript",
    "$authGigacageFallbackScript",
    "'sh', '-lc'",
    '"$1"',
)


require(
    "src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeForwarderExports.cpp",
    "slicer_linux_runtime_forwarder_active_tunnels",
    "slicer_linux_runtime_forwarder_active_callbacks",
    "active_runtime_tunnel_count()",
    "active_queued_main_callback_count()",
)
require(
    "src/slic3r/Utils/BBLNetworkPlugin.cpp",
    "active_source_tunnels() != 0",
    "active_forwarder_callbacks() != 0",
    "slicer_linux_runtime_forwarder_active_tunnels",
    "slicer_linux_runtime_forwarder_active_callbacks",
)
require(
    "src/slic3r/GUI/MediaPlayCtrl.cpp",
    "stop_for_network_reload",
    'm_tasks.push_back("<reload>")',
    "NetworkAgent::active_source_tunnels()",
)
require(
    "src/slic3r/GUI/Printer/PrinterFileSystem.cpp",
    "StaticBambuLib::remove(this)",
    "module = NULL;",
    "storage().copies_",
)
require(
    "tools/slicer_linux_runtime_host/LinuxRuntimeHost.cpp",
    "kMaxFtPayloadBytes",
    "finish_ft_callback",
    "m_module_load_attempted",
)
require(
    "src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeRpcProtocol.cpp",
    "k_max_frame_size",
)
require(
    "src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeRpcClient.cpp",
    "expected_binary_size",
    "binary response size mismatch",
)
require(
    "tools/slicer_linux_runtime/wsl/slicer_linux_runtime_wsl_run_host.sh",
    "mv -Tf",
    "mktemp",
)
for rel in (
    "tools/slicer_linux_runtime/macos/install_runtime_macos.sh",
    "tools/slicer_linux_runtime/macos/verify_runtime_macos.sh",
):
    require(rel, "atomic_copy_file", "mktemp", "mv -f")

forwarder = read("src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeForwarderExports.cpp")
host = read("tools/slicer_linux_runtime_host/LinuxRuntimeHost.cpp")
forwarder_methods = set(re.findall(r'invoke_(?:json|int|void|binary)\(\s*"([^"]+)"', forwarder))
host_methods = set(re.findall(r'method\s*==\s*"([^"]+)"', host))
for method in sorted(forwarder_methods - host_methods):
    errors.append(f"missing host RPC handler: {method}")

plugin = read("src/slic3r/Utils/BBLNetworkPlugin.cpp")
required_exports = set(re.findall(r'get_function\("([^"]+)"\)', plugin))
exported = set(re.findall(r'SLICER_LINUX_RUNTIME_EXPORT\s+[^\n{;]+?\b([A-Za-z_]\w*)\s*\(', forwarder))
for symbol in sorted(required_exports - exported):
    errors.append(f"missing forwarder export: {symbol}")

verify_runtime_text = read("tools/slicer_linux_runtime/wsl/verify_runtime.ps1")
if "SLICER_LINUX_RUNTIME_REQUIRE_LINUX_GUEST=1" in verify_runtime_text:
    fail("Windows package verifier still requires the obsolete Linux guest literal")
if "Assert-TextContains" in verify_runtime_text:
    fail("Windows package verifier still relies on brittle source-text invariants")


# Rosetta compatibility is a macOS Apple-Silicon runtime concern. It must never
# become a Windows/WSL or generic x86 runtime requirement.
windows_launcher_text = read("src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeLauncher_win.cpp")
if "liborcastudio_rosetta_splitlock_compat.so" in windows_launcher_text:
    fail("Windows launcher still requires the Rosetta split-lock shim")

gui_app_text = read("src/slic3r/GUI/GUI_App.cpp")
if gui_app_text.count("liborcastudio_rosetta_splitlock_compat.so") != 1:
    fail("GUI runtime readiness must require the Rosetta shim only in the Apple branch")

network_plugin_text = read("src/slic3r/Utils/BBLNetworkPlugin.cpp")
if network_plugin_text.count("liborcastudio_rosetta_splitlock_compat.so") != 1:
    fail("network plug-in preflight must require the Rosetta shim only in the Apple branch")

if errors:
    print("full Linux transport verification FAILED", file=sys.stderr)
    for error in errors:
        print(f"- {error}", file=sys.stderr)
    raise SystemExit(1)

print("full Linux transport verification OK")
