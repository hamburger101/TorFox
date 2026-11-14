// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

import { LitElement, css, html, nothing } from "lit";

import { base } from "../base.js";
import { call, subscribe, BridgeError } from "../bridge.js";
import { icons } from "../icons.js";

interface TabState {
  id: number;
  title: string;
  url: string;
  internal: boolean;
  loading: boolean;
}

interface NavState {
  url?: string;
  internal?: boolean;
  loading?: boolean;
  canGoBack?: boolean;
  canGoForward?: boolean;
}

type ShellEvent =
  | { type: "state"; tabs: TabState[]; activeId: number; nav: NavState }
  | { type: "focusAddress" };

const NEW_TAB_URL = "torfox://newtab/";

function displayUrl(nav: NavState): string {
  if (!nav.url || nav.url === NEW_TAB_URL) {
    return "";
  }
  return nav.url;
}

type Security = "internal" | "secure" | "insecure" | "none";

function securityOf(nav: NavState): Security {
  const url = nav.url ?? "";
  if (nav.internal) {
    return "internal";
  }
  if (url.startsWith("https://")) {
    return "secure";
  }
  if (url.startsWith("http://")) {
    return "insecure";
  }
  return "none";
}

export class TorFoxShell extends LitElement {
  static override properties = {
    tabs: { state: true },
    activeId: { state: true },
    nav: { state: true },
    editing: { state: true },
    draft: { state: true },
    notice: { state: true },
  };

  declare tabs: TabState[];
  declare activeId: number;
  declare nav: NavState;
  declare editing: boolean;
  declare draft: string;
  declare notice: string;
  private noticeTimer = 0;

  constructor() {
    super();
    this.tabs = [];
    this.activeId = 0;
    this.nav = {};
    this.editing = false;
    this.draft = "";
    this.notice = "";
  }

  override connectedCallback(): void {
    super.connectedCallback();
    subscribe<ShellEvent>("state.subscribe", (e) => this.onEvent(e), (err) =>
      this.showNotice(`TorFox could not connect to its interface (${err.message}).`));
  }

  private onEvent(e: ShellEvent): void {
    if (e.type === "focusAddress") {
      this.focusAddress();
      return;
    }
    const activeChanged = e.activeId !== this.activeId;
    this.tabs = e.tabs;
    this.activeId = e.activeId;
    this.nav = e.nav;
    if (activeChanged) {
      this.editing = false;
      this.notice = "";
    }
  }

  private get input(): HTMLInputElement | null {
    return this.renderRoot.querySelector<HTMLInputElement>("#address");
  }

  private focusAddress(): void {
    void this.updateComplete.then(() => {
      const input = this.input;
      if (input) {
        input.focus();
        input.select();
      }
    });
  }

  private showNotice(message: string): void {
    this.notice = message;
    window.clearTimeout(this.noticeTimer);
    this.noticeTimer = window.setTimeout(() => (this.notice = ""), 6000);
  }

  private async go(e: Event): Promise<void> {
    e.preventDefault();
    const value = this.input?.value ?? "";
    if (!value.trim()) {
      return;
    }
    try {
      await call("nav.go", { input: value });
      this.editing = false;
      this.notice = "";
      this.input?.blur();
    } catch (err) {
      this.showNotice(err instanceof BridgeError ? err.message : String(err));
    }
  }

  private onInput(e: InputEvent): void {
    this.editing = true;
    this.draft = (e.target as HTMLInputElement).value;
  }

  private onKeyDown(e: KeyboardEvent): void {
    if (e.key === "Escape") {
      this.editing = false;
      this.notice = "";
      const input = this.input;
      if (input) {
        input.value = displayUrl(this.nav);
        input.select();
      }
    }
  }

  private closeTab(e: Event, id: number): void {
    e.stopPropagation();
    void call("tab.close", { id });
  }

  private onTabAuxClick(e: MouseEvent, id: number): void {
    if (e.button === 1) {
      e.preventDefault();
      void call("tab.close", { id });
    }
  }

