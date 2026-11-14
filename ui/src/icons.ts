// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Inline SVG icons (no external requests, CSP-safe). 16px grid, 1.6 stroke.

import { svg } from "lit";

const base = (body: ReturnType<typeof svg>) => svg`<svg viewBox="0 0 16 16" width="16" height="16"
  fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round"
  stroke-linejoin="round" aria-hidden="true">${body}</svg>`;

export const icons = {
  back: base(svg`<path d="M13 8H3.5M7.5 3.5 3 8l4.5 4.5"/>`),
  forward: base(svg`<path d="M3 8h9.5M8.5 3.5 13 8l-4.5 4.5"/>`),
  reload: base(svg`<path d="M13 3.5v3.2H9.8"/><path d="M12.6 6.6A5 5 0 1 0 13 9.5"/>`),
  stop: base(svg`<path d="m4 4 8 8M12 4l-8 8"/>`),
  close: base(svg`<path d="m4.5 4.5 7 7M11.5 4.5l-7 7"/>`),
  plus: base(svg`<path d="M8 3v10M3 8h10"/>`),
  settings: base(svg`<circle cx="8" cy="8" r="2.2"/><path d="M8 1.8v1.6M8 12.6v1.6M14.2 8h-1.6M3.4 8H1.8M12.4 3.6l-1.1 1.1M4.7 11.3l-1.1 1.1M12.4 12.4l-1.1-1.1M4.7 4.7 3.6 3.6"/>`),
  lock: base(svg`<rect x="3.5" y="7" width="9" height="6.5" rx="1.5"/><path d="M5.5 7V5.2a2.5 2.5 0 0 1 5 0V7"/>`),
  warn: base(svg`<path d="M8 2.2 14 13H2L8 2.2Z"/><path d="M8 6.5v2.8M8 11.2v.1"/>`),
  search: base(svg`<circle cx="7" cy="7" r="4.2"/><path d="m10.2 10.2 3.3 3.3"/>`),
  general: base(svg`<rect x="2.5" y="2.5" width="11" height="11" rx="2"/><path d="M5.5 6h5M5.5 10h5"/>`),
  shield: base(svg`<path d="M8 1.8 13 3.7v4c0 3.1-2.2 5.4-5 6.5-2.8-1.1-5-3.4-5-6.5v-4L8 1.8Z"/>`),
  download: base(svg`<path d="M8 2.5v7.5M4.8 7 8 10.2 11.2 7M3 13.5h10"/>`),
  onion: base(svg`<path d="M8 2c-.3 1.4-1.4 2.2-2.6 3.2C4 6.3 3 7.6 3 9.4 3 12 5.2 14 8 14s5-2 5-4.6c0-1.8-1-3.1-2.4-4.2C9.4 4.2 8.3 3.4 8 2Z"/><path d="M8 5.2c-.9.9-2.2 2.1-2.2 4.2S6.8 12.6 8 12.6s2.2-1.1 2.2-3.2S8.9 6.1 8 5.2Z"/>`),
  appearance: base(svg`<circle cx="8" cy="8" r="5.5"/><path d="M8 2.5v11a5.5 5.5 0 0 0 0-11Z" fill="currentColor"/>`),
  update: base(svg`<path d="M8 13.5V5M4.8 8.2 8 5l3.2 3.2"/><path d="M3 2.5h10"/>`),
  info: base(svg`<circle cx="8" cy="8" r="5.8"/><path d="M8 7.2v3.8M8 5v.1"/>`),
  home: base(svg`<path d="M2.8 7.4 8 3l5.2 4.4"/><path d="M4.3 6.3v6.7h7.4V6.3"/><path d="M6.8 13V9.8h2.4V13"/>`),
  star: base(svg`<path d="m8 2.3 1.75 3.6 3.95.55-2.86 2.77.68 3.93L8 11.3l-3.52 1.85.68-3.93L2.3 6.45l3.95-.55L8 2.3Z"/>`),
  starFilled: base(svg`<path d="m8 2.3 1.75 3.6 3.95.55-2.86 2.77.68 3.93L8 11.3l-3.52 1.85.68-3.93L2.3 6.45l3.95-.55L8 2.3Z" fill="currentColor"/>`),
  menu: base(svg`<circle cx="3.5" cy="8" r=".6" fill="currentColor"/><circle cx="8" cy="8" r=".6" fill="currentColor"/><circle cx="12.5" cy="8" r=".6" fill="currentColor"/>`),
  folder: base(svg`<path d="M2.5 4.5a1 1 0 0 1 1-1h2.8l1.4 1.5h4.8a1 1 0 0 1 1 1v6a1 1 0 0 1-1 1h-9a1 1 0 0 1-1-1v-7.5Z"/>`),
  globe: base(svg`<circle cx="8" cy="8" r="5.6"/><path d="M2.6 8h10.8M8 2.4c1.6 1.6 2.3 3.5 2.3 5.6S9.6 12 8 13.6M8 2.4C6.4 4 5.7 5.9 5.7 8S6.4 12 8 13.6"/>`),
  history: base(svg`<path d="M3 8a5 5 0 1 0 1.5-3.6"/><path d="M3 3.2v2.4h2.4"/><path d="M8 5.3V8l1.9 1.3"/>`),
  mask: base(svg`<path d="M2 6.2c0-1 .8-1.7 1.8-1.7h8.4c1 0 1.8.7 1.8 1.7v.6c0 2.2-1.6 3.8-3.5 3.8-1 0-1.6-.5-2.5-1.6-.9 1.1-1.5 1.6-2.5 1.6C3.6 10.6 2 9 2 6.8v-.6Z"/><circle cx="5.3" cy="7" r=".7" fill="currentColor"/><circle cx="10.7" cy="7" r=".7" fill="currentColor"/>`),
  chevronUp: base(svg`<path d="m4 10 4-4 4 4"/>`),
  chevronDown: base(svg`<path d="m4 6 4 4 4-4"/>`),
  edit: base(svg`<path d="M10.6 3.2 12.8 5.4 6 12.2l-2.9.7.7-2.9 6.8-6.8Z"/>`),
  trash: base(svg`<path d="M3.5 4.5h9M6.5 4.5V3h3v1.5M4.8 4.5l.6 8.5h5.2l.6-8.5"/>`),
  arrowUp: base(svg`<path d="M8 13V3.5M4 7.5 8 3.5l4 4"/>`),
  arrowDown: base(svg`<path d="M8 3v9.5M4 8.5l4 4 4-4"/>`),
};
