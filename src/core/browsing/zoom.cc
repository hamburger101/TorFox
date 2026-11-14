// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "src/core/browsing/zoom.h"

#include <array>
#include <cmath>

namespace torfox::browsing {

namespace {

constexpr std::array<int, 17> kSteps = {25,  33,  50,  67,  75,  80,  90,  100, 110,
                                        125, 150, 175, 200, 250, 300, 400, 500};

}  // namespace

std::span<const int> ZoomSteps() {
  return kSteps;
}

double ZoomLevelForPercent(int percent) {
  return std::log(percent / 100.0) / std::log(1.2);
}

int ZoomPercentForLevel(double level) {
  return static_cast<int>(std::lround(100.0 * std::pow(1.2, level)));
}

double StepZoomLevel(double level, int direction) {
  const int current = ZoomPercentForLevel(level);
  if (direction > 0) {
    for (int step : kSteps) {
      if (step > current) {
        return ZoomLevelForPercent(step);
      }
    }
    return ZoomLevelForPercent(kSteps.back());
  }
  if (direction < 0) {
    for (auto it = kSteps.rbegin(); it != kSteps.rend(); ++it) {
      if (*it < current) {
        return ZoomLevelForPercent(*it);
      }
    }
    return ZoomLevelForPercent(kSteps.front());
  }
  return level;
}

}  // namespace torfox::browsing
