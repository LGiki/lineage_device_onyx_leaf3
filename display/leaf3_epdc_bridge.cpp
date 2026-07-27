/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * BOOX Leaf3 uses a dummy DSI connector as Android's primary display. The
 * actual E-Ink panel is updated through ONYX's private /dev/ebc interface.
 * Mirror SurfaceFlinger's primary layer stack into a virtual display and
 * forward the changed regions of every composed frame to EBC.
 */

#define LOG_TAG "leaf3_epdc_bridge"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <android-base/properties.h>
#include <binder/ProcessState.h>
#include <gui/BufferItem.h>
#include <gui/BufferItemConsumer.h>
#include <gui/BufferQueue.h>
#include <gui/IGraphicBufferConsumer.h>
#include <gui/IGraphicBufferProducer.h>
#include <gui/SurfaceComposerClient.h>
#include <log/log.h>
#include <ui/DisplayConfig.h>
#include <ui/DisplayState.h>
#include <ui/Fence.h>
#include <ui/GraphicBuffer.h>
#include <ui/PixelFormat.h>
#include <ui/Rect.h>
#include <ui/Rotation.h>
#include <utils/Errors.h>
#include <utils/String8.h>

namespace {

using android::BufferItem;
using android::BufferItemConsumer;
using android::BufferQueue;
using android::DisplayConfig;
using android::Fence;
using android::GraphicBuffer;
using android::IBinder;
using android::IGraphicBufferConsumer;
using android::IGraphicBufferProducer;
using android::NO_ERROR;
using android::PhysicalDisplayId;
using android::ProcessState;
using android::Rect;
using android::ScreenshotClient;
using android::sp;
using android::status_t;
using android::String8;
using android::SurfaceComposerClient;
using android::ui::Dataspace;

constexpr char kEbcDevice[] = "/dev/ebc";
constexpr char kOnyxBrightness[] = "/sys/class/backlight/onyx_bl_br/brightness";
constexpr char kOnyxTemperature[] =
    "/sys/class/backlight/onyx_bl_ct/brightness";
constexpr int kAndroidBrightnessMax = 255;
constexpr int kAndroidBrightnessMin = 10;
constexpr int kOnyxBrightnessMax = 28;
constexpr int kOnyxTemperatureMax = 24;
constexpr char kFrontlightEnabledProperty[] =
    "persist.sys.leaf3.frontlight_enabled";
constexpr char kFrontlightBrightnessOverrideProperty[] =
    "persist.sys.leaf3.frontlight_brightness";
constexpr char kFrontlightTemperatureProperty[] =
    "persist.sys.leaf3.frontlight_temperature";
constexpr char kInteractiveProperty[] = "sys.leaf3.interactive";
constexpr char kAndroidBrightnessProperty[] = "sys.leaf3.android_brightness";
constexpr unsigned long kEbcGetBufferInfo = 0x7003;
constexpr unsigned long kEbcSendUpdate = 0x700c;
constexpr int32_t kEbcAutoTemperature = 0x1000;
constexpr uint32_t kEbcDitherFlag = 0x20000;
// Values recovered from the stock ONYX framework's ViewUpdateHelper.
constexpr uint32_t kWaveformDu = 1;
constexpr uint32_t kWaveformGc16 = 2;
constexpr uint32_t kWaveformAnim = 4;
constexpr uint32_t kWaveformAuto = 5;
constexpr uint32_t kWaveformRegal = 6;
constexpr uint32_t kUpdatePartial = 0;
constexpr uint32_t kUpdateFull = 1;

// Frames arrive from the compositor, so these delays only pace the polling
// fallback used when the virtual display cannot be created.
constexpr useconds_t kFastFrameDelayUs = 80000;
constexpr useconds_t kBalancedFrameDelayUs = 100000;
constexpr useconds_t kQualityFrameDelayUs = 140000;
// The vendor EPDC path becomes unstable when a compositor-rate stream queues
// updates faster than the two internal EBC buffers can drain. This is an
// absolute ioctl-to-ioctl limit; polling and virtual-display capture both use
// it, and split rectangles are serialized through it.
constexpr int64_t kMinimumEbcUpdateIntervalUs = 100000;
constexpr useconds_t kIdleFrameDelayUs = 500000;
constexpr useconds_t kIdleOneSecondDelayUs = 1000000;
constexpr useconds_t kIdleTwoSecondDelayUs = 2000000;
constexpr useconds_t kIdleFiveSecondDelayUs = 5000000;
constexpr useconds_t kTouchSettleDelayUs = 32000;
constexpr useconds_t kRetryDelayUs = 1000000;
constexpr uint32_t kIdleThresholdFrames = 10;
constexpr uint32_t kInputProbeFrames = 6;

// A fast waveform leaves the panel dirty. Rather than counting unchanged
// capture cycles, wait for the compositor to fall quiet for this long and then
// submit one GC16 cleanup.
constexpr int64_t kBalancedCleanupDelayUs = 600000;
constexpr int64_t kQualityCleanupDelayUs = 300000;

constexpr int64_t kStatsPublishIntervalUs = 60000000;
constexpr int64_t kStreamStartupTimeoutUs = 3000000;
// ONYX changes waveform as soon as a drag crosses Android's touch slop. Keep
// the raw-input gesture active briefly after the last movement so a released
// fling stays fast, and allow row hashes to extend an established fling.
constexpr int64_t kScrollGestureWindowUs = 500000;
constexpr int64_t kScrollFlingWindowUs = 1500000;

constexpr char kRefreshModeProperty[] = "persist.sys.leaf3.refresh_mode";
constexpr char kActiveRefreshModeProperty[] = "sys.leaf3.active_refresh_mode";
constexpr char kFullRefreshProperty[] = "sys.leaf3.full_refresh";
constexpr char kClearOnSleepProperty[] = "persist.sys.leaf3.clear_on_sleep";
constexpr char kIdlePolicyProperty[] = "persist.sys.leaf3.idle_policy";
constexpr char kCleanupPolicyProperty[] = "persist.sys.leaf3.cleanup_policy";
constexpr char kContentAwareProperty[] = "persist.sys.leaf3.content_aware";
constexpr char kScrollDetectProperty[] = "persist.sys.leaf3.scroll_detect";
constexpr char kCaptureModeProperty[] = "persist.sys.leaf3.capture_mode";
constexpr char kTouchInputName[] = "cyttsp5_mt";

// Damage is tracked on a tile grid. 32 pixels keeps every rectangle edge
// 8-aligned, which is what the EPDC prefers, without tracking so many tiles
// that coalescing becomes the expensive part.
constexpr uint32_t kTileSize = 32;
// The Leaf3 vendor driver reports two internal EBC buffers. Never queue more
// regional updates from one compositor frame than the driver can retain.
constexpr size_t kMaxUpdateRects = 1;
constexpr size_t kMaxCoalesceRects = 64;
// Splitting only pays off while the rectangles stay much smaller than the
// bounding box they replace.
constexpr uint64_t kSplitBenefitNumerator = 3;
constexpr uint64_t kSplitBenefitDenominator = 4;

// A region whose sampled luminance is this heavily concentrated at black and
// white is text or flat UI rather than an image.
constexpr uint32_t kLuminanceBins = 16;
constexpr uint32_t kBiLevelPercent = 90;
constexpr uint32_t kSignificantBinPercent = 2;
constexpr uint32_t kClassifySamplesPerAxis = 64;

constexpr uint32_t kScrollProbeRows = 16;
constexpr uint32_t kScrollMinVotes = 4;
constexpr int32_t kScrollMinShift = 8;

struct EbcBufferInfo {
  uint32_t words[14];
};

struct EbcUpdate {
  // The driver embeds an mxcfb_rect, whose ABI order is top then left.
  // Reversing these works for full-screen updates but rejects vertical scroll
  // damage with out-of-range x coordinates.
  uint32_t top;
  uint32_t left;
  uint32_t width;
  uint32_t height;
  uint32_t waveform_mode;
  uint32_t update_mode;
  uint32_t update_marker;
  int32_t temperature;
  uint32_t flags;
};

struct ChangedRect {
  uint32_t left;
  uint32_t top;
  uint32_t right;
  uint32_t bottom;

  uint64_t area() const {
    return static_cast<uint64_t>(right - left) * (bottom - top);
  }
};

enum class RefreshMode {
  kBalanced,
  kNormal,
  kSpeed,
  kA2,
  kRegal,
};

enum class IdlePolicy {
  kResponsive,
  kBalanced,
  kBattery,
};

enum class CleanupPolicy {
  kQuality,
  kBalanced,
  kManual,
};

enum class ContentClass {
  kBiLevel,
  kGrayscale,
};

struct Settings {
  RefreshMode mode = RefreshMode::kBalanced;
  IdlePolicy idle = IdlePolicy::kBalanced;
  CleanupPolicy cleanup = CleanupPolicy::kBalanced;
  bool content_aware = false;
  bool scroll_detect = true;
  bool clear_on_sleep = true;
  bool interactive = true;
  bool prefer_stream = false;
  bool frontlight_enabled = true;
  int android_brightness = 128;
  int frontlight_override = -1;
  int frontlight_temperature = 0;
  std::string full_refresh_token;
};

struct BridgeStats {
  uint64_t captures = 0;
  uint64_t comparisons = 0;
  uint64_t changed_frames = 0;
  uint64_t partial_updates = 0;
  uint64_t full_updates = 0;
  uint64_t updated_pixels = 0;
  uint64_t dropped_frames = 0;
  uint64_t split_frames = 0;
  uint64_t bilevel_updates = 0;
  uint64_t scroll_frames = 0;
  uint64_t gesture_scroll_frames = 0;
  uint64_t hash_scroll_frames = 0;
  uint64_t capture_time_us = 0;
  uint64_t compare_time_us = 0;
  uint64_t submit_time_us = 0;
  int64_t last_publish_us = 0;
};

const char *refreshModeName(RefreshMode mode) {
  switch (mode) {
  case RefreshMode::kNormal:
    return "normal";
  case RefreshMode::kSpeed:
    return "speed";
  case RefreshMode::kA2:
    return "a2";
  case RefreshMode::kRegal:
    return "regal";
  case RefreshMode::kBalanced:
  default:
    return "balanced";
  }
}

const char *propertyValue(const char *value, const char *default_value = "") {
  return value == nullptr || value[0] == '\0' ? default_value : value;
}

int parseInteger(const char *value, int default_value) {
  if (value == nullptr || value[0] == '\0') {
    return default_value;
  }

  char *end = nullptr;
  const long parsed = strtol(value, &end, 10);
  return end == value ? default_value : static_cast<int>(parsed);
}

bool readIntegerFile(const char *path, int *value) {
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }

