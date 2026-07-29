#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0

"""Add Leaf3 frame notification and damage delivery to SurfaceFlinger."""

import argparse
import re
from pathlib import Path


PATCHER_VERSION = "8"

UNIQUE_FD_INCLUDE = "#include <android-base/unique_fd.h>\n"
LEAF3_EPDC_INCLUDE = '#include "Leaf3EpdcController.h"\n'
PROPERTIES_INCLUDE = "#include <android-base/properties.h>\n"
FCNTL_INCLUDE = "#include <fcntl.h>\n"
ERRNO_INCLUDE = "#include <errno.h>\n"
UNISTD_INCLUDE = "#include <unistd.h>\n"
SYS_TYPES_INCLUDE = "#include <sys/types.h>\n"

NOTIFIER_STATE = """\
constexpr uint32_t kLeaf3FrameNotifierTransaction = 1037;
constexpr uint32_t kLeaf3TransientHintTransaction = 1038;
constexpr int32_t kLeaf3FrameNotifierVersion = 3;
constexpr int32_t kLeaf3TransientHintVersion = 1;
constexpr int32_t kLeaf3FrameNotifierUnregister = 0;
constexpr int32_t kLeaf3FrameNotifierRegister = 1;
constexpr int32_t kLeaf3FrameNotifierTakeDamage = 2;
constexpr int32_t kLeaf3FrameNotifierRequestFullRefresh = 3;
constexpr int32_t kLeaf3FrameNotifierBlockAndWait = 4;
constexpr char kLeaf3ActiveUidProperty[] = "sys.leaf3.active_uid";

std::mutex gLeaf3FrameNotifierMutex;
base::unique_fd gLeaf3FrameNotifierFd;
Rect gLeaf3FrameDamage;
bool gLeaf3FrameDamageValid = false;
"""

NAMESPACE_MARKER = """\
namespace {

#pragma clang diagnostic push
"""

CREDENTIAL_HOOK = """\
status_t SurfaceFlinger::CheckTransactCodeCredentials(uint32_t code) {
    if (code == kLeaf3FrameNotifierTransaction) {
        IPCThreadState* ipc = IPCThreadState::self();
        if (ipc->getCallingUid() == AID_SYSTEM) {
            return OK;
        }
        ALOGE("Permission Denial: Leaf3 frame notifier pid=%d, uid=%d",
              ipc->getCallingPid(), ipc->getCallingUid());
        return PERMISSION_DENIED;
    }
    if (code == kLeaf3TransientHintTransaction) {
        IPCThreadState* ipc = IPCThreadState::self();
        const uid_t callingUid = ipc->getCallingUid();
        const int activeUid =
                android::base::GetIntProperty(kLeaf3ActiveUidProperty, -1);
        if (activeUid >= 0 && callingUid == static_cast<uid_t>(activeUid)) {
            return OK;
        }
        ALOGE("Permission Denial: Leaf3 transient hint pid=%d, uid=%d, "
              "active uid=%d", ipc->getCallingPid(), callingUid, activeUid);
        return PERMISSION_DENIED;
    }

#pragma clang diagnostic push
"""

CREDENTIAL_MARKER = """\
status_t SurfaceFlinger::CheckTransactCodeCredentials(uint32_t code) {
#pragma clang diagnostic push
"""

