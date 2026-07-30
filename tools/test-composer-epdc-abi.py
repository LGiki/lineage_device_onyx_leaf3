#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0

"""Host checks for the Leaf3/QTI CommitEpdc command layout."""

import importlib.util
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True


COMMAND = 0x08020000
WORDS_PER_UPDATE = 5
MAXIMUM_UPDATES = 8


def load_patcher(root):
    path = root / "tools/patch-lineage-composer-epdc.py"
    spec = importlib.util.spec_from_file_location("leaf3_composer_epdc_patcher", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def encode(updates):
    if not 1 <= len(updates) <= MAXIMUM_UPDATES:
        raise ValueError("CommitEpdc accepts between one and eight updates")
    payload = [len(updates)]
    for update in updates:
        if len(update) != WORDS_PER_UPDATE:
            raise ValueError("an EPDC update must contain five words")
        payload.extend(update)
    header = COMMAND | len(payload)
    return struct.pack(f"<{len(payload) + 1}I", header, *payload)


class CommitEpdcAbiTest(unittest.TestCase):
    def test_single_update(self):
        encoded = encode([(8, 16, 320, 640, 0x105)])
        self.assertEqual(
            struct.unpack("<7I", encoded),
            (0x08020006, 1, 8, 16, 320, 640, 0x105),
        )

    def test_maximum_batch(self):
        updates = [(index, 2, 3, 4, 5) for index in range(8)]
        words = struct.unpack("<42I", encode(updates))
        self.assertEqual(words[0], 0x08020029)
        self.assertEqual(words[1], 8)
        self.assertEqual(words[-5:], updates[-1])

    def test_invalid_batch_sizes(self):
        with self.assertRaises(ValueError):
            encode([])
        with self.assertRaises(ValueError):
            encode([(0, 0, 1, 1, 1)] * 9)

    def test_invalid_record_size(self):
        with self.assertRaises(ValueError):
            encode([(0, 0, 1, 1)])

    def test_production_writer_uses_the_locked_abi(self):
        root = Path(__file__).resolve().parent.parent
        patcher = load_patcher(root)
        controller = (
            root
            / "frameworks/native/services/surfaceflinger/Leaf3EpdcController.cpp"
        ).read_text()
        bridge = (root / "display/leaf3_epdc_bridge.cpp").read_text()
        self.assertNotIn("#define LOG_TAG", controller)
        fixture = """\
namespace impl {

Composer::Composer(const std::string& serviceName) {
    if (mClient == nullptr) {
        LOG_ALWAYS_FATAL("failed to create composer client");
    }
}

Error Composer::execute() {
    for (const auto& cmdErr : commandErrors) {
        auto command =
                static_cast<IComposerClient::Command>(mWriter.getCommand(cmdErr.location));

            if (command == IComposerClient::Command::VALIDATE_DISPLAY ||
                command == IComposerClient::Command::PRESENT_DISPLAY ||
                command == IComposerClient::Command::PRESENT_OR_VALIDATE_DISPLAY) {
            error = cmdErr.error;
        }
    }
}

Error Composer::presentDisplay(Display display, int* outPresentFence)
{
}
"""
        generated = patcher.patch_composer_cpp(fixture)
        expected_writer = """\
    const uint16_t length = static_cast<uint16_t>(1 + updates.size() * 5);
    beginCommand(static_cast<V2_1::IComposerClient::Command>(kLeaf3CommitEpdcCommand), length);
    write(static_cast<uint32_t>(updates.size()));
    for (const auto& update : updates) {
        write(update.left);
        write(update.top);
        write(update.right);
        write(update.bottom);
        write(update.mode);
    }
    endCommand();
"""
        self.assertIn(expected_writer, generated)
        self.assertLess(
            generated.index("mWriter.commitLeaf3Epdc(updates);"),
            generated.index("Error Composer::presentDisplay"),
        )
        self.assertIn(
            "vendor.qti.hardware.display.composer@3.0::IQtiComposerClient",
            generated,
        )
        self.assertIn(
            "isLeaf3CommitEpdcCommand(static_cast<uint32_t>(command))",
            generated,
        )
        composer_path = "services/surfaceflinger/DisplayHardware/ComposerHal.cpp"
        self.assertTrue(
            all(
                marker in generated
                for marker in patcher.REQUIRED_MARKERS[composer_path]
            )
        )
        corrupted = generated.replace(
            "        write(update.top);",
            "        write(update.left);",
            1,
        )
        self.assertFalse(
            all(
                marker in corrupted
                for marker in patcher.REQUIRED_MARKERS[composer_path]
            )
        )
        corrupted_capability = generated.replace(
            "                mSupportsLeaf3Epdc = true;",
            "                mSupportsLeaf3Epdc = false;",
            1,
        )
        self.assertFalse(
            all(
                marker in corrupted_capability
                for marker in patcher.REQUIRED_MARKERS[composer_path]
            )
        )
        corrupted_error_path = generated.replace(
            "                isLeaf3CommitEpdcCommand(static_cast<uint32_t>(command))",
            "                false",
            1,
        )
        self.assertFalse(
            all(
                marker in corrupted_error_path
                for marker in patcher.REQUIRED_MARKERS[composer_path]
            )
        )
        self.assertIn(
            "constexpr size_t kMaximumBatch = kLeaf3MaximumUpdates",
            controller,
        )
        self.assertIn(
            "if (rectArea(candidate) > maximumArea)",
            controller,
        )
        self.assertIn(
            'if (cleanup == "manual")',
            controller,
        )
        self.assertIn(
            "invalidates a settled pass prepared for an older composition",
            controller,
        )
        self.assertIn(
            "const bool pendingNeedsDispatch = shouldDispatchLeaf3Wake(",
            controller,
        )
        self.assertIn(
            "deadline = armLeaf3ImmediateDeadline(systemTime(), armedDeadline);",
            controller,
        )
        self.assertIn(
            "!shouldActivateLeaf3Controller(",
            controller,
        )
        self.assertNotIn(
            "if (requested) {\n      worker = std::thread",
            controller,
        )
        self.assertIn(
            "mImpl->refreshCallback = std::move(callback);\n"
            "  mImpl->activateLocked();",
            controller,
        )
        self.assertIn(
            "kCleanupPolicyPollInterval, wakePending",
            controller,
        )
        self.assertIn("blockAndWaitForIdle()", controller)
        self.assertIn("kSubmissionDrainDelay", controller)
        self.assertIn(
            "requestComposerCommand(kLeaf3FrameNotifierBlockAndWait)",
            bridge,
        )
        self.assertIn(
            "reply.dataAvail() < sizeof(int32_t)",
            bridge,
        )
        self.assertIn(
            "return android::BAD_VALUE;",
            bridge,
        )
        self.assertIn(
            "damage.intersectsDirty(*frame.transient_hint)",
            bridge,
        )
        self.assertIn(
            "leaf3TransientHintAuthorized(mImpl->transientUid, activeUid)",
            controller,
        )
        self.assertIn(
            "else if (!settings.interactive)",
            bridge,
        )
        self.assertIn(
            "__system_property_wait(nullptr, serial, &next, nullptr)",
            bridge,
        )
        self.assertIn("android::ui::Rotation::Rotation0", bridge)
        self.assertIn(
            'std::string(propertyValue(capture_mode, "notify")) == "poll"',
            bridge,
        )
        self.assertIn(
            "setRefreshCallback([this] { signalRefresh(); })",
            patcher.REFRESH_CALLBACK,
        )
        self.assertNotIn(
            "repaintEverything",
            patcher.REFRESH_CALLBACK,
        )

    def test_validation_markers_cover_critical_paths(self):
        root = Path(__file__).resolve().parent.parent
        patcher = load_patcher(root)
        upgraded_header = patcher.upgrade_hwcomposer_h(
            patcher.PREVIOUS_ABSTRACT_EPDC_DAMAGE
            + patcher.WRAPPED_EPDC_DAMAGE_OVERRIDE
        )
        self.assertEqual(
            upgraded_header,
            patcher.DEFAULT_EPDC_DAMAGE + patcher.EPDC_DAMAGE_OVERRIDE,
        )
        upgraded_cpp = patcher.upgrade_hwcomposer_cpp(
            patcher.FORMATTED_INJECTED_COMPOSER_CONSTRUCTOR
            + patcher.FORMATTED_SERVICE_COMPOSER_CONSTRUCTOR
            + patcher.PREVIOUS_FORMATTED_EPDC_DAMAGE_IMPLEMENTATION
        )
        self.assertEqual(
            upgraded_cpp,
            patcher.INJECTED_COMPOSER_CONSTRUCTOR
            + patcher.SERVICE_COMPOSER_CONSTRUCTOR
            + patcher.EPDC_DAMAGE_IMPLEMENTATION,
        )
        upgraded_hwc2 = patcher.upgrade_hwc2_h(
            patcher.PREVIOUS_ABSTRACT_COMMIT_EPDC
            + patcher.WRAPPED_COMMIT_EPDC_OVERRIDE
        )
        self.assertEqual(
            upgraded_hwc2,
            patcher.DEFAULT_COMMIT_EPDC + patcher.COMMIT_EPDC_OVERRIDE,
        )
        upgraded_composer = patcher.upgrade_composer_h(
            patcher.PREVIOUS_ABSTRACT_SUPPORTS_EPDC
            + patcher.PREVIOUS_ABSTRACT_COMPOSER_COMMIT_EPDC
            + patcher.WRAPPED_COMPOSER_COMMIT_EPDC_OVERRIDE
        )
        self.assertEqual(
            upgraded_composer,
            patcher.DEFAULT_SUPPORTS_EPDC
            + patcher.DEFAULT_COMPOSER_COMMIT_EPDC
            + patcher.COMPOSER_COMMIT_EPDC_OVERRIDE,
        )
        self.assertEqual(
            patcher.upgrade_composer_cpp(
                patcher.FORMATTED_QTI_COMPOSER_DESCRIPTOR
            ),
            patcher.QTI_COMPOSER_DESCRIPTOR,
        )
        self.assertEqual(
            patcher.upgrade_surfaceflinger_cpp(
                patcher.FORMATTED_OLD_REFRESH_CALLBACK
            ),
            patcher.REFRESH_CALLBACK,
        )
        upgraded_surfaceflinger = patcher.upgrade_surfaceflinger_cpp(
            '#include "Leaf3EpdcController.h"\n'
            + patcher.DEFAULT_FORCE_CLIENT_COMPOSITION
        )
        self.assertEqual(
            upgraded_surfaceflinger,
            '#include "Leaf3EpdcController.h"\n'
            + patcher.LEAF3_FORCE_CLIENT_COMPOSITION,
        )
        formatted_surfaceflinger = patcher.upgrade_surfaceflinger_cpp(
            '#include "Leaf3EpdcController.h"\n'
            "    refreshArgs.devOptForceClientComposition =\n"
            "        mDebugDisableHWC ||\n"
            "        Leaf3EpdcController::get().isActive();\n"
        )
        self.assertEqual(
            formatted_surfaceflinger,
            '#include "Leaf3EpdcController.h"\n'
            + patcher.LEAF3_FORCE_CLIENT_COMPOSITION,
        )
        macro_wrapped_surfaceflinger = patcher.upgrade_surfaceflinger_cpp(
            '#include "Leaf3EpdcController.h"\n'
            "    refreshArgs.devOptForceClientComposition =\n"
            "            mDebugDisableHWC ||\n"
            "            CC_UNLIKELY(Leaf3EpdcController::get().isActive());\n"
        )
        self.assertEqual(
            macro_wrapped_surfaceflinger,
            '#include "Leaf3EpdcController.h"\n'
            + patcher.LEAF3_FORCE_CLIENT_COMPOSITION,
        )
        wrapped_old_surfaceflinger = patcher.upgrade_surfaceflinger_cpp(
            '#include "Leaf3EpdcController.h"\n'
            "    refreshArgs.devOptForceClientComposition =\n"
            "            mDebugDisableHWC;\n"
        )
        self.assertEqual(
            wrapped_old_surfaceflinger,
            '#include "Leaf3EpdcController.h"\n'
            + patcher.LEAF3_FORCE_CLIENT_COMPOSITION,
        )
        prior_markers = list(
            patcher.REQUIRED_MARKERS[
                "services/surfaceflinger/SurfaceFlinger.cpp"
            ]
        )
        prior_markers.remove(patcher.LEAF3_FORCE_CLIENT_COMPOSITION)
        upgraded_partial_surfaceflinger = patcher.upgrade_surfaceflinger_cpp(
            "\n".join(prior_markers)
            + "\n    refreshArgs.devOptForceClientComposition =\n"
            "        mDebugDisableHWC ||\n"
            "        Leaf3EpdcController::get().isActive();\n"
        )
        self.assertTrue(
            all(
                marker in upgraded_partial_surfaceflinger
                for marker in patcher.REQUIRED_MARKERS[
                    "services/surfaceflinger/SurfaceFlinger.cpp"
                ]
            )
        )
        fresh_surfaceflinger = patcher.upgrade_surfaceflinger_cpp(
            patcher.DEFAULT_FORCE_CLIENT_COMPOSITION
        )
        self.assertEqual(
            fresh_surfaceflinger,
            patcher.DEFAULT_FORCE_CLIENT_COMPOSITION,
        )
        surfaceflinger_fixture = (
            '#include "LayerVector.h"\n'
            "SurfaceFlinger::~SurfaceFlinger() = default;\n"
            "    mCompositionEngine->getHwComposer().setConfiguration(this, "
            "getBE().mComposerSequenceId);\n"
            + patcher.DEFAULT_FORCE_CLIENT_COMPOSITION
            + '    result.append("\\nDisplay identification data:\\n");\n'
        )
        patched_surfaceflinger = patcher.patch_surfaceflinger_cpp(
            surfaceflinger_fixture
        )
        self.assertNotIn(
            patcher.DEFAULT_FORCE_CLIENT_COMPOSITION,
            patched_surfaceflinger,
        )
        self.assertTrue(
            all(
                marker in patched_surfaceflinger
                for marker in patcher.REQUIRED_MARKERS[
                    "services/surfaceflinger/SurfaceFlinger.cpp"
                ]
            )
        )
        hwcomposer = "\n".join(
            patcher.REQUIRED_MARKERS[
                "services/surfaceflinger/DisplayHardware/HWComposer.cpp"
            ]
        )
        composer = "\n".join(
            patcher.REQUIRED_MARKERS[
                "services/surfaceflinger/DisplayHardware/ComposerHal.cpp"
            ]
        )
        self.assertIn("preparePresent(damage, bounds)", hwcomposer)
        self.assertIn('composerFailed("commitEpdc")', hwcomposer)
        self.assertIn('composerFailed("present")', hwcomposer)
        self.assertIn("beginSubmission()", hwcomposer)
        self.assertIn("endSubmission()", hwcomposer)
        self.assertIn("mSupportsLeaf3Epdc = true", composer)
        self.assertIn(
            "isLeaf3CommitEpdcCommand(static_cast<uint32_t>(command))",
            composer,
        )
        surfaceflinger = "\n".join(
            patcher.REQUIRED_MARKERS[
                "services/surfaceflinger/SurfaceFlinger.cpp"
            ]
        )
        self.assertIn(
            "mDebugDisableHWC || Leaf3EpdcController::get().isActive()",
            surfaceflinger,
        )

    def test_production_policy_behavior(self):
        compiler = shutil.which("c++") or shutil.which("clang++")
        if compiler is None:
            self.fail("a C++ compiler is required for the production policy test")
        root = Path(__file__).resolve().parent.parent
        source = """\
#include "frameworks/native/services/surfaceflinger/Leaf3EpdcPolicy.h"

using namespace android;

int main() {
    constexpr auto timerPresent = leaf3PresentPolicy(false);
    static_assert(!timerPresent.restartCleanup && !timerPresent.cancelSettled);
    constexpr auto realPresent = leaf3PresentPolicy(true);
    static_assert(realPresent.restartCleanup && realPresent.cancelSettled);
    static_assert(shouldDispatchLeaf3Wake(true, false, true));
    static_assert(!shouldDispatchLeaf3Wake(false, false, true));
    static_assert(!shouldDispatchLeaf3Wake(true, true, true));
    static_assert(!shouldDispatchLeaf3Wake(true, false, false));
    static_assert(armLeaf3ImmediateDeadline(100, 0) == 100);
    static_assert(armLeaf3ImmediateDeadline(100, 200) == 100);
    static_assert(armLeaf3ImmediateDeadline(100, 90) == 90);
    static_assert(armLeaf3ImmediateDeadline(
                          101, armLeaf3ImmediateDeadline(100, 0)) == 100);
    static_assert(shouldActivateLeaf3Controller(true, true, false, false, true));
    static_assert(
            !shouldActivateLeaf3Controller(false, true, false, false, true));
    static_assert(
            !shouldActivateLeaf3Controller(true, false, false, false, true));
    static_assert(
            !shouldActivateLeaf3Controller(true, true, true, false, true));
    static_assert(
            !shouldActivateLeaf3Controller(true, true, false, true, true));
    static_assert(
            !shouldActivateLeaf3Controller(true, true, false, false, false));
    static_assert(isLeaf3CommitEpdcCommand(kLeaf3CommitEpdcCommand));
    static_assert(!isLeaf3CommitEpdcCommand(0x08010000));
    static_assert(leaf3TransientHintAuthorized(10123, 10123));
    static_assert(!leaf3TransientHintAuthorized(10123, 10124));
    static_assert(!leaf3TransientHintAuthorized(-1, -1));

    const Leaf3PolicyRect separatedDamage[] = {
        {0, 0, 100, 10},
        {0, 90, 100, 100},
    };
    const auto separated = splitLeaf3TransientDamage(
            separatedDamage, 2, Leaf3PolicyRect{0, 20, 100, 80});
    if (separated.overflow || separated.normalCount != 2 ||
        separated.transientCount != 0) return 9;

    const Leaf3PolicyRect fullDamage[] = {{0, 0, 100, 100}};
    const auto partitioned = splitLeaf3TransientDamage(
            fullDamage, 1, Leaf3PolicyRect{20, 20, 80, 80});
    if (partitioned.overflow || partitioned.normalCount != 4 ||
        partitioned.transientCount != 1) return 10;
    uint64_t partitionedArea = 0;
    for (size_t index = 0; index < partitioned.normalCount; ++index) {
        const auto& rect = partitioned.normal[index];
        partitionedArea += static_cast<uint64_t>(rect.right - rect.left) *
                static_cast<uint64_t>(rect.bottom - rect.top);
    }
    for (size_t index = 0; index < partitioned.transientCount; ++index) {
        const auto& rect = partitioned.transient[index];
        partitionedArea += static_cast<uint64_t>(rect.right - rect.left) *
                static_cast<uint64_t>(rect.bottom - rect.top);
    }
    if (partitionedArea != 10000) return 11;

    const Leaf3PolicyRect overflowingDamage[] = {
        {0, 0, 10, 10}, {10, 0, 20, 10}, {20, 0, 30, 10},
        {30, 0, 40, 10}, {40, 0, 50, 10}, {50, 0, 60, 10},
        {60, 0, 70, 10}, {70, 0, 80, 10}, {80, 0, 90, 10},
    };
    const auto overflowing = splitLeaf3TransientDamage(
            overflowingDamage, 9, Leaf3PolicyRect{0, 20, 100, 30});
    if (!overflowing.overflow) return 12;

    Leaf3CleanupSchedule cleanup;
    cleanup.noteActivity(1000000000);
    if (cleanup.deadline(Leaf3CleanupPolicy::Quality) != 1300000000) return 1;
    if (cleanup.deadline(Leaf3CleanupPolicy::Balanced) != 1600000000) return 2;
    if (cleanup.deadline(Leaf3CleanupPolicy::Manual) != 0) return 3;
    if (cleanup.nextCheck(1100000000, Leaf3CleanupPolicy::Balanced, 50000000, false) !=
        1150000000) return 6;
    if (cleanup.nextCheck(1290000000, Leaf3CleanupPolicy::Quality, 50000000, false) !=
        1300000000) return 7;
    if (cleanup.nextCheck(1100000000, Leaf3CleanupPolicy::Balanced, 50000000, true) !=
        0) return 8;
    cleanup.complete(1300000000, true);
    if (cleanup.deadline(Leaf3CleanupPolicy::Quality) != 1600000000) return 4;
    cleanup.complete(1600000000, false);
    if (cleanup.pending()) return 5;
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as temporary:
            temporary_path = Path(temporary)
            source_path = temporary_path / "policy_test.cpp"
            binary_path = temporary_path / "policy_test"
            source_path.write_text(source)
            subprocess.run(
                [
                    compiler,
                    "-std=c++17",
                    "-Wall",
                    "-Werror",
                    "-I",
                    str(root),
                    str(source_path),
                    "-o",
                    str(binary_path),
                ],
                check=True,
            )
            subprocess.run([str(binary_path)], check=True)


if __name__ == "__main__":
    unittest.main()
