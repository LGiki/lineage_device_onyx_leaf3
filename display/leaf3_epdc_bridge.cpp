/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * BOOX Leaf3 uses a dummy DSI connector as Android's primary display. The
 * actual E-Ink panel is updated through ONYX's private /dev/ebc interface.
 * Capture SurfaceFlinger's primary display and forward changed regions to
 * EBC. A frame notifier avoids idle polling and supplies conservative
 * compositor damage for cropped capture.
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
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#define _REALLY_INCLUDE_SYS__SYSTEM_PROPERTIES_H_
#include <sys/_system_properties.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
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
#include <binder/IServiceManager.h>
#include <binder/Parcel.h>
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
#include <utils/String16.h>
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
constexpr int64_t kNotifierInputProbeDelayUs = 250000;
constexpr int64_t kNotifierSafetyProbeDelayUs = 30000000;
constexpr int64_t kNotifierCaptureRetryDelayUs = 100000;
constexpr int64_t kNotifierRaceGraceDelayUs = 500000;
constexpr uint32_t kLeaf3FrameNotifierTransaction = 1037;
constexpr int32_t kLeaf3FrameNotifierVersion = 3;
constexpr int32_t kLeaf3FrameNotifierUnregister = 0;
constexpr int32_t kLeaf3FrameNotifierRegister = 1;
constexpr int32_t kLeaf3FrameNotifierTakeDamage = 2;
constexpr int32_t kLeaf3FrameNotifierRequestFullRefresh = 3;
constexpr int32_t kLeaf3FrameNotifierBlockAndWait = 4;
// ONYX changes waveform as soon as a drag crosses Android's touch slop. Keep
// the raw-input gesture active briefly after the last movement so a released
// fling stays fast, and allow row hashes to extend an established fling.
constexpr int64_t kScrollGestureWindowUs = 500000;
constexpr int64_t kScrollFlingWindowUs = 1500000;
// Stock reader mode keeps page turns fast and cleans only after a selected
// number of pages. Regal mode first exposes the newly composed page through
// AUTO, then applies its quality waveform after the page has remained still.
constexpr int64_t kSettledPageDelayUs = 180000;
constexpr uint64_t kPageTurnMinimumAreaDenominator = 3;

constexpr char kRefreshModeProperty[] = "persist.sys.leaf3.refresh_mode";
constexpr char kActiveRefreshModeProperty[] = "sys.leaf3.active_refresh_mode";
constexpr char kActivePackageProperty[] = "sys.leaf3.active_package";
constexpr char kActivePageIntervalProperty[] = "sys.leaf3.active_page_interval";
constexpr char kActiveContrastProperty[] = "sys.leaf3.active_contrast";
constexpr char kActiveGammaProperty[] = "sys.leaf3.active_gamma";
constexpr char kActiveDitherProperty[] = "sys.leaf3.active_dither";
constexpr char kFullRefreshProperty[] = "sys.leaf3.full_refresh";
constexpr char kClearOnSleepProperty[] = "persist.sys.leaf3.clear_on_sleep";
constexpr char kSleepScreenProperty[] = "persist.sys.leaf3.sleep_screen";
constexpr char kSleepScreenImagePath[] = "/data/misc/leaf3/sleep-screen.argb";
constexpr char kIdlePolicyProperty[] = "persist.sys.leaf3.idle_policy";
constexpr char kCleanupPolicyProperty[] = "persist.sys.leaf3.cleanup_policy";
constexpr char kContentAwareProperty[] = "persist.sys.leaf3.content_aware";
constexpr char kScrollDetectProperty[] = "persist.sys.leaf3.scroll_detect";
constexpr char kCaptureModeProperty[] = "persist.sys.leaf3.capture_mode";
constexpr char kPageIntervalProperty[] = "persist.sys.leaf3.page_interval";
constexpr char kSettledQualityProperty[] = "persist.sys.leaf3.settle_quality";
constexpr char kContrastProperty[] = "persist.sys.leaf3.contrast";
constexpr char kGammaProperty[] = "persist.sys.leaf3.gamma";
constexpr char kDitherProperty[] = "persist.sys.leaf3.dither";
constexpr char kEpdcBackendProperty[] = "persist.sys.leaf3.epdc_backend";
constexpr char kEpdcNativeBlockedProperty[] = "sys.leaf3.epdc_native_blocked";
constexpr char kEpdcNativeStateProperty[] = "sys.leaf3.stat.epdc_native_state";
constexpr char kEpdcActiveBackendProperty[] = "sys.leaf3.stat.epdc_backend";
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
// Automatic GC16 never spans more than one third of the panel. Separate,
// disconnected dirty areas are cleaned in independently paced passes.
constexpr uint64_t kMaximumAutomaticCleanupAreaDenominator = 3;

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
  kReader,
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

enum class CaptureMode {
  kPoll,
  kNotify,
};

enum class SleepScreen {
  kClear,
  kRetain,
  kImage,
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
  int page_interval = 10;
  bool settled_quality = true;
  int contrast = 0;
  int gamma = 100;
  bool dither = true;
  SleepScreen sleep_screen = SleepScreen::kClear;
  bool interactive = true;
  CaptureMode capture_mode = CaptureMode::kNotify;
  bool frontlight_enabled = true;
  int android_brightness = 128;
  int frontlight_override = -1;
  int frontlight_temperature = 0;
  std::string foreground_token;
  std::string full_refresh_token;
};

struct BridgeStats {
  uint64_t captures = 0;
  uint64_t cropped_captures = 0;
  uint64_t captured_pixels = 0;
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
  uint64_t cleanup_updates = 0;
  uint64_t page_turns = 0;
  uint64_t page_cleanups = 0;
  uint64_t settled_updates = 0;
  uint64_t capture_time_us = 0;
  uint64_t compare_time_us = 0;
  uint64_t submit_time_us = 0;
  uint64_t ioctl_time_us = 0;
  uint64_t gate_wait_time_us = 0;
  uint64_t notification_to_capture_us = 0;
  uint64_t notification_to_submit_us = 0;
  uint64_t notified_captures = 0;
  uint64_t notified_submits = 0;
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
  case RefreshMode::kReader:
    return "reader";
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
    const char *active_package = read(active_package_, &changed);
    const char *global_mode = read(global_mode_, &changed);
    const char *idle = read(idle_, &changed);
    const char *cleanup = read(cleanup_, &changed);
    const char *content_aware = read(content_aware_, &changed);
    const char *scroll_detect = read(scroll_detect_, &changed);
    const char *active_page_interval = read(active_page_interval_, &changed);
    const char *page_interval = read(page_interval_, &changed);
    const char *settled_quality = read(settled_quality_, &changed);
    const char *active_contrast = read(active_contrast_, &changed);
    const char *contrast = read(contrast_, &changed);
    const char *active_gamma = read(active_gamma_, &changed);
    const char *gamma = read(gamma_, &changed);
    const char *active_dither = read(active_dither_, &changed);
    const char *dither = read(dither_, &changed);
    const char *clear_on_sleep = read(clear_on_sleep_, &changed);
    const char *sleep_screen = read(sleep_screen_, &changed);
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
    } else if (mode == "reader") {
      settings_.mode = RefreshMode::kReader;
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
    const char *effective_page_interval =
        propertyValue(active_page_interval)[0] == '\0' ? page_interval
                                                       : active_page_interval;
    const int requested_page_interval =
        parseInteger(effective_page_interval, 10);
    const int supported_page_intervals[] = {0, 1, 3, 5, 10, 30, 50};
    settings_.page_interval = 10;
    for (const int supported : supported_page_intervals) {
      if (requested_page_interval == supported) {
        settings_.page_interval = supported;
        break;
      }
    }
    settings_.settled_quality = parseInteger(settled_quality, 1) != 0;
    settings_.contrast =
        std::clamp(parseInteger(propertyValue(active_contrast)[0] == '\0'
                                    ? contrast
                                    : active_contrast,
                                0),
                   -50, 50);
    settings_.gamma = std::clamp(
        parseInteger(
            propertyValue(active_gamma)[0] == '\0' ? gamma : active_gamma, 100),
        50, 200);
    settings_.dither =
        parseInteger(propertyValue(active_dither)[0] == '\0' ? dither
                                                             : active_dither,
                     1) != 0;
    const std::string sleep_screen_value = propertyValue(sleep_screen);
    if (sleep_screen_value == "retain") {
      settings_.sleep_screen = SleepScreen::kRetain;
    } else if (sleep_screen_value == "image") {
      settings_.sleep_screen = SleepScreen::kImage;
    } else if (sleep_screen_value == "clear") {
      settings_.sleep_screen = SleepScreen::kClear;
    } else {
      // Existing installations only have clear_on_sleep. Keep their behavior
      // until Leaf3 Controls writes the new explicit three-state setting.
      settings_.sleep_screen = parseInteger(clear_on_sleep, 1) != 0
                                   ? SleepScreen::kClear
                                   : SleepScreen::kRetain;
    }
    settings_.interactive = parseInteger(interactive, 1) != 0;
    // The virtual-display stream remains quarantined. "notify" retains
    // ScreenshotClient capture and changes only how SurfaceFlinger wakes it.
    // Old installations may retain the quarantined "stream" value. Treat
    // every value except an explicit "poll" as the safe notification path,
    // matching the CLI's default normalization and new-install property.
    settings_.capture_mode =
        std::string(propertyValue(capture_mode, "notify")) == "poll"
            ? CaptureMode::kPoll
            : CaptureMode::kNotify;
    settings_.frontlight_enabled = parseInteger(frontlight_enabled, 1) != 0;
    settings_.android_brightness = std::clamp(
        parseInteger(android_brightness, 128), 0, kAndroidBrightnessMax);
    settings_.frontlight_override =
        std::clamp(parseInteger(frontlight_override, -1), -1, 100);
    settings_.frontlight_temperature =
        std::clamp(parseInteger(frontlight_temperature, 0), 0, 100);
    settings_.foreground_token = propertyValue(active_package);
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
  CachedStringProperty active_package_{kActivePackageProperty};
  CachedStringProperty global_mode_{kRefreshModeProperty};
  CachedStringProperty idle_{kIdlePolicyProperty};
  CachedStringProperty cleanup_{kCleanupPolicyProperty};
  CachedStringProperty content_aware_{kContentAwareProperty};
  CachedStringProperty scroll_detect_{kScrollDetectProperty};
  CachedStringProperty active_page_interval_{kActivePageIntervalProperty};
  CachedStringProperty page_interval_{kPageIntervalProperty};
  CachedStringProperty settled_quality_{kSettledQualityProperty};
  CachedStringProperty active_contrast_{kActiveContrastProperty};
  CachedStringProperty contrast_{kContrastProperty};
  CachedStringProperty active_gamma_{kActiveGammaProperty};
  CachedStringProperty gamma_{kGammaProperty};
  CachedStringProperty active_dither_{kActiveDitherProperty};
  CachedStringProperty dither_{kDitherProperty};
  CachedStringProperty clear_on_sleep_{kClearOnSleepProperty};
  CachedStringProperty sleep_screen_{kSleepScreenProperty};
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
  std::optional<ChangedRect> damage_hint;
  std::optional<ChangedRect> transient_hint;
  int64_t trigger_us = 0;
};