TRANSACTION_HOOK = """\
status_t SurfaceFlinger::onTransact(uint32_t code, const Parcel& data, Parcel* reply,
                                    uint32_t flags) {
    if (code == kLeaf3TransientHintTransaction) {
        const uid_t callingUid = IPCThreadState::self()->getCallingUid();
        const status_t credentialCheck = CheckTransactCodeCredentials(code);
        if (credentialCheck != OK) {
            return credentialCheck;
        }
        CHECK_INTERFACE(ISurfaceComposer, data, reply);
        if (data.readInt32() != kLeaf3TransientHintVersion) {
            return BAD_VALUE;
        }
        const int32_t command = data.readInt32();
        if (command == 0) {
            Leaf3EpdcController::get().clearTransientHint();
            return NO_ERROR;
        }
        if (command != 1) {
            return BAD_VALUE;
        }
        const Rect region(data.readInt32(), data.readInt32(),
                          data.readInt32(), data.readInt32());
        const int32_t durationMs = data.readInt32();
        if (!region.isValid() || region.isEmpty() || region.left < 0 ||
            region.top < 0 || region.right > 16384 || region.bottom > 16384 ||
            durationMs < 100 || durationMs > 2000) {
            return BAD_VALUE;
        }
        Leaf3EpdcController::get().setTransientHint(
                region, static_cast<nsecs_t>(durationMs) * 1000000,
                static_cast<int32_t>(callingUid));
        return NO_ERROR;
    }

    if (code == kLeaf3FrameNotifierTransaction) {
        status_t credentialCheck = CheckTransactCodeCredentials(code);
        if (credentialCheck != OK) {
            return credentialCheck;
        }
        CHECK_INTERFACE(ISurfaceComposer, data, reply);
        if (data.readInt32() != kLeaf3FrameNotifierVersion) {
            return BAD_VALUE;
        }

        const int32_t command = data.readInt32();
        if (command == kLeaf3FrameNotifierBlockAndWait) {
            Leaf3EpdcController::get().blockAndWaitForIdle();
            if (reply != nullptr) {
                reply->writeInt32(NO_ERROR);
            }
            return NO_ERROR;
        }
        if (command == kLeaf3FrameNotifierRequestFullRefresh) {
            Leaf3EpdcController::get().requestFullRefresh();
            if (reply != nullptr) {
                reply->writeInt32(NO_ERROR);
            }
            return NO_ERROR;
        }
        if (command == kLeaf3FrameNotifierTakeDamage) {
            Rect damage;
            Rect transientHint;
            bool damageValid =
                    Leaf3EpdcController::get().takeNotifierDamage(
                            &damage, &transientHint);
            std::lock_guard<std::mutex> lock(gLeaf3FrameNotifierMutex);
            if (!damageValid && gLeaf3FrameDamageValid) {
                damage = gLeaf3FrameDamage;
                damageValid = true;
            }
            if (reply != nullptr) {
                reply->writeInt32(damageValid ? 1 : 0);
                if (damageValid) {
                    reply->writeInt32(damage.left);
                    reply->writeInt32(damage.top);
                    reply->writeInt32(damage.right);
                    reply->writeInt32(damage.bottom);
                }
                reply->writeInt32(transientHint.isEmpty() ? 0 : 1);
                if (!transientHint.isEmpty()) {
                    reply->writeInt32(transientHint.left);
                    reply->writeInt32(transientHint.top);
                    reply->writeInt32(transientHint.right);
                    reply->writeInt32(transientHint.bottom);
                }
            }
            gLeaf3FrameDamageValid = false;
            gLeaf3FrameDamage = Rect();
            return NO_ERROR;
        }

        if (command == kLeaf3FrameNotifierUnregister) {
            std::lock_guard<std::mutex> lock(gLeaf3FrameNotifierMutex);
            gLeaf3FrameNotifierFd.reset();
            gLeaf3FrameDamageValid = false;
            gLeaf3FrameDamage = Rect();
            ALOGI("Leaf3 frame notifier unregistered");
            if (reply != nullptr) {
                reply->writeInt32(NO_ERROR);
            }
            return NO_ERROR;
        }
        if (command != kLeaf3FrameNotifierRegister) {
            return BAD_VALUE;
        }

        const int sourceFd = data.readFileDescriptor();
        base::unique_fd notifierFd(sourceFd >= 0 ? dup(sourceFd) : -1);
        if (notifierFd.get() < 0) {
            return BAD_VALUE;
        }
        const int fdFlags = fcntl(notifierFd.get(), F_GETFL);
        if (fdFlags < 0 ||
            fcntl(notifierFd.get(), F_SETFL, fdFlags | O_NONBLOCK) < 0) {
            return BAD_VALUE;
        }

        {
            std::lock_guard<std::mutex> lock(gLeaf3FrameNotifierMutex);
            gLeaf3FrameNotifierFd = std::move(notifierFd);
            gLeaf3FrameDamageValid = false;
            gLeaf3FrameDamage = Rect();
        }
        ALOGI("Leaf3 frame notifier registered");
        if (reply != nullptr) {
            reply->writeInt32(NO_ERROR);
        }
        return NO_ERROR;
    }

    status_t credentialCheck = CheckTransactCodeCredentials(code);
"""

