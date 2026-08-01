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
constexpr int64_t kLeaf3ReaderPageSettleDelay = 180000000;
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

enum class Leaf3ReaderPageResult {
  None,
  Counted,
  FullRefresh,
};

constexpr bool leaf3ReaderPageCandidate(bool large, bool hintEmpty,
                                        bool splitOverflow,
                                        bool transientScrolling,
                                        bool pageTurnGesture) {
  return large && (hintEmpty || pageTurnGesture ||
                   (!splitOverflow && !transientScrolling));
}

constexpr bool leaf3ShouldTrackTransientGhosts(bool cleanupEnabled,
                                               bool readerPageTurnMotion) {
  return cleanupEnabled && !readerPageTurnMotion;
}

class Leaf3ReaderPageSchedule {
public:
  void notePresent(int64_t now, bool large) {
    if (mFullRefreshPending) {
      mQuietSince = now;
      return;
    }
    if (large) {
      mPending = true;
    }
    if (mPending) {
      mQuietSince = now;
    }
  }

  void reset() {
    mPending = false;
    mFullRefreshPending = false;
    mQuietSince = 0;
    mPageCount = 0;
    mLastGestureId = -1;
  }

  int64_t deadline() const {
    return (mPending || mFullRefreshPending)
               ? mQuietSince + kLeaf3ReaderPageSettleDelay
               : 0;
  }

  Leaf3ReaderPageResult settle(int64_t now, int interval) {
    const int64_t target = deadline();
    if (target == 0 || now < target) {
      return Leaf3ReaderPageResult::None;
    }
    if (mFullRefreshPending) {
      mFullRefreshPending = false;
      mQuietSince = 0;
      return Leaf3ReaderPageResult::FullRefresh;
    }
    mPending = false;
    mQuietSince = 0;
    if (interval <= 0) {
      mPageCount = 0;
      return Leaf3ReaderPageResult::Counted;
    }
    ++mPageCount;
    if (mPageCount < interval) {
      return Leaf3ReaderPageResult::Counted;
    }
    mPageCount = 0;
    return Leaf3ReaderPageResult::FullRefresh;
  }

  Leaf3ReaderPageResult advancePresent(int64_t now, bool large, int interval) {
    const Leaf3ReaderPageResult result = settle(now, interval);
    if (result == Leaf3ReaderPageResult::FullRefresh) {
      return deferFullRefresh(now);
    }
    notePresent(now, large);
    return result;
  }

  Leaf3ReaderPageResult advanceScroll(int64_t now, int interval) {
    const Leaf3ReaderPageResult result = settle(now, interval);
    if (result == Leaf3ReaderPageResult::FullRefresh) {
      return deferFullRefresh(now);
    }
    if (mFullRefreshPending) {
      mQuietSince = now;
      return result;
    }
    // A vertical gesture proves that an unexpired large candidate was a
    // scrolling frame, not a completed static page.
    mPending = false;
    mQuietSince = 0;
    return result;
  }

  Leaf3ReaderPageResult advanceGesture(int64_t now, int32_t gestureId,
                                       int interval) {
    if (gestureId <= 0) {
      return Leaf3ReaderPageResult::None;
    }
    const Leaf3ReaderPageResult settled = settle(now, interval);
    if (settled == Leaf3ReaderPageResult::FullRefresh) {
      mLastGestureId = gestureId;
      return deferFullRefresh(now);
    }
    if (mFullRefreshPending) {
      // A newer gesture will be included in the already-owed cleanup, so it
      // extends the quiet deadline and becomes part of that clean baseline.
      mLastGestureId = gestureId;
      mQuietSince = now;
      return settled;
    }
    if (gestureId == mLastGestureId) {
      return settled;
    }
    mLastGestureId = gestureId;
    // A present can beat its one-way gesture hint and start a static-page
    // debounce. Absorb it only while its quiet window is still active; settle
    // above has already counted a truly separate, expired static page.
    if (mPending) {
      mPending = false;
      mQuietSince = 0;
    }
    if (interval <= 0) {
      mPageCount = 0;
      return Leaf3ReaderPageResult::Counted;
    }
    const int totalPages = mPageCount + 1;
    if (totalPages < interval) {
      mPageCount = totalPages;
      return Leaf3ReaderPageResult::Counted;
    }
    // Count the gesture now, but wait until all of its animation frames have
    // been quiet before cleaning the final composed page.
    mPageCount = 0;
    mFullRefreshPending = true;
    mQuietSince = now;
    return Leaf3ReaderPageResult::Counted;
  }

  int pageCount() const { return mPageCount; }

private:
  Leaf3ReaderPageResult deferFullRefresh(int64_t now) {
    // Damage arriving at the deadline breaks the quiet period. Clean only
    // after this newest frame and any following animation have settled.
    mFullRefreshPending = true;
    mQuietSince = now;
    return Leaf3ReaderPageResult::Counted;
  }

  bool mPending = false;
  bool mFullRefreshPending = false;
  int64_t mQuietSince = 0;
  int mPageCount = 0;
  int32_t mLastGestureId = -1;
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

constexpr bool shouldRunLeaf3Timers(bool active, bool interactive) {
  return active && interactive;
}

constexpr bool isLeaf3CommitEpdcCommand(uint32_t command) {
  return command == kLeaf3CommitEpdcCommand;
}

} // namespace android