struct NotifierDamage {
  std::optional<ChangedRect> damage;
  std::optional<ChangedRect> transient_hint;
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
  virtual void onWake(uint32_t) {}
  virtual int64_t nextWakeDeadlineUs() const { return 0; }
  virtual void onFrameCompared(bool) {}
  virtual bool failed() { return false; }
  virtual bool hasPendingFrame() const { return false; }
  virtual void requestCapture() {}
  virtual void deferCaptureUntil(int64_t) {}
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
  explicit ScreenshotFrameSource(bool damage_aware = false)
      : damage_aware_(damage_aware) {}

  bool start() override {
    while (!(display_id_ = SurfaceComposerClient::getInternalDisplayId())) {
      ALOGW("SurfaceFlinger internal display is not ready");
      usleep(kRetryDelayUs);
    }
    if (damage_aware_) {
      display_token_ = SurfaceComposerClient::getInternalDisplayToken();
      if (display_token_ == nullptr) {
        ALOGW("SurfaceFlinger internal display token is not ready");
        return false;
      }
    }
    return true;
  }

  void stop() override {
    release();
    assembled_.clear();
    display_token_.clear();
    full_width_ = 0;
    full_height_ = 0;
  }

  bool acquire(Frame *frame) override { return acquire(frame, std::nullopt); }

  bool acquire(Frame *frame,
               const std::optional<ChangedRect> &requested_damage) {
    if (!display_id_) {
      return false;
    }

    Dataspace dataspace;
    sp<GraphicBuffer> buffer;
    std::optional<ChangedRect> capture_damage;
    status_t status = NO_ERROR;
    if (damage_aware_ && !assembled_.empty() && requested_damage.has_value()) {
      capture_damage = clippedDamage(*requested_damage);
    }
    if (capture_damage.has_value()) {
      const ChangedRect &rect = *capture_damage;
      status = ScreenshotClient::capture(
          display_token_, Dataspace::V0_SRGB,
          android::ui::PixelFormat::RGBA_8888,
          Rect(static_cast<int32_t>(rect.left), static_cast<int32_t>(rect.top),
               static_cast<int32_t>(rect.right),
               static_cast<int32_t>(rect.bottom)),
          rect.right - rect.left, rect.bottom - rect.top,
          /*useIdentityTransform=*/false, android::ui::Rotation::Rotation0,
          &buffer);
    } else {
      status = ScreenshotClient::capture(*display_id_, &dataspace, &buffer);
    }
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

    if (!damage_aware_) {
      held_ = buffer;
      frame->pixels = static_cast<const uint8_t *>(pixels);
      frame->width = buffer->getWidth();
      frame->height = buffer->getHeight();
      frame->stride = buffer->getStride();
      frame->format = buffer->getPixelFormat();
      frame->damage_hint.reset();
      return true;
    }

    if (!capture_damage.has_value()) {
      full_width_ = buffer->getWidth();
      full_height_ = buffer->getHeight();
      assembled_.resize(static_cast<size_t>(full_width_) * full_height_);
      copyIntoAssembly(static_cast<const uint8_t *>(pixels),
                       buffer->getStride(),
                       ChangedRect{0, 0, full_width_, full_height_});
    } else {
      const ChangedRect &rect = *capture_damage;
      if (buffer->getWidth() != rect.right - rect.left ||
          buffer->getHeight() != rect.bottom - rect.top) {
        ALOGW("cropped capture returned %ux%u for %ux%u damage",
              buffer->getWidth(), buffer->getHeight(), rect.right - rect.left,
              rect.bottom - rect.top);
        buffer->unlock();
        return acquire(frame, std::nullopt);
      }
      copyIntoAssembly(static_cast<const uint8_t *>(pixels),
                       buffer->getStride(), rect);
    }
    buffer->unlock();

    frame->pixels = reinterpret_cast<const uint8_t *>(assembled_.data());
    frame->width = full_width_;
    frame->height = full_height_;
    frame->stride = full_width_;
    frame->format = android::PIXEL_FORMAT_RGBA_8888;
    frame->damage_hint = capture_damage;
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
  std::optional<ChangedRect> clippedDamage(const ChangedRect &damage) const {
    ChangedRect result{
        std::min(damage.left, full_width_),
        std::min(damage.top, full_height_),
        std::min(damage.right, full_width_),
        std::min(damage.bottom, full_height_),
    };
    if (result.left >= result.right || result.top >= result.bottom) {
      return std::nullopt;
    }
    result.left = (result.left / kTileSize) * kTileSize;
    result.top = (result.top / kTileSize) * kTileSize;
    result.right = std::min(
        full_width_, ((result.right + kTileSize - 1) / kTileSize) * kTileSize);
    result.bottom =
        std::min(full_height_,
                 ((result.bottom + kTileSize - 1) / kTileSize) * kTileSize);
    return result;
  }

  void copyIntoAssembly(const uint8_t *pixels, uint32_t stride,
                        const ChangedRect &destination) {
    const uint32_t copy_width = destination.right - destination.left;
    const size_t copy_bytes =
        static_cast<size_t>(copy_width) * sizeof(uint32_t);
    for (uint32_t y = destination.top; y < destination.bottom; ++y) {
      const uint32_t source_y = y - destination.top;
      memcpy(assembled_.data() + static_cast<size_t>(y) * full_width_ +
                 destination.left,
             pixels + static_cast<size_t>(source_y) * stride * sizeof(uint32_t),
             copy_bytes);
    }
  }

  bool damage_aware_ = false;
  std::optional<PhysicalDisplayId> display_id_;
  sp<IBinder> display_token_;
  sp<GraphicBuffer> held_;
  std::vector<uint32_t> assembled_;
  uint32_t full_width_ = 0;
  uint32_t full_height_ = 0;
};

// Uses a private, device-specific SurfaceFlinger transaction to register an
// eventfd and atomically consume coalesced display damage. Capture and EBC
// submission remain on the known-good screenshot production path.
class NotifiedScreenshotFrameSource : public FrameSource {
public:
  explicit NotifiedScreenshotFrameSource(Waiter *waiter) : waiter_(waiter) {}
  ~NotifiedScreenshotFrameSource() override { stop(); }

  bool start() override {
    if (!screenshot_.start()) {
      return false;
    }

    event_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    stop_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (event_fd_ < 0 || stop_fd_ < 0) {
      ALOGW("cannot create frame-notifier eventfds: %s", strerror(errno));
      stop();
      return false;
    }

    surface_flinger_ = android::defaultServiceManager()->checkService(
        android::String16("SurfaceFlinger"));
    if (surface_flinger_ == nullptr) {
      ALOGW("SurfaceFlinger service is unavailable for frame notification");
      stop();
      return false;
    }

    death_observer_ = new DeathObserver(&failed_, waiter_);
    if (surface_flinger_->linkToDeath(death_observer_) != NO_ERROR) {
      ALOGW("cannot monitor SurfaceFlinger frame notifier lifetime");
      stop();
      return false;
    }
    linked_to_death_ = true;

    if (setRegistration(true) != NO_ERROR) {
      ALOGW("SurfaceFlinger rejected the Leaf3 frame notifier");
      stop();
      return false;
    }
    registered_ = true;

    running_.store(true, std::memory_order_release);
    notification_thread_ =
        std::thread(&NotifiedScreenshotFrameSource::notificationThread, this);
    force_initial_capture_ = true;
    next_safety_probe_us_ = monotonicMicros() + kNotifierSafetyProbeDelayUs;
    ALOGI("capture mode: SurfaceFlinger frame notification");
    return true;
  }

