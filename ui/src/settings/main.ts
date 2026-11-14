// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

import { LitElement, css, html, nothing } from "lit";

import { followAppearance } from "../appearance.js";
import { base } from "../base.js";
import { BridgeError, call } from "../bridge.js";
import { ERRORS, SECTIONS, SETTINGS } from "./labels.js";

type Value = boolean | string;

interface SettingInfo {
  key: string;
  section: string;
  type: "bool" | "enum" | "string";
  status: "active" | "planned";
  value: Value;
  default: Value;
  values?: string[];
  maxLength?: number;
}

interface SettingsData {
  sections: string[];
  settings: SettingInfo[];
}

interface AppInfo {
  name: string;
  version: string;
  cef: string;
  chromium: string;
  license: string;
  testBuild: boolean;
}

function errorText(err: unknown): string {
  if (err instanceof BridgeError) {
    return ERRORS[err.message] ?? `TorFox could not change this setting (${err.message}).`;
  }
  return String(err);
}

export class TorFoxSettings extends LitElement {
  static override properties = {
    data: { state: true },
    info: { state: true },
    section: { state: true },
    errors: { state: true },
    loadError: { state: true },
  };

  declare data: SettingsData | null;
  declare info: AppInfo | null;
  declare section: string;
  declare errors: Record<string, string>;
  declare loadError: string;

  constructor() {
    super();
    this.data = null;
    this.info = null;
    this.section = "general";
    this.errors = {};
    this.loadError = "";
  }

  override connectedCallback(): void {
    super.connectedCallback();
    void this.load();
  }

  private async load(): Promise<void> {
    try {
      const [data, info] = await Promise.all([
        call<SettingsData>("settings.get"),
        call<AppInfo>("app.info"),
      ]);
      this.data = data;
      this.info = info;
    } catch (err) {
      this.loadError = errorText(err);
    }
  }

  private async change(setting: SettingInfo, value: Value): Promise<void> {
    const { [setting.key]: _old, ...rest } = this.errors;
    this.errors = rest;
    try {
      const result = await call<{ value: Value }>("settings.set", { key: setting.key, value });
      this.updateValue(setting.key, result.value);
    } catch (err) {
      this.errors = { ...this.errors, [setting.key]: errorText(err) };
      // Show the stored value again, not what the user tried to choose.
      this.requestUpdate();
    }
  }

  private updateValue(key: string, value: Value): void {
    if (!this.data) {
      return;
    }
    this.data = {
      ...this.data,
      settings: this.data.settings.map((s) => (s.key === key ? { ...s, value } : s)),
    };
  }

  private renderControl(s: SettingInfo) {
    const label = SETTINGS[s.key];
    const disabled = s.status !== "active";
    const id = `setting-${s.key.replaceAll(".", "-")}`;
    if (s.type === "bool") {
      return html`<label class="switch">
        <input id=${id} type="checkbox" role="switch" ?disabled=${disabled}
          .checked=${s.value === true}
          @change=${(e: Event) => this.change(s, (e.target as HTMLInputElement).checked)} />
        <span class="track" aria-hidden="true"></span>
      </label>`;
    }
    if (s.type === "enum") {
      return html`<div class="segmented" role="radiogroup" aria-label=${label?.title ?? s.key}>
        ${(s.values ?? []).map(
          (v) => html`<label class="segment ${s.value === v ? "selected" : ""} ${disabled ? "disabled" : ""}">
            <input type="radio" name=${id} .value=${v} ?disabled=${disabled}
              .checked=${s.value === v}
              @change=${() => this.change(s, v)} />
            <span>${label?.values?.[v] ?? v}</span>
          </label>`,
        )}
      </div>`;
    }
    return html`<textarea id=${id} rows="4" spellcheck="false" ?disabled=${disabled}
      maxlength=${s.maxLength ?? 0}
      .value=${typeof s.value === "string" ? s.value : ""}
      @change=${(e: Event) => this.change(s, (e.target as HTMLTextAreaElement).value)}></textarea>`;
  }

  private renderSetting(s: SettingInfo) {
    const label = SETTINGS[s.key];
    const planned = s.status !== "active";
    const error = this.errors[s.key];
    const wide = s.type === "string";
    return html`<div class="row ${planned ? "planned" : ""} ${wide ? "wide" : ""}">
      <div class="row-text">
        <div class="row-title">
          <span>${label?.title ?? s.key}</span>
          ${planned ? html`<span class="badge" title="This feature is not built yet">Not active yet</span>` : nothing}
        </div>
        ${label?.description ? html`<div class="row-desc">${label.description}</div>` : nothing}
        ${error ? html`<div class="row-error" role="alert">${error}</div>` : nothing}
      </div>
      <div class="row-control">${this.renderControl(s)}</div>
    </div>`;
  }

