// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

import { LitElement, css, html, nothing } from "lit";

import { followAppearance } from "../appearance.js";
import { base } from "../base.js";
import { BridgeError, call, subscribe } from "../bridge.js";
import { icons } from "../icons.js";
import { hostOf, pageStyles } from "../page.js";

interface Entry {
  url: string;
  title: string;
  visits: number;
  lastVisit: number;
}

interface ListReply {
  entries: Entry[];
  more: boolean;
}

interface Subscription {
  changed: string;
  available?: boolean;
  notice?: string;
}

const PAGE = 100;

function dayLabel(time: number, now: Date): string {
  const d = new Date(time);
  const start = (x: Date) => new Date(x.getFullYear(), x.getMonth(), x.getDate()).getTime();
  const days = Math.round((start(now) - start(d)) / 86400000);
  if (days === 0) {
    return "Today";
  }
  if (days === 1) {
    return "Yesterday";
  }
  return d.toLocaleDateString(undefined, { weekday: "long", year: "numeric", month: "long",
                                            day: "numeric" });
}

export class TorFoxHistory extends LitElement {
  static override properties = {
    entries: { state: true },
    more: { state: true },
    query: { state: true },
    confirmClear: { state: true },
    error: { state: true },
    available: { state: true },
    notice: { state: true },
  };

  declare entries: Entry[];
  declare more: boolean;
  declare query: string;
  declare confirmClear: boolean;
  declare error: string;
  declare available: boolean;
  declare notice: string;

  constructor() {
    super();
    this.entries = [];
    this.more = false;
    this.query = "";
    this.confirmClear = false;
    this.error = "";
    this.available = true;
    this.notice = "";
  }

  override connectedCallback(): void {
    super.connectedCallback();
    subscribe<Subscription>("history.subscribe", (m) => {
      if (m.available !== undefined) {
        this.available = m.available;
        this.notice = m.notice ?? "";
      }
      void this.load(false);
    }, (err) => (this.error = err.message));
  }

  private async attempt<T>(fn: () => Promise<T>): Promise<T | undefined> {
    try {
      const result = await fn();
      this.error = "";
      return result;
    } catch (err) {
      this.error = err instanceof BridgeError ? err.message : String(err);
      return undefined;
    }
  }

  /** Reloads the list, or with |append| adds the next page. */
  private async load(append: boolean): Promise<void> {
    if (!this.available) {
      return;
    }
    const args: Record<string, unknown> = { limit: PAGE };
    if (this.query) {
      args["query"] = this.query;
    } else if (append && this.entries.length > 0) {
      args["before"] = this.entries[this.entries.length - 1]!.lastVisit;
    }
    const reply = await this.attempt(() => call<ListReply>("history.list", args));
    if (!reply) {
      return;
    }
    this.entries = append ? [...this.entries, ...reply.entries] : reply.entries;
    this.more = reply.more;
  }

  private onSearch(e: InputEvent): void {
    this.query = (e.target as HTMLInputElement).value.trim();
    void this.load(false);
  }

  private open(e: MouseEvent, entry: Entry): void {
    const newTab = e.button === 1 || e.ctrlKey || e.metaKey;
    void this.attempt(() => call("nav.open", { url: entry.url, newTab }));
  }

  private async removeEntry(entry: Entry): Promise<void> {
    await this.attempt(() => call("history.removeUrl", { url: entry.url }));
  }

  private async clear(): Promise<void> {
    if (!this.confirmClear) {
      this.confirmClear = true;
      return;
    }
    this.confirmClear = false;
    await this.attempt(() => call("history.clear"));
  }

  private renderEntries() {
    const now = new Date();
    const out = [];
    let day = "";
    for (const entry of this.entries) {
      const label = dayLabel(entry.lastVisit, now);
      if (label !== day && !this.query) {
        day = label;
        out.push(html`<h2>${label}</h2>`);
      }
      const time = new Date(entry.lastVisit).toLocaleTimeString(undefined,
        { hour: "2-digit", minute: "2-digit" });
      out.push(html`<div class="row">
        <span class="time">${time}</span>
        <button class="row-main" title=${`${entry.title}\n${entry.url}`}
          @click=${(e: MouseEvent) => this.open(e, entry)}
          @auxclick=${(e: MouseEvent) => { if (e.button === 1) { e.preventDefault(); this.open(e, entry); } }}>
          <span class="row-icon">${icons.globe}</span>
          <span class="row-title">${entry.title || entry.url}</span>
          <span class="row-url">${hostOf(entry.url)}</span>
        </button>
        <div class="row-actions">
          <button class="icon-button" title="Remove from history" aria-label="Remove from history"
            @click=${() => this.removeEntry(entry)}>${icons.close}</button>
        </div>
      </div>`);
    }
    return out;
  }

  override render() {
    return html`
      <header>
        <h1>History</h1>
        <label class="search">
          ${icons.search}
          <input id="search" type="search" spellcheck="false" autocomplete="off"
            aria-label="Search history" placeholder="Search history" @input=${this.onSearch} />
        </label>
        ${this.confirmClear
          ? html`<span class="confirm">Clear all history? This cannot be undone.
              <button class="button danger primary" id="confirm-clear" @click=${this.clear}>Clear history</button>
              <button class="button" @click=${() => (this.confirmClear = false)}>Cancel</button></span>`
          : html`<button class="button danger" id="clear" ?disabled=${!this.available}
              @click=${this.clear}>${icons.trash}Clear history</button>`}
      </header>
      ${!this.available
        ? html`<div class="banner" role="alert">${this.notice || "History is unavailable."}</div>`
        : this.notice ? html`<div class="banner info" role="status">${this.notice}</div>` : nothing}
      ${this.error ? html`<div class="banner" role="alert">${this.error}</div>` : nothing}
      <main>
        <p class="note">${icons.info}<span>TorFox records history only in normal windows; private windows are never recorded. Removed entries are overwritten in TorFox's files.</span></p>
        ${this.entries.length === 0
          ? html`<p class="empty">${this.query ? "No history matches." : "No history yet."}</p>`
          : this.renderEntries()}
        ${this.more && !this.query
          ? html`<div class="more"><button class="button" @click=${() => this.load(true)}>Show more</button></div>`
          : nothing}
      </main>
    `;
  }

  static override styles = [base, pageStyles, css`
    main {
      max-width: 900px;
      padding: 12px 32px 48px;
    }
    h2 {
      margin: 20px 0 6px 12px;
      font-size: 13px;
      font-weight: 600;
      color: var(--fg-muted);
    }
    .time {
      flex: none;
      width: 52px;
      font-size: 12px;
      color: var(--fg-faint);
      font-variant-numeric: tabular-nums;
    }
    .note {
      display: flex;
      align-items: center;
      gap: 8px;
      margin: 4px 0 0 12px;
      font-size: 12.5px;
      color: var(--fg-faint);
    }
    .note svg {
      flex: none;
    }
    .confirm {
      display: inline-flex;
      align-items: center;
      gap: 8px;
      font-size: 13px;
      color: var(--danger);
    }
    .more {
      display: flex;
      justify-content: center;
      padding: 16px 0;
    }
  `];
}

customElements.define("tf-history", TorFoxHistory);
followAppearance();
