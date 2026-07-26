#!/usr/bin/env python3
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
DISPATCHER = ROOT / "tools/slicer_linux_runtime/wsl/slicer_linux_runtime_host"

FAKE_HOST = r'''#!/bin/sh
set -eu
mode=${FAKE_HOST_MODE:?}
state=${FAKE_HOST_STATE:?}
case "${1:-}" in
    --probe-load)
        count=0
        [ ! -f "$state" ] || count=$(cat "$state")
        count=$((count + 1))
        printf '%s\n' "$count" > "$state"
        case "$mode:$count" in
            plugin-stage:1|host-stage:1|retry-fails:*)
                echo 'assertion failed [aot_hdr->segment_count <= kMaxSegments]: AOT header specified too many segments' >&2
                echo '(ImageInfo.cpp:71 create_from_header)' >&2
                exit 133
                ;;
            sigbus-stage:1|sigbus-host:1)
                echo 'Bus error (core dumped)' >&2
                exit 135
                ;;
            non-aot:*)
                echo 'ordinary dynamic loader failure' >&2
                exit 1
                ;;
            *) exit 0 ;;
        esac
        ;;
    --probe-stdio-roundtrip)
        if [ "$mode" = host-stage ]; then
            echo 'assertion failed [aot_hdr->segment_count <= kMaxSegments]: AOT header specified too many segments' >&2
            echo '(ImageInfo.cpp:71 create_from_header)' >&2
            exit 133
        fi
        if [ "$mode" = sigbus-host ]; then
            echo 'Bus error (core dumped)' >&2
            exit 135
        fi
        IFS= read -r _ || true
        echo SLICER_RUNTIME_STDIO_OK
        ;;
    *) exit 0 ;;
esac
'''


class DispatcherHarness:
    def __init__(self, mode: str):
        self.temp = tempfile.TemporaryDirectory(prefix="runtime-dispatcher-test-")
        self.root = Path(self.temp.name)
        self.runtime = self.root / "runtime"
        self.fake_bin = self.root / "bin"
        self.cache = self.root / "rosettad"
        self.disable_marker = self.root / "orcastudio-rosetta-aot-disabled"
        self.dropin_dir = self.root / "rosettad.service.d"
        self.runtime.mkdir()
        self.fake_bin.mkdir()
        self.cache.mkdir()

        shutil.copy2(DISPATCHER, self.runtime / "slicer_linux_runtime_host")
        (self.runtime / "slicer_linux_runtime_host_abi1").write_text(FAKE_HOST, encoding="utf-8")
        (self.runtime / "libbambu_networking.so").write_bytes(b"mock-network")
        (self.runtime / "libBambuSource.so").write_bytes(b"mock-source")
        for name in ("slicer_linux_runtime_host", "slicer_linux_runtime_host_abi1"):
            (self.runtime / name).chmod(0o755)

        self.state = self.root / "load-count"
        self.systemctl_log = self.root / "systemctl.log"
        self.aot = self.cache / "bad.aotcache"
        self.aot.write_bytes(b"corrupt")

        (self.fake_bin / "uname").write_text("#!/bin/sh\necho aarch64\n", encoding="utf-8")
        (self.fake_bin / "id").write_text("#!/bin/sh\n[ \"${1:-}\" = -u ] && echo 0 || exit 1\n", encoding="utf-8")
        (self.fake_bin / "systemctl").write_text(
            "#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"$FAKE_SYSTEMCTL_LOG\"\ncase \"${1:-}\" in cat|stop) exit 0 ;; is-active) exit 3 ;; *) exit 0 ;; esac\n",
            encoding="utf-8",
        )
        for name in ("uname", "id", "systemctl"):
            (self.fake_bin / name).chmod(0o755)

        self.env = os.environ.copy()
        self.env.update(
            {
                "PATH": f"{self.fake_bin}:{self.env.get('PATH', '')}",
                "FAKE_HOST_MODE": mode,
                "FAKE_HOST_STATE": str(self.state),
                "FAKE_SYSTEMCTL_LOG": str(self.systemctl_log),
                "SLICER_LINUX_RUNTIME_COMPONENT_DIR": str(self.runtime),
                "SLICER_LINUX_RUNTIME_COMPONENT_SO": str(self.runtime / "libbambu_networking.so"),
                "SLICER_LINUX_RUNTIME_SOURCE_SO": str(self.runtime / "libBambuSource.so"),
                "SLICER_LINUX_RUNTIME_REQUIRE_COMPATIBLE_HOST": "1",
                "SLICER_LINUX_RUNTIME_PREFER_SYSTEM_LOADER": "1",
                "SLICER_LINUX_RUNTIME_DISABLE_ABI0_FALLBACK": "1",
                "SLICER_LINUX_RUNTIME_ROSETTA_CACHE_DIR": str(self.cache),
                "SLICER_LINUX_RUNTIME_ROSETTA_DISABLE_MARKER": str(self.disable_marker),
                "SLICER_LINUX_RUNTIME_ROSETTA_DROPIN_DIR": str(self.dropin_dir),
            }
        )

    def run(self) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(self.runtime / "slicer_linux_runtime_host"), "--print-bin"],
            env=self.env,
            text=True,
            capture_output=True,
            timeout=15,
            check=False,
        )

    def close(self) -> None:
        self.temp.cleanup()


