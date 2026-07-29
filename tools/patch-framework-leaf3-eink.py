#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0

"""Install Leaf3 per-process animation filtering and transient view hints."""

import argparse
from pathlib import Path


PATCHER_VERSION = "2"
VIEW_HOOK = "        Leaf3EinkHelper.noteTouchDispatch(this, event, result);\n"
VIEW_GROUP_HOOK = (
    "        Leaf3EinkHelper.noteTouchDispatch(this, ev, handled);\n"
)
ANIMATOR_OLD = """\
    private float resolveDurationScale() {
        return mDurationScale >= 0f ? mDurationScale : sDurationScale;
    }
"""
ANIMATOR_NEW = """\
    private float resolveDurationScale() {
        if (android.view.Leaf3EinkHelper.animationsFilteredForProcess()) {
            return 0f;
        }
        return mDurationScale >= 0f ? mDurationScale : sDurationScale;
    }
"""


def method_end(text: str, signature: str) -> int:
    start = text.find(signature)
    if start < 0:
        raise ValueError(f"missing {signature.strip()}")
    opening = text.find("{", start)
    if opening < 0:
        raise ValueError(f"missing body for {signature.strip()}")
    depth = 0
    for index in range(opening, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return index
    raise ValueError(f"unterminated body for {signature.strip()}")


def patch_dispatch(text: str, signature: str, return_statement: str,
                   hook: str) -> str:
    if hook in text:
        return text
    end = method_end(text, signature)
    method = text[text.find(signature):end]
    marker = return_statement + "\n"
    marker_at = method.rfind(marker)
    if marker_at < 0:
        raise ValueError(f"missing final {return_statement.strip()}")
    absolute = text.find(signature) + marker_at
    return text[:absolute] + hook + text[absolute:]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("frameworks_base", type=Path)
    parser.add_argument("--check", action="store_true")
    parser.add_argument(
        "--version",
        action="version",
        version=f"Leaf3 framework E-Ink patcher {PATCHER_VERSION}",
    )
    args = parser.parse_args()

    root = args.frameworks_base.resolve()
    view = root / "core/java/android/view/View.java"
    view_group = root / "core/java/android/view/ViewGroup.java"
    animator = root / "core/java/android/animation/ValueAnimator.java"
    helper = root / "core/java/android/view/Leaf3EinkHelper.java"
    template = (
        Path(__file__).resolve().parent.parent
        / "frameworks/base/core/java/android/view/Leaf3EinkHelper.java"
    )
    try:
        view_text = patch_dispatch(
            view.read_text(),
            "    public boolean dispatchTouchEvent(MotionEvent event) {",
            "        return result;",
            VIEW_HOOK,
        )
        view_group_text = patch_dispatch(
            view_group.read_text(),
            "    public boolean dispatchTouchEvent(MotionEvent ev) {",
            "        return handled;",
            VIEW_GROUP_HOOK,
        )
        animator_text = animator.read_text()
        if ANIMATOR_NEW not in animator_text:
            if ANIMATOR_OLD not in animator_text:
                raise ValueError("missing ValueAnimator duration-scale method")
            animator_text = animator_text.replace(
                ANIMATOR_OLD, ANIMATOR_NEW, 1
            )
        helper_text = template.read_text()
    except (OSError, ValueError) as error:
        parser.error(str(error))

    expected = {
        view: view_text,
        view_group: view_group_text,
        animator: animator_text,
        helper: helper_text,
    }
    if args.check:
        stale = [
            str(path)
            for path, content in expected.items()
            if not path.exists() or path.read_text() != content
        ]
        if stale:
            parser.error("Leaf3 framework E-Ink patch is stale: " +
                         ", ".join(stale))
        print(f"{root}: Leaf3 framework E-Ink patch is present")
        return 0

    for path, content in expected.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.exists() or path.read_text() != content:
            path.write_text(content)
    print(f"{root}: installed Leaf3 framework E-Ink hooks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
