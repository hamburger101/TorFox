// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

import { LitElement, css, html, nothing } from "lit";

import { followAppearance } from "../appearance.js";
import { base } from "../base.js";
import { BridgeError, call, subscribe } from "../bridge.js";
import { icons } from "../icons.js";
import { hostOf, pageStyles } from "../page.js";

interface Bookmark {
  id: number;
  parent: number;
  index: number;
  folder: boolean;
  title: string;
  url: string;
  depth?: number;
}

interface Subscription {
  changed: string;
  available?: boolean;
  notice?: string;
}

const BOOKMARKS_BAR = 1;

type Form =
  | { kind: "add-bookmark"; title: string; url: string }
  | { kind: "add-folder"; title: string }
  | { kind: "edit"; id: number; folder: boolean; title: string; url: string };

/** "example.com" means https://example.com; anything with a scheme is kept as typed. */
function normaliseUrl(input: string): string {
  const url = input.trim();
  return url.includes("://") ? url : `https://${url}`;
}

export class TorFoxBookmarks extends LitElement {
  static override properties = {
    folders: { state: true },
    folderId: { state: true },
    items: { state: true },
    query: { state: true },
    form: { state: true },
    confirmDelete: { state: true },
    error: { state: true },
    available: { state: true },
    notice: { state: true },
  };

  declare folders: Bookmark[];
  declare folderId: number;
  declare items: Bookmark[];
  declare query: string;
  declare form: Form | null;
  declare confirmDelete: number;
  declare error: string;
  declare available: boolean;
  declare notice: string;

  constructor() {
    super();
    this.folders = [];
    this.folderId = BOOKMARKS_BAR;
    this.items = [];
    this.query = "";
    this.form = null;
    this.confirmDelete = 0;
    this.error = "";
    this.available = true;
    this.notice = "";
  }

