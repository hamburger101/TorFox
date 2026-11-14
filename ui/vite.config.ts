// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Builds TorFox's internal pages. The output is compiled into the TorFox
// binary (scripts/gen_resources.py) and served only on torfox:// with a strict
// CSP, so the build must produce: external module scripts and stylesheets
// only (no inline code), no data: URIs for code, no source maps.

import { fileURLToPath } from "node:url";
import { defineConfig } from "vite";

const root = fileURLToPath(new URL(".", import.meta.url));

export default defineConfig({
  root,
  base: "/",
  publicDir: false,
  build: {
    outDir: "dist",
    emptyOutDir: true,
    target: "chrome130",
    sourcemap: false,
    assetsInlineLimit: 0,
    modulePreload: { polyfill: false },
    reportCompressedSize: false,
    rolldownOptions: {
      input: {
        shell: `${root}pages/shell/index.html`,
        newtab: `${root}pages/newtab/index.html`,
        settings: `${root}pages/settings/index.html`,
      },
    },
  },
});
