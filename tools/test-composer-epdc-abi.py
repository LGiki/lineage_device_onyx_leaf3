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
        self.assertIn(
            "now, readerPageCandidate, pageInterval",
            controller,
        )
        self.assertIn("mImpl->readerPages.advanceGesture(", controller)
        self.assertIn(
            "mImpl->readerPages.advanceScroll(now, pageInterval)",
            controller,
        )
        self.assertIn("mImpl->transientPageTurn", controller)
        self.assertIn(
            "trackTransientGhosts = leaf3ShouldTrackTransientGhosts(",
            controller,
        )
        self.assertIn(
            "mode == \"reader\" && mImpl->transientPageTurn",
            controller,
        )
        self.assertIn(
            "readerPageTurnActive && cleanup != \"manual\" &&\n"
            "      mImpl->cleanupSchedule.pending()",
            controller,
        )
        self.assertIn(
            "mImpl->transientPageTurn && !transient.isEmpty()",
            controller,
        )
        prepare = controller.index("Leaf3EpdcController::preparePresent(")
        postpone = controller.index(
            "cleanup already owed by earlier scrolling", prepare
        )
        forced_cleanup = controller.index(
            "const nsecs_t cleanupDeadline =", prepare
        )
        self.assertLess(postpone, forced_cleanup)
        self.assertIn("cleanup != \"manual\" && pageInterval > 0", controller)
        self.assertIn("readerPages.settle(now, supportedPageInterval())", controller)
        self.assertRegex(
            controller,
            r"(?s)makeReaderFullRefreshLocked\(\).*?"
            r"return \{\s*makeUpdateLocked\(bounds,\s*"
            r"kWaveformGc16 \| kModeFull \| ditherFlag\(\)\)",
        )
        self.assertIn(
            "readerPresentResult == Leaf3ReaderPageResult::FullRefresh",
            controller,
        )
        self.assertIn(
            "if (!android::base::GetBoolProperty(kInteractiveProperty, true)) {\n"
            "    mImpl->discardPendingWorkLocked();",
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
        self.assertIn("kEbcIoctlRetryAttempts", bridge)
        self.assertIn("resetEbcState", bridge)
        self.assertIn("sleep-screen update failed; keeping the bridge alive", bridge)
        self.assertIn("if (source != nullptr)", bridge)
        self.assertNotIn("return 1;", bridge)
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
    static_assert(shouldRunLeaf3Timers(true, true));
    static_assert(!shouldRunLeaf3Timers(true, false));
    static_assert(!shouldRunLeaf3Timers(false, true));
    static_assert(isLeaf3CommitEpdcCommand(kLeaf3CommitEpdcCommand));
    static_assert(!isLeaf3CommitEpdcCommand(0x08010000));
    static_assert(leaf3TransientHintAuthorized(10123, 10123));
    static_assert(!leaf3TransientHintAuthorized(10123, 10124));
    static_assert(!leaf3TransientHintAuthorized(-1, -1));
    static_assert(leaf3ReaderPageCandidate(true, true, true, false, false));
    static_assert(leaf3ReaderPageCandidate(true, false, false, false, false));
    static_assert(leaf3ReaderPageCandidate(true, false, false, true, true));
    static_assert(!leaf3ReaderPageCandidate(true, false, false, true, false));
    static_assert(!leaf3ReaderPageCandidate(true, false, true, false, false));
    static_assert(!leaf3ReaderPageCandidate(false, true, false, false, false));
    static_assert(leaf3ShouldTrackTransientGhosts(true, false));
    static_assert(!leaf3ShouldTrackTransientGhosts(true, true));
    static_assert(!leaf3ShouldTrackTransientGhosts(false, false));

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

    Leaf3ReaderPageSchedule readerPages;
    readerPages.notePresent(1000000000, true);
    readerPages.notePresent(1100000000, true);
    readerPages.notePresent(1200000000, false);
    if (readerPages.deadline() != 1380000000) return 13;
    if (readerPages.settle(1379999999, 2) !=
            Leaf3ReaderPageResult::None) return 14;
    if (readerPages.settle(1380000000, 2) !=
            Leaf3ReaderPageResult::Counted) return 15;
    if (readerPages.pageCount() != 1 || readerPages.deadline() != 0) return 16;
    readerPages.notePresent(2000000000, true);
    if (readerPages.settle(2180000000, 2) !=
            Leaf3ReaderPageResult::FullRefresh) return 17;
    if (readerPages.pageCount() != 0 || readerPages.deadline() != 0) return 18;
    readerPages.notePresent(3000000000, true);
    if (readerPages.settle(3180000000, 0) !=
            Leaf3ReaderPageResult::Counted) return 19;
    if (readerPages.pageCount() != 0) return 20;
    readerPages.notePresent(4000000000, false);
    if (readerPages.deadline() != 0) return 21;
    readerPages.notePresent(4100000000, true);
    readerPages.reset();
    if (readerPages.deadline() != 0 || readerPages.pageCount() != 0) return 22;
    readerPages.notePresent(5000000000, true);
    if (readerPages.advancePresent(5200000000, true, 2) !=
            Leaf3ReaderPageResult::Counted) return 23;
    if (readerPages.deadline() != 5380000000) return 24;
    if (readerPages.settle(5380000000, 2) !=
            Leaf3ReaderPageResult::FullRefresh) return 25;
    if (readerPages.pageCount() != 0) return 26;
    readerPages.notePresent(6000000000, true);
    if (readerPages.advancePresent(6050000000, true, 2) !=
            Leaf3ReaderPageResult::None) return 27;
    if (readerPages.settle(6230000000, 2) !=
            Leaf3ReaderPageResult::Counted) return 28;
    readerPages.notePresent(7000000000, true);
    if (readerPages.advancePresent(7180000000, true, 2) !=
            Leaf3ReaderPageResult::Counted) return 29;
    if (readerPages.deadline() != 7360000000 ||
            readerPages.pageCount() != 0) return 30;
    if (readerPages.settle(7360000000, 2) !=
            Leaf3ReaderPageResult::FullRefresh) return 64;
    readerPages.reset();
    if (readerPages.advanceGesture(7500000000, 10, 3) !=
            Leaf3ReaderPageResult::Counted) return 31;
    if (readerPages.advanceGesture(7500000001, 10, 3) !=
            Leaf3ReaderPageResult::None) return 32;
    if (readerPages.pageCount() != 1) return 33;
    if (readerPages.advanceGesture(7600000000, 11, 3) !=
            Leaf3ReaderPageResult::Counted) return 34;
    if (readerPages.advanceGesture(7700000000, 12, 3) !=
            Leaf3ReaderPageResult::Counted) return 35;
    if (readerPages.pageCount() != 0 ||
            readerPages.deadline() != 7880000000) return 36;
    if (readerPages.advanceGesture(7800000000, 12, 3) !=
            Leaf3ReaderPageResult::None) return 54;
    if (readerPages.deadline() != 7980000000) return 55;
    if (readerPages.settle(7979999999, 3) !=
            Leaf3ReaderPageResult::None) return 56;
    if (readerPages.settle(7980000000, 3) !=
            Leaf3ReaderPageResult::FullRefresh) return 57;
    readerPages.reset();
    readerPages.notePresent(8000000000, true);
    if (readerPages.advanceGesture(8179999999, 20, 2) !=
            Leaf3ReaderPageResult::Counted) return 37;
    if (readerPages.pageCount() != 1 || readerPages.deadline() != 0) return 38;
    if (readerPages.advanceGesture(8180000000, 20, 2) !=
            Leaf3ReaderPageResult::None) return 39;
    if (readerPages.advanceGesture(8180000000, 0, 2) !=
            Leaf3ReaderPageResult::None) return 40;
    readerPages.reset();
    readerPages.notePresent(9000000000, true);
    if (readerPages.advanceGesture(9180000000, 21, 10) !=
            Leaf3ReaderPageResult::Counted) return 41;
    if (readerPages.pageCount() != 2 || readerPages.deadline() != 0) return 42;
    readerPages.reset();
    if (readerPages.advanceGesture(10000000000, 30, 2) !=
            Leaf3ReaderPageResult::Counted) return 43;
    readerPages.notePresent(10100000000, true);
    if (readerPages.advanceGesture(10280000000, 31, 2) !=
            Leaf3ReaderPageResult::Counted) return 44;
    if (readerPages.pageCount() != 0 ||
            readerPages.deadline() != 10460000000) return 45;
    if (readerPages.settle(10460000000, 2) !=
            Leaf3ReaderPageResult::FullRefresh) return 65;
    readerPages.reset();
    readerPages.notePresent(11000000000, true);
    if (readerPages.advanceGesture(11180000000, 40, 2) !=
            Leaf3ReaderPageResult::Counted) return 46;
    if (readerPages.pageCount() != 0 ||
            readerPages.deadline() != 11360000000) return 47;
    if (readerPages.settle(11360000000, 2) !=
            Leaf3ReaderPageResult::FullRefresh) return 58;
    readerPages.reset();
    readerPages.notePresent(12000000000, true);
    if (readerPages.advanceScroll(12179999999, 10) !=
            Leaf3ReaderPageResult::None) return 48;
    if (readerPages.pageCount() != 0 || readerPages.deadline() != 0) return 49;
    readerPages.notePresent(13000000000, true);
    if (readerPages.advanceScroll(13180000000, 10) !=
            Leaf3ReaderPageResult::Counted) return 50;
    if (readerPages.pageCount() != 1 || readerPages.deadline() != 0) return 51;
    readerPages.notePresent(14000000000, true);
    if (readerPages.advanceScroll(14180000000, 2) !=
            Leaf3ReaderPageResult::Counted) return 52;
    if (readerPages.pageCount() != 0 ||
            readerPages.deadline() != 14360000000) return 53;
    if (readerPages.settle(14360000000, 2) !=
            Leaf3ReaderPageResult::FullRefresh) return 66;
    readerPages.reset();
    if (readerPages.advanceGesture(15000000000, 50, 1) !=
            Leaf3ReaderPageResult::Counted) return 59;
    if (readerPages.deadline() != 15180000000) return 60;
    if (readerPages.advanceGesture(15100000000, 51, 1) !=
            Leaf3ReaderPageResult::None) return 61;
    if (readerPages.pageCount() != 0 ||
            readerPages.deadline() != 15280000000) return 62;
    if (readerPages.advancePresent(15280000000, false, 1) !=
            Leaf3ReaderPageResult::Counted) return 63;
    if (readerPages.deadline() != 15460000000) return 67;
    if (readerPages.settle(15460000000, 1) !=
            Leaf3ReaderPageResult::FullRefresh) return 68;

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
