#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0

"""Force dark three-button navigation icons for the Leaf3's white E-Ink bar."""

import argparse
from pathlib import Path


OLD_CALL = "mNavigationBarController.setIconsDark(mNavigationLight, animateChange());"
NEW_CALL = "mNavigationBarController.setIconsDark(true, animateChange());"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("light_bar_controller", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    path = args.light_bar_controller
    text = path.read_text()

    if NEW_CALL in text:
        print(f"{path}: navigation icons are pinned dark")
        return 0

    if args.check:
        parser.error(f"{path}: navigation icons can still be requested white")
    if text.count(OLD_CALL) != 1:
        parser.error(f"{path}: expected exactly one navigation icon color update")

    path.write_text(text.replace(OLD_CALL, NEW_CALL, 1))
    print(f"{path}: pinned navigation icons dark")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