PREVIOUS_V2_NOTIFIER_STATE = NOTIFIER_STATE.replace(
    "constexpr uint32_t kLeaf3TransientHintTransaction = 1038;\n", ""
).replace(
    "constexpr int32_t kLeaf3FrameNotifierVersion = 3;\n"
    "constexpr int32_t kLeaf3TransientHintVersion = 1;\n",
    "constexpr int32_t kLeaf3FrameNotifierVersion = 2;\n",
).replace(
    'constexpr char kLeaf3ActiveUidProperty[] = "sys.leaf3.active_uid";\n',
    "",
)
PREVIOUS_V2_CREDENTIAL_HOOK = CREDENTIAL_HOOK[
    :CREDENTIAL_HOOK.index(
        "    if (code == kLeaf3TransientHintTransaction) {"
    )
] + CREDENTIAL_HOOK[
    CREDENTIAL_HOOK.index(
        "\n#pragma clang diagnostic push"
    ):
]
PREVIOUS_V2_TRANSACTION_HOOK = TRANSACTION_HOOK[
    TRANSACTION_HOOK.index(
        "status_t SurfaceFlinger::onTransact"
    ):
].replace(
    """\
    if (code == kLeaf3TransientHintTransaction) {
        const uid_t callingUid = IPCThreadState::self()->getCallingUid();
        const status_t credentialCheck = CheckTransactCodeCredentials(code);
        if (credentialCheck != OK) {
            return credentialCheck;
        }
        CHECK_INTERFACE(ISurfaceComposer, data, reply);
        if (data.readInt32() != kLeaf3TransientHintVersion) {
            return BAD_VALUE;
        }
        const int32_t command = data.readInt32();
        if (command == 0) {
            Leaf3EpdcController::get().clearTransientHint();
            return NO_ERROR;
        }
        if (command != 1) {
            return BAD_VALUE;
        }
        const Rect region(data.readInt32(), data.readInt32(),
                          data.readInt32(), data.readInt32());
        const int32_t durationMs = data.readInt32();
        if (!region.isValid() || region.isEmpty() || region.left < 0 ||
            region.top < 0 || region.right > 16384 || region.bottom > 16384 ||
            durationMs < 100 || durationMs > 2000) {
            return BAD_VALUE;
        }
        Leaf3EpdcController::get().setTransientHint(
                region, static_cast<nsecs_t>(durationMs) * 1000000,
                static_cast<int32_t>(callingUid));
        return NO_ERROR;
    }

""",
    "",
).replace(
    "            Rect transientHint;\n", ""
).replace(
    "                    Leaf3EpdcController::get().takeNotifierDamage(\n"
    "                            &damage, &transientHint);",
    "                    Leaf3EpdcController::get().takeNotifierDamage(&damage);",
).replace(
    """\
                reply->writeInt32(transientHint.isEmpty() ? 0 : 1);
                if (!transientHint.isEmpty()) {
                    reply->writeInt32(transientHint.left);
                    reply->writeInt32(transientHint.top);
                    reply->writeInt32(transientHint.right);
                    reply->writeInt32(transientHint.bottom);
                }
""",
    "",
)
PREVIOUS_UNOWNED_TRANSACTION_HOOK = TRANSACTION_HOOK.replace(
    "        const uid_t callingUid = IPCThreadState::self()->getCallingUid();\n",
    "",
    1,
).replace(
    "        Leaf3EpdcController::get().setTransientHint(\n"
    "                region, static_cast<nsecs_t>(durationMs) * 1000000,\n"
    "                static_cast<int32_t>(callingUid));",
    "        Leaf3EpdcController::get().setTransientHint(\n"
    "                region, static_cast<nsecs_t>(durationMs) * 1000000);",
    1,
)

TRANSACTION_MARKER = """\
status_t SurfaceFlinger::onTransact(uint32_t code, const Parcel& data, Parcel* reply,
                                    uint32_t flags) {
    status_t credentialCheck = CheckTransactCodeCredentials(code);
"""