  char buffer[32] = {};
  const ssize_t length = read(fd, buffer, sizeof(buffer) - 1);
  const int saved_errno = errno;
  close(fd);
  if (length <= 0) {
    errno = saved_errno;
    return false;
  }

  char *end = nullptr;
  const long parsed = strtol(buffer, &end, 10);
  if (end == buffer) {
    errno = EINVAL;
    return false;
  }
  *value = static_cast<int>(parsed);
  return true;
}

bool writeIntegerFile(const char *path, int value) {
  const int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }

  char buffer[32] = {};
  const int length = snprintf(buffer, sizeof(buffer), "%d", value);
  const ssize_t written = write(fd, buffer, static_cast<size_t>(length));
  const int saved_errno = errno;
  close(fd);
  if (written != length) {
    errno = saved_errno;
    return false;
  }
  return true;
}

int64_t monotonicMicros() {
  timespec now = {};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return 0;
  }
  return static_cast<int64_t>(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
}

class CachedStringProperty {
public:
  explicit CachedStringProperty(const char *name) : name_(name) {}

  const char *get(bool *changed) {
    *changed = false;
    if (info_ == nullptr) {
      const uint32_t area_serial = __system_property_area_serial();
      if (area_serial != area_serial_) {
        area_serial_ = area_serial;
        info_ = __system_property_find(name_);
      }
    }
    if (info_ == nullptr) {
      return value_.c_str();
    }

    const uint32_t serial = __system_property_serial(info_);
    if (serial != serial_) {
      __system_property_read_callback(
          info_,
          [](void *cookie, const char *, const char *value, uint32_t serial) {
            auto *property = static_cast<CachedStringProperty *>(cookie);
            property->value_ = value;
            property->serial_ = serial;
          },
          this);
      *changed = true;
    }
    return value_.c_str();
  }

private:
  const char *name_;
  const prop_info *info_ = nullptr;
  uint32_t area_serial_ = 0;
  uint32_t serial_ = 0;
  std::string value_;
};

class SettingsCache {
public:
  const Settings &read() {
    bool changed = !initialized_;
    const char *active_mode = read(active_mode_, &changed);
    const char *global_mode = read(global_mode_, &changed);
    const char *idle = read(idle_, &changed);
    const char *cleanup = read(cleanup_, &changed);
    const char *content_aware = read(content_aware_, &changed);
    const char *scroll_detect = read(scroll_detect_, &changed);
    const char *clear_on_sleep = read(clear_on_sleep_, &changed);
    const char *interactive = read(interactive_, &changed);
    const char *capture_mode = read(capture_mode_, &changed);
    const char *frontlight_enabled = read(frontlight_enabled_, &changed);
    const char *android_brightness = read(android_brightness_, &changed);
    const char *frontlight_override = read(frontlight_override_, &changed);
    const char *frontlight_temperature =
        read(frontlight_temperature_, &changed);
    const char *full_refresh = read(full_refresh_, &changed);

    if (!changed) {
      return settings_;
    }
    initialized_ = true;

    const std::string mode = propertyValue(active_mode)[0] == '\0'
                                 ? propertyValue(global_mode, "balanced")
                                 : propertyValue(active_mode);
    if (mode == "normal") {
      settings_.mode = RefreshMode::kNormal;
    } else if (mode == "speed" || mode == "du") {
      settings_.mode = RefreshMode::kSpeed;
    } else if (mode == "a2") {
      settings_.mode = RefreshMode::kA2;
    } else if (mode == "regal") {
      settings_.mode = RefreshMode::kRegal;
    } else {
      settings_.mode = RefreshMode::kBalanced;
    }

    const std::string idle_value = propertyValue(idle, "balanced");
    if (idle_value == "responsive") {
      settings_.idle = IdlePolicy::kResponsive;
    } else if (idle_value == "battery") {
      settings_.idle = IdlePolicy::kBattery;
    } else {
      settings_.idle = IdlePolicy::kBalanced;
    }

    const std::string cleanup_value = propertyValue(cleanup, "balanced");
    if (cleanup_value == "quality") {
      settings_.cleanup = CleanupPolicy::kQuality;
    } else if (cleanup_value == "manual") {
      settings_.cleanup = CleanupPolicy::kManual;
    } else {
      settings_.cleanup = CleanupPolicy::kBalanced;
    }

    // Content-aware GC16 caused repeated large update passes on the preserved
    // vendor EBC stack. Keep the implementation for later composer-native
    // work, but quarantine it in the production bridge.
    (void)content_aware;
    settings_.content_aware = false;
    settings_.scroll_detect = parseInteger(scroll_detect, 1) != 0;
    settings_.clear_on_sleep = parseInteger(clear_on_sleep, 1) != 0;
    settings_.interactive = parseInteger(interactive, 1) != 0;
    // The Qualcomm/ONYX display stack becomes unstable when its virtual
    // display and private EBC path are active together under composition load.
    // Keep the stream implementation for controlled probing, but do not select
    // it in production even if an older data partition persisted "stream".
    (void)capture_mode;
    settings_.prefer_stream = false;
    settings_.frontlight_enabled = parseInteger(frontlight_enabled, 1) != 0;
    settings_.android_brightness = std::clamp(
        parseInteger(android_brightness, 128), 0, kAndroidBrightnessMax);
    settings_.frontlight_override =
        std::clamp(parseInteger(frontlight_override, -1), -1, 100);
    settings_.frontlight_temperature =
        std::clamp(parseInteger(frontlight_temperature, 0), 0, 100);
    settings_.full_refresh_token = propertyValue(full_refresh);
    return settings_;
  }

private:
  static const char *read(CachedStringProperty &property, bool *changed) {
    bool property_changed = false;
    const char *value = property.get(&property_changed);
    *changed |= property_changed;
    return value;
  }

  CachedStringProperty active_mode_{kActiveRefreshModeProperty};
  CachedStringProperty global_mode_{kRefreshModeProperty};
  CachedStringProperty idle_{kIdlePolicyProperty};
  CachedStringProperty cleanup_{kCleanupPolicyProperty};
  CachedStringProperty content_aware_{kContentAwareProperty};
  CachedStringProperty scroll_detect_{kScrollDetectProperty};
  CachedStringProperty clear_on_sleep_{kClearOnSleepProperty};
  CachedStringProperty interactive_{kInteractiveProperty};
  CachedStringProperty capture_mode_{kCaptureModeProperty};
  CachedStringProperty frontlight_enabled_{kFrontlightEnabledProperty};
  CachedStringProperty android_brightness_{kAndroidBrightnessProperty};
  CachedStringProperty frontlight_override_{
      kFrontlightBrightnessOverrideProperty};
  CachedStringProperty frontlight_temperature_{kFrontlightTemperatureProperty};
  CachedStringProperty full_refresh_{kFullRefreshProperty};
  Settings settings_;
  bool initialized_ = false;
};

int mapAndroidBrightness(int android_brightness) {
  if (android_brightness <= 0) {
    return 0;
  }

  // Android's brightness slider is in gamma space, but the light HAL receives
  // a linear 10-255 value. Undo BrightnessUtils.convertGammaToLinear() before
  // mapping the slider position to ONYX's 28 perceptual hardware steps.
  constexpr float kBrightnessRampA = 0.17883277f;
  constexpr float kBrightnessRampB = 0.28466892f;
  constexpr float kBrightnessRampC = 0.55991073f;
  constexpr float kBrightnessRampR = 0.5f;

  const float linear =
      static_cast<float>(std::clamp(android_brightness, kAndroidBrightnessMin,
                                    kAndroidBrightnessMax) -
                         kAndroidBrightnessMin) /
      (kAndroidBrightnessMax - kAndroidBrightnessMin);
  const float expanded = linear * 12.0f;
  const float gamma =
      expanded <= 1.0f ? sqrtf(expanded) * kBrightnessRampR
                       : kBrightnessRampA * logf(expanded - kBrightnessRampB) +
                             kBrightnessRampC;
  return 1 + static_cast<int>(lroundf(std::clamp(gamma, 0.0f, 1.0f) *
                                      (kOnyxBrightnessMax - 1)));
}

class FrontlightBridge {
public:
  void update(const Settings &settings, bool display_on) {
    int target_brightness =
        display_on ? mapAndroidBrightness(settings.android_brightness) : 0;
    if (settings.frontlight_override >= 0 && display_on) {
      target_brightness =
          settings.frontlight_override == 0
              ? 0
              : 1 + (settings.frontlight_override * (kOnyxBrightnessMax - 1) +
                     50) /
                        100;
    }
    if (!settings.frontlight_enabled) {
      target_brightness = 0;
    }

    const int target_temperature =
        (settings.frontlight_temperature * kOnyxTemperatureMax + 50) / 100;

    apply(kOnyxTemperature, target_temperature, &last_temperature_,
          "temperature");
    apply(kOnyxBrightness, target_brightness, &last_brightness_, "brightness");
  }

private:
  void apply(const char *path, int target, int *last_target,
             const char *description) {
    int current = -1;
    if (target == *last_target && readIntegerFile(path, &current) &&
        current == target) {
      return;
    }
    if (!writeIntegerFile(path, target)) {
      ALOGE("cannot set frontlight %s to %d through %s: %s", description,
            target, path, strerror(errno));
      return;
    }
    *last_target = target;
    ALOGI("frontlight %s=%d", description, target);
  }

  int last_brightness_ = -1;
  int last_temperature_ = -1;
};

// Collects the reasons the main loop may need to run again. Without a pending
// reason the loop blocks indefinitely, so a still panel costs no wakeups.
class Waiter {
public:
  static constexpr uint32_t kFrame = 1u << 0;
  static constexpr uint32_t kInput = 1u << 1;
  static constexpr uint32_t kProperty = 1u << 2;

  void signal(uint32_t reason) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_ |= reason;
    }
    condition_.notify_all();
  }

  uint32_t wait(int64_t timeout_us) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (pending_ == 0) {
      if (timeout_us < 0) {
        condition_.wait(lock, [this] { return pending_ != 0; });
      } else {
        condition_.wait_for(lock, std::chrono::microseconds(timeout_us),
                            [this] { return pending_ != 0; });
      }
    }
    const uint32_t reasons = pending_;
    pending_ = 0;
    return reasons;
  }

  void drain() {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_ = 0;
  }

