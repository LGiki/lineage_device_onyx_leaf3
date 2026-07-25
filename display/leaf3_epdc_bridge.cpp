/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * BOOX Leaf3 uses a dummy DSI connector as Android's primary display. The
 * actual E-Ink panel is updated through ONYX's private /dev/ebc interface.
 * Capture SurfaceFlinger's composed primary display and forward changed
 * regions to EBC.
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
#include <unistd.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include <android-base/properties.h>
#include <binder/ProcessState.h>
#include <gui/SurfaceComposerClient.h>
#include <log/log.h>
#include <ui/GraphicBuffer.h>
#include <ui/PixelFormat.h>
#include <utils/Errors.h>

namespace {

using android::GraphicBuffer;
using android::NO_ERROR;
using android::PhysicalDisplayId;
using android::ProcessState;
using android::ScreenshotClient;
using android::sp;
using android::status_t;
using android::SurfaceComposerClient;
using android::ui::Dataspace;

constexpr char kEbcDevice[] = "/dev/ebc";
constexpr char kAndroidBacklight[] =
    "/sys/class/backlight/panel0-backlight/brightness";
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
constexpr useconds_t kFastFrameDelayUs = 80000;
constexpr useconds_t kBalancedFrameDelayUs = 100000;
constexpr useconds_t kQualityFrameDelayUs = 140000;
constexpr useconds_t kIdleFrameDelayUs = 500000;
constexpr useconds_t kTouchSettleDelayUs = 32000;
constexpr useconds_t kRetryDelayUs = 1000000;
constexpr uint32_t kIdleThresholdFrames = 10;
constexpr uint32_t kCleanupAfterUnchangedFrames = 4;
constexpr uint32_t kFastCleanupInterval = 20;
constexpr uint32_t kInputProbeFrames = 6;
constexpr char kRefreshModeProperty[] = "persist.sys.leaf3.refresh_mode";
constexpr char kFullRefreshProperty[] = "sys.leaf3.full_refresh";
constexpr char kClearOnSleepProperty[] = "persist.sys.leaf3.clear_on_sleep";
constexpr char kTouchInputName[] = "cyttsp5_mt";

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
};

