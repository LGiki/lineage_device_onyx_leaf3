/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Leaf3EpdcController.h"

#include <android-base/properties.h>
#include <log/log.h>
#include <utils/Timers.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

namespace android {
namespace {

constexpr char kBackendProperty[] = "persist.sys.leaf3.epdc_backend";
constexpr char kBlockedProperty[] = "sys.leaf3.epdc_native_blocked";
constexpr char kStateProperty[] = "sys.leaf3.stat.epdc_native_state";
constexpr char kActiveModeProperty[] = "sys.leaf3.active_refresh_mode";
constexpr char kGlobalModeProperty[] = "persist.sys.leaf3.refresh_mode";
constexpr char kActivePackageProperty[] = "sys.leaf3.active_package";
constexpr char kActiveUidProperty[] = "sys.leaf3.active_uid";
constexpr char kActivePageIntervalProperty[] = "sys.leaf3.active_page_interval";
constexpr char kActiveDitherProperty[] = "sys.leaf3.active_dither";
constexpr char kCleanupProperty[] = "persist.sys.leaf3.cleanup_policy";
constexpr char kPageIntervalProperty[] = "persist.sys.leaf3.page_interval";
constexpr char kSettledQualityProperty[] = "persist.sys.leaf3.settle_quality";
constexpr char kDitherProperty[] = "persist.sys.leaf3.dither";
constexpr char kInteractiveProperty[] = "sys.leaf3.interactive";

constexpr uint32_t kWaveformDu = 1;
constexpr uint32_t kWaveformGc16 = 2;
constexpr uint32_t kWaveformAnim = 4;
constexpr uint32_t kWaveformAuto = 5;
constexpr uint32_t kWaveformRegal = 6;
constexpr uint32_t kModeFull = 0x20;
constexpr uint32_t kModeDither = 0x100;

constexpr size_t kMaximumBatch = kLeaf3MaximumUpdates;
constexpr int32_t kAlignment = 8;
constexpr int32_t kGhostTile = 32;
constexpr nsecs_t kSettledDelay = 180000000;
constexpr nsecs_t kCleanupPolicyPollInterval = 50000000;
constexpr nsecs_t kStatsInterval = 60000000000;
constexpr nsecs_t kSubmissionDrainDelay = 100000000;

uint64_t rectArea(const Rect &rect) {
  if (!rect.isValid() || rect.isEmpty()) {
    return 0;
  }
  return static_cast<uint64_t>(rect.getWidth()) *
         static_cast<uint64_t>(rect.getHeight());
}

Rect unionRects(const Rect &first, const Rect &second) {
  Rect result(first);
  result.left = std::min(result.left, second.left);
  result.top = std::min(result.top, second.top);
  result.right = std::max(result.right, second.right);
  result.bottom = std::max(result.bottom, second.bottom);
  return result;
}

Rect intersectRects(const Rect &first, const Rect &second) {
  const Rect result(std::max(first.left, second.left),
                    std::max(first.top, second.top),
                    std::min(first.right, second.right),
                    std::min(first.bottom, second.bottom));
  return result.isValid() && !result.isEmpty() ? result : Rect();
}

Rect alignedRect(const Rect &damage, const Rect &bounds) {
  if (damage.isEmpty() || bounds.isEmpty()) {
    return Rect();
  }
  Rect result = damage;
  result.left = std::max(bounds.left, (result.left / kAlignment) * kAlignment);
  result.top = std::max(bounds.top, (result.top / kAlignment) * kAlignment);
  result.right =
      std::min(bounds.right,
               ((result.right + kAlignment - 1) / kAlignment) * kAlignment);
  result.bottom =
      std::min(bounds.bottom,
               ((result.bottom + kAlignment - 1) / kAlignment) * kAlignment);
  return result;
}

uint64_t damageArea(const Region &damage) {
  uint64_t area = 0;
  for (const Rect &rect : damage) {
    area += rectArea(rect);
  }
  return area;
}

int supportedPageInterval() {
  const std::string active =
      android::base::GetProperty(kActivePageIntervalProperty, "");
  const int requested =
      active.empty() ? android::base::GetIntProperty(kPageIntervalProperty, 10)
                     : std::atoi(active.c_str());
  constexpr int supported[] = {0, 1, 3, 5, 10, 30, 50};
  for (const int interval : supported) {
    if (requested == interval) {
      return interval;
    }
  }
  return 10;
}

uint32_t ditherFlag() {
  const std::string active =
      android::base::GetProperty(kActiveDitherProperty, "");
  const bool enabled =
      active.empty() ? android::base::GetBoolProperty(kDitherProperty, true)
                     : active != "0";
  return enabled ? kModeDither : 0;
}

std::string effectiveMode() {
  const std::string active =
      android::base::GetProperty(kActiveModeProperty, "");
  return active.empty()
             ? android::base::GetProperty(kGlobalModeProperty, "balanced")
             : active;
}

} // namespace

class Leaf3EpdcController::Impl {
public:
  Impl()
      : requested(android::base::GetProperty(kBackendProperty, "bridge") ==
                  "composer") {}

