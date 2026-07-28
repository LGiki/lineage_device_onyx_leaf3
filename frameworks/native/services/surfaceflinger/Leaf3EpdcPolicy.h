/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>

namespace android {

constexpr uint32_t kLeaf3CommitEpdcCommand = 0x08020000;
constexpr int64_t kLeaf3QualityCleanupDelay = 300000000;
constexpr int64_t kLeaf3BalancedCleanupDelay = 600000000;

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
