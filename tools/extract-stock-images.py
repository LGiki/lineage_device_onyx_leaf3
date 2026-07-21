#!/usr/bin/env python3
"""Extract the Lineage bring-up partitions from a full Android OTA payload."""

import os
import sys

PARTITIONS = (
    "boot",
    "dtbo",
    "product",
    "system",
    "system_ext",
    "vbmeta",
    "vbmeta_system",
    "vendor",
)


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: extract-stock-images.py <extractor-dir> <payload.bin> <out-dir>", file=sys.stderr)
        return 2

    extractor_dir, payload_path, out_dir = map(os.path.abspath, sys.argv[1:])
    sys.path.insert(0, extractor_dir)
    from extract_android_ota_payload import Payload, PayloadError, parse_payload

    os.makedirs(out_dir, exist_ok=True)
    with open(payload_path, "rb") as payload_file:
        payload = Payload(payload_file)
        payload.Init()
        available = {part.partition_name: part for part in payload.manifest.partitions}
        missing = set(PARTITIONS) - set(available)
        if missing:
            raise PayloadError(f"OTA does not contain: {', '.join(sorted(missing))}")
        for name in PARTITIONS:
            print(f"Extracting {name}.img")
            with open(os.path.join(out_dir, f"{name}.img"), "wb") as output:
                parse_payload(payload, available[name], output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