  ~Impl() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
      condition.notify_all();
    }
    if (worker.joinable()) {
      worker.join();
    }
  }

  bool activeLocked() const {
    return ready && requested && supported && !failed && !blocked &&
           !android::base::GetBoolProperty(kBlockedProperty, false);
  }

  void publishState(const char *state) {
    if (!android::base::SetProperty(kStateProperty, state)) {
      ALOGW("could not publish native EPDC state %s", state);
    }
  }

  void activateLocked() {
    const bool nativeBlocked =
        blocked || android::base::GetBoolProperty(kBlockedProperty, false);
    if (ready || !shouldActivateLeaf3Controller(
                     requested, supported, failed, nativeBlocked,
                     static_cast<bool>(refreshCallback))) {
      return;
    }
    ready = true;
    manualPending = true;
    armTimerLocked();
    if (!worker.joinable()) {
      worker = std::thread([this] { timerLoop(); });
    }
    publishState("ready");
    ALOGI("composer-native EPDC transport is ready");
  }

  void configureBoundsLocked(const Rect &newBounds) {
    if (bounds == newBounds) {
      return;
    }
    bounds = newBounds;
    tileColumns =
        std::max(0, (bounds.getWidth() + kGhostTile - 1) / kGhostTile);
    tileRows = std::max(0, (bounds.getHeight() + kGhostTile - 1) / kGhostTile);
    ghostAge.assign(static_cast<size_t>(tileColumns * tileRows), 0);
    ageSequence = 0;
    cleanupSchedule.clear();
    settledDeadline = 0;
    settledRegion = Rect();
    pageCount = 0;
    transientRegion = Rect();
    transientDeadline = 0;
    transientUid = -1;
  }

  void markFastLocked(const Rect &rect) {
    if (tileColumns <= 0 || tileRows <= 0) {
      return;
    }
    ++ageSequence;
    const int left = std::max(0, (rect.left - bounds.left) / kGhostTile);
    const int top = std::max(0, (rect.top - bounds.top) / kGhostTile);
    const int right = std::min(
        tileColumns, (rect.right - bounds.left + kGhostTile - 1) / kGhostTile);
    const int bottom = std::min(
        tileRows, (rect.bottom - bounds.top + kGhostTile - 1) / kGhostTile);
    for (int y = top; y < bottom; ++y) {
      for (int x = left; x < right; ++x) {
        ghostAge[static_cast<size_t>(y * tileColumns + x)] = ageSequence;
      }
    }
  }

  void clearGhostLocked(const Rect &rect) {
    if (tileColumns <= 0 || tileRows <= 0) {
      return;
    }
    const int left = std::max(0, (rect.left - bounds.left) / kGhostTile);
    const int top = std::max(0, (rect.top - bounds.top) / kGhostTile);
    const int right = std::min(
        tileColumns, (rect.right - bounds.left + kGhostTile - 1) / kGhostTile);
    const int bottom = std::min(
        tileRows, (rect.bottom - bounds.top + kGhostTile - 1) / kGhostTile);
    for (int y = top; y < bottom; ++y) {
      for (int x = left; x < right; ++x) {
        ghostAge[static_cast<size_t>(y * tileColumns + x)] = 0;
      }
    }
  }

  bool hasGhostLocked() const {
    return std::any_of(ghostAge.begin(), ghostAge.end(),
                       [](uint64_t age) { return age != 0; });
  }

  Rect nextCleanupLocked() const {
    uint64_t oldestAge = UINT64_MAX;
    int start = -1;
    for (size_t index = 0; index < ghostAge.size(); ++index) {
      if (ghostAge[index] != 0 && ghostAge[index] < oldestAge) {
        oldestAge = ghostAge[index];
        start = static_cast<int>(index);
      }
    }
    if (start < 0) {
      return Rect();
    }

    const uint64_t maximumArea = std::max<uint64_t>(1, rectArea(bounds) / 3);
    std::vector<uint8_t> visited(ghostAge.size(), 0);
    std::deque<int> queue;
    queue.push_back(start);
    visited[static_cast<size_t>(start)] = 1;
    const int startX = start % tileColumns;
    const int startY = start / tileColumns;
    Rect result(
        bounds.left + startX * kGhostTile, bounds.top + startY * kGhostTile,
        std::min(bounds.right, bounds.left + (startX + 1) * kGhostTile),
        std::min(bounds.bottom, bounds.top + (startY + 1) * kGhostTile));
    while (!queue.empty()) {
      const int index = queue.front();
      queue.pop_front();
      const int x = index % tileColumns;
      const int y = index / tileColumns;
      constexpr int dx[] = {-1, 1, 0, 0};
      constexpr int dy[] = {0, 0, -1, 1};
      for (size_t direction = 0; direction < 4; ++direction) {
        const int nextX = x + dx[direction];
        const int nextY = y + dy[direction];
        if (nextX < 0 || nextX >= tileColumns || nextY < 0 ||
            nextY >= tileRows) {
          continue;
        }
        const int next = nextY * tileColumns + nextX;
        if (!visited[static_cast<size_t>(next)] &&
            ghostAge[static_cast<size_t>(next)] != 0) {
          visited[static_cast<size_t>(next)] = 1;
          const Rect nextRect(
              bounds.left + nextX * kGhostTile, bounds.top + nextY * kGhostTile,
              std::min(bounds.right, bounds.left + (nextX + 1) * kGhostTile),
              std::min(bounds.bottom, bounds.top + (nextY + 1) * kGhostTile));
          const Rect candidate = unionRects(result, nextRect);
          if (rectArea(candidate) > maximumArea) {
            continue;
          }
          result = candidate;
          queue.push_back(next);
        }
      }
    }
    return result;
  }

  Leaf3CleanupPolicy cleanupPolicyLocked() const {
    const std::string policy =
        android::base::GetProperty(kCleanupProperty, "balanced");
    if (policy == "quality") {
      return Leaf3CleanupPolicy::Quality;
    }
    if (policy == "manual") {
      return Leaf3CleanupPolicy::Manual;
    }
    return Leaf3CleanupPolicy::Balanced;
  }

  void applyCleanupPolicyLocked() {
    if (cleanupPolicyLocked() != Leaf3CleanupPolicy::Manual) {
      return;
    }
    cleanupSchedule.clear();
    std::fill(ghostAge.begin(), ghostAge.end(), 0);
    pageCount = 0;
  }

  void discardPendingWorkLocked() {
    wakePending = false;
    wakeDispatched = false;
    manualPending = false;
    cleanupSchedule.clear();
    settledDeadline = 0;
    settledRegion = Rect();
    transientDeadline = 0;
    transientRegion = Rect();
    transientUid = -1;
    std::fill(ghostAge.begin(), ghostAge.end(), 0);
    pageCount = 0;
    armedDeadline = 0;
  }

  void armTimerLocked() {
    if (wakePending) {
      return;
    }
    nsecs_t deadline = 0;
    if (manualPending) {
      // Keep the first immediate deadline stable. Replacing it with a newer
      // systemTime() on every worker iteration prevents the sampled `now`
      // from ever reaching it and starves SurfaceFlinger callback setup.
      deadline = armLeaf3ImmediateDeadline(systemTime(), armedDeadline);
    }
    if (settledDeadline != 0 && (deadline == 0 || settledDeadline < deadline)) {
      deadline = settledDeadline;
    }
    if (cleanupSchedule.pending() && !hasGhostLocked()) {
      cleanupSchedule.clear();
    }
    const nsecs_t cleanupDeadline =
        cleanupSchedule.deadline(cleanupPolicyLocked());
    if (cleanupDeadline != 0 && (deadline == 0 || cleanupDeadline < deadline)) {
      deadline = cleanupDeadline;
    }
    if (deadline != armedDeadline) {
      armedDeadline = deadline;
      condition.notify_all();
    }
  }

  Leaf3EpdcUpdate makeUpdateLocked(const Rect &rect, uint32_t mode) {
    ++commands;
    pixels += rectArea(rect);
    statsDirty = true;
    condition.notify_all();
    return Leaf3EpdcUpdate{static_cast<uint32_t>(rect.left),
                           static_cast<uint32_t>(rect.top),
                           static_cast<uint32_t>(rect.right),
                           static_cast<uint32_t>(rect.bottom), mode};
  }

  std::vector<Leaf3EpdcUpdate> forcedUpdateLocked(nsecs_t now) {
    if (manualPending) {
      manualPending = false;
      wakePending = false;
      wakeDispatched = false;
      cleanupSchedule.clear();
      settledDeadline = 0;
      settledRegion = Rect();
      std::fill(ghostAge.begin(), ghostAge.end(), 0);
      pageCount = 0;
      ++fullRefreshes;
      armTimerLocked();
      return {
          makeUpdateLocked(bounds, kWaveformGc16 | kModeFull | ditherFlag())};
    }
    if (settledDeadline != 0 && now >= settledDeadline &&
        !settledRegion.isEmpty()) {
      const Rect region = settledRegion;
      settledRegion = Rect();
      settledDeadline = 0;
      wakePending = false;
      wakeDispatched = false;
      ++settledUpdates;
      armTimerLocked();
      return {makeUpdateLocked(region, kWaveformRegal | ditherFlag())};
    }
    const nsecs_t cleanupDeadline =
        cleanupSchedule.deadline(cleanupPolicyLocked());
    if (cleanupDeadline != 0 && now >= cleanupDeadline && hasGhostLocked()) {
      const Rect region = nextCleanupLocked();
      clearGhostLocked(region);
      wakePending = false;
      wakeDispatched = false;
      ++cleanupUpdates;
      cleanupSchedule.complete(now, hasGhostLocked());
      armTimerLocked();
      return {makeUpdateLocked(region, kWaveformGc16 | ditherFlag())};
    }
    wakePending = false;
    wakeDispatched = false;
    armTimerLocked();
    return {};
  }

  void timerLoop() {
    std::unique_lock<std::mutex> lock(mutex);
    nextStatsPublish = systemTime() + kStatsInterval;
    while (!stopping) {
      const nsecs_t now = systemTime();
      if (activeLocked()) {
        applyCleanupPolicyLocked();
        armTimerLocked();
      } else {
        discardPendingWorkLocked();
      }
      if (statsDirty && now >= nextStatsPublish) {
        const uint64_t commandSnapshot = commands;
        const uint64_t pixelSnapshot = pixels;
        const uint64_t cleanupSnapshot = cleanupUpdates;
        const uint64_t pageSnapshot = pageCleanups;
        const uint64_t settledSnapshot = settledUpdates;
        const uint64_t transientSnapshot = transientUpdates;
        const uint64_t errorSnapshot = errors;
        statsDirty = false;
        nextStatsPublish = now + kStatsInterval;
        lock.unlock();
        android::base::SetProperty("sys.leaf3.stat.epdc_native_commands",
                                   std::to_string(commandSnapshot));
        android::base::SetProperty("sys.leaf3.stat.epdc_native_pixels",
                                   std::to_string(pixelSnapshot));
        android::base::SetProperty("sys.leaf3.stat.epdc_native_cleanup",
                                   std::to_string(cleanupSnapshot));
        android::base::SetProperty("sys.leaf3.stat.epdc_native_page_cleanups",
                                   std::to_string(pageSnapshot));
        android::base::SetProperty("sys.leaf3.stat.epdc_native_settled",
                                   std::to_string(settledSnapshot));
        android::base::SetProperty("sys.leaf3.stat.epdc_native_transient",
                                   std::to_string(transientSnapshot));
        android::base::SetProperty("sys.leaf3.stat.epdc_native_errors",
                                   std::to_string(errorSnapshot));
        lock.lock();
        continue;
      }

      const bool deadlineExpired =
          armedDeadline != 0 && now >= armedDeadline && !wakePending;
      const bool pendingNeedsDispatch = shouldDispatchLeaf3Wake(
          wakePending, wakeDispatched, static_cast<bool>(refreshCallback));
      if (deadlineExpired || pendingNeedsDispatch) {
        if (deadlineExpired) {
          wakePending = true;
          armedDeadline = 0;
        }
        const auto callback = refreshCallback;
        wakeDispatched = static_cast<bool>(callback);
        if (callback) {
          ++callbackInFlight;
        }
        lock.unlock();
        if (callback) {
          callback();
        }
        lock.lock();
        if (callback) {
          --callbackInFlight;
          condition.notify_all();
        }
        continue;
      }

      nsecs_t waitUntil = 0;
      if (armedDeadline != 0) {
        waitUntil = armedDeadline;
      }
      if (statsDirty && (waitUntil == 0 || nextStatsPublish < waitUntil)) {
        waitUntil = nextStatsPublish;
      }
      const nsecs_t cleanupCheck = cleanupSchedule.nextCheck(
          now, cleanupPolicyLocked(), kCleanupPolicyPollInterval, wakePending);
      if (cleanupCheck != 0) {
        if (waitUntil == 0 || cleanupCheck < waitUntil) {
          waitUntil = cleanupCheck;
        }
      }
      if (waitUntil == 0) {
        condition.wait(lock);
      } else {
        const nsecs_t remaining =
            std::max<nsecs_t>(0, waitUntil - systemTime());
        condition.wait_for(lock, std::chrono::nanoseconds(remaining));
      }
    }
  }

  mutable std::mutex mutex;
  std::condition_variable condition;
  std::thread worker;
  bool stopping = false;
  const bool requested;
  bool supported = false;
  bool ready = false;
  bool failed = false;
  bool blocked = false;
  bool wakePending = false;
  bool wakeDispatched = false;
  bool manualPending = false;
  bool statsDirty = false;
  int callbackInFlight = 0;
  int submissionsInFlight = 0;
  nsecs_t lastSubmissionEnd = 0;
  std::function<void()> refreshCallback;

  Rect bounds;
  int tileColumns = 0;
  int tileRows = 0;
  std::vector<uint64_t> ghostAge;
  uint64_t ageSequence = 0;
  std::string foregroundToken;
  int pageCount = 0;
  Rect settledRegion;
  Rect notifierDamage;
  Rect transientRegion;
  nsecs_t settledDeadline = 0;
  nsecs_t transientDeadline = 0;
  int32_t transientUid = -1;
  Leaf3CleanupSchedule cleanupSchedule;
  nsecs_t armedDeadline = 0;
  nsecs_t nextStatsPublish = 0;

  uint64_t commands = 0;
  uint64_t pixels = 0;
  uint64_t cleanupUpdates = 0;
  uint64_t pageCleanups = 0;
  uint64_t settledUpdates = 0;
  uint64_t transientUpdates = 0;
  uint64_t fullRefreshes = 0;
  uint64_t errors = 0;
};

