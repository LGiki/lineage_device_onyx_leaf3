#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0

"""Install the Leaf3 composer-native EPDC transport into LineageOS 18.1."""

import argparse
from pathlib import Path


PATCHER_VERSION = "7"

PREVIOUS_ABSTRACT_EPDC_DAMAGE = (
    "    virtual void setLeaf3EpdcDamage(DisplayId displayId, const Region& damage,\n"
    "                                    const Rect& bounds) = 0;\n"
)
DEFAULT_EPDC_DAMAGE = (
    "    virtual void setLeaf3EpdcDamage(DisplayId, const Region&, const Rect&) {}\n"
)
WRAPPED_EPDC_DAMAGE_OVERRIDE = (
    "    void setLeaf3EpdcDamage(DisplayId displayId, const Region& damage,\n"
    "                            const Rect& bounds) override;\n"
)
EPDC_DAMAGE_OVERRIDE = (
    "    void setLeaf3EpdcDamage(DisplayId displayId, const Region& damage, "
    "const Rect& bounds) override;\n"
)
FORMATTED_INJECTED_COMPOSER_CONSTRUCTOR = """\
HWComposer::HWComposer(std::unique_ptr<Hwc2::Composer> composer) : mComposer(std::move(composer)) {
    Leaf3EpdcController::get().setComposerSupported(
            mComposer->supportsLeaf3Epdc());
}
"""
INJECTED_COMPOSER_CONSTRUCTOR = """\
HWComposer::HWComposer(std::unique_ptr<Hwc2::Composer> composer) : mComposer(std::move(composer)) {
    Leaf3EpdcController::get().setComposerSupported(mComposer->supportsLeaf3Epdc());
}
"""
FORMATTED_SERVICE_COMPOSER_CONSTRUCTOR = """\
HWComposer::HWComposer(const std::string& composerServiceName)
      : mComposer(std::make_unique<Hwc2::impl::Composer>(composerServiceName)) {
    Leaf3EpdcController::get().setComposerSupported(
            mComposer->supportsLeaf3Epdc());
}
"""
SERVICE_COMPOSER_CONSTRUCTOR = """\
HWComposer::HWComposer(const std::string& composerServiceName)
      : mComposer(std::make_unique<Hwc2::impl::Composer>(composerServiceName)) {
    Leaf3EpdcController::get().setComposerSupported(mComposer->supportsLeaf3Epdc());
}
"""
PREVIOUS_FORMATTED_EPDC_DAMAGE_IMPLEMENTATION = """\
void HWComposer::setLeaf3EpdcDamage(DisplayId displayId, const Region& damage,
                                    const Rect& bounds) {
    RETURN_IF_INVALID_DISPLAY(displayId);
    auto& displayData = mDisplayData[displayId];
    if (displayData.isVirtual) {
        return;
    }
    displayData.leaf3EpdcUpdates =
            Leaf3EpdcController::get().preparePresent(damage, bounds);
}
"""
EPDC_DAMAGE_IMPLEMENTATION = """\
void HWComposer::setLeaf3EpdcDamage(DisplayId displayId, const Region& damage, const Rect& bounds) {
    RETURN_IF_INVALID_DISPLAY(displayId);
    auto& displayData = mDisplayData[displayId];
    if (displayData.isVirtual || !mInternalHwcDisplayId ||
        displayData.hwcDisplay->getId() != *mInternalHwcDisplayId) {
        return;
    }
    displayData.leaf3EpdcUpdates = Leaf3EpdcController::get().preparePresent(damage, bounds);
}
"""
PREVIOUS_ABSTRACT_COMMIT_EPDC = """\
    [[clang::warn_unused_result]] virtual hal::Error commitLeaf3Epdc(
            const std::vector<Leaf3EpdcUpdate>& updates) = 0;
"""
DEFAULT_COMMIT_EPDC = """\
    [[clang::warn_unused_result]] virtual hal::Error commitLeaf3Epdc(
            const std::vector<Leaf3EpdcUpdate>&) {
        return hal::Error::UNSUPPORTED;
    }
"""
WRAPPED_COMMIT_EPDC_OVERRIDE = """\
    hal::Error commitLeaf3Epdc(
            const std::vector<Leaf3EpdcUpdate>& updates) override;
"""
COMMIT_EPDC_OVERRIDE = (
    "    hal::Error commitLeaf3Epdc("
    "const std::vector<Leaf3EpdcUpdate>& updates) override;\n"
)
PREVIOUS_ABSTRACT_SUPPORTS_EPDC = (
    "    virtual bool supportsLeaf3Epdc() const = 0;\n"
)
DEFAULT_SUPPORTS_EPDC = (
    "    virtual bool supportsLeaf3Epdc() const { return false; }\n"
)
PREVIOUS_ABSTRACT_COMPOSER_COMMIT_EPDC = """\
    virtual Error commitLeaf3Epdc(
            Display display, const std::vector<Leaf3EpdcUpdate>& updates) = 0;
"""
DEFAULT_COMPOSER_COMMIT_EPDC = """\
    virtual Error commitLeaf3Epdc(Display, const std::vector<Leaf3EpdcUpdate>&) {
        return Error::UNSUPPORTED;
    }
"""
WRAPPED_COMPOSER_COMMIT_EPDC_OVERRIDE = """\
    Error commitLeaf3Epdc(
            Display display, const std::vector<Leaf3EpdcUpdate>& updates) override;
"""
COMPOSER_COMMIT_EPDC_OVERRIDE = (
    "    Error commitLeaf3Epdc(Display display, "
    "const std::vector<Leaf3EpdcUpdate>& updates) override;\n"
)
FORMATTED_QTI_COMPOSER_DESCRIPTOR = """\
            if (descriptor ==
                "vendor.qti.hardware.display.composer@3.0::IQtiComposerClient") {
"""
QTI_COMPOSER_DESCRIPTOR = (
    '            if (descriptor == '
    '"vendor.qti.hardware.display.composer@3.0::IQtiComposerClient") {\n'
)

