#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0

"""Keep LineageOS 18.1 AssistManager's UI Handler on the main looper."""

import argparse
from pathlib import Path


HANDLER_IMPORT = "import android.os.Handler;\n"
LOOPER_IMPORT = "import android.os.Looper;\n"
OLD_CONSTRUCTION = (
    "mAssistDisclosure = new AssistDisclosure(context, new Handler());"
)
NEW_CONSTRUCTION = (
    "mAssistDisclosure = new AssistDisclosure(\n"
    "                context, new Handler(Looper.getMainLooper()));"
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("assist_manager", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    path = args.assist_manager
    text = path.read_text()

    if NEW_CONSTRUCTION in text and LOOPER_IMPORT in text:
        print(f"{path}: AssistDisclosure uses the main looper")
        return 0

    if args.check:
        parser.error(f"{path}: AssistDisclosure still creates a thread-local Handler")
    if text.count(OLD_CONSTRUCTION) != 1:
        parser.error(
            f"{path}: expected exactly one legacy AssistDisclosure construction"
        )
    if text.count(HANDLER_IMPORT) != 1:
        parser.error(f"{path}: expected exactly one Handler import")

    text = text.replace(HANDLER_IMPORT, HANDLER_IMPORT + LOOPER_IMPORT, 1)
    text = text.replace(OLD_CONSTRUCTION, NEW_CONSTRUCTION, 1)
    path.write_text(text)
    print(f"{path}: pinned AssistDisclosure to the main looper")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