Leaf3EpdcController &Leaf3EpdcController::get() {
  static Leaf3EpdcController controller;
  return controller;
}

Leaf3EpdcController::Leaf3EpdcController() : mImpl(std::make_unique<Impl>()) {}

Leaf3EpdcController::~Leaf3EpdcController() = default;

bool Leaf3EpdcController::isRequested() const { return mImpl->requested; }

bool Leaf3EpdcController::isActive() const {
  std::lock_guard<std::mutex> lock(mImpl->mutex);
  return mImpl->activeLocked();
}

void Leaf3EpdcController::setComposerSupported(bool supported) {
  std::lock_guard<std::mutex> lock(mImpl->mutex);
  mImpl->supported = supported;
  if (!mImpl->requested) {
    mImpl->publishState("bridge");
  } else if (supported) {
    mImpl->activateLocked();
  } else {
    mImpl->failed = true;
    mImpl->discardPendingWorkLocked();
    mImpl->condition.notify_all();
    mImpl->publishState("unsupported");
    ALOGE("QTI composer EPDC extension is unavailable");
  }
}

void Leaf3EpdcController::setRefreshCallback(std::function<void()> callback) {
  std::lock_guard<std::mutex> lock(mImpl->mutex);
  mImpl->refreshCallback = std::move(callback);
  mImpl->activateLocked();
  mImpl->condition.notify_all();
}