COMPOSER_ERROR_CONDITION = """\
            if (command == IComposerClient::Command::VALIDATE_DISPLAY ||
                command == IComposerClient::Command::PRESENT_DISPLAY ||
                command == IComposerClient::Command::PRESENT_OR_VALIDATE_DISPLAY) {
"""
PREVIOUS_COMPOSER_EPDC_ERROR_CONDITION = """\
            if (command == IComposerClient::Command::VALIDATE_DISPLAY ||
                command == IComposerClient::Command::PRESENT_DISPLAY ||
                command == IComposerClient::Command::PRESENT_OR_VALIDATE_DISPLAY ||
                static_cast<uint32_t>(command) == 0x08020000) {
"""
COMPOSER_EPDC_ERROR_CONDITION = """\
            if (command == IComposerClient::Command::VALIDATE_DISPLAY ||
                command == IComposerClient::Command::PRESENT_DISPLAY ||
                command == IComposerClient::Command::PRESENT_OR_VALIDATE_DISPLAY ||
                isLeaf3CommitEpdcCommand(static_cast<uint32_t>(command))) {
"""
PREVIOUS_WRITER_COMMAND = (
    "    beginCommand(static_cast<V2_1::IComposerClient::Command>(0x08020000), length);\n"
)
WRITER_COMMAND = (
    "    beginCommand(static_cast<V2_1::IComposerClient::Command>(kLeaf3CommitEpdcCommand), "
    "length);\n"
)
OLD_REFRESH_CALLBACK = (
    "    Leaf3EpdcController::get().setRefreshCallback("
    "[this] { repaintEverything(); });\n"
)
BARE_REFRESH_CALLBACK = (
    "    Leaf3EpdcController::get().setRefreshCallback("
    "[this] { signalRefresh(); });\n"
)
REFRESH_CALLBACK = (
    "    // Preserve empty damage so deadline-only EPDC passes are not mistaken for new frames.\n"
    "    Leaf3EpdcController::get().setRefreshCallback("
    "[this] { signalRefresh(); });\n"
)
FORMATTED_OLD_REFRESH_CALLBACK = """\
    Leaf3EpdcController::get().setRefreshCallback(
            [this] { repaintEverything(); });
"""
PREVIOUS_HWCOMPOSER_SUBMISSION = """\
    const bool leaf3EpdcQueued = !displayData.leaf3EpdcUpdates.empty();
    if (leaf3EpdcQueued) {
        const auto epdcError = hwcDisplay->commitLeaf3Epdc(displayData.leaf3EpdcUpdates);
        displayData.leaf3EpdcUpdates.clear();
        if (epdcError != hal::Error::NONE) {
            Leaf3EpdcController::get().composerFailed("commitEpdc");
            LOG_HWC_ERROR("commitEpdc", epdcError, displayId);
            return UNKNOWN_ERROR;
        }
    }

"""
PREVIOUS_HWCOMPOSER_PRESENT = """\
    auto error = hwcDisplay->present(&displayData.lastPresentFence);
    if (error != hal::Error::NONE) {
        if (leaf3EpdcQueued) {
            Leaf3EpdcController::get().composerFailed("present");
        }
        LOG_HWC_ERROR("present", error, displayId);
        return UNKNOWN_ERROR;
    }
"""
HWCOMPOSER_PREPARED = """\
    const bool leaf3EpdcPrepared = !displayData.leaf3EpdcUpdates.empty();

"""
HWCOMPOSER_PRESENT = """\
    bool leaf3EpdcQueued = false;
    if (leaf3EpdcPrepared && Leaf3EpdcController::get().beginSubmission()) {
        leaf3EpdcQueued = true;
        const auto epdcError = hwcDisplay->commitLeaf3Epdc(displayData.leaf3EpdcUpdates);
        displayData.leaf3EpdcUpdates.clear();
        if (epdcError != hal::Error::NONE) {
            Leaf3EpdcController::get().endSubmission();
            Leaf3EpdcController::get().composerFailed("commitEpdc");
            LOG_HWC_ERROR("commitEpdc", epdcError, displayId);
            return UNKNOWN_ERROR;
        }
    } else {
        displayData.leaf3EpdcUpdates.clear();
    }

    auto error = hwcDisplay->present(&displayData.lastPresentFence);
    if (leaf3EpdcQueued) {
        Leaf3EpdcController::get().endSubmission();
    }
    if (error != hal::Error::NONE) {
        if (leaf3EpdcQueued) {
            Leaf3EpdcController::get().composerFailed("present");
        }
        LOG_HWC_ERROR("present", error, displayId);
        return UNKNOWN_ERROR;
    }
"""