  void stop() override {
    screenshot_.release();

    if (registered_ && surface_flinger_ != nullptr) {
      (void)setRegistration(false);
      registered_ = false;
    }
    if (linked_to_death_ && surface_flinger_ != nullptr &&
        death_observer_ != nullptr) {
      surface_flinger_->unlinkToDeath(death_observer_);
      linked_to_death_ = false;
    }

    running_.store(false, std::memory_order_release);
    if (stop_fd_ >= 0) {
      const uint64_t signal = 1;
      ssize_t written;
      do {
        written = write(stop_fd_, &signal, sizeof(signal));
      } while (written < 0 && errno == EINTR);
    }
    if (notification_thread_.joinable()) {
      notification_thread_.join();
    }

    death_observer_.clear();
    surface_flinger_.clear();
    if (event_fd_ >= 0) {
      close(event_fd_);
      event_fd_ = -1;
    }
    if (stop_fd_ >= 0) {
      close(stop_fd_);
      stop_fd_ = -1;
    }
    pending_.store(false, std::memory_order_relaxed);
    pending_since_us_.store(0, std::memory_order_relaxed);
    input_probe_deadline_us_ = 0;
    next_safety_probe_us_ = 0;
    capture_retry_deadline_us_ = 0;
    capture_deferred_deadline_us_ = 0;
    capture_not_before_us_ = 0;
    miss_confirmation_deadline_us_ = 0;
    probe_generation_ = 0;
    miss_confirmation_generation_ = 0;
    current_capture_is_probe_ = false;
    force_initial_capture_ = false;
    screenshot_.stop();
  }

  bool acquire(Frame *frame) override {
    const int64_t now_us = monotonicMicros();
    const bool notification_pending = pending_.load(std::memory_order_acquire);
    const bool input_probe =
        input_probe_deadline_us_ != 0 && now_us >= input_probe_deadline_us_;
    const bool safety_probe =
        next_safety_probe_us_ != 0 && now_us >= next_safety_probe_us_;
    if (!force_initial_capture_ && !notification_pending && !input_probe &&
        !safety_probe) {
      return false;
    }
    // The readiness check and notification consumption happen in this method.
    // A notification racing with the caller's optimistic hasPendingFrame()
    // check therefore cannot capture a frame before the EBC gate opens.
    if (capture_not_before_us_ != 0 && now_us < capture_not_before_us_) {
      capture_deferred_deadline_us_ = capture_not_before_us_;
      return false;
    }
    capture_deferred_deadline_us_ = 0;
    capture_not_before_us_ = 0;

    const bool notification = pending_.exchange(false);
    const int64_t notification_trigger_us =
        notification ? pending_since_us_.exchange(0) : 0;

    current_capture_is_probe_ = !force_initial_capture_ && !notification &&
                                (input_probe || safety_probe);
    const bool initial_capture = force_initial_capture_;
    const uint64_t probe_generation =
        notification_generation_.load(std::memory_order_acquire);
    force_initial_capture_ = false;
    input_probe_deadline_us_ = 0;
    capture_retry_deadline_us_ = 0;

    const NotifierDamage notification_damage =
        notification ? takeDamage() : NotifierDamage{};
    if (!screenshot_.acquire(frame, notification_damage.damage)) {
      // Consuming the wake before capture is safe only after a frame has been
      // acquired. Preserve every trigger and retry soon without spinning.
      if (notification) {
        pending_.store(true, std::memory_order_release);
        int64_t empty = 0;
        (void)pending_since_us_.compare_exchange_strong(
            empty, notification_trigger_us, std::memory_order_release,
            std::memory_order_relaxed);
      }
      force_initial_capture_ |= initial_capture;
      if (input_probe) {
        input_probe_deadline_us_ = now_us + kNotifierCaptureRetryDelayUs;
      }
      if (safety_probe) {
        next_safety_probe_us_ = now_us + kNotifierCaptureRetryDelayUs;
      }
      capture_retry_deadline_us_ = now_us + kNotifierCaptureRetryDelayUs;
      current_capture_is_probe_ = false;
      return false;
    }

    frame->transient_hint = notification_damage.transient_hint;
    if (current_capture_is_probe_) {
      probe_generation_ = probe_generation;
    }
    frame->trigger_us = notification_trigger_us;
    next_safety_probe_us_ = now_us + kNotifierSafetyProbeDelayUs;
    return true;
  }

  void release() override { screenshot_.release(); }
  bool streaming() const override { return true; }
  bool hasPendingFrame() const override {
    return pending_.load(std::memory_order_acquire);
  }
  void requestCapture() override { force_initial_capture_ = true; }
  void deferCaptureUntil(int64_t deadline_us) override {
    capture_not_before_us_ = std::max(capture_not_before_us_, deadline_us);
  }
  uint64_t dropped() const override {
    return dropped_.load(std::memory_order_relaxed);
  }

  void onWake(uint32_t reasons) override {
    if ((reasons & Waiter::kFrame) != 0) {
      input_probe_deadline_us_ = 0;
      return;
    }
    if ((reasons & Waiter::kInput) != 0) {
      input_probe_deadline_us_ = monotonicMicros() + kNotifierInputProbeDelayUs;
    }
  }

  int64_t nextWakeDeadlineUs() const override {
    int64_t deadline_us = 0;
    const bool capture_deferred = capture_deferred_deadline_us_ != 0;
    const int64_t deadlines[] = {
        capture_deferred ? 0 : input_probe_deadline_us_,
        capture_deferred ? 0 : next_safety_probe_us_,
        capture_deferred ? 0 : capture_retry_deadline_us_,
        capture_deferred_deadline_us_,
        miss_confirmation_deadline_us_,
    };
    for (const int64_t candidate_us : deadlines) {
      if (candidate_us != 0 &&
          (deadline_us == 0 || candidate_us < deadline_us)) {
        deadline_us = candidate_us;
      }
    }
    return deadline_us;
  }

  void onFrameCompared(bool changed) override {
    if (current_capture_is_probe_ && changed &&
        notification_generation_.load(std::memory_order_acquire) ==
            probe_generation_) {
      // SurfaceFlinger may have committed the captured frame but not yet
      // reached its postFrame() eventfd write. Give that real notification a
      // bounded opportunity to arrive before declaring the notifier stalled.
      miss_confirmation_generation_ = probe_generation_;
      miss_confirmation_deadline_us_ =
          monotonicMicros() + kNotifierRaceGraceDelayUs;
      ALOGW("probe found an unnotified change; awaiting notifier correlation");
    }
    current_capture_is_probe_ = false;
  }

  bool failed() override {
    if (miss_confirmation_deadline_us_ != 0) {
      if (notification_generation_.load(std::memory_order_acquire) !=
          miss_confirmation_generation_) {
        ALOGI("probe damage correlated with a racing frame notification");
        miss_confirmation_deadline_us_ = 0;
      } else if (monotonicMicros() >= miss_confirmation_deadline_us_) {
        ALOGE(
            "frame notifier missed a visible change; falling back to polling");
        miss_confirmation_deadline_us_ = 0;
        failed_.store(true, std::memory_order_release);
      }
    }
    return failed_.load(std::memory_order_acquire);
  }

private:
  class DeathObserver : public IBinder::DeathRecipient {
  public:
    DeathObserver(std::atomic<bool> *failed, Waiter *waiter)
        : failed_(failed), waiter_(waiter) {}

    void binderDied(const android::wp<IBinder> &) override {
      ALOGE("SurfaceFlinger died while frame notification was active");
      failed_->store(true, std::memory_order_release);
      waiter_->signal(Waiter::kFrame);
    }

  private:
    std::atomic<bool> *failed_;
    Waiter *waiter_;
  };

  status_t setRegistration(bool enable) {
    if (surface_flinger_ == nullptr) {
      return android::DEAD_OBJECT;
    }

    android::Parcel data;
    android::Parcel reply;
    data.writeInterfaceToken(android::String16("android.ui.ISurfaceComposer"));
    data.writeInt32(kLeaf3FrameNotifierVersion);
    data.writeInt32(enable ? kLeaf3FrameNotifierRegister
                           : kLeaf3FrameNotifierUnregister);
    if (enable) {
      data.writeFileDescriptor(event_fd_);
    }

    const status_t status = surface_flinger_->transact(
        kLeaf3FrameNotifierTransaction, data, &reply);
    if (status != NO_ERROR) {
      return status;
    }
    return static_cast<status_t>(reply.readInt32());
  }

