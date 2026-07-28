/*
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <ui/Rect.h>
#include <ui/Region.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Leaf3EpdcPolicy.h"

namespace android {

struct Leaf3EpdcUpdate {
  uint32_t left;
  uint32_t top;
  uint32_t right;
  uint32_t bottom;
  uint32_t mode;
};

class Leaf3EpdcController {
public:
  static Leaf3EpdcController &get();

  bool isRequested() const;
  bool isActive() const;
  void setComposerSupported(bool supported);
  void setRefreshCallback(std::function<void()> callback);
  void clearRefreshCallback();

  std::vector<Leaf3EpdcUpdate> preparePresent(const Region &damage,
                                              const Rect &bounds);
  bool takeNotifierDamage(Rect *damage);
  void requestFullRefresh();
  bool beginSubmission();
  void endSubmission();
  void blockAndWaitForIdle();
  void composerFailed(const char *operation);
  std::string dump() const;

private:
  Leaf3EpdcController();
  ~Leaf3EpdcController();
  Leaf3EpdcController(const Leaf3EpdcController &) = delete;
  Leaf3EpdcController &operator=(const Leaf3EpdcController &) = delete;

  class Impl;
  std::unique_ptr<Impl> mImpl;
};

} // namespace android
