// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// torfox://downloads/: this window's downloads. The list lives in memory only
// and is gone when TorFox quits; a private window shows only its own. There is
// deliberately no "open file" action: TorFox only shows a file in its folder.

import { LitElement, css, html, nothing } from "lit";
import { styleMap } from "lit/directives/style-map.js";

import { followAppearance } from "../appearance.js";
import { base } from "../base.js";
import { BridgeError, call, subscribe } from "../bridge.js";
import { icons } from "../icons.js";
import { pageStyles } from "../page.js";

type State = "asking" | "in_progress" | "complete" | "cancelled" | "failed";

interface Entry {
  id: number;
  url: string;
  origin: string;
  name: string;
  received: number;
  total: number;
  size: string;
  state: State;
  private: boolean;
  started: number;
}

interface ListReply {
  entries: Entry[];
  policy: "disabled" | "ask" | "allow";
  private: boolean;
}

function formatBytes(n: number): string {
  if (n < 1024) {
    return `${n} bytes`;
  }
  const units = ["KB", "MB", "GB", "TB"];
  let v = n / 1024;
  let i = 0;
  while (v >= 1024 && i < units.length - 1) {
    v /= 1024;
    i++;
  }
  return `${v < 10 ? v.toFixed(1) : Math.round(v)} ${units[i]}`;
}

function statusText(e: Entry): string {
  switch (e.state) {
    case "asking":
      return "Waiting for your answer";
    case "in_progress":
      return e.total > 0
        ? `${formatBytes(e.received)} of ${formatBytes(e.total)}`
        : `${formatBytes(e.received)}`;
    case "complete":
      return e.size;
    case "cancelled":
      return "Cancelled";
    case "failed":
      return "Failed";
  }
}

export class TorFoxDownloads extends LitElement {
  static override properties = {
    entries: { state: true },
    policy: { state: true },
    privateWindow: { state: true },
    error: { state: true },
  };

  declare entries: Entry[];
  declare policy: string;
  declare privateWindow: boolean;
  declare error: string;

  constructor() {
    super();
    this.entries = [];
    this.policy = "";
    this.privateWindow = false;
    this.error = "";
  }

  override connectedCallback(): void {
    super.connectedCallback();
    subscribe("downloads.subscribe", () => void this.load(),
              (err) => (this.error = err.message));
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

  private async load(): Promise<void> {
    const reply = await this.attempt(() => call<ListReply>("downloads.list"));
    if (reply) {
      this.entries = reply.entries;
      this.policy = reply.policy;
      this.privateWindow = reply.private;
    }
  }

  private act(cmd: string, e: Entry): void {
    void this.attempt(() => call(cmd, { id: e.id }));
  }

  private renderEntry(e: Entry) {
    const running = e.state === "asking" || e.state === "in_progress";
    const percent = e.state === "in_progress" && e.total > 0
      ? Math.min(100, Math.floor((e.received / e.total) * 100)) : -1;
    return html`<div class="row download ${e.state}" data-id=${e.id}>
      <span class="row-icon">${icons.download}</span>
      <div class="body">
        <div class="name" title=${e.name}>${e.name}</div>
        <div class="meta">
          <span class="status">${statusText(e)}</span>
          ${e.origin ? html`<span class="origin" title=${e.url}>${e.origin}</span>` : nothing}
        </div>
        ${percent >= 0
          ? html`<div class="bar" role="progressbar" aria-valuenow=${percent} aria-valuemin="0"
              aria-valuemax="100"><span style=${styleMap({ width: `${percent}%` })}></span></div>`
          : nothing}
      </div>
      <div class="actions">
        ${running
          ? html`<button class="button cancel" @click=${() => this.act("downloads.cancel", e)}>Cancel</button>`
          : nothing}
        ${e.state === "complete"
          ? html`<button class="button reveal" @click=${() => this.act("downloads.reveal", e)}>
              ${icons.folder}Show in folder</button>`
          : nothing}
        ${!running
          ? html`<button class="icon-button remove" title="Remove from list" aria-label="Remove from list"
              @click=${() => this.act("downloads.remove", e)}>${icons.close}</button>`
          : nothing}
      </div>
    </div>`;
  }

  override render() {
    const finished = this.entries.some((e) => e.state !== "asking" && e.state !== "in_progress");
    return html`
      <header>
        <h1>Downloads</h1>
        <button class="button" id="clear" ?disabled=${!finished}
          @click=${() => void this.attempt(() => call("downloads.clear"))}>${icons.trash}Clear finished</button>
      </header>
      ${this.policy === "disabled"
        ? html`<div class="banner info" role="status" id="disabled-banner">
            Downloads are turned off in Settings. Websites cannot save files.</div>`
        : nothing}
      ${this.error ? html`<div class="banner" role="alert">${this.error}</div>` : nothing}
      <main>
        <p class="note">${icons.info}<span>${this.privateWindow
          ? "This list shows only this private window's downloads and is cleared when the window closes. Downloaded files stay on your computer."
          : "This list is kept only until TorFox quits. Downloaded files stay on your computer."}</span></p>
        ${this.entries.length === 0
          ? html`<p class="empty">No downloads.</p>`
          : html`<div class="list">${this.entries.map((e) => this.renderEntry(e))}</div>`}
      </main>
    `;
  }

  static override styles = [base, pageStyles, css`
    header .button {
      margin-left: auto;
    }
    main {
      max-width: 900px;
      padding: 12px 32px 48px;
    }
    .note {
      display: flex;
      align-items: center;
      gap: 8px;
      margin: 4px 0 12px 12px;
      font-size: 12.5px;
      color: var(--fg-faint);
    }
    .note svg {
      flex: none;
    }
    .download {
      align-items: flex-start;
      padding: 10px 12px;
    }
    .download .row-icon {
      margin-top: 2px;
    }
    .body {
      flex: 1;
      min-width: 0;
    }
    .name {
      overflow: hidden;
      white-space: nowrap;
      text-overflow: ellipsis;
      font-weight: 500;
    }
    .cancelled .name,
    .failed .name {
      color: var(--fg-muted);
      text-decoration: line-through;
    }
    .meta {
      display: flex;
      gap: 12px;
      margin-top: 2px;
      font-size: 12.5px;
      color: var(--fg-faint);
    }
    .origin {
      overflow: hidden;
      white-space: nowrap;
      text-overflow: ellipsis;
    }
    .failed .status {
      color: var(--danger);
    }
    .bar {
      height: 4px;
      margin-top: 8px;
      border-radius: 2px;
      background: var(--field);
      overflow: hidden;
    }
    .bar span {
      display: block;
      height: 100%;
      background: var(--accent);
    }
    .actions {
      display: flex;
      align-items: center;
      gap: 6px;
      flex: none;
    }
  `];
}

customElements.define("tf-downloads", TorFoxDownloads);
followAppearance();
