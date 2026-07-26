#!/bin/bash
set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 /path/OrcaStudio.app" >&2
    exit 2
fi

app="$1"
binary="$app/Contents/MacOS/OrcaStudio"

if [ ! -x "$binary" ]; then
    echo "ERROR: OrcaStudio executable not found or not executable: $binary" >&2
    exit 1
fi

codesign --verify --deep --strict --verbose=4 "$app"

python3 - "$app" <<'PY'
import os
import re
import subprocess
import sys

app = sys.argv[1]
errors = []
for directory, _, files in os.walk(app):
    for name in files:
        path = os.path.join(directory, name)
        try:
            kind_result = subprocess.run(["file", path], stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
            kind = kind_result.stdout.decode("utf-8", errors="replace")
        except OSError as exc:
            raise SystemExit(f"ERROR: unable to inspect {path}: {exc}")
        if "Mach-O" not in kind:
            continue
        out = subprocess.run(["otool", "-l", path], stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        if out.returncode != 0:
            errors.append(f"unable to read load commands: {path}")
            continue
        lines = out.stdout.decode("utf-8", errors="replace").splitlines()
        expect_path = False
        for line in lines:
            fields = line.split()
            if len(fields) >= 2 and fields[0] == "cmd" and fields[1] == "LC_RPATH":
                expect_path = True
                continue
            if expect_path:
                match = re.match(r"^\s*path (.+) \(offset \d+\)$", line)
                if match:
                    rpath = match.group(1)
                    if not rpath.startswith("@"):
                        errors.append(f"non-portable LC_RPATH: {path}: {rpath}")
                    expect_path = False
if errors:
    for error in errors:
        print(f"ERROR: {error}", file=sys.stderr)
    raise SystemExit(1)
PY

if command -v syspolicy_check >/dev/null 2>&1; then
    policy_output="$(syspolicy_check distribution "$app" 2>&1 || true)"
    printf '%s\n' "$policy_output"
    if printf '%s\n' "$policy_output" | grep -F "Bad Load Command" >/dev/null; then
        echo "ERROR: syspolicy_check found a bad Mach-O load command" >&2
        exit 1
    fi
fi

python3 - "$binary" <<'PY'
import subprocess
import sys

binary = sys.argv[1]


def emit_raw(output: bytes | None) -> bytes:
    data = output or b""
    if data:
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
    return data


try:
    proc = subprocess.run(
        [binary, "--help"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=45,
        check=False,
    )
except subprocess.TimeoutExpired as exc:
    emit_raw(exc.stdout)
    raise SystemExit("ERROR: OrcaStudio --help did not exit within 45 seconds")

stdout_bytes = emit_raw(proc.stdout)
if proc.returncode != 0:
    raise SystemExit(f"ERROR: OrcaStudio --help exited with code {proc.returncode}")
if b"Usage:" not in stdout_bytes:
    raise SystemExit("ERROR: OrcaStudio --help did not produce the expected usage text")
PY