  private renderSettings(section: string) {
    const items = (this.data?.settings ?? []).filter((s) => s.section === section);
    const groups: { name: string; items: SettingInfo[] }[] = [];
    for (const s of items) {
      const name = SETTINGS[s.key]?.group ?? "";
      let group = groups.find((g) => g.name === name);
      if (!group) {
        group = { name, items: [] };
        groups.push(group);
      }
      group.items.push(s);
    }
    return groups.map(
      (g) => html`<section class="card">
        ${g.name ? html`<h2>${g.name}</h2>` : nothing}
        ${g.items.map((s) => this.renderSetting(s))}
      </section>`,
    );
  }

  private renderAbout() {
    const info = this.info;
    const rows: [string, string][] = info
      ? [
          ["Version", info.version + (info.testBuild ? " (test build)" : "")],
          ["Chromium", info.chromium],
          ["CEF", info.cef],
          ["License", "Mozilla Public License 2.0"],
        ]
      : [];
    return html`
      <section class="card about">
        <h2>TorFox</h2>
        <dl>
          ${rows.map(([k, v]) => html`<div><dt>${k}</dt><dd>${v}</dd></div>`)}
        </dl>
      </section>
      <section class="card prose">
        <p>
          TorFox is an independent browser built on Chromium through the Chromium Embedded
          Framework. It is <strong>not Tor Browser</strong> and does not offer Tor Browser's
          protections against fingerprinting or its anonymity guarantees.
        </p>
        <p>
          TorFox contains no telemetry, usage statistics or crash reporting. Its source code is
          published under the Mozilla Public License 2.0.
        </p>
      </section>
    `;
  }

  override render() {
    const sections = this.data?.sections ?? Object.keys(SECTIONS);
    const current = SECTIONS[this.section];
    return html`
      <nav aria-label="Settings sections">
        <h1>Settings</h1>
        ${sections.map((id) => {
          const label = SECTIONS[id];
          return html`<button class="nav-item ${id === this.section ? "active" : ""}"
            aria-current=${id === this.section ? "page" : "false"}
            @click=${() => (this.section = id)}>
            ${label?.icon ?? nothing}<span>${label?.title ?? id}</span>
          </button>`;
        })}
      </nav>
      <main>
        <h1 class="page-title">${current?.title ?? this.section}</h1>
        ${this.loadError
          ? html`<div class="banner error" role="alert">${this.loadError}</div>`
          : nothing}
        ${current?.note ? html`<div class="banner">${current.note}</div>` : nothing}
        ${this.section === "about" ? this.renderAbout() : this.renderSettings(this.section)}
      </main>
    `;
  }

