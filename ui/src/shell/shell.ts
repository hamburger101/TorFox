// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

import { LitElement, css, html, nothing } from "lit";
// CSSOM, not a style attribute: the strict CSP forbids inline styles.
import { styleProps } from "../style-props.js";

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
  bookmarkable?: boolean;
  bookmarked?: boolean;
  zoom?: number;
}

interface TorStatus {
  state: "off" | "starting" | "connecting" | "connected" | "error";
  progress: number;
  message: string;
  retry: boolean;
  connected: boolean;
}

interface BarItem {
  id: number;
  title: string;
  url: string;
  folder: boolean;
}

type ShellEvent =
  | {
      type: "state";
      private: boolean;
      tor: TorStatus | null;
      tabs: TabState[];
      activeId: number;
      nav: NavState;
      findOpen: boolean;
      bookmarksBar: boolean;
      downloads: { active: number; count: number };
      /** Set while a downloaded, verified update waits for a relaunch. */
      update: { version: string; restarts: boolean } | null;
    }
  | { type: "bookmarksBar"; visible: boolean; items: BarItem[] }
  | { type: "find"; count: number; active: number; final: boolean }
  | { type: "focusAddress" }
  | { type: "focusFind" }
  | { type: "star" }
  | { type: "notice"; text: string }
  | { type: "suggestOpened"; ok: boolean };

/** One row of the address bar's suggestion list (bookmarks and history only). */
interface Suggestion {
  url: string;
  title: string;
  source: "bookmark" | "history";
}

// Typing pauses this long before the list is updated.
const SUGGEST_DELAY_MS = 60;
// After the address bar loses the focus, the list waits this long before it
// goes, so that a press on one of its rows still opens it.
const SUGGEST_BLUR_MS = 200;

const NEW_TAB_URL = "torfox://newtab/";
// Pointer travel before a press on a tab becomes a drag.
const DRAG_THRESHOLD = 5;

function displayUrl(nav: NavState): string {
  if (!nav.url || nav.url === NEW_TAB_URL) {
    return "";
  }
  return nav.url;
}

type Security = "internal" | "onion" | "secure" | "insecure" | "none";

function isOnionUrl(url: string): boolean {
  try {
    const host = new URL(url).hostname.replace(/\.$/, "");
    return host.endsWith(".onion");
  } catch {
    return false;
  }
}

function securityOf(nav: NavState): Security {
  const url = nav.url ?? "";
  if (nav.internal) {
    return "internal";
  }
  // Onion services are reached only through Tor, which encrypts the whole
  // way to the service, http:// included.
  if ((url.startsWith("http://") || url.startsWith("https://")) && isOnionUrl(url)) {
    return "onion";
  }
  if (url.startsWith("https://")) {
    return "secure";
  }
  if (url.startsWith("http://")) {
    return "insecure";
  }
  return "none";
}

/** Bottom-right (or bottom-left) corner of |el| in shell coordinates, for native menus. */
function anchor(el: Element, side: "left" | "right"): { x: number; y: number } {
  const r = el.getBoundingClientRect();
  return { x: Math.max(0, Math.round(side === "right" ? r.right : r.left)),
           y: Math.max(0, Math.round(r.bottom)) };
}

interface Drag {
  id: number;
  pointerId: number;
  startX: number;
  dx: number;
  active: boolean;
  target: number;
}

export class TorFoxShell extends LitElement {
  static override properties = {
    tabs: { state: true },
    activeId: { state: true },
    nav: { state: true },
    isPrivate: { state: true },
    tor: { state: true },
    editing: { state: true },
    draft: { state: true },
    notice: { state: true },
    noticeKind: { state: true },
    findOpen: { state: true },
    findCount: { state: true },
    findActive: { state: true },
    findText: { state: true },
    barVisible: { state: true },
    barItems: { state: true },
    drag: { state: true },
    downloads: { state: true },
    readyUpdate: { state: true },
  };