PREVIOUS_DRAIN_NOTIFIER_STATE = NOTIFIER_STATE.replace(
    "constexpr int32_t kLeaf3FrameNotifierBlockAndWait = 4;\n", ""
)
PREVIOUS_DRAIN_TRANSACTION_HOOK = TRANSACTION_HOOK.replace(
    """\
        if (command == kLeaf3FrameNotifierBlockAndWait) {
            Leaf3EpdcController::get().blockAndWaitForIdle();
            if (reply != nullptr) {
                reply->writeInt32(NO_ERROR);
            }
            return NO_ERROR;
        }
""",
    "",
)
PREVIOUS_NOTIFIER_STATE = PREVIOUS_DRAIN_NOTIFIER_STATE.replace(
    "constexpr int32_t kLeaf3FrameNotifierRequestFullRefresh = 3;\n", ""
)
PREVIOUS_TRANSACTION_HOOK = PREVIOUS_DRAIN_TRANSACTION_HOOK.replace(
    """\
        if (command == kLeaf3FrameNotifierRequestFullRefresh) {
            Leaf3EpdcController::get().requestFullRefresh();
            if (reply != nullptr) {
                reply->writeInt32(NO_ERROR);
            }
            return NO_ERROR;
        }
""",
    "",
).replace(
    """\
            Rect damage;
            bool damageValid =
                    Leaf3EpdcController::get().takeNotifierDamage(&damage);
            std::lock_guard<std::mutex> lock(gLeaf3FrameNotifierMutex);
            if (!damageValid && gLeaf3FrameDamageValid) {
                damage = gLeaf3FrameDamage;
                damageValid = true;
            }
            if (reply != nullptr) {
                reply->writeInt32(damageValid ? 1 : 0);
                if (damageValid) {
                    reply->writeInt32(damage.left);
                    reply->writeInt32(damage.top);
                    reply->writeInt32(damage.right);
                    reply->writeInt32(damage.bottom);
                }
""",
    """\
            std::lock_guard<std::mutex> lock(gLeaf3FrameNotifierMutex);
            if (reply != nullptr) {
                reply->writeInt32(gLeaf3FrameDamageValid ? 1 : 0);
                if (gLeaf3FrameDamageValid) {
                    reply->writeInt32(gLeaf3FrameDamage.left);
                    reply->writeInt32(gLeaf3FrameDamage.top);
                    reply->writeInt32(gLeaf3FrameDamage.right);
                    reply->writeInt32(gLeaf3FrameDamage.bottom);
                }
""",
)

LEGACY_NOTIFIER_STATE = """\
constexpr uint32_t kLeaf3FrameNotifierTransaction = 1037;
constexpr int32_t kLeaf3FrameNotifierVersion = 1;

std::mutex gLeaf3FrameNotifierMutex;
base::unique_fd gLeaf3FrameNotifierFd;
"""

LEGACY_TRANSACTION_HOOK = """\
status_t SurfaceFlinger::onTransact(uint32_t code, const Parcel& data, Parcel* reply,
                                    uint32_t flags) {
    if (code == kLeaf3FrameNotifierTransaction) {
        status_t credentialCheck = CheckTransactCodeCredentials(code);
        if (credentialCheck != OK) {
            return credentialCheck;
        }
        CHECK_INTERFACE(ISurfaceComposer, data, reply);
        if (data.readInt32() != kLeaf3FrameNotifierVersion) {
            return BAD_VALUE;
        }

        const bool enable = data.readInt32() != 0;
        if (!enable) {
            std::lock_guard<std::mutex> lock(gLeaf3FrameNotifierMutex);
            gLeaf3FrameNotifierFd.reset();
            ALOGI("Leaf3 frame notifier unregistered");
            if (reply != nullptr) {
                reply->writeInt32(NO_ERROR);
            }
            return NO_ERROR;
        }

        const int sourceFd = data.readFileDescriptor();
        base::unique_fd notifierFd(sourceFd >= 0 ? dup(sourceFd) : -1);
        if (notifierFd.get() < 0) {
            return BAD_VALUE;
        }
        const int fdFlags = fcntl(notifierFd.get(), F_GETFL);
        if (fdFlags < 0 ||
            fcntl(notifierFd.get(), F_SETFL, fdFlags | O_NONBLOCK) < 0) {
            return BAD_VALUE;
        }

        {
            std::lock_guard<std::mutex> lock(gLeaf3FrameNotifierMutex);
            gLeaf3FrameNotifierFd = std::move(notifierFd);
        }
        ALOGI("Leaf3 frame notifier registered");
        if (reply != nullptr) {
            reply->writeInt32(NO_ERROR);
        }
        return NO_ERROR;
    }

    status_t credentialCheck = CheckTransactCodeCredentials(code);
"""