  override connectedCallback(): void {
    super.connectedCallback();
    subscribe<Subscription>("bookmarks.subscribe", (m) => {
      if (m.available !== undefined) {
        this.available = m.available;
        this.notice = m.notice ?? "";
      }
      void this.refresh();
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

  private async refresh(): Promise<void> {
    if (!this.available) {
      return;
    }
    const folders = await this.attempt(() => call<{ folders: Bookmark[] }>("bookmarks.folders"));
    if (!folders) {
      return;
    }
    this.folders = folders.folders;
    if (!this.folders.some((f) => f.id === this.folderId)) {
      this.folderId = BOOKMARKS_BAR;
    }
    if (this.query) {
      const found = await this.attempt(() =>
        call<{ items: Bookmark[] }>("bookmarks.search", { query: this.query }));
      this.items = found?.items ?? [];
    } else {
      const children = await this.attempt(() =>
        call<{ items: Bookmark[] }>("bookmarks.children", { id: this.folderId }));
      this.items = children?.items ?? [];
    }
  }

  private selectFolder(id: number): void {
    this.folderId = id;
    this.query = "";
    this.form = null;
    this.confirmDelete = 0;
    const search = this.renderRoot.querySelector<HTMLInputElement>("#search");
    if (search) {
      search.value = "";
    }
    void this.refresh();
  }

  private onSearch(e: InputEvent): void {
    this.query = (e.target as HTMLInputElement).value.trim();
    this.form = null;
    void this.refresh();
  }

  private open(e: MouseEvent, b: Bookmark): void {
    if (b.folder) {
      this.selectFolder(b.id);
      return;
    }
    const newTab = e.button === 1 || e.ctrlKey || e.metaKey;
    void this.attempt(() => call("nav.open", { url: b.url, newTab }));
  }

  private async move(b: Bookmark, parent: number, index: number): Promise<void> {
    await this.attempt(() => call("bookmarks.move", { id: b.id, parent, index }));
  }

  private async removeBookmark(b: Bookmark): Promise<void> {
    if (b.folder && this.confirmDelete !== b.id) {
      this.confirmDelete = b.id;  // Folders take everything in them: ask first.
      return;
    }
    this.confirmDelete = 0;
    await this.attempt(() => call("bookmarks.remove", { id: b.id }));
  }

  private async submit(e: Event): Promise<void> {
    e.preventDefault();
    const form = this.form;
    if (!form) {
      return;
    }
    const title = form.title.trim();
    let ok: unknown;
    if (form.kind === "add-bookmark") {
      const url = normaliseUrl(form.url);
      ok = await this.attempt(() =>
        call("bookmarks.add", { parent: this.folderId, title: title || url, url }));
    } else if (form.kind === "add-folder") {
      ok = await this.attempt(() =>
        call("bookmarks.addFolder", { parent: this.folderId, title: title || "New folder" }));
    } else {
      const args: Record<string, unknown> = { id: form.id, title };
      if (!form.folder) {
        args["url"] = normaliseUrl(form.url);
      }
      ok = await this.attempt(() => call("bookmarks.update", args));
    }
    if (ok !== undefined) {
      this.form = null;
    }
  }

  private setField(field: "title" | "url", e: InputEvent): void {
    if (this.form) {
      this.form = { ...this.form, [field]: (e.target as HTMLInputElement).value } as Form;
    }
  }

  private renderForm(form: Form) {
    const label = form.kind === "add-bookmark" ? "Add bookmark"
      : form.kind === "add-folder" ? "Add folder" : "Save";
    const showUrl = form.kind === "add-bookmark" || (form.kind === "edit" && !form.folder);
    return html`<form class="form" @submit=${this.submit}>
      <input class="text title" aria-label="Name" placeholder="Name" .value=${form.title}
        @input=${(e: InputEvent) => this.setField("title", e)} />
      ${showUrl
        ? html`<input class="text url" aria-label="Address" placeholder="https://example.com/"
            .value=${"url" in form ? form.url : ""}
            @input=${(e: InputEvent) => this.setField("url", e)} />`
        : nothing}
      <button class="button primary" type="submit">${label}</button>
      <button class="button" type="button" @click=${() => (this.form = null)}>Cancel</button>
    </form>`;
  }

  private renderRow(b: Bookmark, index: number) {
    if (this.form?.kind === "edit" && this.form.id === b.id) {
      return html`<div class="row editing">${this.renderForm(this.form)}</div>`;
    }
    const searching = !!this.query;
    const last = index === this.items.length - 1;
    const moveTargets = this.folders.filter((f) => f.id !== b.parent && f.id !== b.id);
    return html`<div class="row" data-id=${b.id}>
      <button class="row-main" title=${b.folder ? b.title : `${b.title}\n${b.url}`}
        @click=${(e: MouseEvent) => this.open(e, b)}
        @auxclick=${(e: MouseEvent) => { if (e.button === 1) { e.preventDefault(); this.open(e, b); } }}>
        <span class="row-icon">${b.folder ? icons.folder : icons.globe}</span>
        <span class="row-title">${b.title || b.url}</span>
        ${b.folder ? nothing : html`<span class="row-url">${hostOf(b.url)}</span>`}
      </button>
      ${this.confirmDelete === b.id
        ? html`<span class="confirm">Delete this folder and everything in it?
            <button class="button danger primary" @click=${() => this.removeBookmark(b)}>Delete</button>
            <button class="button" @click=${() => (this.confirmDelete = 0)}>Cancel</button></span>`
        : html`<div class="row-actions">
            ${searching ? nothing : html`
              <button class="icon-button" title="Move up" aria-label="Move up" ?disabled=${index === 0}
                @click=${() => this.move(b, b.parent, index - 1)}>${icons.arrowUp}</button>
              <button class="icon-button" title="Move down" aria-label="Move down" ?disabled=${last}
                @click=${() => this.move(b, b.parent, index + 1)}>${icons.arrowDown}</button>`}
            <select aria-label="Move to folder" title="Move to folder"
              @change=${(e: Event) => {
                const target = Number((e.target as HTMLSelectElement).value);
                if (target) {
                  void this.move(b, target, -1);
                }
              }}>
              <option value="">Move to…</option>
              ${moveTargets.map((f) => html`<option value=${f.id}>${" ".repeat(f.depth ?? 0)}${f.title}</option>`)}
            </select>
            <button class="icon-button" title="Edit" aria-label="Edit"
              @click=${() => (this.form = { kind: "edit", id: b.id, folder: b.folder, title: b.title, url: b.url })}>${icons.edit}</button>
            <button class="icon-button" title="Delete" aria-label="Delete"
              @click=${() => this.removeBookmark(b)}>${icons.trash}</button>
          </div>`}
    </div>`;
  }

  override render() {
    const folder = this.folders.find((f) => f.id === this.folderId);
    return html`
      <header>
        <h1>Bookmarks</h1>
        <label class="search">
          ${icons.search}
          <input id="search" type="search" spellcheck="false" autocomplete="off"
            aria-label="Search bookmarks" placeholder="Search bookmarks" @input=${this.onSearch} />
        </label>
      </header>
      ${!this.available
        ? html`<div class="banner" role="alert">${this.notice || "Bookmarks are unavailable."}</div>`
        : this.notice ? html`<div class="banner info" role="status">${this.notice}</div>` : nothing}
      ${this.error ? html`<div class="banner" role="alert">${this.error}</div>` : nothing}
      <div class="layout">
        <nav aria-label="Folders">
          ${this.folders.map((f) => html`<button class="folder ${f.id === this.folderId && !this.query ? "selected" : ""}"
            data-depth=${Math.min(f.depth ?? 0, 6)}
            @click=${() => this.selectFolder(f.id)}>
            ${icons.folder}<span>${f.title}</span></button>`)}
        </nav>
        <main>
          <div class="list-header">
            <h2>${this.query ? `Results for “${this.query}”` : folder?.title ?? ""}</h2>
            ${this.query || !this.available ? nothing : html`
              <button class="button" @click=${() => (this.form = { kind: "add-bookmark", title: "", url: "" })}>${icons.star}Add bookmark</button>
              <button class="button" @click=${() => (this.form = { kind: "add-folder", title: "" })}>${icons.folder}Add folder</button>`}
          </div>
          ${this.form && this.form.kind !== "edit" ? html`<div class="row editing">${this.renderForm(this.form)}</div>` : nothing}
          ${this.items.length === 0
            ? html`<p class="empty">${this.query ? "No bookmarks match." : "This folder is empty."}</p>`
            : this.items.map((b, i) => this.renderRow(b, i))}
        </main>
      </div>
    `;
  }

  static override styles = [base, pageStyles, css`
    .layout {
      display: grid;
      grid-template-columns: minmax(180px, 240px) 1fr;
      gap: 24px;
      max-width: 1100px;
      padding: 20px 32px 48px;
    }
    nav {
      display: flex;
      flex-direction: column;
      gap: 2px;
    }
    .folder {
      display: flex;
      align-items: center;
      gap: 8px;
      height: 32px;
      padding: 0 10px;
      border: 0;
      border-radius: var(--radius-s);
      background: transparent;
      color: var(--fg-muted);
      text-align: left;
      font-size: 13px;
    }
    .folder span {
      overflow: hidden;
      white-space: nowrap;
      text-overflow: ellipsis;
    }
    .folder svg {
      flex: none;
    }
    .folder:hover {
      background: var(--card);
      color: var(--fg);
    }
    .folder.selected {
      background: var(--accent-soft);
      color: var(--fg);
    }
    .folder[data-depth="1"] { padding-left: 26px; }
    .folder[data-depth="2"] { padding-left: 42px; }
    .folder[data-depth="3"] { padding-left: 58px; }
    .folder[data-depth="4"] { padding-left: 74px; }
    .folder[data-depth="5"] { padding-left: 90px; }
    .folder[data-depth="6"] { padding-left: 106px; }
    .list-header {
      display: flex;
      align-items: center;
      gap: 8px;
      margin-bottom: 8px;
    }
    h2 {
      flex: 1;
      margin: 0;
      font-size: 15px;
      font-weight: 600;
    }
    .form {
      display: flex;
      flex-wrap: wrap;
      align-items: center;
      gap: 8px;
      width: 100%;
      padding: 6px 0;
    }
    .form .title {
      flex: 1 1 200px;
    }
    .form .url {
      flex: 2 1 260px;
    }
    .row.editing {
      background: var(--card);
    }
    .row-actions select {
      height: 28px;
      max-width: 140px;
      font-size: 12px;
    }
    .confirm {
      display: inline-flex;
      align-items: center;
      gap: 8px;
      font-size: 12.5px;
      color: var(--danger);
    }
  `];
}

customElements.define("tf-bookmarks", TorFoxBookmarks);
followAppearance();