def replace_once(text: str, old: str, new: str, description: str) -> str:
    if text.count(old) != 1:
        raise ValueError(f"expected exactly one {description}")
    return text.replace(old, new, 1)


def add_after(text: str, marker: str, addition: str, description: str) -> str:
    return replace_once(text, marker, marker + addition, description)


def upgrade_hwcomposer_h(text: str) -> str:
    if PREVIOUS_ABSTRACT_EPDC_DAMAGE in text:
        text = replace_once(
            text,
            PREVIOUS_ABSTRACT_EPDC_DAMAGE,
            DEFAULT_EPDC_DAMAGE,
            "previous abstract EPDC damage declaration",
        )
    if WRAPPED_EPDC_DAMAGE_OVERRIDE in text:
        text = replace_once(
            text,
            WRAPPED_EPDC_DAMAGE_OVERRIDE,
            EPDC_DAMAGE_OVERRIDE,
            "wrapped EPDC damage override",
        )
    return text


def upgrade_hwcomposer_cpp(text: str) -> str:
    replacements = (
        (
            FORMATTED_INJECTED_COMPOSER_CONSTRUCTOR,
            INJECTED_COMPOSER_CONSTRUCTOR,
            "formatted injected-composer constructor",
        ),
        (
            FORMATTED_SERVICE_COMPOSER_CONSTRUCTOR,
            SERVICE_COMPOSER_CONSTRUCTOR,
            "formatted service-composer constructor",
        ),
        (
            PREVIOUS_FORMATTED_EPDC_DAMAGE_IMPLEMENTATION,
            EPDC_DAMAGE_IMPLEMENTATION,
            "previous formatted EPDC damage implementation",
        ),
    )
    for previous, current, description in replacements:
        if previous in text:
            text = replace_once(text, previous, current, description)
    return text


def upgrade_hwc2_h(text: str) -> str:
    replacements = (
        (
            PREVIOUS_ABSTRACT_COMMIT_EPDC,
            DEFAULT_COMMIT_EPDC,
            "previous abstract CommitEpdc declaration",
        ),
        (
            WRAPPED_COMMIT_EPDC_OVERRIDE,
            COMMIT_EPDC_OVERRIDE,
            "wrapped CommitEpdc override",
        ),
    )
    for previous, current, description in replacements:
        if previous in text:
            text = replace_once(text, previous, current, description)
    return text


def upgrade_composer_h(text: str) -> str:
    replacements = (
        (
            PREVIOUS_ABSTRACT_SUPPORTS_EPDC,
            DEFAULT_SUPPORTS_EPDC,
            "previous abstract EPDC capability declaration",
        ),
        (
            PREVIOUS_ABSTRACT_COMPOSER_COMMIT_EPDC,
            DEFAULT_COMPOSER_COMMIT_EPDC,
            "previous abstract composer CommitEpdc declaration",
        ),
        (
            WRAPPED_COMPOSER_COMMIT_EPDC_OVERRIDE,
            COMPOSER_COMMIT_EPDC_OVERRIDE,
            "wrapped composer CommitEpdc override",
        ),
    )
    for previous, current, description in replacements:
        if previous in text:
            text = replace_once(text, previous, current, description)
    return text


def upgrade_composer_cpp(text: str) -> str:
    if FORMATTED_QTI_COMPOSER_DESCRIPTOR in text:
        text = replace_once(
            text,
            FORMATTED_QTI_COMPOSER_DESCRIPTOR,
            QTI_COMPOSER_DESCRIPTOR,
            "formatted QTI composer descriptor check",
        )
    return text


def upgrade_surfaceflinger_cpp(text: str) -> str:
    if FORMATTED_OLD_REFRESH_CALLBACK in text:
        text = replace_once(
            text,
            FORMATTED_OLD_REFRESH_CALLBACK,
            REFRESH_CALLBACK,
            "formatted forced-refresh callback",
        )
    return text


def patch_android_bp(text: str) -> str:
    return add_after(
        text,
        '        "LayerVector.cpp",\n',
        '        "Leaf3EpdcController.cpp",\n',
        "libsurfaceflinger LayerVector source",
    )


def patch_output_h(text: str) -> str:
    text = add_after(
        text,
        "    compositionengine::Output::FrameFences presentAndGetFrameFences() override;\n",
        "    const Region& getLeaf3PresentDamage() const { return mLeaf3PresentDamage; }\n",
        "Output present-and-fences declaration",
    )
    return add_after(
        text,
        "    ReleasedLayers mReleasedLayers;\n",
        "    Region mLeaf3PresentDamage;\n",
        "Output released-layers member",
    )