  NotifierDamage takeDamage() {
    NotifierDamage result;
    if (surface_flinger_ == nullptr) {
      return result;
    }

    android::Parcel data;
    android::Parcel reply;
    data.writeInterfaceToken(android::String16("android.ui.ISurfaceComposer"));
    data.writeInt32(kLeaf3FrameNotifierVersion);
    data.writeInt32(kLeaf3FrameNotifierTakeDamage);
    const status_t status = surface_flinger_->transact(
        kLeaf3FrameNotifierTransaction, data, &reply);
    if (status != NO_ERROR) {
      return result;
    }

    if (reply.readInt32() != 0) {
      const int32_t left = reply.readInt32();
      const int32_t top = reply.readInt32();
      const int32_t right = reply.readInt32();
      const int32_t bottom = reply.readInt32();
      if (left < 0 || top < 0 || right <= left || bottom <= top) {
        ALOGW("SurfaceFlinger returned invalid damage [%d,%d,%d,%d]", left, top,
              right, bottom);
      } else {
        result.damage = ChangedRect{
            static_cast<uint32_t>(left),
            static_cast<uint32_t>(top),
            static_cast<uint32_t>(right),
            static_cast<uint32_t>(bottom),
        };
      }
    }
    if (reply.readInt32() != 0) {
      const int32_t left = reply.readInt32();
      const int32_t top = reply.readInt32();
      const int32_t right = reply.readInt32();
      const int32_t bottom = reply.readInt32();
      if (left >= 0 && top >= 0 && right > left && bottom > top) {
        result.transient_hint = ChangedRect{
            static_cast<uint32_t>(left),
            static_cast<uint32_t>(top),
            static_cast<uint32_t>(right),
            static_cast<uint32_t>(bottom),
        };
      }
    }
    return result;
  }

  void notificationThread() {
    pollfd descriptors[2] = {
        {event_fd_, POLLIN, 0},
        {stop_fd_, POLLIN, 0},
    };
    while (running_.load(std::memory_order_acquire)) {
      const int result = poll(descriptors, 2, -1);
      if (result < 0) {
        if (errno == EINTR) {
          continue;
        }
        ALOGE("frame notifier poll failed: %s", strerror(errno));
        failed_.store(true, std::memory_order_release);
        waiter_->signal(Waiter::kFrame);
        return;
      }
      if ((descriptors[1].revents & POLLIN) != 0) {
        return;
      }
      if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        ALOGE("frame notifier eventfd became invalid");
        failed_.store(true, std::memory_order_release);
        waiter_->signal(Waiter::kFrame);
        return;
      }
      if ((descriptors[0].revents & POLLIN) == 0) {
        continue;
      }

      uint64_t count = 0;
      if (read(event_fd_, &count, sizeof(count)) !=
          static_cast<ssize_t>(sizeof(count))) {
        if (errno == EAGAIN) {
          continue;
        }
        ALOGE("frame notifier read failed: %s", strerror(errno));
        failed_.store(true, std::memory_order_release);
        waiter_->signal(Waiter::kFrame);
        return;
      }
      notification_generation_.fetch_add(count, std::memory_order_release);
      int64_t no_pending_timestamp = 0;
      const int64_t now_us = monotonicMicros();
      (void)pending_since_us_.compare_exchange_strong(
          no_pending_timestamp, now_us, std::memory_order_release,
          std::memory_order_relaxed);
      const bool already_pending =
          pending_.exchange(true, std::memory_order_acq_rel);
      const uint64_t coalesced = already_pending ? count
                                 : count > 0     ? count - 1
                                                 : 0;
      dropped_.fetch_add(coalesced, std::memory_order_relaxed);
      waiter_->signal(Waiter::kFrame);
    }
  }

  Waiter *waiter_;
  ScreenshotFrameSource screenshot_{/*damage_aware=*/true};
  sp<IBinder> surface_flinger_;
  sp<DeathObserver> death_observer_;
  std::thread notification_thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> pending_{false};
  std::atomic<bool> failed_{false};
  std::atomic<uint64_t> dropped_{0};
  std::atomic<uint64_t> notification_generation_{0};
  std::atomic<int64_t> pending_since_us_{0};
  int event_fd_ = -1;
  int stop_fd_ = -1;
  int64_t input_probe_deadline_us_ = 0;
  int64_t next_safety_probe_us_ = 0;
  int64_t capture_retry_deadline_us_ = 0;
  int64_t capture_deferred_deadline_us_ = 0;
  int64_t capture_not_before_us_ = 0;
  int64_t miss_confirmation_deadline_us_ = 0;
  uint64_t probe_generation_ = 0;
  uint64_t miss_confirmation_generation_ = 0;
  bool force_initial_capture_ = false;
  bool current_capture_is_probe_ = false;
  bool registered_ = false;
  bool linked_to_death_ = false;
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

  void configureToneMapping(int contrast, int gamma) {
    if (contrast == contrast_ && gamma == gamma_) {
      return;
    }
    contrast_ = contrast;
    gamma_ = gamma;
    tone_mapping_enabled_ = contrast_ != 0 || gamma_ != 100;

    const float contrast_scale =
        (259.0f * (static_cast<float>(contrast_) + 255.0f)) /
        (255.0f * (259.0f - static_cast<float>(contrast_)));
    const float gamma_exponent = 100.0f / static_cast<float>(gamma_);
    for (size_t value = 0; value < tone_lut_.size(); ++value) {
      const float contrasted = std::clamp(
          contrast_scale * (static_cast<float>(value) - 128.0f) + 128.0f, 0.0f,
          255.0f);
      const float corrected =
          powf(contrasted / 255.0f, gamma_exponent) * 255.0f;
      tone_lut_[value] =
          static_cast<uint8_t>(std::clamp(lroundf(corrected), 0L, 255L));
    }
    ALOGI("tone mapping contrast=%d gamma=%d", contrast_, gamma_);
  }

  int64_t nextUpdateDeadlineUs() const {
    return last_update_us_ == 0 ? 0
                                : last_update_us_ + kMinimumEbcUpdateIntervalUs;
  }