  declare tabs: TabState[];
  declare activeId: number;
  declare nav: NavState;
  declare isPrivate: boolean;
  declare tor: TorStatus | null;
  declare editing: boolean;
  declare draft: string;
  declare notice: string;
  declare noticeKind: "error" | "info";
  declare findOpen: boolean;
  declare findCount: number;
  declare findActive: number;
  declare findText: string;
  declare barVisible: boolean;
  declare barItems: BarItem[];
  declare drag: Drag | null;
  declare downloads: { active: number; count: number };
  declare readyUpdate: { version: string; restarts: boolean } | null;
  private noticeTimer = 0;
  // The suggestion list: what TorFox last showed, and the highlighted row.
  private suggestions: Suggestion[] = [];
  private suggestHighlight = -1;
  private suggestTimer = 0;
  private suggestBlurTimer = 0;
  // Each query and each hide gets a new number; a reply to an older one is
  // dropped. TorFox handles them in order, so the last one sent wins there too.
  private suggestSeq = 0;
  private suggestPending = false;
  private readonly onResize = (): void => this.hideSuggestions();

  constructor() {
    super();
    this.tabs = [];
    this.activeId = 0;
    this.nav = {};
    this.isPrivate = false;
    this.tor = null;
    this.editing = false;
    this.draft = "";
    this.notice = "";
    this.noticeKind = "error";
    this.findOpen = false;
    this.findCount = -1;
    this.findActive = 0;
    this.findText = "";
    this.barVisible = false;
    this.barItems = [];
    this.drag = null;
    this.downloads = { active: 0, count: 0 };
    this.readyUpdate = null;
  }

  override connectedCallback(): void {
    super.connectedCallback();
    subscribe<ShellEvent>("state.subscribe", (e) => this.onEvent(e), (err) =>
      this.showNotice(`TorFox could not connect to its interface (${err.message}).`));
    // The list is placed under the address bar; a new window size moves it.
    window.addEventListener("resize", this.onResize);
  }

  override disconnectedCallback(): void {
    window.removeEventListener("resize", this.onResize);
    super.disconnectedCallback();
  }

  private onEvent(e: ShellEvent): void {
    switch (e.type) {
      case "focusAddress":
        this.focusAddress();
        return;
      case "focusFind":
        this.focusFind();
        return;
      case "star":
        void this.star();
        return;
      case "notice":
        // Shown once at startup, and may be long: longer than other notices.
        this.showNotice(e.text, "info", 10000);
        return;
      case "suggestOpened":
        // A row of the list was clicked: TorFox opened it in this tab.
        this.forgetSuggestions();
        if (e.ok) {
          this.editing = false;
          this.notice = "";
          this.input?.blur();
        }
        return;
      case "find":
        this.findCount = e.count;
        this.findActive = e.active;
        return;
      case "bookmarksBar":
        this.barVisible = e.visible;
        this.barItems = e.items;
        return;
      case "state": {
        const activeChanged = e.activeId !== this.activeId;
        this.tabs = e.tabs;
        this.activeId = e.activeId;
        this.nav = e.nav;
        this.isPrivate = e.private;
        // Tor stopped working: say why, once.
        if (e.tor && e.tor.state === "error" && this.tor?.state !== "error" && e.tor.message) {
          this.showNotice(e.tor.message);
        }
        this.tor = e.tor ?? null;
        this.barVisible = e.bookmarksBar;
        this.downloads = e.downloads ?? { active: 0, count: 0 };
        this.readyUpdate = e.update ?? null;
        if (!e.findOpen && this.findOpen) {
          this.findText = "";
          this.findCount = -1;
        }
        this.findOpen = e.findOpen;
        if (activeChanged) {
          this.editing = false;
          this.notice = "";
          // TorFox hides the list itself when the tab changes.
          this.forgetSuggestions();
        }
        return;
      }
    }
  }

  private el<T extends Element>(selector: string): T | null {
    return this.renderRoot.querySelector<T>(selector);
  }

