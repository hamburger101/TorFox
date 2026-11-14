// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Stage 0 feasibility spikes. NOT product code: these binaries exist only to
// answer architecture questions (sandbox bootstrap, tab close semantics,
// per-context proxy isolation, Alloy-style pref behaviour) with evidence.

#pragma once

#include "include/cef_app.h"

int SpikeMain(const CefMainArgs& main_args, void* sandbox_info);