def patch_output_cpp(text: str) -> str:
    marker = """\
void Output::postFramebuffer() {
    ATRACE_CALL();
    ALOGV(__FUNCTION__);

    if (!getState().isEnabled) {
        return;
    }

    auto& outputState = editState();
"""
    return add_after(
        text,
        marker,
        "    mLeaf3PresentDamage = outputState.dirtyRegion;\n",
        "Output postFramebuffer state",
    )


def patch_display_cpp(text: str) -> str:
    marker = """\
compositionengine::Output::FrameFences Display::presentAndGetFrameFences() {
    auto result = impl::Output::presentAndGetFrameFences();

    if (!mId) {
        return result;
    }

    auto& hwc = getCompositionEngine().getHwComposer();
"""
    return add_after(
        text,
        marker,
        "    hwc.setLeaf3EpdcDamage(*mId, getLeaf3PresentDamage(), getState().bounds);\n",
        "composition Display HWC lookup",
    )


def patch_hwcomposer_h(text: str) -> str:
    text = add_after(
        text,
        "#include <utils/Timers.h>\n\n",
        '#include "../Leaf3EpdcController.h"\n',
        "HWComposer utility includes",
    )
    text = add_after(
        text,
        "    virtual status_t presentAndGetReleaseFences(DisplayId displayId) = 0;\n",
        "    virtual void setLeaf3EpdcDamage(DisplayId, const Region&, const Rect&) {}\n",
        "abstract HWComposer present declaration",
    )
    text = add_after(
        text,
        "    status_t presentAndGetReleaseFences(DisplayId displayId) override;\n",
        "    void setLeaf3EpdcDamage(DisplayId displayId, const Region& damage, "
        "const Rect& bounds) override;\n",
        "implementation HWComposer present declaration",
    )
    return add_after(
        text,
        "        hal::Error presentError;\n",
        "        std::vector<Leaf3EpdcUpdate> leaf3EpdcUpdates;\n",
        "HWComposer present-error member",
    )


def patch_hwcomposer_cpp(text: str) -> str:
    text = add_after(
        text,
        '#include "../Layer.h" // needed only for debugging\n',
        '#include "../Leaf3EpdcController.h"\n',
        "HWComposer include",
    )
    text = replace_once(
        text,
        "HWComposer::HWComposer(std::unique_ptr<Hwc2::Composer> composer) : "
        "mComposer(std::move(composer)) {\n}\n",
        "HWComposer::HWComposer(std::unique_ptr<Hwc2::Composer> composer) : "
        "mComposer(std::move(composer)) {\n"
        "    Leaf3EpdcController::get().setComposerSupported(mComposer->supportsLeaf3Epdc());\n"
        "}\n",
        "HWComposer injected-composer constructor",
    )
    text = replace_once(
        text,
        "HWComposer::HWComposer(const std::string& composerServiceName)\n"
        "      : mComposer(std::make_unique<Hwc2::impl::Composer>(composerServiceName)) {\n}\n",
        "HWComposer::HWComposer(const std::string& composerServiceName)\n"
        "      : mComposer(std::make_unique<Hwc2::impl::Composer>(composerServiceName)) {\n"
        "    Leaf3EpdcController::get().setComposerSupported(mComposer->supportsLeaf3Epdc());\n"
        "}\n",
        "HWComposer service constructor",
    )
    text = replace_once(
        text,
        "    if (!frameUsesClientComposition) {\n",
        "    if (!frameUsesClientComposition && !Leaf3EpdcController::get().isActive()) {\n",
        "skip-validate condition",
    )
    present_marker = """\
status_t HWComposer::presentAndGetReleaseFences(DisplayId displayId) {
    ATRACE_CALL();

    RETURN_IF_INVALID_DISPLAY(displayId, BAD_INDEX);

    auto& displayData = mDisplayData[displayId];
    auto& hwcDisplay = displayData.hwcDisplay;
"""
    present_hook = present_marker + "\n" + HWCOMPOSER_PREPARED
    text = replace_once(
        text,
        present_marker,
        present_hook,
        "HWComposer present function",
    )
    text = replace_once(
        text,
        "    if (displayData.validateWasSkipped) {\n",
        "    if (displayData.validateWasSkipped) {\n"
        "        displayData.leaf3EpdcUpdates.clear();\n"
        "        // The frame was already presented by presentOrValidate.\n",
        "HWComposer skipped-validation branch",
    )
    text = replace_once(
        text,
        "    auto error = hwcDisplay->present(&displayData.lastPresentFence);\n"
        '    RETURN_IF_HWC_ERROR_FOR("present", error, displayId, UNKNOWN_ERROR);\n',
        HWCOMPOSER_PRESENT,
        "HWComposer final present error handling",
    )
    insertion = "\n" + EPDC_DAMAGE_IMPLEMENTATION
    return text.replace(
        "\nstatus_t HWComposer::presentAndGetReleaseFences(DisplayId displayId) {",
        insertion + "\nstatus_t HWComposer::presentAndGetReleaseFences(DisplayId displayId) {",
        1,
    )


