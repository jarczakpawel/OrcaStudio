#!/usr/bin/env python3
from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
WRAPPER = ROOT / "tools/slicer_linux_runtime_host/slicer-linux-runtime-host-wrapper"
DISPATCHER = ROOT / "tools/slicer_linux_runtime/wsl/slicer_linux_runtime_host"

FAKE_ABI1 = r'''#!/bin/sh
set -eu
case "${1:-}" in
    --probe-load) exit 0 ;;
    --probe-stdio-roundtrip) IFS= read -r _ || true; echo SLICER_RUNTIME_STDIO_OK ;;
    *) exit 0 ;;
esac
'''

FAKE_LIMACTL = r'''#!/bin/sh
set -eu
case "${1:-}" in
    list)
        case "$*" in
            *'.Status'*) echo Running ;;
            *'.SSHConfigFile'*) printf '%s\n' "$FAKE_SSH_CONFIG" ;;
            *) exit 1 ;;
        esac
        ;;
    start) exit 0 ;;
    *) exit 1 ;;
esac
'''

FAKE_SSH = r'''#!/usr/bin/env python3
from __future__ import annotations
import os
import subprocess
import sys

if len(sys.argv) < 2:
    raise SystemExit(64)
command = sys.argv[-1]
data = b"" if "-n" in sys.argv else sys.stdin.buffer.read()
env = os.environ.copy()
env["HOME"] = env["FAKE_GUEST_HOME"]
result = subprocess.run(
    command,
    shell=True,
    input=data,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    env=env,
    check=False,
)
sys.stdout.buffer.write(result.stdout)
sys.stderr.buffer.write(result.stderr)
raise SystemExit(result.returncode)
'''


class MacRuntimeWrapperTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="mac-runtime-wrapper-test-")
        self.root = Path(self.temp.name)
        self.app_support = self.root / "app-support"
        self.guest_home = self.root / "guest-home"
        self.bin_dir = self.root / "bin"
        self.component = self.root / "component"
        self.runtime_a = self.root / "source-a/runtime"
        self.runtime_b = self.root / "different/source-b/runtime"
        for path in (self.app_support, self.guest_home, self.bin_dir, self.component):
            path.mkdir(parents=True)

        self.ssh_config = self.root / "ssh.config"
        self.ssh_config.write_text("Host fake-lima\n  HostName 127.0.0.1\n", encoding="utf-8")
        self.limactl = self.bin_dir / "limactl"
        self.ssh = self.bin_dir / "ssh"
        self.limactl.write_text(FAKE_LIMACTL, encoding="utf-8")
        self.ssh.write_text(FAKE_SSH, encoding="utf-8")
        self.limactl.chmod(0o755)
        self.ssh.chmod(0o755)

        self._make_runtime(self.runtime_a)
        shutil.copytree(self.runtime_a, self.runtime_b)
        for name, data in {
            "libbambu_networking.so": b"network-v1",
            "libBambuSource.so": b"source-v1",
            "liblive555.so": b"media-v1",
            "libagora_rtc_sdk.so": b"agora-v1",
            "libagora-fdkaac.so": b"aac-v1",
            "linux_component_manifest.json": b'{"version":"test"}\n',
        }.items():
            (self.component / name).write_bytes(data)

        legacy = self.guest_home / ".local/share/bambustudio-orcaslicer/linux-runtime"
        legacy.mkdir(parents=True)
        (legacy / "stale").write_text("old", encoding="utf-8")

        self.env = os.environ.copy()
        self.env.update(
            {
                "FAKE_GUEST_HOME": str(self.guest_home),
                "FAKE_SSH_CONFIG": str(self.ssh_config),
                "SLICER_LINUX_RUNTIME_LIMACTL": str(self.limactl),
                "SLICER_LINUX_RUNTIME_SSH": str(self.ssh),
                "SLICER_LINUX_RUNTIME_MAC_APP_SUPPORT_DIR": str(self.app_support),
                "SLICER_LINUX_RUNTIME_MAC_LIMA_INSTANCE": "test-instance",
                "SLICER_LINUX_RUNTIME_REQUIRE_COMPATIBLE_HOST": "1",
            }
        )

    def tearDown(self) -> None:
        self.temp.cleanup()

    @staticmethod
    def _make_runtime(path: Path) -> None:
        path.mkdir(parents=True)
        shutil.copy2(DISPATCHER, path / "slicer_linux_runtime_host")
        (path / "slicer_linux_runtime_host_abi1").write_text(FAKE_ABI1, encoding="utf-8")
        (path / "slicer_linux_runtime_host_abi0").write_text(FAKE_ABI1, encoding="utf-8")
        (path / "liborcastudio_rosetta_splitlock_compat.so").write_bytes(b"mock-shim")
        for name in (
            "slicer_linux_auth_browser",
            "slicer_linux_auth_browser_x86_64",
            "slicer_linux_auth_browser_aarch64",
            "run_auth_browser.sh",
        ):
            (path / name).write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        (path / "ca-certificates.crt").write_text(
            "-----BEGIN CERTIFICATE-----\nTEST\n-----END CERTIFICATE-----\n" * 50,
            encoding="utf-8",
        )
        (path / "slicer_base64.cer").write_bytes(b"test")
        for file in path.iterdir():
            if file.name.startswith("slicer_") or file.name == "run_auth_browser.sh":
                file.chmod(0o755)

    def _run(self, runtime: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [str(WRAPPER), str(runtime / "slicer_linux_runtime_host"), str(runtime), str(self.component), "--print-bin"],
            env=self.env,
            stdin=subprocess.DEVNULL,
            text=True,
            capture_output=True,
            timeout=30,
            check=False,
        )


    def test_control_ssh_does_not_consume_runtime_stdin(self) -> None:
        result = subprocess.run(
            [
                str(WRAPPER),
                str(self.runtime_a / "slicer_linux_runtime_host"),
                str(self.runtime_a),
                str(self.component),
                "--probe-stdio-roundtrip",
            ],
            env=self.env,
            input="x",
            text=True,
            capture_output=True,
            timeout=30,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "SLICER_RUNTIME_STDIO_OK\n")

    def test_payload_is_content_addressed_and_path_independent(self) -> None:
        first = self._run(self.runtime_a)
        self.assertEqual(first.returncode, 0, first.stderr)
        first_bin = first.stdout.strip()
        self.assertIn("linux-runtime-payloads/", first_bin)

        second = self._run(self.runtime_b)
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(second.stdout.strip(), first_bin)

        payload_root = self.guest_home / ".local/share/bambustudio-orcaslicer/linux-runtime-payloads"
        payloads = [path for path in payload_root.iterdir() if path.is_dir() and ".tmp." not in path.name and ".old." not in path.name]
        self.assertEqual(len(payloads), 1)
        marker = payloads[0].name
        self.assertEqual((payloads[0] / ".payload-marker").read_text().strip(), marker)
        self.assertFalse((self.guest_home / ".local/share/bambustudio-orcaslicer/linux-runtime").exists())

        (self.component / "libbambu_networking.so").write_bytes(b"network-v2")
        third = self._run(self.runtime_b)
        self.assertEqual(third.returncode, 0, third.stderr)
        self.assertNotEqual(third.stdout.strip(), first_bin)

        payloads_after = [path for path in payload_root.iterdir() if path.is_dir() and ".tmp." not in path.name and ".old." not in path.name]
        self.assertEqual(len(payloads_after), 2)
        self.assertTrue(all((path / ".payload-marker").read_text().strip() == path.name for path in payloads_after))


if __name__ == "__main__":
    unittest.main(verbosity=2)