void Leaf3EpdcController::clearRefreshCallback() {
  std::unique_lock<std::mutex> lock(mImpl->mutex);
  mImpl->refreshCallback = nullptr;
  mImpl->ready = false;
  mImpl->condition.notify_all();
  mImpl->condition.wait(lock, [this] { return mImpl->callbackInFlight == 0; });
}

std::vector<Leaf3EpdcUpdate>
Leaf3EpdcController::preparePresent(const Region &damage, const Rect &bounds) {
  std::lock_guard<std::mutex> lock(mImpl->mutex);
  if (!damage.isEmpty()) {
    const Rect observed = damage.getBounds();
    mImpl->notifierDamage = mImpl->notifierDamage.isEmpty()
                                ? observed
                                : unionRects(mImpl->notifierDamage, observed);
  }
  if (!mImpl->activeLocked()) {
    return {};
  }
  mImpl->configureBoundsLocked(bounds);
  if (!android::base::GetBoolProperty(kInteractiveProperty, true)) {
    return {};
  }
  const nsecs_t now = systemTime();
  const std::string package =
      android::base::GetProperty(kActivePackageProperty, "");
  if (package != mImpl->foregroundToken) {
    mImpl->foregroundToken = package;
    mImpl->pageCount = 0;
    mImpl->transientRegion = Rect();
    mImpl->transientDeadline = 0;
    mImpl->transientUid = -1;
  }

  const std::string cleanup =
      android::base::GetProperty(kCleanupProperty, "balanced");
  const std::string mode = effectiveMode();
  const bool settledQuality =
      android::base::GetBoolProperty(kSettledQualityProperty, true);

  if (cleanup == "manual") {
    mImpl->applyCleanupPolicyLocked();
  }
  if (mode != "regal" || !settledQuality) {
    mImpl->settledDeadline = 0;
    mImpl->settledRegion = Rect();
  }
  const Leaf3PresentPolicy presentPolicy =
      leaf3PresentPolicy(!damage.isEmpty());
  if (presentPolicy.restartCleanup || presentPolicy.cancelSettled) {
    // Any replacement frame restarts the cleanup quiet period and
    // invalidates a settled pass prepared for an older composition.
    if (presentPolicy.restartCleanup && mImpl->cleanupSchedule.pending() &&
        cleanup != "manual") {
      mImpl->cleanupSchedule.noteActivity(now);
    }
    if (presentPolicy.cancelSettled) {
      mImpl->settledDeadline = 0;
      mImpl->settledRegion = Rect();
    }
  }
  mImpl->armTimerLocked();

  const nsecs_t cleanupDeadline =
      mImpl->cleanupSchedule.deadline(mImpl->cleanupPolicyLocked());
  if (mImpl->wakePending || mImpl->manualPending ||
      (mImpl->settledDeadline != 0 && now >= mImpl->settledDeadline) ||
      (cleanupDeadline != 0 && now >= cleanupDeadline)) {
    auto forced = mImpl->forcedUpdateLocked(now);
    if (!forced.empty()) {
      return forced;
    }
  }

  const Rect rect = alignedRect(damage.getBounds(), bounds);
  if (rect.isEmpty()) {
    return {};
  }

  const int32_t activeUid =
      android::base::GetIntProperty(kActiveUidProperty, -1);
  if (mImpl->transientDeadline != 0 &&
      (now >= mImpl->transientDeadline ||
       !leaf3TransientHintAuthorized(mImpl->transientUid, activeUid))) {
    mImpl->transientRegion = Rect();
    mImpl->transientDeadline = 0;
    mImpl->transientUid = -1;
  }
  const Rect transient =
      alignedRect(intersectRects(mImpl->transientRegion, bounds), bounds);
  std::vector<Leaf3PolicyRect> alignedDamage;
  alignedDamage.reserve(kMaximumBatch);
  for (const Rect &damageRect : damage) {
    const Rect aligned = alignedRect(damageRect, bounds);
    if (!aligned.isEmpty()) {
      alignedDamage.push_back(Leaf3PolicyRect{aligned.left, aligned.top,
                                              aligned.right, aligned.bottom});
    }
  }
  const Leaf3TransientDamageSplit transientSplit = splitLeaf3TransientDamage(
      alignedDamage.data(), alignedDamage.size(),
      Leaf3PolicyRect{transient.left, transient.top, transient.right,
                      transient.bottom});
  const bool transientScrolling =
      !transientSplit.overflow && transientSplit.transientCount != 0;

  const uint64_t panelArea = rectArea(bounds);
  const bool large = panelArea != 0 && damageArea(damage) >= panelArea / 3;
  uint32_t waveform = kWaveformAuto;
  bool fast = false;
  bool readerPage = false;

  if (mode == "speed") {
    waveform = kWaveformDu;
    fast = true;
  } else if (mode == "a2") {
    waveform = kWaveformAnim;
    fast = true;
  } else if (mode == "regal") {
    if (large && settledQuality && !transientScrolling) {
      waveform = kWaveformAuto;
      mImpl->settledRegion = mImpl->settledRegion.isEmpty()
                                 ? rect
                                 : unionRects(mImpl->settledRegion, rect);
      mImpl->settledDeadline = now + kSettledDelay;
    } else {
      waveform = kWaveformRegal;
    }
  } else if (mode == "reader") {
    waveform = kWaveformDu;
    fast = true;
    readerPage = large && !transientScrolling;
    if (readerPage && cleanup != "manual") {
      const int interval = supportedPageInterval();
      ++mImpl->pageCount;
      if (interval > 0 && mImpl->pageCount >= interval) {
        waveform = kWaveformGc16;
        fast = false;
        mImpl->pageCount = 0;
        mImpl->clearGhostLocked(rect);
        if (!mImpl->hasGhostLocked()) {
          mImpl->cleanupSchedule.clear();
        }
        ++mImpl->pageCleanups;
      }
    }
  }

  if (fast && !readerPage && cleanup != "manual") {
    mImpl->markFastLocked(rect);
    mImpl->cleanupSchedule.noteActivity(now);
  }
  if (transientScrolling && waveform != kWaveformAnim && cleanup != "manual") {
    for (size_t index = 0; index < transientSplit.transientCount; ++index) {
      const Leaf3PolicyRect &moving = transientSplit.transient[index];
      mImpl->markFastLocked(
          Rect(moving.left, moving.top, moving.right, moving.bottom));
    }
    mImpl->cleanupSchedule.noteActivity(now);
  }
  mImpl->armTimerLocked();

  const uint32_t updateMode = waveform | ditherFlag();
  if (transientScrolling && waveform != kWaveformAnim) {
    std::vector<Leaf3EpdcUpdate> updates;
    updates.reserve(transientSplit.normalCount + transientSplit.transientCount);
    for (size_t index = 0; index < transientSplit.normalCount; ++index) {
      const Leaf3PolicyRect &normal = transientSplit.normal[index];
      updates.push_back(mImpl->makeUpdateLocked(
          Rect(normal.left, normal.top, normal.right, normal.bottom),
          updateMode));
    }
    for (size_t index = 0; index < transientSplit.transientCount; ++index) {
      const Leaf3PolicyRect &moving = transientSplit.transient[index];
      updates.push_back(mImpl->makeUpdateLocked(
          Rect(moving.left, moving.top, moving.right, moving.bottom),
          kWaveformAnim | ditherFlag()));
    }
    ++mImpl->transientUpdates;
    return updates;
  }
  size_t rectCount = 0;
  damage.getArray(&rectCount);
  if (rectCount == 0 || rectCount > kMaximumBatch) {
    return {mImpl->makeUpdateLocked(rect, updateMode)};
  }

  std::vector<Leaf3EpdcUpdate> updates;
  updates.reserve(rectCount);
  for (const Rect &damageRect : damage) {
    const Rect aligned = alignedRect(damageRect, bounds);
    if (!aligned.isEmpty()) {
      updates.push_back(mImpl->makeUpdateLocked(aligned, updateMode));
    }
  }
  return updates.empty() ? std::vector<Leaf3EpdcUpdate>{mImpl->makeUpdateLocked(
                               rect, updateMode)}
                         : updates;
}