private:
  std::mutex mutex_;
  std::condition_variable condition_;
  uint32_t pending_ = 0;
};

class InputWakeMonitor {
public:
  ~InputWakeMonitor() { reset(); }

  void reset() {
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
    slots_.clear();
    current_slot_ = 0;
    slot_minimum_ = 0;
    x_range_ = 0;
    y_range_ = 0;
    gesture_supported_ = false;
  }

  bool init() {
    reset();
    DIR *directory = opendir("/dev/input");
    if (directory == nullptr) {
      ALOGW("cannot open /dev/input: %s", strerror(errno));
      return false;
    }

    for (dirent *entry = readdir(directory); entry != nullptr;
         entry = readdir(directory)) {
      if (strncmp(entry->d_name, "event", 5) != 0) {
        continue;
      }

      char path[64] = {};
      const int length =
          snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
      if (length <= 0 || static_cast<size_t>(length) >= sizeof(path)) {
        continue;
      }

      const int candidate = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
      if (candidate < 0) {
        continue;
      }

      char name[128] = {};
      if (ioctl(candidate, EVIOCGNAME(sizeof(name)), name) >= 0 &&
          strcmp(name, kTouchInputName) == 0) {
        fd_ = candidate;
        initGestureTracking();
        ALOGI("touch wake-up enabled on %s (%s), gesture tracking %s", path,
              name, gesture_supported_ ? "enabled" : "unavailable");
        break;
      }
      close(candidate);
    }
    closedir(directory);

    return fd_ >= 0;
  }

  // A negative timeout blocks until touch activity arrives.
  bool wait(int64_t timeout_us, bool *scroll_motion,
            bool *dragging_contact_active) {
    *scroll_motion = false;
    *dragging_contact_active = false;
    if (fd_ < 0) {
      if (timeout_us > 0) {
        usleep(static_cast<useconds_t>(timeout_us));
      }
      return false;
    }

    pollfd descriptor = {fd_, POLLIN, 0};
    const int timeout_ms =
        timeout_us < 0 ? -1 : static_cast<int>((timeout_us + 999) / 1000);
    const int result = poll(&descriptor, 1, timeout_ms);
    if (result <= 0 || !(descriptor.revents & POLLIN)) {
      return false;
    }

    input_event events[64];
    for (;;) {
      const ssize_t bytes = read(fd_, events, sizeof(events));
      if (bytes <= 0) {
        break;
      }
      const size_t count = static_cast<size_t>(bytes) / sizeof(input_event);
      for (size_t i = 0; i < count; ++i) {
        *scroll_motion |= processEvent(events[i]);
      }
    }
    *dragging_contact_active = hasDraggingContact();
    return true;
  }

private:
  struct Contact {
    bool active = false;
    bool have_x = false;
    bool have_y = false;
    bool have_origin = false;
    bool dragging = false;
    int32_t x = 0;
    int32_t y = 0;
    int32_t origin_x = 0;
    int32_t origin_y = 0;
    int32_t reported_x = 0;
    int32_t reported_y = 0;
  };

  void initGestureTracking() {
    input_absinfo slot_info = {};
    input_absinfo x_info = {};
    input_absinfo y_info = {};
    if (ioctl(fd_, EVIOCGABS(ABS_MT_POSITION_X), &x_info) < 0 ||
        ioctl(fd_, EVIOCGABS(ABS_MT_POSITION_Y), &y_info) < 0) {
      return;
    }

    size_t slot_count = 1;
    if (ioctl(fd_, EVIOCGABS(ABS_MT_SLOT), &slot_info) == 0) {
      const int64_t reported_slots =
          static_cast<int64_t>(slot_info.maximum) - slot_info.minimum + 1;
      if (reported_slots <= 0 || reported_slots > kMaximumTouchSlots) {
        return;
      }
      slot_count = static_cast<size_t>(reported_slots);
      slot_minimum_ = slot_info.minimum;
      current_slot_ = std::clamp(slot_info.value - slot_info.minimum, 0,
                                 static_cast<int32_t>(slot_count - 1));
    }

    x_range_ = static_cast<int64_t>(x_info.maximum) - x_info.minimum;
    y_range_ = static_cast<int64_t>(y_info.maximum) - y_info.minimum;
    if (x_range_ <= 0 || y_range_ <= 0) {
      return;
    }
    slots_.resize(slot_count);
    gesture_supported_ = true;
    ALOGI("touch gesture axes x_range=%lld y_range=%lld slots=%zu",
          static_cast<long long>(x_range_), static_cast<long long>(y_range_),
          slot_count);
  }

  bool processEvent(const input_event &event) {
    if (!gesture_supported_) {
      return false;
    }
    if (event.type == EV_ABS) {
      if (event.code == ABS_MT_SLOT) {
        const int32_t slot = event.value - slot_minimum_;
        if (slot >= 0 && static_cast<size_t>(slot) < slots_.size()) {
          current_slot_ = slot;
        } else {
          current_slot_ = -1;
        }
        return false;
      }

      if (current_slot_ < 0 ||
          static_cast<size_t>(current_slot_) >= slots_.size()) {
        return false;
      }
      Contact &contact = slots_[current_slot_];
      switch (event.code) {
      case ABS_MT_TRACKING_ID:
        if (event.value < 0) {
          contact = {};
        } else {
          contact = {};
          contact.active = true;
        }
        break;
      case ABS_MT_POSITION_X:
        contact.x = event.value;
        contact.have_x = true;
        break;
      case ABS_MT_POSITION_Y:
        contact.y = event.value;
        contact.have_y = true;
        break;
      default:
        break;
      }
      return false;
    }
    if (event.type != EV_SYN || event.code != SYN_REPORT) {
      return false;
    }

    bool scroll_motion = false;
    for (Contact &contact : slots_) {
      if (!contact.active || !contact.have_x || !contact.have_y) {
        continue;
      }
      if (!contact.have_origin) {
        contact.origin_x = contact.x;
        contact.origin_y = contact.y;
        contact.reported_x = contact.x;
        contact.reported_y = contact.y;
        contact.have_origin = true;
        continue;
      }

      const int64_t delta_x =
          std::abs(static_cast<int64_t>(contact.x) - contact.origin_x);
      const int64_t delta_y =
          std::abs(static_cast<int64_t>(contact.y) - contact.origin_y);
      contact.dragging |=
          delta_x * 100 >= x_range_ || delta_y * 100 >= y_range_;
      if (contact.dragging && (contact.x != contact.reported_x ||
                               contact.y != contact.reported_y)) {
        scroll_motion = true;
      }
      contact.reported_x = contact.x;
      contact.reported_y = contact.y;
    }
    return scroll_motion;
  }

  bool hasDraggingContact() const {
    for (const Contact &contact : slots_) {
      if (contact.active && contact.dragging) {
        return true;
      }
    }
    return false;
  }

  static constexpr int64_t kMaximumTouchSlots = 32;

  int fd_ = -1;
  std::vector<Contact> slots_;
  int32_t current_slot_ = 0;
  int32_t slot_minimum_ = 0;
  int64_t x_range_ = 0;
  int64_t y_range_ = 0;
  bool gesture_supported_ = false;
};

struct Frame {
  const uint8_t *pixels = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
  int32_t format = 0;
};

class FrameSource {
public:
  virtual ~FrameSource() = default;
  virtual bool start() = 0;
  virtual void stop() = 0;
  // Returns true and fills |frame| when a newly composed frame is ready. The
  // pixels stay mapped until release().
  virtual bool acquire(Frame *frame) = 0;
  virtual void release() = 0;
  virtual bool streaming() const = 0;
  virtual uint64_t dropped() const { return 0; }
};

// Mirrors the primary layer stack into a virtual display. SurfaceFlinger
// queues a buffer only when it actually recomposes, so there is nothing to
// poll and no wasted readback of an unchanged screen.
class MirrorFrameSource : public FrameSource {
public:
  explicit MirrorFrameSource(Waiter *waiter) : waiter_(waiter) {}
  ~MirrorFrameSource() override { stop(); }

  bool start() override {
    const sp<IBinder> primary =
        SurfaceComposerClient::getInternalDisplayToken();
    if (primary == nullptr) {
      ALOGW("no internal display token for mirroring");
      return false;
    }

    DisplayConfig config;
    if (SurfaceComposerClient::getActiveDisplayConfig(primary, &config) !=
        NO_ERROR) {
      ALOGW("cannot read the active primary display config");
      return false;
    }
    android::ui::DisplayState state;
    if (SurfaceComposerClient::getDisplayState(primary, &state) != NO_ERROR) {
      ALOGW("cannot read the primary display state");
      return false;
    }

    width_ = static_cast<uint32_t>(config.resolution.getWidth());
    height_ = static_cast<uint32_t>(config.resolution.getHeight());
    if (width_ == 0 || height_ == 0) {
      ALOGW("primary display reports an empty resolution");
      return false;
    }

    BufferQueue::createBufferQueue(&producer_, &consumer_);
    item_consumer_ = new BufferItemConsumer(
        consumer_, GraphicBuffer::USAGE_SW_READ_OFTEN, kBufferCount, false);
    item_consumer_->setName(String8(LOG_TAG));
    item_consumer_->setDefaultBufferSize(width_, height_);
    item_consumer_->setDefaultBufferFormat(android::PIXEL_FORMAT_RGBA_8888);
    listener_ = new Listener(waiter_);
    item_consumer_->setFrameAvailableListener(listener_);

    token_ = SurfaceComposerClient::createDisplay(String8(LOG_TAG),
                                                  /*secure=*/false);
    if (token_ == nullptr) {
      ALOGW("SurfaceFlinger refused to create the mirror display");
      stop();
      return false;
    }

    SurfaceComposerClient::Transaction transaction;
    transaction.setDisplaySurface(token_, producer_);
    transaction.setDisplayLayerStack(token_, state.layerStack);
    transaction.setDisplayProjection(token_, state.orientation,
                                     Rect(state.viewport),
                                     Rect(config.resolution));
    const status_t transaction_status = transaction.apply(true);
    if (transaction_status != NO_ERROR) {
      ALOGW("cannot configure the mirror display: %d", transaction_status);
      stop();
      return false;
    }

    ALOGI("mirroring layer stack %u into a %ux%u virtual display",
          state.layerStack, width_, height_);
    return true;
  }