POST_FRAME_HOOK = """\
void SurfaceFlinger::postFrame() {
    const auto display = ON_MAIN_THREAD(getDefaultDisplayDeviceLocked());
    if (display && getHwComposer().isConnected(*display->getId())) {
        uint32_t flipCount = display->getPageFlipCount();
        if (flipCount % LOG_FRAME_STATS_PERIOD == 0) {
            logFrameStats();
        }

        const uint64_t signal = 1;
        std::lock_guard<std::mutex> lock(gLeaf3FrameNotifierMutex);
        ssize_t written = -1;
        if (gLeaf3FrameNotifierFd.get() >= 0) {
            do {
                written = write(gLeaf3FrameNotifierFd.get(), &signal, sizeof(signal));
            } while (written < 0 && errno == EINTR);
        }
        if (gLeaf3FrameNotifierFd.get() >= 0 &&
            written != static_cast<ssize_t>(sizeof(signal)) && errno != EAGAIN) {
            ALOGE("Leaf3 frame notifier failed: %s", strerror(errno));
            gLeaf3FrameNotifierFd.reset();
        }
    }
}
"""

POST_FRAME_MARKER = """\
void SurfaceFlinger::postFrame() {
    const auto display = ON_MAIN_THREAD(getDefaultDisplayDeviceLocked());
    if (display && getHwComposer().isConnected(*display->getId())) {
        uint32_t flipCount = display->getPageFlipCount();
        if (flipCount % LOG_FRAME_STATS_PERIOD == 0) {
            logFrameStats();
        }
    }
}
"""

DISPLAY_DAMAGE_HOOK_DISPLAY_DEVICE = """\
        const Region dirtyRegion(display->getDirtyRegion(repaintEverything));

        const auto defaultDisplay = ON_MAIN_THREAD(getDefaultDisplayDeviceLocked());
        if (display == defaultDisplay && !dirtyRegion.isEmpty()) {
            const Rect damage = dirtyRegion.getBounds();
            std::lock_guard<std::mutex> lock(gLeaf3FrameNotifierMutex);
            if (gLeaf3FrameNotifierFd.get() < 0) {
                // Damage is needed only while a bridge is registered.
            } else if (!gLeaf3FrameDamageValid) {
                gLeaf3FrameDamage = damage;
                gLeaf3FrameDamageValid = true;
            } else {
                gLeaf3FrameDamage.left =
                        std::min(gLeaf3FrameDamage.left, damage.left);
                gLeaf3FrameDamage.top =
                        std::min(gLeaf3FrameDamage.top, damage.top);
                gLeaf3FrameDamage.right =
                        std::max(gLeaf3FrameDamage.right, damage.right);
                gLeaf3FrameDamage.bottom =
                        std::max(gLeaf3FrameDamage.bottom, damage.bottom);
            }
        }

        // repaint the framebuffer (if needed)
"""

DISPLAY_DAMAGE_MARKER_DISPLAY_DEVICE = """\
        const Region dirtyRegion(display->getDirtyRegion(repaintEverything));

        // repaint the framebuffer (if needed)
"""

DISPLAY_DAMAGE_HOOK_COMPOSITION_ENGINE = """\
        const Region dirtyRegion = display->getDirtyRegion(repaintEverything);

        const auto defaultDisplay = ON_MAIN_THREAD(getDefaultDisplayDeviceLocked());
        if (displayDevice == defaultDisplay && !dirtyRegion.isEmpty()) {
            const Rect damage = dirtyRegion.getBounds();
            std::lock_guard<std::mutex> lock(gLeaf3FrameNotifierMutex);
            if (gLeaf3FrameNotifierFd.get() < 0) {
                // Damage is needed only while a bridge is registered.
            } else if (!gLeaf3FrameDamageValid) {
                gLeaf3FrameDamage = damage;
                gLeaf3FrameDamageValid = true;
            } else {
                gLeaf3FrameDamage.left =
                        std::min(gLeaf3FrameDamage.left, damage.left);
                gLeaf3FrameDamage.top =
                        std::min(gLeaf3FrameDamage.top, damage.top);
                gLeaf3FrameDamage.right =
                        std::max(gLeaf3FrameDamage.right, damage.right);
                gLeaf3FrameDamage.bottom =
                        std::max(gLeaf3FrameDamage.bottom, damage.bottom);
            }
        }

        // repaint the framebuffer (if needed)
"""