bool Leaf3EpdcController::takeNotifierDamage(Rect *damage,
                                             Rect *transientHint) {
  if (damage == nullptr || transientHint == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mImpl->mutex);
  const int32_t activeUid =
      android::base::GetIntProperty(kActiveUidProperty, -1);
  if (mImpl->transientDeadline != 0 &&
      systemTime() < mImpl->transientDeadline &&
      leaf3TransientHintAuthorized(mImpl->transientUid, activeUid)) {
    *transientHint = mImpl->transientRegion;
  } else {
    *transientHint = Rect();
    mImpl->transientRegion = Rect();
    mImpl->transientDeadline = 0;
    mImpl->transientUid = -1;
  }
  if (mImpl->notifierDamage.isEmpty()) {
    return false;
  }
  *damage = mImpl->notifierDamage;
  mImpl->notifierDamage = Rect();
  return true;
}

void Leaf3EpdcController::setTransientHint(const Rect &region, nsecs_t duration,
                                           int32_t ownerUid) {
  std::lock_guard<std::mutex> lock(mImpl->mutex);
  if (!region.isValid() || region.isEmpty() || ownerUid < 0) {
    return;
  }
  const nsecs_t now = systemTime();
  mImpl->transientRegion = region;
  mImpl->transientDeadline =
      now + std::clamp<nsecs_t>(duration, 100000000, 2000000000);
  mImpl->transientUid = ownerUid;
}

