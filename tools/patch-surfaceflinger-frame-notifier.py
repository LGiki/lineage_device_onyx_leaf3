#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0

"""Add Leaf3 frame notification and damage delivery to SurfaceFlinger."""

import argparse
import re
from pathlib import Path


PATCHER_VERSION = "4"

UNIQUE_FD_INCLUDE = "#include <android-base/unique_fd.h>\n"
PROPERTIES_INCLUDE = "#include <android-base/properties.h>\n"
FCNTL_INCLUDE = "#include <fcntl.h>\n"
ERRNO_INCLUDE = "#include <errno.h>\n"
UNISTD_INCLUDE = "#include <unistd.h>\n"
SYS_TYPES_INCLUDE = "#include <sys/types.h>\n"

NOTIFIER_STATE = """\
constexpr uint32_t kLeaf3FrameNotifierTransaction = 1037;
constexpr int32_t kLeaf3FrameNotifierVersion = 2;
constexpr int32_t kLeaf3FrameNotifierUnregister = 0;
constexpr int32_t kLeaf3FrameNotifierRegister = 1;
constexpr int32_t kLeaf3FrameNotifierTakeDamage = 2;

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

#pragma clang diagnostic push
"""

CREDENTIAL_MARKER = """\
status_t SurfaceFlinger::CheckTransactCodeCredentials(uint32_t code) {
#pragma clang diagnostic push
"""

TRANSACTION_HOOK = """\
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

        const int32_t command = data.readInt32();
        if (command == kLeaf3FrameNotifierTakeDamage) {
            std::lock_guard<std::mutex> lock(gLeaf3FrameNotifierMutex);
            if (reply != nullptr) {
                reply->writeInt32(gLeaf3FrameDamageValid ? 1 : 0);
                if (gLeaf3FrameDamageValid) {
                    reply->writeInt32(gLeaf3FrameDamage.left);
                    reply->writeInt32(gLeaf3FrameDamage.top);
                    reply->writeInt32(gLeaf3FrameDamage.right);
                    reply->writeInt32(gLeaf3FrameDamage.bottom);
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

TRANSACTION_MARKER = """\
status_t SurfaceFlinger::onTransact(uint32_t code, const Parcel& data, Parcel* reply,
                                    uint32_t flags) {
    status_t credentialCheck = CheckTransactCodeCredentials(code);
"""

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
            FCNTL_INCLUDE,
            UNISTD_INCLUDE,
            NOTIFIER_STATE,
            CREDENTIAL_HOOK,
            TRANSACTION_HOOK,
            POST_FRAME_HOOK,
        )
    )
    return common_patch_present and all(
        marker in text
        for marker in (
            "const Rect damage = dirtyRegion.getBounds();",
            "gLeaf3FrameDamageValid = true;",
            "std::max(gLeaf3FrameDamage.bottom, damage.bottom);",
        )
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
            PROPERTIES_INCLUDE + UNIQUE_FD_INCLUDE,
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
