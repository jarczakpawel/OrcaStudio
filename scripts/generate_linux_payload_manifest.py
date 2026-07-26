#!/usr/bin/env python3
import argparse
import hashlib
import json
from pathlib import Path

DEFAULT_ABI_VERSION = "02.08.01"
MANIFEST_NAME = "linux_component_manifest.json"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("plugin_dir", type=Path)
    parser.add_argument("--abi-version", default=DEFAULT_ABI_VERSION)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()

    plugin_dir = args.plugin_dir.resolve()
    network = plugin_dir / "libbambu_networking.so"
    source = plugin_dir / "libBambuSource.so"
    if not network.is_file() or not source.is_file():
        raise SystemExit("missing linux payload files")

    files = [
        {"name": network.name, "sha256": sha256(network), "abi_version": args.abi_version},
        {"name": source.name, "sha256": sha256(source)},
    ]
    for name in ("liblive555.so", "libagora_rtc_sdk.so", "libagora-fdkaac.so"):
        path = plugin_dir / name
        if path.is_file():
            files.append({"name": path.name, "sha256": sha256(path)})

    output = args.out.resolve() if args.out else plugin_dir / MANIFEST_NAME
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".tmp")
    temporary.write_text(json.dumps({"schema": 1, "files": files}, indent=2) + "\n", encoding="utf-8")
    temporary.replace(output)
    print(output)


if __name__ == "__main__":
    main()
