// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef TORFOX_CORE_BROWSING_ZOOM_H_
#define TORFOX_CORE_BROWSING_ZOOM_H_

#include <span>

namespace torfox::browsing {

// Page zoom steps, as percentages, matching other Chromium browsers.
std::span<const int> ZoomSteps();

// Chromium zoom levels are logarithmic: factor = 1.2 ^ level, so 0 is 100%.
double ZoomLevelForPercent(int percent);
int ZoomPercentForLevel(double level);

// The next step above (direction > 0) or below (direction < 0) |level|,
// staying at the first or last step at the ends.
double StepZoomLevel(double level, int direction);

}  // namespace torfox::browsing

#endif  // TORFOX_CORE_BROWSING_ZOOM_H_