  void stop() override {
    release();
    if (token_ != nullptr) {
      SurfaceComposerClient::destroyDisplay(token_);
      token_.clear();
    }
    if (item_consumer_ != nullptr) {
      item_consumer_->setFrameAvailableListener(nullptr);
      item_consumer_->abandon();
      item_consumer_.clear();
    }
    listener_.clear();
    for (sp<GraphicBuffer> &slot : slots_) {
      slot.clear();
    }
    producer_.clear();
    consumer_.clear();
  }

  bool acquire(Frame *frame) override {
    if (item_consumer_ == nullptr) {
      return false;
    }
    if (held_buffer_ != nullptr) {
      fillFrame(frame);
      return true;
    }

    // Compositor frames can outpace the panel by a wide margin. Only the most
    // recent one describes what the user should be looking at.
    BufferItem latest;
    bool have_latest = false;
    for (;;) {
      BufferItem item;
      const status_t status = item_consumer_->acquireBuffer(&item, 0, true);
      if (status == BufferQueue::NO_BUFFER_AVAILABLE) {
        break;
      }
      if (status != NO_ERROR) {
        ALOGW("mirror acquireBuffer failed: %d", status);
        break;
      }
      if (item.mSlot < 0 ||
          item.mSlot >= static_cast<int>(BufferQueue::NUM_BUFFER_SLOTS)) {
        ALOGW("mirror returned an out-of-range slot %d", item.mSlot);
        item_consumer_->releaseBuffer(item, Fence::NO_FENCE);
        continue;
      }
      if (item.mGraphicBuffer != nullptr) {
        slots_[item.mSlot] = item.mGraphicBuffer;
      }
      if (have_latest) {
        item_consumer_->releaseBuffer(latest, Fence::NO_FENCE);
        ++dropped_;
      }
      latest = item;
      have_latest = true;
    }

    if (!have_latest) {
      return false;
    }

    const sp<GraphicBuffer> buffer = slots_[latest.mSlot];
    if (buffer == nullptr) {
      ALOGW("mirror slot %d has no buffer", latest.mSlot);
      item_consumer_->releaseBuffer(latest, Fence::NO_FENCE);
      return false;
    }
    const int32_t format = buffer->getPixelFormat();
    if (format != android::PIXEL_FORMAT_RGBA_8888 &&
        format != android::PIXEL_FORMAT_RGBX_8888) {
      ALOGW("unsupported mirror format %d", format);
      item_consumer_->releaseBuffer(latest, Fence::NO_FENCE);
      return false;
    }

    void *pixels = nullptr;
    if (buffer->lock(GraphicBuffer::USAGE_SW_READ_OFTEN, &pixels) != NO_ERROR ||
        pixels == nullptr) {
      ALOGW("mirror buffer lock failed");
      item_consumer_->releaseBuffer(latest, Fence::NO_FENCE);
      return false;
    }

    held_ = latest;
    held_buffer_ = buffer;
    held_pixels_ = static_cast<const uint8_t *>(pixels);
    fillFrame(frame);
    return true;
  }

  void release() override {
    if (held_buffer_ == nullptr) {
      return;
    }
    held_buffer_->unlock();
    held_pixels_ = nullptr;
    held_buffer_.clear();
    if (item_consumer_ != nullptr) {
      item_consumer_->releaseBuffer(held_, Fence::NO_FENCE);
    }
  }

  bool streaming() const override { return true; }
  uint64_t dropped() const override { return dropped_; }

private:
  void fillFrame(Frame *frame) const {
    frame->pixels = held_pixels_;
    frame->width = held_buffer_->getWidth();
    frame->height = held_buffer_->getHeight();
    frame->stride = held_buffer_->getStride();
    frame->format = held_buffer_->getPixelFormat();
  }

  // The drain loop holds at most two buffers and releases stale frames as soon
  // as a newer one is available.
  static constexpr int kBufferCount = 2;

  class Listener : public BufferItemConsumer::FrameAvailableListener {
  public:
    explicit Listener(Waiter *waiter) : waiter_(waiter) {}
    void onFrameAvailable(const BufferItem &) override {
      waiter_->signal(Waiter::kFrame);
    }

  private:
    Waiter *waiter_;
  };

  Waiter *waiter_;
  sp<IGraphicBufferProducer> producer_;
  sp<IGraphicBufferConsumer> consumer_;
  sp<BufferItemConsumer> item_consumer_;
  sp<Listener> listener_;
  sp<IBinder> token_;
  sp<GraphicBuffer> slots_[BufferQueue::NUM_BUFFER_SLOTS];
  sp<GraphicBuffer> held_buffer_;
  const uint8_t *held_pixels_ = nullptr;
  BufferItem held_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint64_t dropped_ = 0;
};

// Retained so a device whose compositor refuses the virtual display still
// shows something. The caller paces this source itself.
class ScreenshotFrameSource : public FrameSource {
public:
  bool start() override {
    while (!(display_id_ = SurfaceComposerClient::getInternalDisplayId())) {
      ALOGW("SurfaceFlinger internal display is not ready");
      usleep(kRetryDelayUs);
    }
    return true;
  }

  void stop() override { release(); }

  bool acquire(Frame *frame) override {
    if (!display_id_) {
      return false;
    }

    Dataspace dataspace;
    sp<GraphicBuffer> buffer;
    const status_t status =
        ScreenshotClient::capture(*display_id_, &dataspace, &buffer);
    if (status != NO_ERROR || buffer == nullptr) {
      ALOGW("display capture failed: %d", status);
      return false;
    }
    const int32_t format = buffer->getPixelFormat();
    if (format != android::PIXEL_FORMAT_RGBA_8888 &&
        format != android::PIXEL_FORMAT_RGBX_8888) {
      ALOGW("unsupported screenshot format %d", format);
      return false;
    }

    void *pixels = nullptr;
    if (buffer->lock(GraphicBuffer::USAGE_SW_READ_OFTEN, &pixels) != NO_ERROR ||
        pixels == nullptr) {
      ALOGW("capture buffer lock failed");
      return false;
    }

    held_ = buffer;
    frame->pixels = static_cast<const uint8_t *>(pixels);
    frame->width = buffer->getWidth();
    frame->height = buffer->getHeight();
    frame->stride = buffer->getStride();
    frame->format = buffer->getPixelFormat();
    return true;
  }

  void release() override {
    if (held_ != nullptr) {
      held_->unlock();
      held_.clear();
    }
  }

  bool streaming() const override { return false; }

private:
  std::optional<PhysicalDisplayId> display_id_;
  sp<GraphicBuffer> held_;
};

class EbcDevice {
public:
  ~EbcDevice() {
    if (buffer_ != MAP_FAILED) {
      munmap(buffer_, buffer_size_);
    }
    if (fd_ >= 0) {
      close(fd_);
    }
  }

  bool init(uint32_t width, uint32_t height) {
    fd_ = open(kEbcDevice, O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
      ALOGE("open(%s) failed: %s", kEbcDevice, strerror(errno));
      return false;
    }

    EbcBufferInfo info = {};
    const int info_size = ioctl(fd_, kEbcGetBufferInfo, nullptr);
    if (info_size != 0 && info_size != static_cast<int>(sizeof(info))) {
      ALOGE("unexpected EBC buffer-info size %d (expected %zu)", info_size,
            sizeof(info));
      return false;
    }

    // The ONYX driver uses words 6 and 7 as the requested/output geometry.
    info.words[6] = width;
    info.words[7] = height;
    if (ioctl(fd_, kEbcGetBufferInfo, &info) < 0) {
      ALOGE("EBC GET_BUFFER_INFO failed: %s", strerror(errno));
      return false;
    }
    if (info.words[6] != width || info.words[7] != height) {
      ALOGE("EBC geometry %ux%u does not match SurfaceFlinger %ux%u",
            info.words[6], info.words[7], width, height);
      return false;
    }

    // The remaining words are undocumented. Log them so a future revision can
    // find out whether the driver accepts a narrower grayscale buffer than the
    // 32-bit one written below.
    ALOGI("EBC buffer info %u %u %u %u %u %u %u %u %u %u %u %u %u %u",
          info.words[0], info.words[1], info.words[2], info.words[3],
          info.words[4], info.words[5], info.words[6], info.words[7],
          info.words[8], info.words[9], info.words[10], info.words[11],
          info.words[12], info.words[13]);

    width_ = width;
    height_ = height;
    buffer_size_ = static_cast<size_t>(width_) * height_ * sizeof(uint32_t);
    buffer_ =
        mmap(nullptr, buffer_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
    if (buffer_ == MAP_FAILED) {
      ALOGE("EBC mmap(%zu) failed: %s", buffer_size_, strerror(errno));
      return false;
    }

    ALOGI("mapped EBC buffer for %ux%u", width_, height_);
    return true;
  }

  bool submit(const uint8_t *pixels, uint32_t stride, const ChangedRect &rect,
              uint32_t waveform, bool dither, bool full_refresh) {
    stage(pixels, stride, rect);
    return sendStagedUpdate(full_refresh ? ChangedRect{0, 0, width_, height_}
                                         : rect,
                            waveform, dither, full_refresh);
  }

  void stage(const uint8_t *pixels, uint32_t stride, const ChangedRect &rect) {
    // The mmap persists between updates. Keep it current by copying only the
    // changed rectangle; full cleanups can then reuse the complete accumulated
    // frame instead of copying another 8 MiB.
    const size_t copy_bytes =
        static_cast<size_t>(rect.right - rect.left) * sizeof(uint32_t);
    for (uint32_t y = rect.top; y < rect.bottom; ++y) {
      memcpy(static_cast<uint8_t *>(buffer_) +
                 (static_cast<size_t>(y) * width_ + rect.left) *
                     sizeof(uint32_t),
             pixels + (static_cast<size_t>(y) * stride + rect.left) *
                          sizeof(uint32_t),
             copy_bytes);
    }
  }

  bool sendStagedUpdate(const ChangedRect &rect, uint32_t waveform, bool dither,
                        bool full_refresh) {
    return sendUpdate(rect, waveform, dither, full_refresh);
  }

  // Re-issues a waveform over the frame already held in the persistent buffer,
  // so a manual cleanup does not have to wait for the compositor to produce
  // another frame.
  bool refreshFull(uint32_t waveform) {
    if (buffer_ == MAP_FAILED) {
      return false;
    }
    return sendUpdate(ChangedRect{0, 0, width_, height_}, waveform,
                      /*dither=*/true, /*full_refresh=*/true);
  }

  bool clear() {
    if (buffer_ == MAP_FAILED) {
      return false;
    }

    // E-Ink retains its last image without power. Replace the application
    // frame with white before Android suspends so an asleep device cannot look
    // like an unresponsive, still-open application.
    memset(buffer_, 0xff, buffer_size_);
    return sendUpdate(ChangedRect{0, 0, width_, height_}, kWaveformGc16,
                      /*dither=*/true, /*full_refresh=*/true);
  }

private:
  bool sendUpdate(const ChangedRect &rect, uint32_t waveform, bool dither,
                  bool full_refresh) {
    if (last_update_us_ != 0) {
      const int64_t remaining =
          last_update_us_ + kMinimumEbcUpdateIntervalUs - monotonicMicros();
      if (remaining > 0) {
        usleep(static_cast<useconds_t>(remaining));
      }
    }

    EbcUpdate update = {};
    update.top = rect.top;
    update.left = rect.left;
    update.width = rect.right - rect.left;
    update.height = rect.bottom - rect.top;
    update.waveform_mode = waveform;
    update.update_mode = full_refresh ? kUpdateFull : kUpdatePartial;
    update.update_marker = marker_++;
    update.temperature = kEbcAutoTemperature;
    update.flags = dither ? kEbcDitherFlag : 0;

    if (ioctl(fd_, kEbcSendUpdate, &update) < 0) {
      ALOGE("EBC SEND_UPDATE marker=%u failed: %s", update.update_marker,
            strerror(errno));
      return false;
    }
    last_update_us_ = monotonicMicros();
    return true;
  }

  int fd_ = -1;
  void *buffer_ = MAP_FAILED;
  size_t buffer_size_ = 0;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t marker_ = 1;
  int64_t last_update_us_ = 0;
};

ChangedRect unionRects(const ChangedRect &left, const ChangedRect &right) {
  return ChangedRect{
      std::min(left.left, right.left),
      std::min(left.top, right.top),
      std::max(left.right, right.right),
      std::max(left.bottom, right.bottom),
  };
}

// Tracks damage on a tile grid so unrelated changes in distant parts of the
// screen do not union into one screen-sized rectangle.
class DamageMap {
public:
  void resize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    columns_ = (width + kTileSize - 1) / kTileSize;
    rows_ = (height + kTileSize - 1) / kTileSize;
    tiles_.assign(static_cast<size_t>(columns_) * rows_, 0);
  }

