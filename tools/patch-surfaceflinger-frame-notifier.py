#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0

"""Add the Leaf3 frame eventfd hook to LineageOS 18.1 SurfaceFlinger."""

import argparse
from pathlib import Path


UNIQUE_FD_INCLUDE = "#include <android-base/unique_fd.h>\n"
PROPERTIES_INCLUDE = "#include <android-base/properties.h>\n"
FCNTL_INCLUDE = "#include <fcntl.h>\n"
ERRNO_INCLUDE = "#include <errno.h>\n"
UNISTD_INCLUDE = "#include <unistd.h>\n"
SYS_TYPES_INCLUDE = "#include <sys/types.h>\n"

NOTIFIER_STATE = """\
constexpr uint32_t kLeaf3FrameNotifierTransaction = 1037;
constexpr int32_t kLeaf3FrameNotifierVersion = 1;

std::mutex gLeaf3FrameNotifierMutex;
base::unique_fd gLeaf3FrameNotifierFd;
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

TRANSACTION_MARKER = """\
status_t SurfaceFlinger::onTransact(uint32_t code, const Parcel& data, Parcel* reply,
                                    uint32_t flags) {
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


def replace_once(text: str, old: str, new: str, description: str) -> str:
    if text.count(old) != 1:
        raise ValueError(f"expected exactly one {description}")
    return text.replace(old, new, 1)


def patched(text: str) -> bool:
    return all(
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("surfaceflinger_cpp", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    path = args.surfaceflinger_cpp
    text = path.read_text()
    if patched(text):
        print(f"{path}: Leaf3 frame notifier is present")
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
    except ValueError as error:
        parser.error(f"{path}: {error}")

    path.write_text(text)
    print(f"{path}: added Leaf3 frame notifier")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