  static override styles = [base, css`
    :host {
      display: flex;
      min-height: 100vh;
      background: var(--page);
      color: var(--fg);
    }
    nav {
      position: sticky;
      top: 0;
      align-self: flex-start;
      flex: none;
      display: flex;
      flex-direction: column;
      gap: 2px;
      width: 240px;
      height: 100vh;
      padding: 20px 12px;
      border-right: 1px solid var(--border);
    }
    nav h1 {
      margin: 4px 12px 16px;
      font-family: var(--font-display);
      font-size: 20px;
      font-weight: 600;
    }
    .nav-item {
      display: flex;
      align-items: center;
      gap: 12px;
      height: 36px;
      padding: 0 12px;
      border: 0;
      border-radius: var(--radius-s);
      background: transparent;
      color: var(--fg-muted);
      font: inherit;
      text-align: left;
    }
    .nav-item:hover {
      background: var(--field);
      color: var(--fg);
    }
    .nav-item.active {
      background: var(--accent-soft);
      color: var(--accent);
      font-weight: 600;
    }
    .nav-item:focus-visible {
      outline: none;
      box-shadow: var(--focus-ring);
    }
    main {
      flex: 1;
      min-width: 0;
      max-width: 760px;
      padding: 24px 40px 64px;
    }
    .page-title {
      margin: 0 0 20px;
      font-family: var(--font-display);
      font-size: 26px;
      font-weight: 600;
      letter-spacing: -0.01em;
    }
    .banner {
      margin-bottom: 16px;
      padding: 12px 16px;
      border-radius: var(--radius-m);
      background: var(--accent-soft);
      color: var(--fg);
      font-size: 13px;
      line-height: 1.5;
    }
    .banner.error {
      background: var(--danger-soft);
      color: var(--danger);
    }
    .card {
      margin-bottom: 16px;
      padding: 4px 20px;
      border-radius: var(--radius-l);
      border: 1px solid var(--border);
      background: var(--card);
    }
    .card h2 {
      margin: 14px 0 2px;
      font-size: 12px;
      font-weight: 600;
      letter-spacing: 0.04em;
      text-transform: uppercase;
      color: var(--fg-faint);
    }
    .row {
      display: flex;
      align-items: center;
      gap: 24px;
      min-height: 60px;
      padding: 12px 0;
    }
    .row + .row {
      border-top: 1px solid var(--border);
    }
    .row.wide {
      flex-direction: column;
      align-items: stretch;
      gap: 10px;
    }
    .row-text {
      flex: 1;
      min-width: 0;
    }
    .row-title {
      display: flex;
      align-items: center;
      gap: 8px;
      font-size: 14px;
    }
    .row.planned .row-title > span:first-child,
    .row.planned .row-desc {
      color: var(--fg-muted);
    }
    .row-desc {
      margin-top: 3px;
      font-size: 12.5px;
      line-height: 1.45;
      color: var(--fg-muted);
    }
    .row-error {
      margin-top: 6px;
      font-size: 12.5px;
      color: var(--danger);
    }
    .row-control {
      flex: none;
    }
    .badge {
      padding: 1px 8px;
      border-radius: 10px;
      border: 1px solid var(--border-strong);
      font-size: 11px;
      color: var(--fg-muted);
      white-space: nowrap;
    }
    .switch {
      position: relative;
      display: inline-block;
      width: 40px;
      height: 22px;
    }
    .switch input {
      position: absolute;
      inset: 0;
      width: 100%;
      height: 100%;
      margin: 0;
      opacity: 0;
    }
    .track {
      position: absolute;
      inset: 0;
      border-radius: 11px;
      background: var(--border-strong);
      transition: background-color 120ms ease;
      pointer-events: none;
    }
    .track::after {
      content: "";
      position: absolute;
      top: 3px;
      left: 3px;
      width: 16px;
      height: 16px;
      border-radius: 50%;
      background: var(--fg);
      transition: transform 120ms ease;
    }
    .switch input:checked + .track {
      background: var(--accent);
    }
    .switch input:checked + .track::after {
      transform: translateX(18px);
      background: var(--on-accent);
    }
    .switch input:disabled + .track {
      opacity: 0.4;
    }
    .switch input:focus-visible + .track {
      box-shadow: var(--focus-ring);
    }
    .segmented {
      display: inline-flex;
      padding: 3px;
      gap: 2px;
      border-radius: var(--radius-m);
      background: var(--field);
    }
    .segment {
      position: relative;
      display: inline-flex;
      align-items: center;
      height: 30px;
      padding: 0 14px;
      border-radius: var(--radius-s);
      font-size: 13px;
      color: var(--fg-muted);
      white-space: nowrap;
    }
    .segment input {
      position: absolute;
      inset: 0;
      width: 100%;
      height: 100%;
      margin: 0;
      opacity: 0;
    }
    .segment:not(.disabled):hover {
      color: var(--fg);
    }
    .segment.selected {
      background: var(--card-raised);
      color: var(--fg);
      box-shadow: 0 1px 2px rgba(0, 0, 0, 0.25);
    }
    .segment.disabled {
      opacity: 0.5;
    }
    .segment:has(input:focus-visible) {
      box-shadow: var(--focus-ring);
    }
    textarea {
      width: 100%;
      padding: 10px 12px;
      border-radius: var(--radius-m);
      border: 1px solid var(--border);
      background: var(--field-focus);
      color: var(--fg);
      font-family: var(--font-mono);
      font-size: 12.5px;
      resize: vertical;
    }
    textarea:disabled {
      opacity: 0.5;
    }
    textarea:focus-visible {
      outline: none;
      border-color: var(--accent);
    }
    .about dl {
      margin: 8px 0 12px;
    }
    .about dl div {
      display: flex;
      gap: 24px;
      padding: 8px 0;
      font-size: 13.5px;
    }
    .about dl div + div {
      border-top: 1px solid var(--border);
    }
    .about dt {
      width: 120px;
      color: var(--fg-muted);
    }
    .about dd {
      margin: 0;
      font-family: var(--font-mono);
      font-size: 12.5px;
      user-select: text;
    }
    .prose p {
      margin: 14px 0;
      font-size: 13.5px;
      line-height: 1.55;
      color: var(--fg-muted);
    }
    .prose strong {
      color: var(--fg);
    }
  `];
}

customElements.define("tf-settings", TorFoxSettings);
followAppearance();
