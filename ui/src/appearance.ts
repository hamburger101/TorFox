// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

import { subscribe } from "./bridge.js";

export type Theme = "dark" | "light" | "system";

interface Appearance {
  theme: Theme;
  dark: boolean;
}

/** Keeps <html data-theme> in sync with the Appearance setting. */
export function followAppearance(): void {
  subscribe<Appearance>("appearance.subscribe", (a) => {
    document.documentElement.dataset["theme"] = a.theme;
  });
}
