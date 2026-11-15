// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// torfox://whatsnew/: opened once after TorFox has been updated. The release
// notes are bundled with TorFox (docs/release-notes/<version>.txt) and shown
// as plain text, never as markup.

import { LitElement, css, html, nothing } from "lit";

import { followAppearance } from "../appearance.js";
import { base } from "../base.js";
import { BridgeError, call } from "../bridge.js";
import { icons } from "../icons.js";
import { pageStyles } from "../page.js";

interface WhatsNew {
  version: string;
  /** The version TorFox was updated from; empty if the page was opened by hand. */
  previous: string;
  /** This version is the update TorFox itself checked and handed to its setup. */
  verified: boolean;
  notes: string;
}

export class TorFoxWhatsNew extends LitElement {
  static override properties = {
    info: { state: true },
    error: { state: true },
  };

  declare info: WhatsNew | null;
  declare error: string;

  constructor() {
    super();
    this.info = null;
    this.error = "";
  }

  override connectedCallback(): void {
    super.connectedCallback();
    void this.load();
  }

  private async load(): Promise<void> {
    try {
      this.info = await call<WhatsNew>("whatsnew.get");
      document.title = `What's new in TorFox ${this.info.version}`;
    } catch (err) {
      this.error = err instanceof BridgeError ? err.message : String(err);
    }
  }

  override render() {
    const info = this.info;
    return html`
      <header>
        <h1>What's new</h1>
      </header>
      ${this.error ? html`<div class="banner" role="alert">${this.error}</div>` : nothing}
      ${info
        ? html`<main>
            <section class="hero">
              <span class="hero-icon">${icons.update}</span>
              <div>
                <h2 id="title">TorFox ${info.version}</h2>
                <p class="from" id="from">${info.previous
                  ? `TorFox was updated from version ${info.previous}.${info.verified
                    ? " TorFox checked this update against its signing key before installing it."
                    : ""}`
                  : "The version of TorFox you are using."}</p>
              </div>
            </section>
            ${info.notes
              ? html`<pre class="notes" id="notes">${info.notes}</pre>`
              : html`<p class="empty" id="notes-missing">This version of TorFox was built without release notes.</p>`}
          </main>`
        : nothing}
    `;
  }

  static override styles = [base, pageStyles, css`
    main {
      max-width: 760px;
      padding: 24px 32px 56px;
    }
    .hero {
      display: flex;
      align-items: center;
      gap: 16px;
      margin-bottom: 20px;
    }
    .hero-icon {
      display: grid;
      place-items: center;
      flex: none;
      width: 48px;
      height: 48px;
      border-radius: 14px;
      background: var(--accent-soft);
      color: var(--accent);
    }
    .hero-icon svg {
      width: 24px;
      height: 24px;
    }
    h2 {
      margin: 0 0 4px;
      font-family: var(--font-display);
      font-size: 20px;
      font-weight: 600;
    }
    .from {
      margin: 0;
      color: var(--fg-muted);
      font-size: 13px;
    }
    .notes {
      margin: 0;
      padding: 18px 20px;
      border: 1px solid var(--border);
      border-radius: var(--radius-l);
      background: var(--card);
      color: var(--fg);
      font-family: var(--font-ui);
      font-size: 14px;
      line-height: 1.6;
      white-space: pre-wrap;
      overflow-wrap: anywhere;
    }
  `];
}

customElements.define("tf-whats-new", TorFoxWhatsNew);
followAppearance();