def patch_hwc2_h(text: str) -> str:
    text = add_after(
        text,
        "#include <vector>\n\n",
        '#include "../Leaf3EpdcController.h"\n',
        "HWC2 standard includes",
    )
    text = add_after(
        text,
        "    [[clang::warn_unused_result]] virtual hal::Error present(\n"
        "            android::sp<android::Fence>* outPresentFence) = 0;\n",
        "    [[clang::warn_unused_result]] virtual hal::Error commitLeaf3Epdc(\n"
        "            const std::vector<Leaf3EpdcUpdate>&) {\n"
        "        return hal::Error::UNSUPPORTED;\n"
        "    }\n",
        "abstract HWC2 present declaration",
    )
    return add_after(
        text,
        "    hal::Error present(android::sp<android::Fence>* outPresentFence) override;\n",
        "    hal::Error commitLeaf3Epdc(const std::vector<Leaf3EpdcUpdate>& updates) override;\n",
        "implementation HWC2 present declaration",
    )


def patch_hwc2_cpp(text: str) -> str:
    marker = """\
Error Display::present(sp<Fence>* outPresentFence)
{
"""
    implementation = """\
Error Display::commitLeaf3Epdc(const std::vector<Leaf3EpdcUpdate>& updates) {
    return static_cast<Error>(mComposer.commitLeaf3Epdc(mId, updates));
}

"""
    return replace_once(
        text,
        marker,
        implementation + marker,
        "HWC2 Display present implementation",
    )


def patch_composer_h(text: str) -> str:
    text = add_after(
        text,
        "#include <utils/StrongPointer.h>\n",
        '\n#include "../Leaf3EpdcController.h"\n',
        "Composer utility includes",
    )
    text = add_after(
        text,
        "    virtual bool isRemote() = 0;\n",
        "    virtual bool supportsLeaf3Epdc() const { return false; }\n",
        "abstract Composer remote declaration",
    )
    text = add_after(
        text,
        "    virtual Error presentDisplay(Display display, int* outPresentFence) = 0;\n",
        "    virtual Error commitLeaf3Epdc(Display, const std::vector<Leaf3EpdcUpdate>&) {\n"
        "        return Error::UNSUPPORTED;\n"
        "    }\n",
        "abstract Composer present declaration",
    )
    text = add_after(
        text,
        "    bool isRemote() override;\n",
        "    bool supportsLeaf3Epdc() const override { return mSupportsLeaf3Epdc; }\n",
        "Composer remote declaration",
    )
    text = add_after(
        text,
        "    Error presentDisplay(Display display, int* outPresentFence) override;\n",
        "    Error commitLeaf3Epdc(Display display, "
        "const std::vector<Leaf3EpdcUpdate>& updates) override;\n",
        "Composer present declaration",
    )
    text = text.replace(
        "        ~CommandWriter() override;\n",
        "        ~CommandWriter() override;\n\n"
        "        void commitLeaf3Epdc(const std::vector<Leaf3EpdcUpdate>& updates);\n",
        1,
    )
    text = text.replace(
        "        ~CommandWriter() override {}\n",
        "        ~CommandWriter() override {}\n"
        "        void commitLeaf3Epdc(const std::vector<Leaf3EpdcUpdate>& updates);\n",
        1,
    )
    return add_after(
        text,
        "    const bool mIsUsingVrComposer;\n",
        "    bool mSupportsLeaf3Epdc = false;\n",
        "Composer VR flag",
    )


def patch_composer_cpp(text: str) -> str:
    writer = """\
void Composer::CommandWriter::commitLeaf3Epdc(const std::vector<Leaf3EpdcUpdate>& updates) {
    if (updates.empty() || updates.size() > 8) {
        return;
    }
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
}

"""
    text = add_after(
        text,
        "namespace impl {\n\n",
        writer,
        "Composer implementation namespace",
    )
    text = add_after(
        text,
        "    if (mClient == nullptr) {\n"
        '        LOG_ALWAYS_FATAL("failed to create composer client");\n'
        "    }\n",
        "\n"
        "    const auto chainResult = mClient->interfaceChain([&](const auto& descriptors) {\n"
        "        for (const auto& descriptor : descriptors) {\n"
        "            if (descriptor == "
        '"vendor.qti.hardware.display.composer@3.0::IQtiComposerClient") {\n'
        "                mSupportsLeaf3Epdc = true;\n"
        "                break;\n"
        "            }\n"
        "        }\n"
        "    });\n"
        "    if (!chainResult.isOk()) {\n"
        "        mSupportsLeaf3Epdc = false;\n"
        "    }\n",
        "Composer client validation",
    )
    text = replace_once(
        text,
        COMPOSER_ERROR_CONDITION,
        COMPOSER_EPDC_ERROR_CONDITION,
        "Composer command-error propagation",
    )
    present = """\
Error Composer::presentDisplay(Display display, int* outPresentFence)
{
"""
    commit = """\
Error Composer::commitLeaf3Epdc(Display display, const std::vector<Leaf3EpdcUpdate>& updates) {
    if (!mSupportsLeaf3Epdc || updates.empty() || updates.size() > 8) {
        return Error::UNSUPPORTED;
    }
    mWriter.selectDisplay(display);
    mWriter.commitLeaf3Epdc(updates);
    return Error::NONE;
}

"""
    return replace_once(
        text,
        present,
        commit + present,
        "Composer present implementation",
    )