  private get input(): HTMLInputElement | null {
    return this.el<HTMLInputElement>("#address");
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

  private focusFind(): void {
    void this.updateComplete.then(() => {
      const input = this.el<HTMLInputElement>("#find");
      if (input) {
        input.focus();
        input.select();
      }
    });
  }

  private showNotice(message: string, kind: "error" | "info" = "error", ms?: number): void {
    this.notice = message;
    this.noticeKind = kind;
    window.clearTimeout(this.noticeTimer);
    this.noticeTimer = window.setTimeout(() => (this.notice = ""), ms ?? (kind === "info" ? 3000 : 6000));
  }

  /** "Relaunch to update". TorFox asks first if private or Tor windows are open. */
  private async relaunch(): Promise<void> {
    try {
      await call("update.install");
    } catch (err) {
      const message = err instanceof BridgeError ? err.message : String(err);
      if (message !== "cancelled") {
        this.showNotice(message);
      }
    }
  }

  private async run(cmd: string, args?: Record<string, unknown>): Promise<void> {
    try {
      await call(cmd, args);
    } catch (err) {
      this.showNotice(err instanceof BridgeError ? err.message : String(err));
    }
  }

  // ----- Address bar -----

  private async go(e: Event): Promise<void> {
    e.preventDefault();
    // Enter on a highlighted suggestion opens its address.
    const picked = this.suggestions[this.suggestHighlight];
    const value = picked ? picked.url : (this.input?.value ?? "");
    this.hideSuggestions();
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
    window.clearTimeout(this.suggestTimer);
    window.clearTimeout(this.suggestBlurTimer);
    this.suggestTimer = window.setTimeout(() => void this.querySuggestions(), SUGGEST_DELAY_MS);
  }

  // ----- Address-bar suggestions -----

  private async querySuggestions(): Promise<void> {
    const input = this.input;
    const form = this.el<HTMLElement>("form.address");
    const text = input?.value ?? "";
    if (!input || !form || (this.renderRoot as ShadowRoot).activeElement !== input || !text.trim()) {
      this.hideSuggestions();
      return;
    }
    // Under the address bar, as wide as it is, in shell coordinates.
    const r = form.getBoundingClientRect();
    const seq = ++this.suggestSeq;
    this.suggestPending = true;
    try {
      const reply = await call<{ items: Suggestion[]; highlight: number }>("suggest.query", {
        // The bridge takes at most 256 bytes; 80 UTF-16 units are at most 240.
        text: text.slice(0, 80),
        x: Math.max(0, Math.round(r.left)),
        y: Math.max(0, Math.round(r.bottom)),
        width: Math.max(1, Math.round(r.width)),
      });
      if (seq === this.suggestSeq) {
        this.suggestPending = false;
        this.suggestions = reply.items;
        this.suggestHighlight = reply.highlight;
      }
    } catch {
      if (seq === this.suggestSeq) {
        this.suggestPending = false;
        this.suggestions = [];
        this.suggestHighlight = -1;
      }
    }
  }

  /** The list is shown, or about to be. */
  private get suggesting(): boolean {
    return this.suggestPending || this.suggestions.length > 0;
  }

  /** Drops the list here, and any reply still to come. */
  private forgetSuggestions(): void {
    window.clearTimeout(this.suggestTimer);
    window.clearTimeout(this.suggestBlurTimer);
    ++this.suggestSeq;
    this.suggestPending = false;
    this.suggestions = [];
    this.suggestHighlight = -1;
  }

  private hideSuggestions(): void {
    this.forgetSuggestions();
    void call("suggest.hide").catch(() => undefined);
  }

  private moveHighlight(step: number): void {
    const n = this.suggestions.length;
    // -1 is the typed text itself, then the rows in order, round again.
    let next = this.suggestHighlight + step;
    if (next < -1) {
      next = n - 1;
    } else if (next >= n) {
      next = -1;
    }
    this.suggestHighlight = next;
    void call("suggest.highlight", { index: next }).catch(() => undefined);
  }

  private onAddressBlur(): void {
    window.clearTimeout(this.suggestBlurTimer);
    this.suggestBlurTimer = window.setTimeout(() => this.hideSuggestions(), SUGGEST_BLUR_MS);
  }

  private onKeyDown(e: KeyboardEvent): void {
    if ((e.key === "ArrowDown" || e.key === "ArrowUp") && this.suggestions.length > 0) {
      e.preventDefault();
      this.moveHighlight(e.key === "ArrowDown" ? 1 : -1);
      return;
    }
    if (e.key === "Escape" && this.suggesting) {
      // The first Escape closes the list and keeps what was typed.
      e.preventDefault();
      this.hideSuggestions();
      return;
    }
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

  private async star(): Promise<void> {
    if (!this.nav.bookmarkable) {
      this.showNotice("Only web pages can be bookmarked.", "info");
      return;
    }
    await this.updateComplete;
    const button = this.el<HTMLElement>("#star");
    const at = button ? anchor(button, "right") : { x: 0, y: 0 };
    await this.run("bookmarks.star", at);
  }

  // ----- Tabs -----

  private closeTab(e: Event, id: number): void {
    e.stopPropagation();
    void call("tab.close", { id });
  }

  private onTabPointerDown(e: PointerEvent, id: number): void {
    if (e.button !== 0 || (e.target as Element).closest(".tab-close")) {
      return;
    }
    (e.currentTarget as Element).setPointerCapture(e.pointerId);
    this.drag = { id, pointerId: e.pointerId, startX: e.clientX, dx: 0, active: false,
                  target: this.tabs.findIndex((t) => t.id === id) };
  }

  private onTabPointerMove(e: PointerEvent): void {
    const drag = this.drag;
    if (!drag || e.pointerId !== drag.pointerId) {
      return;
    }
    const dx = e.clientX - drag.startX;
    const active = drag.active || Math.abs(dx) > DRAG_THRESHOLD;
    // The slot under the pointer: compare with the other tabs' centres.
    let target = 0;
    const others = Array.from(this.renderRoot.querySelectorAll<HTMLElement>(".tab"))
      .filter((el) => Number(el.dataset["id"]) !== drag.id);
    for (const el of others) {
      const r = el.getBoundingClientRect();
      if (e.clientX > r.left + r.width / 2) {
        target++;
      }
    }
    this.drag = { ...drag, dx, active, target };
  }

  private onTabPointerUp(e: PointerEvent, id: number): void {
    const drag = this.drag;
    if (!drag || e.pointerId !== drag.pointerId) {
      return;
    }
    this.drag = null;
    if (!drag.active) {
      void call("tab.select", { id });
      return;
    }
    const from = this.tabs.findIndex((t) => t.id === drag.id);
    if (from >= 0 && drag.target !== from) {
      void this.run("tab.move", { id: drag.id, index: drag.target });
    }
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
    const dragging = this.drag?.active && this.drag.id === t.id;
    return html`<div
      class="tab ${active ? "active" : ""} ${dragging ? "dragging" : ""}"
      role="tab"
      data-id=${t.id}
      aria-selected=${active ? "true" : "false"}
      title=${title}
      style=${styleProps(dragging ? { transform: `translateX(${this.drag?.dx ?? 0}px)` } : {})}
      @pointerdown=${(e: PointerEvent) => this.onTabPointerDown(e, t.id)}
      @pointermove=${(e: PointerEvent) => this.onTabPointerMove(e)}
      @pointerup=${(e: PointerEvent) => this.onTabPointerUp(e, t.id)}
      @pointercancel=${() => (this.drag = null)}
      @auxclick=${(e: MouseEvent) => this.onTabAuxClick(e, t.id)}
    >
      ${t.loading ? html`<span class="spinner" aria-label="Loading"></span>` : nothing}
      <span class="tab-title">${title}</span>
      <button class="tab-close" title="Close tab" aria-label="Close tab"
        @click=${(e: Event) => this.closeTab(e, t.id)}>${icons.close}</button>
    </div>`;
  }

  // ----- Bookmarks bar -----

  private openBarItem(e: MouseEvent, item: BarItem): void {
    if (item.folder) {
      void this.run("bookmarks.folderMenu", { id: item.id, ...anchor(e.currentTarget as Element, "left") });
      return;
    }
    void this.run("nav.open", { url: item.url, newTab: e.button === 1 || e.ctrlKey || e.metaKey });
  }

  private renderBar() {
    return html`<div class="bar" role="toolbar" aria-label="Bookmarks bar">
      ${this.barItems.length === 0
        ? html`<span class="bar-empty">Bookmarks you add to the Bookmarks bar appear here.</span>`
        : this.barItems.map((item) => html`<button class="bar-item" title=${item.folder ? item.title : `${item.title}\n${item.url}`}
            @click=${(e: MouseEvent) => this.openBarItem(e, item)}
            @auxclick=${(e: MouseEvent) => { if (e.button === 1 && !item.folder) { e.preventDefault(); this.openBarItem(e, item); } }}>
            ${item.folder ? icons.folder : icons.globe}<span>${item.title || item.url}</span>
          </button>`)}
      <span class="bar-spacer"></span>
      <button class="bar-item" @click=${() => this.run("app.openBookmarks")}>${icons.star}<span>All bookmarks</span></button>
    </div>`;
  }

  // ----- Find bar -----

  private find(forward: boolean): void {
    void this.run("find.query", { text: this.findText, forward });
  }

  private onFindInput(e: InputEvent): void {
    this.findText = (e.target as HTMLInputElement).value;
    if (!this.findText) {
      this.findCount = -1;
    }
    this.find(true);
  }

  private onFindKeyDown(e: KeyboardEvent): void {
    if (e.key === "Enter") {
      e.preventDefault();
      if (this.findText) {
        this.find(!e.shiftKey);
      }
    } else if (e.key === "Escape") {
      e.preventDefault();
      void this.run("find.stop");
    }
  }

  private renderFind() {
    const status = !this.findText || this.findCount < 0
      ? ""
      : this.findCount === 0 ? "No results" : `${this.findActive} of ${this.findCount}`;
    return html`<div class="findbar" role="search">
      <div class="find-field">
        ${icons.search}
        <input id="find" type="text" spellcheck="false" autocomplete="off" aria-label="Find in page"
          placeholder="Find in page" .value=${this.findText}
          @input=${this.onFindInput} @keydown=${this.onFindKeyDown} />
        <span class="find-status ${this.findCount === 0 && this.findText ? "none" : ""}"
          aria-live="polite">${status}</span>
      </div>
      <button class="icon" title="Previous match" aria-label="Previous match"
        ?disabled=${!this.findText} @click=${() => this.find(false)}>${icons.chevronUp}</button>
      <button class="icon" title="Next match" aria-label="Next match"
        ?disabled=${!this.findText} @click=${() => this.find(true)}>${icons.chevronDown}</button>
      <button class="icon" title="Close find bar" aria-label="Close find bar"
        @click=${() => this.run("find.stop")}>${icons.close}</button>
    </div>`;
  }

  // ----- Toolbar -----

  private renderSecurity() {
    switch (securityOf(this.nav)) {
      case "internal":
        return html`<span class="badge internal" title="TorFox page">TorFox</span>`;
      case "onion":
        return html`<span class="badge onion" title="Onion site: reached only through Tor, which encrypts the connection all the way to the site">${icons.onion}<span>Onion</span></span>`;
      case "secure":
        return html`<span class="badge secure" title="Connection is encrypted (HTTPS)">${icons.lock}</span>`;
      case "insecure":
        return html`<span class="badge insecure" title="Connection is not encrypted">${icons.warn}<span>Not secure</span></span>`;
      default:
        return html`<span class="badge search">${icons.search}</span>`;
    }
  }

  private renderTor(t: TorStatus) {
    const busy = !t.connected && t.state !== "error";
    const label = t.connected ? "Connected"
      : t.state === "error" ? "Not connected"
      : t.state === "off" ? "Not started"
      : `Connecting ${t.progress}%`;
    const title = t.connected
      ? "Tor window: every connection goes through Tor. Nothing is kept after the last Tor window closes."
      : t.message ? `Tor: ${t.message}`
      : "Tor is starting. Pages in this window wait until Tor is connected.";
    return html`
      <span class="tor-badge ${t.connected ? "ok" : t.state === "error" ? "error" : "busy"}"
        id="tor-status" role="status" title=${title}>
        ${icons.onion}<span>Tor</span><span class="tor-state">${label}</span>
        ${busy
          ? html`<span class="tor-progress" aria-hidden="true"><span class="tor-progress-fill"
              style=${styleProps({ width: `${Math.max(4, Math.min(100, t.progress))}%` })}></span></span>`
          : nothing}
      </span>
      ${t.state === "error" && !t.retry
        ? html`<button class="tor-action" id="tor-retry" @click=${() => this.run("tor.connect")}>Try again</button>`
        : nothing}
      <button class="icon tor-identity" id="tor-identity"
        title="New identity: closes every Tor window and starts again with new Tor circuits"
        aria-label="New identity" @click=${() => this.run("tor.newIdentity")}>${icons.broom}</button>`;
  }

  override render() {
    const nav = this.nav;
    const value = this.editing ? this.draft : displayUrl(nav);
    const zoom = nav.zoom ?? 100;
    return html`
      <div class="strip ${this.tor ? "tor" : this.isPrivate ? "private" : ""}" role="tablist" aria-label="Tabs">
        ${this.tabs.map((t) => this.renderTab(t))}
        <button class="icon new-tab" title="New tab" aria-label="New tab"
          @click=${() => call("tab.new")}>${icons.plus}</button>
        ${this.tor
          ? this.renderTor(this.tor)
          : this.isPrivate
          ? html`<span class="private-badge" title="Private window: no history, and cookies and site data are forgotten when it closes">${icons.mask}<span>Private</span></span>`
          : nothing}
      </div>
      <div class="toolbar">
        <button class="icon" title="Back" aria-label="Back" ?disabled=${!nav.canGoBack}
          @click=${() => call("nav.back")}>${icons.back}</button>
        <button class="icon" title="Forward" aria-label="Forward" ?disabled=${!nav.canGoForward}
          @click=${() => call("nav.forward")}>${icons.forward}</button>
        ${nav.loading
          ? html`<button class="icon" title="Stop" aria-label="Stop"
              @click=${() => call("nav.stop")}>${icons.stop}</button>`
          : html`<button class="icon" title="Reload" aria-label="Reload"
              @click=${() => call("nav.reload")}>${icons.reload}</button>`}
        <button class="icon" title="Home" aria-label="Home"
          @click=${() => this.run("nav.home")}>${icons.home}</button>
        <form class="address ${this.notice && this.noticeKind === "error" ? "has-notice" : ""}" @submit=${this.go}>
          ${this.renderSecurity()}
          <input id="address" type="text" spellcheck="false" autocomplete="off"
            aria-label="Address and search bar"
            placeholder=${this.tor ? "Search with DuckDuckGo over Tor or enter an address" : "Search with DuckDuckGo or enter an address"}
            .value=${value}
            @input=${this.onInput}
            @keydown=${this.onKeyDown}
            @blur=${this.onAddressBlur}
            @focus=${(e: FocusEvent) => (e.target as HTMLInputElement).select()} />
          ${this.notice
            ? html`<div class="notice ${this.noticeKind}" role=${this.noticeKind === "error" ? "alert" : "status"} title=${this.notice}>${this.notice}</div>`
            : nothing}
          ${zoom !== 100
            ? html`<button type="button" class="zoom" title="Reset zoom" aria-label="Zoom ${zoom}%, reset"
                @click=${() => this.run("zoom.change", { step: 0 })}>${zoom}%</button>`
            : nothing}
          ${nav.bookmarkable
            ? html`<button type="button" id="star" class="star ${nav.bookmarked ? "on" : ""}"
                title=${nav.bookmarked ? "Edit bookmark" : "Bookmark this page"}
                aria-label=${nav.bookmarked ? "Edit bookmark" : "Bookmark this page"}
                aria-pressed=${nav.bookmarked ? "true" : "false"}
                @click=${() => this.star()}>${nav.bookmarked ? icons.starFilled : icons.star}</button>`
            : nothing}
        </form>
        ${this.downloads.count > 0
          ? html`<button class="icon downloads ${this.downloads.active > 0 ? "active" : ""}" id="downloads"
              title=${this.downloads.active > 0 ? `Downloads (${this.downloads.active} in progress)` : "Downloads"}
              aria-label=${this.downloads.active > 0 ? `Downloads, ${this.downloads.active} in progress` : "Downloads"}
              @click=${() => this.run("app.openDownloads")}>${icons.download}${this.downloads.active > 0
                ? html`<span class="badge">${this.downloads.active}</span>` : nothing}</button>`
          : nothing}
        ${this.readyUpdate
          ? html`<button class="relaunch" id="relaunch"
              title=${this.readyUpdate.restarts
                ? `TorFox ${this.readyUpdate.version} is downloaded and verified. Relaunch TorFox to install it.`
                : `TorFox ${this.readyUpdate.version} is downloaded and verified. TorFox closes and opens it; copy TorFox to Applications.`}
              @click=${() => void this.relaunch()}>${icons.update}<span>${this.readyUpdate.restarts
                ? "Relaunch to update" : "Update TorFox"}</span></button>`
          : nothing}
        <button class="icon" id="menu" title="Menu" aria-label="Menu"
          @click=${(e: MouseEvent) => this.run("app.menu", anchor(e.currentTarget as Element, "right"))}>${icons.menu}</button>
      </div>
      ${this.barVisible ? this.renderBar() : nothing}
      ${this.findOpen ? this.renderFind() : nothing}
    `;
  }

  // Row heights must match BrowserWindow (src/browser/shell/browser_window.cc).
  static override styles = [base, css`
    .relaunch {
      align-self: center;
      display: inline-flex;
      align-items: center;
      gap: 6px;
      height: 28px;
      margin: 0 2px;
      padding: 0 12px 0 9px;
      border: none;
      border-radius: 14px;
      background: var(--accent-soft);
      color: var(--accent);
      font-size: 12.5px;
      font-weight: 600;
      white-space: nowrap;
    }
    .relaunch:hover {
      background: var(--accent);
      color: var(--on-accent);
    }
    .relaunch svg {
      width: 15px;
      height: 15px;
    }
    .downloads {
      position: relative;
    }
    .downloads.active {
      color: var(--accent);
    }
    .downloads .badge {
      position: absolute;
      top: 2px;
      right: 1px;
      min-width: 14px;
      height: 14px;
      padding: 0 3px;
      border-radius: 7px;
      background: var(--accent-strong);
      color: #fff;
      font-size: 10px;
      font-weight: 600;
      line-height: 14px;
      text-align: center;
    }
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
      flex: none;
      height: 38px;
      padding: 0 8px;
      background: var(--strip);
      overflow: hidden;
    }
    .strip.private {
      background: var(--strip-private);
    }
    .strip.tor {
      background: var(--strip-tor);
    }
    .tor-badge {
      display: inline-flex;
      align-items: center;
      gap: 6px;
      align-self: center;
      margin-left: auto;
      padding: 3px 10px;
      border-radius: 12px;
      background: var(--tor-soft);
      color: var(--tor);
      font-size: 12px;
      font-weight: 600;
      white-space: nowrap;
    }
    .tor-badge svg {
      width: 14px;
      height: 14px;
    }
    .tor-state {
      font-weight: 500;
      color: var(--fg-muted);
    }
    .tor-badge.ok .tor-state {
      color: var(--ok);
    }
    .tor-badge.error .tor-state {
      color: var(--danger);
    }
    .tor-progress {
      width: 48px;
      height: 4px;
      border-radius: 2px;
      background: var(--border);
      overflow: hidden;
    }
    .tor-progress-fill {
      display: block;
      height: 100%;
      background: var(--tor);
      transition: width 200ms ease;
    }
    .tor-action {
      align-self: center;
      height: 24px;
      margin-left: 6px;
      padding: 0 10px;
      border: 1px solid var(--border-strong);
      border-radius: 12px;
      background: transparent;
      font-size: 12px;
    }
    .tor-action:hover {
      background: var(--field-hover);
    }
    .tor-identity {
      align-self: center;
      margin-left: 4px;
      width: 28px;
      height: 28px;
      color: var(--tor);
    }
    .tor-action:focus-visible {
      outline: none;
      box-shadow: var(--focus-ring);
    }
    .private-badge {
      display: inline-flex;
      align-items: center;
      gap: 6px;
      align-self: center;
      margin-left: auto;
      padding: 3px 10px;
      border-radius: 12px;
      background: var(--private-soft);
      color: var(--private);
      font-size: 12px;
      font-weight: 600;
      white-space: nowrap;
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
      touch-action: none;
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
    .tab.dragging {
      z-index: 1;
      transition: none;
      box-shadow: var(--shadow);
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
      flex: none;
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
    .tab-close:focus-visible,
    .bar-item:focus-visible,
    .star:focus-visible,
    .zoom:focus-visible {
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
      padding: 0 4px 0 8px;
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
    .badge.onion {
      background: var(--tor-soft);
      color: var(--tor);
      font-weight: 600;
      padding: 0 9px;
    }
    .badge.onion svg {
      width: 14px;
      height: 14px;
    }
    .badge.secure svg,
    .badge.search svg {
      width: 14px;
      height: 14px;
    }
    .zoom {
      flex: none;
      height: 24px;
      padding: 0 8px;
      border: 1px solid var(--border-strong);
      border-radius: 12px;
      background: transparent;
      font-size: 12px;
      color: var(--fg-muted);
    }
    .zoom:hover {
      background: var(--field-hover);
      color: var(--fg);
    }
    .star {
      display: grid;
      place-items: center;
      flex: none;
      width: 28px;
      height: 28px;
      padding: 0;
      border: 0;
      border-radius: 14px;
      background: transparent;
      color: var(--fg-muted);
    }
    .star:hover {
      background: var(--field-hover);
      color: var(--fg);
    }
    .star.on {
      color: var(--accent);
    }
    /* The shell view is a fixed-height strip, so messages stay inside the field. */
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
    .notice.info {
      background: var(--accent-soft);
      color: var(--accent);
    }
    .bar {
      display: flex;
      align-items: center;
      gap: 2px;
      flex: none;
      height: 32px;
      padding: 0 8px;
      border-bottom: 1px solid var(--border);
      overflow: hidden;
    }
    .bar-item {
      display: inline-flex;
      align-items: center;
      gap: 6px;
      flex: 0 1 auto;
      min-width: 0;
      max-width: 200px;
      height: 26px;
      padding: 0 8px;
      border: 0;
      border-radius: var(--radius-s);
      background: transparent;
      font-size: 12.5px;
      color: var(--fg-muted);
    }
    .bar-item:hover {
      background: var(--field-hover);
      color: var(--fg);
    }
    .bar-item svg {
      flex: none;
      width: 14px;
      height: 14px;
    }
    .bar-item span {
      overflow: hidden;
      white-space: nowrap;
      text-overflow: ellipsis;
    }
    .bar-empty {
      padding: 0 8px;
      font-size: 12px;
      color: var(--fg-faint);
      white-space: nowrap;
    }
    .bar-spacer {
      flex: 1;
    }
    .findbar {
      display: flex;
      align-items: center;
      justify-content: flex-end;
      gap: 4px;
      flex: none;
      height: 40px;
      padding: 0 8px;
      border-bottom: 1px solid var(--border);
    }
    .find-field {
      display: flex;
      align-items: center;
      gap: 8px;
      width: min(420px, 60%);
      height: 30px;
      padding: 0 10px;
      border-radius: 15px;
      background: var(--field);
      border: 1px solid transparent;
      color: var(--fg-muted);
    }
    .find-field:focus-within {
      background: var(--field-focus);
      border-color: var(--accent);
    }
    #find {
      flex: 1;
      min-width: 0;
      height: 100%;
      border: 0;
      outline: none;
      background: transparent;
      color: var(--fg);
      font: inherit;
      font-size: 13px;
      user-select: text;
    }
    .find-status {
      flex: none;
      font-size: 12px;
      color: var(--fg-muted);
    }
    .find-status.none {
      color: var(--danger);
    }
  `];
}

customElements.define("tf-shell", TorFoxShell);