enum class RefreshMode {
  kBalanced,
  kNormal,
  kSpeed,
  kA2,
  kRegal,
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

std::string getProperty(const char *name, const char *default_value = "") {
  return android::base::GetProperty(name, default_value);
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

int getIntegerProperty(const char *name, int default_value) {
  const std::string text = getProperty(name);
  if (text.empty()) {
    return default_value;
  }

  char *end = nullptr;
  const long parsed = strtol(text.c_str(), &end, 10);
  return end == text.c_str() ? default_value : static_cast<int>(parsed);
}

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
  bool update() {
    int android_brightness = 0;
    if (!readIntegerFile(kAndroidBacklight, &android_brightness)) {
      if (!read_error_reported_) {
        ALOGE("cannot read Android backlight %s: %s", kAndroidBacklight,
              strerror(errno));
        read_error_reported_ = true;
      }
      return display_on_;
    }
    read_error_reported_ = false;

    android_brightness =
        std::clamp(android_brightness, 0, kAndroidBrightnessMax);
    display_on_ = android_brightness > 0;
    const bool frontlight_enabled =
        getIntegerProperty(kFrontlightEnabledProperty, 1) != 0;
    const int brightness_override = std::clamp(
        getIntegerProperty(kFrontlightBrightnessOverrideProperty, -1), -1, 100);
    int target_brightness = mapAndroidBrightness(android_brightness);
    if (brightness_override >= 0 && android_brightness > 0) {
      target_brightness =
          brightness_override == 0
              ? 0
              : 1 + (brightness_override * (kOnyxBrightnessMax - 1) + 50) / 100;
    }
    if (!frontlight_enabled) {
      target_brightness = 0;
    }

    const int temperature_percent = std::clamp(
        getIntegerProperty(kFrontlightTemperatureProperty, 0), 0, 100);
    const int target_temperature =
        (temperature_percent * kOnyxTemperatureMax + 50) / 100;

    apply(kOnyxTemperature, target_temperature, &last_temperature_,
          "temperature");
    apply(kOnyxBrightness, target_brightness, &last_brightness_, "brightness");
    return display_on_;
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
  bool read_error_reported_ = false;
  bool display_on_ = true;
};

RefreshMode getRefreshMode() {
  const std::string value = getProperty(kRefreshModeProperty, "balanced");
  if (value == "normal") {
    return RefreshMode::kNormal;
  }
  if (value == "speed" || value == "du") {
    return RefreshMode::kSpeed;
  }
  if (value == "a2") {
    return RefreshMode::kA2;
  }
  if (value == "regal") {
    return RefreshMode::kRegal;
  }
  return RefreshMode::kBalanced;
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

ChangedRect unionRects(const ChangedRect &left, const ChangedRect &right) {
  return ChangedRect{
      std::min(left.left, right.left),
      std::min(left.top, right.top),
      std::max(left.right, right.right),
      std::max(left.bottom, right.bottom),
  };
}

class InputWakeMonitor {
public:
  ~InputWakeMonitor() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }

  void init() {
    DIR *directory = opendir("/dev/input");
    if (directory == nullptr) {
      ALOGW("cannot open /dev/input: %s", strerror(errno));
      return;
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
        ALOGI("touch wake-up enabled on %s (%s)", path, name);
        break;
      }
      close(candidate);
    }
    closedir(directory);

    if (fd_ < 0) {
      ALOGW("touch input %s was not found; using timer polling",
            kTouchInputName);
    }
  }

  bool wait(useconds_t timeout_us) {
    if (fd_ < 0) {
      usleep(timeout_us);
      return false;
    }

    pollfd descriptor = {fd_, POLLIN, 0};
    const int timeout_ms = static_cast<int>((timeout_us + 999) / 1000);
    const int result = poll(&descriptor, 1, timeout_ms);
    if (result <= 0 || !(descriptor.revents & POLLIN)) {
      return false;
    }

    input_event events[64];
    while (read(fd_, events, sizeof(events)) > 0) {
    }
    return true;
  }

private:
  int fd_ = -1;
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
              uint32_t waveform, bool full_refresh) {
    // The mmap persists between updates. Keep it current by copying only the
    // changed bounding rectangle; full cleanups can then reuse the complete
    // accumulated frame instead of copying another 8 MiB.
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

    EbcUpdate update = {};
    update.top = full_refresh ? 0 : rect.top;
    update.left = full_refresh ? 0 : rect.left;
    update.width = full_refresh ? width_ : rect.right - rect.left;
    update.height = full_refresh ? height_ : rect.bottom - rect.top;
    update.waveform_mode = waveform;
    update.update_mode = full_refresh ? kUpdateFull : kUpdatePartial;
    update.update_marker = marker_++;
    update.temperature = kEbcAutoTemperature;
    update.flags = kEbcDitherFlag;

    if (ioctl(fd_, kEbcSendUpdate, &update) < 0) {
      ALOGE("EBC SEND_UPDATE marker=%u failed: %s", update.update_marker,
            strerror(errno));
      return false;
    }
    return true;
  }

  bool clear() {
    if (buffer_ == MAP_FAILED) {
      return false;
    }

    // E-Ink retains its last image without power. Replace the application
    // frame with white before Android suspends so an asleep device cannot look
    // like an unresponsive, still-open application.
    memset(buffer_, 0xff, buffer_size_);

    EbcUpdate update = {};
    update.width = width_;
    update.height = height_;
    update.waveform_mode = kWaveformGc16;
    update.update_mode = kUpdateFull;
    update.update_marker = marker_++;
    update.temperature = kEbcAutoTemperature;
    update.flags = kEbcDitherFlag;

    if (ioctl(fd_, kEbcSendUpdate, &update) < 0) {
      ALOGE("EBC sleep clear marker=%u failed: %s", update.update_marker,
            strerror(errno));
      return false;
    }
    return true;
  }

private:
  int fd_ = -1;
  void *buffer_ = MAP_FAILED;
  size_t buffer_size_ = 0;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t marker_ = 1;
};