def patch_surfaceflinger_cpp(text: str) -> str:
    text = add_after(
        text,
        '#include "LayerVector.h"\n',
        '#include "Leaf3EpdcController.h"\n',
        "SurfaceFlinger include",
    )
    text = replace_once(
        text,
        "SurfaceFlinger::~SurfaceFlinger() = default;\n",
        "SurfaceFlinger::~SurfaceFlinger() {\n"
        "    Leaf3EpdcController::get().clearRefreshCallback();\n"
        "}\n",
        "SurfaceFlinger destructor",
    )
    text = add_after(
        text,
        "    mCompositionEngine->getHwComposer().setConfiguration(this, "
        "getBE().mComposerSequenceId);\n",
        REFRESH_CALLBACK,
        "SurfaceFlinger HWC configuration",
    )
    return add_after(
        text,
        '    result.append("\\nDisplay identification data:\\n");\n',
        "    result.append(Leaf3EpdcController::get().dump());\n",
        "SurfaceFlinger dump heading",
    )


PATCHES = {
    "services/surfaceflinger/Android.bp": patch_android_bp,
    "services/surfaceflinger/CompositionEngine/include/compositionengine/impl/Output.h": patch_output_h,
    "services/surfaceflinger/CompositionEngine/src/Output.cpp": patch_output_cpp,
    "services/surfaceflinger/CompositionEngine/src/Display.cpp": patch_display_cpp,
    "services/surfaceflinger/DisplayHardware/HWComposer.h": patch_hwcomposer_h,
    "services/surfaceflinger/DisplayHardware/HWComposer.cpp": patch_hwcomposer_cpp,
    "services/surfaceflinger/DisplayHardware/HWC2.h": patch_hwc2_h,
    "services/surfaceflinger/DisplayHardware/HWC2.cpp": patch_hwc2_cpp,
    "services/surfaceflinger/DisplayHardware/ComposerHal.h": patch_composer_h,
    "services/surfaceflinger/DisplayHardware/ComposerHal.cpp": patch_composer_cpp,
    "services/surfaceflinger/SurfaceFlinger.cpp": patch_surfaceflinger_cpp,
}

