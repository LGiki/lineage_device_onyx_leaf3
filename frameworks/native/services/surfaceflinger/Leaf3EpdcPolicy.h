/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace android {

constexpr uint32_t kLeaf3CommitEpdcCommand = 0x08020000;
constexpr int64_t kLeaf3QualityCleanupDelay = 300000000;
constexpr int64_t kLeaf3BalancedCleanupDelay = 600000000;
constexpr size_t kLeaf3MaximumUpdates = 8;

constexpr bool leaf3TransientHintAuthorized(int32_t ownerUid,
                                            int32_t activeUid) {
  return ownerUid >= 0 && ownerUid == activeUid;
}

struct Leaf3PolicyRect {
  int32_t left;
  int32_t top;
  int32_t right;
  int32_t bottom;

  constexpr bool empty() const { return right <= left || bottom <= top; }
};

struct Leaf3TransientDamageSplit {
  std::array<Leaf3PolicyRect, kLeaf3MaximumUpdates> normal{};
  std::array<Leaf3PolicyRect, kLeaf3MaximumUpdates> transient{};
  size_t normalCount = 0;
  size_t transientCount = 0;
  bool overflow = false;
};

constexpr Leaf3PolicyRect intersectLeaf3Rects(const Leaf3PolicyRect &first,
                                              const Leaf3PolicyRect &second) {
  return Leaf3PolicyRect{
      first.left > second.left ? first.left : second.left,
      first.top > second.top ? first.top : second.top,
      first.right < second.right ? first.right : second.right,
      first.bottom < second.bottom ? first.bottom : second.bottom,
  };
}

inline void
appendLeaf3SplitRect(std::array<Leaf3PolicyRect, kLeaf3MaximumUpdates> *rects,
                     size_t *count, size_t otherCount,
                     const Leaf3PolicyRect &rect, bool *overflow) {
  if (rect.empty() || *overflow) {
    return;
  }
  if (*count + otherCount >= kLeaf3MaximumUpdates) {
    *overflow = true;
    return;
  }
  (*rects)[(*count)++] = rect;
}

inline Leaf3TransientDamageSplit
splitLeaf3TransientDamage(const Leaf3PolicyRect *damage, size_t damageCount,
                          const Leaf3PolicyRect &hint) {
  Leaf3TransientDamageSplit result;
  for (size_t index = 0; index < damageCount && !result.overflow; ++index) {
    const Leaf3PolicyRect current = damage[index];
    if (current.empty()) {
      continue;
    }
    const Leaf3PolicyRect moving = intersectLeaf3Rects(current, hint);
    if (moving.empty()) {
      appendLeaf3SplitRect(&result.normal, &result.normalCount,
                           result.transientCount, current, &result.overflow);
      continue;
    }
    appendLeaf3SplitRect(
        &result.normal, &result.normalCount, result.transientCount,
        Leaf3PolicyRect{current.left, current.top, current.right, moving.top},
        &result.overflow);
    appendLeaf3SplitRect(&result.normal, &result.normalCount,
                         result.transientCount,
                         Leaf3PolicyRect{current.left, moving.bottom,
                                         current.right, current.bottom},
                         &result.overflow);
    appendLeaf3SplitRect(
        &result.normal, &result.normalCount, result.transientCount,
        Leaf3PolicyRect{current.left, moving.top, moving.left, moving.bottom},
        &result.overflow);
    appendLeaf3SplitRect(
        &result.normal, &result.normalCount, result.transientCount,
        Leaf3PolicyRect{moving.right, moving.top, current.right, moving.bottom},
        &result.overflow);
    appendLeaf3SplitRect(&result.transient, &result.transientCount,
                         result.normalCount, moving, &result.overflow);
  }
  return result;
}

enum class Leaf3CleanupPolicy {
  Quality,
  Balanced,
  Manual,
};

class Leaf3CleanupSchedule {
public:
  void noteActivity(int64_t now) {
    mPending = true;
    mQuietSince = now;
  }

  void clear() {
    mPending = false;
    mQuietSince = 0;
  }

  bool pending() const { return mPending; }

  int64_t deadline(Leaf3CleanupPolicy policy) const {
    if (!mPending || policy == Leaf3CleanupPolicy::Manual) {
      return 0;
    }
    return mQuietSince + (policy == Leaf3CleanupPolicy::Quality
                              ? kLeaf3QualityCleanupDelay
                              : kLeaf3BalancedCleanupDelay);
  }

  int64_t nextCheck(int64_t now, Leaf3CleanupPolicy policy,
                    int64_t pollInterval, bool wakeOutstanding) const {
    if (wakeOutstanding) {
      return 0;
    }
    const int64_t target = deadline(policy);
    if (target == 0) {
      return 0;
    }
    const int64_t policyCheck = now + pollInterval;
    return target < policyCheck ? target : policyCheck;
  }

  void complete(int64_t now, bool workRemaining) {
    if (workRemaining) {
      noteActivity(now);
    } else {
      clear();
    }
  }

private:
  bool mPending = false;
  int64_t mQuietSince = 0;
};

struct Leaf3PresentPolicy {
  bool restartCleanup;
  bool cancelSettled;
};

constexpr Leaf3PresentPolicy leaf3PresentPolicy(bool hasDamage) {
  return Leaf3PresentPolicy{hasDamage, hasDamage};
}

constexpr bool shouldDispatchLeaf3Wake(bool wakePending, bool wakeDispatched,
                                       bool callbackAvailable) {
  return wakePending && !wakeDispatched && callbackAvailable;
}

constexpr int64_t armLeaf3ImmediateDeadline(int64_t now,
                                            int64_t armedDeadline) {
  return armedDeadline != 0 && armedDeadline < now ? armedDeadline : now;
}

constexpr bool shouldActivateLeaf3Controller(bool requested, bool supported,
                                             bool failed, bool blocked,
                                             bool callbackAvailable) {
  return requested && supported && !failed && !blocked && callbackAvailable;
}

constexpr bool isLeaf3CommitEpdcCommand(uint32_t command) {
  return command == kLeaf3CommitEpdcCommand;
}

} // namespace android