std::optional<ChangedRect>
findChangedRect(const uint8_t *pixels, uint32_t width, uint32_t height,
                uint32_t stride, const std::vector<uint32_t> &previous) {
  ChangedRect changed = {width, height, 0, 0};
  bool any_changed = false;

  for (uint32_t y = 0; y < height; ++y) {
    const auto *row = reinterpret_cast<const uint32_t *>(
        pixels + static_cast<size_t>(y) * stride * sizeof(uint32_t));
    const auto *old_row = previous.data() + static_cast<size_t>(y) * width;
    if (memcmp(row, old_row, static_cast<size_t>(width) * sizeof(uint32_t)) ==
        0) {
      continue;
    }
    for (uint32_t x = 0; x < width; ++x) {
      if (row[x] == old_row[x]) {
        continue;
      }
      any_changed = true;
      changed.left = std::min(changed.left, x);
      changed.top = std::min(changed.top, y);
      changed.right = std::max(changed.right, x + 1);
      changed.bottom = std::max(changed.bottom, y + 1);
    }
  }

  if (!any_changed) {
    return std::nullopt;
  }

  // The EPDC is happiest with horizontally aligned update rectangles.
  changed.left &= ~7U;
  changed.right = std::min(width, (changed.right + 7U) & ~7U);
  return changed;
}

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

} // namespace

