#!/usr/bin/env python3
"""Validate the exported Ubuntu rootfs used by the Windows WSL runtime."""

from __future__ import annotations

import posixpath
import sys
import tarfile
from pathlib import Path
from typing import NoReturn

DEFAULT_MARKER = "ubuntu-24.04-linux-auth-v3"
MARKER_MEMBER = "etc/orcastudio-linux-auth-runtime"
MANIFEST_MEMBER = "etc/orcastudio-linux-auth-runtime.manifest"
REQUIRED_MANIFEST_KEYS = (
    "ca",
    "xvfb",
    "xvfb_run",
    "xdpyinfo",
    "xkbcomp",
    "xkb_data",
    "x11vnc",
    "websockify",
    "python3",
    "browser",
    "novnc",
)


def fail(message: str) -> NoReturn:
    raise SystemExit(f"WSL rootfs validation failed: {message}")


def normalize_member(name: str) -> str:
    value = name.rstrip("\r\n")
    while value.startswith("./"):
        value = value[2:]
    value = value.lstrip("/")
    value = posixpath.normpath(value)
    if value in ("", "."):
        return ""
    if value == ".." or value.startswith("../"):
        fail(f"unsafe archive member path: {name!r}")
    return value.rstrip("/")


def read_member_text(archive: tarfile.TarFile, member: tarfile.TarInfo, label: str) -> str:
    stream = archive.extractfile(member)
    if stream is None:
        fail(f"{label} is not a readable regular file: {member.name}")
    try:
        return stream.read().decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        fail(f"{label} is not valid UTF-8: {exc}")


def parse_manifest(text: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for line_number, raw_line in enumerate(text.splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            fail(f"invalid manifest line {line_number}: {raw_line!r}")
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip()
        if not key or not value:
            fail(f"invalid manifest assignment on line {line_number}: {raw_line!r}")
        if key in result:
            fail(f"duplicate manifest key: {key}")
        result[key] = value
    return result


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print(f"Usage: {Path(sys.argv[0]).name} <rootfs.tar> [expected-marker]", file=sys.stderr)
        return 2

    tar_path = Path(sys.argv[1])
    expected_marker = sys.argv[2] if len(sys.argv) == 3 else DEFAULT_MARKER
    if not tar_path.is_file() or tar_path.stat().st_size <= 0:
        fail(f"archive is missing or empty: {tar_path}")

    try:
        archive = tarfile.open(tar_path, mode="r:*")
    except (tarfile.TarError, OSError) as exc:
        fail(f"cannot open archive {tar_path}: {exc}")

    with archive:
        entries: dict[str, tarfile.TarInfo] = {}
        for member in archive.getmembers():
            normalized = normalize_member(member.name)
            if normalized and normalized not in entries:
                entries[normalized] = member

        for required in (MARKER_MEMBER, MANIFEST_MEMBER):
            if required not in entries:
                fail(f"missing archive member: {required}")

        marker = read_member_text(archive, entries[MARKER_MEMBER], "runtime marker").strip()
        if marker != expected_marker:
            fail(f"runtime marker mismatch; expected {expected_marker!r}, got {marker!r}")

        manifest = parse_manifest(
            read_member_text(archive, entries[MANIFEST_MEMBER], "runtime manifest")
        )
        if manifest.get("schema") != "1":
            fail(f"unsupported runtime manifest schema: {manifest.get('schema')!r}")
        if manifest.get("marker") != expected_marker:
            fail(
                "runtime manifest marker mismatch; "
                f"expected {expected_marker!r}, got {manifest.get('marker')!r}"
            )

        missing_keys = [key for key in REQUIRED_MANIFEST_KEYS if key not in manifest]
        if missing_keys:
            fail("runtime manifest is missing keys: " + ", ".join(missing_keys))

        missing_members: list[str] = []
        normalized_paths: dict[str, str] = {}
        for key in REQUIRED_MANIFEST_KEYS:
            raw_path = manifest[key]
            if not raw_path.startswith("/"):
                fail(f"manifest path {key} is not absolute: {raw_path!r}")
            normalized = normalize_member(raw_path)
            normalized_paths[key] = normalized
            if normalized not in entries:
                missing_members.append(f"{key}={raw_path}")
        if missing_members:
            fail("manifest paths missing from archive: " + ", ".join(missing_members))

        ca_member = entries[normalized_paths["ca"]]
        ca_text = read_member_text(archive, ca_member, "CA certificate bundle")
        certificate_count = ca_text.count("-----BEGIN CERTIFICATE-----")
        if len(ca_text.encode("utf-8")) < 65536 or certificate_count < 50:
            fail(
                "CA certificate bundle is incomplete: "
                f"bytes={len(ca_text.encode('utf-8'))}, certificates={certificate_count}"
            )

        xkb_member = entries[normalized_paths["xkb_data"]]
        if not xkb_member.isfile() or xkb_member.size <= 0:
            fail(f"XKB rules file is missing or empty: {manifest['xkb_data']}")

    print(
        "WSL rootfs validation OK: "
        f"marker={expected_marker}, manifest_paths={len(REQUIRED_MANIFEST_KEYS)}, "
        f"archive={tar_path}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