void Leaf3EpdcController::clearTransientHint() {
  std::lock_guard<std::mutex> lock(mImpl->mutex);
  mImpl->transientRegion = Rect();
  mImpl->transientDeadline = 0;
  mImpl->transientUid = -1;
}

void Leaf3EpdcController::requestFullRefresh() {
  std::lock_guard<std::mutex> lock(mImpl->mutex);
  if (!mImpl->activeLocked()) {
    return;
  }
  mImpl->manualPending = true;
  mImpl->armTimerLocked();
}

bool Leaf3EpdcController::beginSubmission() {
  std::lock_guard<std::mutex> lock(mImpl->mutex);
  if (!mImpl->activeLocked()) {
    return false;
  }
  ++mImpl->submissionsInFlight;
  return true;
}

void Leaf3EpdcController::endSubmission() {
  std::lock_guard<std::mutex> lock(mImpl->mutex);
  if (mImpl->submissionsInFlight <= 0) {
    ALOGE("native EPDC submission accounting underflow");
    return;
  }
  --mImpl->submissionsInFlight;
  mImpl->lastSubmissionEnd = systemTime();
  mImpl->condition.notify_all();
}

void Leaf3EpdcController::blockAndWaitForIdle() {
  std::unique_lock<std::mutex> lock(mImpl->mutex);
  mImpl->blocked = true;
  mImpl->discardPendingWorkLocked();
  android::base::SetProperty(kBlockedProperty, "1");
  if (!mImpl->failed) {
    mImpl->publishState("blocked");
  }
  mImpl->condition.notify_all();
  mImpl->condition.wait(lock,
                        [this] { return mImpl->submissionsInFlight == 0; });
  const nsecs_t safeAt = mImpl->lastSubmissionEnd + kSubmissionDrainDelay;
  nsecs_t remaining = std::max<nsecs_t>(0, safeAt - systemTime());
  while (remaining > 0) {
    mImpl->condition.wait_for(lock, std::chrono::nanoseconds(remaining));
    remaining = std::max<nsecs_t>(0, safeAt - systemTime());
  }
}

