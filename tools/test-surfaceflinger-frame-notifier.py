#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0

"""Host checks for the Leaf3 SurfaceFlinger notifier upgrade."""

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True


def load_patcher(root):
    path = root / "tools/patch-surfaceflinger-frame-notifier.py"
    spec = importlib.util.spec_from_file_location(
        "leaf3_frame_notifier_patcher", path
    )
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class FrameNotifierPatcherTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = Path(__file__).resolve().parent.parent
        cls.patcher_path = (
            cls.root / "tools/patch-surfaceflinger-frame-notifier.py"
        )
        cls.patcher = load_patcher(cls.root)

    def version_two_fixture(self):
        patcher = self.patcher
        return "".join(
            (
                patcher.UNIQUE_FD_INCLUDE,
                patcher.LEAF3_EPDC_INCLUDE,
                patcher.FCNTL_INCLUDE,
                patcher.UNISTD_INCLUDE,
                patcher.PREVIOUS_V2_NOTIFIER_STATE,
                patcher.PREVIOUS_V2_CREDENTIAL_HOOK,
                patcher.PREVIOUS_V2_TRANSACTION_HOOK,
                patcher.POST_FRAME_HOOK,
                "const Rect damage = dirtyRegion.getBounds();\n",
                "gLeaf3FrameDamageValid = true;\n",
                "std::max(gLeaf3FrameDamage.bottom, damage.bottom);\n",
            )
        )

    def run_patcher(self, source, *arguments):
        return subprocess.run(
            [
                sys.executable,
                str(self.patcher_path),
                str(source),
                *arguments,
            ],
            check=False,
            capture_output=True,
            text=True,
        )

    def test_upgrades_unowned_transient_protocol(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "SurfaceFlinger.cpp"
            source.write_text(
                "".join(
                    (
                        self.patcher.UNIQUE_FD_INCLUDE,
                        self.patcher.LEAF3_EPDC_INCLUDE,
                        self.patcher.FCNTL_INCLUDE,
                        self.patcher.UNISTD_INCLUDE,
                        self.patcher.PREVIOUS_TRANSIENT_V1_NOTIFIER_STATE,
                        self.patcher.CREDENTIAL_HOOK,
                        self.patcher.PREVIOUS_UNOWNED_TRANSACTION_HOOK,
                        self.patcher.POST_FRAME_HOOK,
                        "const Rect damage = dirtyRegion.getBounds();\n",
                        "gLeaf3FrameDamageValid = true;\n",
                        "std::max(gLeaf3FrameDamage.bottom, damage.bottom);\n",
                    )
                )
            )
            self.assertNotEqual(
                self.run_patcher(source, "--check").returncode, 0
            )
            result = self.run_patcher(source)
            self.assertEqual(result.returncode, 0, result.stderr)
            upgraded = source.read_text()
            self.assertIn(self.patcher.TRANSACTION_HOOK, upgraded)
            self.assertNotIn(
                self.patcher.PREVIOUS_UNOWNED_TRANSACTION_HOOK, upgraded
            )
            self.assertEqual(
                self.run_patcher(source, "--check").returncode, 0
            )

    def test_upgrades_version_one_transient_protocol(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "SurfaceFlinger.cpp"
            source.write_text(
                "".join(
                    (
                        self.patcher.UNIQUE_FD_INCLUDE,
                        self.patcher.LEAF3_EPDC_INCLUDE,
                        self.patcher.FCNTL_INCLUDE,
                        self.patcher.UNISTD_INCLUDE,
                        self.patcher.PREVIOUS_TRANSIENT_V1_NOTIFIER_STATE,
                        self.patcher.CREDENTIAL_HOOK,
                        self.patcher.PREVIOUS_TRANSIENT_V1_TRANSACTION_HOOK,
                        self.patcher.POST_FRAME_HOOK,
                    )
                )
            )
            self.assertNotEqual(
                self.run_patcher(source, "--check").returncode, 0
            )
            result = self.run_patcher(source)
            self.assertEqual(result.returncode, 0, result.stderr)
            upgraded = source.read_text()
            self.assertIn(self.patcher.NOTIFIER_STATE, upgraded)
            self.assertIn(self.patcher.TRANSACTION_HOOK, upgraded)
            self.assertEqual(
                self.run_patcher(source, "--check").returncode, 0
            )

    def test_upgrades_version_two_protocol(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "SurfaceFlinger.cpp"
            source.write_text(self.version_two_fixture())
            before = self.run_patcher(source, "--check")
            self.assertNotEqual(before.returncode, 0)
            result = self.run_patcher(source)
            self.assertEqual(result.returncode, 0, result.stderr)
            upgraded = source.read_text()
            self.assertIn(self.patcher.NOTIFIER_STATE, upgraded)
            self.assertIn(self.patcher.CREDENTIAL_HOOK, upgraded)
            self.assertIn(self.patcher.TRANSACTION_HOOK, upgraded)
            self.assertNotIn(
                self.patcher.PREVIOUS_V2_NOTIFIER_STATE, upgraded
            )
            self.assertEqual(
                self.run_patcher(source, "--check").returncode, 0
            )

    def test_protocol_is_bounded_and_returns_transient_damage(self):
        transaction = self.patcher.TRANSACTION_HOOK
        credentials = self.patcher.CREDENTIAL_HOOK
        self.assertIn("durationMs < 100 || durationMs > 2000", transaction)
        self.assertIn("const int32_t pageTurn = data.readInt32()", transaction)
        self.assertIn("const int32_t gestureId = data.readInt32()", transaction)
        self.assertIn("(pageTurn != 0 && pageTurn != 1)", transaction)
        self.assertIn("gestureId <= 0", transaction)
        self.assertIn("pageTurn != 0", transaction)
        self.assertIn("region.right > 16384", transaction)
        self.assertIn("takeNotifierDamage(", transaction)
        self.assertIn("reply->writeInt32(transientHint.left)", transaction)
        self.assertIn(
            "CheckTransactCodeCredentials(code)", transaction
        )
        self.assertIn("kLeaf3ActiveUidProperty", credentials)
        self.assertIn(
            "callingUid == static_cast<uid_t>(activeUid)", credentials
        )
        self.assertIn(
            "static_cast<int32_t>(callingUid)", transaction
        )
        self.assertNotIn(
            "code == kLeaf3TransientHintTransaction ||", credentials
        )

    def test_composer_owned_damage_patch_is_recognized(self):
        patcher = self.patcher
        source = "".join(
            (
                patcher.UNIQUE_FD_INCLUDE,
                patcher.LEAF3_EPDC_INCLUDE,
                patcher.FCNTL_INCLUDE,
                patcher.UNISTD_INCLUDE,
                patcher.NOTIFIER_STATE,
                patcher.CREDENTIAL_HOOK,
                patcher.TRANSACTION_HOOK,
                patcher.POST_FRAME_HOOK,
            )
        )
        self.assertTrue(patcher.patched(source))

    def test_controller_header_defines_nanosecond_type(self):
        header = (
            self.root
            / "frameworks/native/services/surfaceflinger"
            / "Leaf3EpdcController.h"
        ).read_text()
        self.assertIn("#include <utils/Timers.h>", header)
        self.assertIn(
            "const Rect &region, nsecs_t duration, int32_t ownerUid,\n"
            "                        bool pageTurn, int32_t gestureId",
            header,
        )


if __name__ == "__main__":
    unittest.main()