int main() {
  ProcessState::self()->setThreadPoolMaxThreadCount(0);
  ProcessState::self()->startThreadPool();

  std::optional<PhysicalDisplayId> display_id;
  while (!(display_id = SurfaceComposerClient::getInternalDisplayId())) {
    ALOGW("SurfaceFlinger internal display is not ready");
    usleep(kRetryDelayUs);
  }

  EbcDevice ebc;
  FrontlightBridge frontlight;
  InputWakeMonitor input_wake;
  input_wake.init();
  std::vector<uint32_t> previous;
  bool initialized = false;
  bool first_refresh = true;
  bool display_was_on = true;
  bool input_activity = false;
  uint32_t input_probe_frames = 0;
  uint32_t unchanged_frames = 0;
  uint32_t fast_update_count = 0;
  std::optional<ChangedRect> cleanup_rect;
  RefreshMode refresh_mode = getRefreshMode();
  std::string full_refresh_token = getProperty(kFullRefreshProperty);
  ALOGI("refresh mode: %s", refreshModeName(refresh_mode));

  for (;;) {
    // Qualcomm's stock light HAL updates the dummy DSI backlight. Mirror that
    // value to ONYX's real frontlight controller, including the zero written
    // by DisplayPowerController while the screen is off.
    const bool display_on = frontlight.update();
    if (!display_on) {
      if (display_was_on) {
        if (initialized && getIntegerProperty(kClearOnSleepProperty, 1) != 0) {
          ALOGI("display is off; clearing retained frame");
          ebc.clear();
        }
        ALOGI("display is off; suspending SurfaceFlinger capture");
        display_was_on = false;
        previous.clear();
        first_refresh = true;
        unchanged_frames = 0;
        fast_update_count = 0;
        input_probe_frames = 0;
        cleanup_rect.reset();
      }
      input_activity = input_wake.wait(kIdleFrameDelayUs);
      continue;
    }
    if (!display_was_on) {
      ALOGI("display is on; resuming with a full refresh");
      display_was_on = true;
      first_refresh = true;
      input_probe_frames = kInputProbeFrames;
    }

    Dataspace dataspace;
    sp<GraphicBuffer> buffer;
    const status_t capture_status =
        ScreenshotClient::capture(*display_id, &dataspace, &buffer);
    if (capture_status != NO_ERROR || buffer == nullptr) {
      ALOGW("display capture failed: %d", capture_status);
      usleep(kRetryDelayUs);
      continue;
    }

    const uint32_t width = buffer->getWidth();
    const uint32_t height = buffer->getHeight();
    const uint32_t stride = buffer->getStride();
    const int32_t format = buffer->getPixelFormat();
    const ssize_t bytes_per_pixel = android::bytesPerPixel(format);
    if (bytes_per_pixel != static_cast<ssize_t>(sizeof(uint32_t))) {
      ALOGE("unsupported capture format %d (%zu bytes/pixel)", format,
            static_cast<size_t>(std::max<ssize_t>(bytes_per_pixel, 0)));
      usleep(kRetryDelayUs);
      continue;
    }

    if (!initialized) {
      if (!ebc.init(width, height)) {
        return 1;
      }
      initialized = true;
    }

    void *pixels = nullptr;
    const status_t lock_status =
        buffer->lock(GraphicBuffer::USAGE_SW_READ_OFTEN, &pixels);
    if (lock_status != NO_ERROR || pixels == nullptr) {
      ALOGW("capture buffer lock failed: %d", lock_status);
      usleep(kRetryDelayUs);
      continue;
    }

    const RefreshMode requested_mode = getRefreshMode();
    if (requested_mode != refresh_mode) {
      refresh_mode = requested_mode;
      cleanup_rect.reset();
      fast_update_count = 0;
      ALOGI("refresh mode changed to %s", refreshModeName(refresh_mode));
    }

    const std::string requested_full_refresh =
        getProperty(kFullRefreshProperty);
    const bool force_full_refresh =
        requested_full_refresh != full_refresh_token;
    if (force_full_refresh) {
      full_refresh_token = requested_full_refresh;
      ALOGI("manual full refresh requested");
    }

    std::optional<ChangedRect> changed;
    if (first_refresh || force_full_refresh) {
      changed = ChangedRect{0, 0, width, height};
    } else {
      changed = findChangedRect(static_cast<const uint8_t *>(pixels), width,
                                height, stride, previous);
    }

    if (changed) {
      uint32_t waveform = kWaveformAuto;
      bool needs_cleanup = false;
      const bool interaction_active = input_activity || input_probe_frames > 0;
      switch (refresh_mode) {
      case RefreshMode::kSpeed:
        waveform = kWaveformDu;
        needs_cleanup = true;
        break;
      case RefreshMode::kA2:
        waveform = kWaveformAnim;
        needs_cleanup = true;
        break;
      case RefreshMode::kRegal:
        waveform = kWaveformRegal;
        break;
      case RefreshMode::kBalanced:
        if (interaction_active || fast_update_count > 0) {
          waveform = kWaveformAnim;
          needs_cleanup = true;
        }
        break;
      case RefreshMode::kNormal:
        break;
      }

      const bool full_refresh = first_refresh || force_full_refresh;
      if (full_refresh) {
        waveform = kWaveformGc16;
        needs_cleanup = false;
      }

      if (ebc.submit(static_cast<const uint8_t *>(pixels), stride, *changed,
                     waveform, full_refresh)) {
        saveFrameRegion(static_cast<const uint8_t *>(pixels), width, height,
                        stride, *changed, &previous);
        first_refresh = false;
        unchanged_frames = 0;
        if (needs_cleanup) {
          cleanup_rect = cleanup_rect ? unionRects(*cleanup_rect, *changed)
                                      : std::optional<ChangedRect>(*changed);
          ++fast_update_count;
        } else {
          cleanup_rect.reset();
          fast_update_count = 0;
        }
      }
    } else {
      unchanged_frames = std::min(unchanged_frames + 1, kIdleThresholdFrames);
      if (cleanup_rect && (unchanged_frames >= kCleanupAfterUnchangedFrames ||
                           fast_update_count >= kFastCleanupInterval)) {
        if (ebc.submit(static_cast<const uint8_t *>(pixels), stride,
                       *cleanup_rect, kWaveformGc16, true)) {
          cleanup_rect.reset();
          fast_update_count = 0;
        }
      }
    }

    buffer->unlock();
    const bool probing_for_input_frame = input_probe_frames > 0;
    const useconds_t delay =
        unchanged_frames >= kIdleThresholdFrames && !probing_for_input_frame
            ? kIdleFrameDelayUs
            : activeFrameDelay(refresh_mode);
    if (input_probe_frames > 0) {
      --input_probe_frames;
    }
    input_activity = input_wake.wait(delay);
    if (input_activity) {
      // Some applications publish their new frame well after the initial
      // touch. Keep capturing at the active cadence long enough to catch it
      // instead of immediately falling back to the 500 ms idle interval.
      input_probe_frames = kInputProbeFrames;
      // Let InputDispatcher and SurfaceFlinger publish the frame caused by the
      // event before taking the screenshot.
      usleep(kTouchSettleDelayUs);
    }
  }
}
