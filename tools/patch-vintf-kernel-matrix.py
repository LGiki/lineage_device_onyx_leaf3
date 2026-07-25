#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0

"""Allow the stock ONYX 4.19 kernel's autosleep configuration in FCM 5."""

import argparse
from pathlib import Path
import re


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("matrix", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    text = args.matrix.read_text()
    kernel_match = re.search(
        r'<kernel version="4\.19\.110" level="5">.*?</kernel>',
        text,
        flags=re.DOTALL,
    )
    if not kernel_match:
        parser.error(f"{args.matrix}: Android 11 kernel 4.19 FCM block not found")

    block = kernel_match.group(0)
    config_pattern = re.compile(
        r"(<key>CONFIG_PM_AUTOSLEEP</key>\s*"
        r'<value type="tristate">)([ny])'
        r"(</value>)"
    )
    config_match = config_pattern.search(block)
    if not config_match:
        parser.error(f"{args.matrix}: CONFIG_PM_AUTOSLEEP requirement not found")

    current = config_match.group(2)
    if current == "y":
        print(f"{args.matrix}: stock ONYX autosleep requirement is present")
        return 0
    if args.check:
        parser.error(f"{args.matrix}: CONFIG_PM_AUTOSLEEP still requires n")

    patched_block, replacements = config_pattern.subn(r"\1y\3", block, count=1)
    if replacements != 1:
        parser.error(f"{args.matrix}: expected exactly one replacement")

    patched = text[: kernel_match.start()] + patched_block + text[kernel_match.end() :]
    args.matrix.write_text(patched)
    print(f"{args.matrix}: allowed CONFIG_PM_AUTOSLEEP=y for kernel 4.19")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