DISPLAY_DAMAGE_MARKER_COMPOSITION_ENGINE = """\
        const Region dirtyRegion = display->getDirtyRegion(repaintEverything);

        // repaint the framebuffer (if needed)
"""

DISPLAY_DAMAGE_MARKER_COMPOSITION_ENGINE_COMPACT = """\
        const Region dirtyRegion = display->getDirtyRegion(repaintEverything);
        // repaint the framebuffer (if needed)
"""

DISPLAY_DAMAGE_VARIANTS = (
    (
        DISPLAY_DAMAGE_MARKER_COMPOSITION_ENGINE,
        DISPLAY_DAMAGE_HOOK_COMPOSITION_ENGINE,
    ),
    (
        DISPLAY_DAMAGE_MARKER_COMPOSITION_ENGINE_COMPACT,
        DISPLAY_DAMAGE_HOOK_COMPOSITION_ENGINE,
    ),
    (
        DISPLAY_DAMAGE_MARKER_DISPLAY_DEVICE,
        DISPLAY_DAMAGE_HOOK_DISPLAY_DEVICE,
    ),
)


def replace_once(text: str, old: str, new: str, description: str) -> str:
    if text.count(old) != 1:
        raise ValueError(f"expected exactly one {description}")
    return text.replace(old, new, 1)


def patch_display_damage(text: str) -> str:
    dirty_matches = list(
        re.finditer(
            r"^(?P<indent>[ \t]*)const\s+Region\s+dirtyRegion\s*"
            r"(?:=\s*)?\(?\s*[A-Za-z_]\w*\s*->\s*getDirtyRegion\s*"
            r"\(\s*repaintEverything\s*\)\s*\)?\s*;\s*$",
            text,
            re.MULTILINE,
        )
    )
    composition_candidates = []
    for dirty_match in dirty_matches:
        closing_match = re.search(
            r"^}\s*$", text[dirty_match.end() :], re.MULTILINE
        )
        if closing_match is None:
            continue
        function_tail = text[
            dirty_match.end() : dirty_match.end() + closing_match.start()
        ]
        composition_call = re.search(
            r"\bdoDisplayComposition\s*\(\s*([A-Za-z_]\w*)\s*,\s*"
            r"dirtyRegion\b",
            function_tail,
        )
        if composition_call is not None:
            composition_candidates.append(
                (dirty_match, composition_call.group(1))
            )

    if len(composition_candidates) == 0 and LEAF3_EPDC_INCLUDE in text:
        # Current LineageOS 18.1 owns the dirty region in CompositionEngine.
        # The composer EPDC integration records that same region before the
        # final present, and the transaction above consumes it atomically.
        return text
    if len(composition_candidates) != 1:
        raise ValueError(
            "expected exactly one dirtyRegion passed to doDisplayComposition "
            f"(found {len(composition_candidates)} candidates among "
            f"{len(dirty_matches)} dirtyRegion declarations)"
        )

    dirty_match, display_device = composition_candidates[0]
    indent = dirty_match.group("indent")
    continuation = indent + "                "
    damage_hook = f"""

{indent}const auto defaultDisplay = ON_MAIN_THREAD(getDefaultDisplayDeviceLocked());
{indent}if ({display_device} == defaultDisplay && !dirtyRegion.isEmpty()) {{
{indent}    const Rect damage = dirtyRegion.getBounds();
{indent}    std::lock_guard<std::mutex> lock(gLeaf3FrameNotifierMutex);
{indent}    if (gLeaf3FrameNotifierFd.get() < 0) {{
{indent}        // Damage is needed only while a bridge is registered.
{indent}    }} else if (!gLeaf3FrameDamageValid) {{
{indent}        gLeaf3FrameDamage = damage;
{indent}        gLeaf3FrameDamageValid = true;
{indent}    }} else {{
{indent}        gLeaf3FrameDamage.left =
{continuation}std::min(gLeaf3FrameDamage.left, damage.left);
{indent}        gLeaf3FrameDamage.top =
{continuation}std::min(gLeaf3FrameDamage.top, damage.top);
{indent}        gLeaf3FrameDamage.right =
{continuation}std::max(gLeaf3FrameDamage.right, damage.right);
{indent}        gLeaf3FrameDamage.bottom =
{continuation}std::max(gLeaf3FrameDamage.bottom, damage.bottom);
{indent}    }}
{indent}}}"""
    insertion_offset = dirty_match.end()
    return text[:insertion_offset] + damage_hook + text[insertion_offset:]