  // Returns false when the frame is identical to |previous|.
  bool compare(const uint8_t *pixels, uint32_t stride,
               const std::vector<uint32_t> &previous) {
    std::fill(tiles_.begin(), tiles_.end(), 0);
    bool any = false;
    const size_t row_bytes = static_cast<size_t>(width_) * sizeof(uint32_t);

    for (uint32_t y = 0; y < height_; ++y) {
      const uint8_t *row =
          pixels + static_cast<size_t>(y) * stride * sizeof(uint32_t);
      const auto *old_row = reinterpret_cast<const uint8_t *>(
          previous.data() + static_cast<size_t>(y) * width_);
      // bionic's memcmp is SIMD, so an unchanged row costs one wide pass and
      // no per-pixel work at all.
      if (memcmp(row, old_row, row_bytes) == 0) {
        continue;
      }

      uint8_t *tile_row =
          tiles_.data() + static_cast<size_t>(y / kTileSize) * columns_;
      for (uint32_t column = 0; column < columns_; ++column) {
        if (tile_row[column] != 0) {
          continue;
        }
        const uint32_t x = column * kTileSize;
        const size_t bytes =
            static_cast<size_t>(std::min(kTileSize, width_ - x)) *
            sizeof(uint32_t);
        const size_t offset = static_cast<size_t>(x) * sizeof(uint32_t);
        if (memcmp(row + offset, old_row + offset, bytes) != 0) {
          tile_row[column] = 1;
          any = true;
        }
      }
    }
    return any;
  }

  ChangedRect bounds() const {
    ChangedRect result = {width_, height_, 0, 0};
    for (uint32_t row = 0; row < rows_; ++row) {
      for (uint32_t column = 0; column < columns_; ++column) {
        if (tiles_[static_cast<size_t>(row) * columns_ + column] == 0) {
          continue;
        }
        result.left = std::min(result.left, column * kTileSize);
        result.top = std::min(result.top, row * kTileSize);
        result.right =
            std::max(result.right, std::min(width_, (column + 1) * kTileSize));
        result.bottom =
            std::max(result.bottom, std::min(height_, (row + 1) * kTileSize));
      }
    }
    return result;
  }

  uint64_t dirtyArea() const {
    uint64_t area = 0;
    for (uint32_t row = 0; row < rows_; ++row) {
      const uint32_t tile_height =
          std::min(kTileSize, height_ - row * kTileSize);
      for (uint32_t column = 0; column < columns_; ++column) {
        if (tiles_[static_cast<size_t>(row) * columns_ + column] == 0) {
          continue;
        }
        const uint32_t tile_width =
            std::min(kTileSize, width_ - column * kTileSize);
        area += static_cast<uint64_t>(tile_width) * tile_height;
      }
    }
    return area;
  }

  // Visit horizontally contiguous dirty tiles without coalescing unrelated
  // rows. This is used for memory copies only; the EBC driver still receives
  // the single, conservatively paced update rectangle.
  template <typename Callback> void forEachDirtyRun(Callback callback) const {
    for (uint32_t row = 0; row < rows_; ++row) {
      uint32_t column = 0;
      while (column < columns_) {
        if (tiles_[static_cast<size_t>(row) * columns_ + column] == 0) {
          ++column;
          continue;
        }
        const uint32_t start = column;
        while (column < columns_ &&
               tiles_[static_cast<size_t>(row) * columns_ + column] != 0) {
          ++column;
        }
        callback(ChangedRect{
            start * kTileSize,
            row * kTileSize,
            std::min(width_, column * kTileSize),
            std::min(height_, (row + 1) * kTileSize),
        });
      }
    }
  }

  // Merges dirty tiles into at most kMaxUpdateRects rectangles, falling back
  // to the bounding box when splitting would not save enough panel area.
  std::vector<ChangedRect> rectangles() const {
    std::vector<ChangedRect> rects;
    std::vector<size_t> previous_row;
    std::vector<size_t> current_row;

    for (uint32_t row = 0; row < rows_; ++row) {
      current_row.clear();
      uint32_t column = 0;
      while (column < columns_) {
        if (tiles_[static_cast<size_t>(row) * columns_ + column] == 0) {
          ++column;
          continue;
        }
        const uint32_t start = column;
        while (column < columns_ &&
               tiles_[static_cast<size_t>(row) * columns_ + column] != 0) {
          ++column;
        }

        const ChangedRect run = {
            start * kTileSize,
            row * kTileSize,
            std::min(width_, column * kTileSize),
            std::min(height_, (row + 1) * kTileSize),
        };

        size_t extended = rects.size();
        for (size_t index : previous_row) {
          if (rects[index].left == run.left &&
              rects[index].right == run.right &&
              rects[index].bottom == run.top) {
            rects[index].bottom = run.bottom;
            extended = index;
            break;
          }
        }
        if (extended == rects.size()) {
          rects.push_back(run);
        }
        current_row.push_back(extended);

        if (rects.size() > kMaxCoalesceRects) {
          return {bounds()};
        }
      }
      previous_row.swap(current_row);
    }

    if (rects.empty()) {
      return {};
    }

    while (rects.size() > kMaxUpdateRects) {
      mergeCheapestPair(&rects);
    }

    const ChangedRect box = bounds();
    uint64_t total = 0;
    for (const ChangedRect &rect : rects) {
      total += rect.area();
    }
    if (rects.size() > 1 && total * kSplitBenefitDenominator >
                                box.area() * kSplitBenefitNumerator) {
      return {box};
    }
    return rects;
  }

private:
  static uint64_t intersectionArea(const ChangedRect &left,
                                   const ChangedRect &right) {
    const uint32_t overlap_width = std::min(left.right, right.right) -
                                   std::min(std::min(left.right, right.right),
                                            std::max(left.left, right.left));
    const uint32_t overlap_height =
        std::min(left.bottom, right.bottom) -
        std::min(std::min(left.bottom, right.bottom),
                 std::max(left.top, right.top));
    return static_cast<uint64_t>(overlap_width) * overlap_height;
  }

  static bool overlaps(const ChangedRect &left, const ChangedRect &right) {
    return left.left < right.right && right.left < left.right &&
           left.top < right.bottom && right.top < left.bottom;
  }

  static void mergeOverlaps(std::vector<ChangedRect> *rects) {
    bool merged = true;
    while (merged) {
      merged = false;
      for (size_t i = 0; i < rects->size() && !merged; ++i) {
        for (size_t j = i + 1; j < rects->size(); ++j) {
          if (!overlaps((*rects)[i], (*rects)[j])) {
            continue;
          }
          (*rects)[i] = unionRects((*rects)[i], (*rects)[j]);
          rects->erase(rects->begin() + static_cast<ptrdiff_t>(j));
          merged = true;
          break;
        }
      }
    }
  }

  static void mergeCheapestPair(std::vector<ChangedRect> *rects) {
    size_t best_first = 0;
    size_t best_second = 1;
    uint64_t best_cost = UINT64_MAX;
    for (size_t i = 0; i < rects->size(); ++i) {
      for (size_t j = i + 1; j < rects->size(); ++j) {
        const uint64_t covered = (*rects)[i].area() + (*rects)[j].area() -
                                 intersectionArea((*rects)[i], (*rects)[j]);
        const uint64_t cost =
            unionRects((*rects)[i], (*rects)[j]).area() - covered;
        if (cost < best_cost) {
          best_cost = cost;
          best_first = i;
          best_second = j;
        }
      }
    }
    (*rects)[best_first] =
        unionRects((*rects)[best_first], (*rects)[best_second]);
    rects->erase(rects->begin() + static_cast<ptrdiff_t>(best_second));
    mergeOverlaps(rects);
  }

  std::vector<uint8_t> tiles_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t columns_ = 0;
  uint32_t rows_ = 0;
};

uint32_t luminance(uint32_t pixel) {
  // Capture buffers are RGBA_8888, so the red channel is the low byte.
  const uint32_t red = pixel & 0xffu;
  const uint32_t green = (pixel >> 8) & 0xffu;
  const uint32_t blue = (pixel >> 16) & 0xffu;
  return (red * 54u + green * 183u + blue * 19u) >> 8;
}