  uint64_t gateWaitTimeUs() const { return gate_wait_time_us_; }
  uint64_t ioctlTimeUs() const { return ioctl_time_us_; }

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
      uint32_t *destination = static_cast<uint32_t *>(buffer_) +
                              static_cast<size_t>(y) * width_ + rect.left;
      const uint32_t *source = reinterpret_cast<const uint32_t *>(pixels) +
                               static_cast<size_t>(y) * stride + rect.left;
      if (!tone_mapping_enabled_) {
        memcpy(destination, source, copy_bytes);
        continue;
      }
      for (uint32_t x = rect.left; x < rect.right; ++x) {
        const uint32_t pixel = *source++;
        const uint32_t luminance =
            ((pixel & 0xffu) * 77u + ((pixel >> 8) & 0xffu) * 150u +
             ((pixel >> 16) & 0xffu) * 29u) >>
            8;
        const uint32_t adjusted = tone_lut_[luminance];
        *destination++ =
            0xff000000u | adjusted | (adjusted << 8) | (adjusted << 16);
      }
    }
  }

  bool sendStagedUpdate(const ChangedRect &rect, uint32_t waveform, bool dither,
                        bool full_refresh) {
    return sendUpdate(rect, waveform, dither, full_refresh);
  }

  // Re-issues a waveform over the frame already held in the persistent buffer,
  // so a manual cleanup does not have to wait for the compositor to produce
  // another frame.
  bool refreshFull(uint32_t waveform, bool dither) {
    if (buffer_ == MAP_FAILED) {
      return false;
    }
    return sendUpdate(ChangedRect{0, 0, width_, height_}, waveform, dither,
                      /*full_refresh=*/true);
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

  bool showSleepImage() {
    if (buffer_ == MAP_FAILED) {
      return false;
    }

    const int image_fd = open(kSleepScreenImagePath, O_RDONLY | O_CLOEXEC);
    if (image_fd < 0) {
      ALOGE("could not open sleep image %s: %s", kSleepScreenImagePath,
            strerror(errno));
      return false;
    }
    struct stat image_stat = {};
    const bool valid_size =
        fstat(image_fd, &image_stat) == 0 &&
        image_stat.st_size == static_cast<off_t>(buffer_size_);
    if (!valid_size) {
      ALOGE("sleep image has an invalid size");
      close(image_fd);
      return false;
    }

    uint8_t *destination = static_cast<uint8_t *>(buffer_);
    size_t remaining = buffer_size_;
    while (remaining != 0) {
      const ssize_t bytes_read = read(image_fd, destination, remaining);
      if (bytes_read <= 0) {
        ALOGE("could not read sleep image: %s",
              bytes_read == 0 ? "unexpected end of file" : strerror(errno));
        close(image_fd);
        return false;
      }
      destination += bytes_read;
      remaining -= static_cast<size_t>(bytes_read);
    }
    close(image_fd);
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
        gate_wait_time_us_ += static_cast<uint64_t>(remaining);
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

    const int64_t ioctl_start_us = monotonicMicros();
    if (ioctl(fd_, kEbcSendUpdate, &update) < 0) {
      ALOGE("EBC SEND_UPDATE marker=%u failed: %s", update.update_marker,
            strerror(errno));
      return false;
    }
    ioctl_time_us_ += static_cast<uint64_t>(monotonicMicros() - ioctl_start_us);
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
  std::array<uint8_t, 256> tone_lut_{};
  int contrast_ = 0;
  int gamma_ = 100;
  bool tone_mapping_enabled_ = false;
  uint64_t gate_wait_time_us_ = 0;
  uint64_t ioctl_time_us_ = 0;
};

ChangedRect unionRects(const ChangedRect &left, const ChangedRect &right) {
  return ChangedRect{
      std::min(left.left, right.left),
      std::min(left.top, right.top),
      std::max(left.right, right.right),
      std::max(left.bottom, right.bottom),
  };
}

bool overlaps(const ChangedRect &left, const ChangedRect &right) {
  return left.left < right.right && right.left < left.right &&
         left.top < right.bottom && right.top < left.bottom;
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
               const std::vector<uint32_t> &previous,
               const std::optional<ChangedRect> &limit = std::nullopt) {
    std::fill(tiles_.begin(), tiles_.end(), 0);
    bool any = false;
    const uint32_t left = limit.has_value() ? limit->left : 0;
    const uint32_t top = limit.has_value() ? limit->top : 0;
    const uint32_t right = limit.has_value() ? limit->right : width_;
    const uint32_t bottom = limit.has_value() ? limit->bottom : height_;
    if (left >= right || top >= bottom || right > width_ || bottom > height_) {
      return false;
    }
    const uint32_t first_column = left / kTileSize;
    const uint32_t last_column =
        std::min(columns_, (right + kTileSize - 1) / kTileSize);
    const size_t row_bytes =
        static_cast<size_t>(right - left) * sizeof(uint32_t);
    const size_t row_offset = static_cast<size_t>(left) * sizeof(uint32_t);

    for (uint32_t y = top; y < bottom; ++y) {
      const uint8_t *row =
          pixels + static_cast<size_t>(y) * stride * sizeof(uint32_t);
      const auto *old_row = reinterpret_cast<const uint8_t *>(
          previous.data() + static_cast<size_t>(y) * width_);
      // bionic's memcmp is SIMD, so an unchanged row costs one wide pass and
      // no per-pixel work at all.
      if (memcmp(row + row_offset, old_row + row_offset, row_bytes) == 0) {
        continue;
      }

      uint8_t *tile_row =
          tiles_.data() + static_cast<size_t>(y / kTileSize) * columns_;
      for (uint32_t column = first_column; column < last_column; ++column) {
        if (tile_row[column] != 0) {
          continue;
        }
        const uint32_t x = column * kTileSize;
        const uint32_t tile_left = std::max(x, left);
        const uint32_t tile_right = std::min(x + kTileSize, right);
        const size_t bytes =
            static_cast<size_t>(tile_right - tile_left) * sizeof(uint32_t);
        const size_t offset = static_cast<size_t>(tile_left) * sizeof(uint32_t);
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

  bool intersectsDirty(const ChangedRect &region) const {
    const uint32_t right = std::min(width_, region.right);
    const uint32_t bottom = std::min(height_, region.bottom);
    if (region.left >= right || region.top >= bottom) {
      return false;
    }
    const uint32_t first_column = region.left / kTileSize;
    const uint32_t last_column =
        std::min(columns_, (right + kTileSize - 1) / kTileSize);
    const uint32_t first_row = region.top / kTileSize;
    const uint32_t last_row =
        std::min(rows_, (bottom + kTileSize - 1) / kTileSize);
    for (uint32_t row = first_row; row < last_row; ++row) {
      for (uint32_t column = first_column; column < last_column; ++column) {
        if (tiles_[static_cast<size_t>(row) * columns_ + column] != 0) {
          return true;
        }
      }
    }
    return false;
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

// Ages tiles touched by fast waveforms. Cleanup starts with the oldest tile and
// grows through adjacent dirty tiles without crossing the automatic area cap,
// avoiding a large bounding box between unrelated parts of the screen.
class GhostMap {
public:
  void resize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    columns_ = (width + kTileSize - 1) / kTileSize;
    rows_ = (height + kTileSize - 1) / kTileSize;
    ages_.assign(static_cast<size_t>(columns_) * rows_, 0);
  }

  void clear() { std::fill(ages_.begin(), ages_.end(), 0); }

  void clear(const ChangedRect &rect) {
    forEachTile(rect, [&](size_t index) { ages_[index] = 0; });
  }

  void markFast(const ChangedRect &rect) {
    forEachTile(rect, [&](size_t index) {
      ages_[index] =
          static_cast<uint8_t>(std::min<int>(UINT8_MAX, ages_[index] + 1));
    });
  }

  bool hasDirty() const {
    return std::any_of(ages_.begin(), ages_.end(),
                       [](uint8_t age) { return age != 0; });
  }

  ChangedRect nextCleanup(uint64_t maximum_area) const {
    if (!hasDirty()) {
      return {};
    }

    const auto seed_iterator = std::max_element(ages_.begin(), ages_.end());
    const size_t seed = static_cast<size_t>(seed_iterator - ages_.begin());
    const uint32_t seed_row = static_cast<uint32_t>(seed / columns_);
    const uint32_t seed_column = static_cast<uint32_t>(seed % columns_);
    ChangedRect result = tileRect(seed_column, seed_row);

    std::vector<uint8_t> visited(ages_.size(), 0);
    std::vector<size_t> queue;
    queue.push_back(seed);
    visited[seed] = 1;
    for (size_t cursor = 0; cursor < queue.size(); ++cursor) {
      const size_t index = queue[cursor];
      const uint32_t row = static_cast<uint32_t>(index / columns_);
      const uint32_t column = static_cast<uint32_t>(index % columns_);
      const int32_t neighbor_columns[] = {
          static_cast<int32_t>(column) - 1,
          static_cast<int32_t>(column) + 1,
          static_cast<int32_t>(column),
          static_cast<int32_t>(column),
      };
      const int32_t neighbor_rows[] = {
          static_cast<int32_t>(row),
          static_cast<int32_t>(row),
          static_cast<int32_t>(row) - 1,
          static_cast<int32_t>(row) + 1,
      };
      for (size_t neighbor = 0; neighbor < 4; ++neighbor) {
        const int32_t next_column = neighbor_columns[neighbor];
        const int32_t next_row = neighbor_rows[neighbor];
        if (next_column < 0 || next_row < 0 ||
            next_column >= static_cast<int32_t>(columns_) ||
            next_row >= static_cast<int32_t>(rows_)) {
          continue;
        }
        const size_t next_index =
            static_cast<size_t>(next_row) * columns_ + next_column;
        if (visited[next_index] != 0 || ages_[next_index] == 0) {
          continue;
        }
        visited[next_index] = 1;
        const ChangedRect candidate =
            unionRects(result, tileRect(static_cast<uint32_t>(next_column),
                                        static_cast<uint32_t>(next_row)));
        if (candidate.area() > maximum_area) {
          continue;
        }
        result = candidate;
        queue.push_back(next_index);
      }
    }
    return result;
  }

private:
  template <typename Callback>
  void forEachTile(const ChangedRect &rect, Callback callback) {
    const uint32_t first_column = std::min(columns_, rect.left / kTileSize);
    const uint32_t last_column =
        std::min(columns_, (rect.right + kTileSize - 1) / kTileSize);
    const uint32_t first_row = std::min(rows_, rect.top / kTileSize);
    const uint32_t last_row =
        std::min(rows_, (rect.bottom + kTileSize - 1) / kTileSize);
    for (uint32_t row = first_row; row < last_row; ++row) {
      for (uint32_t column = first_column; column < last_column; ++column) {
        callback(static_cast<size_t>(row) * columns_ + column);
      }
    }
  }

  ChangedRect tileRect(uint32_t column, uint32_t row) const {
    return ChangedRect{
        column * kTileSize,
        row * kTileSize,
        std::min(width_, (column + 1) * kTileSize),
        std::min(height_, (row + 1) * kTileSize),
    };
  }

  std::vector<uint8_t> ages_;
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
  publishStat("sys.leaf3.stat.captures_cropped", stats->cropped_captures,
              &success);
  publishStat("sys.leaf3.stat.capture_pixels", stats->captured_pixels,
              &success);
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
  publishStat("sys.leaf3.stat.cleanup", stats->cleanup_updates, &success);
  publishStat("sys.leaf3.stat.page_turns", stats->page_turns, &success);
  publishStat("sys.leaf3.stat.page_cleanups", stats->page_cleanups, &success);
  publishStat("sys.leaf3.stat.settled", stats->settled_updates, &success);
  publishStat("sys.leaf3.stat.capture_us", stats->capture_time_us, &success);
  publishStat("sys.leaf3.stat.compare_us", stats->compare_time_us, &success);
  publishStat("sys.leaf3.stat.submit_us", stats->submit_time_us, &success);
  publishStat("sys.leaf3.stat.ioctl_us", stats->ioctl_time_us, &success);
  publishStat("sys.leaf3.stat.gate_wait_us", stats->gate_wait_time_us,
              &success);
  publishStat("sys.leaf3.stat.notify_capture_us",
              stats->notification_to_capture_us, &success);
  publishStat("sys.leaf3.stat.notify_submit_us",
              stats->notification_to_submit_us, &success);
  publishStat("sys.leaf3.stat.notified_captures", stats->notified_captures,
              &success);
  publishStat("sys.leaf3.stat.notified_submits", stats->notified_submits,
              &success);
  if (!success) {
    ALOGW("one or more bridge statistics properties could not be published");
  }
  ALOGI("stats captures=%llu comparisons=%llu changed=%llu partial=%llu "
        "full=%llu pixels=%llu dropped=%llu split=%llu bilevel=%llu "
        "scroll=%llu gesture_scroll=%llu hash_scroll=%llu page_turns=%llu "
        "page_cleanups=%llu settled=%llu capture_us=%llu compare_us=%llu "
        "submit_us=%llu ioctl_us=%llu gate_wait_us=%llu notify_capture_us=%llu "
        "notify_submit_us=%llu",
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
        static_cast<unsigned long long>(stats->page_turns),
        static_cast<unsigned long long>(stats->page_cleanups),
        static_cast<unsigned long long>(stats->settled_updates),
        static_cast<unsigned long long>(stats->capture_time_us),
        static_cast<unsigned long long>(stats->compare_time_us),
        static_cast<unsigned long long>(stats->submit_time_us),
        static_cast<unsigned long long>(stats->ioctl_time_us),
        static_cast<unsigned long long>(stats->gate_wait_time_us),
        static_cast<unsigned long long>(stats->notification_to_capture_us),
        static_cast<unsigned long long>(stats->notification_to_submit_us));
  stats->last_publish_us = now_us;
}

useconds_t activeFrameDelay(RefreshMode mode) {
  switch (mode) {
  case RefreshMode::kSpeed:
  case RefreshMode::kA2:
  case RefreshMode::kReader:
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

std::unique_ptr<FrameSource> startFrameSource(CaptureMode mode,
                                              Waiter *waiter) {
  if (mode == CaptureMode::kNotify) {
    auto notified = std::make_unique<NotifiedScreenshotFrameSource>(waiter);
    if (notified->start()) {
      android::base::SetProperty("sys.leaf3.stat.capture_mode", "notify");
      return notified;
    }
    ALOGW("frame notification unavailable; falling back to polling");
  }

  auto screenshot = std::make_unique<ScreenshotFrameSource>();
  screenshot->start();
  android::base::SetProperty("sys.leaf3.stat.capture_mode", "poll");
  ALOGI("capture mode: periodic screenshot");
  return screenshot;
}

status_t requestComposerCommand(int32_t command) {
  const sp<IBinder> surface_flinger =
      android::defaultServiceManager()->checkService(
          android::String16("SurfaceFlinger"));
  if (surface_flinger == nullptr) {
    return android::NAME_NOT_FOUND;
  }

  android::Parcel data;
  android::Parcel reply;
  data.writeInterfaceToken(android::String16("android.ui.ISurfaceComposer"));
  data.writeInt32(kLeaf3FrameNotifierVersion);
  data.writeInt32(command);
  const status_t status =
      surface_flinger->transact(kLeaf3FrameNotifierTransaction, data, &reply);
  return status == NO_ERROR ? static_cast<status_t>(reply.readInt32()) : status;
}

status_t requestComposerFullRefresh() {
  return requestComposerCommand(kLeaf3FrameNotifierRequestFullRefresh);
}

bool blockComposerAndWaitForIdle() {
  const status_t status =
      requestComposerCommand(kLeaf3FrameNotifierBlockAndWait);
  if (status == NO_ERROR) {
    return true;
  }
  if (status == android::NAME_NOT_FOUND || status == android::DEAD_OBJECT) {
    // A dead SurfaceFlinger cannot have a live native submission. The boot
    // block property prevents its replacement from enabling the native path.
    return true;
  }
  ALOGE("could not confirm composer-native EPDC idle state: %d; "
        "direct-EBC fallback remains inhibited",
        status);
  return false;
}

bool waitForPropertyChange(uint32_t *serial, int timeout_seconds) {
  timespec timeout = {timeout_seconds, 0};
  uint32_t next = *serial;
  if (!__system_property_wait(nullptr, *serial, &next, &timeout)) {
    return false;
  }
  *serial = next;
  return true;
}

// Composer-native mode still needs the ONYX frontlight bridge, but it must not
// touch /dev/ebc or capture the display. Returning false requests a one-way
// fallback to the existing direct-EBC loop for the remainder of this boot.
bool runComposerSupportLoop() {
  SettingsCache settings_cache;
  FrontlightBridge frontlight;
  Settings settings = settings_cache.read();
  std::string full_refresh_token = settings.full_refresh_token;
  bool was_interactive = settings.interactive;
  uint32_t serial = __system_property_area_serial();
  const int64_t ready_deadline_us = monotonicMicros() + 5000000;
  bool ready = false;
  bool warned_sleep_screen = false;

  for (;;) {
    settings = settings_cache.read();
    frontlight.update(settings, settings.interactive);

    const bool blocked =
        android::base::GetBoolProperty(kEpdcNativeBlockedProperty, false);
    const std::string state =
        android::base::GetProperty(kEpdcNativeStateProperty, "probing");
    if (blocked || state == "failed" || state == "unsupported") {
      android::base::SetProperty(kEpdcNativeBlockedProperty, "1");
      if (!blockComposerAndWaitForIdle()) {
        (void)waitForPropertyChange(&serial, 1);
        continue;
      }
      android::base::SetProperty(kEpdcActiveBackendProperty, "bridge-fallback");
      ALOGE("composer-native EPDC is %s; entering direct-EBC fallback",
            blocked ? "blocked" : state.c_str());
      return false;
    }
    if (!ready && state == "ready") {
      ready = true;
      android::base::SetProperty(kEpdcActiveBackendProperty, "composer");
      ALOGI("EPDC backend: composer-native (frontlight-only bridge)");
    }
    if (!ready && monotonicMicros() >= ready_deadline_us) {
      android::base::SetProperty(kEpdcNativeBlockedProperty, "1");
      ALOGE("composer-native EPDC readiness timed out; blocking native path");
      continue;
    }

    if (ready && settings.full_refresh_token != full_refresh_token) {
      full_refresh_token = settings.full_refresh_token;
      const status_t status = requestComposerFullRefresh();
      if (status != NO_ERROR) {
        ALOGE("composer full-refresh request failed: %d", status);
        android::base::SetProperty(kEpdcNativeBlockedProperty, "1");
        continue;
      }
    }
    if (ready && settings.interactive && !was_interactive) {
      const status_t status = requestComposerFullRefresh();
      if (status != NO_ERROR) {
        ALOGE("composer wake refresh request failed: %d", status);
        android::base::SetProperty(kEpdcNativeBlockedProperty, "1");
        continue;
      }
    }
    was_interactive = settings.interactive;
    if (ready && settings.sleep_screen != SleepScreen::kRetain &&
        !warned_sleep_screen) {
      ALOGW("sleep-screen rendering is unavailable in composer-native mode; "
            "the retained panel image will remain");
      warned_sleep_screen = true;
    }

    if (!ready) {
      (void)waitForPropertyChange(&serial, 1);
    } else if (!settings.interactive) {
      android::base::WaitForProperty(kInteractiveProperty, "1");
      serial = __system_property_area_serial();
    } else {
      uint32_t next = serial;
      if (__system_property_wait(nullptr, serial, &next, nullptr)) {
        serial = next;
      }
    }
  }
}

} // namespace

int main() {
  ProcessState::self()->setThreadPoolMaxThreadCount(4);
  ProcessState::self()->startThreadPool();

  const bool composer_requested =
      android::base::GetProperty(kEpdcBackendProperty, "bridge") == "composer";
  if (composer_requested) {
    (void)runComposerSupportLoop();
  } else if (android::base::GetBoolProperty(kEpdcNativeBlockedProperty,
                                            false)) {
    // A service restart can observe the persistent bridge selection after an
    // emergency rollback. Reconfirm the native writer is drained before this
    // new process opens /dev/ebc.
    while (!blockComposerAndWaitForIdle()) {
      sleep(1);
    }
  }
  android::base::SetProperty(kEpdcActiveBackendProperty,
                             composer_requested ? "bridge-fallback" : "bridge");

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
  GhostMap ghost;
  ScrollDetector scroll;
  BridgeStats stats;
  std::vector<uint32_t> previous;

  SettingsCache settings_cache;
  Settings settings = settings_cache.read();
  std::unique_ptr<FrameSource> source;
  if (settings.interactive) {
    source = startFrameSource(settings.capture_mode, &waiter);
  }

  bool initialized = false;
  bool first_refresh = true;
  bool display_was_on = settings.interactive;
  bool pending_full_refresh = false;
  bool cleanup_pending = false;
  bool scroll_in_progress = false;
  bool tone_reprocess_pending = false;
  CaptureMode source_requested_mode = settings.capture_mode;
  uint32_t unchanged_frames = 0;
  uint32_t idle_poll_count = 0;
  uint32_t input_probe_frames = 0;
  uint64_t source_dropped_seen = 0;
  uint32_t panel_width = 0;
  uint32_t panel_height = 0;
  uint64_t panel_pixels = 0;
  uint64_t maximum_cleanup_area = 0;
  int64_t cleanup_deadline_us = 0;
  int64_t last_scroll_activity_us = 0;
  int64_t settled_quality_deadline_us = 0;
  std::optional<ChangedRect> settled_quality_damage;
  int page_turn_count = 0;
  int applied_contrast = 1000;
  int applied_gamma = 0;
  int applied_dither = -1;
  int applied_page_interval = -1;
  std::string active_foreground_token = settings.foreground_token;
  uint64_t accounted_gate_wait_us = 0;
  uint64_t accounted_ioctl_us = 0;
  std::string full_refresh_token = settings.full_refresh_token;
  RefreshMode active_mode = settings.mode;
  ALOGI("refresh mode: %s", refreshModeName(active_mode));

  for (;;) {
    settings = settings_cache.read();
    frontlight.update(settings, settings.interactive);
    if (settings.contrast != applied_contrast ||
        settings.gamma != applied_gamma ||
        static_cast<int>(settings.dither) != applied_dither) {
      applied_contrast = settings.contrast;
      applied_gamma = settings.gamma;
      applied_dither = settings.dither ? 1 : 0;
      ebc.configureToneMapping(applied_contrast, applied_gamma);
      tone_reprocess_pending = initialized;
      if (source != nullptr) {
        source->requestCapture();
      }
    }
    if (!settings.settled_quality) {
      settled_quality_damage.reset();
      settled_quality_deadline_us = 0;
    }
    if (settings.page_interval != applied_page_interval) {
      applied_page_interval = settings.page_interval;
      page_turn_count = 0;
    }
    if (settings.foreground_token != active_foreground_token) {
      active_foreground_token = settings.foreground_token;
      page_turn_count = 0;
      settled_quality_damage.reset();
      settled_quality_deadline_us = 0;
    }

    if (cleanup_pending && settings.cleanup == CleanupPolicy::kManual) {
      cleanup_pending = false;
      ghost.clear();
      cleanup_deadline_us = 0;
    }
    if (settings.cleanup == CleanupPolicy::kManual) {
      page_turn_count = 0;
    }

    if (settings.full_refresh_token != full_refresh_token) {
      full_refresh_token = settings.full_refresh_token;
      pending_full_refresh = true;
      ALOGI("manual full refresh requested");
    }

    if (!settings.interactive) {
      if (display_was_on) {
        if (initialized && settings.sleep_screen != SleepScreen::kRetain) {
          ALOGI("display is off; rendering sleep screen");
          const int64_t submit_start_us = monotonicMicros();
          const bool rendered = settings.sleep_screen == SleepScreen::kImage
                                    ? ebc.showSleepImage()
                                    : ebc.clear();
          if (!rendered) {
            if (settings.sleep_screen != SleepScreen::kImage || !ebc.clear()) {
              return 1;
            }
            ALOGW("sleep image unavailable; cleared the panel instead");
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
        ghost.clear();
        scroll_in_progress = false;
        settled_quality_damage.reset();
        settled_quality_deadline_us = 0;
        page_turn_count = 0;
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
      source = startFrameSource(settings.capture_mode, &waiter);
      source_requested_mode = settings.capture_mode;
      source_dropped_seen = 0;
    }

    if (settings.capture_mode != source_requested_mode) {
      ALOGI("capture preference changed; recreating the frame source");
      source->stop();
      source = startFrameSource(settings.capture_mode, &waiter);
      source_requested_mode = settings.capture_mode;
      source_dropped_seen = 0;
      first_refresh = true;
      previous.clear();
      scroll.reset();
      cleanup_pending = false;
      ghost.clear();
      scroll_in_progress = false;
      settled_quality_damage.reset();
      settled_quality_deadline_us = 0;
      page_turn_count = 0;
      cleanup_deadline_us = 0;
    }

    if (source->failed()) {
      ALOGW("frame notification failed; continuing with periodic screenshots");
      source->stop();
      source = startFrameSource(CaptureMode::kPoll, &waiter);
      source_dropped_seen = 0;
      input_probe_frames = kInputProbeFrames;
      idle_poll_count = 0;
    }

    if (settings.mode != active_mode) {
      active_mode = settings.mode;
      cleanup_pending = false;
      ghost.clear();
      settled_quality_damage.reset();
      settled_quality_deadline_us = 0;
      page_turn_count = 0;
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
      if (!ebc.refreshFull(kWaveformGc16, settings.dither)) {
        return 1;
      }
      stats.submit_time_us +=
          static_cast<uint64_t>(monotonicMicros() - submit_start_us);
      ++stats.full_updates;
      stats.updated_pixels += panel_pixels;
      cleanup_pending = false;
      ghost.clear();
      scroll_in_progress = false;
      settled_quality_damage.reset();
      settled_quality_deadline_us = 0;
      page_turn_count = 0;
      cleanup_deadline_us = 0;
    }

    // Pace before capture, not after it. SurfaceFlinger continues to merge
    // damage while this loop waits, so the subsequent screenshot contains the
    // newest page rather than an already-stale frame captured at the start of
    // the EBC buffer interval.
    if (initialized) {
      source->deferCaptureUntil(ebc.nextUpdateDeadlineUs());
    }
    if (initialized && source->hasPendingFrame()) {
      const int64_t gate_start_us = monotonicMicros();
      for (;;) {
        const int64_t remaining_us =
            ebc.nextUpdateDeadlineUs() - monotonicMicros();
        if (remaining_us <= 0) {
          break;
        }
        const uint32_t reasons = waiter.wait(remaining_us);
        source->onWake(reasons);
        if ((reasons & Waiter::kInput) != 0) {
          input_probe_frames = kInputProbeFrames;
        }
      }
      const uint64_t waited_us =
          static_cast<uint64_t>(monotonicMicros() - gate_start_us);
      stats.gate_wait_time_us += waited_us;
    }

    if (settled_quality_damage.has_value() &&
        monotonicMicros() >= settled_quality_deadline_us &&
        !source->hasPendingFrame()) {
      const int64_t submit_start_us = monotonicMicros();
      if (!ebc.sendStagedUpdate(*settled_quality_damage, kWaveformRegal,
                                settings.dither,
                                /*full_refresh=*/false)) {
        return 1;
      }
      stats.submit_time_us +=
          static_cast<uint64_t>(monotonicMicros() - submit_start_us);
      ++stats.partial_updates;
      ++stats.settled_updates;
      stats.updated_pixels += settled_quality_damage->area();
      settled_quality_damage.reset();
      settled_quality_deadline_us = 0;
    }

    // A manual or settled-quality update above may have moved the EBC
    // deadline after the first pacing check. Carry the final deadline into
    // acquire() as well so a racing notification cannot capture too early.
    if (initialized) {
      source->deferCaptureUntil(ebc.nextUpdateDeadlineUs());
    }
    Frame frame;
    const int64_t capture_start_us = monotonicMicros();
    const bool have_frame = source->acquire(&frame);
    if (have_frame) {
      ++stats.captures;
      if (frame.damage_hint.has_value()) {
        ++stats.cropped_captures;
        stats.captured_pixels += frame.damage_hint->area();
      } else {
        stats.captured_pixels +=
            static_cast<uint64_t>(frame.width) * frame.height;
      }
      stats.capture_time_us +=
          static_cast<uint64_t>(monotonicMicros() - capture_start_us);
      if (frame.trigger_us != 0 && capture_start_us >= frame.trigger_us) {
        stats.notification_to_capture_us +=
            static_cast<uint64_t>(capture_start_us - frame.trigger_us);
        ++stats.notified_captures;
      }
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
        maximum_cleanup_area = std::max<uint64_t>(
            kTileSize * kTileSize,
            panel_pixels / kMaximumAutomaticCleanupAreaDenominator);
        damage.resize(frame.width, frame.height);
        ghost.resize(frame.width, frame.height);
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
      const bool tone_reprocess = tone_reprocess_pending && !full_refresh;

      std::vector<ChangedRect> rects;
      bool changed = false;
      if (full_refresh || tone_reprocess) {
        rects.push_back(ChangedRect{0, 0, frame.width, frame.height});
        changed = true;
      } else {
        const int64_t compare_start_us = monotonicMicros();
        changed = damage.compare(frame.pixels, frame.stride, previous,
                                 frame.damage_hint);
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

      const bool hinted_scrolling =
          changed && !full_refresh && !tone_reprocess &&
          frame.transient_hint.has_value() &&
          damage.intersectsDirty(*frame.transient_hint);
      bool scrolling = hinted_scrolling;
      if (changed && !full_refresh && !tone_reprocess &&
          settings.scroll_detect && !hinted_scrolling) {
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
      if (hinted_scrolling) {
        ++stats.scroll_frames;
      }
      if (changed) {
        // Narrow damage such as a scrollbar or caret is intentionally not
        // promoted to A2, but it must not erase a live drag or the extended
        // row-hash window of an established fling.
        scroll_in_progress =
            scrolling || (settings.scroll_detect &&
                          (gesture_scrolling || established_fling_live));
      }

      if (changed) {
        const ChangedRect changed_bounds =
            full_refresh ? ChangedRect{0, 0, frame.width, frame.height}
                         : damage.bounds();
        const uint64_t changed_area =
            full_refresh ? panel_pixels : damage.dirtyArea();
        const bool large_static_change =
            !full_refresh && !tone_reprocess && !scrolling &&
            changed_bounds.area() * kPageTurnMinimumAreaDenominator >=
                panel_pixels &&
            changed_area * kPageTurnMinimumAreaDenominator >= panel_pixels;
        const bool reader_page_turn =
            active_mode == RefreshMode::kReader && large_static_change;
        bool page_cleanup_due = false;
        if (reader_page_turn) {
          ++stats.page_turns;
          if (settings.cleanup != CleanupPolicy::kManual &&
              settings.page_interval > 0) {
            ++page_turn_count;
            if (page_turn_count >= settings.page_interval) {
              page_turn_count = 0;
              page_cleanup_due = true;
            }
          }
        }
        const bool settle_regal_page = active_mode == RefreshMode::kRegal &&
                                       settings.settled_quality &&
                                       large_static_change;
        if (settled_quality_damage.has_value() && !settle_regal_page) {
          if (scrolling) {
            settled_quality_damage.reset();
            settled_quality_deadline_us = 0;
          } else {
            settled_quality_deadline_us =
                monotonicMicros() + kSettledPageDelayUs;
          }
        }

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
            !full_refresh && !tone_reprocess &&
            damage.dirtyArea() * kSplitBenefitDenominator <
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
            // Show a large new page with AUTO immediately. Applying REGAL to
            // every intermediate frame makes page turns feel serialized; a
            // separately paced settled pass below restores its final quality.
            waveform = settle_regal_page ? kWaveformAuto : kWaveformRegal;
            break;
          case RefreshMode::kReader:
            waveform = page_cleanup_due ? kWaveformGc16 : kWaveformDu;
            fast = !page_cleanup_due;
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
            }
            // A touch probe only wakes capture; it is not evidence of motion.
            // Keep taps, dialogs and other static UI on AUTO unless the
            // gesture or row-hash detectors positively identify scrolling.
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
          } else if (tone_reprocess) {
            // A user-requested tone change must redraw the accumulated frame,
            // but it does not require a flashing full-update mode.
            waveform = kWaveformAuto;
            fast = false;
          }

          // Dithering trades sharpness for tonal range. Text only loses.
          const bool dither =
              settings.dither && !(settings.content_aware && bilevel);
          if (settings.content_aware && bilevel) {
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
          if (fast && !full_refresh && !reader_page_turn) {
            if (sparse_damage) {
              damage.forEachDirtyRun([&](const ChangedRect &dirty_run) {
                ghost.markFast(dirty_run);
              });
            } else {
              ghost.markFast(update_rect);
            }
          }

          if (full_refresh) {
            ++stats.full_updates;
            stats.updated_pixels += panel_pixels;
          } else {
            ++stats.partial_updates;
            stats.updated_pixels += rect.area();
          }
          if (frame.trigger_us != 0) {
            const int64_t submitted_us = monotonicMicros();
            if (submitted_us >= frame.trigger_us) {
              stats.notification_to_submit_us +=
                  static_cast<uint64_t>(submitted_us - frame.trigger_us);
              ++stats.notified_submits;
            }
          }
          if (page_cleanup_due) {
            ++stats.page_cleanups;
            ghost.clear(update_rect);
            cleanup_pending = ghost.hasDirty();
            if (!cleanup_pending) {
              cleanup_deadline_us = 0;
            }
          }
          if (settle_regal_page) {
            settled_quality_damage =
                settled_quality_damage.has_value()
                    ? unionRects(*settled_quality_damage, update_rect)
                    : update_rect;
            settled_quality_deadline_us =
                monotonicMicros() + kSettledPageDelayUs;
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
          tone_reprocess_pending = false;
          pending_full_refresh = false;
          unchanged_frames = 0;
          idle_poll_count = 0;
          if (full_refresh) {
            cleanup_pending = false;
            ghost.clear();
            scroll_in_progress = false;
            settled_quality_damage.reset();
            settled_quality_deadline_us = 0;
            page_turn_count = 0;
            cleanup_deadline_us = 0;
          } else if (settings.cleanup == CleanupPolicy::kManual) {
            cleanup_pending = false;
            ghost.clear();
            scroll_in_progress = false;
            cleanup_deadline_us = 0;
          } else if (reader_page_turn) {
            // Reader pages follow the stock count-based policy. Do not insert
            // a quiet-time cleanup after every fast page; the selected page
            // interval determines when GC16 is used. A cleanup already owed by
            // an earlier scroll remains independent and is merely postponed
            // until page turning becomes quiet.
            if (cleanup_pending) {
              cleanup_deadline_us =
                  monotonicMicros() + cleanupDelay(settings.cleanup);
            }
          } else {
            cleanup_pending = cleanup_pending || needs_cleanup;
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

      source->onFrameCompared(changed);
      source->release();
    }

    // A fast waveform ages only the tiles it touched. Once the compositor is
    // quiet, clean one bounded connected region starting at the oldest tile.
    // Disconnected regions remain pending for separately paced passes.
    if (cleanup_pending && settings.cleanup != CleanupPolicy::kManual &&
        initialized && ghost.hasDirty()) {
      const bool quiet = monotonicMicros() >= cleanup_deadline_us;
      if (quiet) {
        const ChangedRect cleanup_damage =
            ghost.nextCleanup(maximum_cleanup_area);
        if (cleanup_damage.area() == 0) {
          cleanup_pending = false;
          cleanup_deadline_us = 0;
          continue;
        }
        const int64_t submit_start_us = monotonicMicros();
        if (!ebc.sendStagedUpdate(cleanup_damage, kWaveformGc16,
                                  settings.dither,
                                  /*full_refresh=*/false)) {
          return 1;
        }
        stats.submit_time_us +=
            static_cast<uint64_t>(monotonicMicros() - submit_start_us);
        ++stats.partial_updates;
        ++stats.cleanup_updates;
        stats.updated_pixels += cleanup_damage.area();
        ghost.clear(cleanup_damage);
        cleanup_pending = ghost.hasDirty();
        if (cleanup_pending) {
          cleanup_deadline_us =
              monotonicMicros() + cleanupDelay(settings.cleanup);
        } else {
          scroll_in_progress = false;
          cleanup_deadline_us = 0;
        }
      }
    }

    if (input_probe_frames > 0) {
      --input_probe_frames;
    }
    const uint64_t driver_gate_wait_us = ebc.gateWaitTimeUs();
    if (driver_gate_wait_us >= accounted_gate_wait_us) {
      stats.gate_wait_time_us += driver_gate_wait_us - accounted_gate_wait_us;
    }
    accounted_gate_wait_us = driver_gate_wait_us;
    const uint64_t driver_ioctl_us = ebc.ioctlTimeUs();
    if (driver_ioctl_us >= accounted_ioctl_us) {
      stats.ioctl_time_us += driver_ioctl_us - accounted_ioctl_us;
    }
    accounted_ioctl_us = driver_ioctl_us;
    publishStats(&stats);

    if (source->streaming()) {
      // Nothing to poll: sleep until the compositor, a touch or a settings
      // change has something to say, or until cleanup or a notifier health
      // probe comes due.
      int64_t timeout_us = -1;
      if (cleanup_pending && cleanup_deadline_us != 0) {
        timeout_us =
            std::max<int64_t>(0, cleanup_deadline_us - monotonicMicros());
      }
      if (settled_quality_damage.has_value() &&
          settled_quality_deadline_us != 0) {
        const int64_t settled_timeout_us = std::max<int64_t>(
            0, settled_quality_deadline_us - monotonicMicros());
        timeout_us = timeout_us < 0 ? settled_timeout_us
                                    : std::min(timeout_us, settled_timeout_us);
      }
      const int64_t source_deadline_us = source->nextWakeDeadlineUs();
      if (source_deadline_us != 0) {
        const int64_t source_timeout_us =
            std::max<int64_t>(0, source_deadline_us - monotonicMicros());
        timeout_us = timeout_us < 0 ? source_timeout_us
                                    : std::min(timeout_us, source_timeout_us);
      }
      const uint32_t reasons = waiter.wait(timeout_us);
      source->onWake(reasons);
      if ((reasons & Waiter::kInput) != 0) {
        input_probe_frames = kInputProbeFrames;
      }
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
    if (settled_quality_damage.has_value() &&
        settled_quality_deadline_us != 0) {
      const int64_t remaining = settled_quality_deadline_us - monotonicMicros();
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
