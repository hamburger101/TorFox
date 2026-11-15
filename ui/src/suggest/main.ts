// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// The address bar's suggestion list: a page of its own, shown by TorFox over
// the top of the web page while the address bar is being edited. It only
// shows what TorFox sends (bookmarks and history entries matching the typed
// text, never anything from the network) and reports which row was clicked
// by its position: it cannot name an address of its own.

import { LitElement, css, html } from "lit";

import { followAppearance } from "../appearance.js";
import { base } from "../base.js";
import { call, subscribe } from "../bridge.js";
import { icons } from "../icons.js";

interface Suggestion {
  url: string;
  title: string;
  source: "bookmark" | "history";
}

interface SuggestState {
  items: Suggestion[];
  highlight: number;
}

/** The address as shown: without "https://" and a lone trailing slash. */
function shortUrl(url: string): string {
  return url.replace(/^https:\/\//, "").replace(/^([^/?#]+)\/$/, "$1");
}

export class TorFoxSuggest extends LitElement {
  static override properties = {
    items: { state: true },
    highlight: { state: true },
  };

  declare items: Suggestion[];
  declare highlight: number;

  constructor() {
    super();
    this.items = [];
    this.highlight = -1;
  }

  override connectedCallback(): void {
    super.connectedCallback();
    subscribe<SuggestState>("suggest.subscribe", (s) => {
      this.items = s.items;
      this.highlight = s.highlight;
    });
  }

  private open(e: MouseEvent, index: number): void {
    // The primary button only; the press, not the release, so the address
    // bar is not left in between.
    if (e.button !== 0) {
      return;
    }
    e.preventDefault();
    void call("suggest.open", { index }).catch(() => undefined);
  }

  override render() {
    return html`<ul role="listbox" aria-label="Suggestions">
      ${this.items.map(
        (s, i) => html`<li role="option" aria-selected=${i === this.highlight ? "true" : "false"}
            class=${i === this.highlight ? "highlight" : ""} @mousedown=${(e: MouseEvent) => this.open(e, i)}>
          <span class="icon" title=${s.source === "bookmark" ? "Bookmark" : "History"}>
            ${s.source === "bookmark" ? icons.star : icons.history}</span>
          <span class="title">${s.title || shortUrl(s.url)}</span>
          ${s.title ? html`<span class="url">${shortUrl(s.url)}</span>` : ""}
        </li>`,
      )}
    </ul>`;
  }

  static override styles = [
    base,
    css`
      :host {
        display: block;
        height: 100vh;
        overflow: hidden;
        background: var(--card-raised);
        color: var(--fg);
        font: 13px/1 var(--font-ui);
        border: 1px solid var(--border-strong);
        border-radius: 0 0 var(--radius-m) var(--radius-m);
        user-select: none;
        cursor: default;
      }
      ul {
        margin: 0;
        padding: 4px;
        list-style: none;
      }
      /* TorFox sizes the overlay for 32px rows, 4px of padding and a 1px border
         (kSuggestRowHeight and kSuggestFrameHeight in browser_window.cc). */
      li {
        display: flex;
        align-items: center;
        gap: 10px;
        height: 32px;
        padding: 0 10px;
        border-radius: var(--radius-s);
        white-space: nowrap;
        overflow: hidden;
      }
      li:hover {
        background: var(--field-hover);
      }
      li.highlight {
        background: var(--accent-soft);
      }
      .icon {
        display: inline-flex;
        flex: none;
        width: 16px;
        height: 16px;
        color: var(--fg-muted);
      }
      .icon svg {
        width: 16px;
        height: 16px;
      }
      .title {
        flex: 0 1 auto;
        min-width: 0;
        overflow: hidden;
        text-overflow: ellipsis;
      }
      .url {
        flex: 1 1 0;
        min-width: 0;
        overflow: hidden;
        text-overflow: ellipsis;
        color: var(--fg-muted);
      }
      .url::before {
        content: "— ";
        color: var(--fg-faint);
      }
    `,
  ];
}

customElements.define("tf-suggest", TorFoxSuggest);
followAppearance();