// Text and flat UI put nearly all of their luminance at the two extremes,
// while photographs spread it out. DU and undithered updates suit the former
// and ruin the latter.
ContentClass classifyRegion(const uint8_t *pixels, uint32_t stride,
                            const ChangedRect &rect) {
  const uint32_t width = rect.right - rect.left;
  const uint32_t height = rect.bottom - rect.top;
  if (width == 0 || height == 0) {
    return ContentClass::kBiLevel;
  }

  const uint32_t step_x = std::max(1u, width / kClassifySamplesPerAxis);
  const uint32_t step_y = std::max(1u, height / kClassifySamplesPerAxis);
  uint32_t total = 0;
  uint32_t histogram[kLuminanceBins] = {};

  for (uint32_t y = rect.top; y < rect.bottom; y += step_y) {
    const auto *row = reinterpret_cast<const uint32_t *>(
        pixels + static_cast<size_t>(y) * stride * sizeof(uint32_t));
    // Shear the sample columns row by row. Fixed strides alias badly against
    // the regular pixel patterns that hairlines and dividers produce.
    for (uint32_t x = rect.left + (y % step_x); x < rect.right; x += step_x) {
      const uint32_t luma = luminance(row[x]);
      ++total;
      ++histogram[std::min(kLuminanceBins - 1, luma * kLuminanceBins / 256u)];
    }
  }

  if (total == 0) {
    return ContentClass::kBiLevel;
  }
  uint32_t significant_bins = 0;
  uint32_t dominant_samples = 0;
  for (uint32_t count : histogram) {
    if (count * 100u < total * kSignificantBinPercent) {
      continue;
    }
    ++significant_bins;
    dominant_samples += count;
  }
  return significant_bins <= 2 &&
                 dominant_samples * 100u >= total * kBiLevelPercent
             ? ContentClass::kBiLevel
             : ContentClass::kGrayscale;
}

uint64_t hashRow(const uint32_t *row, uint32_t width) {
  constexpr uint64_t kOffsetBasis = 1469598103934665603ULL;
  constexpr uint64_t kPrime = 1099511628211ULL;
  uint64_t hash = kOffsetBasis;
  uint32_t index = 0;
  for (; index + 1 < width; index += 2) {
    uint64_t pair = 0;
    memcpy(&pair, row + index, sizeof(pair));
    hash = (hash ^ pair) * kPrime;
  }
  if (index < width) {
    hash = (hash ^ row[index]) * kPrime;
  }
  return hash;
}

// Recognises a vertical scroll so the whole moved area can be redrawn with a
// fast waveform once, instead of provoking a full cleanup on every frame.
class ScrollDetector {
public:
  void resize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    hashes_.assign(height, 0);
    valid_.assign(height, 0);
  }

  void invalidate(uint32_t top, uint32_t bottom) {
    if (valid_.empty()) {
      return;
    }
    const uint32_t last = std::min<uint32_t>(bottom, valid_.size());
    for (uint32_t y = top; y < last; ++y) {
      valid_[y] = 0;
    }
  }

  void reset() { std::fill(valid_.begin(), valid_.end(), 0); }

  std::optional<int32_t> detect(const uint8_t *pixels, uint32_t stride,
                                const std::vector<uint32_t> &previous,
                                const ChangedRect &bounds) {
    if (valid_.size() != height_ ||
        previous.size() != static_cast<size_t>(width_) * height_) {
      return std::nullopt;
    }

    refreshPreviousHashes(previous);

    std::unordered_map<uint64_t, int32_t> rows;
    rows.reserve(height_ * 2);
    for (uint32_t y = 0; y < height_; ++y) {
      const auto inserted = rows.emplace(hashes_[y], static_cast<int32_t>(y));
      if (!inserted.second) {
        // Blank and repeated rows carry no positional information.
        inserted.first->second = kAmbiguousRow;
      }
    }

    const uint32_t span = bounds.bottom - bounds.top;
    const uint32_t step = std::max(1u, span / kScrollProbeRows);
    std::unordered_map<int32_t, uint32_t> votes;
    for (uint32_t y = bounds.top; y < bounds.bottom; y += step) {
      const auto *row = reinterpret_cast<const uint32_t *>(
          pixels + static_cast<size_t>(y) * stride * sizeof(uint32_t));
      const auto found = rows.find(hashRow(row, width_));
      if (found == rows.end() || found->second == kAmbiguousRow) {
        continue;
      }
      ++votes[static_cast<int32_t>(y) - found->second];
    }

    int32_t best_shift = 0;
    uint32_t best_votes = 0;
    for (const auto &vote : votes) {
      if (vote.second > best_votes) {
        best_votes = vote.second;
        best_shift = vote.first;
      }
    }

    const int32_t magnitude = best_shift < 0 ? -best_shift : best_shift;
    if (best_votes < kScrollMinVotes || magnitude < kScrollMinShift) {
      return std::nullopt;
    }
    return best_shift;
  }

private:
  static constexpr int32_t kAmbiguousRow = -1;

  void refreshPreviousHashes(const std::vector<uint32_t> &previous) {
    for (uint32_t y = 0; y < height_; ++y) {
      if (valid_[y] != 0) {
        continue;
      }
      hashes_[y] =
          hashRow(previous.data() + static_cast<size_t>(y) * width_, width_);
      valid_[y] = 1;
    }
  }

  std::vector<uint64_t> hashes_;
  std::vector<uint8_t> valid_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

void saveFrameRegion(const uint8_t *pixels, uint32_t width, uint32_t height,
                     uint32_t stride, const ChangedRect &rect,
                     std::vector<uint32_t> *destination) {
  if (destination->size() != static_cast<size_t>(width) * height) {
    destination->resize(static_cast<size_t>(width) * height);
  }
  const size_t copy_bytes =
      static_cast<size_t>(rect.right - rect.left) * sizeof(uint32_t);
  for (uint32_t y = rect.top; y < rect.bottom; ++y) {
    memcpy(destination->data() + static_cast<size_t>(y) * width + rect.left,
           pixels +
               (static_cast<size_t>(y) * stride + rect.left) * sizeof(uint32_t),
           copy_bytes);
  }
}

void publishStat(const char *name, uint64_t value, bool *success) {
  *success &= android::base::SetProperty(name, std::to_string(value));
}

void publishStats(BridgeStats *stats, bool force = false) {
  const int64_t now_us = monotonicMicros();
  if (!force && stats->last_publish_us != 0 &&
      now_us - stats->last_publish_us < kStatsPublishIntervalUs) {
    return;
  }

  bool success = true;
  publishStat("sys.leaf3.stat.captures", stats->captures, &success);
  publishStat("sys.leaf3.stat.comparisons", stats->comparisons, &success);
  publishStat("sys.leaf3.stat.changed", stats->changed_frames, &success);
  publishStat("sys.leaf3.stat.partial", stats->partial_updates, &success);
  publishStat("sys.leaf3.stat.full", stats->full_updates, &success);
  publishStat("sys.leaf3.stat.pixels", stats->updated_pixels, &success);
  publishStat("sys.leaf3.stat.dropped", stats->dropped_frames, &success);
  publishStat("sys.leaf3.stat.split", stats->split_frames, &success);
  publishStat("sys.leaf3.stat.bilevel", stats->bilevel_updates, &success);
  publishStat("sys.leaf3.stat.scroll", stats->scroll_frames, &success);
  publishStat("sys.leaf3.stat.scroll_gesture", stats->gesture_scroll_frames,
              &success);
  publishStat("sys.leaf3.stat.scroll_hash", stats->hash_scroll_frames,
              &success);
  publishStat("sys.leaf3.stat.capture_us", stats->capture_time_us, &success);
  publishStat("sys.leaf3.stat.compare_us", stats->compare_time_us, &success);
  publishStat("sys.leaf3.stat.submit_us", stats->submit_time_us, &success);
  if (!success) {
    ALOGW("one or more bridge statistics properties could not be published");
  }
  ALOGI("stats captures=%llu comparisons=%llu changed=%llu partial=%llu "
        "full=%llu pixels=%llu dropped=%llu split=%llu bilevel=%llu "
        "scroll=%llu gesture_scroll=%llu hash_scroll=%llu capture_us=%llu "
        "compare_us=%llu submit_us=%llu",
        static_cast<unsigned long long>(stats->captures),
        static_cast<unsigned long long>(stats->comparisons),
        static_cast<unsigned long long>(stats->changed_frames),
        static_cast<unsigned long long>(stats->partial_updates),
        static_cast<unsigned long long>(stats->full_updates),
        static_cast<unsigned long long>(stats->updated_pixels),
        static_cast<unsigned long long>(stats->dropped_frames),
        static_cast<unsigned long long>(stats->split_frames),
        static_cast<unsigned long long>(stats->bilevel_updates),
        static_cast<unsigned long long>(stats->scroll_frames),
        static_cast<unsigned long long>(stats->gesture_scroll_frames),
        static_cast<unsigned long long>(stats->hash_scroll_frames),
        static_cast<unsigned long long>(stats->capture_time_us),
        static_cast<unsigned long long>(stats->compare_time_us),
        static_cast<unsigned long long>(stats->submit_time_us));
  stats->last_publish_us = now_us;
}

useconds_t activeFrameDelay(RefreshMode mode) {
  switch (mode) {
  case RefreshMode::kSpeed:
  case RefreshMode::kA2:
    return kFastFrameDelayUs;
  case RefreshMode::kNormal:
  case RefreshMode::kRegal:
    return kQualityFrameDelayUs;
  case RefreshMode::kBalanced:
  default:
    return kBalancedFrameDelayUs;
  }
}

useconds_t idleFrameDelay(IdlePolicy policy, uint32_t idle_poll_count) {
  if (idle_poll_count < 2) {
    return kIdleFrameDelayUs;
  }
  if (policy == IdlePolicy::kResponsive || idle_poll_count < 4) {
    return kIdleOneSecondDelayUs;
  }
  if (policy == IdlePolicy::kBalanced || idle_poll_count < 6) {
    return kIdleTwoSecondDelayUs;
  }
  return kIdleFiveSecondDelayUs;
}

int64_t cleanupDelay(CleanupPolicy policy) {
  return policy == CleanupPolicy::kQuality ? kQualityCleanupDelayUs
                                           : kBalancedCleanupDelayUs;
}