void Leaf3EpdcController::composerFailed(const char *operation) {
  std::lock_guard<std::mutex> lock(mImpl->mutex);
  if (mImpl->failed) {
    return;
  }
  mImpl->failed = true;
  mImpl->discardPendingWorkLocked();
  ++mImpl->errors;
  mImpl->statsDirty = true;
  android::base::SetProperty(kBlockedProperty, "1");
  mImpl->publishState("failed");
  mImpl->condition.notify_all();
  ALOGE("composer-native EPDC disabled after %s failed", operation);
}

std::string Leaf3EpdcController::dump() const {
  std::lock_guard<std::mutex> lock(mImpl->mutex);
  std::ostringstream output;
  output << "Leaf3 composer EPDC: requested=" << mImpl->requested
         << " supported=" << mImpl->supported
         << " active=" << mImpl->activeLocked() << " failed=" << mImpl->failed
         << " blocked=" << mImpl->blocked << " commands=" << mImpl->commands
         << " pixels=" << mImpl->pixels << " cleanup=" << mImpl->cleanupUpdates
         << " page_cleanup=" << mImpl->pageCleanups
         << " settled=" << mImpl->settledUpdates
         << " transient=" << mImpl->transientUpdates
         << " full=" << mImpl->fullRefreshes << " errors=" << mImpl->errors
         << "\n";
  return output.str();
}

} // namespace android