  private renderTab(t: TabState) {
    const active = t.id === this.activeId;
    const title = t.title || (t.internal ? "New Tab" : t.url);
    return html`<div
      class="tab ${active ? "active" : ""}"
      role="tab"
      aria-selected=${active ? "true" : "false"}
      title=${title}
      @click=${() => call("tab.select", { id: t.id })}
      @auxclick=${(e: MouseEvent) => this.onTabAuxClick(e, t.id)}
    >
      ${t.loading ? html`<span class="spinner" aria-label="Loading"></span>` : nothing}
      <span class="tab-title">${title}</span>
      <button class="tab-close" title="Close tab" aria-label="Close tab"
        @click=${(e: Event) => this.closeTab(e, t.id)}>${icons.close}</button>
    </div>`;
  }

  private renderSecurity() {
    switch (securityOf(this.nav)) {
      case "internal":
        return html`<span class="badge internal" title="TorFox page">TorFox</span>`;
      case "secure":
        return html`<span class="badge secure" title="Connection is encrypted (HTTPS)">${icons.lock}</span>`;
      case "insecure":
        return html`<span class="badge insecure" title="Connection is not encrypted">${icons.warn}<span>Not secure</span></span>`;
      default:
        return html`<span class="badge search">${icons.search}</span>`;
    }
  }

  override render() {
    const nav = this.nav;
    const value = this.editing ? this.draft : displayUrl(nav);
    return html`
      <div class="strip" role="tablist" aria-label="Tabs">
        ${this.tabs.map((t) => this.renderTab(t))}
        <button class="icon new-tab" title="New tab (Ctrl+T)" aria-label="New tab"
          @click=${() => call("tab.new")}>${icons.plus}</button>
      </div>
      <div class="toolbar">
        <button class="icon" title="Back" aria-label="Back" ?disabled=${!nav.canGoBack}
          @click=${() => call("nav.back")}>${icons.back}</button>
        <button class="icon" title="Forward" aria-label="Forward" ?disabled=${!nav.canGoForward}
          @click=${() => call("nav.forward")}>${icons.forward}</button>
        ${nav.loading
          ? html`<button class="icon" title="Stop" aria-label="Stop"
              @click=${() => call("nav.stop")}>${icons.stop}</button>`
          : html`<button class="icon" title="Reload (F5)" aria-label="Reload"
              @click=${() => call("nav.reload")}>${icons.reload}</button>`}
        <form class="address ${this.notice ? "has-notice" : ""}" @submit=${this.go}>
          ${this.renderSecurity()}
          <input id="address" type="text" spellcheck="false" autocomplete="off"
            aria-label="Address and search bar"
            placeholder="Search with DuckDuckGo or enter an address"
            .value=${value}
            @input=${this.onInput}
            @keydown=${this.onKeyDown}
            @focus=${(e: FocusEvent) => (e.target as HTMLInputElement).select()} />
          ${this.notice
            ? html`<div class="notice" role="alert" title=${this.notice}>${this.notice}</div>`
            : nothing}
        </form>
        <button class="icon" title="Settings" aria-label="Settings"
          @click=${() => call("app.openSettings")}>${icons.settings}</button>
      </div>
    `;
  }