void inputThread(Waiter *waiter, std::atomic<int64_t> *last_touch_us,
                 std::atomic<int64_t> *last_scroll_motion_us,
                 std::atomic<bool> *dragging_contact_active) {
  InputWakeMonitor monitor;
  if (!monitor.init()) {
    ALOGW("touch input %s was not found; scroll detection and touch-assisted "
          "polling fallback are disabled",
          kTouchInputName);
    return;
  }
  for (;;) {
    bool scroll_motion = false;
    bool active_drag = false;
    if (monitor.wait(-1, &scroll_motion, &active_drag)) {
      const int64_t now_us = monotonicMicros();
      last_touch_us->store(now_us, std::memory_order_relaxed);
      dragging_contact_active->store(active_drag, std::memory_order_relaxed);
      if (scroll_motion) {
        last_scroll_motion_us->store(now_us, std::memory_order_relaxed);
      }
      waiter->signal(Waiter::kInput);
    }
  }
}

void propertyThread(Waiter *waiter) {
  uint32_t serial = __system_property_area_serial();
  for (;;) {
    if (android::base::GetProperty(kInteractiveProperty, "1") != "1") {
      // Wait on the exact property while asleep. A global property-area wait
      // would wake for every unrelated property change on the device.
      android::base::WaitForProperty(kInteractiveProperty, "1");
      serial = __system_property_area_serial();
      waiter->signal(Waiter::kProperty);
      continue;
    }

    uint32_t next = serial;
    if (!__system_property_wait(nullptr, serial, &next, nullptr)) {
      sleep(1);
      continue;
    }
    serial = next;
    waiter->signal(Waiter::kProperty);
  }
}

std::unique_ptr<FrameSource> startFrameSource(bool prefer_stream,
                                              Waiter *waiter) {
  if (prefer_stream) {
    auto mirror = std::make_unique<MirrorFrameSource>(waiter);
    if (mirror->start()) {
      // Creating the display forces a composition, so a healthy mirror always
      // produces its first frame quickly.
      const int64_t deadline = monotonicMicros() + kStreamStartupTimeoutUs;
      for (;;) {
        Frame frame;
        if (mirror->acquire(&frame)) {
          // Keep this first, guaranteed frame locked. The main loop consumes it
          // as the startup full refresh instead of waiting for a second change.
          android::base::SetProperty("sys.leaf3.stat.capture_mode", "stream");
          ALOGI("capture mode: compositor stream");
          return mirror;
        }
        const int64_t remaining = deadline - monotonicMicros();
        if (remaining <= 0) {
          break;
        }
        waiter->wait(std::min<int64_t>(remaining, 200000));
      }
      ALOGW("the mirror display produced no frame; falling back to polling");
    }
    mirror->stop();
  }

  auto screenshot = std::make_unique<ScreenshotFrameSource>();
  screenshot->start();
  android::base::SetProperty("sys.leaf3.stat.capture_mode", "poll");
  ALOGI("capture mode: periodic screenshot");
  return screenshot;
}

} // namespace

