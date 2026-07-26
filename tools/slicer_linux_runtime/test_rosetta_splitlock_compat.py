#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
from __future__ import annotations

import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE_DIR = ROOT / "tools/slicer_linux_runtime_host"
SOURCE = SOURCE_DIR / "rosetta_splitlock_compat.c"
UNIT_SOURCE = SOURCE_DIR / "test_rosetta_splitlock_compat.c"
SIGNAL_SOURCE = SOURCE_DIR / "test_rosetta_splitlock_signal.c"
CONCURRENCY_SOURCE = SOURCE_DIR / "test_rosetta_splitlock_concurrency.c"


def run(*args: str, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        args,
        cwd=ROOT,
        env=env,
        text=True,
        capture_output=True,
        timeout=60,
        check=False,
    )
    if result.returncode != 0:
        if result.stdout:
            sys.stderr.write(result.stdout)
        if result.stderr:
            sys.stderr.write(result.stderr)
        raise subprocess.CalledProcessError(
            result.returncode, args, output=result.stdout, stderr=result.stderr
        )
    return result


def find_compiler() -> str:
    requested = os.environ.get("SLICER_LINUX_RUNTIME_ROSETTA_SPLITLOCK_CC") or os.environ.get("CC")
    candidates = [requested] if requested else ["gcc-14", "clang", "cc"]
    for candidate in candidates:
        if candidate:
            compiler = shutil.which(candidate)
            if compiler:
                return compiler
    raise SystemExit(
        "No supported C compiler found. Set SLICER_LINUX_RUNTIME_ROSETTA_SPLITLOCK_CC "
        "to gcc-14, clang, or another compatible x86_64 compiler."
    )


def main() -> int:
    if platform.machine().lower() not in {"x86_64", "amd64"}:
        print("Rosetta split-lock compatibility build test skipped on non-x86_64 host")
        return 0

    compiler = find_compiler()
    print(f"Rosetta split-lock compiler: {compiler}")

    with tempfile.TemporaryDirectory(prefix="rosetta-splitlock-test-") as temporary:
        build = Path(temporary)
        shim = build / "liborcastudio_rosetta_splitlock_compat.so"
        unit = build / "test_rosetta_splitlock_compat"
        signal_test = build / "test_rosetta_splitlock_signal"
        concurrency_test = build / "test_rosetta_splitlock_concurrency"
        common = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"]

        run(
            compiler,
            *common,
            "-fPIC",
            "-shared",
            "-fvisibility=hidden",
            "-Wl,-z,relro,-z,now",
            "-Wl,--no-undefined",
            "-o",
            str(shim),
            str(SOURCE),
        )
        run(compiler, *common, "-o", str(unit), str(UNIT_SOURCE))
        unit_result = run(str(unit))
        if "tests OK" not in unit_result.stdout:
            raise SystemExit("Rosetta split-lock decoder/emulator unit test did not complete")

        run(compiler, *common, "-o", str(signal_test), str(SIGNAL_SOURCE))
        env = os.environ.copy()
        env.update(
            {
                "LD_PRELOAD": str(shim),
                "SLICER_LINUX_RUNTIME_ROSETTA_SPLITLOCK_COMPAT": "1",
            }
        )
        signal_result = run(str(signal_test), env=env)
        if "xadd-old=11 xadd-new=18 xchg-old=31 xchg-new=23" not in signal_result.stdout:
            raise SystemExit("Rosetta split-lock/xchg signal-path integration result is invalid")
        if "compatibility enabled" not in signal_result.stderr:
            raise SystemExit("Rosetta split-lock compatibility constructor did not run")

        run(compiler, *common, "-pthread", "-o", str(concurrency_test), str(CONCURRENCY_SOURCE))
        concurrency_result = run(str(concurrency_test), env=env)
        if "counter=32000 expected=32000" not in concurrency_result.stdout:
            raise SystemExit("Rosetta split-lock concurrent emulation result is invalid")

    print("Rosetta split-lock compatibility verification OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
