#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0

"""Host checks for the Leaf3 framework E-Ink hook patcher."""

import importlib.util
import re
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

NAVIGATION_BAR_INFLATER_SOURCE = """\
package com.android.systemui.statusbar.phone;

import android.content.Context;
import android.graphics.drawable.Icon;

import com.android.systemui.statusbar.policy.KeyButtonView;

public class NavigationBarInflaterView {
    public static final String IME_SWITCHER = "ime_switcher";
    private int mNavBarMode = NAV_BAR_MODE_3BUTTON;

    public NavigationBarInflaterView() {
        mNavBarMode = Dependency.get(NavigationModeController.class).addListener(this);
    }

    protected String getDefaultLayout() {
        final int defaultResource = QuickStepContract.isGesturalMode(mNavBarMode)
                ? R.string.config_navBarLayoutHandle
                : R.string.config_navBarLayout;
        return getContext().getString(defaultResource);
    }

    @Override
    public void onNavigationModeChanged(int mode) {
        mNavBarMode = mode;
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
    }

    @Override
    protected void onDetachedFromWindow() {
        super.onDetachedFromWindow();
    }

    private View createView(String button, ViewGroup parent, LayoutInflater inflater) {
        View v = null;
        if (HOME.equals(button)) {
            v = inflater.inflate(R.layout.home, parent, false);
        } else if (IME_SWITCHER.equals(button)) {
            v = inflater.inflate(R.layout.ime_switcher, parent, false);
        }
        return v;
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

    def run_navigation_refresh_patcher(self, source, *arguments):
        return subprocess.run(
            [
                sys.executable,
                str(
                    self.root
                    / "tools/patch-systemui-leaf3-refresh-button.py"
                ),
                str(source),
                *arguments,
            ],
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

    def test_property_policy_avoids_expanded_appdomain_attribute(self):
        policy = (
            self.root
            / "sepolicy/system_ext/private/leaf3_epdc_bridge.te"
        ).read_text()
        self.assertNotIn("get_prop(appdomain, leaf3_config_prop)", policy)
        self.assertIn("untrusted_app_29", policy)
        self.assertIn("platform_app", policy)
        self.assertIn("system_app", policy)
        self.assertIn(
            "set_prop(platform_app, leaf3_refresh_prop)", policy
        )
        self.assertNotIn(
            "set_prop(platform_app, leaf3_config_prop)", policy
        )
        property_contexts = (
            self.root
            / "sepolicy/system_ext/private/property_contexts"
        ).read_text()
        self.assertIn(
            "sys.leaf3.full_refresh "
            "u:object_r:leaf3_refresh_prop:s0",
            property_contexts,
        )

    def test_sleep_screen_file_is_core_owned_data(self):
        policy = (
            self.root
            / "sepolicy/system_ext/private/leaf3_epdc_bridge.te"
        ).read_text()
        declaration = re.search(
            r"type leaf3_sleep_screen_data_file,([^;]+);", policy
        )
        self.assertIsNotNone(declaration)
        self.assertIn("data_file_type", declaration.group(1))
        self.assertIn("core_data_file_type", declaration.group(1))

    def test_navigation_refresh_default_continues_property_list(self):
        lines = (self.root / "device.mk").read_text().splitlines()
        property_line = "persist.sys.leaf3.nav_refresh_button=0"
        index = next(
            position
            for position, line in enumerate(lines)
            if line.strip() == property_line
        )
        self.assertGreater(index, 0)
        self.assertTrue(
            lines[index - 1].rstrip().endswith("\\"),
            f"{property_line} is not part of the preceding property list",
        )

    def test_navigation_refresh_uses_key_button_drawable(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "NavigationBarInflaterView.java"
            source.write_text(NAVIGATION_BAR_INFLATER_SOURCE)
            result = self.run_navigation_refresh_patcher(source)
            self.assertEqual(result.returncode, 0, result.stderr)
            patched = source.read_text()
            self.assertIn(
                "import com.android.systemui.statusbar.policy."
                "KeyButtonDrawable;",
                patched,
            )
            self.assertIn("KeyButtonDrawable.create(", patched)
            self.assertIn("refreshDrawable.setDarkIntensity(1f);", patched)
            self.assertIn(
                "((KeyButtonView) v).setImageDrawable(refreshDrawable);",
                patched,
            )
            self.assertIn("catch (RuntimeException exception)", patched)
            self.assertIn(
                "Could not request an E-Ink refresh", patched
            )
            self.assertIn(
                "getContext().registerReceiver(mLeaf3RefreshReceiver,",
                patched,
            )
            self.assertIn(
                "getContext().unregisterReceiver(mLeaf3RefreshReceiver);",
                patched,
            )
            self.assertEqual(
                self.run_navigation_refresh_patcher(
                    source, "--check"
                ).returncode,
                0,
            )

        layout = (
            self.root
            / "overlay/frameworks/base/packages/SystemUI/res/layout/"
            "leaf3_refresh.xml"
        ).read_text()
        self.assertNotIn("android:src=", layout)
        self.assertNotIn("android:tint=", layout)

    def test_navigation_refresh_upgrades_unsafe_vector_patch(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "NavigationBarInflaterView.java"
            source.write_text(NAVIGATION_BAR_INFLATER_SOURCE)
            self.assertEqual(
                self.run_navigation_refresh_patcher(source).returncode,
                0,
            )
            patcher = importlib.util.spec_from_file_location(
                "leaf3_navigation_refresh_patcher",
                self.root
                / "tools/patch-systemui-leaf3-refresh-button.py",
            )
            module = importlib.util.module_from_spec(patcher)
            patcher.loader.exec_module(module)
            old = (
                source.read_text()
                .replace(
                    module.CREATE_VIEW, module.PREVIOUS_CREATE_VIEW
                )
                .replace(module.BROADCAST_IMPORTS, "")
                .replace(module.ACTION_CONSTANT, "")
                .replace(module.FIELD, module.OLD_FIELD)
                .replace(module.CONSTRUCTOR, module.OLD_CONSTRUCTOR)
                .replace(module.HELPER, module.OLD_HELPER)
                .replace(module.LIFECYCLE, module.DETACH_MARKER)
            )
            source.write_text(old)

            check = self.run_navigation_refresh_patcher(source, "--check")
            self.assertNotEqual(check.returncode, 0)
            self.assertIn("outdated", check.stderr)

            upgrade = self.run_navigation_refresh_patcher(source)
            self.assertEqual(upgrade.returncode, 0, upgrade.stderr)
            upgraded = source.read_text()
            self.assertIn(module.CREATE_VIEW, upgraded)
            self.assertIn(module.KEY_BUTTON_DRAWABLE_IMPORT, upgraded)
            self.assertNotIn(module.PREVIOUS_CREATE_VIEW, upgraded)
            self.assertIn(
                "import android.content.BroadcastReceiver;", upgraded
            )
            self.assertIn("import android.content.Intent;", upgraded)
            self.assertIn(
                "import android.content.IntentFilter;", upgraded
            )
            self.assertIn(module.ACTION_CONSTANT, upgraded)
            self.assertIn(module.FIELD, upgraded)
            self.assertIn(module.CONSTRUCTOR, upgraded)
            self.assertNotIn(module.OLD_CONSTRUCTOR, upgraded)
            self.assertIn(module.HELPER, upgraded)
            self.assertIn(module.LIFECYCLE, upgraded)

    def test_navigation_refresh_repairs_partial_notification_upgrade(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "NavigationBarInflaterView.java"
            source.write_text(NAVIGATION_BAR_INFLATER_SOURCE)
            self.assertEqual(
                self.run_navigation_refresh_patcher(source).returncode,
                0,
            )
            patcher = importlib.util.spec_from_file_location(
                "leaf3_partial_navigation_refresh_patcher",
                self.root
                / "tools/patch-systemui-leaf3-refresh-button.py",
            )
            module = importlib.util.module_from_spec(patcher)
            patcher.loader.exec_module(module)
            partial = (
                source.read_text()
                .replace(module.BROADCAST_IMPORTS, "")
                .replace(module.FIELD, module.OLD_FIELD)
                .replace(module.HELPER, module.OLD_HELPER)
                .replace(module.LIFECYCLE, module.DETACH_MARKER)
            )
            self.assertIn(module.ACTION_CONSTANT, partial)
            self.assertIn(module.CONSTRUCTOR, partial)
            source.write_text(partial)

            upgrade = self.run_navigation_refresh_patcher(source)
            self.assertEqual(upgrade.returncode, 0, upgrade.stderr)
            upgraded = source.read_text()
            self.assertIn(module.FIELD, upgraded)
            self.assertIn(module.HELPER, upgraded)
            self.assertIn(module.LIFECYCLE, upgraded)
            self.assertEqual(
                self.run_navigation_refresh_patcher(
                    source, "--check"
                ).returncode,
                0,
            )

    def test_controls_backend_switch_requires_confirmation_and_reboot(self):
        activity = (
            self.root
            / "app/src/org/lineageos/leaf3controls/MainActivity.java"
        ).read_text()
        layout = (self.root / "app/res/layout/activity_main.xml").read_text()
        manifest = (self.root / "app/AndroidManifest.xml").read_text()
        privapp_permissions = (
            self.root
            / "app/privapp-permissions-org.lineageos.leaf3controls.xml"
        ).read_text()
        build_script = (self.root / "build-lineage-arch.sh").read_text()
        self.assertIn("confirmBackendSwitch(", activity)
        self.assertIn(
            "SystemProperties.set(Leaf3Settings.EPDC_BACKEND, backend)",
            activity,
        )
        self.assertIn("powerManager.reboot(null)", activity)
        self.assertIn("R.string.backend_composer_confirmation", activity)
        self.assertIn('android:id="@+id/backend_modes"', layout)
        self.assertIn('android:id="@+id/apply_backend"', layout)
        self.assertIn("android.permission.REBOOT", manifest)
        self.assertIn("android.permission.REBOOT", privapp_permissions)
        self.assertIn(
            "new Intent(Leaf3Settings.NAV_REFRESH_BUTTON_CHANGED)",
            activity,
        )
        self.assertIn('.setPackage("com.android.systemui")', activity)
        self.assertIn(
            "Leaf3 Controls allowlist is missing backend-switch reboot access",
            build_script,
        )


if __name__ == "__main__":
    unittest.main()