int main() {
  ProcessState::self()->setThreadPoolMaxThreadCount(4);
  ProcessState::self()->startThreadPool();

  Waiter waiter;
  std::atomic<int64_t> last_touch_us{0};
  std::atomic<int64_t> last_scroll_motion_us{0};
  std::atomic<bool> dragging_contact_active{false};
  std::thread(inputThread, &waiter, &last_touch_us, &last_scroll_motion_us,
              &dragging_contact_active)
      .detach();
  std::thread(propertyThread, &waiter).detach();

  EbcDevice ebc;
  FrontlightBridge frontlight;
  DamageMap damage;
  ScrollDetector scroll;
  BridgeStats stats;
  std::vector<uint32_t> previous;

  SettingsCache settings_cache;
  Settings settings = settings_cache.read();
  std::unique_ptr<FrameSource> source;
  if (settings.interactive) {
    source = startFrameSource(settings.prefer_stream, &waiter);
  }

  bool initialized = false;
  bool first_refresh = true;
  bool display_was_on = settings.interactive;
  bool pending_full_refresh = false;
  bool cleanup_pending = false;
  bool cleanup_damage_valid = false;
  bool scroll_in_progress = false;
  bool source_prefers_stream = settings.prefer_stream;
  uint32_t unchanged_frames = 0;
  uint32_t idle_poll_count = 0;
  uint32_t input_probe_frames = 0;
  uint64_t source_dropped_seen = 0;
  uint32_t panel_width = 0;
  uint32_t panel_height = 0;
  uint64_t panel_pixels = 0;
  int64_t cleanup_deadline_us = 0;
  int64_t last_scroll_activity_us = 0;
  ChangedRect cleanup_damage = {};
  std::string full_refresh_token = settings.full_refresh_token;
  RefreshMode active_mode = settings.mode;
  ALOGI("refresh mode: %s", refreshModeName(active_mode));

  for (;;) {
    settings = settings_cache.read();
    frontlight.update(settings, settings.interactive);

    if (cleanup_pending && settings.cleanup == CleanupPolicy::kManual) {
      cleanup_pending = false;
      cleanup_damage_valid = false;
      cleanup_deadline_us = 0;
    }

    if (settings.full_refresh_token != full_refresh_token) {
      full_refresh_token = settings.full_refresh_token;
      pending_full_refresh = true;
      ALOGI("manual full refresh requested");
    }

    if (!settings.interactive) {
      if (display_was_on) {
        if (initialized && settings.clear_on_sleep) {
          ALOGI("display is off; clearing retained frame");
          const int64_t submit_start_us = monotonicMicros();
          if (!ebc.clear()) {
            return 1;
          }
          ++stats.full_updates;
          stats.updated_pixels += panel_pixels;
          stats.submit_time_us +=
              static_cast<uint64_t>(monotonicMicros() - submit_start_us);
        }
        ALOGI("display is off; releasing the compositor frame source");
        source->stop();
        source.reset();
        display_was_on = false;
        first_refresh = true;
        pending_full_refresh = false;
        previous.clear();
        scroll.reset();
        cleanup_pending = false;
        cleanup_damage_valid = false;
        scroll_in_progress = false;
        cleanup_deadline_us = 0;
        unchanged_frames = 0;
        idle_poll_count = 0;
        input_probe_frames = 0;
      }
      publishStats(&stats, /*force=*/true);
      // Block on the interactive property itself rather than waking on a
      // timer. Nothing here needs to run again until Android comes back.
      android::base::WaitForProperty(kInteractiveProperty, "1");
      waiter.drain();
      continue;
    }

    if (!display_was_on) {
      ALOGI("display is on; resuming with a full refresh");
      display_was_on = true;
      first_refresh = true;
      input_probe_frames = kInputProbeFrames;
      source = startFrameSource(settings.prefer_stream, &waiter);
      source_prefers_stream = settings.prefer_stream;
      source_dropped_seen = 0;
    }

    if (settings.prefer_stream != source_prefers_stream) {
      ALOGI("capture preference changed; recreating the frame source");
      source->stop();
      source = startFrameSource(settings.prefer_stream, &waiter);
      source_prefers_stream = settings.prefer_stream;
      source_dropped_seen = 0;
      first_refresh = true;
      previous.clear();
      scroll.reset();
      cleanup_pending = false;
      cleanup_damage_valid = false;
      scroll_in_progress = false;
      cleanup_deadline_us = 0;
    }

    if (settings.mode != active_mode) {
      active_mode = settings.mode;
      cleanup_pending = false;
      cleanup_damage_valid = false;
      cleanup_deadline_us = 0;
      unchanged_frames = 0;
      idle_poll_count = 0;
      input_probe_frames = kInputProbeFrames;
      ALOGI("refresh mode changed to %s", refreshModeName(active_mode));
    }

    // A manual cleanup must work even when the compositor is quiet, so it is
    // served from the persistent EBC buffer rather than a new frame.
    if (pending_full_refresh && initialized && !first_refresh) {
      pending_full_refresh = false;
      const int64_t submit_start_us = monotonicMicros();
      if (!ebc.refreshFull(kWaveformGc16)) {
        return 1;
      }
      stats.submit_time_us +=
          static_cast<uint64_t>(monotonicMicros() - submit_start_us);
      ++stats.full_updates;
      stats.updated_pixels += panel_pixels;
      cleanup_pending = false;
      cleanup_damage_valid = false;
      scroll_in_progress = false;
      cleanup_deadline_us = 0;
    }

    Frame frame;
    const int64_t capture_start_us = monotonicMicros();
    const bool have_frame = source->acquire(&frame);
    if (have_frame) {
      ++stats.captures;
      stats.capture_time_us +=
          static_cast<uint64_t>(monotonicMicros() - capture_start_us);
      const uint64_t source_dropped = source->dropped();
      if (source_dropped >= source_dropped_seen) {
        stats.dropped_frames += source_dropped - source_dropped_seen;
      }
      source_dropped_seen = source_dropped;
    }

    if (have_frame) {
      if (!initialized) {
        if (!ebc.init(frame.width, frame.height)) {
          source->release();
          return 1;
        }
        initialized = true;
        panel_width = frame.width;
        panel_height = frame.height;
        panel_pixels = static_cast<uint64_t>(frame.width) * frame.height;
        damage.resize(frame.width, frame.height);
        scroll.resize(frame.width, frame.height);
      }

      if (frame.width != panel_width || frame.height != panel_height) {
        // The EBC mapping is fixed at the geometry probed on the first frame,
        // so a resized compositor output cannot be forwarded safely.
        ALOGE("frame %ux%u no longer matches the %ux%u panel", frame.width,
              frame.height, panel_width, panel_height);
        source->release();
        return 1;
      }

      const bool geometry_changed =
          previous.size() != static_cast<size_t>(frame.width) * frame.height;
      const bool full_refresh = first_refresh || geometry_changed;

      std::vector<ChangedRect> rects;
      bool changed = false;
      if (full_refresh) {
        rects.push_back(ChangedRect{0, 0, frame.width, frame.height});
        changed = true;
      } else {
        const int64_t compare_start_us = monotonicMicros();
        changed = damage.compare(frame.pixels, frame.stride, previous);
        if (changed) {
          rects = damage.rectangles();
          changed = !rects.empty();
        }
        ++stats.comparisons;
        stats.compare_time_us +=
            static_cast<uint64_t>(monotonicMicros() - compare_start_us);
      }

      const int64_t now_us = monotonicMicros();
      const int64_t touch_us = last_touch_us.load(std::memory_order_relaxed);
      const int64_t scroll_motion_us =
          last_scroll_motion_us.load(std::memory_order_relaxed);
      last_scroll_activity_us =
          std::max(last_scroll_activity_us, scroll_motion_us);
      const bool active_drag =
          dragging_contact_active.load(std::memory_order_relaxed);
      const bool recent_scroll_motion =
          scroll_motion_us != 0 &&
          now_us - scroll_motion_us < kScrollGestureWindowUs;
      // A contact that has crossed touch slop remains a gesture until its
      // tracking ID is released. Recent motion then provides the short
      // post-release continuation used for fling startup.
      const bool gesture_scrolling = active_drag || recent_scroll_motion;
      const bool established_fling_live =
          scroll_in_progress && last_scroll_activity_us != 0 &&
          now_us - last_scroll_activity_us < kScrollFlingWindowUs;

      bool scrolling = false;
      if (changed && !full_refresh && settings.scroll_detect) {
        const ChangedRect box = damage.bounds();
        // App content rarely occupies the status and navigation bars. Requiring
        // half of the physical panel excluded otherwise valid Settings and
        // browser viewports, so use one third while retaining the row-shift
        // vote check that rejects ordinary taps.
        if (static_cast<uint64_t>(box.bottom - box.top) * 3 > frame.height) {
          // Generic touch opens only the initial hash-detection window. Once
          // scrolling is established, only past-slop motion or another
          // positive row-hash match may extend the fling window.
          const bool initial_hash_window =
              touch_us != 0 && now_us - touch_us < kScrollGestureWindowUs;
          const bool allow_hash =
              scroll_in_progress ? established_fling_live : initial_hash_window;
          const bool hash_scrolling =
              !gesture_scrolling && allow_hash &&
              scroll.detect(frame.pixels, frame.stride, previous, box)
                  .has_value();
          if (hash_scrolling) {
            last_scroll_activity_us = now_us;
          }
          scrolling = gesture_scrolling || hash_scrolling;
          if (gesture_scrolling) {
            ++stats.gesture_scroll_frames;
          } else if (hash_scrolling) {
            ++stats.hash_scroll_frames;
          }
          if (scrolling) {
            ++stats.scroll_frames;
          }
        }
      }
      if (changed) {
        // Narrow damage such as a scrollbar or caret is intentionally not
        // promoted to A2, but it must not erase a live drag or the extended
        // row-hash window of an established fling.
        scroll_in_progress =
            settings.scroll_detect &&
            (scrolling || gesture_scrolling || established_fling_live);
      }

      if (changed) {
        ++stats.changed_frames;
        if (rects.size() > 1) {
          ++stats.split_frames;
        }

        // A clock and a keyboard can make the safe, single update rectangle
        // span nearly the whole panel even though only a few tiles changed.
        // The persistent EBC mmap already holds the rest of the frame, so
        // copy sparse tiles rather than the large bounding rectangle. Dense
        // changes keep the cheaper contiguous copy path.
        const uint64_t update_area = rects.front().area();
        const bool sparse_damage =
            !full_refresh && damage.dirtyArea() * kSplitBenefitDenominator <
                                 update_area * kSplitBenefitNumerator;
        if (sparse_damage) {
          damage.forEachDirtyRun([&](const ChangedRect &rect) {
            ebc.stage(frame.pixels, frame.stride, rect);
          });
        } else {
          for (const ChangedRect &rect : rects) {
            ebc.stage(frame.pixels, frame.stride, rect);
          }
        }

        bool submitted_any = false;
        bool needs_cleanup = false;

        for (const ChangedRect &rect : rects) {
          const ContentClass content =
              settings.content_aware
                  ? classifyRegion(frame.pixels, frame.stride, rect)
                  : ContentClass::kGrayscale;
          // Do not apply DU to a large bounding rectangle made from sparse,
          // distant tiles. It would unnecessarily disturb unchanged grayscale
          // content between those tiles.
          const bool bilevel =
              content == ContentClass::kBiLevel && !sparse_damage;

          uint32_t waveform = kWaveformAuto;
          bool fast = false;
          switch (active_mode) {
          case RefreshMode::kSpeed:
            waveform = kWaveformDu;
            fast = true;
            break;
          case RefreshMode::kA2:
            waveform = kWaveformAnim;
            fast = true;
            break;
          case RefreshMode::kRegal:
            waveform = kWaveformRegal;
            break;
          case RefreshMode::kNormal:
            waveform = kWaveformAuto;
            break;
          case RefreshMode::kBalanced:
            if (scrolling) {
              waveform = kWaveformAnim;
              fast = true;
            } else if (settings.content_aware && bilevel) {
              waveform = kWaveformDu;
              fast = true;
            } else if (settings.content_aware) {
              // AUTO is the proven-safe grayscale path. Explicit GC16 here
              // made mixed or sparse damage drive large quality passes and,
              // followed by cleanup, could overload the vendor EBC stack.
              waveform = kWaveformAuto;
            } else if (input_probe_frames > 0) {
              waveform = kWaveformAnim;
              fast = true;
            }
            break;
          }

          // Scroll detection is an explicit global behavior, not a Balanced
          // mode detail. Apply it after the per-app mode decision so Normal
          // and Regal profiles also become responsive while moving.
          if (scrolling) {
            waveform = kWaveformAnim;
            fast = true;
          }

          if (full_refresh) {
            waveform = kWaveformGc16;
            fast = false;
          }

          // Dithering trades sharpness for tonal range. Text only loses.
          const bool dither = !(settings.content_aware && bilevel);
          if (!dither) {
            ++stats.bilevel_updates;
          }

          const int64_t submit_start_us = monotonicMicros();
          const ChangedRect update_rect =
              full_refresh ? ChangedRect{0, 0, frame.width, frame.height}
                           : rect;
          if (!ebc.sendStagedUpdate(update_rect, waveform, dither,
                                    full_refresh)) {
            source->release();
            return 1;
          }
          stats.submit_time_us +=
              static_cast<uint64_t>(monotonicMicros() - submit_start_us);
          submitted_any = true;
          needs_cleanup |= fast;
          if (fast && !full_refresh) {
            cleanup_damage = cleanup_damage_valid
                                 ? unionRects(cleanup_damage, update_rect)
                                 : update_rect;
            cleanup_damage_valid = true;
          }

          if (full_refresh) {
            ++stats.full_updates;
            stats.updated_pixels += panel_pixels;
          } else {
            ++stats.partial_updates;
            stats.updated_pixels += rect.area();
          }

          scroll.invalidate(rect.top, rect.bottom);
        }

        if (submitted_any) {
          if (sparse_damage) {
            damage.forEachDirtyRun([&](const ChangedRect &rect) {
              saveFrameRegion(frame.pixels, frame.width, frame.height,
                              frame.stride, rect, &previous);
            });
          } else {
            for (const ChangedRect &rect : rects) {
              saveFrameRegion(frame.pixels, frame.width, frame.height,
                              frame.stride, rect, &previous);
            }
          }
          first_refresh = false;
          pending_full_refresh = false;
          unchanged_frames = 0;
          idle_poll_count = 0;
          if (full_refresh) {
            cleanup_pending = false;
            cleanup_damage_valid = false;
            scroll_in_progress = false;
            cleanup_deadline_us = 0;
          } else if (settings.cleanup == CleanupPolicy::kManual) {
            cleanup_pending = false;
            cleanup_damage_valid = false;
            scroll_in_progress = false;
            cleanup_deadline_us = 0;
          } else {
            if (needs_cleanup) {
              cleanup_pending = true;
            }
            // Once a fast update has made cleanup necessary, every subsequent
            // changed frame postpones GC16. This prevents cleanup flashes from
            // being inserted into a continuous scroll merely because one
            // intermediate frame was classified as grayscale.
            if (cleanup_pending) {
              cleanup_deadline_us =
                  monotonicMicros() + cleanupDelay(settings.cleanup);
            }
          }
        }
      } else {
        unchanged_frames = std::min(unchanged_frames + 1, kIdleThresholdFrames);
      }

      source->release();
    }

    // A fast waveform leaves the affected region dirty. Clean only the union
    // of those regions once the compositor is quiet; repeatedly applying
    // full-screen GC16 for small content-aware text updates is both wasteful
    // and too much load for this vendor EBC path.
    if (cleanup_pending && settings.cleanup != CleanupPolicy::kManual &&
        initialized && cleanup_damage_valid) {
      const bool quiet = monotonicMicros() >= cleanup_deadline_us;
      if (quiet) {
        const int64_t submit_start_us = monotonicMicros();
        if (!ebc.sendStagedUpdate(cleanup_damage, kWaveformGc16,
                                  /*dither=*/true,
                                  /*full_refresh=*/false)) {
          return 1;
        }
        stats.submit_time_us +=
            static_cast<uint64_t>(monotonicMicros() - submit_start_us);
        ++stats.partial_updates;
        stats.updated_pixels += cleanup_damage.area();
        cleanup_pending = false;
        cleanup_damage_valid = false;
        scroll_in_progress = false;
        cleanup_deadline_us = 0;
      }
    }

    if (input_probe_frames > 0) {
      --input_probe_frames;
    }
    publishStats(&stats);

    if (source->streaming()) {
      // Nothing to poll: sleep until the compositor, a touch or a settings
      // change has something to say, or until a deferred cleanup comes due.
      int64_t timeout_us = -1;
      if (cleanup_pending && cleanup_deadline_us != 0) {
        timeout_us =
            std::max<int64_t>(0, cleanup_deadline_us - monotonicMicros());
      }
      waiter.wait(timeout_us);
      continue;
    }

    useconds_t delay = activeFrameDelay(active_mode);
    if (unchanged_frames >= kIdleThresholdFrames && input_probe_frames == 0) {
      delay = idleFrameDelay(settings.idle, idle_poll_count);
      ++idle_poll_count;
    }
    if (cleanup_pending && cleanup_deadline_us != 0) {
      const int64_t remaining = cleanup_deadline_us - monotonicMicros();
      delay = static_cast<useconds_t>(
          std::clamp<int64_t>(remaining, 0, static_cast<int64_t>(delay)));
    }
    if (waiter.wait(delay) & Waiter::kInput) {
      input_probe_frames = kInputProbeFrames;
      idle_poll_count = 0;
      // Let InputDispatcher and SurfaceFlinger publish the frame caused by the
      // event before taking the screenshot.
      usleep(kTouchSettleDelayUs);
    }
  }
}