REQUIRED_MARKERS = {
    "services/surfaceflinger/Android.bp": (
        '        "Leaf3EpdcController.cpp",\n',
    ),
    "services/surfaceflinger/CompositionEngine/include/compositionengine/impl/Output.h": (
        "    const Region& getLeaf3PresentDamage() const { return mLeaf3PresentDamage; }\n",
        "    Region mLeaf3PresentDamage;\n",
    ),
    "services/surfaceflinger/CompositionEngine/src/Output.cpp": (
        "    mLeaf3PresentDamage = outputState.dirtyRegion;\n"
        "    outputState.dirtyRegion.clear();\n",
    ),
    "services/surfaceflinger/CompositionEngine/src/Display.cpp": (
        "    hwc.setLeaf3EpdcDamage(*mId, getLeaf3PresentDamage(), getState().bounds);\n"
        "    hwc.presentAndGetReleaseFences(*mId);\n",
    ),
    "services/surfaceflinger/DisplayHardware/HWComposer.h": (
        '#include "../Leaf3EpdcController.h"\n',
        "    virtual void setLeaf3EpdcDamage(DisplayId, const Region&, const Rect&) {}\n",
        "    void setLeaf3EpdcDamage(DisplayId displayId, const Region& damage, "
        "const Rect& bounds) override;\n",
        "        std::vector<Leaf3EpdcUpdate> leaf3EpdcUpdates;\n",
    ),
    "services/surfaceflinger/DisplayHardware/HWComposer.cpp": (
        '#include "../Leaf3EpdcController.h"\n',
        "HWComposer::HWComposer(std::unique_ptr<Hwc2::Composer> composer) : "
        "mComposer(std::move(composer)) {\n"
        "    Leaf3EpdcController::get().setComposerSupported(mComposer->supportsLeaf3Epdc());\n"
        "}\n",
        "HWComposer::HWComposer(const std::string& composerServiceName)\n"
        "      : mComposer(std::make_unique<Hwc2::impl::Composer>(composerServiceName)) {\n"
        "    Leaf3EpdcController::get().setComposerSupported(mComposer->supportsLeaf3Epdc());\n"
        "}\n",
        "    if (!frameUsesClientComposition && !Leaf3EpdcController::get().isActive()) {\n",
        "void HWComposer::setLeaf3EpdcDamage(DisplayId displayId, const Region& damage, "
        "const Rect& bounds) {\n"
        "    RETURN_IF_INVALID_DISPLAY(displayId);\n"
        "    auto& displayData = mDisplayData[displayId];\n"
        "    if (displayData.isVirtual || !mInternalHwcDisplayId ||\n"
        "        displayData.hwcDisplay->getId() != *mInternalHwcDisplayId) {\n",
        "    displayData.leaf3EpdcUpdates = "
        "Leaf3EpdcController::get().preparePresent(damage, bounds);\n"
        "}\n",
        "    const bool leaf3EpdcPrepared = !displayData.leaf3EpdcUpdates.empty();\n",
        "        displayData.leaf3EpdcUpdates.clear();\n"
        "        // The frame was already presented by presentOrValidate.\n",
        "    if (leaf3EpdcPrepared && Leaf3EpdcController::get().beginSubmission()) {\n",
        "            Leaf3EpdcController::get().endSubmission();\n"
        '            Leaf3EpdcController::get().composerFailed("commitEpdc");\n',
        "        Leaf3EpdcController::get().endSubmission();\n"
        "    }\n"
        "    if (error != hal::Error::NONE) {\n",
        '            Leaf3EpdcController::get().composerFailed("commitEpdc");\n',
        '            Leaf3EpdcController::get().composerFailed("present");\n',
    ),
    "services/surfaceflinger/DisplayHardware/HWC2.h": (
        '#include "../Leaf3EpdcController.h"\n',
        "    [[clang::warn_unused_result]] virtual hal::Error commitLeaf3Epdc(\n"
        "            const std::vector<Leaf3EpdcUpdate>&) {\n"
        "        return hal::Error::UNSUPPORTED;\n"
        "    }\n",
        "    hal::Error commitLeaf3Epdc(const std::vector<Leaf3EpdcUpdate>& updates) override;\n",
    ),
    "services/surfaceflinger/DisplayHardware/HWC2.cpp": (
        "Error Display::commitLeaf3Epdc(const std::vector<Leaf3EpdcUpdate>& updates) {\n"
        "    return static_cast<Error>(mComposer.commitLeaf3Epdc(mId, updates));\n"
        "}\n",
    ),
    "services/surfaceflinger/DisplayHardware/ComposerHal.h": (
        '#include "../Leaf3EpdcController.h"\n',
        "    virtual bool supportsLeaf3Epdc() const { return false; }\n",
        "    virtual Error commitLeaf3Epdc(Display, const std::vector<Leaf3EpdcUpdate>&) {\n"
        "        return Error::UNSUPPORTED;\n"
        "    }\n",
        "    bool supportsLeaf3Epdc() const override { return mSupportsLeaf3Epdc; }\n",
        "    Error commitLeaf3Epdc(Display display, "
        "const std::vector<Leaf3EpdcUpdate>& updates) override;\n",
        "        void commitLeaf3Epdc(const std::vector<Leaf3EpdcUpdate>& updates);\n",
        "    bool mSupportsLeaf3Epdc = false;\n",
    ),
    "services/surfaceflinger/DisplayHardware/ComposerHal.cpp": (
        "    if (updates.empty() || updates.size() > 8) {\n"
        "        return;\n"
        "    }\n"
        "    const uint16_t length = static_cast<uint16_t>(1 + updates.size() * 5);\n",
        WRITER_COMMAND
        + "    write(static_cast<uint32_t>(updates.size()));\n"
        "    for (const auto& update : updates) {\n"
        "        write(update.left);\n"
        "        write(update.top);\n"
        "        write(update.right);\n"
        "        write(update.bottom);\n"
        "        write(update.mode);\n"
        "    }\n"
        "    endCommand();\n",
        "            if (descriptor == "
        '"vendor.qti.hardware.display.composer@3.0::IQtiComposerClient") {\n'
        "                mSupportsLeaf3Epdc = true;\n"
        "                break;\n"
        "            }\n",
        "    if (!chainResult.isOk()) {\n"
        "        mSupportsLeaf3Epdc = false;\n"
        "    }\n",
        "                command == IComposerClient::Command::PRESENT_OR_VALIDATE_DISPLAY ||\n"
        "                isLeaf3CommitEpdcCommand(static_cast<uint32_t>(command))) {\n",
        "    if (!mSupportsLeaf3Epdc || updates.empty() || updates.size() > 8) {\n"
        "        return Error::UNSUPPORTED;\n"
        "    }\n"
        "    mWriter.selectDisplay(display);\n"
        "    mWriter.commitLeaf3Epdc(updates);\n"
        "    return Error::NONE;\n",
    ),
    "services/surfaceflinger/SurfaceFlinger.cpp": (
        '#include "Leaf3EpdcController.h"\n',
        "SurfaceFlinger::~SurfaceFlinger() {\n"
        "    Leaf3EpdcController::get().clearRefreshCallback();\n"
        "}\n",
        REFRESH_CALLBACK,
        "    result.append(Leaf3EpdcController::get().dump());\n",
    ),
}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("frameworks_native", type=Path)
    parser.add_argument("--check", action="store_true")
    parser.add_argument(
        "--version",
        action="version",
        version=f"Leaf3 composer EPDC patcher {PATCHER_VERSION}",
    )
    args = parser.parse_args()

    root = args.frameworks_native.resolve()
    template_root = (
        Path(__file__).resolve().parent.parent
        / "frameworks/native/services/surfaceflinger"
    )
    writes = {}
    try:
        for relative, transform in PATCHES.items():
            path = root / relative
            text = path.read_text()
            if relative == "services/surfaceflinger/DisplayHardware/HWComposer.h":
                text = upgrade_hwcomposer_h(text)
            if relative == "services/surfaceflinger/DisplayHardware/HWComposer.cpp":
                text = upgrade_hwcomposer_cpp(text)
            if relative == "services/surfaceflinger/DisplayHardware/HWC2.h":
                text = upgrade_hwc2_h(text)
            if relative == "services/surfaceflinger/DisplayHardware/ComposerHal.h":
                text = upgrade_composer_h(text)
            if relative == "services/surfaceflinger/DisplayHardware/ComposerHal.cpp":
                text = upgrade_composer_cpp(text)
            if relative == "services/surfaceflinger/SurfaceFlinger.cpp":
                text = upgrade_surfaceflinger_cpp(text)
            if (
                relative
                == "services/surfaceflinger/DisplayHardware/HWComposer.cpp"
                and PREVIOUS_HWCOMPOSER_SUBMISSION in text
            ):
                text = replace_once(
                    text,
                    PREVIOUS_HWCOMPOSER_SUBMISSION,
                    HWCOMPOSER_PREPARED,
                    "previous untracked composer submission",
                )
                text = replace_once(
                    text,
                    "    if (displayData.validateWasSkipped) {\n",
                    "    if (displayData.validateWasSkipped) {\n"
                    "        displayData.leaf3EpdcUpdates.clear();\n"
                    "        // The frame was already presented by presentOrValidate.\n",
                    "previous skipped-validation branch",
                )
                text = replace_once(
                    text,
                    PREVIOUS_HWCOMPOSER_PRESENT,
                    HWCOMPOSER_PRESENT,
                    "previous untracked composer present",
                )
            if (
                relative
                == "services/surfaceflinger/DisplayHardware/ComposerHal.cpp"
                and "void Composer::CommandWriter::commitLeaf3Epdc" in text
            ):
                for previous_condition in (
                    COMPOSER_ERROR_CONDITION,
                    PREVIOUS_COMPOSER_EPDC_ERROR_CONDITION,
                ):
                    if previous_condition in text:
                        text = replace_once(
                            text,
                            previous_condition,
                            COMPOSER_EPDC_ERROR_CONDITION,
                            "previous composer command-error handling",
                        )
                        break
                if PREVIOUS_WRITER_COMMAND in text:
                    text = replace_once(
                        text,
                        PREVIOUS_WRITER_COMMAND,
                        WRITER_COMMAND,
                        "previous composer EPDC writer command",
                    )
            if (
                relative == "services/surfaceflinger/SurfaceFlinger.cpp"
                and REFRESH_CALLBACK not in text
            ):
                for previous_callback in (
                    OLD_REFRESH_CALLBACK,
                    BARE_REFRESH_CALLBACK,
                ):
                    if previous_callback in text:
                        text = replace_once(
                            text,
                            previous_callback,
                            REFRESH_CALLBACK,
                            "previous forced-refresh callback",
                        )
                        break
            markers = REQUIRED_MARKERS[relative]
            present = tuple(marker in text for marker in markers)
            if all(present):
                writes[path] = text
            elif any(present):
                raise ValueError(f"{path}: partial Leaf3 composer EPDC patch")
            else:
                writes[path] = transform(text)

        for name in (
            "Leaf3EpdcPolicy.h",
            "Leaf3EpdcController.h",
            "Leaf3EpdcController.cpp",
        ):
            source = template_root / name
            destination = root / "services/surfaceflinger" / name
            expected = source.read_text()
            if args.check and (
                not destination.exists() or destination.read_text() != expected
            ):
                parser.error(f"{destination}: Leaf3 EPDC source is missing or stale")
            writes[destination] = expected
    except (OSError, ValueError) as error:
        parser.error(str(error))

    if args.check:
        incomplete = [
            str(path)
            for path, expected in writes.items()
            if not path.exists()
            or path.read_text() != expected
            or (
                path.name
                not in (
                    "Leaf3EpdcPolicy.h",
                    "Leaf3EpdcController.h",
                    "Leaf3EpdcController.cpp",
                )
                and not all(
                    marker in expected
                    for marker in REQUIRED_MARKERS[
                        str(path.relative_to(root))
                    ]
                )
            )
        ]
        if incomplete:
            parser.error("Leaf3 composer EPDC patch is incomplete: " + ", ".join(incomplete))
        print(f"{root}: Leaf3 composer EPDC patch is present")
        return 0

    for path, text in writes.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.exists() or path.read_text() != text:
            path.write_text(text)
    print(f"{root}: installed Leaf3 composer-native EPDC transport")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