  static override styles = [base, css`
    :host {
      display: flex;
      flex-direction: column;
      height: 100vh;
      background: var(--toolbar);
      user-select: none;
      overflow: hidden;
    }
    button {
      font: inherit;
      color: inherit;
    }
    .strip {
      display: flex;
      align-items: flex-end;
      gap: 2px;
      height: 38px;
      padding: 0 8px;
      background: var(--strip);
      overflow: hidden;
    }
    .tab {
      position: relative;
      display: flex;
      align-items: center;
      gap: 8px;
      flex: 0 1 220px;
      min-width: 72px;
      height: 32px;
      padding: 0 6px 0 12px;
      border-radius: var(--radius-m) var(--radius-m) 0 0;
      color: var(--fg-muted);
      cursor: default;
      transition: background-color 120ms ease, color 120ms ease;
    }
    .tab:hover {
      background: var(--tab-hover);
      color: var(--fg);
    }
    .tab.active {
      background: var(--toolbar);
      color: var(--fg);
    }
    .tab:not(.active) + .tab:not(.active)::before {
      content: "";
      position: absolute;
      left: -1px;
      top: 9px;
      bottom: 9px;
      width: 1px;
      background: var(--border);
    }
    .tab-title {
      flex: 1;
      min-width: 0;
      overflow: hidden;
      white-space: nowrap;
      text-overflow: ellipsis;
      font-size: 12.5px;
    }
    .tab-close {
      display: grid;
      place-items: center;
      width: 20px;
      height: 20px;
      padding: 0;
      border: 0;
      border-radius: 5px;
      background: transparent;
      opacity: 0.7;
    }
    .tab-close:hover {
      background: var(--field-hover);
      opacity: 1;
    }
    .tab-close svg {
      width: 12px;
      height: 12px;
    }
    .spinner {
      width: 12px;
      height: 12px;
      flex: none;
      border: 1.6px solid var(--border-strong);
      border-top-color: var(--accent);
      border-radius: 50%;
      animation: spin 0.8s linear infinite;
    }
    @keyframes spin {
      to {
        transform: rotate(360deg);
      }
    }
    .toolbar {
      display: flex;
      align-items: center;
      gap: 4px;
      height: 44px;
      padding: 0 8px;
      border-bottom: 1px solid var(--border);
    }
    .icon {
      display: grid;
      place-items: center;
      flex: none;
      width: 32px;
      height: 32px;
      padding: 0;
      border: 0;
      border-radius: var(--radius-s);
      background: transparent;
      color: var(--fg);
    }
    .icon:hover:not(:disabled) {
      background: var(--field-hover);
    }
    .icon:disabled {
      color: var(--fg-faint);
    }
    .icon:focus-visible,
    .tab-close:focus-visible {
      outline: none;
      box-shadow: var(--focus-ring);
    }
    .new-tab {
      align-self: center;
      margin-left: 4px;
      width: 28px;
      height: 28px;
    }
    .address {
      position: relative;
      flex: 1;
      display: flex;
      align-items: center;
      gap: 6px;
      height: 34px;
      margin: 0 6px;
      padding: 0 12px 0 8px;
      border-radius: 17px;
      background: var(--field);
      border: 1px solid transparent;
      transition: background-color 120ms ease, border-color 120ms ease;
    }
    .address:hover {
      background: var(--field-hover);
    }
    .address:focus-within {
      background: var(--field-focus);
      border-color: var(--accent);
    }
    .address.has-notice {
      border-color: var(--danger);
    }
    #address {
      flex: 1;
      min-width: 0;
      height: 100%;
      border: 0;
      outline: none;
      background: transparent;
      color: var(--fg);
      font: inherit;
      font-size: 14px;
      user-select: text;
    }
    #address::placeholder {
      color: var(--fg-faint);
    }
    .badge {
      display: inline-flex;
      align-items: center;
      gap: 4px;
      flex: none;
      height: 24px;
      padding: 0 6px;
      border-radius: 12px;
      font-size: 12px;
      color: var(--fg-muted);
    }
    .badge.internal {
      background: var(--accent-soft);
      color: var(--accent);
      font-weight: 600;
      padding: 0 9px;
    }
    .badge.insecure {
      color: var(--danger);
    }
    .badge.secure svg,
    .badge.search svg {
      width: 14px;
      height: 14px;
    }
    /* The shell view is a fixed 82 DIP strip, so messages stay inside the field. */
    .notice {
      flex: 0 1 auto;
      max-width: 60%;
      padding: 2px 8px;
      border-radius: 10px;
      background: var(--danger-soft);
      font-size: 12px;
      color: var(--danger);
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }
  `];
}

customElements.define("tf-shell", TorFoxShell);