def patched(text: str) -> bool:
    common_patch_present = all(
        marker in text
        for marker in (
            UNIQUE_FD_INCLUDE,
            LEAF3_EPDC_INCLUDE,
            FCNTL_INCLUDE,
            UNISTD_INCLUDE,
            NOTIFIER_STATE,
            CREDENTIAL_HOOK,
            TRANSACTION_HOOK,
            POST_FRAME_HOOK,
        )
    )
    legacy_damage_present = all(
        marker in text
        for marker in (
            "const Rect damage = dirtyRegion.getBounds();",
            "gLeaf3FrameDamageValid = true;",
            "std::max(gLeaf3FrameDamage.bottom, damage.bottom);",
        )
    )
    return common_patch_present and (
        legacy_damage_present
        or "Leaf3EpdcController::get().takeNotifierDamage(\n"
           "                            &damage, &transientHint)" in text
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("surfaceflinger_cpp", type=Path)
    parser.add_argument("--check", action="store_true")
    parser.add_argument(
        "--version",
        action="version",
        version=f"Leaf3 SurfaceFlinger patcher {PATCHER_VERSION}",
    )
    args = parser.parse_args()

    path = args.surfaceflinger_cpp
    text = path.read_text()
    if patched(text):
        print(f"{path}: Leaf3 frame notifier is present")
        return 0
    if (
        NOTIFIER_STATE in text
        and CREDENTIAL_HOOK in text
        and PREVIOUS_UNOWNED_TRANSACTION_HOOK in text
        and POST_FRAME_HOOK in text
    ):
        if args.check:
            parser.error(
                f"{path}: Leaf3 transient hints need UID ownership"
            )
        try:
            text = replace_once(
                text,
                PREVIOUS_UNOWNED_TRANSACTION_HOOK,
                TRANSACTION_HOOK,
                "unowned transient-hint transaction",
            )
        except ValueError as error:
            parser.error(f"{path}: {error}")
        path.write_text(text)
        print(f"{path}: bound transient E-Ink hints to caller UID")
        return 0
    if (
        PREVIOUS_V2_NOTIFIER_STATE in text
        and PREVIOUS_V2_CREDENTIAL_HOOK in text
        and PREVIOUS_V2_TRANSACTION_HOOK in text
        and POST_FRAME_HOOK in text
    ):
        if args.check:
            parser.error(
                f"{path}: Leaf3 frame notifier needs transient-hint support"
            )
        try:
            text = replace_once(
                text,
                PREVIOUS_V2_NOTIFIER_STATE,
                NOTIFIER_STATE,
                "version-two notifier state",
            )
            text = replace_once(
                text,
                PREVIOUS_V2_CREDENTIAL_HOOK,
                CREDENTIAL_HOOK,
                "version-two notifier credentials",
            )
            text = replace_once(
                text,
                PREVIOUS_V2_TRANSACTION_HOOK,
                TRANSACTION_HOOK,
                "version-two notifier transaction",
            )
        except ValueError as error:
            parser.error(f"{path}: {error}")
        path.write_text(text)
        print(f"{path}: added transient E-Ink region hints")
        return 0
    if (
        PREVIOUS_DRAIN_NOTIFIER_STATE in text
        and PREVIOUS_DRAIN_TRANSACTION_HOOK in text
        and POST_FRAME_HOOK in text
        and CREDENTIAL_HOOK in text
    ):
        if args.check:
            parser.error(
                f"{path}: Leaf3 frame notifier needs native-drain support"
            )
        try:
            text = replace_once(
                text,
                PREVIOUS_DRAIN_NOTIFIER_STATE,
                NOTIFIER_STATE,
                "previous drain notifier state",
            )
            text = replace_once(
                text,
                PREVIOUS_DRAIN_TRANSACTION_HOOK,
                TRANSACTION_HOOK,
                "previous drain notifier transaction",
            )
        except ValueError as error:
            parser.error(f"{path}: {error}")
        path.write_text(text)
        print(f"{path}: added composer-native drain requests")
        return 0
    if (
        PREVIOUS_NOTIFIER_STATE in text
        and PREVIOUS_TRANSACTION_HOOK in text
        and POST_FRAME_HOOK in text
        and CREDENTIAL_HOOK in text
    ):
        if args.check:
            parser.error(
                f"{path}: Leaf3 frame notifier needs composer-refresh support"
            )
        try:
            text = replace_once(
                text,
                UNIQUE_FD_INCLUDE,
                UNIQUE_FD_INCLUDE
                + ("" if LEAF3_EPDC_INCLUDE in text else LEAF3_EPDC_INCLUDE),
                "Leaf3 unique-fd include",
            )
            text = replace_once(
                text,
                PREVIOUS_NOTIFIER_STATE,
                NOTIFIER_STATE,
                "previous notifier state",
            )
            text = replace_once(
                text,
                PREVIOUS_TRANSACTION_HOOK,
                TRANSACTION_HOOK,
                "previous notifier transaction",
            )
        except ValueError as error:
            parser.error(f"{path}: {error}")
        path.write_text(text)
        print(f"{path}: added composer-native full-refresh requests")
        return 0
    if (
        LEGACY_NOTIFIER_STATE in text
        and LEGACY_TRANSACTION_HOOK in text
        and POST_FRAME_HOOK in text
        and CREDENTIAL_HOOK in text
    ):
        if args.check:
            parser.error(f"{path}: legacy Leaf3 frame notifier needs upgrading")
        try:
            text = replace_once(
                text,
                UNIQUE_FD_INCLUDE,
                UNIQUE_FD_INCLUDE
                + ("" if LEAF3_EPDC_INCLUDE in text else LEAF3_EPDC_INCLUDE),
                "legacy Leaf3 unique-fd include",
            )
            text = replace_once(
                text,
                LEGACY_NOTIFIER_STATE,
                NOTIFIER_STATE,
                "legacy notifier state",
            )
            text = replace_once(
                text,
                LEGACY_TRANSACTION_HOOK,
                TRANSACTION_HOOK,
                "legacy notifier transaction",
            )
            text = patch_display_damage(text)
        except ValueError as error:
            parser.error(f"{path}: {error}")
        path.write_text(text)
        print(f"{path}: upgraded Leaf3 frame notifier with damage delivery")
        return 0
    if args.check:
        parser.error(f"{path}: Leaf3 frame notifier is missing or incomplete")
    if any(
        marker in text
        for marker in (
            UNIQUE_FD_INCLUDE,
            "kLeaf3FrameNotifierTransaction",
            "Leaf3 frame notifier registered",
            "Leaf3 frame notifier failed",
            "kLeaf3FrameNotifierTakeDamage",
        )
    ):
        parser.error(f"{path}: partial Leaf3 frame notifier patch found")

    try:
        text = replace_once(
            text,
            PROPERTIES_INCLUDE,
            PROPERTIES_INCLUDE
            + UNIQUE_FD_INCLUDE
            + ("" if LEAF3_EPDC_INCLUDE in text else LEAF3_EPDC_INCLUDE),
            "android-base properties include",
        )
        text = replace_once(
            text, ERRNO_INCLUDE, ERRNO_INCLUDE + FCNTL_INCLUDE, "errno include"
        )
        text = replace_once(
            text,
            SYS_TYPES_INCLUDE,
            SYS_TYPES_INCLUDE + UNISTD_INCLUDE,
            "sys/types include",
        )
        text = replace_once(
            text,
            NAMESPACE_MARKER,
            "namespace {\n\n" + NOTIFIER_STATE + "\n#pragma clang diagnostic push\n",
            "anonymous namespace marker",
        )
        text = replace_once(
            text,
            CREDENTIAL_MARKER,
            CREDENTIAL_HOOK,
            "credential-check function",
        )
        text = replace_once(
            text,
            TRANSACTION_MARKER,
            TRANSACTION_HOOK,
            "SurfaceFlinger transaction function",
        )
        text = replace_once(
            text, POST_FRAME_MARKER, POST_FRAME_HOOK, "postFrame function"
        )
        text = patch_display_damage(text)
    except ValueError as error:
        parser.error(f"{path}: {error}")

    path.write_text(text)
    print(f"{path}: added Leaf3 frame notifier")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
