// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

import { LitElement, css, html } from "lit";

import { followAppearance } from "../appearance.js";
import { base } from "../base.js";
import { BridgeError, call } from "../bridge.js";
import { icons } from "../icons.js";

// The notes below describe the current build and must stay truthful: they
// change only when the feature behind them is implemented and tested.
const BUILD_NOTES = [
  {
    icon: icons.onion,
    title: "Tor is not available yet",
    text: "Onion addresses are refused in this build. TorFox never sends them to a search engine or to ordinary DNS.",
  },
  {
    icon: icons.download,
    title: "Downloads are blocked",
    text: "Download protection is still being built, so this build cancels every download.",
  },
  {
    icon: icons.shield,
    title: "Site permissions are denied",
    text: "Camera, microphone, location, notifications and other permission requests are refused automatically.",
  },
];

export class TorFoxNewTab extends LitElement {
  static override properties = {
    error: { state: true },
  };

  declare error: string;

  constructor() {
    super();
    this.error = "";
  }

  override firstUpdated(): void {
    this.renderRoot.querySelector<HTMLInputElement>("#q")?.focus();
  }

  private async search(e: Event): Promise<void> {
    e.preventDefault();
    const input = this.renderRoot.querySelector<HTMLInputElement>("#q");
    const value = input?.value ?? "";
    if (!value.trim()) {
      return;
    }
    try {
      await call("nav.go", { input: value });
      this.error = "";
    } catch (err) {
      this.error = err instanceof BridgeError ? err.message : String(err);
    }
  }

  override render() {
    return html`
      <main>
        <h1 class="wordmark"><span class="mark">${icons.shield}</span>TorFox</h1>
        <form class="search ${this.error ? "has-error" : ""}" @submit=${this.search}>
          ${icons.search}
          <input id="q" type="text" spellcheck="false" autocomplete="off"
            aria-label="Search with DuckDuckGo or enter an address"
            placeholder="Search with DuckDuckGo or enter an address"
            @input=${() => (this.error = "")} />
        </form>
        ${this.error
          ? html`<p class="error" role="alert">${this.error}</p>`
          : html`<p class="hint">Searches go to DuckDuckGo. TorFox has no telemetry.</p>`}
        <section class="notes" aria-label="About this build">
          ${BUILD_NOTES.map(
            (n) => html`<article class="note">
              <span class="note-icon">${n.icon}</span>
              <div>
                <h2>${n.title}</h2>
                <p>${n.text}</p>
              </div>
            </article>`,
          )}
        </section>
      </main>
    `;
  }

  static override styles = [base, css`
    :host {
      display: block;
      min-height: 100vh;
      background: var(--page);
      color: var(--fg);
    }
    main {
      display: flex;
      flex-direction: column;
      align-items: center;
      max-width: 720px;
      margin: 0 auto;
      padding: 14vh 24px 48px;
    }
    .wordmark {
      display: flex;
      align-items: center;
      gap: 12px;
      margin: 0 0 28px;
      font-family: var(--font-display);
      font-size: 34px;
      font-weight: 600;
      letter-spacing: -0.02em;
    }
    .mark {
      display: grid;
      place-items: center;
      width: 44px;
      height: 44px;
      border-radius: 12px;
      background: var(--accent-soft);
      color: var(--accent);
    }
    .mark svg {
      width: 24px;
      height: 24px;
    }
    .search {
      display: flex;
      align-items: center;
      gap: 10px;
      width: 100%;
      height: 48px;
      padding: 0 18px;
      border-radius: 24px;
      border: 1px solid var(--border);
      background: var(--card);
      color: var(--fg-muted);
      box-shadow: var(--shadow);
      transition: border-color 120ms ease;
    }
    .search:focus-within {
      border-color: var(--accent);
    }
    .search.has-error {
      border-color: var(--danger);
    }
    #q {
      flex: 1;
      min-width: 0;
      height: 100%;
      border: 0;
      outline: none;
      background: transparent;
      color: var(--fg);
      font: inherit;
      font-size: 15px;
    }
    #q::placeholder {
      color: var(--fg-faint);
    }
    .hint,
    .error {
      margin: 12px 0 0;
      font-size: 12.5px;
      color: var(--fg-faint);
    }
    .error {
      color: var(--danger);
    }
    .notes {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
      gap: 12px;
      width: 100%;
      margin-top: 56px;
    }
    .note {
      display: flex;
      gap: 12px;
      padding: 14px 16px;
      border-radius: var(--radius-l);
      background: var(--card);
      border: 1px solid var(--border);
    }
    .note-icon {
      flex: none;
      color: var(--accent);
      padding-top: 1px;
    }
    .note h2 {
      margin: 0 0 4px;
      font-size: 13px;
      font-weight: 600;
    }
    .note p {
      margin: 0;
      font-size: 12.5px;
      line-height: 1.45;
      color: var(--fg-muted);
    }
  `];
}

customElements.define("tf-newtab", TorFoxNewTab);
followAppearance();
