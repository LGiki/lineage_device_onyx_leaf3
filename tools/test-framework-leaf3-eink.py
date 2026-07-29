#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0

"""Host checks for the Leaf3 framework E-Ink hook patcher."""

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True


def load_patcher(root):
    path = root / "tools/patch-framework-leaf3-eink.py"
    spec = importlib.util.spec_from_file_location(
        "leaf3_framework_eink_patcher", path
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


VIEW_SOURCE = """\
package android.view;

public class View {
    public boolean dispatchTouchEvent(MotionEvent event) {
        boolean result = false;
        if (event != null) {
            result = true;
        }
        return result;
    }
}
"""

VIEW_GROUP_SOURCE = """\
package android.view;

public class ViewGroup extends View {
    public boolean dispatchTouchEvent(MotionEvent ev) {
        boolean handled = false;
        if (ev != null) {
            handled = super.dispatchTouchEvent(ev);
        }
        return handled;
    }
}
"""

ANIMATOR_SOURCE = """\
package android.animation;

public class ValueAnimator {
    private float mDurationScale;
    private static float sDurationScale;

    private float resolveDurationScale() {
        return mDurationScale >= 0f ? mDurationScale : sDurationScale;
    }
}
"""


class FrameworkEinkPatcherTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(__file__).resolve().parent.parent
        cls.patcher_path = cls.root / "tools/patch-framework-leaf3-eink.py"
        cls.patcher = load_patcher(cls.root)

    def make_fixture(self, temporary):
        root = Path(temporary)
        files = {
            "core/java/android/view/View.java": VIEW_SOURCE,
            "core/java/android/view/ViewGroup.java": VIEW_GROUP_SOURCE,
            "core/java/android/animation/ValueAnimator.java": ANIMATOR_SOURCE,
        }
        for relative, content in files.items():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        return root

    def run_patcher(self, root, *arguments):
        return subprocess.run(
            [sys.executable, str(self.patcher_path), str(root), *arguments],
            check=False,
            capture_output=True,
            text=True,
        )

    def test_installs_touch_and_animation_hooks(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_fixture(temporary)
            result = self.run_patcher(root)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(
                self.patcher.VIEW_HOOK.strip(),
                (root / "core/java/android/view/View.java").read_text(),
            )
            self.assertIn(
                self.patcher.VIEW_GROUP_HOOK.strip(),
                (root / "core/java/android/view/ViewGroup.java").read_text(),
            )
            self.assertIn(
                "Leaf3EinkHelper.animationsFilteredForProcess()",
                (
                    root
                    / "core/java/android/animation/ValueAnimator.java"
                ).read_text(),
            )
            self.assertEqual(
                (
                    root / "core/java/android/view/Leaf3EinkHelper.java"
                ).read_text(),
                (
                    self.root
                    / "frameworks/base/core/java/android/view/"
                    "Leaf3EinkHelper.java"
                ).read_text(),
            )
            helper = (
                root / "core/java/android/view/Leaf3EinkHelper.java"
            ).read_text()
            self.assertIn(
                "SystemProperties.getBoolean(SCROLL_DETECT, true)", helper
            )
            self.assertIn(
                "SystemProperties.getBoolean(FILTER_ANIMATIONS, false)",
                helper,
            )
            self.assertIn(
                "if (!sTransientHintsEnabled || view == null", helper
            )
            self.assertIn(
                "postFrameCallback(HINT_FRAME_CALLBACK)", helper
            )
            self.assertIn(
                "if (sPendingHintCommand != PENDING_HINT_SET)", helper
            )
            self.assertIn(
                "addOnPreDrawListener(sFlingDrawListener)", helper
            )
            self.assertIn("FLING_QUIET_TIMEOUT_MS", helper)
            self.assertIn("viewMotionSignature(flingView)", helper)
            self.assertIn("sGestureHinted = false", helper)
            self.assertIn(
                "if (sGestureHinted) {\n"
                "                    startFlingRenewal();",
                helper,
            )
            self.assertNotIn("sLastFlingDraw", helper)
            self.assertIn(
                "Process.myUid() == SystemProperties.getInt(ACTIVE_UID, -1)",
                helper,
            )
            self.assertNotIn(
                "case MotionEvent.ACTION_MOVE:\n"
                "                if (!handled || !sHaveDown)",
                helper,
            )

    def test_is_idempotent_and_check_detects_drift(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_fixture(temporary)
            self.assertEqual(self.run_patcher(root).returncode, 0)
            first = (root / "core/java/android/view/View.java").read_text()
            self.assertEqual(self.run_patcher(root).returncode, 0)
            self.assertEqual(
                (root / "core/java/android/view/View.java").read_text(), first
            )
            self.assertEqual(self.run_patcher(root, "--check").returncode, 0)
            helper = root / "core/java/android/view/Leaf3EinkHelper.java"
            helper.write_text(helper.read_text() + "\n")
            self.assertNotEqual(
                self.run_patcher(root, "--check").returncode, 0
            )


if __name__ == "__main__":
    unittest.main()