class RuntimeDispatcherTests(unittest.TestCase):
    def test_aot_cache_recovery(self) -> None:
        h = DispatcherHarness("plugin-stage")
        try:
            result = h.run()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), str(h.runtime / "slicer_linux_runtime_host_abi1"))
            self.assertEqual(h.state.read_text().strip(), "2")
            self.assertFalse(h.aot.exists())
            self.assertTrue(h.disable_marker.is_file())
            self.assertIn("ConditionPathExists=!/etc/orcastudio-rosetta-aot-disabled", (h.dropin_dir / "10-orcastudio-disable-aot.conf").read_text())
            self.assertIn("executable probe passed", result.stderr)
            self.assertIn("uncached translation probe succeeded", result.stderr)
            self.assertIn("stop rosettad.service", h.systemctl_log.read_text())
        finally:
            h.close()

    def test_aot_failure_in_bare_host_is_identified(self) -> None:
        h = DispatcherHarness("host-stage")
        try:
            result = h.run()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(h.state.read_text().strip(), "2")
            self.assertFalse(h.aot.exists())
            self.assertTrue(h.disable_marker.is_file())
            self.assertIn("executable probe also failed", result.stderr)
            self.assertIn("uncached translation probe succeeded", result.stderr)
        finally:
            h.close()

    def test_plugin_sigbus_retries_with_uncached_rosetta(self) -> None:
        h = DispatcherHarness("sigbus-stage")
        try:
            result = h.run()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(h.state.read_text().strip(), "2")
            self.assertFalse(h.aot.exists())
            self.assertTrue(h.disable_marker.is_file())
            self.assertIn("executable probe passed", result.stderr)
            self.assertIn("uncached translation probe succeeded", result.stderr)
        finally:
            h.close()

    def test_bare_host_sigbus_is_not_misclassified_as_plugin_failure(self) -> None:
        h = DispatcherHarness("sigbus-host")
        try:
            result = h.run()
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue(h.aot.exists())
            self.assertIn("executable probe also failed", result.stderr)
            self.assertNotIn("uncached translation probe succeeded", result.stderr)
        finally:
            h.close()

    def test_non_aot_failure_does_not_clear_cache(self) -> None:
        h = DispatcherHarness("non-aot")
        try:
            result = h.run()
            self.assertNotEqual(result.returncode, 0)
            self.assertTrue(h.aot.exists())
            self.assertFalse(h.systemctl_log.exists())
            self.assertNotIn("AOT cache failure detected", result.stderr)
            self.assertIn("ordinary dynamic loader failure", result.stderr)
        finally:
            h.close()

    def test_failed_retry_is_not_reported_as_recovered(self) -> None:
        h = DispatcherHarness("retry-fails")
        try:
            result = h.run()
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(h.state.read_text().strip(), "2")
            self.assertFalse(h.aot.exists())
            self.assertNotIn("uncached translation probe succeeded", result.stderr)
            self.assertIn("AOT header specified too many segments", result.stderr)
        finally:
            h.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
